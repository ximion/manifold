#!/bin/sh
#
# Copyright (C) 2026 Matthias Klumpp <matthias@tenstral.net>
#
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Write the list of languages we have translations for to po/LINGUAS.

find "${MESON_SOURCE_ROOT}/po" \
    -type f \
    -iname "*.po" \
    -printf '%f\n' \
    | grep -oP '.*(?=[.])' | sort \
    > "${MESON_SOURCE_ROOT}/po/LINGUAS"
