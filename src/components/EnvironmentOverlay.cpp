#include "EnvironmentOverlay.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "components/UITheme.h"
#include "fontIds.h"

int EnvironmentOverlay::reservedHeight(const GfxRenderer& renderer, const int bottomInset) {
  return HalEnvironmentSensor::supported() ? renderer.getLineHeight(SMALL_FONT_ID) + 8 + bottomInset : 0;
}

void EnvironmentOverlay::draw(const GfxRenderer& renderer, const HalEnvironmentSensor::Reading& reading,
                              const bool snapshot, const int bottomInset) {
  if (!HalEnvironmentSensor::supported()) return;
  char value[64];
  if (reading.valid) {
    std::snprintf(value, sizeof(value), tr(STR_ENVIRONMENT_READING), static_cast<double>(reading.temperatureC),
                  static_cast<unsigned>(std::lround(reading.humidityPct)));
  } else {
    std::snprintf(value, sizeof(value), "%s", tr(STR_ENVIRONMENT_UNAVAILABLE));
  }
  char text[96];
  std::snprintf(text, sizeof(text), snapshot ? tr(STR_ENVIRONMENT_BEFORE_SLEEP) : "%s", value);

  int top, right, bottom, left;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  const Rect viewport{left, top, renderer.getScreenWidth() - left - right, renderer.getScreenHeight() - top - bottom};
  const int height = renderer.getLineHeight(SMALL_FONT_ID) + 8;
  const int width = std::min(viewport.width, renderer.getTextWidth(SMALL_FONT_ID, text) + 16);
  const int y = viewport.y + viewport.height - bottomInset - height;
  const int x = viewport.x + (viewport.width - width) / 2;

  // Overlay gray planes encode only gray selector pixels: clear our B/W badge
  // out of both selector planes. Absolute planes instead need its actual pixels.
  const bool selectors = renderer.getRenderMode() != GfxRenderer::BW && !renderer.grayPlanesAreAbsolute();
  renderer.fillRect(x, y, width, height, selectors);
  if (!selectors) UITheme::drawCenteredText(renderer, viewport, SMALL_FONT_ID, y + 4, text);
}
