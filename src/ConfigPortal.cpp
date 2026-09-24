#include "ConfigPortal.h"

namespace {

// HTMLの特殊文字（<>&"）をエンティティにエスケープする。
// ユーザ入力をHTMLに埋め込む際のXSS対策として使用する。
String htmlEscape(const String& value) {
  String escaped;
  escaped.reserve(value.length() + 16);
  for (size_t i = 0; i < value.length(); ++i) {
    switch (value[i]) {
      case '&':
        escaped += F("&amp;");
        break;
      case '<':
        escaped += F("&lt;");
        break;
      case '>':
        escaped += F("&gt;");
        break;
      case '"':
        escaped += F("&quot;");
        break;
      default:
        escaped += value[i];
        break;
    }
  }
  return escaped;
}

// TTSエンジン種別の文字列を正規化する。
// 不正な値が来た場合はデフォルトの "voicevox_compatible" を返す。
String sanitizedTtsEngineType(const String& value) {
  return value == "simple_wav" ? "simple_wav" : "voicevox_compatible";
}

// 文字列をJSON文字列値としてエスケープする（"\ と制御文字を処理）。
String jsonEscape(const String& value) {
  String out;
  out.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    switch (c) {
      case '"':  out += F("\\\""); break;
      case '\\': out += F("\\\\"); break;
      case '\n': out += F("\\n"); break;
      case '\r': out += F("\\r"); break;
      case '\t': out += F("\\t"); break;
      default:   out += c; break;
    }
  }
  return out;
}

// <option> タグの selected 属性を返す（現在の値と一致する場合のみ付与）
String selectedAttribute(const String& value, const char* option) {
  if (value == option) {
    return " selected";
  }
  return "";
}

// 共通のHTMLヘッダ（スタイル・ナビゲーション付き）を生成する
String commonHead(const char* title) {
  String h;
  h += F("<!doctype html><html lang='en'><head>"
         "<meta charset='utf-8'><meta name='viewport' "
         "content='width=device-width,initial-scale=1'>"
         "<title>");
  h += title;
  h += F("</title><style>"
         "body{font-family:sans-serif;max-width:640px;margin:24px auto;"
         "padding:0 16px;background:#111;color:#eee}"
         "h1{margin-bottom:4px}nav{margin-bottom:20px}"
         "nav a{color:#8fd3ff;margin-right:16px;text-decoration:none}"
         "nav a:hover{text-decoration:underline}"
         "label{display:block;margin-top:14px}"
         "input,select,textarea{box-sizing:border-box;width:100%;padding:10px;"
         "margin-top:5px;border-radius:8px;border:1px solid #555;"
         "background:#222;color:#fff}"
         ".card{background:#1a1a1a;border-radius:12px;padding:16px;margin:14px 0}"
         ".card h3{margin:0 0 10px}"
         "table{width:100%;border-collapse:collapse}"
         "td{padding:6px 2px;vertical-align:top}"
         "td:first-child{color:#888;width:42%;white-space:nowrap}"
         ".ok{color:#76d275}.warn{color:#ffd27f}.err{color:#ff6b6b}"
         ".hint{font-size:0.85em;color:#888;margin-top:3px}"
         "button{margin-top:20px;padding:12px 20px;border:0;border-radius:9px;"
         "background:#76d275;color:#111;font-weight:bold;cursor:pointer}"
         "</style></head><body>");
  return h;
}

// 共通ナビゲーションバーを生成する
String commonNav() {
  return F("<nav><a href='/'>&#9881; Settings</a>"
           "<a href='/status'>&#10003; Status</a></nav>");
}

}  // namespace

bool ConfigPortal::begin() {
  load();           // NVSから設定を読み込む
  registerRoutes(); // Webサーバのルートを登録する

  // 保存済みSSIDがあればWiFi接続を試みる
  if (!config_.wifiSsid.isEmpty() && connectWifi()) {
    server_.begin(); // 接続成功: STAモードでWebサーバ起動
    return true;
  }

  // 接続失敗: セットアップ用APを起動する
  startPortal();
  return false;
}

// WebサーバのHTTPリクエストを処理する（毎フレーム呼ぶ必要がある）
void ConfigPortal::update() {
  server_.handleClient();
}

bool ConfigPortal::isPortalActive() const {
  return portalActive_;
}

bool ConfigPortal::isConnected() const {
  return WiFi.status() == WL_CONNECTED;
}

const AppConfig& ConfigPortal::config() const {
  return config_;
}

// APモード時はsoftAPのIPを、STA接続時はDHCPで取得したIPを返す
IPAddress ConfigPortal::localIp() const {
  return portalActive_ ? WiFi.softAPIP() : WiFi.localIP();
}

String ConfigPortal::accessPointName() const {
  return accessPointName_;
}

void ConfigPortal::setRuntimeStatus(const RuntimeStatus& status) {
  runtimeStatus_ = status;
}

void ConfigPortal::setSpeakRequestHandler(
    std::function<bool(const String&)> handler) {
  speakRequestFn_ = handler;
}

void ConfigPortal::setSpeakingProbe(std::function<bool()> probe) {
  speakingProbe_ = probe;
}

// SETTINGSメニューを開いたときに呼ぶ。
// 既存のWiFi接続（STA）を切断せず、AP_STAモードで追加APを起動する。
void ConfigPortal::startSettingsAp() {
  if (settingsApActive_ || portalActive_) return;
  ensureAccessPointName();
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(accessPointName_.c_str(), "stackchan");
  settingsApActive_ = true;
}

// SETTINGSメニューを閉じたときに呼ぶ。APを停止してSTAモードに戻す。
void ConfigPortal::stopSettingsAp() {
  if (!settingsApActive_) return;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  settingsApActive_ = false;
}

bool ConfigPortal::isSettingsApActive() const {
  return settingsApActive_;
}

IPAddress ConfigPortal::settingsApIp() const {
  return WiFi.softAPIP();
}

// NVSのnamespace "stackchan" から設定を読み込む。
// 古いキー名（vv_host, vv_port 等）との後方互換性も維持する。
void ConfigPortal::load() {
  preferences_.begin("stackchan", true);
  config_.wifiSsid = preferences_.getString("ssid", "");
  config_.wifiPassword = preferences_.getString("password", "");
  config_.ttsHost = preferences_.getString(
      "tts_host", preferences_.getString("vv_host", config_.ttsHost));
  config_.ttsPort = preferences_.getUShort(
      "tts_port", preferences_.getUShort("vv_port", config_.ttsPort));
  config_.ttsSpeaker = preferences_.getString(
      "tts_speaker",
      String(preferences_.getInt("vv_speaker", config_.ttsSpeaker.toInt())));
  config_.ttsEngineType = sanitizedTtsEngineType(
      preferences_.getString("tts_engine", config_.ttsEngineType));
  config_.speechText =
      preferences_.getString("speech", config_.speechText);
  config_.cameraGaze = preferences_.getBool("cam_gaze", config_.cameraGaze);
  config_.gamingRgb = preferences_.getBool("gaming_rgb", config_.gamingRgb);
  preferences_.end();
}

// 現在の設定をNVSに保存する（保存後は再起動が必要）
void ConfigPortal::save() {
  preferences_.begin("stackchan", false);
  preferences_.putString("ssid", config_.wifiSsid);
  preferences_.putString("password", config_.wifiPassword);
  preferences_.putString("tts_host", config_.ttsHost);
  preferences_.putUShort("tts_port", config_.ttsPort);
  preferences_.putString("tts_speaker", config_.ttsSpeaker);
  preferences_.putString("tts_engine", config_.ttsEngineType);
  preferences_.putString("speech", config_.speechText);
  preferences_.putBool("cam_gaze", config_.cameraGaze);
  preferences_.putBool("gaming_rgb", config_.gamingRgb);
  preferences_.end();
}

// 保存済みSSID/パスワードでWiFiに接続する（最大15秒待機）
bool ConfigPortal::connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(config_.wifiSsid.c_str(), config_.wifiPassword.c_str());

  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 15000) {
    delay(100);
  }
  return WiFi.status() == WL_CONNECTED;
}

// APのSSID名をMACアドレス末尾3バイトで生成する（未設定の場合のみ）
void ConfigPortal::ensureAccessPointName() {
  if (!accessPointName_.isEmpty()) return;
  const uint64_t chipId = ESP.getEfuseMac();
  char suffix[7];
  snprintf(suffix, sizeof(suffix), "%06llX",
           static_cast<unsigned long long>(chipId & 0xFFFFFF));
  accessPointName_ = "StackChan-Setup-" + String(suffix);
}

// WiFi接続失敗時のセットアップ用APを起動する
void ConfigPortal::startPortal() {
  portalActive_ = true;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP_STA);
  ensureAccessPointName();
  WiFi.softAP(accessPointName_.c_str(), "stackchan");
  server_.begin();
}

// WebサーバのURLルートを登録する
void ConfigPortal::registerRoutes() {
  // GET / → 設定ページ（現在値を事前入力済み）
  server_.on("/", HTTP_GET, [this]() {
    server_.send(200, "text/html; charset=utf-8", pageHtml());
  });

  // POST /save → 設定を保存して再起動
  server_.on("/save", HTTP_POST, [this]() {
    const String manualSsid = server_.arg("manual_ssid");
    config_.wifiSsid =
        manualSsid.isEmpty() ? server_.arg("ssid") : manualSsid;

    // パスワードは送信された場合のみ更新（空欄なら既存値を維持）
    const String submittedPassword = server_.arg("password");
    if (!submittedPassword.isEmpty()) {
      config_.wifiPassword = submittedPassword;
    }

    config_.ttsHost = server_.arg("tts_host");
    config_.ttsPort =
        constrain(server_.arg("tts_port").toInt(), 1, 65535);
    config_.ttsSpeaker = server_.arg("speaker");
    config_.ttsSpeaker.trim();
    if (config_.ttsSpeaker.isEmpty()) {
      config_.ttsSpeaker = "3";
    }
    config_.ttsEngineType =
        sanitizedTtsEngineType(server_.arg("tts_engine"));
    config_.speechText = server_.arg("speech");
    // チェックボックスは未チェック時にPOSTされないため、存在で判定する
    config_.cameraGaze = server_.hasArg("camera_gaze");
    config_.gamingRgb = server_.hasArg("gaming_rgb");
    save();

    server_.send(200, "text/html; charset=utf-8",
                 pageHtml("Saved. Restarting StackChan..."));
    delay(800);
    ESP.restart();
  });

  // GET /status → システム状態ページ（5秒自動更新）
  server_.on("/status", HTTP_GET, [this]() {
    server_.send(200, "text/html; charset=utf-8", statusHtml());
  });

  // POST /api/speak → Gateway からの発話指示（ブラウザ送信→自動発話）。
  // 任意の text フォーム引数、または text/plain 本文で発話テキストを指定できる。
  // 話し中なら busy を返す。受け付けたら ok:true（実際の再生は loop() 側）。
  server_.on("/api/speak", HTTP_POST, [this]() {
    String text;
    if (server_.hasArg("text")) {
      text = server_.arg("text");
    } else if (server_.hasArg("plain")) {
      text = server_.arg("plain"); // text/plain 本文
    }
    const bool accepted = speakRequestFn_ ? speakRequestFn_(text) : false;
    if (accepted) {
      server_.send(200, "application/json", "{\"ok\":true}");
    } else {
      server_.send(200, "application/json",
                   "{\"ok\":false,\"error\":\"busy\"}");
    }
  });

  // GET /api/status → デバイス状態をJSONで返す（Gateway の疎通確認用）
  server_.on("/api/status", HTTP_GET, [this]() {
    server_.send(200, "application/json", apiStatusJson());
  });
}

// 設定WebページのHTMLを生成する。
// 現在保存されている値をフォームに事前入力する。
// パスワードは値を表示せず、設定済みかどうかのみを示す。
String ConfigPortal::pageHtml(const String& message) {
  String html;
  html.reserve(5500);
  html += commonHead("StackChan Setup");
  html += F("<h1>StackChan Setup</h1>");
  html += commonNav();

  if (!message.isEmpty()) {
    html += "<p><strong>" + htmlEscape(message) + "</strong></p>";
  }

  html += F("<form method='post' action='/save'>");

  // --- Wi-Fi SSID ---
  html += F("<label>Wi-Fi SSID"
            "<select name='ssid' style='box-sizing:border-box;width:100%;"
            "padding:10px;margin-top:5px;border-radius:8px;border:1px solid #555;"
            "background:#222;color:#fff'>");
  html += wifiOptionsHtml();
  html += F("</select></label>"
            "<p><a href='/' style='color:#8fd3ff'>Rescan nearby Wi-Fi</a></p>"
            "<label>Enter SSID manually (for hidden networks)"
            "<input name='manual_ssid' placeholder='Takes priority when filled'>"
            "</label>");

  // --- Wi-Fi パスワード（値は表示しない、設定済み状態のみ示す）---
  html += F("<label>Wi-Fi Password");
  if (!config_.wifiPassword.isEmpty()) {
    html += F("<span class='hint'> &mdash; currently set, leave blank to keep</span>");
  } else {
    html += F("<span class='hint'> &mdash; not set</span>");
  }
  html += F("<input type='password' name='password' "
            "placeholder='Enter to change'></label>");

  // --- TTS エンジン種別 ---
  html += F("<label>TTS Engine Type"
            "<select name='tts_engine' style='box-sizing:border-box;width:100%;"
            "padding:10px;margin-top:5px;border-radius:8px;border:1px solid #555;"
            "background:#222;color:#fff'>");
  html += "<option value='voicevox_compatible'" +
          selectedAttribute(config_.ttsEngineType, "voicevox_compatible") +
          ">voicevox_compatible</option>";
  html += "<option value='simple_wav'" +
          selectedAttribute(config_.ttsEngineType, "simple_wav") +
          ">simple_wav</option>";
  html += F("</select></label>");

  // --- TTS ホスト・ポート・話者（すべて現在値を事前入力）---
  html += F("<label>TTS Host");
  html += "<input name='tts_host' value='" + htmlEscape(config_.ttsHost) + "'>";
  html += F("</label><label>TTS Port");
  html += "<input type='number' name='tts_port' min='1' max='65535' value='" +
          String(config_.ttsPort) + "'>";
  html += F("</label><label>Speaker / Style ID (Zundamon Normal: 3)");
  html += "<input name='speaker' value='" + htmlEscape(config_.ttsSpeaker) + "'>";
  html += F("</label>");

  // --- Aボタンで話すテキスト（現在値を事前入力）---
  html += F("<label>Text to speak (A button)");
  html += "<textarea name='speech' rows='4'>" +
          htmlEscape(config_.speechText) + "</textarea>";
  html += F("</label>");

  // --- カメラ目線（明るい方向へ目を向ける）---
  html += F("<label style='margin-top:16px'>"
            "<input type='checkbox' name='camera_gaze' value='1' "
            "style='width:auto;margin-right:8px'");
  if (config_.cameraGaze) {
    html += F(" checked");
  }
  html += F(">Camera gaze (look toward bright light)</label>"
           "<p class='hint'>Uses the front camera. On CoreS3 the camera shares "
           "the internal I2C bus with the touch screen; if touch becomes "
           "unresponsive, turn this OFF and save.</p>");

  // --- ゲーミングRGB（顔と本体LEDを虹色に循環）---
  html += F("<label style='margin-top:16px'>"
            "<input type='checkbox' name='gaming_rgb' value='1' "
            "style='width:auto;margin-right:8px'");
  if (config_.gamingRgb) {
    html += F(" checked");
  }
  html += F(">Gaming RGB (rainbow face &amp; LED)</label>"
           "<p class='hint'>Cycles the avatar face and the body LED through "
           "rainbow colors. Turn OFF for the normal two-tone face and "
           "status-colored LED.</p>");

  html += F("<button type='submit'>Save &amp; Restart</button></form>"
            "<p style='color:#555;font-size:0.85em;margin-top:20px'>"
            "Configure your TTS server to accept HTTP connections "
            "from the same LAN.</p>");

  // --- Local LLM Chat（Android Gateway の /ask をブラウザから直接叩く）---
  // ESP32 は中継しない。fetch で http://<tts_host>:<tts_port>/ask に POST し、
  // 返ってきた JSON（question/answer/stackchan）を画面に表示する。
  // 発話自体は Gateway が StackChan の /api/speak を呼ぶことで行われる。
  html += chatHtml();

  html += F("</body></html>");
  return html;
}

// Local LLM Chat カードと、Gateway /ask を呼ぶフロントエンド JS を生成する。
// TTS Host/Port は保存済みの設定値をそのまま JS 文字列/数値として埋め込む。
String ConfigPortal::chatHtml() {
  String html;
  html.reserve(2200);

  html += F("<div class='card'><h3>Local LLM Chat</h3>"
            "<textarea id='chatText' rows='3'>");
  html += htmlEscape(F("自己紹介して"));
  html += F("</textarea>"
           "<button type='button' id='chatSend' "
           "onclick='askLlm()'>LLM&#12391;&#22238;&#31572;&#12375;&#12390;&#21796;&#12427;</button>"
           "<p id='chatStatus' class='hint'></p>"
           "<pre id='chatAnswer' style='white-space:pre-wrap;word-break:break-word;"
           "background:#222;color:#fff;border-radius:8px;padding:10px;"
           "margin-top:10px;display:none'></pre>"
           "</div>");

  // Gateway のベースURL（保存済みの TTS Host/Port）を JS に埋め込む
  html += F("<script>var GW_HOST=\"");
  html += jsonEscape(config_.ttsHost);
  html += F("\";var GW_PORT=");
  html += String(config_.ttsPort);
  html += F(";\n"
           "function askLlm(){"
           "var t=document.getElementById('chatText').value;"
           "var st=document.getElementById('chatStatus');"
           "var pre=document.getElementById('chatAnswer');"
           "var btn=document.getElementById('chatSend');"
           "if(!GW_HOST){st.textContent='TTS Host is not set';return;}"
           "st.textContent='thinking...';st.className='hint';"
           "btn.disabled=true;"
           "var url='http://'+GW_HOST+':'+GW_PORT+'/ask';"
           "fetch(url,{method:'POST',"
           "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
           "body:'text='+encodeURIComponent(t)})"
           ".then(function(r){if(!r.ok)throw new Error('HTTP '+r.status);return r.json();})"
           ".then(function(d){"
           "var out='';"
           "if(d.question!=null)out+='Q: '+d.question+'\\n\\n';"
           "if(d.answer!=null)out+='A: '+d.answer;"
           "if(d.stackchan!=null)out+='\\n\\n[stackchan] '+"
           "(typeof d.stackchan==='object'?JSON.stringify(d.stackchan):d.stackchan);"
           "pre.textContent=out||JSON.stringify(d);"
           "pre.style.display='block';"
           "st.textContent='done';st.className='ok';"
           "})"
           ".catch(function(e){"
           "st.textContent='fetch error: '+e.message;st.className='err';"
           "})"
           ".finally(function(){btn.disabled=false;});"
           "}\n"
           "</script>");
  return html;
}

// システム状態ページのHTMLを生成する。
// WiFi状態・TTS設定・システム情報・キャリブレーション状態を表示する。
// 5秒ごとに自動更新する。
String ConfigPortal::statusHtml() {
  String html;
  html.reserve(4000);

  // 5秒ごとに自動更新（<meta refresh>）
  html += F("<!doctype html><html lang='en'><head>"
            "<meta charset='utf-8'><meta name='viewport' "
            "content='width=device-width,initial-scale=1'>"
            "<meta http-equiv='refresh' content='5'>"
            "<title>StackChan Status</title>");

  // commonHead のスタイルを直接埋め込む（<meta refresh>と競合しないよう分離）
  html += F("<style>"
            "body{font-family:sans-serif;max-width:640px;margin:24px auto;"
            "padding:0 16px;background:#111;color:#eee}"
            "h1{margin-bottom:4px}nav{margin-bottom:20px}"
            "nav a{color:#8fd3ff;margin-right:16px;text-decoration:none}"
            "nav a:hover{text-decoration:underline}"
            ".card{background:#1a1a1a;border-radius:12px;padding:16px;margin:14px 0}"
            ".card h3{margin:0 0 10px}"
            "table{width:100%;border-collapse:collapse}"
            "td{padding:6px 2px;vertical-align:top}"
            "td:first-child{color:#888;width:42%;white-space:nowrap}"
            ".ok{color:#76d275}.warn{color:#ffd27f}.err{color:#ff6b6b}"
            ".sub{color:#555;font-size:0.85em}"
            "</style></head><body>");

  html += F("<h1>StackChan Status</h1>");
  html += commonNav();

  // --- Wi-Fi 状態 ---
  html += F("<div class='card'><h3>Wi-Fi</h3><table>");
  if (isConnected()) {
    const int32_t rssi = WiFi.RSSI();
    const char* strength =
        rssi >= -55 ? "strong" : (rssi >= -70 ? "medium" : "weak");
    html += F("<tr><td>Status</td><td class='ok'>Connected</td></tr>");
    html += "<tr><td>SSID</td><td>" + htmlEscape(WiFi.SSID()) + "</td></tr>";
    html += "<tr><td>IP Address</td><td>" +
            WiFi.localIP().toString() + "</td></tr>";
    html += "<tr><td>Signal</td><td>" + String(rssi) + " dBm (" +
            String(strength) + ")</td></tr>";
    html += "<tr><td>Gateway</td><td>" +
            WiFi.gatewayIP().toString() + "</td></tr>";
  } else if (portalActive_) {
    html += F("<tr><td>Status</td><td class='warn'>Setup AP (not connected)</td></tr>");
    html += "<tr><td>AP Name</td><td>" + htmlEscape(accessPointName_) + "</td></tr>";
    html += "<tr><td>AP IP</td><td>" + WiFi.softAPIP().toString() + "</td></tr>";
    html += F("<tr><td>Password</td><td>stackchan</td></tr>");
  } else {
    html += F("<tr><td>Status</td><td class='err'>Not connected</td></tr>");
    if (!config_.wifiSsid.isEmpty()) {
      html += "<tr><td>Saved SSID</td><td>" +
              htmlEscape(config_.wifiSsid) + "</td></tr>";
    }
  }

  // Settings AP が起動中ならその情報も表示する
  if (settingsApActive_) {
    html += "<tr><td>Settings AP</td><td class='ok'>" +
            htmlEscape(accessPointName_) + "<br><span class='sub'>" +
            WiFi.softAPIP().toString() + " / pass: stackchan</span></td></tr>";
  }
  html += F("</table></div>");

  // --- TTS サーバ設定 ---
  html += F("<div class='card'><h3>TTS Server</h3><table>");
  html += "<tr><td>Engine</td><td>" +
          htmlEscape(config_.ttsEngineType) + "</td></tr>";
  html += "<tr><td>Host</td><td>" + htmlEscape(config_.ttsHost) + "</td></tr>";
  html += "<tr><td>Port</td><td>" + String(config_.ttsPort) + "</td></tr>";
  html += "<tr><td>Speaker ID</td><td>" +
          htmlEscape(config_.ttsSpeaker) + "</td></tr>";

  // TTS URL を表示（同一LAN接続時のみクリック可能）
  if (isConnected()) {
    const String ttsUrl = "http://" + config_.ttsHost + ":" +
                          String(config_.ttsPort) + "/";
    html += "<tr><td>URL</td><td><a href='" + htmlEscape(ttsUrl) +
            "' style='color:#8fd3ff'>" + htmlEscape(ttsUrl) + "</a></td></tr>";
  }
  html += F("</table></div>");

  // --- アプリ状態 ---
  html += F("<div class='card'><h3>App</h3><table>");
  html += "<tr><td>Mode</td><td>" +
          htmlEscape(runtimeStatus_.appMode) + "</td></tr>";
  html += String("<tr><td>Servo Cal</td><td class='") +
          (runtimeStatus_.servoCalibrated ? "ok'>OK" : "warn'>Not calibrated") +
          "</td></tr>";
  html += String("<tr><td>IMU Cal</td><td class='") +
          (runtimeStatus_.imuCalibrated ? "ok'>OK" : "warn'>Not calibrated") +
          "</td></tr>";
  html += String("<tr><td>Camera Gaze</td><td class='") +
          (runtimeStatus_.cameraActive ? "ok'>Active" : "warn'>Off") +
          "</td></tr>";
  html += String("<tr><td>Gaming RGB</td><td class='") +
          (config_.gamingRgb ? "ok'>On" : "warn'>Off") +
          "</td></tr>";
  html += F("</table></div>");

  // --- 診断（クラッシュ調査用）---
  // Last Reset が PANIC=コードのクラッシュ、BROWNOUT=電源不足、
  // TASK/INT WDT=ハング、POWERON=正常な電源投入。
  // Max Alloc が Free に比べて極端に小さい場合はヒープ断片化のサイン。
  html += F("<div class='card'><h3>Diagnostics</h3><table>");
  {
    const char* rrClass = "ok";
    if (runtimeStatus_.resetReason == "PANIC" ||
        runtimeStatus_.resetReason == "BROWNOUT" ||
        runtimeStatus_.resetReason.indexOf("WDT") >= 0) {
      rrClass = "err";
    }
    html += String("<tr><td>Last Reset</td><td class='") + rrClass + "'>" +
            htmlEscape(runtimeStatus_.resetReason) + "</td></tr>";
  }
  html += "<tr><td>Free Heap</td><td>" +
          String(runtimeStatus_.freeHeap / 1024) + " KB</td></tr>";
  html += String("<tr><td>Min Free Heap</td><td class='") +
          (runtimeStatus_.minFreeHeap < 20000 ? "err" : "ok") + "'>" +
          String(runtimeStatus_.minFreeHeap / 1024) + " KB</td></tr>";
  html += String("<tr><td>Max Alloc Block</td><td class='") +
          (runtimeStatus_.maxAllocHeap < 12000 ? "err" : "ok") + "'>" +
          String(runtimeStatus_.maxAllocHeap / 1024) + " KB</td></tr>";
  html += "<tr><td>Free PSRAM</td><td>" +
          String(runtimeStatus_.freePsram / 1024) + " KB</td></tr>";
  html += F("</table></div>");

  // --- システム情報 ---
  const uint32_t uptimeSec = millis() / 1000;
  char uptime[16];
  snprintf(uptime, sizeof(uptime), "%02lu:%02lu:%02lu",
           (unsigned long)(uptimeSec / 3600),
           (unsigned long)((uptimeSec % 3600) / 60),
           (unsigned long)(uptimeSec % 60));
  const uint32_t freeHeap = ESP.getFreeHeap();

  html += F("<div class='card'><h3>System</h3><table>");
  html += "<tr><td>Uptime</td><td>" + String(uptime) + "</td></tr>";
  html += "<tr><td>Free Heap</td><td>" + String(freeHeap / 1024) +
          " KB <span class='sub'>(" + String(freeHeap) + " B)</span></td></tr>";
  html += "<tr><td>CPU Freq</td><td>" +
          String(ESP.getCpuFreqMHz()) + " MHz</td></tr>";
  html += "<tr><td>Flash Size</td><td>" +
          String(ESP.getFlashChipSize() / 1024 / 1024) + " MB</td></tr>";
  html += F("</table></div>");

  html += F("<p class='sub'>Auto-refreshes every 5 seconds</p>"
            "</body></html>");
  return html;
}

// 周辺のWiFiをスキャンして<option>タグのリストを生成する。
// 重複SSIDを除外し、電波強度・セキュリティ情報を付記する。
// 保存済みSSIDが見つかれば selected 属性を付けて事前選択する。
String ConfigPortal::wifiOptionsHtml() {
  String options;
  options.reserve(2000);

  const int count = WiFi.scanNetworks(false, true);
  if (count <= 0) {
    options += F("<option value=''>No Wi-Fi networks found</option>");
    WiFi.scanDelete();
    return options;
  }

  bool currentSsidFound = false;
  for (int i = 0; i < count; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) {
      continue;
    }

    // 重複SSIDは除外する
    bool duplicate = false;
    for (int previous = 0; previous < i; ++previous) {
      if (WiFi.SSID(previous) == ssid) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;

    const int32_t rssi = WiFi.RSSI(i);
    const char* strength =
        rssi >= -55 ? "strong" : (rssi >= -70 ? "medium" : "weak");
    const bool secured = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    const bool selected = ssid == config_.wifiSsid;
    currentSsidFound |= selected;

    options += "<option value='" + htmlEscape(ssid) + "'";
    if (selected) options += F(" selected");
    options += ">" + htmlEscape(ssid) + " (" + String(strength) +
               ", " + String(rssi) + " dBm";
    if (secured) options += F(", secured");
    options += F(")</option>");
  }

  // 保存済みSSIDがスキャン結果にない場合（圏外など）は末尾に追加する
  if (!config_.wifiSsid.isEmpty() && !currentSsidFound) {
    options += "<option value='" + htmlEscape(config_.wifiSsid) +
               "' selected>" + htmlEscape(config_.wifiSsid) +
               " (saved, not in range)</option>";
  }

  WiFi.scanDelete();
  return options;
}

// GET /api/status が返すJSONを生成する。
// 項目: ok, connected, speaking, ip, tts_host, tts_port, tts_engine
String ConfigPortal::apiStatusJson() {
  const bool spk = speakingProbe_ ? speakingProbe_() : false;
  String ip;
  if (isConnected()) {
    ip = WiFi.localIP().toString();
  } else if (portalActive_ || settingsApActive_) {
    ip = WiFi.softAPIP().toString();
  }

  String j;
  j.reserve(360);
  j += F("{\"ok\":true,\"connected\":");
  j += isConnected() ? F("true") : F("false");
  j += F(",\"speaking\":");
  j += spk ? F("true") : F("false");
  j += F(",\"ip\":\"");
  j += ip;
  j += F("\",\"tts_host\":\"");
  j += jsonEscape(config_.ttsHost);
  j += F("\",\"tts_port\":");
  j += String(config_.ttsPort);
  j += F(",\"tts_engine\":\"");
  j += jsonEscape(config_.ttsEngineType);
  j += F("\",\"reset_reason\":\"");
  j += jsonEscape(runtimeStatus_.resetReason);
  j += F("\",\"free_heap\":");
  j += String(runtimeStatus_.freeHeap);
  j += F(",\"min_free_heap\":");
  j += String(runtimeStatus_.minFreeHeap);
  j += F(",\"max_alloc_heap\":");
  j += String(runtimeStatus_.maxAllocHeap);
  j += F(",\"free_psram\":");
  j += String(runtimeStatus_.freePsram);
  j += F("}");
  return j;
}
