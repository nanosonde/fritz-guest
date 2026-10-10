/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host test: codec, reliable transport, client FSM and UCI validation.
 * Built with ASan/UBSan by `just test-protocol`; no OpenWrt libraries needed. */
#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "ctrl.h"
#include "log.h"
#include "proto.h"

bool fg_debug;
void fg_log(int prio, const char *fmt, ...) { (void)prio; (void)fmt; }
void fg_log_rl(int prio, const char *fmt, ...) { (void)prio; (void)fmt; }

static unsigned checks;
#define CHECK(cond) do { checks++; if (!(cond)) { \
	fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

#define LOCAL_CONN 0x11111111u
#define LOCAL_SID 0x22222222u
#define PEER_CONN 777u
#define PEER_SID 1001u
#define MAX_SENT 128

/* --- test harness: captured output and callback log --------------------- */

struct sent { uint8_t data[FG_MAX_MSG]; size_t len; };

static struct {
	struct sent sent[MAX_SENT];
	unsigned n_sent, seen;
	unsigned setups, ups, downs;
	int setup_result;
	bool last_up_active;
	int circuit_events;
	bool circuit_active;
	enum fg_reason down_reason;
	uint32_t setup_peer_conn, setup_peer_sid;
} h;

static void op_send(void *ctx, const uint8_t *msg, size_t len)
{
	(void)ctx;
	if (h.n_sent < MAX_SENT && len <= FG_MAX_MSG) {
		memcpy(h.sent[h.n_sent].data, msg, len);
		h.sent[h.n_sent++].len = len;
	}
}

static int op_setup(void *ctx, uint32_t pc, uint32_t ps)
{
	(void)ctx;
	h.setups++;
	h.setup_peer_conn = pc;
	h.setup_peer_sid = ps;
	return h.setup_result;
}

static void op_up(void *ctx, bool active) { (void)ctx; h.ups++; h.last_up_active = active; }
static void op_circuit(void *ctx, bool active) { (void)ctx; h.circuit_events++; h.circuit_active = active; }
static void op_down(void *ctx, enum fg_reason r, const char *d) { (void)ctx; (void)d; h.downs++; h.down_reason = r; }

static const struct fg_ctrl_ops ops = { op_send, op_setup, op_up, op_circuit, op_down };

/* Peer view of the reliable channel. */
static struct { uint16_t ns, nr; } peer;

static void reset(struct fg_ctrl *c, unsigned retry_max)
{
	struct fg_ctrl_params p = {
		.conn_id = LOCAL_CONN, .session_id = LOCAL_SID, .router_id = 0xc0a8b2fd,
		.serial = 42, .hostname = "openwrt-test", .retry_max = retry_max, .hello_ms = 30000,
	};

	memset(&h, 0, sizeof(h));
	memset(&peer, 0, sizeof(peer));
	fg_ctrl_init(c, &p, &ops, NULL);
}

static void parse_sent(unsigned i, struct fg_msg *m)
{
	CHECK(i < h.n_sent);
	CHECK(fg_parse(h.sent[i].data, h.sent[i].len, m) == 0);
}

static uint16_t sent_type(unsigned i)
{
	struct fg_msg m;

	parse_sent(i, &m);
	return m.zlb ? FG_MSG_ACK : m.type;
}

/* Peer reads everything new we sent; non-ACK messages advance its Nr. */
static unsigned peer_read(void)
{
	unsigned n = 0;

	for (; h.seen < h.n_sent; h.seen++) {
		struct fg_msg m;

		parse_sent(h.seen, &m);
		if (!m.zlb && m.type != FG_MSG_ACK && m.ns == peer.nr) {
			peer.nr++;
			n++;
		}
	}
	return n;
}

static void raw_avp(struct fg_buf *b, uint16_t vendor, uint16_t type, const void *val, size_t len,
		    bool mandatory, bool hidden)
{
	uint8_t *p = b->data + b->len;

	fg_wr16(p, (uint16_t)(6 + len) | (mandatory ? 0x8000 : 0) | (hidden ? 0x4000 : 0));
	fg_wr16(p + 2, vendor);
	fg_wr16(p + 4, type);
	memcpy(p + 6, val, len);
	b->len += 6 + len;
}

static void deliver(struct fg_ctrl *c, struct fg_buf *b, int64_t now)
{
	bool ack = b->len == FG_HDR_LEN || fg_rd16(b->data + FG_HDR_LEN + 6) == FG_MSG_ACK;

	fg_write_header(b->data, b->len, LOCAL_CONN, peer.ns, peer.nr);
	if (!ack)
		peer.ns++;
	fg_ctrl_input(c, b->data, b->len, now);
}

static void peer_ack(struct fg_ctrl *c, int64_t now)
{
	struct fg_buf b;

	peer_read();
	fg_build_start(&b, FG_MSG_ACK);
	deliver(c, &b, now);
}

static void build_sccrp(struct fg_buf *b, bool ethernet)
{
	fg_build_start(b, FG_MSG_SCCRP);
	fg_put_avp(b, FG_AVP_HOSTNAME, "fritz.box", 9, true);
	fg_put_u32(b, FG_AVP_ROUTER_ID, 0xc0a8b201, true);
	fg_put_u32(b, FG_AVP_CONN_ID, PEER_CONN, true);
	fg_put_u16(b, FG_AVP_PW_CAPS, ethernet ? FG_PW_ETHERNET : 7, true);
	fg_put_avp(b, FG_AVP_VENDOR_NAME, "AVM GmbH", 8, false);
}

static void build_icrp(struct fg_buf *b)
{
	fg_build_start(b, FG_MSG_ICRP);
	fg_put_u32(b, FG_AVP_LOCAL_SID, PEER_SID, true);
	fg_put_u32(b, FG_AVP_REMOTE_SID, LOCAL_SID, true);
	fg_put_u16(b, FG_AVP_CIRCUIT, FG_CIRCUIT_ACTIVE | FG_CIRCUIT_NEW, true);
	fg_put_u16(b, FG_AVP_L2SPEC, FG_L2SPEC_DEFAULT, true);
	fg_put_u16(b, FG_AVP_DATA_SEQ, 0, true);
}

/* Drive SCCRQ..SCCCN; returns with ICRQ in flight. */
static void to_icrq(struct fg_ctrl *c, int64_t now)
{
	struct fg_buf b;

	fg_ctrl_start(c, now);
	CHECK(peer_read() == 1);
	build_sccrp(&b, true);
	deliver(c, &b, now);
	CHECK(c->state == FG_CTRL_ESTABLISHED);
	CHECK(sent_type(h.n_sent - 1) == FG_MSG_SCCCN);
	peer_ack(c, now);
	CHECK(sent_type(h.n_sent - 1) == FG_MSG_ICRQ);
	peer_read();
}

/* Full establishment; returns with the session up. */
static void establish(struct fg_ctrl *c, int64_t now)
{
	struct fg_buf b;

	to_icrq(c, now);
	build_icrp(&b);
	deliver(c, &b, now);
	CHECK(h.setups == 1 && h.setup_peer_conn == PEER_CONN && h.setup_peer_sid == PEER_SID);
	CHECK(sent_type(h.n_sent - 1) == FG_MSG_ICCN);
	peer_ack(c, now);
	CHECK(h.ups == 1 && h.last_up_active);
	CHECK(c->sess == FG_SESS_ESTABLISHED);
}

/* Expect the last sent message to be StopCCN with the given result code. */
static void expect_stopccn(struct fg_ctrl *c, uint16_t result, uint16_t error)
{
	struct fg_msg m;
	const struct fg_avp *a;
	uint32_t id;
	bool dup;

	parse_sent(h.n_sent - 1, &m);
	CHECK(m.type == FG_MSG_STOPCCN);
	a = fg_find(&m, FG_VENDOR_IETF, FG_AVP_RESULT, &dup);
	CHECK(a && a->len >= 2 && fg_rd16(a->val) == result);
	if (error)
		CHECK(a->len >= 4 && fg_rd16(a->val + 2) == error);
	CHECK(fg_get_u32(&m, FG_VENDOR_IETF, FG_AVP_CONN_ID, &id) == 0 && id == LOCAL_CONN);
	CHECK(c->state == FG_CTRL_STOPPING);
}

/* --- tests ------------------------------------------------------------- */

static void test_outgoing_bytes(void)
{
	struct fg_ctrl c;
	struct fg_msg m;
	uint16_t v;
	uint32_t id;
	bool dup;
	const struct fg_avp *a;

	reset(&c, 5);
	fg_ctrl_start(&c, 0);
	CHECK(h.n_sent == 1);
	/* T/L/S set, version 3, length excludes the zero Session ID (added by the kernel). */
	CHECK(fg_rd16(h.sent[0].data) == 0xc803);
	CHECK(fg_rd16(h.sent[0].data + 2) == h.sent[0].len);
	CHECK(fg_rd32(h.sent[0].data + 4) == 0);
	parse_sent(0, &m);
	CHECK(m.type == FG_MSG_SCCRQ && m.ns == 0 && m.nr == 0 && m.avps[0].mandatory);
	/* Same AVP order as the live-tested probe. */
	CHECK(m.n_avps == 6);
	CHECK(m.avps[1].type == FG_AVP_HOSTNAME && m.avps[1].len == 12);
	CHECK(m.avps[2].type == FG_AVP_ROUTER_ID && m.avps[3].type == FG_AVP_CONN_ID);
	CHECK(m.avps[4].type == FG_AVP_PW_CAPS && m.avps[5].type == FG_AVP_RWS);
	CHECK(fg_get_u32(&m, 0, FG_AVP_CONN_ID, &id) == 0 && id == LOCAL_CONN);
	CHECK(fg_get_u16(&m, 0, FG_AVP_PW_CAPS, &v) == 0 && v == FG_PW_ETHERNET);
	CHECK(fg_get_u16(&m, 0, FG_AVP_RWS, &v) == 0 && v == 1);

	reset(&c, 5);
	to_icrq(&c, 0);
	parse_sent(h.n_sent - 1, &m);
	CHECK(m.conn_id == PEER_CONN);
	CHECK(fg_get_u32(&m, 0, FG_AVP_LOCAL_SID, &id) == 0 && id == LOCAL_SID);
	CHECK(fg_get_u32(&m, 0, FG_AVP_REMOTE_SID, &id) == 0 && id == 0);
	CHECK(fg_get_u16(&m, 0, FG_AVP_PW_TYPE, &v) == 0 && v == FG_PW_ETHERNET);
	CHECK(fg_get_u16(&m, 0, FG_AVP_CIRCUIT, &v) == 0 && v == 3);
	CHECK(fg_get_u16(&m, 0, FG_AVP_L2SPEC, &v) == 0 && v == 1);
	CHECK(fg_get_u16(&m, 0, FG_AVP_DATA_SEQ, &v) == 0 && v == 0);
	a = fg_find(&m, 0, FG_AVP_SERIAL, &dup);
	CHECK(a && !a->mandatory);
	a = fg_find(&m, 0, FG_AVP_REMOTE_END, &dup);
	CHECK(a && a->len == 15 && !memcmp(a->val, "fritz.box:guest", 15));
}

static void test_malformed(void)
{
	struct fg_buf b;
	struct fg_msg m;
	uint8_t junk[FG_MAX_MSG];

	fg_build_start(&b, FG_MSG_HELLO);
	fg_write_header(b.data, b.len, 1, 0, 0);
	CHECK(fg_parse(b.data, b.len, &m) == 0);
	CHECK(fg_parse(b.data, 11, &m) == -1);			/* truncated header */
	CHECK(fg_parse(b.data, b.len - 1, &m) == -1);		/* length mismatch */
	memcpy(junk, b.data, b.len);
	fg_wr16(junk, 0xc802);					/* version 2 */
	CHECK(fg_parse(junk, b.len, &m) == -1);
	fg_wr16(junk, 0x4803);					/* T bit clear */
	CHECK(fg_parse(junk, b.len, &m) == -1);
	memcpy(junk, b.data, b.len);
	fg_wr16(junk + FG_HDR_LEN, 0x8000 | 5);			/* AVP length < 6 */
	CHECK(fg_parse(junk, b.len, &m) == -1);
	fg_wr16(junk + FG_HDR_LEN, 0x8000 | 9);			/* AVP overruns message */
	CHECK(fg_parse(junk, b.len, &m) == -1);
	fg_wr16(junk + FG_HDR_LEN, 0xc000 | 8);			/* hidden Message Type */
	CHECK(fg_parse(junk, b.len, &m) == -1);

	fg_build_start(&b, FG_MSG_HELLO);			/* Message Type not first */
	b.len = FG_HDR_LEN;
	fg_put_u32(&b, FG_AVP_CONN_ID, 1, true);
	fg_put_u16(&b, FG_AVP_MSG_TYPE, FG_MSG_HELLO, true);
	fg_write_header(b.data, b.len, 1, 0, 0);
	CHECK(fg_parse(b.data, b.len, &m) == -1);

	fg_build_start(&b, FG_MSG_HELLO);			/* too many AVPs */
	for (int i = 0; i < FG_MAX_AVPS; i++)
		fg_put_avp(&b, 200, NULL, 0, false);
	fg_write_header(b.data, b.len, 1, 0, 0);
	CHECK(fg_parse(b.data, b.len, &m) == -1);

	/* Bounded-memory fuzz: random bytes and mutations must never crash. */
	srand(1);
	for (int i = 0; i < 200000; i++) {
		size_t len = (size_t)rand() % sizeof(junk);

		for (size_t j = 0; j < len; j++)
			junk[j] = (uint8_t)rand();
		if (i & 1) {
			fg_wr16(junk, 0xc803);
			if (len >= 4)
				fg_wr16(junk + 2, (uint16_t)len);
		}
		fg_parse(junk, len, &m);
	}

	/* Duplicate / wrong-width / hidden AVP lookups. */
	fg_build_start(&b, FG_MSG_ICRP);
	fg_put_u32(&b, FG_AVP_LOCAL_SID, 1, true);
	fg_put_u32(&b, FG_AVP_LOCAL_SID, 2, true);
	fg_put_u16(&b, FG_AVP_REMOTE_SID, 3, true);
	raw_avp(&b, 0, FG_AVP_CIRCUIT, "\0\3", 2, true, true);
	fg_write_header(b.data, b.len, 1, 0, 0);
	CHECK(fg_parse(b.data, b.len, &m) == 0);
	uint32_t v32;
	uint16_t v16;
	CHECK(fg_get_u32(&m, 0, FG_AVP_LOCAL_SID, &v32) == -2);
	CHECK(fg_get_u32(&m, 0, FG_AVP_REMOTE_SID, &v32) == -2);
	CHECK(fg_get_u16(&m, 0, FG_AVP_CIRCUIT, &v16) == -2);
	CHECK(fg_get_u16(&m, 0, FG_AVP_RWS, &v16) == -1);
}

static void test_handshake_and_teardown(void)
{
	struct fg_ctrl c;
	struct fg_buf b;
	struct fg_msg m;

	reset(&c, 5);
	establish(&c, 0);
	CHECK(h.downs == 0);
	/* Local stop: StopCCN result 1, then ACK completes the close. */
	fg_ctrl_stop(&c, 1000);
	expect_stopccn(&c, FG_RESULT_CLEAR, 0);
	parse_sent(h.n_sent - 1, &m);
	CHECK(m.ns == 4);
	peer_ack(&c, 1100);
	CHECK(fg_ctrl_finished(&c));
	CHECK(h.downs == 0);

	/* ZLB acknowledgement form is accepted too. */
	reset(&c, 5);
	fg_ctrl_start(&c, 0);
	peer_read();
	build_sccrp(&b, true);
	deliver(&c, &b, 0);
	peer_read();
	b.len = FG_HDR_LEN;
	deliver(&c, &b, 0);
	CHECK(sent_type(h.n_sent - 1) == FG_MSG_ICRQ);
}

static void test_retransmission(void)
{
	struct fg_ctrl c;
	struct fg_msg m;
	struct fg_buf b;
	const int64_t expect[] = { 1000, 3000, 7000, 15000, 23000 };

	reset(&c, 5);
	fg_ctrl_start(&c, 0);
	for (unsigned i = 0; i < 5; i++) {
		CHECK(fg_ctrl_next_deadline(&c) == expect[i]);
		fg_ctrl_timer(&c, expect[i]);
		CHECK(h.n_sent == i + 2 && sent_type(i + 1) == FG_MSG_SCCRQ);
		parse_sent(i + 1, &m);
		CHECK(m.ns == 0);
	}
	CHECK(fg_ctrl_next_deadline(&c) == 31000);
	CHECK(fg_ctrl_cycle_ms(5) == 31000);
	fg_ctrl_timer(&c, 31000);
	CHECK(fg_ctrl_finished(&c) && h.downs == 1 && h.down_reason == FG_R_TIMEOUT);

	/* Retransmission keeps Ns but carries the updated Nr. */
	reset(&c, 5);
	to_icrq(&c, 0);
	parse_sent(h.n_sent - 1, &m);
	CHECK(m.ns == 2 && m.nr == 1);
	fg_build_start(&b, FG_MSG_HELLO);
	fg_write_header(b.data, b.len, LOCAL_CONN, peer.ns++, 2);	/* does not ack ICRQ */
	fg_ctrl_input(&c, b.data, b.len, 10);
	CHECK(sent_type(h.n_sent - 1) == FG_MSG_ACK);
	fg_ctrl_timer(&c, 1000);
	parse_sent(h.n_sent - 1, &m);
	CHECK(m.type == FG_MSG_ICRQ && m.ns == 2 && m.nr == 2);
}

static void test_duplicates_and_order(void)
{
	struct fg_ctrl c;
	struct fg_buf b, icrp;
	struct fg_msg m;
	unsigned sent;

	reset(&c, 5);
	to_icrq(&c, 0);
	build_icrp(&icrp);
	fg_write_header(icrp.data, icrp.len, LOCAL_CONN, peer.ns, peer.nr);
	icrp.data[8] = 0; icrp.data[9] = (uint8_t)(peer.ns + 1);		/* future Ns */
	sent = h.n_sent;
	fg_ctrl_input(&c, icrp.data, icrp.len, 0);
	CHECK(h.n_sent == sent && h.setups == 0);			/* out-of-order: dropped */

	build_icrp(&b);
	deliver(&c, &b, 0);
	CHECK(h.setups == 1);
	/* Same ICRP again (our ACK was lost): re-ACK, never re-process. */
	fg_write_header(b.data, b.len, LOCAL_CONN, (uint16_t)(peer.ns - 1), peer.nr);
	fg_ctrl_input(&c, b.data, b.len, 0);
	CHECK(h.setups == 1);
	parse_sent(h.n_sent - 1, &m);
	CHECK(m.type == FG_MSG_ACK && m.nr == peer.ns);

	/* Nr acknowledging an unsent message: dropped entirely. */
	sent = h.n_sent;
	fg_build_start(&b, FG_MSG_HELLO);
	fg_write_header(b.data, b.len, LOCAL_CONN, peer.ns, (uint16_t)(c.ns_next + 1));
	fg_ctrl_input(&c, b.data, b.len, 0);
	CHECK(h.n_sent == sent && c.nr == peer.ns);

	/* Wrong recipient connection ID: dropped. */
	fg_write_header(b.data, b.len, LOCAL_CONN + 1, peer.ns, peer.nr);
	fg_ctrl_input(&c, b.data, b.len, 0);
	CHECK(h.n_sent == sent);
}

static void test_wraparound_and_hello(void)
{
	struct fg_ctrl c;
	struct fg_buf b;
	struct fg_msg m;

	reset(&c, 5);
	establish(&c, 0);
	/* Move both sequence spaces to the edge of 16-bit wrap. */
	c.ns_next = 0xfffe;
	c.nr = 0xfffe;
	peer.ns = 0xfffe;
	peer.nr = 0xfffe;
	h.seen = h.n_sent;
	for (int i = 0; i < 4; i++) {
		fg_build_start(&b, FG_MSG_HELLO);
		deliver(&c, &b, 100 + i);
		CHECK(sent_type(h.n_sent - 1) == FG_MSG_ACK);
		h.seen = h.n_sent;
	}
	CHECK(c.nr == 2 && peer.ns == 2);
	for (int i = 0; i < 4; i++) {
		int64_t t = fg_ctrl_next_deadline(&c);

		fg_ctrl_timer(&c, t);
		parse_sent(h.n_sent - 1, &m);
		CHECK(m.type == FG_MSG_HELLO && m.ns == (uint16_t)(0xfffe + i));
		peer_ack(&c, t);
		CHECK(!c.inflight);
	}
	CHECK(c.ns_next == 2 && h.downs == 0);

	/* Lost HELLO: reliable delivery gives up and reports a timeout. */
	int64_t t = fg_ctrl_next_deadline(&c);
	fg_ctrl_timer(&c, t);
	CHECK(sent_type(h.n_sent - 1) == FG_MSG_HELLO);
	while (!fg_ctrl_finished(&c))
		fg_ctrl_timer(&c, fg_ctrl_next_deadline(&c));
	CHECK(h.downs == 1 && h.down_reason == FG_R_TIMEOUT);
	CHECK(fg_ctrl_next_deadline(&c) == -1);
}

static void test_peer_teardown(void)
{
	struct fg_ctrl c;
	struct fg_buf b;
	uint8_t rc[2] = { 0, 3 };
	unsigned sent;

	/* Peer CDN closes the session; we close the empty connection. */
	reset(&c, 5);
	establish(&c, 0);
	fg_build_start(&b, FG_MSG_CDN);
	fg_put_avp(&b, FG_AVP_RESULT, rc, 2, true);
	fg_put_u32(&b, FG_AVP_LOCAL_SID, PEER_SID, true);
	fg_put_u32(&b, FG_AVP_REMOTE_SID, LOCAL_SID, true);
	deliver(&c, &b, 10);
	CHECK(h.downs == 1 && h.down_reason == FG_R_PEER_CLOSED);
	expect_stopccn(&c, FG_RESULT_CLEAR, 0);
	/* Bounded: unanswered StopCCN finishes after one retransmission cycle. */
	while (!fg_ctrl_finished(&c))
		fg_ctrl_timer(&c, fg_ctrl_next_deadline(&c));
	CHECK(h.downs == 1);

	/* Peer StopCCN: ACK, linger and re-ACK its retransmission. */
	reset(&c, 5);
	establish(&c, 0);
	fg_build_start(&b, FG_MSG_STOPCCN);
	fg_put_avp(&b, FG_AVP_RESULT, rc, 2, true);
	fg_put_u32(&b, FG_AVP_CONN_ID, PEER_CONN, true);
	deliver(&c, &b, 10);
	CHECK(h.downs == 1 && h.down_reason == FG_R_PEER_CLOSED);
	CHECK(sent_type(h.n_sent - 1) == FG_MSG_ACK && c.state == FG_CTRL_LINGER);
	sent = h.n_sent;
	fg_write_header(b.data, b.len, LOCAL_CONN, (uint16_t)(peer.ns - 1), peer.nr);
	fg_ctrl_input(&c, b.data, b.len, 20);
	CHECK(h.n_sent == sent + 1 && sent_type(sent) == FG_MSG_ACK);
	fg_ctrl_timer(&c, 10 + 31000);
	CHECK(fg_ctrl_finished(&c));

	/* StopCCN while still negotiating counts as rejection. */
	reset(&c, 5);
	fg_ctrl_start(&c, 0);
	peer_read();
	deliver(&c, &b, 10);
	CHECK(h.down_reason == FG_R_PEER_REJECTED);
}

static void test_sli_and_peer_calls(void)
{
	struct fg_ctrl c;
	struct fg_buf b;
	struct fg_msg m;
	uint32_t id;

	reset(&c, 5);
	establish(&c, 0);
	fg_build_start(&b, FG_MSG_SLI);
	fg_put_u32(&b, FG_AVP_LOCAL_SID, PEER_SID, true);
	fg_put_u32(&b, FG_AVP_REMOTE_SID, LOCAL_SID, true);
	fg_put_u16(&b, FG_AVP_CIRCUIT, 0, true);
	deliver(&c, &b, 0);
	CHECK(h.circuit_events == 1 && !h.circuit_active && !c.circuit_active);
	fg_build_start(&b, FG_MSG_SLI);
	fg_put_u32(&b, FG_AVP_LOCAL_SID, PEER_SID, true);
	fg_put_u32(&b, FG_AVP_REMOTE_SID, LOCAL_SID, true);
	fg_put_u16(&b, FG_AVP_CIRCUIT, FG_CIRCUIT_ACTIVE, true);
	deliver(&c, &b, 0);
	CHECK(h.circuit_events == 2 && h.circuit_active);

	/* Peer-initiated session: rejected with CDN, no second session. */
	fg_build_start(&b, FG_MSG_ICRQ);
	fg_put_u32(&b, FG_AVP_LOCAL_SID, 5555, true);
	fg_put_u32(&b, FG_AVP_REMOTE_SID, 0, true);
	deliver(&c, &b, 0);
	parse_sent(h.n_sent - 1, &m);
	CHECK(m.type == FG_MSG_CDN);
	CHECK(fg_get_u32(&m, 0, FG_AVP_REMOTE_SID, &id) == 0 && id == 5555);
	CHECK(h.setups == 1 && h.downs == 0 && c.state == FG_CTRL_ESTABLISHED);
}

typedef void (*mutate_fn)(struct fg_buf *b);

static void icrp_cookie(struct fg_buf *b) { fg_put_u32(b, FG_AVP_COOKIE, 0x12345678, true); }
static void icrp_seq(struct fg_buf *b) { b->len -= 8; fg_put_u16(b, FG_AVP_DATA_SEQ, 2, true); }
static void icrp_offset(struct fg_buf *b) { raw_avp(b, FG_VENDOR_AVM, FG_AVM_OFFSET, "\0\4", 2, true, false); }
static void icrp_unknown_m(struct fg_buf *b) { raw_avp(b, 9, 7, "\0\5", 2, true, false); }
static void icrp_unknown_opt(struct fg_buf *b) { raw_avp(b, 9, 7, "\0\5", 2, false, false); }
static void icrp_hidden(struct fg_buf *b) { raw_avp(b, 0, 74, "\0\0\0\0\0\0\0\0", 8, false, true); }
static void icrp_wrong_sid(struct fg_buf *b)
{
	fg_build_start(b, FG_MSG_ICRP);
	fg_put_u32(b, FG_AVP_LOCAL_SID, PEER_SID, true);
	fg_put_u32(b, FG_AVP_REMOTE_SID, LOCAL_SID + 1, true);
	fg_put_u16(b, FG_AVP_CIRCUIT, 3, true);
	fg_put_u16(b, FG_AVP_L2SPEC, 1, true);
}
static void icrp_no_l2spec(struct fg_buf *b)
{
	fg_build_start(b, FG_MSG_ICRP);
	fg_put_u32(b, FG_AVP_LOCAL_SID, PEER_SID, true);
	fg_put_u32(b, FG_AVP_REMOTE_SID, LOCAL_SID, true);
	fg_put_u16(b, FG_AVP_CIRCUIT, 3, true);
}

static void expect_icrp_result(mutate_fn fn, enum fg_reason reason, uint16_t error)
{
	struct fg_ctrl c;
	struct fg_buf b;

	reset(&c, 5);
	to_icrq(&c, 0);
	build_icrp(&b);
	fn(&b);
	deliver(&c, &b, 0);
	if (reason == FG_R_NONE) {
		CHECK(h.downs == 0 && h.setups == 1);
		return;
	}
	CHECK(h.setups == 0 && h.downs == 1 && h.down_reason == reason);
	expect_stopccn(&c, FG_RESULT_GENERAL, error);
}

static void test_rejections(void)
{
	struct fg_ctrl c;
	struct fg_buf b;

	expect_icrp_result(icrp_cookie, FG_R_UNSUPPORTED_PEER, FG_ERR_RANGE);
	expect_icrp_result(icrp_seq, FG_R_UNSUPPORTED_PEER, FG_ERR_RANGE);
	expect_icrp_result(icrp_offset, FG_R_UNSUPPORTED_PEER, FG_ERR_RANGE);
	expect_icrp_result(icrp_no_l2spec, FG_R_UNSUPPORTED_PEER, FG_ERR_RANGE);
	expect_icrp_result(icrp_wrong_sid, FG_R_PROTOCOL_ERROR, FG_ERR_RANGE);
	expect_icrp_result(icrp_unknown_m, FG_R_UNSUPPORTED_PEER, FG_ERR_UNKNOWN_MANDATORY);
	expect_icrp_result(icrp_hidden, FG_R_UNSUPPORTED_PEER, FG_ERR_VENDOR);
	expect_icrp_result(icrp_unknown_opt, FG_R_NONE, 0);

	/* SCCRP without Ethernet capability. */
	reset(&c, 5);
	fg_ctrl_start(&c, 0);
	peer_read();
	build_sccrp(&b, false);
	deliver(&c, &b, 0);
	CHECK(h.down_reason == FG_R_UNSUPPORTED_PEER);
	expect_stopccn(&c, FG_RESULT_GENERAL, FG_ERR_RANGE);
	CHECK(fg_rd32(h.sent[h.n_sent - 1].data + 4) == PEER_CONN);

	/* Peer demanding control message authentication. */
	reset(&c, 5);
	fg_ctrl_start(&c, 0);
	peer_read();
	build_sccrp(&b, true);
	fg_put_avp(&b, FG_AVP_NONCE, "0123456789abcdef", 16, true);
	deliver(&c, &b, 0);
	CHECK(h.down_reason == FG_R_UNSUPPORTED_PEER && c.state == FG_CTRL_STOPPING);

	/* Kernel setup failure tears the connection down. */
	reset(&c, 5);
	h.setup_result = -1;
	to_icrq(&c, 0);
	h.setup_result = -1;
	build_icrp(&b);
	deliver(&c, &b, 0);
	CHECK(h.down_reason == FG_R_KERNEL_ERROR);

	/* Message illegal in the current state. */
	reset(&c, 5);
	establish(&c, 0);
	build_sccrp(&b, true);
	deliver(&c, &b, 0);
	CHECK(h.down_reason == FG_R_PROTOCOL_ERROR && c.state == FG_CTRL_STOPPING);
}

static const char *const *opts_kv;

static const char *opt_get(void *ctx, const char *name, bool *is_list)
{
	(void)ctx;
	*is_list = false;
	for (const char *const *kv = opts_kv; *kv; kv += 2) {
		if (strcmp(kv[0], name))
			continue;
		if (!kv[1])
			*is_list = true;
		return kv[1];
	}
	return NULL;
}

static int parse_cfg(struct fg_config *cfg, const char *const *kv)
{
	char err[160];

	opts_kv = kv;
	return fg_config_parse(cfg, opt_get, NULL, err, sizeof(err));
}

static void test_config(void)
{
	struct fg_config cfg, other;

	CHECK(parse_cfg(&cfg, (const char *const[]){ "enabled", "0", "bridge", "", NULL }) == 0);
	CHECK(!cfg.enabled && cfg.mtu == 1500 && cfg.hello_interval == 30 && !strcmp(cfg.uplink, "lan"));
	CHECK(parse_cfg(&cfg, (const char *const[]){ "enabled", "1", "server", "192.168.178.1",
		"bridge", "br-guest", "mtu", "1280", "retry_max", "10", NULL }) == 0);
	CHECK(cfg.enabled && cfg.mtu == 1280 && cfg.retry_max == 10 && !strcmp(cfg.server_str, "192.168.178.1"));
	CHECK(cfg.server == inet_addr("192.168.178.1"));
	CHECK(parse_cfg(&other, (const char *const[]){ "enabled", "1", "server", "192.168.178.1",
		"bridge", "br-guest", "mtu", "1280", "retry_max", "10", "future_option", "x", NULL }) == 0);
	CHECK(fg_config_equal(&cfg, &other));

	static const char *const bad[][7] = {
		{ "enabled", "yes", NULL },
		{ "enabled", "1", "bridge", "br-guest", NULL },
		{ "enabled", "1", "server", "192.168.178.1", NULL },
		{ "server", "fritz.box", NULL },
		{ "server", "224.0.0.1", NULL },
		{ "server", "255.255.255.255", NULL },
		{ "server", "0.0.0.0", NULL },
		{ "server", "127.0.0.1", NULL },
		{ "server", "192.168.178.1 ", NULL },
		{ "uplink", "lan;rm", NULL },
		{ "bridge", "fritzguest0", NULL },
		{ "bridge", "br/guest", NULL },
		{ "bridge", "a-very-long-bridge-name", NULL },
		{ "mtu", "1501", NULL },
		{ "mtu", "-1", NULL },
		{ "mtu", "1500x", NULL },
		{ "hello_interval", "9", NULL },
		{ "retry_max", "0", NULL },
		{ "reconnect_max", "301", NULL },
		{ "server", NULL, NULL },	/* list option */
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
		CHECK(parse_cfg(&cfg, bad[i]) == -1);
}

int main(void)
{
	test_outgoing_bytes();
	test_malformed();
	test_handshake_and_teardown();
	test_retransmission();
	test_duplicates_and_order();
	test_wraparound_and_hello();
	test_peer_teardown();
	test_sli_and_peer_calls();
	test_rejections();
	test_config();
	printf("protocol-test: %u checks passed\n", checks);
	return 0;
}
