#if defined(CROSSMUX_METALIO_EINK4_PLUS_PLATFORM)

#include <Arduino.h>
#include <esp_arduino_version.h>
#include <esp_system.h>
#include <sdkconfig.h>
#include <soc/soc_caps.h>

#if !CONFIG_IDF_TARGET_ESP32S31
#error "The Metalio E-Ink 4 Plus platform probe requires ESP32-S31"
#endif

static_assert(ESP_ARDUINO_VERSION_MAJOR == 4);
static_assert(SOC_LCD_RGB_SUPPORTED && SOC_RMT_SUPPORTED);
static_assert(SOC_SDMMC_HOST_SUPPORTED && SOC_LEDC_SUPPORTED);

void setup() { Serial.begin(115200); }

void loop() {
  Serial.printf("CrossMux Metalio E-Ink 4 Plus platform probe: IDF %s, Flash %lu B, PSRAM %lu B\n",
                esp_get_idf_version(), static_cast<unsigned long>(ESP.getFlashChipSize()),
                static_cast<unsigned long>(ESP.getPsramSize()));
  delay(2000);
}

#endif
