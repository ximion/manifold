#!/bin/bash
#
# Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
#
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Build Manifold, run its tests and install it to a staging directory, in a
# CI container (see Containerfile).
#
# Extra arguments for `meson setup` and `meson test` are taken from the
# CI_MESON_ARGS and CI_TEST_ARGS environment variables.
#
set -euo pipefail

srcdir=$(cd "$(dirname "$0")/../.." && pwd)

if [ "$(id -u)" -eq 0 ]; then
    uid=$(stat -c %u "$srcdir")
    gid=$(stat -c %g "$srcdir")

    # with rootless podman, root of the container owns the tree, and the tests run as root
    if [ "$uid" -ne 0 ]; then
        getent group "$gid" > /dev/null || groupadd -g "$gid" ci
        getent passwd "$uid" > /dev/null || useradd -m -u "$uid" -g "$gid" ci
        exec runuser -u "$(getent passwd "$uid" | cut -d: -f1)" -- "$0" "$@"
    fi
fi

# where install-deps.sh installed a newer GCC than the distribution's default one, build with it
if [ -d /opt/rh/gcc-toolset-15/root/usr/bin ]; then
    export PATH="/opt/rh/gcc-toolset-15/root/usr/bin:$PATH"
fi

# a new one, the source tree may have a build directory of its own
builddir="build.ci"
read -ra meson_args <<< "${CI_MESON_ARGS:-}"
read -ra test_args <<< "${CI_TEST_ARGS:-}"

# ensure essential programs are there
for program in b3sum varlinkctl; do
    if ! command -v "$program" > /dev/null; then
        echo "$program is missing, which the tests need" >&2
        exit 1
    fi
done

echo "::group::Configure"
meson setup "$builddir" "$srcdir" \
    --buildtype=debugoptimized \
    -Dmaintainer=true \
    -Ddocs=false \
    "${meson_args[@]}"
echo "::endgroup::"

echo "::group::Build"
meson compile -C "$builddir"
echo "::endgroup::"

echo "::group::Test"
meson test -C "$builddir" --print-errorlogs "${test_args[@]}"
echo "::endgroup::"

# a skipped tests mean lost coverage, e.g. when the host denies user namespaces
python3 - "$builddir/meson-logs/testlog.json" << 'EOF'
import json
import sys

with open(sys.argv[1]) as log:
    skipped = [test['name'] for test in map(json.loads, log) if test['result'] == 'SKIP']
if skipped:
    sys.exit('Tests were skipped: ' + ', '.join(skipped))
EOF

echo "::group::Install"
meson install -C "$builddir" --destdir "$builddir/destdir"
echo "::endgroup::"
