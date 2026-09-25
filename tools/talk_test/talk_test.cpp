// src/talk/ の読み上げ前処理を PC で動かすテスト用ドライバ。
// 1行1文を読み、VOICEVOX 風の記法（句 "/"、間 "、"、アクセント "'"、無声化 "_"）で出力する。
// --units を付けると、本体が再生するモーラの並びを出す（H:カ 高い音 / L:カ 低い音 / D:ス 無声 /
// P:180 間 ms。長音「ー」は前の母音に置き換え済み）。run.py --wav が音声のプレビューに使う。
//
//   talk_test <ja.dic> <en.dic> [--units] < sentences.txt

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "talk/EnReader.h"
#include "talk/JaDict.h"
#include "talk/TextReader.h"

static std::vector<uint8_t> readFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: talk_test ja.dic en.dic < lines\n");
    return 2;
  }
  const std::vector<uint8_t> ja = readFile(argv[1]);
  const std::vector<uint8_t> en = readFile(argv[2]);
  talk::JaDict jaDict;
  talk::EnDict enDict;
  if (!jaDict.open(ja.data(), ja.size())) {
    std::fprintf(stderr, "ja.dic open failed\n");
    return 1;
  }
  if (!enDict.open(en.data(), en.size())) {
    std::fprintf(stderr, "en.dic open failed\n");
    return 1;
  }
  talk::EnReader enReader;
  enReader.setDict(&enDict);
  talk::TextReader reader(jaDict, enReader);
  const bool units = argc > 3 && std::string(argv[3]) == "--units";

  std::string line;
  double totalMs = 0;
  int lines = 0;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto t0 = std::chrono::steady_clock::now();
    talk::Utterance u;
    reader.read(line, u);
    const auto t1 = std::chrono::steady_clock::now();
    totalMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
    ++lines;
    if (!units) {
      std::printf("%s\n", talk::toNotation(u).c_str());
      continue;
    }
    // BuiltinVoice::planSpeech と同じ規則でモーラを並べる
    std::string out;
    for (const talk::AccentPhrase& ph : u) {
      char vowel = 0;
      for (size_t i = 0; i < ph.moras.size(); ++i) {
        const talk::Mora& m = ph.moras[i];
        if (m.kana == "ッ") {
          out += "P:70 ";
          continue;
        }
        const bool high = talk::moraIsHigh(ph, i) || (ph.question && i + 1 == ph.moras.size());
        std::string name = talk::playableMora(m.kana);
        if (name == "ー") {
          if (!vowel) continue;
          name = talk::vowelKana(vowel);
        } else if (const char v = talk::moraVowel(name)) {
          vowel = v;
        }
        out += std::string(m.devoiced ? "D:" : high ? "H:" : "L:") + name + " ";
      }
      if (ph.pauseAfterMs) out += "P:" + std::to_string(ph.pauseAfterMs) + " ";
    }
    std::printf("%s\n", out.c_str());
  }
  std::fprintf(stderr, "%d lines, %.2f ms/line\n", lines, lines ? totalMs / lines : 0.0);
  return 0;
}
