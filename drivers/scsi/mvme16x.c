/*
 * Detection routine for the NCR53c710 based MVME16x SCSI Controllers for Linux.
 *
 * Based on work by Alan Hourihane
 */
#include <linux/types.h>
#include <linux/mm.h>
#include <linux/blk.h>
#include <linux/sched.h>
#include <linux/version.h>
#include <linux/zorro.h>

#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/mvme16xhw.h>
#include <asm/irq.h>

#include "scsi.h"
#include "hosts.h"
#include "53c7xx.h"
#include "mvme16x.h"
#include "asm/mvme16xhw.h"

#include<linux/stat.h>

struct proc_dir_entry proc_scsi_mvme16x = {
    PROC_SCSI_MVME16x, 7, "MVME16x",
    S_IFDIR | S_IRUGO | S_IXUGO, 2
};

extern ncr53c7xx_init (Scsi_Host_Template *tpnt, int board, int chip,
			u32 base, int io_port, int irq, int dma,
			long long options, int clock);

int mvme16x_scsi_detect(Scsi_Host_Template *tpnt)
{
    static unsigned char called = 0;
    int clock;
    long long options;

    if (mvme16x_config & MVME16x_CONFIG_NO_SCSICHIP) {
	printk ("SCSI detection disabled, SCSI chip not present\n");
	return 0;
    }
    if (called)
	return 0;

    tpnt->proc_dir = &proc_scsi_mvme16x;

    options = OPTION_MEMORY_MAPPED|OPTION_DEBUG_TEST1|OPTION_INTFLY|OPTION_SYNCHRONOUS|OPTION_ALWAYS_SYNCHRONOUS|OPTION_DISCONNECT;

    clock = 66000000;	/* 66MHz SCSI Clock */

    ncr53c7xx_init(tpnt, 0, 710, (u32)0xfff47000,
			0, 0x55, DMA_NONE,
			options, clock);
    called = 1;
    return 1;
}
