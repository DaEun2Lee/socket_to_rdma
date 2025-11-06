#ifndef SOCKET_H_
#define SOCKET_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdbool.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>
#include "rdma_common.h"

#define SO_IP "10.0.10.1"                //Dest IP
#define SO_SND_PORT 3300                //sock-> wanproxy
#define SO_RCV_PORT 8080                //sock -> rdma
#define SO_BUFFER_SIZE 1048576		//8192		//8*1024

struct socket_info {
	int server_fd;
	struct sockaddr_in address;
	int opt;			//1
	int addrlen;			//addrlen = sizeof(address)
	char buffer[SO_BUFFER_SIZE];	//= {0}
};

void *server_thread();
void *client_thread();
void *socket_info();

void print_socket_info_info(struct socket_info *c_info);

int sock_select(struct req_msg *req_msg_, struct rtn_msg *rtn_msg);
int sock_epoll_create1(struct req_msg *req_msg_, struct rtn_msg *rtn_msg_);
int sock_epoll_ctl(struct req_msg *req_msg_, struct rtn_msg *rtn_msg_);
int sock_epoll_wait(struct req_msg *req_msg_, struct rtn_msg *rtn_msg_);
int sock_poll(struct req_msg *req_msg_, struct rtn_msg *rtn_msg_);

int sock_fcntl (struct req_msg *req_msg_, struct rtn_msg *rtn_msg_, int sockfd);
int sock_setsockopt (struct req_msg *req_msg_, struct rtn_msg *rtn_msg_, int sockfd);
int sock_getsockopt (struct req_msg *req_msg_, struct rtn_msg *rtn_msg_, int sockfd);
int sock_getpeername(struct req_msg *req_msg_, struct rtn_msg *rtn_msg_, int sockfd);
int sock_getsockname(struct req_msg *req_msg_, struct rtn_msg *rtn_msg_, int sockfd);


int sock_socket(struct rtn_msg *rtn_msg_);
bool sock_bind(struct rtn_msg *rtn_msg_, int sockfd, uint16_t port);
bool sock_listen(struct rtn_msg *rtn_msg_, int sockfd);
int sock_accept(struct rtn_msg *rtn_msg_, int sockfd, uint16_t port);
void sock_close(struct rtn_msg *rtn_msg_, int sockfd);
bool sock_connect(struct rtn_msg *rtn_msg_, int sockfd, uint16_t port);
void sock_send_message(struct rtn_msg *rtn_msg_, int sockfd, const char *data, int data_sz);
int sock_read_message(struct rtn_msg *rtn_msg_, int sockfd, int buf_sz);	


#endif // SOCKET_H_

