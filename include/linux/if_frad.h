/*
 * DLCI/FRAD	Definitions for Frame Relay Access Devices.  DLCI devices are
 *		created for each DLCI associated with a FRAD.  The FRAD driver
 *		is not truely a network device, but the lower level device
 *		handler.  This allows other FRAD manufacturers to use the DLCI
 *		code, including it's RFC1490 encapsulation along side the current
 *		implementation for the Sangoma cards.
 *
 * Version:	@(#)if_ifrad.h	0.10	23 Mar 96
 *
 * Author:	Mike McLagan <mike.mclagan@linux.org>
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */

#ifndef _FRAD_H_
#define _FRAD_H_

/* Stuctures and constants associated with the DLCI device driver */

#define DLCI_DEVADD	(SIOCDEVPRIVATE)
#define DLCI_DEVDEL	(SIOCDEVPRIVATE + 1)

struct dlci_add
{
   char  devname[IFNAMSIZ];
   short dlci;
};

#define DLCI_GET_CONF	(SIOCDEVPRIVATE + 2)
#define DLCI_SET_CONF	(SIOCDEVPRIVATE + 3)

/* These are related to the Sangoma FRAD */
struct dlci_conf {
   short flags;
   short CIR_fwd;
   short Bc_fwd;
   short Be_fwd;
   short CIR_bwd;
   short Bc_bwd;
   short Be_bwd; 

/* these are part of the status read */
   short Tc_fwd;
   short Tc_bwd;
   short Tf_max;
   short Tb_max;
};

#define DLCI_GET_SLAVE	(SIOCDEVPRIVATE + 4)

/* configuration flags for DLCI */
#define DLCI_IGNORE_CIR_OUT	0x0001
#define DLCI_ACCOUNT_CIR_IN	0x0002
#define DLCI_BUFFER_IF		0x0008

#define DLCI_VALID_FLAGS	0x000B


/* defines for the actual Frame Relay hardware */
#define FRAD_GET_CONF	(SIOCDEVPRIVATE)
#define FRAD_SET_CONF	(SIOCDEVPRIVATE + 1)

#define FRAD_LAST_IOCTL	FRAD_SET_CONF

struct frad_conf 
{
   short station;
   short flags;
   short kbaud;
   short clocking;
   short mtu;
   short T391;
   short T392;
   short N391;
   short N392;
   short N393;
   short CIR_fwd;
   short Bc_fwd;
   short Be_fwd;
   short CIR_bwd;
   short Bc_bwd;
   short Be_bwd;

/* Add new fields here, above is a mirror of the sangoma_conf */

};

#define FRAD_STATION_CPE	0x0000
#define FRAD_STATION_NODE	0x0001

#define FRAD_TX_IGNORE_CIR	0x0001
#define FRAD_RX_ACCOUNT_CIR	0x0002
#define FRAD_DROP_ABORTED	0x0004
#define FRAD_BUFFERIF		0x0008
#define FRAD_STATS		0x0010
#define FRAD_MCI		0x0100
#define FRAD_AUTODLCI		0x8000
#define FRAD_VALID_FLAGS	0x811F

#define FRAD_CLOCK_INT		0x0001
#define FRAD_CLOCK_EXT		0x0000

#ifdef __KERNEL__

struct fradhdr
{
   /* these are the fields of an RFC 1490 header               */
   unsigned char  control;
   unsigned char  pad;		/* for IP packets, this can be the NLPID */
   unsigned char  NLPID;
   unsigned char  OUI[3];
   unsigned short PID;
};

/* see RFC 1490 for the definition of the following */
#define FRAD_I_UI		0x03

#define FRAD_P_PADDING		0x00
#define FRAD_P_Q933		0x08
#define FRAD_P_SNAP		0x80
#define FRAD_P_CLNP		0x81
#define FRAD_P_IP		0xCC

struct dlci_local
{
   struct enet_statistics stats;
   struct device          *slave;
   struct dlci_conf       config;
   int                    configured;

   /* callback function */
   void              (*receive)(struct sk_buff *skb, struct device *);
};

struct frad_local
{
   struct enet_statistics stats;
   struct timer_list timer;

   /* devices which this FRAD is slaved to */
   struct device     *master[CONFIG_DLCI_MAX];
   short             dlci[CONFIG_DLCI_MAX];

   /* callback functions */
   int               (*activate)(struct device *, struct device *);
   int               (*deactivate)(struct device *, struct device *);
   int               (*assoc)(struct device *, struct device *);
   int               (*deassoc)(struct device *, struct device *);
   int               (*dlci_conf)(struct device *, struct device *, int get);

   int               initialized;	/* mem_start, port, irq set ? */
   int               configured;	/* has this device been configured */
   int               type;		/* adapter type */
   int               state;		/* state of the S502/8 control latch */
   int               buffer;		/* current buffer for S508 firmware */
   struct frad_conf  config;
};

int register_frad(const char *name);
int unregister_frad(const char *name);

#endif __KERNEL__

#endif
