/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include <libubox/ulog.h>

#include "log.h"

#define FG_RL_WINDOW 10
#define FG_RL_BURST 5

bool fg_debug;

static void vlog(int prio, const char *fmt, va_list ap)
{
	char buf[256];

	if (prio == LOG_DEBUG && !fg_debug)
		return;
	vsnprintf(buf, sizeof(buf), fmt, ap);
	ulog(prio, "%s\n", buf);
}

void fg_log(int prio, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vlog(prio, fmt, ap);
	va_end(ap);
}

void fg_log_rl(int prio, const char *fmt, ...)
{
	static time_t window;
	static unsigned count, suppressed;
	struct timespec ts;
	va_list ap;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	if (ts.tv_sec - window >= FG_RL_WINDOW) {
		if (suppressed)
			fg_log(LOG_WARNING, "%u similar messages suppressed", suppressed);
		window = ts.tv_sec;
		count = suppressed = 0;
	}
	if (++count > FG_RL_BURST) {
		suppressed++;
		return;
	}
	va_start(ap, fmt);
	vlog(prio, fmt, ap);
	va_end(ap);
}
