#pragma once

#include <HalEnvironmentSensor.h>

class GfxRenderer;

namespace EnvironmentOverlay {
// Includes the bottom spacing, so faces can reserve the same band in all modes.
int reservedHeight(const GfxRenderer& renderer, int bottomInset);
bool visibleReadingChanged(const HalEnvironmentSensor::Reading& before, const HalEnvironmentSensor::Reading& after);
void draw(const GfxRenderer& renderer, const HalEnvironmentSensor::Reading& reading, bool snapshot,
          int bottomInset = 8);
}  // namespace EnvironmentOverlay
