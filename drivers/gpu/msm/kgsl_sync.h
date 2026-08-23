/* Copyright (c) 2012, The Linux Foundation. All rights reserved. */
#ifndef __KGSL_SYNC_H
#define __KGSL_SYNC_H

#include <linux/sync.h>
#include "kgsl_device.h"

struct kgsl_sync_timeline {
	struct sync_timeline timeline;
	unsigned int last_timestamp;
};

struct kgsl_sync_pt {
	struct sync_pt pt;
	unsigned int timestamp;
};

#ifdef CONFIG_SYNC
int kgsl_add_fence_event(struct kgsl_device *device, u32 context_id,
	u32 timestamp, void __user *data, int len,
	struct kgsl_device_private *owner);
int kgsl_sync_timeline_create(struct kgsl_context *context);
void kgsl_sync_timeline_signal(struct sync_timeline *timeline,
	unsigned int timestamp);
void kgsl_sync_timeline_destroy(struct kgsl_context *context);
#else
static inline int kgsl_add_fence_event(struct kgsl_device *device,
	u32 context_id, u32 timestamp, void __user *data, int len,
	struct kgsl_device_private *owner)
{
	return -EINVAL;
}
static inline int kgsl_sync_timeline_create(struct kgsl_context *context)
{
	context->timeline = NULL;
	return 0;
}
static inline void kgsl_sync_timeline_signal(struct sync_timeline *timeline,
	unsigned int timestamp) { }
static inline void kgsl_sync_timeline_destroy(struct kgsl_context *context) { }
#endif

#endif
