// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * Copyright (c) 2024 Oracle.  All rights reserved.
 */

/* #include <linux/module.h>
#include <linux/slab.h> */
#include <linux/xarray.h>
#include <linux/types.h>
#include <linux/kref.h>
#include <linux/completion.h>
#include <linux/wait.h>
#include <linux/sched.h>

#include <linux/sunrpc/svc_rdma.h>
#include <linux/sunrpc/rdma_rn.h>

#include "xprt_rdma.h"
#include <trace/events/rpcrdma.h>

/* Per-ib_device private data for rpcrdma */
struct rpcrdma_device {
	struct kref		rd_kref;
	unsigned long		rd_flags;
	struct ib_device	*rd_device;
	struct xarray		rd_xa;
	struct completion	rd_done;

	/*
	 * The notification whose ->rn_done() rpcrdma_remove_one() is
	 * running, and the task running it. Both are protected by the
	 * rd_xa lock; rd_cb_wq is woken when the callback returns.
	 */
	struct rpcrdma_notification *rd_cb;
	struct task_struct	*rd_cb_task;
	wait_queue_head_t	rd_cb_wq;
};

#define RPCRDMA_RD_F_REMOVING	(0)

static struct ib_client rpcrdma_ib_client;

/*
 * Listeners have no associated device, so we never register them.
 * Note that ib_get_client_data() does not check if @device is
 * NULL for us.
 */
static struct rpcrdma_device *rpcrdma_get_client_data(struct ib_device *device)
{
	if (!device)
		return NULL;
	return ib_get_client_data(device, &rpcrdma_ib_client);
}

/**
 * rpcrdma_rn_register - register to get device removal notifications
 * @device: device to monitor
 * @rn: notification object that wishes to be notified
 * @done: callback to notify caller of device removal
 *
 * Returns zero on success. The callback in rn_done is guaranteed
 * to be invoked when the device is removed, unless this notification
 * is unregistered first.
 *
 * On failure, a negative errno is returned. rn->rn_done is left
 * NULL on every failure path (it is armed before xa_alloc but
 * cleared again if xa_alloc fails), so the @rn may safely be
 * passed to rpcrdma_rn_unregister() without a separate
 * registered/unregistered flag in the caller.
 */
int rpcrdma_rn_register(struct ib_device *device,
			struct rpcrdma_notification *rn,
			void (*done)(struct rpcrdma_notification *rn))
{
	struct rpcrdma_device *rd = rpcrdma_get_client_data(device);
	int ret;

	if (!rd)
		return -ENETUNREACH;

	/*
	 * Arm rn_done before @rn is published: once it is visible in
	 * rd_xa, rpcrdma_remove_one() can call it. rn_done doubles as
	 * the registration sentinel for rpcrdma_rn_unregister(), so it
	 * is cleared again on every failure path.
	 *
	 * The REMOVING test and the insertion happen under the rd_xa
	 * lock, which rpcrdma_remove_one() also takes to set the flag.
	 * Either the registration is visible to its callback walk, or
	 * it sees the flag and backs out. __xa_alloc() may drop the
	 * lock to allocate, so the flag is tested again afterwards.
	 */
	rn->rn_done = done;
	xa_lock(&rd->rd_xa);
	if (test_bit(RPCRDMA_RD_F_REMOVING, &rd->rd_flags)) {
		ret = -ENETUNREACH;
		goto out_unlock;
	}
	ret = __xa_alloc(&rd->rd_xa, &rn->rn_index, rn, xa_limit_32b,
			 GFP_KERNEL);
	if (ret < 0) {
		ret = -ENOMEM;
		goto out_unlock;
	}
	if (test_bit(RPCRDMA_RD_F_REMOVING, &rd->rd_flags)) {
		__xa_erase(&rd->rd_xa, rn->rn_index);
		ret = -ENETUNREACH;
		goto out_unlock;
	}
	kref_get(&rd->rd_kref);
	xa_unlock(&rd->rd_xa);
	trace_rpcrdma_client_register(device, rn);
	return 0;

out_unlock:
	xa_unlock(&rd->rd_xa);
	rn->rn_done = NULL;
	return ret;
}

static void rpcrdma_rn_release(struct kref *kref)
{
	struct rpcrdma_device *rd = container_of(kref, struct rpcrdma_device,
						 rd_kref);

	trace_rpcrdma_client_completion(rd->rd_device);
	complete(&rd->rd_done);
}

/**
 * rpcrdma_rn_unregister - stop device removal notifications
 * @device: monitored device
 * @rn: notification object that no longer wishes to be notified
 *
 * It is safe to call this on an @rn whose registration never
 * completed or failed; rn_done == NULL is treated as
 * never-registered and the call is a no-op.
 */
void rpcrdma_rn_unregister(struct ib_device *device,
			   struct rpcrdma_notification *rn)
{
	struct rpcrdma_device *rd = rpcrdma_get_client_data(device);

	if (!rd)
		return;

	xa_lock(&rd->rd_xa);
	/*
	 * If another task is running @rn's removal callback, wait for it
	 * to return before @rn can be freed. A callback that unregisters
	 * its own notification runs in rd_cb_task and does not wait.
	 */
	while (rd->rd_cb == rn && rd->rd_cb_task != current) {
		xa_unlock(&rd->rd_xa);
		wait_event(rd->rd_cb_wq, READ_ONCE(rd->rd_cb) != rn);
		xa_lock(&rd->rd_xa);
	}

	/*
	 * rn_done is the registration sentinel: rpcrdma_rn_register
	 * leaves it NULL on every failure path, so a non-NULL rn_done
	 * marks a completed registration. A NULL rn_done means this
	 * notification was never registered (or its registration failed)
	 * or has already been unregistered, and the call is a no-op.
	 * Without this guard, rn_index == 0 from a kzalloc'd parent would
	 * erase another caller's slot 0 and underflow rd_kref.
	 */
	if (!rn->rn_done) {
		xa_unlock(&rd->rd_xa);
		return;
	}
	rn->rn_done = NULL;
	__xa_erase(&rd->rd_xa, rn->rn_index);
	xa_unlock(&rd->rd_xa);

	trace_rpcrdma_client_unregister(device, rn);
	kref_put(&rd->rd_kref, rpcrdma_rn_release);
}

/**
 * rpcrdma_add_one - ib_client device insertion callback
 * @device: device about to be inserted
 *
 * Returns zero on success. xprtrdma private data has been allocated
 * for this device. On failure, a negative errno is returned.
 */
static int rpcrdma_add_one(struct ib_device *device)
{
	struct rpcrdma_device *rd;

	rd = kzalloc_obj(*rd);
	if (!rd)
		return -ENOMEM;

	kref_init(&rd->rd_kref);
	xa_init_flags(&rd->rd_xa, XA_FLAGS_ALLOC);
	rd->rd_device = device;
	init_completion(&rd->rd_done);
	init_waitqueue_head(&rd->rd_cb_wq);
	ib_set_client_data(device, &rpcrdma_ib_client, rd);

	trace_rpcrdma_client_add_one(device);
	return 0;
}

/**
 * rpcrdma_remove_one - ib_client device removal callback
 * @device: device about to be removed
 * @client_data: this module's private per-device data
 *
 * Upon return, all transports associated with @device have divested
 * themselves from IB hardware resources.
 */
static void rpcrdma_remove_one(struct ib_device *device,
			       void *client_data)
{
	struct rpcrdma_device *rd = client_data;
	struct rpcrdma_notification *rn;
	unsigned long index;

	trace_rpcrdma_client_remove_one(device);

	xa_lock(&rd->rd_xa);
	set_bit(RPCRDMA_RD_F_REMOVING, &rd->rd_flags);
	xa_unlock(&rd->rd_xa);

	xa_for_each(&rd->rd_xa, index, rn) {
		/*
		 * @rn may have been unregistered and freed since the walk
		 * found it, so only use it if it is still in rd_xa. Once
		 * rd_cb names it, rpcrdma_rn_unregister() waits for the
		 * callback, and @rn is not touched after it returns: a
		 * callback may unregister and free its own notification.
		 */
		xa_lock(&rd->rd_xa);
		if (xa_load(&rd->rd_xa, index) != rn) {
			xa_unlock(&rd->rd_xa);
			continue;
		}
		rd->rd_cb = rn;
		rd->rd_cb_task = current;
		xa_unlock(&rd->rd_xa);

		rn->rn_done(rn);

		xa_lock(&rd->rd_xa);
		rd->rd_cb = NULL;
		rd->rd_cb_task = NULL;
		xa_unlock(&rd->rd_xa);
		wake_up_all(&rd->rd_cb_wq);
	}

	/*
	 * Wait only if there are still outstanding notification
	 * registrants for this device.
	 */
	if (!refcount_dec_and_test(&rd->rd_kref.refcount)) {
		trace_rpcrdma_client_wait_on(device);
		wait_for_completion(&rd->rd_done);
	}

	trace_rpcrdma_client_remove_one_done(device);
	xa_destroy(&rd->rd_xa);
	kfree(rd);
}

static struct ib_client rpcrdma_ib_client = {
	.name		= "rpcrdma",
	.add		= rpcrdma_add_one,
	.remove		= rpcrdma_remove_one,
};

/**
 * rpcrdma_ib_client_unregister - unregister ib_client for xprtrdma
 *
 * cel: watch for orphaned rpcrdma_device objects on module unload
 */
void rpcrdma_ib_client_unregister(void)
{
	ib_unregister_client(&rpcrdma_ib_client);
}

/**
 * rpcrdma_ib_client_register - register ib_client for rpcrdma
 *
 * Returns zero on success, or a negative errno.
 */
int rpcrdma_ib_client_register(void)
{
	return ib_register_client(&rpcrdma_ib_client);
}
