/* SPDX-License-Identifier: GPL-2.0-or-later */
/* RFC 3931 L2TPv3 control message codec (no I/O, no allocation). */
#ifndef FG_PROTO_H
#define FG_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FG_HDR_LEN 12
#define FG_MAX_MSG 512
#define FG_MAX_AVPS 64

enum fg_msg_type {
	FG_MSG_SCCRQ = 1,
	FG_MSG_SCCRP = 2,
	FG_MSG_SCCCN = 3,
	FG_MSG_STOPCCN = 4,
	FG_MSG_HELLO = 6,
	FG_MSG_OCRQ = 7,
	FG_MSG_OCRP = 8,
	FG_MSG_OCCN = 9,
	FG_MSG_ICRQ = 10,
	FG_MSG_ICRP = 11,
	FG_MSG_ICCN = 12,
	FG_MSG_CDN = 14,
	FG_MSG_WEN = 15,
	FG_MSG_SLI = 16,
	FG_MSG_ACK = 20,
};

enum fg_avp_type {
	FG_AVP_MSG_TYPE = 0,
	FG_AVP_RESULT = 1,
	FG_AVP_HOSTNAME = 7,
	FG_AVP_VENDOR_NAME = 8,
	FG_AVP_RWS = 10,
	FG_AVP_SERIAL = 15,
	FG_AVP_RANDOM_VECTOR = 36,
	FG_AVP_MSG_DIGEST = 59,
	FG_AVP_ROUTER_ID = 60,
	FG_AVP_CONN_ID = 61,
	FG_AVP_PW_CAPS = 62,
	FG_AVP_LOCAL_SID = 63,
	FG_AVP_REMOTE_SID = 64,
	FG_AVP_COOKIE = 65,
	FG_AVP_REMOTE_END = 66,
	FG_AVP_PW_TYPE = 68,
	FG_AVP_L2SPEC = 69,
	FG_AVP_DATA_SEQ = 70,
	FG_AVP_CIRCUIT = 71,
	FG_AVP_NONCE = 73,
};

#define FG_VENDOR_IETF 0
#define FG_VENDOR_AVM 872
#define FG_AVM_OFFSET 1

#define FG_PW_ETHERNET 5
#define FG_L2SPEC_DEFAULT 1
#define FG_CIRCUIT_ACTIVE 0x0001
#define FG_CIRCUIT_NEW 0x0002
#define FG_REMOTE_END_ID "fritz.box:guest"

/* StopCCN / CDN result and general error codes (RFC 3931 5.4.2) */
#define FG_RESULT_CLEAR 1
#define FG_RESULT_GENERAL 2
#define FG_CDN_PERMANENT 5
#define FG_ERR_NONE 0
#define FG_ERR_LENGTH 2
#define FG_ERR_RANGE 3
#define FG_ERR_VENDOR 6
#define FG_ERR_UNKNOWN_MANDATORY 8

struct fg_avp {
	uint16_t vendor;
	uint16_t type;
	uint16_t len;
	bool mandatory;
	bool hidden;
	const uint8_t *val;
};

struct fg_msg {
	uint32_t conn_id;
	uint16_t ns;
	uint16_t nr;
	bool zlb;
	uint16_t type_vendor;
	uint16_t type;
	bool type_mandatory;
	unsigned n_avps;
	struct fg_avp avps[FG_MAX_AVPS];
};

/* Parse a control message starting at the T bit (no leading zero Session ID).
 * Returns 0, or -1 if the header or AVP framing is malformed. */
int fg_parse(const uint8_t *buf, size_t len, struct fg_msg *m);

/* Recognized means this implementation understands the AVP's meaning. */
bool fg_avp_recognized(const struct fg_avp *a);

/* Returns 0 if found, -1 if absent, -2 if duplicated or wrong length. */
int fg_get_u16(const struct fg_msg *m, uint16_t vendor, uint16_t type, uint16_t *out);
int fg_get_u32(const struct fg_msg *m, uint16_t vendor, uint16_t type, uint32_t *out);
/* Returns the single matching AVP, NULL if absent; *dup set on duplicates. */
const struct fg_avp *fg_find(const struct fg_msg *m, uint16_t vendor, uint16_t type, bool *dup);

struct fg_buf {
	uint8_t data[FG_MAX_MSG];
	size_t len;
	bool overflow;
};

void fg_build_start(struct fg_buf *b, uint16_t msg_type);
void fg_put_avp(struct fg_buf *b, uint16_t type, const void *val, size_t len, bool mandatory);
void fg_put_u16(struct fg_buf *b, uint16_t type, uint16_t v, bool mandatory);
void fg_put_u32(struct fg_buf *b, uint16_t type, uint32_t v, bool mandatory);
/* Writes the control header in place; body must already be complete. */
void fg_write_header(uint8_t *msg, size_t len, uint32_t conn_id, uint16_t ns, uint16_t nr);

static inline uint16_t fg_rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t fg_rd32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static inline void fg_wr16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xff; }
static inline void fg_wr32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24; p[1] = (v >> 16) & 0xff; p[2] = (v >> 8) & 0xff; p[3] = v & 0xff;
}

/* 16-bit serial arithmetic: true if a precedes b. */
static inline bool fg_seq_lt(uint16_t a, uint16_t b) { return (uint16_t)(b - a - 1) < 0x8000; }

const char *fg_msg_name(uint16_t type);

#endif
