/*********************************************************************
 *                
 * Filename:      ircomm_param.c
 * Version:       1.0
 * Description:   Parameter handling for the IrCOMM protocol
 * Status:        Experimental.
 * Author:        Dag Brattli <dagb@cs.uit.no>
 * Created at:    Mon Jun  7 10:25:11 1999
 * Modified at:   Wed Aug 25 13:48:14 1999
 * Modified by:   Dag Brattli <dagb@cs.uit.no>
 * 
 *     Copyright (c) 1999 Dag Brattli, All Rights Reserved.
 *     
 *     This program is free software; you can redistribute it and/or 
 *     modify it under the terms of the GNU General Public License as 
 *     published by the Free Software Foundation; either version 2 of 
 *     the License, or (at your option) any later version.
 * 
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *     GNU General Public License for more details.
 * 
 *     You should have received a copy of the GNU General Public License 
 *     along with this program; if not, write to the Free Software 
 *     Foundation, Inc., 59 Temple Place, Suite 330, Boston, 
 *     MA 02111-1307 USA
 *     
 ********************************************************************/

#include <net/irda/irda.h>
#include <net/irda/parameters.h>

#include <net/irda/ircomm_core.h>
#include <net/irda/ircomm_tty_attach.h>
#include <net/irda/ircomm_tty.h>

#include <net/irda/ircomm_param.h>

static int ircomm_param_service_type(void *instance, param_t *param, int get);
static int ircomm_param_port_type(void *instance, param_t *param, int get);
static int ircomm_param_port_name(void *instance, param_t *param, int get);
static int ircomm_param_service_type(void *instance, param_t *param, int get);
static int ircomm_param_data_rate(void *instance, param_t *param, int get);
static int ircomm_param_data_format(void *instance, param_t *param, int get);
static int ircomm_param_flow_control(void *instance, param_t *param, int get);
static int ircomm_param_xon_xoff(void *instance, param_t *param, int get);
static int ircomm_param_enq_ack(void *instance, param_t *param, int get);
static int ircomm_param_line_status(void *instance, param_t *param, int get);
static int ircomm_param_dte(void *instance, param_t *param, int get);
static int ircomm_param_dce(void *instance, param_t *param, int get);
static int ircomm_param_poll(void *instance, param_t *param, int get);

static pi_minor_info_t pi_minor_call_table_common[] = {
	{ ircomm_param_service_type, PV_INT_8_BITS },
	{ ircomm_param_port_type,    PV_INT_8_BITS },
	{ ircomm_param_port_name,    PV_STRING }
};
static pi_minor_info_t pi_minor_call_table_non_raw[] = {
	{ ircomm_param_data_rate,    PV_INT_32_BITS | PV_BIG_ENDIAN },
	{ ircomm_param_data_format,  PV_INT_8_BITS },
	{ ircomm_param_flow_control, PV_INT_8_BITS },
	{ ircomm_param_xon_xoff,     PV_INT_16_BITS },
	{ ircomm_param_enq_ack,      PV_INT_16_BITS },
	{ ircomm_param_line_status,  PV_INT_8_BITS }
};
static pi_minor_info_t pi_minor_call_table_9_wire[] = {
	{ ircomm_param_dte,          PV_INT_8_BITS },
	{ ircomm_param_dce,          PV_INT_8_BITS },
	{ ircomm_param_poll,         PV_INT_8_BITS },
};

static pi_major_info_t pi_major_call_table[] = {
	{ pi_minor_call_table_common,  3 },
	{ pi_minor_call_table_non_raw, 6 },
 	{ pi_minor_call_table_9_wire,  3 }
/* 	{ pi_minor_call_table_centronics }  */
};

pi_param_info_t ircomm_param_info = { pi_major_call_table, 3, 0x0f, 4 };

/*
 * Function ircomm_param_flush (self)
 *
 *    Flush (send) out all queued parameters
 *
 */
int ircomm_param_flush(struct ircomm_tty_cb *self)
{
	if (self->ctrl_skb) {
		ircomm_control_request(self->ircomm, self->ctrl_skb);
		self->ctrl_skb = NULL;	
	}
	return 0;
}

/*
 * Function ircomm_param_request (self, pi, flush)
 *
 *    Queue a parameter for the control channel
 *
 */
int ircomm_param_request(struct ircomm_tty_cb *self, __u8 pi, int flush)
{
	struct sk_buff *skb;
	int count;

	DEBUG(2, __FUNCTION__ "()\n");

	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return -1;);

	if (self->state != IRCOMM_TTY_READY) {
		DEBUG(2, __FUNCTION__ "(), not ready yet!\n");
		return 0;
	}

	/* Make sure we don't send parameters for raw mode */
	if (self->service_type == IRCOMM_3_WIRE_RAW)
		return 0;

	skb = self->ctrl_skb;
	
	if (!skb) {
		skb = dev_alloc_skb(256);
		if (!skb)
			return -1;
		
		skb_reserve(skb, self->max_header_size);

		self->ctrl_skb = skb;		
	}
	/* 
	 * Inserting is a little bit tricky since we don't know how much
	 * room we will need. But this should hopefully work OK 
	 */
	count = irda_param_insert(self, pi, skb->tail, skb_tailroom(skb),
				  &ircomm_param_info);
	if (count > 0)
		skb_put(skb, count);

	if (flush) {
		ircomm_control_request(self->ircomm, skb);
		self->ctrl_skb = NULL;		
	}
	return count;
}

/*
 * Function ircomm_param_service_type (self, buf, len)
 *
 *    Handle service type, this function will both be called after the LM-IAS
 *    query and then the remote device sends its initial paramters
 *
 */
static int ircomm_param_service_type(void *instance, param_t *param, int get)
{
	struct ircomm_tty_cb *self = (struct ircomm_tty_cb *) instance;
	__u8 service_type = param->pv.b; /* We know it's a one byte integer */

	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return -1;);

	if (get) {
		param->pv.b = self->session.service_type;
		return 0;
	}

	/*
	 * Now choose a preferred service type of those available
	 */
	if (service_type & IRCOMM_3_WIRE_RAW) {
		DEBUG(2, __FUNCTION__ "(), peer supports 3 wire raw\n");
		self->session.service_type |= IRCOMM_3_WIRE_RAW;
	}
	if (service_type & IRCOMM_3_WIRE) {
		DEBUG(2, __FUNCTION__ "(), peer supports 3 wire\n");
		self->session.service_type |= IRCOMM_3_WIRE;
	}
	if (service_type & IRCOMM_9_WIRE) {
		DEBUG(2, __FUNCTION__ "(), peer supports 9 wire\n");
		self->session.service_type |= IRCOMM_9_WIRE;
	}
	if (service_type & IRCOMM_CENTRONICS) {
		DEBUG(2, __FUNCTION__ "(), peer supports Centronics\n");
		self->session.service_type |= IRCOMM_CENTRONICS;
	}
	
	self->session.service_type &= self->service_type;
	if (!self->session.service_type) {
		DEBUG(2, __FUNCTION__"(), No common service type to use!\n");
		return -1;
	}
	
	DEBUG(2, __FUNCTION__ "(), resulting service type=0x%02x\n", 
	      self->session.service_type);

	return 0;
}

/*
 * Function ircomm_param_port_type (self, param)
 *
 *    
 *
 */
static int ircomm_param_port_type(void *instance, param_t *param, int get)
{
	DEBUG(0, __FUNCTION__ "(), not impl.\n");

	return 0;
}

/*
 * Function ircomm_param_port_name (self, param)
 *
 *    
 *
 */
static int ircomm_param_port_name(void *instance, param_t *param, int get)
{
	struct ircomm_tty_cb *self = (struct ircomm_tty_cb *) instance;
	
	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return -1;);
	
	if (get)
		DEBUG(0, __FUNCTION__ "(), not imp!\n");
	else
		DEBUG(0, __FUNCTION__ "(), port-name=%s\n", param->pv.c);

	return 0;
}

/*
 * Function ircomm_param_data_rate (self, param)
 *
 *    
 *
 */
static int ircomm_param_data_rate(void *instance, param_t *param, int get)
{
	struct ircomm_tty_cb *self = (struct ircomm_tty_cb *) instance;
	
	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return -1;);

	if (get)
		param->pv.i = self->session.data_rate;
	else
		self->session.data_rate = param->pv.i;
	
	DEBUG(2, __FUNCTION__ "(), data rate = %d\n", param->pv.i);

	return 0;
}

/*
 * Function ircomm_param_data_format (self, param)
 *
 *    
 *
 */
static int ircomm_param_data_format(void *instance, param_t *param, int get)
{
	struct ircomm_tty_cb *self = (struct ircomm_tty_cb *) instance;

	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return -1;);

	if (get)
		param->pv.b = self->session.data_format;
	else
		self->session.data_format = param->pv.b;
	
	DEBUG(1, __FUNCTION__ "(), data format = 0x%02x\n", param->pv.b);

	return 0;
}

/*
 * Function ircomm_param_flow_control (self, param)
 *
 *    
 *
 */
static int ircomm_param_flow_control(void *instance, param_t *param, int get)
{
	struct ircomm_tty_cb *self = (struct ircomm_tty_cb *) instance;

	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return -1;);
	
	if (get)
		param->pv.b = self->session.flow_control;
	else
		self->session.flow_control = param->pv.b;

	DEBUG(1, __FUNCTION__ "(), flow control = 0x%02x\n", param->pv.b);

	return 0;
}

/*
 * Function ircomm_param_xon_xoff (self, param)
 *
 *    
 *
 */
static int ircomm_param_xon_xoff(void *instance, param_t *param, int get)
{
	DEBUG(2, __FUNCTION__ "(), not impl.\n");

	return 0;
}

/*
 * Function ircomm_param_enq_ack (self, param)
 *
 *    
 *
 */
static int ircomm_param_enq_ack(void *instance, param_t *param, int get)
{
	DEBUG(2, __FUNCTION__ "(), not impl.\n");

	return 0;
}

/*
 * Function ircomm_param_line_status (self, param)
 *
 *    
 *
 */
static int ircomm_param_line_status(void *instance, param_t *param, int get)
{
	DEBUG(2, __FUNCTION__ "(), not impl.\n");

	return 0;
}

/*
 * Function ircomm_param_dte (instance, param)
 *
 *    If we get here, there must be some sort of null-modem connection, and
 *    we are probably working in server mode as well.
 */
static int ircomm_param_dte(void *instance, param_t *param, int get)
{
	struct ircomm_tty_cb *self = (struct ircomm_tty_cb *) instance;
	__u8 dte;

	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return -1;);

	if (get)
		param->pv.b = self->session.dte;
	else {
		dte = param->pv.b;
		
		/* Null modem cable emulator */
		self->session.null_modem = TRUE;

		if (dte & IRCOMM_DELTA_DTR)
			self->session.dce |= (IRCOMM_DELTA_DSR|
					      IRCOMM_DELTA_RI |
					      IRCOMM_DELTA_CD);
		if (dte & IRCOMM_DTR)
			self->session.dce |= (IRCOMM_DSR|
					      IRCOMM_RI |
					      IRCOMM_CD);
		
		if (dte & IRCOMM_DELTA_RTS)
			self->session.dce |= IRCOMM_DELTA_CTS;
		if (dte & IRCOMM_RTS)
			self->session.dce |= IRCOMM_CTS;

		/* Take appropriate actions */
		ircomm_tty_check_modem_status(self);

		/* 
		 * Send reply, and remember not to set delta values for the
		 * initial parameters 
		 */
		self->session.dte = (IRCOMM_DTR| IRCOMM_RTS);
		ircomm_param_request(self, IRCOMM_DTE, TRUE);
	}
	DEBUG(1, __FUNCTION__ "(), dte = 0x%02x\n", param->pv.b);

	return 0;
}

/*
 * Function ircomm_param_dce (instance, param)
 *
 *    
 *
 */
static int ircomm_param_dce(void *instance, param_t *param, int get)
{
	struct ircomm_tty_cb *self = (struct ircomm_tty_cb *) instance;
	__u8 dce;

	DEBUG(1, __FUNCTION__ "(), dce = 0x%02x\n", param->pv.b);

	dce = param->pv.b;

	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return -1;);

	self->session.dce = dce;

	/* Check if any of the settings have changed */
	if (dce & 0x0f) {
		if (dce & IRCOMM_DELTA_CTS) {
			DEBUG(2, __FUNCTION__ "(), CTS \n");
		}
	}

	ircomm_tty_check_modem_status(self);

	return 0;
}

/*
 * Function ircomm_param_poll (instance, param)
 *
 *    
 *
 */
static int ircomm_param_poll(void *instance, param_t *param, int get)
{
	DEBUG(0, __FUNCTION__ "(), not impl.\n");

	return 0;
}








