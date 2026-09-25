"""Canonical firmware target mapping and artifact naming."""

FIRMWARE_NAME = 'crossmux-ar0c'


TARGETS = {
    'xteink_x4_pro': {
        'deviceSlug': 'x4pro',
        'models': ['xteink_x4_pro'],
        'boardTag': 'x4pro',
        'chip': 'ESP32-S3',
        'chipId': 0x0009,
        'environments': {'stable': 'x4pro-gh_release', 'nightly': 'x4pro_nightly'},
        'supportedChannels': ['stable', 'nightly'],
        'fullInstall': True,
    },
    'waveshare_epaper_397': {
        'deviceSlug': 'waveshare-epaper-397',
        'models': ['waveshare_epaper_397'],
        'boardTag': 'waveshare_epaper_397',
        'chip': 'ESP32-S3',
        'chipId': 0x0009,
        'environments': {'nightly': 'waveshare_epaper_397_nightly'},
        'supportedChannels': ['nightly'],
        'fullInstall': True,
    },
}

FLAVOR_TOKENS = {'global': 'global', 'zh-CN': 'cn'}
CHANNELS = ('stable', 'nightly')


def targets_for(channel):
    if channel not in CHANNELS:
        raise KeyError(channel)
    return {target_id: target for target_id, target in TARGETS.items() if channel in target['supportedChannels']}


def environment_for(target_id, channel, flavor):
    if flavor not in FLAVOR_TOKENS:
        raise KeyError(flavor)
    return TARGETS[target_id]['environments'][channel]


def version_for(base_version, target_id, channel, flavor, short_sha):
    target = TARGETS[target_id]
    if channel == 'stable':
        return base_version
    if channel != 'nightly':
        raise KeyError(channel)
    parts = [base_version]
    parts.append(target['deviceSlug'])
    if flavor not in FLAVOR_TOKENS:
        raise KeyError(flavor)
    return f"{'-'.join(parts)}-rc+{short_sha[:7]}"


def asset_name(target_id, source_name):
    target = TARGETS[target_id]
    return f"{FIRMWARE_NAME}-{target['deviceSlug']}-{source_name}"


def manifest_name(target_id, flavor):
    target = TARGETS[target_id]
    return f"{target['deviceSlug']}-{FLAVOR_TOKENS[flavor]}-manifest.json"


def matrix(channel, only_target=None):
    if only_target is not None and only_target not in targets_for(channel):
        raise ValueError(f'{only_target} is not a {channel} target')
    return {
        'include': [
            {
                'targetId': target_id,
                'deviceSlug': target['deviceSlug'],
                'environment': environment_for(target_id, channel, 'global'),
            }
            for target_id, target in targets_for(channel).items()
            if only_target is None or target_id == only_target
        ]
    }
