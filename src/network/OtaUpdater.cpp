#include "OtaUpdater.h"

// clang-format off
// HttpDownloader.h pulls Arduino/SdFat, whose macros collide with lwip's
// ip4_addr.h unless seen first. Pin this order; clang-format would otherwise sort
// the local header last and break the build.
#include "HttpDownloader.h"
#include <ArduinoJson.h>
#include <Logging.h>
#include <Memory.h>
#include <ReleaseJsonParser.h>
#include <esp_ota_ops.h>
#include <esp_wifi.h>
#include <mbedtls/sha256.h>
// clang-format on

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "CrossPointSettings.h"
#include "FirmwareBoardTag.h"
#include "FirmwareFlasher.h"

namespace {
constexpr size_t MAX_RELEASE_JSON = 8192;
constexpr std::string_view FORK_RELEASE_BASE = "https://ooo.ar0c.com/releases/download/";

const char* targetId() {
#if FREEINK_DEVICE_X4PRO
  return "xteink_x4_pro";
#elif FREEINK_DEVICE_WAVESHARE_EPAPER_397
  return "waveshare_epaper_397";
#else
  return nullptr;
#endif
}

bool isForkBuildUrl(const std::string_view url, const std::string_view channel) {
  if (!url.starts_with(FORK_RELEASE_BASE)) return false;
  const std::string_view path = url.substr(FORK_RELEASE_BASE.size());
  const std::string prefix = std::string(channel) + "-build-";
  const size_t slash = path.find('/');
  return slash != std::string_view::npos && path.starts_with(prefix) && slash > prefix.size() &&
         slash + 1 < path.size() && path.find('/', slash + 1) == std::string_view::npos &&
         path.find_first_of("?#\\") == std::string_view::npos;
}

bool fetchBoundedJson(const std::string& url, uint8_t* buffer, size_t& size) {
  size = 0;
  bool overflow = false;
  const bool fetched = HttpDownloader::fetchVerifiedUrl(url, [&](const uint8_t* bytes, const size_t length) {
    if (length > MAX_RELEASE_JSON - size) {
      overflow = true;
      return false;
    }
    std::memcpy(buffer + size, bytes, length);
    size += length;
    return true;
  });
  if (overflow) LOG_ERR("OTA", "Fork release JSON exceeds %zu bytes", MAX_RELEASE_JSON);
  return fetched && !overflow;
}

bool decodeSha256(const char* hex, std::array<uint8_t, 32>& digest) {
  if (!hex || std::strlen(hex) != digest.size() * 2) return false;
  for (size_t i = 0; i < digest.size(); ++i) {
    unsigned value = 0;
    if (std::sscanf(hex + i * 2, "%2x", &value) != 1) return false;
    const auto valid = [](const char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    };
    if (!valid(hex[i * 2]) || !valid(hex[i * 2 + 1])) return false;
    digest[i] = static_cast<uint8_t>(value);
  }
  return true;
}

bool safeAssetName(const std::string_view name) {
  return name.starts_with("crossmux-ar0c-") && name.ends_with("-firmware.bin") &&
         std::all_of(name.begin(), name.end(), [](const char c) {
           return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
         });
}

}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate(const Channel requestedChannel) {
  updateAvailable = false;
  latestVersion.clear();
  otaUrl.clear();
  otaSize = totalSize = releaseNoteCount = 0;
  channel = requestedChannel;
  const char* target = targetId();
  if (!target || (requestedChannel == Channel::Stable && std::strcmp(target, "xteink_x4_pro") != 0)) {
    return UNSUPPORTED_CHANNEL;
  }
  const char* channelName = requestedChannel == Channel::Nightly ? "nightly" : "stable";
  const char* flavor = SETTINGS.contentProfile == CrossPointSettings::ContentProfile::China ? "zh-CN" : "global";
  const std::string indexUrl = std::string(FORK_RELEASE_BASE) + channelName + "/release-index.json";

  // Both S3 targets have PSRAM. A bounded response buffer avoids std::string
  // growth during TLS and is released before firmware download begins.
  auto response = memory::makePsramByteBufferNoThrow(MAX_RELEASE_JSON);
  if (!response) return OOM_ERROR;
  size_t responseSize = 0;
  if (!fetchBoundedJson(indexUrl, response.get(), responseSize)) return HTTP_ERROR;

  std::string manifestUrl;
  std::string indexVersion;
  std::string indexRevision;
  {
    JsonDocument index;
    if (deserializeJson(index, reinterpret_cast<char*>(response.get()), responseSize) ||
        index["schemaVersion"].as<int>() != 1 || std::strcmp(index["channel"] | "", channelName) != 0)
      return JSON_PARSE_ERROR;
    JsonVariantConst entry = index["targets"][target];
    if (std::strcmp(entry["targetId"] | "", target) != 0 ||
        std::strlen(entry["boardTag"] | "") != board_tag::boardNameLen() ||
        std::memcmp(entry["boardTag"] | "", board_tag::boardName(), board_tag::boardNameLen()) != 0) {
      return JSON_PARSE_ERROR;
    }
    JsonVariantConst variant = entry["variants"][flavor];
    manifestUrl = variant["manifestUrl"] | "";
    indexVersion = variant["version"] | "";
    indexRevision = variant["crossmuxSha"] | "";
    if (!isForkBuildUrl(manifestUrl, channelName) || indexVersion.empty() || indexVersion.size() > 63 ||
        indexRevision.size() != 40)
      return JSON_PARSE_ERROR;

    JsonArrayConst notes = index["releaseNotes"][flavor].as<JsonArrayConst>();
    for (JsonVariantConst note : notes) {
      const char* value = note.as<const char*>();
      if (!value || releaseNoteCount >= releaseNotes.size()) break;
      const size_t length = std::strlen(value);
      if (length == 0 || length >= releaseNotes[0].size()) continue;
      std::memcpy(releaseNotes[releaseNoteCount].data(), value, length + 1);
      ++releaseNoteCount;
    }
  }

  if (!fetchBoundedJson(manifestUrl, response.get(), responseSize)) return HTTP_ERROR;
  {
    JsonDocument manifest;
    if (deserializeJson(manifest, reinterpret_cast<char*>(response.get()), responseSize) ||
        manifest["schemaVersion"].as<int>() != 1 || std::strcmp(manifest["channel"] | "", channelName) != 0 ||
        std::strcmp(manifest["targetId"] | "", target) != 0 || std::strcmp(manifest["flavor"] | "", flavor) != 0 ||
        std::strcmp(manifest["version"] | "", indexVersion.c_str()) != 0 ||
        std::strcmp(manifest["crossmuxSha"] | "", indexRevision.c_str()) != 0 ||
        std::strlen(manifest["boardTag"] | "") != board_tag::boardNameLen() ||
        std::memcmp(manifest["boardTag"] | "", board_tag::boardName(), board_tag::boardNameLen()) != 0) {
      return JSON_PARSE_ERROR;
    }
    bool found = false;
    for (JsonVariantConst asset : manifest["assets"].as<JsonArrayConst>()) {
      if (std::strcmp(asset["role"] | "", "firmware") != 0) continue;
      if (found) return JSON_PARSE_ERROR;
      const char* name = asset["name"] | "";
      const size_t size = asset["size"].as<size_t>();
      if (!safeAssetName(name) || size < 1024 || !decodeSha256(asset["sha256"] | "", otaSha256)) {
        return JSON_PARSE_ERROR;
      }
      const size_t slash = manifestUrl.rfind('/');
      otaUrl = manifestUrl.substr(0, slash + 1) + name;
      otaSize = totalSize = size;
      found = true;
    }
    if (!found) return JSON_PARSE_ERROR;
  }
  latestVersion = indexVersion;
  updateAvailable = true;
  LOG_INF("OTA", "Fork %s update: %s %zu bytes", channelName, latestVersion.c_str(), otaSize);
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty()) {
    return false;
  }
  switch (channel) {
    case Channel::Stable:
      break;
    case Channel::Nightly:
      if (latestVersion == CROSSPOINT_VERSION) return false;
      // Release builds embed the same seven-character source revision after '+'.
      // A local timestamped development build has no comparable release SHA.
      if (const size_t plus = latestVersion.rfind('+'); plus != std::string::npos && latestVersion.size() - plus == 8) {
        const std::string_view current = CROSSPOINT_VERSION;
        const size_t currentPlus = current.rfind('+');
        if (currentPlus != std::string_view::npos &&
            current.substr(currentPlus + 1) == std::string_view(latestVersion).substr(plus + 1))
          return false;
      }
      return true;
  }
  if (latestVersion == CROSSPOINT_VERSION) return false;

  int currentMajor = 0, currentMinor = 0, currentPatch = 0;
  int latestMajor = 0, latestMinor = 0, latestPatch = 0;

  const auto currentVersion = CROSSPOINT_VERSION;

  // semantic version check (only match on 3 segments)
  if (sscanf(latestVersion.c_str(), "%d.%d.%d", &latestMajor, &latestMinor, &latestPatch) != 3) return false;
  if (sscanf(currentVersion, "%d.%d.%d", &currentMajor, &currentMinor, &currentPatch) != 3) return true;

  /*
   * Compare major versions.
   * If they differ, return true if latest major version greater than current major version
   * otherwise return false.
   */
  if (latestMajor != currentMajor) return latestMajor > currentMajor;

  /*
   * Compare minor versions.
   * If they differ, return true if latest minor version greater than current minor version
   * otherwise return false.
   */
  if (latestMinor != currentMinor) return latestMinor > currentMinor;

  /*
   * Check patch versions.
   */
  if (latestPatch != currentPatch) return latestPatch > currentPatch;

  // If we reach here, it means all segments are equal.
  // One final check, if we're on an RC build (contains "-rc"), we should consider the latest version as newer even if
  // the segments are equal, since RC builds are pre-release versions.
  if (strstr(currentVersion, "-rc") != nullptr) {
    return true;
  }

  return false;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx) {
  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  // esp_https_ota is hardwired to esp-tls/mbedTLS, whose precompiled build on this
  // package can't negotiate TLS 1.3 (see SecureClient.h). Drive the OTA partition
  // ourselves and stream the firmware through HttpDownloader, which runs over
  // wolfSSL when FREEINK_NET_WOLFSSL is set, reusing its redirect handling for the
  // GitHub -> CDN hop.
  const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
  if (!updatePartition) {
    LOG_ERR("OTA", "No OTA partition available");
    return INTERNAL_UPDATE_ERROR;
  }
  if (otaSize == 0 || otaSize > updatePartition->size) {
    LOG_ERR("OTA", "Fork image does not fit OTA partition: %zu > %u", otaSize,
            static_cast<unsigned>(updatePartition->size));
    return INTERNAL_UPDATE_ERROR;
  }

  esp_ota_handle_t otaHandle = 0;
  esp_err_t esp_err = esp_ota_begin(updatePartition, OTA_SIZE_UNKNOWN, &otaHandle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_begin failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  /* For better timing and connectivity, we disable power saving for WiFi */
  esp_wifi_set_ps(WIFI_PS_NONE);

  processedSize = 0;
  int lastReportedPct = -1;
  bool flashOk = true;
  // The image streams in chunks; only the first bytes carry the header. Buffer
  // the first 14 bytes so we can read chip_id (esp_image_header_t offset 12)
  // and reject a wrong-MCU image before it overwrites the OTA partition.
  uint8_t hdr[14];
  size_t hdrLen = 0;
  bool wrongChip = false;
  board_tag::Scanner boardScanner;
  bool wrongBoard = false;
  mbedtls_sha256_context shaCtx;
  mbedtls_sha256_init(&shaCtx);
  if (mbedtls_sha256_starts(&shaCtx, 0) != 0) {
    mbedtls_sha256_free(&shaCtx);
    esp_ota_abort(otaHandle);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    return INTERNAL_UPDATE_ERROR;
  }
  bool hashOk = true;
  const bool fetchOk = HttpDownloader::fetchVerifiedUrl(otaUrl, [&](const uint8_t* data, size_t len) {
    if (len > otaSize - processedSize) {
      hashOk = false;
      return false;
    }
    if (hdrLen < sizeof(hdr)) {
      const size_t take = std::min(len, sizeof(hdr) - hdrLen);
      std::memcpy(hdr + hdrLen, data, take);
      hdrLen += take;
      if (hdrLen == sizeof(hdr)) {
        uint16_t imageChip;
        std::memcpy(&imageChip, hdr + 12, sizeof(imageChip));
        const uint16_t deviceChip = firmware_flash::runningPartitionChipId();
        if (deviceChip != 0xFFFF && imageChip != deviceChip) {
          LOG_ERR("OTA", "wrong chip: image=0x%04X device=0x%04X", imageChip, deviceChip);
          wrongChip = true;
          return false;  // abort the transfer
        }
      }
    }
    boardScanner.feed(data, len);
    if (boardScanner.mismatch()) {
      LOG_ERR("OTA", "wrong board: image=%s device=%.*s", boardScanner.foundName(),
              static_cast<int>(board_tag::boardNameLen()), board_tag::boardName());
      wrongBoard = true;
      return false;  // abort before selecting the incomplete image as bootable
    }
    if (mbedtls_sha256_update(&shaCtx, data, len) != 0) {
      hashOk = false;
      return false;
    }
    if (esp_ota_write(otaHandle, data, len) != ESP_OK) {
      flashOk = false;
      return false;  // abort the transfer
    }
    processedSize += len;
    // Fire the callback only on whole-percent change. Per-chunk updates wake the
    // render task, whose framebuffer work contends with TLS on the internal arena,
    // and e-ink can't repaint faster than a percent tick anyway.
    if (onProgress && totalSize > 0) {
      const int pct = static_cast<int>(static_cast<uint64_t>(processedSize) * 100 / totalSize);
      if (pct != lastReportedPct) {
        lastReportedPct = pct;
        onProgress(ctx);
      }
    }
    return true;
  });

  /* Return back to default power saving for WiFi in case of failing */
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  uint8_t actualDigest[32] = {};
  if (mbedtls_sha256_finish(&shaCtx, actualDigest) != 0 || processedSize != otaSize ||
      std::memcmp(actualDigest, otaSha256.data(), otaSha256.size()) != 0)
    hashOk = false;
  mbedtls_sha256_free(&shaCtx);

  if (wrongChip || wrongBoard || !boardScanner.matched()) {
    LOG_ERR("OTA", "Firmware install aborted: wrong device");
    esp_ota_abort(otaHandle);
    return WRONG_DEVICE_ERROR;
  }

  if (!fetchOk || !flashOk || !hashOk) {
    LOG_ERR("OTA", "Firmware install failed (%s)", flashOk ? "download" : "flash write");
    esp_ota_abort(otaHandle);
    return flashOk ? HTTP_ERROR : INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_ota_end(otaHandle);  // verifies the written image
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_end failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_ota_set_boot_partition(updatePartition);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_set_boot_partition failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed");
  return OK;
}
