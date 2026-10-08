"""Prioritize image size during the two supported boards' existing LTO link.

Compile-time flags, features, resources and partition sizes stay unchanged.
The pinned GCC 14 toolchain supports -Oz; setting it after the platform's
link setup overrides its default optimization level for LTO code generation.
"""

Import("env")

env.AppendUnique(LINKFLAGS=["-Oz"])
print("CrossMux OTA size policy: existing LTO link uses -Oz")
