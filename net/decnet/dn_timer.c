/*
 * DECnet       An implementation of the DECnet protocol suite for the LINUX
 *              operating system.  DECnet is implemented using the  BSD Socket
 *              interface as the means of communication with the user level.
 *
 *              DECnet Socket Timer Functions
 *
 * Author:      Steve Whitehouse <SteveW@ACM.org>
 *
 *
 * Changes:
 *       Steve Whitehouse      : Made keepalive timer part of the same
 *                               timer idea.
 *       Steve Whitehouse      : Added checks for sk->sock_readers
 */
#include <linux/config.h>
#include <linux/net.h>
#include <linux/socket.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/timer.h>
#include <net/sock.h>
#include <asm/spinlock.h>
#include <asm/atomic.h>
#include <net/dn.h>

/*
 * Fast timer is for delayed acks (200mS max)
 * Slow timer is for everything else (n * 500mS)
 */

#define FAST_INTERVAL (HZ/5)
#define SLOW_INTERVAL (HZ/2)

static void dn_slow_timer(unsigned long arg);

void dn_start_slow_timer(struct sock *sk)
{
	sk->timer.expires = jiffies + SLOW_INTERVAL;
	sk->timer.function = dn_slow_timer;
	sk->timer.data = (unsigned long)sk;

	add_timer(&sk->timer);
}

void dn_stop_slow_timer(struct sock *sk)
{
	unsigned long cpuflags;

	save_flags(cpuflags);
	cli();
	del_timer(&sk->timer);
	restore_flags(cpuflags);
}

static void dn_slow_timer(unsigned long arg)
{
	struct sock *sk = (struct sock *)arg;
	struct dn_scp *scp = &sk->protinfo.dn;

	if (atomic_read(&sk->sock_readers)) {
		sk->timer.expires = jiffies + HZ / 10;
		add_timer(&sk->timer);
		return;
	}

	/*
	 * The persist timer is the standard slow timer used for retransmits
	 * in both connection establishment and disconnection as well as
	 * in the RUN state. The different states are catered for by changing
	 * the function pointer in the socket. Setting the timer to a value
	 * of zero turns it off. We allow the persist_fxn to turn the
	 * timer off in a permant way by returning non-zero, so that
	 * timer based routines may remove sockets.
	 */
	if (scp->persist && scp->persist_fxn) {
		if (scp->persist <= SLOW_INTERVAL) {
			scp->persist = 0;

			if (scp->persist_fxn(sk))
				return;
		} else {
			scp->persist -= SLOW_INTERVAL;
		}
	}

	/*
	 * Check for keepalive timeout. After the other timer 'cos if
	 * the previous timer caused a retransmit, we don't need to
	 * do this. scp->stamp is the last time that we sent a packet.
	 * The keepalive function sends a link service packet to the
	 * other end. If it remains unacknowledged, the standard
	 * socket timers will eventually shut the socket down. Each
	 * time we do this, scp->stamp will be updated, thus
	 * we won't try and send another until scp->keepalive has passed
	 * since the last successful transmission.
	 */
	if (scp->keepalive && scp->keepalive_fxn && (scp->state == DN_RUN)) {
		if ((jiffies - scp->stamp) >= scp->keepalive)
			scp->keepalive_fxn(sk);
	}

	sk->timer.expires = jiffies + SLOW_INTERVAL;

	add_timer(&sk->timer);
}

static void dn_fast_timer(unsigned long arg)
{
	struct sock *sk = (struct sock *)arg;
	struct dn_scp *scp = &sk->protinfo.dn;

	if (atomic_read(&sk->sock_readers)) {
		scp->delack_timer.expires = jiffies + HZ / 20;
		add_timer(&scp->delack_timer);
		return;
	}

	scp->delack_pending = 0;

	if (scp->delack_fxn)
		scp->delack_fxn(sk);
}

void dn_start_fast_timer(struct sock *sk)
{
	struct dn_scp *scp = &sk->protinfo.dn;
	unsigned long cpuflags;

	save_flags(cpuflags);
	cli();
	if (!scp->delack_pending) {
		scp->delack_pending = 1;
		scp->delack_timer.next =
		scp->delack_timer.prev = NULL;
		scp->delack_timer.expires = jiffies + FAST_INTERVAL;
		scp->delack_timer.data = (unsigned long)sk;
		scp->delack_timer.function = dn_fast_timer;
		add_timer(&scp->delack_timer);
	}
	restore_flags(cpuflags);
}

void dn_stop_fast_timer(struct sock *sk)
{
	struct dn_scp *scp = &sk->protinfo.dn;
	unsigned long cpuflags;

	save_flags(cpuflags);
	cli();
	if (scp->delack_pending) {
		scp->delack_pending = 0;
		del_timer(&scp->delack_timer);
	}
	restore_flags(cpuflags);
}

