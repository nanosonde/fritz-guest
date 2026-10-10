/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <libubox/blobmsg.h>
#include <libubus.h>

#include "netifd.h"

#define FG_UBUS_TIMEOUT 3000
#define FG_MAX_IFACES 32

static struct blob_buf b;

static int lookup(struct ubus_context *ctx, const char *iface, uint32_t *id)
{
	char path[64];

	if (snprintf(path, sizeof(path), "network.interface.%s", iface) >= (int)sizeof(path))
		return -EINVAL;
	return ubus_lookup_id(ctx, path, id) ? -ENOENT : 0;
}

static void copy_name(char *dst, size_t len, const char *src)
{
	if (strlen(src) < len)
		strcpy(dst, src);
	else
		dst[0] = 0;
}

enum { ST_UP, ST_L3, ST_V4, __ST_MAX };
static const struct blobmsg_policy status_policy[__ST_MAX] = {
	[ST_UP] = { "up", BLOBMSG_TYPE_BOOL },
	[ST_L3] = { "l3_device", BLOBMSG_TYPE_STRING },
	[ST_V4] = { "ipv4-address", BLOBMSG_TYPE_ARRAY },
};

static const struct blobmsg_policy addr_policy = { "address", BLOBMSG_TYPE_STRING };

static void status_cb(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct fg_iface_status *st = req->priv;
	struct blob_attr *tb[__ST_MAX], *cur;
	size_t rem;

	(void)type;
	blobmsg_parse(status_policy, __ST_MAX, tb, blob_data(msg), blob_len(msg));
	st->up = tb[ST_UP] && blobmsg_get_bool(tb[ST_UP]);
	if (tb[ST_L3])
		copy_name(st->l3_device, sizeof(st->l3_device), blobmsg_get_string(tb[ST_L3]));
	if (!tb[ST_V4])
		return;
	blobmsg_for_each_attr(cur, tb[ST_V4], rem) {
		struct blob_attr *addr;
		struct in_addr a;

		if (blobmsg_type(cur) != BLOBMSG_TYPE_TABLE || st->n_addr >= FG_MAX_ADDRS)
			continue;
		blobmsg_parse(&addr_policy, 1, &addr, blobmsg_data(cur), blobmsg_data_len(cur));
		if (addr && inet_pton(AF_INET, blobmsg_get_string(addr), &a) == 1)
			st->addr[st->n_addr++] = a.s_addr;
	}
}

int fg_netifd_status(struct ubus_context *ctx, const char *iface, struct fg_iface_status *st)
{
	uint32_t id;
	int ret;

	memset(st, 0, sizeof(*st));
	ret = lookup(ctx, iface, &id);
	if (ret)
		return ret;
	blob_buf_init(&b, 0);
	ret = ubus_invoke(ctx, id, "status", b.head, status_cb, st, FG_UBUS_TIMEOUT);
	return ret == UBUS_STATUS_NOT_FOUND ? -ENOENT : ret ? -EIO : 0;
}

enum { IF_NAME, IF_UP, IF_DEVICE, __IF_MAX };
static const struct blobmsg_policy iface_policy[__IF_MAX] = {
	[IF_NAME] = { "interface", BLOBMSG_TYPE_STRING },
	[IF_UP] = { "up", BLOBMSG_TYPE_BOOL },
	[IF_DEVICE] = { "device", BLOBMSG_TYPE_STRING },
};

static const struct blobmsg_policy dump_policy = { "interface", BLOBMSG_TYPE_ARRAY };

struct dump_ctx {
	const char *bridge;	/* NULL: collect all names */
	char names[FG_MAX_IFACES][48];
	unsigned n;
	bool up;
};

static void dump_cb(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct dump_ctx *dc = req->priv;
	struct blob_attr *list, *cur;
	size_t rem;

	(void)type;
	blobmsg_parse(&dump_policy, 1, &list, blob_data(msg), blob_len(msg));
	if (!list)
		return;
	blobmsg_for_each_attr(cur, list, rem) {
		struct blob_attr *tb[__IF_MAX];
		bool up;

		if (blobmsg_type(cur) != BLOBMSG_TYPE_TABLE)
			continue;
		blobmsg_parse(iface_policy, __IF_MAX, tb, blobmsg_data(cur), blobmsg_data_len(cur));
		if (!tb[IF_NAME] || strlen(blobmsg_get_string(tb[IF_NAME])) >= sizeof(dc->names[0]))
			continue;
		up = tb[IF_UP] && blobmsg_get_bool(tb[IF_UP]);
		if (dc->bridge) {
			if (!tb[IF_DEVICE] || strcmp(blobmsg_get_string(tb[IF_DEVICE]), dc->bridge))
				continue;
			/* Prefer an interface that is up. */
			if (dc->n && (dc->up || !up))
				continue;
			dc->n = 0;
			dc->up = up;
		}
		if (dc->n < FG_MAX_IFACES)
			strcpy(dc->names[dc->n++], blobmsg_get_string(tb[IF_NAME]));
	}
}

static int dump(struct ubus_context *ctx, struct dump_ctx *dc)
{
	uint32_t id;

	if (ubus_lookup_id(ctx, "network.interface", &id))
		return -ENOENT;
	blob_buf_init(&b, 0);
	return ubus_invoke(ctx, id, "dump", b.head, dump_cb, dc, FG_UBUS_TIMEOUT) ? -EIO : 0;
}

int fg_netifd_bridge_iface(struct ubus_context *ctx, const char *bridge, char *out, size_t outlen,
			   bool *up)
{
	struct dump_ctx dc = { .bridge = bridge };
	int ret = dump(ctx, &dc);

	if (ret)
		return ret;
	if (!dc.n || strlen(dc.names[0]) >= outlen)
		return -ENOENT;
	strcpy(out, dc.names[0]);
	*up = dc.up;
	return 0;
}

int fg_netifd_link(struct ubus_context *ctx, const char *iface, const char *dev, bool add)
{
	uint32_t id;
	int ret = lookup(ctx, iface, &id);

	if (ret)
		return ret;
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "name", dev);
	/* link-ext would wait for a link event that never comes for an existing device. */
	blobmsg_add_u8(&b, "link-ext", 0);
	ret = ubus_invoke(ctx, id, add ? "add_device" : "remove_device", b.head, NULL, NULL,
			  FG_UBUS_TIMEOUT);
	return ret == UBUS_STATUS_NOT_FOUND ? -ENOENT : ret ? -EIO : 0;
}

void fg_netifd_forget(struct ubus_context *ctx, const char *dev)
{
	struct dump_ctx dc = { 0 };

	if (dump(ctx, &dc))
		return;
	for (unsigned i = 0; i < dc.n; i++)
		fg_netifd_link(ctx, dc.names[i], dev, false);
}
