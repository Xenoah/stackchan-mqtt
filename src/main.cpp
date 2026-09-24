// StackChan Codex - メインエントリポイント
//
// M5Stack CoreS3 + StackChanボディを使ったデスクロボットのメインファームウェア。
//
// 主な機能:
//   - VoiceVox互換またはSimpleWav TTSサーバへのWiFi経由音声合成
//   - アバターフェース（表情・顔型・カラーパレット・目パターン・変形）
//   - PCM波形解析によるリアルタイム口パク同期（30ms間隔）
//   - LEVEL HOLDモード: IMU加速度を使ったPID制御でサーボを水平維持
//   - タッチスクリーンの下スワイプでモード・設定メニューを開く
//   - 頭部タッチセンサで表情・パレット等を操作
//   - WiFiでアクセス可能な設定Webサーバ（ポート80）

#include <M5Unified.h>
#include <esp_camera.h>
#include <esp_system.h>
#include <math.h>

#include "AvatarFaceController.h"
#include "CalibrationController.h"
#include "ConfigPortal.h"
#include "VoiceVoxClient.h"
#include "hardware_features.h"

// グローバルオブジェクト（各サブシステムのコントローラ）
AvatarFaceController avatarFace;          // アバター描画・アニメーション管理
CalibrationController calibrationController; // サーボ・IMUキャリブレーション
ConfigPortal configPortal;               // WiFi接続・設定Webサーバ
TtsClient ttsClient;                     // TTS音声合成クライアント

// 口パク同期の設定
constexpr uint32_t LIP_FRAME_INTERVAL_MS = 30;  // 口パク更新間隔（30ms）
constexpr uint32_t LIP_INPUT_TIMEOUT_MS = 220;  // 入力がなければ口を閉じるまでの時間

// 口パク同期の状態変数
int currentMouthOpen = 0;         // 現在の口の開き度合い（0〜100）
int targetMouthOpen = 0;          // 目標の口の開き度合い（TTSから設定）
bool lipSyncActive = false;       // 口パク同期が動作中かどうか
bool speaking = false;            // TTSが話し中かどうか
bool ignoreNextTopClick = false;  // スワイプ後の誤クリック防止フラグ

// Gateway からの POST /api/speak 受付用。
// WebServer ハンドラ内では再生せず（ブロッキングのため）、フラグを立てて
// loop() が拾って実際の発話を行う。pendingApiSpeakText が空なら "__CURRENT__" を発話する。
bool pendingApiSpeak = false;     // /api/speak の発話待ちがあるか
String pendingApiSpeakText;       // /api/speak で指定された発話テキスト（空=__CURRENT__）

// --- 診断（クラッシュ調査用）---
// 「しばらく動かすと落ちる」の原因切り分けのため、再起動理由とヒープ推移を記録する。
String bootResetReason = "?";     // 起動時に記録した直近の再起動理由
uint32_t minFreeHeapEver = UINT32_MAX; // 起動以降に観測した最小空きヒープ

// esp_reset_reason_t を人間が読めるラベルに変換する。
// PANIC=コードのクラッシュ、BROWNOUT=電源不足、TASK/INT WDT=ハング、POWERON=通常。
const char* resetReasonLabel(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
  }
}

// --- カメラ目線（明るい方向へ視線を向ける）---
// CoreS3 内蔵カメラ(GC0308)で低解像度フレームを取得し、輝度の重心方向へ目を向ける。
// カメラとタッチは内部I2Cバスを共有するため、設定(cameraGaze)でON/OFFできる。
extern bool modeMenuOpen;          // メニュー表示状態（定義は後方）
extern bool settingsInfoOpen;      // 設定情報画面の表示状態（定義は後方）

bool cameraGazeAvailable = false;  // カメラ初期化に成功したか
bool cameraGazeActive = false;     // 実際に視線制御中か
uint32_t lastCameraGazeAt = 0;     // 最後にフレーム解析した時刻
float camGazeSmoothH = 0.0f;       // 平滑化した水平視線
float camGazeSmoothV = 0.0f;       // 平滑化した垂直視線

// カメラ目線のチューニング
constexpr uint32_t kCamGazeIntervalMs = 250; // フレーム解析間隔（4fps）
constexpr float kCamGazeGain = 1.6f;          // 視線の振り幅（大げさに）
constexpr float kCamGazeSmoothing = 0.35f;    // 平滑化係数（0=なめらか）
constexpr int kCamGazeHSign = 1;              // 水平の符号（左右が逆なら -1 にする）

// GC0308 カメラ設定（M5CoreS3 ライブラリの設定を基に QQVGA で軽量化）
camera_config_t cameraGazeConfig = {
    .pin_pwdn = -1, .pin_reset = -1, .pin_xclk = -1,
    .pin_sscb_sda = 12, .pin_sscb_scl = 11,
    .pin_d7 = 47, .pin_d6 = 48, .pin_d5 = 16, .pin_d4 = 15,
    .pin_d3 = 42, .pin_d2 = 41, .pin_d1 = 40, .pin_d0 = 39,
    .pin_vsync = 46, .pin_href = 38, .pin_pclk = 45,
    .xclk_freq_hz = 20000000,
    .ledc_timer = LEDC_TIMER_0, .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_RGB565,
    .frame_size = FRAMESIZE_QQVGA,  // 160x120（軽量）
    .jpeg_quality = 0, .fb_count = 2,
    .fb_location = CAMERA_FB_IN_PSRAM,
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    .sccb_i2c_port = -1,
};

// カメラを初期化する（config の cameraGaze が true のときだけ setup() から呼ぶ）。
void setupCameraGaze() {
  M5.In_I2C.release(); // 内部I2Cを解放してカメラSCCBに渡す
  const esp_err_t err = esp_camera_init(&cameraGazeConfig);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x (camera gaze disabled)\n", err);
    cameraGazeAvailable = false;
    return;
  }
  cameraGazeAvailable = true;
  Serial.println("Camera gaze ready");
}

// 1フレーム取得して輝度重心の方向へ視線を向ける（throttle 付き・非ブロッキング）。
void updateCameraGaze() {
  if (!cameraGazeAvailable) {
    return;
  }
  // 発話中・メニュー表示中は I2C/CPU 競合を避けて休む
  if (speaking || modeMenuOpen || settingsInfoOpen) {
    return;
  }
  const uint32_t now = millis();
  if (now - lastCameraGazeAt < kCamGazeIntervalMs) {
    return;
  }
  lastCameraGazeAt = now;

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb || fb->format != PIXFORMAT_RGB565) {
    if (fb) esp_camera_fb_return(fb);
    return;
  }

  // RGB565（リトルエンディアン）から近似輝度を計算し、重心を求める。
  // 負荷軽減のため縦横ともに間引いてサンプリングする。
  const int W = fb->width;
  const int H = fb->height;
  const uint8_t* p = fb->buf;
  const int stepX = 4;
  const int stepY = 4;
  uint64_t sumLum = 0, sumX = 0, sumY = 0;
  for (int y = 0; y < H; y += stepY) {
    const uint8_t* row = p + (size_t)y * W * 2;
    for (int x = 0; x < W; x += stepX) {
      const uint16_t px = row[x * 2] | (row[x * 2 + 1] << 8);
      const int r = (px >> 11) & 0x1F;
      const int g = (px >> 5) & 0x3F;
      const int b = px & 0x1F;
      // 近似輝度（5/6/5bit を概ね揃える）
      const uint32_t lum = (uint32_t)(r << 1) + g + (uint32_t)(b << 1);
      sumLum += lum;
      sumX += (uint64_t)lum * x;
      sumY += (uint64_t)lum * y;
    }
  }
  esp_camera_fb_return(fb);

  if (sumLum == 0) {
    return; // 真っ暗：視線は維持
  }

  // 重心を 0..1 に正規化 → -1..1 の視線へ
  const float cx = (float)sumX / (float)sumLum / (float)W; // 0..1
  const float cy = (float)sumY / (float)sumLum / (float)H; // 0..1
  float h = (cx - 0.5f) * 2.0f * kCamGazeGain * kCamGazeHSign;
  float v = (cy - 0.5f) * 2.0f * kCamGazeGain; // 上(y小)=負=上向き

  // 平滑化（急な動きを抑える）
  camGazeSmoothH += (h - camGazeSmoothH) * kCamGazeSmoothing;
  camGazeSmoothV += (v - camGazeSmoothV) * kCamGazeSmoothing;

  avatarFace.setGaze(camGazeSmoothV, camGazeSmoothH);
  cameraGazeActive = true;
}
uint32_t lipSyncLastInputAt = 0;  // 最後に口パク入力があった時刻
uint32_t lipSyncLastFrameAt = 0;  // 最後に口パクフレームを更新した時刻

// 起動時サーボ選択ダイアログの結果
enum class ServoStartupChoice {
  KeepPosition, // キャリブレーションせずに現在位置を維持
  Calibrate,    // フルキャリブレーションを実行
};

// 起動時サーボダイアログのボタン判定結果
enum class ServoPromptButton {
  None,
  No,
  Yes,
};

// アプリの動作モード
enum class AppMode {
  LocalLlm,  // 通常モード: TTSサーバと通信して音声合成
  LevelHold, // 水平維持モード: IMUでサーボを安定化
};

// 下スワイプメニューのボタン
enum class ModeMenuButton {
  None,
  LocalLlm,  // LOCAL LLMモードに切り替え
  LevelHold, // LEVEL HOLDモードに切り替え
  Settings,  // 設定画面を開く
  Close,     // メニューを閉じる
};

// PID（比例-積分-微分）コントローラ。
// LEVEL HOLDモードでサーボを水平に維持するために使用する。
struct PidController {
  float kp = 0.0f;          // 比例ゲイン: 誤差に対する応答の強さ
  float ki = 0.0f;          // 積分ゲイン: 持続する誤差を消去する（このアプリでは0）
  float kd = 0.0f;          // 微分ゲイン: 急な変化を抑制するダンパー効果
  float integral = 0.0f;    // 積分項の累積値
  float previousError = 0.0f; // 前回の誤差（微分計算に使用）
  bool hasPrevious = false;   // 前回誤差があるかどうか（最初のフレームでは微分を0にする）

  // 状態をリセットする（モード切り替え時などに呼ぶ）
  void reset() {
    integral = 0.0f;
    previousError = 0.0f;
    hasPrevious = false;
  }

  // 誤差と経過時間からPID出力を計算する
  float update(float error, float dt) {
    integral += error * dt;
    integral = constrain(integral, -80.0f, 80.0f); // 積分飽和を防ぐ
    const float derivative =
        hasPrevious && dt > 0.0f ? (error - previousError) / dt : 0.0f;
    previousError = error;
    hasPrevious = true;
    return kp * error + ki * integral + kd * derivative;
  }
};

// デッドバンド処理: 小さな誤差（±deadband以内）を0に丸める。
// これにより、サーボのわずかな揺れ（ジッタ）を防ぐ。
float applyDeadband(float value, float deadband) {
  if (fabsf(value) <= deadband) {
    return 0.0f;
  }
  return value > 0.0f ? value - deadband : value + deadband;
}

// current を target に向かって最大 maxStep ずつ近づける。
// 急激な位置変化によるサーボへの衝撃を防ぐレートリミッタ。
int moveTowardByStep(int current, int target, int maxStep) {
  if (target > current) {
    return min(current + maxStep, target);
  }
  if (target < current) {
    return max(current - maxStep, target);
  }
  return current;
}

// 文字列からTTSエンジン種別に変換する
TtsEngineType ttsEngineTypeFromString(const String& value) {
  return value == "simple_wav" ? TtsEngineType::SimpleWav
                               : TtsEngineType::VoiceVoxCompatible;
}

// 起動時UIや各種メニューで使う共有キャンバスを返す。
// 画面サイズが変わった場合（回転後など）はキャンバスを再作成する。
// フリッカーフリー描画のために、描画→pushSprite()の順で使う。
M5Canvas& startupCanvas() {
  static M5Canvas canvas(&M5.Display);
  static int16_t canvasWidth = 0;
  static int16_t canvasHeight = 0;

  const int16_t width = M5.Display.width();
  const int16_t height = M5.Display.height();
  if (canvasWidth != width || canvasHeight != height) {
    canvas.deleteSprite();
    canvas.setColorDepth(16);
    canvas.createSprite(width, height);
    canvasWidth = width;
    canvasHeight = height;
  }
  return canvas;
}

// LEVEL HOLDモードの各種定数
constexpr uint32_t LEVEL_HOLD_UPDATE_INTERVAL_MS = 80;  // PID更新間隔（80ms）
constexpr int LEVEL_HOLD_SERVO_SPEED = 160;              // サーボ移動速度
constexpr float LEVEL_HOLD_FILTER_ALPHA = 0.35f;         // ローパスフィルタ係数（0に近いほど平滑化）
constexpr float LEVEL_HOLD_DEADBAND_DEG = 0.6f;          // ジッタ防止デッドバンド（±0.6度）
constexpr float LEVEL_HOLD_MAX_OUTPUT_DEG = 35.0f;       // PID出力の最大値（±35度）
constexpr int LEVEL_HOLD_MAX_STEP_DEG = 5;               // 1フレームの最大移動量（5度）
constexpr float LEVEL_HOLD_ACCEL_MIN_NORM = 0.05f;       // 有効な加速度ベクトルの最小長さ

// アプリの状態変数
AppMode currentMode = AppMode::LocalLlm; // 現在の動作モード
bool levelHoldActive = false;            // LEVEL HOLDが有効かどうか
uint32_t levelHoldLastUpdateAt = 0;      // 最後にPIDを更新した時刻

// LEVEL HOLD用PIDコントローラ（ロール・ピッチそれぞれ独立）
// kp=1.15/1.35: 応答感度、kd=0.12/0.14: ダンパー効果（ki=0.0: 積分なし）
PidController levelHoldRollPid{1.15f, 0.0f, 0.12f};
PidController levelHoldPitchPid{1.35f, 0.0f, 0.14f};

// ローパスフィルタの状態変数
bool levelHoldFilterReady = false;         // フィルタが初期化済みかどうか
float levelHoldFilteredRollDeg = 0.0f;    // フィルタ後のロール角（度）
float levelHoldFilteredPitchDeg = 0.0f;   // フィルタ後のピッチ角（度）
int levelHoldYawTarget = 0;   // 現在のサーボヨー目標値（レートリミット後）
int levelHoldPitchTarget = 0; // 現在のサーボピッチ目標値（レートリミット後）

// 通常モード用の小さな身体モーション
enum class BodyMotionState {
  Stopped,
  Idle,
  Joy,
};

constexpr uint32_t BODY_MOTION_UPDATE_INTERVAL_MS = 80;
constexpr int BODY_IDLE_SERVO_SPEED = 160;       // 16.0度/秒
constexpr int BODY_JOY_SERVO_SPEED = 720;        // 72.0度/秒
constexpr int BODY_HOME_YAW = 0;                 // 正面
constexpr int BODY_HOME_PITCH = 200;             // 20.0度上向き
constexpr int BODY_IDLE_YAW_AMPLITUDE = 18;      // 1.8度
constexpr int BODY_IDLE_PITCH_AMPLITUDE = 7;     // 0.7度
constexpr int BODY_JOY_YAW_AMPLITUDE = 300;      // 30.0度
constexpr uint32_t BODY_JOY_DURATION_MS = 4800;  // 左右3往復を柔らかく
constexpr float BODY_JOY_CYCLES = 3.0f;
constexpr int BODY_MOTION_MAX_STEP = 24;         // 1更新あたり2.4度

BodyMotionState bodyMotionState = BodyMotionState::Stopped;
bool bodyMotionSkipAutoStart = false;
uint32_t bodyMotionStartedAt = 0;
uint32_t bodyMotionLastUpdateAt = 0;
int bodyMotionYawBase = 0;
int bodyMotionPitchBase = 0;
int bodyMotionYawTarget = 0;
int bodyMotionPitchTarget = 0;
int bodyJoyCenterYaw = 0;
int bodyJoyCenterPitch = 0;

// メニューとSettings情報画面の状態変数
bool modeMenuOpen = false;          // モードメニューが開いているかどうか
bool settingsInfoOpen = false;      // SETTINGS情報画面が開いているかどうか
bool displayWasTouching = false;    // 前フレームでタッチされていたかどうか
int16_t displayTouchStartX = 0;     // スワイプ開始X座標
int16_t displayTouchStartY = 0;     // スワイプ開始Y座標
int16_t displayTouchLastX = 0;      // スワイプ現在X座標
int16_t displayTouchLastY = 0;      // スワイプ現在Y座標
uint32_t displayTouchStartedAt = 0; // スワイプ開始時刻
ModeMenuButton modeMenuPressed = ModeMenuButton::None; // 押下中のメニューボタン

// モード名の文字列を返す（ステータス表示用）
const char* appModeName(AppMode mode) {
  return mode == AppMode::LevelHold ? "LEVEL HOLD" : "LOCAL LLM";
}

// LEVEL HOLDモードのPIDとフィルタ状態をリセットする
void resetLevelHoldPid() {
  levelHoldRollPid.reset();
  levelHoldPitchPid.reset();
  levelHoldFilterReady = false;
  levelHoldFilteredRollDeg = 0.0f;
  levelHoldFilteredPitchDeg = 0.0f;
  levelHoldYawTarget = 0;
  levelHoldPitchTarget = 0;
  levelHoldLastUpdateAt = millis();
}

int clampBodyYaw(int value) {
  const auto& calibration = calibrationController.data();
  return constrain(value, calibration.yawMin, calibration.yawMax);
}

int clampBodyPitch(int value) {
  const auto& calibration = calibrationController.data();
  return constrain(value, calibration.pitchMin, calibration.pitchMax);
}

int bodyHomeYaw() {
  return clampBodyYaw(BODY_HOME_YAW);
}

int bodyHomePitch() {
  return clampBodyPitch(BODY_HOME_PITCH);
}

bool bodyMotionCanUseServo() {
  return calibrationController.data().servoValid &&
         currentMode == AppMode::LocalLlm &&
         !levelHoldActive &&
         !configPortal.isPortalActive() &&
         !modeMenuOpen &&
         !settingsInfoOpen &&
         !speaking;
}

void stopBodyMotion(bool releaseServos) {
  if (bodyMotionState == BodyMotionState::Stopped) {
    return;
  }

  M5StackChan.Motion.stop();
  bodyMotionState = BodyMotionState::Stopped;

  if (releaseServos) {
    delay(30);
    M5StackChan.Motion.setTorqueEnabled(false);
    M5StackChan.Motion.setAutoAngleSyncEnabled(true);
    M5StackChan.Motion.setAutoTorqueReleaseEnabled(true);
    M5StackChan.setServoPowerEnabled(false);
  }
}

bool startBodyMotion(bool ignoreAutoStartSkip = false) {
  if (!calibrationController.data().servoValid ||
      currentMode != AppMode::LocalLlm ||
      levelHoldActive ||
      (!ignoreAutoStartSkip && bodyMotionSkipAutoStart)) {
    return false;
  }

  M5StackChan.setServoPowerEnabled(true);
  delay(80);
  M5StackChan.Motion.setAutoAngleSyncEnabled(true);
  M5StackChan.Motion.setAutoTorqueReleaseEnabled(false);
  M5StackChan.Motion.setTorqueEnabled(true);
  delay(40);

  const auto angles = M5StackChan.Motion.getCurrentAngles();
  bodyMotionYawBase = bodyHomeYaw();
  bodyMotionPitchBase = bodyHomePitch();
  bodyMotionYawTarget = clampBodyYaw(angles.x);
  bodyMotionPitchTarget = clampBodyPitch(angles.y);
  bodyMotionStartedAt = millis();
  bodyMotionLastUpdateAt = 0;
  bodyMotionState = BodyMotionState::Idle;

  M5StackChan.Motion.move(
      bodyMotionYawTarget, bodyMotionPitchTarget, BODY_IDLE_SERVO_SPEED);
  Serial.printf(
      "Body motion idle started current=(%d,%d) home=(%d,%d)\n",
      bodyMotionYawTarget, bodyMotionPitchTarget,
      bodyMotionYawBase, bodyMotionPitchBase);
  return true;
}

// --- ゲーミングRGB（本体LED）---
// この時刻まではステータス色を保持し、虹色サイクルを一時停止する。
uint32_t gamingLedHoldUntil = 0;
// 最後に虹色LEDを更新した時刻（throttle用）
uint32_t gamingLedLastUpdateAt = 0;

// HSV(0..1) を RGB(0..255) に変換する（LED用）。
void hsvToRgb888(float h, float s, float v, uint8_t& r, uint8_t& g,
                 uint8_t& b) {
  h -= floorf(h);
  const float hf = h * 6.0f;
  const int i = static_cast<int>(hf) % 6;
  const float f = hf - floorf(hf);
  const float p = v * (1.0f - s);
  const float q = v * (1.0f - f * s);
  const float t = v * (1.0f - (1.0f - f) * s);
  float rf = 0, gf = 0, bf = 0;
  switch (i) {
    case 0: rf = v; gf = t; bf = p; break;
    case 1: rf = q; gf = v; bf = p; break;
    case 2: rf = p; gf = v; bf = t; break;
    case 3: rf = p; gf = q; bf = v; break;
    case 4: rf = t; gf = p; bf = v; break;
    default: rf = v; gf = p; bf = q; break;
  }
  r = static_cast<uint8_t>(rf * 255.0f);
  g = static_cast<uint8_t>(gf * 255.0f);
  b = static_cast<uint8_t>(bf * 255.0f);
}

// ステータス色をLEDに表示する。ゲーミングRGB有効時は holdMs の間その色を
// 保持してから虹色サイクルへ戻る（TTS/エラー等の状態表示を残すため）。
void showStatusLed(uint8_t r, uint8_t g, uint8_t b,
                   uint32_t holdMs = 1800) {
  M5StackChan.showRgbColor(r, g, b);
  gamingLedHoldUntil = millis() + holdMs;
}

// ゲーミングRGB有効時に、本体LEDを虹色でゆっくり循環させる（serviceApp から毎フレーム）。
// 顔(avatarFace)の虹色フェーズに色相を合わせて、画面とLEDの色を揃える。
void updateGamingLed(uint32_t now) {
  if (!configPortal.config().gamingRgb) {
    return;
  }
  if (static_cast<int32_t>(now - gamingLedHoldUntil) < 0) {
    return; // ステータス色を保持中
  }
  if (now - gamingLedLastUpdateAt < 40) {
    return; // 約25fpsにthrottle
  }
  gamingLedLastUpdateAt = now;

  uint8_t r, g, b;
  // LEDは眩しすぎないよう明度を抑える（V=0.3）。色相は顔と同期。
  hsvToRgb888(avatarFace.gamingHue(), 1.0f, 0.30f, r, g, b);
  M5StackChan.showRgbColor(r, g, b);
}

void triggerPetHappyMotion() {
  ignoreNextTopClick = true;
  avatarFace.setExpression(m5avatar::Expression::Happy);
  avatarFace.showStatus("HAPPY", 1000);
  avatarFace.returnToDefaultAfter(BODY_JOY_DURATION_MS + 800);
  showStatusLed(96, 24, 72);

  if (!calibrationController.data().servoValid ||
      currentMode != AppMode::LocalLlm ||
      levelHoldActive) {
    return;
  }
  if (bodyMotionState == BodyMotionState::Stopped &&
      !startBodyMotion(true)) {
    return;
  }

  bodyJoyCenterYaw = bodyHomeYaw();
  bodyJoyCenterPitch = bodyHomePitch();
  bodyMotionYawTarget = bodyJoyCenterYaw;
  bodyMotionPitchTarget = bodyJoyCenterPitch;
  bodyMotionStartedAt = millis();
  bodyMotionLastUpdateAt = 0;
  bodyMotionState = BodyMotionState::Joy;
}

void updateBodyMotion() {
  if (bodyMotionState == BodyMotionState::Stopped) {
    if (!bodyMotionSkipAutoStart && bodyMotionCanUseServo()) {
      startBodyMotion();
    }
    return;
  }
  if (!bodyMotionCanUseServo()) {
    return;
  }

  const uint32_t now = millis();
  if (now - bodyMotionLastUpdateAt < BODY_MOTION_UPDATE_INTERVAL_MS) {
    return;
  }
  bodyMotionLastUpdateAt = now;

  if (bodyMotionState == BodyMotionState::Joy) {
    const uint32_t elapsed = now - bodyMotionStartedAt;
    if (elapsed >= BODY_JOY_DURATION_MS) {
      bodyMotionYawBase = bodyHomeYaw();
      bodyMotionPitchBase = bodyHomePitch();
      bodyMotionYawTarget = bodyMotionYawBase;
      bodyMotionPitchTarget = bodyMotionPitchBase;
      bodyMotionStartedAt = now;
      bodyMotionState = BodyMotionState::Idle;
      M5StackChan.Motion.move(
          bodyMotionYawTarget, bodyMotionPitchTarget,
          BODY_IDLE_SERVO_SPEED);
      showStatusLed(0, 48, 0);
      return;
    }

    const float t = elapsed / static_cast<float>(BODY_JOY_DURATION_MS);
    const float envelope = sinf(PI * t);
    const float swing =
        sinf(2.0f * PI * BODY_JOY_CYCLES * t) * envelope;
    const int yaw = clampBodyYaw(
        bodyJoyCenterYaw +
        static_cast<int>(roundf(BODY_JOY_YAW_AMPLITUDE * swing)));
    M5StackChan.Motion.move(yaw, bodyJoyCenterPitch, BODY_JOY_SERVO_SPEED);
    bodyMotionYawTarget = yaw;
    bodyMotionPitchTarget = bodyJoyCenterPitch;
    return;
  }

  const float elapsed = (now - bodyMotionStartedAt) / 1000.0f;
  int desiredYaw =
      bodyMotionYawBase +
      static_cast<int>(roundf(BODY_IDLE_YAW_AMPLITUDE *
                              sinf(elapsed * 2.1f)));
  int desiredPitch =
      bodyMotionPitchBase +
      static_cast<int>(roundf(BODY_IDLE_PITCH_AMPLITUDE *
                              sinf(elapsed * 1.45f + 1.2f)));

  desiredYaw = clampBodyYaw(desiredYaw);
  desiredPitch = clampBodyPitch(desiredPitch);
  bodyMotionYawTarget = moveTowardByStep(
      bodyMotionYawTarget, desiredYaw, BODY_MOTION_MAX_STEP);
  bodyMotionPitchTarget = moveTowardByStep(
      bodyMotionPitchTarget, desiredPitch, BODY_MOTION_MAX_STEP);
  M5StackChan.Motion.move(
      bodyMotionYawTarget, bodyMotionPitchTarget, BODY_IDLE_SERVO_SPEED);
}

// LEVEL HOLDモードを開始する。
// キャリブレーションが未完了またはIMUが使えない場合はエラーを表示してfalseを返す。
bool startLevelHoldMode() {
  const auto& calibration = calibrationController.data();
  if (!calibration.servoValid || !calibration.imuLevelValid ||
      !M5.Imu.isEnabled()) {
    avatarFace.setExpression(m5avatar::Expression::Doubt);
    avatarFace.showStatus("CAL REQUIRED", 2500);
    avatarFace.returnToDefaultAfter(2500);
    showStatusLed(96, 48, 0); // 橙色LED: キャリブレーション必要
    return false;
  }

  stopBodyMotion(false);

  // サーボ電源をONにしてトルクを有効化、ホーム位置に移動する
  M5StackChan.setServoPowerEnabled(true);
  delay(150);
  M5StackChan.Motion.setAutoAngleSyncEnabled(true);
  M5StackChan.Motion.setAutoTorqueReleaseEnabled(false);
  M5StackChan.Motion.setTorqueEnabled(true);
  delay(80);
  M5StackChan.Motion.move(0, 0, LEVEL_HOLD_SERVO_SPEED); // センターへ移動
  delay(120);
  M5StackChan.Motion.setAutoAngleSyncEnabled(false);
  resetLevelHoldPid();
  levelHoldActive = true;
  currentMode = AppMode::LevelHold;
  avatarFace.resetToDefault();
  avatarFace.showStatus("LEVEL HOLD", 1800);
  showStatusLed(0, 64, 96); // 水色LED: LEVEL HOLD動作中
  Serial.println("Mode changed: LEVEL HOLD");
  return true;
}

// LEVEL HOLDモードを停止する。サーボのトルクを切って電源をOFFにする。
void stopLevelHoldMode() {
  if (!levelHoldActive) {
    return;
  }
  M5StackChan.Motion.stop();
  delay(50);
  M5StackChan.Motion.setTorqueEnabled(false);
  M5StackChan.Motion.setAutoAngleSyncEnabled(true);
  M5StackChan.Motion.setAutoTorqueReleaseEnabled(true);
  M5StackChan.setServoPowerEnabled(false);
  levelHoldActive = false;
}

// 指定モードに切り替える
void activateMode(AppMode mode) {
  if (mode == AppMode::LevelHold) {
    if (!startLevelHoldMode()) {
      currentMode = AppMode::LocalLlm; // 失敗した場合はLOCAL LLMに戻す
    }
    return;
  }

  stopLevelHoldMode();
  currentMode = AppMode::LocalLlm;
  avatarFace.resetToDefault();
  avatarFace.showStatus("LOCAL LLM", 1800);
  showStatusLed(0, 48, 0); // 緑色LED: 通常動作
  startBodyMotion();
  Serial.println("Mode changed: LOCAL LLM");
}

// LEVEL HOLDモードのPID制御メインループ。80ms間隔で実行する。
//
// アルゴリズム:
//   1. IMU加速度を取得
//   2. ローパスフィルタで高周波ノイズを除去（alpha=0.35）
//   3. ロール・ピッチ誤差にデッドバンドを適用（±0.6度以下はゼロに）
//   4. PIDコントローラで補正角度を計算
//   5. レートリミッタで1フレーム最大5度に制限してサーボへ送る
void updateLevelHoldMode() {
  if (!levelHoldActive) {
    return;
  }

  const uint32_t now = millis();
  if (now - levelHoldLastUpdateAt < LEVEL_HOLD_UPDATE_INTERVAL_MS) {
    return; // まだ更新タイミングではない
  }
  const float dt = (now - levelHoldLastUpdateAt) / 1000.0f; // 経過時間（秒）
  levelHoldLastUpdateAt = now;

  if (M5.Imu.update() == m5::IMU_Class::sensor_mask_none) {
    return; // IMUのデータが更新されていない場合はスキップ
  }

  float ax = 0.0f;
  float ay = 0.0f;
  float az = 0.0f;
  M5.Imu.getAccel(&ax, &ay, &az);

  // 加速度ベクトルの大きさが小さすぎる（自由落下状態など）場合はスキップ
  const float accelNorm = sqrtf(ax * ax + ay * ay + az * az);
  if (accelNorm < LEVEL_HOLD_ACCEL_MIN_NORM) {
    return;
  }

  const auto& calibration = calibrationController.data();
  // キャリブレーション時のゼロ点を引いて水平からの偏差を求める
  const float rollDeg =
      atan2f(ay, az) * 180.0f / PI - calibration.levelRollDeg;
  const float pitchDeg =
      atan2f(-ax, sqrtf(ay * ay + az * az)) * 180.0f / PI -
      calibration.levelPitchDeg;

  // 一次ローパスフィルタ: filteredVal += (newVal - filteredVal) * alpha
  // alpha=0.35 → 高周波ノイズを抑えつつ応答性を維持
  if (!levelHoldFilterReady) {
    levelHoldFilteredRollDeg = rollDeg;   // 初回は即時設定
    levelHoldFilteredPitchDeg = pitchDeg;
    levelHoldFilterReady = true;
  } else {
    levelHoldFilteredRollDeg +=
        (rollDeg - levelHoldFilteredRollDeg) * LEVEL_HOLD_FILTER_ALPHA;
    levelHoldFilteredPitchDeg +=
        (pitchDeg - levelHoldFilteredPitchDeg) * LEVEL_HOLD_FILTER_ALPHA;
  }

  // デッドバンド適用: ±0.6度以内の微小誤差は無視（サーボのジッタ防止）
  const float rollError =
      applyDeadband(levelHoldFilteredRollDeg, LEVEL_HOLD_DEADBAND_DEG);
  const float pitchError =
      applyDeadband(levelHoldFilteredPitchDeg, LEVEL_HOLD_DEADBAND_DEG);

  // PID計算: rollはヨー軸、pitchはピッチ軸に対応（符号注意）
  const int yawCorrection = static_cast<int>(roundf(constrain(
      levelHoldRollPid.update(rollError, dt),
      -LEVEL_HOLD_MAX_OUTPUT_DEG,
      LEVEL_HOLD_MAX_OUTPUT_DEG)));
  const int pitchCorrection = static_cast<int>(roundf(constrain(
      -levelHoldPitchPid.update(pitchError, dt), // ピッチは符号が逆
      -LEVEL_HOLD_MAX_OUTPUT_DEG,
      LEVEL_HOLD_MAX_OUTPUT_DEG)));

  // キャリブレーションで記録した可動範囲内にクランプする
  const int yawTarget = constrain(
      yawCorrection, calibration.yawMin, calibration.yawMax);
  const int pitchTarget = constrain(
      pitchCorrection, calibration.pitchMin, calibration.pitchMax);

  // レートリミッタ: 1フレームで最大5度しか動かさない（急激な動きを防ぐ）
  levelHoldYawTarget = moveTowardByStep(
      levelHoldYawTarget, yawTarget, LEVEL_HOLD_MAX_STEP_DEG);
  levelHoldPitchTarget = moveTowardByStep(
      levelHoldPitchTarget, pitchTarget, LEVEL_HOLD_MAX_STEP_DEG);

  M5StackChan.Motion.move(
      levelHoldYawTarget, levelHoldPitchTarget, LEVEL_HOLD_SERVO_SPEED);
}

// 現在アクティブなモードの更新処理を呼ぶ
void updateActiveMode() {
  updateLevelHoldMode();
  updateBodyMotion();
}

// TTS再生中の口パク入力を受け取る（VoiceVoxClientから呼ばれる）
// level: PCMサンプルの平均振幅から算出した0〜100の口の開き度合い
// 表情を大げさに見せるため、開き具合を 1.5 倍に増幅してから適用する。
void setLipSyncLevel(int level) {
  targetMouthOpen = constrain(static_cast<int>(level * 1.5f), 0, 100);
  lipSyncLastInputAt = millis();
  lipSyncActive = true;
}

// 音声再生終了時に口を閉じるトリガー（VoiceVoxClientから呼ばれる）
void stopLipSync() {
  targetMouthOpen = 0;
  lipSyncActive = true;
  lipSyncLastInputAt = millis();
}

// 口パクアニメーションを更新する（30ms間隔）。
// 目標値に向かってスムーズに加速・減速させる（線形補間より自然な動き）。
void updateLipSync() {
  const uint32_t now = millis();

  // 最後の入力から220ms経過したら口を閉じる（無音区間の検出）
  if (lipSyncActive && now - lipSyncLastInputAt >= LIP_INPUT_TIMEOUT_MS) {
    targetMouthOpen = 0;
  }

  if (!lipSyncActive ||
      now - lipSyncLastFrameAt < LIP_FRAME_INTERVAL_MS) {
    return;
  }
  lipSyncLastFrameAt = now;

  // 開くときは素早く（残り距離の半分）、閉じるときはゆっくり（残り距離の1/4）
  if (currentMouthOpen < targetMouthOpen) {
    currentMouthOpen +=
        max(3, (targetMouthOpen - currentMouthOpen + 1) / 2);
    currentMouthOpen = min(currentMouthOpen, targetMouthOpen);
  } else if (currentMouthOpen > targetMouthOpen) {
    currentMouthOpen -=
        max(2, (currentMouthOpen - targetMouthOpen + 3) / 4);
    currentMouthOpen = max(currentMouthOpen, targetMouthOpen);
  }

  avatarFace.setMouthOpenRatio(currentMouthOpen / 100.0f);

  // 口が完全に閉じたら口パクを停止する
  if (currentMouthOpen == 0 && targetMouthOpen == 0) {
    lipSyncActive = false;
  }
}

// メインループで毎フレーム呼ぶサービス関数。
// 全サブシステムの更新処理をまとめて実行する。
// TTS再生中もこれを呼ぶことで、Webサーバの応答などを継続する。
void serviceApp() {
  M5StackChan.update();    // タッチセンサ・ボタン・LED等の更新
  configPortal.update();   // Webサーバのリクエスト処理
  avatarFace.update();     // アバターのステータス・まばたき・ショーケース更新
  updateLipSync();         // 口パクアニメーション更新
  updateActiveMode();      // LEVEL HOLDなどのモード固有処理
  updateGamingLed(millis()); // ゲーミングRGB（本体LEDの虹色循環）

  // 空きヒープの最小値を毎フレーム追跡する（断片化・リーク検出用）
  const uint32_t freeHeapNow = ESP.getFreeHeap();
  if (freeHeapNow < minFreeHeapEver) {
    minFreeHeapEver = freeHeapNow;
  }

  // /statusページ用のランタイム状態を2秒ごとに更新する
  static uint32_t lastStatusUpdateAt = 0;
  const uint32_t now = millis();
  if (now - lastStatusUpdateAt >= 2000) {
    RuntimeStatus status;
    status.appMode = appModeName(currentMode);
    status.servoCalibrated = calibrationController.data().servoValid;
    status.imuCalibrated = calibrationController.data().imuLevelValid;
    status.resetReason = bootResetReason;
    status.freeHeap = freeHeapNow;
    status.minFreeHeap = minFreeHeapEver;
    status.maxAllocHeap = ESP.getMaxAllocHeap();
    status.freePsram = ESP.getFreePsram();
    status.cameraActive = cameraGazeActive;
    configPortal.setRuntimeStatus(status);
    lastStatusUpdateAt = now;
  }

  // 15秒ごとにヒープ状況をシリアルへ出力する（クラッシュ前の推移を確認するため）。
  // free が徐々に減る=リーク、free に対し max-alloc が極端に小さい=断片化。
  static uint32_t lastHeapLogAt = 0;
  if (now - lastHeapLogAt >= 15000) {
    Serial.printf("[heap] free=%u min=%u maxblk=%u psram=%u\n",
                  (unsigned)freeHeapNow, (unsigned)minFreeHeapEver,
                  (unsigned)ESP.getMaxAllocHeap(),
                  (unsigned)ESP.getFreePsram());
    lastHeapLogAt = now;
  }
}

// 指定テキストをTTSで読み上げる（共通処理）。
// 話し中・WiFi未接続・ショーケース中の場合は早期リターンする。
// simple_wav エンジンではこの text がそのまま Gateway の /synthesis に POST される。
//   - 設定テキスト "__REASK_LAST__"（Aボタン）→ Gateway が最後の質問を再LLM処理
//   - "__CURRENT__"（/api/speak 既定）→ Gateway が current.wav を返す
void speakText(const String& text) {
  if (speaking || !configPortal.isConnected()) {
    return;
  }

  // ショーケースモード中は一旦停止する
  if (avatarFace.isShowcaseEnabled()) {
    avatarFace.toggleShowcase();
  }

  speaking = true;
  avatarFace.setExpression(m5avatar::Expression::Happy);
  avatarFace.showStatus("TTS", 900);
  showStatusLed(0, 0, 96); // 青色LED: TTS通信中

  // 設定値からTTSリクエストを構築する
  const AppConfig& appConfig = configPortal.config();
  TtsConfig ttsConfig;
  ttsConfig.host = appConfig.ttsHost;
  ttsConfig.port = appConfig.ttsPort;
  ttsConfig.speaker = appConfig.ttsSpeaker;
  ttsConfig.engineType =
      ttsEngineTypeFromString(appConfig.ttsEngineType);

  const bool success = ttsClient.speak(ttsConfig, text);
  stopLipSync(); // 再生終了後に口を閉じる

  if (success) {
    avatarFace.resetToDefault();
    showStatusLed(0, 48, 0); // 緑色LED: 正常
  } else {
    avatarFace.setExpression(m5avatar::Expression::Angry);
    avatarFace.showStatus("TTS ERROR", 2500);
    avatarFace.returnToDefaultAfter(2500);
    showStatusLed(96, 0, 0, 3000); // 赤色LED: エラー（3秒保持）
    Serial.printf("TTS error: %s\n", ttsClient.lastError().c_str());
  }

  speaking = false;
}

// 設定された「Text to speak」をTTSで読み上げる（Aボタン・頭タッチ用）。
void speakConfiguredText() {
  speakText(configPortal.config().speechText);
}

// 頭部タッチセンサの入力を処理する。
// スワイプでパレット切り替え、ダブルクリックで目パターン、3回クリックで変形、
// 長押しでショーケース、シングルクリックでTTS読み上げ。
void handleTopTouch() {
  auto& touch = M5StackChan.TouchSensor;

  // スワイプは単独で処理し、後のクリック判定を無効化する
  if (touch.wasSwipedForward()) {
    ignoreNextTopClick = true;
    avatarFace.nextPalette(1); // 前方スワイプ: 次のパレット
    triggerPetHappyMotion();
  } else if (touch.wasSwipedBackward()) {
    ignoreNextTopClick = true;
    avatarFace.nextPalette(-1); // 後方スワイプ: 前のパレット
    triggerPetHappyMotion();
  }

  if (touch.wasDoubleClicked()) {
    ignoreNextTopClick = false;
    avatarFace.nextEyePattern(); // ダブルクリック: 次の目パターン
  } else if (touch.wasSingleClicked()) {
    if (ignoreNextTopClick) {
      ignoreNextTopClick = false; // スワイプ後の誤クリックを無視
    } else {
      speakConfiguredText(); // シングルクリック: TTS読み上げ
    }
  } else if (touch.wasDecideClickCount() &&
             touch.getClickCount() >= 3) {
    ignoreNextTopClick = false;
    avatarFace.nextTransform(); // 3回以上クリック: 次の変形パターン
  }

  if (touch.wasHold()) {
    ignoreNextTopClick = false;
    avatarFace.toggleShowcase(); // 長押し: ショーケースモード切り替え
  }
}

// タッチ座標からメニューボタンを判定する。
// ボタン領域: rowX〜rowX+rowW の横幅内で、各ボタンのY座標範囲。
ModeMenuButton modeMenuButtonAt(int16_t x, int16_t y) {
  const int16_t width = M5.Display.width();
  const int16_t rowX = 18;
  const int16_t rowW = width - rowX * 2;

  if (x < rowX || x > rowX + rowW) {
    return ModeMenuButton::None; // ボタン横幅の外側
  }
  if (y >= 48 && y <= 88) {
    return ModeMenuButton::LocalLlm;
  }
  if (y >= 96 && y <= 136) {
    return ModeMenuButton::LevelHold;
  }
  if (y >= 144 && y <= 184) {
    return ModeMenuButton::Settings;
  }
  if (y >= 192 && y <= 232) {
    return ModeMenuButton::Close;
  }
  return ModeMenuButton::None;
}

// モード選択メニューを描画する。
// 4つのボタン（LOCAL LLM / LEVEL HOLD / SETTINGS / CLOSE）を表示する。
// 選択中のモードは黄色ボーダーで強調、押下中のボタンは明るい色で表示する。
void drawModeMenu(ModeMenuButton pressed) {
  auto& display = startupCanvas();
  const int16_t width = display.width();
  constexpr int16_t rowX = 18;
  const int16_t rowW = width - rowX * 2;
  constexpr int16_t rowH = 40; // 4ボタンを収めるため46→40pxに縮小

  display.fillScreen(TFT_BLACK);
  display.setFont(&fonts::Font2);
  display.setTextDatum(middle_center);
  display.setTextSize(2);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.drawString("MODE", width / 2, 18);

  display.setTextSize(1);
  display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display.drawString(appModeName(currentMode), width / 2, 36); // 現在のモードを表示

  // 各ボタンを描画するラムダ
  // 現在選択中のモードに対応するボタンは黄色ボーダーで強調する
  auto drawRow = [&](ModeMenuButton button, const char* label,
                     int16_t y, uint16_t color) {
    const bool selected =
        (button == ModeMenuButton::LocalLlm &&
         currentMode == AppMode::LocalLlm) ||
        (button == ModeMenuButton::LevelHold &&
         currentMode == AppMode::LevelHold);
    const bool isPressed = pressed == button;
    // 押下中: color（明るい色）、選択中: 0x2945（やや明るい暗色）、通常: 0x2104（暗色）
    const uint16_t fill =
        isPressed ? color : (selected ? 0x2945 : 0x2104);
    display.fillRoundRect(rowX, y, rowW, rowH, 8, fill);
    display.drawRoundRect(rowX, y, rowW, rowH, 8,
                          selected ? TFT_YELLOW : TFT_DARKGREY);
    display.setTextColor(TFT_WHITE, fill);
    display.setTextSize(2);
    display.drawString(label, width / 2, y + rowH / 2);
  };

  drawRow(ModeMenuButton::LocalLlm, "LOCAL LLM", 48, 0x03E0);   // 緑
  drawRow(ModeMenuButton::LevelHold, "LEVEL HOLD", 96, 0x035F);  // 青
  drawRow(ModeMenuButton::Settings, "SETTINGS", 144, 0x8010);    // 紫
  drawRow(ModeMenuButton::Close, "CLOSE", 192, 0x4208);          // ダークグレー
  display.pushSprite(0, 0); // キャンバスを画面に転送（フリッカーフリー）
}

// SETTINGS選択時に表示するIPアドレス情報画面を描画する。
// WiFi接続中なら緑色でURLを、APが起動中ならシアン色でAPのURLを表示する。
void drawSettingsInfo() {
  auto& display = startupCanvas();
  const int16_t width = display.width();
  const int16_t height = display.height();

  display.fillScreen(TFT_BLACK);
  display.setFont(&fonts::Font2);
  display.setTextDatum(middle_center);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextSize(2);
  display.drawString("SETTINGS", width / 2, 20);

  display.setTextSize(1);
  int16_t y = 50;

  // WiFi接続中の場合: ローカルIPアドレスのURLを表示
  if (configPortal.isConnected()) {
    const String url = "http://" + configPortal.localIp().toString() + "/";
    display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    display.drawString("Open in browser (Wi-Fi):", width / 2, y);
    y += 18;
    display.setTextColor(TFT_GREEN, TFT_BLACK);
    display.drawString(url, width / 2, y);
    y += 26;
  }

  // SETTINGSメニュー用APが起動中の場合: ホットスポット情報を表示
  if (configPortal.isSettingsApActive()) {
    display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    display.drawString("Hotspot: " + configPortal.accessPointName(), width / 2, y);
    y += 16;
    display.drawString("Password: stackchan", width / 2, y);
    y += 18;
    const String apUrl = "http://" + configPortal.settingsApIp().toString() + "/";
    display.setTextColor(TFT_CYAN, TFT_BLACK);
    display.drawString(apUrl, width / 2, y); // 192.168.4.1
  }

  display.setTextColor(TFT_DARKGREY, TFT_BLACK);
  display.drawString("Tap to close", width / 2, height - 14);
  display.pushSprite(0, 0);
}

// SETTINGS情報画面を開く。
// APを起動してから画面を表示する（APの起動完了を待たずにUIを先に出す）。
void openSettingsInfo() {
  settingsInfoOpen = true;
  configPortal.startSettingsAp(); // WIFI_AP_STAモードでAPを追加起動
  avatarFace.pauseDrawing();
  delay(20);
  drawSettingsInfo();
}

// SETTINGS情報画面を閉じる。APを停止してアバターを再開する。
void closeSettingsInfo() {
  configPortal.stopSettingsAp(); // APを停止してSTAモードに戻す
  settingsInfoOpen = false;
  avatarFace.resumeDrawing();
  avatarFace.resetToDefault();
}

// メニューを開く。アバターの描画を一時停止して画面を上書きする。
void openModeMenu() {
  if (modeMenuOpen) {
    return;
  }
  modeMenuOpen = true;
  modeMenuPressed = ModeMenuButton::None;
  avatarFace.pauseDrawing();
  delay(20);
  drawModeMenu(modeMenuPressed);
}

// メニューを閉じる。アバターの描画を再開してデフォルト状態に戻す。
void closeModeMenu() {
  if (!modeMenuOpen) {
    return;
  }
  modeMenuOpen = false;
  modeMenuPressed = ModeMenuButton::None;
  avatarFace.resumeDrawing();
  avatarFace.resetToDefault();
}

// タッチスクリーンの入力を処理する。
// 下からの上スワイプ（高さ44px以内から70px以上、横ブレ100px以内、1.2秒以内）でメニューを開く。
// 戻り値: メニューかSETTINGS画面が処理を消費した場合はtrue（他の入力処理をスキップする）
bool handleDisplayTouch() {
  int16_t x = 0;
  int16_t y = 0;
  const bool touching = M5.Display.getTouch(&x, &y);

  // SETTINGS情報画面が開いている: タッチリリースで閉じる
  if (settingsInfoOpen) {
    if (!touching && displayWasTouching) {
      closeSettingsInfo();
    }
    displayWasTouching = touching;
    return true; // 他の入力処理をブロック
  }

  // モードメニューが開いている: ボタンのハイライトと選択処理
  if (modeMenuOpen) {
    const ModeMenuButton current =
        touching ? modeMenuButtonAt(x, y) : ModeMenuButton::None;
    // ボタンが変わったら再描画してハイライト更新
    if (touching && current != modeMenuPressed) {
      modeMenuPressed = current;
      drawModeMenu(modeMenuPressed);
    }
    if (!touching && displayWasTouching) {
      const ModeMenuButton selected = modeMenuPressed;
      closeModeMenu(); // メニューを閉じてからモードを切り替える
      if (selected == ModeMenuButton::LocalLlm) {
        activateMode(AppMode::LocalLlm);
      } else if (selected == ModeMenuButton::LevelHold) {
        activateMode(AppMode::LevelHold);
      } else if (selected == ModeMenuButton::Settings) {
        openSettingsInfo(); // SETTINGS情報画面を開く
      }
    }
    displayWasTouching = touching;
    return true; // 他の入力処理をブロック
  }

  // メニューもSETTINGSも開いていない: スワイプジェスチャーを検出する
  if (touching && !displayWasTouching) {
    // タッチ開始座標と時刻を記録
    displayTouchStartX = x;
    displayTouchStartY = y;
    displayTouchLastX = x;
    displayTouchLastY = y;
    displayTouchStartedAt = millis();
  } else if (touching) {
    // タッチ中は現在座標を更新し続ける
    displayTouchLastX = x;
    displayTouchLastY = y;
  } else if (!touching && displayWasTouching) {
    const int16_t height = M5.Display.height();
    const int16_t verticalTravel =
        displayTouchStartY - displayTouchLastY;    // 上方向が正（開始Y - 終了Y）
    const int16_t horizontalTravel =
        abs(displayTouchLastX - displayTouchStartX); // 横ブレの絶対値
    const uint32_t duration = millis() - displayTouchStartedAt;

    // 上スワイプ判定:
    //   - 画面下部44px以内から開始
    //   - 70px以上上に移動
    //   - 横ブレ100px以下
    //   - 所要時間1.2秒以下
    if (displayTouchStartY >= height - 44 &&
        verticalTravel >= 70 &&
        horizontalTravel <= 100 &&
        duration <= 1200) {
      openModeMenu();
      displayWasTouching = false;
      return true;
    }
  }

  displayWasTouching = touching;
  return false;
}

// 起動直後に表示するフォールバック顔（アバター起動前の暫定表示）。
// StackChan BSPの初期化中もこの顔が表示され続ける。
void drawBootFallbackFace() {
  auto cfg = M5.config();
  M5.begin(cfg);

  if (M5.Display.width() < M5.Display.height()) {
    M5.Display.setRotation(1); // 横向きに設定
  }
  M5.Display.setBrightness(160);
  M5.Display.fillScreen(TFT_BLACK);

  const int cx = M5.Display.width() / 2;
  const int cy = M5.Display.height() / 2;
  // シンプルな2つの白い円（目）と白い丸角矩形（口）で顔を描く
  M5.Display.fillCircle(cx - 64, cy - 18, 12, TFT_WHITE); // 左目
  M5.Display.fillCircle(cx + 64, cy - 18, 12, TFT_WHITE); // 右目
  M5.Display.fillRoundRect(cx - 42, cy + 30, 84, 7, 3, TFT_WHITE); // 口
}

// タッチ座標から起動時サーボ選択ダイアログのボタンを判定する。
// 左半分=NO、右半分=YES。ボタン領域: y=160〜226。
ServoPromptButton servoPromptButtonAt(int16_t x, int16_t y) {
  constexpr int16_t kButtonTop = 160;
  constexpr int16_t kButtonBottom = 226;

  if (y < kButtonTop || y > kButtonBottom) {
    return ServoPromptButton::None;
  }
  return x < M5.Display.width() / 2 ? ServoPromptButton::No
                                    : ServoPromptButton::Yes;
}

// 起動時サーボ選択ダイアログを描画する。
// 赤いNOボタン（左）と緑のYESボタン（右）を表示する。
void drawServoStartupPrompt(ServoPromptButton pressed) {
  auto& display = startupCanvas();
  const int16_t width = display.width();
  constexpr int16_t kMargin = 14;
  constexpr int16_t kGap = 10;
  constexpr int16_t kButtonTop = 160;
  constexpr int16_t kButtonHeight = 66;
  const int16_t buttonWidth =
      (width - kMargin * 2 - kGap) / 2;
  const int16_t noX = kMargin;
  const int16_t yesX = noX + buttonWidth + kGap;

  // 押下中は明るい色、通常は暗い色
  const uint16_t noColor =
      pressed == ServoPromptButton::No ? 0x7800 : 0x4208;  // 赤 / 暗いグレー
  const uint16_t yesColor =
      pressed == ServoPromptButton::Yes ? 0x03E0 : 0x0260; // 緑 / 暗い緑

  display.fillScreen(TFT_BLACK);
  display.setFont(&fonts::Font2);
  display.setTextDatum(middle_center);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextSize(2);
  display.drawString("SERVO STARTUP", width / 2, 30);

  display.setTextSize(1);
  display.drawString("Run full calibration?", width / 2, 76);
  display.setTextColor(TFT_YELLOW, TFT_BLACK); // 警告は黄色で強調
  display.drawString("Clear the area before selecting YES", width / 2, 111);
  display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display.drawString("Touch a button to continue", width / 2, 137);

  display.fillRoundRect(
      noX, kButtonTop, buttonWidth, kButtonHeight, 10, noColor);
  display.drawRoundRect(
      noX, kButtonTop, buttonWidth, kButtonHeight, 10, TFT_WHITE);
  display.fillRoundRect(
      yesX, kButtonTop, buttonWidth, kButtonHeight, 10, yesColor);
  display.drawRoundRect(
      yesX, kButtonTop, buttonWidth, kButtonHeight, 10, TFT_WHITE);

  display.setTextColor(TFT_WHITE);
  display.setTextSize(2);
  display.drawString("NO", noX + buttonWidth / 2, kButtonTop + 33);
  display.drawString("YES", yesX + buttonWidth / 2, kButtonTop + 33);
  display.pushSprite(0, 0);
}

// 起動時サーボ選択ダイアログを表示してユーザの選択を待つ。
// ブロッキング処理（アバター描画を一時停止してダイアログを表示）。
// 戻り値: NO=KeepPosition、YES=Calibrate
ServoStartupChoice askServoStartupChoice() {
  avatarFace.pauseDrawing();
  delay(40);
  drawServoStartupPrompt(ServoPromptButton::None);

  int16_t touchX = 0;
  int16_t touchY = 0;

  // 起動中に画面に触れていた指をリリースするまで待つ（誤操作防止）
  while (M5.Display.getTouch(&touchX, &touchY)) {
    M5StackChan.update();
    delay(10);
  }

  bool wasTouching = false;
  ServoPromptButton pressed = ServoPromptButton::None;

  while (true) {
    M5StackChan.update();
    const bool touching = M5.Display.getTouch(&touchX, &touchY);
    const ServoPromptButton current =
        touching ? servoPromptButtonAt(touchX, touchY)
                 : ServoPromptButton::None;

    // ボタンが変わったら再描画してハイライト更新
    if (touching && current != pressed) {
      pressed = current;
      drawServoStartupPrompt(pressed);
    }

    // 指を離したとき、押していたボタンで判定する
    if (!touching && wasTouching) {
      const ServoPromptButton selected = pressed;
      pressed = ServoPromptButton::None;
      drawServoStartupPrompt(pressed);

      if (selected == ServoPromptButton::No) {
        avatarFace.resumeDrawing();
        avatarFace.resetToDefault();
        return ServoStartupChoice::KeepPosition;
      }
      if (selected == ServoPromptButton::Yes) {
        avatarFace.resumeDrawing();
        avatarFace.resetToDefault();
        return ServoStartupChoice::Calibrate;
      }
    }

    wasTouching = touching;
    delay(10);
  }
}

// セットアップ処理。電源投入後に1回だけ実行される。
void setup() {
  Serial.begin(115200);
  delay(100);
  const esp_reset_reason_t resetReason = esp_reset_reason();
  bootResetReason = resetReasonLabel(resetReason); // /status で表示する
  Serial.printf("\nStackChan boot, reset reason=%d (%s)\n",
                static_cast<int>(resetReason), bootResetReason.c_str());

  // まず最初にディスプレイを起動してフォールバック顔を表示する。
  // StackChan BSPの初期化（次のステップ）はボディ配線の問題で時間がかかることがあるため、
  // 画面を先に出しておくことでハング時もユーザに状態が分かるようにする。
  drawBootFallbackFace();
  Serial.println("Fallback face ready");

  Serial.println("Initializing StackChan BSP...");
  M5StackChan.begin(); // サーボ・LED・タッチセンサ・IMU等の初期化
  Serial.println("StackChan BSP ready");

  // サーボAPIは有効にしつつ、不意な動きを防ぐためトルクと電源はOFFにする
  M5StackChan.Motion.setTorqueEnabled(false);
  M5StackChan.setServoPowerEnabled(false);
  M5StackChan.showRgbColor(0, 0, 0); // 全LED消灯
  Serial.println("Servo torque and power disabled");

  // NVSからキャリブレーションデータを読み込む
  calibrationController.load();
  const auto& savedCalibration = calibrationController.data();
  Serial.printf(
      "Calibration servo=%d imu=%d yaw=[%d,%d] pitch=[%d,%d] "
      "level=(%.3f,%.3f)\n",
      savedCalibration.servoValid, savedCalibration.imuLevelValid,
      savedCalibration.yawMin, savedCalibration.yawMax,
      savedCalibration.pitchMin, savedCalibration.pitchMax,
      savedCalibration.levelRollDeg,
      savedCalibration.levelPitchDeg);

  if (M5.Display.width() < M5.Display.height()) {
    M5.Display.setRotation(1); // 横向きに設定
  }
  M5.Display.setBrightness(160);
  M5.Display.fillScreen(TFT_BLACK);

  // WiFi接続前にアバターを起動しておく。
  // WiFi接続（最大15秒）や設定APモード中も顔が表示され続ける。
  Serial.println("Starting avatar...");
  avatarFace.begin();
  avatarFace.resetToDefault();
  avatarFace.showStatus("BOOT", 1200);
  Serial.println("Avatar started");

  // 起動時サーボ選択ダイアログ（キャリブレーションするかどうか）
  const ServoStartupChoice servoChoice = askServoStartupChoice();
  if (servoChoice == ServoStartupChoice::Calibrate) {
    Serial.println("Servo startup choice: CALIBRATE");
    bodyMotionSkipAutoStart = true;
    const bool calibrationOk =
        calibrationController.run(avatarFace); // フルキャリブレーション実行
    bodyMotionSkipAutoStart = !calibrationOk;
  } else {
    Serial.println("Servo startup choice: NO");
    bodyMotionSkipAutoStart = false;
    M5StackChan.Motion.setTorqueEnabled(false);
    M5StackChan.setServoPowerEnabled(false);
    avatarFace.showStatus("SERVO: NO", 1800);
  }

  M5.Speaker.begin();
  M5.Speaker.setVolume(255); // スピーカー音量（0〜255、最大）
  // TTS再生中のコールバックを登録する
  ttsClient.setCallbacks(setLipSyncLevel, serviceApp);

  // Gateway 連携用のコールバックを登録する（configPortal.begin() の前に設定）。
  // /api/speak: 話し中・発話待ちなら busy。受け付けたら発話待ちフラグを立てる。
  configPortal.setSpeakRequestHandler([](const String& text) -> bool {
    if (speaking || pendingApiSpeak) {
      return false; // busy
    }
    pendingApiSpeakText = text; // 空なら loop() 側で "__CURRENT__" を使う
    pendingApiSpeak = true;
    return true;
  });
  // /api/status: 現在の発話状態を返すためのプローブ
  configPortal.setSpeakingProbe([]() -> bool {
    return speaking || pendingApiSpeak;
  });

  Serial.println("Starting network configuration...");
  const bool wifiConnected = configPortal.begin();

  // ゲーミングRGB（顔の虹色循環）を設定値に従って有効化する
  avatarFace.setGamingRgb(configPortal.config().gamingRgb);

  if (!wifiConnected) {
    // WiFi接続失敗: セットアップAPモードで起動
    Serial.printf("Setup AP: %s, http://192.168.4.1\n",
                  configPortal.accessPointName().c_str());
    avatarFace.resetToDefault();
    avatarFace.showStatus("SETUP: 192.168.4.1", 0); // 常時表示
    showStatusLed(48, 32, 0); // 橙色LED: セットアップ待ち
    return; // ループに入る（APモードではメニューは使えない）
  }

  // WiFi接続成功
  Serial.printf("StackChan IP: %s\n",
                configPortal.localIp().toString().c_str());
  Serial.printf("TTS: %s http://%s:%u speaker=%s\n",
                configPortal.config().ttsEngineType.c_str(),
                configPortal.config().ttsHost.c_str(),
                configPortal.config().ttsPort,
                configPortal.config().ttsSpeaker.c_str());

  avatarFace.resetToDefault();
  avatarFace.showStatus("LOCAL LLM");
  showStatusLed(0, 48, 0); // 緑色LED: 正常動作中
  startBodyMotion();
  Serial.println("Mode default: LOCAL LLM");
}

// メインループ。約5ms間隔で繰り返し実行される。
void loop() {
  serviceApp(); // 全サブシステムの更新

  // APセットアップモード中はタッチ・ボタン処理をスキップする
  // （ループ内でWebサーバの応答のみ行う）
  if (configPortal.isPortalActive()) {
    delay(5);
    return;
  }

  // タッチスクリーンの処理（メニュー・スワイプ）が入力を消費した場合はスキップ
  if (handleDisplayTouch()) {
    delay(5);
    return;
  }

  // Gateway からの /api/speak 発話待ちを処理する（ブラウザ送信→自動発話）。
  // WebServer ハンドラ内ではフラグを立てるだけで、実際の再生はここで行う。
  if (pendingApiSpeak && !speaking) {
    pendingApiSpeak = false;
    String text = pendingApiSpeakText;
    pendingApiSpeakText = "";
    if (text.isEmpty()) {
      text = "__CURRENT__"; // 既定: Gateway の current.wav を再生する
    }
    speakText(text);
    delay(5);
    return;
  }

  // ボタンA: 設定テキストをTTSで読み上げる（"__REASK_LAST__" 運用で最後の質問を再LLM）
  if (M5.BtnA.wasPressed()) {
    speakConfiguredText();
  }

  // ボタンB: 次の表情に切り替える（TTS中は無効）
  if (!speaking && M5.BtnB.wasPressed()) {
    avatarFace.nextExpression();
  }

  // ボタンC: 次の顔型に切り替える（TTS中は無効）
  if (!speaking && M5.BtnC.wasPressed()) {
    avatarFace.nextFace();
  }

  // 頭部タッチセンサの処理（TTS中は無効）
  if (!speaking) {
    handleTopTouch();
  }

  delay(5);
}
