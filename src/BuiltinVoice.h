#pragma once

#include <Arduino.h>
#include <FS.h>

#include <vector>

#include "VoiceVoxClient.h"  // LipSyncCallback / ServiceCallback

// 内蔵ボイス（TTS サーバーなしで喋るためのオフライン音声）。
//
// tools/make_voice_pack.py が VOICEVOX で作ったボイスパック（LittleFS の /voice.pak）を
// 使い、文章を「セリフ断片・数字・数字＋単位」のクリップへ最長一致で分解して、
// つなげて再生する。実況の文面はソースの文字列リテラルからパックを作るので、
// 実況で喋る文章はほぼすべて読める。辞書に無い部分（ジョブ名など）は読み飛ばし、
// 「」で囲まれたジョブ名は「作品」と読み替える。
class BuiltinVoice {
 public:
  // LittleFS をマウントしてボイスパックの索引を読み込む。
  // 戻り値: 使える=true（パックが無ければ false、エラー内容は lastError()）
  bool begin();

  void setCallbacks(LipSyncCallback lipSync, ServiceCallback service);

  bool isReady() const { return ready_; }
  uint16_t clipCount() const { return count_; }
  uint32_t sampleRate() const { return rate_; }
  const String& lastError() const { return lastError_; }

  // 文章を内蔵ボイスで読み上げる（ブロッキング、口パク付き）。
  // 読めるクリップが1つも無ければ何もせず false を返す。
  bool speak(const String& text);

 private:
  struct Clip {
    uint32_t keyOffset;   // index_ 内のキー位置
    uint32_t dataOffset;  // データ部先頭からの位置
    uint32_t samples;
    int16_t predictor;    // ADPCM の初期値
    uint8_t keyLen;
    uint8_t stepIndex;
  };

  struct Step {
    int16_t clip;      // -1 = 無音
    uint16_t pauseMs;
  };

  const char* key(const Clip& clip) const { return index_ + clip.keyOffset; }
  int findKey(const char* text, size_t len) const;
  int longestPlainKeyAt(const char* text, size_t remaining, size_t* matched) const;
  void plan(const String& text, std::vector<Step>& steps) const;
  size_t planNumber(const char* s, size_t n, size_t begin, size_t end,
                    std::vector<Step>& steps) const;
  void addClip(std::vector<Step>& steps, int clip) const;
  void addPause(std::vector<Step>& steps, uint16_t ms) const;

  bool playClip(const Clip& clip);
  void playSilence(uint16_t ms);
  void submit(size_t sampleCount);

  bool ready_ = false;
  uint8_t codec_ = 1;      // 0 = IMA-ADPCM, 1 = μ-law
  uint32_t rate_ = 12000;
  uint16_t count_ = 0;
  uint32_t dataStart_ = 0;
  char* index_ = nullptr;  // キー文字列を含む索引（PSRAM）
  Clip* clips_ = nullptr;  // キーのバイト順に並んだクリップ（PSRAM）
  uint16_t bucket_[257] = {};  // 先頭バイトごとのクリップ範囲
  File file_;
  size_t bufferIndex_ = 0;
  LipSyncCallback lipSync_ = nullptr;
  ServiceCallback service_ = nullptr;
  String lastError_;
};
