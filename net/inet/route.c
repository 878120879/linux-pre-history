/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		ROUTE - implementation of the IP router.
 *
 * Version:	@(#)route.c	1.0.14	05/31/93
 *
 * Authors:	Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *
 * Fixes:
 *		Alan Cox	:	Verify area fixes.
 *		Alan Cox	:	cli() protects routing changes
 *		Rui Oliveira	:	ICMP routing table updates
 *		(rco@di.uminho.pt)	Routing table insertion and update
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#include <asm/segment.h>
#include <asm/system.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <linux/errno.h>
#include <linux/in.h>
#include "inet.h"
#include "dev.h"
#include "ip.h"
#include "protocol.h"
#include "route.h"
#include "tcp.h"
#include "skbuff.h"
#include "sock.h"
#include "arp.h"
#include "icmp.h"


static struct rtable *rt_base = NULL;


/* Dump the contents of a routing table entry. */
static void
rt_print(struct rtable *rt)
{
  if (rt == NULL || inet_debug != DBG_RT) return;

  printk("RT: %06lx NXT=%06lx FLAGS=0x%02x\n",
		(long) rt, (long) rt->rt_next, rt->rt_flags);
  printk("    TARGET=%s ", in_ntoa(rt->rt_dst));
  printk("GW=%s ", in_ntoa(rt->rt_gateway));
  printk("    DEV=%s USE=%ld REF=%d\n",
	(rt->rt_dev == NULL) ? "NONE" : rt->rt_dev->name,
	rt->rt_use, rt->rt_refcnt);
}


/*
 * Remove a routing table entry.
 */
static void rt_del(unsigned long dst)
{
	struct rtable *r, **rp;
	unsigned long flags;

	DPRINTF((DBG_RT, "RT: flushing for dst %s\n", in_ntoa(dst)));
	rp = &rt_base;
	save_flags(flags);
	cli();
	while((r = *rp) != NULL) {
		if (r->rt_dst != dst) {
			rp = &r->rt_next;
			continue;
		}
		*rp = r->rt_next;
		kfree_s(r, sizeof(struct rtable));
	} 
	restore_flags(flags);
}


/*
 * Remove all routing table entries for a device.
 */
void rt_flush(struct device *dev)
{
	struct rtable *r;
	struct rtable **rp;
	unsigned long flags;

	DPRINTF((DBG_RT, "RT: flushing for dev 0x%08lx (%s)\n", (long)dev, dev->name));
	rp = &rt_base;
	cli();
	save_flags(flags);
	while ((r = *rp) != NULL) {
		if (r->rt_dev != dev) {
			rp = &r->rt_next;
			continue;
		}
		*rp = r->rt_next;
		kfree_s(r, sizeof(struct rtable));
	} 
	restore_flags(flags);
}

/*
 * Used by 'rt_add()' when we can't get the netmask from the device..
 */
static unsigned long guess_mask(unsigned long dst)
{
	unsigned long mask = 0xffffffff;

	while (mask & dst)
		mask <<= 8;
	return ~mask;
}

/*
 * rewrote rt_add(), as the old one was weird. Linus
 */
void
rt_add(short flags, unsigned long dst, unsigned long gw, struct device *dev)
{
	struct rtable *r, *rt;
	struct rtable **rp;
	unsigned long mask;
	unsigned long cpuflags;

	/* Allocate an entry. */
	rt = (struct rtable *) kmalloc(sizeof(struct rtable), GFP_ATOMIC);
	if (rt == NULL) {
		DPRINTF((DBG_RT, "RT: no memory for new route!\n"));
		return;
	}
	/* Fill in the fields. */
	memset(rt, 0, sizeof(struct rtable));
	rt->rt_flags = (flags | RTF_UP);
  	/*
	 * Gateway to our own interface is really direct
	 */
	if (gw == dev->pa_addr || gw == dst) {
		gw=0;
		rt->rt_flags&=~RTF_GATEWAY;
	}
	if (gw != 0) 
		rt->rt_flags |= RTF_GATEWAY;
	rt->rt_dev = dev;
	rt->rt_gateway = gw;
	if (flags & RTF_HOST) {
		mask = 0xffffffff;
		rt->rt_dst = dst;
	} else {
		if (!((dst ^ dev->pa_addr) & dev->pa_mask)) {
			mask = dev->pa_mask;
			dst &= mask;
			if (flags & RTF_DYNAMIC) {
				kfree_s(rt, sizeof(struct rtable));
				/*printk("Dynamic route to my own net rejected\n");*/
				return;
			}
		} else
			mask = guess_mask(dst);
		rt->rt_dst = dst;
	}
	rt->rt_mask = mask;
	rt_print(rt);
	/*
	 * What we have to do is loop though this until we have
	 * found the first address which has a higher generality than
	 * the one in rt.  Then we can put rt in right before it.
	 */
	save_flags(cpuflags);
	cli();
	/* remove old route if we are getting a duplicate. */
	rp = &rt_base;
	while ((r = *rp) != NULL) {
	  	if (r->rt_dst != dst) {
  			rp = &r->rt_next;
  			continue;
	  	}
  		*rp = r->rt_next;
		kfree_s(r, sizeof(struct rtable));
	}
	/* add the new route */
	rp = &rt_base;
	while ((r = *rp) != NULL) {
		if ((r->rt_mask & mask) != mask)
			break;
		rp = &r->rt_next;
	}
	rt->rt_next = r;
	*rp = rt;
	restore_flags(cpuflags);
	return;
}


static int
rt_new(struct rtentry *r)
{
  struct device *dev;
  struct rtable *rt;

  if ((r->rt_dst.sa_family != AF_INET) ||
      (r->rt_gateway.sa_family != AF_INET)) {
	DPRINTF((DBG_RT, "RT: We only know about AF_INET !\n"));
	return(-EAFNOSUPPORT);
  }

  /*
   * I admit that the following bits of code were "inspired" by
   * the Berkeley UNIX system source code.  I could think of no
   * other way to find out how to make it compatible with it (I
   * want this to be compatible to get "routed" up and running).
   * -FvK
   */

  /* If we have a 'gateway' route here, check the correct address. */
  if (!(r->rt_flags & RTF_GATEWAY))
	dev = dev_check(((struct sockaddr_in *) &r->rt_dst)->sin_addr.s_addr);
  else
	if ((rt = rt_route(((struct sockaddr_in *) &r->rt_gateway)->sin_addr.
			   s_addr,NULL)))
	    dev = rt->rt_dev;
	else
	    dev = NULL;

  DPRINTF((DBG_RT, "RT: dev for %s gw ",
	in_ntoa((*(struct sockaddr_in *)&r->rt_dst).sin_addr.s_addr)));
  DPRINTF((DBG_RT, "%s (0x%04X) is 0x%X (%s)\n",
	in_ntoa((*(struct sockaddr_in *)&r->rt_gateway).sin_addr.s_addr),
	r->rt_flags, dev, (dev == NULL) ? "NONE" : dev->name));

  if (dev == NULL) return(-ENETUNREACH);

  rt_add(r->rt_flags, (*(struct sockaddr_in *) &r->rt_dst).sin_addr.s_addr,
	 (*(struct sockaddr_in *) &r->rt_gateway).sin_addr.s_addr, dev);

  return(0);
}


static int
rt_kill(struct rtentry *r)
{
  struct sockaddr_in *trg;

  trg = (struct sockaddr_in *) &r->rt_dst;
  rt_del(trg->sin_addr.s_addr);

  return(0);
}


/* Called from the PROCfs module. */
int
rt_get_info(char *buffer)
{
  struct rtable *r;
  char *pos;

  pos = buffer;

  pos += sprintf(pos,
		 "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\n");
  
  /* This isn't quite right -- r->rt_dst is a struct! */
  for (r = rt_base; r != NULL; r = r->rt_next) {
        pos += sprintf(pos, "%s\t%08lX\t%08lX\t%02X\t%d\t%lu\t%d\n",
		r->rt_dev->name, r->rt_dst, r->rt_gateway,
		r->rt_flags, r->rt_refcnt, r->rt_use, r->rt_metric);
  }
  return(pos - buffer);
}


/*
 * rewrote this too.. Maybe somebody can understand it now. Linus
 */
struct rtable * rt_route(unsigned long daddr, struct options *opt)
{
	struct rtable *rt;
	int type;

  /*
   * This is a hack, I think. -FvK
   */
	if ((type=chk_addr(daddr)) == IS_MYADDR) daddr = my_addr();

  /*
   * Loop over the IP routing table to find a route suitable
   * for this packet.  Note that we really should have a look
   * at the IP options to see if we have been given a hint as
   * to what kind of path we should use... -FvK
   */
  /*
   * This depends on 'rt_mask' and the ordering set up in 'rt_add()' - Linus
   */
	for (rt = rt_base; rt != NULL; rt = rt->rt_next) {
		if (!((rt->rt_dst ^ daddr) & rt->rt_mask)) {
			rt->rt_use++;
			return rt;
		}
		/* broadcast addresses can be special cases.. */
		if ((rt->rt_dev->flags & IFF_BROADCAST) &&
		     rt->rt_dev->pa_brdaddr == daddr) {
			rt->rt_use++;
			return(rt);
		}
	}
	return NULL;
}


int
rt_ioctl(unsigned int cmd, void *arg)
{
  struct device *dev;
  struct rtentry rt;
  char namebuf[32];
  int ret;
  int err;

  switch(cmd) {
	case DDIOCSDBG:
		ret = dbg_ioctl(arg, DBG_RT);
		break;
	case SIOCADDRT:
	case SIOCDELRT:
		if (!suser()) return(-EPERM);
		err=verify_area(VERIFY_READ, arg, sizeof(struct rtentry));
		if(err)
			return err;
		memcpy_fromfs(&rt, arg, sizeof(struct rtentry));
		if (rt.rt_dev) {
		    err=verify_area(VERIFY_READ, rt.rt_dev, sizeof namebuf);
		    if(err)
		    	return err;
		    memcpy_fromfs(&namebuf, rt.rt_dev, sizeof namebuf);
		    dev = dev_get(namebuf);
		    rt.rt_dev = dev;
		}
		ret = (cmd == SIOCDELRT) ? rt_kill(&rt) : rt_new(&rt);
		break;
	default:
		ret = -EINVAL;
  }

  return(ret);
}
