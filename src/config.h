/* SPDX-License-Identifier: GPL-2.0-or-later */
/* UCI contract validation (pure; the libuci reader lives in uciconf.c). */
#ifndef FG_CONFIG_H
#define FG_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FG_SECTION "guest"
#define FG_SECTION_TYPE "connection"
#define FG_DEVICE "fritzguest0"
#define FG_NAME_MAX 15

struct fg_config {
	bool enabled;
	uint32_t server;	/* network byte order */
	char server_str[16];
	char uplink[FG_NAME_MAX + 1];
	char bridge[FG_NAME_MAX + 1];
	unsigned mtu;
	unsigned hello_interval;
	unsigned retry_max;
	unsigned reconnect_max;
};

/* Returns the option's string value or NULL if unset; *is_list marks list options. */
typedef const char *(*fg_opt_get)(void *ctx, const char *name, bool *is_list);

void fg_config_defaults(struct fg_config *cfg);
int fg_config_parse(struct fg_config *cfg, fg_opt_get get, void *ctx, char *err, size_t errlen);
bool fg_config_equal(const struct fg_config *a, const struct fg_config *b);
bool fg_valid_ifname(const char *s);
bool fg_valid_netname(const char *s);

/* libuci readers (uciconf.c). */
int fg_uci_load(const char *package, struct fg_config *cfg, char *err, size_t errlen);
/* 1: UCI bridge device, 0: no such device section, -1: exists but not a bridge. */
int fg_uci_bridge_state(const char *bridge);

#endif
