/*
 * (C) 2012-2013 by Pablo Neira Ayuso <pablo@netfilter.org>
 * (C) 2013 by Tomasz Bursztyka <tomasz.bursztyka@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This code has been sponsored by Sophos Astaro <http://www.sophos.com>
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <netdb.h>
#include <errno.h>
#include <inttypes.h>

#include <xtables.h>

#include <linux/netfilter/nf_log.h>
#include <linux/netfilter/xt_comment.h>
#include <linux/netfilter/xt_limit.h>
#include <linux/netfilter/xt_NFLOG.h>

#include <libmnl/libmnl.h>
#include <libnftnl/rule.h>
#include <libnftnl/expr.h>

#include "nft-shared.h"
#include "nft-bridge.h"
#include "xshared.h"
#include "nft.h"

extern struct nft_family_ops nft_family_ops_ipv4;
extern struct nft_family_ops nft_family_ops_ipv6;
extern struct nft_family_ops nft_family_ops_arp;
extern struct nft_family_ops nft_family_ops_bridge;

void add_meta(struct nftnl_rule *r, uint32_t key)
{
	struct nftnl_expr *expr;

	expr = nftnl_expr_alloc("meta");
	if (expr == NULL)
		return;

	nftnl_expr_set_u32(expr, NFTNL_EXPR_META_KEY, key);
	nftnl_expr_set_u32(expr, NFTNL_EXPR_META_DREG, NFT_REG_1);

	nftnl_rule_add_expr(r, expr);
}

void add_payload(struct nftnl_rule *r, int offset, int len, uint32_t base)
{
	struct nftnl_expr *expr;

	expr = nftnl_expr_alloc("payload");
	if (expr == NULL)
		return;

	nftnl_expr_set_u32(expr, NFTNL_EXPR_PAYLOAD_BASE, base);
	nftnl_expr_set_u32(expr, NFTNL_EXPR_PAYLOAD_DREG, NFT_REG_1);
	nftnl_expr_set_u32(expr, NFTNL_EXPR_PAYLOAD_OFFSET, offset);
	nftnl_expr_set_u32(expr, NFTNL_EXPR_PAYLOAD_LEN, len);

	nftnl_rule_add_expr(r, expr);
}

/* bitwise operation is = sreg & mask ^ xor */
void add_bitwise_u16(struct nftnl_rule *r, uint16_t mask, uint16_t xor)
{
	struct nftnl_expr *expr;

	expr = nftnl_expr_alloc("bitwise");
	if (expr == NULL)
		return;

	nftnl_expr_set_u32(expr, NFTNL_EXPR_BITWISE_SREG, NFT_REG_1);
	nftnl_expr_set_u32(expr, NFTNL_EXPR_BITWISE_DREG, NFT_REG_1);
	nftnl_expr_set_u32(expr, NFTNL_EXPR_BITWISE_LEN, sizeof(uint16_t));
	nftnl_expr_set(expr, NFTNL_EXPR_BITWISE_MASK, &mask, sizeof(uint16_t));
	nftnl_expr_set(expr, NFTNL_EXPR_BITWISE_XOR, &xor, sizeof(uint16_t));

	nftnl_rule_add_expr(r, expr);
}

void add_bitwise(struct nftnl_rule *r, uint8_t *mask, size_t len)
{
	struct nftnl_expr *expr;
	uint32_t xor[4] = { 0 };

	expr = nftnl_expr_alloc("bitwise");
	if (expr == NULL)
		return;

	nftnl_expr_set_u32(expr, NFTNL_EXPR_BITWISE_SREG, NFT_REG_1);
	nftnl_expr_set_u32(expr, NFTNL_EXPR_BITWISE_DREG, NFT_REG_1);
	nftnl_expr_set_u32(expr, NFTNL_EXPR_BITWISE_LEN, len);
	nftnl_expr_set(expr, NFTNL_EXPR_BITWISE_MASK, mask, len);
	nftnl_expr_set(expr, NFTNL_EXPR_BITWISE_XOR, &xor, len);

	nftnl_rule_add_expr(r, expr);
}

void add_cmp_ptr(struct nftnl_rule *r, uint32_t op, void *data, size_t len)
{
	struct nftnl_expr *expr;

	expr = nftnl_expr_alloc("cmp");
	if (expr == NULL)
		return;

	nftnl_expr_set_u32(expr, NFTNL_EXPR_CMP_SREG, NFT_REG_1);
	nftnl_expr_set_u32(expr, NFTNL_EXPR_CMP_OP, op);
	nftnl_expr_set(expr, NFTNL_EXPR_CMP_DATA, data, len);

	nftnl_rule_add_expr(r, expr);
}

void add_cmp_u8(struct nftnl_rule *r, uint8_t val, uint32_t op)
{
	add_cmp_ptr(r, op, &val, sizeof(val));
}

void add_cmp_u16(struct nftnl_rule *r, uint16_t val, uint32_t op)
{
	add_cmp_ptr(r, op, &val, sizeof(val));
}

void add_cmp_u32(struct nftnl_rule *r, uint32_t val, uint32_t op)
{
	add_cmp_ptr(r, op, &val, sizeof(val));
}

void add_iniface(struct nftnl_rule *r, char *iface, uint32_t op)
{
	int iface_len;

	iface_len = strlen(iface);

	add_meta(r, NFT_META_IIFNAME);
	if (iface[iface_len - 1] == '+') {
		if (iface_len > 1)
			add_cmp_ptr(r, op, iface, iface_len - 1);
	} else
		add_cmp_ptr(r, op, iface, iface_len + 1);
}

void add_outiface(struct nftnl_rule *r, char *iface, uint32_t op)
{
	int iface_len;

	iface_len = strlen(iface);

	add_meta(r, NFT_META_OIFNAME);
	if (iface[iface_len - 1] == '+') {
		if (iface_len > 1)
			add_cmp_ptr(r, op, iface, iface_len - 1);
	} else
		add_cmp_ptr(r, op, iface, iface_len + 1);
}

void add_addr(struct nftnl_rule *r, enum nft_payload_bases base, int offset,
	      void *data, void *mask, size_t len, uint32_t op)
{
	const unsigned char *m = mask;
	bool bitwise = false;
	int i, j;

	for (i = 0; i < len; i++) {
		if (m[i] != 0xff) {
			bitwise = m[i] != 0;
			break;
		}
	}
	for (j = i + 1; !bitwise && j < len; j++)
		bitwise = !!m[j];

	if (!bitwise)
		len = i;

	add_payload(r, offset, len, base);

	if (bitwise)
		add_bitwise(r, mask, len);

	add_cmp_ptr(r, op, data, len);
}

void add_proto(struct nftnl_rule *r, int offset, size_t len,
	       uint8_t proto, uint32_t op)
{
	add_payload(r, offset, len, NFT_PAYLOAD_NETWORK_HEADER);
	add_cmp_u8(r, proto, op);
}

void add_l4proto(struct nftnl_rule *r, uint8_t proto, uint32_t op)
{
	add_meta(r, NFT_META_L4PROTO);
	add_cmp_u8(r, proto, op);
}

bool is_same_interfaces(const char *a_iniface, const char *a_outiface,
			unsigned const char *a_iniface_mask,
			unsigned const char *a_outiface_mask,
			const char *b_iniface, const char *b_outiface,
			unsigned const char *b_iniface_mask,
			unsigned const char *b_outiface_mask)
{
	int i;

	for (i = 0; i < IFNAMSIZ; i++) {
		if (a_iniface_mask[i] != b_iniface_mask[i]) {
			DEBUGP("different iniface mask %x, %x (%d)\n",
			a_iniface_mask[i] & 0xff, b_iniface_mask[i] & 0xff, i);
			return false;
		}
		if ((a_iniface[i] & a_iniface_mask[i])
		    != (b_iniface[i] & b_iniface_mask[i])) {
			DEBUGP("different iniface\n");
			return false;
		}
		if (a_outiface_mask[i] != b_outiface_mask[i]) {
			DEBUGP("different outiface mask\n");
			return false;
		}
		if ((a_outiface[i] & a_outiface_mask[i])
		    != (b_outiface[i] & b_outiface_mask[i])) {
			DEBUGP("different outiface\n");
			return false;
		}
	}

	return true;
}

static void parse_ifname(const char *name, unsigned int len, char *dst, unsigned char *mask)
{
	if (len == 0)
		return;

	memcpy(dst, name, len);
	if (name[len - 1] == '\0') {
		if (mask)
			memset(mask, 0xff, len);
		return;
	}

	if (len >= IFNAMSIZ)
		return;

	/* wildcard */
	dst[len++] = '+';
	if (len >= IFNAMSIZ)
		return;
	dst[len++] = 0;
	if (mask)
		memset(mask, 0xff, len - 2);
}

int parse_meta(struct nftnl_expr *e, uint8_t key, char *iniface,
		unsigned char *iniface_mask, char *outiface,
		unsigned char *outiface_mask, uint8_t *invflags)
{
	uint32_t value;
	const void *ifname;
	uint32_t len;

	switch(key) {
	case NFT_META_IIF:
		value = nftnl_expr_get_u32(e, NFTNL_EXPR_CMP_DATA);
		if (nftnl_expr_get_u32(e, NFTNL_EXPR_CMP_OP) == NFT_CMP_NEQ)
			*invflags |= IPT_INV_VIA_IN;

		if_indextoname(value, iniface);

		memset(iniface_mask, 0xff, strlen(iniface)+1);
		break;
	case NFT_META_OIF:
		value = nftnl_expr_get_u32(e, NFTNL_EXPR_CMP_DATA);
		if (nftnl_expr_get_u32(e, NFTNL_EXPR_CMP_OP) == NFT_CMP_NEQ)
			*invflags |= IPT_INV_VIA_OUT;

		if_indextoname(value, outiface);

		memset(outiface_mask, 0xff, strlen(outiface)+1);
		break;
	case NFT_META_BRI_IIFNAME:
	case NFT_META_IIFNAME:
		ifname = nftnl_expr_get(e, NFTNL_EXPR_CMP_DATA, &len);
		if (nftnl_expr_get_u32(e, NFTNL_EXPR_CMP_OP) == NFT_CMP_NEQ)
			*invflags |= IPT_INV_VIA_IN;

		parse_ifname(ifname, len, iniface, iniface_mask);
		break;
	case NFT_META_BRI_OIFNAME:
	case NFT_META_OIFNAME:
		ifname = nftnl_expr_get(e, NFTNL_EXPR_CMP_DATA, &len);
		if (nftnl_expr_get_u32(e, NFTNL_EXPR_CMP_OP) == NFT_CMP_NEQ)
			*invflags |= IPT_INV_VIA_OUT;

		parse_ifname(ifname, len, outiface, outiface_mask);
		break;
	default:
		return -1;
	}

	return 0;
}

static void nft_parse_target(struct nft_xt_ctx *ctx, struct nftnl_expr *e)
{
	uint32_t tg_len;
	const char *targname = nftnl_expr_get_str(e, NFTNL_EXPR_TG_NAME);
	const void *targinfo = nftnl_expr_get(e, NFTNL_EXPR_TG_INFO, &tg_len);
	struct xtables_target *target;
	struct xt_entry_target *t;
	size_t size;

	target = xtables_find_target(targname, XTF_TRY_LOAD);
	if (target == NULL)
		return;

	size = XT_ALIGN(sizeof(struct xt_entry_target)) + tg_len;

	t = xtables_calloc(1, size);
	memcpy(&t->data, targinfo, tg_len);
	t->u.target_size = size;
	t->u.user.revision = nftnl_expr_get_u32(e, NFTNL_EXPR_TG_REV);
	strcpy(t->u.user.name, target->name);

	target->t = t;

	ctx->h->ops->parse_target(target, ctx->cs);
}

static void nft_parse_match(struct nft_xt_ctx *ctx, struct nftnl_expr *e)
{
	uint32_t mt_len;
	const char *mt_name = nftnl_expr_get_str(e, NFTNL_EXPR_MT_NAME);
	const void *mt_info = nftnl_expr_get(e, NFTNL_EXPR_MT_INFO, &mt_len);
	struct xtables_match *match;
	struct xtables_rule_match **matches;
	struct xt_entry_match *m;

	switch (ctx->h->family) {
	case NFPROTO_IPV4:
	case NFPROTO_IPV6:
	case NFPROTO_BRIDGE:
		matches = &ctx->cs->matches;
		break;
	default:
		fprintf(stderr, "BUG: nft_parse_match() unknown family %d\n",
			ctx->h->family);
		exit(EXIT_FAILURE);
	}

	match = xtables_find_match(mt_name, XTF_TRY_LOAD, matches);
	if (match == NULL)
		return;

	m = xtables_calloc(1, sizeof(struct xt_entry_match) + mt_len);
	memcpy(&m->data, mt_info, mt_len);
	m->u.match_size = mt_len + XT_ALIGN(sizeof(struct xt_entry_match));
	m->u.user.revision = nftnl_expr_get_u32(e, NFTNL_EXPR_TG_REV);
	strcpy(m->u.user.name, match->name);

	match->m = m;

	if (ctx->h->ops->parse_match != NULL)
		ctx->h->ops->parse_match(match, ctx->cs);
}

void get_cmp_data(struct nftnl_expr *e, void *data, size_t dlen, bool *inv)
{
	uint32_t len;
	uint8_t op;

	memcpy(data, nftnl_expr_get(e, NFTNL_EXPR_CMP_DATA, &len), dlen);
	op = nftnl_expr_get_u32(e, NFTNL_EXPR_CMP_OP);
	if (op == NFT_CMP_NEQ)
		*inv = true;
	else
		*inv = false;
}

static void nft_meta_set_to_target(struct nft_xt_ctx *ctx)
{
	struct xtables_target *target;
	struct xt_entry_target *t;
	unsigned int size;
	const char *targname;

	switch (ctx->meta.key) {
	case NFT_META_NFTRACE:
		if (ctx->immediate.data[0] == 0)
			return;
		targname = "TRACE";
		break;
	default:
		return;
	}

	target = xtables_find_target(targname, XTF_TRY_LOAD);
	if (target == NULL)
		return;

	size = XT_ALIGN(sizeof(struct xt_entry_target)) + target->size;

	t = xtables_calloc(1, size);
	t->u.target_size = size;
	t->u.user.revision = target->revision;
	strcpy(t->u.user.name, targname);

	target->t = t;

	ctx->h->ops->parse_target(target, ctx->cs);
}

static void nft_parse_meta(struct nft_xt_ctx *ctx, struct nftnl_expr *e)
{
	ctx->meta.key = nftnl_expr_get_u32(e, NFTNL_EXPR_META_KEY);

	if (nftnl_expr_is_set(e, NFTNL_EXPR_META_SREG) &&
	    (ctx->flags & NFT_XT_CTX_IMMEDIATE) &&
	     nftnl_expr_get_u32(e, NFTNL_EXPR_META_SREG) == ctx->immediate.reg) {
		ctx->flags &= ~NFT_XT_CTX_IMMEDIATE;
		nft_meta_set_to_target(ctx);
		return;
	}

	ctx->reg = nftnl_expr_get_u32(e, NFTNL_EXPR_META_DREG);
	ctx->flags |= NFT_XT_CTX_META;
}

static void nft_parse_payload(struct nft_xt_ctx *ctx, struct nftnl_expr *e)
{
	if (ctx->flags & NFT_XT_CTX_PAYLOAD) {
		memcpy(&ctx->prev_payload, &ctx->payload,
		       sizeof(ctx->prev_payload));
		ctx->flags |= NFT_XT_CTX_PREV_PAYLOAD;
	}

	ctx->reg = nftnl_expr_get_u32(e, NFTNL_EXPR_PAYLOAD_DREG);
	ctx->payload.base = nftnl_expr_get_u32(e, NFTNL_EXPR_PAYLOAD_BASE);
	ctx->payload.offset = nftnl_expr_get_u32(e, NFTNL_EXPR_PAYLOAD_OFFSET);
	ctx->payload.len = nftnl_expr_get_u32(e, NFTNL_EXPR_PAYLOAD_LEN);
	ctx->flags |= NFT_XT_CTX_PAYLOAD;
}

static void nft_parse_bitwise(struct nft_xt_ctx *ctx, struct nftnl_expr *e)
{
	uint32_t reg, len;
	const void *data;

	reg = nftnl_expr_get_u32(e, NFTNL_EXPR_BITWISE_SREG);
	if (ctx->reg && reg != ctx->reg)
		return;

	data = nftnl_expr_get(e, NFTNL_EXPR_BITWISE_XOR, &len);
	memcpy(ctx->bitwise.xor, data, len);
	data = nftnl_expr_get(e, NFTNL_EXPR_BITWISE_MASK, &len);
	memcpy(ctx->bitwise.mask, data, len);
	ctx->flags |= NFT_XT_CTX_BITWISE;
}

static struct xtables_match *
nft_create_match(struct nft_xt_ctx *ctx,
		 struct iptables_command_state *cs,
		 const char *name)
{
	struct xtables_match *match;
	struct xt_entry_match *m;
	unsigned int size;

	match = xtables_find_match(name, XTF_TRY_LOAD,
				   &cs->matches);
	if (!match)
		return NULL;

	size = XT_ALIGN(sizeof(struct xt_entry_match)) + match->size;
	m = xtables_calloc(1, size);
	m->u.match_size = size;
	m->u.user.revision = match->revision;

	strcpy(m->u.user.name, match->name);
	match->m = m;

	xs_init_match(match);

	return match;
}

static struct xt_udp *nft_udp_match(struct nft_xt_ctx *ctx,
			            struct iptables_command_state *cs)
{
	struct xt_udp *udp = ctx->tcpudp.udp;
	struct xtables_match *match;

	if (!udp) {
		match = nft_create_match(ctx, cs, "udp");
		if (!match)
			return NULL;

		udp = (void*)match->m->data;
		ctx->tcpudp.udp = udp;
	}

	return udp;
}

static struct xt_tcp *nft_tcp_match(struct nft_xt_ctx *ctx,
			            struct iptables_command_state *cs)
{
	struct xt_tcp *tcp = ctx->tcpudp.tcp;
	struct xtables_match *match;

	if (!tcp) {
		match = nft_create_match(ctx, cs, "tcp");
		if (!match)
			return NULL;

		tcp = (void*)match->m->data;
		ctx->tcpudp.tcp = tcp;
	}

	return tcp;
}

static void nft_parse_udp_range(struct nft_xt_ctx *ctx,
			        struct iptables_command_state *cs,
			        int sport_from, int sport_to,
			        int dport_from, int dport_to,
				uint8_t op)
{
	struct xt_udp *udp = nft_udp_match(ctx, cs);

	if (!udp)
		return;

	if (sport_from >= 0) {
		switch (op) {
		case NFT_RANGE_NEQ:
			udp->invflags |= XT_UDP_INV_SRCPT;
			/* fallthrough */
		case NFT_RANGE_EQ:
			udp->spts[0] = sport_from;
			udp->spts[1] = sport_to;
			break;
		}
	}

	if (dport_to >= 0) {
		switch (op) {
		case NFT_CMP_NEQ:
			udp->invflags |= XT_UDP_INV_DSTPT;
			/* fallthrough */
		case NFT_CMP_EQ:
			udp->dpts[0] = dport_from;
			udp->dpts[1] = dport_to;
			break;
		}
	}
}

static void nft_parse_tcp_range(struct nft_xt_ctx *ctx,
			        struct iptables_command_state *cs,
			        int sport_from, int sport_to,
			        int dport_from, int dport_to,
				uint8_t op)
{
	struct xt_tcp *tcp = nft_tcp_match(ctx, cs);

	if (!tcp)
		return;

	if (sport_from >= 0) {
		switch (op) {
		case NFT_RANGE_NEQ:
			tcp->invflags |= XT_TCP_INV_SRCPT;
			/* fallthrough */
		case NFT_RANGE_EQ:
			tcp->spts[0] = sport_from;
			tcp->spts[1] = sport_to;
			break;
		}
	}

	if (dport_to >= 0) {
		switch (op) {
		case NFT_CMP_NEQ:
			tcp->invflags |= XT_TCP_INV_DSTPT;
			/* fallthrough */
		case NFT_CMP_EQ:
			tcp->dpts[0] = dport_from;
			tcp->dpts[1] = dport_to;
			break;
		}
	}
}

static void nft_parse_udp(struct nft_xt_ctx *ctx,
			  struct iptables_command_state *cs,
			  int sport, int dport,
			  uint8_t op)
{
	struct xt_udp *udp = nft_udp_match(ctx, cs);

	if (!udp)
		return;

	if (sport >= 0) {
		switch (op) {
		case NFT_CMP_NEQ:
			udp->invflags |= XT_UDP_INV_SRCPT;
			/* fallthrough */
		case NFT_CMP_EQ:
			udp->spts[0] = sport;
			udp->spts[1] = sport;
			break;
		case NFT_CMP_LT:
			udp->spts[1] = sport > 1 ? sport - 1 : 1;
			break;
		case NFT_CMP_LTE:
			udp->spts[1] = sport;
			break;
		case NFT_CMP_GT:
			udp->spts[0] = sport < 0xffff ? sport + 1 : 0xffff;
			break;
		case NFT_CMP_GTE:
			udp->spts[0] = sport;
			break;
		}
	}
	if (dport >= 0) {
		switch (op) {
		case NFT_CMP_NEQ:
			udp->invflags |= XT_UDP_INV_DSTPT;
			/* fallthrough */
		case NFT_CMP_EQ:
			udp->dpts[0] = dport;
			udp->dpts[1] = dport;
			break;
		case NFT_CMP_LT:
			udp->dpts[1] = dport > 1 ? dport - 1 : 1;
			break;
		case NFT_CMP_LTE:
			udp->dpts[1] = dport;
			break;
		case NFT_CMP_GT:
			udp->dpts[0] = dport < 0xffff ? dport + 1 : 0xffff;
			break;
		case NFT_CMP_GTE:
			udp->dpts[0] = dport;
			break;
		}
	}
}

static void nft_parse_tcp(struct nft_xt_ctx *ctx,
			  struct iptables_command_state *cs,
			  int sport, int dport,
			  uint8_t op)
{
	struct xt_tcp *tcp = nft_tcp_match(ctx, cs);

	if (!tcp)
		return;

	if (sport >= 0) {
		switch (op) {
		case NFT_CMP_NEQ:
			tcp->invflags |= XT_TCP_INV_SRCPT;
			/* fallthrough */
		case NFT_CMP_EQ:
			tcp->spts[0] = sport;
			tcp->spts[1] = sport;
			break;
		case NFT_CMP_LT:
			tcp->spts[1] = sport > 1 ? sport - 1 : 1;
			break;
		case NFT_CMP_LTE:
			tcp->spts[1] = sport;
			break;
		case NFT_CMP_GT:
			tcp->spts[0] = sport < 0xffff ? sport + 1 : 0xffff;
			break;
		case NFT_CMP_GTE:
			tcp->spts[0] = sport;
			break;
		}
	}

	if (dport >= 0) {
		switch (op) {
		case NFT_CMP_NEQ:
			tcp->invflags |= XT_TCP_INV_DSTPT;
			/* fallthrough */
		case NFT_CMP_EQ:
			tcp->dpts[0] = dport;
			tcp->dpts[1] = dport;
			break;
		case NFT_CMP_LT:
			tcp->dpts[1] = dport > 1 ? dport - 1 : 1;
			break;
		case NFT_CMP_LTE:
			tcp->dpts[1] = dport;
			break;
		case NFT_CMP_GT:
			tcp->dpts[0] = dport < 0xffff ? dport + 1 : 0xffff;
			break;
		case NFT_CMP_GTE:
			tcp->dpts[0] = dport;
			break;
		}
	}
}

static void nft_parse_th_port(struct nft_xt_ctx *ctx,
			      struct iptables_command_state *cs,
			      uint8_t proto,
			      int sport, int dport, uint8_t op)
{
	switch (proto) {
	case IPPROTO_UDP:
		nft_parse_udp(ctx, cs, sport, dport, op);
		break;
	case IPPROTO_TCP:
		nft_parse_tcp(ctx, cs, sport, dport, op);
		break;
	}
}

static void nft_parse_th_port_range(struct nft_xt_ctx *ctx,
				    struct iptables_command_state *cs,
				    uint8_t proto,
				    int sport_from, int sport_to,
				    int dport_from, int dport_to, uint8_t op)
{
	switch (proto) {
	case IPPROTO_UDP:
		nft_parse_udp_range(ctx, cs, sport_from, sport_to, dport_from, dport_to, op);
		break;
	case IPPROTO_TCP:
		nft_parse_tcp_range(ctx, cs, sport_from, sport_to, dport_from, dport_to, op);
		break;
	}
}

static void nft_parse_tcp_flags(struct nft_xt_ctx *ctx,
				struct iptables_command_state *cs,
				uint8_t op, uint8_t flags, uint8_t mask)
{
	struct xt_tcp *tcp = nft_tcp_match(ctx, cs);

	if (!tcp)
		return;

	if (op == NFT_CMP_NEQ)
		tcp->invflags |= XT_TCP_INV_FLAGS;
	tcp->flg_cmp = flags;
	tcp->flg_mask = mask;
}

static void nft_parse_transport(struct nft_xt_ctx *ctx,
				struct nftnl_expr *e,
				struct iptables_command_state *cs)
{
	uint32_t sdport;
	uint16_t port;
	uint8_t proto, op;
	unsigned int len;

	switch (ctx->h->family) {
	case NFPROTO_IPV4:
		proto = ctx->cs->fw.ip.proto;
		break;
	case NFPROTO_IPV6:
		proto = ctx->cs->fw6.ipv6.proto;
		break;
	default:
		proto = 0;
		break;
	}

	nftnl_expr_get(e, NFTNL_EXPR_CMP_DATA, &len);
	op = nftnl_expr_get_u32(e, NFTNL_EXPR_CMP_OP);

	switch(ctx->payload.offset) {
	case 0: /* th->sport */
		switch (len) {
		case 2: /* load sport only */
			port = ntohs(nftnl_expr_get_u16(e, NFTNL_EXPR_CMP_DATA));
			nft_parse_th_port(ctx, cs, proto, port, -1, op);
			return;
		case 4: /* load both src and dst port */
			sdport = ntohl(nftnl_expr_get_u32(e, NFTNL_EXPR_CMP_DATA));
			nft_parse_th_port(ctx, cs, proto, sdport >> 16, sdport & 0xffff, op);
			return;
		}
		break;
	case 2: /* th->dport */
		switch (len) {
		case 2:
			port = ntohs(nftnl_expr_get_u16(e, NFTNL_EXPR_CMP_DATA));
			nft_parse_th_port(ctx, cs, proto, -1, port, op);
			return;
		}
		break;
	case 13: /* th->flags */
		if (len == 1 && proto == IPPROTO_TCP) {
			uint8_t flags = nftnl_expr_get_u8(e, NFTNL_EXPR_CMP_DATA);
			uint8_t mask = ~0;

			if (ctx->flags & NFT_XT_CTX_BITWISE) {
				memcpy(&mask, &ctx->bitwise.mask, sizeof(mask));
				ctx->flags &= ~NFT_XT_CTX_BITWISE;
			}
			nft_parse_tcp_flags(ctx, cs, op, flags, mask);
		}
		return;
	}
}

static void nft_parse_transport_range(struct nft_xt_ctx *ctx,
				      struct nftnl_expr *e,
				      struct iptables_command_state *cs)
{
	unsigned int len_from, len_to;
	uint8_t proto, op;
	uint16_t from, to;

	switch (ctx->h->family) {
	case NFPROTO_IPV4:
		proto = ctx->cs->fw.ip.proto;
		break;
	case NFPROTO_IPV6:
		proto = ctx->cs->fw6.ipv6.proto;
		break;
	default:
		proto = 0;
		break;
	}

	nftnl_expr_get(e, NFTNL_EXPR_RANGE_FROM_DATA, &len_from);
	nftnl_expr_get(e, NFTNL_EXPR_RANGE_FROM_DATA, &len_to);
	if (len_to != len_from || len_to != 2)
		return;

	op = nftnl_expr_get_u32(e, NFTNL_EXPR_RANGE_OP);

	from = ntohs(nftnl_expr_get_u16(e, NFTNL_EXPR_RANGE_FROM_DATA));
	to = ntohs(nftnl_expr_get_u16(e, NFTNL_EXPR_RANGE_TO_DATA));

	switch(ctx->payload.offset) {
	case 0:
		nft_parse_th_port_range(ctx, cs, proto, from, to, -1, -1, op);
		return;
	case 2:
		to = ntohs(nftnl_expr_get_u16(e, NFTNL_EXPR_RANGE_TO_DATA));
		nft_parse_th_port_range(ctx, cs, proto, -1, -1, from, to, op);
		return;
	}
}

static void nft_parse_cmp(struct nft_xt_ctx *ctx, struct nftnl_expr *e)
{
	uint32_t reg;

	reg = nftnl_expr_get_u32(e, NFTNL_EXPR_CMP_SREG);
	if (ctx->reg && reg != ctx->reg)
		return;

	if (ctx->flags & NFT_XT_CTX_META) {
		ctx->h->ops->parse_meta(ctx, e, ctx->cs);
		ctx->flags &= ~NFT_XT_CTX_META;
	}
	/* bitwise context is interpreted from payload */
	if (ctx->flags & NFT_XT_CTX_PAYLOAD) {
		switch (ctx->payload.base) {
		case NFT_PAYLOAD_LL_HEADER:
			if (ctx->h->family == NFPROTO_BRIDGE)
				ctx->h->ops->parse_payload(ctx, e, ctx->cs);
			break;
		case NFT_PAYLOAD_NETWORK_HEADER:
			ctx->h->ops->parse_payload(ctx, e, ctx->cs);
			break;
		case NFT_PAYLOAD_TRANSPORT_HEADER:
			nft_parse_transport(ctx, e, ctx->cs);
			break;
		}
	}
}

static void nft_parse_counter(struct nftnl_expr *e, struct xt_counters *counters)
{
	counters->pcnt = nftnl_expr_get_u64(e, NFTNL_EXPR_CTR_PACKETS);
	counters->bcnt = nftnl_expr_get_u64(e, NFTNL_EXPR_CTR_BYTES);
}

static void nft_parse_immediate(struct nft_xt_ctx *ctx, struct nftnl_expr *e)
{
	const char *chain = nftnl_expr_get_str(e, NFTNL_EXPR_IMM_CHAIN);
	struct iptables_command_state *cs = ctx->cs;
	struct xt_entry_target *t;
	uint32_t size;
	int verdict;

	if (nftnl_expr_is_set(e, NFTNL_EXPR_IMM_DATA)) {
		const void *imm_data;
		uint32_t len;

		imm_data = nftnl_expr_get_data(e, NFTNL_EXPR_IMM_DATA, &len);

		if (len > sizeof(ctx->immediate.data))
			return;

		memcpy(ctx->immediate.data, imm_data, len);
		ctx->immediate.len = len;
		ctx->immediate.reg = nftnl_expr_get_u32(e, NFTNL_EXPR_IMM_DREG);
		ctx->flags |= NFT_XT_CTX_IMMEDIATE;
		return;
	}

	verdict = nftnl_expr_get_u32(e, NFTNL_EXPR_IMM_VERDICT);
	/* Standard target? */
	switch(verdict) {
	case NF_ACCEPT:
		cs->jumpto = "ACCEPT";
		break;
	case NF_DROP:
		cs->jumpto = "DROP";
		break;
	case NFT_RETURN:
		cs->jumpto = "RETURN";
		break;;
	case NFT_GOTO:
		if (ctx->h->ops->set_goto_flag)
			ctx->h->ops->set_goto_flag(cs);
		/* fall through */
	case NFT_JUMP:
		cs->jumpto = chain;
		/* fall through */
	default:
		return;
	}

	cs->target = xtables_find_target(cs->jumpto, XTF_TRY_LOAD);
	if (!cs->target)
		return;

	size = XT_ALIGN(sizeof(struct xt_entry_target)) + cs->target->size;
	t = xtables_calloc(1, size);
	t->u.target_size = size;
	t->u.user.revision = cs->target->revision;
	strcpy(t->u.user.name, cs->jumpto);
	cs->target->t = t;
}

static void nft_parse_limit(struct nft_xt_ctx *ctx, struct nftnl_expr *e)
{
	__u32 burst = nftnl_expr_get_u32(e, NFTNL_EXPR_LIMIT_BURST);
	__u64 unit = nftnl_expr_get_u64(e, NFTNL_EXPR_LIMIT_UNIT);
	__u64 rate = nftnl_expr_get_u64(e, NFTNL_EXPR_LIMIT_RATE);
	struct xtables_rule_match **matches;
	struct xtables_match *match;
	struct xt_rateinfo *rinfo;
	size_t size;

	switch (ctx->h->family) {
	case NFPROTO_IPV4:
	case NFPROTO_IPV6:
	case NFPROTO_BRIDGE:
		matches = &ctx->cs->matches;
		break;
	default:
		fprintf(stderr, "BUG: nft_parse_limit() unknown family %d\n",
			ctx->h->family);
		exit(EXIT_FAILURE);
	}

	match = xtables_find_match("limit", XTF_TRY_LOAD, matches);
	if (match == NULL)
		return;

	size = XT_ALIGN(sizeof(struct xt_entry_match)) + match->size;
	match->m = xtables_calloc(1, size);
	match->m->u.match_size = size;
	strcpy(match->m->u.user.name, match->name);
	match->m->u.user.revision = match->revision;
	xs_init_match(match);

	rinfo = (void *)match->m->data;
	rinfo->avg = XT_LIMIT_SCALE * unit / rate;
	rinfo->burst = burst;

	if (ctx->h->ops->parse_match != NULL)
		ctx->h->ops->parse_match(match, ctx->cs);
}

static void nft_parse_log(struct nft_xt_ctx *ctx, struct nftnl_expr *e)
{
	struct xtables_target *target;
	struct xt_entry_target *t;
	size_t target_size;
	/*
	 * In order to handle the longer log-prefix supported by nft, instead of
	 * using struct xt_nflog_info, we use a struct with a compatible layout, but
	 * a larger buffer for the prefix.
	 */
	struct xt_nflog_info_nft {
		__u32 len;
		__u16 group;
		__u16 threshold;
		__u16 flags;
		__u16 pad;
		char  prefix[NF_LOG_PREFIXLEN];
	} info = {
		.group     = nftnl_expr_get_u16(e, NFTNL_EXPR_LOG_GROUP),
		.threshold = nftnl_expr_get_u16(e, NFTNL_EXPR_LOG_QTHRESHOLD),
	};
	if (nftnl_expr_is_set(e, NFTNL_EXPR_LOG_SNAPLEN)) {
		info.len = nftnl_expr_get_u32(e, NFTNL_EXPR_LOG_SNAPLEN);
		info.flags = XT_NFLOG_F_COPY_LEN;
	}
	if (nftnl_expr_is_set(e, NFTNL_EXPR_LOG_PREFIX))
		snprintf(info.prefix, sizeof(info.prefix), "%s",
			 nftnl_expr_get_str(e, NFTNL_EXPR_LOG_PREFIX));

	target = xtables_find_target("NFLOG", XTF_TRY_LOAD);
	if (target == NULL)
		return;

	target_size = XT_ALIGN(sizeof(struct xt_entry_target)) +
		      XT_ALIGN(sizeof(struct xt_nflog_info_nft));

	t = xtables_calloc(1, target_size);
	t->u.target_size = target_size;
	strcpy(t->u.user.name, target->name);
	t->u.user.revision = target->revision;

	target->t = t;

	memcpy(&target->t->data, &info, sizeof(info));

	ctx->h->ops->parse_target(target, ctx->cs);
}

static void nft_parse_lookup(struct nft_xt_ctx *ctx, struct nft_handle *h,
			     struct nftnl_expr *e)
{
	if (ctx->h->ops->parse_lookup)
		ctx->h->ops->parse_lookup(ctx, e);
}

static void nft_parse_range(struct nft_xt_ctx *ctx, struct nftnl_expr *e)
{
	uint32_t reg;

	reg = nftnl_expr_get_u32(e, NFTNL_EXPR_RANGE_SREG);
	if (reg != ctx->reg)
		return;

	if (ctx->flags & NFT_XT_CTX_PAYLOAD) {
		switch (ctx->payload.base) {
		case NFT_PAYLOAD_TRANSPORT_HEADER:
			nft_parse_transport_range(ctx, e, ctx->cs);
			break;
		default:
			break;
		}
	}
}

void nft_rule_to_iptables_command_state(struct nft_handle *h,
					const struct nftnl_rule *r,
					struct iptables_command_state *cs)
{
	struct nftnl_expr_iter *iter;
	struct nftnl_expr *expr;
	struct nft_xt_ctx ctx = {
		.cs = cs,
		.h = h,
		.table = nftnl_rule_get_str(r, NFTNL_RULE_TABLE),
	};

	iter = nftnl_expr_iter_create(r);
	if (iter == NULL)
		return;

	ctx.iter = iter;
	expr = nftnl_expr_iter_next(iter);
	while (expr != NULL) {
		const char *name =
			nftnl_expr_get_str(expr, NFTNL_EXPR_NAME);

		if (strcmp(name, "counter") == 0)
			nft_parse_counter(expr, &ctx.cs->counters);
		else if (strcmp(name, "payload") == 0)
			nft_parse_payload(&ctx, expr);
		else if (strcmp(name, "meta") == 0)
			nft_parse_meta(&ctx, expr);
		else if (strcmp(name, "bitwise") == 0)
			nft_parse_bitwise(&ctx, expr);
		else if (strcmp(name, "cmp") == 0)
			nft_parse_cmp(&ctx, expr);
		else if (strcmp(name, "immediate") == 0)
			nft_parse_immediate(&ctx, expr);
		else if (strcmp(name, "match") == 0)
			nft_parse_match(&ctx, expr);
		else if (strcmp(name, "target") == 0)
			nft_parse_target(&ctx, expr);
		else if (strcmp(name, "limit") == 0)
			nft_parse_limit(&ctx, expr);
		else if (strcmp(name, "lookup") == 0)
			nft_parse_lookup(&ctx, h, expr);
		else if (strcmp(name, "log") == 0)
			nft_parse_log(&ctx, expr);
		else if (strcmp(name, "range") == 0)
			nft_parse_range(&ctx, expr);

		expr = nftnl_expr_iter_next(iter);
	}

	nftnl_expr_iter_destroy(iter);

	if (nftnl_rule_is_set(r, NFTNL_RULE_USERDATA)) {
		const void *data;
		uint32_t len, size;
		const char *comment;

		data = nftnl_rule_get_data(r, NFTNL_RULE_USERDATA, &len);
		comment = get_comment(data, len);
		if (comment) {
			struct xtables_match *match;
			struct xt_entry_match *m;

			match = xtables_find_match("comment", XTF_TRY_LOAD,
						   &cs->matches);
			if (match == NULL)
				return;

			size = XT_ALIGN(sizeof(struct xt_entry_match))
				+ match->size;
			m = xtables_calloc(1, size);

			strncpy((char *)m->data, comment, match->size - 1);
			m->u.match_size = size;
			m->u.user.revision = 0;
			strcpy(m->u.user.name, match->name);

			match->m = m;
		}
	}

	if (!cs->jumpto)
		cs->jumpto = "";
}

void nft_clear_iptables_command_state(struct iptables_command_state *cs)
{
	xtables_rule_matches_free(&cs->matches);
	if (cs->target) {
		free(cs->target->t);
		cs->target->t = NULL;

		if (cs->target == cs->target->next) {
			free(cs->target);
			cs->target = NULL;
		}
	}
}

void nft_ipv46_save_chain(const struct nftnl_chain *c, const char *policy)
{
	const char *chain = nftnl_chain_get_str(c, NFTNL_CHAIN_NAME);
	uint64_t pkts = nftnl_chain_get_u64(c, NFTNL_CHAIN_PACKETS);
	uint64_t bytes = nftnl_chain_get_u64(c, NFTNL_CHAIN_BYTES);

	printf(":%s %s [%"PRIu64":%"PRIu64"]\n",
	       chain, policy ?: "-", pkts, bytes);
}

void save_matches_and_target(const struct iptables_command_state *cs,
			     bool goto_flag, const void *fw,
			     unsigned int format)
{
	struct xtables_rule_match *matchp;

	for (matchp = cs->matches; matchp; matchp = matchp->next) {
		if (matchp->match->alias) {
			printf(" -m %s",
			       matchp->match->alias(matchp->match->m));
		} else
			printf(" -m %s", matchp->match->name);

		if (matchp->match->save != NULL) {
			/* cs->fw union makes the trick */
			matchp->match->save(fw, matchp->match->m);
		}
	}

	if ((format & (FMT_NOCOUNTS | FMT_C_COUNTS)) == FMT_C_COUNTS)
		printf(" -c %llu %llu",
		       (unsigned long long)cs->counters.pcnt,
		       (unsigned long long)cs->counters.bcnt);

	if (cs->target != NULL) {
		if (cs->target->alias) {
			printf(" -j %s", cs->target->alias(cs->target->t));
		} else
			printf(" -j %s", cs->jumpto);

		if (cs->target->save != NULL) {
			cs->target->save(fw, cs->target->t);
		}
	} else if (strlen(cs->jumpto) > 0) {
		printf(" -%c %s", goto_flag ? 'g' : 'j', cs->jumpto);
	}

	printf("\n");
}

void print_matches_and_target(struct iptables_command_state *cs,
			      unsigned int format)
{
	struct xtables_rule_match *matchp;

	for (matchp = cs->matches; matchp; matchp = matchp->next) {
		if (matchp->match->print != NULL) {
			matchp->match->print(&cs->fw, matchp->match->m,
					     format & FMT_NUMERIC);
		}
	}

	if (cs->target != NULL) {
		if (cs->target->print != NULL) {
			cs->target->print(&cs->fw, cs->target->t,
					  format & FMT_NUMERIC);
		}
	}
}

struct nft_family_ops *nft_family_ops_lookup(int family)
{
	switch (family) {
	case AF_INET:
		return &nft_family_ops_ipv4;
	case AF_INET6:
		return &nft_family_ops_ipv6;
	case NFPROTO_ARP:
		return &nft_family_ops_arp;
	case NFPROTO_BRIDGE:
		return &nft_family_ops_bridge;
	default:
		break;
	}

	return NULL;
}

bool compare_matches(struct xtables_rule_match *mt1,
		     struct xtables_rule_match *mt2)
{
	struct xtables_rule_match *mp1;
	struct xtables_rule_match *mp2;

	for (mp1 = mt1, mp2 = mt2; mp1 && mp2; mp1 = mp1->next, mp2 = mp2->next) {
		struct xt_entry_match *m1 = mp1->match->m;
		struct xt_entry_match *m2 = mp2->match->m;

		if (strcmp(m1->u.user.name, m2->u.user.name) != 0) {
			DEBUGP("mismatching match name\n");
			return false;
		}

		if (m1->u.user.match_size != m2->u.user.match_size) {
			DEBUGP("mismatching match size\n");
			return false;
		}

		if (memcmp(m1->data, m2->data,
			   mp1->match->userspacesize) != 0) {
			DEBUGP("mismatch match data\n");
			return false;
		}
	}

	/* Both cursors should be NULL */
	if (mp1 != mp2) {
		DEBUGP("mismatch matches amount\n");
		return false;
	}

	return true;
}

bool compare_targets(struct xtables_target *tg1, struct xtables_target *tg2)
{
	if (tg1 == NULL && tg2 == NULL)
		return true;

	if (tg1 == NULL || tg2 == NULL)
		return false;
	if (tg1->userspacesize != tg2->userspacesize)
		return false;

	if (strcmp(tg1->t->u.user.name, tg2->t->u.user.name) != 0)
		return false;

	if (memcmp(tg1->t->data, tg2->t->data, tg1->userspacesize) != 0)
		return false;

	return true;
}

void nft_ipv46_parse_target(struct xtables_target *t,
			    struct iptables_command_state *cs)
{
	cs->target = t;
	cs->jumpto = t->name;
}

void nft_check_xt_legacy(int family, bool is_ipt_save)
{
	static const char tables6[] = "/proc/net/ip6_tables_names";
	static const char tables4[] = "/proc/net/ip_tables_names";
	static const char tablesa[] = "/proc/net/arp_tables_names";
	const char *prefix = "ip";
	FILE *fp = NULL;
	char buf[1024];

	switch (family) {
	case NFPROTO_IPV4:
		fp = fopen(tables4, "r");
		break;
	case NFPROTO_IPV6:
		fp = fopen(tables6, "r");
		prefix = "ip6";
		break;
	case NFPROTO_ARP:
		fp = fopen(tablesa, "r");
		prefix = "arp";
		break;
	default:
		break;
	}

	if (!fp)
		return;

	if (fgets(buf, sizeof(buf), fp))
		fprintf(stderr, "# Warning: %stables-legacy tables present, use %stables-legacy%s to see them\n",
			prefix, prefix, is_ipt_save ? "-save" : "");
	fclose(fp);
}
