/*
 * Regular lowlevel cardbus driver ("yenta")
 *
 * (C) Copyright 1999 Linus Torvalds
 */
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/sched.h>
#include <linux/interrupt.h>

#include <pcmcia/ss.h>

#include <asm/io.h>

#include "yenta.h"
#include "i82365.h"

/* Don't ask.. */
#define to_cycles(ns)	((ns)/120)
#define to_ns(cycles)	((cycles)*120)

/* Fixme! */
static int yenta_inquire(pci_socket_t *socket, socket_cap_t *cap)
{
	cap->features = SS_CAP_PAGE_REGS | SS_CAP_PCCARD | SS_CAP_CARDBUS;
	cap->irq_mask = 0;
	cap->map_size = 0;
	cap->pci_irq = socket->irq;
	cap->cardbus = 1;
	cap->cb_bus = NULL;
	cap->bus = NULL;

printk("yenta_inquire()\n");

	return 0;
}

/*
 * Silly interface. We convert the cardbus status to a internal status,
 * and we probably really should keep it in cardbus status form and
 * only convert for old-style 16-bit PCMCIA cards..
 */
static int yenta_get_status(pci_socket_t *socket, unsigned int *value)
{
	u32 state = cb_readl(socket, CB_SOCKET_STATE);
	u8 status;
	unsigned int val;

	/* Convert from Yenta status to old-style status */
	val  = (state & CB_CARDSTS) ? SS_STSCHG : 0;
	val |= (state & (CB_CDETECT1 | CB_CDETECT2)) ? 0 : SS_DETECT;
	val |= (state & CB_PWRCYCLE) ? SS_POWERON | SS_READY : 0;
	val |= (state & CB_CBCARD) ? SS_CARDBUS : 0;
	val |= (state & CB_3VCARD) ? SS_3VCARD : 0;
	val |= (state & CB_XVCARD) ? SS_XVCARD : 0;

	/* Get the old compatibility status too.. */
	status = exca_readb(socket, I365_STATUS);
	val |= (status & I365_CS_WRPROT) ? SS_WRPROT : 0;
	val |= (status & I365_CS_READY) ? SS_READY : 0;
	val |= (status & I365_CS_POWERON) ? SS_POWERON : 0;

printk("yenta_get_status(%p)= %x\n", socket, val);

	*value = val;
	return 0;
}

static int yenta_Vcc_power(u32 control)
{
	switch ((control >> CB_VCCCTRL) & CB_PWRBITS) {
	case CB_PWR5V: return 50;
	case CB_PWR3V: return 33;
	default: return 0;
	}
}

static int yenta_Vpp_power(u32 control)
{
	switch ((control >> CB_VPPCTRL) & CB_PWRBITS) {
	case CB_PWR12V: return 120;
	case CB_PWR5V: return 50;
	case CB_PWR3V: return 33;
	default: return 0;
	}
}

static int yenta_get_socket(pci_socket_t *socket, socket_state_t *state)
{
	u32 control = cb_readl(socket, CB_SOCKET_CONTROL);
	u8 reg;

	state->Vcc = yenta_Vcc_power(control);
	state->Vpp = yenta_Vpp_power(control);
	state->io_irq = socket->irq;

	reg = exca_readb(socket, I365_POWER);
	state->flags = (reg & I365_PWR_AUTO) ? SS_PWR_AUTO : 0;
	state->flags |= (reg & I365_PWR_OUT) ? SS_OUTPUT_ENA : 0;

	reg = exca_readb(socket, I365_INTCTL);
	state->flags |= (reg & I365_PC_RESET) ? 0 : SS_RESET;
	state->flags |= (reg & I365_PC_IOCARD) ? SS_IOCARD : 0;

	reg = exca_readb(socket, I365_CSCINT);
	state->csc_mask = (reg & I365_CSC_DETECT) ? SS_DETECT : 0;
	if (state->flags & SS_IOCARD) {
		state->csc_mask |= (reg & I365_CSC_STSCHG) ? SS_STSCHG : 0;
	} else {
		state->csc_mask |= (reg & I365_CSC_BVD1) ? SS_BATDEAD : 0;
		state->csc_mask |= (reg & I365_CSC_BVD2) ? SS_BATWARN : 0;
		state->csc_mask |= (reg & I365_CSC_READY) ? SS_READY : 0;
	}

printk("yenta_get_socket(%p) = %d, %d\n", socket, state->Vcc, state->Vpp);

	return 0;
}

static int yenta_set_socket(pci_socket_t *socket, socket_state_t *state)
{
	u8 reg;
	u16 bridge;
	u32 control;

printk("yenta_set_socket(%p, %d, %d, %x)\n", socket, state->Vcc, state->Vpp, state->flags);

	bridge = config_readw(socket, CB_BRIDGE_CONTROL);
	bridge &= ~CB_BRIDGE_CRST;
	bridge |= (state->flags & SS_RESET) ? CB_BRIDGE_CRST : 0;
	config_writew(socket, CB_BRIDGE_CONTROL, bridge);
	
	reg = socket->irq;
	reg |= (state->flags & SS_RESET) ? 0 : I365_PC_RESET;
	reg |= (state->flags & SS_IOCARD) ? I365_PC_IOCARD : 0;
	exca_writeb(socket, I365_INTCTL, reg);

	reg = I365_PWR_NORESET;
	control = 0;	/* CB_STOPCLK ? Better power management */
	switch (state->Vcc) {
	case 33:
		control |= CB_PWR3V << CB_VCCCTRL;
		reg |= I365_VCC_5V;
		break;
	case 50:
		control |= CB_PWR5V << CB_VCCCTRL;
		reg |= I365_VCC_5V;
		break;
	}
	switch (state->Vpp) {
	case 33:
		control |= CB_PWR3V << CB_VPPCTRL;
		reg |= I365_VPP1_5V;
		break;
	case 50:
		control |= CB_PWR5V << CB_VPPCTRL;
		reg |= I365_VPP1_5V;
		break;
	case 120:
		control |= CB_PWR12V << CB_VPPCTRL;
		reg |= I365_VPP1_12V;
		break;
	}
	cb_writel(socket, CB_SOCKET_CONTROL, control);

	reg |= (state->flags & SS_PWR_AUTO) ? I365_PWR_AUTO : 0;
	reg |= (state->flags & SS_OUTPUT_ENA) ? I365_PWR_OUT : 0;
	if (state->flags & SS_PWR_AUTO) reg |= I365_PWR_AUTO;
	exca_writeb(socket, I365_POWER, reg);

	reg = socket->irq;
	reg |= (state->csc_mask & SS_DETECT) ? I365_CSC_DETECT : 0;
	if (state->flags & SS_IOCARD) {
		reg |= (state->csc_mask & SS_STSCHG) ? I365_CSC_STSCHG : 0;
	} else {
		reg |= (state->csc_mask & SS_BATDEAD) ? I365_CSC_BVD1 : 0;
		reg |= (state->csc_mask & SS_BATWARN) ? I365_CSC_BVD2 : 0;
		reg |= (state->csc_mask & SS_READY) ? I365_CSC_READY : 0;
	}
	exca_writeb(socket, I365_CSCINT, reg);
	exca_readb(socket, I365_CSC);

	return 0;
}

static int yenta_get_io_map(pci_socket_t *socket, struct pccard_io_map *io)
{
	int map;
	unsigned char ioctl, addr;

	map = io->map;
	if (map > 1)
		return -EINVAL;

	io->start = exca_readw(socket, I365_IO(map)+I365_W_START);
	io->stop = exca_readw(socket, I365_IO(map)+I365_W_STOP);

	ioctl = exca_readb(socket, I365_IOCTL);
	addr = exca_readb(socket, I365_ADDRWIN);
	io->speed = to_ns(ioctl & I365_IOCTL_WAIT(map)) ? 1 : 0;
	io->flags  = (addr & I365_ENA_IO(map)) ? MAP_ACTIVE : 0;
	io->flags |= (ioctl & I365_IOCTL_0WS(map)) ? MAP_0WS : 0;
	io->flags |= (ioctl & I365_IOCTL_16BIT(map)) ? MAP_16BIT : 0;
	io->flags |= (ioctl & I365_IOCTL_IOCS16(map)) ? MAP_AUTOSZ : 0;

printk("yenta_get_io_map(%d) = %x, %x, %x\n", map, io->start, io->stop, io->flags);

	return 0;
}

static int yenta_set_io_map(pci_socket_t *socket, struct pccard_io_map *io)
{
	int map;
	unsigned char ioctl, addr, enable;

	map = io->map;

printk("yenta_set_io_map(%d, %x, %x, %x)\n", map, io->start, io->stop, io->flags);

	if (map > 1)
		return -EINVAL;

	enable = I365_ENA_IO(map);
	addr = exca_readb(socket, I365_ADDRWIN);

	/* Disable the window before changing it.. */
	if (addr & enable) {
		addr &= ~enable;
		exca_writeb(socket, I365_ADDRWIN, addr);
	}

	exca_writew(socket, I365_IO(map)+I365_W_START, io->start);
	exca_writew(socket, I365_IO(map)+I365_W_STOP, io->stop);

	ioctl = exca_readb(socket, I365_IOCTL) & ~I365_IOCTL_MASK(map);
	if (io->flags & MAP_0WS) ioctl |= I365_IOCTL_0WS(map);
	if (io->flags & MAP_16BIT) ioctl |= I365_IOCTL_16BIT(map);
	if (io->flags & MAP_AUTOSZ) ioctl |= I365_IOCTL_IOCS16(map);
	exca_writeb(socket, I365_IOCTL, ioctl);

	if (io->flags & MAP_ACTIVE)
		exca_writeb(socket, I365_ADDRWIN, addr | enable);
	return 0;
}

static int yenta_get_mem_map(pci_socket_t *socket, struct pccard_mem_map *mem)
{
	int map;
	unsigned char addr;
	unsigned int start, stop, page, offset;

	map = mem->map;
	if (map > 4)
		return -EINVAL;

	addr = exca_readb(socket, I365_ADDRWIN);
	mem->flags = (addr & I365_ENA_MEM(map)) ? MAP_ACTIVE : 0;

	start = exca_readw(socket, I365_MEM(map) + I365_W_START);
	mem->flags |= (start & I365_MEM_16BIT) ? MAP_16BIT : 0;
	mem->flags |= (start & I365_MEM_0WS) ? MAP_0WS : 0;
	start = (start & 0x0fff) << 12;

	stop = exca_readw(socket, I365_MEM(map) + I365_W_STOP);
	mem->speed  = (stop & I365_MEM_WS0) ? 1 : 0;
	mem->speed += (stop & I365_MEM_WS1) ? 2 : 0;
	mem->speed = to_ns(mem->speed);
	stop = ((u_long)(stop & 0x0fff) << 12) + 0x0fff;

	offset = exca_readw(socket, I365_MEM(map) + I365_W_OFF);
	mem->flags |= (offset & I365_MEM_WRPROT) ? MAP_WRPROT : 0;
	mem->flags |= (offset & I365_MEM_REG) ? MAP_ATTRIB : 0;
	offset = ((offset & 0x3fff) << 12) + start;
	mem->card_start = offset & 0x3ffffff;

	page = exca_readb(socket, CB_MEM_PAGE(map)) << 24;
	mem->sys_start = start + page;
	mem->sys_stop = start + page;

printk("yenta_get_map(%d) = %lx, %lx, %x\n", map, mem->sys_start, mem->sys_stop, mem->card_start);

	return 0;
}

static int yenta_set_mem_map(pci_socket_t *socket, struct pccard_mem_map *mem)
{
	int map;
	unsigned char addr, enable;
	unsigned int start, stop, card_start;
	unsigned short word;

	map = mem->map;
	start = mem->sys_start;
	stop = mem->sys_stop;
	card_start = mem->card_start;

printk("yenta_set_map(%d, %x, %x, %x)\n", map, start, stop, card_start);

	if (map > 4 || start > stop || ((start ^ stop) >> 24) || (card_start >> 26) || mem->speed > 1000)
		return -EINVAL;

	enable = I365_ENA_MEM(map);
	addr = exca_readb(socket, I365_ADDRWIN);
	if (addr & enable) {
		addr &= ~enable;
		exca_writeb(socket, I365_ADDRWIN, addr);
	}

	word = (start >> 12) & 0x0fff;
	if (mem->flags & MAP_16BIT)
		word |= I365_MEM_16BIT;
	if (mem->flags & MAP_0WS)
		word |= I365_MEM_0WS;
	exca_writew(socket, I365_MEM(map) + I365_W_START, word);

	word = (stop >> 12) & 0x0fff;
	switch (to_cycles(mem->speed)) {
		case 0: break;
		case 1:  word |= I365_MEM_WS0; break;
		case 2:  word |= I365_MEM_WS1; break;
		default: word |= I365_MEM_WS1 | I365_MEM_WS0; break;
	}
	exca_writew(socket, I365_MEM(map) + I365_W_STOP, word);

	word = ((card_start - start) >> 12) & 0x3fff;
	if (mem->flags & MAP_WRPROT)
		word |= I365_MEM_WRPROT;
	if (mem->flags & MAP_ATTRIB)
		word |= I365_MEM_REG;
	exca_writew(socket, I365_MEM(map) + I365_W_OFF, word);

	if (mem->flags & MAP_ACTIVE)
		exca_writeb(socket, I365_ADDRWIN, addr | enable);
	return 0;
}

static int yenta_get_bridge(pci_socket_t *socket, struct cb_bridge_map *m)
{
	printk("yenta_get_bridge() called\n");
	return -EINVAL;
}

static int yenta_set_bridge(pci_socket_t *socket, struct cb_bridge_map *m)
{
	printk("yenta_set_bridge() called\n");
	return -EINVAL;
}

static void yenta_proc_setup(pci_socket_t *socket, struct proc_dir_entry *base)
{
	/* Not done yet */
}

static void yenta_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	pci_socket_t *sock = (pci_socket_t *) dev_id;
	u32 event = readl(sock->base + CB_SOCKET_EVENT);

	/* Clear interrupt status for the event */
	writel(event, sock->base +CB_SOCKET_EVENT);

	printk("Socket interrupt event %08x\n", event);
}

/*
 * Initialize a cardbus controller. Make sure we have a usable
 * interrupt, and that we can map the cardbus area. Fill in the
 * socket information structure..
 */
static int yenta_open(pci_socket_t *socket)
{
	struct pci_dev *dev = socket->dev;

	/*
	 * Do some basic sanity checking..
	 */
	if (pci_enable_device(dev)) {
		printk("Unable to enable device\n");
		return -1;
	}
	if (!dev->irq) {
		printk("No cardbus irq!\n");
		return -1;
	}
	if (!dev->resource[0].start) {
		printk("No cardbus resource!\n");
		return -1;
	}

	/*
	 * Ok, start setup.. Map the cardbus registers,
	 * and request the IRQ.
	 */
	socket->base = ioremap(dev->resource[0].start, 0x1000);
	if (!socket->base)
		return -1;
	if (request_irq(dev->irq, yenta_interrupt, SA_SHIRQ, dev->name, socket))
		return -1;
	socket->irq = dev->irq;

	/* Enable all events */
	writel(0x0f, socket->base + 4);

	printk("Socket status: %08x\n", readl(socket->base + 8));
	return 0;
}

/*
 * Close it down - release our resources and go home..
 */
static void yenta_close(pci_socket_t *sock)
{
	if (sock->irq)
		free_irq(sock->irq, sock);
	if (sock->base)
		iounmap(sock->base);
}

struct pci_socket_ops yenta_operations = {
	yenta_open,
	yenta_close,
	yenta_inquire,
	yenta_get_status,
	yenta_get_socket,
	yenta_set_socket,
	yenta_get_io_map,
	yenta_set_io_map,
	yenta_get_mem_map,
	yenta_set_mem_map,
	yenta_get_bridge,
	yenta_set_bridge,
	yenta_proc_setup
};
