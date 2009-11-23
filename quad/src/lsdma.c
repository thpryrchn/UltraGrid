/* lsdma.c
 *
 * DMA linked-list buffer management for the Linear Systems DMA controller.
  *
 * Copyright (C) 1999 Tony Bolger <d7v@indigo.ie>
 * Copyright (C) 2000-2007 Linear Systems Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either Version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public Licence for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * Linear Systems can be contacted at <http://www.linsys.ca/>.
 *
 */

#include <linux/version.h> /* LINUX_VERSION_CODE */
#include <linux/slab.h> /* kmalloc () */
#include <linux/pci.h> /* pci_pool_create () */
#include <linux/errno.h> /* error codes */
#include <linux/mm.h> /* get_page () */

#include <asm/bitops.h> /* clear_bit () */
#include <asm/uaccess.h> /* copy_from_user () */

#include "lsdma.h"

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,5))
static inline void
pci_dma_sync_single_for_cpu (struct pci_dev *hwdev,
	dma_addr_t dma_handle,
	size_t size,
	int direction)
{
	if (direction == PCI_DMA_FROMDEVICE) {
		pci_dma_sync_single (hwdev, dma_handle, size, direction);
	}
	return;
}

static inline void
pci_dma_sync_single_for_device (struct pci_dev *hwdev,
	dma_addr_t dma_handle,
	size_t size,
	int direction)
{
	if (direction == PCI_DMA_TODEVICE) {
		pci_dma_sync_single (hwdev, dma_handle, size, direction);
	}
	return;
}
#endif

/* Static function prototypes */
static int lsdma_alloc_buffers (struct lsdma_dma *dma);
static void lsdma_free_buffers (struct lsdma_dma *dma);
static int lsdma_alloc_descriptors (struct lsdma_dma *dma);
static void lsdma_free_descriptors (struct lsdma_dma *dma);
static int lsdma_map_buffers (struct lsdma_dma *dma, u32 data_addr);
static void lsdma_unmap_buffers (struct lsdma_dma *dma);
static struct page *lsdma_nopage (struct vm_area_struct *vma,
	unsigned long address,
	int *type);

struct vm_operations_struct lsdma_vm_ops = {
	.nopage = lsdma_nopage
};

/**
 * lsdma_alloc_buffers - allocate DMA buffers
 * @dma: DMA information structure
 *
 * Returns a negative error code on failure and 0 on success.
 **/
static int
lsdma_alloc_buffers (struct lsdma_dma *dma)
{
	const unsigned int tail_size = dma->bufsize % PAGE_SIZE;
	const unsigned int tails_per_buf = (tail_size > 0) ? 1 : 0;
	const unsigned int total_pointers =
		dma->pointers_per_buf * dma->buffers;
	const unsigned int full_pages =
		dma->bufsize / PAGE_SIZE * dma->buffers;
	const unsigned int tails_per_page = (tail_size > 0) ?
		((dma->flags & LSDMA_MMAP) ? 1 : (PAGE_SIZE / tail_size)) : 0;
	const unsigned int tail_pages = (tail_size > 0) ?
		dma->buffers / tails_per_page +
		((dma->buffers % tails_per_page) ? 1 : 0) : 0;
	const unsigned int total_pages = full_pages + tail_pages;
	unsigned int i, bufnum;

	/* Allocate an array of pointers to pages */
	if ((dma->page = (unsigned long *)kmalloc (
		total_pages * sizeof (*dma->page),
		GFP_KERNEL)) == NULL) {
		goto NO_PAGE_PTR;
	}

	/* Allocate an array of pointers to locations in the page array */
	if ((dma->vpage = (unsigned char **)kmalloc (
		total_pointers * sizeof (*dma->vpage),
		GFP_KERNEL)) == NULL) {
		goto NO_VPAGE_PTR;
	}

	/* Allocate pages */
	for (i = 0; i < total_pages; i++) {
		if ((dma->page[i] = get_zeroed_page (GFP_KERNEL)) == 0) {
			int j;

			for (j = 0; j < i; j++) {
				free_page (dma->page[j]);
			}
			goto NO_BUF;
		}
	}

	/* Fill dma->vpage[] with pointers to pages and parts of pages
	 * in dma->page[] */
	for (i = 0; i < total_pointers; i++) {
		bufnum = i / dma->pointers_per_buf;
		dma->vpage[i] = (unsigned char *)(((tail_size > 0) &&
			((i % dma->pointers_per_buf) ==
			(dma->pointers_per_buf - 1))) ?
			dma->page[full_pages + bufnum / tails_per_page] +
			(bufnum % tails_per_page) * tail_size :
			dma->page[i - bufnum * tails_per_buf]);
	}

	return 0;

NO_BUF:
	kfree (dma->vpage);
NO_VPAGE_PTR:
	kfree (dma->page);
NO_PAGE_PTR:
	return -ENOMEM;
}

/**
 * lsdma_free_buffers - free DMA buffers
 * @dma: DMA information structure
 **/
static void
lsdma_free_buffers (struct lsdma_dma *dma)
{
	const unsigned int tail_size = dma->bufsize % PAGE_SIZE;
	const unsigned int full_pages =
		dma->bufsize / PAGE_SIZE * dma->buffers;
	const unsigned int tails_per_page = (tail_size > 0) ?
		((dma->flags & LSDMA_MMAP) ? 1 : (PAGE_SIZE / tail_size)) : 0;
	const unsigned int tail_pages = (tail_size > 0) ?
		dma->buffers / tails_per_page +
		((dma->buffers % tails_per_page) ? 1 : 0) : 0;
	const unsigned int total_pages = full_pages + tail_pages;
	unsigned int i;

	for (i = 0; i < total_pages; i++) {
		free_page (dma->page[i]);
	}
	kfree (dma->vpage);
	kfree (dma->page);
	return;
}

/**
 * lsdma_alloc_descriptors - allocate DMA descriptors
 * @dma: DMA information structure
 *
 * Returns a negative error code on failure and 0 on success.
 **/
static int
lsdma_alloc_descriptors (struct lsdma_dma *dma)
{
	const unsigned int total_pointers =
		dma->pointers_per_buf * dma->buffers;
	dma_addr_t dma_addr, first_dma_addr;
	struct lsdma_desc *desc;
	unsigned int i;

	/* Allocate an array of pointers to descriptors */
	if ((dma->desc = (struct lsdma_desc **)kmalloc (
		total_pointers * sizeof (*dma->desc),
		GFP_KERNEL)) == NULL) {
		goto NO_DESC_PTR;
	}

	/* Allocate the DMA descriptors */
	if ((dma->desc_pool = pci_pool_create ("lsdma",
		dma->pdev,
		sizeof (struct lsdma_desc),
		32,
		0)) == NULL) {
		goto NO_PCI_POOL;
	}
	if ((desc = dma->desc[0] = pci_pool_alloc (dma->desc_pool,
		GFP_KERNEL, &first_dma_addr)) == NULL) {
		goto NO_DESC;
	}

	for (i = 1; i < total_pointers; i++) {
		if ((dma->desc[i] = pci_pool_alloc (dma->desc_pool,
			GFP_KERNEL,
			&dma_addr)) == NULL) {
			unsigned int j;

			for (j = i - 1; j > 0; j--) {
				desc = dma->desc[j - 1];
				dma_addr = lsdma_desc_to_dma (desc->next_desc, desc->next_desc_h);
				pci_pool_free (dma->desc_pool,
					dma->desc[j],
					dma_addr);
			}
			pci_pool_free (dma->desc_pool,
				dma->desc[0],
				first_dma_addr);
			goto NO_DESC;
		}

		desc->next_desc = lsdma_dma_to_desc_low (dma_addr);
		desc->next_desc_h = lsdma_dma_to_desc_high (dma_addr);

		desc = dma->desc[i];
	}

	desc->next_desc = lsdma_dma_to_desc_low (first_dma_addr);
	desc->next_desc_h = lsdma_dma_to_desc_high (first_dma_addr);

	return 0;

NO_DESC:
	pci_pool_destroy (dma->desc_pool);
NO_PCI_POOL:
	kfree (dma->desc);
NO_DESC_PTR:
	return -ENOMEM;
}

/**
 * lsdma_free_descriptors - free DMA descriptors
 * @dma: DMA information structure
 **/
static void
lsdma_free_descriptors (struct lsdma_dma *dma)
{
	const unsigned int total_pointers =
		dma->pointers_per_buf * dma->buffers;
	struct lsdma_desc *prev_desc = dma->desc[total_pointers - 1];
	dma_addr_t last_dma_addr = lsdma_desc_to_dma (prev_desc->next_desc, prev_desc->next_desc_h);
	unsigned int i;

	for (i = total_pointers - 1; i > 0; i--) {
		prev_desc = dma->desc[i - 1];
		pci_pool_free (dma->desc_pool,
			dma->desc[i],
			lsdma_desc_to_dma (prev_desc->next_desc, prev_desc->next_desc_h));
	}
	pci_pool_free (dma->desc_pool,
		dma->desc[0], last_dma_addr);
	pci_pool_destroy (dma->desc_pool);
	kfree (dma->desc);
	return;
}

/**
 * lsdma_map_buffers - map DMA buffers
 * @dma: DMA information structure
 * @data_addr: local bus address of the data register
 *
 * Returns a negative error code on failure and 0 on success.
 **/
static int
lsdma_map_buffers (struct lsdma_dma *dma, u32 data_addr)
{
	const unsigned int total_pointers =
		dma->pointers_per_buf * dma->buffers;
	const unsigned int last_block_size = dma->bufsize -
		((dma->pointers_per_buf - 1) * PAGE_SIZE);
	struct lsdma_desc *desc;
	dma_addr_t dma_addr;
	unsigned int i;

	if (dma->direction == PCI_DMA_TODEVICE) {
		for (i = 0; i < total_pointers; i++) {
			desc = dma->desc[i];
			desc->dest_addr = data_addr;
			if ((i % dma->pointers_per_buf) ==
				(dma->pointers_per_buf - 1)) {
				/* This is the last descriptor for this buffer */
				desc->csr = LSDMA_DESC_CSR_INT |
					LSDMA_DESC_CSR_EOL |
					last_block_size;
			} else {
				desc->csr = PAGE_SIZE;
			}
			if ((dma_addr = pci_map_page (dma->pdev,
				virt_to_page (dma->vpage[i]),
				offset_in_page (dma->vpage[i]),
				(desc->csr & LSDMA_DESC_CSR_TOTALXFERSIZE),
				dma->direction)) == 0) {
				unsigned int j;

				for (j = 0; j < i; j++) {
					desc = dma->desc[j];
					pci_unmap_page (dma->pdev,
						lsdma_desc_to_dma (desc->src_addr, desc->src_addr_h),
						(desc->csr & LSDMA_DESC_CSR_TOTALXFERSIZE),
						dma->direction);
				}
				return -ENOMEM;
			}
			desc->src_addr = lsdma_dma_to_desc_low (dma_addr);
			desc->src_addr_h = lsdma_dma_to_desc_high (dma_addr);
		}
	} else {
		for (i = 0; i < total_pointers; i++) {
			desc = dma->desc[i];
			desc->src_addr = data_addr;
			if ((i % dma->pointers_per_buf) ==
				(dma->pointers_per_buf - 1)) {
				/* This is the last descriptor for this buffer */
				desc->csr = LSDMA_DESC_CSR_INT |
					LSDMA_DESC_CSR_DIRECTION |
					last_block_size;
			} else {
				desc->csr = LSDMA_DESC_CSR_DIRECTION |
					PAGE_SIZE;
			}
			if ((dma_addr = pci_map_page (dma->pdev,
				virt_to_page (dma->vpage[i]),
				offset_in_page (dma->vpage[i]),
				(desc->csr & LSDMA_DESC_CSR_TOTALXFERSIZE),
				dma->direction)) == 0) {
				unsigned int j;

				for (j = 0; j < i; j++) {
					desc = dma->desc[j];
					pci_unmap_page (dma->pdev,
						lsdma_desc_to_dma (desc->dest_addr, desc->dest_addr_h),
						(desc->csr & LSDMA_DESC_CSR_TOTALXFERSIZE),
						dma->direction);
				}
				return -ENOMEM;
			}
			desc->dest_addr = lsdma_dma_to_desc_low (dma_addr);
			desc->dest_addr_h = lsdma_dma_to_desc_high (dma_addr);
		}
	}
	return 0;
}

/**
 * lsdma_unmap_buffers - unmap DMA buffers
 * @dma: DMA information structure
 **/
static void
lsdma_unmap_buffers (struct lsdma_dma *dma)
{
	const unsigned int total_pointers =
		dma->pointers_per_buf * dma->buffers;
	struct lsdma_desc *desc;
	dma_addr_t dma_addr;
	unsigned int i;

	for (i = 0; i < total_pointers; i++) {
		desc = dma->desc[i];
		if (dma->direction == PCI_DMA_TODEVICE) {
			dma_addr = lsdma_desc_to_dma (desc->src_addr,
				desc->src_addr_h);
		} else {
			dma_addr = lsdma_desc_to_dma (desc->dest_addr,
				desc->dest_addr_h);
		}
		pci_unmap_page (dma->pdev,
			dma_addr,
			(desc->csr & LSDMA_DESC_CSR_TOTALXFERSIZE),
			dma->direction);
	}
	return;
}

/**
 * lsdma_alloc - Create a DMA information structure
 * @pdev: PCI device
 * @data_addr: local bus address of the data register
 * @buffers: number of buffers
 * @bufsize: number of bytes in each buffer
 * @direction: direction of data flow
 * @flags: allocation flags
 *
 * Returns %NULL on failure and a pointer to
 * the DMA information structure on success.
 **/
struct lsdma_dma *
lsdma_alloc (struct pci_dev *pdev,
	u32 data_addr,
	unsigned int buffers,
	unsigned int bufsize,
	unsigned int direction,
	unsigned int flags)
{
	const unsigned int tail_size = bufsize % PAGE_SIZE;
	const unsigned int tails_per_buf = (tail_size > 0) ? 1 : 0;
	struct lsdma_dma *dma;

	/* Allocate and initialize a DMA information structure */
	dma = (struct lsdma_dma *)kmalloc (sizeof (*dma), GFP_KERNEL);
	if (dma == NULL) {
		goto NO_DMA;
	}
	memset (dma, 0, sizeof (*dma));
	dma->pdev = pdev;
	dma->buffers = buffers;
	dma->bufsize = bufsize;
	dma->pointers_per_buf = bufsize / PAGE_SIZE + tails_per_buf;
	dma->direction = direction;
	dma->flags = flags;

	/* Allocate DMA buffers */
	if (lsdma_alloc_buffers (dma) < 0) {
		goto NO_BUF;
	}

	/* Allocate DMA descriptors */
	if (lsdma_alloc_descriptors (dma) < 0) {
		goto NO_DESC;
	}

	/* Map DMA buffers */
	if (lsdma_map_buffers (dma, data_addr) < 0) {
		goto NO_MAP;
	}

	return dma;

NO_MAP:
	lsdma_free_descriptors (dma);
NO_DESC:
	lsdma_free_buffers (dma);
NO_BUF:
	kfree (dma);
NO_DMA:
	return NULL;
}

/**
 * lsdma_free - Destroy a DMA information structure
 * @dma: DMA information structure
 **/
void
lsdma_free (struct lsdma_dma *dma)
{
	lsdma_unmap_buffers (dma);
	lsdma_free_descriptors (dma);
	lsdma_free_buffers (dma);
	kfree (dma);

	return;
}

/**
 * lsdma_read - copy data from driver buffer to user buffer
 * @dma: DMA information structure
 * @data: user buffer
 * @length: size of data
 *
 * Returns a negative error code on failure and
 * the number of bytes copied on success.
 **/
ssize_t
lsdma_read (struct lsdma_dma *dma, char *data, size_t length)
{
	/* Amount of data remaining in the current buffer */
	const size_t max_length = dma->bufsize - dma->cpu_offset;
	size_t chunkvpage, chunkoffset, chunksize;
	size_t copied = 0; /* Number of bytes copied */
	struct lsdma_desc *desc;

	/* Copy the rest of this buffer or the requested amount,
	 * whichever is less */
	if (length > max_length) {
		length = max_length;
	}
	while (length > 0) {
		chunkvpage = dma->cpu_buffer * dma->pointers_per_buf +
			dma->cpu_offset / PAGE_SIZE;
		chunkoffset = dma->cpu_offset % PAGE_SIZE;
		desc = dma->desc[chunkvpage];
		chunksize = (desc->csr & LSDMA_DESC_CSR_TOTALXFERSIZE) - chunkoffset;
		if (chunksize > length) {
			chunksize = length;
		}
		pci_dma_sync_single_for_cpu (dma->pdev,
			lsdma_desc_to_dma (desc->dest_addr, desc->dest_addr_h),
			(desc->csr & LSDMA_DESC_CSR_TOTALXFERSIZE),
			PCI_DMA_FROMDEVICE);
		if (copy_to_user (data + copied,
			dma->vpage[chunkvpage] + chunkoffset,
			chunksize)) {
			pci_dma_sync_single_for_device (dma->pdev,
				lsdma_desc_to_dma (desc->dest_addr, desc->dest_addr_h),
				(desc->csr & LSDMA_DESC_CSR_TOTALXFERSIZE),
				PCI_DMA_FROMDEVICE);
			return -EFAULT;
		}
		pci_dma_sync_single_for_device (dma->pdev,
			lsdma_desc_to_dma (desc->dest_addr, desc->dest_addr_h),
			(desc->csr & LSDMA_DESC_CSR_TOTALXFERSIZE),
			PCI_DMA_FROMDEVICE);
		dma->cpu_offset += chunksize;
		copied += chunksize;
		length -= chunksize;
	}

	if (copied == max_length) {
		dma->cpu_buffer = (dma->cpu_buffer + 1) % dma->buffers;
		dma->cpu_offset = 0;
	}
	return copied;
}

/**
 * lsdma_write - copy data from user buffer to driver buffer
 * @dma: DMA information structure
 * @data: user buffer
 * @length: size of data
 *
 * Returns a negative error code on failure and
 * the number of bytes copied on success.
 **/
ssize_t
lsdma_write (struct lsdma_dma *dma, const char *data, size_t length)
{
	/* Amount of free space remaining in the current buffer */
	const size_t max_length = dma->bufsize - dma->cpu_offset;
	size_t chunkvpage = 0, chunkoffset, max_chunk, chunksize;
	size_t copied = 0; /* Number of bytes copied */
	struct lsdma_desc *desc;

	/* Copy the rest of this buffer or the requested amount,
	 * whichever is less */
	if (length > max_length) {
		length = max_length;
	}
	while (length > 0) {
		chunkvpage = dma->cpu_buffer * dma->pointers_per_buf +
			dma->cpu_offset / PAGE_SIZE;
		chunkoffset = dma->cpu_offset % PAGE_SIZE;
		desc = dma->desc[chunkvpage];
		chunksize = max_chunk = (desc->csr & LSDMA_DESC_CSR_TOTALXFERSIZE) - chunkoffset;
		if (chunksize > length) {
			chunksize = length;
		}
		pci_dma_sync_single_for_cpu (dma->pdev,
			lsdma_desc_to_dma (desc->src_addr, desc->src_addr_h),
			(desc->csr & LSDMA_DESC_CSR_TOTALXFERSIZE),
			PCI_DMA_TODEVICE);
		if (copy_from_user (
			dma->vpage[chunkvpage] + chunkoffset,
			data + copied, chunksize)) {
			pci_dma_sync_single_for_device (dma->pdev,
				lsdma_desc_to_dma (desc->src_addr, desc->src_addr_h),
				(desc->csr & LSDMA_DESC_CSR_TOTALXFERSIZE),
				PCI_DMA_TODEVICE);
			return -EFAULT;
		}
		pci_dma_sync_single_for_device (dma->pdev,
			lsdma_desc_to_dma (desc->src_addr, desc->src_addr_h),
			(desc->csr & LSDMA_DESC_CSR_TOTALXFERSIZE),
			PCI_DMA_TODEVICE);
		dma->cpu_offset += chunksize;
		copied += chunksize;
		length -= chunksize;
	}

	/* If we've filled a buffer,
	 * link the descriptors for the buffer into the DMA chain */
	if (copied == max_length) {
		const unsigned int prevbuffer =
			(dma->cpu_buffer + dma->buffers - 1) %
			dma->buffers;
		const unsigned int oldlastvpage =
			prevbuffer * dma->pointers_per_buf +
			dma->pointers_per_buf - 1;

		desc = dma->desc[chunkvpage];
		desc->csr |= LSDMA_DESC_CSR_EOL;
		dma->cpu_buffer = (dma->cpu_buffer + 1) % dma->buffers;
		dma->cpu_offset = 0;
		desc = dma->desc[oldlastvpage];
		clear_bit (LSDMA_DESC_CSR_EOL_ORDER,
			(unsigned long *)&desc->csr);
	}
	return copied;
}

/**
 * lsdma_head_desc_bus_addr - return the bus address of the head descriptor
 * @dma: DMA information structure
 **/
dma_addr_t
lsdma_head_desc_bus_addr (struct lsdma_dma *dma)
{
	const unsigned int prevbuffer =
		(dma->dev_buffer + dma->buffers - 1) % dma->buffers;
	const unsigned int oldlastvpage =
		prevbuffer * dma->pointers_per_buf +
		dma->pointers_per_buf - 1;
	struct lsdma_desc *desc = dma->desc[oldlastvpage];

	return (lsdma_desc_to_dma (desc->next_desc, desc->next_desc_h));
}

/**
 * lsdma_tx_link_all - flush the transmit buffers
 * @dma: DMA information structure
 *
 * If there are any complete dwords in the transmit buffer
 * currently being written to, add them to the end of the
 * DMA chain.
 **/
void
lsdma_tx_link_all (struct lsdma_dma *dma)
{
	const unsigned int ptr = dma->cpu_buffer * dma->pointers_per_buf +
		dma->cpu_offset / PAGE_SIZE;
	const unsigned int actual_bytes =
		(dma->cpu_offset % PAGE_SIZE) & ~0x3;
	const unsigned int prevbuffer =
		(dma->cpu_buffer + dma->buffers - 1) %
		dma->buffers;
	const unsigned int oldlastptr = prevbuffer * dma->pointers_per_buf +
		dma->pointers_per_buf - 1;
	struct lsdma_desc *desc = dma->desc[ptr];
	struct lsdma_desc *oldlastdesc = dma->desc[oldlastptr];

	desc->csr = actual_bytes |
		LSDMA_DESC_CSR_EOL | LSDMA_DESC_CSR_INT;
	clear_bit (LSDMA_DESC_CSR_EOL_ORDER,
		(unsigned long *)&oldlastdesc->csr);
	clear_bit (LSDMA_DESC_CSR_INT_ORDER,
		(unsigned long *)&oldlastdesc->csr);
	return;
}

/**
 * lsdma_reset - reset the descriptor chain
 * @dma: DMA information structure
 **/
void
lsdma_reset (struct lsdma_dma *dma)
{
	const unsigned int total_pointers =
		dma->pointers_per_buf * dma->buffers;
	const unsigned int last_block_size = dma->bufsize -
		((dma->pointers_per_buf - 1) * PAGE_SIZE);
	unsigned int i;
	struct lsdma_desc *desc;

	if (dma->direction == PCI_DMA_TODEVICE) {
		for (i = 0; i < total_pointers; i++) {
			desc = dma->desc[i];
			if ((i % dma->pointers_per_buf) ==
				(dma->pointers_per_buf - 1)) {
				/* This is the last descriptor for this buffer */
				desc->csr = LSDMA_DESC_CSR_INT |
					LSDMA_DESC_CSR_EOL |
					last_block_size;
			} else {
				desc->csr = PAGE_SIZE;
			}
		}
	}
	dma->cpu_buffer = 0;
	dma->cpu_offset = 0;
	dma->dev_buffer = 0;
	return;
}

/**
 * lsdma_nopage - page fault handler
 * @vma: VMA
 * @address: address of the faulted page
 * @type: pointer to returned fault type
 *
 * Returns a pointer to the page if it is available,
 * NOPAGE_SIGBUS otherwise.
 **/
static struct page *
lsdma_nopage (struct vm_area_struct *vma,
	unsigned long address,
	int *type)
{
	struct lsdma_dma *dma = vma->vm_private_data;
	unsigned long offset = address - vma->vm_start +
		(vma->vm_pgoff << PAGE_SHIFT);
	void *pageptr;
	struct page *pg = NOPAGE_SIGBUS;

	if (offset >= dma->pointers_per_buf * dma->buffers * PAGE_SIZE) {
		goto OUT;
	}
	pageptr = dma->vpage[offset >> PAGE_SHIFT];
	pg = virt_to_page (pageptr);
	get_page (pg);
	if (type) {
		*type = VM_FAULT_MINOR;
	}
OUT:
	return pg;
}

/**
 * lsdma_txdqbuf - Dequeue a write buffer
 * @dma: DMA information structure
 * @bufnum: buffer number
 *
 * Do everything which normally precedes a copy from a user buffer
 * to a driver buffer.
 **/
ssize_t
lsdma_txdqbuf (struct lsdma_dma *dma, size_t bufnum)
{
	unsigned int i;
	struct lsdma_desc *desc;

	if (bufnum != dma->cpu_buffer) {
		return -EINVAL;
	}
	for (i = 0; i < dma->pointers_per_buf; i++) {
		desc = dma->desc[dma->cpu_buffer * dma->pointers_per_buf + i];

		pci_dma_sync_single_for_cpu (dma->pdev,
			lsdma_desc_to_dma (desc->src_addr, desc->src_addr_h),
			(desc->csr & LSDMA_DESC_CSR_TOTALXFERSIZE),
			PCI_DMA_TODEVICE);
	}
	return dma->bufsize;
}

/**
 * lsdma_txqbuf - Queue a write buffer
 * @dma: DMA information structure
 * @bufnum: buffer number
 *
 * Do everything which normally follows a copy from a user buffer
 * to a driver buffer.
 **/
ssize_t
lsdma_txqbuf (struct lsdma_dma *dma, size_t bufnum)
{
	const unsigned int prevbuffer =
		(dma->cpu_buffer + dma->buffers - 1) %
		dma->buffers;
	const unsigned int oldlastvpage =
		prevbuffer * dma->pointers_per_buf +
		dma->pointers_per_buf - 1;
	unsigned int i, vpage = 0;
	struct lsdma_desc *desc;

	if (bufnum != dma->cpu_buffer) {
		return -EINVAL;
	}
	for (i = 0; i < dma->pointers_per_buf; i++) {
		vpage = dma->cpu_buffer * dma->pointers_per_buf + i;
		desc = dma->desc[vpage];
		pci_dma_sync_single_for_device (dma->pdev,
			lsdma_desc_to_dma (desc->src_addr, desc->src_addr_h),
			(desc->csr & LSDMA_DESC_CSR_TOTALXFERSIZE),
			PCI_DMA_TODEVICE);
	}
	desc = dma->desc[vpage];
	desc->csr |= LSDMA_DESC_CSR_EOL;
	dma->cpu_buffer = (dma->cpu_buffer + 1) % dma->buffers;
	dma->cpu_offset = 0;
	desc = dma->desc[oldlastvpage];
	clear_bit (LSDMA_DESC_CSR_EOL_ORDER,
		(unsigned long *)&desc->csr);
	return dma->bufsize;
}

/**
 * lsdma_rxdqbuf - Dequeue a read buffer
 * @dma: DMA information structure
 * @bufnum: buffer number
 *
 * Do everything which normally precedes a copy from a driver buffer
 * to a user buffer.
 **/
ssize_t
lsdma_rxdqbuf (struct lsdma_dma *dma, size_t bufnum)
{
	unsigned int i;
	struct lsdma_desc *desc;

	if (bufnum != dma->cpu_buffer) {
		return -EINVAL;
	}
	for (i = 0; i < dma->pointers_per_buf; i++) {
		desc = dma->desc[dma->cpu_buffer * dma->pointers_per_buf + i];
		pci_dma_sync_single_for_cpu (dma->pdev,
			lsdma_desc_to_dma (desc->dest_addr, desc->dest_addr_h),
			(desc->csr & LSDMA_DESC_CSR_TOTALXFERSIZE),
			PCI_DMA_FROMDEVICE);
	}
	return dma->bufsize;
}

/**
 * lsdma_rxqbuf - Queue a read buffer
 * @dma: DMA information structure
 * @bufnum: buffer number
 *
 * Do everything which normally follows a copy from a driver buffer
 * to a user buffer.
 **/
ssize_t
lsdma_rxqbuf (struct lsdma_dma *dma, size_t bufnum)
{
	unsigned int i;
	struct lsdma_desc *desc;

	if (bufnum != dma->cpu_buffer) {
		return -EINVAL;
	}
	for (i = 0; i < dma->pointers_per_buf; i++) {
		desc = dma->desc[dma->cpu_buffer * dma->pointers_per_buf + i];
		pci_dma_sync_single_for_device (dma->pdev,
			lsdma_desc_to_dma (desc->dest_addr, desc->dest_addr_h),
			(desc->csr & LSDMA_DESC_CSR_TOTALXFERSIZE),
			PCI_DMA_FROMDEVICE);
	}
	dma->cpu_buffer = (dma->cpu_buffer + 1) % dma->buffers;
	dma->cpu_offset = 0;
	return dma->bufsize;
}

