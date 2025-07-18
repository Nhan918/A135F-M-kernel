/*
 * Procedures for maintaining information about logical memory blocks.
 *
 * Peter Bergner, IBM Corp.	June 2001.
 * Copyright (C) 2001 Peter Bergner.
 *
 *      This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/bitops.h>
#include <linux/poison.h>
#include <linux/pfn.h>
#include <linux/debugfs.h>
#include <linux/kmemleak.h>
#include <linux/seq_file.h>
#include <linux/memblock.h>
#include <linux/bootmem.h>

#include <asm/sections.h>
#include <linux/io.h>
#include <linux/proc_fs.h>
#include <linux/sort.h>

#include "internal.h"

/**
 * DOC: memblock overview
 *
 * Memblock is a method of managing memory regions during the early
 * boot period when the usual kernel memory allocators are not up and
 * running.
 *
 * Memblock views the system memory as collections of contiguous
 * regions. There are several types of these collections:
 *
 * * ``memory`` - describes the physical memory available to the
 *   kernel; this may differ from the actual physical memory installed
 *   in the system, for instance when the memory is restricted with
 *   ``mem=`` command line parameter
 * * ``reserved`` - describes the regions that were allocated
 * * ``physmap`` - describes the actual physical memory regardless of
 *   the possible restrictions; the ``physmap`` type is only available
 *   on some architectures.
 *
 * Each region is represented by :c:type:`struct memblock_region` that
 * defines the region extents, its attributes and NUMA node id on NUMA
 * systems. Every memory type is described by the :c:type:`struct
 * memblock_type` which contains an array of memory regions along with
 * the allocator metadata. The memory types are nicely wrapped with
 * :c:type:`struct memblock`. This structure is statically initialzed
 * at build time. The region arrays for the "memory" and "reserved"
 * types are initially sized to %INIT_MEMBLOCK_REGIONS and for the
 * "physmap" type to %INIT_PHYSMEM_REGIONS.
 * The :c:func:`memblock_allow_resize` enables automatic resizing of
 * the region arrays during addition of new regions. This feature
 * should be used with care so that memory allocated for the region
 * array will not overlap with areas that should be reserved, for
 * example initrd.
 *
 * The early architecture setup should tell memblock what the physical
 * memory layout is by using :c:func:`memblock_add` or
 * :c:func:`memblock_add_node` functions. The first function does not
 * assign the region to a NUMA node and it is appropriate for UMA
 * systems. Yet, it is possible to use it on NUMA systems as well and
 * assign the region to a NUMA node later in the setup process using
 * :c:func:`memblock_set_node`. The :c:func:`memblock_add_node`
 * performs such an assignment directly.
 *
 * Once memblock is setup the memory can be allocated using either
 * memblock or bootmem APIs.
 *
 * As the system boot progresses, the architecture specific
 * :c:func:`mem_init` function frees all the memory to the buddy page
 * allocator.
 *
 * If an architecure enables %CONFIG_ARCH_DISCARD_MEMBLOCK, the
 * memblock data structures will be discarded after the system
 * initialization compltes.
 */

static struct memblock_region memblock_memory_init_regions[INIT_MEMBLOCK_REGIONS] __initdata_memblock;
static struct memblock_region memblock_reserved_init_regions[INIT_MEMBLOCK_REGIONS] __initdata_memblock;
#ifdef CONFIG_HAVE_MEMBLOCK_PHYS_MAP
static struct memblock_region memblock_physmem_init_regions[INIT_PHYSMEM_REGIONS] __initdata_memblock;
#endif

struct memblock memblock __initdata_memblock = {
	.memory.regions		= memblock_memory_init_regions,
	.memory.cnt		= 1,	/* empty dummy entry */
	.memory.max		= INIT_MEMBLOCK_REGIONS,
	.memory.name		= "memory",

	.reserved.regions	= memblock_reserved_init_regions,
	.reserved.cnt		= 1,	/* empty dummy entry */
	.reserved.max		= INIT_MEMBLOCK_REGIONS,
	.reserved.name		= "reserved",

#ifdef CONFIG_HAVE_MEMBLOCK_PHYS_MAP
	.physmem.regions	= memblock_physmem_init_regions,
	.physmem.cnt		= 1,	/* empty dummy entry */
	.physmem.max		= INIT_PHYSMEM_REGIONS,
	.physmem.name		= "physmem",
#endif

	.bottom_up		= false,
	.current_limit		= MEMBLOCK_ALLOC_ANYWHERE,
};

int memblock_debug __initdata_memblock;
static bool system_has_some_mirror __initdata_memblock = false;
static int memblock_can_resize __initdata_memblock;
static int memblock_memory_in_slab __initdata_memblock = 0;
static int memblock_reserved_in_slab __initdata_memblock = 0;

enum memblock_flags __init_memblock choose_memblock_flags(void)
{
	return system_has_some_mirror ? MEMBLOCK_MIRROR : MEMBLOCK_NONE;
}

/* adjust *@size so that (@base + *@size) doesn't overflow, return new size */
static inline phys_addr_t memblock_cap_size(phys_addr_t base, phys_addr_t *size)
{
	return *size = min(*size, PHYS_ADDR_MAX - base);
}

/*
 * Address comparison utilities
 */
static unsigned long __init_memblock memblock_addrs_overlap(phys_addr_t base1, phys_addr_t size1,
				       phys_addr_t base2, phys_addr_t size2)
{
	return ((base1 < (base2 + size2)) && (base2 < (base1 + size1)));
}

bool __init_memblock memblock_overlaps_region(struct memblock_type *type,
					phys_addr_t base, phys_addr_t size)
{
	unsigned long i;

	for (i = 0; i < type->cnt; i++)
		if (memblock_addrs_overlap(base, size, type->regions[i].base,
					   type->regions[i].size))
			break;
	return i < type->cnt;
}

/**
 * __memblock_find_range_bottom_up - find free area utility in bottom-up
 * @start: start of candidate range
 * @end: end of candidate range, can be %MEMBLOCK_ALLOC_ANYWHERE or
 *       %MEMBLOCK_ALLOC_ACCESSIBLE
 * @size: size of free area to find
 * @align: alignment of free area to find
 * @nid: nid of the free area to find, %NUMA_NO_NODE for any node
 * @flags: pick from blocks based on memory attributes
 *
 * Utility called from memblock_find_in_range_node(), find free area bottom-up.
 *
 * Return:
 * Found address on success, 0 on failure.
 */
static phys_addr_t __init_memblock
__memblock_find_range_bottom_up(phys_addr_t start, phys_addr_t end,
				phys_addr_t size, phys_addr_t align, int nid,
				enum memblock_flags flags)
{
	phys_addr_t this_start, this_end, cand;
	u64 i;

	for_each_free_mem_range(i, nid, flags, &this_start, &this_end, NULL) {
		this_start = clamp(this_start, start, end);
		this_end = clamp(this_end, start, end);

		cand = round_up(this_start, align);
		if (cand < this_end && this_end - cand >= size)
			return cand;
	}

	return 0;
}

/**
 * __memblock_find_range_top_down - find free area utility, in top-down
 * @start: start of candidate range
 * @end: end of candidate range, can be %MEMBLOCK_ALLOC_ANYWHERE or
 *       %MEMBLOCK_ALLOC_ACCESSIBLE
 * @size: size of free area to find
 * @align: alignment of free area to find
 * @nid: nid of the free area to find, %NUMA_NO_NODE for any node
 * @flags: pick from blocks based on memory attributes
 *
 * Utility called from memblock_find_in_range_node(), find free area top-down.
 *
 * Return:
 * Found address on success, 0 on failure.
 */
static phys_addr_t __init_memblock
__memblock_find_range_top_down(phys_addr_t start, phys_addr_t end,
			       phys_addr_t size, phys_addr_t align, int nid,
			       enum memblock_flags flags)
{
	phys_addr_t this_start, this_end, cand;
	u64 i;

	for_each_free_mem_range_reverse(i, nid, flags, &this_start, &this_end,
					NULL) {
		this_start = clamp(this_start, start, end);
		this_end = clamp(this_end, start, end);

		if (this_end < size)
			continue;

		cand = round_down(this_end - size, align);
		if (cand >= this_start)
			return cand;
	}

	return 0;
}

/**
 * memblock_find_in_range_node - find free area in given range and node
 * @size: size of free area to find
 * @align: alignment of free area to find
 * @start: start of candidate range
 * @end: end of candidate range, can be %MEMBLOCK_ALLOC_ANYWHERE or
 *       %MEMBLOCK_ALLOC_ACCESSIBLE
 * @nid: nid of the free area to find, %NUMA_NO_NODE for any node
 * @flags: pick from blocks based on memory attributes
 *
 * Find @size free area aligned to @align in the specified range and node.
 *
 * Return:
 * Found address on success, 0 on failure.
 */
phys_addr_t __init_memblock memblock_find_in_range_node(phys_addr_t size,
					phys_addr_t align, phys_addr_t start,
					phys_addr_t end, int nid,
					enum memblock_flags flags)
{
	/* pump up @end */
	if (end == MEMBLOCK_ALLOC_ACCESSIBLE ||
	    end == MEMBLOCK_ALLOC_KASAN)
		end = memblock.current_limit;

	/* avoid allocating the first page */
	start = max_t(phys_addr_t, start, PAGE_SIZE);
	end = max(start, end);

	if (memblock_bottom_up())
		return __memblock_find_range_bottom_up(start, end, size, align,
						       nid, flags);
	else
		return __memblock_find_range_top_down(start, end, size, align,
						      nid, flags);
}

/**
 * memblock_find_in_range - find free area in given range
 * @start: start of candidate range
 * @end: end of candidate range, can be %MEMBLOCK_ALLOC_ANYWHERE or
 *       %MEMBLOCK_ALLOC_ACCESSIBLE
 * @size: size of free area to find
 * @align: alignment of free area to find
 *
 * Find @size free area aligned to @align in the specified range.
 *
 * Return:
 * Found address on success, 0 on failure.
 */
phys_addr_t __init_memblock memblock_find_in_range(phys_addr_t start,
					phys_addr_t end, phys_addr_t size,
					phys_addr_t align)
{
	phys_addr_t ret;
	enum memblock_flags flags = choose_memblock_flags();

again:
	ret = memblock_find_in_range_node(size, align, start, end,
					    NUMA_NO_NODE, flags);

	if (!ret && (flags & MEMBLOCK_MIRROR)) {
		pr_warn("Could not allocate %pap bytes of mirrored memory\n",
			&size);
		flags &= ~MEMBLOCK_MIRROR;
		goto again;
	}

	return ret;
}

static void __init_memblock memblock_remove_region(struct memblock_type *type, unsigned long r)
{
	type->total_size -= type->regions[r].size;
	memmove(&type->regions[r], &type->regions[r + 1],
		(type->cnt - (r + 1)) * sizeof(type->regions[r]));
	type->cnt--;

	/* Special case for empty arrays */
	if (type->cnt == 0) {
		WARN_ON(type->total_size != 0);
		type->cnt = 1;
		type->regions[0].base = 0;
		type->regions[0].size = 0;
		type->regions[0].flags = 0;
		memblock_set_region_node(&type->regions[0], MAX_NUMNODES);
	}
}

#ifdef CONFIG_ARCH_DISCARD_MEMBLOCK
/**
 * memblock_discard - discard memory and reserved arrays if they were allocated
 */
void __init memblock_discard(void)
{
	phys_addr_t addr, size;

	if (memblock.reserved.regions != memblock_reserved_init_regions) {
		addr = __pa(memblock.reserved.regions);
		size = PAGE_ALIGN(sizeof(struct memblock_region) *
				  memblock.reserved.max);
		__memblock_free_late(addr, size);
	}

	if (memblock.memory.regions != memblock_memory_init_regions) {
		addr = __pa(memblock.memory.regions);
		size = PAGE_ALIGN(sizeof(struct memblock_region) *
				  memblock.memory.max);
		__memblock_free_late(addr, size);
	}
}
#endif

/**
 * memblock_double_array - double the size of the memblock regions array
 * @type: memblock type of the regions array being doubled
 * @new_area_start: starting address of memory range to avoid overlap with
 * @new_area_size: size of memory range to avoid overlap with
 *
 * Double the size of the @type regions array. If memblock is being used to
 * allocate memory for a new reserved regions array and there is a previously
 * allocated memory range [@new_area_start, @new_area_start + @new_area_size]
 * waiting to be reserved, ensure the memory used by the new array does
 * not overlap.
 *
 * Return:
 * 0 on success, -1 on failure.
 */
static int __init_memblock memblock_double_array(struct memblock_type *type,
						phys_addr_t new_area_start,
						phys_addr_t new_area_size)
{
	struct memblock_region *new_array, *old_array;
	phys_addr_t old_alloc_size, new_alloc_size;
	phys_addr_t old_size, new_size, addr, new_end;
	int use_slab = slab_is_available();
	int *in_slab;

	/* We don't allow resizing until we know about the reserved regions
	 * of memory that aren't suitable for allocation
	 */
	if (!memblock_can_resize)
		return -1;

	/* Calculate new doubled size */
	old_size = type->max * sizeof(struct memblock_region);
	new_size = old_size << 1;
	/*
	 * We need to allocated new one align to PAGE_SIZE,
	 *   so we can free them completely later.
	 */
	old_alloc_size = PAGE_ALIGN(old_size);
	new_alloc_size = PAGE_ALIGN(new_size);

	/* Retrieve the slab flag */
	if (type == &memblock.memory)
		in_slab = &memblock_memory_in_slab;
	else
		in_slab = &memblock_reserved_in_slab;

	/* Try to find some space for it.
	 *
	 * WARNING: We assume that either slab_is_available() and we use it or
	 * we use MEMBLOCK for allocations. That means that this is unsafe to
	 * use when bootmem is currently active (unless bootmem itself is
	 * implemented on top of MEMBLOCK which isn't the case yet)
	 *
	 * This should however not be an issue for now, as we currently only
	 * call into MEMBLOCK while it's still active, or much later when slab
	 * is active for memory hotplug operations
	 */
	if (use_slab) {
		new_array = kmalloc(new_size, GFP_KERNEL);
		addr = new_array ? __pa(new_array) : 0;
	} else {
		/* only exclude range when trying to double reserved.regions */
		if (type != &memblock.reserved)
			new_area_start = new_area_size = 0;

		addr = memblock_find_in_range(new_area_start + new_area_size,
						memblock.current_limit,
						new_alloc_size, PAGE_SIZE);
		if (!addr && new_area_size)
			addr = memblock_find_in_range(0,
				min(new_area_start, memblock.current_limit),
				new_alloc_size, PAGE_SIZE);

		new_array = addr ? __va(addr) : NULL;
	}
	if (!addr) {
		pr_err("memblock: Failed to double %s array from %ld to %ld entries !\n",
		       type->name, type->max, type->max * 2);
		return -1;
	}

	new_end = addr + new_size - 1;
	memblock_dbg("memblock: %s is doubled to %ld at [%pa-%pa]",
			type->name, type->max * 2, &addr, &new_end);

	/*
	 * Found space, we now need to move the array over before we add the
	 * reserved region since it may be our reserved array itself that is
	 * full.
	 */
	memcpy(new_array, type->regions, old_size);
	memset(new_array + type->max, 0, old_size);
	old_array = type->regions;
	type->regions = new_array;
	type->max <<= 1;

	/* Free old array. We needn't free it if the array is the static one */
	if (*in_slab)
		kfree(old_array);
	else if (old_array != memblock_memory_init_regions &&
		 old_array != memblock_reserved_init_regions)
		memblock_free(__pa(old_array), old_alloc_size);

	/*
	 * Reserve the new array if that comes from the memblock.  Otherwise, we
	 * needn't do it
	 */
	if (!use_slab)
		BUG_ON(memblock_reserve(addr, new_alloc_size));

	/* Update slab flag */
	*in_slab = use_slab;

	return 0;
}

/**
 * memblock_merge_regions - merge neighboring compatible regions
 * @type: memblock type to scan
 *
 * Scan @type and merge neighboring compatible regions.
 */
static void __init_memblock memblock_merge_regions(struct memblock_type *type)
{
	int i = 0;

	/* cnt never goes below 1 */
	while (i < type->cnt - 1) {
		struct memblock_region *this = &type->regions[i];
		struct memblock_region *next = &type->regions[i + 1];

		if (this->base + this->size != next->base ||
		    memblock_get_region_node(this) !=
		    memblock_get_region_node(next) ||
		    this->flags != next->flags) {
			BUG_ON(this->base + this->size > next->base);
			i++;
			continue;
		}

		this->size += next->size;
		/* move forward from next + 1, index of which is i + 2 */
		memmove(next, next + 1, (type->cnt - (i + 2)) * sizeof(*next));
		type->cnt--;
	}
}

/**
 * memblock_insert_region - insert new memblock region
 * @type:	memblock type to insert into
 * @idx:	index for the insertion point
 * @base:	base address of the new region
 * @size:	size of the new region
 * @nid:	node id of the new region
 * @flags:	flags of the new region
 *
 * Insert new memblock region [@base, @base + @size) into @type at @idx.
 * @type must already have extra room to accommodate the new region.
 */
static void __init_memblock memblock_insert_region(struct memblock_type *type,
						   int idx, phys_addr_t base,
						   phys_addr_t size,
						   int nid,
						   enum memblock_flags flags)
{
	struct memblock_region *rgn = &type->regions[idx];

	BUG_ON(type->cnt >= type->max);
	memmove(rgn + 1, rgn, (type->cnt - idx) * sizeof(*rgn));
	rgn->base = base;
	rgn->size = size;
	rgn->flags = flags;
	memblock_set_region_node(rgn, nid);
	type->cnt++;
	type->total_size += size;
}

#define NAME_SIZE	14
struct reserved_mem_reg {
	phys_addr_t	base;
	long		size;
	bool		nomap;			/*  1/16 byte */
	bool		reusable;		/*  1/16 byte */
	char		name[NAME_SIZE];	/* 14/16 byte */
};

static struct reserved_mem_reg kernel_mem_reg[] = {
	[MEMSIZE_KERNEL_KERNEL]		= {0, 0, false, false, "Kernel    "},
	[MEMSIZE_KERNEL_PAGING]		= {0, 0, false, false, "paging    "},
	[MEMSIZE_KERNEL_LOGBUF]		= {0, 0, false, false, "log_buffer"},
	[MEMSIZE_KERNEL_PIDHASH]	= {0, 0, false, false, "pid_hash  "},
	[MEMSIZE_KERNEL_VFSHASH]	= {0, 0, false, false, "vfs_hash  "},
	[MEMSIZE_KERNEL_MM_INIT]	= {0, 0, false, false, "mm_init   "},
	[MEMSIZE_KERNEL_OTHERS]		= {0, 0, false, false, "others    "},
};

#define MAX_RESERVED_MEM_REG	64
static struct reserved_mem_reg reserved_mem_reg[MAX_RESERVED_MEM_REG];
static int reserved_mem_reg_count;

static enum memsize_kernel_type memsize_kernel_type = MEMSIZE_KERNEL_STOP;
static const char *memsize_reserved_name;

void set_memsize_kernel_type(enum memsize_kernel_type type)
{
	memsize_kernel_type = type;
}

void set_memsize_reserved_name(const char *name)
{
	memsize_reserved_name = name;
}

void unset_memsize_reserved_name(void)
{
	memsize_reserved_name = NULL;
}

/* assume that freeing region is NOT bigger than the previous region */
void free_memsize_reserved(phys_addr_t free_base, phys_addr_t free_size)
{
	int i;
	struct reserved_mem_reg *rmem_reg;
	phys_addr_t free_end, end;

	for (i = 0 ; i < reserved_mem_reg_count; i++) {
		rmem_reg = &reserved_mem_reg[i];

		end = rmem_reg->base + rmem_reg->size;
		if (free_base < rmem_reg->base ||
		    free_base >= end)
			continue;

		free_end = free_base + free_size;
		if (free_base == rmem_reg->base) {
			rmem_reg->size -= free_size;
			if (rmem_reg->size != 0)
				rmem_reg->base += free_size;
		} else if (free_end == end) {
			rmem_reg->size -= free_size;
		} else {
			record_memsize_reserved(rmem_reg->name,
				free_end, end - free_end, rmem_reg->nomap,
				rmem_reg->reusable);
			rmem_reg->size = free_base - rmem_reg->base;
		}
	}
}

void record_memsize_size_only(enum memsize_kernel_type type, long size)
{
	struct reserved_mem_reg *rmem_reg;

	if (type >= ARRAY_SIZE(kernel_mem_reg)) {
		pr_err("type index is out ouf kernel_mem_reg\n");
		return;
	}
	rmem_reg = &kernel_mem_reg[type];
	rmem_reg->size += size;
}

static int memsize_get_valid_name_size(const char *name)
{
	char *found;
	int name_size;

	name_size = strlen(name);
	found = strstr(name, "_region");
	if (found)
		name_size = found - name;
	found = strchr(name, '@');
	if (found && (name_size > found - name))
		name_size = found - name;
	if (name_size > NAME_SIZE - 1)
		name_size = NAME_SIZE - 1;

	return name_size;
}

static inline struct reserved_mem_reg *memsize_get_new_reg(void)
{
	if (reserved_mem_reg_count == ARRAY_SIZE(reserved_mem_reg)) {
		pr_err("not enough space on reserved_mem_reg\n");
		return NULL;
	}
	return &reserved_mem_reg[reserved_mem_reg_count++];
}

/* The memory region can be added into memblock reserved even after the same
 * memory region was already removed out of memblock memory. Let's assume that
 * additions to memblock reserved are valid information to be clear. Get the
 * new address as a new region and remove the new address out of the existing
 * region.
 */
static bool memsize_update_nomap_region(const char *name, phys_addr_t base,
					phys_addr_t size, bool nomap)
{
	int i;
	struct reserved_mem_reg *rmem_reg, *new_reg;
	int name_size;

	if (!name || nomap)
		return false;

	for (i = 0; i < reserved_mem_reg_count; i++) {
		rmem_reg = &reserved_mem_reg[i];

		if (!rmem_reg->nomap)
			continue;
		if (base < rmem_reg->base)
			continue;
		if (base + size > rmem_reg->base + rmem_reg->size)
			continue;

		name_size = memsize_get_valid_name_size(name);
		if (base == rmem_reg->base && size == rmem_reg->size) {
			strncpy(rmem_reg->name, name, name_size);
			rmem_reg->name[NAME_SIZE - 1] = '\0';
			return true;
		}
		new_reg = memsize_get_new_reg();
		if (!new_reg)
			return true;
		new_reg->base = base;
		new_reg->size = size;
		new_reg->nomap = nomap;
		new_reg->reusable = false;
		strncpy(new_reg->name, name, name_size);
		new_reg->name[NAME_SIZE - 1] = '\0';

		if (base == rmem_reg->base && size < rmem_reg->size) {
			rmem_reg->base = base + size;
			rmem_reg->size -= size;
		} else if (base + size == rmem_reg->base + rmem_reg->size) {
			rmem_reg->size -= size;
		} else {
			new_reg = memsize_get_new_reg();
			if (!new_reg)
				return true;
			new_reg->base = base + size;
			new_reg->size = (rmem_reg->base + rmem_reg->size)
					- (base + size);
			new_reg->nomap = nomap;
			new_reg->reusable = false;
			strcpy(new_reg->name, "unknown");
			rmem_reg->size = base - rmem_reg->base;
		}
		return true;
	}

	return false;
}

void record_memsize_reserved(const char *name, phys_addr_t base,
			     phys_addr_t size, bool nomap, bool reusable)
{
	struct reserved_mem_reg *rmem_reg;
	int name_size;

	if (memsize_update_nomap_region(name, base, size, nomap))
		return;

	rmem_reg = memsize_get_new_reg();
	if (!rmem_reg)
		return;

	rmem_reg->base = base;
	rmem_reg->size = size;
	rmem_reg->nomap = nomap;
	rmem_reg->reusable = reusable;

	if (!name) {
		strcpy(rmem_reg->name, "unknown");
	} else {
		name_size = memsize_get_valid_name_size(name);
		strncpy(rmem_reg->name, name, name_size);
		rmem_reg->name[NAME_SIZE - 1] = '\0';
	}
}

/* This function will be called to by early_init_dt_scan_nodes */
void record_memsize_memory_hole(void)
{
	phys_addr_t base, end;
	phys_addr_t prev_end, hole_s;
	int idx;
	struct memblock_region *rgn;
	int memblock_cnt = (int)memblock.memory.cnt;

	/* assume that the hole size is less than 256 MB */
	for_each_memblock_type(idx, (&memblock.memory), rgn) {
		if (idx == 0)
			prev_end = round_down(base, SZ_256M);
		else
			prev_end = end;
		base = rgn->base;
		end = rgn->base + rgn->size;

		/* only for the last */
		if (idx + 1 == memblock_cnt) {
			hole_s = round_up(end, SZ_256M) - end;
			if (hole_s)
				record_memsize_reserved(NULL, end, hole_s, 1, 0);
		}

		/* for each region */
		hole_s = base - prev_end;
		if (!hole_s)
			continue;
		if (hole_s < SZ_256M) {
			record_memsize_reserved(NULL, prev_end, hole_s, 1, 0);
		} else {
			phys_addr_t hole_s1, hole_s2;

			hole_s1 = round_up(prev_end, SZ_256M) - prev_end;
			if (hole_s1)
				record_memsize_reserved(NULL, prev_end,
							hole_s1, 1, 0);
			hole_s2 = base % SZ_256M;
			if (hole_s2)
				record_memsize_reserved(NULL, base - hole_s2,
							hole_s2, 1, 0);
		}
	}
}

/**
 * memblock_add_range - add new memblock region
 * @type: memblock type to add new region into
 * @base: base address of the new region
 * @size: size of the new region
 * @nid: nid of the new region
 * @flags: flags of the new region
 *
 * Add new memblock region [@base, @base + @size) into @type.  The new region
 * is allowed to overlap with existing ones - overlaps don't affect already
 * existing regions.  @type is guaranteed to be minimal (all neighbouring
 * compatible regions are merged) after the addition.
 *
 * Return:
 * 0 on success, -errno on failure.
 */
int __init_memblock memblock_add_range(struct memblock_type *type,
				phys_addr_t base, phys_addr_t size,
				int nid, enum memblock_flags flags)
{
	bool insert = false;
	phys_addr_t obase = base;
	phys_addr_t end = base + memblock_cap_size(base, &size);
	int idx, nr_new;
	struct memblock_region *rgn;
	unsigned long new_size = 0;

	if (!size)
		return 0;

	/* special case for empty array */
	if (type->regions[0].size == 0) {
		WARN_ON(type->cnt != 1 || type->total_size);
		type->regions[0].base = base;
		type->regions[0].size = size;
		type->regions[0].flags = flags;
		memblock_set_region_node(&type->regions[0], nid);
		type->total_size = size;
		new_size = (unsigned long)size;
		goto done;
	}
repeat:
	/*
	 * The following is executed twice.  Once with %false @insert and
	 * then with %true.  The first counts the number of regions needed
	 * to accommodate the new area.  The second actually inserts them.
	 */
	base = obase;
	nr_new = 0;

	for_each_memblock_type(idx, type, rgn) {
		phys_addr_t rbase = rgn->base;
		phys_addr_t rend = rbase + rgn->size;

		if (rbase >= end)
			break;
		if (rend <= base)
			continue;
		/*
		 * @rgn overlaps.  If it separates the lower part of new
		 * area, insert that portion.
		 */
		if (rbase > base) {
#ifdef CONFIG_HAVE_MEMBLOCK_NODE_MAP
			WARN_ON(nid != memblock_get_region_node(rgn));
#endif
			WARN_ON(flags != rgn->flags);
			nr_new++;
			if (insert) {
				memblock_insert_region(type, idx++, base,
						       rbase - base, nid,
						       flags);
				new_size += (unsigned long)(rbase - base);
			}
		}
		/* area below @rend is dealt with, forget about it */
		base = min(rend, end);
	}

	/* insert the remaining portion */
	if (base < end) {
		nr_new++;
		if (insert) {
			memblock_insert_region(type, idx, base, end - base,
					       nid, flags);
			new_size += (unsigned long)(end - base);
		}
	}

	if (!nr_new)
		return 0;

	/*
	 * If this was the first round, resize array and repeat for actual
	 * insertions; otherwise, merge and return.
	 */
	if (!insert) {
		while (type->cnt + nr_new > type->max)
			if (memblock_double_array(type, obase, size) < 0)
				return -ENOMEM;
		insert = true;
		goto repeat;
	} else {
		memblock_merge_regions(type);
	}
done:
	if (memsize_reserved_name && type == &memblock.reserved)
		record_memsize_reserved(memsize_reserved_name, obase, size,
					false, false);
	else if (memsize_kernel_type != MEMSIZE_KERNEL_STOP &&
			type == &memblock.reserved)
		record_memsize_size_only(memsize_kernel_type, (long)new_size);
	return 0;
}

/**
 * memblock_add_node - add new memblock region within a NUMA node
 * @base: base address of the new region
 * @size: size of the new region
 * @nid: nid of the new region
 *
 * Add new memblock region [@base, @base + @size) to the "memory"
 * type. See memblock_add_range() description for mode details
 *
 * Return:
 * 0 on success, -errno on failure.
 */
int __init_memblock memblock_add_node(phys_addr_t base, phys_addr_t size,
				       int nid)
{
	return memblock_add_range(&memblock.memory, base, size, nid, 0);
}

/**
 * memblock_add - add new memblock region
 * @base: base address of the new region
 * @size: size of the new region
 *
 * Add new memblock region [@base, @base + @size) to the "memory"
 * type. See memblock_add_range() description for mode details
 *
 * Return:
 * 0 on success, -errno on failure.
 */
int __init_memblock memblock_add(phys_addr_t base, phys_addr_t size)
{
	phys_addr_t end = base + size - 1;

	memblock_dbg("memblock_add: [%pa-%pa] %pF\n",
		     &base, &end, (void *)_RET_IP_);

	return memblock_add_range(&memblock.memory, base, size, MAX_NUMNODES, 0);
}

/**
 * memblock_isolate_range - isolate given range into disjoint memblocks
 * @type: memblock type to isolate range for
 * @base: base of range to isolate
 * @size: size of range to isolate
 * @start_rgn: out parameter for the start of isolated region
 * @end_rgn: out parameter for the end of isolated region
 *
 * Walk @type and ensure that regions don't cross the boundaries defined by
 * [@base, @base + @size).  Crossing regions are split at the boundaries,
 * which may create at most two more regions.  The index of the first
 * region inside the range is returned in *@start_rgn and end in *@end_rgn.
 *
 * Return:
 * 0 on success, -errno on failure.
 */
static int __init_memblock memblock_isolate_range(struct memblock_type *type,
					phys_addr_t base, phys_addr_t size,
					int *start_rgn, int *end_rgn)
{
	phys_addr_t end = base + memblock_cap_size(base, &size);
	int idx;
	struct memblock_region *rgn;

	*start_rgn = *end_rgn = 0;

	if (!size)
		return 0;

	/* we'll create at most two more regions */
	while (type->cnt + 2 > type->max)
		if (memblock_double_array(type, base, size) < 0)
			return -ENOMEM;

	for_each_memblock_type(idx, type, rgn) {
		phys_addr_t rbase = rgn->base;
		phys_addr_t rend = rbase + rgn->size;

		if (rbase >= end)
			break;
		if (rend <= base)
			continue;

		if (rbase < base) {
			/*
			 * @rgn intersects from below.  Split and continue
			 * to process the next region - the new top half.
			 */
			rgn->base = base;
			rgn->size -= base - rbase;
			type->total_size -= base - rbase;
			memblock_insert_region(type, idx, rbase, base - rbase,
					       memblock_get_region_node(rgn),
					       rgn->flags);
		} else if (rend > end) {
			/*
			 * @rgn intersects from above.  Split and redo the
			 * current region - the new bottom half.
			 */
			rgn->base = end;
			rgn->size -= end - rbase;
			type->total_size -= end - rbase;
			memblock_insert_region(type, idx--, rbase, end - rbase,
					       memblock_get_region_node(rgn),
					       rgn->flags);
		} else {
			/* @rgn is fully contained, record it */
			if (!*end_rgn)
				*start_rgn = idx;
			*end_rgn = idx + 1;
		}
	}

	return 0;
}

static int __init_memblock memblock_remove_range(struct memblock_type *type,
					  phys_addr_t base, phys_addr_t size)
{
	int start_rgn, end_rgn;
	int i, ret;

	ret = memblock_isolate_range(type, base, size, &start_rgn, &end_rgn);
	if (ret)
		return ret;

	if (memsize_reserved_name && type == &memblock.memory)
		record_memsize_reserved(memsize_reserved_name, base, size,
					true, false);
	else if (memsize_reserved_name && type == &memblock.reserved)
		free_memsize_reserved(base, size);
	else if (memsize_kernel_type != MEMSIZE_KERNEL_STOP
			&& type == &memblock.reserved)
		record_memsize_size_only(memsize_kernel_type, size * -1);

	for (i = end_rgn - 1; i >= start_rgn; i--)
		memblock_remove_region(type, i);
	return 0;
}

int __init_memblock memblock_remove(phys_addr_t base, phys_addr_t size)
{
	phys_addr_t end = base + size - 1;

	memblock_dbg("memblock_remove: [%pa-%pa] %pS\n",
		     &base, &end, (void *)_RET_IP_);

	return memblock_remove_range(&memblock.memory, base, size);
}


int __init_memblock memblock_free(phys_addr_t base, phys_addr_t size)
{
	phys_addr_t end = base + size - 1;

	memblock_dbg("   memblock_free: [%pa-%pa] %pF\n",
		     &base, &end, (void *)_RET_IP_);

	kmemleak_free_part_phys(base, size);
	return memblock_remove_range(&memblock.reserved, base, size);
}
EXPORT_SYMBOL_GPL(memblock_free);

int __init_memblock memblock_reserve(phys_addr_t base, phys_addr_t size)
{
	phys_addr_t end = base + size - 1;

	memblock_dbg("memblock_reserve: [%pa-%pa] %pF\n",
		     &base, &end, (void *)_RET_IP_);

	return memblock_add_range(&memblock.reserved, base, size, MAX_NUMNODES, 0);
}

/**
 * memblock_setclr_flag - set or clear flag for a memory region
 * @base: base address of the region
 * @size: size of the region
 * @set: set or clear the flag
 * @flag: the flag to udpate
 *
 * This function isolates region [@base, @base + @size), and sets/clears flag
 *
 * Return: 0 on success, -errno on failure.
 */
static int __init_memblock memblock_setclr_flag(phys_addr_t base,
				phys_addr_t size, int set, int flag)
{
	struct memblock_type *type = &memblock.memory;
	int i, ret, start_rgn, end_rgn;

	ret = memblock_isolate_range(type, base, size, &start_rgn, &end_rgn);
	if (ret)
		return ret;

	for (i = start_rgn; i < end_rgn; i++)
		if (set)
			memblock_set_region_flags(&type->regions[i], flag);
		else
			memblock_clear_region_flags(&type->regions[i], flag);

	memblock_merge_regions(type);
	return 0;
}

/**
 * memblock_mark_hotplug - Mark hotpluggable memory with flag MEMBLOCK_HOTPLUG.
 * @base: the base phys addr of the region
 * @size: the size of the region
 *
 * Return: 0 on success, -errno on failure.
 */
int __init_memblock memblock_mark_hotplug(phys_addr_t base, phys_addr_t size)
{
	return memblock_setclr_flag(base, size, 1, MEMBLOCK_HOTPLUG);
}

/**
 * memblock_clear_hotplug - Clear flag MEMBLOCK_HOTPLUG for a specified region.
 * @base: the base phys addr of the region
 * @size: the size of the region
 *
 * Return: 0 on success, -errno on failure.
 */
int __init_memblock memblock_clear_hotplug(phys_addr_t base, phys_addr_t size)
{
	return memblock_setclr_flag(base, size, 0, MEMBLOCK_HOTPLUG);
}

/**
 * memblock_mark_mirror - Mark mirrored memory with flag MEMBLOCK_MIRROR.
 * @base: the base phys addr of the region
 * @size: the size of the region
 *
 * Return: 0 on success, -errno on failure.
 */
int __init_memblock memblock_mark_mirror(phys_addr_t base, phys_addr_t size)
{
	system_has_some_mirror = true;

	return memblock_setclr_flag(base, size, 1, MEMBLOCK_MIRROR);
}

/**
 * memblock_mark_nomap - Mark a memory region with flag MEMBLOCK_NOMAP.
 * @base: the base phys addr of the region
 * @size: the size of the region
 *
 * Return: 0 on success, -errno on failure.
 */
int __init_memblock memblock_mark_nomap(phys_addr_t base, phys_addr_t size)
{
	return memblock_setclr_flag(base, size, 1, MEMBLOCK_NOMAP);
}

/**
 * memblock_clear_nomap - Clear flag MEMBLOCK_NOMAP for a specified region.
 * @base: the base phys addr of the region
 * @size: the size of the region
 *
 * Return: 0 on success, -errno on failure.
 */
int __init_memblock memblock_clear_nomap(phys_addr_t base, phys_addr_t size)
{
	return memblock_setclr_flag(base, size, 0, MEMBLOCK_NOMAP);
}

/**
 * __next_reserved_mem_region - next function for for_each_reserved_region()
 * @idx: pointer to u64 loop variable
 * @out_start: ptr to phys_addr_t for start address of the region, can be %NULL
 * @out_end: ptr to phys_addr_t for end address of the region, can be %NULL
 *
 * Iterate over all reserved memory regions.
 */
void __init_memblock __next_reserved_mem_region(u64 *idx,
					   phys_addr_t *out_start,
					   phys_addr_t *out_end)
{
	struct memblock_type *type = &memblock.reserved;

	if (*idx < type->cnt) {
		struct memblock_region *r = &type->regions[*idx];
		phys_addr_t base = r->base;
		phys_addr_t size = r->size;

		if (out_start)
			*out_start = base;
		if (out_end)
			*out_end = base + size - 1;

		*idx += 1;
		return;
	}

	/* signal end of iteration */
	*idx = ULLONG_MAX;
}

/**
 * __next__mem_range - next function for for_each_free_mem_range() etc.
 * @idx: pointer to u64 loop variable
 * @nid: node selector, %NUMA_NO_NODE for all nodes
 * @flags: pick from blocks based on memory attributes
 * @type_a: pointer to memblock_type from where the range is taken
 * @type_b: pointer to memblock_type which excludes memory from being taken
 * @out_start: ptr to phys_addr_t for start address of the range, can be %NULL
 * @out_end: ptr to phys_addr_t for end address of the range, can be %NULL
 * @out_nid: ptr to int for nid of the range, can be %NULL
 *
 * Find the first area from *@idx which matches @nid, fill the out
 * parameters, and update *@idx for the next iteration.  The lower 32bit of
 * *@idx contains index into type_a and the upper 32bit indexes the
 * areas before each region in type_b.	For example, if type_b regions
 * look like the following,
 *
 *	0:[0-16), 1:[32-48), 2:[128-130)
 *
 * The upper 32bit indexes the following regions.
 *
 *	0:[0-0), 1:[16-32), 2:[48-128), 3:[130-MAX)
 *
 * As both region arrays are sorted, the function advances the two indices
 * in lockstep and returns each intersection.
 */
void __init_memblock __next_mem_range(u64 *idx, int nid,
				      enum memblock_flags flags,
				      struct memblock_type *type_a,
				      struct memblock_type *type_b,
				      phys_addr_t *out_start,
				      phys_addr_t *out_end, int *out_nid)
{
	int idx_a = *idx & 0xffffffff;
	int idx_b = *idx >> 32;

	if (WARN_ONCE(nid == MAX_NUMNODES,
	"Usage of MAX_NUMNODES is deprecated. Use NUMA_NO_NODE instead\n"))
		nid = NUMA_NO_NODE;

	for (; idx_a < type_a->cnt; idx_a++) {
		struct memblock_region *m = &type_a->regions[idx_a];

		phys_addr_t m_start = m->base;
		phys_addr_t m_end = m->base + m->size;
		int	    m_nid = memblock_get_region_node(m);

		/* only memory regions are associated with nodes, check it */
		if (nid != NUMA_NO_NODE && nid != m_nid)
			continue;

		/* skip hotpluggable memory regions if needed */
		if (movable_node_is_enabled() && memblock_is_hotpluggable(m))
			continue;

		/* if we want mirror memory skip non-mirror memory regions */
		if ((flags & MEMBLOCK_MIRROR) && !memblock_is_mirror(m))
			continue;

		/* skip nomap memory unless we were asked for it explicitly */
		if (!(flags & MEMBLOCK_NOMAP) && memblock_is_nomap(m))
			continue;

		if (!type_b) {
			if (out_start)
				*out_start = m_start;
			if (out_end)
				*out_end = m_end;
			if (out_nid)
				*out_nid = m_nid;
			idx_a++;
			*idx = (u32)idx_a | (u64)idx_b << 32;
			return;
		}

		/* scan areas before each reservation */
		for (; idx_b < type_b->cnt + 1; idx_b++) {
			struct memblock_region *r;
			phys_addr_t r_start;
			phys_addr_t r_end;

			r = &type_b->regions[idx_b];
			r_start = idx_b ? r[-1].base + r[-1].size : 0;
			r_end = idx_b < type_b->cnt ?
				r->base : PHYS_ADDR_MAX;

			/*
			 * if idx_b advanced past idx_a,
			 * break out to advance idx_a
			 */
			if (r_start >= m_end)
				break;
			/* if the two regions intersect, we're done */
			if (m_start < r_end) {
				if (out_start)
					*out_start =
						max(m_start, r_start);
				if (out_end)
					*out_end = min(m_end, r_end);
				if (out_nid)
					*out_nid = m_nid;
				/*
				 * The region which ends first is
				 * advanced for the next iteration.
				 */
				if (m_end <= r_end)
					idx_a++;
				else
					idx_b++;
				*idx = (u32)idx_a | (u64)idx_b << 32;
				return;
			}
		}
	}

	/* signal end of iteration */
	*idx = ULLONG_MAX;
}

/**
 * __next_mem_range_rev - generic next function for for_each_*_range_rev()
 *
 * @idx: pointer to u64 loop variable
 * @nid: node selector, %NUMA_NO_NODE for all nodes
 * @flags: pick from blocks based on memory attributes
 * @type_a: pointer to memblock_type from where the range is taken
 * @type_b: pointer to memblock_type which excludes memory from being taken
 * @out_start: ptr to phys_addr_t for start address of the range, can be %NULL
 * @out_end: ptr to phys_addr_t for end address of the range, can be %NULL
 * @out_nid: ptr to int for nid of the range, can be %NULL
 *
 * Finds the next range from type_a which is not marked as unsuitable
 * in type_b.
 *
 * Reverse of __next_mem_range().
 */
void __init_memblock __next_mem_range_rev(u64 *idx, int nid,
					  enum memblock_flags flags,
					  struct memblock_type *type_a,
					  struct memblock_type *type_b,
					  phys_addr_t *out_start,
					  phys_addr_t *out_end, int *out_nid)
{
	int idx_a = *idx & 0xffffffff;
	int idx_b = *idx >> 32;

	if (WARN_ONCE(nid == MAX_NUMNODES, "Usage of MAX_NUMNODES is deprecated. Use NUMA_NO_NODE instead\n"))
		nid = NUMA_NO_NODE;

	if (*idx == (u64)ULLONG_MAX) {
		idx_a = type_a->cnt - 1;
		if (type_b != NULL)
			idx_b = type_b->cnt;
		else
			idx_b = 0;
	}

	for (; idx_a >= 0; idx_a--) {
		struct memblock_region *m = &type_a->regions[idx_a];

		phys_addr_t m_start = m->base;
		phys_addr_t m_end = m->base + m->size;
		int m_nid = memblock_get_region_node(m);

		/* only memory regions are associated with nodes, check it */
		if (nid != NUMA_NO_NODE && nid != m_nid)
			continue;

		/* skip hotpluggable memory regions if needed */
		if (movable_node_is_enabled() && memblock_is_hotpluggable(m))
			continue;

		/* if we want mirror memory skip non-mirror memory regions */
		if ((flags & MEMBLOCK_MIRROR) && !memblock_is_mirror(m))
			continue;

		/* skip nomap memory unless we were asked for it explicitly */
		if (!(flags & MEMBLOCK_NOMAP) && memblock_is_nomap(m))
			continue;

		if (!type_b) {
			if (out_start)
				*out_start = m_start;
			if (out_end)
				*out_end = m_end;
			if (out_nid)
				*out_nid = m_nid;
			idx_a--;
			*idx = (u32)idx_a | (u64)idx_b << 32;
			return;
		}

		/* scan areas before each reservation */
		for (; idx_b >= 0; idx_b--) {
			struct memblock_region *r;
			phys_addr_t r_start;
			phys_addr_t r_end;

			r = &type_b->regions[idx_b];
			r_start = idx_b ? r[-1].base + r[-1].size : 0;
			r_end = idx_b < type_b->cnt ?
				r->base : PHYS_ADDR_MAX;
			/*
			 * if idx_b advanced past idx_a,
			 * break out to advance idx_a
			 */

			if (r_end <= m_start)
				break;
			/* if the two regions intersect, we're done */
			if (m_end > r_start) {
				if (out_start)
					*out_start = max(m_start, r_start);
				if (out_end)
					*out_end = min(m_end, r_end);
				if (out_nid)
					*out_nid = m_nid;
				if (m_start >= r_start)
					idx_a--;
				else
					idx_b--;
				*idx = (u32)idx_a | (u64)idx_b << 32;
				return;
			}
		}
	}
	/* signal end of iteration */
	*idx = ULLONG_MAX;
}

#ifdef CONFIG_HAVE_MEMBLOCK_NODE_MAP
/*
 * Common iterator interface used to define for_each_mem_range().
 */
void __init_memblock __next_mem_pfn_range(int *idx, int nid,
				unsigned long *out_start_pfn,
				unsigned long *out_end_pfn, int *out_nid)
{
	struct memblock_type *type = &memblock.memory;
	struct memblock_region *r;

	while (++*idx < type->cnt) {
		r = &type->regions[*idx];

		if (PFN_UP(r->base) >= PFN_DOWN(r->base + r->size))
			continue;
		if (nid == MAX_NUMNODES || nid == r->nid)
			break;
	}
	if (*idx >= type->cnt) {
		*idx = -1;
		return;
	}

	if (out_start_pfn)
		*out_start_pfn = PFN_UP(r->base);
	if (out_end_pfn)
		*out_end_pfn = PFN_DOWN(r->base + r->size);
	if (out_nid)
		*out_nid = r->nid;
}

/**
 * memblock_set_node - set node ID on memblock regions
 * @base: base of area to set node ID for
 * @size: size of area to set node ID for
 * @type: memblock type to set node ID for
 * @nid: node ID to set
 *
 * Set the nid of memblock @type regions in [@base, @base + @size) to @nid.
 * Regions which cross the area boundaries are split as necessary.
 *
 * Return:
 * 0 on success, -errno on failure.
 */
int __init_memblock memblock_set_node(phys_addr_t base, phys_addr_t size,
				      struct memblock_type *type, int nid)
{
	int start_rgn, end_rgn;
	int i, ret;

	ret = memblock_isolate_range(type, base, size, &start_rgn, &end_rgn);
	if (ret)
		return ret;

	for (i = start_rgn; i < end_rgn; i++)
		memblock_set_region_node(&type->regions[i], nid);

	memblock_merge_regions(type);
	return 0;
}
#endif /* CONFIG_HAVE_MEMBLOCK_NODE_MAP */

static phys_addr_t __init memblock_alloc_range_nid(phys_addr_t size,
					phys_addr_t align, phys_addr_t start,
					phys_addr_t end, int nid,
					enum memblock_flags flags)
{
	phys_addr_t found;

	if (!align)
		align = SMP_CACHE_BYTES;

	found = memblock_find_in_range_node(size, align, start, end, nid,
					    flags);
	if (found && !memblock_reserve(found, size)) {
		/*
		 * The min_count is set to 0 so that memblock allocations are
		 * never reported as leaks.
		 */
		kmemleak_alloc_phys(found, size, 0, 0);
		return found;
	}
	return 0;
}

phys_addr_t __init memblock_alloc_range(phys_addr_t size, phys_addr_t align,
					phys_addr_t start, phys_addr_t end,
					enum memblock_flags flags)
{
	return memblock_alloc_range_nid(size, align, start, end, NUMA_NO_NODE,
					flags);
}

phys_addr_t __init memblock_alloc_base_nid(phys_addr_t size,
					phys_addr_t align, phys_addr_t max_addr,
					int nid, enum memblock_flags flags)
{
	return memblock_alloc_range_nid(size, align, 0, max_addr, nid, flags);
}

phys_addr_t __init memblock_alloc_nid(phys_addr_t size, phys_addr_t align, int nid)
{
	enum memblock_flags flags = choose_memblock_flags();
	phys_addr_t ret;

again:
	ret = memblock_alloc_base_nid(size, align, MEMBLOCK_ALLOC_ACCESSIBLE,
				      nid, flags);

	if (!ret && (flags & MEMBLOCK_MIRROR)) {
		flags &= ~MEMBLOCK_MIRROR;
		goto again;
	}
	return ret;
}

phys_addr_t __init __memblock_alloc_base(phys_addr_t size, phys_addr_t align, phys_addr_t max_addr)
{
	return memblock_alloc_base_nid(size, align, max_addr, NUMA_NO_NODE,
				       MEMBLOCK_NONE);
}

phys_addr_t __init memblock_alloc_base(phys_addr_t size, phys_addr_t align, phys_addr_t max_addr)
{
	phys_addr_t alloc;

	alloc = __memblock_alloc_base(size, align, max_addr);

	if (alloc == 0)
		panic("ERROR: Failed to allocate %pa bytes below %pa.\n",
		      &size, &max_addr);

	return alloc;
}

phys_addr_t __init memblock_alloc(phys_addr_t size, phys_addr_t align)
{
	return memblock_alloc_base(size, align, MEMBLOCK_ALLOC_ACCESSIBLE);
}

phys_addr_t __init memblock_alloc_try_nid(phys_addr_t size, phys_addr_t align, int nid)
{
	phys_addr_t res = memblock_alloc_nid(size, align, nid);

	if (res)
		return res;
	return memblock_alloc_base(size, align, MEMBLOCK_ALLOC_ACCESSIBLE);
}

#if defined(CONFIG_NO_BOOTMEM)
/**
 * memblock_virt_alloc_internal - allocate boot memory block
 * @size: size of memory block to be allocated in bytes
 * @align: alignment of the region and block's size
 * @min_addr: the lower bound of the memory region to allocate (phys address)
 * @max_addr: the upper bound of the memory region to allocate (phys address)
 * @nid: nid of the free area to find, %NUMA_NO_NODE for any node
 *
 * The @min_addr limit is dropped if it can not be satisfied and the allocation
 * will fall back to memory below @min_addr. Also, allocation may fall back
 * to any node in the system if the specified node can not
 * hold the requested memory.
 *
 * The allocation is performed from memory region limited by
 * memblock.current_limit if @max_addr == %BOOTMEM_ALLOC_ACCESSIBLE.
 *
 * The memory block is aligned on %SMP_CACHE_BYTES if @align == 0.
 *
 * The phys address of allocated boot memory block is converted to virtual and
 * allocated memory is reset to 0.
 *
 * In addition, function sets the min_count to 0 using kmemleak_alloc for
 * allocated boot memory block, so that it is never reported as leaks.
 *
 * Return:
 * Virtual address of allocated memory block on success, NULL on failure.
 */
static void * __init memblock_virt_alloc_internal(
				phys_addr_t size, phys_addr_t align,
				phys_addr_t min_addr, phys_addr_t max_addr,
				int nid)
{
	phys_addr_t alloc;
	void *ptr;
	enum memblock_flags flags = choose_memblock_flags();

	if (WARN_ONCE(nid == MAX_NUMNODES, "Usage of MAX_NUMNODES is deprecated. Use NUMA_NO_NODE instead\n"))
		nid = NUMA_NO_NODE;

	/*
	 * Detect any accidental use of these APIs after slab is ready, as at
	 * this moment memblock may be deinitialized already and its
	 * internal data may be destroyed (after execution of free_all_bootmem)
	 */
	if (WARN_ON_ONCE(slab_is_available()))
		return kzalloc_node(size, GFP_NOWAIT, nid);

	if (!align)
		align = SMP_CACHE_BYTES;

	if (max_addr > memblock.current_limit)
		max_addr = memblock.current_limit;
again:
	alloc = memblock_find_in_range_node(size, align, min_addr, max_addr,
					    nid, flags);
	if (alloc && !memblock_reserve(alloc, size))
		goto done;

	if (nid != NUMA_NO_NODE) {
		alloc = memblock_find_in_range_node(size, align, min_addr,
						    max_addr, NUMA_NO_NODE,
						    flags);
		if (alloc && !memblock_reserve(alloc, size))
			goto done;
	}

	if (min_addr) {
		min_addr = 0;
		goto again;
	}

	if (flags & MEMBLOCK_MIRROR) {
		flags &= ~MEMBLOCK_MIRROR;
		pr_warn("Could not allocate %pap bytes of mirrored memory\n",
			&size);
		goto again;
	}

	return NULL;
done:
	ptr = phys_to_virt(alloc);

	/* Skip kmemleak for kasan_init() due to high volume. */
	if (max_addr != MEMBLOCK_ALLOC_KASAN)
		/*
		 * The min_count is set to 0 so that bootmem allocated
		 * blocks are never reported as leaks. This is because many
		 * of these blocks are only referred via the physical
		 * address which is not looked up by kmemleak.
		 */
		kmemleak_alloc(ptr, size, 0, 0);

	return ptr;
}

/**
 * memblock_virt_alloc_try_nid_raw - allocate boot memory block without zeroing
 * memory and without panicking
 * @size: size of memory block to be allocated in bytes
 * @align: alignment of the region and block's size
 * @min_addr: the lower bound of the memory region from where the allocation
 *	  is preferred (phys address)
 * @max_addr: the upper bound of the memory region from where the allocation
 *	      is preferred (phys address), or %BOOTMEM_ALLOC_ACCESSIBLE to
 *	      allocate only from memory limited by memblock.current_limit value
 * @nid: nid of the free area to find, %NUMA_NO_NODE for any node
 *
 * Public function, provides additional debug information (including caller
 * info), if enabled. Does not zero allocated memory, does not panic if request
 * cannot be satisfied.
 *
 * Return:
 * Virtual address of allocated memory block on success, NULL on failure.
 */
void * __init memblock_virt_alloc_try_nid_raw(
			phys_addr_t size, phys_addr_t align,
			phys_addr_t min_addr, phys_addr_t max_addr,
			int nid)
{
	void *ptr;

	memblock_dbg("%s: %llu bytes align=0x%llx nid=%d from=%pa max_addr=%pa %pF\n",
		     __func__, (u64)size, (u64)align, nid, &min_addr,
		     &max_addr, (void *)_RET_IP_);

	ptr = memblock_virt_alloc_internal(size, align,
					   min_addr, max_addr, nid);
#ifdef CONFIG_DEBUG_VM
	if (ptr && size > 0)
		memset(ptr, PAGE_POISON_PATTERN, size);
#endif
	return ptr;
}

/**
 * memblock_virt_alloc_try_nid_nopanic - allocate boot memory block
 * @size: size of memory block to be allocated in bytes
 * @align: alignment of the region and block's size
 * @min_addr: the lower bound of the memory region from where the allocation
 *	  is preferred (phys address)
 * @max_addr: the upper bound of the memory region from where the allocation
 *	      is preferred (phys address), or %BOOTMEM_ALLOC_ACCESSIBLE to
 *	      allocate only from memory limited by memblock.current_limit value
 * @nid: nid of the free area to find, %NUMA_NO_NODE for any node
 *
 * Public function, provides additional debug information (including caller
 * info), if enabled. This function zeroes the allocated memory.
 *
 * Return:
 * Virtual address of allocated memory block on success, NULL on failure.
 */
void * __init memblock_virt_alloc_try_nid_nopanic(
				phys_addr_t size, phys_addr_t align,
				phys_addr_t min_addr, phys_addr_t max_addr,
				int nid)
{
	void *ptr;

	memblock_dbg("%s: %llu bytes align=0x%llx nid=%d from=%pa max_addr=%pa %pF\n",
		     __func__, (u64)size, (u64)align, nid, &min_addr,
		     &max_addr, (void *)_RET_IP_);

	ptr = memblock_virt_alloc_internal(size, align,
					   min_addr, max_addr, nid);
	if (ptr)
		memset(ptr, 0, size);
	return ptr;
}

/**
 * memblock_virt_alloc_try_nid - allocate boot memory block with panicking
 * @size: size of memory block to be allocated in bytes
 * @align: alignment of the region and block's size
 * @min_addr: the lower bound of the memory region from where the allocation
 *	  is preferred (phys address)
 * @max_addr: the upper bound of the memory region from where the allocation
 *	      is preferred (phys address), or %BOOTMEM_ALLOC_ACCESSIBLE to
 *	      allocate only from memory limited by memblock.current_limit value
 * @nid: nid of the free area to find, %NUMA_NO_NODE for any node
 *
 * Public panicking version of memblock_virt_alloc_try_nid_nopanic()
 * which provides debug information (including caller info), if enabled,
 * and panics if the request can not be satisfied.
 *
 * Return:
 * Virtual address of allocated memory block on success, NULL on failure.
 */
void * __init memblock_virt_alloc_try_nid(
			phys_addr_t size, phys_addr_t align,
			phys_addr_t min_addr, phys_addr_t max_addr,
			int nid)
{
	void *ptr;

	memblock_dbg("%s: %llu bytes align=0x%llx nid=%d from=%pa max_addr=%pa %pF\n",
		     __func__, (u64)size, (u64)align, nid, &min_addr,
		     &max_addr, (void *)_RET_IP_);
	ptr = memblock_virt_alloc_internal(size, align,
					   min_addr, max_addr, nid);
	if (ptr) {
		memset(ptr, 0, size);
		return ptr;
	}

	panic("%s: Failed to allocate %llu bytes align=0x%llx nid=%d from=%pa max_addr=%pa\n",
	      __func__, (u64)size, (u64)align, nid, &min_addr, &max_addr);
	return NULL;
}
#endif

/**
 * __memblock_free_early - free boot memory block
 * @base: phys starting address of the  boot memory block
 * @size: size of the boot memory block in bytes
 *
 * Free boot memory block previously allocated by memblock_virt_alloc_xx() API.
 * The freeing memory will not be released to the buddy allocator.
 */
void __init __memblock_free_early(phys_addr_t base, phys_addr_t size)
{
	memblock_free(base, size);
}

/**
 * __memblock_free_late - free bootmem block pages directly to buddy allocator
 * @base: phys starting address of the  boot memory block
 * @size: size of the boot memory block in bytes
 *
 * This is only useful when the bootmem allocator has already been torn
 * down, but we are still initializing the system.  Pages are released directly
 * to the buddy allocator, no bootmem metadata is updated because it is gone.
 */
void __memblock_free_late(phys_addr_t base, phys_addr_t size)
{
	phys_addr_t cursor, end;

	free_memsize_reserved(base, size);
	end = base + size - 1;
	memblock_dbg("%s: [%pa-%pa] %pF\n",
		     __func__, &base, &end, (void *)_RET_IP_);
	kmemleak_free_part_phys(base, size);
	cursor = PFN_UP(base);
	end = PFN_DOWN(base + size);

	for (; cursor < end; cursor++) {
		__free_pages_bootmem(pfn_to_page(cursor), cursor, 0);
		totalram_pages++;
	}
}

/*
 * Remaining API functions
 */

phys_addr_t __init_memblock memblock_phys_mem_size(void)
{
	return memblock.memory.total_size;
}

phys_addr_t __init_memblock memblock_reserved_size(void)
{
	return memblock.reserved.total_size;
}

phys_addr_t __init memblock_mem_size(unsigned long limit_pfn)
{
	unsigned long pages = 0;
	struct memblock_region *r;
	unsigned long start_pfn, end_pfn;

	for_each_memblock(memory, r) {
		start_pfn = memblock_region_memory_base_pfn(r);
		end_pfn = memblock_region_memory_end_pfn(r);
		start_pfn = min_t(unsigned long, start_pfn, limit_pfn);
		end_pfn = min_t(unsigned long, end_pfn, limit_pfn);
		pages += end_pfn - start_pfn;
	}

	return PFN_PHYS(pages);
}

/* lowest address */
phys_addr_t __init_memblock memblock_start_of_DRAM(void)
{
	return memblock.memory.regions[0].base;
}

phys_addr_t __init_memblock memblock_end_of_DRAM(void)
{
	int idx = memblock.memory.cnt - 1;

	return (memblock.memory.regions[idx].base + memblock.memory.regions[idx].size);
}

static phys_addr_t __init_memblock __find_max_addr(phys_addr_t limit)
{
	phys_addr_t max_addr = PHYS_ADDR_MAX;
	struct memblock_region *r;

	/*
	 * translate the memory @limit size into the max address within one of
	 * the memory memblock regions, if the @limit exceeds the total size
	 * of those regions, max_addr will keep original value PHYS_ADDR_MAX
	 */
	for_each_memblock(memory, r) {
		if (limit <= r->size) {
			max_addr = r->base + limit;
			break;
		}
		limit -= r->size;
	}

	return max_addr;
}

void __init memblock_enforce_memory_limit(phys_addr_t limit)
{
	phys_addr_t max_addr = PHYS_ADDR_MAX;

	if (!limit)
		return;

	max_addr = __find_max_addr(limit);

	/* @limit exceeds the total size of the memory, do nothing */
	if (max_addr == PHYS_ADDR_MAX)
		return;

	/* truncate both memory and reserved regions */
	memblock_remove_range(&memblock.memory, max_addr,
			      PHYS_ADDR_MAX);
	memblock_remove_range(&memblock.reserved, max_addr,
			      PHYS_ADDR_MAX);
}

void __init memblock_cap_memory_range(phys_addr_t base, phys_addr_t size)
{
	int start_rgn, end_rgn;
	int i, ret;

	if (!size)
		return;

	ret = memblock_isolate_range(&memblock.memory, base, size,
						&start_rgn, &end_rgn);
	if (ret)
		return;

	/* remove all the MAP regions */
	for (i = memblock.memory.cnt - 1; i >= end_rgn; i--)
		if (!memblock_is_nomap(&memblock.memory.regions[i]))
			memblock_remove_region(&memblock.memory, i);

	for (i = start_rgn - 1; i >= 0; i--)
		if (!memblock_is_nomap(&memblock.memory.regions[i]))
			memblock_remove_region(&memblock.memory, i);

	/* truncate the reserved regions */
	memblock_remove_range(&memblock.reserved, 0, base);
	memblock_remove_range(&memblock.reserved,
			base + size, PHYS_ADDR_MAX);
}

void __init memblock_mem_limit_remove_map(phys_addr_t limit)
{
	phys_addr_t max_addr;

	if (!limit)
		return;

	max_addr = __find_max_addr(limit);

	/* @limit exceeds the total size of the memory, do nothing */
	if (max_addr == PHYS_ADDR_MAX)
		return;

	memblock_cap_memory_range(0, max_addr);
}

static int __init_memblock memblock_search(struct memblock_type *type, phys_addr_t addr)
{
	unsigned int left = 0, right = type->cnt;

	do {
		unsigned int mid = (right + left) / 2;

		if (addr < type->regions[mid].base)
			right = mid;
		else if (addr >= (type->regions[mid].base +
				  type->regions[mid].size))
			left = mid + 1;
		else
			return mid;
	} while (left < right);
	return -1;
}

bool __init memblock_is_reserved(phys_addr_t addr)
{
	return memblock_search(&memblock.reserved, addr) != -1;
}

bool __init_memblock memblock_is_memory(phys_addr_t addr)
{
	return memblock_search(&memblock.memory, addr) != -1;
}

bool __init_memblock memblock_is_map_memory(phys_addr_t addr)
{
	int i = memblock_search(&memblock.memory, addr);

	if (i == -1)
		return false;
	return !memblock_is_nomap(&memblock.memory.regions[i]);
}

#ifdef CONFIG_HAVE_MEMBLOCK_NODE_MAP
int __init_memblock memblock_search_pfn_nid(unsigned long pfn,
			 unsigned long *start_pfn, unsigned long *end_pfn)
{
	struct memblock_type *type = &memblock.memory;
	int mid = memblock_search(type, PFN_PHYS(pfn));

	if (mid == -1)
		return -1;

	*start_pfn = PFN_DOWN(type->regions[mid].base);
	*end_pfn = PFN_DOWN(type->regions[mid].base + type->regions[mid].size);

	return type->regions[mid].nid;
}
#endif

/**
 * memblock_is_region_memory - check if a region is a subset of memory
 * @base: base of region to check
 * @size: size of region to check
 *
 * Check if the region [@base, @base + @size) is a subset of a memory block.
 *
 * Return:
 * 0 if false, non-zero if true
 */
bool __init_memblock memblock_is_region_memory(phys_addr_t base, phys_addr_t size)
{
	int idx = memblock_search(&memblock.memory, base);
	phys_addr_t end = base + memblock_cap_size(base, &size);

	if (idx == -1)
		return false;
	return (memblock.memory.regions[idx].base +
		 memblock.memory.regions[idx].size) >= end;
}

bool __init_memblock memblock_overlaps_memory(phys_addr_t base,
					      phys_addr_t size)
{
	memblock_cap_size(base, &size);

	return memblock_overlaps_region(&memblock.memory, base, size);
}
EXPORT_SYMBOL_GPL(memblock_overlaps_memory);

/**
 * memblock_is_region_reserved - check if a region intersects reserved memory
 * @base: base of region to check
 * @size: size of region to check
 *
 * Check if the region [@base, @base + @size) intersects a reserved
 * memory block.
 *
 * Return:
 * True if they intersect, false if not.
 */
bool __init_memblock memblock_is_region_reserved(phys_addr_t base, phys_addr_t size)
{
	memblock_cap_size(base, &size);
	return memblock_overlaps_region(&memblock.reserved, base, size);
}

void __init_memblock memblock_trim_memory(phys_addr_t align)
{
	phys_addr_t start, end, orig_start, orig_end;
	struct memblock_region *r;

	for_each_memblock(memory, r) {
		orig_start = r->base;
		orig_end = r->base + r->size;
		start = round_up(orig_start, align);
		end = round_down(orig_end, align);

		if (start == orig_start && end == orig_end)
			continue;

		if (start < end) {
			r->base = start;
			r->size = end - start;
		} else {
			memblock_remove_region(&memblock.memory,
					       r - memblock.memory.regions);
			r--;
		}
	}
}

void __init_memblock memblock_set_current_limit(phys_addr_t limit)
{
	memblock.current_limit = limit;
}

phys_addr_t __init_memblock memblock_get_current_limit(void)
{
	return memblock.current_limit;
}

static void __init_memblock memblock_dump(struct memblock_type *type)
{
	phys_addr_t base, end, size;
	enum memblock_flags flags;
	int idx;
	struct memblock_region *rgn;

	pr_info(" %s.cnt  = 0x%lx\n", type->name, type->cnt);

	for_each_memblock_type(idx, type, rgn) {
		char nid_buf[32] = "";

		base = rgn->base;
		size = rgn->size;
		end = base + size - 1;
		flags = rgn->flags;
#ifdef CONFIG_HAVE_MEMBLOCK_NODE_MAP
		if (memblock_get_region_node(rgn) != MAX_NUMNODES)
			snprintf(nid_buf, sizeof(nid_buf), " on node %d",
				 memblock_get_region_node(rgn));
#endif
		pr_info(" %s[%#x]\t[%pa-%pa], %pa bytes%s flags: %#x\n",
			type->name, idx, &base, &end, &size, nid_buf, flags);
	}
}

void __init_memblock __memblock_dump_all(void)
{
	pr_info("MEMBLOCK configuration:\n");
	pr_info(" memory size = %pa reserved size = %pa\n",
		&memblock.memory.total_size,
		&memblock.reserved.total_size);

	memblock_dump(&memblock.memory);
	memblock_dump(&memblock.reserved);
#ifdef CONFIG_HAVE_MEMBLOCK_PHYS_MAP
	memblock_dump(&memblock.physmem);
#endif
}

void __init memblock_allow_resize(void)
{
	memblock_can_resize = 1;
}

static int __init early_memblock(char *p)
{
	if (p && strstr(p, "debug"))
		memblock_debug = 1;
	return 0;
}
early_param("memblock", early_memblock);

static int memblock_debug_show(struct seq_file *m, void *private)
{
	struct memblock_type *type = m->private;
	struct memblock_region *reg;
	int i;
	phys_addr_t end;

	for (i = 0; i < type->cnt; i++) {
		reg = &type->regions[i];
		end = reg->base + reg->size - 1;

		seq_printf(m, "%4d: ", i);
		seq_printf(m, "%pa..%pa\n", &reg->base, &end);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(memblock_debug);

static int memsize_kernel_show(struct seq_file *m, void *private)
{
	int i;
	unsigned long total, initmem, kernel, text, rodata, data, bss, etc;
	struct reserved_mem_reg *rmem_reg;
	unsigned long unsigned_size;

	initmem = __init_end - __init_begin;
	rmem_reg = &kernel_mem_reg[MEMSIZE_KERNEL_KERNEL];
	kernel = rmem_reg->size - initmem;
	text = _etext - _text;
	rodata = __end_rodata - __start_rodata;
	if (__start_rodata < _etext)
		text -= rodata;
	data = _edata - _sdata;
	bss = __bss_stop - __bss_start;
	etc = kernel - text - rodata - data - bss;

	seq_printf(m, " Kernel     : %8lu KB\n"
		      "  .text     : %8lu KB\n"
		      "  .rodata   : %8lu KB\n"
		      "  .data     : %8lu KB\n"
		      "  .BSS      : %8lu KB\n"
		      "  .ETC      : %8lu KB\n",
		   DIV_ROUND_UP(kernel, SZ_1K),
		   DIV_ROUND_UP(text, SZ_1K),
		   DIV_ROUND_UP(rodata, SZ_1K),
		   DIV_ROUND_UP(data, SZ_1K),
		   DIV_ROUND_UP(bss, SZ_1K),
		   DIV_ROUND_UP(etc, SZ_1K));

	total = kernel;
	for (i = MEMSIZE_KERNEL_KERNEL + 1; i < MEMSIZE_KERNEL_STOP; i++) {
		rmem_reg = &kernel_mem_reg[i];
		unsigned_size = (unsigned long)rmem_reg->size;

		seq_printf(m, " %s : %8lu KB\n", rmem_reg->name,
			   DIV_ROUND_UP(unsigned_size, SZ_1K));
		total += unsigned_size;
	}
	seq_printf(m, " Total      : %8lu KB\n", DIV_ROUND_UP(total, SZ_1K));

	return 0;
}

static unsigned long get_memsize_kernel(void)
{
	int i;
	unsigned long total;
	struct reserved_mem_reg *rmem_reg;

	rmem_reg = &kernel_mem_reg[MEMSIZE_KERNEL_KERNEL];
	total = rmem_reg->size - (__init_end - __init_begin);

	for (i = MEMSIZE_KERNEL_KERNEL + 1; i < MEMSIZE_KERNEL_STOP; i++) {
		rmem_reg = &kernel_mem_reg[i];
		total += (unsigned long)rmem_reg->size;
	}

	return total;
}

static int proc_memsize_kernel_open(struct inode *inode, struct file *file)
{
	return single_open(file, memsize_kernel_show, NULL);
}

static const struct file_operations proc_memsize_kernel_fops = {
	.open = proc_memsize_kernel_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int __rmem_reg_cmp(const void *a, const void *b)
{
	const struct reserved_mem_reg *ra = a, *rb = b;

	if (ra->base > rb->base)
		return -1;

	if (ra->base < rb->base)
		return 1;

	return 0;
}

static int memsize_reserved_show(struct seq_file *m, void *private)
{
	int i;
	struct reserved_mem_reg *rmem_reg;
	unsigned long dt_reserved = 0, reusable = 0, kernel, total;
	unsigned long system = totalram_pages << PAGE_SHIFT;

	sort(reserved_mem_reg, reserved_mem_reg_count,
	     sizeof(reserved_mem_reg[0]), __rmem_reg_cmp, NULL);
	seq_printf(m, "v1\n");
	for (i = 0 ; i < reserved_mem_reg_count; i++) {
		rmem_reg = &reserved_mem_reg[i];
		seq_printf(m, "0x%09lx-0x%09lx 0x%08lx ( %7lu KB ) %s %s %s\n",
#if defined(CONFIG_SAMSUNG_PRODUCT_SHIP)
			   0UL,
			   0UL,
#else
			   (unsigned long)rmem_reg->base,
			   (unsigned long)(rmem_reg->base + rmem_reg->size),
#endif
			   (unsigned long)rmem_reg->size,
			   (unsigned long)DIV_ROUND_UP(rmem_reg->size, SZ_1K),
#if defined(CONFIG_SAMSUNG_PRODUCT_SHIP)
			   "xxxxx",
#else
			   rmem_reg->nomap ? "nomap" : "  map",
#endif
			   rmem_reg->reusable ? "reusable" : "unusable",
			   rmem_reg->name);
		if (rmem_reg->reusable)
			reusable += (unsigned long)rmem_reg->size;
		else
			dt_reserved += (unsigned long)rmem_reg->size;
	}

	kernel = get_memsize_kernel();
	seq_printf(m, "0x%09lx-0x%09lx 0x%08lx ( %7lu KB ) %s %s %s\n",
		   0UL, 0UL, kernel, DIV_ROUND_UP(kernel, SZ_1K), "xxxxx",
		   "unusable", "kernel");
	total = kernel + dt_reserved + system;

	seq_printf(m, "\n");
	seq_printf(m, "Reserved    : %7lu KB\n",
		   DIV_ROUND_UP(kernel + dt_reserved, SZ_1K));
	seq_printf(m, " .kernel    : %7lu KB\n",
		   DIV_ROUND_UP(kernel, SZ_1K));
	seq_printf(m, " .DT&EPARAM : %7lu KB\n",
		   DIV_ROUND_UP(dt_reserved, SZ_1K));
	seq_printf(m, "System      : %7lu KB\n",
		   DIV_ROUND_UP(system, SZ_1K));
	seq_printf(m, " .common    : %7lu KB\n",
		   DIV_ROUND_UP(system - reusable, SZ_1K));
	seq_printf(m, " .reusable  : %7lu KB\n",
		   DIV_ROUND_UP(reusable, SZ_1K));
	seq_printf(m, "Total       : %7lu KB ( %5lu.%02lu MB )\n",
		   DIV_ROUND_UP(total, SZ_1K),
		   total >> 20, ((total % SZ_1M) * 100) >> 20);
	return 0;
}

static int proc_memsize_reserved_open(struct inode *inode, struct file *file)
{
	return single_open(file, memsize_reserved_show, NULL);
}

static const struct file_operations proc_memsize_reserved_fops = {
	.open = proc_memsize_reserved_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int __init memblock_init_debugfs(void)
{
	struct dentry *root = debugfs_create_dir("memblock", NULL);
	if (!root)
		return -ENXIO;
	debugfs_create_file("memory", 0444, root,
			    &memblock.memory, &memblock_debug_fops);
	debugfs_create_file("reserved", 0444, root,
			    &memblock.reserved, &memblock_debug_fops);
	if (proc_mkdir("memsize", NULL)) {
		proc_create("memsize/kernel", 0, NULL, &proc_memsize_kernel_fops);
		proc_create("memsize/reserved", 0, NULL, &proc_memsize_reserved_fops);
	}
#ifdef CONFIG_HAVE_MEMBLOCK_PHYS_MAP
	debugfs_create_file("physmem", 0444, root,
			    &memblock.physmem, &memblock_debug_fops);
#endif

	return 0;
}
__initcall(memblock_init_debugfs);
