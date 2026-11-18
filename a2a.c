/*
 * a2a.c -- Agent2Agent (A2A) server: gork as a remote agent other agents delegate to.
 *
 * Spec: https://a2a-protocol.org/latest/specification/
 * JSON-RPC binding: application/json, A2A-Version header.
 * Operations (section 5.3): message/send, tasks/get, tasks/cancel,
 * push-notification configs (later).
 *
 * One A2A task = one gork conversation started from the message's text parts.
 * file parts -> read_file; data parts -> dropped.
 * contextId groups tasks onto one conversation.
 * artifacts carry the final answer as a single text part.
 * input-required = tui_confirm() shape without terminal (lazier: --a2a-trust).
 *
 * Not for GORK_NO_FORK, GORK_BARE, --with-serial, firmware builds.
 */
#ifndef GORK_NO_FORK

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "port.h"
#include "a2a.h"
#include "relay.h"              /* struct buf */
#include "cJSON/cJSON.h"

/* ------------------------------------------------------------------ config */

#define A2A_MAX_TASKS  64
#define A2A_PORT_DEFAULT 8445
#define A2A_VERSION "1.0"

/* ------------------------------------------------------------------ task */

struct a2a_task {
    char           *id;              /* task id */
    char           *context_id;      /* contextId from message */
    char           *status;          /* submitted, working, input-required, completed, failed, canceled, rejected */
    cJSON          *messages;        /* gork's conversation (cJSON array) */
    char           *artifact_text;   /* final answer text */
    int             has_artifact;
};

static struct a2a_task tasks[A2A_MAX_TASKS];
static int             task_count;

/* ------------------------------------------------------------------ utils */

static char *a2a_strdup(const char *s)
{
    if (s == NULL) return NULL;
    char *d = malloc(strlen(s) + 1);
    if (d) strcpy(d, s);
    return d;
}

static void a2a_task_free(struct a2a_task *t)
{
    if (t == NULL) return;
    free(t->id);
    free(t->context_id);
    free(t->status);
    if (t->messages) cJSON_Delete(t->messages);
    free(t->artifact_text);
    memset(t, 0, sizeof *t);
}

static struct a2a_task *a2a_task_find(const char *id)
{
    for (int i = 0; i < task_count; i++)
        if (tasks[i].id && strcmp(tasks[i].id, id) == 0)
            return &tasks[i];
    return NULL;
}

static struct a2a_task *a2a_task_find_by_context(const char *context_id)
{
    for (int i = 0; i < task_count; i++)
        if (tasks[i].context_id && strcmp(tasks[i].context_id, context_id) == 0)
            return &tasks[i];
    return NULL;
}

static char *a2a_uuid(void)
{
    /* cheap uuid: random + counter */
    static int ctr = 0;
    char *s = malloc(64);
    if (!s) return NULL;
    sprintf(s, "task-%08x-%04x", (unsigned)time(NULL), ++ctr);
    return s;
}

/* ------------------------------------------------------------------ Agent Card */

static const char *a2a_agent_card =
    "{"
    "  \"name\": \"gork\","
    "  \"description\": \"C89 AI coding agent for retro and modern systems\","
    "  \"version\": \"1.7.0\","
    "  \"skills\": ["
    "    { \"id\": \"coding\", \"name\": \"Code editing\", \"description\": \"Read, write, edit files and run commands\" }"
    "  ],"
    "  \"capabilities\": {"
    "    \"streaming\": false,"
    "    \"pushNotifications\": false,"
    "    \"stateTransitionHistory\": false"
    "  },"
    "  \"securitySchemes\": {"
    "    \"bearer\": { \"type\": \"http\", \"scheme\": \"bearer\" }"
    "  },"
    "  \"supportedInterfaces\": [\"jsonrpc\"]"
    "}";

/* ------------------------------------------------------------------ JSON-RPC */

static void a2a_send_json(int fd, cJSON *obj)
{
    if (!obj) return;
    char *s = cJSON_PrintUnformatted(obj);
    if (!s) { cJSON_Delete(obj); return; }
    size_t n = strlen(s);
    char hdr[128];
    int hl = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "A2A-Version: %s\r\n"
        "Connection: close\r\n"
        "\r\n", n, A2A_VERSION);
    (void) write(fd, hdr, hl);
    (void) write(fd, s, n);
    free(s);
    cJSON_Delete(obj);
}

static void a2a_send_error(int fd, cJSON *id, int code, const char *msg)
{
    cJSON *e = cJSON_CreateObject(), *obj = cJSON_CreateObject();
    if (!e || !obj) { cJSON_Delete(e); cJSON_Delete(obj); return; }
    cJSON_AddNumberToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", msg);
    cJSON_AddItemToObject(obj, "jsonrpc", cJSON_CreateString("2.0"));
    if (id) cJSON_AddItemToObject(obj, "id", cJSON_Duplicate(id, 1));
    cJSON_AddItemToObject(obj, "error", e);
    a2a_send_json(fd, obj);
}

static void a2a_send_result(int fd, cJSON *id, cJSON *result)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return;
    cJSON_AddStringToObject(obj, "jsonrpc", "2.0");
    if (id) cJSON_AddItemToObject(obj, "id", cJSON_Duplicate(id, 1));
    cJSON_AddItemToObject(obj, "result", result);
    a2a_send_json(fd, obj);
}

/* ------------------------------------------------------------------ message/send */

static cJSON *a2a_extract_text(cJSON *parts)
{
    if (!cJSON_IsArray(parts)) return NULL;
    struct buf buf;
    if (buf_init(&buf) != 0) return NULL;
    cJSON *p;
    cJSON_ArrayForEach(p, parts) {
        cJSON *type = cJSON_GetObjectItem(p, "type");
        if (!cJSON_IsString(type) || strcmp(type->valuestring, "text") != 0) continue;
        cJSON *text = cJSON_GetObjectItem(p, "text");
        if (!cJSON_IsString(text)) continue;
        if (buf.len > 0) buf_add(&buf, "\n", 1);
        buf_add(&buf, text->valuestring, strlen(text->valuestring));
    }
    cJSON *msg = buf.len > 0 ? cJSON_CreateString(buf.p) : NULL;
    buf_free(&buf);
    return msg;
}

static void a2a_handle_message_send(int fd, cJSON *id, cJSON *params)
{
    cJSON *msg = cJSON_GetObjectItem(params, "message");
    if (!msg) { a2a_send_error(fd, id, -32602, "message/send needs message"); return; }

    cJSON *tid = cJSON_GetObjectItem(msg, "taskId");
    cJSON *cid = cJSON_GetObjectItem(msg, "contextId");
    cJSON *parts = cJSON_GetObjectItem(msg, "parts");

    cJSON *text = a2a_extract_text(parts);
    if (!text) { a2a_send_error(fd, id, -32602, "no text part in message"); return; }

    struct a2a_task *task;
    if (tid && cJSON_IsString(tid)) {
        task = a2a_task_find(tid->valuestring);
    } else {
        task = a2a_task_find_by_context(cid && cJSON_IsString(cid) ? cid->valuestring : "");
    }
    if (!task) {
        if (task_count >= A2A_MAX_TASKS) {
            a2a_send_error(fd, id, -32603, "task limit reached");
            cJSON_Delete(text);
            return;
        }
        task = &tasks[task_count++];
        task->id = a2a_uuid();
        task->context_id = cid && cJSON_IsString(cid) ? a2a_strdup(cid->valuestring) : a2a_strdup("default");
        task->status = a2a_strdup("submitted");
        task->messages = cJSON_CreateArray();
        task->has_artifact = 0;
        task->artifact_text = NULL;
    }
    task->status = a2a_strdup("working");

    if (append_text_message(task->messages, "user", text->valuestring) != 0) {
        cJSON_Delete(text);
        a2a_send_error(fd, id, -32603, "out of memory");
        return;
    }
    cJSON_Delete(text);

    /* run the task synchronously (could be async with push later) */
    const char *stop = run_task(task->conn, task->messages);
    if (stop && strcmp(stop, "end_turn") != 0 && strcmp(stop, "max_turn_requests") != 0) {
        task->status = a2a_strdup("failed");
        a2a_send_error(fd, id, -32603, "task failed");
        return;
    }

    /* last assistant message is the artifact */
    int n = cJSON_GetArraySize(task->messages);
    cJSON *last = n > 0 ? cJSON_GetArrayItem(task->messages, n - 1) : NULL;
    if (last && cJSON_GetObjectItem(last, "role") &&
        strcmp(cJSON_GetObjectItem(last, "role")->valuestring, "assistant") == 0) {
        cJSON *content = cJSON_GetObjectItem(last, "content");
        if (cJSON_IsArray(content)) {
            cJSON *c;
            cJSON_ArrayForEach(c, content) {
                cJSON *type = cJSON_GetObjectItem(c, "type");
                if (cJSON_IsString(type) && strcmp(type->valuestring, "text") == 0) {
                    cJSON *txt = cJSON_GetObjectItem(c, "text");
                    if (cJSON_IsString(txt)) {
                        task->artifact_text = a2a_strdup(txt->valuestring);
                        task->has_artifact = 1;
                        break;
                    }
                }
            }
        }
    }
    task->status = a2a_strdup("completed");

    /* return Task object */
    cJSON *res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "id", task->id);
    cJSON_AddStringToObject(res, "contextId", task->context_id);
    cJSON_AddStringToObject(res, "status", task->status);
    if (task->has_artifact) {
        cJSON *art = cJSON_CreateObject();
        cJSON *p = cJSON_CreateArray();
        cJSON *ct = cJSON_CreateObject();
        cJSON_AddStringToObject(ct, "type", "text");
        cJSON_AddStringToObject(ct, "text", task->artifact_text);
        cJSON_AddItemToArray(p, ct);
        cJSON_AddItemToObject(art, "parts", p);
        cJSON_AddItemToObject(res, "artifacts", art);
    }
    a2a_send_result(fd, id, res);
}

/* ------------------------------------------------------------------ tasks/get */

static void a2a_handle_tasks_get(int fd, cJSON *id, cJSON *params)
{
    cJSON *tid = cJSON_GetObjectItem(params, "id");
    if (!tid || !cJSON_IsString(tid)) { a2a_send_error(fd, id, -32602, "tasks/get needs id"); return; }
    struct a2a_task *task = a2a_task_find(tid->valuestring);
    if (!task) { a2a_send_error(fd, id, -32601, "task not found"); return; }
    cJSON *res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "id", task->id);
    cJSON_AddStringToObject(res, "contextId", task->context_id);
    cJSON_AddStringToObject(res, "status", task->status);
    if (task->has_artifact) {
        cJSON *art = cJSON_CreateObject();
        cJSON *p = cJSON_CreateArray();
        cJSON *ct = cJSON_CreateObject();
        cJSON_AddStringToObject(ct, "type", "text");
        cJSON_AddStringToObject(ct, "text", task->artifact_text);
        cJSON_AddItemToArray(p, ct);
        cJSON_AddItemToObject(art, "parts", p);
        cJSON_AddItemToObject(res, "artifacts", art);
    }
    a2a_send_result(fd, id, res);
}

/* ------------------------------------------------------------------ tasks/cancel */

static void a2a_handle_tasks_cancel(int fd, cJSON *id, cJSON *params)
{
    cJSON *tid = cJSON_GetObjectItem(params, "id");
    if (!tid || !cJSON_IsString(tid)) { a2a_send_error(fd, id, -32602, "tasks/cancel needs id"); return; }
    struct a2a_task *task = a2a_task_find(tid->valuestring);
    if (!task) { a2a_send_error(fd, id, -32601, "task not found"); return; }
    task->status = a2a_strdup("canceled");
    cJSON *res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "id", task->id);
    cJSON_AddStringToObject(res, "status", "canceled");
    a2a_send_result(fd, id, res);
}

/* ------------------------------------------------------------------ dispatch */

static void a2a_dispatch(int fd, cJSON *req)
{
    cJSON *id = cJSON_GetObjectItem(req, "id");
    cJSON *method = cJSON_GetObjectItem(req, "method");
    cJSON *params = cJSON_GetObjectItem(req, "params");
    if (!cJSON_IsString(method)) { a2a_send_error(fd, id, -32600, "invalid method"); return; }
    const char *m = method->valuestring;
    if (strcmp(m, "message/send") == 0) a2a_handle_message_send(fd, id, params);
    else if (strcmp(m, "tasks/get") == 0) a2a_handle_tasks_get(fd, id, params);
    else if (strcmp(m, "tasks/cancel") == 0) a2a_handle_tasks_cancel(fd, id, params);
    else { a2a_send_error(fd, id, -32601, "method not found"); }
}

/* ------------------------------------------------------------------ HTTP server */

static int a2a_read_request(int fd, cJSON **out)
{
    struct buf buf;
    if (buf_init(&buf) != 0) return -1;
    char chunk[4096];
    ssize_t n;
    int content_len = -1;
    while ((n = read(fd, chunk, sizeof chunk)) > 0) {
        if (buf_add(&buf, chunk, n) != 0) { buf_free(&buf); return -1; }
        if (content_len < 0) {
            char *cl = strstr(buf.p, "Content-Length:");
            if (cl) content_len = atoi(cl + 15);
        }
        if (content_len >= 0 && buf.len >= content_len + 200) break; /* rough */
        if (strstr(buf.p, "\r\n\r\n") && content_len >= 0 && buf.len >= content_len) break;
    }
    if (buf.len == 0) { buf_free(&buf); return 0; }
    char *body = strstr(buf.p, "\r\n\r\n");
    if (!body) { buf_free(&buf); return 0; }
    body += 4;
    *out = cJSON_Parse(body);
    buf_free(&buf);
    return *out ? 1 : 0;
}

int a2a_serve(int port)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = INADDR_ANY };
    if (bind(sock, (struct sockaddr *)&addr, sizeof addr) < 0) { close(sock); return -1; }
    if (listen(sock, 16) < 0) { close(sock); return -1; }
    for (;;) {
        int fd = accept(sock, NULL, NULL);
        if (fd < 0) continue;
        cJSON *req = NULL;
        if (a2a_read_request(fd, &req) == 1 && req) {
            /* check .well-known/agent-card.json path first */
            /* (simplified: just dispatch JSON-RPC) */
            a2a_dispatch(fd, req);
            cJSON_Delete(req);
        }
        close(fd);
    }
    close(sock);
    return 0;
}

#else /* GORK_NO_FORK */

int a2a_serve(int port) { (void) port; return -1; }

#endif /* !GORK_NO_FORK */