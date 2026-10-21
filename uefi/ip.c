/*
 * ip.c -- a small TCP/IP stack of gork's own, on the network card itself
 * (SimpleNetwork: whole Ethernet frames), for firmware that offers the
 * card but no TCP: U-Boot's UEFI, or a PC with its network stack turned
 * off.  net.c uses it when it finds no TCP4.
 *
 * Enough for what relay.c does, and no more: IPv4 over Ethernet, ARP,
 * DHCP (or GORK_IP), DNS over UDP, and one TCP connection at a time, as a
 * client.  TCP retransmits from the oldest unacknowledged byte (go-back-
 * N), keeps segments that arrive in order only, and advertises what its
 * receive buffer has free; no window scaling, no fragments.  Answers ARP
 * and ping for its address, so the network can find it.
 *
 * Everything is polled, from ip_poll(): libc.c's waits call it through
 * efi_net_poll().
 */

#include "efi.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void efi_check_break(void);

static EFI_GUID snp_guid = EFI_SIMPLE_NETWORK_PROTOCOL_GUID;
static EFI_SIMPLE_NETWORK_PROTOCOL *snp;

static uint8_t mac[6];
static uint8_t my_ip[4], mask[4], gw[4], dns_ip[4];
static const uint8_t bcast_mac[6] = { 255, 255, 255, 255, 255, 255 };
static const uint8_t bcast_ip[4] = { 255, 255, 255, 255 };
static const uint8_t zero_ip[4];

static uint8_t rx[1600], tx[1600];

/* ------------------------------------------------------------------ */
/* bytes                                                              */
/* ------------------------------------------------------------------ */

static void put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t) (v >> 8);
    p[1] = (uint8_t) v;
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v >> 16);
    put16(p + 2, v & 0xffff);
}

static unsigned get16(const uint8_t *p) { return (unsigned) p[0] << 8 | p[1]; }

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t) get16(p) << 16 | get16(p + 2);
}

/* the ones' complement sum, as IP, ICMP, UDP and TCP take it */
static uint32_t sum16(uint32_t s, const uint8_t *p, size_t n)
{
    for (; n > 1; p += 2, n -= 2) s += get16(p);
    if (n) s += (uint32_t) p[0] << 8;
    return s;
}

static unsigned fold(uint32_t s)
{
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return ~s & 0xffff;
}

/* UDP and TCP: the sum over the pseudo header and the segment */
static unsigned l4_sum(const uint8_t *src, const uint8_t *dst, int proto,
                       const uint8_t *seg, size_t n)
{
    uint8_t ph[12];

    memcpy(ph, src, 4);
    memcpy(ph + 4, dst, 4);
    ph[8] = 0;
    ph[9] = (uint8_t) proto;
    put16(ph + 10, (unsigned) n);
    return fold(sum16(sum16(0, ph, 12), seg, n));
}

static int same_net(const uint8_t *a)
{
    int i;

    for (i = 0; i < 4; i++)
        if ((a[i] & mask[i]) != (my_ip[i] & mask[i])) return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Ethernet                                                           */
/* ------------------------------------------------------------------ */

/* The frame in tx: its header filled in here, then the card. */
static int eth_send(const uint8_t *dst, unsigned type, size_t len)
{
    clock_t end = clock() + CLOCKS_PER_SEC;
    EFI_STATUS s;
    void      *done;

    memcpy(tx, dst, 6);
    memcpy(tx + 6, mac, 6);
    put16(tx + 12, type);
    len += 14;
    if (len < 60) {                     /* the shortest frame Ethernet sends */
        memset(tx + len, 0, 60 - len);
        len = 60;
    }
    while ((s = snp->Transmit(snp, 0, len, tx, NULL, NULL, NULL))
               == EFI_NOT_READY && clock() < end)
        snp->GetStatus(snp, NULL, &done);
    if (s != EFI_SUCCESS) return -1;
    /* tx is used again at once: wait until the card hands it back */
    do {
        done = NULL;
        snp->GetStatus(snp, NULL, &done);
    } while (done == NULL && clock() < end);
    return 0;
}

/* ------------------------------------------------------------------ */
/* ARP                                                                */
/* ------------------------------------------------------------------ */

#define NARP 8
static int arp_only;            /* while resolving: tx holds a packet */
static struct { uint8_t ip[4], mac[6]; int used; } arp[NARP];
static int arp_next;

static void arp_learn(const uint8_t *ip, const uint8_t *hw)
{
    int i;

    for (i = 0; i < NARP; i++)
        if (arp[i].used && memcmp(arp[i].ip, ip, 4) == 0) {
            memcpy(arp[i].mac, hw, 6);
            return;
        }
    i = arp_next++ % NARP;
    memcpy(arp[i].ip, ip, 4);
    memcpy(arp[i].mac, hw, 6);
    arp[i].used = 1;
}

static int arp_find(const uint8_t *ip, uint8_t *hw)
{
    int i;

    for (i = 0; i < NARP; i++)
        if (arp[i].used && memcmp(arp[i].ip, ip, 4) == 0) {
            memcpy(hw, arp[i].mac, 6);
            return 0;
        }
    return -1;
}

static void arp_send(unsigned op, const uint8_t *thw, const uint8_t *tip,
                     const uint8_t *dst)
{
    uint8_t *a = tx + 14;

    put16(a, 1);                        /* Ethernet */
    put16(a + 2, 0x0800);               /* IPv4 */
    a[4] = 6;
    a[5] = 4;
    put16(a + 6, op);
    memcpy(a + 8, mac, 6);
    memcpy(a + 14, my_ip, 4);
    memcpy(a + 18, thw, 6);
    memcpy(a + 24, tip, 4);
    eth_send(dst, 0x0806, 28);
}

static void arp_in(const uint8_t *a, size_t n)
{
    if (n < 28 || get16(a) != 1 || get16(a + 2) != 0x0800) return;
    if (memcmp(a + 14, zero_ip, 4) != 0) arp_learn(a + 14, a + 8);
    if (get16(a + 6) == 1 && memcmp(a + 24, my_ip, 4) == 0 &&
        memcmp(my_ip, zero_ip, 4) != 0) {
        uint8_t who[4], hw[6];

        memcpy(who, a + 14, 4);
        memcpy(hw, a + 8, 6);
        arp_send(2, hw, who, hw);
    }
}

static void ip_in(const uint8_t *p, size_t n, const uint8_t *src_mac);

/* What the card has received, handled; at most `max` frames. */
static void poll_card(int max)
{
    while (max-- > 0) {
        UINTN size = sizeof rx;

        if (snp->Receive(snp, NULL, &size, rx, NULL, NULL, NULL) != EFI_SUCCESS)
            return;
        if (size < 14) continue;
        if (get16(rx + 12) == 0x0806) arp_in(rx + 14, size - 14);
        else if (get16(rx + 12) == 0x0800 && !arp_only)
            ip_in(rx + 14, size - 14, rx + 6);
    }
}

static void tcp_timers(void);

void ip_poll(void)
{
    if (snp == NULL) return;
    poll_card(32);
    if (!arp_only) tcp_timers();
}

/* Wait for *done, polling, for up to `ms`: 0, or -1 at the end. */
static int wait_until(volatile int *done, long ms)
{
    clock_t end = clock() + ms * CLOCKS_PER_SEC / 1000;

    while (!*done) {
        if (clock() >= end) return -1;
        ip_poll();
        efi_check_break();
        efi_bs->Stall(500);
    }
    return 0;
}

/* The next hop's hardware address: 0, or -1 if it does not answer. */
static int arp_resolve(const uint8_t *ip, uint8_t *hw)
{
    const uint8_t *hop = same_net(ip) ? ip : gw;
    int            tries;

    if (memcmp(ip, bcast_ip, 4) == 0) {
        memcpy(hw, bcast_mac, 6);
        return 0;
    }
    for (tries = 0; tries < 4; tries++) {
        if (arp_find(hop, hw) == 0) return 0;
        arp_send(1, (const uint8_t *) "\0\0\0\0\0\0", hop, bcast_mac);
        {
            clock_t end = clock() + CLOCKS_PER_SEC;

            while (clock() < end) {
                ip_poll();
                if (arp_find(hop, hw) == 0) return 0;
                efi_check_break();
                efi_bs->Stall(500);
            }
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* IPv4                                                               */
/* ------------------------------------------------------------------ */

static unsigned ip_id;

/* The n bytes at tx + 34 (after the Ethernet and IP headers) to dst. */
static int ip_send(const uint8_t *dst, int proto, size_t n)
{
    uint8_t  hw[6];
    uint8_t *h = tx + 14;

    if (memcmp(dst, bcast_ip, 4) == 0 ||
        arp_find(same_net(dst) ? dst : gw, hw) == 0) {
        if (memcmp(dst, bcast_ip, 4) == 0) memcpy(hw, bcast_mac, 6);
    } else {
        /* asking uses tx, and only ARP is handled meanwhile: what other
           frames come in now are sent again by their senders */
        static uint8_t save[sizeof tx];
        int            r;

        memcpy(save, tx + 34, n);
        arp_only = 1;
        r = arp_resolve(dst, hw);
        arp_only = 0;
        memcpy(tx + 34, save, n);
        if (r != 0) return -1;
    }
    h[0] = 0x45;
    h[1] = 0;
    put16(h + 2, (unsigned) (20 + n));
    put16(h + 4, ip_id++);
    put16(h + 6, 0x4000);               /* don't fragment */
    h[8] = 64;
    h[9] = (uint8_t) proto;
    put16(h + 10, 0);
    memcpy(h + 12, my_ip, 4);
    memcpy(h + 16, dst, 4);
    put16(h + 10, fold(sum16(0, h, 20)));
    return eth_send(hw, 0x0800, 20 + n);
}

static void udp_in(const uint8_t *ip, const uint8_t *u, size_t n);
static void tcp_in(const uint8_t *ip, const uint8_t *t, size_t n);

static void ip_in(const uint8_t *p, size_t n, const uint8_t *src_mac)
{
    size_t hl, total;

    if (n < 20 || (p[0] >> 4) != 4) return;
    hl = (size_t) (p[0] & 15) * 4;
    total = get16(p + 2);
    if (hl < 20 || total < hl || total > n || fold(sum16(0, p, hl)) != 0)
        return;
    if ((get16(p + 6) & 0x3fff) != 0) return;   /* a fragment */
    if (memcmp(p + 16, my_ip, 4) != 0 && memcmp(p + 16, bcast_ip, 4) != 0 &&
        memcmp(my_ip, zero_ip, 4) != 0)
        return;
    if (memcmp(p + 12, zero_ip, 4) != 0 && same_net(p + 12))
        arp_learn(p + 12, src_mac);
    switch (p[9]) {
    case 1:                             /* ICMP: answer a ping */
        if (total - hl >= 8 && p[hl] == 8 && memcmp(p + 16, my_ip, 4) == 0 &&
            total - hl <= sizeof tx - 34) {
            uint8_t from[4];
            size_t  k = total - hl;

            memcpy(from, p + 12, 4);
            memcpy(tx + 34, p + hl, k);
            tx[34] = 0;
            put16(tx + 36, 0);
            put16(tx + 36, fold(sum16(0, tx + 34, k)));
            ip_send(from, 1, k);
        }
        break;
    case 17:
        udp_in(p, p + hl, total - hl);
        break;
    case 6:
        tcp_in(p, p + hl, total - hl);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* UDP: one reply waited for at a time (DHCP, DNS)                    */
/* ------------------------------------------------------------------ */

static struct {
    unsigned     port;          /* the local port waited on; 0: none */
    uint8_t      data[1500];
    size_t       len;
    volatile int got;
} udp_wait;

static int udp_send(const uint8_t *dst, unsigned sport, unsigned dport,
                    const uint8_t *data, size_t n)
{
    uint8_t *u = tx + 34;

    if (n > sizeof tx - 42) return -1;
    memmove(u + 8, data, n);
    put16(u, sport);
    put16(u + 2, dport);
    put16(u + 4, (unsigned) (8 + n));
    put16(u + 6, 0);
    put16(u + 6, l4_sum(my_ip, dst, 17, u, 8 + n));
    return ip_send(dst, 17, 8 + n);
}

static void udp_in(const uint8_t *ip, const uint8_t *u, size_t n)
{
    size_t len;

    (void) ip;
    if (n < 8 || udp_wait.got || get16(u + 2) != udp_wait.port) return;
    len = get16(u + 4);
    if (len < 8 || len > n || len - 8 > sizeof udp_wait.data) return;
    memcpy(udp_wait.data, u + 8, len - 8);
    udp_wait.len = len - 8;
    udp_wait.got = 1;
}

/* ------------------------------------------------------------------ */
/* DHCP                                                               */
/* ------------------------------------------------------------------ */

/* The value of option `code` in a DHCP reply, and its length into *n. */
static const uint8_t *dhcp_opt(const uint8_t *d, size_t len, int code,
                               size_t *n)
{
    size_t i = 240;

    if (len < 240 || get32(d + 236) != 0x63825363) return NULL;
    while (i < len && d[i] != 255) {
        if (d[i] == 0) {
            i++;
            continue;
        }
        if (i + 1 >= len || i + 2 + d[i + 1] > len) return NULL;
        if (d[i] == code) {
            *n = d[i + 1];
            return d + i + 2;
        }
        i += 2 + d[i + 1];
    }
    return NULL;
}

/* Send a DISCOVER (type 1) or a REQUEST (3) for `want` from `server`,
   and wait up to `ms` for an answer of the type in `reply`: 0, or -1. */
static int dhcp_round(uint32_t xid, int type, const uint8_t *want,
                      const uint8_t *server, int reply, long ms)
{
    uint8_t m[300];
    size_t  k, n;
    const uint8_t *v;
    clock_t end;

    memset(m, 0, sizeof m);
    m[0] = 1;
    m[1] = 1;
    m[2] = 6;
    put32(m + 4, xid);
    put16(m + 10, 0x8000);              /* answer by broadcast */
    memcpy(m + 28, mac, 6);
    put32(m + 236, 0x63825363);
    k = 240;
    m[k++] = 53; m[k++] = 1; m[k++] = (uint8_t) type;
    if (type == 3) {
        m[k++] = 50; m[k++] = 4; memcpy(m + k, want, 4); k += 4;
        m[k++] = 54; m[k++] = 4; memcpy(m + k, server, 4); k += 4;
    }
    m[k++] = 55; m[k++] = 3; m[k++] = 1; m[k++] = 3; m[k++] = 6;
    m[k++] = 255;

    udp_wait.port = 68;
    udp_wait.got = 0;
    if (udp_send(bcast_ip, 68, 67, m, k < 300 ? 300 : k) != 0) return -1;
    end = clock() + ms * CLOCKS_PER_SEC / 1000;
    while (clock() < end) {
        if (wait_until(&udp_wait.got, 100) == 0) {
            const uint8_t *d = udp_wait.data;

            if (udp_wait.len >= 240 && d[0] == 2 && get32(d + 4) == xid &&
                (v = dhcp_opt(d, udp_wait.len, 53, &n)) != NULL && n == 1) {
                if (v[0] == reply) return 0;
                if (v[0] == 6) return -1;       /* NAK */
            }
            udp_wait.got = 0;
        }
    }
    return -1;
}

static int dhcp(void)
{
    uint32_t       xid = (uint32_t) rand() << 16 ^ (uint32_t) clock() ^ get32(mac + 2);
    uint8_t        offer[4], server[4];
    const uint8_t *v;
    size_t         n;
    int            tries;

    for (tries = 0; tries < 4; tries++) {
        if (dhcp_round(xid, 1, NULL, NULL, 2, 3000) != 0) continue;
        memcpy(offer, udp_wait.data + 16, 4);
        v = dhcp_opt(udp_wait.data, udp_wait.len, 54, &n);
        if (v == NULL || n != 4) continue;
        memcpy(server, v, 4);
        if (dhcp_round(xid, 3, offer, server, 5, 3000) != 0) continue;

        memcpy(my_ip, udp_wait.data + 16, 4);
        if ((v = dhcp_opt(udp_wait.data, udp_wait.len, 1, &n)) != NULL && n == 4)
            memcpy(mask, v, 4);
        else
            memcpy(mask, "\377\377\377\0", 4);
        if ((v = dhcp_opt(udp_wait.data, udp_wait.len, 3, &n)) != NULL && n >= 4)
            memcpy(gw, v, 4);
        if ((v = dhcp_opt(udp_wait.data, udp_wait.len, 6, &n)) != NULL && n >= 4)
            memcpy(dns_ip, v, 4);
        udp_wait.port = 0;
        return 0;
    }
    udp_wait.port = 0;
    return -1;
}

/* GORK_IP = "ADDRESS/BITS GATEWAY [DNS]": a static address. */
static int parse_ip(const char *s, uint8_t *out, const char **end)
{
    unsigned long v;
    char         *e;
    int           i;

    for (i = 0; i < 4; i++) {
        v = strtoul(s, &e, 10);
        if (e == s || v > 255 || (i < 3 && *e != '.')) return -1;
        out[i] = (uint8_t) v;
        s = i < 3 ? e + 1 : e;
    }
    *end = s;
    return 0;
}

static int static_ip(const char *s)
{
    const char   *e;
    unsigned long bits = 24;
    uint32_t      m;

    if (parse_ip(s, my_ip, &e) != 0) return -1;
    if (*e == '/') {
        bits = strtoul(e + 1, (char **) &e, 10);
        if (bits < 1 || bits > 30) return -1;
    }
    m = 0xffffffffu << (32 - bits);
    put32(mask, m);
    while (*e == ' ') e++;
    if (*e != '\0' && parse_ip(e, gw, &e) != 0) return -1;
    while (*e == ' ') e++;
    if (*e != '\0' && parse_ip(e, dns_ip, &e) != 0) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* starting                                                           */
/* ------------------------------------------------------------------ */

/* Something different at each start, for ports, sequence numbers and
   DHCP: each run of gork begins with clock() at 0, and a port used again
   too soon is refused by the other end. */
static unsigned seed(void)
{
    EFI_TIME t;
    uint64_t c = 0;

    memset(&t, 0, sizeof t);
    efi_st->RuntimeServices->GetTime(&t, NULL);
#if defined(__x86_64__)
    c = __builtin_ia32_rdtsc();
#elif defined(__aarch64__)
    __asm__ volatile ("mrs %0, cntvct_el0" : "=r" (c));
#endif
    return (unsigned) (c ^ c >> 32) ^ t.Nanosecond ^
           (unsigned) time(NULL) * 2654435761u ^ get32(mac + 2);
}

/* A network card that is up, and an address: 0, or -1 with a message. */
int ip_start(void)
{
    static int  tried;
    EFI_HANDLE *h = NULL;
    UINTN       n = 0, i;
    const char *conf = getenv("GORK_IP");

    if (snp != NULL) return 0;
    if (tried) return -1;
    tried = 1;
    if (efi_bs->LocateHandleBuffer(ByProtocol, &snp_guid, NULL, &n, &h)
            != EFI_SUCCESS || n == 0) {
        fprintf(stderr, "gork: no network card: the firmware has no driver "
                        "for it\n");
        return -1;
    }
    for (i = 0; i < n && snp == NULL; i++) {
        EFI_SIMPLE_NETWORK_PROTOCOL *s;

        if (efi_bs->HandleProtocol(h[i], &snp_guid, (void **) &s) != EFI_SUCCESS)
            continue;
        if (s->Mode->State == EfiSimpleNetworkStopped) s->Start(s);
        if (s->Mode->State == EfiSimpleNetworkStarted) s->Initialize(s, 0, 0);
        if (s->Mode->State != EfiSimpleNetworkInitialized) continue;
        if (s->Mode->MediaPresentSupported && !s->Mode->MediaPresent) continue;
        if (s->Mode->HwAddressSize != 6) continue;      /* Ethernet only */
        snp = s;
    }
    efi_bs->FreePool(h);
    if (snp == NULL) {
        fprintf(stderr, "gork: no network card is up (is a cable in?)\n");
        return -1;
    }
    snp->ReceiveFilters(snp, EFI_SIMPLE_NETWORK_RECEIVE_UNICAST |
                             EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST, 0, 0, 0, NULL);
    memcpy(mac, snp->Mode->CurrentAddress.Addr, 6);
    srand(seed());
    ip_id = (unsigned) rand();

    if (conf != NULL && *conf != '\0') {
        if (static_ip(conf) != 0) {
            fprintf(stderr, "gork: GORK_IP: want ADDRESS/BITS GATEWAY [DNS], "
                            "e.g. 192.168.1.50/24 192.168.1.1 192.168.1.1\n");
            snp = NULL;
            return -1;
        }
        return 0;
    }
    fprintf(stderr, "gork: waiting for an address (DHCP, gork's own)...\n");
    if (dhcp() != 0) {
        fprintf(stderr, "gork: no answer from DHCP; set an address with "
                        "`set GORK_IP 192.168.1.50/24 192.168.1.1`\n");
        snp = NULL;
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* DNS                                                                */
/* ------------------------------------------------------------------ */

/* past a name at d[i] in a reply of n bytes, or 0 if it runs off */
static size_t skip_name(const uint8_t *d, size_t i, size_t n)
{
    while (i < n) {
        if (d[i] == 0) return i + 1;
        if ((d[i] & 0xc0) == 0xc0) return i + 2;
        i += 1 + d[i];
    }
    return 0;
}

/* An address for `name`: 0, or -1 with errno. */
int ip_resolve(const char *name, uint8_t *out)
{
    uint8_t     q[300];
    size_t      k = 12, i, n;
    unsigned    id = (unsigned) rand() & 0xffff, port = 49152 + (rand() & 0x3fff);
    const char *p = name;
    int         tries;

    if (memcmp(dns_ip, zero_ip, 4) == 0) {
        errno = ENOENT;
        fprintf(stderr, "gork: no DNS server known: give the relay as an "
                        "address, or a DNS in GORK_IP\n");
        return -1;
    }
    memset(q, 0, sizeof q);
    put16(q, id);
    put16(q + 2, 0x0100);               /* recursion desired */
    put16(q + 4, 1);
    while (*p != '\0') {                /* the name, label by label */
        size_t len = strcspn(p, ".");

        if (len == 0 || len > 63 || k + len + 6 > sizeof q) {
            errno = EINVAL;
            return -1;
        }
        q[k++] = (uint8_t) len;
        memcpy(q + k, p, len);
        k += len;
        p += len;
        if (*p == '.') p++;
    }
    q[k++] = 0;
    put16(q + k, 1);                    /* A */
    put16(q + k + 2, 1);                /* IN */
    k += 4;

    for (tries = 0; tries < 3; tries++) {
        udp_wait.port = port;
        udp_wait.got = 0;
        if (udp_send(dns_ip, port, 53, q, k) != 0) continue;
        if (wait_until(&udp_wait.got, 3000) != 0) continue;
        {
            const uint8_t *d = udp_wait.data;
            unsigned       an;

            n = udp_wait.len;
            if (n < 12 || get16(d) != id || !(d[2] & 0x80)) continue;
            udp_wait.port = 0;
            if ((d[3] & 15) != 0) break;        /* no such name, or failure */
            an = get16(d + 6);
            i = skip_name(d, 12, n);
            if (i == 0 || i + 4 > n) break;
            i += 4;
            while (an-- > 0 && i != 0 && i + 10 <= n) {
                i = skip_name(d, i, n);
                if (i == 0 || i + 10 > n) break;
                if (get16(d + i) == 1 && get16(d + i + 2) == 1 &&
                    get16(d + i + 8) == 4 && i + 14 <= n) {
                    memcpy(out, d + i + 10, 4);
                    return 0;
                }
                i += 10 + get16(d + i + 8);
            }
            break;
        }
    }
    udp_wait.port = 0;
    errno = ENOENT;
    return -1;
}

/* ------------------------------------------------------------------ */
/* TCP: one connection                                                */
/* ------------------------------------------------------------------ */

#define T_FIN 0x01
#define T_SYN 0x02
#define T_RST 0x04
#define T_PSH 0x08
#define T_ACK 0x10

#define SBUF  32768             /* sent and not yet acknowledged, or not sent */
#define RBUF  65535             /* received and not yet read */

enum { CLOSED, SYN_SENT, OPEN };

static struct {
    int      state, error;
    uint8_t  rip[4];
    unsigned rport, lport;
    uint32_t iss, snd_una, snd_nxt, snd_max, rcv_nxt;   /* snd_max: the
                                   furthest sent, before any going back */
    unsigned snd_wnd, mss;
    uint8_t  sbuf[SBUF];
    size_t   slen;              /* bytes in sbuf, from snd_una on */
    uint8_t  rbuf[RBUF];
    size_t   rlen, rat;         /* rbuf[rat..rlen) is unread */
    int      fin_in;            /* the other end has closed */
    int      closing, fin_sent, fin_acked;
    clock_t  rto_at;            /* when to send again, or 0 */
    long     rto;               /* in ticks */
    int      retries;
} tc;

static unsigned next_port;

/* One segment from sequence number `seq`: flags and len bytes of sbuf. */
static void tcp_send(uint32_t seq, unsigned flags, size_t off, size_t len)
{
    uint8_t *t = tx + 34;
    size_t   hl = flags & T_SYN ? 24 : 20;
    size_t   free_ = RBUF - (tc.rlen - tc.rat);

    put16(t, tc.lport);
    put16(t + 2, tc.rport);
    put32(t + 4, seq);
    put32(t + 8, flags & T_ACK ? tc.rcv_nxt : 0);
    t[12] = (uint8_t) (hl / 4 << 4);
    t[13] = (uint8_t) flags;
    put16(t + 14, (unsigned) (free_ > 65535 ? 65535 : free_));
    put16(t + 16, 0);
    put16(t + 18, 0);
    if (flags & T_SYN) {                /* our MSS */
        t[20] = 2;
        t[21] = 4;
        put16(t + 22, 1460);
    }
    if (len > 0) memcpy(t + hl, tc.sbuf + off, len);
    put16(t + 16, l4_sum(my_ip, tc.rip, 6, t, hl + len));
    ip_send(tc.rip, 6, hl + len);
}

static void tcp_ack(void) { tcp_send(tc.snd_nxt, T_ACK, 0, 0); }

/* Send what the window allows, and a FIN once all is sent if closing. */
static void tcp_output(void)
{
    for (;;) {
        size_t   sent = tc.snd_nxt - tc.snd_una - (tc.fin_sent ? 1 : 0);
        size_t   wnd = tc.snd_wnd, room, part;

        if (tc.state != OPEN || sent >= tc.slen) break;
        room = wnd > sent ? wnd - sent : 0;
        if (room == 0) break;
        part = tc.slen - sent;
        if (part > room) part = room;
        if (part > tc.mss) part = tc.mss;
        tcp_send(tc.snd_nxt, T_ACK | T_PSH, sent, part);
        tc.snd_nxt += (uint32_t) part;
        if (tc.snd_nxt - tc.snd_max < 0x80000000u) tc.snd_max = tc.snd_nxt;
        if (tc.rto_at == 0) tc.rto_at = clock() + tc.rto;
    }
    if (tc.state == OPEN && tc.closing && !tc.fin_sent &&
        tc.snd_nxt - tc.snd_una == tc.slen) {
        tcp_send(tc.snd_nxt, T_ACK | T_FIN, 0, 0);
        tc.snd_nxt++;
        if (tc.snd_nxt - tc.snd_max < 0x80000000u) tc.snd_max = tc.snd_nxt;
        tc.fin_sent = 1;
        if (tc.rto_at == 0) tc.rto_at = clock() + tc.rto;
    }
}

static void tcp_fail(int e)
{
    tc.state = CLOSED;
    tc.error = e;
    tc.rto_at = 0;
}

static void tcp_timers(void)
{
    if (tc.state == CLOSED || tc.rto_at == 0 || clock() < tc.rto_at) return;
    if (++tc.retries > 8) {
        tcp_fail(ETIMEDOUT);
        return;
    }
    tc.rto = tc.rto * 2 > 16 * CLOCKS_PER_SEC ? 16 * CLOCKS_PER_SEC : tc.rto * 2;
    tc.rto_at = clock() + tc.rto;
    if (tc.state == SYN_SENT) {
        tcp_send(tc.iss, T_SYN, 0, 0);
        return;
    }
    /* go back to the oldest byte not acknowledged, and send from there */
    tc.snd_nxt = tc.snd_una;
    tc.fin_sent = 0;
    if (tc.snd_wnd == 0 && tc.slen > 0) {       /* probe a closed window */
        tcp_send(tc.snd_nxt, T_ACK, 0, 1);
        return;
    }
    tcp_output();
}

static void tcp_in(const uint8_t *ip, const uint8_t *t, size_t n)
{
    size_t   hl, len;
    unsigned flags;
    uint32_t seq, ack;

    if (n < 20 || tc.state == CLOSED || get16(t) != tc.rport ||
        get16(t + 2) != tc.lport || memcmp(ip + 12, tc.rip, 4) != 0)
        return;
    if (l4_sum(ip + 12, ip + 16, 6, t, n) != 0) return;
    hl = (size_t) (t[12] >> 4) * 4;
    if (hl < 20 || hl > n) return;
    flags = t[13];
    seq = get32(t + 4);
    ack = get32(t + 8);
    len = n - hl;

    if (flags & T_RST) {
        if (tc.state == SYN_SENT ? (flags & T_ACK) && ack == tc.iss + 1
                                 : seq == tc.rcv_nxt)
            tcp_fail(tc.state == SYN_SENT ? ECONNREFUSED : ECONNRESET);
        return;
    }

    if (tc.state == SYN_SENT) {
        size_t i;

        if ((flags & (T_SYN | T_ACK)) != (T_SYN | T_ACK) || ack != tc.iss + 1)
            return;
        tc.rcv_nxt = seq + 1;
        tc.snd_una = tc.snd_nxt = tc.snd_max = ack;
        tc.snd_wnd = get16(t + 14);
        tc.mss = 536;
        for (i = 20; i + 1 < hl;) {         /* the other end's MSS */
            if (t[i] == 0) break;
            if (t[i] == 1) {
                i++;
                continue;
            }
            if (t[i] == 2 && t[i + 1] == 4 && i + 4 <= hl)
                tc.mss = get16(t + i + 2);
            if (t[i + 1] < 2) break;
            i += t[i + 1];
        }
        if (tc.mss > 1460) tc.mss = 1460;
        if (tc.mss < 64) tc.mss = 64;
        tc.state = OPEN;
        tc.rto_at = 0;
        tc.retries = 0;
        tc.rto = CLOCKS_PER_SEC;
        tcp_ack();
        tcp_output();
        return;
    }

    if (flags & T_ACK) {
        uint32_t acked = ack - tc.snd_una;

        if (acked > 0 && acked <= tc.snd_max - tc.snd_una) {
            size_t data = acked;

            if (data > tc.slen) {               /* our FIN too */
                tc.fin_acked = 1;
                data = tc.slen;
            }
            memmove(tc.sbuf, tc.sbuf + data, tc.slen - data);
            tc.slen -= data;
            tc.snd_una = ack;
            if (ack - tc.snd_nxt < 0x80000000u) tc.snd_nxt = ack;
            tc.retries = 0;
            tc.rto = CLOCKS_PER_SEC;
            tc.rto_at = tc.snd_una == tc.snd_max ? 0 : clock() + tc.rto;
        }
        if (ack == tc.snd_una) tc.snd_wnd = get16(t + 14);
    }

    if (len > 0 || (flags & T_FIN)) {
        if (seq == tc.rcv_nxt && !tc.fin_in) {
            size_t take = len, room;

            if (tc.rat > 0 && tc.rlen + take > RBUF) {  /* make room */
                memmove(tc.rbuf, tc.rbuf + tc.rat, tc.rlen - tc.rat);
                tc.rlen -= tc.rat;
                tc.rat = 0;
            }
            room = RBUF - tc.rlen;
            if (take > room) take = room;
            memcpy(tc.rbuf + tc.rlen, t + hl, take);
            tc.rlen += take;
            tc.rcv_nxt += (uint32_t) take;
            if ((flags & T_FIN) && take == len) {
                tc.rcv_nxt++;
                tc.fin_in = 1;
            }
        }
        tcp_ack();          /* in order or not: say where we are */
    }
    tcp_output();
}

int ip_tcp_connect(const uint8_t *ip, unsigned port)
{
    clock_t end;

    if (snp == NULL && ip_start() != 0) {
        errno = ENETDOWN;
        return -1;
    }
    memset(&tc, 0, sizeof tc);
    memcpy(tc.rip, ip, 4);
    tc.rport = port;
    if (next_port == 0) next_port = 49152 + ((unsigned) rand() & 0x3fff);
    tc.lport = next_port++;
    if (next_port > 65000) next_port = 49152;
    tc.iss = (uint32_t) rand() << 16 ^ (uint32_t) rand() ^ (uint32_t) clock() << 8;
    tc.snd_una = tc.snd_nxt = tc.snd_max = tc.iss;
    tc.mss = 536;
    tc.rto = CLOCKS_PER_SEC;
    tc.state = SYN_SENT;
    tcp_send(tc.iss, T_SYN, 0, 0);
    if (tc.state == SYN_SENT) tc.rto_at = clock() + tc.rto;
    end = clock() + 30 * CLOCKS_PER_SEC;
    while (tc.state == SYN_SENT && clock() < end) {
        ip_poll();
        efi_check_break();
        efi_bs->Stall(500);
    }
    if (tc.state == OPEN) return 0;
    errno = tc.error != 0 ? tc.error : ETIMEDOUT;
    tc.state = CLOSED;
    return -1;
}

int ip_tcp_write(const void *p, size_t n)
{
    size_t k;

    while (tc.state == OPEN && tc.slen == SBUF) {   /* wait for room */
        ip_poll();
        efi_check_break();
        efi_bs->Stall(500);
    }
    if (tc.state != OPEN) {
        errno = tc.error != 0 ? tc.error : EPIPE;
        return -1;
    }
    k = SBUF - tc.slen;
    if (k > n) k = n;
    memcpy(tc.sbuf + tc.slen, p, k);
    tc.slen += k;
    tcp_output();
    return (int) k;
}

int ip_tcp_ready(void)
{
    ip_poll();
    return tc.rat < tc.rlen || tc.fin_in || tc.state != OPEN;
}

int ip_tcp_read(void *p, size_t n)
{
    while (tc.rat == tc.rlen && !tc.fin_in && tc.state == OPEN) {
        ip_poll();
        efi_check_break();
        efi_bs->Stall(500);
    }
    if (tc.rat < tc.rlen) {
        int window_was_shut = RBUF - (tc.rlen - tc.rat) < tc.mss;

        if (n > tc.rlen - tc.rat) n = tc.rlen - tc.rat;
        memcpy(p, tc.rbuf + tc.rat, n);
        tc.rat += n;
        if (tc.rat == tc.rlen) tc.rat = tc.rlen = 0;
        if (window_was_shut) tcp_ack();         /* tell it there is room */
        return (int) n;
    }
    if (tc.fin_in) return 0;
    errno = tc.error != 0 ? tc.error : ECONNRESET;
    return -1;
}

/* Our FIN once everything is sent, waited for up to 2 s; a RST if the
   other end had not finished. */
void ip_tcp_close(void)
{
    clock_t end = clock() + 2 * CLOCKS_PER_SEC;

    if (tc.state == OPEN) {
        if (!tc.fin_in) {
            tcp_send(tc.snd_nxt, T_RST | T_ACK, 0, 0);
        } else {
            tc.closing = 1;
            tcp_output();
            while (tc.state == OPEN && !tc.fin_acked && clock() < end) {
                ip_poll();
                efi_bs->Stall(500);
            }
        }
    } else if (tc.state == SYN_SENT) {
        tcp_send(tc.snd_nxt, T_RST, 0, 0);
    }
    tc.state = CLOSED;
    tc.rto_at = 0;
}
