# StackChan × Termux Local LLM Gateway

ブラウザ操作型ロボ端末プロジェクト。StackChan を「薄い再生クライアント」にし、
Android Termux 上の Gateway が LLM 推論・音声生成・履歴管理をすべて担う。
音声認識 (STT) は使わない。

- ベース: release tag **β3.5.0**（simple_wav 方式）
- main ブランチの ConversationController/STT/LLM 分離実装は**使わない**

---

## 全体フロー

1. Android Termux で Gateway を起動（llama-server + FastAPI）
2. スマホ/PC ブラウザから Gateway UI を開く
3. パスワードでログイン
4. 質問入力 + 回答言語/長さ/TTS音声/ピッチ/音量/速度 を設定
5. 「送信して喋らせる」
6. Gateway が LLM で回答生成 → 同じテキストから WAV 生成
7. Gateway が StackChan の `POST /api/speak` を呼ぶ
8. StackChan が Gateway の `POST /synthesis` から WAV を取得し発話
9. （A ボタン）StackChan が `__REASK_LAST__` を /synthesis に POST → 最後の質問を再 LLM 処理して発話

### 重要仕様
- 表示回答と発話内容は意味として一致させる
- デフォルト日本語。英語選択時は表示も発話も英語
- 日本語は表示が漢字混じり、音声生成時のみ漢字→かな変換可。**読み上げ用テキストも UI に表示**
- 設定は確実に永続保存。履歴から再発話/声だけ変更/削除が可能
- パスワードログインでセキュリティ確保

---

## Part A: StackChan ファームウェア（最小変更）

β3.5.0 をベースに以下のみ追加・変更する。

| # | 変更 | 状態 |
|---|------|------|
| A1 | `M5.Speaker.setVolume(160)` → `255` | ✅ |
| A2 | `speakConfiguredText()` を `speakText(text)` にリファクタ（A ボタンは従来通り設定テキストを発話） | ✅ |
| A3 | `POST /api/speak` 追加。busy なら `{"ok":false,"error":"busy"}`、受付なら `{"ok":true}` | ✅ |
| A4 | `GET /api/status` 追加。`ok/connected/speaking/ip/tts_host/tts_port/tts_engine` を返す | ✅ |
| A5 | A ボタン運用: 設定の「Text to speak」に `__REASK_LAST__` を入れる（コード変更不要） | ✅ |

### 設計メモ
- `/api/speak` は WebServer ハンドラ内で同期再生せず、`pendingApiSpeak` フラグを立てて即 `{"ok":true}` を返す。実際の TTS 再生は `loop()` が拾って実行（再生はブロッキングのため）。
- `/api/speak` のデフォルト発話テキストは `__CURRENT__`（Gateway は current.wav を返す＝再 LLM しない）。`text` フォーム引数で上書き可。
- A ボタン → `speakConfiguredText()` → 設定テキスト（`__REASK_LAST__`）を /synthesis に POST → Gateway が再 LLM。
- `speaking` 状態は `ConfigPortal` にプローブ用コールバックで渡す。

### StackChan 側 想定設定
- TTS Engine Type: `simple_wav`
- TTS Host: Android スマホの LAN 内 IP
- TTS Port: `50021`
- Speaker / Style ID: `3`
- Text to speak: `__REASK_LAST__`

---

## Part B: Android Termux Gateway（FastAPI）

`gateway/` 配下に実装。

- llama.cpp `llama-server` → `http://127.0.0.1:8080/v1/chat/completions`
- Gateway listen → `0.0.0.0:50021`
- TTS: `espeak-ng`（ja は pykakasi で漢字→かな変換）
- 保存先:
  - `~/stackchan_gateway/settings.json`
  - `~/stackchan_gateway/history/`
  - `~/stackchan_gateway/current.json` / `current.wav`
  - `~/stackchan_gateway/logs/`

### settings.json 項目
`password_hash, password_salt, session_secret, stackchan_host, stackchan_port,
voice_lang, tts_voice, pitch, volume, speed, answer_mode, kanji_to_kana, auto_speak,
max_history, system_prompt`

デフォルト: stackchan_port=80, voice_lang=ja, tts_voice=default, pitch=80, volume=200, speed=150,
answer_mode=short, kanji_to_kana=true, auto_speak=true, max_history=50。
初期パスワード `stackchan`（salt+sha256 で保存、平文不可）。

### エンドポイント
`GET /`, `POST /login`, `POST /logout`, `GET /status`, `POST /settings`,
`POST /ask`, `POST /voice/rebuild`, `GET /current.wav`,
`POST /synthesis`（StackChan 互換: `__REASK_LAST__` で再 LLM、通常は current.wav）,
`POST /stackchan/speak`, `GET /history`,
`POST /history/{id}/speak`, `POST /history/{id}/revoice`, `POST /history/{id}/delete`

### 音声生成
- ja: `espeak-ng -v ja`（必要に応じ pykakasi で読み変換、読み上げ用テキストを保存・表示）
- en: `espeak-ng -v en-us`
- tts_voice: `default`, `f1`, `f2`, `m1`, `m2`, `m3`（espeak-ng の `+f1` 等を付与）
- pitch `-p` 0–99（既定80）, volume `-a` 0–200（既定200）, speed `-s` 80–260（既定150）

### 完了条件
1. β3.5.0 ベースでビルド可 ✅
2. スピーカー音量 255 ✅
3. `/api/speak` 追加 ✅
4. ブラウザ送信で A ボタン不要で発話 ✅（/ask → set_current → /api/speak → /synthesis）
5. A ボタンで最後の質問を再 LLM 発話 ✅（`__REASK_LAST__` → process_question）
6. TTS音声/ピッチ/音量/速度をブラウザ変更可 ✅（設定保存 + revoice）
7. 音声だけ再生成可 ✅（/voice/rebuild, /history/{id}/revoice）
8. settings.json 永続保存 ✅（原子的書き込み）
9. 履歴から再発話/声変更/削除 ✅
10. パスワードログイン ✅（salt+sha256, 署名Cookie）
11. 表示回答と発話一致 ✅（answer=表示, speech_text=読み上げ。両方UI表示）
12. デフォルト日本語 ✅

### 既知の制約
- `/synthesis` の `__REASK_LAST__` は LLM 推論を**同期実行**する。StackChan(simple_wav)
  の HTTP タイムアウトは約 30 秒（`VoiceVoxClient.cpp` `kNetworkTimeoutMs`）なので、
  モデルが遅いと A ボタン再発話がタイムアウトする可能性がある。短い回答長(`hitokoto`/`short`)
  と小型モデル推奨。ブラウザ送信フローは LLM を先に終えてから `/api/speak` を呼ぶため影響なし。

---

## Part C: 安定化・表現強化・カメラ目線（2026-06-28）

| # | 項目 | 内容 | 状態 |
|---|------|------|------|
| C1 | クラッシュ原因修正 | アバター描画の `init(8)`→`init(1)`。m5avatar は毎フレーム 320x240 スプライトを createSprite/deleteSprite する。colorDepth=8 だと約77KB/フレームの連続確保が必要で、WiFi/TTS/HTML 確保とぶつかりヒープ断片化→確保失敗→クラッシュ（全状況・放置中も発生）。colorDepth=1 で約9.6KB/フレームに削減し断片化耐性を8倍に。見た目は StackChan 本来の2トーン（パレットの前景/背景色は反映） | ✅ |
| C2 | クラッシュ診断 | 起動時の再起動理由（PANIC/BROWNOUT/TASK_WDT 等）と空きヒープ/最小ヒープ/最大連続ブロック/PSRAM を `/status` ページ・`/api/status`・シリアル(15秒毎)に表示。次回クラッシュ後に `/status` の **Last Reset** を見れば原因種別が分かる | ✅ |
| C3 | 漢字読み上げ | Gateway で読み上げ前にマークダウン/装飾記号を除去（`sanitize_for_speech`）してから pykakasi で かな 変換。espeak が「アスタリスク」等と読むのを防止 | ✅ |
| C4 | ピッチ上げ | Gateway デフォルト pitch 80→90（UIで 0–99 調整可、最大99） | ✅ |
| C5 | 顔の動きを大げさに | 口パク開き具合を1.5倍に増幅 + Normal変形時に呼吸ズーム（±6%, 約2.4秒周期）で顔全体がゆっくり拡大縮小 | ✅ |
| C6 | カメラ目線 | CoreS3 内蔵カメラ(GC0308)を QQVGA RGB565 で取得し、輝度重心の方向へ視線(`setGaze`)。4fps・発話中/メニュー中は休止・初期化失敗時は自動無効。設定ページの **Camera gaze** で ON/OFF（NVS保存） | ✅ |

### カメラ目線の注意（重要）
- CoreS3 ではカメラの SCCB が**内部I2C（タッチ/IMU/PMICと同じ 11/12 ピン）を共有**する。
  `esp_camera_init` 前に `M5.In_I2C.release()` が必要なため、**カメラ有効時はタッチ操作が
  効かなくなる可能性**がある。その場合はスマホの設定ページで **Camera gaze を OFF→保存**
  すれば、再起動後カメラを初期化せずタッチが復帰する（起動時のサーボ選択ダイアログは
  カメラ初期化より前なので影響しない）。
- 左右が逆に見える場合は `main.cpp` の `kCamGazeHSign` を `-1` にする。
- カメラは PSRAM を使用（QQVGA 2バッファ ≈ 76KB）。本体の動作はクラッシュ修正(C1)後に確認推奨。

---

## Part D: 設定画面チャットUI・ゲーミングRGB（2026-06-28 / v1.1.5）

| # | 項目 | 内容 | 状態 |
|---|------|------|------|
| D1 | Local LLM Chat UI | ConfigPortal の設定ページ（`GET /`）に「Local LLM Chat」カードを追加。textarea（初期値「自己紹介して」）+ ボタン「LLMで回答して喋る」+ status + 回答用 `<pre>`。ブラウザの `fetch` が `http://<tts_host>:<tts_port>/ask` へ `application/x-www-form-urlencoded` の `text` をPOSTし、JSON（`question`/`answer`/`stackchan`）を整形表示。**ESP32側に中継APIは作らない**（メモリ節約のためJSON処理はブラウザ側）。送信中 `thinking...`、エラー時 `fetch error: 詳細`。Gateway URL は保存済み `config_.ttsHost`/`ttsPort` をJSへ埋め込み。Gateway は `Access-Control-Allow-Origin: *` 前提 | ✅ |
| D2 | ゲーミングRGB（画面） | `AvatarFaceController` に虹色循環を追加。`update()` 末尾で `updateGamingPalette()`（約6秒で1周・約30fps throttle）。colorDepth=1 の2トーン描画に合わせ、背景=虹色／前景=補色で循環。`setGamingRgb(bool)`/`isGamingRgb()`/`gamingHue()` を公開。OFFで通常パレットへ即復帰 | ✅ |
| D3 | ゲーミングRGB（LED） | `main.cpp` の `serviceApp()` から `updateGamingLed()` を毎フレーム実行。色相は `avatarFace.gamingHue()` と同期、明度控えめ。ステータスLEDは `showStatusLed(r,g,b,holdMs)` 経由に置換し、指定時間だけ色を保持してから虹色へ戻す（TTS=青/エラー=赤(3秒)/通常=緑 等の意味を維持） | ✅ |
| D4 | 設定永続化・UI | `AppConfig.gamingRgb`（既定true）を追加。NVSキー `gaming_rgb` で load/save。設定ページにチェックボックス「Gaming RGB (rainbow face & LED)」、`/status` の App カードに On/Off 表示。起動時 `configPortal.begin()` 後に `avatarFace.setGamingRgb(config.gamingRgb)` で反映 | ✅ |

---

## 進捗ログ
- 2026-06-27: β3.5.0 に復帰確認（HEAD == β3.5.0, working tree clean）。本ドキュメント作成。
- 2026-06-27: ファームウェア A1–A5 実装・ビルド成功（RAM 18.1%, Flash 18.3%）。
- 2026-06-27: Gateway 実装完了（app.py / ui.py / run.sh / stop.sh / requirements / README）。
  TestClient で 16/16 のスモークテスト合格（認証・設定・/ask・/synthesis・履歴・revoice・削除）。
- 2026-06-28: ユーザ報告「全状況でしばらく動かすと落ちる」を調査→アバターの毎フレーム77KB
  スプライト確保が断片化要因と特定し colorDepth=1 へ修正(C1)。診断表示(C2)、漢字読み整形(C3)、
  ピッチ既定上げ(C4)、口/呼吸の誇張(C5)、カメラ目線(C6) を実装。ファーム build 成功
  （RAM 19.5%, Flash 19.1%）。Gateway py_compile OK。
- 2026-06-28: 設定画面に Local LLM Chat UI(D1) を追加（ブラウザ直 fetch、ESP32中継なし）。
  ゲーミングRGB(D2–D4) を実装：顔(画面)＋本体LEDを虹色循環、設定ON/OFF・NVS保存。
  ファーム build 成功（RAM 18.2%, Flash 18.4%）。**v1.1.5（Gaming RGB Edition）としてリリース**。
  README.md / CLAUDE.md / CHANGELOG.md を更新。
