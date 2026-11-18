/*
 * acp.c -- Agent Client Protocol front end.  JSON-RPC 2.0, one object per
 * line, on stdin and stdout: the editor is the client, gork is the agent.
 *
 * ACP shares JSON-RPC with MCP, so mcp.c is the shape to copy, and its poll
 * loop while a reply is in flight is the precedent for doing a request and a
 * reply over the same pair of pipes -- which is what a permission ask needs
 * in the middle of a prompt turn.  Here the pair is stdin and stdout.
 *
 * Only framing, method dispatch, the notifications and the permission round
 * trip live here.  The conversation, the tools, the approval decision, the
 * confinement root and the streaming text belong to main.c, which fills in
 * `struct acp_hooks`.  stdout carries protocol messages and nothing else;
 * a human reads stderr.
 *
 *   https://agentclientprotocol.com/protocol/v1/
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>           /* fd_set, select */
#include <unistd.h>

#include "port.h"
#include "acp.h"
#include "relay.h"              /* struct buf */
#include "cJSON/cJSON.h"

#ifndef GORK_NO_FORK

static int         cancelled;   /* the client cancelled this turn */
static char        session_id[64];
static int         have_session;
static int         next_id = 100;   /* gork's own requests start high */
static const struct acp_hooks *hooks;

static int  dispatch(cJSON *msg);   /* acp_ask reads while a turn runs */

/* main.c's conversation for this session, opaque here. */
static void       *conv;

/* ------------------------------------------------------------------ output */

static void send_line(cJSON *msg)
{
    char  *s;
    size_t n;

    if (msg == NULL) return;
    s = cJSON_PrintUnformatted(msg);
    if (s == NULL) return;
    n = strlen(s);
    /* A JSON string holds no raw newline, so this is exactly one line. */
    if (fwrite(s, 1, n, stdout) == n && fputc('\n', stdout) != EOF)
        fflush(stdout);
    free(s);
}

static void send_result(cJSON *id, cJSON *result)
{
    cJSON *m = cJSON_CreateObject();
    cJSON *r = result != NULL ? result : cJSON_CreateObject();

    if (m == NULL || r == NULL) {
        cJSON_Delete(m);
        cJSON_Delete(r);
        return;
    }
    cJSON_AddStringToObject(m, "jsonrpc", "2.0");
    if (id != NULL) cJSON_AddItemToObject(m, "id", cJSON_Duplicate(id, 1));
    cJSON_AddItemToObject(m, "result", r);
    send_line(m);
    cJSON_Delete(m);
}

static void send_error(cJSON *id, int code, const char *msg)
{
    cJSON *m = cJSON_CreateObject(), *e;

    if (m == NULL) return;
    cJSON_AddStringToObject(m, "jsonrpc", "2.0");
    if (id != NULL) cJSON_AddItemToObject(m, "id", cJSON_Duplicate(id, 1));
    e = cJSON_CreateObject();
    if (e == NULL || cJSON_AddNumberToObject(e, "code", (double) code) == NULL ||
        cJSON_AddStringToObject(e, "message", msg) == NULL) {
        cJSON_Delete(e);
        cJSON_Delete(m);
        return;
    }
    cJSON_AddItemToObject(m, "error", e);
    send_line(m);
    cJSON_Delete(m);
}

/* One session/update notification.  `update` is owned. */
static void notify(cJSON *update)
{
    cJSON *m, *p;

    if (update == NULL) return;
    if (!have_session) {
        cJSON_Delete(update);
        return;
    }
    m = cJSON_CreateObject();
    p = cJSON_CreateObject();
    if (m == NULL || p == NULL ||
        cJSON_AddStringToObject(p, "sessionId", session_id) == NULL) {
        cJSON_Delete(m);
        cJSON_Delete(p);
        cJSON_Delete(update);
        return;
    }
    cJSON_AddItemToObject(p, "update", update);
    cJSON_AddStringToObject(m, "jsonrpc", "2.0");
    cJSON_AddStringToObject(m, "method", "session/update");
    cJSON_AddItemToObject(m, "params", p);
    send_line(m);
    cJSON_Delete(m);
}

static cJSON *update_of(const char *what)
{
    cJSON *u = cJSON_CreateObject();

    if (u != NULL && cJSON_AddStringToObject(u, "sessionUpdate", what) == NULL) {
        cJSON_Delete(u);
        return NULL;
    }
    return u;
}

/* ------------------------------------------------------------------ input
 *
 * stdin is read with read() and select(), not stdio, for one reason: a
 * session/cancel has to be seen *while* a prompt runs.  relay.c polls
 * relay_abort every 200 ms and main.c's check_abort asks acp_cancelled();
 * this side then reads only what select() says is already waiting, so the
 * answer's own reader never blocks behind an editor that has gone quiet.
 *
 * Complete lines queue up as raw text (a message cannot nest: a
 * session/prompt that arrives mid-turn waits its turn), except a cancel,
 * which is taken out of the queue the moment it lands.
 */

static struct buf rx;           /* bytes of the line being read, no '\n' */
static struct buf queue;        /* complete lines waiting, '\n'-separated */
static int        rx_eof;

/* main() reaches acp_cancelled() before the serve loop has read anything
   only in theory, but a NULL `queue` here would be a segfault, not a gap. */
static int input_init(void)
{
    if (queue.p == NULL && buf_init(&queue) != 0) return -1;
    if (rx.p == NULL && buf_init(&rx) != 0) return -1;
    return 0;
}

/* 1 if stdin has bytes now, or after waiting when `block`; -1 at end. */
static int stdin_wait(int block)
{
    fd_set         rd;
    struct timeval tv;
    int            r;

    if (rx_eof) return -1;
    FD_ZERO(&rd);
    FD_SET(0, &rd);
    tv.tv_sec  = 0;
    tv.tv_usec = 0;
    r = select(1, &rd, NULL, NULL, block ? NULL : &tv);
    return r > 0 ? 1 : 0;
}

/* Read what is waiting into `queue`, one complete line at a time.  Blocks
   only with `block`.  Returns 1 on progress, 0 for nothing ready (or EINTR),
   -1 at end of input. */
static int pump(int block)
{
    char   chunk[1024];
    long   n;
    size_t i, seg;
    int    ready;

    ready = stdin_wait(block);
    if (ready <= 0) return ready;
    n = (long) read(0, chunk, sizeof chunk);
    if (n <= 0) {
        if (n < 0 && errno == EINTR) return 0;
        rx_eof = 1;
        return -1;
    }
    seg = 0;
    for (i = 0; i < (size_t) n; i++) {
        if (chunk[i] != '\n') continue;
        if (buf_add(&rx, chunk + seg, i - seg) != 0 ||
            (rx.len > 0 && buf_add(&queue, rx.p, rx.len) != 0) ||
            buf_add(&queue, "\n", 1) != 0)
            goto oom;
        rx.len = 0;
        rx.p[0] = '\0';
        seg = i + 1;
    }
    if (seg < (size_t) n && buf_add(&rx, chunk + seg, (size_t) n - seg) != 0)
        goto oom;
    return 1;
oom:
    /* Losing the client's input is not survivable mid-session: stop reading
       and let the session end the way it does at end of input. */
    fprintf(stderr, "gork: out of memory reading ACP input\n");
    rx_eof = 1;
    return -1;
}

/* The next complete message, or NULL at end of input (or, without `block`,
   when none has arrived).  A blank or unparsable line is skipped rather
   than fatal: a client that logs to stdout would otherwise stop the
   session. */
static cJSON *next_message(int block)
{
    cJSON *msg;
    char  *nl;
    size_t n, rest;

    if (input_init() != 0) return NULL;
    for (;;) {
        if (queue.len > 0 &&
            (nl = (char *) memchr(queue.p, '\n', queue.len)) != NULL) {
            n = (size_t) (nl - queue.p);
            *nl = '\0';                 /* parse the line in place */
            msg = n > 0 ? cJSON_Parse(queue.p) : NULL;
            rest = queue.len - (n + 1);
            memmove(queue.p, nl + 1, rest);
            queue.len = rest;
            queue.p[queue.len] = '\0';
            if (msg != NULL) return msg;
            continue;
        }
        switch (pump(block)) {
        case -1: return NULL;           /* end of input */
        case 0:  if (!block) return NULL;
                 break;                 /* EINTR: wait again */
        default: break;
        }
    }
}

/*
 * Has the client cancelled the turn that is running?  main.c's check_abort
 * calls this every time relay.c polls relay_abort, so it must not block:
 * it takes whatever has arrived apart enough to spot a session/cancel and
 * leaves everything else in the queue for after the turn.
 */
int acp_cancelled(void)
{
    char  *nl, *line;
    size_t off = 0, n, rest;
    cJSON *msg, *method;

    if (cancelled) return 1;
    if (input_init() != 0) return 0;
    (void) pump(0);                     /* no waiting: only what is there */
    while (off < queue.len) {
        line = queue.p + off;
        nl = (char *) memchr(line, '\n', queue.len - off);
        if (nl == NULL) break;          /* a partial line: not a message yet */
        n = (size_t) (nl - line);
        *nl = '\0';
        msg = cJSON_Parse(line);
        *nl = '\n';
        if (msg != NULL) {
            method = cJSON_GetObjectItem(msg, "method");
            if (cJSON_IsString(method) &&
                strcmp(method->valuestring, "session/cancel") == 0) {
                /* Take it out: the serve loop must not read it again. */
                rest = queue.len - (off + n + 1);
                memmove(line, nl + 1, rest);
                queue.len = off + rest;
                queue.p[queue.len] = '\0';
                cJSON_Delete(msg);
                cancelled = 1;
                return 1;
            }
            cJSON_Delete(msg);
        }
        off += n + 1;
    }
    return 0;
}

/* ------------------------------------------------- permission round trip */

/*
 * Ask the client whether a tool may run, and wait for its answer.  This runs
 * inside a prompt turn, so anything the client sends meanwhile (a cancel, a
 * notification) is dispatched rather than dropped.
 */
int acp_ask(const char *tool_call_id, const char *title)
{
    cJSON *m, *p, *tc, *opts, *o, *msg, *id, *res, *out, *k, *opt;
    int    want, answer = 0;

    if (!have_session) return 0;
    want = ++next_id;
    m = cJSON_CreateObject();
    p = cJSON_CreateObject();
    tc = cJSON_CreateObject();
    opts = cJSON_CreateArray();
    if (m == NULL || p == NULL || tc == NULL || opts == NULL) {
        cJSON_Delete(m); cJSON_Delete(p); cJSON_Delete(tc);
        cJSON_Delete(opts);
        return 0;
    }
    if (cJSON_AddStringToObject(p, "sessionId", session_id) == NULL ||
        cJSON_AddStringToObject(tc, "toolCallId",
                                tool_call_id != NULL ? tool_call_id : "") == NULL ||
        cJSON_AddStringToObject(tc, "title",
                                title != NULL ? title : "run a tool") == NULL)
        goto fail;
    cJSON_AddItemToObject(p, "toolCall", tc);
    tc = NULL;

    /* The two options the TODO names, and gork's own answer to `always`. */
    o = cJSON_CreateObject();
    if (o == NULL ||
        cJSON_AddStringToObject(o, "optionId", "allow-once") == NULL ||
        cJSON_AddStringToObject(o, "name", "Allow once") == NULL ||
        cJSON_AddStringToObject(o, "kind", "allow_once") == NULL) {
        cJSON_Delete(o);
        goto fail;
    }
    cJSON_AddItemToArray(opts, o);
    o = cJSON_CreateObject();
    if (o == NULL ||
        cJSON_AddStringToObject(o, "optionId", "allow-always") == NULL ||
        cJSON_AddStringToObject(o, "name", "Allow always") == NULL ||
        cJSON_AddStringToObject(o, "kind", "allow_always") == NULL) {
        cJSON_Delete(o);
        goto fail;
    }
    cJSON_AddItemToArray(opts, o);
    o = cJSON_CreateObject();
    if (o == NULL ||
        cJSON_AddStringToObject(o, "optionId", "reject-once") == NULL ||
        cJSON_AddStringToObject(o, "name", "Reject") == NULL ||
        cJSON_AddStringToObject(o, "kind", "reject_once") == NULL) {
        cJSON_Delete(o);
        goto fail;
    }
    cJSON_AddItemToArray(opts, o);
    cJSON_AddItemToObject(p, "options", opts);
    opts = NULL;

    cJSON_AddStringToObject(m, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(m, "id", (double) want);
    cJSON_AddStringToObject(m, "method", "session/request_permission");
    cJSON_AddItemToObject(m, "params", p);
    send_line(m);
    cJSON_Delete(m);
    m = NULL;

    /* Wait for the reply with this id, dispatching whatever else arrives. */
    for (;;) {
        msg = next_message(1);
        if (msg == NULL) break;                 /* client gone: no answer */
        id = cJSON_GetObjectItem(msg, "id");
        if (cJSON_IsNumber(id) && id->valueint == want) {
            res = cJSON_GetObjectItem(msg, "result");
            out = res != NULL ? cJSON_GetObjectItem(res, "outcome") : NULL;
            k   = out != NULL ? cJSON_GetObjectItem(out, "outcome") : NULL;
            opt = out != NULL ? cJSON_GetObjectItem(out, "optionId") : NULL;
            if (cJSON_IsString(k) && strcmp(k->valuestring, "selected") == 0 &&
                cJSON_IsString(opt)) {
                if (strncmp(opt->valuestring, "allow", 5) == 0)
                    answer = strstr(opt->valuestring, "always") != NULL ? 2 : 1;
            }
            /* `cancelled` leaves answer 0; the turn ends after this too. */
            cJSON_Delete(msg);
            break;
        }
        dispatch(msg);
        cJSON_Delete(msg);
        /* A cancel while the ask is open ends the wait: the turn is over, so
           there is nobody left to approve anything.  Answer 0 = declined. */
        if (cancelled) break;
    }
    return answer;

fail:
    cJSON_Delete(opts);
    cJSON_Delete(tc);
    cJSON_Delete(p);
    cJSON_Delete(m);
    return 0;
}

/* main.c's tool cards.  The strings are main.c's, not taken. */
void acp_tool_start(const char *id, const char *name, const char *kind,
                    const char *title)
{
    cJSON *u = update_of("tool_call");

    if (u == NULL) return;
    if ((id != NULL && cJSON_AddStringToObject(u, "toolCallId", id) == NULL) ||
        (title != NULL && cJSON_AddStringToObject(u, "title", title) == NULL) ||
        (kind != NULL && cJSON_AddStringToObject(u, "kind", kind) == NULL) ||
        (name != NULL && cJSON_AddStringToObject(u, "name", name) == NULL) ||
        cJSON_AddStringToObject(u, "status", "pending") == NULL) {
        cJSON_Delete(u);
        return;
    }
    notify(u);
}

void acp_tool_done(const char *id, int failed)
{
    cJSON *u = update_of("tool_call_update");
    cJSON *arr, *c;

    if (u == NULL) return;
    if ((id != NULL && cJSON_AddStringToObject(u, "toolCallId", id) == NULL) ||
        cJSON_AddStringToObject(u, "status",
                                failed ? "failed" : "completed") == NULL) {
        cJSON_Delete(u);
        return;
    }
    /* Some clients draw the card's result area from `content`; an empty
       array is the honest answer here, since the tool's text goes back to
       the model, not to the editor. */
    arr = cJSON_CreateArray();
    c = cJSON_CreateObject();
    if (arr == NULL || c == NULL) {
        cJSON_Delete(arr);
        cJSON_Delete(c);
        cJSON_Delete(u);
        return;
    }
    cJSON_AddItemToArray(arr, c);
    cJSON_AddItemToObject(u, "content", arr);
    notify(u);
}

/* Whole text at once.  gork's stream_text() hands pieces over already, so a
   chunk per piece is a one-line change in main.c; the shape here is the same. */
void acp_text(const char *text)
{
    cJSON *u, *c;

    if (text == NULL || *text == '\0') return;
    u = update_of("agent_message_chunk");
    c = cJSON_CreateObject();
    if (u == NULL || c == NULL ||
        cJSON_AddStringToObject(c, "type", "text") == NULL ||
        cJSON_AddStringToObject(c, "text", text) == NULL) {
        cJSON_Delete(u);
        cJSON_Delete(c);
        return;
    }
    cJSON_AddItemToObject(u, "content", c);
    notify(u);
}

void acp_usage(void)
{
    cJSON *u;

    if (!have_session || hooks == NULL || hooks->tokens == NULL || conv == NULL)
        return;
    u = update_of("usage_update");
    if (u == NULL) return;
    if (cJSON_AddNumberToObject(u, "used",
                                (double) hooks->tokens(conv)) == NULL ||
        cJSON_AddNumberToObject(u, "size",
                                (double) hooks->ctx_window) == NULL) {
        cJSON_Delete(u);
        return;
    }
    notify(u);
}

/* ---------------------------------------------------------------- methods */

static void do_initialize(cJSON *id, cJSON *params)
{
    cJSON *r, *caps, *info, *auth, *pc, *mc;
    int    want = 1;

    if (params != NULL) {
        cJSON *v = cJSON_GetObjectItem(params, "protocolVersion");

        if (cJSON_IsNumber(v)) want = v->valueint;
    }
    if (want != 1)
        fprintf(stderr, "gork: the client asked for ACP v%d; this speaks "
                        "v1\n", want);

    r = cJSON_CreateObject();
    caps = cJSON_CreateObject();
    info = cJSON_CreateObject();
    auth = cJSON_CreateArray();
    pc = cJSON_CreateObject();
    mc = cJSON_CreateObject();
    if (r == NULL || caps == NULL || info == NULL || auth == NULL ||
        pc == NULL || mc == NULL ||
        /* v1 is the only major version gork speaks, so answer with it. */
        cJSON_AddNumberToObject(r, "protocolVersion", 1) == NULL ||
        /* No session/load: gork holds one conversation in memory. */
        cJSON_AddBoolToObject(caps, "loadSession", 0) == NULL ||
        cJSON_AddStringToObject(info, "name", "gork") == NULL ||
        cJSON_AddStringToObject(info, "title", "gork") == NULL ||
        cJSON_AddStringToObject(info, "version", GORK_VERSION) == NULL) {
        cJSON_Delete(r); cJSON_Delete(caps); cJSON_Delete(info);
        cJSON_Delete(auth); cJSON_Delete(pc); cJSON_Delete(mc);
        send_error(id, -32603, "out of memory");
        return;
    }
    /* Text in, text out. */
    cJSON_AddBoolToObject(pc, "image", 0);
    cJSON_AddBoolToObject(pc, "audio", 0);
    cJSON_AddBoolToObject(pc, "embeddedContext", 0);
    cJSON_AddItemToObject(caps, "promptCapabilities", pc);
    /* gork starts its own MCP servers from its config, not from the editor. */
    cJSON_AddBoolToObject(mc, "http", 0);
    cJSON_AddBoolToObject(mc, "sse", 0);
    cJSON_AddItemToObject(caps, "mcpCapabilities", mc);
    cJSON_AddItemToObject(r, "agentCapabilities", caps);
    cJSON_AddItemToObject(r, "agentInfo", info);
    /* Empty: gork's key is its own config's or the relay's, not the editor's. */
    cJSON_AddItemToObject(r, "authMethods", auth);
    send_result(id, r);
}

static void do_session_new(cJSON *id, cJSON *params, const char *cwd_default)
{
    cJSON *r, *cwd;
    const char *dir;
    static int serial;

    dir = cwd_default;
    if (params != NULL) {
        cwd = cJSON_GetObjectItem(params, "cwd");

        if (cJSON_IsString(cwd)) dir = cwd->valuestring;
    }
    if (dir == NULL) {
        send_error(id, -32602, "session/new needs a cwd");
        return;
    }
    if (conv != NULL) hooks->conv_free(conv);
    conv = hooks->conv_new(dir);
    if (conv == NULL) {
        send_error(id, -32603, "out of memory");
        return;
    }
    sprintf(session_id, "gork-%d", ++serial);
    have_session = 1;
    r = cJSON_CreateObject();
    if (r == NULL || cJSON_AddStringToObject(r, "sessionId",
                                             session_id) == NULL) {
        cJSON_Delete(r);
        send_error(id, -32603, "out of memory");
        return;
    }
    send_result(id, r);
}

static void do_session_prompt(cJSON *id, cJSON *params)
{
    cJSON      *prompt, *block, *type, *text;
    const char *stop;
    struct buf  all;

    if (conv == NULL || !have_session) {
        send_error(id, -32602, "no session");
        return;
    }
    prompt = params != NULL ? cJSON_GetObjectItem(params, "prompt") : NULL;
    if (!cJSON_IsArray(prompt)) {
        send_error(id, -32602, "session/prompt needs a prompt array");
        return;
    }

    /* The text blocks, newline between: gork's model sees one user message.
       Other block types are dropped -- the capabilities above say text only. */
    if (buf_init(&all) != 0) {
        send_error(id, -32603, "out of memory");
        return;
    }
    cJSON_ArrayForEach(block, prompt) {
        type = cJSON_GetObjectItem(block, "type");
        if (!cJSON_IsString(type) ||
            strcmp(type->valuestring, "text") != 0)
            continue;
        text = cJSON_GetObjectItem(block, "text");
        if (!cJSON_IsString(text)) continue;
        if ((all.len > 0 && buf_add(&all, "\n", 1) != 0) ||
            buf_add(&all, text->valuestring, strlen(text->valuestring)) != 0) {
            buf_free(&all);
            send_error(id, -32603, "out of memory");
            return;
        }
    }

    cancelled = 0;
    stop = hooks->run(conv, all.p);
    buf_free(&all);

    if (cancelled) stop = "cancelled";
    if (stop == NULL) stop = "end_turn";
    {
        cJSON *r = cJSON_CreateObject();

        if (r == NULL ||
            cJSON_AddStringToObject(r, "stopReason", stop) == NULL) {
            cJSON_Delete(r);
            send_error(id, -32603, "out of memory");
            return;
        }
        acp_usage();
        send_result(id, r);
    }
}

static int dispatch(cJSON *msg)
{
    cJSON *id, *method, *params, *res, *o, *k, *opt;

    if (!cJSON_IsObject(msg)) return 0;

    method = cJSON_GetObjectItem(msg, "method");
    if (!cJSON_IsString(method)) {
        /* A reply to a request gork made, or noise on the wire. */
        res = cJSON_GetObjectItem(msg, "result");
        o   = res != NULL ? cJSON_GetObjectItem(res, "outcome") : NULL;
        k   = o != NULL ? cJSON_GetObjectItem(o, "outcome") : NULL;
        opt = o != NULL ? cJSON_GetObjectItem(o, "optionId") : NULL;
        if (o != NULL && cJSON_IsString(k) &&
            strcmp(k->valuestring, "cancelled") == 0)
            cancelled = 1;
        (void) opt;
        return 0;
    }

    id = cJSON_GetObjectItem(msg, "id");
    params = cJSON_GetObjectItem(msg, "params");

    if (strcmp(method->valuestring, "initialize") == 0) {
        do_initialize(id, params);
        return 0;
    }
    if (strcmp(method->valuestring, "authenticate") == 0) {
        send_result(id, NULL);              /* gork advertises no auth */
        return 0;
    }
    if (strcmp(method->valuestring, "session/new") == 0) {
        do_session_new(id, params, NULL);
        return 0;
    }
    if (strcmp(method->valuestring, "session/prompt") == 0) {
        do_session_prompt(id, params);
        return 0;
    }
    if (strcmp(method->valuestring, "session/cancel") == 0) {
        cancelled = 1;                      /* a notification: no reply */
        return 0;
    }
    if (strcmp(method->valuestring, "exit") == 0) {
        send_result(id, NULL);
        return 1;
    }
    if (id == NULL) return 0;               /* a notification gork ignores */
    send_error(id, -32601, "method not found");
    return 0;
}

/* ------------------------------------------------------------------ serve */

int acp_serve(const struct acp_hooks *h)
{
    cJSON *msg;
    int    rc;

    hooks = h;
    for (;;) {
        msg = next_message(1);
        if (msg == NULL) break;             /* the client closed stdin */
        rc = dispatch(msg);
        cJSON_Delete(msg);
        if (rc != 0) break;
    }
    if (conv != NULL) {
        hooks->conv_free(conv);
        conv = NULL;
    }
    return 0;
}

#else /* GORK_NO_FORK */

int  acp_serve(const struct acp_hooks *h) { (void) h; return -1; }
void acp_text(const char *t) { (void) t; }
void acp_tool_start(const char *a, const char *b, const char *c,
                    const char *d) { (void) a; (void) b; (void) c; (void) d; }
void acp_tool_done(const char *a, int b) { (void) a; (void) b; }
void acp_usage(void) { }
int  acp_ask(const char *a, const char *b) { (void) a; (void) b; return 0; }
int  acp_cancelled(void) { return 0; }

#endif /* !GORK_NO_FORK */
