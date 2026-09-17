// SPDX-License-Identifier: GPL-2.0

#include <kunit/test.h>

#include "pci-dmabuf.h"

static void nvme_dmabuf_range_crosses_controller_pages(struct kunit *test)
{
	struct nvme_dmabuf_map map = {
		.nr_entries = 8,
	};
	struct nvme_dmabuf_range range;

	KUNIT_ASSERT_TRUE(test, nvme_dmabuf_map_range(&map,
			NVME_CTRL_PAGE_SIZE / 2, 2 * NVME_CTRL_PAGE_SIZE,
			&range));
	KUNIT_EXPECT_EQ(test, range.first, 0U);
	KUNIT_EXPECT_EQ(test, range.offset, NVME_CTRL_PAGE_SIZE / 2);
	KUNIT_EXPECT_EQ(test, range.nents, 3U);
}

static void nvme_dmabuf_range_uses_bio_cursor(struct kunit *test)
{
	struct nvme_dmabuf_map map = {
		.nr_entries = 8,
	};
	struct nvme_dmabuf_range range;

	KUNIT_ASSERT_TRUE(test, nvme_dmabuf_map_range(&map,
			2 * NVME_CTRL_PAGE_SIZE, NVME_CTRL_PAGE_SIZE, &range));
	KUNIT_EXPECT_EQ(test, range.first, 2U);
	KUNIT_EXPECT_EQ(test, range.offset, 0U);
	KUNIT_EXPECT_EQ(test, range.nents, 1U);
}

static void nvme_dmabuf_range_rejects_overrun(struct kunit *test)
{
	struct nvme_dmabuf_map map = {
		.nr_entries = 8,
	};
	struct nvme_dmabuf_range range;

	KUNIT_EXPECT_FALSE(test, nvme_dmabuf_map_range(&map,
			7 * NVME_CTRL_PAGE_SIZE, 2 * NVME_CTRL_PAGE_SIZE,
			&range));
}

static void nvme_dmabuf_sgl_counts_requested_segments(struct kunit *test)
{
	struct scatterlist sgl[3];
	struct sg_table sgt = {
		.sgl = sgl,
		.nents = ARRAY_SIZE(sgl),
		.orig_nents = ARRAY_SIZE(sgl),
	};
	struct nvme_dmabuf_map map = {
		.sgt = &sgt,
		.nr_entries = 5,
	};

	sg_init_table(sgl, ARRAY_SIZE(sgl));
	sg_dma_len(&sgl[0]) = 2 * NVME_CTRL_PAGE_SIZE;
	sg_dma_len(&sgl[1]) = 2 * NVME_CTRL_PAGE_SIZE;
	sg_dma_len(&sgl[2]) = NVME_CTRL_PAGE_SIZE;

	KUNIT_EXPECT_EQ(test, nvme_dmabuf_sgl_nents(&map,
			NVME_CTRL_PAGE_SIZE + NVME_CTRL_PAGE_SIZE / 2,
			3 * NVME_CTRL_PAGE_SIZE), 3U);
}

static void nvme_dmabuf_sgl_rejects_incomplete_mapping(struct kunit *test)
{
	struct scatterlist sgl[3];
	struct sg_table sgt = {
		.sgl = sgl,
		.nents = 2,
		.orig_nents = ARRAY_SIZE(sgl),
	};
	struct nvme_dmabuf_map map = {
		.sgt = &sgt,
		.nr_entries = 5,
	};

	sg_init_table(sgl, ARRAY_SIZE(sgl));
	sg_dma_len(&sgl[0]) = 2 * NVME_CTRL_PAGE_SIZE;
	sg_dma_len(&sgl[1]) = 2 * NVME_CTRL_PAGE_SIZE;
	sg_dma_len(&sgl[2]) = NVME_CTRL_PAGE_SIZE;

	KUNIT_EXPECT_EQ(test, nvme_dmabuf_sgl_nents(&map,
			NVME_CTRL_PAGE_SIZE + NVME_CTRL_PAGE_SIZE / 2,
			3 * NVME_CTRL_PAGE_SIZE), 0U);
}

static struct kunit_case nvme_dmabuf_range_cases[] = {
	KUNIT_CASE(nvme_dmabuf_range_crosses_controller_pages),
	KUNIT_CASE(nvme_dmabuf_range_uses_bio_cursor),
	KUNIT_CASE(nvme_dmabuf_range_rejects_overrun),
	KUNIT_CASE(nvme_dmabuf_sgl_counts_requested_segments),
	KUNIT_CASE(nvme_dmabuf_sgl_rejects_incomplete_mapping),
	{}
};

static struct kunit_suite nvme_dmabuf_range_suite = {
	.name = "nvme-dmabuf",
	.test_cases = nvme_dmabuf_range_cases,
};

kunit_test_suite(nvme_dmabuf_range_suite);

MODULE_LICENSE("GPL");
