/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include "proto.h"

#define FG_FLAGS_MASK 0xc80f
#define FG_FLAGS_V3 0xc803
#define FG_AVP_M 0x8000
#define FG_AVP_H 0x4000
#define FG_AVP_LEN_MASK 0x03ff

int fg_parse(const uint8_t *buf, size_t len, struct fg_msg *m)
{
	size_t off = FG_HDR_LEN;
	uint16_t flags, length;

	memset(m, 0, sizeof(*m));
	if (len < FG_HDR_LEN)
		return -1;
	flags = fg_rd16(buf);
	length = fg_rd16(buf + 2);
	if ((flags & FG_FLAGS_MASK) != FG_FLAGS_V3 || length != len)
		return -1;
	m->conn_id = fg_rd32(buf + 4);
	m->ns = fg_rd16(buf + 8);
	m->nr = fg_rd16(buf + 10);

	while (off < len) {
		struct fg_avp *a;
		uint16_t bits, alen;

		if (len - off < 6 || m->n_avps >= FG_MAX_AVPS)
			return -1;
		bits = fg_rd16(buf + off);
		alen = bits & FG_AVP_LEN_MASK;
		if (alen < 6 || alen > len - off)
			return -1;
		a = &m->avps[m->n_avps++];
		a->mandatory = bits & FG_AVP_M;
		a->hidden = bits & FG_AVP_H;
		a->vendor = fg_rd16(buf + off + 2);
		a->type = fg_rd16(buf + off + 4);
		a->len = alen - 6;
		a->val = buf + off + 6;
		off += alen;
	}

	if (!m->n_avps) {
		m->zlb = true;
		return 0;
	}
	/* Message Type must be first, unhidden and two octets long. */
	if (m->avps[0].type != FG_AVP_MSG_TYPE || m->avps[0].hidden || m->avps[0].len != 2)
		return -1;
	m->type_vendor = m->avps[0].vendor;
	m->type = fg_rd16(m->avps[0].val);
	m->type_mandatory = m->avps[0].mandatory;
	return 0;
}

bool fg_avp_recognized(const struct fg_avp *a)
{
	static const uint8_t known[] = {
		0, 1, 2, 3, 4, 5, 7, 8, 10, 15, 19, 24, 25, 34, 36, 58, 59, 60, 61,
		62, 63, 64, 65, 66, 68, 69, 70, 71, 72, 73, 74, 75,
	};

	if (a->vendor == FG_VENDOR_AVM)
		return a->type == FG_AVM_OFFSET;
	if (a->vendor != FG_VENDOR_IETF)
		return false;
	for (size_t i = 0; i < sizeof(known); i++)
		if (a->type == known[i])
			return true;
	return false;
}

const struct fg_avp *fg_find(const struct fg_msg *m, uint16_t vendor, uint16_t type, bool *dup)
{
	const struct fg_avp *found = NULL;

	*dup = false;
	/* Index 0 is the Message Type AVP. */
	for (unsigned i = 1; i < m->n_avps; i++) {
		const struct fg_avp *a = &m->avps[i];

		if (a->vendor != vendor || a->type != type)
			continue;
		if (found)
			*dup = true;
		found = a;
	}
	return found;
}

static int get_fixed(const struct fg_msg *m, uint16_t vendor, uint16_t type, size_t width,
		     const uint8_t **val)
{
	bool dup;
	const struct fg_avp *a = fg_find(m, vendor, type, &dup);

	if (!a)
		return -1;
	if (dup || a->hidden || a->len != width)
		return -2;
	*val = a->val;
	return 0;
}

int fg_get_u16(const struct fg_msg *m, uint16_t vendor, uint16_t type, uint16_t *out)
{
	const uint8_t *v;
	int ret = get_fixed(m, vendor, type, 2, &v);

	if (!ret)
		*out = fg_rd16(v);
	return ret;
}

int fg_get_u32(const struct fg_msg *m, uint16_t vendor, uint16_t type, uint32_t *out)
{
	const uint8_t *v;
	int ret = get_fixed(m, vendor, type, 4, &v);

	if (!ret)
		*out = fg_rd32(v);
	return ret;
}

void fg_build_start(struct fg_buf *b, uint16_t msg_type)
{
	memset(b->data, 0, FG_HDR_LEN);
	b->len = FG_HDR_LEN;
	b->overflow = false;
	fg_put_u16(b, FG_AVP_MSG_TYPE, msg_type, true);
}

void fg_put_avp(struct fg_buf *b, uint16_t type, const void *val, size_t len, bool mandatory)
{
	uint8_t *p;

	if (len > FG_AVP_LEN_MASK - 6 || b->len + 6 + len > sizeof(b->data)) {
		b->overflow = true;
		return;
	}
	p = b->data + b->len;
	fg_wr16(p, (uint16_t)(len + 6) | (mandatory ? FG_AVP_M : 0));
	fg_wr16(p + 2, FG_VENDOR_IETF);
	fg_wr16(p + 4, type);
	if (len)
		memcpy(p + 6, val, len);
	b->len += 6 + len;
}

void fg_put_u16(struct fg_buf *b, uint16_t type, uint16_t v, bool mandatory)
{
	uint8_t val[2];

	fg_wr16(val, v);
	fg_put_avp(b, type, val, sizeof(val), mandatory);
}

void fg_put_u32(struct fg_buf *b, uint16_t type, uint32_t v, bool mandatory)
{
	uint8_t val[4];

	fg_wr32(val, v);
	fg_put_avp(b, type, val, sizeof(val), mandatory);
}

void fg_write_header(uint8_t *msg, size_t len, uint32_t conn_id, uint16_t ns, uint16_t nr)
{
	fg_wr16(msg, FG_FLAGS_V3);
	fg_wr16(msg + 2, (uint16_t)len);
	fg_wr32(msg + 4, conn_id);
	fg_wr16(msg + 8, ns);
	fg_wr16(msg + 10, nr);
}

const char *fg_msg_name(uint16_t type)
{
	switch (type) {
	case FG_MSG_SCCRQ: return "SCCRQ";
	case FG_MSG_SCCRP: return "SCCRP";
	case FG_MSG_SCCCN: return "SCCCN";
	case FG_MSG_STOPCCN: return "StopCCN";
	case FG_MSG_HELLO: return "HELLO";
	case FG_MSG_OCRQ: return "OCRQ";
	case FG_MSG_OCRP: return "OCRP";
	case FG_MSG_OCCN: return "OCCN";
	case FG_MSG_ICRQ: return "ICRQ";
	case FG_MSG_ICRP: return "ICRP";
	case FG_MSG_ICCN: return "ICCN";
	case FG_MSG_CDN: return "CDN";
	case FG_MSG_WEN: return "WEN";
	case FG_MSG_SLI: return "SLI";
	case FG_MSG_ACK: return "ACK";
	default: return "unknown";
	}
}
