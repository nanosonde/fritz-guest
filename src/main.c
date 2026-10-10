/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <libmnl/libmnl.h>
#include <libubox/uloop.h>
#include <libubox/ulog.h>
#include <libubox/utils.h>
#include <libubus.h>

#include "config.h"
#include "ctrl.h"
#include "linux.h"
#include "log.h"
#include "netifd.h"

#ifndef FG_VERSION
#define FG_VERSION "0.1.0"
#endif

#define EVAL_DELAY_MS 300
#define ATTACH_TIMEOUT_MS 10000
#define STABLE_MS 60000
#define MAX_BACKOFF_SHIFT 9

enum state { ST_DISABLED, ST_WAITING, ST_CONNECTING, ST_CONNECTED, ST_BACKOFF, ST_STOPPING, ST_ERROR };

static const char *const state_names[] = {
	"disabled", "waiting", "connecting", "connected", "backoff", "stopping", "error",
};

enum { RES_OK, RES_WAIT, RES_UNSAFE };

struct binding {
	int uplink_ifindex;
	char uplink_dev[IF_NAMESIZE];
	uint32_t local;
	int bridge_ifindex;
	char bridge_iface[48];
};

struct conn {
	struct fg_ctrl ctrl;
	struct uloop_fd fd;
	struct uloop_timeout timer;
	struct binding b;
	struct fg_config cfg;
	uint32_t conn_id;
	uint32_t session_id;
	bool tunnel_created;
	bool session_created;
	bool session_up;
	bool attach_requested;
	int dev_ifindex;
	char kernel_err[96];
};

static struct {
	const char *package;
	struct fg_config cfg;
	bool config_valid;
	enum state state;
	const char *reason;
	char last_error[160];
	char reload_error[160];
	struct conn *active;
	struct conn *draining;
	struct fg_nl nl;
	struct mnl_socket *mon;
	struct uloop_fd mon_fd;
	struct ubus_auto_conn ubus;
	bool ubus_ready;
	unsigned backoff_step;
	uint32_t reconnects;
	int64_t connected_since;
	bool shutting_down;
} d = { .package = "fritz-guest", .reason = "" };

static void evaluate(void);
static void conn_after(struct conn *c);

static int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint32_t random32(void)
{
	uint32_t v;

	while (getrandom(&v, sizeof(v), 0) != (ssize_t)sizeof(v))
		;
	return v;
}

static uint32_t random_id(void)
{
	uint32_t v;

	do
		v = random32();
	while (!v);
	return v;
}

static void set_state(enum state st, const char *reason, const char *detail)
{
	if (detail && *detail)
		snprintf(d.last_error, sizeof(d.last_error), "%s", detail);
	if (st != d.state || strcmp(reason, d.reason))
		fg_log(st == ST_ERROR ? LOG_ERR : LOG_NOTICE, "state %s%s%s%s%s%s", state_names[st],
		       *reason ? " (" : "", reason, *reason ? ")" : "",
		       detail && *detail ? ": " : "", detail ? detail : "");
	d.state = st;
	d.reason = reason;
}

static void eval_cb(struct uloop_timeout *t) { (void)t; evaluate(); }
static struct uloop_timeout eval_timer = { .cb = eval_cb };

static void schedule_evaluate(int ms)
{
	if (!eval_timer.pending || !ms)
		uloop_timeout_set(&eval_timer, ms);
}

/* --- connection objects ------------------------------------------------- */

static void detach(struct conn *c)
{
	struct fg_link l;

	if (c->session_created && c->dev_ifindex)
		fg_link_set(&d.nl, c->dev_ifindex, 0, 0, -1);
	if (c->attach_requested && d.ubus_ready)
		fg_netifd_link(&d.ubus.ctx, c->b.bridge_iface, FG_DEVICE, false);
	c->attach_requested = false;
	/* Fallback if netifd was unreachable: never leave the port enslaved. */
	if (c->session_created && c->dev_ifindex &&
	    !fg_link_get(&d.nl, NULL, c->dev_ifindex, &l) && l.master)
		fg_link_set(&d.nl, c->dev_ifindex, 0, -1, 0);
}

static void conn_free(struct conn *c)
{
	if (c == d.active)
		d.active = NULL;
	if (c == d.draining)
		d.draining = NULL;
	uloop_timeout_cancel(&c->timer);
	uloop_fd_delete(&c->fd);
	detach(c);
	if (c->session_created)
		fg_l2tp_session_delete(&d.nl, c->conn_id, c->session_id);
	if (c->tunnel_created)
		fg_l2tp_tunnel_delete(&d.nl, c->conn_id);
	close(c->fd.fd);
	FG_DBG("released control connection %u", c->conn_id);
	free(c);
}

static void reap_cb(struct uloop_timeout *t)
{
	(void)t;
	if (d.draining && fg_ctrl_finished(&d.draining->ctrl))
		conn_free(d.draining);
	if (d.shutting_down && !d.draining)
		uloop_end();
}
static struct uloop_timeout reap_timer = { .cb = reap_cb };

static void attach_timeout_cb(struct uloop_timeout *t);
static void stable_cb(struct uloop_timeout *t);
static void backoff_cb(struct uloop_timeout *t);
static struct uloop_timeout attach_timer = { .cb = attach_timeout_cb };
static struct uloop_timeout stable_timer = { .cb = stable_cb };
static struct uloop_timeout backoff_timer = { .cb = backoff_cb };

/* Stop forwarding immediately; the control connection finishes in the background. */
static void retire(struct conn *c)
{
	if (c == d.active)
		d.active = NULL;
	uloop_timeout_cancel(&attach_timer);
	uloop_timeout_cancel(&stable_timer);
	detach(c);
	if (d.draining && d.draining != c)
		conn_free(d.draining);
	d.draining = c;
}

static void teardown_active(void)
{
	struct conn *c = d.active;

	if (!c)
		return;
	retire(c);
	fg_ctrl_stop(&c->ctrl, now_ms());
	conn_after(c);
}

static void start_backoff(const char *reason, const char *detail)
{
	unsigned shift = d.backoff_step < MAX_BACKOFF_SHIFT ? d.backoff_step : MAX_BACKOFF_SHIFT;
	unsigned base = 1u << shift;
	unsigned ms;

	if (d.shutting_down)
		return;
	if (base > d.cfg.reconnect_max)
		base = d.cfg.reconnect_max;
	d.backoff_step++;
	ms = base * 1000 + random32() % (base * 250 + 1);
	set_state(ST_BACKOFF, reason, detail);
	FG_DBG("reconnect in %u ms", ms);
	uloop_timeout_set(&backoff_timer, (int)ms);
}

static void conn_fail(struct conn *c, const char *reason, const char *detail)
{
	if (c != d.active)
		return;
	teardown_active();
	start_backoff(reason, detail);
}

static void backoff_cb(struct uloop_timeout *t)
{
	(void)t;
	d.reconnects++;
	set_state(ST_WAITING, "", NULL);
	evaluate();
}

static void stable_cb(struct uloop_timeout *t)
{
	(void)t;
	d.backoff_step = 0;
}

static void attach_timeout_cb(struct uloop_timeout *t)
{
	(void)t;
	if (d.active && d.state != ST_CONNECTED)
		conn_fail(d.active, "bridge_attach_error",
			  "netifd did not attach " FG_DEVICE " to the guest bridge");
}

static void conn_timer_cb(struct uloop_timeout *t)
{
	struct conn *c = container_of(t, struct conn, timer);

	fg_ctrl_timer(&c->ctrl, now_ms());
	conn_after(c);
}

static void conn_after(struct conn *c)
{
	int64_t deadline;

	if (fg_ctrl_finished(&c->ctrl)) {
		uloop_timeout_cancel(&c->timer);
		if (c == d.active) {
			retire(c);
			start_backoff("timeout", "control connection closed");
		}
		uloop_timeout_set(&reap_timer, 0);
		return;
	}
	deadline = fg_ctrl_next_deadline(&c->ctrl);
	if (deadline < 0) {
		uloop_timeout_cancel(&c->timer);
	} else {
		int64_t delta = deadline - now_ms();

		uloop_timeout_set(&c->timer, delta > 0 ? (int)delta : 0);
	}
}

static void conn_fd_cb(struct uloop_fd *u, unsigned int events)
{
	struct conn *c = container_of(u, struct conn, fd);
	uint8_t buf[4096];

	(void)events;
	for (int i = 0; i < 16; i++) {
		/* l2tp_ip reports the source as a plain sockaddr_in. */
		struct sockaddr_in sa;
		socklen_t salen = sizeof(sa);
		ssize_t n = recvfrom(u->fd, buf, sizeof(buf), MSG_TRUNC, (struct sockaddr *)&sa, &salen);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				fg_log_rl(LOG_WARNING, "control receive failed: %s", strerror(errno));
			break;
		}
		if ((size_t)n > sizeof(buf)) {
			fg_log_rl(LOG_WARNING, "dropped oversized control message (%zd bytes)", n);
			continue;
		}
		if (salen < sizeof(sa) || sa.sin_addr.s_addr != c->cfg.server) {
			fg_log_rl(LOG_WARNING, "dropped control message from unexpected source");
			continue;
		}
		fg_ctrl_input(&c->ctrl, buf, (size_t)n, now_ms());
	}
	conn_after(c);
}

static void op_send(void *ctx, const uint8_t *msg, size_t len)
{
	struct conn *c = ctx;

	if (send(c->fd.fd, msg, len, MSG_DONTWAIT) < 0)
		fg_log_rl(LOG_WARNING, "control send failed: %s", strerror(errno));
}

static int op_session_setup(void *ctx, uint32_t peer_conn_id, uint32_t peer_session_id)
{
	struct conn *c = ctx;
	struct fg_link l;
	int ret;

	if (!fg_link_get(&d.nl, FG_DEVICE, 0, &l)) {
		snprintf(c->kernel_err, sizeof(c->kernel_err),
			 FG_DEVICE " already exists and is not owned by this connection");
		return -1;
	}
	ret = fg_l2tp_tunnel_create(&d.nl, c->fd.fd, c->conn_id, peer_conn_id);
	if (ret) {
		snprintf(c->kernel_err, sizeof(c->kernel_err), "L2TP tunnel create failed: %s", strerror(-ret));
		return -1;
	}
	c->tunnel_created = true;
	ret = fg_l2tp_session_create(&d.nl, c->conn_id, c->session_id, peer_session_id, FG_DEVICE);
	if (ret) {
		snprintf(c->kernel_err, sizeof(c->kernel_err), "L2TP Ethernet session create failed: %s",
			 strerror(-ret));
		return -1;
	}
	c->session_created = true;
	ret = fg_link_get(&d.nl, FG_DEVICE, 0, &l);
	if (!ret) {
		c->dev_ifindex = l.ifindex;
		ret = fg_link_set(&d.nl, l.ifindex, c->cfg.mtu, 0, -1);
	}
	if (ret) {
		snprintf(c->kernel_err, sizeof(c->kernel_err), "cannot configure " FG_DEVICE ": %s",
			 strerror(-ret));
		return -1;
	}
	fg_log(LOG_INFO, "created " FG_DEVICE " (tunnel %u/%u, session %u/%u, mtu %u)",
	       c->conn_id, peer_conn_id, c->session_id, peer_session_id, c->cfg.mtu);
	return 0;
}

static void op_session_up(void *ctx, bool circuit_active)
{
	struct conn *c = ctx;

	(void)circuit_active;
	c->session_up = true;
	schedule_evaluate(0);
}

static void op_circuit(void *ctx, bool active)
{
	(void)ctx;
	(void)active;
	schedule_evaluate(0);
}

static void op_down(void *ctx, enum fg_reason r, const char *detail)
{
	struct conn *c = ctx;

	if (c != d.active)
		return;
	retire(c);
	start_backoff(fg_reason_name(r), r == FG_R_KERNEL_ERROR && c->kernel_err[0] ? c->kernel_err : detail);
}

static const struct fg_ctrl_ops ctrl_ops = {
	.send = op_send,
	.session_setup = op_session_setup,
	.session_up = op_session_up,
	.circuit = op_circuit,
	.down = op_down,
};

static void start_conn(const struct binding *b)
{
	struct fg_ctrl_params p = { 0 };
	struct fg_link l;
	struct conn *c;
	char host[64] = "";
	int fd = -EADDRINUSE;

	/* The draining connection may still own the device name. */
	if (d.draining)
		conn_free(d.draining);
	if (!fg_link_get(&d.nl, FG_DEVICE, 0, &l)) {
		start_backoff("kernel_error", FG_DEVICE " exists but is not owned by this daemon");
		return;
	}
	c = calloc(1, sizeof(*c));
	if (!c) {
		start_backoff("kernel_error", "out of memory");
		return;
	}
	c->b = *b;
	c->cfg = d.cfg;
	for (int i = 0; i < 8 && fd == -EADDRINUSE; i++) {
		c->conn_id = random_id();
		fd = fg_l2tp_socket(b->local, d.cfg.server, c->conn_id);
	}
	if (fd < 0) {
		char detail[96];

		snprintf(detail, sizeof(detail), "cannot open L2TP/IP socket: %s%s", strerror(-fd),
			 fd == -EPROTONOSUPPORT || fd == -ESOCKTNOSUPPORT ? " (kmod-l2tp-ip missing?)" : "");
		free(c);
		start_backoff("kernel_error", detail);
		return;
	}
	c->session_id = random_id();
	c->fd.fd = fd;
	c->fd.cb = conn_fd_cb;
	c->timer.cb = conn_timer_cb;

	gethostname(host, sizeof(host) - 1);
	p.conn_id = c->conn_id;
	p.session_id = c->session_id;
	p.router_id = ntohl(b->local);
	p.serial = random32();
	p.hostname = host;
	p.retry_max = d.cfg.retry_max;
	p.hello_ms = d.cfg.hello_interval * 1000;
	fg_ctrl_init(&c->ctrl, &p, &ctrl_ops, c);

	uloop_fd_add(&c->fd, ULOOP_READ);
	d.active = c;
	set_state(ST_CONNECTING, "", NULL);
	fg_log(LOG_INFO, "connecting to %s from %s via %s (control connection %u)",
	       d.cfg.server_str, inet_ntoa((struct in_addr){ b->local }), b->uplink_dev, c->conn_id);
	fg_ctrl_start(&c->ctrl, now_ms());
	conn_after(c);
}

/* --- readiness and safety ----------------------------------------------- */

static int resolve(struct binding *b, const char **reason, char *detail, size_t dlen)
{
	struct fg_iface_status st;
	struct fg_link up, br;
	struct fg_route rt;
	bool br_up = false;
	int ret;

	memset(b, 0, sizeof(*b));
	*reason = "uplink_missing";
	if (!d.ubus_ready) {
		snprintf(detail, dlen, "ubus is not available");
		return RES_WAIT;
	}
	ret = fg_netifd_status(&d.ubus.ctx, d.cfg.uplink, &st);
	if (ret || !st.up || !st.l3_device[0] || !st.n_addr) {
		snprintf(detail, dlen, "interface '%s' is not up with an IPv4 address", d.cfg.uplink);
		return RES_WAIT;
	}
	if (fg_link_get(&d.nl, st.l3_device, 0, &up)) {
		snprintf(detail, dlen, "uplink device '%s' does not exist", st.l3_device);
		return RES_WAIT;
	}

	*reason = "bridge_missing";
	ret = fg_uci_bridge_state(d.cfg.bridge);
	if (ret <= 0) {
		snprintf(detail, dlen, ret ? "network device '%s' is not of type bridge"
				       : "no 'config device' of type bridge named '%s'", d.cfg.bridge);
		if (ret)
			*reason = "bridge_unsafe";
		return ret ? RES_UNSAFE : RES_WAIT;
	}
	if (fg_link_get(&d.nl, d.cfg.bridge, 0, &br)) {
		snprintf(detail, dlen, "bridge device '%s' does not exist", d.cfg.bridge);
		return RES_WAIT;
	}
	*reason = "bridge_unsafe";
	if (strcmp(br.kind, "bridge")) {
		snprintf(detail, dlen, "device '%s' is not a Linux bridge", d.cfg.bridge);
		return RES_UNSAFE;
	}
	if (br.ifindex == up.ifindex || up.master == br.ifindex || up.link == br.ifindex) {
		snprintf(detail, dlen, "guest bridge '%s' carries the uplink device '%s'",
			 d.cfg.bridge, up.name);
		return RES_UNSAFE;
	}

	ret = fg_route_get(&d.nl, d.cfg.server, &rt);
	if (!ret && (rt.oif == br.ifindex || (d.active && rt.oif == d.active->dev_ifindex))) {
		snprintf(detail, dlen, "route to %s uses the guest network", d.cfg.server_str);
		return RES_UNSAFE;
	}
	*reason = "uplink_missing";
	if (ret || rt.oif != up.ifindex) {
		snprintf(detail, dlen, "server %s is not routed via uplink device '%s'",
			 d.cfg.server_str, up.name);
		return RES_WAIT;
	}
	if (rt.gateway) {
		snprintf(detail, dlen, "server %s is not on the uplink's local network", d.cfg.server_str);
		return RES_WAIT;
	}
	for (unsigned i = 0; i < st.n_addr; i++)
		if (st.addr[i] == rt.prefsrc)
			b->local = rt.prefsrc;
	if (!rt.prefsrc)
		b->local = st.addr[0];
	if (!b->local) {
		snprintf(detail, dlen, "no address of '%s' is the source for %s", d.cfg.uplink,
			 d.cfg.server_str);
		return RES_WAIT;
	}

	*reason = "bridge_missing";
	/* An empty bridge has no carrier, so its interface may be down until we join. */
	ret = fg_netifd_bridge_iface(&d.ubus.ctx, d.cfg.bridge, b->bridge_iface,
				     sizeof(b->bridge_iface), &br_up);
	if (ret) {
		snprintf(detail, dlen, "no network interface uses bridge '%s'", d.cfg.bridge);
		return RES_WAIT;
	}
	if (!strcmp(b->bridge_iface, d.cfg.uplink)) {
		*reason = "bridge_unsafe";
		snprintf(detail, dlen, "bridge '%s' belongs to the uplink interface", d.cfg.bridge);
		return RES_UNSAFE;
	}
	b->uplink_ifindex = up.ifindex;
	strcpy(b->uplink_dev, up.name);
	b->bridge_ifindex = br.ifindex;
	return RES_OK;
}

static bool binding_equal(const struct binding *a, const struct binding *b)
{
	return a->uplink_ifindex == b->uplink_ifindex && a->local == b->local &&
	       a->bridge_ifindex == b->bridge_ifindex && !strcmp(a->bridge_iface, b->bridge_iface);
}

static void check_attachment(struct conn *c)
{
	struct fg_link l;
	int ret;

	if (!c->session_up)
		return;
	if (fg_link_get(&d.nl, NULL, c->dev_ifindex, &l) || strcmp(l.name, FG_DEVICE)) {
		conn_fail(c, "kernel_error", FG_DEVICE " disappeared");
		return;
	}
	if (!c->ctrl.circuit_active) {
		if (c->attach_requested || (l.flags & IFF_UP))
			detach(c);
		uloop_timeout_cancel(&attach_timer);
		set_state(ST_CONNECTING, "circuit_inactive", "peer marked the guest circuit inactive");
		return;
	}
	if (!c->attach_requested) {
		ret = fg_netifd_link(&d.ubus.ctx, c->b.bridge_iface, FG_DEVICE, true);
		if (ret) {
			conn_fail(c, "bridge_attach_error", "netifd rejected adding " FG_DEVICE " to the bridge");
			return;
		}
		c->attach_requested = true;
		uloop_timeout_set(&attach_timer, ATTACH_TIMEOUT_MS);
		if (fg_link_get(&d.nl, NULL, c->dev_ifindex, &l))
			return;
	}
	if (l.master == c->b.bridge_ifindex) {
		if (!(l.flags & IFF_UP))
			fg_link_set(&d.nl, c->dev_ifindex, 0, 1, -1);
		if (d.state != ST_CONNECTED) {
			uloop_timeout_cancel(&attach_timer);
			uloop_timeout_set(&stable_timer, STABLE_MS);
			d.connected_since = now_ms();
			set_state(ST_CONNECTED, "", NULL);
			fg_log(LOG_NOTICE, FG_DEVICE " attached to %s", d.cfg.bridge);
		}
	} else if (d.state == ST_CONNECTED) {
		conn_fail(c, "bridge_attach_error", FG_DEVICE " left the guest bridge");
	}
}

static void evaluate(void)
{
	struct binding b;
	const char *reason;
	char detail[160];
	int ret;

	if (d.shutting_down || !d.config_valid)
		return;
	if (!d.cfg.enabled) {
		teardown_active();
		uloop_timeout_cancel(&backoff_timer);
		set_state(ST_DISABLED, "", NULL);
		return;
	}
	if (d.state == ST_BACKOFF)
		return;
	ret = resolve(&b, &reason, detail, sizeof(detail));
	if (ret != RES_OK) {
		teardown_active();
		if (d.state != (ret == RES_UNSAFE ? ST_ERROR : ST_WAITING) || strcmp(d.reason, reason))
			set_state(ret == RES_UNSAFE ? ST_ERROR : ST_WAITING, reason, detail);
		return;
	}
	if (d.active) {
		if (!binding_equal(&b, &d.active->b)) {
			fg_log(LOG_NOTICE, "uplink or bridge changed; renegotiating");
			teardown_active();
			d.reconnects++;
			set_state(ST_WAITING, "", NULL);
			schedule_evaluate(EVAL_DELAY_MS);
			return;
		}
		check_attachment(d.active);
		return;
	}
	start_conn(&b);
}

/* --- configuration ------------------------------------------------------ */

static void reload(void)
{
	struct fg_config cand;
	char err[160];

	if (fg_uci_load(d.package, &cand, err, sizeof(err))) {
		snprintf(d.reload_error, sizeof(d.reload_error), "%s", err);
		fg_log(LOG_ERR, "configuration rejected, keeping active settings: %s", err);
		if (!d.config_valid)
			set_state(ST_ERROR, "invalid_config", err);
		schedule_evaluate(0);
		return;
	}
	d.reload_error[0] = 0;
	if (d.config_valid && fg_config_equal(&cand, &d.cfg)) {
		schedule_evaluate(0);
		return;
	}
	fg_log(LOG_NOTICE, "applying new configuration");
	teardown_active();
	uloop_timeout_cancel(&backoff_timer);
	d.cfg = cand;
	d.config_valid = true;
	d.backoff_step = 0;
	set_state(cand.enabled ? ST_WAITING : ST_DISABLED, "", NULL);
	schedule_evaluate(0);
}

static void hup_cb(struct uloop_signal *s)
{
	(void)s;
	reload();
}

/* --- ubus --------------------------------------------------------------- */

static struct blob_buf sb;

static void add_null(const char *name)
{
	blobmsg_add_field(&sb, BLOBMSG_TYPE_UNSPEC, name, NULL, 0);
}

static void add_id(const char *name, uint32_t v)
{
	if (v)
		blobmsg_add_u64(&sb, name, v);
	else
		add_null(name);
}

static void add_counter(const char *name, bool valid, uint64_t v)
{
	char buf[24];

	if (!valid) {
		add_null(name);
		return;
	}
	snprintf(buf, sizeof(buf), "%" PRIu64, v);
	blobmsg_add_string(&sb, name, buf);
}

static int status_handler(struct ubus_context *ctx, struct ubus_object *obj,
			  struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct conn *c = d.active;
	struct fg_link l = { 0 };
	bool dev = c && c->session_created && !fg_link_get(&d.nl, NULL, c->dev_ifindex, &l);
	char local[INET_ADDRSTRLEN] = "";

	(void)obj;
	(void)method;
	(void)msg;
	if (c)
		inet_ntop(AF_INET, &c->b.local, local, sizeof(local));
	blob_buf_init(&sb, 0);
	blobmsg_add_u32(&sb, "schema_version", 1);
	blobmsg_add_string(&sb, "state", state_names[d.state]);
	blobmsg_add_string(&sb, "reason", d.reason);
	blobmsg_add_string(&sb, "last_error", d.last_error);
	blobmsg_add_string(&sb, "reload_error", d.reload_error);
	blobmsg_add_u8(&sb, "enabled", d.cfg.enabled);
	blobmsg_add_string(&sb, "server", d.cfg.server_str);
	blobmsg_add_string(&sb, "uplink", d.cfg.uplink);
	blobmsg_add_string(&sb, "local_address", local);
	blobmsg_add_string(&sb, "bridge", d.cfg.bridge);
	blobmsg_add_string(&sb, "device", dev ? FG_DEVICE : "");
	add_id("control_id", c ? c->conn_id : 0);
	add_id("peer_control_id", c ? c->ctrl.peer_conn_id : 0);
	add_id("session_id", c && c->ctrl.sess != FG_SESS_NONE ? c->session_id : 0);
	add_id("peer_session_id", c ? c->ctrl.peer_session_id : 0);
	blobmsg_add_u32(&sb, "mtu", d.cfg.mtu);
	if (d.state == ST_CONNECTED)
		blobmsg_add_u64(&sb, "connected_seconds", (uint64_t)(now_ms() - d.connected_since) / 1000);
	else
		add_null("connected_seconds");
	blobmsg_add_u32(&sb, "reconnects", d.reconnects);
	add_counter("rx_bytes", dev && l.has_stats, dev ? l.rx_bytes : 0);
	add_counter("tx_bytes", dev && l.has_stats, dev ? l.tx_bytes : 0);
	return ubus_send_reply(ctx, req, sb.head);
}

static const struct ubus_method methods[] = {
	UBUS_METHOD_NOARG("status", status_handler),
};

static struct ubus_object_type object_type = UBUS_OBJECT_TYPE("fritz-guest", methods);

static struct ubus_object object = {
	.name = "fritz-guest",
	.type = &object_type,
	.methods = methods,
	.n_methods = ARRAY_SIZE(methods),
};

static void netifd_event_cb(struct ubus_context *ctx, struct ubus_event_handler *ev,
			    const char *type, struct blob_attr *msg)
{
	(void)ctx;
	(void)ev;
	(void)type;
	(void)msg;
	schedule_evaluate(EVAL_DELAY_MS);
}

static struct ubus_event_handler netifd_event = { .cb = netifd_event_cb };

static void ubus_connected(struct ubus_context *ctx)
{
	if (!d.ubus_ready) {
		if (ubus_add_object(ctx, &object))
			fg_log(LOG_ERR, "cannot register ubus object fritz-guest");
		if (ubus_register_event_handler(ctx, &netifd_event, "network.interface"))
			fg_log(LOG_ERR, "cannot subscribe to netifd events");
		if (!d.active)
			fg_netifd_forget(ctx, FG_DEVICE);
	}
	d.ubus_ready = true;
	schedule_evaluate(0);
}

static void mon_cb(struct uloop_fd *u, unsigned int events)
{
	(void)u;
	(void)events;
	if (fg_rtnl_monitor_drain(d.mon))
		schedule_evaluate(EVAL_DELAY_MS);
}

/* --- main --------------------------------------------------------------- */

static void shutdown_timeout_cb(struct uloop_timeout *t)
{
	(void)t;
	uloop_end();
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [--config <uci-package>] [--check-config] [--debug] [--version]\n", prog);
}

static bool valid_package(const char *s)
{
	if (!*s || strlen(s) > 32)
		return false;
	for (; *s; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-' || *s == '_'))
			return false;
	return true;
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "config", required_argument, NULL, 'c' },
		{ "check-config", no_argument, NULL, 'C' },
		{ "debug", no_argument, NULL, 'd' },
		{ "version", no_argument, NULL, 'V' },
		{ "help", no_argument, NULL, 'h' },
		{ 0 }
	};
	struct uloop_timeout shutdown_timer = { .cb = shutdown_timeout_cb };
	struct uloop_signal hup = { .signo = SIGHUP, .cb = hup_cb };
	bool check = false;
	char err[160];
	int opt;

	while ((opt = getopt_long(argc, argv, "c:CdVh", opts, NULL)) != -1) {
		switch (opt) {
		case 'c':
			d.package = optarg;
			break;
		case 'C':
			check = true;
			break;
		case 'd':
			fg_debug = true;
			break;
		case 'V':
			printf("fritz-guestd %s\n", FG_VERSION);
			return 0;
		default:
			usage(argv[0]);
			return opt == 'h' ? 0 : 2;
		}
	}
	if (optind != argc || !valid_package(d.package)) {
		usage(argv[0]);
		return 2;
	}

	if (check) {
		ulog_open(ULOG_STDIO, LOG_DAEMON, "fritz-guestd");
		if (fg_uci_load(d.package, &d.cfg, err, sizeof(err))) {
			fprintf(stderr, "invalid configuration: %s\n", err);
			return 1;
		}
		if (d.cfg.enabled && fg_uci_bridge_state(d.cfg.bridge) == 0)
			fprintf(stderr, "warning: no 'config device' of type bridge named '%s'\n", d.cfg.bridge);
		printf("configuration valid (%s)\n", d.cfg.enabled ? "enabled" : "disabled");
		return 0;
	}

	ulog_open(ULOG_SYSLOG, LOG_DAEMON, "fritz-guestd");
	signal(SIGPIPE, SIG_IGN);
	uloop_init();

	opt = fg_nl_open(&d.nl);
	if (opt) {
		fg_log(LOG_ERR, "cannot open netlink sockets: %s", strerror(-opt));
		return 1;
	}
	d.mon = fg_rtnl_monitor_open();
	if (!d.mon) {
		fg_log(LOG_ERR, "cannot subscribe to rtnetlink events: %s", strerror(errno));
		return 1;
	}
	d.mon_fd.fd = mnl_socket_get_fd(d.mon);
	d.mon_fd.cb = mon_cb;
	uloop_fd_add(&d.mon_fd, ULOOP_READ);

	fg_config_defaults(&d.cfg);
	if (fg_uci_load(d.package, &d.cfg, err, sizeof(err))) {
		fg_config_defaults(&d.cfg);
		set_state(ST_ERROR, "invalid_config", err);
	} else {
		d.config_valid = true;
		set_state(d.cfg.enabled ? ST_WAITING : ST_DISABLED, "", NULL);
	}

	uloop_signal_add(&hup);
	d.ubus.cb = ubus_connected;
	ubus_auto_connect(&d.ubus);
	fg_log(LOG_INFO, "fritz-guestd %s started", FG_VERSION);

	uloop_run();

	d.shutting_down = true;
	uloop_timeout_cancel(&eval_timer);
	uloop_timeout_cancel(&backoff_timer);
	teardown_active();
	set_state(ST_STOPPING, "", NULL);
	if (d.draining && !fg_ctrl_finished(&d.draining->ctrl)) {
		uloop_timeout_set(&shutdown_timer,
				  (int)fg_ctrl_cycle_ms(d.cfg.retry_max) + 2000);
		uloop_run();
	}
	if (d.draining)
		conn_free(d.draining);
	if (d.active)
		conn_free(d.active);
	uloop_timeout_cancel(&shutdown_timer);
	ubus_auto_shutdown(&d.ubus);
	mnl_socket_close(d.mon);
	fg_nl_close(&d.nl);
	uloop_done();
	fg_log(LOG_INFO, "stopped");
	return 0;
}
