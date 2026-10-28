/*
 * fakeapi.c -- a fake Messages API on 127.0.0.1:PORT (default 8099), plain
 * HTTP/1.0, for testing gork where the guest cannot reach the host.
 * A user turn starting with "go" gets tool calls (list_dir, write_file,
 * read_file, run_command); "w1" a write_file, "w2" one of a name and
 * text beyond ASCII, "t1" a run_command of sleep 100, "m1" a call to the
 * MCP server named fake (testing/fakemcp.py), "m2" one to the MCP server
 * named sh, "esc" two paths outside the working directory, and "slow"
 * waits 30 s.  Only the last user turn counts: one
 * holding tool results, or anything else, gets a short text reply.  Each
 * request body is appended to fakeapi.log.  testing/vmcheck.sh runs gork
 * against it.
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TOOL1(id, name, input) \
"{\"id\":\"m1\",\"type\":\"message\",\"role\":\"assistant\",\"stop_reason\":\"tool_use\"," \
"\"content\":[{\"type\":\"tool_use\",\"id\":\"" id "\",\"name\":\"" name "\",\"input\":" input "}]," \
"\"usage\":{\"input_tokens\":1000,\"output_tokens\":20}}"

static const struct { const char *turn, *reply; } one_call[] = {
    { "w1",  TOOL1("t1", "write_file", "{\"path\":\"w1.txt\",\"content\":\"x\"}") },
    /* a name and text in UTF-8 beyond ASCII: a lobster sandwich, a euro,
       a dash and an emoji, for the Mac's Mac Roman */
    { "w2",  TOOL1("t1", "write_file", "{\"path\":\"r\xc3\xa4ksm\xc3\xb6rg\xc3\xa5s.txt\","
                   "\"content\":\"\xe2\x82\xac 5 \xe2\x80\x94 \xf0\x9f\x98\x80\\n\"}") },
    { "t1",  TOOL1("t1", "run_command", "{\"command\":\"sleep 100\"}") },
    { "m1",  TOOL1("t1", "mcp__fake__echo", "{\"text\":\"hello over MCP\"}") },
    { "m2",  TOOL1("t1", "mcp__sh__echo", "{\"text\":\"hi\"}") },
    { NULL, NULL }
};

static const char esc_reply[] =
"{\"id\":\"m1\",\"type\":\"message\",\"role\":\"assistant\",\"stop_reason\":\"tool_use\","
"\"content\":[{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"write_file\",\"input\":{\"path\":\"../escape.txt\",\"content\":\"x\"}},"
"{\"type\":\"tool_use\",\"id\":\"t2\",\"name\":\"read_file\",\"input\":{\"path\":\"/etc/passwd\"}}],"
"\"usage\":{\"input_tokens\":1000,\"output_tokens\":20}}";

static const char tools_reply[] =
"{\"id\":\"m1\",\"type\":\"message\",\"role\":\"assistant\",\"stop_reason\":\"tool_use\","
"\"content\":[{\"type\":\"text\",\"text\":\"Working.\"},"
"{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"list_dir\",\"input\":{\"path\":\".\"}},"
"{\"type\":\"tool_use\",\"id\":\"t2\",\"name\":\"write_file\",\"input\":{\"path\":\"out.txt\",\"content\":\"a\\nb\\n\"}},"
"{\"type\":\"tool_use\",\"id\":\"t3\",\"name\":\"read_file\",\"input\":{\"path\":\"out.txt\"}},"
"{\"type\":\"tool_use\",\"id\":\"t4\",\"name\":\"run_command\",\"input\":{\"command\":\"uname -a\"}}],"
"\"usage\":{\"input_tokens\":1000,\"output_tokens\":20}}";

static const char text_reply[] =
"{\"id\":\"m2\",\"type\":\"message\",\"role\":\"assistant\",\"stop_reason\":\"end_turn\","
"\"content\":[{\"type\":\"text\",\"text\":\"PONG from the fake API\"}],"
"\"usage\":{\"input_tokens\":1200,\"output_tokens\":9}}";

static char req[200000];

/* The user turn is a string, or a text block in the TUI. */
static int turn_is(const char *body, const char *turn)
{
    char pat[64];

    sprintf(pat, "\"content\":\"%s\"", turn);
    if (strstr(body, pat) != NULL) return 1;
    sprintf(pat, "\"text\":\"%s\"", turn);
    return strstr(body, pat) != NULL;
}

int main(int argc, char **argv)
{
    struct sockaddr_in sa;
    int                s, c, n, len, want, one = 1;
    char              *body, *p, head[256];
    const char        *rep, *last, *u;
    FILE              *log;
    int                i;

    signal(SIGPIPE, SIG_IGN);   /* a client that gave up must not end us */
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(argc > 1 ? atoi(argv[1]) : 8099);
    sa.sin_addr.s_addr = htonl(0x7f000001L);
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return 1; }
#ifdef SO_REUSEADDR
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *) &one, sizeof one);
#endif
    if (bind(s, (struct sockaddr *) &sa, sizeof sa) < 0) {
        /* Plan 9's APE binds only the wildcard address */
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s, (struct sockaddr *) &sa, sizeof sa) < 0) { perror("bind"); return 1; }
        fprintf(stderr, "fakeapi: listening on all addresses\n");
    }
    if (listen(s, 5) < 0) { perror("listen"); return 1; }
    for (;;) {
        c = accept(s, (struct sockaddr *) 0, 0);
        if (c < 0) { perror("accept"); continue; }
        len = 0;
        want = -1;
        body = NULL;
        while (len < (int) sizeof req - 1) {
            n = read(c, req + len, sizeof req - 1 - len);
            if (n <= 0) break;
            len += n;
            req[len] = '\0';
            if (body == NULL && (p = strstr(req, "\r\n\r\n")) != NULL) {
                body = p + 4;
                p = strstr(req, "Content-Length:");
                if (p == NULL) p = strstr(req, "content-length:");
                want = p != NULL ? atoi(p + 15) : 0;
            }
            if (body != NULL && len - (body - req) >= want) break;
        }
        if (body == NULL) body = req + len;
        log = fopen("fakeapi.log", "a");
        if (log != NULL) { fputs(body, log); fputs("\n", log); fclose(log); }
        /* The last user turn: the TUI sends the whole conversation. */
        last = body;
        while ((u = strstr(last + 1, "\"role\":\"user\"")) != NULL)
            last = u;
        rep = text_reply;
        if (strstr(last, "tool_result") == NULL) {
            if (strstr(last, "\"content\":\"go") != NULL ||
                strstr(last, "\"text\":\"go") != NULL)
                rep = tools_reply;
            else if (turn_is(last, "esc"))
                rep = esc_reply;
            else if (turn_is(last, "slow"))
                sleep(30);
            for (i = 0; one_call[i].turn != NULL; i++)
                if (turn_is(last, one_call[i].turn))
                    rep = one_call[i].reply;
        }
        sprintf(head, "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n"
                      "Content-Length: %d\r\nConnection: close\r\n\r\n",
                (int) strlen(rep));
        write(c, head, strlen(head));
        write(c, rep, strlen(rep));
        close(c);
    }
}
