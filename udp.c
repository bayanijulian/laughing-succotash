#include "udp.h"

udp_t* udp_create(const char *port, size_t msg_send_size, size_t msg_recv_size) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype =  SOCK_DGRAM;
    hints.ai_flags =  AI_PASSIVE;

    getaddrinfo(NULL, port, &hints, &res); // port to recv

    int sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    
    if (sockfd < 0) {
        perror("udp_create: socket");
        exit(1);
    
    }
    // printf("socket created at %d\n", sockfd);

    int optval = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval)) == -1) {
        close(sockfd);
        perror("udp_create: set socket options");
        exit(1);
    }

    if (bind(sockfd, res->ai_addr, res->ai_addrlen) != 0) {
        perror("udp_create: bind()");
        exit(1);
    }

	udp_t *udp = malloc(sizeof(udp_t));
	udp->sockfd = sockfd;

    udp->msg_send = malloc(msg_send_size);
    udp->msg_send_size = msg_send_size;
    udp->bytes_to_send = 0;
    udp->bytes_sent = -1;
    udp->server_addr_size = sizeof(struct sockaddr_in);

    udp->msg_recv = malloc(msg_recv_size);
    udp->msg_recv_size = msg_recv_size;
    udp->bytes_recv = -1;
    udp->client_addr_size = sizeof(struct sockaddr_in);
	return udp;
}

int udp_set_server_addr(udp_t *udp, char *addr, int port) {
    // uses UDP's current client address and sets it to send
    if (addr == NULL && port == -1) {
        if (udp->client_addr_size != sizeof(struct sockaddr_in)) {
            fprintf(stderr, "udp_set_server_addr: client addr size is not expexted\n");
        }
        udp->server_addr = udp->client_addr;
        return 0;
    }

    // addr is an IPv4 address
    struct sockaddr_in *server_address = &udp->server_addr;
    server_address->sin_family = AF_INET;
    server_address->sin_port = htons(port);
    inet_pton(AF_INET, addr, &server_address->sin_addr);
    return 0;
}

int udp_send(udp_t* udp) {
    // if (udp->server_addr == NULL) {
    //     fprintf(stderr, "udp_send: no send address set");
    //     return -1;
    // }

    int sockfd = udp->sockfd;
    char *msg = udp->msg_send;
    size_t msg_size = udp->bytes_to_send;

    int flags = 0;
    struct sockaddr *addr = (struct sockaddr*)&udp->server_addr;
    socklen_t addr_size = udp->server_addr_size;

	ssize_t bytes_sent = sendto(sockfd, msg, msg_size, flags, addr, addr_size);
    udp->bytes_sent = bytes_sent;

    if (bytes_sent == -1) {
        perror("udp_send");
		return -1;
    }

	return 0;
}

int udp_recv(udp_t* udp) {
    int sockfd = udp->sockfd;
    char *buffer = udp->msg_recv;
    size_t buffer_size = udp->msg_recv_size;
    int flags = 0;

    struct sockaddr *addr = (struct sockaddr*)&udp->client_addr;
    socklen_t *addr_size = &udp->client_addr_size;

	ssize_t bytes_recv = recvfrom(sockfd, buffer, buffer_size, flags, addr, addr_size);
    
    udp->bytes_recv = bytes_recv;

	if (bytes_recv == -1) {
		// perror("udp_recv");
		return -1;
	}

	return 0;
}

int udp_delete(udp_t* udp) {
	if (udp == NULL) {
        return -1;
    }

    free(udp->msg_recv);
    free(udp->msg_send);

    close(udp->sockfd);
    free(udp);
    return 0;
}
