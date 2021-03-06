/*
 *  linux/arch/arm/mm/mmap.c
 */
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/shm.h>
#include <linux/sched.h>
#include <linux/io.h>
#include <linux/personality.h>
#include <linux/random.h>
#include <linux/security.h>
#include <asm/cachetype.h>
#include <linux/ratelimit.h>

#define COLOUR_ALIGN(addr,pgoff)		\
	((((addr)+SHMLBA-1)&~(SHMLBA-1)) +	\
	 (((pgoff)<<PAGE_SHIFT) & (SHMLBA-1)))

/* gap between mmap and stack */
#define MIN_GAP (128*1024*1024UL)
#define MAX_GAP ((TASK_SIZE)/6*5)

static int mmap_is_legacy(void)
{
	if (current->personality & ADDR_COMPAT_LAYOUT)
		return 1;

	if (rlimit(RLIMIT_STACK) == RLIM_INFINITY)
		return 1;

	return sysctl_legacy_va_layout;
}

static unsigned long mmap_base(unsigned long rnd)
{
	unsigned long gap = rlimit(RLIMIT_STACK);

	if (gap < MIN_GAP)
		gap = MIN_GAP;
	else if (gap > MAX_GAP)
		gap = MAX_GAP;

	return PAGE_ALIGN(TASK_SIZE - gap - rnd);
}

/*
 * We need to ensure that shared mappings are correctly aligned to
 * avoid aliasing issues with VIPT caches.  We need to ensure that
 * a specific page of an object is always mapped at a multiple of
 * SHMLBA bytes.
 *
 * We unconditionally provide this function for all cases, however
 * in the VIVT case, we optimise out the alignment rules.
 */
unsigned long
arch_get_unmapped_area(struct file *filp, unsigned long addr,
		unsigned long len, unsigned long pgoff, unsigned long flags)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	int do_align = 0;
	int aliasing = cache_is_vipt_aliasing();
	struct vm_unmapped_area_info info;
	static DEFINE_RATELIMIT_STATE(mmap_rs, DEFAULT_RATELIMIT_INTERVAL,
						DEFAULT_RATELIMIT_BURST);

	/*
	 * We only need to do colour alignment if either the I or D
	 * caches alias.
	 */
	if (aliasing)
		do_align = filp || (flags & MAP_SHARED);

	/*
	 * We enforce the MAP_FIXED case.
	 */
	if (flags & MAP_FIXED) {
		if (aliasing && flags & MAP_SHARED &&
		    (addr - (pgoff << PAGE_SHIFT)) & (SHMLBA - 1))
			return -EINVAL;
		return addr;
	}

	if (len > TASK_SIZE - mmap_min_addr) {
		if (__ratelimit(&mmap_rs)) {
			printk(KERN_ERR "%s %d - (len > TASK_SIZE - mmap_min_addr) len=0x%lx "
				"TASK_SIZE=0x%lx mmap_min_addr=0x%lx pid=%d total_vm=0x%lx addr=0x%lx\n",
				__func__, __LINE__,
				len, TASK_SIZE, mmap_min_addr, current->pid,
				current->mm->total_vm, addr);
		}
		return -ENOMEM;
	}

	if (addr) {
		if (do_align)
			addr = COLOUR_ALIGN(addr, pgoff);
		else
			addr = PAGE_ALIGN(addr);

		vma = find_vma(mm, addr);
		if (TASK_SIZE - len >= addr && addr >= mmap_min_addr &&
		    (!vma || addr + len <= vm_start_gap(vma)))
			return addr;
	}

	info.flags = 0;
	info.length = len;
	info.low_limit = max(mm->mmap_base, mmap_min_addr);
	info.high_limit = TASK_SIZE;
	info.align_mask = do_align ? (PAGE_MASK & (SHMLBA - 1)) : 0;
	info.align_offset = pgoff << PAGE_SHIFT;
	addr = vm_unmapped_area(&info);
	if (addr == -ENOMEM) {
		if (__ratelimit(&mmap_rs)) {
			printk(KERN_ERR "%s %d - NOMEM from vm_unmapped_area "
				"pid=%d total_vm=0x%lx flags=0x%lx length=0x%lx low_limit=0x%lx "
				"high_limit=0x%lx align_mask=0x%lx align_offset 0x%lx\n",
				__func__, __LINE__,
				current->pid, current->mm->total_vm,
				info.flags, info.length, info.low_limit,
				info.high_limit, info.align_mask,
				info.align_offset);
		}
	}
	return addr;
}

unsigned long
arch_get_unmapped_area_topdown(struct file *filp, const unsigned long addr0,
			const unsigned long len, const unsigned long pgoff,
			const unsigned long flags)
{
	struct vm_area_struct *vma;
	struct mm_struct *mm = current->mm;
	unsigned long addr = addr0;
	int do_align = 0;
	int aliasing = cache_is_vipt_aliasing();
	struct vm_unmapped_area_info info;
	static DEFINE_RATELIMIT_STATE(mmap_rs, DEFAULT_RATELIMIT_INTERVAL,
						DEFAULT_RATELIMIT_BURST);

	/*
	 * We only need to do colour alignment if either the I or D
	 * caches alias.
	 */
	if (aliasing)
		do_align = filp || (flags & MAP_SHARED);

	/* requested length too big for entire address space */
	if (len > TASK_SIZE - mmap_min_addr) {
		if (__ratelimit(&mmap_rs)) {
			printk(KERN_ERR "%s %d - (len > TASK_SIZE - mmap_min_addr) len=%lx "
				"TASK_SIZE=0x%lx mmap_min_addr=0x%lx pid=%d total_vm=0x%lx addr=0x%lx\n",
				__func__, __LINE__,
				len, TASK_SIZE, mmap_min_addr, current->pid,
				current->mm->total_vm, addr);
		}
		return -ENOMEM;
	}

	if (flags & MAP_FIXED) {
		if (aliasing && flags & MAP_SHARED &&
		    (addr - (pgoff << PAGE_SHIFT)) & (SHMLBA - 1))
			return -EINVAL;
		return addr;
	}

	/* requesting a specific address */
	if (addr) {
		if (do_align)
			addr = COLOUR_ALIGN(addr, pgoff);
		else
			addr = PAGE_ALIGN(addr);
		vma = find_vma(mm, addr);
		if (TASK_SIZE - len >= addr && addr >= mmap_min_addr &&
				(!vma || addr + len <= vm_start_gap(vma)))
			return addr;
	}

	info.flags = VM_UNMAPPED_AREA_TOPDOWN;
	info.length = len;
	info.low_limit = max(FIRST_USER_ADDRESS, mmap_min_addr);
	info.high_limit = mm->mmap_base;
	info.align_mask = do_align ? (PAGE_MASK & (SHMLBA - 1)) : 0;
	info.align_offset = pgoff << PAGE_SHIFT;
	addr = vm_unmapped_area(&info);

	/*
	 * A failed mmap() very likely causes application failure,
	 * so fall back to the bottom-up function here. This scenario
	 * can happen with large stack limits and large mmap()
	 * allocations.
	 */
	if (addr & ~PAGE_MASK) {
		VM_BUG_ON(addr != -ENOMEM);
		info.flags = 0;
		info.low_limit = mm->mmap_base;
		info.high_limit = TASK_SIZE;
		addr = vm_unmapped_area(&info);
	}
	if (addr == -ENOMEM) {
		if (__ratelimit(&mmap_rs)) {
			printk(KERN_ERR "%s %d - NOMEM from vm_unmapped_area "
				"pid=%d total_vm=0x%lx flags=0x%lx length=0x%lx low_limit=0x%lx "
				"high_limit=0x%lx align_mask=0x%lx align_offset 0x%lx\n",
				__func__, __LINE__,
				current->pid, current->mm->total_vm,
				info.flags, info.length, info.low_limit,
				info.high_limit, info.align_mask,
				info.align_offset);
		}
	}

	return addr;
}

unsigned long arch_mmap_rnd(void)
{
	unsigned long rnd;

	rnd = (unsigned long)get_random_int() & ((1 << mmap_rnd_bits) - 1);

	return rnd << PAGE_SHIFT;
}

void arch_pick_mmap_layout(struct mm_struct *mm)
{
	unsigned long random_factor = 0UL;

	if (current->flags & PF_RANDOMIZE)
		random_factor = arch_mmap_rnd();

	if (mmap_is_legacy()) {
		mm->mmap_base = TASK_UNMAPPED_BASE + random_factor;
		mm->get_unmapped_area = arch_get_unmapped_area;
	} else {
		mm->mmap_base = mmap_base(random_factor);
		mm->get_unmapped_area = arch_get_unmapped_area_topdown;
	}
}

/*
 * You really shouldn't be using read() or write() on /dev/mem.  This
 * might go away in the future.
 */
int valid_phys_addr_range(phys_addr_t addr, size_t size)
{
	if (addr < PHYS_OFFSET)
		return 0;
	if (addr + size > __pa(high_memory - 1) + 1)
		return 0;

	return 1;
}

/*
 * Do not allow /dev/mem mappings beyond the supported physical range.
 */
int valid_mmap_phys_addr_range(unsigned long pfn, size_t size)
{
	return (pfn + (size >> PAGE_SHIFT)) <= (1 + (PHYS_MASK >> PAGE_SHIFT));
}

#ifdef CONFIG_STRICT_DEVMEM

#include <linux/ioport.h>

/*
 * devmem_is_allowed() checks to see if /dev/mem access to a certain
 * address is valid. The argument is a physical page number.
 * We mimic x86 here by disallowing access to system RAM as well as
 * device-exclusive MMIO regions. This effectively disable read()/write()
 * on /dev/mem.
 */
int devmem_is_allowed(unsigned long pfn)
{
	if (iomem_is_exclusive(pfn << PAGE_SHIFT))
		return 0;
	if (!page_is_ram(pfn))
		return 1;
	return 0;
}

#endif
