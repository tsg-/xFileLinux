// SPDX-License-Identifier: GPL-2.0
/*
 * dma-buf I/O contexts for RPC-over-RDMA direct data placement
 *
 * A context attaches a dma-buf to the RDMA device the transport is
 * connected through, and keeps a table of the DMA address of every
 * DMA_BUF_IO_PAGE_SIZE page of the mapping, so a request can register
 * any range of the dma-buf as a memory region without mapping it again.
 */

#include <linux/dma-buf.h>
#include <linux/dma-buf-io.h>
#include <linux/dma-resv.h>
#include <linux/slab.h>
#include <linux/sunrpc/rdma_rn.h>
#include <rdma/ib_verbs.h>

#include "xprt_rdma.h"

MODULE_IMPORT_NS("DMA_BUF");

static void rpcrdma_dmabuf_invalidate(struct dma_buf_attachment *attach)
{
	struct dma_buf_io_ctx *base = attach->importer_priv;
	struct rpcrdma_dmabuf_ctx *ctx = base->dev_priv;

	/*
	 * dma_buf_io_invalidate_mappings() waits for every request holding
	 * the map. End them first, so the exporter does not wait for an RPC
	 * that is stuck behind an unreachable server.
	 */
	if (ctx->owner.cancel)
		ctx->owner.cancel(ctx->owner.data, -ECANCELED);
	dma_buf_io_invalidate_mappings(base);
	if (ctx->owner.resume)
		ctx->owner.resume(ctx->owner.data);
}

static const struct dma_buf_attach_ops rpcrdma_dmabuf_importer_ops = {
	.invalidate_mappings	= rpcrdma_dmabuf_invalidate,
	.allow_peer2peer	= true,
};

static struct dma_buf_io_map *rpcrdma_dmabuf_map(struct dma_buf_io_ctx *base)
{
	unsigned int nr_entries = base->dmabuf->size / DMA_BUF_IO_PAGE_SIZE;
	struct rpcrdma_dmabuf_ctx *ctx = base->dev_priv;
	struct rpcrdma_dmabuf_map *map;
	struct sg_table *sgt;
	int ret;

	dma_resv_assert_held(base->dmabuf->resv);

	map = kvmalloc_flex(*map, dma_list, nr_entries);
	if (!map)
		return ERR_PTR(-ENOMEM);
	map->nr_entries = nr_entries;

	sgt = dma_buf_map_attachment(ctx->attach, base->dir);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto out_free;
	}
	ret = dma_buf_io_fill_dma_list(sgt, map->dma_list, nr_entries);
	if (ret)
		goto out_unmap;
	ret = dma_buf_io_init_map(base, &map->base, sgt);
	if (ret)
		goto out_unmap;
	map->sgt = sgt;
	return &map->base;

out_unmap:
	dma_buf_unmap_attachment(ctx->attach, sgt, base->dir);
out_free:
	kvfree(map);
	return ERR_PTR(ret);
}

static void rpcrdma_dmabuf_unmap(struct dma_buf_io_ctx *base,
				 struct dma_buf_io_map *map_base)
{
	struct rpcrdma_dmabuf_ctx *ctx = base->dev_priv;
	struct rpcrdma_dmabuf_map *map = to_rpcrdma_dmabuf_map(map_base);

	dma_resv_assert_held(base->dmabuf->resv);

	dma_buf_unmap_attachment(ctx->attach, map->sgt, base->dir);
	kvfree(map);
}

/*
 * The RDMA device is going away. End the context's I/O, detach it so no
 * new map can be created, and drop the device, all before
 * rpcrdma_remove_one() lets the device go. rpcrdma_rn_unregister() may be
 * called from here: the notifier does not wait for its own callback.
 */
static void rpcrdma_dmabuf_removal(struct rpcrdma_notification *rn)
{
	struct rpcrdma_dmabuf_ctx *ctx =
		container_of(rn, struct rpcrdma_dmabuf_ctx, rn);
	struct ib_device *device;

	mutex_lock(&ctx->lock);
	if (ctx->releasing) {
		/* release unregisters @rn, waiting for this callback */
		mutex_unlock(&ctx->lock);
		return;
	}
	ctx->removing = true;
	if (ctx->owner.cancel)
		ctx->owner.cancel(ctx->owner.data, -EXDEV);
	dma_buf_io_detach(ctx->base);
	dma_buf_detach(ctx->base->dmabuf, ctx->attach);
	ctx->attach = NULL;
	device = ctx->device;
	mutex_unlock(&ctx->lock);

	rpcrdma_rn_unregister(device, rn);
	ib_device_put(device);
	complete(&ctx->removed);
}

static void rpcrdma_dmabuf_release(struct dma_buf_io_ctx *base)
{
	struct rpcrdma_dmabuf_ctx *ctx = base->dev_priv;
	struct ib_device *device = NULL;
	bool removing;

	mutex_lock(&ctx->lock);
	removing = ctx->removing;
	if (!removing) {
		ctx->releasing = true;
		dma_buf_detach(base->dmabuf, ctx->attach);
		ctx->attach = NULL;
		device = ctx->device;
	}
	mutex_unlock(&ctx->lock);

	if (removing) {
		/* the removal callback still uses @ctx->rn until it completes */
		wait_for_completion(&ctx->removed);
	} else {
		rpcrdma_rn_unregister(device, &ctx->rn);
		ib_device_put(device);
	}
	if (ctx->owner.release)
		ctx->owner.release(ctx->owner.data);
	kfree(ctx);
}

static const struct dma_buf_io_ops rpcrdma_dmabuf_io_ops = {
	.map		= rpcrdma_dmabuf_map,
	.unmap		= rpcrdma_dmabuf_unmap,
	.release	= rpcrdma_dmabuf_release,
};

/**
 * rpcrdma_dmabuf_check - check a dma-buf payload can be placed on a device
 * @buf: xdr_buf carrying a dma-buf payload
 * @device: device the payload is about to be registered on
 *
 * Returns 0, -EOPNOTSUPP if the map was not created by xprtrdma, -ECANCELED
 * if it is being invalidated, -EXDEV if its buffer was attached for another
 * device, or -EINVAL if the payload lies outside the buffer.
 */
int rpcrdma_dmabuf_check(const struct xdr_buf *buf, struct ib_device *device)
{
	struct dma_buf_io_map *base = buf->dmabuf;
	struct rpcrdma_dmabuf_ctx *ctx;

	if (!base || base->ctx->dev_ops != &rpcrdma_dmabuf_io_ops)
		return -EOPNOTSUPP;
	/* its DMA addresses no longer describe the buffer's memory */
	if (percpu_ref_is_dying(&base->refs))
		return -ECANCELED;
	ctx = base->ctx->dev_priv;
	if (ctx->device != device)
		return -EXDEV;
	if ((u64)buf->page_base + buf->page_len >
	    (u64)to_rpcrdma_dmabuf_map(base)->nr_entries * DMA_BUF_IO_PAGE_SIZE)
		return -EINVAL;
	return 0;
}

/**
 * xprt_rdma_init_dma_buf_io_ctx - attach a dma-buf for direct data placement
 * @xprt: transport the I/O will be sent on
 * @base: context being created by dma_buf_io_ctx_create()
 * @owner: upper-layer hooks for ending the context's I/O and releasing it
 *
 * Attaches the dma-buf to the RDMA device @xprt is currently connected
 * through and records that device; requests on another device are refused
 * when they are marshalled.
 *
 * Returns 0 on success, -ENOTCONN if @xprt is not connected, -EOPNOTSUPP
 * on a kernel whose PAGE_SIZE is not 4 KiB or for a device without a DMA
 * device, -EINVAL for a dma-buf that is not a
 * multiple of DMA_BUF_IO_PAGE_SIZE or is larger than UINT_MAX, or the
 * error from attaching the dma-buf (-EBUSY when peer-to-peer DMA between
 * the exporter and the device is not possible).
 */
int xprt_rdma_init_dma_buf_io_ctx(struct rpc_xprt *xprt,
				  struct dma_buf_io_ctx *base,
				  const struct rpc_dma_buf_owner *owner)
{
	struct rpcrdma_xprt *r_xprt = rpcx_to_rdmax(xprt);
	struct dma_buf_attachment *attach;
	struct rpcrdma_dmabuf_ctx *ctx;
	struct ib_device *device = NULL;
	int ret;

	/*
	 * A dma-buf MR is built from DMA_BUF_IO_PAGE_SIZE pages, but the chunk
	 * segment budget is sized in PAGE_SIZE pages and would run out first.
	 */
	if (PAGE_SIZE != DMA_BUF_IO_PAGE_SIZE)
		return -EOPNOTSUPP;
	if (base->dmabuf->size > UINT_MAX ||
	    !IS_ALIGNED(base->dmabuf->size, DMA_BUF_IO_PAGE_SIZE))
		return -EINVAL;

	/*
	 * Snapshot the device under rx_unmap_rwsem, which disconnect takes
	 * for write before tearing the endpoint down, and drop it before
	 * attaching: dma_buf_dynamic_attach() takes the reservation lock,
	 * which must never nest inside a transport lock.
	 */
	down_read(&r_xprt->rx_unmap_rwsem);
	if (r_xprt->rx_ep && ib_device_try_get(r_xprt->rx_ep->re_id->device))
		device = r_xprt->rx_ep->re_id->device;
	up_read(&r_xprt->rx_unmap_rwsem);
	if (!device)
		return -ENOTCONN;

	ret = -ENOMEM;
	ctx = kzalloc_obj(*ctx);
	if (!ctx)
		goto out_put;
	mutex_init(&ctx->lock);
	init_completion(&ctx->removed);
	ctx->base = base;
	ctx->device = device;
	ctx->owner = *owner;

	/* the exporter may invalidate as soon as the attachment exists */
	base->dev_priv = ctx;
	base->dev_ops = &rpcrdma_dmabuf_io_ops;
	attach = ib_dma_buf_dynamic_attach(device, base->dmabuf,
					   &rpcrdma_dmabuf_importer_ops, base);
	if (IS_ERR(attach)) {
		ret = PTR_ERR(attach);
		goto out_free;
	}
	ctx->attach = attach;

	ret = rpcrdma_rn_register(device, &ctx->rn, rpcrdma_dmabuf_removal);
	if (ret)
		goto out_detach;
	return 0;

out_detach:
	dma_buf_detach(base->dmabuf, attach);
out_free:
	base->dev_priv = NULL;
	base->dev_ops = NULL;
	kfree(ctx);
out_put:
	ib_device_put(device);
	return ret;
}
