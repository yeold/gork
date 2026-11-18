/*
 * acp.h -- Agent Client Protocol (ACP) front end: gork as an agent an editor
 * drives over JSON-RPC 2.0 on stdin/stdout.  main.c owns the conversation,
 * the tools and the config; acp.c owns the wire.
 *
 *   https://agentclientprotocol.com/protocol/v1/
 */
#ifndef GORK_ACP_H
#define GORK_ACP_H

struct acp_hooks {
    /* A fresh conversation confined to `cwd`.  NULL on OOM. */
    void *(*conv_new)(const char *cwd);
    void  (*conv_free)(void *conv);

    /* Run one prompt through the agent loop, and return the ACP stopReason:
       "end_turn", "cancelled" or "max_turn_requests". */
    const char *(*run)(void *conv, const char *text);

    /* Tokens in the conversation now, for usage_update.  0 = not known. */
    long  (*tokens)(void *conv);

    int    ctx_window;              /* for usage_update's `size` */
};

/* Serve ACP on stdin/stdout until the client closes it or sends `exit`. */
int acp_serve(const struct acp_hooks *h);

/* Notifications and the permission ask, called from main.c during a turn.
   The string arguments are borrowed, not taken. */
void acp_text(const char *text);
void acp_tool_start(const char *tool_call_id, const char *name,
                    const char *kind, const char *title);
void acp_tool_done(const char *tool_call_id, int failed);
void acp_usage(void);
int  acp_ask(const char *tool_call_id, const char *title);

/* 1 if the client has cancelled the turn that is running.  main.c's
   check_abort calls it while a reply is in flight, so it must not block:
   it reads only what select() says is already waiting on stdin. */
int  acp_cancelled(void);

#endif /* GORK_ACP_H */
