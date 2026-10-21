#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#define private public
#include "SdmmcBlockDevice.h"
#undef private

namespace {
constexpr size_t kSectors = 1024;
std::array<uint8_t, kSectors * 512> media;
struct sdmmc_card_t {
  struct { size_t capacity; } csd;
};
constexpr int ESP_OK = 0;
size_t reads = 0;
bool failMultiRead = false;
bool failRead = false;
bool failWrite = false;

int sdmmc_read_sectors(sdmmc_card_t*, void* dst, size_t sector, size_t count) {
  ++reads;
  if (failRead || (failMultiRead && count > 1) || sector >= kSectors || count > kSectors - sector) return -1;
  memcpy(dst, media.data() + sector * 512, count * 512);
  return ESP_OK;
}
int sdmmc_write_sectors(sdmmc_card_t*, const void* src, size_t sector, size_t count) {
  if (failWrite || sector >= kSectors || count > kSectors - sector) return -1;
  memcpy(media.data() + sector * 512, src, count * 512);
  return ESP_OK;
}
void heap_caps_free(void* p) { free(p); }
void sdmmc_host_deinit() {}
void mount(freeink::SdmmcBlockDevice& device) {
  device._card = calloc(1, sizeof(sdmmc_card_t));
  static_cast<sdmmc_card_t*>(device._card)->csd.capacity = kSectors;
  device._dmaBuffer = static_cast<uint8_t*>(malloc(4096));
  assert(device._card && device._dmaBuffer);
}
void check(freeink::SdmmcBlockDevice& device, Sector_t sector) {
  std::array<uint8_t, 512> actual{};
  assert(device.readSector(sector, actual.data()));
  assert(memcmp(actual.data(), media.data() + sector * 512, 512) == 0);
}
}  // namespace

namespace freeink {
#include "methods.inc"
}  // namespace freeink

int main() {
  for (size_t i = 0; i < media.size(); ++i) media[i] = static_cast<uint8_t>(i ^ (i >> 9) ^ (i >> 17));
  freeink::SdmmcBlockDevice device;
  mount(device);
  // A complete FAT-like sequential scan must preserve every byte while avoiding
  // one native SD command per sector. Random reads must not cause bulk overfetch.
  reads = 0;
  for (Sector_t sector = 10; sector < 810; ++sector) check(device, sector);
  printf("800 sequential sectors: %zu native reads\n", reads);
  fflush(stdout);
  assert(reads <= 102);
  const size_t beforeRandom = reads;
  for (Sector_t sector : {1u, 900u, 20u, 950u}) check(device, sector);
  assert(reads == beforeRandom + 4);

  // Writes (including failures) reuse the bounce buffer and must invalidate it.
  std::array<uint8_t, 512> replacement{};
  replacement.fill(0xA7);
  check(device, 100);
  check(device, 101);
  assert(device.writeSector(102, replacement.data()));
  check(device, 102);
  failWrite = true;
  assert(!device.writeSector(103, replacement.data()));
  failWrite = false;
  check(device, 103);

  // Explicit multi-sector I/O also reuses the buffer; a prior cached hit must
  // never return bytes from that unrelated transfer.
  check(device, 200);
  check(device, 201);
  std::array<uint8_t, 512 * 19> bulk{};
  assert(device.readSectors(400, bulk.data(), 19));
  assert(memcmp(bulk.data(), media.data() + 400 * 512, bulk.size()) == 0);
  check(device, 202);
  bulk.fill(0x5C);
  assert(device.writeSectors(500, bulk.data(), 19));
  for (Sector_t sector = 500; sector < 519; ++sector) check(device, sector);

  // Speculation may fail even though the requested sector is readable. Fall
  // back to that sector; actual requested-sector errors must still propagate.
  check(device, 300);
  failMultiRead = true;
  check(device, 301);
  failMultiRead = false;
  failRead = true;
  assert(!device.readSector(800, replacement.data()));
  failRead = false;
  check(device, 800);
  check(device, kSectors - 2);
  check(device, kSectors - 1);
  assert(!device.readSector(kSectors, replacement.data()));
  assert(!device.readSectors(std::numeric_limits<Sector_t>::max(), replacement.data(), 2));
  assert(!device.readSectors(0, replacement.data(), std::numeric_limits<size_t>::max()));
  assert(!device.readSectors(0, nullptr, 1));
  assert(!device.readSectors(0, replacement.data(), 0));

  check(device, 600);
  check(device, 601);
  device.end();
  media[602 * 512] ^= 0xFF;
  mount(device);
  check(device, 602);
  device.end();
  assert(!device.readSector(0, replacement.data()));
  puts("SDMMC read-ahead data, boundaries, coherence and failure regressions passed");
}
