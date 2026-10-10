/* SPDX-License-Identifier: GPL-2.0-or-later */
/* netifd queries and dynamic bridge membership over ubus. */
#ifndef FG_NETIFD_H
#define FG_NETIFD_H

#include <net/if.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ubus_context;

#define FG_MAX_ADDRS 8

struct fg_iface_status {
	bool up;
	char l3_device[IF_NAMESIZE];
	unsigned n_addr;
	uint32_t addr[FG_MAX_ADDRS];	/* network byte order */
};

/* 0 on success, -ENOENT if the interface is unknown, other -errno on ubus failure. */
int fg_netifd_status(struct ubus_context *ctx, const char *iface, struct fg_iface_status *st);
/* Find the logical interface whose device is `bridge`; 0 if found. */
int fg_netifd_bridge_iface(struct ubus_context *ctx, const char *bridge, char *out, size_t outlen,
			   bool *up);
/* add_device/remove_device on the logical interface that owns the bridge. */
int fg_netifd_link(struct ubus_context *ctx, const char *iface, const char *dev, bool add);
/* Drop stale dynamic membership of `dev` from every interface (crash recovery). */
void fg_netifd_forget(struct ubus_context *ctx, const char *dev);

#endif
