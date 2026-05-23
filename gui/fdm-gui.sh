#!/usr/bin/env bash
# Wrapper around fdm-gui that strips env vars known to break dynamic linking
# when the binary is launched from a snap-confined shell (e.g. the integrated
# terminal of snap-installed VS Code). Snap leaks library paths that point at
# its bundled core20 libc / libstdc++, which are too old for binaries built
# against the system glibc.
#
# If you're already in a clean shell, this is a no-op.
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec env \
    -u LD_LIBRARY_PATH \
    -u LD_PRELOAD \
    -u QT_PLUGIN_PATH \
    -u QT_QPA_PLATFORM_PLUGIN_PATH \
    "${here}/fdm-gui" "$@"
