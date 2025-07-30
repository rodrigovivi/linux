// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#include <net/genetlink.h>
#include <uapi/drm/drm_netlink.h>

#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_managed.h>
#include <drm/drm_netlink.h>
#include <drm/drm_print.h>

DEFINE_XARRAY(drm_dev_xarray);

/**
 * drm_genl_reply - response to a request
 * @msg: socket buffer
 * @info: receiver information
 * @usrhdr: pointer to user specific header in the message buffer
 *
 * RETURNS:
 * 0 on success and negative error code on failure
 */
int drm_genl_reply(struct sk_buff *msg, struct genl_info *info, void *usrhdr)
{
	int ret;

	genlmsg_end(msg, usrhdr);

	ret = genlmsg_reply(msg, info);
	if (ret)
		nlmsg_free(msg);

	return ret;
}
EXPORT_SYMBOL(drm_genl_reply);

/**
 * drm_genl_alloc_msg - allocate genl message buffer
 * @dev: drm_device for which the message is being allocated
 * @info: receiver information
 * @msg_size: size of the msg buffer that needs to be allocated
 * @usrhdr: pointer to user specific header in the message buffer
 *
 * RETURNS:
 * pointer to new allocated buffer on success, NULL on failure
 */
struct sk_buff *
drm_genl_alloc_msg(struct drm_device *dev,
		   struct genl_info *info,
		   size_t msg_size, void **usrhdr)
{
	struct sk_buff *new_msg;

	new_msg = genlmsg_new(msg_size, GFP_KERNEL);
	if (!new_msg)
		return new_msg;

	*usrhdr = genlmsg_put_reply(new_msg, info, dev->drm_genl_family, 0, info->genlhdr->cmd);
	if (!*usrhdr) {
		nlmsg_free(new_msg);
		new_msg = NULL;
	}

	return new_msg;
}
EXPORT_SYMBOL(drm_genl_alloc_msg);

static struct drm_device *genl_to_dev(struct genl_info *info)
{
	return xa_load(&drm_dev_xarray, info->nlhdr->nlmsg_type);
}

static int drm_genl_list_errors(struct sk_buff *msg, struct genl_info *info)
{
	struct drm_device *dev = genl_to_dev(info);

	if (info->genlhdr->cmd == DRM_RAS_CMD_READ_ALL) {
		if (GENL_REQ_ATTR_CHECK(info, DRM_RAS_ATTR_READ_ALL))
			return -EINVAL;
	} else {
		if (GENL_REQ_ATTR_CHECK(info, DRM_RAS_ATTR_QUERY))
			return -EINVAL;
	}

	if (WARN_ON(!dev->driver->genl_ops[info->genlhdr->cmd].doit))
		return -EOPNOTSUPP;

	return dev->driver->genl_ops[info->genlhdr->cmd].doit(dev, msg, info);
}

static int drm_genl_read_error(struct sk_buff *msg, struct genl_info *info)
{
	struct drm_device *dev = genl_to_dev(info);

	if (GENL_REQ_ATTR_CHECK(info, DRM_RAS_ATTR_ERROR_ID))
		return -EINVAL;

	if (WARN_ON(!dev->driver->genl_ops[info->genlhdr->cmd].doit))
		return -EOPNOTSUPP;

	return dev->driver->genl_ops[info->genlhdr->cmd].doit(dev, msg, info);
}

/* attribute policies */
static const struct nla_policy drm_attr_policy_query[DRM_ATTR_MAX + 1] = {
	[DRM_RAS_ATTR_QUERY] = { .type = NLA_U8 },
};

static const struct nla_policy drm_attr_policy_read_one[DRM_ATTR_MAX + 1] = {
	[DRM_RAS_ATTR_ERROR_ID] = { .type = NLA_U64 },
};

static const struct nla_policy drm_attr_policy_read_all[DRM_ATTR_MAX + 1] = {
	[DRM_RAS_ATTR_READ_ALL] = { .type = NLA_U8 },
};

/* drm genl operations definition */
const struct genl_ops drm_genl_ops[] = {
	{
		.cmd = DRM_RAS_CMD_QUERY,
		.doit = drm_genl_list_errors,
		.policy = drm_attr_policy_query,
	},
	{
		.cmd = DRM_RAS_CMD_READ_ONE,
		.doit = drm_genl_read_error,
		.policy = drm_attr_policy_read_one,
	},
	{
		.cmd = DRM_RAS_CMD_READ_ALL,
		.doit = drm_genl_list_errors,
		.policy = drm_attr_policy_read_all,
	},
	{
		.cmd = DRM_RAS_CMD_READ_BLOCK,
		.doit = drm_genl_read_error,
		.policy = drm_attr_policy_read_one,
	},
};

static void drm_genl_family_init(struct drm_device *dev)
{
	dev->drm_genl_family = drmm_kzalloc(dev, sizeof(struct genl_family),
					    GFP_KERNEL);

	/* Use drm primary node name eg: card0 to name the genl family */
	snprintf(dev->drm_genl_family->name, sizeof(dev->drm_genl_family->name),
		 "%s", dev->primary->kdev->kobj.name);
	dev->drm_genl_family->version = DRM_GENL_VERSION;
	dev->drm_genl_family->parallel_ops = true;
	dev->drm_genl_family->ops = drm_genl_ops;
	dev->drm_genl_family->n_ops = ARRAY_SIZE(drm_genl_ops);
	dev->drm_genl_family->maxattr = DRM_ATTR_MAX;
	dev->drm_genl_family->module = dev->dev->driver->owner;
}

static void drm_genl_deregister(struct drm_device *dev, void *arg)
{
	drm_dbg_driver(dev, "unregistering genl family %s\n", dev->drm_genl_family->name);

	xa_erase(&drm_dev_xarray, dev->drm_genl_family->id);

	genl_unregister_family(dev->drm_genl_family);
}

/**
 * drm_genl_register - Register genl family
 * @dev: drm_device for which genl family needs to be registered
 *
 * RETURNS:
 * 0 on success and negative error code on failure
 */
int drm_genl_register(struct drm_device *dev)
{
	int ret;

	drm_genl_family_init(dev);

	ret = genl_register_family(dev->drm_genl_family);
	if (ret < 0) {
		drm_warn(dev, "genl family registration failed\n");
		return ret;
	}
	drm_dbg_driver(dev, "genl family id %d and name %s\n", dev->drm_genl_family->id,
		       dev->drm_genl_family->name);

	ret = xa_err(xa_store(&drm_dev_xarray, dev->drm_genl_family->id, dev, GFP_KERNEL));
	if (ret)
		goto genl_unregister;

	ret = drmm_add_action_or_reset(dev, drm_genl_deregister, NULL);

	return ret;

genl_unregister:
	genl_unregister_family(dev->drm_genl_family);
	return ret;
}

/**
 * drm_genl_exit: destroy drm_dev_xarray
 */
void drm_genl_exit(void)
{
	xa_destroy(&drm_dev_xarray);
}
