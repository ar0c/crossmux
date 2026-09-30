"""Compile project-owned portable HAL additions beside the pinned simulator HAL."""
Import("env")
from pathlib import Path

# The simulator replaces the hardware HAL library. Its BoardConfig stub is
# sufficient for the cache-only diagnostic reader, whose hardware probe access
# is excluded by SIMULATOR. Compile the real implementation exactly once.
# BuildSources runs before the library finder adds dependency include paths.
# Resolve the pinned native BoardConfig header for this isolated source too.
env.Append(CPPPATH=[str(Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env.subst("$PIOENV") / "simulator" / "src")])
env.BuildSources(
    "$BUILD_DIR/project-hal",
    "$PROJECT_DIR/lib/hal",
    src_filter=["+<HalDisplayDiagnostics.cpp>"],
)
