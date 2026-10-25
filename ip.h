/*
 * ip.h -- gork's own TCP/IP (ip.c), for firmware that has a network card
 * but no TCP, and the network card it needs from the port.
 */
#ifndef GORK_IP_H
#define GORK_IP_H

#include <stddef.h>
#include <stdint.h>

/* ip.c: one TCP connection at a time; -1 with errno on failure. */
int  ip_start(void);            /* the card, and an address: DHCP or GORK_IP */
void ip_poll(void);             /* what has arrived, and the TCP timers */
int  ip_resolve(const char *name, uint8_t *addr);
int  ip_tcp_connect(const uint8_t *addr, unsigned port);
int  ip_tcp_write(const void *p, size_t n);
int  ip_tcp_read(void *p, size_t n);
int  ip_tcp_ready(void);
void ip_tcp_close(void);

/* The port's network card: whole Ethernet frames, polled. */
int  card_open(uint8_t mac[6]); /* 0 with its address, or -1 with a message */
int  card_send(const void *frame, size_t len);  /* 0, or -1; the frame may
                                                   be reused on return */
int  card_recv(void *buf, size_t size);         /* a frame's length, or 0 */
void card_pause(void);          /* about half a millisecond, between polls */
void card_break(void);          /* the user's interrupt, if they asked */
unsigned card_seed(void);       /* something different at each start */
const char *card_ip_hint(void); /* how to set GORK_IP, as a command */

#endif
