/*
 * SNI specific definitions
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 1997 by Ralf Baechle
 */
#ifndef __ASM_MIPS_SNI_H 
#define __ASM_MIPS_SNI_H 

#define SNI_PORT_BASE	0xb4000000

/*
 * ASIC PCI registers for little endian configuration.
 */
#ifndef __MIPSEL__
#error "Fix me for big endian"
#endif
#define PCIMT_UCONF		0xbfff0000
#define PCIMT_IOADTIMEOUT2	0xbfff0008
#define PCIMT_IOMEMCONF		0xbfff0010
#define PCIMT_IOMMU		0xbfff0018
#define PCIMT_IOADTIMEOUT1	0xbfff0020
#define PCIMT_DMAACCESS		0xbfff0028
#define PCIMT_DMAHIT		0xbfff0030
#define PCIMT_ERRSTATUS		0xbfff0038
#define PCIMT_ERRADDR		0xbfff0040
#define PCIMT_SYNDROME		0xbfff0048
#define PCIMT_ITPEND		0xbfff0050
#define PCIMT_IRQSEL		0xbfff0058
#define PCIMT_TESTMEM		0xbfff0060
#define PCIMT_ECCREG		0xbfff0068
#define PCIMT_CONFIG_ADDRESS	0xbfff0070
#define PCIMT_ASIC_ID		0xbfff0078	/* read */
#define PCIMT_SOFT_RESET	0xbfff0078	/* write */
#define PCIMT_PIA_OE		0xbfff0080
#define PCIMT_PIA_DATAOUT	0xbfff0088
#define PCIMT_PIA_DATAIN	0xbfff0090
#define PCIMT_CACHECONF		0xbfff0098
#define PCIMT_INVSPACE		0xbfff00a0
#define PCIMT_PCI_CONF		0xbfff0100

/*
 * Data port for the PCI bus.
 */
#define PCIMT_CONFIG_DATA	0xb4000cfc

/*
 * Board specific registers
 */
#define PCIMT_CSMSR		0xbfd00000
#define PCIMT_CSSWITCH		0xbfd10000
#define PCIMT_CSITPEND		0xbfd20000
#define PCIMT_AUTO_PO_EN	0xbfd30000
#define PCIMT_CLR_TEMP		0xbfd40000
#define PCIMT_AUTO_PO_DIS	0xbfd50000
#define PCIMT_EXMSR		0xbfd60000
#define PCIMT_UNUSED1		0xbfd70000
#define PCIMT_CSWCSM		0xbfd80000
#define PCIMT_UNUSED2		0xbfd90000
#define PCIMT_CSLED		0xbfda0000
#define PCIMT_CSMAPISA		0xbfdb0000
#define PCIMT_CSRSTBP		0xbfdc0000
#define PCIMT_CLRPOFF		0xbfdd0000
#define PCIMT_CSTIMER		0xbfde0000
#define PCIMT_PWDN		0xbfdf0000

/*
 * Interrupt 0-16 are reserved for PCI and EISA interrupts.  The
 * interrupts from 16 are assigned to the other interrupts generated
 * by the PCI chipset.
 */
#define PCIMT_IRQ_ETHERNET	16
#define PCIMT_IRQ_TEMPERATURE	17
#define PCIMT_IRQ_EISA_NMI	18
#define PCIMT_IRQ_POWER_OFF	19
#define PCIMT_IRQ_BUTTON	20
#define PCIMT_IRQ_INTA		21
#define PCIMT_IRQ_INTB		22
#define PCIMT_IRQ_INTC		23
#define PCIMT_IRQ_INTD		24
#define PCIMT_IRQ_SCSI		25

/*
 * Base address for the mapped 16mb EISA bus segment.
 */
#define PCIMT_EISA_BASE		0xb0000000

#endif /* __ASM_MIPS_SNI_H */
