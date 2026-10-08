"""Apply the reviewed link-only -Oz policy to Waveshare's existing LTO link.

Compile-time flags, features, resources and partition sizes stay unchanged.
The pinned GCC 14 toolchain supports -Oz. The prior measured image saving
from this link-only flag was zero; dictionary pruning provides the new margin.
"""

Import("env")

env.AppendUnique(LINKFLAGS=["-Oz"])
print("CrossMux OTA size policy: existing LTO link uses -Oz")
