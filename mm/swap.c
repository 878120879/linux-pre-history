/*
 *  linux/mm/swap.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 * This file should contain most things doing the swapping from/to disk.
 * Started 18.12.91
 */

#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/head.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/stat.h>

static int lowest_bit = 0;
static int highest_bit = 0;

extern unsigned long free_page_list;

/*
 * The following are used to make sure we don't thrash too much...
 */
#define NR_LAST_FREE_PAGES 32
static unsigned long last_free_pages[NR_LAST_FREE_PAGES] = {0,};

#define SWAP_BITS (4096<<3)

#define bitop(name,op) \
static inline int name(char * addr,unsigned int nr) \
{ \
int __res; \
__asm__ __volatile__("bt" op " %1,%2; adcl $0,%0" \
:"=g" (__res) \
:"r" (nr),"m" (*(addr)),"0" (0)); \
return __res; \
}

bitop(bit,"")
bitop(setbit,"s")
bitop(clrbit,"r")

static char * swap_bitmap = NULL;
static char * swap_lockmap = NULL;
unsigned int swap_device = 0;
struct inode * swap_file = NULL;

void rw_swap_page(int rw, unsigned int nr, char * buf)
{
	static struct wait_queue * lock_queue = NULL;

	if (!swap_lockmap) {
		printk("No swap lock-map\n");
		return;
	}
	while (setbit(swap_lockmap,nr))
		sleep_on(&lock_queue);
	if (swap_device) {
		ll_rw_page(rw,swap_device,nr,buf);
	} else if (swap_file) {
		unsigned int zones[4];
		unsigned int block = nr << 2;
		int i;

		for (i = 0; i < 4; i++)
			if (!(zones[i] = bmap(swap_file,block++))) {
				printk("rw_swap_page: bad swap file\n");
				return;
			}
		ll_rw_swap_file(rw,swap_file->i_dev, zones,4,buf);
	} else
		printk("re_swap_page: no swap file or device\n");
	if (!clrbit(swap_lockmap,nr))
		printk("rw_swap_page: lock already cleared\n");
	wake_up(&lock_queue);
}

static unsigned int get_swap_page(void)
{
	unsigned int nr;

	if (!swap_bitmap)
		return 0;
	for (nr = lowest_bit; nr <= highest_bit ; nr++)
		if (clrbit(swap_bitmap,nr)) {
			if (nr == highest_bit)
				highest_bit--;
			return lowest_bit = nr;
		}
	return 0;
}

void swap_free(unsigned int swap_nr)
{
	if (!swap_nr)
		return;
	if (swap_bitmap && swap_nr < SWAP_BITS) {
		if (swap_nr < lowest_bit)
			lowest_bit = swap_nr;
		if (swap_nr > highest_bit)
			highest_bit = swap_nr;
		if (!setbit(swap_bitmap,swap_nr))
			return;
	}
	printk("swap_free: swap-space bitmap bad (bit %d)\n",swap_nr);
	return;
}

void swap_in(unsigned long *table_ptr)
{
	unsigned long swap_nr;
	unsigned long page;

	swap_nr = *table_ptr;
	if (1 & swap_nr) {
		printk("trying to swap in present page\n\r");
		return;
	}
	if (!swap_nr) {
		printk("No swap page in swap_in\n\r");
		return;
	}
	if (!swap_bitmap) {
		printk("Trying to swap in without swap bit-map");
		*table_ptr = BAD_PAGE;
		return;
	}
	page = get_free_page(GFP_KERNEL);
	if (!page) {
		oom(current);
		page = BAD_PAGE;
	} else	
		read_swap_page(swap_nr>>1, (char *) page);
	if (*table_ptr != swap_nr) {
		free_page(page);
		return;
	}
	swap_free(swap_nr>>1);
	*table_ptr = page | (PAGE_DIRTY | 7);
}

int try_to_swap_out(unsigned long * table_ptr)
{
	int i;
	unsigned long page;
	unsigned long swap_nr;

	page = *table_ptr;
	if (!(PAGE_PRESENT & page))
		return 0;
	*table_ptr &= ~PAGE_ACCESSED;
	if (PAGE_ACCESSED & page)
		return 0;
	if (page < low_memory || page >= high_memory)
		return 0;
	for (i = 0; i < NR_LAST_FREE_PAGES; i++)
		if (last_free_pages[i] == (page & 0xfffff000))
			return 0;
	if (PAGE_DIRTY & page) {
		page &= 0xfffff000;
		if (mem_map[MAP_NR(page)] != 1)
			return 0;
		if (!(swap_nr = get_swap_page()))
			return 0;
		*table_ptr = swap_nr<<1;
		invalidate();
		write_swap_page(swap_nr, (char *) page);
		free_page(page);
		return 1;
	}
	page &= 0xfffff000;
	*table_ptr = 0;
	invalidate();
	free_page(page);
	return 1;
}

static int swap_task = 1;
static int swap_table = 0;
static int swap_page = 0;

/*
 * sys_idle() does nothing much: it just searches for likely candidates for
 * swapping out or forgetting about. This speeds up the search when we
 * actually have to swap.
 */
int sys_idle(void)
{
	struct task_struct * p;
	unsigned long page;

	need_resched = 1;
	if (swap_task >= NR_TASKS)
		swap_task = 1;
	p = task[swap_task];
	if (!p || !p->swappable) {
		swap_task++;
		return 0;
	}
	if (swap_table >= 1024) {
		swap_task++;
		swap_table = 0;
		return 0;
	}
	page = ((unsigned long *) p->tss.cr3)[swap_table];
	if (!(page & 1) || (page < low_memory)) {
		swap_table++;
		return 0;
	}
	page &= 0xfffff000;
	if (swap_page >= 1024) {
		swap_page = 0;
		swap_table++;
		return 0;
	}
	page = *(swap_page + (unsigned long *) page);
	if ((page < low_memory) || !(page & PAGE_PRESENT) || (page & PAGE_ACCESSED))
		swap_page++;
	return 0;
}

/*
 * Go through the page tables, searching for a user page that
 * we can swap out.
 *
 * We now check that the process is swappable (normally only 'init'
 * is un-swappable), allowing high-priority processes which cannot be
 * swapped out (things like user-level device drivers (Not implemented)).
 */
int swap_out(unsigned int priority)
{
	int counter = NR_TASKS;
	int pg_table;
	struct task_struct * p;

	counter <<= priority;
check_task:
	if (counter-- < 0)
		return 0;
	if (swap_task >= NR_TASKS) {
		swap_task = 1;
		goto check_task;
	}
	p = task[swap_task];
	if (!p || !p->swappable) {
		swap_task++;
		goto check_task;
	}
check_dir:
	if (swap_table >= 1024) {
		swap_table = 0;
		swap_task++;
		goto check_task;
	}
	pg_table = ((unsigned long *) p->tss.cr3)[swap_table];
	if (pg_table < low_memory) {
		swap_table++;
		goto check_dir;
	}
	if (!(1 & pg_table)) {
		printk("bad page-table at pg_dir[%d]: %08x\n\r",
			swap_table,pg_table);
		((unsigned long *) p->tss.cr3)[swap_table] = 0;
		swap_table++;
		goto check_dir;
	}
	pg_table &= 0xfffff000;
check_table:
	if (swap_page >= 1024) {
		swap_page = 0;
		swap_table++;
		goto check_dir;
	}
	if (try_to_swap_out(swap_page + (unsigned long *) pg_table)) {
		p->rss--;
		return 1;
	}
	swap_page++;
	goto check_table;
}

static int try_to_free_page(void)
{
	if (shrink_buffers(0))
		return 1;
	if (swap_out(0))
		return 1;
	if (shrink_buffers(1))
		return 1;
	if (swap_out(1))
		return 1;
	if (shrink_buffers(2))
		return 1;
	if (swap_out(2))
		return 1;
	if (shrink_buffers(3))
		return 1;
	return swap_out(3);
}

/*
 * Get physical address of first (actually last :-) free page, and mark it
 * used. If no free pages left, return 0.
 */
unsigned long get_free_page(int priority)
{
	unsigned long result;
	static unsigned long index = 0;

repeat:
	result = free_page_list;
	if (result) {
		if ((result & 0xfff) || result < low_memory || result >= high_memory) {
			free_page_list = 0;
			printk("Result = %08x - memory map destroyed\n");
			panic("mm error");
		}
		free_page_list = *(unsigned long *) result;
		nr_free_pages--;
		if (mem_map[MAP_NR(result)]) {
			printk("Free page %08x has mem_map = %d\n",
				result,mem_map[MAP_NR(result)]);
			goto repeat;
		}
		mem_map[MAP_NR(result)] = 1;
		__asm__ __volatile__("cld ; rep ; stosl"
			::"a" (0),"c" (1024),"D" (result)
			:"di","cx");
		if (index >= NR_LAST_FREE_PAGES)
			index = 0;
		last_free_pages[index] = result;
		index++;
		return result;
	}
	if (nr_free_pages) {
		printk("Damn. mm_free_page count is off by %d\r\n",
			nr_free_pages);
		nr_free_pages = 0;
	}
	if (priority <= GFP_BUFFER)
		return 0;
	if (try_to_free_page())
		goto repeat;
	return 0;
}

/*
 * Written 01/25/92 by Simmule Turner, heavily changed by Linus.
 *
 * The swapon system call
 */
int sys_swapon(const char * specialfile)
{
	struct inode * swap_inode;
	char * tmp;
	int i,j;

	if (!suser())
		return -EPERM;
	i = namei(specialfile,&swap_inode);
	if (i)
		return i;
	if (swap_file || swap_device || swap_bitmap || swap_lockmap) {
		iput(swap_inode);
		return -EBUSY;
	}
	if (S_ISBLK(swap_inode->i_mode)) {
		swap_device = swap_inode->i_rdev;
		iput(swap_inode);
	} else if (S_ISREG(swap_inode->i_mode))
		swap_file = swap_inode;
	else {
		iput(swap_inode);
		return -EINVAL;
	}
	tmp = (char *) get_free_page(GFP_USER);
	swap_lockmap = (char *) get_free_page(GFP_USER);
	if (!tmp || !swap_lockmap) {
		printk("Unable to start swapping: out of memory :-)\n");
		free_page((long) tmp);
		free_page((long) swap_lockmap);
		iput(swap_file);
		swap_device = 0;
		swap_file = NULL;
		swap_bitmap = NULL;
		swap_lockmap = NULL;
		return -ENOMEM;
	}
	read_swap_page(0,tmp);
	if (strncmp("SWAP-SPACE",tmp+4086,10)) {
		printk("Unable to find swap-space signature\n\r");
		free_page((long) tmp);
		free_page((long) swap_lockmap);
		iput(swap_file);
		swap_device = 0;
		swap_file = NULL;
		swap_bitmap = NULL;
		swap_lockmap = NULL;
		return -EINVAL;
	}
	memset(tmp+4086,0,10);
	j = 0;
	lowest_bit = 0;
	highest_bit = 0;
	for (i = 1 ; i < SWAP_BITS ; i++)
		if (bit(tmp,i)) {
			if (!lowest_bit)
				lowest_bit = i;
			highest_bit = i;
			j++;
		}
	if (!j) {
		printk("Empty swap-file\n");
		free_page((long) tmp);
		free_page((long) swap_lockmap);
		iput(swap_file);
		swap_device = 0;
		swap_file = NULL;
		swap_bitmap = NULL;
		swap_lockmap = NULL;
		return -EINVAL;
	}
	swap_bitmap = tmp;
	printk("Adding Swap: %d pages (%d bytes) swap-space\n\r",j,j*4096);
	return 0;
}
