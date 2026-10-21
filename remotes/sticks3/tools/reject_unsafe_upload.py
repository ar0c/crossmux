"""Reject generic flash targets before PlatformIO can write rescue/data regions."""
from SCons.Script import COMMAND_LINE_TARGETS

blocked = {'upload', 'uploadfs', 'uploadfsota', 'erase'} & set(COMMAND_LINE_TARGETS)
if blocked:
    raise RuntimeError(
        'Generic StickS3 upload/erase is disabled. Use tools/flash_app.py with '
        'a verified full backup and the previous application image; see README.md.'
    )
