/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfc-progress-bar.h"

#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

constexpr int BAR_LENGTH_MAX = 20;
constexpr int TERMINAL_WIDTH_DEFAULT = 80;

/* redrawing more often than this is wasted on the eye */
constexpr gint64 REDRAW_INTERVAL_USEC = 100 * 1000;

static const char *const PARTIAL_BLOCKS[] = {" ", "▏", "▎", "▍", "▌", "▋", "▊", "▉"};
static const char FULL_BLOCK[] = "█";

struct MfcProgressBar {
    gboolean is_tty;
    gboolean utf8;
    char *label;
    gint64 start_time;
    gint64 last_draw_time;
    gboolean drawn;
};

MfcProgressBar *
mfc_progress_bar_new(gboolean is_tty)
{
    MfcProgressBar *bar = g_new0(MfcProgressBar, 1);

    bar->is_tty = is_tty;
    bar->utf8 = g_get_charset(NULL);
    return bar;
}

void
mfc_progress_bar_free(MfcProgressBar *bar)
{
    if (bar == NULL)
        return;

    mfc_progress_bar_finish(bar);
    g_free(bar->label);
    g_free(bar);
}

static int
mfc_terminal_width(void)
{
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0 || ws.ws_col == 0)
        return TERMINAL_WIDTH_DEFAULT;
    return ws.ws_col;
}

/**
 * mfc_progress_bar_get_label:
 *
 * Returns: (nullable): the label of the bar that was begun, or %NULL
 */
const char *
mfc_progress_bar_get_label(MfcProgressBar *bar)
{
    return bar->label;
}

/**
 * mfc_progress_bar_start:
 * @label: what the bar is about, shown in front of it
 *
 * Begin a bar. A bar that was begun before is finished first.
 */
void
mfc_progress_bar_start(MfcProgressBar *bar, const char *label)
{
    mfc_progress_bar_finish(bar);

    g_free(bar->label);
    bar->label = g_strdup(label);
    bar->start_time = g_get_monotonic_time();
    bar->last_draw_time = 0;
}

/**
 * mfc_progress_bar_update:
 * @percent_done: how far the bar is, from 0 to 100
 * @bytes_done: bytes done so far, to show the rate, or 0
 *
 * Draw the bar.
 */
void
mfc_progress_bar_update(MfcProgressBar *bar, double percent_done, guint64 bytes_done)
{
    g_autoptr(GString) line = NULL;
    g_autofree char *speed = NULL;
    gint64 now = g_get_monotonic_time();
    gint64 elapsed_usec = now - bar->start_time;
    guint percent;
    int bar_length;
    int n_full;
    int partial;
    int i;

    if (!bar->is_tty || bar->label == NULL)
        return;
    if (percent_done < 100 && now - bar->last_draw_time < REDRAW_INTERVAL_USEC)
        return;
    bar->last_draw_time = now;

    percent = (guint) CLAMP(percent_done, 0, 100);
    if (bytes_done > 0 && elapsed_usec >= G_USEC_PER_SEC) {
        g_autofree char *rate = g_format_size(
            (guint64) (bytes_done * G_USEC_PER_SEC / (guint64) elapsed_usec));

        speed = g_strdup_printf("%s/s", rate);
    }

    bar_length = MIN(BAR_LENGTH_MAX,
                     mfc_terminal_width() - ((int) strlen(bar->label) + 8 + (speed ? 14 : 0)));
    if (bar_length < 1)
        bar_length = 1;
    n_full = (bar_length * (int) percent) / 100;
    partial = (((bar_length * (int) percent) % 100) * (int) G_N_ELEMENTS(PARTIAL_BLOCKS)) / 100;

    line = g_string_new(bar->label);
    g_string_append(line, " \x1b[2m");
    for (i = 0; i < n_full; i++)
        g_string_append(line, bar->utf8 ? FULL_BLOCK : "#");
    if (i < bar_length) {
        g_string_append(line, bar->utf8 ? PARTIAL_BLOCKS[partial] : (partial >= 4 ? "-" : " "));
        i++;
    }
    g_string_append(line, "\x1b[22m");
    for (; i < bar_length; i++)
        g_string_append_c(line, ' ');
    g_string_append_printf(line, " %3u%%", percent);
    if (speed != NULL)
        g_string_append_printf(line, "  %s", speed);

    /* overwrite the line, and clear what a longer line left behind */
    g_print("\r%s\x1b[K", line->str);
    fflush(stdout);
    bar->drawn = TRUE;
}

/**
 * mfc_progress_bar_finish:
 *
 * End the bar, leaving its last state on the terminal.
 */
void
mfc_progress_bar_finish(MfcProgressBar *bar)
{
    if (bar->drawn)
        g_print("\n");
    bar->drawn = FALSE;
    g_clear_pointer(&bar->label, g_free);
}
