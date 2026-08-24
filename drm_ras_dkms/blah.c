// SPDX-License-Identifier: GPL-2.0

/*
 * blah - example drm-ras consumer for the standalone linux-5.15.y backport.
 *
 * 5.15 has no accel subsystem, so unlike the upstream drivers/accel/blah this
 * is a plain platform module with no DRM dependency: it only links against the
 * node register/unregister/event API exported by drm_ras.ko. It registers two
 * error-counter nodes with a deliberately non-contiguous error range to
 * exercise every drm-ras code path.
 */

#include <linux/atomic.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include "drm_ras.h"

#define BLAH_NAME "blah"

/*
 * Example error IDs. IDs 0 and 1 exist, ID 2 is intentionally missing, and
 * BLAH_ERR_EU lives at ID 3 -> query returns -ENOENT for 2 so drm-ras skips it.
 */
enum blah_error_id {
	BLAH_ERR_L3		= 0,
	BLAH_ERR_SAMPLER	= 1,
	/* ID 2 intentionally unimplemented -> query returns -ENOENT */
	BLAH_ERR_EU		= 3,
	BLAH_ERR_MAX,
};

static const char * const blah_error_names[BLAH_ERR_MAX] = {
	[BLAH_ERR_L3]		= "l3",
	[BLAH_ERR_SAMPLER]	= "sampler",
	[BLAH_ERR_EU]		= "eu",
};

enum blah_node_id {
	BLAH_NODE_CORRECTABLE	= 0,
	BLAH_NODE_UNCORRECTABLE	= 1,
	BLAH_NODE_MAX,
};

static const char * const blah_node_names[BLAH_NODE_MAX] = {
	[BLAH_NODE_CORRECTABLE]		= "correctable",
	[BLAH_NODE_UNCORRECTABLE]	= "uncorrectable",
};

/**
 * struct blah_ras - Per-node RAS state
 * @node: drm-ras node; recovered from callbacks via node->priv
 * @counters: software error counters, one per &enum blah_error_id
 */
struct blah_ras {
	struct drm_ras_node node;
	atomic_t counters[BLAH_ERR_MAX];
};

static struct blah_ras blah_nodes[BLAH_NODE_MAX];
static struct platform_device *blah_pdev;

static int blah_query_error_counter(struct drm_ras_node *node, u32 error_id,
				    const char **name, u32 *val)
{
	struct blah_ras *ras = node->priv;

	if (error_id >= BLAH_ERR_MAX || !blah_error_names[error_id])
		return -ENOENT;

	*name = blah_error_names[error_id];
	*val = atomic_read(&ras->counters[error_id]);

	return 0;
}

static int blah_clear_error_counter(struct drm_ras_node *node, u32 error_id)
{
	struct blah_ras *ras = node->priv;

	if (error_id >= BLAH_ERR_MAX || !blah_error_names[error_id])
		return -ENOENT;

	atomic_set(&ras->counters[error_id], 0);

	return 0;
}

/*
 * Simulate a hardware error: bump the counter and multicast an event. Real
 * drivers call drm_ras_nl_error_event() from process context (IRQ bottom-half).
 */
static void blah_inject_error(struct blah_ras *ras, u32 error_id)
{
	u32 value;
	int ret;

	if (error_id >= BLAH_ERR_MAX || !blah_error_names[error_id])
		return;

	value = atomic_inc_return(&ras->counters[error_id]);

	ret = drm_ras_nl_error_event(&ras->node, error_id,
				     blah_error_names[error_id], value);
	if (ret)
		pr_warn(BLAH_NAME ": error-event failed: %d\n", ret);
}

static int __init blah_init(void)
{
	int i, ret;

	blah_pdev = platform_device_register_simple(BLAH_NAME, -1, NULL, 0);
	if (IS_ERR(blah_pdev)) {
		pr_err(BLAH_NAME ": failed to register platform device\n");
		return PTR_ERR(blah_pdev);
	}

	for (i = 0; i < BLAH_NODE_MAX; i++) {
		struct blah_ras *ras = &blah_nodes[i];
		struct drm_ras_node *node = &ras->node;

		node->device_name = dev_name(&blah_pdev->dev);
		node->node_name = blah_node_names[i];
		node->type = DRM_RAS_NODE_TYPE_ERROR_COUNTER;
		node->error_counter_range.first = BLAH_ERR_L3;
		node->error_counter_range.last = BLAH_ERR_MAX - 1;
		node->query_error_counter = blah_query_error_counter;
		node->clear_error_counter = blah_clear_error_counter;
		node->priv = ras;

		ret = drm_ras_node_register(node);
		if (ret) {
			pr_err(BLAH_NAME ": failed to register node %d: %d\n",
			       i, ret);
			goto err_unwind;
		}
	}

	/* Emit one event per node so subscribers see traffic on load. */
	blah_inject_error(&blah_nodes[BLAH_NODE_CORRECTABLE], BLAH_ERR_L3);
	blah_inject_error(&blah_nodes[BLAH_NODE_UNCORRECTABLE], BLAH_ERR_EU);

	return 0;

err_unwind:
	while (--i >= 0)
		drm_ras_node_unregister(&blah_nodes[i].node);
	platform_device_unregister(blah_pdev);
	return ret;
}

static void __exit blah_exit(void)
{
	int i;

	for (i = 0; i < BLAH_NODE_MAX; i++)
		drm_ras_node_unregister(&blah_nodes[i].node);

	platform_device_unregister(blah_pdev);
}

module_init(blah_init);
module_exit(blah_exit);

MODULE_DESCRIPTION("Blah drm-ras example consumer (standalone 5.15 backport)");
MODULE_AUTHOR("Rodrigo Vivi");
MODULE_LICENSE("GPL");
