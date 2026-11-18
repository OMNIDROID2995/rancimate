import subprocess
import datetime
import os
import tempfile

Import("env")

# RANCIMATE: the version is the upstream release it is based on plus this suffix
# (e.g. v1.10.0-rancimate). It must stay valid semver: the OTA updater parses it
# and compares it with the latest release tag of the RANCIMATE repository.
RANCIMATE_SUFFIX = "-rancimate"


def get_firmware_specifier_build_flag():
    # Nearest upstream v* release tag; our own -rancimate tags, nightly, db and one-off
    # release tags (scripts/release.sh) must not leak into the version.
    ret = subprocess.run(
        ["git", "describe", "--tags", "--abbrev=0", "--match", "v[0-9]*", "--exclude", "*" + RANCIMATE_SUFFIX + "*"],
        stdout=subprocess.PIPE,
        text=True,
    )
    build_version = ret.stdout.strip() + RANCIMATE_SUFFIX
    build_flag = "#define BUILD_GIT_VERSION \"" + build_version + "\""
    print ("Build version: " + build_version)
    return build_flag

def get_time_specifier_build_flag():
    build_timestamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    build_flag = "#define BUILD_TIMESTAMP \"" + build_timestamp + "\""
    print ("Build date: " + build_timestamp)
    return build_flag

content = (
    '#pragma once\n' +
    '#ifndef GIT_VERSION_H\n' +
    '#define GIT_VERSION_H\n' +
    get_firmware_specifier_build_flag() + '\n' +
    get_time_specifier_build_flag() + '\n'
    '#endif\n'
)

# Write to a unique temp file and swap it in atomically, so a build running in
# parallel in the same workspace never reads a truncated version.h.
fd, tmp_path = tempfile.mkstemp(dir='src', prefix='version.h.', suffix='.tmp')
try:
    with os.fdopen(fd, 'w') as f:
        f.write(content)
    os.replace(tmp_path, 'src/version.h')
except BaseException:
    os.unlink(tmp_path)
    raise
