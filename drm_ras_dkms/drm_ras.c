// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 *
 * Standalone backport of the DRM RAS Generic Netlink core to linux-5.15.y.
 *
 * Upstream splits this across drivers/gpu/drm/drm_ras.c (handlers + node
 * management) and an auto-generated drm_ras_nl.c (genl family, using the modern
 * genl_split_ops / genlmsg_iput / genl_info_dump / GENL_REQ_ATTR_CHECK /
 * genl_info_init_ntf helpers). None of those exist in 5.15, so the family here
 * is expressed with classic struct genl_ops (one op carries both .doit and
 * .dumpit), family-level .policy/.mcgrps, and genlmsg_put()/genlmsg_put_reply()/
 * genlmsg_reply()/genlmsg_multicast_allns().
 *
 * The module has no DRM subsystem dependency: it is pure Generic Netlink plus
 * an xarray, and exports the node register/unregister/event API for consumers
 * (e.g. blah.ko) to link against.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/xarray.h>

#include <net/netlink.h>
#include <net/genetlink.h>

#include "drm_ras.h"

/**
 * DOC: DRM RAS Node Management
 *
 * Each driver may register one or more RAS nodes, stored in a global xarray
 * and exposed to userspace through the `drm-ras` Generic Netlink family:
 *
 * 1. LIST_NODES: dump all registered nodes (id, names, type).
 * 2. GET_ERROR_COUNTER: get one counter (doit) or all counters of a node (dump).
 * 3. CLEAR_ERROR_COUNTER: clear one counter of a node (doit).
 * 4. ERROR_EVENT: multicast an error event to the "error-report" group.
 *
 * For error-counter nodes the driver must implement query_error_counter() and
 * provide error_counter_range.last. Ranges may be non-contiguous: the driver
 * returns -ENOENT for unimplemented IDs so drm-ras skips them in a dump.
 */

static DEFINE_XARRAY_ALLOC(drm_ras_xa);

/*
 * The netlink dump context carries state across multiple dumpit calls. It is
 * reused by both dumps: @restart is an xarray id for LIST_NODES and an error
 * id for GET_ERROR_COUNTER.
 */
struct drm_ras_ctx {
	unsigned long restart;
};

/* Family-wide request policy (covers the error-counter request attributes). */
static const struct nla_policy
drm_ras_nl_policy[DRM_RAS_A_ERROR_COUNTER_ATTRS_MAX + 1] = {
	[DRM_RAS_A_ERROR_COUNTER_ATTRS_NODE_ID]  = { .type = NLA_U32 },
	[DRM_RAS_A_ERROR_COUNTER_ATTRS_ERROR_ID] = { .type = NLA_U32 },
};

enum {
	DRM_RAS_NLGRP_ERROR_REPORT,
};

static const struct genl_multicast_group drm_ras_mcgrps[] = {
	[DRM_RAS_NLGRP_ERROR_REPORT] = { .name = DRM_RAS_MCGRP_ERROR_REPORT },
};

/* Forward declarations of the netlink handlers. */
static int drm_ras_nl_list_nodes_dumpit(struct sk_buff *skb,
					struct netlink_callback *cb);
static int drm_ras_nl_get_error_counter_doit(struct sk_buff *skb,
					     struct genl_info *info);
static int drm_ras_nl_get_error_counter_dumpit(struct sk_buff *skb,
						struct netlink_callback *cb);
static int drm_ras_nl_clear_error_counter_doit(struct sk_buff *skb,
					       struct genl_info *info);

static const struct genl_ops drm_ras_nl_ops[] = {
	{
		.cmd	= DRM_RAS_CMD_LIST_NODES,
		.dumpit	= drm_ras_nl_list_nodes_dumpit,
		.flags	= GENL_ADMIN_PERM,
	},
	{
		/*
		 * A single 5.15 op holds both callbacks; genl dispatches to
		 * .dumpit when NLM_F_DUMP is set, otherwise to .doit.
		 */
		.cmd	= DRM_RAS_CMD_GET_ERROR_COUNTER,
		.doit	= drm_ras_nl_get_error_counter_doit,
		.dumpit	= drm_ras_nl_get_error_counter_dumpit,
		.flags	= GENL_ADMIN_PERM,
	},
	{
		.cmd	= DRM_RAS_CMD_CLEAR_ERROR_COUNTER,
		.doit	= drm_ras_nl_clear_error_counter_doit,
		.flags	= GENL_ADMIN_PERM,
	},
};

static struct genl_family drm_ras_nl_family __ro_after_init = {
	.name		= DRM_RAS_FAMILY_NAME,
	.version	= DRM_RAS_FAMILY_VERSION,
	.maxattr	= DRM_RAS_A_ERROR_COUNTER_ATTRS_MAX,
	.policy		= drm_ras_nl_policy,
	.netnsok	= true,
	.parallel_ops	= true,
	.module		= THIS_MODULE,
	.ops		= drm_ras_nl_ops,
	.n_ops		= ARRAY_SIZE(drm_ras_nl_ops),
	.mcgrps		= drm_ras_mcgrps,
	.n_mcgrps	= ARRAY_SIZE(drm_ras_mcgrps),
};

static int get_node_error_counter(u32 node_id, u32 error_id,
				  const char **name, u32 *value)
{
	struct drm_ras_node *node;

	node = xa_load(&drm_ras_xa, node_id);
	if (!node || !node->query_error_counter)
		return -ENOENT;

	if (error_id < node->error_counter_range.first ||
	    error_id > node->error_counter_range.last)
		return -EINVAL;

	return node->query_error_counter(node, error_id, name, value);
}

static int msg_reply_value(struct sk_buff *msg, u32 error_id,
			   const char *error_name, u32 value)
{
	int ret;

	ret = nla_put_u32(msg, DRM_RAS_A_ERROR_COUNTER_ATTRS_ERROR_ID, error_id);
	if (ret)
		return ret;

	ret = nla_put_string(msg, DRM_RAS_A_ERROR_COUNTER_ATTRS_ERROR_NAME,
			     error_name);
	if (ret)
		return ret;

	return nla_put_u32(msg, DRM_RAS_A_ERROR_COUNTER_ATTRS_ERROR_VALUE,
			   value);
}

static int msg_put_error_event_attrs(struct sk_buff *msg,
				      struct drm_ras_node *node, u32 error_id,
				      const char *error_name, u32 value)
{
	int ret;

	ret = nla_put_string(msg, DRM_RAS_A_ERROR_EVENT_ATTRS_DEVICE_NAME,
			     node->device_name);
	if (ret)
		return ret;

	ret = nla_put_u32(msg, DRM_RAS_A_ERROR_EVENT_ATTRS_NODE_ID, node->id);
	if (ret)
		return ret;

	ret = nla_put_string(msg, DRM_RAS_A_ERROR_EVENT_ATTRS_NODE_NAME,
			     node->node_name);
	if (ret)
		return ret;

	ret = nla_put_u32(msg, DRM_RAS_A_ERROR_EVENT_ATTRS_ERROR_ID, error_id);
	if (ret)
		return ret;

	ret = nla_put_string(msg, DRM_RAS_A_ERROR_EVENT_ATTRS_ERROR_NAME,
			     error_name);
	if (ret)
		return ret;

	return nla_put_u32(msg, DRM_RAS_A_ERROR_EVENT_ATTRS_ERROR_VALUE, value);
}

/**
 * drm_ras_nl_list_nodes_dumpit() - Dump all registered RAS nodes
 *
 * Return: 0 when the dump is complete, a positive skb length to be called
 *         again for continuation, or a negative error code.
 */
static int drm_ras_nl_list_nodes_dumpit(struct sk_buff *skb,
					struct netlink_callback *cb)
{
	struct drm_ras_ctx *ctx = (void *)cb->ctx;
	struct drm_ras_node *node;
	unsigned long id;
	void *hdr;
	int ret = 0;

	xa_for_each_start(&drm_ras_xa, id, node, ctx->restart) {
		hdr = genlmsg_put(skb, NETLINK_CB(cb->skb).portid,
				  cb->nlh->nlmsg_seq, &drm_ras_nl_family,
				  NLM_F_MULTI, DRM_RAS_CMD_LIST_NODES);
		if (!hdr) {
			ret = -EMSGSIZE;
			break;
		}

		if (nla_put_u32(skb, DRM_RAS_A_NODE_ATTRS_NODE_ID, node->id) ||
		    nla_put_string(skb, DRM_RAS_A_NODE_ATTRS_DEVICE_NAME,
				   node->device_name) ||
		    nla_put_string(skb, DRM_RAS_A_NODE_ATTRS_NODE_NAME,
				   node->node_name) ||
		    nla_put_u32(skb, DRM_RAS_A_NODE_ATTRS_NODE_TYPE,
				node->type)) {
			genlmsg_cancel(skb, hdr);
			ret = -EMSGSIZE;
			break;
		}

		genlmsg_end(skb, hdr);
	}

	if (ret == -EMSGSIZE) {
		ctx->restart = id;
		return skb->len;
	}

	return 0;
}

/**
 * drm_ras_nl_get_error_counter_dumpit() - Dump all error counters of a node
 *
 * Return: 0 when the dump is complete, a positive skb length to continue, or
 *         a negative error code.
 */
static int drm_ras_nl_get_error_counter_dumpit(struct sk_buff *skb,
						struct netlink_callback *cb)
{
	struct nlattr *tb[DRM_RAS_A_ERROR_COUNTER_ATTRS_MAX + 1];
	struct drm_ras_ctx *ctx = (void *)cb->ctx;
	struct drm_ras_node *node;
	const char *error_name;
	u32 node_id, error_id, value;
	void *hdr;
	int ret;

	/*
	 * 5.15 does not hand a genl_info to dumpit, so parse the request
	 * attributes straight from the dump request header.
	 */
	ret = genlmsg_parse(cb->nlh, &drm_ras_nl_family, tb,
			    DRM_RAS_A_ERROR_COUNTER_ATTRS_MAX,
			    drm_ras_nl_policy, NULL);
	if (ret)
		return ret;

	if (!tb[DRM_RAS_A_ERROR_COUNTER_ATTRS_NODE_ID])
		return -EINVAL;

	node_id = nla_get_u32(tb[DRM_RAS_A_ERROR_COUNTER_ATTRS_NODE_ID]);

	node = xa_load(&drm_ras_xa, node_id);
	if (!node)
		return -ENOENT;

	ret = 0;
	for (error_id = max_t(u32, node->error_counter_range.first,
			      ctx->restart);
	     error_id <= node->error_counter_range.last;
	     error_id++) {
		ret = get_node_error_counter(node_id, error_id,
					     &error_name, &value);
		/*
		 * For non-contiguous ranges the driver returns -ENOENT to mean
		 * "skip this ID when listing all errors".
		 */
		if (ret == -ENOENT) {
			ret = 0;
			continue;
		}
		if (ret)
			return ret;

		hdr = genlmsg_put(skb, NETLINK_CB(cb->skb).portid,
				  cb->nlh->nlmsg_seq, &drm_ras_nl_family,
				  NLM_F_MULTI, DRM_RAS_CMD_GET_ERROR_COUNTER);
		if (!hdr) {
			ret = -EMSGSIZE;
			break;
		}

		if (msg_reply_value(skb, error_id, error_name, value)) {
			genlmsg_cancel(skb, hdr);
			ret = -EMSGSIZE;
			break;
		}

		genlmsg_end(skb, hdr);
	}

	if (ret == -EMSGSIZE) {
		ctx->restart = error_id;
		return skb->len;
	}

	return 0;
}

static int doit_reply_value(struct genl_info *info, u32 node_id, u32 error_id)
{
	const char *error_name;
	struct sk_buff *msg;
	void *hdr;
	u32 value;
	int ret;

	/* Query first so a failed lookup never leaks a half-built reply. */
	ret = get_node_error_counter(node_id, error_id, &error_name, &value);
	if (ret)
		return ret;

	msg = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	hdr = genlmsg_put_reply(msg, info, &drm_ras_nl_family, 0,
				DRM_RAS_CMD_GET_ERROR_COUNTER);
	if (!hdr) {
		ret = -EMSGSIZE;
		goto free_msg;
	}

	ret = msg_reply_value(msg, error_id, error_name, value);
	if (ret)
		goto cancel_msg;

	genlmsg_end(msg, hdr);

	return genlmsg_reply(msg, info);

cancel_msg:
	genlmsg_cancel(msg, hdr);
free_msg:
	nlmsg_free(msg);
	return ret;
}

/**
 * drm_ras_nl_get_error_counter_doit() - Query one error counter of a node
 */
static int drm_ras_nl_get_error_counter_doit(struct sk_buff *skb,
					      struct genl_info *info)
{
	u32 node_id, error_id;

	if (!info->attrs[DRM_RAS_A_ERROR_COUNTER_ATTRS_NODE_ID] ||
	    !info->attrs[DRM_RAS_A_ERROR_COUNTER_ATTRS_ERROR_ID])
		return -EINVAL;

	node_id = nla_get_u32(info->attrs[DRM_RAS_A_ERROR_COUNTER_ATTRS_NODE_ID]);
	error_id = nla_get_u32(info->attrs[DRM_RAS_A_ERROR_COUNTER_ATTRS_ERROR_ID]);

	return doit_reply_value(info, node_id, error_id);
}

/**
 * drm_ras_nl_clear_error_counter_doit() - Clear one error counter of a node
 */
static int drm_ras_nl_clear_error_counter_doit(struct sk_buff *skb,
					       struct genl_info *info)
{
	struct drm_ras_node *node;
	u32 node_id, error_id;

	if (!info->attrs[DRM_RAS_A_ERROR_COUNTER_ATTRS_NODE_ID] ||
	    !info->attrs[DRM_RAS_A_ERROR_COUNTER_ATTRS_ERROR_ID])
		return -EINVAL;

	node_id = nla_get_u32(info->attrs[DRM_RAS_A_ERROR_COUNTER_ATTRS_NODE_ID]);
	error_id = nla_get_u32(info->attrs[DRM_RAS_A_ERROR_COUNTER_ATTRS_ERROR_ID]);

	node = xa_load(&drm_ras_xa, node_id);
	if (!node || !node->clear_error_counter)
		return -ENOENT;

	if (error_id < node->error_counter_range.first ||
	    error_id > node->error_counter_range.last)
		return -EINVAL;

	return node->clear_error_counter(node, error_id);
}

/**
 * drm_ras_nl_error_event() - Report an error event to userspace
 * @node: Node structure
 * @error_id: ID of the error
 * @error_name: Name of the error
 * @value: Value of the error counter
 *
 * Multicast an error event on the "error-report" group.
 *
 * Context: Process context only (uses %GFP_KERNEL and multicasts to all
 *          netns). Callers in atomic context must defer.
 *
 * Return: 0 on success, or negative errno on failure.
 */
int drm_ras_nl_error_event(struct drm_ras_node *node, u32 error_id,
			   const char *error_name, u32 value)
{
	struct sk_buff *msg;
	void *hdr;
	int ret;

	if (!node || !error_name)
		return -EINVAL;

	/* Check the node is currently registered */
	if (xa_load(&drm_ras_xa, node->id) != node)
		return -ENOENT;

	/* Currently only Error Counter events are supported */
	if (node->type != DRM_RAS_NODE_TYPE_ERROR_COUNTER)
		return -EOPNOTSUPP;

	/* Check the error ID is within the valid range */
	if (error_id < node->error_counter_range.first ||
	    error_id > node->error_counter_range.last)
		return -EINVAL;

	msg = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	hdr = genlmsg_put(msg, 0, 0, &drm_ras_nl_family, 0,
			  DRM_RAS_CMD_ERROR_EVENT);
	if (!hdr) {
		ret = -EMSGSIZE;
		goto free_msg;
	}

	ret = msg_put_error_event_attrs(msg, node, error_id, error_name, value);
	if (ret)
		goto cancel_msg;

	genlmsg_end(msg, hdr);

	/*
	 * genlmsg_multicast_allns() consumes @msg. -ESRCH (no listeners) is
	 * expected and not an error for the caller.
	 */
	genlmsg_multicast_allns(&drm_ras_nl_family, msg, 0,
				DRM_RAS_NLGRP_ERROR_REPORT);
	return 0;

cancel_msg:
	genlmsg_cancel(msg, hdr);
free_msg:
	nlmsg_free(msg);
	return ret;
}
EXPORT_SYMBOL(drm_ras_nl_error_event);

/**
 * drm_ras_node_register() - Register a new RAS node
 * @node: Node structure to register
 *
 * Return: 0 on success, or negative errno on failure.
 */
int drm_ras_node_register(struct drm_ras_node *node)
{
	if (!node->device_name || !node->node_name)
		return -EINVAL;

	/* Currently, only Error Counter nodes are supported */
	if (node->type != DRM_RAS_NODE_TYPE_ERROR_COUNTER)
		return -EINVAL;

	/* Mandatory entries for Error Counter Node */
	if (!node->error_counter_range.last || !node->query_error_counter)
		return -EINVAL;

	return xa_alloc(&drm_ras_xa, &node->id, node, xa_limit_32b, GFP_KERNEL);
}
EXPORT_SYMBOL(drm_ras_node_register);

/**
 * drm_ras_node_unregister() - Unregister a previously registered node
 * @node: Node structure to unregister
 */
void drm_ras_node_unregister(struct drm_ras_node *node)
{
	xa_erase(&drm_ras_xa, node->id);
}
EXPORT_SYMBOL(drm_ras_node_unregister);

static int __init drm_ras_init(void)
{
	return genl_register_family(&drm_ras_nl_family);
}
module_init(drm_ras_init);

static void __exit drm_ras_exit(void)
{
	genl_unregister_family(&drm_ras_nl_family);
}
module_exit(drm_ras_exit);

MODULE_DESCRIPTION("DRM RAS Generic Netlink core (standalone 5.15 backport)");
MODULE_AUTHOR("Rodrigo Vivi");
MODULE_LICENSE("Dual MIT/GPL");
