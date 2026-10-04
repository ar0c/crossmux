#pragma once

#include <cstddef>
#include <cstdint>

// The last OTA attempt remains on the SD card after a reset or rollback.
// Stages are fixed literals; never write the firmware URL or account data here.
namespace OtaDiagnostic {
inline constexpr char PATH[] = "/ota-diagnostic.jsonl";

void begin(size_t expectedBytes, uint32_t targetSlot);
void checkpoint(const char* stage, size_t writtenBytes, size_t expectedBytes, uint32_t targetSlot = 0,
                int errorCode = 0);
// Called once after the SD card mounts. Appends only when the prior attempt
// reached the restart boundary, so routine boots do not grow the file.
void recordBoot();
void recordSetupDone();
}  // namespace OtaDiagnostic
