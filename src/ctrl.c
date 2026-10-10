/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <string.h>

#include "ctrl.h"
#include "log.h"

#define FG_DEFAULT_PEER_RWS 4
#define FG_ERR_RESOURCES 4

const char *fg_reason_name(enum fg_reason r)
{
	switch (r) {
	case FG_R_TIMEOUT: return "timeout";
	case FG_R_PEER_REJECTED: return "peer_rejected";
	case FG_R_PEER_CLOSED: return "peer_closed";
	case FG_R_UNSUPPORTED_PEER: return "unsupported_peer";
	case FG_R_PROTOCOL_ERROR: return "protocol_error";
	case FG_R_KERNEL_ERROR: return "kernel_error";
	default: return "";
	}
}

unsigned fg_ctrl_cycle_ms(unsigned retry_max)
{
	unsigned total = 0, step = FG_RTX_INITIAL_MS;

	for (unsigned i = 0; i <= retry_max; i++) {
		total += step;
		step = step * 2 > FG_RTX_MAX_MS ? FG_RTX_MAX_MS : step * 2;
	}
	return total;
}

void fg_ctrl_init(struct fg_ctrl *c, const struct fg_ctrl_params *p,
		  const struct fg_ctrl_ops *ops, void *ctx)
{
	memset(c, 0, sizeof(*c));
	c->p = *p;
	c->ops = ops;
	c->ctx = ctx;
	c->peer_rws = FG_DEFAULT_PEER_RWS;
}

static void transmit(struct fg_ctrl *c, struct fg_queued *m, uint16_t ns)
{
	fg_write_header(m->data, m->len, c->peer_conn_id, ns, c->nr);
	c->tx_flag = true;
	c->ops->send(c->ctx, m->data, m->len);
}

static void send_ack(struct fg_ctrl *c)
{
	struct fg_buf b;

	fg_build_start(&b, FG_MSG_ACK);
	fg_write_header(b.data, b.len, c->peer_conn_id, c->ns_next, c->nr);
	FG_DBG("TX ACK Ns=%u Nr=%u", c->ns_next, c->nr);
	c->ops->send(c->ctx, b.data, b.len);
}

/* One message in flight satisfies every peer window (RWS >= 1). */
static void kick(struct fg_ctrl *c, int64_t now)
{
	struct fg_queued *m;

	if (c->inflight || !c->q_count ||
	    c->state == FG_CTRL_CLOSED || c->state == FG_CTRL_LINGER)
		return;
	m = &c->q[c->q_head];
	c->inflight = true;
	c->inflight_ns = c->ns_next++;
	c->retries = 0;
	c->rtx_ms = FG_RTX_INITIAL_MS;
	c->rtx_deadline = now + c->rtx_ms;
	FG_DBG("TX %s Ns=%u Nr=%u", fg_msg_name(m->type), c->inflight_ns, c->nr);
	transmit(c, m, c->inflight_ns);
}

static int enqueue(struct fg_ctrl *c, const struct fg_buf *b, uint16_t type)
{
	struct fg_queued *m;

	if (b->overflow || c->q_count >= FG_QUEUE_LEN)
		return -1;
	m = &c->q[(c->q_head + c->q_count) % FG_QUEUE_LEN];
	memcpy(m->data, b->data, b->len);
	m->len = b->len;
	m->type = type;
	c->q_count++;
	return 0;
}

static void clear_queue(struct fg_ctrl *c)
{
	c->q_count = 0;
	c->inflight = false;
}

static void report_down(struct fg_ctrl *c, enum fg_reason r, const char *detail)
{
	if (c->down_reported)
		return;
	c->down_reported = true;
	fg_log(LOG_NOTICE, "control connection %u down (%s): %s",
	       c->p.conn_id, fg_reason_name(r), detail);
	c->ops->down(c->ctx, r, detail);
}

static void queue_stopccn(struct fg_ctrl *c, int64_t now, uint16_t result, uint16_t error)
{
	struct fg_buf b;
	uint8_t rc[4];

	fg_wr16(rc, result);
	fg_wr16(rc + 2, error);
	fg_build_start(&b, FG_MSG_STOPCCN);
	fg_put_avp(&b, FG_AVP_RESULT, rc, result == FG_RESULT_GENERAL ? 4 : 2, true);
	fg_put_u32(&b, FG_AVP_CONN_ID, c->p.conn_id, true);
	/* Keep only an in-flight message: its Ns is already on the wire. */
	c->q_count = c->inflight ? 1 : 0;
	enqueue(c, &b, FG_MSG_STOPCCN);
	c->state = FG_CTRL_STOPPING;
	c->end_deadline = now + fg_ctrl_cycle_ms(c->p.retry_max);
	kick(c, now);
}

static void fail(struct fg_ctrl *c, int64_t now, enum fg_reason r, uint16_t error,
		 const char *detail)
{
	report_down(c, r, detail);
	if (c->state == FG_CTRL_ESTABLISHED || c->state == FG_CTRL_WAIT_SCCRP)
		queue_stopccn(c, now, FG_RESULT_GENERAL, error);
}

static void acked(struct fg_ctrl *c)
{
	uint16_t type = c->q[c->q_head].type;

	c->inflight = false;
	c->q_head = (c->q_head + 1) % FG_QUEUE_LEN;
	c->q_count--;
	FG_DBG("%s acknowledged", fg_msg_name(type));

	if (type == FG_MSG_STOPCCN) {
		c->state = FG_CTRL_CLOSED;
		clear_queue(c);
	} else if (type == FG_MSG_ICCN && c->sess == FG_SESS_WAIT_ICCN_ACK &&
		   c->state == FG_CTRL_ESTABLISHED) {
		c->sess = FG_SESS_ESTABLISHED;
		fg_log(LOG_INFO, "session %u/%u established", c->p.session_id, c->peer_session_id);
		c->ops->session_up(c->ctx, c->circuit_active);
	}
}

static uint16_t result_code(const struct fg_msg *m, uint16_t *error)
{
	bool dup;
	const struct fg_avp *a = fg_find(m, FG_VENDOR_IETF, FG_AVP_RESULT, &dup);

	*error = a && a->len >= 4 ? fg_rd16(a->val + 2) : 0;
	return a && a->len >= 2 ? fg_rd16(a->val) : 0;
}

static void handle_sccrp(struct fg_ctrl *c, const struct fg_msg *m, int64_t now)
{
	const struct fg_avp *a;
	uint32_t router_id;
	uint16_t rws = FG_DEFAULT_PEER_RWS;
	bool dup, ethernet = false;
	int ret;

	if (!c->peer_conn_id) {
		fail(c, now, FG_R_PROTOCOL_ERROR, FG_ERR_RANGE,
		     "SCCRP without valid Assigned Control Connection ID");
		return;
	}
	a = fg_find(m, FG_VENDOR_IETF, FG_AVP_HOSTNAME, &dup);
	if (!a || dup || !a->len || fg_get_u32(m, FG_VENDOR_IETF, FG_AVP_ROUTER_ID, &router_id)) {
		fail(c, now, FG_R_PROTOCOL_ERROR, FG_ERR_LENGTH, "SCCRP without valid Host Name/Router ID");
		return;
	}
	a = fg_find(m, FG_VENDOR_IETF, FG_AVP_PW_CAPS, &dup);
	if (!a || dup || a->len % 2) {
		fail(c, now, FG_R_PROTOCOL_ERROR, FG_ERR_LENGTH, "SCCRP without valid PW Capabilities List");
		return;
	}
	for (unsigned i = 0; i < a->len; i += 2)
		if (fg_rd16(a->val + i) == FG_PW_ETHERNET)
			ethernet = true;
	if (!ethernet) {
		fail(c, now, FG_R_UNSUPPORTED_PEER, FG_ERR_RANGE, "peer does not offer Ethernet pseudowires");
		return;
	}
	ret = fg_get_u16(m, FG_VENDOR_IETF, FG_AVP_RWS, &rws);
	if (ret == -2 || !rws) {
		fail(c, now, FG_R_PROTOCOL_ERROR, FG_ERR_RANGE, "SCCRP with invalid Receive Window Size");
		return;
	}
	c->peer_rws = rws;
	c->state = FG_CTRL_ESTABLISHED;
	fg_log(LOG_INFO, "control connection %u/%u established", c->p.conn_id, c->peer_conn_id);

	struct fg_buf b;
	fg_build_start(&b, FG_MSG_SCCCN);
	enqueue(c, &b, FG_MSG_SCCCN);

	fg_build_start(&b, FG_MSG_ICRQ);
	fg_put_u32(&b, FG_AVP_LOCAL_SID, c->p.session_id, true);
	fg_put_u32(&b, FG_AVP_REMOTE_SID, 0, true);
	fg_put_u32(&b, FG_AVP_SERIAL, c->p.serial, false);
	fg_put_u16(&b, FG_AVP_PW_TYPE, FG_PW_ETHERNET, true);
	fg_put_avp(&b, FG_AVP_REMOTE_END, FG_REMOTE_END_ID, strlen(FG_REMOTE_END_ID), true);
	fg_put_u16(&b, FG_AVP_CIRCUIT, FG_CIRCUIT_ACTIVE | FG_CIRCUIT_NEW, true);
	fg_put_u16(&b, FG_AVP_L2SPEC, FG_L2SPEC_DEFAULT, true);
	fg_put_u16(&b, FG_AVP_DATA_SEQ, 0, true);
	enqueue(c, &b, FG_MSG_ICRQ);
	c->sess = FG_SESS_WAIT_ICRP;
}

static void handle_stopccn(struct fg_ctrl *c, const struct fg_msg *m, int64_t now)
{
	char detail[64];
	uint16_t error, result = result_code(m, &error);

	snprintf(detail, sizeof(detail), "peer sent StopCCN (result %u, error %u)", result, error);
	report_down(c, c->sess == FG_SESS_ESTABLISHED ? FG_R_PEER_CLOSED : FG_R_PEER_REJECTED, detail);
	clear_queue(c);
	/* RFC 3931 3.3.2: keep state to re-acknowledge retransmissions. */
	c->state = FG_CTRL_LINGER;
	c->end_deadline = now + fg_ctrl_cycle_ms(c->p.retry_max);
}

static void handle_icrp(struct fg_ctrl *c, const struct fg_msg *m, int64_t now)
{
	const struct fg_avp *a;
	uint32_t lsid, rsid;
	uint16_t circuit, l2spec, seq, offset;
	bool dup;
	int ret;

	if (c->sess != FG_SESS_WAIT_ICRP) {
		fail(c, now, FG_R_PROTOCOL_ERROR, FG_ERR_NONE, "unexpected ICRP");
		return;
	}
	if (fg_get_u32(m, FG_VENDOR_IETF, FG_AVP_LOCAL_SID, &lsid) || !lsid ||
	    fg_get_u32(m, FG_VENDOR_IETF, FG_AVP_REMOTE_SID, &rsid) || rsid != c->p.session_id) {
		fail(c, now, FG_R_PROTOCOL_ERROR, FG_ERR_RANGE, "ICRP with invalid session IDs");
		return;
	}
	if (fg_get_u16(m, FG_VENDOR_IETF, FG_AVP_CIRCUIT, &circuit)) {
		fail(c, now, FG_R_PROTOCOL_ERROR, FG_ERR_LENGTH, "ICRP without valid Circuit Status");
		return;
	}
	a = fg_find(m, FG_VENDOR_IETF, FG_AVP_COOKIE, &dup);
	if (dup || (a && a->len)) {
		fail(c, now, FG_R_UNSUPPORTED_PEER, FG_ERR_RANGE, "peer assigned a cookie (unsupported)");
		return;
	}
	/* RFC 3931 5.4.4: an absent AVP means no sublayer, not the default one. */
	ret = fg_get_u16(m, FG_VENDOR_IETF, FG_AVP_L2SPEC, &l2spec);
	if (ret || l2spec != FG_L2SPEC_DEFAULT) {
		fail(c, now, ret == -2 ? FG_R_PROTOCOL_ERROR : FG_R_UNSUPPORTED_PEER, FG_ERR_RANGE,
		     "peer did not request the default L2-Specific Sublayer");
		return;
	}
	ret = fg_get_u16(m, FG_VENDOR_IETF, FG_AVP_DATA_SEQ, &seq);
	if (ret == -2 || (!ret && seq)) {
		fail(c, now, ret == -2 ? FG_R_PROTOCOL_ERROR : FG_R_UNSUPPORTED_PEER, FG_ERR_RANGE,
		     "peer requires data sequencing (unsupported)");
		return;
	}
	ret = fg_get_u16(m, FG_VENDOR_AVM, FG_AVM_OFFSET, &offset);
	if (ret == -2 || (!ret && offset)) {
		fail(c, now, ret == -2 ? FG_R_PROTOCOL_ERROR : FG_R_UNSUPPORTED_PEER, FG_ERR_RANGE,
		     "peer requires a nonzero AVM payload offset (unsupported)");
		return;
	}

	c->peer_session_id = lsid;
	c->circuit_active = circuit & FG_CIRCUIT_ACTIVE;
	if (c->ops->session_setup(c->ctx, c->peer_conn_id, lsid)) {
		fail(c, now, FG_R_KERNEL_ERROR, FG_ERR_RESOURCES, "kernel session setup failed");
		return;
	}

	struct fg_buf b;
	fg_build_start(&b, FG_MSG_ICCN);
	fg_put_u32(&b, FG_AVP_LOCAL_SID, c->p.session_id, true);
	fg_put_u32(&b, FG_AVP_REMOTE_SID, lsid, true);
	fg_put_u16(&b, FG_AVP_CIRCUIT, FG_CIRCUIT_ACTIVE | FG_CIRCUIT_NEW, true);
	enqueue(c, &b, FG_MSG_ICCN);
	c->sess = FG_SESS_WAIT_ICCN_ACK;
}

static void handle_cdn(struct fg_ctrl *c, const struct fg_msg *m, int64_t now)
{
	char detail[64];
	uint32_t lsid, rsid;
	uint16_t error, result;
	enum fg_reason reason;

	if (fg_get_u32(m, FG_VENDOR_IETF, FG_AVP_REMOTE_SID, &rsid) ||
	    fg_get_u32(m, FG_VENDOR_IETF, FG_AVP_LOCAL_SID, &lsid)) {
		fail(c, now, FG_R_PROTOCOL_ERROR, FG_ERR_LENGTH, "malformed CDN");
		return;
	}
	if (c->sess == FG_SESS_NONE || rsid != c->p.session_id) {
		FG_DBG("ignoring CDN for unknown session %u", rsid);
		return;
	}
	result = result_code(m, &error);
	snprintf(detail, sizeof(detail), "peer sent CDN (result %u, error %u)", result, error);
	reason = c->sess == FG_SESS_ESTABLISHED ? FG_R_PEER_CLOSED : FG_R_PEER_REJECTED;
	c->sess = FG_SESS_NONE;
	report_down(c, reason, detail);
	/* Only one session per connection is supported; close the now empty connection. */
	queue_stopccn(c, now, FG_RESULT_CLEAR, FG_ERR_NONE);
}

static void handle_sli(struct fg_ctrl *c, const struct fg_msg *m, int64_t now)
{
	uint32_t rsid;
	uint16_t circuit;
	bool active;
	int ret;

	if (fg_get_u32(m, FG_VENDOR_IETF, FG_AVP_REMOTE_SID, &rsid)) {
		fail(c, now, FG_R_PROTOCOL_ERROR, FG_ERR_LENGTH, "malformed SLI");
		return;
	}
	if (c->sess == FG_SESS_NONE || rsid != c->p.session_id)
		return;
	ret = fg_get_u16(m, FG_VENDOR_IETF, FG_AVP_CIRCUIT, &circuit);
	if (ret == -2) {
		fail(c, now, FG_R_PROTOCOL_ERROR, FG_ERR_LENGTH, "malformed SLI Circuit Status");
		return;
	}
	if (ret)
		return;
	active = circuit & FG_CIRCUIT_ACTIVE;
	if (active == c->circuit_active)
		return;
	c->circuit_active = active;
	fg_log(LOG_NOTICE, "peer marked guest circuit %s", active ? "active" : "inactive");
	if (c->sess == FG_SESS_ESTABLISHED)
		c->ops->circuit(c->ctx, active);
}

static void reject_call(struct fg_ctrl *c, const struct fg_msg *m, int64_t now)
{
	struct fg_buf b;
	uint8_t rc[2];
	uint32_t lsid;

	if (fg_get_u32(m, FG_VENDOR_IETF, FG_AVP_LOCAL_SID, &lsid) || !lsid) {
		fail(c, now, FG_R_PROTOCOL_ERROR, FG_ERR_LENGTH, "malformed peer call request");
		return;
	}
	fg_log_rl(LOG_NOTICE, "rejecting peer-initiated %s", fg_msg_name(m->type));
	fg_wr16(rc, FG_CDN_PERMANENT);
	fg_build_start(&b, FG_MSG_CDN);
	fg_put_avp(&b, FG_AVP_RESULT, rc, sizeof(rc), true);
	/* No local session is allocated for a rejected request. */
	fg_put_u32(&b, FG_AVP_LOCAL_SID, 0, true);
	fg_put_u32(&b, FG_AVP_REMOTE_SID, lsid, true);
	if (enqueue(c, &b, FG_MSG_CDN))
		fail(c, now, FG_R_PROTOCOL_ERROR, FG_ERR_RESOURCES, "control send queue full");
}

static bool screen_avps(struct fg_ctrl *c, const struct fg_msg *m, int64_t now)
{
	char detail[64];

	for (unsigned i = 1; i < m->n_avps; i++) {
		const struct fg_avp *a = &m->avps[i];

		if (a->hidden) {
			fail(c, now, FG_R_UNSUPPORTED_PEER, FG_ERR_VENDOR,
			     "peer uses hidden AVPs (shared secret unsupported)");
			return false;
		}
		if (a->vendor == FG_VENDOR_IETF &&
		    (a->type == FG_AVP_MSG_DIGEST || a->type == FG_AVP_NONCE)) {
			fail(c, now, FG_R_UNSUPPORTED_PEER, FG_ERR_VENDOR,
			     "peer requires control message authentication (unsupported)");
			return false;
		}
		if (a->mandatory && !fg_avp_recognized(a)) {
			snprintf(detail, sizeof(detail), "unknown mandatory AVP %u/%u in %s",
				 a->vendor, a->type, fg_msg_name(m->type));
			fail(c, now, FG_R_UNSUPPORTED_PEER, FG_ERR_UNKNOWN_MANDATORY, detail);
			return false;
		}
	}
	return true;
}

static void handle(struct fg_ctrl *c, const struct fg_msg *m, int64_t now)
{
	char detail[64];

	FG_DBG("RX %s Ns=%u Nr=%u", fg_msg_name(m->type), m->ns, m->nr);
	if (c->state == FG_CTRL_STOPPING || c->state == FG_CTRL_CLOSED) {
		if (m->type_vendor == FG_VENDOR_IETF && m->type == FG_MSG_STOPCCN)
			c->state = FG_CTRL_CLOSED;
		return;
	}
	if (m->type_vendor != FG_VENDOR_IETF) {
		if (m->type_mandatory)
			fail(c, now, FG_R_UNSUPPORTED_PEER, FG_ERR_VENDOR, "unknown mandatory vendor message");
		return;
	}
	if (m->type == FG_MSG_SCCRP && c->state == FG_CTRL_WAIT_SCCRP) {
		uint32_t id;

		if (!fg_get_u32(m, FG_VENDOR_IETF, FG_AVP_CONN_ID, &id))
			c->peer_conn_id = id;
	}
	if (m->type != FG_MSG_STOPCCN && !screen_avps(c, m, now))
		return;

	switch (m->type) {
	case FG_MSG_SCCRP:
		if (c->state == FG_CTRL_WAIT_SCCRP) {
			handle_sccrp(c, m, now);
			return;
		}
		break;
	case FG_MSG_STOPCCN:
		handle_stopccn(c, m, now);
		return;
	case FG_MSG_HELLO:
	case FG_MSG_WEN:
		if (c->state == FG_CTRL_ESTABLISHED)
			return;
		break;
	case FG_MSG_ICRP:
		if (c->state == FG_CTRL_ESTABLISHED) {
			handle_icrp(c, m, now);
			return;
		}
		break;
	case FG_MSG_CDN:
		if (c->state == FG_CTRL_ESTABLISHED) {
			handle_cdn(c, m, now);
			return;
		}
		break;
	case FG_MSG_SLI:
		if (c->state == FG_CTRL_ESTABLISHED) {
			handle_sli(c, m, now);
			return;
		}
		break;
	case FG_MSG_ICRQ:
	case FG_MSG_OCRQ:
		if (c->state == FG_CTRL_ESTABLISHED) {
			reject_call(c, m, now);
			return;
		}
		break;
	case FG_MSG_SCCRQ:
	case FG_MSG_SCCCN:
	case FG_MSG_OCRP:
	case FG_MSG_OCCN:
	case FG_MSG_ICCN:
		break;
	default:
		if (m->type_mandatory)
			break;
		return;
	}
	snprintf(detail, sizeof(detail), "unexpected %s (type %u) in current state",
		 fg_msg_name(m->type), m->type);
	fail(c, now, FG_R_PROTOCOL_ERROR, FG_ERR_NONE, detail);
}

void fg_ctrl_start(struct fg_ctrl *c, int64_t now)
{
	struct fg_buf b;
	const char *host = c->p.hostname && *c->p.hostname ? c->p.hostname : "OpenWrt";
	size_t hlen = strnlen(host, 64);

	c->state = FG_CTRL_WAIT_SCCRP;
	c->last_rx = now;
	fg_build_start(&b, FG_MSG_SCCRQ);
	fg_put_avp(&b, FG_AVP_HOSTNAME, host, hlen, true);
	fg_put_u32(&b, FG_AVP_ROUTER_ID, c->p.router_id, true);
	fg_put_u32(&b, FG_AVP_CONN_ID, c->p.conn_id, true);
	fg_put_u16(&b, FG_AVP_PW_CAPS, FG_PW_ETHERNET, true);
	fg_put_u16(&b, FG_AVP_RWS, 1, true);
	enqueue(c, &b, FG_MSG_SCCRQ);
	kick(c, now);
}

void fg_ctrl_input(struct fg_ctrl *c, const uint8_t *buf, size_t len, int64_t now)
{
	struct fg_msg m;

	if (c->state == FG_CTRL_IDLE || c->state == FG_CTRL_CLOSED)
		return;
	if (fg_parse(buf, len, &m)) {
		fg_log_rl(LOG_WARNING, "dropped malformed control message (%zu bytes)", len);
		return;
	}
	if (m.conn_id != c->p.conn_id) {
		fg_log_rl(LOG_WARNING, "dropped control message for connection %u", m.conn_id);
		return;
	}
	if (fg_seq_lt(c->ns_next, m.nr)) {
		fg_log_rl(LOG_WARNING, "dropped control message acknowledging unsent Nr=%u", m.nr);
		return;
	}
	c->last_rx = now;
	if (c->inflight && fg_seq_lt(c->inflight_ns, m.nr))
		acked(c);

	if (m.zlb || (m.type_vendor == FG_VENDOR_IETF && m.type == FG_MSG_ACK)) {
		kick(c, now);
		return;
	}
	if (m.ns != c->nr) {
		if (fg_seq_lt(m.ns, c->nr)) {
			FG_DBG("duplicate Ns=%u (expected %u)", m.ns, c->nr);
			send_ack(c);
		} else {
			fg_log_rl(LOG_INFO, "dropped out-of-order Ns=%u (expected %u)", m.ns, c->nr);
		}
		return;
	}
	c->nr++;
	c->tx_flag = false;
	if (c->state != FG_CTRL_LINGER)
		handle(c, &m, now);
	kick(c, now);
	if (!c->tx_flag)
		send_ack(c);
}

void fg_ctrl_timer(struct fg_ctrl *c, int64_t now)
{
	if (c->state == FG_CTRL_IDLE || c->state == FG_CTRL_CLOSED)
		return;
	if ((c->state == FG_CTRL_STOPPING || c->state == FG_CTRL_LINGER) && now >= c->end_deadline) {
		c->state = FG_CTRL_CLOSED;
		clear_queue(c);
		return;
	}
	if (c->inflight && now >= c->rtx_deadline) {
		struct fg_queued *m = &c->q[c->q_head];

		if (c->retries >= c->p.retry_max) {
			char detail[64];

			snprintf(detail, sizeof(detail), "%s not acknowledged after %u retransmissions",
				 fg_msg_name(m->type), c->retries);
			if (c->state != FG_CTRL_STOPPING)
				report_down(c, FG_R_TIMEOUT, detail);
			c->state = FG_CTRL_CLOSED;
			clear_queue(c);
			return;
		}
		c->retries++;
		c->rtx_ms = c->rtx_ms * 2 > FG_RTX_MAX_MS ? FG_RTX_MAX_MS : c->rtx_ms * 2;
		c->rtx_deadline = now + c->rtx_ms;
		FG_DBG("retransmit %s Ns=%u Nr=%u (%u)", fg_msg_name(m->type), c->inflight_ns, c->nr, c->retries);
		transmit(c, m, c->inflight_ns);
	}
	if (c->state == FG_CTRL_ESTABLISHED && !c->inflight && !c->q_count &&
	    now - c->last_rx >= (int64_t)c->p.hello_ms) {
		struct fg_buf b;

		fg_build_start(&b, FG_MSG_HELLO);
		enqueue(c, &b, FG_MSG_HELLO);
		kick(c, now);
	}
}

void fg_ctrl_stop(struct fg_ctrl *c, int64_t now)
{
	c->down_reported = true;
	switch (c->state) {
	case FG_CTRL_IDLE:
	case FG_CTRL_WAIT_SCCRP:
		c->state = FG_CTRL_CLOSED;
		clear_queue(c);
		break;
	case FG_CTRL_ESTABLISHED:
		queue_stopccn(c, now, FG_RESULT_CLEAR, FG_ERR_NONE);
		break;
	default:
		break;
	}
}

int64_t fg_ctrl_next_deadline(const struct fg_ctrl *c)
{
	int64_t d = -1;

	if (c->state == FG_CTRL_IDLE || c->state == FG_CTRL_CLOSED)
		return -1;
	if (c->inflight)
		d = c->rtx_deadline;
	if (c->state == FG_CTRL_STOPPING || c->state == FG_CTRL_LINGER)
		d = d < 0 || c->end_deadline < d ? c->end_deadline : d;
	if (c->state == FG_CTRL_ESTABLISHED && !c->inflight && !c->q_count) {
		int64_t h = c->last_rx + c->p.hello_ms;

		d = d < 0 || h < d ? h : d;
	}
	return d;
}
