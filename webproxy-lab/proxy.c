#include "csapp.h"
#include <ctype.h>
#include <pthread.h>
#include <stddef.h>
#include <stdatomic.h>   // last_use, g_ticks

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

typedef struct Entry {
    char *key;
    char *data;
    size_t size;
    struct Entry *prev, *next;        // LRU Doubly구조 
    _Atomic unsigned long last_use;   // Lazy LRU 최근 접근 틱
} Entry;

typedef struct {
    Entry *head;   // MRU
    Entry *tail;   // LRU
    size_t bytes_used;
    pthread_rwlock_t rw;
} Cache;

// 전역 캐시 초기화 
static Cache g_cache = {
    .head = NULL, .tail = NULL, .bytes_used = 0,
    .rw = PTHREAD_RWLOCK_INITIALIZER
};

// 전역 타임스탬프
static _Atomic unsigned long g_ticks = 1;

static int get_cache(const char *key, char **out_data, size_t *out_size);

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
    "Firefox/10.0.3\r\n";

static void doit_proxy(int clientfd);
static void parse_uri(const char *uri, char *host, char *path, char *port);
static void request_headers(rio_t *client_rio, char *host, char *hdr_buf, size_t hdr_bufsz, int *has_host);
static void *handle_mul_cli(void * arg);

int main(int argc, char **argv) {

  if (argc != 2) {
      fprintf(stderr, "Usage: %s <port>\n", argv[0]);
      exit(1);
  }

  int listenfd = Open_listenfd(argv[1]);
  while (1) {
      struct sockaddr_storage clientaddr;
      socklen_t clientlen = sizeof(clientaddr);
      pthread_t tid;
      int *clientfd = malloc(sizeof(int));

      *clientfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);

      // handle multi client with thread
      pthread_create(&tid, NULL, handle_mul_cli, clientfd );
  }
}
// void * 반환 
static void *handle_mul_cli(void * arg) {
  pthread_detach(pthread_self()); //detach로 좀비 thread 회수

  int clientfd = *(int *)arg;
  free(arg);

  doit_proxy(clientfd);
  Close(clientfd);

  return(NULL);
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
    // 예) url = "http://localhost:8080/index.html"
    char host[MAXLINE], path[MAXLINE], port[16];
    parse_uri(uri, host, path, port);
 
    char key[MAXLINE];
    snprintf(key, sizeof(key), "%s:%s%s", host, port, path);
     
    char *cached = NULL; 
    size_t csize = 0;

    if (get_cache(key, &cached, &csize)) {
        // 캐시 히트시 바로 전송
        rio_writen(clientfd, cached, csize);
        free(cached);
        return;
    }

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

    size_t total = 0;
    int cacheable = 1;
    char *resp_buf = malloc(MAX_OBJECT_SIZE);
    if (!resp_buf) cacheable = 0;

    while ((cnt = Rio_readnb(&rio_server, buf, sizeof(buf))) > 0) {

      Rio_writen(clientfd, buf, cnt);

      //캐시 버퍼에 누적 (MAX_OBJECT_SIZE 넘으면 캐시 포기)
      if (cacheable) {
        if (total + (size_t)cnt <= MAX_OBJECT_SIZE) {
            memcpy(resp_buf + total, buf, (size_t)cnt);
            total += (size_t)cnt;
        } else {
            cacheable = 0;
        }
      } 
    }
    if (cacheable) {
        // 헤더+바디 전체 넣기 
        put_cache(key, resp_buf, total);
    }
    free(resp_buf);

    Close(serverfd);
}

/* 절대 uri 처리 
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
/* Utility*/
static Entry* find_entry(const char *key) {
    for (Entry *ent = g_cache.head; ent; ent = ent->next)
        if (strcmp(ent->key, key) == 0) return ent;
    return NULL;
}

// 첫 삽입 
static void insert_first(Entry *ent) {
    ent->prev = NULL;
    ent->next = g_cache.head;
    if (g_cache.head) g_cache.head->prev = ent;
    g_cache.head = ent;
    if (!g_cache.tail) g_cache.tail = ent;
}

// 재연결 후 삭제
static void list_remove_nolock(Entry *ent) {
    if (ent->prev) ent->prev->next = ent->next; 
    else g_cache.head = ent->next;
    
    if (ent->next) ent->next->prev = ent->prev; 
    else g_cache.tail = ent->prev;

    ent->prev = ent->next = NULL;
}

static void free_entry(Entry *ent) {
    free(ent->key);
    free(ent->data);
    free(ent);
}

// last_use가 가장 오래된 엔트리 선택 
static Entry* find_oldest(void) {
    Entry *cur = g_cache.head, *victim = NULL;
    unsigned long best = (unsigned long)-1;
    while (cur) {
        unsigned long lu = atomic_load_explicit(&cur->last_use, memory_order_relaxed);
        if (lu < best) { best = lu; victim = cur; }
        cur = cur->next;
    }
    return victim;
}

int get_cache(const char *key, char **out, size_t *out_sz) {
    int hit = 0;
    char *copy = NULL;
    size_t n = 0;

    pthread_rwlock_rdlock(&g_cache.rw);               
    Entry *ent = find_entry(key);
    if (ent) {
        // 복사본 생성 락 안에서 짧게 복사 (use-after-free 방지)
        copy = (char *)malloc(ent->size);
        if (copy) {
            memcpy(copy, ent->data, ent->size);
            n = ent->size;
            hit = 1;

            // 최근 접근 틱만 원자적으로 갱신 (리스트 이동 없음)
            unsigned long t = atomic_fetch_add_explicit(&g_ticks, 1, memory_order_relaxed);
            atomic_store_explicit(&ent->last_use, t, memory_order_relaxed);
        }
    }
    pthread_rwlock_unlock(&g_cache.rw);

    if (!hit) return 0;
    *out = copy;
    *out_sz = n;
    return 1;
}

void put_cache(const char *key, const char *blob, size_t n) {
    if (n > MAX_OBJECT_SIZE) return;

    pthread_rwlock_wrlock(&g_cache.rw);  

    // 중복 키 있으면 삭제 후 넣기
    Entry *dupe = find_entry(key);
    if (dupe) {
        g_cache.bytes_used -= dupe->size;
        list_remove_nolock(dupe);
        free_entry(dupe);
    }

    // 가장 오래 안 쓴 것부터 제거
    while (g_cache.bytes_used + n > MAX_CACHE_SIZE) {
        Entry *oldest = find_oldest();
        if (!oldest) break;
        g_cache.bytes_used -= oldest->size;
        list_remove_nolock(oldest);
        free_entry(oldest);
    }

    // 새 엔트리 생성
    Entry *new_enty = (Entry *)malloc(sizeof(*new_enty));
    // if (!new_enty) { pthread_rwlock_unlock(&g_cache.rw); return; }
    new_enty->key = strdup(key);
    new_enty->data = (char *)malloc(n);
    if (!new_enty->key || !new_enty->data) {
        free(new_enty->key); 
        free(new_enty->data); 
        free(new_enty);
        pthread_rwlock_unlock(&g_cache.rw);
        return;
    }
    memcpy(new_enty->data, blob, n);
    new_enty->size = n;
    new_enty->prev = new_enty->next = NULL;

    unsigned long t = atomic_fetch_add_explicit(&g_ticks, 1, memory_order_relaxed);
    atomic_store_explicit(&new_enty->last_use, t, memory_order_relaxed);

    insert_first(new_enty);
    g_cache.bytes_used += n;

    pthread_rwlock_unlock(&g_cache.rw);
}


