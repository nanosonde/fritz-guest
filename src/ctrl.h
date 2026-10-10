/* SPDX-License-Identifier: GPL-2.0-or-later */
/* L2TPv3 reliable control transport plus client connection/session FSM.
 * Pure state machine: I/O, kernel actions and time are supplied by the caller. */
#ifndef FG_CTRL_H
#define FG_CTRL_H

#include <stdbool.h>
#include <stdint.h>

#include "proto.h"

#define FG_QUEUE_LEN 4
#define FG_RTX_INITIAL_MS 1000
#define FG_RTX_MAX_MS 8000

enum fg_reason {
	FG_R_NONE,
	FG_R_TIMEOUT,
	FG_R_PEER_REJECTED,
	FG_R_PEER_CLOSED,
	FG_R_UNSUPPORTED_PEER,
	FG_R_PROTOCOL_ERROR,
	FG_R_KERNEL_ERROR,
};

enum fg_ctrl_state {
	FG_CTRL_IDLE,
	FG_CTRL_WAIT_SCCRP,
	FG_CTRL_ESTABLISHED,
	FG_CTRL_STOPPING,	/* our StopCCN is being delivered */
	FG_CTRL_LINGER,		/* peer StopCCN received; re-ACK retransmissions */
	FG_CTRL_CLOSED,
};

enum fg_sess_state {
	FG_SESS_NONE,
	FG_SESS_WAIT_ICRP,
	FG_SESS_WAIT_ICCN_ACK,
	FG_SESS_ESTABLISHED,
};

struct fg_ctrl_params {
	uint32_t conn_id;
	uint32_t session_id;
	uint32_t router_id;
	uint32_t serial;
	const char *hostname;
	unsigned retry_max;
	unsigned hello_ms;
};

struct fg_ctrl_ops {
	void (*send)(void *ctx, const uint8_t *msg, size_t len);
	/* Create kernel tunnel/session after an acceptable ICRP; 0 on success. */
	int (*session_setup)(void *ctx, uint32_t peer_conn_id, uint32_t peer_session_id);
	/* ICCN acknowledged: the session may carry traffic if the circuit is active. */
	void (*session_up)(void *ctx, bool circuit_active);
	void (*circuit)(void *ctx, bool active);
	/* Called once when the connection fails or the peer closes it. */
	void (*down)(void *ctx, enum fg_reason reason, const char *detail);
};

struct fg_queued {
	uint16_t type;
	size_t len;
	uint8_t data[FG_MAX_MSG];
};

struct fg_ctrl {
	struct fg_ctrl_params p;
	const struct fg_ctrl_ops *ops;
	void *ctx;

	enum fg_ctrl_state state;
	enum fg_sess_state sess;
	bool down_reported;
	bool circuit_active;
	bool tx_flag;

	uint32_t peer_conn_id;
	uint32_t peer_session_id;
	uint16_t peer_rws;

	uint16_t ns_next;
	uint16_t nr;

	struct fg_queued q[FG_QUEUE_LEN];
	unsigned q_head, q_count;
	bool inflight;
	uint16_t inflight_ns;
	unsigned retries;
	unsigned rtx_ms;
	int64_t rtx_deadline;

	int64_t last_rx;
	int64_t end_deadline;
};

void fg_ctrl_init(struct fg_ctrl *c, const struct fg_ctrl_params *p,
		  const struct fg_ctrl_ops *ops, void *ctx);
void fg_ctrl_start(struct fg_ctrl *c, int64_t now);
void fg_ctrl_input(struct fg_ctrl *c, const uint8_t *buf, size_t len, int64_t now);
void fg_ctrl_timer(struct fg_ctrl *c, int64_t now);
/* Graceful local close: StopCCN with bounded reliable delivery. */
void fg_ctrl_stop(struct fg_ctrl *c, int64_t now);
/* Absolute monotonic deadline in ms, or -1 when no timer is needed. */
int64_t fg_ctrl_next_deadline(const struct fg_ctrl *c);
/* Time needed for one full retransmission cycle with retry_max. */
unsigned fg_ctrl_cycle_ms(unsigned retry_max);

static inline bool fg_ctrl_finished(const struct fg_ctrl *c) { return c->state == FG_CTRL_CLOSED; }

const char *fg_reason_name(enum fg_reason r);

#endif
