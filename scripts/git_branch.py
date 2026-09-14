"""
PlatformIO pre-build script: inject git branch and short SHA into
CROSSPOINT_VERSION for development environments.

Results in a version string like:  1.1.0-dev-feat-kosync-xpath-05c6cf8
Release environments are unaffected; they set CROSSPOINT_VERSION in the ini.
"""

import configparser
import os
import subprocess
import sys
import re
import shutil
from pathlib import Path


def x4pro_identity(base_version, short_sha):
    if not re.fullmatch(r'\d+\.\d+\.\d+', base_version) or not re.fullmatch(r'[0-9a-f]{7,12}', short_sha):
        raise ValueError('Fork identity requires a numeric base version and a real git revision')
    version = f'{base_version}-ar0c-x4pro+{short_sha}'
    if len(version.encode('ascii')) >= 32:
        raise ValueError('Fork version exceeds the ESP application descriptor limit')
    return version, f'crossmux-ar0c-{base_version}-x4pro-{short_sha}.bin'


def warn(msg):
    print(f'WARNING [git_branch.py]: {msg}', file=sys.stderr)


def run_git_value(project_dir, args, label):
    try:
        value = subprocess.check_output(
            ['git', *args],
            text=True, stderr=subprocess.PIPE, cwd=project_dir
        ).strip()
        # Strip characters that would break a C string literal
        return ''.join(c for c in value if c not in '"\\')
    except FileNotFoundError:
        warn(f'git not found on PATH; {label} suffix will be "unknown"')
        return 'unknown'
    except subprocess.CalledProcessError as e:
        warn(
            f'git command failed (exit {e.returncode}): '
            f'{e.stderr.strip()}; {label} suffix will be "unknown"'
        )
        return 'unknown'
    except OSError as e:
        warn(
            f'OS error reading git {label}: {e}; '
            f'{label} suffix will be "unknown"'
        )
        return 'unknown'
    except Exception as e:  # pylint: disable=broad-exception-caught
        warn(
            f'Unexpected error reading git {label}: {e}; '
            f'{label} suffix will be "unknown"'
        )
        return 'unknown'


def get_git_branch(project_dir):
    branch = run_git_value(
        project_dir, ['rev-parse', '--abbrev-ref', 'HEAD'], 'branch'
    )
    # Detached HEAD has no branch name.
    if branch == 'HEAD':
        return 'detached'
    return branch


def get_git_short_sha(project_dir):
    return run_git_value(
        project_dir, ['rev-parse', '--short', 'HEAD'], 'short SHA'
    )


def get_base_version(project_dir):
    ini_path = os.path.join(project_dir, 'platformio.ini')
    if not os.path.isfile(ini_path):
        warn(f'platformio.ini not found at {ini_path}; base version will be "0.0.0"')
        return '0.0.0'
    config = configparser.ConfigParser()
    config.read(ini_path, encoding='utf-8')
    if not config.has_option('crosspoint', 'version'):
        warn('No [crosspoint] version in platformio.ini; base version will be "0.0.0"')
        return '0.0.0'
    return config.get('crosspoint', 'version')


def inject_version(env):
    pioenv = env['PIOENV']
    # Only applies to development environments; release envs set the
    # version via build_flags in platformio.ini and are unaffected.
    if pioenv not in ('default', 'sticky', 'eego_a4', 'murphy_m4', 'waveshare_epaper_397', 'metalio_eink4', 'x4pro'):
        return

    project_dir = env['PROJECT_DIR']
    base_version = get_base_version(project_dir)
    short_sha = get_git_short_sha(project_dir)
    if pioenv == 'x4pro':
        version_string, filename = x4pro_identity(base_version, short_sha)

        def export_firmware(target, source, env):
            image = Path(target[0].get_abspath())
            destination = image.parent / filename
            shutil.copyfile(image, destination)
            print(f'ar0c firmware: {destination}')

        env.AddPostAction('$BUILD_DIR/${PROGNAME}.bin', export_firmware)
    elif pioenv in ('default', 'sticky'):
        version_string = f'{base_version}-dev-{get_git_branch(project_dir)}-{short_sha}'
    else:
        device = pioenv.replace('_', '-')
        version_string = f'{base_version}-{device}-rc+{short_sha}'

    env.Append(CPPDEFINES=[('CROSSPOINT_VERSION', f'\\"{version_string}\\"')])
    print(f'CrossPoint build version: {version_string}')


# PlatformIO/SCons entry point — Import and env are SCons builtins injected at runtime.
# When run directly with Python (e.g. for validation), a lightweight fake env is used
# so the git/version logic can be exercised without a full build.
try:
    Import('env')           # noqa: F821  # type: ignore[name-defined]
    inject_version(env)     # noqa: F821  # type: ignore[name-defined]
except NameError:
    class _Env(dict):
        def Append(self, **_): pass

    _project_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    inject_version(_Env({'PIOENV': 'default', 'PROJECT_DIR': _project_dir}))
