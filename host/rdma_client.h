#ifndef RDMA_CLIENT_H
#define RDMA_CLIENT_H

#include <poll.h>
#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <inttypes.h>
#include "rdma_common.h"
#include "hash_delee.h"


/* These are basic RDMA resources */
/* These are RDMA connection related resources */
struct client_r_info{
	/* These are RDMA connection related resources */
	struct rdma_event_channel *cm_event_channel;
	struct rdma_cm_id *cm_client_id;
	struct ibv_pd *pd;
	struct ibv_comp_channel *io_completion_channel;
	struct ibv_cq *client_cq;
	struct ibv_qp *client_qp;

	struct rdma_cm_event *cm_event;
	struct ibv_mr *client_metadata_mr, *server_metadata_mr;
	struct ibv_mr *req_mr, *rtn_mr;

	struct rdma_buffer_attr client_metadata_attr, server_metadata_attr;

	struct req_msg req;	//src
	struct rtn_msg rtn;	//dst

	struct ibv_send_wr client_send_wr, *bad_client_send_wr;
	struct ibv_recv_wr server_recv_wr, *bad_server_recv_wr;

	struct ibv_sge client_send_sge, server_recv_sge;

	int sockfd;
	int eventfd;	/* event_fd */
	struct ibv_mr *eventfd_mr;
};

extern struct sockaddr_in *server_sockaddr;

/* This function prepares client side connection resources for an RDMA connection */
int client_prepare_connection(struct sockaddr_in *s_addr, struct client_r_info* r_info);
/* Pre-posts a receive buffer before calling rdma_connect () */
int client_pre_post_recv_buffer(struct client_r_info* r_info);
/* Connects to the RDMA server */
int client_connect_to_server(struct client_r_info* r_info);
/* Exchange buffer metadata with the server. The client sends its, and then receives
 * from the server. The client-side metadata on the server is _not_ used because
 * this program is client driven. But it shown here how to do it for the illustration
 * purposes
 */
int client_xchange_metadata_with_server(struct client_r_info* r_info);
/* This function does :
 * 1) Prepare memory buffers for RDMA operations 
 * 1) RDMA write from src -> remote buffer 
 * 2) RDMA read from remote bufer -> dst
 */ 
int client_remote_memory_ops(struct client_r_info* r_info);
/* This function disconnects the RDMA connection from the server and cleans up 
 * all the resources.
 */
int client_disconnect_and_clean(struct client_r_info* r_info);
int client_recv_disconnect_and_clean(struct client_r_info* r_info);
int null_client_r_info(struct client_r_info* r_info);
void set_sockaddr();
int post_write_req(struct client_r_info* r_info);
int post_recv_rtn(struct client_r_info* r_info);
int connect_server(struct client_r_info *r_info);
int set_req_sock(struct client_r_info *r_info, int sockfd, int req_rdma, uint64_t buf_sz, uint16_t port, const char *message, int *p_errno, void *buf);
	// int flags, int epfd, int op, int fd, struct epoll_event *event, int maxevents, int timeout);
int process_rdma_get_cq_event(struct client_r_info* r_info);
int process_poll_cq_imm (struct client_r_info* r_info);

#endif // RDMA_CLIENT_H
