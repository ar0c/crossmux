"""Fail-closed USB debug build boundary; also used by the OTA packager."""
from pathlib import Path

DEBUG_ENV = 'waveshare_epaper_397_debug'
DEBUG_DEFINE = 'CROSSMUX_USB_DEBUG'
DEBUG_MARKER = b'CROSSMUX-USB-DEBUG-V1;'


def validate_config(environment, definitions, source_filter):
    values = []
    for define in definitions:
        if isinstance(define, (list, tuple)):
            name, *value = define
            if name == DEBUG_DEFINE:
                values.append(str(value[0]) if value else '1')
        elif str(define).split('=')[0] == DEBUG_DEFINE:
            values.append(str(define).partition('=')[2] or '1')
    debug = environment == DEBUG_ENV
    if (debug and values != ['1']) or (not debug and values):
        raise ValueError('USB debug flag is permitted only in the explicit debug environment')
    filters = ' '.join(source_filter) if isinstance(source_filter, (list, tuple)) else source_filter
    if '-<debug/>' not in filters or (('+<debug/>' in filters) != debug):
        raise ValueError('USB debug sources must be excluded by default and explicitly included only for debug')


def contains_debug(path):
    overlap = b''
    with Path(path).open('rb') as stream:
        while chunk := stream.read(65536):
            data = overlap + chunk
            if DEBUG_MARKER in data:
                return True
            overlap = data[-(len(DEBUG_MARKER) - 1):]
    return False


def verify_image(path, *, debug=False):
    if contains_debug(path) != debug:
        raise ValueError('USB debug image identity disagrees with the selected build/release target')


def configure(env):
    definitions = env.ParseFlags(env['BUILD_FLAGS']).get('CPPDEFINES', [])
    validate_config(env['PIOENV'], definitions, env.GetProjectOption('build_src_filter'))

    def check_image(target, source, env):
        verify_image(target[0].get_abspath(), debug=env['PIOENV'] == DEBUG_ENV)
        print('USB debug boundary verified: ' + ('debug-only' if env['PIOENV'] == DEBUG_ENV else 'excluded'))

    env.AddPostAction('$BUILD_DIR/${PROGNAME}.bin', check_image)


try:
    Import('env')
except NameError:
    pass
else:
    configure(env)
