/* $Id: devmap.c,v 1.5 1997/05/14 20:44:59 davem Exp $
 * promdevmap.c:  Map device/IO areas to virtual addresses.
 *
 * Copyright (C) 1995 David S. Miller (davem@caip.rutgers.edu)
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>

#include <asm/openprom.h>
#include <asm/oplib.h>

/* XXX Let's get rid of this thing if we can... */
extern struct task_struct *current_set[NR_CPUS];

/* Just like the routines in palloc.c, these should not be used
 * by the kernel at all.  Bootloader facility mainly.  And again,
 * this is only available on V2 proms and above.
 */

/* Map physical device address 'paddr' in IO space 'ios' of size
 * 'num_bytes' to a virtual address, with 'vhint' being a hint to
 * the prom as to where you would prefer the mapping.  We return
 * where the prom actually mapped it.
 */
char *
prom_mapio(char *vhint, int ios, unsigned int paddr, unsigned int num_bytes)
{
	unsigned long flags;
	char *ret;

	save_flags(flags); cli();
	if((num_bytes == 0) || (paddr == 0)) ret = (char *) 0x0;
	else
	ret = (*(romvec->pv_v2devops.v2_dumb_mmap))(vhint, ios, paddr,
						    num_bytes);
	__asm__ __volatile__("ld [%0], %%g6\n\t" : :
			     "r" (&current_set[hard_smp_processor_id()]) :
			     "memory");
	restore_flags(flags);
	return ret;
}

/* Unmap an IO/device area that was mapped using the above routine. */
void
prom_unmapio(char *vaddr, unsigned int num_bytes)
{
	unsigned long flags;

	if(num_bytes == 0x0) return;
	save_flags(flags); cli();
	(*(romvec->pv_v2devops.v2_dumb_munmap))(vaddr, num_bytes);
	__asm__ __volatile__("ld [%0], %%g6\n\t" : :
			     "r" (&current_set[hard_smp_processor_id()]) :
			     "memory");
	restore_flags(flags);
	return;
}
