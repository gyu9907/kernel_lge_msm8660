/* Copyright (c) 2012, The Linux Foundation. All rights reserved. */
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "kgsl_sync.h"

static struct sync_pt *kgsl_sync_pt_create(struct sync_timeline *timeline,
	unsigned int timestamp)
{
	struct sync_pt *pt = sync_pt_create(timeline,
		(int) sizeof(struct kgsl_sync_pt));

	if (pt)
		((struct kgsl_sync_pt *) pt)->timestamp = timestamp;
	return pt;
}

static struct sync_pt *kgsl_sync_pt_dup(struct sync_pt *pt)
{
	return kgsl_sync_pt_create(pt->parent,
		((struct kgsl_sync_pt *) pt)->timestamp);
}

static int kgsl_sync_pt_has_signaled(struct sync_pt *pt)
{
	struct kgsl_sync_pt *kpt = (struct kgsl_sync_pt *) pt;
	struct kgsl_sync_timeline *timeline =
		(struct kgsl_sync_timeline *) pt->parent;

	return timestamp_cmp(timeline->last_timestamp, kpt->timestamp) >= 0;
}

static int kgsl_sync_pt_compare(struct sync_pt *a, struct sync_pt *b)
{
	return timestamp_cmp(((struct kgsl_sync_pt *) a)->timestamp,
		((struct kgsl_sync_pt *) b)->timestamp);
}

struct kgsl_fence_event_priv {
	struct kgsl_context *context;
	unsigned int timestamp;
};

static void kgsl_fence_event_cb(struct kgsl_device *device, void *priv,
	u32 context_id, u32 timestamp)
{
	struct kgsl_fence_event_priv *event = priv;

	kgsl_sync_timeline_signal(event->context->timeline, event->timestamp);
	kgsl_context_put(event->context);
	kfree(event);
}

int kgsl_add_fence_event(struct kgsl_device *device, u32 context_id,
	u32 timestamp, void __user *data, int len,
	struct kgsl_device_private *owner)
{
	struct kgsl_timestamp_event_fence out;
	struct kgsl_fence_event_priv *event;
	struct kgsl_context *context;
	struct sync_pt *pt;
	struct sync_fence *fence;
	int ret;

	if (len != sizeof(out))
		return -EINVAL;

	context = kgsl_find_context(owner, context_id);
	if (!context)
		return -EINVAL;

	event = kzalloc(sizeof(*event), GFP_KERNEL);
	if (!event)
		return -ENOMEM;

	kgsl_context_get(context);
	event->context = context;
	event->timestamp = timestamp;

	pt = kgsl_sync_pt_create(context->timeline, timestamp);
	if (!pt) {
		ret = -ENOMEM;
		goto err_event;
	}

	fence = sync_fence_create("kgsl-fence", pt);
	if (!fence) {
		sync_pt_free(pt);
		ret = -ENOMEM;
		goto err_event;
	}

	out.fence_fd = get_unused_fd_flags(0);
	if (out.fence_fd < 0) {
		ret = out.fence_fd;
		goto err_fence;
	}

	if (copy_to_user(data, &out, sizeof(out))) {
		ret = -EFAULT;
		goto err_fd;
	}

	ret = kgsl_add_event(device, context_id, timestamp,
		kgsl_fence_event_cb, event, owner);
	if (ret)
		goto err_fd;

	sync_fence_install(fence, out.fence_fd);
	return 0;

err_fd:
	put_unused_fd(out.fence_fd);
err_fence:
	sync_fence_put(fence);
err_event:
	kgsl_context_put(context);
	kfree(event);
	return ret;
}

static const struct sync_timeline_ops kgsl_sync_timeline_ops = {
	.driver_name = "kgsl-timeline",
	.dup = kgsl_sync_pt_dup,
	.has_signaled = kgsl_sync_pt_has_signaled,
	.compare = kgsl_sync_pt_compare,
};

int kgsl_sync_timeline_create(struct kgsl_context *context)
{
	struct kgsl_sync_timeline *timeline;

	context->timeline = sync_timeline_create(&kgsl_sync_timeline_ops,
		(int) sizeof(*timeline), "kgsl-timeline");
	if (!context->timeline)
		return -ENOMEM;

	timeline = (struct kgsl_sync_timeline *) context->timeline;
	timeline->last_timestamp = 0;
	return 0;
}

void kgsl_sync_timeline_signal(struct sync_timeline *timeline,
	unsigned int timestamp)
{
	struct kgsl_sync_timeline *kgsl_timeline =
		(struct kgsl_sync_timeline *) timeline;

	if (timestamp_cmp(timestamp, kgsl_timeline->last_timestamp) > 0)
		kgsl_timeline->last_timestamp = timestamp;
	sync_timeline_signal(timeline);
}

void kgsl_sync_timeline_destroy(struct kgsl_context *context)
{
	if (context->timeline)
		sync_timeline_destroy(context->timeline);
}
