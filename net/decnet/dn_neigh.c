/*
 * DECnet       An implementation of the DECnet protocol suite for the LINUX
 *              operating system.  DECnet is implemented using the  BSD Socket
 *              interface as the means of communication with the user level.
 *
 *              DECnet Neighbour Functions (Adjacency Database and 
 *                                                        On-Ethernet Cache)
 *
 * Author:      Steve Whitehouse <SteveW@ACM.org>
 *
 *
 * Changes:
 *     Steve Whitehouse     : Fixed router listing routine
 *     Steve Whitehouse     : Added error_report functions
 *     Steve Whitehouse     : Added default router detection
 *     Steve Whitehouse     : Hop counts in outgoing messages
 *     Steve Whitehouse     : Fixed src/dst in outgoing messages so
 *                            forwarding now stands a good chance of
 *                            working.
 *     Steve Whitehouse     : Fixed neighbour states (for now anyway).
 *
 */

#include <linux/config.h>
#include <linux/net.h>
#include <linux/socket.h>
#include <linux/if_arp.h>
#include <linux/if_ether.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/string.h>
#include <asm/atomic.h>
#include <asm/spinlock.h>
#include <net/neighbour.h>
#include <net/dst.h>
#include <net/dn.h>
#include <net/dn_dev.h>
#include <net/dn_neigh.h>
#include <net/dn_route.h>

static int dn_neigh_construct(struct neighbour *);
static void dn_long_error_report(struct neighbour *, struct sk_buff *);
static void dn_short_error_report(struct neighbour *, struct sk_buff *);
static int dn_long_output(struct sk_buff *);
static int dn_short_output(struct sk_buff *);
static int dn_phase3_output(struct sk_buff *);


/*
 * For talking to broadcast devices: Ethernet & PPP
 */
static struct neigh_ops dn_long_ops = {
	AF_DECnet,
	NULL,
	NULL,
	dn_long_error_report,
	dn_long_output,
	dn_long_output,
	dev_queue_xmit,
	dev_queue_xmit
};

/*
 * For talking to pointopoint and multidrop devices: DDCMP and X.25
 */
static struct neigh_ops dn_short_ops = {
	AF_DECnet,
	NULL,
	NULL,
	dn_short_error_report,
	dn_short_output,
	dn_short_output,
	dev_queue_xmit,
	dev_queue_xmit
};

/*
 * For talking to DECnet phase III nodes
 */
static struct neigh_ops dn_phase3_ops = {
	AF_DECnet,
	NULL,
	NULL,
	dn_short_error_report, /* Can use short version here */
	dn_phase3_output,
	dn_phase3_output,
	dev_queue_xmit,
	dev_queue_xmit
};

struct neigh_table dn_neigh_table = {
	NULL,
	PF_DECnet,
	sizeof(struct dn_neigh),
	ETH_ALEN,
	dn_neigh_construct,
	NULL, /* pconstructor */
	NULL, /* pdestructor */
	NULL, /* proxyredo */
	{ 
		NULL,
		NULL,
		&dn_neigh_table,
		0,
		NULL,
		NULL,
		30 * HZ,	/* base_reachable_time */
		1 * HZ,		/* retrans_time */
		60 * HZ,	/* gc_staletime */
		30 * HZ,	/* reachable_time */
		5 * HZ,		/* delay_probe_time */
		3,	/* queue_len */
		0,	/* ucast_probes */
		0,	/* app_probes */
		0,	/* mcast_probes */
		0,	/* anycast_delay */
		0,	/* proxy_delay */
		0,	/* proxy_qlen */
		1 * HZ,	/* locktime */
	},
	30 * HZ,	/* gc_interval */
	128,		/* gc_thresh1 */
	512,		/* gc_thresh2 */
	1024,		/* gc_thresh3 */
	
};

static int dn_neigh_construct(struct neighbour *neigh)
{
	struct device *dev = neigh->dev;
	struct dn_neigh *dn = (struct dn_neigh *)neigh;
	struct dn_dev *dn_db = (struct dn_dev *)dev->dn_ptr;

	if (dn_db == NULL)
		return -EINVAL;

	if (dn_db->neigh_parms)
		neigh->parms = dn_db->neigh_parms;

	if (dn_db->use_long)
		neigh->ops = &dn_long_ops;
	else
		neigh->ops = &dn_short_ops;

	if (dn->flags & DN_NDFLAG_P3)
		neigh->ops = &dn_phase3_ops;

	neigh->nud_state = NUD_NOARP;
	neigh->output = neigh->ops->connected_output;

	dn->blksize = 230;

	return 0;
}

static void dn_long_error_report(struct neighbour *neigh, struct sk_buff *skb)
{
	struct dn_skb_cb *cb = (struct dn_skb_cb *)skb->cb;
	unsigned char *ptr;

	printk(KERN_DEBUG "dn_long_error_report: called\n");

	if (!(cb->rt_flags & DN_RT_F_RQR)) {
		kfree_skb(skb);
		return;
	}

	skb_push(skb, skb->data - skb->nh.raw);
	ptr = skb->data;

	*(unsigned short *)ptr = dn_htons(skb->len - 2);
	ptr += 2;

	if (*ptr & DN_RT_F_PF) {
		char padlen = (*ptr & ~DN_RT_F_PF);
		ptr += padlen;
	}

	*ptr++ |= (cb->rt_flags & ~DN_RT_F_RQR) | DN_RT_F_RTS;

	ptr += 2;
	dn_dn2eth(ptr, dn_ntohs(cb->src));
	ptr += 8;
	dn_dn2eth(ptr, dn_ntohs(cb->dst));
	ptr += 6;
	*ptr = 0;

	skb->dst->neighbour->ops->queue_xmit(skb);
}


static void dn_short_error_report(struct neighbour *neigh, struct sk_buff *skb)
{
	struct dn_skb_cb *cb = (struct dn_skb_cb *)skb->cb;
	unsigned char *ptr;

	printk(KERN_DEBUG "dn_short_error_report: called\n");

	if (!(cb->rt_flags & DN_RT_F_RQR)) {
		kfree_skb(skb);
		return;
	}

	skb_push(skb, skb->data - skb->nh.raw);
	ptr = skb->data;

	*(unsigned short *)ptr = dn_htons(skb->len - 2);
	ptr += 2;
	*ptr++ = (cb->rt_flags & ~DN_RT_F_RQR) | DN_RT_F_RTS;

	*(dn_address *)ptr = cb->src;
	ptr += 2;
	*(dn_address *)ptr = cb->dst;
	ptr += 2;
	*ptr = 0;

	skb->dst->neighbour->ops->queue_xmit(skb);
}


static int dn_long_output(struct sk_buff *skb)
{
	struct dst_entry *dst = skb->dst;
	struct neighbour *neigh = dst->neighbour;
	struct device *dev = neigh->dev;
	struct dn_dev *dn_db = dev->dn_ptr;
	int headroom = dev->hard_header_len + sizeof(struct dn_long_packet) + 3;
	unsigned char *data;
	struct dn_long_packet *lp;
	struct dn_skb_cb *cb = (struct dn_skb_cb *)skb->cb;


	if (skb_headroom(skb) < headroom) {
		struct sk_buff *skb2 = skb_realloc_headroom(skb, headroom);
		if (skb2 == NULL) {
			if (net_ratelimit())
				printk(KERN_CRIT "dn_long_output: no memory\n");
			kfree_skb(skb);
			return -ENOBUFS;
		}
		kfree_skb(skb);
		skb = skb2;
		if (net_ratelimit())
			printk(KERN_INFO "dn_long_output: Increasing headroom\n");
	}

	data = skb_push(skb, sizeof(struct dn_long_packet) + 3);
	lp = (struct dn_long_packet *)(data+3);

	*((unsigned short *)data) = dn_htons(skb->len - 2);
	*(data + 2) = 1 | DN_RT_F_PF; /* Padding */

	lp->msgflg   = DN_RT_PKT_LONG|(cb->rt_flags&(DN_RT_F_IE|DN_RT_F_RQR|DN_RT_F_RTS));
	lp->d_area   = lp->d_subarea = 0;
	dn_dn2eth(lp->d_id, cb->dst);
	lp->s_area   = lp->s_subarea = 0;
	dn_dn2eth(lp->s_id, cb->src);
	lp->nl2      = 0;
	lp->visit_ct = cb->hops & 0x3f;
	lp->s_class  = 0;
	lp->pt       = 0;

	skb->nh.raw = skb->data;

	if (dev->hard_header(skb, dev, ntohs(skb->protocol), neigh->ha,
			dn_db->addr, skb->len) >= 0)
		return neigh->ops->queue_xmit(skb);

	if (net_ratelimit())
		printk(KERN_DEBUG "dn_long_output: oops, can't sent packet\n");

	kfree_skb(skb);
	return -EINVAL;
}

static int dn_short_output(struct sk_buff *skb)
{
	struct dst_entry *dst = skb->dst;
	struct neighbour *neigh = dst->neighbour;
	struct device *dev = neigh->dev;
	int headroom = dev->hard_header_len + sizeof(struct dn_short_packet) + 2;
	struct dn_short_packet *sp;
	unsigned char *data;
	struct dn_skb_cb *cb = (struct dn_skb_cb *)skb->cb;


        if (skb_headroom(skb) < headroom) {
                struct sk_buff *skb2 = skb_realloc_headroom(skb, headroom);
                if (skb2 == NULL) {
			if (net_ratelimit())
                        	printk(KERN_CRIT "dn_short_output: no memory\n");
                        kfree_skb(skb);
                        return -ENOBUFS;
                }
                kfree_skb(skb);
                skb = skb2;
		if (net_ratelimit())
                	printk(KERN_INFO "dn_short_output: Increasing headroom\n");
        }

	data = skb_push(skb, sizeof(struct dn_short_packet) + 2);
	*((unsigned short *)data) = dn_htons(skb->len - 2);
	sp = (struct dn_short_packet *)(data+2);

	sp->msgflg     = DN_RT_PKT_SHORT|(cb->rt_flags&(DN_RT_F_RQR|DN_RT_F_RTS));
	sp->dstnode    = cb->dst;
	sp->srcnode    = cb->src;
	sp->forward    = cb->hops & 0x3f;

	skb->nh.raw = skb->data;

	if (dev->hard_header(skb, dev, ntohs(skb->protocol), neigh->ha,
			NULL, skb->len) >= 0)
		return neigh->ops->queue_xmit(skb);

	kfree_skb(skb);
	return -EINVAL;
}

/*
 * Phase 3 output is the same is short output, execpt that
 * it clears the area bits before transmission.
 */
static int dn_phase3_output(struct sk_buff *skb)
{
	struct dst_entry *dst = skb->dst;
	struct neighbour *neigh = dst->neighbour;
	struct device *dev = neigh->dev;
	int headroom = dev->hard_header_len + sizeof(struct dn_short_packet) + 2;
	struct dn_short_packet *sp;
	unsigned char *data;
	struct dn_skb_cb *cb = (struct dn_skb_cb *)skb->cb;

	if (skb_headroom(skb) < headroom) {
		struct sk_buff *skb2 = skb_realloc_headroom(skb, headroom);
		if (skb2 == NULL) {
			if (net_ratelimit())
				printk(KERN_CRIT "dn_phase3_output: no memory\n");
			kfree_skb(skb);
			return -ENOBUFS;
		}
		kfree_skb(skb);
		skb = skb2;
		if (net_ratelimit())
			printk(KERN_INFO "dn_phase3_output: Increasing headroom\n");
	}

	data = skb_push(skb, sizeof(struct dn_short_packet) + 2);
	((unsigned short *)data) = dn_htons(skb->len - 2);
	sp = (struct dn_short_packet *)(data + 2);

	sp->msgflg   = DN_RT_PKT_SHORT|(cb->rt_flags&(DN_RT_F_RQR|DN_RT_F_RTS));
	sp->dstnode  = cb->dst & __constant_htons(0x03ff);
	sp->srcnode  = cb->src & __constant_htons(0x03ff);
	sp->forward  = cb->hops & 0x3f;

	skb->nh.raw = skb->data;

	if (dev->hard_header(skb, dev, ntohs(skb->protocol), neigh->ha,
			NULL, skb->len) >= 0)
		return neigh->ops->queue_xmit(skb);

	kfree_skb(skb);
	return -EINVAL;
}

/*
 * Unfortunately, the neighbour code uses the device in its hash
 * function, so we don't get any advantage from it. This function
 * basically does a neigh_lookup(), but without comparing the device
 * field. This is required for the On-Ethernet cache
 */
struct neighbour *dn_neigh_lookup(struct neigh_table *tbl, void *ptr)
{
	int i;
	struct neighbour *neigh;

	start_bh_atomic();
	for(i = 0; i < NEIGH_HASHMASK; i++) {
		for(neigh = tbl->hash_buckets[i]; neigh != NULL; neigh = neigh->next) {
			if (memcmp(neigh->primary_key, ptr, ETH_ALEN) == 0) {
				atomic_inc(&neigh->refcnt);
				end_bh_atomic();
				return neigh;
			}
		}
	}
	end_bh_atomic();

	return NULL;
}


/*
 * Any traffic on a pointopoint link causes the timer to be reset
 * for the entry in the neighbour table.
 */
void dn_neigh_pointopoint_notify(struct sk_buff *skb)
{
	return;
}

/*
 * Pointopoint link receives a hello message
 */
void dn_neigh_pointopoint_hello(struct sk_buff *skb)
{
	kfree_skb(skb);
}

/*
 * Ethernet router hello message received
 */
void dn_neigh_router_hello(struct sk_buff *skb)
{
	struct rtnode_hello_message *msg = (struct rtnode_hello_message *)skb->data;

	struct neighbour *neigh;
	struct dn_neigh *dn;
	struct dn_dev *dn_db;

	start_bh_atomic();
	neigh = __neigh_lookup(&dn_neigh_table, msg->id, skb->dev, 1);
	end_bh_atomic();

	dn = (struct dn_neigh *)neigh;

	if (neigh) {
		neigh_update(neigh, msg->id, NUD_NOARP, 1, 0);
		neigh->used = jiffies;

		dn_db = (struct dn_dev *)neigh->dev->dn_ptr;

		dn->blksize  = dn_ntohs(msg->blksize);
		dn->priority = msg->priority;

		dn->flags &= ~DN_NDFLAG_P3;

		switch(msg->iinfo & DN_RT_INFO_TYPE) {
			case DN_RT_INFO_L1RT:
				dn->flags &=~DN_NDFLAG_R2;
				dn->flags |= DN_NDFLAG_R1;
			case DN_RT_INFO_L2RT:
				dn->flags |= DN_NDFLAG_R2;
		}

		if (!dn_db->router) {
			dn_db->router = neigh_clone(neigh);
		} else {
			if (msg->priority > ((struct dn_neigh *)dn_db->router)->priority)
				neigh_release(xchg(&dn_db->router, neigh_clone(neigh)));
		}

		neigh_release(neigh);
	}

	kfree_skb(skb);
}

/*
 * Endnode hello message received
 */
void dn_neigh_endnode_hello(struct sk_buff *skb)
{
	struct endnode_hello_message *msg = (struct endnode_hello_message *)skb->data;
	struct neighbour *neigh;
	struct dn_neigh *dn;

	start_bh_atomic();
	neigh = __neigh_lookup(&dn_neigh_table, msg->id, skb->dev, 1);
	end_bh_atomic();

	dn = (struct dn_neigh *)neigh;

	if (neigh) {
		neigh_update(neigh, msg->id, NUD_NOARP, 1, 0);
		neigh->used = jiffies;

		dn->flags   &= ~(DN_NDFLAG_R1 | DN_NDFLAG_R2);
		dn->blksize  = dn_ntohs(msg->blksize);
		dn->priority = 0;

		neigh_release(neigh);
	}

	kfree_skb(skb);
}


#ifdef CONFIG_DECNET_ROUTER
static char *dn_find_slot(char *base, int max, int priority)
{
	int i;
	unsigned char *min = NULL;

	base += 6; /* skip first id */

	for(i = 0; i < max; i++) {
		if (!min || (*base < *min))
			min = base;
		base += 7; /* find next priority */
	}

	if (!min)
		return NULL;

	return (*min < priority) ? (min - 6) : NULL;
}

int dn_neigh_elist(struct device *dev, unsigned char *ptr, int n)
{
	int t = 0;
	int i;
	struct neighbour *neigh;
	struct dn_neigh *dn;
	struct neigh_table *tbl = &dn_neigh_table;
	unsigned char *rs = ptr;

	start_bh_atomic();

	for(i = 0; i < NEIGH_HASHMASK; i++) {
		for(neigh = tbl->hash_buckets[i]; neigh != NULL; neigh = neigh->next) {
			if (neigh->dev != dev)
				continue;
			dn = (struct dn_neigh *)neigh;
			if (!(dn->flags & (DN_NDFLAG_R1|DN_NDFLAG_R2)))
				continue;
			if (decnet_node_type == DN_RT_INFO_L1RT && (dn->flags & DN_NDFLAG_R2))
				continue;
			if (t == n)
				rs = dn_find_slot(ptr, n, dn->priority);
			else
				t++;
			if (rs == NULL)
				continue;
			memcpy(rs, dn->addr, ETH_ALEN);
			rs += 6;
			*rs = neigh->nud_state & NUD_CONNECTED ? 0x80 : 0x0;
			*rs |= dn->priority;
			rs++;
		}
	}

	end_bh_atomic();

	return t;
}
#endif /* CONFIG_DECNET_ROUTER */



#ifdef CONFIG_PROC_FS
int dn_neigh_get_info(char *buffer, char **start, off_t offset, int length, int dummy)
{
        int len     = 0;
        off_t pos   = 0;
        off_t begin = 0;
	struct neighbour *n;
	int i;
	char buf[DN_ASCBUF_LEN];

	len += sprintf(buffer + len, "Addr    Flags State Use Blksize Dev\n");

	neigh_table_lock(&dn_neigh_table);
	for(i=0;i <= NEIGH_HASHMASK; i++) {
		n = dn_neigh_table.hash_buckets[i];
		for(; n != NULL; n = n->next) {
			struct dn_neigh *dn = (struct dn_neigh *)n;

			len += sprintf(buffer+len, "%-7s %s%s%s   %02x    %02d  %07ld %-8s\n",
					dn_addr2asc(dn_ntohs(dn_eth2dn(dn->addr)), buf),
					(dn->flags&DN_NDFLAG_R1) ? "1" : "-",
					(dn->flags&DN_NDFLAG_R2) ? "2" : "-",
					(dn->flags&DN_NDFLAG_P3) ? "3" : "-",
					dn->n.nud_state,
					atomic_read(&dn->n.refcnt),
					dn->blksize,
					(dn->n.dev) ? dn->n.dev->name : "?");

			pos = begin + len;

                	if (pos < offset) {
                        	len = 0;
                        	begin = pos;
                	}

                	if (pos > offset + length)
                       		break;
		}
	}
	neigh_table_unlock(&dn_neigh_table);

        *start = buffer + (offset - begin);
        len   -= offset - begin;

        if (len > length) len = length;

        return len;
}

static struct proc_dir_entry proc_net_dn_neigh = {
        PROC_NET_DN_ADJ, 12, "decnet_neigh",
        S_IFREG | S_IRUGO, 1, 0, 0,
        0, &proc_net_inode_operations,
        dn_neigh_get_info
};

#endif

void __init dn_neigh_init(void)
{
	neigh_table_init(&dn_neigh_table);

#ifdef CONFIG_PROC_FS
	proc_net_register(&proc_net_dn_neigh);
#endif /* CONFIG_PROC_FS */
}

#ifdef CONFIG_DECNET_MODULE
void dn_neigh_cleanup(void)
{
#ifdef CONFIG_PROC_FS
	proc_net_unregister(PROC_NET_DN_ADJ);
#endif /* CONFIG_PROC_FS */
	neigh_table_clear(&dn_neigh_table);
}
#endif /* CONFIG_DECNET_MODULE */
