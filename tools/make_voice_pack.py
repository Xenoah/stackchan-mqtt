"""StackChan 内蔵ボイスパック（data/voice.pak）を VOICEVOX で生成する。

StackChan を TTS サーバーなしで喋らせるため、実況に出てくるセリフの断片・数字・
「数字＋単位」を一度だけ VOICEVOX で合成し、本体のフラッシュ（LittleFS）へ焼き込む。
本体（BuiltinVoice）は実況文をこの辞書で最長一致に分解してつなげて再生する。

セリフ断片は src/PrintCommentator.cpp と src/PrinterState.cpp の文字列リテラルから
自動抽出するので、実況の文面を変えたらこのスクリプトを再実行すればよい。

使い方:
    1. VOICEVOX（またはエンジン run.exe）を起動しておく
    2. python tools/make_voice_pack.py
    3. pio run -t uploadfs      # data/ を LittleFS として書き込む

パック形式（リトルエンディアン）:
    "SCVP" u16:version u8:codec u8:0 u32:sample_rate u32:count u32:index_bytes
    index: count 回 [u8:key_len key u32:data_offset u32:samples i16:predictor u8:step u8:0]
    data : codec=0 IMA-ADPCM 4bit（下位ニブルが先）/ codec=1 G.711 μ-law 8bit

既定は μ-law 12kHz（約 9MB）。IMA-ADPCM 16kHz は半分の大きさだが、
VOICEVOX の声では SNR が 15dB 前後まで落ちてザラつくため選択式にしている。

キーの種類:
    断片     そのままの文字列（例: "印刷スタート！"）
    #N       数字（例: "#200" = にひゃく）
    =N単位   連濁・促音を含む数字＋単位（例: "=1時間", "=45%", "=23分"）
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import io
import json
import pathlib
import re
import struct
import sys
import urllib.parse
import urllib.request
import wave

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCES = ["src/PrintCommentator.cpp", "src/PrinterState.cpp"]

# ソースに無いが実況で読む語（AMS の材質、ジョブ名の代わり、テスト用のあいさつ）
EXTRA_FRAGMENTS = [
    "作品",
    "PLA", "PETG", "ABS", "ASA", "TPU", "PC", "PA", "PVA", "HIPS", "PET",
    "PLA-CF", "PETG-CF", "PA-CF", "PET-CF", "PAHT-CF", "PA6-CF", "PPA-CF", "PLA-S", "PA-S",
    "こんにちは", "ありがとう", "おはよう", "おやすみ", "よろしくね", "スタックチャンだよ",
    "テスト", "聞こえる？",
]

# 連濁・促音が起きる「数字＋単位」は丸ごと合成しておく
UNIT_RANGES = {
    "%": range(0, 101),
    "分": range(1, 60),
    "時間": range(1, 49),
    "時": range(0, 24),
    "層": range(1, 100),
    "件": range(1, 11),
}

JAPANESE = re.compile(r"[\u3040-\u30ff\u4e00-\u9fff\uff66-\uff9f]")
FORMAT_SPEC = re.compile(r"%[-+ 0#]*\d*(?:\.\d+)?(?:l|ll|h)?[diuoxXsfcp]")
TOKEN = re.compile(r'("(?:\\.|[^"\\])*")|(//[^\n]*)|(/\*.*?\*/)', re.S)
LEADING_PUNCT = "。、！？!?,"


def extract_fragments() -> list[str]:
    """ソースの文字列リテラルから日本語を含むセリフ断片を集める（コメントは除外）。"""
    found: set[str] = set()
    for rel in SOURCES:
        text = (ROOT / rel).read_text(encoding="utf-8")
        for match in TOKEN.finditer(text):
            literal = match.group(1)
            if not literal:
                continue  # コメント
            value = bytes(literal[1:-1], "utf-8").decode("unicode_escape").encode("latin-1").decode("utf-8")
            value = value.strip()
            if not value or not JAPANESE.search(value) or FORMAT_SPEC.search(value):
                continue
            found.add(value)
    return sorted(found)


def build_entries() -> dict[str, str]:
    """キー → VOICEVOX に渡す読み上げテキスト"""
    entries: dict[str, str] = {}
    for fragment in extract_fragments() + EXTRA_FRAGMENTS:
        speech = fragment.lstrip(LEADING_PUNCT) or fragment
        entries[fragment] = speech
    numbers = list(range(0, 100)) + [h * 100 for h in range(1, 10)] + [t * 1000 for t in range(1, 10)]
    for n in numbers:
        entries[f"#{n}"] = str(n)
    for unit, values in UNIT_RANGES.items():
        for n in values:
            entries[f"={n}{unit}"] = f"{n}{unit}"
    return entries


def http_post(url: str, data: bytes | None, content_type: str | None) -> bytes:
    request = urllib.request.Request(url, data=data or b"", method="POST")
    if content_type:
        request.add_header("Content-Type", content_type)
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read()


def synthesize(base: str, speaker: int, text: str, speed: float, rate: int) -> list[int]:
    """VOICEVOX で合成し、16bit モノラル PCM サンプル列を返す。"""
    query_url = f"{base}/audio_query?speaker={speaker}&text={urllib.parse.quote(text)}"
    query = json.loads(http_post(query_url, None, None))
    query["speedScale"] = speed
    query["prePhonemeLength"] = 0.04
    query["postPhonemeLength"] = 0.08
    query["outputSamplingRate"] = rate
    query["outputStereo"] = False
    wav_bytes = http_post(f"{base}/synthesis?speaker={speaker}",
                          json.dumps(query).encode("utf-8"), "application/json")
    with wave.open(io.BytesIO(wav_bytes)) as wav:
        if wav.getsampwidth() != 2 or wav.getnchannels() != 1 or wav.getframerate() != rate:
            raise RuntimeError(f"unexpected wav format for {text!r}")
        raw = wav.readframes(wav.getnframes())
    return list(struct.unpack(f"<{len(raw) // 2}h", raw))


def trim(samples: list[int], rate: int, threshold: int = 250) -> list[int]:
    """前後の無音を詰める（つなぎ目の間延びを防ぐ）。前 15ms・後 40ms は残す。"""
    first = next((i for i, s in enumerate(samples) if abs(s) > threshold), 0)
    last = next((i for i in range(len(samples) - 1, -1, -1) if abs(samples[i]) > threshold), len(samples) - 1)
    start = max(0, first - rate * 15 // 1000)
    end = min(len(samples), last + rate * 40 // 1000)
    return samples[start:end] if end > start else samples


STEP_TABLE = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
]
INDEX_TABLE = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]


def adpcm_encode(samples: list[int]) -> bytes:
    """IMA-ADPCM（初期値 predictor=0, index=0）でエンコードする。"""
    predictor = 0
    index = 0
    out = bytearray((len(samples) + 1) // 2)
    for i, sample in enumerate(samples):
        step = STEP_TABLE[index]
        diff = sample - predictor
        code = 0
        if diff < 0:
            code = 8
            diff = -diff
        if diff >= step:
            code |= 4
            diff -= step
        step >>= 1
        if diff >= step:
            code |= 2
            diff -= step
        step >>= 1
        if diff >= step:
            code |= 1
        # デコーダと同じ計算で予測値を更新する（誤差が積もらないように）
        step = STEP_TABLE[index]
        d = step >> 3
        if code & 4:
            d += step
        if code & 2:
            d += step >> 1
        if code & 1:
            d += step >> 2
        predictor = predictor - d if code & 8 else predictor + d
        predictor = max(-32768, min(32767, predictor))
        index = max(0, min(88, index + INDEX_TABLE[code]))
        if i % 2 == 0:
            out[i // 2] = code
        else:
            out[i // 2] |= code << 4
    return bytes(out)


MULAW_BIAS = 0x84
MULAW_CLIP = 32635
MULAW_EXP = [0, 0, 1, 1] + [2] * 4 + [3] * 8 + [4] * 16 + [5] * 32 + [6] * 64 + [7] * 128


def mulaw_encode(samples: list[int]) -> bytes:
    """G.711 μ-law でエンコードする（1サンプル1バイト）。"""
    out = bytearray(len(samples))
    for i, sample in enumerate(samples):
        sign = 0x80 if sample < 0 else 0
        magnitude = min(-sample if sample < 0 else sample, MULAW_CLIP) + MULAW_BIAS
        exponent = MULAW_EXP[(magnitude >> 7) & 0xFF]
        mantissa = (magnitude >> (exponent + 3)) & 0x0F
        out[i] = ~(sign | (exponent << 4) | mantissa) & 0xFF
    return bytes(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--voicevox", default="http://127.0.0.1:50021", help="VOICEVOX エンジンの URL")
    parser.add_argument("--speaker", type=int, default=3, help="話者 / スタイル ID（ずんだもん ノーマル = 3）")
    parser.add_argument("--speed", type=float, default=1.1, help="話速（1.0 = 標準）")
    parser.add_argument("--codec", choices=["mulaw", "adpcm"], default="mulaw", help="圧縮方式")
    parser.add_argument("--rate", type=int, default=None, help="サンプリングレート（既定: mulaw=12000, adpcm=16000）")
    parser.add_argument("--out", default=str(ROOT / "data" / "voice.pak"))
    parser.add_argument("--cache", default=str(ROOT / ".pio" / "voice_cache"))
    parser.add_argument("--jobs", type=int, default=3)
    parser.add_argument("--list", action="store_true", help="合成せずにキー一覧だけ表示する")
    args = parser.parse_args()

    if args.rate is None:
        args.rate = 12000 if args.codec == "mulaw" else 16000
    entries = build_entries()
    if args.list:
        for key, speech in entries.items():
            print(f"{key}\t{speech}")
        print(f"{len(entries)} entries", file=sys.stderr)
        return 0

    try:
        version = urllib.request.urlopen(f"{args.voicevox}/version", timeout=5).read().decode()
    except OSError as exc:
        print(f"VOICEVOX に接続できません（{args.voicevox}）: {exc}", file=sys.stderr)
        return 1
    print(f"VOICEVOX {version} / speaker={args.speaker} / {args.codec} {args.rate}Hz / {len(entries)} entries")

    cache_dir = pathlib.Path(args.cache)
    cache_dir.mkdir(parents=True, exist_ok=True)

    def render(item: tuple[str, str]) -> tuple[str, list[int]]:
        key, speech = item
        tag = f"{args.speaker}|{args.speed}|{args.rate}|{speech}"
        cache_file = cache_dir / (hashlib.sha1(tag.encode("utf-8")).hexdigest() + ".pcm")
        if cache_file.exists():
            raw = cache_file.read_bytes()
            return key, list(struct.unpack(f"<{len(raw) // 2}h", raw))
        samples = trim(synthesize(args.voicevox, args.speaker, speech, args.speed, args.rate), args.rate)
        cache_file.write_bytes(struct.pack(f"<{len(samples)}h", *samples))
        return key, samples

    rendered: dict[str, list[int]] = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for done, (key, samples) in enumerate(pool.map(render, entries.items()), start=1):
            rendered[key] = samples
            if done % 50 == 0 or done == len(entries):
                print(f"  {done}/{len(entries)}")

    index = bytearray()
    data = bytearray()
    for key in sorted(rendered, key=lambda k: k.encode("utf-8")):
        samples = rendered[key]
        key_bytes = key.encode("utf-8")
        if len(key_bytes) > 255:
            raise RuntimeError(f"key too long: {key}")
        encoded = mulaw_encode(samples) if args.codec == "mulaw" else adpcm_encode(samples)
        index += struct.pack("<B", len(key_bytes)) + key_bytes
        index += struct.pack("<IIhBB", len(data), len(samples), 0, 0, 0)
        data += encoded

    codec = 1 if args.codec == "mulaw" else 0
    header = b"SCVP" + struct.pack("<HBBIII", 1, codec, 0, args.rate, len(rendered), len(index))
    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(header + index + data)
    seconds = sum(len(s) for s in rendered.values()) / args.rate
    print(f"wrote {out} ({out.stat().st_size / 1024 / 1024:.2f} MB, {len(rendered)} clips, {seconds:.0f} s of audio)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
