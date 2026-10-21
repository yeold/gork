/*
 * net.c -- BSD sockets for gork.efi, on the firmware's TCP4 protocol:
 * socket, connect, read, write, close and select (libc.c), for TCP over
 * IPv4, one connection at a time as relay.c makes them.  Names are
 * looked up with DNS4 when the firmware has it.  The first network card
 * with an address is used; one without gets DHCP started, and is waited
 * for.  Firmware with no TCP4 at all (U-Boot, a PC with its network
 * stack turned off) gets gork's own stack instead, ip.c, on the card
 * itself.
 *
 * Everything the firmware does is asynchronous: a call takes a token
 * whose event is signalled when it is done, and the stack only moves
 * while it is polled, so every wait here polls.
 */

#include "efi.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

int  efi_fd_new_socket(int sock);
int  efi_fd_socket(int fd);
void efi_check_break(void);

static EFI_GUID tcp_sb_guid = EFI_TCP4_SERVICE_BINDING_PROTOCOL_GUID;
static EFI_GUID tcp_guid    = EFI_TCP4_PROTOCOL_GUID;
static EFI_GUID cfg_guid    = EFI_IP4_CONFIG2_PROTOCOL_GUID;
static EFI_GUID dns_sb_guid = EFI_DNS4_SERVICE_BINDING_PROTOCOL_GUID;
static EFI_GUID dns_guid    = EFI_DNS4_PROTOCOL_GUID;

static EFI_HANDLE nic;          /* the card in use, once tcp_service() found it */

/* which stack: 0 not chosen yet, 1 the firmware's, 2 ip.c, -1 none */
static int stack;
static int fw_tcp;              /* the firmware has TCP4, address or not */

#define RX_SIZE 16384

struct sock {
    int                           used;
    int                           own;      /* on ip.c */
    EFI_SERVICE_BINDING_PROTOCOL *sb;
    EFI_HANDLE                    child;
    EFI_TCP4_PROTOCOL            *tcp;
    int                           connected, eof, error;
    /* the receive in flight, and what it brought that is not read yet */
    EFI_TCP4_IO_TOKEN             rx_token;
    EFI_TCP4_RECEIVE_DATA         rx_data;
    volatile int                  rx_done;
    int                           rx_pending;
    char                          rx_buf[RX_SIZE];
    size_t                        rx_len, rx_at;
};

#define NSOCK 4
static struct sock socks[NSOCK];

static void EFIAPI set_flag(EFI_EVENT e, void *flag)
{
    (void) e;
    *(volatile int *) flag = 1;
}

static int new_event(volatile int *flag, EFI_EVENT *e)
{
    *flag = 0;
    return efi_bs->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, set_flag,
                               (void *) flag, e) == EFI_SUCCESS ? 0 : -1;
}

void efi_net_poll(void)
{
    int i;

    for (i = 0; i < NSOCK; i++)
        if (socks[i].used && socks[i].tcp != NULL)
            socks[i].tcp->Poll(socks[i].tcp);
    if (stack == 2) ip_poll();
}

/* Wait for *flag, polling tcp, for at most `secs`: 0, or -1 at the end. */
static int wait_for(EFI_TCP4_PROTOCOL *tcp, volatile int *flag, long secs)
{
    clock_t end = clock() + secs * CLOCKS_PER_SEC;

    while (!*flag) {
        if (clock() >= end) return -1;
        if (tcp != NULL) tcp->Poll(tcp);
        efi_check_break();
        efi_bs->Stall(1000);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* the network card, and its address                                  */
/* ------------------------------------------------------------------ */

static int has_address(EFI_IP4_CONFIG2_PROTOCOL *cfg)
{
    EFI_IP4_CONFIG2_INTERFACE_INFO info;
    UINTN                          size = sizeof info;
    EFI_STATUS                     s;

    s = cfg->GetData(cfg, Ip4Config2DataTypeInterfaceInfo, &size, &info);
    if (s == EFI_BUFFER_TOO_SMALL) {
        /* a route table follows: fetch it all, look at the start */
        char *big = malloc(size);

        if (big == NULL) return 0;
        s = cfg->GetData(cfg, Ip4Config2DataTypeInterfaceInfo, &size, big);
        if (s == EFI_SUCCESS) memcpy(&info, big, sizeof info);
        free(big);
    }
    return s == EFI_SUCCESS && (info.StationAddress.Addr[0] |
                                info.StationAddress.Addr[1] |
                                info.StationAddress.Addr[2] |
                                info.StationAddress.Addr[3]) != 0;
}

/* The TCP4 service of a card with an address, starting DHCP on each one
   without and waiting up to 20 seconds.  NULL, with a message, if none. */
static EFI_SERVICE_BINDING_PROTOCOL *tcp_service(void)
{
    static EFI_SERVICE_BINDING_PROTOCOL *found;
    EFI_HANDLE                *h = NULL;
    UINTN                      n = 0, i;
    clock_t                    end;
    int                        asked = 0;

    if (found != NULL) return found;
    if (efi_bs->LocateHandleBuffer(ByProtocol, &tcp_sb_guid, NULL, &n, &h)
            != EFI_SUCCESS || n == 0) {
        /* Booted from a disk, the firmware may not have started its
           network drivers on the card: do what `connect -r` does. */
        EFI_HANDLE *all = NULL;
        UINTN       na = 0;

        if (efi_bs->LocateHandleBuffer(AllHandles, NULL, NULL, &na, &all)
                == EFI_SUCCESS) {
            for (i = 0; i < na; i++)
                efi_bs->ConnectController(all[i], NULL, NULL, 1);
            efi_bs->FreePool(all);
        }
        n = 0;
        h = NULL;
    }
    if (n == 0 &&
        (efi_bs->LocateHandleBuffer(ByProtocol, &tcp_sb_guid, NULL, &n, &h)
            != EFI_SUCCESS || n == 0)) {
        n = 0;
        return NULL;            /* choose_stack() takes ip.c then */
    }
    fw_tcp = 1;
    end = clock() + 20 * CLOCKS_PER_SEC;
    for (;;) {
        for (i = 0; i < n; i++) {
            EFI_IP4_CONFIG2_PROTOCOL *cfg;

            if (efi_bs->HandleProtocol(h[i], &cfg_guid, (void **) &cfg)
                    != EFI_SUCCESS)
                continue;
            if (has_address(cfg)) {
                nic = h[i];
                efi_bs->HandleProtocol(h[i], &tcp_sb_guid, (void **) &found);
                efi_bs->FreePool(h);
                return found;
            }
            if (!asked) {
                /* EDK2 starts cards static, with no address: what
                   `ifconfig -s eth0 dhcp` does.  A static address set on
                   purpose was found above and is left alone. */
                uint32_t policy = Ip4Config2PolicyDhcp;

                cfg->SetData(cfg, Ip4Config2DataTypePolicy, sizeof policy,
                             &policy);
            }
        }
        if (!asked) {
            fprintf(stderr, "gork: waiting for an address (DHCP)...\n");
            asked = 1;
        }
        if (clock() >= end) break;
        efi_check_break();
        efi_bs->Stall(100000);
    }
    efi_bs->FreePool(h);
    fprintf(stderr, "gork: no network card got an address; set one with "
                    "the shell's ifconfig\n");
    return NULL;
}

/* The firmware's TCP4 if it has one, else gork's own: 1 or 2, or -1
   with a message.  Never both: the firmware's stack, once loaded, takes
   the card's frames itself. */
static int choose_stack(void)
{
    if (stack != 0) return stack;
    if (tcp_service() != NULL) return stack = 1;
    if (fw_tcp) return stack = -1;          /* TCP4, but no address */
    return stack = ip_start() == 0 ? 2 : -1;
}

/* ------------------------------------------------------------------ */
/* sockets                                                            */
/* ------------------------------------------------------------------ */

int socket(int domain, int type, int proto)
{
    int i, fd;

    (void) proto;
    if (domain != AF_INET || type != SOCK_STREAM) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    for (i = 0; i < NSOCK && socks[i].used; i++)
        ;
    if (i == NSOCK) {
        errno = EMFILE;
        return -1;
    }
    memset(&socks[i], 0, sizeof socks[i]);
    socks[i].used = 1;
    if ((fd = efi_fd_new_socket(i)) < 0) socks[i].used = 0;
    return fd;
}

static struct sock *get_sock(int fd)
{
    int i = efi_fd_socket(fd);

    if (i < 0) {
        errno = ENOTSOCK;
        return NULL;
    }
    return &socks[i];
}

int connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    const struct sockaddr_in *in = (const struct sockaddr_in *) addr;
    struct sock              *k = get_sock(fd);
    EFI_TCP4_CONFIG_DATA      cd;
    EFI_TCP4_COMPLETION_TOKEN tok;
    volatile int              done;
    EFI_STATUS                s;
    clock_t                   end;
    uint32_t                  ip;

    if (k == NULL) return -1;
    if (len < sizeof *in || in->sin_family != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    if (choose_stack() < 0) {
        errno = ENETDOWN;
        return -1;
    }
    if (stack == 2) {
        uint8_t a[4];

        memcpy(a, &in->sin_addr.s_addr, 4);
        if (ip_tcp_connect(a, ntohs(in->sin_port)) != 0) return -1;
        k->own = 1;
        k->connected = 1;
        return 0;
    }
    k->sb = tcp_service();
    if ((s = k->sb->CreateChild(k->sb, &k->child)) != EFI_SUCCESS ||
        (s = efi_bs->HandleProtocol(k->child, &tcp_guid, (void **) &k->tcp))
            != EFI_SUCCESS) {
        errno = efi_errno(s);
        return -1;
    }

    memset(&cd, 0, sizeof cd);
    cd.TimeToLive = 64;
    cd.AccessPoint.UseDefaultAddress = 1;
    ip = in->sin_addr.s_addr;            /* network order: bytes as written */
    memcpy(cd.AccessPoint.RemoteAddress.Addr, &ip, 4);
    cd.AccessPoint.RemotePort = ntohs(in->sin_port);
    cd.AccessPoint.ActiveFlag = 1;
    /* EFI_NO_MAPPING while the address is still being set up */
    end = clock() + 10 * CLOCKS_PER_SEC;
    while ((s = k->tcp->Configure(k->tcp, &cd)) == EFI_NO_MAPPING &&
           clock() < end) {
        efi_check_break();
        efi_bs->Stall(100000);
    }
    if (s != EFI_SUCCESS) {
        errno = efi_errno(s);
        return -1;
    }

    if (new_event(&done, &tok.Event) != 0) {
        errno = ENOMEM;
        return -1;
    }
    tok.Status = EFI_SUCCESS;
    s = k->tcp->Connect(k->tcp, &tok);
    if (s == EFI_SUCCESS && wait_for(k->tcp, &done, 30) != 0) {
        k->tcp->Cancel(k->tcp, &tok);
        tok.Status = EFI_TIMEOUT;
    }
    efi_bs->CloseEvent(tok.Event);
    if (s == EFI_SUCCESS) s = tok.Status;
    if (s != EFI_SUCCESS) {
        errno = efi_errno(s);
        return -1;
    }
    k->connected = 1;
    return 0;
}

/* Start a receive into rx_buf if none is in flight. */
static void rx_start(struct sock *k)
{
    if (k->rx_pending || k->eof || k->error || !k->connected) return;
    if (k->rx_token.CompletionToken.Event == NULL &&
        new_event(&k->rx_done, &k->rx_token.CompletionToken.Event) != 0) {
        k->error = ENOMEM;
        return;
    }
    k->rx_done = 0;
    k->rx_data.UrgentFlag = 0;
    k->rx_data.DataLength = RX_SIZE;
    k->rx_data.FragmentCount = 1;
    k->rx_data.FragmentTable[0].FragmentLength = RX_SIZE;
    k->rx_data.FragmentTable[0].FragmentBuffer = k->rx_buf;
    k->rx_token.CompletionToken.Status = EFI_SUCCESS;
    k->rx_token.Packet.RxData = &k->rx_data;
    if (k->tcp->Receive(k->tcp, &k->rx_token) != EFI_SUCCESS) {
        k->eof = 1;                     /* closed by now */
        return;
    }
    k->rx_pending = 1;
}

/* A finished receive: its data into rx_len, or the end. */
static void rx_finish(struct sock *k)
{
    EFI_STATUS s;

    if (!k->rx_pending || !k->rx_done) return;
    k->rx_pending = 0;
    s = k->rx_token.CompletionToken.Status;
    if (s == EFI_SUCCESS) {
        k->rx_len = k->rx_data.DataLength;
        k->rx_at = 0;
    } else if (s == EFI_CONNECTION_FIN) {
        k->eof = 1;
    } else {
        k->error = efi_errno(s);
    }
}

int efi_sock_ready(int i)
{
    struct sock *k = &socks[i];

    if (k->own) return ip_tcp_ready();
    if (k->rx_at < k->rx_len || k->eof || k->error) return 1;
    rx_start(k);
    k->tcp->Poll(k->tcp);
    rx_finish(k);
    return k->rx_at < k->rx_len || k->eof || k->error;
}

int efi_sock_read(int i, void *p, size_t n)
{
    struct sock *k = &socks[i];

    if (k->own) return ip_tcp_read(p, n);
    while (k->rx_at == k->rx_len && !k->eof && !k->error) {
        rx_start(k);
        if (k->rx_pending) wait_for(k->tcp, &k->rx_done, 3600);
        rx_finish(k);
    }
    if (k->rx_at < k->rx_len) {
        if (n > k->rx_len - k->rx_at) n = k->rx_len - k->rx_at;
        memcpy(p, k->rx_buf + k->rx_at, n);
        k->rx_at += n;
        return (int) n;
    }
    if (k->eof) return 0;
    errno = k->error;
    return -1;
}

int efi_sock_write(int i, const void *p, size_t n)
{
    struct sock           *k = &socks[i];
    EFI_TCP4_IO_TOKEN      tok;
    EFI_TCP4_TRANSMIT_DATA tx;
    volatile int           done;
    EFI_STATUS             s;
    size_t                 part = n > 32768 ? 32768 : n;

    if (k->own) return ip_tcp_write(p, n);
    if (!k->connected) {
        errno = EBADF;
        return -1;
    }
    if (new_event(&done, &tok.CompletionToken.Event) != 0) {
        errno = ENOMEM;
        return -1;
    }
    memset(&tx, 0, sizeof tx);
    tx.Push = 1;
    tx.DataLength = (uint32_t) part;
    tx.FragmentCount = 1;
    tx.FragmentTable[0].FragmentLength = (uint32_t) part;
    tx.FragmentTable[0].FragmentBuffer = (void *) p;
    tok.CompletionToken.Status = EFI_SUCCESS;
    tok.Packet.TxData = &tx;
    s = k->tcp->Transmit(k->tcp, &tok);
    if (s == EFI_SUCCESS && wait_for(k->tcp, &done, 120) != 0) {
        k->tcp->Cancel(k->tcp, &tok.CompletionToken);
        tok.CompletionToken.Status = EFI_TIMEOUT;
    }
    efi_bs->CloseEvent(tok.CompletionToken.Event);
    if (s == EFI_SUCCESS) s = tok.CompletionToken.Status;
    if (s != EFI_SUCCESS) {
        errno = efi_errno(s);
        return -1;
    }
    return (int) part;
}

int efi_sock_close(int i)
{
    struct sock         *k = &socks[i];
    EFI_TCP4_CLOSE_TOKEN tok;
    volatile int         done;

    if (k->own) ip_tcp_close();
    if (k->tcp != NULL) {
        if (k->rx_pending) {
            k->tcp->Cancel(k->tcp, &k->rx_token.CompletionToken);
            k->tcp->Poll(k->tcp);
        }
        if (k->connected && new_event(&done, &tok.CompletionToken.Event) == 0) {
            tok.AbortOnClose = k->error != 0 || !k->eof;
            tok.CompletionToken.Status = EFI_SUCCESS;
            if (k->tcp->Close(k->tcp, &tok) == EFI_SUCCESS)
                wait_for(k->tcp, &done, 2);
            efi_bs->CloseEvent(tok.CompletionToken.Event);
        }
        k->tcp->Configure(k->tcp, NULL);
    }
    if (k->rx_token.CompletionToken.Event != NULL)
        efi_bs->CloseEvent(k->rx_token.CompletionToken.Event);
    if (k->child != NULL) k->sb->DestroyChild(k->sb, k->child);
    memset(k, 0, sizeof *k);
    return 0;
}

void efi_net_end(void)
{
    int i;

    for (i = 0; i < NSOCK; i++)
        if (socks[i].used) efi_sock_close(i);
}

ssize_t send(int fd, const void *p, size_t n, int flags)
{
    struct sock *k = get_sock(fd);

    (void) flags;
    return k == NULL ? -1 : efi_sock_write((int) (k - socks), p, n);
}

ssize_t recv(int fd, void *p, size_t n, int flags)
{
    struct sock *k = get_sock(fd);

    (void) flags;
    return k == NULL ? -1 : efi_sock_read((int) (k - socks), p, n);
}

int shutdown(int fd, int how)
{
    (void) fd;
    (void) how;
    return 0;
}

/* ------------------------------------------------------------------ */
/* addresses                                                          */
/* ------------------------------------------------------------------ */

in_addr_t inet_addr(const char *s)
{
    unsigned long  part[4];
    unsigned char  b[4];
    in_addr_t      v;
    int            i;
    char          *end;

    for (i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9') return INADDR_NONE;
        part[i] = strtoul(s, &end, 10);
        if (part[i] > 255 || (i < 3 && *end != '.') || (i == 3 && *end != '\0'))
            return INADDR_NONE;
        s = end + 1;
    }
    for (i = 0; i < 4; i++) b[i] = (unsigned char) part[i];
    memcpy(&v, b, 4);
    return v;
}

/* A name, through the firmware's DNS4 and the servers DHCP gave (or the
   shell's `ifconfig -s eth0 static ... DNS` set): 0, or -1 with a
   message. */
static int dns_lookup(const char *name, in_addr_t *out)
{
    EFI_SERVICE_BINDING_PROTOCOL *sb;
    EFI_HANDLE                    child = NULL;
    EFI_DNS4_PROTOCOL            *dns;
    EFI_DNS4_CONFIG_DATA          cd;
    EFI_DNS4_COMPLETION_TOKEN     tok;
    volatile int                  done;
    CHAR16                       *w;
    EFI_STATUS                    s;
    int                           rc = -1;

    if (choose_stack() < 0) return -1;
    if (stack == 2) return ip_resolve(name, (uint8_t *) out) == 0 ? 0 : -1;
    if (efi_bs->HandleProtocol(nic, &dns_sb_guid, (void **) &sb) != EFI_SUCCESS) {
        fprintf(stderr, "gork: %s: the firmware has no DNS: give the relay "
                        "as an address\n", name);
        return -1;
    }
    if (sb->CreateChild(sb, &child) != EFI_SUCCESS ||
        efi_bs->HandleProtocol(child, &dns_guid, (void **) &dns) != EFI_SUCCESS) {
        fprintf(stderr, "gork: %s: cannot start DNS\n", name);
        goto out;
    }
    memset(&cd, 0, sizeof cd);
    cd.UseDefaultSetting = 1;
    cd.EnableDnsCache = 1;
    cd.Protocol = 17;
    cd.RetryCount = 3;
    cd.RetryInterval = 2;
    if ((s = dns->Configure(dns, &cd)) != EFI_SUCCESS) {
        fprintf(stderr, "gork: %s: cannot set up DNS (%s): no DNS server "
                        "known?\n", name, strerror(efi_errno(s)));
        goto out;
    }
    if ((w = efi_wide(name, 0)) == NULL || new_event(&done, &tok.Event) != 0) {
        free(w);
        goto out;
    }
    tok.Status = EFI_SUCCESS;
    tok.RetryCount = 3;
    tok.RetryInterval = 2;
    tok.RspData.H2AData = NULL;
    s = dns->HostNameToIp(dns, w, &tok);
    if (s == EFI_SUCCESS) {
        clock_t end = clock() + 15 * CLOCKS_PER_SEC;

        while (!done && clock() < end) {
            dns->Poll(dns);
            efi_check_break();
            efi_bs->Stall(1000);
        }
        if (!done) {
            dns->Cancel(dns, &tok);
            s = EFI_TIMEOUT;
        } else {
            s = tok.Status;
        }
    }
    efi_bs->CloseEvent(tok.Event);
    free(w);
    if (s == EFI_SUCCESS && tok.RspData.H2AData != NULL &&
        tok.RspData.H2AData->IpCount > 0) {
        memcpy(out, tok.RspData.H2AData->IpList[0].Addr, 4);
        rc = 0;
    } else if (s != EFI_SUCCESS && s != EFI_NOT_FOUND) {
        /* relay.c says the name did not resolve; this says why */
        fprintf(stderr, "gork: DNS: %s\n", strerror(efi_errno(s)));
    }
    if (tok.RspData.H2AData != NULL) {          /* the caller's to free */
        if (tok.RspData.H2AData->IpList != NULL)
            efi_bs->FreePool(tok.RspData.H2AData->IpList);
        efi_bs->FreePool(tok.RspData.H2AData);
    }
    dns->Configure(dns, NULL);
out:
    if (child != NULL) sb->DestroyChild(sb, child);
    return rc;
}

/* A dotted address, or a name for dns_lookup(). */
struct hostent *gethostbyname(const char *name)
{
    static struct hostent he;
    static in_addr_t      addr;
    static char          *list[2];

    addr = inet_addr(name);
    if (addr == INADDR_NONE && dns_lookup(name, &addr) != 0) return NULL;
    list[0] = (char *) &addr;
    list[1] = NULL;
    he.h_name = (char *) name;
    he.h_addrtype = AF_INET;
    he.h_length = 4;
    he.h_addr_list = list;
    return &he;
}
