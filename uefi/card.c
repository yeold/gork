/*
 * card.c -- the network card for ip.c on UEFI: SimpleNetwork, whole
 * Ethernet frames, for firmware with a card but no TCP4.
 */

#include "efi.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

void efi_check_break(void);

static EFI_GUID snp_guid = EFI_SIMPLE_NETWORK_PROTOCOL_GUID;
static EFI_SIMPLE_NETWORK_PROTOCOL *snp;

int card_open(uint8_t mac[6])
{
    EFI_HANDLE *h = NULL;
    UINTN       n = 0, i;

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
    return 0;
}

int card_send(const void *frame, size_t len)
{
    clock_t    end = clock() + CLOCKS_PER_SEC;
    EFI_STATUS s;
    void      *done;

    while ((s = snp->Transmit(snp, 0, len, (void *) frame, NULL, NULL, NULL))
               == EFI_NOT_READY && clock() < end)
        snp->GetStatus(snp, NULL, &done);
    if (s != EFI_SUCCESS) return -1;
    /* the frame is used again at once: wait until the card hands it back */
    do {
        done = NULL;
        snp->GetStatus(snp, NULL, &done);
    } while (done == NULL && clock() < end);
    return 0;
}

int card_recv(void *buf, size_t size)
{
    UINTN n = size;

    if (snp->Receive(snp, NULL, &n, buf, NULL, NULL, NULL) != EFI_SUCCESS)
        return 0;
    return (int) n;
}

void card_pause(void) { efi_bs->Stall(500); }

void card_break(void) { efi_check_break(); }

/* Each run of gork begins with clock() at 0, and a port used again too
   soon is refused by the other end: the time of day and the CPU's counter. */
unsigned card_mtu(void) { return 1500; }

unsigned card_seed(void)
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
    return (unsigned) (c ^ c >> 32) ^ t.Nanosecond;
}

const char *card_ip_hint(void)
{
    return "set GORK_IP 192.168.1.50/24 192.168.1.1";
}
