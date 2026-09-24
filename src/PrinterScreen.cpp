#include "PrinterScreen.h"

#include <math.h>

#include "PrinterJson.h"

namespace {

const lgfx::IFont* const kJp = &fonts::lgfxJapanGothicP_16;
const lgfx::IFont* const kBig = &fonts::Font4;

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

constexpr uint16_t kBg = rgb(14, 16, 20);
constexpr uint16_t kCard = rgb(23, 26, 33);
constexpr uint16_t kLine = rgb(38, 43, 53);
constexpr uint16_t kText = rgb(233, 235, 241);
constexpr uint16_t kSub = rgb(139, 146, 163);
constexpr uint16_t kGreen = rgb(74, 222, 128);
constexpr uint16_t kBlue = rgb(96, 165, 250);
constexpr uint16_t kAmber = rgb(251, 191, 36);
constexpr uint16_t kRed = rgb(248, 113, 113);
constexpr uint16_t kPurple = rgb(192, 132, 252);

uint16_t phaseColor(PrintPhase phase) {
  switch (phase) {
    case PrintPhase::Running: return kGreen;
    case PrintPhase::Prepare:
    case PrintPhase::Slicing: return kBlue;
    case PrintPhase::Pause:   return kAmber;
    case PrintPhase::Finish:  return kPurple;
    case PrintPhase::Failed:  return kRed;
    default:                  return kSub;
  }
}

uint16_t from888(uint32_t rgb888) {
  return rgb((rgb888 >> 16) & 0xFF, (rgb888 >> 8) & 0xFF, rgb888 & 0xFF);
}

bool isLight(uint32_t rgb888) {
  const int r = (rgb888 >> 16) & 0xFF;
  const int g = (rgb888 >> 8) & 0xFF;
  const int b = rgb888 & 0xFF;
  return (r * 299 + g * 587 + b * 114) / 1000 > 150;
}

// 幅に収まらない UTF-8 文字列を末尾 "…" で切り詰めて描く
void drawFitted(M5Canvas& c, const String& text, int x, int y, int maxWidth) {
  if (c.textWidth(text) <= maxWidth) {
    c.drawString(text, x, y);
    return;
  }
  const int ellipsisW = c.textWidth("…");
  String cut = text;
  while (cut.length() > 0 && c.textWidth(cut) + ellipsisW > maxWidth) {
    int len = cut.length();
    do {
      --len;
    } while (len > 0 && (static_cast<uint8_t>(cut[len]) & 0xC0) == 0x80);
    cut.remove(len);
  }
  c.drawString(cut + "…", x, y);
}

String tempPair(float current, float target) {
  String s = isnan(current) ? String("-") : String(static_cast<int>(lroundf(current)));
  if (!isnan(target) && target > 0.0f) {
    s += "/" + String(static_cast<int>(lroundf(target)));
  }
  return s + "℃";
}

const char* linkLabel(const PrinterState& s) {
  switch (s.link) {
    case LinkState::Online:      return s.synced ? "接続中" : "取得中…";
    case LinkState::Connecting:  return "接続試行中";
    case LinkState::WaitingWifi: return "Wi-Fi待ち";
    case LinkState::Error:       return "接続エラー";
    default:                     return "未設定";
  }
}

}  // namespace

void drawPrinterScreen(M5Canvas& c, const PrinterState& s,
                       const String& lastComment, bool voice) {
  const int w = c.width();
  const int h = c.height();
  const uint16_t accent = phaseColor(s.phase);
  const bool online = s.link == LinkState::Online && s.synced;
  const bool active = s.isActive();

  c.fillScreen(kBg);
  c.setTextSize(1);
  c.setTextWrap(false);

  // --- ヘッダー ---
  c.fillRect(0, 0, w, 26, kCard);
  c.fillCircle(12, 13, 5, online ? accent : kRed);
  c.setFont(kJp);
  c.setTextColor(kText, kCard);
  c.setTextDatum(middle_left);
  c.drawString(online ? printPhaseLabelJa(s.phase) : linkLabel(s), 24, 14);
  c.setTextDatum(middle_right);
  c.setTextColor(kSub, kCard);
  String right = voice ? "実況ON" : "実況OFF";
  if (online && s.wifiDbm != 0) right = String(s.wifiDbm) + "dBm  " + right;
  c.drawString(right, w - 8, 14);

  // --- ジョブ名 ---
  c.setTextColor(kText, kBg);
  c.setTextDatum(top_left);
  String job = s.jobName;
  if (job.isEmpty()) job = active ? "（名前なし）" : "ジョブなし";
  drawFitted(c, job, 10, 32, w - 20);

  // --- 進捗リング ---
  const int cx = 60;
  const int cy = 100;
  const int percent = s.phase == PrintPhase::Finish ? 100 : max(s.percent, 0);
  c.fillArc(cx, cy, 44, 34, 0, 360, kLine);
  if (percent > 0) {
    c.fillArc(cx, cy, 44, 34, 270, 270 + 360 * percent / 100, accent);
  }
  c.setFont(kBig);
  c.setTextColor(kText, kBg);
  c.setTextDatum(middle_center);
  c.drawString(s.percent < 0 && !active ? String("--") : String(percent) + "%",
               cx, cy + 1);

  // --- 右側の情報 ---
  c.setFont(kJp);
  c.setTextDatum(top_left);
  const int tx = 118;
  int ty = 60;
  auto row = [&](const char* label, const String& value) {
    c.setTextColor(kSub, kBg);
    c.drawString(label, tx, ty);
    c.setTextColor(kText, kBg);
    drawFitted(c, value, tx + 70, ty, w - tx - 78);
    ty += 21;
  };
  if (active) {
    row("残り", remainingJa(s.remainingMin));
    const String eta = etaClockShort(s.remainingMin);
    row("完成予定", eta.isEmpty() ? String("--") : eta);
  } else {
    row("状態", String(printPhaseLabelJa(s.phase)));
    row("速度", String(speedLabelJa(s.speed)));
  }
  row("レイヤー", s.totalLayers > 0 ? String(max(s.layer, 0)) + " / " +
                                          String(s.totalLayers)
                                    : String("--"));
  const char* stage = stageLabelJa(s.stage);
  row("工程", active && stage != nullptr ? String(stage) : String("--"));

  // --- 温度 ---
  c.fillRoundRect(8, 146, w - 16, 26, 8, kCard);
  c.setTextDatum(middle_left);
  c.setTextColor(kSub, kCard);
  c.drawString("ノズル", 16, 159);
  c.setTextColor(kText, kCard);
  c.drawString(tempPair(s.nozzleTemp, s.nozzleTarget), 70, 159);
  c.setTextColor(kSub, kCard);
  c.drawString("ベッド", 170, 159);
  c.setTextColor(kText, kCard);
  c.drawString(tempPair(s.bedTemp, s.bedTarget), 222, 159);

  // --- AMS（先頭ユニット）またはファン ---
  if (s.amsCount > 0) {
    const int chipW = (w - 16 - 3 * 6) / 4;
    for (int i = 0; i < PrinterState::kTraysPerAms; ++i) {
      const AmsTray& t = s.trays[0][i];
      const int x = 8 + i * (chipW + 6);
      const int y = 178;
      const bool on = s.trayNow == i;
      if (t.present) {
        c.fillRoundRect(x, y, chipW, 22, 6, from888(t.color));
        c.setTextColor(isLight(t.color) ? kBg : kText);
      } else {
        c.fillRoundRect(x, y, chipW, 22, 6, kCard);
        c.setTextColor(kSub);
      }
      if (on) {
        c.drawRoundRect(x - 1, y - 1, chipW + 2, 24, 7, kText);
        c.drawRoundRect(x - 2, y - 2, chipW + 4, 26, 8, kGreen);
      }
      c.setTextDatum(middle_center);
      c.drawString(t.present ? String(t.type) : String("空"), x + chipW / 2,
                   y + 12);
    }
  } else {
    c.setTextDatum(middle_left);
    c.setTextColor(kSub, kBg);
    auto fan = [](int v) { return v < 0 ? String("-") : String(v) + "%"; };
    c.drawString("ファン 部品 " + fan(s.partFan) + "  補助 " + fan(s.auxFan) +
                     "  庫内 " + fan(s.chamberFan),
                 10, 189);
  }

  // --- 最新の実況 ---
  c.fillRoundRect(4, 208, w - 8, 29, 8, kCard);
  c.setTextDatum(middle_left);
  if (s.hmsCount > 0) {
    c.setTextColor(kAmber, kCard);
    drawFitted(c, "お知らせ " + String(s.hmsCount) + "件: " + hmsCodeString(s.hms[0]),
               12, 223, w - 24);
  } else {
    c.setTextColor(lastComment.isEmpty() ? kSub : kText, kCard);
    drawFitted(c, lastComment.isEmpty() ? String("タップで顔に戻ります") : lastComment,
               12, 223, w - 24);
  }
}
