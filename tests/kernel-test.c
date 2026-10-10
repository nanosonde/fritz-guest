/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Kernel integration test for src/linux.c in a disposable network namespace.
 * Run via `just test-kernel` (unshare -rn); every object lives in that namespace.
 * Two managed L2TPv3/IP tunnels over loopback stand in for client and FRITZ!Box. */
#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "linux.h"
#include "proto.h"

static unsigned checks;
#define CHECK(cond) do { checks++; if (!(cond)) { \
	fprintf(stderr, "%s:%d: CHECK failed: %s (errno %s)\n", __FILE__, __LINE__, #cond, strerror(errno)); \
	exit(1); } } while (0)

#define CONN_A 100
#define CONN_B 200
#define SID_A 1000
#define SID_B 2000

static struct fg_nl nl;

static void sh(const char *cmd)
{
	CHECK(system(cmd) == 0);
}

static int ifindex(const char *name)
{
	struct fg_link l;

	return fg_link_get(&nl, name, 0, &l) ? 0 : l.ifindex;
}

static bool wait_gone(const char *name)
{
	for (int i = 0; i < 100; i++) {
		if (!ifindex(name))
			return true;
		usleep(20000);
	}
	return false;
}

static int packet_socket(const char *dev)
{
	struct sockaddr_ll sll = { .sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ALL) };
	int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));

	CHECK(fd >= 0);
	sll.sll_ifindex = ifindex(dev);
	CHECK(bind(fd, (struct sockaddr *)&sll, sizeof(sll)) == 0);
	return fd;
}

/* Send a tagged frame on one device and expect it unchanged on another. */
static void expect_frame(const char *from, const char *to, size_t len)
{
	uint8_t frame[1600], buf[2048];
	int tx = packet_socket(from), rx = packet_socket(to);
	struct sockaddr_ll sll = { .sll_family = AF_PACKET, .sll_ifindex = ifindex(from), .sll_halen = 6 };
	bool seen = false;

	memset(frame, 0xff, 6);
	memcpy(frame + 6, "\x02\x11\x22\x33\x44\x55", 6);
	frame[12] = 0x88;
	frame[13] = 0xb5;
	for (size_t i = 14; i < len; i++)
		frame[i] = (uint8_t)(i * 7 + len);
	memcpy(sll.sll_addr, frame, 6);
	CHECK(sendto(tx, frame, len, 0, (struct sockaddr *)&sll, sizeof(sll)) == (ssize_t)len);
	for (int i = 0; i < 50 && !seen; i++) {
		struct pollfd p = { .fd = rx, .events = POLLIN };
		ssize_t n;

		if (poll(&p, 1, 100) <= 0)
			continue;
		n = recv(rx, buf, sizeof(buf), 0);
		seen = n == (ssize_t)len && !memcmp(buf, frame, len);
	}
	CHECK(seen);
	close(tx);
	close(rx);
}

static void test_control_framing(int a, int b)
{
	struct fg_buf m;
	uint8_t buf[256];
	struct sockaddr_in src;
	socklen_t slen = sizeof(src);
	struct pollfd p = { .fd = b, .events = POLLIN };
	ssize_t n;

	/* The kernel adds/strips the zero Session ID: userspace sees the T-bit header. */
	fg_build_start(&m, FG_MSG_HELLO);
	fg_write_header(m.data, m.len, CONN_B, 7, 9);
	CHECK(send(a, m.data, m.len, 0) == (ssize_t)m.len);
	CHECK(poll(&p, 1, 1000) == 1);
	n = recvfrom(b, buf, sizeof(buf), 0, (struct sockaddr *)&src, &slen);
	CHECK(n == (ssize_t)m.len && !memcmp(buf, m.data, m.len));
	CHECK(src.sin_addr.s_addr == htonl(0x7f000001));

	/* A message for another connection ID is not delivered to this socket. */
	fg_write_header(m.data, m.len, CONN_B + 1, 0, 0);
	CHECK(send(a, m.data, m.len, 0) == (ssize_t)m.len);
	CHECK(poll(&p, 1, 200) == 0);
}

int main(void)
{
	struct fg_link l;
	struct fg_route rt;
	int a, b, ret;

	CHECK(fg_nl_open(&nl) == 0);
	CHECK(fg_link_set(&nl, ifindex("lo"), 1500, 1, -1) == 0);

	a = fg_l2tp_socket(htonl(0x7f000001), htonl(0x7f000002), CONN_A);
	if (a == -EPROTONOSUPPORT || a == -ESOCKTNOSUPPORT || a == -EAFNOSUPPORT) {
		printf("SKIP kernel-test: L2TP/IP unavailable (%s)\n", strerror(-a));
		return 77;
	}
	CHECK(a >= 0);
	b = fg_l2tp_socket(htonl(0x7f000002), htonl(0x7f000001), CONN_B);
	CHECK(b >= 0);
	CHECK(fg_l2tp_socket(htonl(0x7f000001), htonl(0x7f000002), CONN_A) == -EADDRINUSE);
	CHECK(fg_route_get(&nl, htonl(0x7f000002), &rt) == 0 && rt.oif == ifindex("lo") && !rt.gateway);

	test_control_framing(a, b);

	/* Foreign objects that must survive untouched. */
	sh("ip link add br-foreign type bridge && ip link add vf0 type veth peer name vf1 && "
	   "ip link set vf0 master br-foreign && ip link add br-guest type bridge && "
	   "ip link add vg0 type veth peer name vg1 && ip link set vg0 master br-guest && "
	   "ip link set br-guest up && ip link set vg0 up && ip link set vg1 up && "
	   "ip link set br-guest type bridge stp_state 0 forward_delay 0");

	ret = fg_l2tp_tunnel_create(&nl, a, CONN_A, CONN_B);
	if (ret == -ENOENT) {
		printf("SKIP kernel-test: generic netlink family l2tp unavailable\n");
		return 77;
	}
	CHECK(ret == 0);
	CHECK(fg_l2tp_tunnel_create(&nl, b, CONN_B, CONN_A) == 0);
	CHECK(fg_l2tp_session_create(&nl, CONN_A, SID_A, SID_B, "fritzguest0") == 0);
	CHECK(fg_l2tp_session_create(&nl, CONN_B, SID_B, SID_A, "peer0") == 0);
	/* Device names are never adopted. */
	CHECK(fg_l2tp_session_create(&nl, CONN_A, SID_A + 1, SID_B, "fritzguest0") < 0);

	CHECK(fg_link_get(&nl, "fritzguest0", 0, &l) == 0);
	CHECK(!(l.flags & IFF_UP));
	CHECK(fg_link_set(&nl, l.ifindex, 1500, -1, -1) == 0);
	CHECK(fg_link_get(&nl, NULL, l.ifindex, &l) == 0 && l.mtu == 1500 && !l.master);
	CHECK(fg_link_set(&nl, ifindex("peer0"), 1500, 1, -1) == 0);

	CHECK(fg_link_set(&nl, l.ifindex, 0, -1, ifindex("br-guest")) == 0);
	CHECK(fg_link_set(&nl, l.ifindex, 0, 1, -1) == 0);
	CHECK(fg_link_get(&nl, NULL, l.ifindex, &l) == 0 && l.master == ifindex("br-guest"));
	CHECK(fg_link_get(&nl, "br-guest", 0, &l) == 0 && !strcmp(l.kind, "bridge"));

	/* Guest Ethernet both ways, including a full 1500-byte payload (fragmented outer IPv4). */
	expect_frame("peer0", "vg1", 60);
	expect_frame("vg1", "peer0", 60);
	expect_frame("peer0", "vg1", 1514);
	expect_frame("vg1", "peer0", 1514);
	CHECK(fg_link_get(&nl, "fritzguest0", 0, &l) == 0 && l.has_stats && l.rx_bytes && l.tx_bytes);

	/* Orderly teardown: down, detach, delete session and tunnel. */
	CHECK(fg_link_set(&nl, l.ifindex, 0, 0, 0) == 0);
	CHECK(fg_link_get(&nl, NULL, l.ifindex, &l) == 0 && !l.master && !(l.flags & IFF_UP));
	CHECK(fg_l2tp_session_delete(&nl, CONN_A, SID_A) == 0);
	CHECK(fg_l2tp_tunnel_delete(&nl, CONN_A) == 0);
	CHECK(wait_gone("fritzguest0"));
	CHECK(ifindex("peer0") && ifindex("br-guest") && ifindex("vg0"));
	CHECK(fg_link_get(&nl, "vf0", 0, &l) == 0 && l.master == ifindex("br-foreign"));
	close(a);

	/* Crash safety: closing the managed socket alone removes tunnel, session and device. */
	a = fg_l2tp_socket(htonl(0x7f000001), htonl(0x7f000002), CONN_A);
	CHECK(a >= 0);
	CHECK(fg_l2tp_tunnel_create(&nl, a, CONN_A, CONN_B) == 0);
	CHECK(fg_l2tp_session_create(&nl, CONN_A, SID_A, SID_B, "fritzguest0") == 0);
	CHECK(fg_link_set(&nl, ifindex("fritzguest0"), 0, -1, ifindex("br-guest")) == 0);
	close(a);
	CHECK(wait_gone("fritzguest0"));
	CHECK(ifindex("br-guest") && ifindex("peer0"));

	close(b);
	CHECK(wait_gone("peer0"));
	fg_nl_close(&nl);
	printf("kernel-test: %u checks passed\n", checks);
	return 0;
}
