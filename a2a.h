#ifndef GORK_A2A_H
#define GORK_A2A_H

#ifdef GORK_A2A

/* Serve A2A on TCP port.  Blocks forever.  Returns 0 on clean shutdown,
   -1 on error.  port 0 means default (8445). */
int a2a_serve(int port);

#endif /* GORK_A2A */

#endif /* GORK_A2A_H */