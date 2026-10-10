/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Linux kernel interfaces: managed L2TP/IP socket, generic netlink l2tp, rtnetlink. */
#ifndef FG_LINUX_H
#define FG_LINUX_H

#include <net/if.h>
#include <stdbool.h>
#include <stdint.h>

struct mnl_socket;

struct fg_nl {
	struct mnl_socket *rtnl;
	struct mnl_socket *genl;
	uint16_t l2tp_family;
	uint32_t seq;
};

struct fg_link {
	int ifindex;
	char name[IF_NAMESIZE];
	char kind[16];
	int master;
	int link;
	unsigned flags;
	unsigned mtu;
	bool has_stats;
	uint64_t rx_bytes;
	uint64_t tx_bytes;
};

struct fg_route {
	int oif;
	uint32_t prefsrc;	/* network byte order, 0 if absent */
	bool gateway;
};

int fg_nl_open(struct fg_nl *nl);
void fg_nl_close(struct fg_nl *nl);

/* Managed tunnel socket: bound to (local, conn_id), connected to server. Returns fd or -errno. */
int fg_l2tp_socket(uint32_t local_be, uint32_t server_be, uint32_t conn_id);

int fg_l2tp_tunnel_create(struct fg_nl *nl, int fd, uint32_t conn_id, uint32_t peer_conn_id);
int fg_l2tp_tunnel_delete(struct fg_nl *nl, uint32_t conn_id);
int fg_l2tp_session_create(struct fg_nl *nl, uint32_t conn_id, uint32_t session_id,
			   uint32_t peer_session_id, const char *ifname);
int fg_l2tp_session_delete(struct fg_nl *nl, uint32_t conn_id, uint32_t session_id);

/* Look up by name (ifindex 0) or index; -ENODEV if absent. */
int fg_link_get(struct fg_nl *nl, const char *name, int ifindex, struct fg_link *out);
/* mtu 0 = keep; up -1 = keep; master -1 = keep, 0 = detach. */
int fg_link_set(struct fg_nl *nl, int ifindex, unsigned mtu, int up, int master);
int fg_route_get(struct fg_nl *nl, uint32_t dst_be, struct fg_route *out);

/* Event socket for link/address/route changes; returns the mnl socket or NULL. */
struct mnl_socket *fg_rtnl_monitor_open(void);
/* Drain pending events; returns >0 if any event was read, 0 if none, <0 on error. */
int fg_rtnl_monitor_drain(struct mnl_socket *nl);

#endif
