/* $begin tinymain */
/*
 * tiny.c - A simple, iterative HTTP/1.0 Web server that uses the
 *     GET method to serve static and dynamic content.
 *
 * Updated 11/2019 droh
 *   - Fixed sprintf() aliasing issue in serve_static(), and clienterror().
 */
#include "csapp.h"
#include <sys/socket.h>
#include <stdio.h>

void send_response(int cli, int code, const char *msg, const char *body) {
    char header[512];
    sprintf(header,
        "HTTP/1.1 %d %s\r\n"
        "Server: Tiny\r\n"
        "Content-Length: %ld\r\n"
        "Content-Type: text/html\r\n\r\n",
        code, msg, strlen(body));
    send(cli, header, strlen(header), 0);
    send(cli, body, strlen(body), 0);
}

int main(int argc, char **argv){
    if (argc != 2) { 
        fprintf(stderr,"usage: %s <port>\n", argv[0]); exit(1); 
    }
   
    struct sockaddr_in serveraddr;
    serveraddr.sin_family = AF_INET; //IPv4
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY); // 모두 허용 (0.0.0.0) // big endian으로 
    serveraddr.sin_port= htons(atoi(argv[1]));

    int newsocket = socket(serveraddr.sin_family, SOCK_STREAM, 0);
    if (newsocket < 0){
        exit(1);
    }
    
    int opt = 1;
    if (setsockopt(newsocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        exit(1);
    }

    socklen_t address_len = sizeof(serveraddr);

    if (bind(newsocket, (struct sockaddr *)&serveraddr, address_len) < 0){
        exit(1);
    }

    if (listen(newsocket, 1024) < 0){
        exit(1);
    }
    printf("listening on 0.0.0.0:%s\n", argv[1]);

    while (1){
        // client용 
        struct sockaddr_in clientaddr;
        socklen_t c_addr_len = sizeof(clientaddr);
        
        // client 통신용 socket 생성 
        int cli_socket = accept(newsocket,(struct sockaddr *restrict)&clientaddr,&c_addr_len);

        if (cli_socket < 0) { perror("accept");
            continue; 
        }
        char request_buf[4096];

        // read(int fildes, void *buf, size_t nbyte);
        ssize_t nb = read(cli_socket, request_buf, sizeof(request_buf)-1);
        if (nb <= 0) { close(cli_socket); continue; }
        request_buf[nb] = '\0';                      

        // request line 
        // request buf 에서 첫 \r\n 찾아서 request line 문자열로 만들기
        char *line_end = strstr(request_buf, "\r\n");
        if (!line_end) { send_response(cli_socket, 400, "Bad Request", "<h1>400 Bad Request</h1>\r\n"); break; }
        *line_end = '\0';

        // http request parsing <HTTP Method> <Request URL> <HTTP Version>
        // GET /index.html HTTP/1.1
        char *method, *url, *version;
        method = strtok(request_buf, " ");
        url = strtok(NULL," ");
        version = strtok(NULL," ");

        // 루트 → index.html
        if (strcmp(url, "/") == 0) url = "/index.html";

        // 선행 '/' 제거해서 상대경로로
        if (url[0] == '/') url++;

        printf("URL: %s\n", url);
        int status = 200; // 기본 status
        char body_buf[4096];        
        char header_buf[512];

        //GET만 받기
        if (!method || strcmp(method, "GET") != 0) {
            send_response(cli_socket, 400, "Bad Request", "<h1>400 Bad Request</h1>\r\n");
            continue;
        }
          //GET URL확인 

        // url 열기 
        int open_fd = open(url, O_RDONLY);
        if (open_fd < 0) {
            send_response(cli_socket, 404, "Not Found", "<h1>404 Not Found</h1>>\r\n");
            continue;
        }      
        
        // url 파일 읽고 바디 버퍼에 넣기
        ssize_t read_nb = read(open_fd, body_buf,sizeof(body_buf));
        if (read_nb < 0 ) {perror("read error"); exit(1);}

        // header 
        sprintf(header_buf,
            "HTTP/1.1 200 OK\r\n"
            "Server: Tiny\r\n"
            "Content-Length: %ld\r\n"
            "Content-Type: text/html\r\n\r\n",
            read_nb
        );

        send(cli_socket,header_buf,strlen(header_buf), 0);
        send(cli_socket,body_buf, read_nb, 0);
        close(open_fd);

    }

    close(newsocket);
}