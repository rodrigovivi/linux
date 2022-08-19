/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_RTP_
#define _XE_RTP_

#include <linux/xarray.h>
#include <linux/types.h>

#include "xe_rtp_types.h"

#include "i915_reg_defs.h"

/*
 * Register table poke infrastructure
 */

struct xe_hw_engine;
struct xe_gt;
struct xe_reg_sr;

/*
 * Helper macros - not to be used outside this header.
 */
/* This counts to 12. Any more, it will return 13th argument. */
#define __COUNT_ARGS(_0, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _n, X...) _n
#define COUNT_ARGS(X...) __COUNT_ARGS(, ##X, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)

#define __CONCAT(a, b) a ## b
#define CONCATENATE(a, b) __CONCAT(a, b)

#define __CALL_FOR_EACH_1(MACRO_, x, ...) MACRO_(x)
#define __CALL_FOR_EACH_2(MACRO_, x, ...)					\
	MACRO_(x) __CALL_FOR_EACH_1(MACRO_, ##__VA_ARGS__)
#define __CALL_FOR_EACH_3(MACRO_, x, ...)					\
	MACRO_(x) __CALL_FOR_EACH_2(MACRO_, ##__VA_ARGS__)
#define __CALL_FOR_EACH_4(MACRO_, x, ...)					\
	MACRO_(x) __CALL_FOR_EACH_3(MACRO_, ##__VA_ARGS__)

#define _CALL_FOR_EACH(NARGS_, MACRO_, x, ...)					\
	CONCATENATE(__CALL_FOR_EACH_, NARGS_)(MACRO_, x, ##__VA_ARGS__)
#define CALL_FOR_EACH(MACRO_, x, ...)						\
	_CALL_FOR_EACH(COUNT_ARGS(x, ##__VA_ARGS__), MACRO_, x, ##__VA_ARGS__)

/*
 * Helper macros for concatenating prefix - do not use them directly outside
 * this header
 */
#define __ADD_XE_RTP_FLAG_PREFIX(x) CONCATENATE(XE_RTP_FLAG_, x) |
#define __ADD_XE_RTP_RULE_PREFIX(x) CONCATENATE(XE_RTP_RULE_, x) ,

/*
 * Macros to encode rules to match against platform, IP version, stepping, etc.
 * Shouldn't be used directly - see XE_RTP_RULES()
 */
#define _XE_RTP_RULE_PLATFORM(plat__)						\
	{ .match_type = XE_RTP_MATCH_PLATFORM, .platform = plat__ }
#define XE_RTP_RULE_PLATFORM(plat_)						\
	_XE_RTP_RULE_PLATFORM(XE_##plat_)

#define _XE_RTP_RULE_SUBPLATFORM(plat__, sub__)					\
	{ .match_type = XE_RTP_MATCH_SUBPLATFORM,				\
	  .platform = plat__, .subplatform = sub__ }
#define XE_RTP_RULE_SUBPLATFORM(plat_, sub_)					\
	_XE_RTP_RULE_SUBPLATFORM(XE_##plat_, XE_SUBPLATFORM_##plat_##_##sub_)

#define _XE_RTP_RULE_STEP(start__, end__)					\
	{ .match_type = XE_RTP_MATCH_STEP,					\
	  .step_start = start__, .step_end = end__ }
#define XE_RTP_RULE_STEP(start_, end_)						\
	_XE_RTP_RULE_STEP(STEP_##start_, STEP_##end_)

#define _XE_RTP_RULE_ENGINE_CLASS(cls__)					\
	{ .match_type = XE_RTP_MATCH_ENGINE_CLASS,				\
	  .engine_class = (cls__) }
#define XE_RTP_RULE_ENGINE_CLASS(cls_)						\
	_XE_RTP_RULE_ENGINE_CLASS(XE_ENGINE_CLASS_##cls_)

#define XE_RTP_RULE_FUNC(func__)						\
	{ .match_type = XE_RTP_MATCH_FUNC,					\
	  .match_func = (func__) }

/**
 * @XE_RTP_WR: Helper to write @val_ to a register, overriding all the bits. The
 * correspondent notation in bspec is:
 *
 * REGNAME = VALUE
 */
#define XE_RTP_WR(reg_, val_, ...)						\
	.regval = { .reg = (reg_), .clr_bits = ~0u, .set_bits = (val_),		\
		    .read_mask = (~0u), ##__VA_ARGS__ }

/**
 * @XE_RTP_SET: Set bits from @val_ in the register.
 *
 * For masked registers this translates to a single write, while for other
 * registers it's a RMW. The correspondent bspec notation is (example for bits 2
 * and 5 but could be any):
 *
 * REGNAME[2] = 1
 * REGNAME[5] = 1
 */
#define XE_RTP_SET(reg_, val_, ...)						\
	.regval = { .reg = (reg_), .clr_bits = (val_), .set_bits = (val_),	\
		    .read_mask = (val_), ##__VA_ARGS__ }

/**
 * @XE_RTP_CLR: Clear bits from @val_ in the register.
 *
 * For masked registers this translates to a single write, while for other
 * registers it's a RMW. The correspondent bspec notation is (example for bits 2
 * and 5 but could be any):
 *
 * REGNAME[2] = 0
 * REGNAME[5] = 0
 */
#define XE_RTP_CLR(reg_, val_, ...)						\
	.regval = { .reg = (reg_), .clr_bits = (val_), .set_bits = 0,		\
		    .read_mask = (val_), ##__VA_ARGS__ }

/**
 * @XE_RTP_FIELD_SET: Set a bit range, defined by @mask_bits_, to the value in
 * @val_.
 *
 * For masked registers this translates to a single write, while for other
 * registers it's a RMW. The correspondent bspec notation is:
 *
 * REGNAME[<end>:<start>] = VALUE
 */
#define XE_RTP_FIELD_SET(reg_, mask_bits_, val_, ...)				\
	.regval = { .reg = (reg_), .clr_bits = (mask_bits_), .set_bits = (val_),\
		    .read_mask = (mask_bits_), ##__VA_ARGS__ }

/**
 * @XE_RTP_NAME: Helper to set the name in xe_rtp_entry
 *
 * TODO: maybe move this behind a debug config?
 */
#define XE_RTP_NAME(s_)	.name = (s_)

/**
 * @XE_RTP_FLAG: Helper to add flags to a xe_rtp_regval entry without needing the
 * XE_RTP_FLAG_ prefix. Example:
 *
 * { XE_RTP_NAME("test-entry"),
 *   XE_RTP_FLAG(FOREACH_ENGINE, MASKED_REG),
 *   ...
 * }
 */
#define XE_RTP_FLAG(f1_, ...)							\
	.flags = (CALL_FOR_EACH(__ADD_XE_RTP_FLAG_PREFIX, f1_, ##__VA_ARGS__) 0)

/**
 * @XE_RTP_RULES: Helper to set the rules to a xe_rtp_entry
 *
 * At least one rule is needed and up to 4 are supported. Multiple rules are
 * AND'ed together, i.e. all the rules must evaluate to true for the entry to
 * be processed. See XE_RTP_MATCH_* for the possible match rules. Example:
 *
 *
 * { XE_RTP_NAME("test-entry"),
 *   XE_RTP_RULES(SUBPLATFORM(DG2, G10), STEP(A0, B0)),
 *   ...
 * }
 */
#define XE_RTP_RULES(r1, ...)							\
	.n_rules = COUNT_ARGS(r1, ##__VA_ARGS__),				\
	.rules = (struct xe_rtp_rule[]) {					\
		CALL_FOR_EACH(__ADD_XE_RTP_RULE_PREFIX, r1, ##__VA_ARGS__)	\
	}

/**
 * @xe_rtp_process: Process all rtp @entries from @entries adding them to @sr.
 *
 * Walk the table pointed by @entries (with an empty sentinel) and add all
 * entries with matching rules to @sr. If @hwe is not NULL, its mmio_base is
 * used to calculate the right register offset
 */
void xe_rtp_process(const struct xe_rtp_entry *entries, struct xe_reg_sr *sr,
		    struct xe_gt *gt, struct xe_hw_engine *hwe);

#endif
