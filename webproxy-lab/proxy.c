#include "csapp.h"
#include <ctype.h>

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
    "Firefox/10.0.3\r\n";

static void doit_proxy(int clientfd);
static void parse_uri(const char *uri, char *host, char *path, char *port);
static void request_headers(rio_t *client_rio, char *host, char *hdr_buf, size_t hdr_bufsz, int *has_host);

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);
    }

    int listenfd = Open_listenfd(argv[1]);
    while (1) {
        struct sockaddr_storage clientaddr;
        socklen_t clientlen = sizeof(clientaddr);
        int clientfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        doit_proxy(clientfd);
        Close(clientfd);
    }
}

static void doit_proxy(int clientfd) {
    rio_t rio_client;
    Rio_readinitb(&rio_client, clientfd);

    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    if (Rio_readlineb(&rio_client, buf, sizeof(buf)) <= 0) return;

    /* Parse request line */
    if (sscanf(buf, "%s %s %s", method, uri, version) != 3) return;
    if (strcasecmp(method, "GET")) {
        return;
    }

    // URI parse: host, path, port 
    // 예) uri = "http://localhost:8080/index.html"
    char host[MAXLINE], path[MAXLINE], port[16];
    parse_uri(uri, host, path, port);

    /* 조건 HTTP/1.0 만 허용 */
    char server_req[MAXLINE];
    int n = snprintf(server_req, sizeof(server_req), "GET %s HTTP/1.0\r\n,",path);
    if (n < 0 || n >= (int)sizeof(server_req)) return;

    // 나머지 headers 담기 
    char hdrs[MAXLINE*8];
    int has_host = 0;
    //host 찾기 
    request_headers(&rio_client, host, hdrs, sizeof(hdrs), &has_host);

    // HTTP 1.0은 Host가 없을 수 있음
    // 클라이언트가 Host 헤더를 안 보냈다면,Host 헤더를 만들어 줘야함
    char host_line[MAXLINE] = {0};
    if (!has_host) {
        snprintf(host_line, sizeof(host_line), "Host: %s\r\n", host);
    }

    char request_f[MAXLINE * 12];
    int written = snprintf(request_f, sizeof(request_f),
        "GET %s HTTP/1.0\r\n"
        "%s"  // Host (없으면 빈 문자열) 
        "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n"
        "Connection: close\r\n"
        "Proxy-Connection: close\r\n"
        "%s"  // 기타 header
        "\r\n", 
        path,
        host_line,
        hdrs
    );
    if (written < 0 || written >= (int)sizeof(request_f)) return;

    int serverfd = Open_clientfd(host, port);
    if (serverfd < 0) return;

    Rio_writen(serverfd, request_f, strlen(request_f));

    rio_t rio_server;
    Rio_readinitb(&rio_server, serverfd);
    ssize_t cnt;
    while ((cnt = Rio_readnb(&rio_server, buf, sizeof(buf))) > 0) {
        Rio_writen(clientfd, buf, cnt);
    }

    Close(serverfd);
}

/* 절대 url 처리 
  http://host[:port]/path  (default port 80) */
static void parse_uri(const char *uri, char *host, char *path, char *port) {
    const char *u = uri;

    if (!strncasecmp(u, "http://", 7)) u += 7;

    // host[:port][/<path>] 
    const char *slash = strchr(u, '/');
    if (slash) {
        size_t hostlen = slash - u;
        strncpy(host, u, hostlen);
        host[hostlen] = '\0';
        snprintf(path, MAXLINE, "/%s", slash + 1);
        if (path[1] == '\0') strcpy(path, "/");
    } else {
        strcpy(host, u);
        strcpy(path, "/");
    }

    /* host:port */
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = '\0';
        strcpy(port, colon + 1);
    } else {
        strcpy(port, "80");
    }
}

static void request_headers(rio_t *client_rio, char *host, char *hdr_buf, size_t hdr_bufsz, int *has_host) {
    *has_host = 0;
    hdr_buf[0] = '\0';

    char line[MAXLINE];
    while (1) {
        if (Rio_readlineb(client_rio, line, sizeof(line)) <= 0) break;
        if (!strcmp(line, "\r\n")) break; // 헤더 끝

        char key[MAXLINE]; int i = 0;
        while (line[i] && line[i] != ':' && i < (int)sizeof(key)-1) {
            key[i] = line[i];
            i++;
        }
        key[i] = '\0';
        for (int j = 0; key[j]; ++j) key[j] = tolower((unsigned char)key[j]);

        if (!strncmp(key, "host", 4)) {
            *has_host = 1;
            continue;
        }
        if (!strncmp(key, "user-agent", 10)) continue;
        if (!strncmp(key, "connection", 10)) continue;
        if (!strncmp(key, "proxy-connection", 16)) continue;

        size_t cur = strlen(hdr_buf);
        size_t left = hdr_bufsz - cur - 1;
        if (left > 0) {
            strncat(hdr_buf, line, left);
        }
    }
}
