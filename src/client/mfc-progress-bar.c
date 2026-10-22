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

constexpr int BAR_LENGTH_MIN = 10;
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
 * mfc_shorten_label:
 *
 * Returns: (transfer full): @label, cut to @max_chars characters with an
 *   ellipsis if it is longer
 */
static char *
mfc_shorten_label(const char *label, int max_chars, gboolean utf8)
{
    const char *ellipsis = utf8 ? "…" : "...";
    int ellipsis_chars = utf8 ? 1 : 3;
    g_autofree char *start = NULL;

    if (g_utf8_strlen(label, -1) <= max_chars)
        return g_strdup(label);
    if (max_chars <= ellipsis_chars)
        return g_utf8_substring(label, 0, max_chars);
    start = g_utf8_substring(label, 0, max_chars - ellipsis_chars);
    return g_strconcat(start, ellipsis, NULL);
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
    g_autofree char *label = NULL;
    gint64 now = g_get_monotonic_time();
    gint64 elapsed_usec = now - bar->start_time;
    guint percent;
    int room;
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

    /* the line has to fit the terminal, or every redraw starts a new line. The label and the bar
     * share the width that is left after the space between them (1), the percentage (5), the
     * rate (14) and the last column (1). If that is too little, we shorten the bar first, and
     * then cut the label */
    room = mfc_terminal_width() - 1 - 5 - (speed != NULL ? 14 : 0) - 1;
    bar_length = CLAMP(room - (int) g_utf8_strlen(bar->label, -1), BAR_LENGTH_MIN, BAR_LENGTH_MAX);
    label = mfc_shorten_label(bar->label, MAX(room - bar_length, 1), bar->utf8);
    n_full = (bar_length * (int) percent) / 100;
    partial = (((bar_length * (int) percent) % 100) * (int) G_N_ELEMENTS(PARTIAL_BLOCKS)) / 100;

    line = g_string_new(label);
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
