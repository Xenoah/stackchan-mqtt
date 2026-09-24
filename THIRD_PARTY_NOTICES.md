# Third-Party Notices

このプロジェクトは、以下を含むオープンソースソフトウェアを利用しています。
各ライブラリの最新の利用条件は、リンク先の公式リポジトリも確認してください。

## M5Stack-Avatar

- Project: [stack-chan/m5stack-avatar](https://github.com/stack-chan/m5stack-avatar)
- Version: `0.10.0`
- Copyright: Copyright (c) 2018 Shinya Ishikawa
- License: MIT License

顔、表情、目、口、視線、呼吸、配色、吹き出し、変形の描画に使用しています。

```text
MIT License

Copyright (c) 2018 Shinya Ishikawa

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## M5Stack libraries

- [M5Unified](https://github.com/m5stack/M5Unified)
- [M5GFX](https://github.com/m5stack/M5GFX)
- [StackChan-BSP](https://github.com/m5stack/StackChan-BSP)
- [M5CoreS3](https://github.com/m5stack/M5CoreS3)
- [M5Unit-NFC](https://github.com/m5stack/M5Unit-NFC)

## IRremoteESP8266

- [crankyoldgit/IRremoteESP8266](https://github.com/crankyoldgit/IRremoteESP8266)

## PubSubClient

- Project: [knolleary/pubsubclient](https://github.com/knolleary/pubsubclient)
- Version: `^2.8`
- License: MIT License

Bambu Lab プリンターとの MQTT 通信に使用しています。

## ArduinoJson

- Project: [bblanchon/ArduinoJson](https://github.com/bblanchon/ArduinoJson)
- Version: `^7.4.2`
- License: MIT License

プリンターの状態 JSON の解析と、Web API の JSON 生成に使用しています。

## Bambu Lab LAN MQTT

プリンターとの通信方式（トピック、`pushall`、`bblp` ユーザー）は
[Xenoah/ESP32-bambu-MQTT](https://github.com/Xenoah/ESP32-bambu-MQTT) を踏襲しています。
各フィールドの意味は OpenBambuAPI / ha-bambulab などコミュニティの調査を参考にしました。
Bambu Lab は Bambu Lab 社の商標であり、本プロジェクトは同社とは関係ありません。

## VOICEVOX

VOICEVOXおよびキャラクター音声は本リポジトリへ同梱していません。
利用者が別途用意したVOICEVOX EngineへHTTP接続します。

- [VOICEVOX公式サイト](https://voicevox.hiroshiba.jp/)
- [VOICEVOX Engine](https://github.com/VOICEVOX/voicevox_engine)

音声利用時はVOICEVOXおよび各キャラクターの利用規約に従ってください。
