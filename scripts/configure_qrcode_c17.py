"""Compile the pinned QRCode C library with its pre-C23 bool declarations."""

Import("env")  # noqa: F821


def compile_qrcode(build_env, node):
    return build_env.Object(node, CFLAGS=build_env.get("CFLAGS", []) + ["-std=gnu17"])[0]


env.AddBuildMiddleware(compile_qrcode, "*/QRCode/src/qrcode.c")
