/*
 * serline.c -- the serial port, for a build with no network (GORK_SERIAL):
 * see serline.h.  Strict C89.  One implementation per system:
 *
 *   Unix     termios, and select() for the wait
 *   DOS      the 8250/16550 UART itself, COM1 to COM4, its receive side on
 *            the interrupt: DJGPP (32-bit, DPMI) and Open Watcom (16-bit)
 *   Windows  the Win32 comm API, which Windows 95 and NT 3.1 have
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "port.h"

#ifndef GORK_SERIAL
typedef int gork_no_serial_line;        /* C89 wants something in a file */
#else

#include "relay.h"
#include "serline.h"

/* ------------------------------------------------------------------ */
#if defined(__MSDOS__)
/* ------------------------------------------------------------------ */

#ifdef __DJGPP__
#include <dos.h>
#include <dpmi.h>
#include <go32.h>
#include <pc.h>
#include <sys/farptr.h>
#define IN(port)        inportb(port)
#define OUT(port, v)    outportb(port, v)
#define CLI()           disable()
#define STI()           enable()
#define TICKS()         _farpeekl(_dos_ds, 0x46c)
#else /* Open Watcom, real mode */
/* No stack checks: in the interrupt handler they would measure the stack
   of whatever was interrupted, and end gork.  For the same reason this
   file is compiled with -zu (dos16build.sh), so that the handler does not
   reach the data through SS, which is not DGROUP there. */
#pragma off (check_stack)
#include <conio.h>
#include <dos.h>
#include <i86.h>
#define IN(port)        inp(port)
#define OUT(port, v)    outp(port, v)
#define CLI()           _disable()
#define STI()           _enable()
#define TICKS()         (*(volatile unsigned long __far *)MK_FP(0x40, 0x6c))
#endif

/* UART registers, from the base */
#define RBR 0                   /* receive (read), transmit (write) */
#define IER 1
#define IIR 2                   /* and FCR, written */
#define LCR 3
#define MCR 4
#define LSR 5
#define MSR 6

/* Ticks since t0, which the BIOS counter passes at midnight: from 0. */
static unsigned long since(unsigned long *t0) {
  unsigned long t = TICKS();

  if (t < *t0)
    *t0 = t;
  return t - *t0;
}

#define RING 4096               /* a power of 2 */
#define RTS_OFF (RING - 512)    /* with rtscts: hold the far end off here */
#define RTS_ON  (RING / 4)

/* What the interrupt handler touches: locked under DPMI. */
static volatile unsigned char ring[RING];
static volatile unsigned head, tail;    /* handler puts at head */
static volatile int overrun;
static unsigned base;
static int irq, flow, opened;
static unsigned char old_mask, old_mcr;

/* What the UART holds, into the ring. */
static void uart_drain(void) {
  while (IN(base + LSR) & 0x01) {
    unsigned char c = (unsigned char)IN(base + RBR);

    if (((head + 1) & (RING - 1)) == tail) {
      overrun = 1;
    } else {
      ring[head] = c;
      head = (head + 1) & (RING - 1);
    }
  }
  if (flow && ((head - tail) & (RING - 1)) >= RTS_OFF)
    OUT(base + MCR, 0x09);      /* DTR, OUT2: RTS down */
}

/* The interrupt.  The PIC wants an edge: should a byte have come after
   the drain, the UART's interrupt line would stay up and never rise
   again.  Off and on again, it rises if anything is waiting. */
static void uart_take(void) {
  uart_drain();
  OUT(base + IER, 0);
  OUT(base + IER, 0x01);
  OUT(0x20, 0x20);              /* end of interrupt, to the PIC */
}

#ifdef __DJGPP__
static _go32_dpmi_seginfo old_vec, new_vec;

static void uart_isr(void) {
  uart_take();
}
static void uart_isr_end(void) {
}

static int hook(void) {
  int vec = 8 + irq;

  if (_go32_dpmi_lock_code((void *)uart_drain,
                           (char *)uart_isr_end - (char *)uart_drain) != 0 ||
      _go32_dpmi_lock_data((void *)ring, sizeof ring) != 0 ||
      _go32_dpmi_lock_data((void *)&head, sizeof head) != 0 ||
      _go32_dpmi_lock_data((void *)&tail, sizeof tail) != 0 ||
      _go32_dpmi_lock_data((void *)&overrun, sizeof overrun) != 0 ||
      _go32_dpmi_lock_data((void *)&base, sizeof base) != 0 ||
      _go32_dpmi_lock_data((void *)&flow, sizeof flow) != 0) {
    fprintf(stderr, "gork: serial: cannot lock the interrupt handler\n");
    return -1;
  }
  _go32_dpmi_get_protected_mode_interrupt_vector(vec, &old_vec);
  new_vec.pm_offset = (unsigned long)uart_isr;
  new_vec.pm_selector = _go32_my_cs();
  if (_go32_dpmi_allocate_iret_wrapper(&new_vec) != 0) {
    fprintf(stderr, "gork: serial: no interrupt wrapper\n");
    return -1;
  }
  _go32_dpmi_set_protected_mode_interrupt_vector(vec, &new_vec);
  return 0;
}

static void unhook(void) {
  _go32_dpmi_set_protected_mode_interrupt_vector(8 + irq, &old_vec);
  _go32_dpmi_free_iret_wrapper(&new_vec);
}
#else
static void (__interrupt __far *old_vec)(void);

static void __interrupt __far uart_isr(void) {
  uart_take();
}

static int hook(void) {
  old_vec = _dos_getvect(8 + irq);
  _dos_setvect(8 + irq, uart_isr);
  return 0;
}

static void unhook(void) {
  _dos_setvect(8 + irq, old_vec);
}
#endif

/* The port's base address: the BIOS's, or the usual one. */
static unsigned com_base(int n) {
  static const unsigned usual[4] = { 0x3f8, 0x2f8, 0x3e8, 0x2e8 };
  unsigned b;

#ifdef __DJGPP__
  b = _farpeekw(_dos_ds, 0x400 + 2 * (n - 1));
#else
  b = *(unsigned __far *)MK_FP(0x40, 2 * (n - 1));
#endif
  return b != 0 ? b : usual[n - 1];
}

static void at_exit(void) {
  line_close();                 /* or the next interrupt jumps into DOS */
}

int line_open(const char *dev, long baud, int rtscts) {
  static int registered;
  unsigned div;
  int n;

  if (strlen(dev) != 4 || (dev[0] != 'C' && dev[0] != 'c') ||
      (dev[1] != 'O' && dev[1] != 'o') || (dev[2] != 'M' && dev[2] != 'm') ||
      dev[3] < '1' || dev[3] > '4') {
    fprintf(stderr, "gork: serial: want COM1 to COM4, not '%s'\n", dev);
    return -1;
  }
  if (baud < 50 || baud > 115200 || 115200L % baud != 0) {
    fprintf(stderr, "gork: serial: %ld is not a speed the UART has\n", baud);
    return -1;
  }
  n = dev[3] - '0';
  base = com_base(n);
  irq = n == 1 || n == 3 ? 4 : 3;
  flow = rtscts;
  div = (unsigned)(115200L / baud);

  OUT(base + IER, 0);
  OUT(base + LCR, 0x80);        /* DLAB, for the divisor */
  OUT(base + RBR, div & 0xff);
  OUT(base + IER, div >> 8);
  OUT(base + LCR, 0x03);        /* 8N1 */
  if ((IN(base + LCR) & 0x7f) != 0x03) {
    fprintf(stderr, "gork: serial: no UART at %Xh (%s)\n", base, dev);
    return -1;
  }
  OUT(base + IIR, 0xc7);        /* 16550: the FIFOs on, cleared, 14 bytes */
  old_mcr = (unsigned char)IN(base + MCR);
  head = tail = 0;
  overrun = 0;
  if (hook() != 0)
    return -1;
  CLI();
  old_mask = (unsigned char)IN(0x21);
  OUT(0x21, old_mask & ~(1 << irq));
  OUT(base + MCR, 0x0b);        /* DTR, RTS, OUT2: the interrupt line on */
  OUT(base + IER, 0x01);        /* on received data */
  STI();
  while (IN(base + LSR) & 0x01) /* what was there before */
    (void)IN(base + RBR);
  opened = 1;
  if (!registered) {
    atexit(at_exit);
    registered = 1;
  }
  return 0;
}

int line_write(const unsigned char *p, size_t n) {
  unsigned long t0 = TICKS();

  while (n > 0) {
    if ((IN(base + LSR) & 0x20) &&
        (!flow || (IN(base + MSR) & 0x10))) {
      OUT(base + RBR, *p++);
      n--;
      t0 = TICKS();
    } else if (since(&t0) > 18UL * (unsigned long)relay_timeout) {
      fprintf(stderr, "gork: timeout while sending%s\n",
              flow ? ": CTS stays down" : "");
      return -1;
    }
  }
  return 0;
}

int line_read(unsigned char *p, size_t n) {
  unsigned long t0 = TICKS();
  size_t got = 0;

  /* 4 ticks, 220 ms; the UART is emptied here too, should an interrupt
     have gone astray */
  while (head == tail && since(&t0) < 4) {
    CLI();
    uart_drain();
    STI();
  }
  if (overrun) {
    overrun = 0;
    fprintf(stderr, "gork: serial: bytes lost, the buffer was full: "
                    "try rtscts, or a lower speed\n");
    return -1;
  }
  while (got < n && head != tail) {
    p[got++] = ring[tail];
    tail = (tail + 1) & (RING - 1);
  }
  if (flow && ((head - tail) & (RING - 1)) <= RTS_ON)
    OUT(base + MCR, 0x0b);      /* RTS up again */
  return (int)got;
}

void line_flush(void) {
  CLI();
  tail = head;
  overrun = 0;
  STI();
  if (flow)
    OUT(base + MCR, 0x0b);
}

void line_close(void) {
  if (!opened)
    return;
  opened = 0;
  CLI();
  OUT(base + IER, 0);
  OUT(base + MCR, old_mcr);
  OUT(0x21, (IN(0x21) & ~(1 << irq)) | (old_mask & (1 << irq)));
  STI();
  unhook();
}

/* ------------------------------------------------------------------ */
#elif defined(_WIN32)
/* ------------------------------------------------------------------ */

#include <windows.h>

static HANDLE com = INVALID_HANDLE_VALUE;

static void win_error(const char *what) {
  fprintf(stderr, "gork: serial: %s: Windows error %lu\n", what,
          (unsigned long)GetLastError());
}

int line_open(const char *dev, long baud, int rtscts) {
  char name[256 + 8];
  DCB dcb;
  COMMTIMEOUTS to;

  /* COM10 and up only as \\.\COM10, which Windows 95 also takes */
  if (strlen(dev) > 256) {
    fprintf(stderr, "gork: serial: device name too long\n");
    return -1;
  }
  if (strncmp(dev, "\\\\.\\", 4) != 0 && strlen(dev) > 4)
    sprintf(name, "\\\\.\\%s", dev);
  else
    strcpy(name, dev);
  com = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                    OPEN_EXISTING, 0, NULL);
  if (com == INVALID_HANDLE_VALUE) {
    fprintf(stderr, "gork: %s: cannot open it (Windows error %lu)\n", dev,
            (unsigned long)GetLastError());
    return -1;
  }
  memset(&dcb, 0, sizeof dcb);
  dcb.DCBlength = sizeof dcb;
  if (!GetCommState(com, &dcb)) {
    fprintf(stderr, "gork: %s: not a serial port (Windows error %lu)\n",
            dev, (unsigned long)GetLastError());
    line_close();
    return -1;
  }
  dcb.BaudRate = (DWORD)baud;
  dcb.ByteSize = 8;
  dcb.Parity = NOPARITY;
  dcb.StopBits = ONESTOPBIT;
  dcb.fBinary = TRUE;
  dcb.fParity = FALSE;
  dcb.fOutxCtsFlow = rtscts ? TRUE : FALSE;
  dcb.fRtsControl = rtscts ? RTS_CONTROL_HANDSHAKE : RTS_CONTROL_ENABLE;
  dcb.fOutxDsrFlow = FALSE;
  dcb.fDtrControl = DTR_CONTROL_ENABLE;
  dcb.fDsrSensitivity = FALSE;
  dcb.fOutX = dcb.fInX = FALSE;
  dcb.fErrorChar = dcb.fNull = FALSE;
  dcb.fAbortOnError = FALSE;
  if (!SetCommState(com, &dcb)) {
    fprintf(stderr, "gork: %s: %ld baud: the port refuses it (Windows "
                    "error %lu)\n", dev, baud, (unsigned long)GetLastError());
    line_close();
    return -1;
  }
  SetupComm(com, 8192, 8192);
  /* a read returns what came, after 200 ms at most, or as soon as the
     line goes quiet for 20 */
  memset(&to, 0, sizeof to);
  to.ReadIntervalTimeout = 20;
  to.ReadTotalTimeoutConstant = 200;
  to.WriteTotalTimeoutConstant = (DWORD)relay_timeout * 1000;
  if (!SetCommTimeouts(com, &to)) {
    win_error("timeouts");
    line_close();
    return -1;
  }
  PurgeComm(com, PURGE_RXCLEAR | PURGE_TXCLEAR);
  return 0;
}

int line_write(const unsigned char *p, size_t n) {
  while (n > 0) {
    DWORD w = 0;

    if (!WriteFile(com, p, (DWORD)n, &w, NULL)) {
      win_error("write");
      return -1;
    }
    if (w == 0) {
      fprintf(stderr, "gork: timeout while sending\n");
      return -1;
    }
    p += w;
    n -= w;
  }
  return 0;
}

int line_read(unsigned char *p, size_t n) {
  DWORD r = 0;

  if (!ReadFile(com, p, (DWORD)n, &r, NULL)) {
    win_error("read");
    return -1;
  }
  return (int)r;
}

void line_flush(void) {
  PurgeComm(com, PURGE_RXCLEAR);
}

void line_close(void) {
  if (com != INVALID_HANDLE_VALUE)
    CloseHandle(com);
  com = INVALID_HANDLE_VALUE;
}

/* ------------------------------------------------------------------ */
#elif defined(GORK_EFI)
/* ------------------------------------------------------------------ */

/* UEFI: the firmware's Serial I/O protocol.  COMn is the nth port that
   has it, in the firmware's order.  The firmware may make every serial
   port a console, whose driver then reads it for keys and writes the
   console's text to it: the port is opened exclusively, which stops that
   driver, and given back to it at the end. */
#include "uefi/efi.h"

static EFI_GUID sio_guid = EFI_SERIAL_IO_PROTOCOL_GUID;
static EFI_SERIAL_IO_PROTOCOL *sio;
static EFI_HANDLE sio_handle;

int line_open(const char *dev, long baud, int rtscts) {
  EFI_HANDLE *h;
  EFI_STATUS s;
  UINTN n;
  uint32_t ctl;
  long k;
  char *end;

  k = strtol(dev + ((dev[0] == 'C' || dev[0] == 'c') ? 3 : 0), &end, 10);
  if (*end != '\0' || k < 1 ||
      ((dev[0] == 'C' || dev[0] == 'c') &&
       (strlen(dev) < 4 || (dev[1] != 'O' && dev[1] != 'o') ||
        (dev[2] != 'M' && dev[2] != 'm')))) {
    fprintf(stderr, "gork: serial: want COM1, COM2, ...: the nth serial "
                    "port the firmware has, not '%s'\n", dev);
    return -1;
  }
  if (efi_bs->LocateHandleBuffer(ByProtocol, &sio_guid, NULL, &n, &h)
      != EFI_SUCCESS)
    n = 0;
  if ((UINTN)k > n) {
    fprintf(stderr, "gork: serial: the firmware has %lu serial port%s, "
                    "and no %s\n", (unsigned long)n, n == 1 ? "" : "s", dev);
    if (n > 0)
      efi_bs->FreePool(h);
    return -1;
  }
  sio_handle = h[k - 1];
  efi_bs->FreePool(h);
  s = efi_bs->OpenProtocol(sio_handle, &sio_guid, (void **)&sio, efi_image,
                           NULL, EFI_OPEN_PROTOCOL_EXCLUSIVE);
  if (s != EFI_SUCCESS) {
    fprintf(stderr, "gork: serial: %s: in use, and cannot be had alone\n",
            dev);
    sio = NULL;
    return -1;
  }
  /* no parity (1), 8 bits, one stop bit (1); 200 ms per byte at most */
  s = sio->SetAttributes(sio, (uint64_t)baud, 0, 200000, 1, 8, 1);
  if (s != EFI_SUCCESS) {
    fprintf(stderr, "gork: serial: %s: %ld baud: the firmware refuses it\n",
            dev, baud);
    line_close();
    return -1;
  }
  {
    static int registered;      /* the console back at the end */

    if (!registered && atexit(line_close) == 0)
      registered = 1;
  }
  ctl = EFI_SERIAL_DATA_TERMINAL_READY | EFI_SERIAL_REQUEST_TO_SEND |
        (rtscts ? EFI_SERIAL_HARDWARE_FLOW_CONTROL_ENABLE : 0);
  if (sio->SetControl(sio, ctl) != EFI_SUCCESS && rtscts) {
    fprintf(stderr, "gork: serial: %s: the firmware has no RTS/CTS flow "
                    "control\n", dev);
    line_close();
    return -1;
  }
  return 0;
}

int line_write(const unsigned char *p, size_t n) {
  time_t end = time(NULL) + relay_timeout;

  while (n > 0) {
    UINTN w = n;
    EFI_STATUS s = sio->Write(sio, &w, (void *)p);

    if (s != EFI_SUCCESS && s != EFI_TIMEOUT) {
      fprintf(stderr, "gork: serial: write failed\n");
      return -1;
    }
    p += w;
    n -= w;
    if (n > 0 && w == 0 && time(NULL) >= end) {
      fprintf(stderr, "gork: timeout while sending\n");
      return -1;
    }
  }
  return 0;
}

int line_read(unsigned char *p, size_t n) {
  UINTN r = n;
  EFI_STATUS s = sio->Read(sio, &r, p);

  /* EFI_TIMEOUT: a byte did not come in time, after r that did */
  if (s != EFI_SUCCESS && s != EFI_TIMEOUT) {
    fprintf(stderr, "gork: serial: read failed%s\n",
            s == EFI_DEVICE_ERROR ? ": bytes lost, try rtscts or a lower "
                                    "speed" : "");
    return -1;
  }
  return (int)r;
}

void line_flush(void) {
  unsigned char junk[256];

  while (line_read(junk, sizeof junk) > 0)
    ;
}

void line_close(void) {
  if (sio == NULL)
    return;
  efi_bs->CloseProtocol(sio_handle, &sio_guid, efi_image, NULL);
  efi_bs->ConnectController(sio_handle, NULL, NULL, 1); /* the console */
  sio = NULL;
}

/* ------------------------------------------------------------------ */
#elif defined(GORK_EMX)
/* ------------------------------------------------------------------ */

/* OS/2: COM.SYS through DosDevIOCtl, category 1 (IOCTL_ASYNC).  EMX's
   termios covers only its own terminals, not the COM ports. */
#define INCL_DOSFILEMGR
#define INCL_DOSDEVICES
#define INCL_DOSDEVIOCTL
#define INCL_DOSERRORS
#include <os2.h>

#ifndef ASYNC_EXTSETBAUDRATE
#define ASYNC_EXTSETBAUDRATE 0x43
#endif

static HFILE com;
static int com_open;

static int ioctl_set(ULONG cat, ULONG fn, void *parm, ULONG plen) {
  ULONG pl = plen, dl = 0;

  return (int)DosDevIOCtl(com, cat, fn, parm, plen, &pl, NULL, 0, &dl);
}

int line_open(const char *dev, long baud, int rtscts) {
  ULONG action, len, dl;
  APIRET rc;
  DCBINFO dcb;
  struct {
    UCHAR data, parity, stop;
  } lc;
  struct {
    ULONG baud;
    UCHAR fraction;
  } ext;
  USHORT b16;

  rc = DosOpen((PSZ)dev, &com, &action, 0, FILE_NORMAL, OPEN_ACTION_OPEN_IF_EXISTS,
               OPEN_ACCESS_READWRITE | OPEN_SHARE_DENYREADWRITE |
               OPEN_FLAGS_FAIL_ON_ERROR | OPEN_FLAGS_NOINHERIT, NULL);
  if (rc != NO_ERROR) {
    fprintf(stderr, "gork: %s: cannot open it (OS/2 error %lu)\n", dev,
            (unsigned long)rc);
    return -1;
  }
  com_open = 1;
  /* above 19200 only through the extended call, which Warp's COM.SYS has */
  ext.baud = (ULONG)baud;
  ext.fraction = 0;
  if (ioctl_set(IOCTL_ASYNC, ASYNC_EXTSETBAUDRATE, &ext, sizeof ext) != 0) {
    b16 = (USHORT)baud;
    if (baud > 65535L ||
        ioctl_set(IOCTL_ASYNC, ASYNC_SETBAUDRATE, &b16, sizeof b16) != 0) {
      fprintf(stderr, "gork: %s: %ld baud: the port refuses it\n", dev, baud);
      line_close();
      return -1;
    }
  }
  lc.data = 8;
  lc.parity = 0;
  lc.stop = 0;                  /* one */
  if (ioctl_set(IOCTL_ASYNC, ASYNC_SETLINECTRL, &lc, sizeof lc) != 0) {
    fprintf(stderr, "gork: %s: cannot set 8N1\n", dev);
    line_close();
    return -1;
  }
  len = 0;
  dl = sizeof dcb;
  if (DosDevIOCtl(com, IOCTL_ASYNC, ASYNC_GETDCBINFO, NULL, 0, &len, &dcb,
                  sizeof dcb, &dl) != NO_ERROR) {
    fprintf(stderr, "gork: %s: not a serial port\n", dev);
    line_close();
    return -1;
  }
  /* DTR on; RTS on, or for the handshake; CTS minded with rtscts; no
     XON/XOFF, no replacing of bytes; a read waits 0.2 s for something */
  dcb.fbCtlHndShake = MODE_DTR_CONTROL | (rtscts ? MODE_CTS_HANDSHAKE : 0);
  dcb.fbFlowReplace = rtscts ? MODE_RTS_HANDSHAKE : MODE_RTS_CONTROL;
  dcb.fbTimeout = (dcb.fbTimeout & ~0x07) | MODE_WAIT_READ_TIMEOUT;
  dcb.usReadTimeout = 19;       /* hundredths, less one */
  dcb.usWriteTimeout = relay_timeout > 650 ? 65000 :
                       (USHORT)(relay_timeout * 100);
  if (ioctl_set(IOCTL_ASYNC, ASYNC_SETDCBINFO, &dcb, sizeof dcb) != 0) {
    fprintf(stderr, "gork: %s: cannot set its flow control\n", dev);
    line_close();
    return -1;
  }
  line_flush();
  return 0;
}

int line_write(const unsigned char *p, size_t n) {
  while (n > 0) {
    ULONG w = 0;
    APIRET rc = DosWrite(com, (PVOID)p, (ULONG)n, &w);

    if (rc != NO_ERROR) {
      fprintf(stderr, "gork: serial: write: OS/2 error %lu\n",
              (unsigned long)rc);
      return -1;
    }
    if (w == 0) {
      fprintf(stderr, "gork: timeout while sending\n");
      return -1;
    }
    p += w;
    n -= w;
  }
  return 0;
}

int line_read(unsigned char *p, size_t n) {
  ULONG r = 0;
  APIRET rc = DosRead(com, p, (ULONG)n, &r);

  if (rc != NO_ERROR) {
    fprintf(stderr, "gork: serial: read: OS/2 error %lu\n",
            (unsigned long)rc);
    return -1;
  }
  return (int)r;
}

void line_flush(void) {
  UCHAR cmd = 0, data = 0;
  ULONG pl = 1, dl = 1;

  DosDevIOCtl(com, IOCTL_GENERAL, DEV_FLUSHINPUT, &cmd, 1, &pl, &data, 1,
              &dl);
}

void line_close(void) {
  if (com_open)
    DosClose(com);
  com_open = 0;
}

/* ------------------------------------------------------------------ */
#else /* Unix: termios */
/* ------------------------------------------------------------------ */

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#ifndef GORK_NO_SELECT
#include <sys/time.h>
#endif

static int fd = -1;

static const struct {
  long baud;
  speed_t code;
} speeds[] = {
  { 1200, B1200 }, { 2400, B2400 }, { 4800, B4800 }, { 9600, B9600 },
  { 19200, B19200 }, { 38400, B38400 },
#ifdef B57600
  { 57600, B57600 },
#endif
#ifdef B115200
  { 115200, B115200 },
#endif
#ifdef B230400
  { 230400, B230400 },
#endif
  { 0, B0 }
};

int line_open(const char *dev, long baud, int rtscts) {
  struct termios t;
  int i;

  for (i = 0; speeds[i].baud != 0 && speeds[i].baud != baud; i++)
    ;
  if (speeds[i].baud == 0) {
    fprintf(stderr, "gork: serial: %ld is not a speed this system has\n",
            baud);
    return -1;
  }
#ifndef CRTSCTS
  if (rtscts) {
    fprintf(stderr, "gork: serial: this system has no RTS/CTS flow "
                    "control\n");
    return -1;
  }
#endif
  /* O_NONBLOCK: do not wait for a carrier the null modem may not give */
  fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) {
    fprintf(stderr, "gork: %s: %s\n", dev, strerror(errno));
    return -1;
  }
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
  fcntl(fd, F_SETFD, FD_CLOEXEC);       /* run_command's children */
  if (tcgetattr(fd, &t) != 0) {
    fprintf(stderr, "gork: %s: not a serial line: %s\n", dev,
            strerror(errno));
    line_close();
    return -1;
  }
  t.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL |
                 IXON | IXOFF);
  t.c_iflag |= IGNPAR;
  t.c_oflag &= ~OPOST;
  t.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
  t.c_cflag &= ~(CSIZE | PARENB | CSTOPB | HUPCL);
  t.c_cflag |= CS8 | CREAD | CLOCAL;
#ifdef CRTSCTS
  if (rtscts)
    t.c_cflag |= CRTSCTS;
  else
    t.c_cflag &= ~CRTSCTS;
#endif
  t.c_cc[VMIN] = 1;
  t.c_cc[VTIME] = 0;
  cfsetispeed(&t, speeds[i].code);
  cfsetospeed(&t, speeds[i].code);
  if (tcsetattr(fd, TCSANOW, &t) != 0) {
    fprintf(stderr, "gork: %s: %s\n", dev, strerror(errno));
    line_close();
    return -1;
  }
  return 0;
}

int line_write(const unsigned char *p, size_t n) {
  while (n > 0) {
    int w = (int)write(fd, p, n);

    if (w < 0) {
      if (errno == EINTR)
        fprintf(stderr, "gork: timeout while sending\n");
      else
        fprintf(stderr, "gork: serial line: %s\n", strerror(errno));
      return -1;
    }
    p += w;
    n -= (size_t)w;
  }
  return 0;
}

int line_read(unsigned char *p, size_t n) {
  int r;
#ifndef GORK_NO_SELECT
  fd_set rd;
  struct timeval tv;

  FD_ZERO(&rd);
  FD_SET(fd, &rd);
  tv.tv_sec = 0;
  tv.tv_usec = 200000;
  r = select(fd + 1, &rd, NULL, NULL, &tv);
  if (r == 0)
    return 0;
  if (r < 0) {
    if (errno == EINTR)
      return -2;
    perror("gork: select");
    return -1;
  }
#endif
  r = (int)read(fd, p, n);
  if (r > 0)
    return r;
  if (r < 0 && errno == EINTR)
    return -2;
  fprintf(stderr, "gork: serial line: %s\n",
          r == 0 ? "hung up" : strerror(errno));
  return -1;
}

void line_flush(void) {
  tcflush(fd, TCIFLUSH);
}

void line_close(void) {
  if (fd >= 0)
    close(fd);
  fd = -1;
}

#endif /* systems */
#endif /* GORK_SERIAL */
