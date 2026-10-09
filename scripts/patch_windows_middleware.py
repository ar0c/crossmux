"""Repair the pinned pioarduino Windows middleware dispatch (build tooling only)."""

import hashlib
import inspect
import os
from pathlib import Path
import textwrap


def make_middleware(middlewares, fallback):
    import fnmatch

    def dispatch(build_env, node):
        result = node
        matched = False
        path = node.srcnode().get_path().replace("\\", "/")
        for callback, pattern in middlewares:
            if pattern and not fnmatch.fnmatch(path, pattern):
                continue
            matched = True
            result = callback(build_env, result) if callback.__code__.co_argcount == 2 else callback(result)
            if not result:
                return None
        # Custom middleware owns compilation and may replace the source. Do not
        # compile the original again or replace Object with a fake return value.
        return result if matched else fallback(build_env, node)

    return dispatch


START = "            def integrated_middleware(env, node):"
END = "            # Replace all middlewares"
MARKER = "            # CrossMux: preserve middleware patterns and source replacements\n"
PINNED_HASH = "4bfa0220fd5a61e72b0a79fbed8ddad54b117fbda8edf42a2ef8256456670338"
# Platform 55.03.311 no longer installs an integrated middleware dispatcher.
# It uses SCons response files for Windows command lengths, leaving Core's
# pattern matching and replacement-object handling intact. Recognize only the
# reviewed, complete builder source; an unknown platform still fails closed.
RESPONSE_FILE_BUILDER_HASH = "b6763e4165b4c79c7cb481dd4e3d705397460e72062fc5b6be762ca6e5fc3009"


def patch_source(source):
    if hashlib.sha256(source.encode()).hexdigest() == RESPONSE_FILE_BUILDER_HASH:
        return source
    replacement = MARKER + textwrap.indent(inspect.getsource(make_middleware), "            ")
    replacement += (
        "\n            integrated_middleware = make_middleware(\n"
        "                existing_middlewares, smart_include_length_shorten)\n\n"
    )
    if replacement in source:
        return source
    start = source.find(START)
    end = source.find(END, start)
    if start < 0 or end < 0 or hashlib.sha256(source[start:end].encode()).hexdigest() != PINNED_HASH:
        raise RuntimeError("Unsupported pioarduino Windows middleware; review the pinned platform")
    return source[:start] + replacement + source[end:]


if "Import" in globals() and os.name == "nt":
    Import("env")  # noqa: F821
    builder = Path(env.PioPlatform().get_dir()) / "builder/frameworks/arduino.py"
    original = builder.read_text(encoding="utf-8")
    patched = patch_source(original)
    if patched != original:
        builder.write_text(patched, encoding="utf-8", newline="\n")
