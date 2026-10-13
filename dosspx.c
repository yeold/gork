/*
 * dosspx.c -- SPX for the DOS build, through the IPX driver's own interface
 * (Novell's IPX.COM or IPXODI.COM, or PDIPX over a packet driver).
 *
 * The driver runs in real mode and works on event control blocks (ECBs)
 * that it fills in the background, so the ECBs and their buffers live in
 * one block of DOS memory and are copied in and out.  No event service
 * routines: every ECB is polled, and the driver is given time in between.
 * One connection at a time, which is all relay.c needs.
 */
#ifdef __MSDOS__

#include <dpmi.h>
#include <go32.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/movedata.h>

#include "port.h"

#define HDR      42             /* IPX and SPX headers */
#define DATA     534            /* 576, what every IPX path carries */
#define ECB      48             /* with two fragments */
#define SLOT     (ECB + HDR + DATA)
#define LISTEN   4              /* receive ECBs kept posted */
#define NSLOTS   (LISTEN + 1)   /* and one to connect, send and end with */

/* ECB offsets */
enum { E_INUSE = 8, E_CODE = 9, E_SOCKET = 10, E_NFRAG = 34, E_FRAG = 36 };
/* header offsets */
enum { H_LEN = 2, H_DEST = 6, H_CTL = 30, H_STREAM = 31, H_SEQ = 36 };

#define STREAM_END 0xfe         /* the partner ended the connection */

static int           seg, sel;          /* the DOS memory block */
static unsigned      entry_seg, entry_off;
static unsigned      sock, conn;
static int           is_open;
static unsigned      rx_seq;            /* the packet due next */
static char          pend[DATA];        /* received, not yet handed out */
static size_t        pend_len, pend_off;

static unsigned ipx(unsigned bx, unsigned ax, unsigned dx, int slot,
                    unsigned *dx_out) {
    __dpmi_regs r;

    memset(&r, 0, sizeof r);
    r.x.bx = bx;
    r.x.ax = ax;
    r.x.dx = dx;
    r.x.es = seg;
    r.x.si = slot * SLOT;
    r.x.cs = entry_seg;
    r.x.ip = entry_off;
    __dpmi_simulate_real_mode_procedure_retf(&r);
    if (dx_out != NULL)
        *dx_out = r.x.dx;
    return r.h.al;
}

static void     put(int slot, int off, const void *p, size_t n) {
    dosmemput(p, n, seg * 16 + slot * SLOT + off);
}
static void     get(int slot, int off, void *p, size_t n) {
    dosmemget(seg * 16 + slot * SLOT + off, n, p);
}
static unsigned byte_at(int slot, int off) {
    unsigned char b;

    get(slot, off, &b, 1);
    return b;
}

/* An ECB over its slot's header, and `data` bytes after it if nonzero. */
static void ecb_init(int slot, size_t data) {
    unsigned char  e[ECB];
    unsigned short f[6];

    memset(e, 0, sizeof e);
    e[E_SOCKET] = (unsigned char) sock;         /* as the driver gave it */
    e[E_SOCKET + 1] = (unsigned char) (sock >> 8);
    e[E_NFRAG] = data ? 2 : 1;
    f[0] = slot * SLOT + ECB;                   /* offset, segment, size */
    f[1] = seg;
    f[2] = HDR;
    f[3] = slot * SLOT + ECB + HDR;
    f[4] = seg;
    f[5] = (unsigned short) data;
    memcpy(e + E_FRAG, f, sizeof f);
    put(slot, 0, e, sizeof e);
}

/* Wait for the driver to finish with an ECB; its completion code, or -1
   after `secs`, or -2 if relay_abort asked to stop. */
static int ecb_wait(int slot, int secs, int (*stop)(void)) {
    time_t  end = time(NULL) + secs;
    clock_t asked = clock();

    while (byte_at(slot, E_INUSE) != 0) {
        ipx(0x000a, 0, 0, 0, NULL);             /* relinquish control */
        if (time(NULL) > end)
            return -1;
        if (stop != NULL && clock() - asked > CLOCKS_PER_SEC / 5) {
            asked = clock();
            if (stop())
                return -2;
        }
    }
    return (int) byte_at(slot, E_CODE);
}

static void listen_post(int slot) {
    ecb_init(slot, DATA);
    ipx(0x0017, 0, 0, slot, NULL);
}

void dosspx_close(void) {
    if (!is_open)
        return;
    if (conn != 0) {
        ecb_init(LISTEN, 0);
        ipx(0x0013, 0, conn, LISTEN, NULL);     /* end the connection */
        ecb_wait(LISTEN, 3, NULL);
    }
    ipx(0x0001, 0, sock, 0, NULL);              /* cancels what is posted */
    __dpmi_free_dos_memory(sel);
    is_open = 0;
    conn = 0;
}

int dosspx_open(const unsigned char *net, const unsigned char *node,
                unsigned socket, int (*stop)(void)) {
    __dpmi_regs   r;
    unsigned char hdr[HDR];
    int           i, code;

    memset(&r, 0, sizeof r);
    r.x.ax = 0x7a00;
    __dpmi_int(0x2f, &r);
    if (r.h.al != 0xff) {
        fprintf(stderr, "gork: no IPX driver loaded\n");
        return -1;
    }
    entry_seg = r.x.es;
    entry_off = r.x.di;
    if (ipx(0x0010, 0, 0, 0, NULL) != 0xff) {
        fprintf(stderr, "gork: the IPX driver has no SPX\n");
        return -1;
    }
    seg = __dpmi_allocate_dos_memory((NSLOTS * SLOT + 15) / 16, &sel);
    if (seg < 0) {
        fprintf(stderr, "gork: no DOS memory for SPX\n");
        return -1;
    }
    if (ipx(0x0000, 0, 0, 0, &sock) != 0) {     /* any free socket */
        fprintf(stderr, "gork: cannot open an IPX socket\n");
        __dpmi_free_dos_memory(sel);
        return -1;
    }
    is_open = 1;
    conn = 0;
    rx_seq = 0;
    pend_len = pend_off = 0;
    /* SPX needs somewhere to receive before it will connect */
    for (i = 0; i < LISTEN; i++)
        listen_post(i);

    ecb_init(LISTEN, 0);
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr + H_DEST, net, 4);
    memcpy(hdr + H_DEST + 4, node, 6);
    hdr[H_DEST + 10] = (unsigned char) (socket >> 8);
    hdr[H_DEST + 11] = (unsigned char) socket;
    put(LISTEN, ECB, hdr, sizeof hdr);
    /* AL: retries, the driver's default; AH: watch the connection */
    code = ipx(0x0011, 0x0100, 0, LISTEN, &conn);
    if (code == 0)
        code = ecb_wait(LISTEN, 30, stop);
    if (code != 0) {
        if (code == -2) {
            ipx(0x0014, 0, conn, 0, NULL);      /* abort */
        } else if (code == 0xed || code == -1) {
            fprintf(stderr, "gork: no answer from the relay on SPX\n");
        } else {
            fprintf(stderr, "gork: SPX connect failed, code %02X\n", code);
        }
        conn = 0;
        dosspx_close();
        return code == -2 ? -2 : -1;
    }
    return 0;
}

int dosspx_write(const char *p, size_t n) {
    unsigned char hdr[HDR];

    while (n > 0) {
        size_t part = n > DATA ? DATA : n;
        int    code;

        ecb_init(LISTEN, part);
        memset(hdr, 0, sizeof hdr);             /* SPX fills in the rest */
        put(LISTEN, ECB, hdr, sizeof hdr);
        put(LISTEN, ECB + HDR, p, part);
        ipx(0x0016, 0, conn, LISTEN, NULL);
        code = ecb_wait(LISTEN, 60, NULL);
        if (code != 0) {
            fprintf(stderr, "gork: SPX send failed, code %02X\n",
                    code & 0xff);
            return -1;
        }
        p += part;
        n -= part;
    }
    return 0;
}

/* Up to n bytes: how many, 0 at the end of the connection, -1 on error,
   -3 if nothing has arrived yet. */
int dosspx_read(char *buf, size_t n) {
    unsigned char hdr[HDR];
    unsigned      code, len;

    if (pend_off == pend_len) {
        int i, slot = -1, failed = -1;

        ipx(0x000a, 0, 0, 0, NULL);
        /* The driver does not use the ECBs in the order they were posted:
           take the finished one that holds the next packet in sequence. */
        for (i = 0; i < LISTEN && slot < 0; i++) {
            if (byte_at(i, E_INUSE) != 0)
                continue;
            code = byte_at(i, E_CODE);
            get(i, ECB, hdr, sizeof hdr);
            if (code != 0)
                failed = (int) code;
            else if ((unsigned) (hdr[H_SEQ] << 8 | hdr[H_SEQ + 1]) == rx_seq)
                slot = i;
        }
        if (slot < 0) {
            if (failed == 0xec)                 /* ended by the partner */
                return 0;
            if (failed >= 0) {
                fprintf(stderr, "gork: SPX receive failed, code %02X\n",
                        failed);
                return -1;
            }
            return -3;
        }
        rx_seq = (rx_seq + 1) & 0xffff;
        if (hdr[H_STREAM] == STREAM_END)
            return 0;
        len = (hdr[H_LEN] << 8 | hdr[H_LEN + 1]) - HDR;
        if (len > DATA)
            len = DATA;
        get(slot, ECB + HDR, pend, len);
        pend_len = len;
        pend_off = 0;
        listen_post(slot);
        if (len == 0)
            return -3;
    }
    if (n > pend_len - pend_off)
        n = pend_len - pend_off;
    memcpy(buf, pend + pend_off, n);
    pend_off += n;
    return (int) n;
}

#endif /* __MSDOS__ */
