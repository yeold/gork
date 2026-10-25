// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * glue.c -- barebox's half of gork: the `gork` command, the calls of bb.h
 * on barebox's own, and the network card for ip.c (the card_* calls of
 * ip.h).  Built as barebox builds its commands; gork's half is the one
 * object gork.o, whose names are gork_* (bb.h says why).
 */
#include <common.h>
#include <command.h>
#include <complete.h>
#include <console.h>
#include <clock.h>
#include <dirent.h>
#include <environment.h>
#include <errno.h>
#include <fcntl.h>
#include <fs.h>
#include <libfile.h>
#include <malloc.h>
#include <net.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <asm/setjmp.h>
#include <linux/stat.h>
#include <linux/time.h>
#include <rtc.h>
#include <linux/rtc.h>

#include "bb.h"

/* ip.h's card_*, which this file gives ip.c (ip.h itself would bring
   the compiler's stdint.h, which barebox's types do not agree with) */
int card_open(uint8_t mac[6]);
int card_send(const void *frame, size_t len);
int card_recv(void *buf, size_t size);
void card_pause(void);
void card_break(void);
unsigned card_seed(void);
const char *card_ip_hint(void);

int  gork_start(int argc, char **argv);    /* libc.c: then main() */
void gork_exit(int status) __attribute__((noreturn));

/* gork's variables, data and bss together (barebox/gork.lds): saved as
   they are before the first run and put back before each run after it,
   since a command, unlike a program, keeps them from one run to the next */
extern char __gork_data_start[], __gork_data_end[];
static void *gork_data_saved;

static void gork_reset(void)
{
	size_t n = __gork_data_end - __gork_data_start;

	if (!gork_data_saved) {
		gork_data_saved = malloc(n);
		if (gork_data_saved)
			memcpy(gork_data_saved, __gork_data_start, n);
	} else {
		memcpy(__gork_data_start, gork_data_saved, n);
	}
}

/* barebox's calls give -errno, or -1 and errno */
static long err(long r)
{
	if (r >= 0)
		return r;
	return r == -1 && errno > 0 ? -errno : r;
}

/* ------------------------------------------------------------------ */
/* memory, files, the environment                                      */
/* ------------------------------------------------------------------ */

void *bb_malloc(size_t n) { return malloc(n); }
void *bb_realloc(void *p, size_t n) { return realloc(p, n); }
void bb_free(void *p) { free(p); }

int bb_open(const char *path, int flags, unsigned mode)
{
	return err(open(path, flags, mode));
}

long bb_read(int fd, void *p, size_t n) { return err(read(fd, p, n)); }
long bb_write(int fd, const void *p, size_t n) { return err(write(fd, p, n)); }
int bb_close(int fd) { return err(close(fd)); }

long long bb_lseek(int fd, long long off, int whence)
{
	loff_t r = lseek(fd, off, whence);

	return r < 0 ? (errno > 0 ? -errno : -EINVAL) : r;
}

int bb_stat(const char *path, int follow, int *isdir, long long *size)
{
	struct stat st;
	int r = follow ? stat(path, &st) : lstat(path, &st);

	if (r < 0)
		return err(r);
	*isdir = S_ISDIR(st.st_mode);
	*size = st.st_size;
	return 0;
}

int bb_mkdir(const char *path) { return err(mkdir(path, 0755)); }
int bb_unlink(const char *path) { return err(unlink(path)); }
int bb_rmdir(const char *path) { return err(rmdir(path)); }

/* barebox renames nothing: a copy, then the original goes */
int bb_rename(const char *from, const char *to)
{
	int r = copy_file(from, to, 0);

	if (r < 0)
		return err(r);
	return err(unlink(from));
}

const char *bb_getcwd(void) { return getcwd(); }
int bb_chdir(const char *path) { return err(chdir(path)); }
void *bb_opendir(const char *path) { return opendir(path); }

const char *bb_readdir(void *d)
{
	struct dirent *e = readdir(d);

	return e ? e->d_name : NULL;
}

void bb_closedir(void *d) { closedir(d); }

const char *bb_getenv(const char *name) { return getenv(name); }
int bb_setenv(const char *name, const char *value) { return err(setenv(name, value)); }

/* ------------------------------------------------------------------ */
/* the console, time, the shell                                       */
/* ------------------------------------------------------------------ */

void bb_console_write(const char *p, size_t n)
{
	while (n--)
		putchar(*p++);
}

int bb_console_getc(void) { return tstc() ? getchar() : -1; }
int bb_ctrlc(void) { return ctrlc(); }
unsigned long long bb_ms(void) { return get_time_ns() / 1000000; }
void bb_udelay(unsigned us) { udelay(us); }

long long bb_epoch(void)
{
#ifdef CONFIG_RTC_CLASS
	struct rtc_device *rtc = rtc_lookup("rtc0");
	struct rtc_time tm;
	unsigned long t;

	if (!IS_ERR_OR_NULL(rtc) && rtc_read_time(rtc, &tm) == 0 &&
	    rtc_tm_to_time(&tm, &t) == 0)
		return t;
#endif
	return 0;
}

/* Output, while a command runs: what the first console that shows stdout
   would have printed, taken instead; the others print nothing. */
static char *cap;
static size_t cap_len, cap_size;
static struct console_device *cap_con;

static void cap_putc(struct console_device *cdev, char c)
{
	char *p;

	if (cdev != cap_con)
		return;
	if (cap_len + 1 >= cap_size) {
		p = realloc(cap, cap_size ? cap_size * 2 : 4096);
		if (!p)
			return;
		cap = p;
		cap_size = cap_size ? cap_size * 2 : 4096;
	}
	cap[cap_len++] = c;
}

static int cap_puts(struct console_device *cdev, const char *s, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		cap_putc(cdev, s[i]);
	return n;
}

#define NCON 8
static struct console_device *saved_con[NCON];
static void (*saved_putc[NCON])(struct console_device *, char);
static int (*saved_puts[NCON])(struct console_device *, const char *, size_t);

int bb_run(const char *line, char **out, size_t *len)
{
	int n = 0, i, r;

	if (!out)
		return run_command("%s", line);
	cap = NULL;
	cap_len = cap_size = 0;
	cap_con = NULL;
	for_each_console(cdev) {
		if (n == NCON)
			break;
		if (!cap_con && (cdev->f_active & CONSOLE_STDOUT))
			cap_con = cdev;
		saved_con[n] = cdev;
		saved_putc[n] = cdev->putc;
		saved_puts[n] = cdev->puts;
		cdev->putc = cap_putc;
		cdev->puts = cap_puts;
		n++;
	}
	r = run_command("%s", line);
	for (i = 0; i < n; i++) {
		saved_con[i]->putc = saved_putc[i];
		saved_con[i]->puts = saved_puts[i];
	}
	*out = cap;
	*len = cap_len;
	return r;
}

/* ------------------------------------------------------------------ */
/* the network card                                                   */
/* ------------------------------------------------------------------ */

/* Frames are taken from the card as it hands them up, before barebox's
   own stack sees them, and kept here until ip.c asks. */
#define NFRAMES 16
static struct eth_device *card;
static unsigned char frames[NFRAMES][1536];
static int lens[NFRAMES], head, tail;

static int take_frame(struct eth_device *edev, unsigned char **packet, int *len)
{
	(void)edev;
	if ((tail + 1) % NFRAMES != head && *len <= (int)sizeof frames[0]) {
		memcpy(frames[tail], *packet, *len);
		lens[tail] = *len;
		tail = (tail + 1) % NFRAMES;
	}
	return -ENOMSG;                 /* ours: barebox's stack never sees it */
}

int card_open(uint8_t mac[6])
{
	const char *name = getenv("GORK_ETH");
	struct eth_device *e;

	card = NULL;
	if (name && *name)
		card = eth_get_byname(name);
	else
		for_each_netdev(e) {
			card = e;
			break;
		}
	if (!card) {
		printf("gork: no network card%s%s\n", name && *name ? " " : "",
		       name && *name ? name : "");
		return -1;
	}
	if (eth_open(card) < 0) {
		printf("gork: %s does not come up\n", card->devname);
		card = NULL;
		return -1;
	}
	head = tail = 0;
	card->rx_preprocessor = take_frame;
	memcpy(mac, card->ethaddr, 6);
	return 0;
}

/* The card back to barebox, at the end of the command. */
static void card_close(void)
{
	if (card)
		card->rx_preprocessor = NULL;
	card = NULL;
}

int card_send(const void *frame, size_t len)
{
	return card && eth_send_raw(card, (void *)frame, len) >= 0 ? 0 : -1;
}

int card_recv(void *buf, size_t size)
{
	int n;

	if (!card)
		return 0;
	if (head == tail)
		eth_rx();               /* the card's driver, into take_frame */
	if (head == tail)
		return 0;
	n = lens[head] < (int)size ? lens[head] : (int)size;
	memcpy(buf, frames[head], n);
	head = (head + 1) % NFRAMES;
	return n;
}

void card_pause(void) { udelay(500); }

void card_break(void)
{
	if (ctrlc()) {
		printf("\ngork: interrupted\n");
		gork_exit(130);
	}
}

unsigned card_seed(void) { return random32() ^ (unsigned)get_time_ns(); }

const char *card_ip_hint(void)
{
	return "GORK_IP=\"192.168.1.50/24 192.168.1.1\"";
}

/* ------------------------------------------------------------------ */
/* the command                                                        */
/* ------------------------------------------------------------------ */

static jmp_buf done;
static int status;

/* gork's own stack: barebox's is 32K (STACK_SIZE), and gork keeps a few
   paths of 4K each on it.  exit() comes back by longjmp, which puts
   barebox's stack back; a return does too, below. */
#define GORK_STACK (1024 * 1024)
static char *gork_stack;

static noinline int run_on_own_stack(int argc, char **argv)
{
	register long r asm("x0") = argc;
	register char **v asm("x1") = argv;
	register char *top asm("x2") = gork_stack + GORK_STACK;

	asm volatile(
		"mov	x19, sp\n"     /* callee-saved: barebox's stack */
		"mov	sp, x2\n"
		"bl	gork_start\n"
		"mov	sp, x19\n"
		: "+r"(r), "+r"(v), "+r"(top)
		:
		: "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
		  "x12", "x13", "x14", "x15", "x16", "x17", "x18", "x19",
		  "x30", "memory", "cc");
	return r;
}

void bb_exit(int s)
{
	status = s;
	longjmp(done, 1);
}

static int do_gork(int argc, char *argv[])
{
	status = 0;
	if (!gork_stack && !(gork_stack = memalign(16, GORK_STACK))) {
		printf("gork: out of memory\n");
		return COMMAND_ERROR;
	}
	if (setjmp(done) == 0) {
		gork_reset();           /* its variables as at the first run */
		status = run_on_own_stack(argc, argv);
		gork_exit(status);      /* flushes, frees, comes back here */
	}
	card_close();
	return status ? COMMAND_ERROR : COMMAND_SUCCESS;
}

BAREBOX_CMD_HELP_START(gork)
BAREBOX_CMD_HELP_TEXT("An AI agent: a task to the Messages API, with tools on")
BAREBOX_CMD_HELP_TEXT("barebox's files and shell. Batch mode: gork \"TASK\".")
BAREBOX_CMD_HELP_TEXT("Settings in /env/gork.conf or $GORK_CONFIG; network on")
BAREBOX_CMD_HELP_TEXT("gork's own TCP/IP ($GORK_ETH, $GORK_IP, else DHCP).")
BAREBOX_CMD_HELP_END

BAREBOX_CMD_START(gork)
	.cmd		= do_gork,
	BAREBOX_CMD_DESC("AI agent harness")
	BAREBOX_CMD_OPTS("\"TASK\" | --version")
	BAREBOX_CMD_GROUP(CMD_GRP_MISC)
	BAREBOX_CMD_HELP(cmd_gork_help)
BAREBOX_CMD_END
