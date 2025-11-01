#include "csapp.h"
#include <sys/socket.h>
#include <stdio.h>

int main(int argc, char **argv){
    if (argc != 2) { 
        fprintf(stderr,"usage: %s <port>\n", argv[0]); exit(1); 
    }

    // struct sockaddr_in {
    // unsigned short sin_family;  // 주소 체계 
    // unsigned short sin_port;    // 포트 번호 (network byte order)
    // struct in_addr sin_addr;    // IP 주소 (network byte order)
    // unsigned char sin_zero[8];  // 패딩 (항상 0으로 초기화)
    // };    
    struct sockaddr_in serveraddr;
    serveraddr.sin_family = AF_INET; //IPv4
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY); // 모두 허용 (0.0.0.0) // big endian으로 
    serveraddr.sin_port= htons(atoi(argv[1]));

    // socket(int domain, int type, int protocol);
    // listen 전용 socket
    int newsocket = socket(serveraddr.sin_family, SOCK_STREAM, 0);
    if (newsocket < 0){
        exit(1);
    }
    
    int opt = 1;
    if (setsockopt(newsocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        exit(1);
    }

    // 소켓 총 길이 
    socklen_t address_len = sizeof(serveraddr);

    //bind(int socket, const struct sockaddr *address, socklen_t address_len);
    if (bind(newsocket, (struct sockaddr *)&serveraddr, address_len) < 0){
        exit(1);
    }

    // listen(int socket, int backlog);
    if (listen(newsocket, 1024) < 0){
        exit(1);
    }
    printf("listening on 0.0.0.0:%s\n", argv[1]);

    //accept(int socket, struct sockaddr *restrict address, socklen_t *restrict address_len);
    while (1){
        // client용 
        struct sockaddr_in clientaddr;
        socklen_t c_addr_len = sizeof(clientaddr);
        
        // client 통신용 socket 생성 
        int cli_socket = accept(newsocket,(struct sockaddr *restrict)&clientaddr,&c_addr_len);

        if (cli_socket < 0) { perror("accept");
            continue; 
        }
        char buf[4096];
        while (1){
            // read(int fildes, void *buf, size_t nbyte);
            ssize_t nb = read(cli_socket, buf, sizeof(buf));

            // send(int socket, const void *buffer, size_t length, int flags);
            if (nb > 0){
                if( send(cli_socket, buf, nb, 0) < 0) perror("send");
            } else if (nb == 0){
                close(cli_socket);
                break;
            }
            // read error 
            else perror("read");
        }
    }
    close(newsocket);
}