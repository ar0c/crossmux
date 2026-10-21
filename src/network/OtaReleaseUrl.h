#pragma once

#include <algorithm>
#include <string_view>

namespace ota_release {
inline constexpr std::string_view BASE = "https://release.ar0c.com/releases/download/";

// Exact HTTPS authorities and literal paths only; no normalization or redirects.
inline std::string_view releasePath(const std::string_view url) {
  if (url.starts_with(BASE)) return url.substr(BASE.size());
  return {};
}

inline bool isBuildUrl(const std::string_view url, const std::string_view channel) {
  auto path = releasePath(url);
  if (!path.starts_with(channel)) return false;
  path.remove_prefix(channel.size());
  if (!path.starts_with("-build-")) return false;
  path.remove_prefix(7);
  if (path.size() < 46) return false;
  for (size_t i = 0; i < 40; ++i) {
    const char c = path[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  path.remove_prefix(40);
  for (int field = 0; field < 2; ++field) {
    if (!path.starts_with("-")) return false;
    path.remove_prefix(1);
    size_t digits = 0;
    while (digits < path.size() && path[digits] >= '0' && path[digits] <= '9') ++digits;
    if (digits == 0) return false;
    path.remove_prefix(digits);
  }
  if (!path.starts_with("/")) return false;
  path.remove_prefix(1);
  if (path.empty() || path.size() > 128 || path.find("..") != std::string_view::npos) return false;
  return std::all_of(path.begin(), path.end(), [](const char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
  });
}

inline bool isReleaseUrl(const std::string_view url) {
  const auto path = releasePath(url);
  return path == "nightly/release-index.json" || path == "stable/release-index.json" || isBuildUrl(url, "nightly") ||
         isBuildUrl(url, "stable");
}
}  // namespace ota_release
