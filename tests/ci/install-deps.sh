#!/bin/bash
#
# Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
#
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Install everything building and testing Manifold needs, on the distribution
# this runs on. Runs as root, when the CI container image is built.
#
set -euo pipefail

# the BLAKE3 C library, built from source where the distribution has no package of it
BLAKE3_VERSION=1.8.7
BLAKE3_SHA256=c6782a28842b1c0478524ac06a4f2ede784038ee298d6e2162c0b089c4306a3c

install_blake3()
{
    local workdir

    workdir=$(mktemp -d)
    curl -fsSL -o "$workdir/blake3.tar.gz" \
        "https://github.com/BLAKE3-team/BLAKE3/archive/refs/tags/$BLAKE3_VERSION.tar.gz"
    echo "$BLAKE3_SHA256  $workdir/blake3.tar.gz" | sha256sum --check --quiet
    tar -xzf "$workdir/blake3.tar.gz" -C "$workdir" --strip-components=1

    cmake -S "$workdir/c" -B "$workdir/build" \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=ON
    cmake --build "$workdir/build"
    cmake --install "$workdir/build"
    ldconfig

    rm -rf "$workdir"
}

# shellcheck source=/dev/null
. /etc/os-release

case " $ID ${ID_LIKE:-} " in
    *" debian "* | *" ubuntu "*)
        export DEBIAN_FRONTEND=noninteractive
        install=(apt-get install -yq --no-install-recommends)
        packages=(
            build-essential
            meson
            ninja-build
            pkgconf
            git
            ca-certificates
            python3
            systemd
            systemd-dev
            libsystemd-dev
            libglib2.0-dev
            libcurl4-openssl-dev
            libappstream-dev
            libzip-dev
            libcryptsetup-dev
            libelf-dev
            gobject-introspection
            libgirepository1.0-dev
            b3sum
        )
        blake3_build_packages=(cmake curl)

        apt-get update -q
        # not every release has it, it is built from source then
        if apt-cache show libblake3-dev > /dev/null 2>&1; then
            packages+=(libblake3-dev)
        fi
        ;;

    *" fedora "* | *" rhel "*)
        install=(dnf install -y)
        # libraries are named by the pkg-config files they provide, which is the same everywhere
        packages=(
            gcc
            meson
            ninja-build
            pkgconfig
            git
            python3
            systemd
            util-linux
            shadow-utils
            'pkgconfig(glib-2.0)'
            'pkgconfig(gobject-2.0)'
            'pkgconfig(gio-2.0)'
            'pkgconfig(libcurl)'
            'pkgconfig(appstream)'
            'pkgconfig(libsystemd)'
            'pkgconfig(systemd)'
            'pkgconfig(udev)'
            'pkgconfig(libzip)'
            'pkgconfig(libcryptsetup)'
            'pkgconfig(libelf)'
            'pkgconfig(gobject-introspection-1.0)'
            'pkgconfig(libblake3)'
            b3sum
        )
        blake3_build_packages=(cmake curl gcc-c++)

        # the enterprise distributions have some of the packages in CRB and EPEL only,
        # and a newer GCC than their default one as toolset (see build-and-test.sh)
        if [ "$ID" != fedora ]; then
            "${install[@]}" dnf-plugins-core epel-release
            dnf config-manager --set-enabled crb
            # its libstdc++ for linking, which their libblake3 needs
            packages+=(gcc-toolset-15-gcc gcc-toolset-15-libstdc++-devel)
        fi
        ;;

    *" arch "*)
        install=(pacman -S --noconfirm --needed)
        packages=(
            base-devel
            meson
            git
            python
            glib2
            glib2-devel
            curl
            appstream
            systemd
            libzip
            cryptsetup
            libelf
            gobject-introspection
            libblake3
            b3sum
        )
        blake3_build_packages=(cmake)

        pacman -Syu --noconfirm
        ;;

    *)
        echo "Unknown distribution: $ID" >&2
        exit 1
        ;;
esac

"${install[@]}" "${packages[@]}"

if ! pkg-config --exists libblake3; then
    "${install[@]}" "${blake3_build_packages[@]}"
    install_blake3
fi
