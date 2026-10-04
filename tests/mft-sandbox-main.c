/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "mft-sandbox.h"

#include <errno.h>
#include <stdio.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s PROGRAM [ARGUMENT]...\n", argv[0]);
        return 1;
    }

    mft_sandbox_enter();
    execvp(argv[1], argv + 1);
    fprintf(stderr, "Unable to run %s: %s\n", argv[1], g_strerror(errno));
    return 1;
}
