/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

#include "config.h"

void fg_config_defaults(struct fg_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	strcpy(cfg->uplink, "lan");
	cfg->mtu = 1500;
	cfg->hello_interval = 30;
	cfg->retry_max = 5;
	cfg->reconnect_max = 60;
}

bool fg_valid_netname(const char *s)
{
	size_t n = strlen(s);

	if (!n || n > FG_NAME_MAX)
		return false;
	for (; *s; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
		      (*s >= '0' && *s <= '9') || *s == '_'))
			return false;
	return true;
}

bool fg_valid_ifname(const char *s)
{
	size_t n = strlen(s);

	if (!n || n > FG_NAME_MAX || !strcmp(s, ".") || !strcmp(s, ".."))
		return false;
	for (; *s; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
		      (*s >= '0' && *s <= '9') || *s == '_' || *s == '-' || *s == '.'))
			return false;
	return true;
}

static int parse_uint(const char *s, unsigned min, unsigned max, unsigned *out)
{
	unsigned long v = 0;

	if (!*s || strlen(s) > 5)
		return -1;
	for (; *s; s++) {
		if (*s < '0' || *s > '9')
			return -1;
		v = v * 10 + (unsigned)(*s - '0');
	}
	if (v < min || v > max)
		return -1;
	*out = (unsigned)v;
	return 0;
}

static bool unicast_ipv4(uint32_t be)
{
	uint32_t a = ntohl(be);
	uint8_t first = a >> 24;

	return first != 0 && first != 127 && first < 224;
}

#define FAIL(...) do { snprintf(err, errlen, __VA_ARGS__); return -1; } while (0)

int fg_config_parse(struct fg_config *cfg, fg_opt_get get, void *ctx, char *err, size_t errlen)
{
	static const struct {
		const char *name;
		unsigned min, max;
		size_t off;
	} nums[] = {
		{ "mtu", 1280, 1500, offsetof(struct fg_config, mtu) },
		{ "hello_interval", 10, 300, offsetof(struct fg_config, hello_interval) },
		{ "retry_max", 1, 10, offsetof(struct fg_config, retry_max) },
		{ "reconnect_max", 10, 300, offsetof(struct fg_config, reconnect_max) },
	};
	static const char *const known[] = {
		"enabled", "server", "uplink", "bridge", "mtu", "hello_interval",
		"retry_max", "reconnect_max",
	};
	const char *v;
	bool list;

	fg_config_defaults(cfg);
	for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
		get(ctx, known[i], &list);
		if (list)
			FAIL("option '%s' must be a single value, not a list", known[i]);
	}

	v = get(ctx, "enabled", &list);
	if (v) {
		if (!strcmp(v, "1"))
			cfg->enabled = true;
		else if (strcmp(v, "0"))
			FAIL("option 'enabled' must be 0 or 1");
	}

	v = get(ctx, "server", &list);
	if (v && *v) {
		struct in_addr a;

		if (strlen(v) >= sizeof(cfg->server_str) || inet_pton(AF_INET, v, &a) != 1 ||
		    !unicast_ipv4(a.s_addr))
			FAIL("option 'server' must be a unicast IPv4 address literal");
		cfg->server = a.s_addr;
		inet_ntop(AF_INET, &a, cfg->server_str, sizeof(cfg->server_str));
	} else if (cfg->enabled) {
		FAIL("option 'server' is required when enabled");
	}

	v = get(ctx, "uplink", &list);
	if (v) {
		if (!fg_valid_netname(v))
			FAIL("option 'uplink' must be a logical interface name");
		strcpy(cfg->uplink, v);
	}

	v = get(ctx, "bridge", &list);
	if (v && *v) {
		if (!fg_valid_ifname(v) || !strcmp(v, FG_DEVICE))
			FAIL("option 'bridge' must be a bridge device name");
		strcpy(cfg->bridge, v);
	} else if (cfg->enabled) {
		FAIL("option 'bridge' is required when enabled");
	}

	for (size_t i = 0; i < sizeof(nums) / sizeof(nums[0]); i++) {
		v = get(ctx, nums[i].name, &list);
		if (v && parse_uint(v, nums[i].min, nums[i].max,
				    (unsigned *)((char *)cfg + nums[i].off)))
			FAIL("option '%s' must be an integer %u..%u", nums[i].name, nums[i].min, nums[i].max);
	}
	return 0;
}

bool fg_config_equal(const struct fg_config *a, const struct fg_config *b)
{
	return a->enabled == b->enabled && a->server == b->server &&
	       !strcmp(a->uplink, b->uplink) && !strcmp(a->bridge, b->bridge) &&
	       a->mtu == b->mtu && a->hello_interval == b->hello_interval &&
	       a->retry_max == b->retry_max && a->reconnect_max == b->reconnect_max;
}
