/*
 *  Originally from linux/drivers/char/mem.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  Added devfs support. 
 *    Jan-11-1998, C. Scott Ananian <cananian@alumni.princeton.edu>
 *  Shared /dev/zero mmaping support, Feb 2000, Kanoj Sarcar <kanoj@sgi.com>
 */

#include <linux/mm.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mman.h>
#include <linux/random.h>
#include <linux/init.h>
#include <linux/raw.h>
#include <linux/tty.h>
#include <linux/capability.h>
#include <linux/ptrace.h>
#include <linux/device.h>
#include <asm/pgalloc.h>
#include <asm/uaccess.h>
#include <asm/io.h>
#include <asm/hypervisor.h>

static inline int uncached_access(struct file *file)
{
	if (file->f_flags & O_SYNC)
		return 1;
	/* Xen sets correct MTRR type on non-RAM for us. */
	return 0;
}

static inline int range_is_allowed(unsigned long pfn, unsigned long size)
{
#ifdef CONFIG_STRICT_DEVMEM
	u64 from = ((u64)pfn) << PAGE_SHIFT;
	u64 to = from + size;
	u64 cursor = from;

	while (cursor < to) {
		if (!devmem_is_allowed(pfn)) {
			printk(KERN_INFO
		"Program %s tried to access /dev/mem between %Lx->%Lx.\n",
				current->comm, from, to);
			return 0;
		}
		cursor += PAGE_SIZE;
		pfn++;
	}
#endif
	return 1;
}

/*
 * This funcion reads the *physical* memory. The f_pos points directly to the 
 * memory location. 
 */
static ssize_t read_mem(struct file * file, char __user * buf,
			size_t count, loff_t *ppos)
{
	unsigned long p = *ppos, ignored;
	ssize_t read = 0, sz;
	void __iomem *v;

	while (count > 0) {
		/*
		 * Handle first page in case it's not aligned
		 */
		if (-p & (PAGE_SIZE - 1))
			sz = -p & (PAGE_SIZE - 1);
		else
			sz = PAGE_SIZE;

		sz = min_t(unsigned long, sz, count);

		if (!range_is_allowed(p >> PAGE_SHIFT, count))
			return -EPERM;

		v = ioremap(p, sz);
		if (IS_ERR(v) || v == NULL) {
			/*
			 * Some programs (e.g., dmidecode) groove off into
			 * weird RAM areas where no tables can possibly exist
			 * (because Xen will have stomped on them!). These
			 * programs get rather upset if we let them know that
			 * Xen failed their access, so we fake out a read of
			 * all zeroes.
			 */
			if (clear_user(buf, count))
				return -EFAULT;
			read += count;
			break;
		}

		ignored = copy_to_user(buf, v, sz);
		iounmap(v);
		if (ignored)
			return -EFAULT;
		buf += sz;
		p += sz;
		count -= sz;
		read += sz;
	}

	*ppos += read;
	return read;
}

static ssize_t write_mem(struct file * file, const char __user * buf, 
			 size_t count, loff_t *ppos)
{
	unsigned long p = *ppos, ignored;
	ssize_t written = 0, sz;
	void __iomem *v;

	while (count > 0) {
		/*
		 * Handle first page in case it's not aligned
		 */
		if (-p & (PAGE_SIZE - 1))
			sz = -p & (PAGE_SIZE - 1);
		else
			sz = PAGE_SIZE;

		sz = min_t(unsigned long, sz, count);

		if (!range_is_allowed(p >> PAGE_SHIFT, sz))
			return -EPERM;

		v = ioremap(p, sz);
		if (v == NULL)
			break;
		if (IS_ERR(v)) {
			if (written == 0)
				return PTR_ERR(v);
			break;
		}

		ignored = copy_from_user(v, buf, sz);
		iounmap(v);
		if (ignored) {
			written += sz - ignored;
			if (written)
				break;
			return -EFAULT;
		}
		buf += sz;
		p += sz;
		count -= sz;
		written += sz;
	}

	*ppos += written;
	return written;
}

#ifndef ARCH_HAS_DEV_MEM_MMAP_MEM
static struct vm_operations_struct mmap_mem_ops = {
#ifdef CONFIG_HAVE_IOREMAP_PROT
	.access = generic_access_phys
#endif
};

static int xen_mmap_mem(struct file * file, struct vm_area_struct * vma)
{
	size_t size = vma->vm_end - vma->vm_start;

	if (uncached_access(file))
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	if (!range_is_allowed(vma->vm_pgoff, size))
		return -EPERM;

	if (!phys_mem_access_prot_allowed(file, vma->vm_pgoff, size,
						&vma->vm_page_prot))
		return -EINVAL;

	vma->vm_ops = &mmap_mem_ops;

	/* We want to return the real error code, not EAGAIN. */
	return direct_remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff,
				      size, vma->vm_page_prot, DOMID_IO);
}
#endif

/*
 * The memory devices use the full 32/64 bits of the offset, and so we cannot
 * check against negative addresses: they are ok. The return value is weird,
 * though, in that case (0).
 *
 * also note that seeking relative to the "end of file" isn't supported:
 * it has no meaning, so it returns -EINVAL.
 */
static loff_t memory_lseek(struct file * file, loff_t offset, int orig)
{
	loff_t ret;

	mutex_lock(&file->f_path.dentry->d_inode->i_mutex);
	switch (orig) {
		case 0:
			file->f_pos = offset;
			ret = file->f_pos;
			force_successful_syscall_return();
			break;
		case 1:
			file->f_pos += offset;
			ret = file->f_pos;
			force_successful_syscall_return();
			break;
		default:
			ret = -EINVAL;
	}
	mutex_unlock(&file->f_path.dentry->d_inode->i_mutex);
	return ret;
}

static int open_mem(struct inode * inode, struct file * filp)
{
	return capable(CAP_SYS_RAWIO) ? 0 : -EPERM;
}

const struct file_operations mem_fops = {
	.llseek		= memory_lseek,
	.read		= read_mem,
	.write		= write_mem,
	.mmap		= xen_mmap_mem,
	.open		= open_mem,
};
