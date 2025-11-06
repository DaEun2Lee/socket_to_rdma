/*
 * This is a RDMA server side code.
 *
 * TODO: Cleanup previously allocated resources in case of an error condition
 */
#ifndef RDMA_SERVER_H
#define RDMA_SERVER_H

#include <pthread.h>
#include "rdma_common.h"
#include "socket.h"
#include "hash_delee.h"

/* These are the RDMA resources needed to setup an RDMA connection */
/* Event channel, where connection management (cm) related events are relayed */

struct server_r_info{
	struct sockaddr_in server_sockaddr;

	struct rdma_event_channel *cm_event_channel;
	struct rdma_cm_id *cm_server_id;

	/* by delee */
	struct rdma_cm_event *cm_event;

	/* read-write lock */
	pthread_rwlock_t rwlock;
};

struct client_r_info{
	struct rdma_cm_id *cm_client_id;
	struct ibv_pd *pd;
	// struct ibv_comp_channel **io_completion_channel;
	struct ibv_comp_channel *io_completion_channel;
	struct ibv_cq *cq;

//	struct ibv_qp_init_attr qp_init_attr;
	struct ibv_qp *client_qp;

	/* RDMA memory resources */
//	struct ibv_mr *client_metadata_mr, *server_buffer_mr, *server_metadata_mr;
	struct ibv_mr *client_metadata_mr, *server_metadata_mr;
	struct ibv_mr *req_mr, *rtn_mr;

	struct rdma_buffer_attr client_metadata_attr, server_metadata_attr;
	struct req_msg req;	
	struct rtn_msg rtn;

	struct ibv_recv_wr client_recv_wr, *bad_client_recv_wr;
	struct ibv_send_wr server_send_wr, *bad_server_send_wr;
	struct ibv_sge client_recv_sge, server_send_sge;

	int status;
	int sockfd;
	int t_id;
};


// in rdma_server.c
extern struct server_r_info* s_info;
extern struct client_r_info* global_c_info;
extern struct ibv_context *global_context;

// in rdma_sock_linker.c
extern struct HashTable* rdma_ht;
extern struct SocketHashTable* sock_ht;


int null_server_r_info(struct server_r_info* s_info);
int null_client_r_info(struct client_r_info* c_info);

/* When we call this function cm_client_id must be set to a valid identifier.
 * This is where, we prepare client connection before we accept it. This
 * mainly involve pre-posting a receive buffer to receive client side
 * RDMA credentials
 */

int set_server_r_info(struct sockaddr_in *server_sockaddr, int port);
/* Starts an RDMA server by allocating basic connection resources */
int start_rdma_server(struct server_r_info* s_info, struct sockaddr_in *server_addr);
int waiting_client_conn(struct server_r_info* s_info, struct client_r_info* c_info);
int setup_client_resources(struct client_r_info* c_info);
int make_io_completion_channel(struct ibv_comp_channel **io_completion_channel);
// int make_io_completion_channel(struct ibv_comp_channel *io_completion_channel);
int alloc_pd(struct client_r_info* c_info);
int make_cq(struct client_r_info* c_info);
int make_cq_for_handler(struct ibv_comp_channel *io_completion_channel, struct ibv_pd **pd, struct ibv_cq **cq);
int make_qp(struct client_r_info* c_info);
/* Pre-posts a receive buffer and accepts an RDMA client connection */
int accept_client_connection(struct server_r_info *s_info, struct client_r_info *c_info);
/* This function sends server side buffer metadata to the connected client */
int send_server_metadata_to_client(struct client_r_info* c_info);
/* This is server side logic. Server passively waits for the client to call
* rdma_disconnect() and then it will clean up its resources */
int disconnect_and_cleanup(struct server_r_info* s_info, struct client_r_info* c_info);
int destroy_server_id(struct server_r_info *s_info);

int post_write_rtn (struct client_r_info* c_info);
int post_recv_req(struct client_r_info *c_info);
int write_msg_to_client(struct client_r_info* c_info);
int process_poll_cq_imm (struct client_r_info* c_info);
int process_rdma_get_cq_event(struct client_r_info* c_info);
int connect_client_r_info(struct server_r_info* s_info, struct client_r_info* c_info);
void start_thread();
void rdma_req_event(struct client_r_info* c_info);
int register_rtn_buffer(struct client_r_info *c_info);

int process_rdma_cm_event_with_global(struct rdma_event_channel *echannel, struct rdma_cm_event **cm_event);
int global_connect_client_r_info(struct server_r_info* s_info, struct client_r_info* c_info);
int global_waiting_client_conn(struct server_r_info* s_info, struct client_r_info* c_info);
int global_accept_client_connection(struct server_r_info *s_info, struct client_r_info *c_info);
int global_disconnect_and_cleanup(struct server_r_info* s_info, struct client_r_info* c_info);
int server_disconnect_and_cleanup(struct server_r_info* s_info, struct client_r_info* c_info);

#endif /* RDMA_SERVER_H */
