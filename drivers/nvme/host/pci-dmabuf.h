/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NVME_PCI_DMABUF_H
#define _NVME_PCI_DMABUF_H

#include <linux/dma-buf-io.h>
#include <linux/limits.h>
#include <linux/scatterlist.h>

#include "nvme.h"

struct nvme_dmabuf_map {
	struct dma_buf_io_map	base;
	struct sg_table		*sgt;
	unsigned int		nr_entries;
	dma_addr_t		dma_list[];
};

struct nvme_dmabuf_range {
	unsigned int	first;
	unsigned int	offset;
	unsigned int	nents;
};

static inline bool nvme_dmabuf_map_range(const struct nvme_dmabuf_map *map,
		unsigned int bvec_done, unsigned int length,
		struct nvme_dmabuf_range *range)
{
	unsigned int first = bvec_done >> NVME_CTRL_PAGE_SHIFT;
	unsigned int offset = bvec_done & (NVME_CTRL_PAGE_SIZE - 1);
	unsigned int nents;

	if (!length || first >= map->nr_entries ||
	    length > UINT_MAX - offset)
		return false;

	nents = DIV_ROUND_UP(offset + length, NVME_CTRL_PAGE_SIZE);
	if (nents > map->nr_entries - first)
		return false;

	range->first = first;
	range->offset = offset;
	range->nents = nents;
	return true;
}

static inline unsigned int nvme_dmabuf_sgl_nents(
		const struct nvme_dmabuf_map *map, unsigned int bvec_done,
		unsigned int length)
{
	struct nvme_dmabuf_range range;
	struct scatterlist *sg;
	unsigned long i;
	unsigned int offset = bvec_done, remaining = length, nents = 0;

	if (!map->sgt || !nvme_dmabuf_map_range(map, bvec_done, length,
						&range))
		return 0;

	for_each_sgtable_dma_sg(map->sgt, sg, i) {
		unsigned int sg_len = sg_dma_len(sg);

		if (!sg_len)
			return 0;
		if (offset >= sg_len) {
			offset -= sg_len;
			continue;
		}

		sg_len -= offset;
		offset = 0;
		if (nents == UINT_MAX)
			return 0;
		nents++;
		if (remaining <= sg_len)
			return nents;
		remaining -= sg_len;
	}

	return 0;
}

#endif /* _NVME_PCI_DMABUF_H */
