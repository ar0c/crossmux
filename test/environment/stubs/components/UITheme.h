#pragma once
#include <GfxRenderer.h>
struct Rect {
  int x, y, width, height;
};
struct UITheme {
  static void drawCenteredText(const GfxRenderer& renderer, Rect, int, int, const char* text) {
    ++renderer.textDraws;
    renderer.text = text;
  }
};
