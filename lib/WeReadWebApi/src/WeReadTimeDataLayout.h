#pragma once

namespace WeReadTime {
inline constexpr char kTimeDataDirectory[] = "/WeReadSync";
inline constexpr char kTimeHistoryDirectory[] = "/WeReadSync/history";
inline constexpr char kLegacyTimeManifest[] = "/weread-device-handover.json";
inline constexpr char kTimeManifest[] = "/WeReadSync/weread-device-handover.json";
enum class TimeFileKind { Missing, File, Invalid };

// Only relocate immutable handover metadata, never live WRTL/WRP2 journals.
// Restart-safe per file: a duplicate is a conflict, not permission to overwrite.
template <class Store>
bool relocateTimeFile(Store& store, const char* oldPath, const char* newPath) {
  const auto oldKind = store.kind(oldPath);
  const auto newKind = store.kind(newPath);
  if (oldKind == TimeFileKind::Invalid || newKind == TimeFileKind::Invalid) return false;
  if (oldKind == TimeFileKind::Missing) return true;
  if (newKind != TimeFileKind::Missing) return false;
  if (!store.ensureDirectory() || !store.moveNoReplace(oldPath, newPath)) return false;
  return store.kind(oldPath) == TimeFileKind::Missing && store.kind(newPath) == TimeFileKind::File;
}
}  // namespace WeReadTime
