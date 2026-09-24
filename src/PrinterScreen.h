#pragma once

#include <M5Unified.h>

#include "PrinterState.h"

// 本体画面のプリンタ詳細画面（顔をタップすると開く、16bit フルカラー）。
// 進捗リング・残り時間・完成予定・温度・AMS・最新の実況をまとめて表示する。
void drawPrinterScreen(M5Canvas& canvas, const PrinterState& state,
                       const String& lastComment, bool voice);
