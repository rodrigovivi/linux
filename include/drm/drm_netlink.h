/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef __DRM_NETLINK_H__
#define __DRM_NETLINK_H__

#include <linux/types.h>

struct drm_device;
struct genl_info;
struct sk_buff;

enum mcgrps_events {
	DRM_GENL_MCAST_CORR_ERR,
	DRM_GENL_MCAST_UNCORR_ERR,
};

struct driver_genl_ops {
	int (*doit)(struct drm_device *dev, struct sk_buff *skb,
		    struct genl_info *info);

/*FIXME: DO IT? can't we find a better name?*/
};

#if IS_ENABLED(CONFIG_NET)
int drm_genl_register(struct drm_device *dev);
void drm_genl_exit(void);
int drm_genl_reply(struct sk_buff *msg, struct genl_info *info, void *usrhdr);
struct sk_buff *
drm_genl_alloc_msg(struct drm_device *dev,
		   struct genl_info *info,
		   size_t msg_size, void **usrhdr);
#else
static inline int drm_genl_register(struct drm_device *dev) { return 0; }
static inline void drm_genl_exit(void) {}
static inline int drm_genl_reply(struct sk_buff *msg,
				 struct genl_info *info,
				 void *usrhdr) { return 0; }
static inline struct skb_buff *
drm_genl_alloc_msg(struct drm_device *dev,
		   struct genl_info *info,
		   size_t msg_size, void **usrhdr) { return NULL; }
#endif

#endif
