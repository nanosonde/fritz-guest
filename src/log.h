/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Logging shared by daemon modules; tests supply their own implementation. */
#ifndef FG_LOG_H
#define FG_LOG_H

#include <stdbool.h>
#include <syslog.h>

extern bool fg_debug;

void fg_log(int prio, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Rate-limited variant for events a remote peer can trigger repeatedly. */
void fg_log_rl(int prio, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define FG_DBG(...) do { if (fg_debug) fg_log(LOG_DEBUG, __VA_ARGS__); } while (0)

#endif
