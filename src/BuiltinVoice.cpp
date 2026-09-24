#include "BuiltinVoice.h"

#include <LittleFS.h>
#include <M5Unified.h>

namespace {

constexpr char kPackPath[] = "/voice.pak";
constexpr uint8_t kCodecAdpcm = 0;
constexpr uint8_t kCodecMulaw = 1;

// 再生バッファ（TtsClient と同じく3面を回し、再生中・再生待ち・書き込み中に使い分ける）
constexpr size_t kBufferCount = 3;
constexpr size_t kBufferSamples = 1024;
alignas(4) int16_t pcmBuffers[kBufferCount][kBufferSamples];
uint8_t rawBuffer[kBufferSamples];

// 文の区切りの間（ms）
constexpr uint16_t kSentencePauseMs = 220;
constexpr uint16_t kCommaPauseMs = 110;
constexpr uint16_t kAfterSentenceClipPauseMs = 150;

const int16_t kStepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,
    19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
    5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
const int8_t kIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8,
                                -1, -1, -1, -1, 2, 4, 6, 8};

int16_t mulawToLinear(uint8_t value) {
  value = ~value;
  const int sign = value & 0x80;
  const int exponent = (value >> 4) & 0x07;
  const int mantissa = value & 0x0F;
  const int sample = (((mantissa << 3) + 0x84) << exponent) - 0x84;
  return static_cast<int16_t>(sign ? -sample : sample);
}

size_t utf8Length(uint8_t lead) {
  if (lead >= 0xF0) return 4;
  if (lead >= 0xE0) return 3;
  if (lead >= 0xC0) return 2;
  return 1;
}

bool startsWith(const char* s, size_t remaining, const char* prefix) {
  const size_t len = strlen(prefix);
  return remaining >= len && memcmp(s, prefix, len) == 0;
}

// 区切り記号なら間の長さを返す（len に記号のバイト数）
uint16_t pauseFor(const char* s, size_t remaining, size_t* len) {
  static const struct {
    const char* mark;
    uint16_t ms;
  } kMarks[] = {
      {"。", kSentencePauseMs}, {"！", kSentencePauseMs}, {"？", kSentencePauseMs},
      {"…", kSentencePauseMs},  {"、", kCommaPauseMs},    {"，", kCommaPauseMs},
      {"!", kSentencePauseMs},  {"?", kSentencePauseMs},  {".", kSentencePauseMs},
      {",", kCommaPauseMs},     {"　", 60},               {" ", 60},
      {"\n", kSentencePauseMs},
  };
  for (const auto& m : kMarks) {
    if (startsWith(s, remaining, m.mark)) {
      *len = strlen(m.mark);
      return m.ms;
    }
  }
  *len = 0;
  return 0;
}

bool endsWithSentenceMark(const char* key, size_t len) {
  static const char* const kMarks[] = {"。", "！", "？", "…"};
  for (const char* mark : kMarks) {
    const size_t ml = strlen(mark);
    if (len >= ml && memcmp(key + len - ml, mark, ml) == 0) return true;
  }
  return false;
}

}  // namespace

bool BuiltinVoice::begin() {
  ready_ = false;
  if (!LittleFS.begin(false)) {
    lastError_ = "LittleFS mount failed (run: pio run -t uploadfs)";
    return false;
  }
  file_ = LittleFS.open(kPackPath, "r");
  if (!file_) {
    lastError_ = "voice pack not found (run tools/make_voice_pack.py)";
    return false;
  }

  uint8_t header[20];
  if (file_.read(header, sizeof(header)) != sizeof(header) ||
      memcmp(header, "SCVP", 4) != 0) {
    lastError_ = "invalid voice pack header";
    return false;
  }
  const uint16_t version = header[4] | (header[5] << 8);
  codec_ = header[6];
  memcpy(&rate_, header + 8, 4);
  uint32_t count = 0;
  uint32_t indexBytes = 0;
  memcpy(&count, header + 12, 4);
  memcpy(&indexBytes, header + 16, 4);
  if (version != 1 || (codec_ != kCodecAdpcm && codec_ != kCodecMulaw) ||
      count == 0 || count > 20000 || rate_ < 4000 || rate_ > 48000) {
    lastError_ = "unsupported voice pack";
    return false;
  }

  index_ = static_cast<char*>(ps_malloc(indexBytes));
  clips_ = static_cast<Clip*>(ps_malloc(sizeof(Clip) * count));
  if (index_ == nullptr || clips_ == nullptr ||
      file_.read(reinterpret_cast<uint8_t*>(index_), indexBytes) != indexBytes) {
    lastError_ = "voice pack index read failed";
    return false;
  }

  // 索引: [u8 keyLen][key][u32 offset][u32 samples][i16 predictor][u8 step][u8 0]
  uint32_t pos = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (pos + 1 > indexBytes) break;
    Clip& c = clips_[i];
    c.keyLen = static_cast<uint8_t>(index_[pos]);
    c.keyOffset = pos + 1;
    pos += 1 + c.keyLen;
    if (pos + 12 > indexBytes) {
      lastError_ = "voice pack index truncated";
      return false;
    }
    memcpy(&c.dataOffset, index_ + pos, 4);
    memcpy(&c.samples, index_ + pos + 4, 4);
    memcpy(&c.predictor, index_ + pos + 8, 2);
    c.stepIndex = static_cast<uint8_t>(index_[pos + 10]);
    pos += 12;
  }
  count_ = static_cast<uint16_t>(count);
  dataStart_ = sizeof(header) + indexBytes;

  // キーはバイト順に並んでいるので、先頭バイトごとの範囲を作っておく
  uint16_t c = 0;
  for (int b = 0; b < 256; ++b) {
    bucket_[b] = c;
    while (c < count_ && static_cast<uint8_t>(key(clips_[c])[0]) == b) ++c;
  }
  bucket_[256] = count_;

  ready_ = true;
  lastError_ = "";
  Serial.printf("[voice] builtin voice ready: %u clips, %s %luHz\n", count_,
                codec_ == kCodecMulaw ? "mulaw" : "adpcm",
                static_cast<unsigned long>(rate_));
  return true;
}

void BuiltinVoice::setCallbacks(LipSyncCallback lipSync,
                                ServiceCallback service) {
  lipSync_ = lipSync;
  service_ = service;
}

int BuiltinVoice::findKey(const char* text, size_t len) const {
  int lo = 0;
  int hi = static_cast<int>(count_) - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    const Clip& c = clips_[mid];
    const int cmp = memcmp(key(c), text, min<size_t>(c.keyLen, len));
    if (cmp == 0 && c.keyLen == len) return mid;
    if (cmp < 0 || (cmp == 0 && c.keyLen < len)) {
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return -1;
}

int BuiltinVoice::longestPlainKeyAt(const char* text, size_t remaining,
                                    size_t* matched) const {
  *matched = 0;
  if (remaining == 0) return -1;
  const uint8_t first = static_cast<uint8_t>(text[0]);
  if (first == '#' || first == '=') return -1;  // 数字用のキーは直接一致させない
  int best = -1;
  for (uint16_t i = bucket_[first]; i < bucket_[first + 1]; ++i) {
    const Clip& c = clips_[i];
    if (c.keyLen <= remaining && c.keyLen > *matched &&
        memcmp(key(c), text, c.keyLen) == 0) {
      best = i;
      *matched = c.keyLen;
    }
  }
  return best;
}

void BuiltinVoice::addClip(std::vector<Step>& steps, int clip) const {
  if (clip < 0) return;
  steps.push_back({static_cast<int16_t>(clip), 0});
}

void BuiltinVoice::addPause(std::vector<Step>& steps, uint16_t ms) const {
  if (steps.empty()) return;  // 先頭の間は不要
  if (steps.back().clip < 0) {
    steps.back().pauseMs = max(steps.back().pauseMs, ms);
    return;
  }
  steps.push_back({-1, ms});
}

// 数字列 [begin, end) を読む。直後の単位（分・時間・% など）は促音・連濁を含めて
// まとめて録音したクリップ（"=23分" など）を優先する。戻り値は次に読む位置。
size_t BuiltinVoice::planNumber(const char* s, size_t n, size_t begin,
                                size_t end, std::vector<Step>& steps) const {
  char key[24];
  if (end - begin > 4) {
    // 5桁以上は1桁ずつ読む（実況ではまず出てこない）
    for (size_t i = begin; i < end; ++i) {
      snprintf(key, sizeof(key), "#%c", s[i]);
      addClip(steps, findKey(key, strlen(key)));
    }
    return end;
  }

  int value = 0;
  for (size_t i = begin; i < end; ++i) value = value * 10 + (s[i] - '0');
  const int total = value;
  if (value >= 1000) {
    snprintf(key, sizeof(key), "#%d", value / 1000 * 1000);
    addClip(steps, findKey(key, strlen(key)));
    value %= 1000;
  }
  if (value >= 100) {
    snprintf(key, sizeof(key), "#%d", value / 100 * 100);
    addClip(steps, findKey(key, strlen(key)));
    value %= 100;
  }

  static const char* const kUnits[] = {"時間", "%", "分", "時", "層", "件"};
  for (const char* unit : kUnits) {
    if (!startsWith(s + end, n - end, unit)) continue;
    // 「層目だよ」のように単位で始まる長い断片があればそちらに任せる
    size_t longer = 0;
    longestPlainKeyAt(s + end, n - end, &longer);
    if (longer > strlen(unit)) break;
    if (value > 0 || total == 0) {
      snprintf(key, sizeof(key), "=%d%s", value, unit);
      const int clip = findKey(key, strlen(key));
      if (clip >= 0) {
        addClip(steps, clip);
        return end + strlen(unit);
      }
    }
    break;
  }

  if (value > 0 || total == 0) {
    snprintf(key, sizeof(key), "#%d", value);
    addClip(steps, findKey(key, strlen(key)));
  }
  return end;
}

void BuiltinVoice::plan(const String& text, std::vector<Step>& steps) const {
  const char* s = text.c_str();
  const size_t n = text.length();
  size_t i = 0;
  while (i < n) {
    const uint8_t c = static_cast<uint8_t>(s[i]);

    // 「ジョブ名」は読めないので「作品」と言い換える
    if (startsWith(s + i, n - i, "「")) {
      const char* close = strstr(s + i, "」");
      addClip(steps, findKey("作品", strlen("作品")));
      i = close ? static_cast<size_t>(close - s) + strlen("」") : n;
      continue;
    }

    // 数字（先に「1層目が終わったよ！…」のような数字で始まる断片を探す）
    if (c >= '0' && c <= '9') {
      size_t end = i;
      while (end < n && s[end] >= '0' && s[end] <= '9') ++end;
      size_t matched = 0;
      const int clip = longestPlainKeyAt(s + i, n - i, &matched);
      if (clip >= 0 && matched > end - i) {
        addClip(steps, clip);
        i += matched;
        continue;
      }
      i = planNumber(s, n, i, end, steps);
      continue;
    }

    size_t markLen = 0;
    const uint16_t pause = pauseFor(s + i, n - i, &markLen);
    size_t matched = 0;
    const int clip = longestPlainKeyAt(s + i, n - i, &matched);
    if (clip >= 0) {
      if (pause > 0) addPause(steps, pause);
      addClip(steps, clip);
      if (endsWithSentenceMark(key(clips_[clip]), matched)) {
        addPause(steps, kAfterSentenceClipPauseMs);
      }
      i += matched;
      continue;
    }
    if (pause > 0) {
      addPause(steps, pause);
      i += markLen;
      continue;
    }
    i += utf8Length(c);  // 読めない文字は飛ばす
  }
  if (!steps.empty() && steps.back().clip < 0) steps.pop_back();
}

bool BuiltinVoice::speak(const String& text) {
  if (!ready_) return false;
  std::vector<Step> steps;
  steps.reserve(32);
  plan(text, steps);
  bool hasClip = false;
  for (const Step& step : steps) hasClip |= step.clip >= 0;
  if (!hasClip) {
    lastError_ = "no speakable words";
    return false;
  }

  Serial.printf("[voice] builtin: %u steps\n", static_cast<unsigned>(steps.size()));
  bool ok = true;
  for (const Step& step : steps) {
    if (step.clip < 0) {
      playSilence(step.pauseMs);
    } else if (!playClip(clips_[step.clip])) {
      ok = false;
      break;
    }
  }
  while (M5.Speaker.isPlaying()) {
    if (service_) service_();
    delay(5);
  }
  if (lipSync_) lipSync_(0);
  return ok;
}

void BuiltinVoice::submit(size_t sampleCount) {
  int16_t* samples = pcmBuffers[bufferIndex_];
  if (lipSync_) {
    // TtsClient と同じ基準で平均振幅を 0〜100 の口の開きにする
    uint32_t amplitude = 0;
    for (size_t i = 0; i < sampleCount; ++i) {
      amplitude += samples[i] < 0 ? -samples[i] : samples[i];
    }
    const int average = static_cast<int>(amplitude / max<size_t>(1, sampleCount));
    lipSync_(constrain((average - 180) * 100 / (5200 - 180), 0, 100));
  }
  M5.Speaker.playRaw(samples, sampleCount, rate_, false, 1, 0, false);
  bufferIndex_ = (bufferIndex_ + 1) % kBufferCount;
  if (service_) service_();
}

bool BuiltinVoice::playClip(const Clip& clip) {
  if (!file_.seek(dataStart_ + clip.dataOffset)) {
    lastError_ = "voice pack seek failed";
    return false;
  }
  uint32_t remaining = clip.samples;
  int predictor = clip.predictor;
  int stepIndex = clip.stepIndex;
  while (remaining > 0) {
    const size_t count = min<size_t>(remaining, kBufferSamples);
    int16_t* out = pcmBuffers[bufferIndex_];
    if (codec_ == kCodecMulaw) {
      if (file_.read(rawBuffer, count) != count) {
        lastError_ = "voice pack read failed";
        return false;
      }
      for (size_t i = 0; i < count; ++i) out[i] = mulawToLinear(rawBuffer[i]);
    } else {
      // count は偶数（最後の端数だけ奇数になり得る）
      const size_t bytes = (count + 1) / 2;
      if (file_.read(rawBuffer, bytes) != bytes) {
        lastError_ = "voice pack read failed";
        return false;
      }
      for (size_t i = 0; i < count; ++i) {
        const uint8_t code =
            (i % 2 == 0) ? (rawBuffer[i / 2] & 0x0F) : (rawBuffer[i / 2] >> 4);
        const int step = kStepTable[stepIndex];
        int diff = step >> 3;
        if (code & 4) diff += step;
        if (code & 2) diff += step >> 1;
        if (code & 1) diff += step >> 2;
        predictor += (code & 8) ? -diff : diff;
        predictor = constrain(predictor, -32768, 32767);
        stepIndex = constrain(stepIndex + kIndexTable[code], 0, 88);
        out[i] = static_cast<int16_t>(predictor);
      }
    }
    submit(count);
    remaining -= count;
  }
  return true;
}

void BuiltinVoice::playSilence(uint16_t ms) {
  uint32_t remaining = rate_ * ms / 1000;
  while (remaining > 0) {
    const size_t count = min<size_t>(remaining, kBufferSamples);
    memset(pcmBuffers[bufferIndex_], 0, count * sizeof(int16_t));
    submit(count);
    remaining -= count;
  }
}
