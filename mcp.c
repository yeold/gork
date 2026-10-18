/*
 * mcp.c -- Model Context Protocol client: stdio and streamable HTTP.
 *
 * Servers come from ~/.gork/mcp.json, in the "mcpServers" shape Claude Code
 * and Cline use.  One with a "command" is a child process speaking
 * newline-delimited JSON-RPC 2.0 on its stdin/stdout.  One with a "url" is
 * spoken to by HTTP POST, each message a request of its own; the answer
 * comes back as JSON or as a stream of server-sent events.  gork has no
 * TLS: an https:// server is reached through a relay that has ("relay":
 * "host:port"), as the API is.  At startup gork runs initialize and
 * tools/list, and offers each tool to the model as mcp__<server>__<tool>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "port.h"
#ifndef GORK_NO_SELECT
#include <sys/time.h>
#endif
#include "relay.h"              /* struct buf */
#include "mcp.h"

#define INIT_TIMEOUT     30     /* seconds for initialize / tools/list */
#define MCP_OUT_MAX      65536  /* tool output beyond this is cut off */
#define MAX_PAGES        50     /* tools/list pages, against a looping cursor */
#define TOOL_NAME_MAX    64     /* the API's limit on tool names */
#define PROTOCOL_VERSION "2025-06-18"

struct server {
    char      *name;
    pid_t      pid;
    int        in_fd, out_fd;   /* its stdin, its stdout */
    struct buf rx;              /* bytes read but not yet a whole line */
    int        dead;
    /* streamable HTTP */
    int        http;
    char      *conn_host;       /* where to connect: the server, or a relay */
    long       conn_port;
    char      *host, *path;     /* the URL's, for the request line */
    char      *headers;         /* the "headers" map, as CRLF-ended lines */
    char       session[256];    /* Mcp-Session-Id, once the server gave one */
    char       proto[32];       /* the protocol version agreed on */
};

struct mtool {
    int    srv;                 /* index into servers */
    char  *tool;                /* the server's own name for it */
    cJSON *def;                 /* {name, description, input_schema} */
    int    always;
};

static struct server *servers;
static int            nservers;
static struct mtool  *mtools;
static int            nmtools;
static int            next_id = 1;
static char           rpc_err[256];

static char *mstrdup(const char *s)
{
    char *p = (char *) malloc(strlen(s) + 1);

    if (p != NULL) strcpy(p, s);
    return p;
}

static int adds(struct buf *b, const char *s)
{
    return buf_add(b, s, strlen(s));
}

/* ------------------------------------------------------------------ */
/* wire                                                               */
/* ------------------------------------------------------------------ */

static int write_all(int fd, const char *p, size_t n)
{
    int w;

    while (n > 0) {
        w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t) w;
    }
    return 0;
}

/* POST one message to an HTTP server, with what this session needs in the
   headers.  The HTTP status, the body in `resp` and the header block in
   `head` (both the caller's to free when the status is not negative), or
   -1, or -2 if the user pressed Esc. */
static int http_send(struct server *s, cJSON *msg, int timeout,
                     struct buf *resp, struct buf *head)
{
    char       *text = cJSON_PrintUnformatted(msg);
    struct buf  extra;
    int         status, saved = relay_timeout;

    head->p = NULL;
    if (text == NULL || buf_init(&extra) != 0) {
        free(text);
        return -1;
    }
    if (adds(&extra, "Accept: application/json, text/event-stream\r\n") != 0 ||
        (s->session[0] != '\0' &&
         (adds(&extra, "Mcp-Session-Id: ") != 0 ||
          adds(&extra, s->session) != 0 ||
          adds(&extra, "\r\n") != 0)) ||
        (s->proto[0] != '\0' &&
         (adds(&extra, "MCP-Protocol-Version: ") != 0 ||
          adds(&extra, s->proto) != 0 ||
          adds(&extra, "\r\n") != 0)) ||
        adds(&extra, s->headers) != 0) {
        buf_free(&extra);
        free(text);
        return -1;
    }
    relay_timeout = timeout;
    status = relay_http("POST", s->conn_host, s->conn_port, s->host, s->path,
                        extra.p, text, strlen(text), resp, head);
    relay_timeout = saved;
    buf_free(&extra);
    free(text);
    if (status < 0) {
        buf_free(resp);
        return status;
    }
    if (head->p == NULL) {          /* no header block: nothing came */
        buf_free(resp);
        return -1;
    }
    return status;
}

/* A notification or a response to the server: the answer is 202 and no
   body.  Esc is not looked at: it may be the user's Esc being reported. */
static int http_post_only(struct server *s, cJSON *msg)
{
    struct buf resp, head;
    int        status, (*saved)(void) = relay_abort;

    relay_abort = NULL;
    status = http_send(s, msg, 10, &resp, &head);
    relay_abort = saved;
    if (status < 0) return -1;
    buf_free(&resp);
    buf_free(&head);
    return status >= 200 && status < 300 ? 0 : -1;
}

/* One message, one line.  SIGPIPE is ignored only for the write, so a dead
   server is an error here -- not the death of gork -- while run_command's
   children still get the default. */
static int send_msg(struct server *s, cJSON *msg)
{
    char *text;
    void (*old)(int);
    int   rc;

    if (s->http) return http_post_only(s, msg);
    text = cJSON_PrintUnformatted(msg);
    if (text == NULL) return -1;
    old = signal(SIGPIPE, SIG_IGN);
    rc  = (write_all(s->in_fd, text, strlen(text)) == 0 &&
           write_all(s->in_fd, "\n", 1) == 0) ? 0 : -1;
    signal(SIGPIPE, old);
    free(text);
    if (rc != 0) s->dead = 1;
    return rc;
}

static int stopped;             /* read_line: the user pressed Esc */

/* Next whole line from the server, or NULL at EOF, once `deadline` passes,
   or (with `stopped` set) when the user pressed Esc. */
static char *read_line(struct server *s, time_t deadline)
{
    char            chunk[4096], *nl, *line;
    size_t          n;
    int             r;
    long            left;
#ifndef GORK_NO_SELECT
    fd_set          rf;
    struct timeval  tv;
#endif

    for (;;) {
        nl = (char *) memchr(s->rx.p, '\n', s->rx.len);
        if (nl != NULL) {
            n = (size_t) (nl - s->rx.p);
            line = (char *) malloc(n + 1);
            if (line == NULL) return NULL;
            memcpy(line, s->rx.p, n);
            line[n] = '\0';
            s->rx.len -= n + 1;
            memmove(s->rx.p, nl + 1, s->rx.len);
            s->rx.p[s->rx.len] = '\0';
            return line;
        }

        left = (long) (deadline - time(NULL));
        if (left <= 0) return NULL;
        /* In the TUI, look for Esc five times a second. */
        if (relay_abort != NULL && relay_abort()) {
            stopped = 1;
            return NULL;
        }
#ifdef GORK_NO_SELECT
        r = read_timed(s->out_fd, chunk, sizeof chunk, left);
        if (r < 0 && errno == EINTR) continue;
#else
        FD_ZERO(&rf);
        FD_SET(s->out_fd, &rf);
        tv.tv_sec  = relay_abort != NULL ? 0 : left;
        tv.tv_usec = relay_abort != NULL ? 200000L : 0;
        r = select(s->out_fd + 1, &rf, NULL, NULL, &tv);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) return NULL;
        if (r == 0) continue;

        r = read(s->out_fd, chunk, sizeof chunk);
#endif
        if (r <= 0) {
            s->dead = 1;
            return NULL;
        }
        if (buf_add(&s->rx, chunk, (size_t) r) != 0) return NULL;
    }
}

/* Servers may ask things of the client.  gork offers no client features,
   so everything but ping gets "method not found". */
static void answer(struct server *s, cJSON *req)
{
    cJSON *r = cJSON_CreateObject(), *e;

    if (r == NULL) return;
    cJSON_AddStringToObject(r, "jsonrpc", "2.0");
    cJSON_AddItemToObject(r, "id",
                          cJSON_Duplicate(cJSON_GetObjectItem(req, "id"), 1));
    if (strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "ping") == 0) {
        cJSON_AddObjectToObject(r, "result");
    } else {
        e = cJSON_AddObjectToObject(r, "error");
        cJSON_AddNumberToObject(e, "code", -32601);
        cJSON_AddStringToObject(e, "message", "not supported by gork");
    }
    send_msg(s, r);
    cJSON_Delete(r);
}

static void notify(struct server *s, const char *method)
{
    cJSON *n = cJSON_CreateObject();

    if (n == NULL) return;
    cJSON_AddStringToObject(n, "jsonrpc", "2.0");
    cJSON_AddStringToObject(n, "method", method);
    send_msg(s, n);
    cJSON_Delete(n);
}

/* The user pressed Esc: tell the server.  Its answer, if one still comes,
   is skipped like any late one. */
static void cancel(struct server *s, int want)
{
    cJSON *req = cJSON_CreateObject(), *params;

    params = req != NULL ? cJSON_AddObjectToObject(req, "params") : NULL;
    if (params != NULL) {
        cJSON_AddStringToObject(req, "jsonrpc", "2.0");
        cJSON_AddStringToObject(req, "method", "notifications/cancelled");
        cJSON_AddNumberToObject(params, "requestId", want);
        send_msg(s, req);
    }
    cJSON_Delete(req);
    strcpy(rpc_err, "aborted by the user");
}

static void add_message(cJSON *all, const char *text)
{
    cJSON *m = cJSON_Parse(text), *item;

    if (cJSON_IsArray(m)) {             /* a batch: each on its own */
        while ((item = cJSON_DetachItemFromArray(m, 0)) != NULL)
            cJSON_AddItemToArray(all, item);
        cJSON_Delete(m);
    } else if (m != NULL) {
        cJSON_AddItemToArray(all, m);
    }
}

/* The messages in an HTTP reply, as an array: the body is JSON, or a
   stream of server-sent events whose data are. */
static cJSON *reply_messages(const struct buf *head, const char *body)
{
    cJSON      *all = cJSON_CreateArray();
    struct buf  data;
    char        type[128], *t;
    const char *p = body, *e, *q;
    size_t      n, k;

    if (all == NULL) return NULL;
    type[0] = '\0';
    relay_header(head, "content-type", type, sizeof type);
    for (t = type; *t; t++) *t = (char) tolower((unsigned char) *t);
    if (strstr(type, "text/event-stream") == NULL) {
        add_message(all, body);
        return all;
    }
    if (buf_init(&data) != 0) return all;
    for (;;) {
        e = strchr(p, '\n');
        n = e != NULL ? (size_t) (e - p) : strlen(p);
        if (n > 0 && p[n - 1] == '\r') n--;
        if (n == 0) {                   /* an event ends: its data */
            if (data.len > 0) add_message(all, data.p);
            data.len = 0;
            data.p[0] = '\0';
        } else if (n >= 5 && memcmp(p, "data:", 5) == 0) {
            q = p + 5;
            k = n - 5;
            if (k > 0 && *q == ' ') {
                q++;
                k--;
            }
            if ((data.len > 0 && adds(&data, "\n") != 0) ||
                buf_add(&data, q, k) != 0)
                break;
        }
        if (e == NULL) break;
        p = e + 1;
    }
    if (data.len > 0) add_message(all, data.p);
    buf_free(&data);
    return all;
}

/* rpc() over HTTP: the request is a POST, and its answer comes in the
   reply, with whatever else the server sends meanwhile. */
static cJSON *http_rpc(struct server *s, cJSON *req, int want, int timeout)
{
    struct buf  resp, head;
    cJSON      *msgs, *msg, *id, *m, *err, *emsg, *res = NULL;
    char        sid[sizeof s->session];
    int         status, failed = 0;

    status = http_send(s, req, timeout, &resp, &head);
    if (status == -2) {
        cancel(s, want);
        return NULL;
    }
    if (status < 0) {
        strcpy(rpc_err, "cannot reach the server");
        return NULL;
    }
    if (s->session[0] == '\0' &&
        relay_header(&head, "mcp-session-id", sid, sizeof sid) == 0)
        strcpy(s->session, sid);
    if (status != 200) {
        if (status == 404 && s->session[0] != '\0')
            strcpy(rpc_err, "the server ended the session");
        else
            sprintf(rpc_err, "HTTP status %d", status);
        buf_free(&resp);
        buf_free(&head);
        return NULL;
    }
    msgs = reply_messages(&head, resp.p);
    buf_free(&resp);
    buf_free(&head);
    strcpy(rpc_err, "the reply held no answer");
    cJSON_ArrayForEach(msg, msgs) {
        id = cJSON_GetObjectItem(msg, "id");
        m  = cJSON_GetObjectItem(msg, "method");
        if (cJSON_IsString(m) && id != NULL) {
            answer(s, msg);
        } else if (m == NULL && cJSON_IsNumber(id) &&
                   (int) id->valuedouble == want && res == NULL && !failed) {
            err = cJSON_GetObjectItem(msg, "error");
            if (err != NULL) {
                emsg = cJSON_GetObjectItem(err, "message");
                sprintf(rpc_err, "%.200s", cJSON_IsString(emsg)
                                           ? emsg->valuestring : "error");
                failed = 1;
                continue;
            }
            res = cJSON_DetachItemFromObject(msg, "result");
            if (res == NULL) {
                strcpy(rpc_err, "response had no result");
                failed = 1;
            }
        }
    }
    cJSON_Delete(msgs);
    return res;
}

/* Send a request and wait for its response, answering whatever the server
   asks meanwhile and skipping notifications, log noise, and late replies to
   requests that already timed out.  Takes ownership of params.  Returns the
   detached result, or NULL with the reason in rpc_err. */
static cJSON *rpc(struct server *s, const char *method, cJSON *params,
                  int timeout)
{
    cJSON  *req, *msg, *id, *m, *res, *err, *emsg;
    int     want = next_id++;
    time_t  deadline = time(NULL) + timeout;
    char   *line;

    if (s->dead) {
        cJSON_Delete(params);
        strcpy(rpc_err, "server has exited");
        return NULL;
    }
    req = cJSON_CreateObject();
    if (req == NULL) {
        cJSON_Delete(params);
        strcpy(rpc_err, "out of memory");
        return NULL;
    }
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", want);
    cJSON_AddStringToObject(req, "method", method);
    if (params != NULL) cJSON_AddItemToObject(req, "params", params);
    if (s->http) {
        msg = http_rpc(s, req, want, timeout);
        cJSON_Delete(req);
        return msg;
    }
    if (send_msg(s, req) != 0) {
        cJSON_Delete(req);
        strcpy(rpc_err, "cannot write to server");
        return NULL;
    }
    cJSON_Delete(req);

    for (;;) {
        line = read_line(s, deadline);
        if (line == NULL && stopped) {
            stopped = 0;
            cancel(s, want);
            return NULL;
        }
        if (line == NULL) {
            strcpy(rpc_err, s->dead ? "server exited"
                                    : "server did not answer in time");
            return NULL;
        }
        msg = cJSON_Parse(line);
        free(line);
        if (msg == NULL) continue;              /* not JSON: stray output */

        id = cJSON_GetObjectItem(msg, "id");
        m  = cJSON_GetObjectItem(msg, "method");
        if (cJSON_IsString(m) && id != NULL) {
            answer(s, msg);
        } else if (m == NULL && cJSON_IsNumber(id) &&
                   (int) id->valuedouble == want) {
            err = cJSON_GetObjectItem(msg, "error");
            if (err != NULL) {
                emsg = cJSON_GetObjectItem(err, "message");
                sprintf(rpc_err, "%.200s", cJSON_IsString(emsg)
                                           ? emsg->valuestring : "error");
                cJSON_Delete(msg);
                return NULL;
            }
            res = cJSON_DetachItemFromObject(msg, "result");
            cJSON_Delete(msg);
            if (res == NULL) strcpy(rpc_err, "response had no result");
            return res;
        }
        cJSON_Delete(msg);
    }
}

/* ------------------------------------------------------------------ */
/* servers                                                            */
/* ------------------------------------------------------------------ */

static int spawn(struct server *s, cJSON *cfg)
{
    cJSON *cmd  = cJSON_GetObjectItem(cfg, "command");
    cJSON *args = cJSON_GetObjectItem(cfg, "args");
    cJSON *env  = cJSON_GetObjectItem(cfg, "env");
    cJSON *a;
    char **argv;
    int    i, to[2], from[2], dn;

    argv = (char **) malloc((cJSON_GetArraySize(args) + 2) * sizeof *argv);
    if (argv == NULL) return -1;
    argv[0] = cmd->valuestring;
    i = 1;
    cJSON_ArrayForEach(a, args)
        if (cJSON_IsString(a)) argv[i++] = a->valuestring;
    argv[i] = NULL;

    if (pipe(to) != 0) {
        free(argv);
        return -1;
    }
    if (pipe(from) != 0) {
        close(to[0]);
        close(to[1]);
        free(argv);
        return -1;
    }

    s->pid = fork();
    if (s->pid == 0) {
        dup2(to[0], 0);
        dup2(from[1], 1);
        /* Server logs would spill into the TUI; GORK_TRACE lets them out. */
        if (getenv("GORK_TRACE") == NULL &&
            (dn = open("/dev/null", O_WRONLY)) >= 0)
            dup2(dn, 2);
        close(to[0]);
        close(to[1]);
        close(from[0]);
        close(from[1]);
        /* putenv, not setenv: Solaris before 10 has no setenv.  The
           strings are never freed; exec replaces the image anyway. */
        cJSON_ArrayForEach(a, env) {
            char *kv;
            if (!cJSON_IsString(a)) continue;
            kv = malloc(strlen(a->string) + strlen(a->valuestring) + 2);
            if (kv == NULL) continue;
            sprintf(kv, "%s=%s", a->string, a->valuestring);
            putenv(kv);
        }
        execvp(argv[0], argv);
        _exit(127);
    }

    free(argv);
    close(to[0]);
    close(from[1]);
    if (s->pid < 0) {
        close(to[1]);
        close(from[0]);
        return -1;
    }
    s->in_fd  = to[1];
    s->out_fd = from[0];
    /* Later children (other servers, run_command) must not hold these, or
       a server never sees EOF on its stdin. */
    fcntl(s->in_fd, F_SETFD, FD_CLOEXEC);
    fcntl(s->out_fd, F_SETFD, FD_CLOEXEC);
    return 0;
}

static char *span(const char *p, size_t n)
{
    char *q = (char *) malloc(n + 1);

    if (q != NULL) {
        memcpy(q, p, n);
        q[n] = '\0';
    }
    return q;
}

/* "host:port" into conn_host and conn_port; 0, or -1 if it is not one. */
static int host_port(struct server *s, const char *hp, long dflt)
{
    const char *c = strrchr(hp, ':');
    char       *end;

    if (c == NULL) {
        s->conn_port = dflt;
        s->conn_host = mstrdup(hp);
    } else {
        s->conn_port = strtol(c + 1, &end, 10);
        if (*end != '\0' || s->conn_port <= 0 || s->conn_port > 65535)
            return -1;
        s->conn_host = span(hp, (size_t) (c - hp));
    }
    return s->conn_host != NULL && *s->conn_host != '\0' ? 0 : -1;
}

/* An HTTP server from its entry: "url", and maybe "relay" and "headers".
   0, or -1 having said why. */
static int http_setup(struct server *s, cJSON *cfg)
{
    const char *u = cJSON_GetObjectItem(cfg, "url")->valuestring, *h, *e;
    cJSON      *relay = cJSON_GetObjectItem(cfg, "relay"), *kv;
    struct buf  lines;
    int         tls;

    if (strncmp(u, "http://", 7) == 0) {
        tls = 0;
        h = u + 7;
    } else if (strncmp(u, "https://", 8) == 0) {
        tls = 1;
        h = u + 8;
    } else {
        fprintf(stderr, "gork: MCP server %s: want an http:// or https:// "
                        "url\n", s->name);
        return -1;
    }
    e = h + strcspn(h, "/?#");
    s->http = 1;
    s->host = span(h, (size_t) (e - h));
    s->path = *e == '/' ? span(e, strcspn(e, "#"))
                        : (char *) malloc(strcspn(e, "#") + 2);
    if (s->host == NULL || s->path == NULL) return -1;
    if (*e != '/') {                    /* "?q" or nothing: from the root */
        s->path[0] = '/';
        memcpy(s->path + 1, e, strcspn(e, "#"));
        s->path[1 + strcspn(e, "#")] = '\0';
    }
    if (cJSON_IsString(relay) ? host_port(s, relay->valuestring, 0) != 0
                              : tls) {
        fprintf(stderr, "gork: MCP server %s: %s\n", s->name,
                cJSON_IsString(relay)
                ? "want \"relay\": \"host:port\""
                : "gork has no TLS: an https:// server needs a "
                  "\"relay\": \"host:port\" that has");
        return -1;
    }
    if (!cJSON_IsString(relay) && host_port(s, s->host, 80) != 0) {
        fprintf(stderr, "gork: MCP server %s: bad url\n", s->name);
        return -1;
    }

    if (buf_init(&lines) != 0) return -1;
    cJSON_ArrayForEach(kv, cJSON_GetObjectItem(cfg, "headers")) {
        if (!cJSON_IsString(kv) || strpbrk(kv->string, ":\r\n") != NULL ||
            strpbrk(kv->valuestring, "\r\n") != NULL) {
            fprintf(stderr, "gork: MCP server %s: header %s skipped\n",
                    s->name, kv->string);
            continue;
        }
        if (adds(&lines, kv->string) != 0 ||
            adds(&lines, ": ") != 0 ||
            adds(&lines, kv->valuestring) != 0 ||
            adds(&lines, "\r\n") != 0) {
            buf_free(&lines);
            return -1;
        }
    }
    s->headers = lines.p;
    return 0;
}

/* The end of an HTTP session: DELETE it, if the server gave it an id. */
static void http_stop(struct server *s)
{
    struct buf resp, head, extra;
    int        (*saved)(void) = relay_abort, timeout = relay_timeout;

    s->dead = 1;
    if (s->session[0] == '\0' || buf_init(&extra) != 0) return;
    if (adds(&extra, "Mcp-Session-Id: ") == 0 &&
        adds(&extra, s->session) == 0 &&
        adds(&extra, "\r\n") == 0) {
        relay_abort = NULL;
        relay_timeout = 5;
        head.p = NULL;
        if (relay_http("DELETE", s->conn_host, s->conn_port, s->host,
                       s->path, extra.p, "", 0, &resp, &head) >= 0)
            buf_free(&head);
        buf_free(&resp);
        relay_abort = saved;
        relay_timeout = timeout;
    }
    buf_free(&extra);
    s->session[0] = '\0';
}

static void stop_server(struct server *s)
{
    if (s->http) {
        if (!s->dead) http_stop(s);
        return;
    }
    if (s->pid <= 0) return;
    close(s->in_fd);
    close(s->out_fd);
    kill(s->pid, SIGTERM);
    waitpid(s->pid, NULL, 0);
    s->pid  = -1;
    s->dead = 1;
}

static int listed(cJSON *names, const char *name)
{
    cJSON *n;

    cJSON_ArrayForEach(n, names)
        if (cJSON_IsString(n) && strcmp(n->valuestring, name) == 0) return 1;
    return 0;
}

static void add_tool(int si, cJSON *t, cJSON *auto_approve)
{
    cJSON        *name   = cJSON_GetObjectItem(t, "name");
    cJSON        *desc   = cJSON_GetObjectItem(t, "description");
    cJSON        *schema = cJSON_GetObjectItem(t, "inputSchema");
    cJSON        *def, *sch;
    struct mtool *nt;
    char          full[TOOL_NAME_MAX + 8];
    const char   *p;

    if (!cJSON_IsString(name)) return;
    if (strlen(servers[si].name) + strlen(name->valuestring) + 7 >
        TOOL_NAME_MAX) {
        fprintf(stderr, "gork: MCP tool %s.%s: name too long, skipped\n",
                servers[si].name, name->valuestring);
        return;
    }
    strcpy(full, "mcp__");                  /* length checked above */
    strcat(full, servers[si].name);
    strcat(full, "__");
    strcat(full, name->valuestring);
    for (p = full; *p; p++) {
        if (!isalnum((unsigned char) *p) && *p != '_' && *p != '-') {
            fprintf(stderr, "gork: MCP tool %s: name has characters the "
                            "API refuses, skipped\n", full);
            return;
        }
    }

    sch = cJSON_IsObject(schema) ? cJSON_Duplicate(schema, 1)
                                 : cJSON_Parse("{\"type\":\"object\"}");
    def = cJSON_CreateObject();
    nt  = (struct mtool *) realloc(mtools, (nmtools + 1) * sizeof *mtools);
    if (nt != NULL) mtools = nt;
    if (sch == NULL || def == NULL || nt == NULL ||
        cJSON_AddStringToObject(def, "name", full) == NULL ||
        cJSON_AddStringToObject(def, "description", cJSON_IsString(desc)
                                ? desc->valuestring : "") == NULL ||
        (nt[nmtools].tool = mstrdup(name->valuestring)) == NULL) {
        cJSON_Delete(sch);
        cJSON_Delete(def);
        return;
    }
    cJSON_AddItemToObject(def, "input_schema", sch);
    nt[nmtools].srv    = si;
    nt[nmtools].def    = def;
    nt[nmtools].always = listed(auto_approve, name->valuestring);
    nmtools++;
}

static void start_server(const char *name, cJSON *cfg)
{
    struct server *ns, *s;
    cJSON         *params, *info, *res, *nc, *pv;
    char          *cursor = NULL;
    int            si, pages = 0;

    ns = (struct server *) realloc(servers, (nservers + 1) * sizeof *servers);
    if (ns == NULL) return;
    servers = ns;
    si = nservers;
    s  = &servers[si];
    memset(s, 0, sizeof *s);
    s->pid = -1;
    if ((s->name = mstrdup(name)) == NULL || buf_init(&s->rx) != 0) {
        free(s->name);
        return;
    }
    if (cJSON_IsString(cJSON_GetObjectItem(cfg, "url"))) {
        if (http_setup(s, cfg) != 0) {
            buf_free(&s->rx);
            free(s->name);
            return;
        }
    } else if (spawn(s, cfg) != 0) {
        fprintf(stderr, "gork: MCP server %s: cannot start: %s\n",
                name, strerror(errno));
        buf_free(&s->rx);
        free(s->name);
        return;
    }
    nservers++;

    params = cJSON_CreateObject();
    info   = cJSON_AddObjectToObject(params, "clientInfo");
    cJSON_AddStringToObject(params, "protocolVersion", PROTOCOL_VERSION);
    cJSON_AddObjectToObject(params, "capabilities");
    cJSON_AddStringToObject(info, "name", "gork");
    cJSON_AddStringToObject(info, "version", "0.1");
    res = rpc(s, "initialize", params, INIT_TIMEOUT);
    if (res == NULL) {
        fprintf(stderr, "gork: MCP server %s: initialize failed: %s\n",
                name, rpc_err);
        stop_server(s);
        return;
    }
    pv = cJSON_GetObjectItem(res, "protocolVersion");
    if (cJSON_IsString(pv) && strlen(pv->valuestring) < sizeof s->proto)
        strcpy(s->proto, pv->valuestring);
    cJSON_Delete(res);
    notify(s, "notifications/initialized");

    do {
        params = cJSON_CreateObject();
        if (params != NULL && cursor != NULL)
            cJSON_AddStringToObject(params, "cursor", cursor);
        free(cursor);
        cursor = NULL;

        res = rpc(&servers[si], "tools/list", params, INIT_TIMEOUT);
        if (res == NULL) {
            fprintf(stderr, "gork: MCP server %s: tools/list failed: %s\n",
                    name, rpc_err);
            break;
        }
        cJSON_ArrayForEach(nc, cJSON_GetObjectItem(res, "tools")) {
            /* A "tools" list keeps only those: some servers' schemas
               would fill the context window on their own. */
            if (cJSON_IsArray(cJSON_GetObjectItem(cfg, "tools")) &&
                (!cJSON_IsString(cJSON_GetObjectItem(nc, "name")) ||
                 !listed(cJSON_GetObjectItem(cfg, "tools"),
                         cJSON_GetObjectItem(nc, "name")->valuestring)))
                continue;
            add_tool(si, nc, cJSON_GetObjectItem(cfg, "autoApprove"));
        }
        nc = cJSON_GetObjectItem(res, "nextCursor");
        if (cJSON_IsString(nc) && *nc->valuestring != '\0')
            cursor = mstrdup(nc->valuestring);
        cJSON_Delete(res);
    } while (cursor != NULL && ++pages < MAX_PAGES);
    free(cursor);
}

/* Zed puts command, args and env (or url and headers) in a "transport"
   object: take them from there, where the entry has none of its own. */
static void lift_transport(cJSON *cfg)
{
    static const char *const keys[] = {
        "command", "args", "env", "url", "headers"
    };
    cJSON *t = cJSON_GetObjectItem(cfg, "transport"), *v;
    size_t i;

    if (!cJSON_IsObject(t)) return;
    for (i = 0; i < sizeof keys / sizeof *keys; i++) {
        v = cJSON_GetObjectItem(t, keys[i]);
        if (v != NULL && cJSON_GetObjectItem(cfg, keys[i]) == NULL)
            cJSON_AddItemToObject(cfg, keys[i], cJSON_Duplicate(v, 1));
    }
}

void mcp_start(const char *path)
{
    FILE      *f;
    struct buf b;
    char       chunk[4096];
    size_t     n;
    cJSON     *root, *cfg;

    f = fopen(path, "rb");
    if (f == NULL) {
        if (errno != ENOENT)
            fprintf(stderr, "gork: cannot read %s: %s\n", path,
                    strerror(errno));
        return;
    }
    if (buf_init(&b) != 0) {
        fclose(f);
        return;
    }
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
        if (buf_add(&b, chunk, n) != 0) break;
    fclose(f);
    root = cJSON_Parse(b.p);
    buf_free(&b);
    if (root == NULL) {
        fprintf(stderr, "gork: %s is not valid JSON; no MCP servers\n", path);
        return;
    }

    cJSON_ArrayForEach(cfg, cJSON_GetObjectItem(root, "mcpServers")) {
        if (cJSON_IsTrue(cJSON_GetObjectItem(cfg, "disabled"))) continue;
        lift_transport(cfg);
        if (!cJSON_IsString(cJSON_GetObjectItem(cfg, "command")) &&
            !cJSON_IsString(cJSON_GetObjectItem(cfg, "url"))) {
            fprintf(stderr, "gork: MCP server %s: no command and no url\n",
                    cfg->string);
            continue;
        }
        start_server(cfg->string, cfg);
    }
    cJSON_Delete(root);
}

void mcp_stop(void)
{
    int i;

    for (i = 0; i < nservers; i++) stop_server(&servers[i]);
}

/* ------------------------------------------------------------------ */
/* tools                                                              */
/* ------------------------------------------------------------------ */

int mcp_tool_count(void)
{
    return nmtools;
}

int mcp_add_tools(cJSON *tools)
{
    cJSON *d;
    int    i;

    for (i = 0; i < nmtools; i++) {
        d = cJSON_Duplicate(mtools[i].def, 1);
        if (d == NULL) return -1;
        cJSON_AddItemToArray(tools, d);
    }
    return 0;
}

int mcp_find(const char *name)
{
    int i;

    for (i = 0; i < nmtools; i++)
        if (strcmp(cJSON_GetObjectItem(mtools[i].def, "name")->valuestring,
                   name) == 0)
            return i;
    return -1;
}

int *mcp_always(int i)
{
    return &mtools[i].always;
}

char *mcp_call(int i, cJSON *input, int *is_err)
{
    struct mtool  *t = &mtools[i];
    struct server *s = &servers[t->srv];
    cJSON         *params, *res, *item, *type, *text, *sc;
    struct buf     b;
    char           msg[320], *p;

    *is_err = 1;

    params = cJSON_CreateObject();
    if (params == NULL) return NULL;
    cJSON_AddStringToObject(params, "name", t->tool);
    cJSON_AddItemToObject(params, "arguments", input != NULL
                          ? cJSON_Duplicate(input, 1) : cJSON_CreateObject());

    res = rpc(s, "tools/call", params, relay_timeout);
    if (res == NULL) {
        sprintf(msg, "error: MCP server %.50s: %.200s", s->name, rpc_err);
        return mstrdup(msg);
    }
    if (buf_init(&b) != 0) {
        cJSON_Delete(res);
        return NULL;
    }

    /* Text is what a model can use; other content types are named, not
       dropped silently. */
    cJSON_ArrayForEach(item, cJSON_GetObjectItem(res, "content")) {
        type = cJSON_GetObjectItem(item, "type");
        text = cJSON_GetObjectItem(item, "text");
        if (cJSON_IsString(text)) {
            p = text->valuestring;
        } else {
            sprintf(msg, "[%.50s content omitted]",
                    cJSON_IsString(type) ? type->valuestring : "unknown");
            p = msg;
        }
        if (buf_add(&b, p, strlen(p)) != 0 || buf_add(&b, "\n", 1) != 0)
            goto oom;
    }
    sc = cJSON_GetObjectItem(res, "structuredContent");
    if (b.len == 0 && sc != NULL) {
        p = cJSON_PrintUnformatted(sc);
        if (p == NULL || buf_add(&b, p, strlen(p)) != 0) {
            free(p);
            goto oom;
        }
        free(p);
    }
    if (b.len > MCP_OUT_MAX) {
        b.len = MCP_OUT_MAX;
        b.p[b.len] = '\0';
        if (buf_add(&b, "\n[output truncated]", 19) != 0) goto oom;
    }

    *is_err = cJSON_IsTrue(cJSON_GetObjectItem(res, "isError"));
    cJSON_Delete(res);
    return b.p;

oom:
    buf_free(&b);
    cJSON_Delete(res);
    return NULL;
}
