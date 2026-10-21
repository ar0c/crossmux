"""Compile project-owned portable HAL additions beside the pinned simulator HAL."""
Import("env")
from pathlib import Path

# The simulator replaces the hardware HAL library. Its BoardConfig stub is
# sufficient for portable diagnostic and environment fallbacks. Hardware probe access and
# the TLS allocator are excluded by SIMULATOR. Compile each portable fallback
# exactly once, including local TLS-memory additions when present.
# BuildSources runs before the library finder adds dependency include paths.
# Resolve the pinned native BoardConfig header for this isolated source too.
env.Append(CPPPATH=[str(Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env.subst("$PIOENV") / "simulator" / "src")])
env.BuildSources(
    "$BUILD_DIR/project-hal",
    "$PROJECT_DIR/lib/hal",
    src_filter=["+<HalDisplayDiagnostics.cpp>", "+<HalTlsMemory.cpp>", "+<HalEnvironmentSensor.cpp>"],
)
