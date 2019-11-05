#ifndef UDP_H
#define UDP_H

#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

typedef struct udp {
	int sockfd;
    
    char *msg_send;
    size_t msg_send_size;
    size_t bytes_to_send;
    ssize_t bytes_sent;

    struct sockaddr_in server_addr;
    socklen_t server_addr_size;

    char *msg_recv;
    size_t msg_recv_size;
    ssize_t bytes_recv;

    struct sockaddr_in client_addr;
    socklen_t client_addr_size;
} udp_t;

udp_t* udp_create(const char *port, size_t msg_send_size, size_t msg_recv_size);

int udp_set_server_addr(udp_t * udp, char *addr, int port);

int udp_send(udp_t* udp);

int udp_recv(udp_t* udp);

int udp_delete(udp_t* udp);

#endif /* UDP_H */
