// SPDX-License-Identifier: GPL-2.0

#include <kunit/test.h>

#include <linux/bio.h>
#include <linux/blk-integrity.h>
#include <linux/blkdev.h>
#include <linux/dma-buf-io.h>
#include <linux/uio.h>

#define BIO_DMABUF_TEST_SPLIT_BYTES	(8 * 512)

static struct bio *bio_dmabuf_test_alloc(struct block_device *bdev,
		struct dma_buf_io_map *map, unsigned int direction,
		unsigned int offset, unsigned int bytes)
{
	struct iov_iter iter;
	struct bio *bio;

	iov_iter_dmabuf_map(&iter, direction, map, offset, bytes);
	bio = bio_alloc(NULL, 0,
		direction == WRITE ? REQ_OP_WRITE : REQ_OP_READ, GFP_KERNEL);
	if (!bio)
		return NULL;
	if (bio_iov_iter_get_pages(bio, &iter)) {
		bio_put(bio);
		return NULL;
	}
	bio->bi_bdev = bdev;
	return bio;
}

static void bio_dmabuf_iterator_type_test(struct kunit *test)
{
	struct dma_buf_io_map map = {};
	struct iov_iter iter;

	iov_iter_dmabuf_map(&iter, WRITE, &map, 0, PAGE_SIZE);

	KUNIT_EXPECT_TRUE(test, iov_iter_is_dmabuf_map(&iter));
	KUNIT_EXPECT_PTR_EQ(test, iter.dmabuf_map, &map);
}

static void bio_dmabuf_iterator_cursor_test(struct kunit *test)
{
	struct dma_buf_io_map map = {};
	struct iov_iter iter;
	struct iov_iter_state state;

	iov_iter_dmabuf_map(&iter, WRITE, &map, PAGE_SIZE, 2 * PAGE_SIZE);
	iov_iter_save_state(&iter, &state);
	iov_iter_advance(&iter, PAGE_SIZE);

	KUNIT_EXPECT_EQ(test, iter.iov_offset, (size_t)2 * PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, iov_iter_count(&iter), (size_t)PAGE_SIZE);

	iov_iter_restore(&iter, &state);
	KUNIT_EXPECT_EQ(test, iter.iov_offset, (size_t)PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, iov_iter_count(&iter), (size_t)2 * PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, iov_iter_alignment(&iter),
			(size_t)3 * PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, iov_iter_gap_alignment(&iter), 0UL);
}

static void bio_dmabuf_iterator_alignment_test(struct kunit *test)
{
	struct dma_buf_io_map map = {};
	struct iov_iter iter;

	iov_iter_dmabuf_map(&iter, WRITE, &map, 1, PAGE_SIZE);
	KUNIT_EXPECT_FALSE(test, iov_iter_is_aligned(&iter, 3,
						     SECTOR_SIZE - 1));

	iov_iter_dmabuf_map(&iter, WRITE, &map, 4, PAGE_SIZE);
	KUNIT_EXPECT_TRUE(test, iov_iter_is_aligned(&iter, 3,
						    SECTOR_SIZE - 1));

	iov_iter_dmabuf_map(&iter, WRITE, &map, 4, PAGE_SIZE + 1);
	KUNIT_EXPECT_FALSE(test, iov_iter_is_aligned(&iter, 3,
						     SECTOR_SIZE - 1));
}

static void bio_dmabuf_iterator_zero_test(struct kunit *test)
{
	struct dma_buf_io_map map = {};
	struct iov_iter iter;

	iov_iter_dmabuf_map(&iter, READ, &map, 0, PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, iov_iter_zero(PAGE_SIZE, &iter), 0UL);
	KUNIT_EXPECT_EQ(test, iov_iter_count(&iter), (size_t)PAGE_SIZE);
}

static void bio_dmabuf_split_keeps_map_cursor_test(struct kunit *test)
{
	struct dma_buf_io_map map = {
		.seg_shift = PAGE_SHIFT,
	};
	struct bio *bio, *first, *second;
	const unsigned int offset = 2 * BIO_DMABUF_TEST_SPLIT_BYTES;

	bio = bio_dmabuf_test_alloc(NULL, &map, WRITE, offset,
					3 * BIO_DMABUF_TEST_SPLIT_BYTES);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, bio);
	KUNIT_EXPECT_TRUE(test, op_is_dmabuf(bio->bi_opf));
	KUNIT_EXPECT_PTR_EQ(test, bio->bi_dmabuf_map, &map);
	KUNIT_EXPECT_EQ(test, bio->bi_iter.bi_bvec_done, offset);

	first = bio_split(bio, BIO_DMABUF_TEST_SPLIT_BYTES >> 9,
			  GFP_KERNEL, &fs_bio_set);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, first);
	second = bio_split(bio, BIO_DMABUF_TEST_SPLIT_BYTES >> 9,
			  GFP_KERNEL, &fs_bio_set);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, second);

	KUNIT_EXPECT_PTR_EQ(test, first->bi_dmabuf_map, &map);
	KUNIT_EXPECT_PTR_EQ(test, second->bi_dmabuf_map, &map);
	KUNIT_EXPECT_PTR_EQ(test, bio->bi_dmabuf_map, &map);

	KUNIT_EXPECT_EQ(test, first->bi_iter.bi_bvec_done, offset);
	KUNIT_EXPECT_EQ(test, second->bi_iter.bi_bvec_done,
			offset + BIO_DMABUF_TEST_SPLIT_BYTES);
	KUNIT_EXPECT_EQ(test, bio->bi_iter.bi_bvec_done,
			offset + 2 * BIO_DMABUF_TEST_SPLIT_BYTES);
	KUNIT_EXPECT_EQ(test, first->bi_iter.bi_size,
			BIO_DMABUF_TEST_SPLIT_BYTES);
	KUNIT_EXPECT_EQ(test, second->bi_iter.bi_size,
			BIO_DMABUF_TEST_SPLIT_BYTES);
	KUNIT_EXPECT_EQ(test, bio->bi_iter.bi_size,
			BIO_DMABUF_TEST_SPLIT_BYTES);

	bio_put(first);
	bio_put(second);
	bio_put(bio);
}

static void bio_dmabuf_limit_split_test(struct kunit *test)
{
	struct queue_limits limits = {
		.logical_block_size = SECTOR_SIZE,
		.max_segments = 1,
	};
	struct dma_buf_io_map map = {
		.seg_shift = PAGE_SHIFT,
	};
	struct bio *bio, *split;
	unsigned int segs;

	bio = bio_dmabuf_test_alloc(NULL, &map, WRITE, 0, 3 * PAGE_SIZE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, bio);
	split = bio_split_rw(bio, &limits, &segs, &fs_bio_set,
				     2 * PAGE_SIZE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, split);
	KUNIT_EXPECT_EQ(test, segs, 1U);
	KUNIT_EXPECT_EQ(test, split->bi_iter.bi_size, (unsigned int)PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, bio->bi_iter.bi_size, 2U * PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, split->bi_iter.bi_bvec_done, 0U);
	KUNIT_EXPECT_EQ(test, bio->bi_iter.bi_bvec_done, (unsigned int)PAGE_SIZE);

	bio_put(split);
	bio_put(bio);
}

#if IS_ENABLED(CONFIG_BLK_DEV_INTEGRITY)
struct bio_dmabuf_test_endio {
	bool called;
};

static void bio_dmabuf_test_endio(struct bio *bio)
{
	struct bio_dmabuf_test_endio *endio = bio->bi_private;

	endio->called = true;
}

static blk_status_t bio_dmabuf_test_verify(struct blk_integrity_iter *iter)
{
	return BLK_STS_OK;
}

static void bio_dmabuf_integrity_is_rejected_test(struct kunit *test)
{
	struct blk_integrity_profile profile = {
		.verify_fn = bio_dmabuf_test_verify,
		.name = "bio-dmabuf-kunit",
	};
	struct request_queue *queue;
	struct gendisk *disk;
	struct block_device *bdev;
	struct dma_buf_io_map map = {
		.seg_shift = PAGE_SHIFT,
	};
	struct bio_dmabuf_test_endio endio = {};
	struct bio *bio;

	queue = kunit_kzalloc(test, sizeof(*queue), GFP_KERNEL);
	disk = kunit_kzalloc(test, sizeof(*disk), GFP_KERNEL);
	bdev = kunit_kzalloc(test, sizeof(*bdev), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, queue);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, disk);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, bdev);
	queue->integrity.profile = &profile;
	queue->integrity.flags = BLK_INTEGRITY_VERIFY;
	queue->integrity.interval_exp = SECTOR_SHIFT;
	queue->integrity.tuple_size = 8;
	disk->queue = queue;
	bdev->bd_disk = disk;
	bdev->bd_queue = queue;

	bio = bio_dmabuf_test_alloc(bdev, &map, READ, 0,
					BIO_DMABUF_TEST_SPLIT_BYTES);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, bio);
	bio->bi_private = &endio;
	bio->bi_end_io = bio_dmabuf_test_endio;

	KUNIT_EXPECT_FALSE(test, bio_integrity_prep(bio));
	KUNIT_EXPECT_EQ(test, bio->bi_status, BLK_STS_NOTSUPP);
	KUNIT_EXPECT_TRUE(test, endio.called);

	bio_put(bio);
}
#endif

static struct kunit_case bio_dmabuf_test_cases[] = {
	KUNIT_CASE(bio_dmabuf_iterator_type_test),
	KUNIT_CASE(bio_dmabuf_iterator_cursor_test),
	KUNIT_CASE(bio_dmabuf_iterator_alignment_test),
	KUNIT_CASE(bio_dmabuf_iterator_zero_test),
	KUNIT_CASE(bio_dmabuf_split_keeps_map_cursor_test),
	KUNIT_CASE(bio_dmabuf_limit_split_test),
#if IS_ENABLED(CONFIG_BLK_DEV_INTEGRITY)
	KUNIT_CASE(bio_dmabuf_integrity_is_rejected_test),
#endif
	{}
};

static struct kunit_suite bio_dmabuf_test_suite = {
	.name = "bio-dmabuf",
	.test_cases = bio_dmabuf_test_cases,
};

kunit_test_suite(bio_dmabuf_test_suite);

MODULE_LICENSE("GPL");
