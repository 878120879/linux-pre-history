#ifndef _ALPHA_TERMIOS_H
#define _ALPHA_TERMIOS_H

#include <asm/ioctls.h>
#include <asm/termbits.h>

struct sgttyb {
	char	sg_ispeed;
	char	sg_ospeed;
	char	sg_erase;
	char	sg_kill;
	short	sg_flags;
};

struct tchars {
	char	t_intrc;
	char	t_quitc;
	char	t_startc;
	char	t_stopc;
	char	t_eofc;
	char	t_brkc;
};

struct ltchars {
	char	t_suspc;
	char	t_dsuspc;
	char	t_rprntc;
	char	t_flushc;
	char	t_werasc;
	char	t_lnextc;
};

struct winsize {
	unsigned short ws_row;
	unsigned short ws_col;
	unsigned short ws_xpixel;
	unsigned short ws_ypixel;
};

#define NCC 8
struct termio {
	unsigned short c_iflag;		/* input mode flags */
	unsigned short c_oflag;		/* output mode flags */
	unsigned short c_cflag;		/* control mode flags */
	unsigned short c_lflag;		/* local mode flags */
	unsigned char c_line;		/* line discipline */
	unsigned char c_cc[NCC];	/* control characters */
};

/*
 * c_cc characters in the termio structure.  Oh, how I love being
 * backwardly compatible.  Notice that character 4 and 5 are
 * interpreted differently depending on whether ICANON is set in
 * c_lflag.  If it's set, they are used as _VEOF and _VEOL, otherwise
 * as _VMIN and V_TIME.  This is for compatibility with OSF/1 (which
 * is compatible with sysV)...
 */
#define _VINTR	0
#define _VQUIT	1
#define _VERASE	2
#define _VKILL	3
#define _VEOF	4
#define _VMIN	4
#define _VEOL	5
#define _VTIME	5
#define _VEOL2	6
#define _VSWTC	7

/* line disciplines */
#define N_TTY		0
#define N_SLIP		1
#define N_MOUSE		2
#define N_PPP		3
#define N_AX25		5

#ifdef __KERNEL__
/*	eof=^D		eol=\0		eol2=\0		erase=del
	werase=^W	kill=^U		reprint=^R	sxtc=\0
	intr=^C		quit=^\		susp=^Z		<OSF/1 VDSUSP>
	start=^Q	stop=^S		lnext=^V	discard=^U
	vmin=\1		vtime=\0
*/
#define INIT_C_CC "\004\000\000\177\027\025\022\000\003\034\032\000\021\023\026\025\001\000"

/*
 * Translate a "termio" structure into a "termios". Ugh.
 */
#define SET_LOW_TERMIOS_BITS(termios, termio, x) { \
	unsigned short __tmp; \
	get_user(__tmp,&(termio)->x); \
	*(unsigned short *) &(termios)->x = __tmp; \
}

#define user_termio_to_kernel_termios(termios, termio) \
do { \
	SET_LOW_TERMIOS_BITS(termios, termio, c_iflag); \
	SET_LOW_TERMIOS_BITS(termios, termio, c_oflag); \
	SET_LOW_TERMIOS_BITS(termios, termio, c_cflag); \
	SET_LOW_TERMIOS_BITS(termios, termio, c_lflag); \
	get_user((termios)->c_cc[VINTR], &(termio)->c_cc[_VINTR]); \
	get_user((termios)->c_cc[VQUIT], &(termio)->c_cc[_VQUIT]); \
	get_user((termios)->c_cc[VERASE], &(termio)->c_cc[_VERASE]); \
	get_user((termios)->c_cc[VKILL], &(termio)->c_cc[_VKILL]); \
	get_user((termios)->c_cc[VEOF], &(termio)->c_cc[_VEOF]); \
	get_user((termios)->c_cc[VMIN], &(termio)->c_cc[_VMIN]); \
	get_user((termios)->c_cc[VEOL], &(termio)->c_cc[_VEOL]); \
	get_user((termios)->c_cc[VTIME], &(termio)->c_cc[_VTIME]); \
	get_user((termios)->c_cc[VEOL2], &(termio)->c_cc[_VEOL2]); \
	get_user((termios)->c_cc[VSWTC], &(termio)->c_cc[_VSWTC]); \
} while(0)

/*
 * Translate a "termios" structure into a "termio". Ugh.
 *
 * Note the "fun" _VMIN overloading.
 */
#define kernel_termios_to_user_termio(termio, termios) \
do { \
	put_user((termios)->c_iflag, &(termio)->c_iflag); \
	put_user((termios)->c_oflag, &(termio)->c_oflag); \
	put_user((termios)->c_cflag, &(termio)->c_cflag); \
	put_user((termios)->c_lflag, &(termio)->c_lflag); \
	put_user((termios)->c_line, &(termio)->c_line); \
	put_user((termios)->c_cc[VINTR], &(termio)->c_cc[_VINTR]); \
	put_user((termios)->c_cc[VQUIT], &(termio)->c_cc[_VQUIT]); \
	put_user((termios)->c_cc[VERASE], &(termio)->c_cc[_VERASE]); \
	put_user((termios)->c_cc[VKILL], &(termio)->c_cc[_VKILL]); \
	put_user((termios)->c_cc[VEOF], &(termio)->c_cc[_VEOF]); \
	put_user((termios)->c_cc[VEOL], &(termio)->c_cc[_VEOL]); \
	put_user((termios)->c_cc[VEOL2], &(termio)->c_cc[_VEOL2]); \
	put_user((termios)->c_cc[VSWTC], &(termio)->c_cc[_VSWTC]); \
	if (!((termios)->c_lflag & ICANON)) { \
		put_user((termios)->c_cc[VMIN], &(termio)->c_cc[_VMIN]); \
		put_user((termios)->c_cc[VTIME], &(termio)->c_cc[_VTIME]); \
	} \
} while(0)

#define user_termios_to_kernel_termios(k, u) copy_from_user(k, u, sizeof(struct termios))
#define kernel_termios_to_user_termios(u, k) copy_to_user(u, k, sizeof(struct termios))

#endif	/* __KERNEL__ */

#endif	/* _ALPHA_TERMIOS_H */
