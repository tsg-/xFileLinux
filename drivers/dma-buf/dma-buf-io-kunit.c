// SPDX-License-Identifier: GPL-2.0

#include <kunit/test.h>

#include <linux/completion.h>
#include <linux/dma-buf-io.h>
#include <linux/dma-buf.h>
#include <linux/dma-resv.h>
#include <linux/fs.h>
#include <linux/kthread.h>

#define DMA_BUF_IO_TEST_TIMEOUT	(5 * HZ)

struct dma_buf_io_test {
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attach;
	struct dma_buf_io_ctx *ctx;
	struct file file;
	struct device dev;
	struct completion map_entered;
	struct completion allow_map;
	struct completion creator_done;
	struct completion map_held;
	struct completion allow_drop;
	struct completion holder_done;
	struct completion mover_attempted;
	struct completion move_notify_entered;
	struct completion mover_done;
	struct completion unmap_done;
	atomic_t unmaps;
	bool saw_published_map;
	int holder_error;
};

struct dma_buf_io_test_map {
	struct dma_buf_io_map base;
};

static void dma_buf_io_test_device_release(struct device *dev)
{
}

static int dma_buf_io_test_pin(struct dma_buf_attachment *attach)
{
	return 0;
}

static void dma_buf_io_test_unpin(struct dma_buf_attachment *attach)
{
}

static struct sg_table *
dma_buf_io_test_map_dma_buf(struct dma_buf_attachment *attach,
			    enum dma_data_direction direction)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static void dma_buf_io_test_unmap_dma_buf(struct dma_buf_attachment *attach,
					  struct sg_table *sgt,
					  enum dma_data_direction direction)
{
}

static void dma_buf_io_test_release_dma_buf(struct dma_buf *dmabuf)
{
}

static const struct dma_buf_ops dma_buf_io_test_dma_buf_ops = {
	.pin		= dma_buf_io_test_pin,
	.unpin		= dma_buf_io_test_unpin,
	.map_dma_buf	= dma_buf_io_test_map_dma_buf,
	.unmap_dma_buf	= dma_buf_io_test_unmap_dma_buf,
	.release	= dma_buf_io_test_release_dma_buf,
};

static void dma_buf_io_test_unmap(struct dma_buf_io_ctx *ctx,
				  struct dma_buf_io_map *map)
{
	struct dma_buf_io_test *test = ctx->dev_priv;

	atomic_inc(&test->unmaps);
	complete(&test->unmap_done);
}

static struct dma_buf_io_map *
dma_buf_io_test_map(struct dma_buf_io_ctx *ctx)
{
	struct dma_buf_io_test *test = ctx->dev_priv;
	struct dma_buf_io_test_map *map;
	int ret;

	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (!map)
		return ERR_PTR(-ENOMEM);

	ret = dma_buf_io_init_map(ctx, &map->base);
	if (ret) {
		kfree(map);
		return ERR_PTR(ret);
	}

	map->base.seg_shift = PAGE_SHIFT;
	complete(&test->map_entered);
	if (!wait_for_completion_timeout(&test->allow_map,
					 DMA_BUF_IO_TEST_TIMEOUT)) {
		percpu_ref_exit(&map->base.refs);
		kfree(map);
		return ERR_PTR(-ETIMEDOUT);
	}

	return &map->base;
}

static void dma_buf_io_test_release(struct dma_buf_io_ctx *ctx)
{
	struct dma_buf_io_test *test = ctx->dev_priv;

	dma_buf_detach(ctx->dmabuf, test->attach);
	test->attach = NULL;
}

static const struct dma_buf_io_ops dma_buf_io_test_ops = {
	.map		= dma_buf_io_test_map,
	.unmap		= dma_buf_io_test_unmap,
	.release	= dma_buf_io_test_release,
};

static void dma_buf_io_test_move_notify(struct dma_buf_attachment *attach)
{
	struct dma_buf_io_test *test = attach->importer_priv;
	struct dma_buf_io_map *map;

	complete(&test->move_notify_entered);
	scoped_guard(mutex, &test->ctx->map_mutex) {
		map = rcu_dereference_protected(test->ctx->map,
				lockdep_is_held(&test->ctx->map_mutex));
		test->saw_published_map = !!map;
	}
	dma_buf_io_invalidate_mappings(test->ctx);
}

static const struct dma_buf_attach_ops dma_buf_io_test_importer_ops = {
	.move_notify	= dma_buf_io_test_move_notify,
};

static int dma_buf_io_test_init_ctx(struct file *file,
				    struct dma_buf_io_ctx *ctx)
{
	struct dma_buf_io_test *test = file->private_data;

	test->attach = dma_buf_dynamic_attach(ctx->dmabuf, &test->dev,
					      &dma_buf_io_test_importer_ops,
					      test);
	if (IS_ERR(test->attach))
		return PTR_ERR(test->attach);

	ctx->dev_priv = test;
	ctx->dev_ops = &dma_buf_io_test_ops;
	return 0;
}

static const struct file_operations dma_buf_io_test_fops = {
	.init_dma_buf_io_ctx	= dma_buf_io_test_init_ctx,
};

static struct dma_buf_io_test *
dma_buf_io_test_create(struct kunit *test)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct dma_buf_io_test *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;

	init_completion(&ctx->map_entered);
	init_completion(&ctx->allow_map);
	init_completion(&ctx->creator_done);
	init_completion(&ctx->map_held);
	init_completion(&ctx->allow_drop);
	init_completion(&ctx->holder_done);
	init_completion(&ctx->mover_attempted);
	init_completion(&ctx->move_notify_entered);
	init_completion(&ctx->mover_done);
	init_completion(&ctx->unmap_done);
	atomic_set(&ctx->unmaps, 0);

	ctx->dev.init_name = "dma-buf-io-kunit";
	ctx->dev.release = dma_buf_io_test_device_release;
	device_initialize(&ctx->dev);

	exp_info.ops = &dma_buf_io_test_dma_buf_ops;
	exp_info.size = PAGE_SIZE;
	exp_info.flags = O_RDWR;
	exp_info.priv = ctx;
	ctx->dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(ctx->dmabuf)) {
		put_device(&ctx->dev);
		return NULL;
	}

	ctx->file.f_op = &dma_buf_io_test_fops;
	ctx->file.private_data = ctx;
	if (dma_buf_io_ctx_create(&ctx->file, ctx->dmabuf,
				  DMA_BIDIRECTIONAL, &ctx->ctx)) {
		dma_buf_put(ctx->dmabuf);
		put_device(&ctx->dev);
		return NULL;
	}

	return ctx;
}

static void dma_buf_io_test_destroy(struct dma_buf_io_test *test)
{
	if (test->ctx)
		dma_buf_io_ctx_release(test->ctx);
	if (test->dmabuf)
		dma_buf_put(test->dmabuf);
	put_device(&test->dev);
}

static int dma_buf_io_test_creator(void *data)
{
	struct dma_buf_io_test *test = data;
	struct dma_buf_io_map *map;

	map = dma_buf_io_create_map(test->ctx);
	if (!IS_ERR(map))
		dma_buf_io_map_drop(map);
	complete(&test->creator_done);
	return 0;
}

static int dma_buf_io_test_holder(void *data)
{
	struct dma_buf_io_test *test = data;
	struct dma_buf_io_map *map;

	map = dma_buf_io_create_map(test->ctx);
	if (IS_ERR(map)) {
		test->holder_error = PTR_ERR(map);
		complete(&test->map_held);
		complete(&test->holder_done);
		return 0;
	}

	complete(&test->map_held);
	if (!wait_for_completion_timeout(&test->allow_drop,
					 DMA_BUF_IO_TEST_TIMEOUT))
		test->holder_error = -ETIMEDOUT;
	dma_buf_io_map_drop(map);
	complete(&test->holder_done);
	return 0;
}

static int dma_buf_io_test_mover(void *data)
{
	struct dma_buf_io_test *test = data;

	complete(&test->mover_attempted);
	dma_resv_lock(test->dmabuf->resv, NULL);
	dma_buf_move_notify(test->dmabuf);
	dma_resv_unlock(test->dmabuf->resv);
	complete(&test->mover_done);
	return 0;
}

static bool dma_buf_io_test_wait(struct kunit *test,
				 struct completion *completion,
				 const char *what)
{
	if (wait_for_completion_timeout(completion, DMA_BUF_IO_TEST_TIMEOUT))
		return true;

	KUNIT_FAIL(test, "timed out waiting for %s", what);
	return false;
}

static void dma_buf_io_publish_before_move_notify_test(struct kunit *test)
{
	struct dma_buf_io_test *ctx;
	struct task_struct *creator = NULL;
	struct task_struct *mover = NULL;
	struct dma_buf_io_map *map;

	ctx = dma_buf_io_test_create(test);
	KUNIT_ASSERT_NOT_NULL(test, ctx);

	creator = kthread_create(dma_buf_io_test_creator, ctx,
			         "dma_buf_io_creator");
	if (IS_ERR(creator)) {
		KUNIT_FAIL(test, "failed to start map creator");
		creator = NULL;
		goto out;
	}
	get_task_struct(creator);
	wake_up_process(creator);
	if (!dma_buf_io_test_wait(test, &ctx->map_entered, "map callback"))
		goto out;

	mover = kthread_create(dma_buf_io_test_mover, ctx, "dma_buf_io_mover");
	if (IS_ERR(mover)) {
		KUNIT_FAIL(test, "failed to start mover");
		mover = NULL;
		goto out;
	}
	get_task_struct(mover);
	wake_up_process(mover);
	if (!dma_buf_io_test_wait(test, &ctx->mover_attempted,
				  "mover attempt"))
		goto out;
	KUNIT_EXPECT_FALSE(test, completion_done(&ctx->mover_done));

	complete(&ctx->allow_map);
	if (!dma_buf_io_test_wait(test, &ctx->creator_done, "map creator") ||
	    !dma_buf_io_test_wait(test, &ctx->mover_done, "move notify") ||
	    !dma_buf_io_test_wait(test, &ctx->unmap_done, "map unmap"))
		goto out;

	KUNIT_EXPECT_TRUE(test, ctx->saw_published_map);
	scoped_guard(mutex, &ctx->ctx->map_mutex) {
		map = rcu_dereference_protected(ctx->ctx->map,
				lockdep_is_held(&ctx->ctx->map_mutex));
		KUNIT_EXPECT_NULL(test, map);
	}
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->unmaps), 1);
out:
	complete_all(&ctx->allow_map);
	if (creator)
		kthread_stop_put(creator);
	if (mover)
		kthread_stop_put(mover);
	dma_buf_io_test_destroy(ctx);
}

static void dma_buf_io_move_waits_for_active_map_test(struct kunit *test)
{
	struct dma_buf_io_test *ctx;
	struct task_struct *holder = NULL;
	struct task_struct *mover = NULL;
	struct dma_buf_io_map *map;

	ctx = dma_buf_io_test_create(test);
	KUNIT_ASSERT_NOT_NULL(test, ctx);

	holder = kthread_create(dma_buf_io_test_holder, ctx,
				"dma_buf_io_holder");
	if (IS_ERR(holder)) {
		KUNIT_FAIL(test, "failed to start map holder");
		holder = NULL;
		goto out;
	}
	get_task_struct(holder);
	wake_up_process(holder);
	complete(&ctx->allow_map);
	if (!dma_buf_io_test_wait(test, &ctx->map_held, "map holder"))
		goto out;
	if (ctx->holder_error) {
		KUNIT_FAIL(test, "map holder failed: %d", ctx->holder_error);
		goto out;
	}

	mover = kthread_create(dma_buf_io_test_mover, ctx,
			       "dma_buf_io_mover");
	if (IS_ERR(mover)) {
		KUNIT_FAIL(test, "failed to start mover");
		mover = NULL;
		goto out;
	}
	get_task_struct(mover);
	wake_up_process(mover);
	if (!dma_buf_io_test_wait(test, &ctx->move_notify_entered,
				  "move notify entry"))
		goto out;
	KUNIT_EXPECT_FALSE(test, wait_for_completion_timeout(&ctx->mover_done,
						      HZ / 10));

	complete(&ctx->allow_drop);
	if (!dma_buf_io_test_wait(test, &ctx->holder_done, "map holder") ||
	    !dma_buf_io_test_wait(test, &ctx->mover_done, "move notify") ||
	    !dma_buf_io_test_wait(test, &ctx->unmap_done, "map unmap"))
		goto out;

	KUNIT_EXPECT_TRUE(test, ctx->saw_published_map);
	scoped_guard(mutex, &ctx->ctx->map_mutex) {
		map = rcu_dereference_protected(ctx->ctx->map,
				lockdep_is_held(&ctx->ctx->map_mutex));
		KUNIT_EXPECT_NULL(test, map);
	}
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->unmaps), 1);
out:
	complete_all(&ctx->allow_map);
	complete_all(&ctx->allow_drop);
	if (holder)
		kthread_stop_put(holder);
	if (mover)
		kthread_stop_put(mover);
	dma_buf_io_test_destroy(ctx);
}

static struct kunit_case dma_buf_io_test_cases[] = {
	KUNIT_CASE(dma_buf_io_publish_before_move_notify_test),
	KUNIT_CASE(dma_buf_io_move_waits_for_active_map_test),
	{}
};

static struct kunit_suite dma_buf_io_test_suite = {
	.name = "dma-buf-io",
	.test_cases = dma_buf_io_test_cases,
};

kunit_test_suite(dma_buf_io_test_suite);

MODULE_LICENSE("GPL");
