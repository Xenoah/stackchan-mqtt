#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

#include <functional>

// アプリ全体で共有する設定値。NVSから読み込み・保存される。
struct AppConfig {
  String wifiSsid;                              // 接続先WiFiのSSID
  String wifiPassword;                          // WiFiパスワード
  String ttsHost = "192.168.1.2";              // TTSサーバのIPアドレス
  uint16_t ttsPort = 50021;                    // TTSサーバのポート番号
  String ttsSpeaker = "3";                     // 話者ID（VoiceVox: ずんだもんノーマル=3）
  String ttsEngineType = "voicevox_compatible"; // TTSエンジン種別
  String speechText =
      "Hello! I am Zundamon. I can now talk using StackChan!"; // Aボタンで話すデフォルトテキスト

  // カメラを使って明るい方向に目線を向ける機能。
  // CoreS3 ではカメラと内部I2C（タッチ等）がバスを共有するため、
  // タッチが効かなくなる/不安定な場合はこのスイッチをOFFにする。
  bool cameraGaze = true;

  // ゲーミングRGB演出。ONにすると顔（画面）と本体LEDが虹色にゆっくり循環する。
  // OFFにすると通常の2トーン表示＋ステータス色LEDに戻る。
  bool gamingRgb = true;
};

// /statusページに表示するアプリ実行時の状態。
// main.cppからsetRuntimeStatus()で定期的に更新する。
struct RuntimeStatus {
  String appMode = "---";       // 現在のアプリモード名（"LOCAL LLM" 等）
  bool servoCalibrated = false; // サーボキャリブレーション済みかどうか
  bool imuCalibrated = false;   // IMUキャリブレーション済みかどうか

  // --- 診断情報（クラッシュ調査用）---
  String resetReason = "";      // 直近の再起動理由（PANIC/BROWNOUT/WDT等）
  uint32_t freeHeap = 0;        // 現在の空きヒープ（バイト）
  uint32_t minFreeHeap = 0;     // 起動以降の最小空きヒープ（バイト）
  uint32_t maxAllocHeap = 0;    // 確保可能な最大連続ブロック（フラグメント指標）
  uint32_t freePsram = 0;       // 空きPSRAM（バイト）
  bool cameraActive = false;    // カメラ目線が動作中かどうか
};

// WiFi接続・設定用Webサーバを管理するクラス。
//
// 動作フロー:
//   begin() 呼び出し時、保存済みSSIDで接続を試みる。
//   - 接続成功 → STAモードのままWebサーバ起動（ポート80）
//   - 接続失敗 → APモード（StackChan-Setup-XXXXXX）でWebサーバ起動
//
// ページ:
//   GET /       設定フォーム（現在値を事前入力、パスワードは伏せる）
//   POST /save  設定を保存して再起動
//   GET /status システム状態の確認ページ（5秒自動更新）
class ConfigPortal {
 public:
  // NVSから設定を読み込み、WiFi接続またはAPを起動する。
  // 戻り値: WiFi接続成功=true、APモード=false
  bool begin();

  // Webサーバのリクエストを処理する（メインループから毎フレーム呼ぶ）
  void update();

  // セットアップ用APが起動中かどうか
  bool isPortalActive() const;

  // WiFiに接続中かどうか
  bool isConnected() const;

  // 現在の設定値を返す
  const AppConfig& config() const;

  // デバイスのIPアドレスを返す（AP時は192.168.4.1、STA時はDHCPアドレス）
  IPAddress localIp() const;

  // セットアップ用APのSSID名を返す（例: "StackChan-Setup-AABBCC"）
  String accessPointName() const;

  // SETTINGSメニュー用APを追加起動する（既存WiFi接続を維持したまま）
  // WIFI_AP_STAモードでAPを起動し、192.168.4.1でWebUIにアクセス可能にする
  void startSettingsAp();

  // SETTINGSメニュー用APを停止し、STAモードに戻す
  void stopSettingsAp();

  // SETTINGSメニュー用APが起動中かどうか
  bool isSettingsApActive() const;

  // APのIPアドレスを返す（通常 192.168.4.1）
  IPAddress settingsApIp() const;

  // /statusページに表示するアプリ実行時の状態を更新する（serviceApp()から定期呼び出し）
  void setRuntimeStatus(const RuntimeStatus& status);

  // POST /api/speak のハンドラを登録する。
  // 引数: 発話テキスト（空なら呼び出し側で既定値を使う）。
  // 戻り値: 受け付けた=true、話し中などで拒否=false（busy）。
  void setSpeakRequestHandler(std::function<bool(const String&)> handler);

  // GET /api/status の speaking フィールド用に、現在の発話状態を返すプローブを登録する。
  void setSpeakingProbe(std::function<bool()> probe);

 private:
  Preferences preferences_;          // ESP32 NVS（不揮発ストレージ）アクセス
  WebServer server_{80};             // ポート80のHTTPサーバ
  AppConfig config_;                 // 現在の設定値
  RuntimeStatus runtimeStatus_;      // アプリ実行時の状態（ステータスページ用）
  bool portalActive_ = false;        // セットアップAPが起動中かどうか
  bool settingsApActive_ = false;    // SETTINGSメニュー用APが起動中かどうか
  String accessPointName_;           // APのSSID名（例: "StackChan-Setup-AABBCC"）

  // Gateway 連携用コールバック
  std::function<bool(const String&)> speakRequestFn_; // POST /api/speak 受付
  std::function<bool()> speakingProbe_;               // GET /api/status の speaking

  // NVSから設定を読み込む
  void load();

  // NVSに設定を保存する
  void save();

  // 保存済みSSID/パスワードでWiFi接続を試みる（タイムアウト15秒）
  bool connectWifi();

  // セットアップ用APを起動する（接続失敗時）
  void startPortal();

  // APのSSID名を初期化する（未設定の場合のみ、MACアドレス末尾6桁を使用）
  void ensureAccessPointName();

  // WebサーバのURLルートを登録する（GET /、POST /save、GET /status）
  void registerRoutes();

  // 設定WebページのHTMLを生成する（現在値を事前入力、パスワードは伏せる）
  String pageHtml(const String& message = "");

  // システム状態ページのHTMLを生成する（5秒自動更新）
  String statusHtml();

  // Local LLM Chat カード（Gateway /ask をブラウザから直接叩くフロントエンド）を生成する
  String chatHtml();

  // WiFiスキャン結果を<option>タグのリストとして返す
  String wifiOptionsHtml();

  // GET /api/status が返す JSON 文字列を生成する
  String apiStatusJson();
};
