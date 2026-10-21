#pragma once
#include <cassert>
#include <string>

struct GfxRenderer {
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };
  RenderMode mode = BW;
  bool absolute = false;
  int width = 480, height = 800;
  mutable int fills = 0, textDraws = 0;
  mutable bool fillBlack = false;
  mutable int fillY = 0, fillHeight = 0;
  mutable std::string text;
  int getLineHeight(int) const { return 20; }
  int getTextWidth(int, const char* value) const { return static_cast<int>(std::string(value).size()) * 6; }
  int getScreenWidth() const { return width; }
  int getScreenHeight() const { return height; }
  void getOrientedViewableTRBL(int* t, int* r, int* b, int* l) const { *t = *r = *b = *l = 4; }
  RenderMode getRenderMode() const { return mode; }
  bool grayPlanesAreAbsolute() const { return absolute; }
  void fillRect(int x, int y, int w, int h, bool black) const {
    assert(x >= 4 && y >= 4 && x + w <= width - 4 && y + h <= height - 4);
    ++fills;
    fillBlack = black;
    fillY = y;
    fillHeight = h;
  }
};
