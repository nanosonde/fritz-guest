/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>

#include <libmnl/libmnl.h>
#include <linux/genetlink.h>
#include <linux/l2tp.h>
#include <linux/rtnetlink.h>

#include "linux.h"

/* musl's netinet/in.h does not define it. */
#ifndef IPPROTO_L2TP
#define IPPROTO_L2TP 115
#endif

static int talk(struct mnl_socket *sock, uint32_t *seqp, struct nlmsghdr *nlh,
		mnl_cb_t cb, void *data)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	uint32_t portid = mnl_socket_get_portid(sock);
	ssize_t n;
	int ret;

	nlh->nlmsg_flags |= NLM_F_REQUEST | NLM_F_ACK;
	nlh->nlmsg_seq = ++*seqp;
	if (mnl_socket_sendto(sock, nlh, nlh->nlmsg_len) < 0)
		return -errno;
	for (;;) {
		n = mnl_socket_recvfrom(sock, buf, sizeof(buf));
		if (n < 0)
			return -errno;
		ret = mnl_cb_run(buf, (size_t)n, nlh->nlmsg_seq, portid, cb, data);
		if (ret < 0)
			return errno ? -errno : -EPROTO;
		if (ret == MNL_CB_STOP)
			return 0;
	}
}

static struct mnl_socket *open_bound(int bus)
{
	struct mnl_socket *s = mnl_socket_open2(bus, SOCK_CLOEXEC);

	if (s && mnl_socket_bind(s, 0, MNL_SOCKET_AUTOPID) < 0) {
		mnl_socket_close(s);
		s = NULL;
	}
	return s;
}

int fg_nl_open(struct fg_nl *nl)
{
	int err;

	memset(nl, 0, sizeof(*nl));
	nl->rtnl = open_bound(NETLINK_ROUTE);
	nl->genl = open_bound(NETLINK_GENERIC);
	if (!nl->rtnl || !nl->genl) {
		err = errno ? -errno : -EIO;
		fg_nl_close(nl);
		return err;
	}
	return 0;
}

void fg_nl_close(struct fg_nl *nl)
{
	if (nl->rtnl)
		mnl_socket_close(nl->rtnl);
	if (nl->genl)
		mnl_socket_close(nl->genl);
	nl->rtnl = nl->genl = NULL;
	nl->l2tp_family = 0;
}

int fg_l2tp_socket(uint32_t local_be, uint32_t server_be, uint32_t conn_id)
{
	struct sockaddr_l2tpip a = { .l2tp_family = AF_INET };
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, IPPROTO_L2TP);
	int err;

	if (fd < 0)
		return -errno;
	a.l2tp_addr.s_addr = local_be;
	a.l2tp_conn_id = conn_id;
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0)
		goto fail;
	a.l2tp_addr.s_addr = server_be;
	a.l2tp_conn_id = 0;
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0)
		goto fail;
	return fd;
fail:
	err = -errno;
	close(fd);
	return err;
}

static int family_cb(const struct nlmsghdr *nlh, void *data)
{
	struct nlattr *attr;
	uint16_t *id = data;

	mnl_attr_for_each(attr, nlh, sizeof(struct genlmsghdr)) {
		if (mnl_attr_get_type(attr) == CTRL_ATTR_FAMILY_ID &&
		    mnl_attr_validate(attr, MNL_TYPE_U16) >= 0)
			*id = mnl_attr_get_u16(attr);
	}
	return MNL_CB_OK;
}

static struct nlmsghdr *genl_msg(char *buf, uint16_t type, uint8_t cmd, uint8_t version)
{
	struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
	struct genlmsghdr *g;

	nlh->nlmsg_type = type;
	g = mnl_nlmsg_put_extra_header(nlh, sizeof(*g));
	g->cmd = cmd;
	g->version = version;
	return nlh;
}

static int l2tp_msg(struct fg_nl *nl, char *buf, uint8_t cmd, struct nlmsghdr **out)
{
	if (!nl->l2tp_family) {
		struct nlmsghdr *nlh = genl_msg(buf, GENL_ID_CTRL, CTRL_CMD_GETFAMILY, 1);
		int ret;

		mnl_attr_put_strz(nlh, CTRL_ATTR_FAMILY_NAME, L2TP_GENL_NAME);
		ret = talk(nl->genl, &nl->seq, nlh, family_cb, &nl->l2tp_family);
		if (ret)
			return ret;
		if (!nl->l2tp_family)
			return -ENOENT;
	}
	*out = genl_msg(buf, nl->l2tp_family, cmd, L2TP_GENL_VERSION);
	return 0;
}

int fg_l2tp_tunnel_create(struct fg_nl *nl, int fd, uint32_t conn_id, uint32_t peer_conn_id)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;
	int ret = l2tp_msg(nl, buf, L2TP_CMD_TUNNEL_CREATE, &nlh);

	if (ret)
		return ret;
	mnl_attr_put_u32(nlh, L2TP_ATTR_CONN_ID, conn_id);
	mnl_attr_put_u32(nlh, L2TP_ATTR_PEER_CONN_ID, peer_conn_id);
	mnl_attr_put_u8(nlh, L2TP_ATTR_PROTO_VERSION, 3);
	mnl_attr_put_u16(nlh, L2TP_ATTR_ENCAP_TYPE, L2TP_ENCAPTYPE_IP);
	mnl_attr_put_u32(nlh, L2TP_ATTR_FD, (uint32_t)fd);
	return talk(nl->genl, &nl->seq, nlh, NULL, NULL);
}

int fg_l2tp_tunnel_delete(struct fg_nl *nl, uint32_t conn_id)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;
	int ret = l2tp_msg(nl, buf, L2TP_CMD_TUNNEL_DELETE, &nlh);

	if (ret)
		return ret;
	mnl_attr_put_u32(nlh, L2TP_ATTR_CONN_ID, conn_id);
	return talk(nl->genl, &nl->seq, nlh, NULL, NULL);
}

int fg_l2tp_session_create(struct fg_nl *nl, uint32_t conn_id, uint32_t session_id,
			   uint32_t peer_session_id, const char *ifname)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;
	int ret = l2tp_msg(nl, buf, L2TP_CMD_SESSION_CREATE, &nlh);

	if (ret)
		return ret;
	mnl_attr_put_u32(nlh, L2TP_ATTR_CONN_ID, conn_id);
	mnl_attr_put_u32(nlh, L2TP_ATTR_SESSION_ID, session_id);
	mnl_attr_put_u32(nlh, L2TP_ATTR_PEER_SESSION_ID, peer_session_id);
	mnl_attr_put_u16(nlh, L2TP_ATTR_PW_TYPE, L2TP_PWTYPE_ETH);
	mnl_attr_put_u8(nlh, L2TP_ATTR_L2SPEC_TYPE, L2TP_L2SPECTYPE_DEFAULT);
	mnl_attr_put_strz(nlh, L2TP_ATTR_IFNAME, ifname);
	return talk(nl->genl, &nl->seq, nlh, NULL, NULL);
}

int fg_l2tp_session_delete(struct fg_nl *nl, uint32_t conn_id, uint32_t session_id)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;
	int ret = l2tp_msg(nl, buf, L2TP_CMD_SESSION_DELETE, &nlh);

	if (ret)
		return ret;
	mnl_attr_put_u32(nlh, L2TP_ATTR_CONN_ID, conn_id);
	mnl_attr_put_u32(nlh, L2TP_ATTR_SESSION_ID, session_id);
	return talk(nl->genl, &nl->seq, nlh, NULL, NULL);
}

static int linkinfo_cb(const struct nlattr *attr, void *data)
{
	struct fg_link *l = data;

	if (mnl_attr_get_type(attr) == IFLA_INFO_KIND && mnl_attr_validate(attr, MNL_TYPE_STRING) >= 0) {
		strncpy(l->kind, mnl_attr_get_str(attr), sizeof(l->kind) - 1);
		l->kind[sizeof(l->kind) - 1] = 0;
	}
	return MNL_CB_OK;
}

static int link_cb(const struct nlmsghdr *nlh, void *data)
{
	struct ifinfomsg *ifi = mnl_nlmsg_get_payload(nlh);
	struct fg_link *l = data;
	struct nlattr *attr;

	if (nlh->nlmsg_type != RTM_NEWLINK)
		return MNL_CB_OK;
	memset(l, 0, sizeof(*l));
	l->ifindex = ifi->ifi_index;
	l->flags = ifi->ifi_flags;
	mnl_attr_for_each(attr, nlh, sizeof(*ifi)) {
		switch (mnl_attr_get_type(attr)) {
		case IFLA_IFNAME:
			if (mnl_attr_validate(attr, MNL_TYPE_STRING) >= 0) {
				strncpy(l->name, mnl_attr_get_str(attr), sizeof(l->name) - 1);
				l->name[sizeof(l->name) - 1] = 0;
			}
			break;
		case IFLA_MASTER:
			if (mnl_attr_validate(attr, MNL_TYPE_U32) >= 0)
				l->master = (int)mnl_attr_get_u32(attr);
			break;
		case IFLA_LINK:
			if (mnl_attr_validate(attr, MNL_TYPE_U32) >= 0)
				l->link = (int)mnl_attr_get_u32(attr);
			break;
		case IFLA_MTU:
			if (mnl_attr_validate(attr, MNL_TYPE_U32) >= 0)
				l->mtu = mnl_attr_get_u32(attr);
			break;
		case IFLA_STATS64:
			if (mnl_attr_get_payload_len(attr) >= sizeof(struct rtnl_link_stats64)) {
				struct rtnl_link_stats64 st;

				memcpy(&st, mnl_attr_get_payload(attr), sizeof(st));
				l->has_stats = true;
				l->rx_bytes = st.rx_bytes;
				l->tx_bytes = st.tx_bytes;
			}
			break;
		case IFLA_LINKINFO:
			mnl_attr_parse_nested(attr, linkinfo_cb, l);
			break;
		}
	}
	return MNL_CB_OK;
}

int fg_link_get(struct fg_nl *nl, const char *name, int ifindex, struct fg_link *out)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
	struct ifinfomsg *ifi;
	int ret;

	nlh->nlmsg_type = RTM_GETLINK;
	ifi = mnl_nlmsg_put_extra_header(nlh, sizeof(*ifi));
	ifi->ifi_family = AF_UNSPEC;
	ifi->ifi_index = ifindex;
	if (!ifindex) {
		if (!name || !*name || strlen(name) >= IF_NAMESIZE)
			return -ENODEV;
		mnl_attr_put_strz(nlh, IFLA_IFNAME, name);
	}
	memset(out, 0, sizeof(*out));
	ret = talk(nl->rtnl, &nl->seq, nlh, link_cb, out);
	if (!ret && !out->ifindex)
		ret = -ENODEV;
	return ret;
}

int fg_link_set(struct fg_nl *nl, int ifindex, unsigned mtu, int up, int master)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
	struct ifinfomsg *ifi;

	nlh->nlmsg_type = RTM_NEWLINK;
	ifi = mnl_nlmsg_put_extra_header(nlh, sizeof(*ifi));
	ifi->ifi_family = AF_UNSPEC;
	ifi->ifi_index = ifindex;
	if (up >= 0) {
		ifi->ifi_change = IFF_UP;
		ifi->ifi_flags = up ? IFF_UP : 0;
	}
	if (mtu)
		mnl_attr_put_u32(nlh, IFLA_MTU, mtu);
	if (master >= 0)
		mnl_attr_put_u32(nlh, IFLA_MASTER, (uint32_t)master);
	return talk(nl->rtnl, &nl->seq, nlh, NULL, NULL);
}

static int route_cb(const struct nlmsghdr *nlh, void *data)
{
	struct rtmsg *rtm = mnl_nlmsg_get_payload(nlh);
	struct fg_route *r = data;
	struct nlattr *attr;

	if (nlh->nlmsg_type != RTM_NEWROUTE)
		return MNL_CB_OK;
	mnl_attr_for_each(attr, nlh, sizeof(*rtm)) {
		switch (mnl_attr_get_type(attr)) {
		case RTA_OIF:
			if (mnl_attr_validate(attr, MNL_TYPE_U32) >= 0)
				r->oif = (int)mnl_attr_get_u32(attr);
			break;
		case RTA_PREFSRC:
			if (mnl_attr_validate(attr, MNL_TYPE_U32) >= 0)
				r->prefsrc = mnl_attr_get_u32(attr);
			break;
		case RTA_GATEWAY:
			r->gateway = true;
			break;
		}
	}
	return MNL_CB_OK;
}

int fg_route_get(struct fg_nl *nl, uint32_t dst_be, struct fg_route *out)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
	struct rtmsg *rtm;

	nlh->nlmsg_type = RTM_GETROUTE;
	rtm = mnl_nlmsg_put_extra_header(nlh, sizeof(*rtm));
	rtm->rtm_family = AF_INET;
	rtm->rtm_dst_len = 32;
	mnl_attr_put_u32(nlh, RTA_DST, dst_be);
	memset(out, 0, sizeof(*out));
	return talk(nl->rtnl, &nl->seq, nlh, route_cb, out);
}

struct mnl_socket *fg_rtnl_monitor_open(void)
{
	struct mnl_socket *s = mnl_socket_open2(NETLINK_ROUTE, SOCK_CLOEXEC | SOCK_NONBLOCK);

	if (s && mnl_socket_bind(s, RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV4_ROUTE,
				 MNL_SOCKET_AUTOPID) < 0) {
		mnl_socket_close(s);
		s = NULL;
	}
	return s;
}

int fg_rtnl_monitor_drain(struct mnl_socket *s)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	int events = 0;

	for (;;) {
		ssize_t n = mnl_socket_recvfrom(s, buf, sizeof(buf));

		if (n > 0) {
			events++;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == ENOBUFS)
			return 1;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return events;
		return n < 0 ? -errno : events;
	}
}
