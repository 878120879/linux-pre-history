/*
 *  linux/fs/nfs/file.c
 *
 *  Copyright (C) 1992  Rick Sladkey
 *
 *  Changes Copyright (C) 1994 by Florian La Roche
 *   - Do not copy data too often around in the kernel.
 *   - In nfs_file_read the return value of kmalloc wasn't checked.
 *   - Put in a better version of read look-ahead buffering. Original idea
 *     and implementation by Wai S Kok elekokws@ee.nus.sg.
 *
 *  Expire cache on write to a file by Wai S Kok (Oct 1994).
 *
 *  Total rewrite of read side for new NFS buffer cache.. Linus.
 *
 *  nfs regular file handling functions
 */

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/stat.h>
#include <linux/mm.h>
#include <linux/nfs_fs.h>
#include <linux/malloc.h>
#include <linux/pagemap.h>

#include <asm/segment.h>
#include <asm/system.h>

static int nfs_file_mmap(struct inode *, struct file *, struct vm_area_struct *);
static int nfs_file_read(struct inode *, struct file *, char *, int);
static int nfs_file_write(struct inode *, struct file *, const char *, int);
static int nfs_fsync(struct inode *, struct file *);
static int nfs_readpage(struct inode * inode, struct page * page);

static struct file_operations nfs_file_operations = {
	NULL,			/* lseek - default */
	nfs_file_read,		/* read */
	nfs_file_write,		/* write */
	NULL,			/* readdir - bad */
	NULL,			/* select - default */
	NULL,			/* ioctl - default */
	nfs_file_mmap,		/* mmap */
	NULL,			/* no special open is needed */
	NULL,			/* release */
	nfs_fsync,		/* fsync */
};

struct inode_operations nfs_file_inode_operations = {
	&nfs_file_operations,	/* default file operations */
	NULL,			/* create */
	NULL,			/* lookup */
	NULL,			/* link */
	NULL,			/* unlink */
	NULL,			/* symlink */
	NULL,			/* mkdir */
	NULL,			/* rmdir */
	NULL,			/* mknod */
	NULL,			/* rename */
	NULL,			/* readlink */
	NULL,			/* follow_link */
	nfs_readpage,		/* readpage */
	NULL,			/* writepage */
	NULL,			/* bmap */
	NULL			/* truncate */
};

static inline void revalidate_inode(struct nfs_server * server, struct inode * inode)
{
	struct nfs_fattr fattr;

	if (jiffies - NFS_READTIME(inode) < server->acregmax)
		return;

	NFS_READTIME(inode) = jiffies;
	if (nfs_proc_getattr(server, NFS_FH(inode), &fattr) == 0) {
		nfs_refresh_inode(inode, &fattr);
		if (fattr.mtime.seconds == NFS_OLDMTIME(inode))
			return;
		NFS_OLDMTIME(inode) = fattr.mtime.seconds;
	}
	invalidate_inode_pages(inode);
}


static int nfs_file_read(struct inode * inode, struct file * file,
	char * buf, int count)
{
	revalidate_inode(NFS_SERVER(inode), inode);
	return generic_file_read(inode, file, buf, count);
}

static int nfs_file_mmap(struct inode * inode, struct file * file, struct vm_area_struct * vma)
{
	revalidate_inode(NFS_SERVER(inode), inode);
	return generic_file_mmap(inode, file, vma);
}

static int nfs_fsync(struct inode *inode, struct file *file)
{
	return 0;
}

static inline int do_read_nfs(struct inode * inode, struct page * page,
	char * buf, unsigned long pos)
{
	int result, refresh = 0;
	int count = PAGE_SIZE;
	int rsize = NFS_SERVER(inode)->rsize;
	struct nfs_fattr fattr;

	page->locked = 1;
	do {
		if (count < rsize)
			rsize = count;
		result = nfs_proc_read(NFS_SERVER(inode), NFS_FH(inode), 
			pos, rsize, buf, &fattr);
		if (result < 0)
			break;
		refresh = 1;
		count -= result;
		pos += result;
		buf += result;
		if (result < rsize)
			break;
	} while (count);

	memset(buf, 0, count);
	if (refresh) {
		nfs_refresh_inode(inode, &fattr);
		result = 0;
		page->uptodate = 1;
	}
	page->locked = 0;
	wake_up(&page->wait);
	return result;
}

static int nfs_readpage(struct inode * inode, struct page * page)
{
	int error;
	unsigned long address;

	address = page_address(page);
	page->count++;
	error = do_read_nfs(inode, page, (char *) address, page->offset);
	free_page(address);
	return error;
}

static int nfs_file_write(struct inode *inode, struct file *file, const char *buf,
			  int count)
{
	int result, hunk, i, n, pos;
	struct nfs_fattr fattr;

	if (!inode) {
		printk("nfs_file_write: inode = NULL\n");
		return -EINVAL;
	}
	if (!S_ISREG(inode->i_mode)) {
		printk("nfs_file_write: write to non-file, mode %07o\n",
			inode->i_mode);
		return -EINVAL;
	}
	if (count <= 0)
		return 0;

	pos = file->f_pos;
	if (file->f_flags & O_APPEND)
		pos = inode->i_size;
	n = NFS_SERVER(inode)->wsize;
	for (i = 0; i < count; i += n) {
		hunk = count - i;
		if (hunk >= n)
			hunk = n;
		result = nfs_proc_write(inode,
			pos, hunk, buf, &fattr);
		if (result < 0)
			return result;
		pos += hunk;
		buf += hunk;
		if (hunk < n) {
			i += hunk;
			break;
		}
	}
	file->f_pos = pos;
	nfs_refresh_inode(inode, &fattr);
	return i;
}

