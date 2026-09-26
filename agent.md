# stackchan-mqtt 作業引き継ぎ

最終更新: 2026-09-26（JST）。このファイルはユーザー指定の `agent.md`。
添付された前セッションの会話、現在のコード、今回の実機確認を区別して記録する。
過去の仕様は `CLAUDE.md`、利用手順は `README.md`、版ごとの差分は `CHANGELOG.md` も参照。
古い章にある「実機未確認」「3MB app」などは当時の記録であり、現状は下記を優先する。

## 1. ユーザーの目的と再開地点

当初の依頼は次の3機能をそれぞれ実装し、push とプレリリースを行うこと。

1. プリンター稼働時に自動で MQTT モードへ移行。
2. スタックチャン本体のタッチキーボードで Wi-Fi と MQTT を設定。
3. 本体だけで日本語と英語の任意の文章を読み上げる。

その後の依頼は実機への書き込み、設定ボタンの重複解消、印刷開始時の画面遷移修正、
顔より設定等の UI を優先すること、時々起きる再起動の調査・修正。
これらの実装は v2.3.2 まで完了済み。再開する作業は新機能の作り直しではなく、
v2.3.2 修正後の実機監視と、途中で切れた USB/Wi-Fi の原因確認。
今回の依頼は「内容を把握し、agent.md に詳細に書いて、続きを行って」。

前セッション末尾では COM4 復帰待ちが失敗し、その後認証切れで停止していた。
前のバックグラウンドタスクが今も動いているとは扱わない。

## 2. リポジトリとバージョン

- 作業場所: `C:\GitHub\stackchan-mqtt`。
- origin: `https://github.com/Xenoah/stackchan-mqtt.git`。
- 再開時 HEAD: `bec9c77`、作業ツリーに既存の未コミット変更なし。
- ファーム版: `src/Version.h` の v2.3.2。
- `529883e`: v2.1.0 自動 MQTT モード。
- `2b374cb`: v2.2.0 本体キーボードと接続設定。
- `6fe6c41`: v2.3.0 日本語・英語の自由文読み上げ。
- `9dd5b31`: v2.3.1 メニュー整理、顔描画競合と画面遷移修正。
- `c43097d`: 顔描画タスクを 8KB に変更、スタック監視追加。
- `bec9c77`: v2.3.2 リリース。
- 前会話では v2.1.0〜v2.3.2 は push・プレリリース公開済み。
  最新の公開先は `https://github.com/Xenoah/stackchan-mqtt/releases/tag/v2.3.2`。
  今回は公開状態を再照会しておらず、この点は前会話の記録。

## 3. ハードウェアと構成

- 本体: StackChan / M5Stack CoreS3、ESP32-S3、flash 16MiB、PSRAM。
- プリンター: Bambu Lab P1S、LAN MQTT/TLS 8883。
- PlatformIO 環境: `m5stack-cores3`、Arduino。
- 主な固定依存: M5Unified 0.2.17、m5stack-avatar v0.10.0、StackChan-BSP 1.1.0、
  M5CoreS3 1.0.1。PubSubClient ^2.8、ArduinoJson ^7.4.2。
- USB: COM4（2026-09-26 再確認、VID:PID 303A:1001）。番号は変更され得る。
- 前回・今回の本体 IP: `192.168.0.190`。DHCP のため恒久固定と仮定しない。
- Wi-Fi パスワード、プリンターのアクセスコード等を文書・コミット・出力に書かない。

### パーティション（v2.3.0 以降）

`partitions.csv` は **ASCII のみ**。PlatformIO の Windows コードページ読み込み対策。

| 領域 | offset | size | 用途 |
| --- | --- | --- | --- |
| nvs | 0x9000 | 0x5000 | Wi-Fi・各種設定・校正値 |
| otadata | 0xE000 | 0x2000 | 起動情報 |
| app0 | 0x10000 | 0x5C0000 | 5.75MiB、ファームと読み辞書、OTA スロットなし |
| spiffs | 0x5D0000 | 0xA20000 | ラベルは spiffs、実体は LittleFS / voice.pak |
| coredump | 0xFF0000 | 0x10000 | 64KiB のクラッシュ記録 |

全消去は設定・校正値・証拠を失うので診断に使わない。
更新は `pio run -t upload`、音声データの変更時のみ `uploadfs`。
v2.2以前からv2.3へ移る場合はパーティションとモーラ入り音声の両方の更新が必要。
この本体は既に v2.3.2 とモーラ入り音声を書き込み済み。

## 4. 実装の要点

### MQTT モードと画面遷移

- `src/main.cpp`: `AppMode::Printer` が表示上の MQTT。
- `updateAutoPrinterMode()` は `loop()` から呼ぶ。監視有効、状態同期済み、
  `isActive()`、Online、自動切替 ON、手動抑止なしで MQTT へ移る。
- PREPARE/印刷/一時停止が対象。発話中は切り替えず、発話後に処理。
- 完了等の非稼働が5分続いたら元のモードへ戻る。通信断だけで印刷終了とは扱わない。
- `selectModeManually()` は自動復帰の記録を消す。印刷中に別モードを手動選択すると
  そのジョブ中の自動 MQTT を抑止する。テストで切り替えた場合もこの仕様に注意。
- v2.3.1以降 MQTT に入ると **プリンター詳細画面** を優先して開く。
  詳細画面をタップで閉じると顔と HUD。モードと表示中画面は同一概念ではない。
- `BambuMqttClient.*`: core 0 の専用 12KB タスク、再接続バックオフ、
  定期 pushall、共有状態はミューテックスを介した snapshot。
- `PrinterState.*` / `PrinterJson.*`: 差分通知の状態保持・Web JSON。
- `PrintCommentator.*`: 状態変化を実況キューへ。HTTP ハンドラ内で同期発話しない。
- `FaceHud.*`: 顔スプライト内部で描画。`PrinterScreen.*` は詳細画面。

### 本体設定 UI

- `TouchKeyboard.*`: QWERTY、大文字/固定、記号、IP 用数字、伏せ字表示、長押し削除。
- `SetupUi.h`: ボタン、色、タッチの押下/離し判定などの共通部品。
- `SetupScreens.*`: Wi-Fi スキャン→接続テスト→保存→再起動、プリンター設定。
- Wi-Fi パスワードとプリンターアクセスコードの空欄は保存済み値を維持する場面あり。
- v2.3.1でメニューを3列×2段＋閉じるへ整理。「設定」に Wi-Fi/プリンター/接続情報を集約。
- プリンター未登録時の MQTT/プリンタータイルは登録場所を案内する。
- `ConfigPortal.*`: NVS と設定・Web API、`WebPages.h`: ダッシュボード。
- `serviceApp()` は設定 UI / TTS 中も呼ばれるため、入れ子の副作用に注意。

### 内蔵音声

- `BuiltinVoice.*`: 文全体を既存クリップで読めれば自然な句音声、無理ならモーラ合成。
- `src/talk/`: Arduino 非依存の読み処理。`TextReader`、`JaReader`/`JaDict`、
  `EnReader`、`Numbers`、`Kana`、`BlockStore`。
- 日本語辞書は NAIST-jdic 由来、約2.8MB。64語単位の raw deflate、256×256 連接コスト。
- 英語辞書は CMUdict 由来、約0.66MB。頻出語＋綴りからの推定、カタカナ発音。
- `dict/ja.dic` / `dict/en.dic` は追跡済み、`board_build.embed_files` で組み込む。
- `tools/make_dict.py`: 辞書の再生成。通常のビルドで再生成する必要はない。
- `tools/make_voice_pack.py`: VOICEVOX から `data/voice.pak` を生成。
  音声は約9.1MiB、982クリップ、μ-law 12kHz。モーラ141音の高低・無声化を含む。
- `data/voice.pak`、生成キャッシュ `.pio/` は gitignore。
- モーラ接続は5msクロスフェード。任意の文章が対象だが、全語を正しく読める保証ではない。
  固有名詞・未知語・音声の自然さには制約がある。
- サーバー TTS 失敗時に内蔵へフォールバックし、3分間サーバー再試行を休止。
  内蔵のみなら `tts_engine=builtin` だが、依頼なく保存設定を変更しない。
- Gateway 用 `__CURRENT__` / `__REASK_LAST__` をそのまま読まない。
  `__SAY__` は本文を読む。`gateway/` は任意の外部 LLM/TTS 経路。
- ライセンス・音声表記は `THIRD_PARTY_NOTICES.md` を維持する。

## 5. 過去のクラッシュと修正

### v2.3.1: 設定画面と顔の同時描画

キーボードの `pushSprite` / `spiEndTransaction` で assert。
m5stack-avatar の `setExpression()` が停止中でも描画を再開して LCD を競合させていた。
`AvatarFaceController` で UI 中の表情を保留し、再開時に反映。
顔描画をフレーム間で止める処理、顔の優先度0（メインループは1）を導入。

### v2.3.2: 顔描画スタック不足

コアダンプの DebugException / stack canary と `drawLoop` のスタック位置から確認。
ライブラリ固定2KBでは HUD/字幕を描くと不足。`replaceDrawTask()` で
8KB の `faceDraw` に差し替え、`m5avatar::drawTaskHandle` も置き換える。
`[heap] ... stack face=... loop=... mqtt=...` を15秒周期で出力。
これらは ESP32 環境でのスタック最小残量（バイト）、現在瞬間の使用量ではない。

### 前セッションの検証結果（今回の実測と混同しない）

- 1回目の30分記録: PANIC/再起動なし。モード切替5往復、テスト発話、印刷/キャンセルを観測。
- face 最小残量5,532B、8,192B中の最大使用2,660B。元の2KBを超えていた。
- ヒープは終盤約152,412Bで一定。
- 大半はプリンター詳細画面で顔描画停止中だったため、HUD を出し続ける長時間試験は未完。
- v2.3.2 の2回目記録: 23:49:53 開始、翌00:11:07にCOM4消失。
  直前は free=153,792、min=145,372、face=6,380、loop=4,988、mqtt=9,168。
- 00:19:54の終了まで復帰せず、Wi-Fiにも到達できなかった。
- **電源断/USB抜けは仮説**。両方不通という観測だけでは、ハングや電源系不具合まで除外できない。
  「クラッシュではないと確定」とは記録しない。復帰後の reset_reason / ダンプで裏付ける。
- 前の監視は一時フォルダのスクリプトと bash の tail/grep に依存し、切断通知を大量に繰り返した。
  COM4待ちの bash ループも fork 失敗。その後の PowerShell 待ちも exit 4 で終了。

## 6. 今回の再開時確認（2026-09-26）

- COM4 の存在と、DTR/RTS を False にして開いた際の `[heap]` 出力を確認。
- HTTP `192.168.0.190` は通常 sandbox から失敗したが、ネットワーク権限付き実行では応答。
  **sandbox の接続失敗を本体停止と誤認しない**。
- `/status`: v2.3.2、稼働時間00:21:30を確認（08:48頃）。
- `/api/status`: `reset_reason=POWERON`、Wi-Fi接続済み、LOCAL LLM、監視有効。
  free_heap 約191〜192KB、最小188,712B（APIアクセス後）、PSRAM 8,103,315B。
- シリアル: face=6,380B、loop=4,988B、mqtt=10,060B。
- `/api/printer`: enabled=true、auto_mode=true、link=connecting、synced=false、phase=UNKNOWN。
  プリンターの電源・到達性は未確認。印刷開始の自動切替を現在実機で確認したとは言えない。
- TTS は `voicevox_compatible` のまま。設定変更は行っていない。
- `POWERON` は今回の起動が電源投入によることを示す。前夜からの全経緯を単独で証明するものではない。
- 08:53頃、coredump 領域を読出し、意図した再起動を1回実施。
  保存先 `.pio/diagnostics/20260926/coredump.bin`（64KiB）。対応確認用の現行 ELF も同フォルダに保存。
- ダンプ全体 SHA256: `e8a9f7033bde353f41bbf49c56cb0883218dd8d4f6463fa790a1324362f929b4`。
  既知の `coredump2.bin`（前回）とはファイル全体が異なる。
- `esp_coredump` が記録したアプリ識別値は `749070a47ed7589e`。
  前セッションの `dist/v2.3.1/firmware.bin` の app descriptor の ELF SHA256
  `749070a47ed7589e1e4e483a1619935101d5f29d8fb2c32573478ab6ec9e389e` と一致。
  現行 v2.3.2 は `05a8953beb2bbea5c2ffa9679de74697da2a92a282a060687980b991ab1f8d68`。
  **残っているのは v2.3.1 の記録。v2.3.2 の新規クラッシュを示すダンプではない**。
  現行 ELF を使った解析は不一致として正しく拒否された。対応する旧 ELF がないため詳細な
  スタックトレースの再解釈は行わず、バイナリの識別値とダンプ内 `drawLoop` 文字列を確認した。
- 再起動後の MQTT は `connect failed state=-2`（TCP/TLS 接続失敗）。
  これだけではプリンター電源OFF、IP変更、ネットワーク不通の区別はできない。
- 08:56:31〜09:06:31に新しいツールで600秒監視を完了。
  `monitor.log` / `monitor.summary.json`: 接続1回、切断0、起動行0、異常候補行0、heapサンプル40。
- 09:04:13にプリンターの自動再接続が成功。APIで `online / synced=true / IDLE` を確認。
  MQTT/TLS接続に伴い free_heap は約192KBから約153KBへ変化し、その後約152〜153KB。
  この段差を経時的なリークとは扱わない。最小ヒープ145,496B、最小face残量6,380B、
  loop=4,924B、mqtt=8,204B。
- esptoolの読出しに伴うリセット後はAPIの `reset_reason=UNKNOWN`。
  診断操作前の `POWERON` と区別する。監視中に新たな起動ログ/USB切断はない。
- 09:10頃、待機状態を確認してWeb APIで MQTT/LOCAL LLM を2往復。
  各回、受付応答だけでなく `/api/status` のモード変更を確認し、最初のLOCAL LLMへ戻した。
- `/api/speak` で「Hello! 今日は3時に1分だけ休憩して、PETGで印刷するよ。」を送信。
  `speaking=true→false` と `[voice] builtin: 40 steps` を確認。
  `/status` でも982クリップ・自由文対応・v2.3.2を確認。音の自然さの聴覚評価はしていない。
- この動作確認のため09:09:01〜09:12:01に180秒の追加監視を完了。
  `functional.log` / `functional.summary.json`: 接続1回、切断0、起動行0、異常候補行0、heapサンプル12。
  終了時free_heap=150,300B、最小ヒープ145,496B、最小face残量6,188B、loop=4,924B、mqtt=8,204B。
  10分監視と合わせて13分を記録（2区間の間には未記録時間あり）。監視は両方終了済み。
- 実際の印刷開始、自動モード復帰、タッチキーボードの目視・操作は今回未検証。
  プリンターへの印刷開始/停止コマンドや保存設定の変更は行っていない。

### 今回の検証

- `pio run -e m5stack-cores3`: SUCCESS。
  RAM 74,400 / 327,680B（22.7%）、Flash 5,197,981 / 6,029,312B（86.2%）。
- `python -m py_compile tools/monitor_serial.py`: 成功。
- 実在しないCOMポートで3秒監視: 2回再試行、接続待ち表示は1回、指定時間で終了しJSON保存。
- 模擬USB切断→1回ポート不在→再接続: 分割UTF-8の復元、切断前の途中行を次接続と混ぜないこと、
  各ポートclose、異常候補とメモリ集計、既存ログを上書きしないことを確認。
- `git diff --check`: 成功。
- ファームの追加修正を要する新規障害は今回の観測では見つからなかった。
  今回の変更対象は引き継ぎ・診断ツール・README/CHANGELOG。ファーム公開版は引き続きv2.3.2。

### 今回追加した監視ツール

- `tools/monitor_serial.py` は前の一時フォルダの監視処理を再利用可能にしたもの。
- `--port` / `--duration`（秒、既定1800）/ `--output` / `--baud`。
- DTR/RTS=False にしてポートを開く。単調時計で期間管理し、USB切断は2秒間隔で再試行。
- 接続待ちを毎回エラー表示しない。受信UTF-8を行単位に組み立て、切断前の断片も記録。
- ログは `.pio/diagnostics/`、JSONは同名の `.summary.json`。既存ファイルの上書きは拒否。
- Ctrl+Cでもポートを閉じ集計を保存。OSによる強制終了では終了集計が残らない場合がある。
- fault_lines は異常候補の行数、boot_lines は起動文の行数。クラッシュ回数と断定しない。
- ログには本体の設定値や発話が出る場合があるため、未確認の生ログを公開しない。

## 7. 開発・確認コマンド（PowerShell）

```powershell
# PATH に pio が無い場合もこの環境の既存インストールを使える
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e m5stack-cores3
python -m serial.tools.list_ports -v
python tools/monitor_serial.py --port COM4 --duration 1800

# ファーム更新が必要なときのみ。シリアル監視を閉じてから実行
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -t upload --upload-port COM4
# パック更新が必要なときのみ
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -t uploadfs --upload-port COM4

# ホストでの日本語・英語の読み確認（ziglang、初回はminiz取得が必要）
python tools/talk_test/run.py "今日は3時にPETGで印刷するよ"
python tools/talk_test/run.py --wav .pio/sample.wav "こんにちは、Hello!"
```

- Python は pyserial / esptool 5.2.0 / ziglang インストール済み。
- PowerShell の日本語読み書きは明示的に UTF-8。`Get-Content -Encoding UTF8`。
- PlatformIO はユーザーディレクトリのロック/キャッシュへの書込み権限が必要なことがある。
- 現行 ELF は `.pio/build/m5stack-cores3/firmware.elf`。
  ダンプ解析前に対応 ELF を保存する。違う版の ELF でアドレスを断定しない。
- 診断ログ・ダンプは `.pio/diagnostics/` 等の無視対象へ保存。機密を含む可能性があるため公開しない。
- ダンプ読出し例（本体が再起動する。事前に reset_reason / 稼働時間を取得）:

```powershell
python -m esptool --chip esp32s3 --port COM4 --baud 921600 read-flash 0xFF0000 0x10000 .pio/diagnostics/coredump.bin
```

## 8. 残課題と次回の判断

1. 保存ダンプの版の確認は完了。v2.3.1 のものなので現行 v2.3.2 での新規障害とは扱わない。
2. 復帰後10分の監視は完了。次回も `tools/monitor_serial.py` で記録し、
   切断/再接続と監視自体の終了を区別する。終了済みの監視を継続中と案内しない。
3. プリンターが Online になったら、実際の印刷開始→MQTT詳細画面、自動復帰、
   手動抑止、本体設定/キーボードの表示を確認する。依頼なく印刷を開始/停止しない。
4. 顔＋HUDを出し続ける長時間監視と設定画面往復は引き続き重要。
   コード/ログだけで見た目や音声の自然さを確認済みにしない。
5. 新規PANICがあれば最新ダンプと対応ELFで調査。証拠なしに追加のファーム修正を積まない。
6. 新たな機能・不具合修正をリリースするときは README/CHANGELOG/Version、
   バイナリと SHA256SUMS、書込み確認をそろえてから従来どおり push/プレリリース。
   引き継ぎ文書や診断ツールだけの変更でファーム版を進める必要はない。
