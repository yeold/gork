/*
 * fakeapi.c -- a fake Messages API on 127.0.0.1:PORT (default 8099), plain
 * HTTP/1.0, for testing gork where the guest cannot reach the host.
 * A user turn starting with "go" gets tool calls (list_dir, write_file,
 * read_file, run_command); a turn holding tool results, or anything else,
 * gets a short text reply.  Each request body is appended to fakeapi.log.
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

int main(int argc, char **argv)
{
    struct sockaddr_in sa;
    int                s, c, n, len, want, one = 1;
    char              *body, *p, head[256];
    const char        *rep;
    FILE              *log;

    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(argc > 1 ? atoi(argv[1]) : 8099);
    sa.sin_addr.s_addr = htonl(0x7f000001L);
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return 1; }
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *) &one, sizeof one);
    if (bind(s, (struct sockaddr *) &sa, sizeof sa) < 0) { perror("bind"); return 1; }
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
        if (strstr(body, "tool_result") == NULL &&
            (strstr(body, "\"content\":\"go") != NULL ||
             strstr(body, "\"text\":\"go") != NULL))
            rep = tools_reply;
        else
            rep = text_reply;
        sprintf(head, "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n"
                      "Content-Length: %d\r\nConnection: close\r\n\r\n",
                (int) strlen(rep));
        write(c, head, strlen(head));
        write(c, rep, strlen(rep));
        close(c);
    }
}
