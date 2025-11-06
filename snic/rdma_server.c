#include "config.h"
#include "rdma_server.h"

struct server_r_info *s_info;
struct client_r_info *ctrl_c_info, *global_c_info;
struct ibv_context *global_context;

/* In rdma_sock_linker.c */ 
struct HashTable* rdma_ht;
struct SocketHashTable* sock_ht;

int null_server_r_info(struct server_r_info* s_info)
{
	// pthread_rwlock_init(&s_info->rwlock, NULL);

	s_info->cm_event_channel = NULL;
	s_info->cm_server_id = NULL;
	s_info->cm_event = NULL;
	return 1;
}

int null_client_r_info(struct client_r_info* c_info)
{
	// pthread_rwlock_init(&c_info->rwlock, NULL);

	c_info->cm_client_id = NULL;
	c_info->pd = NULL;
	c_info->io_completion_channel = NULL;
	c_info->cq = NULL;

	c_info->client_qp = NULL;

	c_info->req_mr = NULL;
	c_info->rtn_mr = NULL;
	c_info->bad_client_recv_wr = NULL;
	c_info->bad_server_send_wr = NULL;

	return 1;
}

int set_server_r_info(struct sockaddr_in *server_sockaddr, int port)
{
	int ret;

	memset(server_sockaddr, 0, sizeof(struct sockaddr_in));
	server_sockaddr->sin_family = AF_INET; /* standard IP NET address */
	server_sockaddr->sin_addr.s_addr = htonl(INADDR_ANY); /* passed address */

	ret = get_addr(DEFAULT_RDMA_SERVER_IP, (struct sockaddr*) server_sockaddr);
	if (ret) {
		errorInfoMes("Invalid IP \n");
		return ret;
	}

	if(port > 0){
		server_sockaddr->sin_port = htons(port); /* use port */
	} else {
		/* If still zero, that mean no port info provided */
		server_sockaddr->sin_port = htons(DEFAULT_RDMA_PORT); /* use default port */
	}
	return ret;
}

int start_rdma_server(struct server_r_info *s_info, struct sockaddr_in *server_addr)
{
	// pthread_rwlock_wrlock(&s_info->rwlock);

	int ret = -1;

	s_info->cm_event_channel = rdma_create_event_channel();
	if (!s_info->cm_event_channel) {
		errorInfoMes("Creating cm event channel failed with errno : (%d)", -errno);
		// pthread_rwlock_unlock(&s_info->rwlock);
		return -errno;
	}
	debugInfoMes("RDMA CM event channel is created successfully at %p \n", s_info->cm_event_channel);

	ret = rdma_create_id(s_info->cm_event_channel, &s_info->cm_server_id, NULL, RDMA_PS_TCP);
	// ret = rdma_create_id(s_info->cm_event_channel, &s_info->cm_server_id, s_info, RDMA_PS_IB);
	if (ret) {
		errorInfoMes("Creating server cm id failed with errno: %d ", -errno);
		// pthread_rwlock_unlock(&s_info->rwlock);
		return -errno;
	}
	debugInfoMes("A RDMA connection id for the server is created \n");

	ret = rdma_bind_addr(s_info->cm_server_id, (struct sockaddr*) server_addr);
	if (ret) {
		errorInfoMes("Failed to bind server address, errno: %d \n", -errno);
		// pthread_rwlock_unlock(&s_info->rwlock);
		return -errno;
	}
	debugInfoMes("Server RDMA CM id is successfully binded \n");

	ret = rdma_listen(s_info->cm_server_id, 8); /* backlog = 8 clients, same as TCP, see man listen*/
	if (ret) {
		errorInfoMes("rdma_listen failed to listen on server address, errno: %d ", -errno);
		// pthread_rwlock_unlock(&s_info->rwlock);
		return -errno;
	}
	debugInfoMes("Server is listening successfully at: %s , port: %d \n",
			inet_ntoa(server_addr->sin_addr),
			ntohs(server_addr->sin_port));

	// pthread_rwlock_unlock(&s_info->rwlock);
	return ret;
}

int waiting_client_conn(struct server_r_info* s_info, struct client_r_info* c_info)
{
	// pthread_rwlock_wrlock(&s_info->rwlock);
	int ret = -1;
	ret = process_rdma_cm_event(s_info->cm_event_channel,
			RDMA_CM_EVENT_CONNECT_REQUEST,
			&s_info->cm_event);

	c_info->status = RDMA_CM_EVENT_CONNECT_REQUEST;
	c_info->cm_client_id = s_info->cm_event->id;
	//@delee - ADD
	c_info->cm_client_id->context = (void *)c_info;

	ret = rdma_ack_cm_event(s_info->cm_event);
	if (ret) {
		errorInfoMes("Failed to acknowledge the cm event errno: %d \n", -errno);
		// pthread_rwlock_unlock(&s_info->rwlock);
		return -errno;
	}
	debugInfoMes("A new RDMA client connection id is stored at %p\n", c_info->cm_client_id);

	// pthread_rwlock_unlock(&s_info->rwlock);
	return ret;
}

int setup_client_resources(struct client_r_info* c_info)
{
	int ret = -1;

	if(!c_info->cm_client_id){
		errorInfoMes("Client id is still NULL \n");
		return -EINVAL;
	}

	c_info->pd = ibv_alloc_pd(c_info->cm_client_id->verbs);
	if (!c_info->pd) {
		errorInfoMes("Failed to allocate a protection domain errno: %d\n", -errno);
		return -errno;
	}
	debugInfoMes("A new protection domain is allocated at %p \n", c_info->pd);

	c_info->io_completion_channel = ibv_create_comp_channel(c_info->cm_client_id->verbs);
	if (!c_info->io_completion_channel) {
		errorInfoMes("Failed to create an I/O completion event channel, %d\n", -errno);
		return -errno;
	}
	debugInfoMes("An I/O completion event channel is created at %p \n", c_info->io_completion_channel);

	c_info->cq = ibv_create_cq(c_info->cm_client_id->verbs, /* which device*/
				CQ_CAPACITY, /* maximum capacity*/
				NULL, /* user context, not used here */
				c_info->io_completion_channel, /* which IO completion channel */
				0 /* signaling vector, not used here*/
				);
	if (!c_info->cq) {
		errorInfoMes("Failed to create a completion queue (cq), errno: %d\n", -errno);
		return -errno;
	}
	debugInfoMes("Completion queue (CQ) is created at %p with %d elements \n", c_info->cq, c_info->cq->cqe);

	ret = ibv_req_notify_cq(c_info->cq, /* on which CQ */
			0 /* 0 = all event type, no filter*/
			);
	if (ret) {
		errorInfoMes("Failed to request notifications on CQ errno: %d \n", -errno);
		return -errno;
	}

	struct ibv_qp_init_attr qp_init_attr;
	memset(&qp_init_attr, 0, sizeof qp_init_attr);
	qp_init_attr.cap.max_recv_sge = MAX_SGE; /* Maximum SGE per receive posting */
	qp_init_attr.cap.max_recv_wr = MAX_WR; /* Maximum receive posting capacity */
	qp_init_attr.cap.max_send_sge = MAX_SGE; /* Maximum SGE per send posting */
	qp_init_attr.cap.max_send_wr = MAX_WR; /* Maximum send posting capacity */
	qp_init_attr.qp_type = IBV_QPT_RC; /* QP type, RC = Reliable connection */
	/* We use same completion queue, but one can use different queues */
	qp_init_attr.recv_cq = c_info->cq; /* Where should I notify for receive completion operations */
	qp_init_attr.send_cq = c_info->cq; /* Where should I notify for send completion operations */
	/*Lets create a QP */
	ret = rdma_create_qp(c_info->cm_client_id /* which connection id */,
						c_info->pd /* which protection domain*/,
						&qp_init_attr /* Initial attributes */);
	if (ret) {
		errorInfoMes("Failed to create QP due to errno: %d\n", -errno);
		return -errno;
	}

	/* Save the reference for handy typing but is not required */
	c_info->client_qp = c_info->cm_client_id->qp;
	debugInfoMes("Client QP created at %p\n", c_info->client_qp);
	return ret;
}

int make_io_completion_channel(struct ibv_comp_channel **io_completion_channel)
{
	int ret = -1;
	*io_completion_channel = ibv_create_comp_channel(global_context);
	if (!(*io_completion_channel)) {
		errorInfoMes("Failed to create an I/O completion event channel, %d\n", -errno);
		return -errno;
	}
	debugInfoMes("An I/O completion event channel is created at %p \n",	*io_completion_channel);

	return 0;
}

int alloc_pd(struct client_r_info* c_info)
{
	int ret = -1;

	c_info->pd = ibv_alloc_pd(c_info->cm_client_id->verbs);
	if (!c_info->pd) {
		errorInfoMes("Failed to allocate a protection domain errno: %d\n", -errno);
		return -errno;
	}
}

int make_cq(struct client_r_info* c_info)
{
	int ret;

	if(!c_info->cm_client_id){
		errorInfoMes("Client id is still NULL \n");
		return -EINVAL;
	}

	if (!c_info->pd) {
		errorInfoMes("Failed to allocate a protection domain errno: %d\n", -errno);
		return -errno;
	}
	debugInfoMes("A new protection domain is allocated at %p \n", c_info->pd);

	if (!c_info->cq) {
		errorInfoMes("Failed to create a completion queue (cq), errno: %d\n", -errno);
		return -errno;
	}
	debugInfoMes("Completion queue (CQ) is created at %p with %d elements \n", c_info->cq, c_info->cq->cqe);

	ret = ibv_req_notify_cq(c_info->cq, /* on which CQ */
		0 /* 0 = all event type, no filter*/
		);
	if (ret) {
		errorInfoMes("Failed to request notifications on CQ errno: %d \n", -errno);
		return -errno;
	}

	struct ibv_qp_init_attr qp_init_attr;
	memset(&qp_init_attr, 0, sizeof qp_init_attr);
	qp_init_attr.cap.max_recv_sge = MAX_SGE; /* Maximum SGE per receive posting */
	qp_init_attr.cap.max_recv_wr = MAX_WR; /* Maximum receive posting capacity */
	qp_init_attr.cap.max_send_sge = MAX_SGE; /* Maximum SGE per send posting */
	qp_init_attr.cap.max_send_wr = MAX_WR; /* Maximum send posting capacity */
	qp_init_attr.qp_type = IBV_QPT_RC; /* QP type, RC = Reliable connection */
	/* We use same completion queue, but one can use different queues */
	qp_init_attr.recv_cq = c_info->cq; /* Where should I notify for receive completion operations */
	qp_init_attr.send_cq = c_info->cq; /* Where should I notify for send completion operations */
	/*Lets create a QP */
	ret = rdma_create_qp(c_info->cm_client_id /* which connection id */,
					c_info->pd /* which protection domain*/,
					&qp_init_attr /* Initial attributes */);
	if (ret) {
		errorInfoMes("Failed to create QP due to errno: %d\n", -errno);
		perror("ibv_create_qp");
		return -errno;
	}

	/* Save the reference for handy typing but is not required */
	c_info->client_qp = c_info->cm_client_id->qp;
	debugInfoMes("Client QP created at %p\n", c_info->client_qp);
	return ret;
}
int make_cq_for_handler(struct ibv_comp_channel *io_completion_channel, struct ibv_pd **pd, struct ibv_cq **cq)
{
	int ret;

	*pd = ibv_alloc_pd(global_context);

	if (!(*pd)) {
		errorInfoMes("Failed to allocate a protection domain errno: %d\n", -errno);
		return -errno;
	}
	debugInfoMes("A new protection domain is allocated at %p \n", *pd);


	*cq = ibv_create_cq(global_context, /* which device*/
		CQ_CAPACITY, /* maximum capacity*/
		NULL , /* user context, not used here */
		io_completion_channel, /* which IO completion channel */
		0 /* signaling vector, not used here*/
		);

	if (!*cq) {
		errorInfoMes("Failed to create a completion queue (cq), errno: %d\n", -errno);
		return -errno;
	}
	debugInfoMes("Completion queue (CQ) is created at %p with %d elements \n", *cq, (*cq)->cqe);

	ret = ibv_req_notify_cq(*cq, /* on which CQ */
		0 /* 0 = all event type, no filter*/
		);
	if (ret) {
		errorInfoMes("Failed to request notifications on CQ errno: %d \n", -errno);
		return -errno;
	}
	return 0;
}
int make_qp(struct client_r_info* c_info)
{
	int ret;
	if(!c_info->cm_client_id){
		errorInfoMes("Client id is still NULL \n");
		return -EINVAL;
	}

	struct ibv_qp_init_attr qp_init_attr;
	memset(&qp_init_attr, 0, sizeof qp_init_attr);
	qp_init_attr.cap.max_recv_sge = MAX_SGE; /* Maximum SGE per receive posting */
	qp_init_attr.cap.max_recv_wr = MAX_WR; /* Maximum receive posting capacity */
	qp_init_attr.cap.max_send_sge = MAX_SGE; /* Maximum SGE per send posting */
	qp_init_attr.cap.max_send_wr = MAX_WR; /* Maximum send posting capacity */
	qp_init_attr.qp_type = IBV_QPT_RC; /* QP type, RC = Reliable connection */
	qp_init_attr.qp_context = c_info;
	/* We use same completion queue, but one can use different queues */
	qp_init_attr.recv_cq = c_info->cq; /* Where should I notify for receive completion operations */
	qp_init_attr.send_cq = c_info->cq; /* Where should I notify for send completion operations */
	/*Lets create a QP */
	ret = rdma_create_qp(c_info->cm_client_id /* which connection id */,
					c_info->pd /* which protection domain*/,
					&qp_init_attr /* Initial attributes */);
	if (ret) {
		errorInfoMes("Failed to create QP due to errno: %d\n", -errno);
		perror("ibv_create_qp");
		return -errno;
	}

	// add_num_conn(c_info->rdma_thread);

	/* Save the reference for handy typing but is not required */
	c_info->client_qp = c_info->cm_client_id->qp;
	debugInfoMes("Client QP created at %p\n", c_info->client_qp);
	return ret;
}


int accept_client_connection(struct server_r_info *s_info, struct client_r_info *c_info)
{
	struct rdma_conn_param conn_param;
	struct sockaddr_in remote_sockaddr;
	int ret = -1;

	if(!c_info->cm_client_id || !c_info->client_qp) {
		errorInfoMes("Client resources are not properly setup\n");
		return -EINVAL;
	}

	/* Set rtn_msg_mr */
	c_info->client_metadata_mr = rdma_buffer_register(
									c_info->pd,								/* which protection domain */
									&c_info->client_metadata_attr,			/* what memory */
									sizeof(c_info->client_metadata_attr),	/* what length */
									(IBV_ACCESS_LOCAL_WRITE |
									IBV_ACCESS_REMOTE_WRITE |
									IBV_ACCESS_REMOTE_READ) 				/* access permissions */
									);
	if(!c_info->client_metadata_mr){
		errorInfoMes("Failed to register client attr buffer\n");
		return -ENOMEM;
	}

	//post_recv of metadata
	c_info->client_recv_sge.addr = (uint64_t) c_info->client_metadata_mr->addr;	// same as &client_buffer_attr
	c_info->client_recv_sge.length = c_info->client_metadata_mr->length;
	c_info->client_recv_sge.lkey = c_info->client_metadata_mr->lkey;

	/* Now we link this SGE to the work request (WR) */
	memset(&c_info->client_recv_wr, 0, sizeof(c_info->client_recv_wr));
	c_info->client_recv_wr.sg_list = &c_info->client_recv_sge;
	c_info->client_recv_wr.num_sge = 1; // only one SGE

	ret = ibv_post_recv(c_info->client_qp,				/* which QP */
						&c_info->client_recv_wr,		/* receive work request*/
						&c_info->bad_client_recv_wr);	/* error WRs */

	if (ret) {
		errorInfoMes("Failed to pre-post the receive buffer, errno: %d \n", ret);
		return -ret;
	}
	debugInfoMes("Receive buffer pre-posting is successful \n");
	
	
	memset(&conn_param, 0, sizeof(conn_param));
	conn_param.initiator_depth = 3;
	conn_param.responder_resources = 3;
	ret = rdma_accept(c_info->cm_client_id, &conn_param);
	if (ret) {
		errorInfoMes("Failed to accept the connection, errno: %d \n", -errno);
		return -errno;
	}
	debugInfoMes("Going to wait for : RDMA_CM_EVENT_ESTABLISHED event \n");

	// pthread_rwlock_wrlock(&s_info->rwlock);
	ret = process_rdma_cm_event(s_info->cm_event_channel, RDMA_CM_EVENT_ESTABLISHED, &s_info->cm_event);
	if (c_info->status == RDMA_CM_EVENT_ESTABLISHED) {
		errorInfoMes("Failed to get the cm event, errnp: %d \n", -errno);
		return -errno;
 	}
	c_info->status = RDMA_CM_EVENT_ESTABLISHED;
	debugInfoMes("STATUS is RDMA_CM_EVENT_ESTABLISHED\n");

	ret = rdma_ack_cm_event(s_info->cm_event);
	// pthread_rwlock_unlock(&s_info->rwlock);
	if (ret) {
		errorInfoMes("Failed to acknowledge the cm event %d\n", -errno);
		return -errno;
	}

	while(c_info->status != RDMA_CM_EVENT_ESTABLISHED){
		usleep(500);
	}
	debugInfoMes("STATUS is RDMA_CM_EVENT_ESTABLISHED\n");

	struct ibv_qp_attr attr;
	struct ibv_qp_init_attr init_attr;
	ibv_query_qp(c_info->client_qp, &attr, IBV_QP_STATE, &init_attr);
	debugInfoMes("QP state after rdma_accept: %d\n", attr.qp_state); // IBV_QPS_RTS = 4

	/* Just FYI: How to extract connection information */
	memcpy(&remote_sockaddr /* where to save */,
			rdma_get_peer_addr(c_info->cm_client_id) /* gives you remote sockaddr */,
			sizeof(struct sockaddr_in) /* max size */
			);
	debugInfoMes("A new connection is accepted from %s \n", inet_ntoa(remote_sockaddr.sin_addr));

	struct ibv_wc wc;
	ret = process_work_completion_events(c_info->io_completion_channel, &wc, 1);
	if (ret != 1) {
		errorInfoMes("Failed to send server metadata, ret = %d \n", ret);
		return ret;
	}
	/* We got client_metadata*/

	c_info->rtn_mr = rdma_buffer_register(
		c_info->pd,						/* which protection domain */
		&c_info->rtn,
		sizeof(c_info->rtn),	/* what size to allocate */
		(IBV_ACCESS_LOCAL_WRITE|
		IBV_ACCESS_REMOTE_READ|
		IBV_ACCESS_REMOTE_WRITE));		/* access permissions */

	return 1;
}

int send_server_metadata_to_client(struct client_r_info* c_info)
{
	struct ibv_wc wc;
	int ret = -1;
	// pthread_rwlock_wrlock(&c_info->rwlock);

	if(!c_info->cm_client_id || !c_info->client_qp) {
		errorInfoMes("Client resources are not properly setup\n");
		return -EINVAL;
	}

	c_info->req_mr = rdma_buffer_register(
		c_info->pd,					/* which protection domain */
		&c_info->req,
		sizeof(c_info->req),		/* what size to allocate */
		(IBV_ACCESS_LOCAL_WRITE|
		IBV_ACCESS_REMOTE_READ|
		IBV_ACCESS_REMOTE_WRITE));	/* access permissions */

	if(!c_info->req_mr){
		errorInfoMes("Server failed to create a buffer \n");
		/* we assume that it is due to out of memory error */
		// pthread_rwlock_unlock(&c_info->rwlock);
		return -ENOMEM;
	}

	c_info->server_metadata_mr = rdma_buffer_register(
		c_info->pd, /* which protection domain*/
		&c_info->server_metadata_attr, /* which memory to register */
		sizeof(c_info->server_metadata_attr), /* what is the size of memory */
		(IBV_ACCESS_LOCAL_WRITE|
		IBV_ACCESS_REMOTE_READ|
		IBV_ACCESS_REMOTE_WRITE) /* access permissions */
		);

	/* Fille server_metadata */
	c_info->server_metadata_attr.address = (uint64_t)(uintptr_t) c_info->req_mr->addr;
	c_info->server_metadata_attr.length = (uint32_t) c_info->req_mr->length;
	c_info->server_metadata_attr.stag.local_stag = c_info->req_mr->lkey;
	c_info->server_metadata_attr.stag.remote_stag = c_info->req_mr->rkey;

	c_info->server_send_sge.addr = (uint64_t) c_info->server_metadata_mr->addr;
	c_info->server_send_sge.length = c_info->server_metadata_mr->length;
	c_info->server_send_sge.lkey = c_info->server_metadata_mr->lkey;

	memset(&c_info->server_send_wr, 0, sizeof(c_info->server_send_wr));
	c_info->server_send_wr.sg_list = &c_info->server_send_sge; //@delee
	c_info->server_send_wr.num_sge = 1; // only 1 SGE element in the array
	c_info->server_send_wr.opcode = IBV_WR_SEND; // This is a send request
	c_info->server_send_wr.send_flags = IBV_SEND_SIGNALED; // We want to get notification

	ret = ibv_post_send(c_info->client_qp /* which QP */,
						&c_info->server_send_wr /* Send request that we prepared before */,
						&c_info->bad_server_send_wr /* In case of error, this will contain failed requests */);
	if (ret) {
		errorInfoMes("Posting of server metdata failed, errno: %d \n", -errno);
		// pthread_rwlock_unlock(&c_info->rwlock);
		return -errno;
	}

	// ret = process_work_completion_events(c_info->io_completion_channel, &wc, 1);
	// if (ret != 1) {
	// 	errorInfoMes("Failed to send server metadata, ret = %d \n", ret);
	// 	return ret;
	// }

	debugInfoMes("Local buffer metadata has been sent to the client \n");
	debugInfoMes("c_info->server_metadata_attr\n");
	show_rdma_buffer_attr(&c_info->server_metadata_attr);
	debugInfoMes("c_info->client_metadata_attr\n");
	show_rdma_buffer_attr(&c_info->client_metadata_attr);
	// pthread_rwlock_unlock(&c_info->rwlock);
	return 0;
}

int disconnect_and_cleanup(struct server_r_info* s_info, struct client_r_info* c_info)
{
//	struct rdma_cm_event *cm_event = NULL;
	int ret = -1;
	/* Now we wait for the client to send us disconnect event */
	debugInfoMes("Waiting for cm event: RDMA_CM_EVENT_DISCONNECTED\n");

	// pthread_rwlock_wrlock(&s_info->rwlock);
	ret = process_rdma_cm_event(s_info->cm_event_channel,
			RDMA_CM_EVENT_DISCONNECTED,
			&s_info->cm_event
			);
	if (ret) {
		errorInfoMes("Failed to get disconnect event, ret = %d \n", ret);
		// pthread_rwlock_unlock(&s_info->rwlock);
		// pthread_rwlock_unlock(&c_info->rwlock);
 		return ret;
	}
	// pthread_rwlock_wrlock(&c_info->rwlock);
	c_info->status = RDMA_CM_EVENT_DISCONNECTED;
	/* We acknowledge the event */
	ret = rdma_ack_cm_event(s_info->cm_event);
	// pthread_rwlock_unlock(&s_info->rwlock);
	if (ret) {
		errorInfoMes("Failed to acknowledge the cm event %d\n", -errno);
		// pthread_rwlock_unlock(&c_info->rwlock);
		return -errno;
	}

	while(c_info->status != RDMA_CM_EVENT_DISCONNECTED){
		// usleep(1000);
	}

	debugInfoMes("A disconnect event is received from the client...\n");
	/* We free all the resources */
	/* Destroy QP */
	rdma_destroy_qp(c_info->cm_client_id);
	/* Destroy client cm id */
	ret = rdma_destroy_id(c_info->cm_client_id);
	if (ret) {
		errorInfoMes("Failed to destroy client id cleanly, %d \n", -errno);
		// we continue anyways;
	}
	/* Destroy CQ */
	ret = ibv_destroy_cq(c_info->cq);
	if (ret) {
		errorInfoMes("Failed to destroy completion queue cleanly, %d \n", -errno);
		// we continue anyways;
	}
	/* Destroy completion channel */
	ret = ibv_destroy_comp_channel(c_info->io_completion_channel);
	if (ret) {
		errorInfoMes("Failed to destroy completion channel cleanly, %d \n", -errno);
		// we continue anyways;
	}
	/* Destroy memory buffers */
	rdma_buffer_deregister(c_info->req_mr);
	rdma_buffer_deregister(c_info->rtn_mr);
	/* Destroy protection domain */
	ret = ibv_dealloc_pd(c_info->pd);
	if (ret) {
		errorInfoMes("Failed to destroy client protection domain cleanly, %d \n", -errno);
		// we continue anyways;
	}

	// pthread_rwlock_unlock(&c_info->rwlock);
	// pthread_rwlock_destroy(&c_info->rwlock);

	return 0;
}

int destroy_server_id(struct server_r_info *s_info)
{
	// pthread_rwlock_wrlock(&s_info->rwlock);
	int ret = -1;
	/* Destroy rdma server id */
	ret = rdma_destroy_id(s_info->cm_server_id);
	if (ret) {
		errorInfoMes("Failed to destroy server id cleanly, %d \n", -errno);
		// we continue anyways;
	}

	rdma_destroy_event_channel(s_info->cm_event_channel);

	// pthread_rwlock_unlock(&s_info->rwlock);
	// pthread_rwlock_destroy(&s_info->rwlock);

	debugInfoMes("Server shut-down is complete \n");

	return ret;
}

int post_write_rtn (struct client_r_info* c_info)
{
	c_info->server_send_sge.addr   = (uint64_t) c_info->rtn_mr->addr;
	c_info->server_send_sge.length = c_info->rtn_mr->length;
	c_info->server_send_sge.lkey   = c_info->rtn_mr->lkey;

	memset(&c_info->server_send_wr, 0, sizeof(c_info->server_send_wr));
	c_info->server_send_wr.sg_list = &c_info->server_send_sge;
	c_info->server_send_wr.num_sge = 1;
	c_info->server_send_wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
	c_info->server_send_wr.send_flags = IBV_SEND_SIGNALED;
	c_info->server_send_wr.imm_data = htonl(c_info->sockfd);

	c_info->server_send_wr.wr.rdma.remote_addr = c_info->client_metadata_attr.address;
	c_info->server_send_wr.wr.rdma.rkey = c_info->client_metadata_attr.stag.remote_stag;


	int ret = -1;
	ret = ibv_post_send(c_info->client_qp, &c_info->server_send_wr, &c_info->bad_server_send_wr);

	return ret;
}

int post_recv_req(struct client_r_info *c_info)
{
	int ret;

	c_info->client_recv_sge.addr = (uintptr_t) c_info->req_mr->addr;
	c_info->client_recv_sge.length = c_info->req_mr->length;
	c_info->client_recv_sge.lkey = c_info->req_mr->lkey;

	/* Now we link this SGE to the work request (WR) */
	memset(&c_info->client_recv_wr, 0, sizeof(c_info->client_recv_wr));
	c_info->client_recv_wr.sg_list = &c_info->client_recv_sge;
	c_info->client_recv_wr.num_sge = 1; // only one SGE

	ret = ibv_post_recv(c_info->client_qp /* which QP */,
			&c_info->client_recv_wr /* receive work request*/,
			&c_info->bad_client_recv_wr /* error WRs */
			);
	if (ret) {
		errorInfoMes("Failed to pre-post the receive buffer, errno: %d \n", ret);
		return -ret;
	}
	debugInfoMes("Receive buffer pre-posting is successful \n");

	// struct ibv_wc wc[2];
	// ret = process_work_completion_events(c_info->io_completion_channel, &wc, 1);
	// if (ret != 1) {
	// 	errorInfoMes("Failed to receive client buffer after accept, ret = %d \n", ret);
	// 	return -1;
	// }

	return 1;
}

int write_msg_to_client(struct client_r_info* c_info)
{
	/* We need to transmit this buffer. So we create a send request.
	* A send request consists of multiple SGE elements. In our case, we only
	* have one.
	*/
	int ret;

	if (c_info->req.rdma_req == RDMA_REQ_READ && c_info->rtn.rtn_value > 0) {
		uint64_t offset = 0;
		uint64_t header_sz = 12;
		uint64_t total_size = header_sz + c_info->rtn.rtn_value;
	
		while (offset < total_size) {
			uint64_t bytes_to_copy = (total_size - offset < BUFFER_SIZE)
									? total_size - offset
									: BUFFER_SIZE - header_sz;
			c_info->server_send_sge.length = (bytes_to_copy+1);
			show_rtn_msg(&c_info->rtn);
			ret = post_write_rtn(c_info);
			if (ret != 0) {
				errorInfoMes("RDMA_Send failed at offset %lu, ret = %d", offset, ret);
				return ret;
			}
			offset += bytes_to_copy;
			debugInfoMes("Sent %lu bytes to client, total sent: %lu bytes\n", bytes_to_copy, offset);
		}
		memset(c_info->rtn.data, 0, sizeof(offset)); 
	} else {
		// c_info->server_send_sge.length = 12;
		debugInfoMes("Send rtn_msg:\n");
		show_rtn_msg(&c_info->rtn);
		ret = post_write_rtn(c_info);
		if (ret != 0) {
			errorInfoMes("RDMA_Send failed\n");
			return ret;
		}
	}

	c_info->rtn.errno_ = 0;
	return 0;
}

int process_poll_cq_imm(struct client_r_info* c_info)
{
	struct ibv_wc *wc = malloc(sizeof(struct ibv_wc) * 1);
	memset(wc, 0, sizeof(struct ibv_wc));
	struct ibv_cq *cq_ptr = NULL;
	void *context = NULL;
	int ret = -1;
	//polling
	while(true){
		debugInfoMes("Waiting for CQ event...\n");

		ret = ibv_get_cq_event(c_info->io_completion_channel,	/* IO channel where we are expecting the notification */
								&c_info->cq,		/* which CQ has an activity. This should be the same as CQ we created before */
								&context);		/* Associated CQ user context, which we did set */

		if(ret != 0)
			continue;
		/* Similar to connection management events, we need to acknowledge CQ events */
		ibv_ack_cq_events(c_info->cq,
			1 /* we received one event notification. This is not number of WC elements */);
		/* Request for more notifications. */
		ret = ibv_req_notify_cq(c_info->cq, 0);
		if (ret){
			errorInfoMes("Failed to request further notifications %d \n", -errno);
			return -errno;
		} 
		while ((ret = ibv_poll_cq(c_info->cq, 1, wc)) > 0) {
			if (wc->status != IBV_WC_SUCCESS) {
				errorInfoMes("Work completion (WC) has error status: %s at index %d\n",
				ibv_wc_status_str(wc->status));
				/* return negative value */
				return -(wc->status);
			}
			debugInfoMes("wc->opcode is %d\n", wc->opcode);
			switch (wc->opcode) {
				case IBV_WC_RECV_RDMA_WITH_IMM:
					debugInfoMes("RECV_RDMA_WITH_IMM received\n");

					// for debugginh 
					if(post_recv_req(c_info) < 0){
						errorInfoMes("Fail to post_recv_req\n");
					}
					debugInfoMes("After post_recv_req\n");

					int recv_sockfd = ntohl(wc->imm_data);
					c_info->sockfd = recv_sockfd;
					debugInfoMes("recv_sockfd is %d\n", recv_sockfd);
					debugInfoMes("c_info->sockfd is %d\n", c_info->sockfd);
					rdma_req_event(c_info);
					break;
				case IBV_WC_RECV:
					debugInfoMes("RECV received\n");
					break;
				case IBV_WC_SEND:
					debugInfoMes("SEND complete\n");
					break;
				default:
					break;
				}
			
		}
	}
	free(wc);
	return 1;
}


int process_rdma_get_cq_event(struct client_r_info* c_info)
{
	struct ibv_cq *cq_ptr = NULL;
	void *context = NULL;
	int ret = -1;

	ret = ibv_get_cq_event(c_info->io_completion_channel,	/* IO channel where we are expecting the notification */
		&cq_ptr,		/* which CQ has an activity. This should be the same as CQ we created before */
		&context);		/* Associated CQ user context, which we did set */
	if(ret) {
		errorInfoMes("Failed to get next CQ event due to %d \n", -errno);
		return -errno;
	}

	/* Request for more notifications. */
	ret = ibv_req_notify_cq(cq_ptr, 0);
	if (ret){
		errorInfoMes("Failed to request further notifications %d \n", -errno);
		return -errno;
	}

	/* Similar to connection management events, we need to acknowledge CQ events */
	ibv_ack_cq_events(cq_ptr,
		1 /* we received one event notification. This is not number of WC elements */);
	return 1;
}

int connect_client_r_info(struct server_r_info* s_info, struct client_r_info* c_info)
{
	int ret;
 
	debugInfoMes("Start waiting client\n");
	ret = waiting_client_conn(s_info, c_info);
	if (ret) {
		errorInfoMes("Failed to waiting event from client, ret = %d \n", ret);
		return ret;
	}

	ret = setup_client_resources(c_info);

	if (ret) {
		errorInfoMes("Failed to setup client resources, ret = %d \n", ret);
		return ret;
	}
	debugInfoMes("setup_client_resources\n");

	ret = accept_client_connection(s_info, c_info);
	if (ret < 0) {
		errorInfoMes("Failed to handle client cleanly, ret = %d \n", ret);
		return -ret;
	}
	debugInfoMes("Before send_server_metadata_to_client\n");
	ret = send_server_metadata_to_client(c_info);
	if (ret != 0){
		errorInfoMes("Failed to send server metadata to the client, ret = %d \n", ret);
		return ret;
	}

	if(post_recv_req(c_info) < 0){
		errorInfoMes("\n");
	}
	debugInfoMes("After post_recv_req\n");

	return 0;	// -ret
}

void start_thread()
{
	int ret;
	struct sockaddr_in *server_sockaddr = malloc(sizeof(struct sockaddr_in));

	s_info = malloc(sizeof(struct server_r_info));
	null_server_r_info(s_info);

	/* Init hash table */
	rdma_ht = createHashTable();
	sock_ht = createSocketHashTable();

	/* Set rdma server */
	ret = set_server_r_info(server_sockaddr, -1);
	if(ret) {
		errorInfoMes("Failed to set server sockaddr, ret = %d \n", ret);
		return;
	}

	ret = start_rdma_server(s_info, server_sockaddr);
	if (ret) {
		errorInfoMes("RDMA server failed to start cleanly, ret = %d \n", ret);
		return;
	}

	pid_t tid = syscall(SYS_gettid);
    debugInfoMes("Thread %d is creating a new conenction...\n", tid);

	global_c_info = malloc(sizeof(struct client_r_info));
	null_client_r_info(global_c_info);

	ret = connect_client_r_info(s_info, global_c_info);
	if (ret != 0) {
		errorInfoMes("RDMA server failed to conenct global client cleanly, ret = %d \n", ret);
		return;
	}

	process_poll_cq_imm(global_c_info); 
}

void rdma_req_event(struct client_r_info* c_info) {

	debugInfoMes("sockfd is %d\n", c_info->sockfd);
	show_req_msg(&c_info->req);
	switch (c_info->req.rdma_req) {
		case RDMA_REQ_SOCKET: {
			debugInfoMes("RDMA_REQ_SOCKET\n");

			sock_socket(&c_info->rtn);
			warningInfoMes("sock_socket done\n");
			write_msg_to_client(c_info);

			break;
		}
		case RDMA_REQ_ACCEPT: {
			debugInfoMes("RDMA_REQ_ACCEPT\n");

			sock_accept(&c_info->rtn, c_info->sockfd, c_info->req.port);
			write_msg_to_client(c_info);
			break;
		}
		/* For Client-side */
		case RDMA_REQ_CONNECT: {
			debugInfoMes("RDMA_REQ_CONNECT\n");

			warningInfoMes("Before sock_connect\n");
			sock_connect(&c_info->rtn, c_info->sockfd, c_info->req.port);
			write_msg_to_client(c_info);

			break;
		}
		case RDMA_REQ_CLOSE: {
			debugInfoMes("RDMA_REQ_CLOSE\n");
			debugInfoMes("sockfd is %d\n", c_info->sockfd);

			sock_close(&c_info->rtn, c_info->sockfd);
			write_msg_to_client(c_info);

			break;
		}
		case RDMA_REQ_READ: {
			debugInfoMes("RDMA_REQ_READ\n");

			sock_read_message(&c_info->rtn, c_info->sockfd, c_info->req.buf_sz);
			write_msg_to_client(c_info);
			break;
		}
		case RDMA_REQ_WRITE: {
			debugInfoMes("RDMA_REQ_WRITE\n");

			sock_send_message(&c_info->rtn, c_info->sockfd, c_info->req.data, c_info->req.buf_sz);
			write_msg_to_client(c_info);
			memset(c_info->req.data, 0, sizeof(struct req_msg));
			break;
		}
		case RDMA_REQ_BIND: {
			debugInfoMes("RDMA_REQ_BIND\n");

			sock_bind(&c_info->rtn, c_info->sockfd, c_info->req.port);
			debugInfoMes("Binding port is %d\n", (uint16_t)c_info->req.port);
			write_msg_to_client(c_info);
			break;
		}
		case RDMA_REQ_LISTEN: {
			debugInfoMes("RDMA_REQ_LISTEN\n");

			sock_listen(&c_info->rtn, c_info->sockfd);
			write_msg_to_client(c_info);
			break;
		}
		case RDMA_REQ_SELECT: {
			debugInfoMes("RDMA_REQ_SELECT\n");

			/* select */
			int ret = sock_select(&c_info->req, &c_info->rtn);
			debugInfoMes("sock_select rtn is %d\n", ret);
			warningInfoMes("c_info->req.rdma_req is %d\n", c_info->req.rdma_req);
			write_msg_to_client(c_info);
			memset(c_info->rtn.data, 0, sizeof(struct rtn_msg));
			break;
		}
		case RDMA_REQ_EPOLL_CREATE1: {
			debugInfoMes("RDMA_REQ_EPOLL_CREATE1\n");

			struct socket_info *new_sock_info = socket_info_init(new_sock_info);
			int epfd = sock_epoll_create1(&c_info->req, &c_info->rtn);

			if (epfd < 0 ){
				errorInfoMes("Failed to get sockfd. sockfd is %d\n", epfd);
				write_msg_to_client(c_info);
				return;
			}
			debugInfoMes("epfd is %d\n", epfd);
			c_info->sockfd = epfd;
			write_msg_to_client(c_info);
			socket_insertByValue(sock_ht, epfd, new_sock_info);

			break;
		}
		case RDMA_REQ_EPOLL_CTL: {
			debugInfoMes("RDMA_REQ_EPOLL_CTL\n");
			if(c_info == NULL)
				errorInfoMes("c_info is NULL\n");

			int ret = sock_epoll_ctl(&c_info->req, &c_info->rtn);
			debugInfoMes("epoll_ctl is success\n");
			write_msg_to_client(c_info);
			break;
		}
		case RDMA_REQ_EPOLL_WAIT: {
			debugInfoMes("RDMA_REQ_EPOLL_WAIT\n");
			if(c_info == NULL)
				errorInfoMes("c_info is NULL\n");

			int ret = sock_epoll_wait(&c_info->req, &c_info->rtn);
			debugInfoMes("epoll_wait is success\n");
			write_msg_to_client(c_info);
			break;
		}
		case RDMA_REQ_POLL: {
			debugInfoMes("RDMA_REQ_POLL\n");
			if(c_info == NULL)
				errorInfoMes("c_info is NULL\n");
			warningInfoMes("c_info->req.rdma_req is %d\n", c_info->req.rdma_req);

			int ret = sock_poll(&c_info->req, &c_info->rtn);
			debugInfoMes("poll is success\n");
			write_msg_to_client(c_info);
			break;
		}
		case RDMA_REQ_FCNTL: {
			debugInfoMes("RDMA_REQ_FCNTL\n");
			if(c_info == NULL)
				errorInfoMes("c_info is NULL\n");

			int ret = sock_fcntl(&c_info->req, &c_info->rtn, c_info->sockfd);
			if (ret < 0) {
				errorInfoMes("Failed to get sockfd. sockfd is %d\n", ret);
				write_msg_to_client(c_info);
				break;
			}
			debugInfoMes("fcntl is success\n");
			write_msg_to_client(c_info);
			break;
		}
		case RDMA_REQ_SETSOCKOPT: {
			debugInfoMes("RDMA_REQ_SETSOCKOPT\n");
			if(c_info == NULL)
				errorInfoMes("c_info is NULL\n");

			int ret = sock_setsockopt(&c_info->req, &c_info->rtn, c_info->sockfd);
			if (ret < 0) {
				errorInfoMes("Failed to get sockfd. sockfd is %d\n", ret);
				write_msg_to_client(c_info);
				break;
			}
			debugInfoMes("setsockopt is success\n");
			write_msg_to_client(c_info);
			break;

		}
		case RDMA_REQ_GETSOCKOPT: {
			debugInfoMes("RDMA_REQ_GETSOCKOPT\n");
			if(c_info == NULL)
				errorInfoMes("c_info is NULL\n");

			int ret = sock_getsockopt(&c_info->req, &c_info->rtn, c_info->sockfd);
			if (ret < 0) {
				errorInfoMes("Failed to get sockfd. sockfd is %d\n", ret);
				write_msg_to_client(c_info);
				break;
			}
			debugInfoMes("getsockopt is success\n");
			write_msg_to_client(c_info);
			break;
		}
		case RDMA_REQ_GETPEERNAME: {
			debugInfoMes("RDMA_REQ_GETPEERNAME\n");
			if(c_info == NULL)
				errorInfoMes("c_info is NULL\n");

			int ret = sock_getpeername(&c_info->req, &c_info->rtn, c_info->sockfd);
			if (ret < 0) {
				errorInfoMes("Failed to get sockfd. sockfd is %d\n", ret);
				write_msg_to_client(c_info);
				break;
			}
			debugInfoMes("getpeername is success\n");
			write_msg_to_client(c_info);
			break;
		}
		case RDMA_REQ_GETSOCKNAME: {
			debugInfoMes("RDMA_REQ_GETSOCKNAME\n");
			if(c_info == NULL)
				errorInfoMes("c_info is NULL\n");

			int ret = sock_getsockname(&c_info->req, &c_info->rtn, c_info->sockfd);
			if (ret < 0) {
				errorInfoMes("Failed to get sockfd. sockfd is %d\n", ret);
				write_msg_to_client(c_info);
				break;
			}
			debugInfoMes("getsockname is success\n");
			write_msg_to_client(c_info);
			break;
		}
		case RDMA_REQ_NEW_RDMA_CONN: {
			debugInfoMes("RDMA_REQ_NEW_RDMA_CONN\n");
			if(c_info == NULL)
				errorInfoMes("c_info is NULL\n");

			pid_t tid = syscall(SYS_gettid);
			debugInfoMes("[HOOK] Thread %d is creating a new connection...\n", tid);

			struct client_r_info *new_c_info = malloc(sizeof(struct client_r_info));
			null_client_r_info(new_c_info);

			c_info->rtn.rtn_value = 1;
			write_msg_to_client(c_info);

			debugInfoMes("write_msg_to_client is success\n");

			int ret = connect_client_r_info(s_info, new_c_info);
			if (ret != 0) {
				errorInfoMes("RDMA server failed to conenct global client cleanly, ret = %d \n", ret);
				return;
			}

			pthread_t t_id;
			if (pthread_create(&t_id, NULL, process_poll_cq_imm, new_c_info)){
				errorInfoMes("Failed to create rdma_sock thread");
			}
			pthread_detach(t_id);

			// process_rdma_cm_event_with_global(s_info->cm_event_channel, &s_info->cm_event);

			break;
		}
		case RDMA_REQ_DEL_RDMA_CONN: {
			debugInfoMes("RDMA_REQ_DEL_RDMA_CONN\n");
			if(c_info == NULL)
				errorInfoMes("c_info is NULL\n");

			c_info->rtn.rtn_value = 1;
			write_msg_to_client(c_info);

			global_disconnect_and_cleanup(s_info, c_info);
			deleteByKey(rdma_ht, c_info->sockfd);
			return 0;
			// break;
		}
		default:
			debugInfoMes("Unknown RDMA request: %d\n", c_info->req.rdma_req);
			c_info->rtn.rtn_value = -1;
			write_msg_to_client(c_info);
			break;
	}
}

int process_rdma_cm_event_with_global(struct rdma_event_channel *echannel, struct rdma_cm_event **cm_event)
{
	enum rdma_cm_event_type expected_event = RDMA_CM_EVENT_CONNECT_REQUEST;
	int ret = 1;

	for(;;){
		ret = rdma_get_cm_event(echannel, cm_event);
		if (ret) {
			errorInfoMes("Failed to retrieve a cm event, errno: %d \n",	-errno);
			continue;
		}

		if(0 != (*cm_event)->status){
			errorInfoMes("CM event has non zero status: %d\n", (*cm_event)->status);
			ret = -((*cm_event)->status);
			rdma_ack_cm_event(*cm_event);
			continue;
		}

		if((*cm_event)->event == RDMA_CM_EVENT_CONNECT_REQUEST){
			struct client_r_info* new_c_info;
			new_c_info = malloc(sizeof(struct client_r_info));
			null_client_r_info(new_c_info);

			new_c_info->status = RDMA_CM_EVENT_CONNECT_REQUEST;
			new_c_info->cm_client_id = s_info->cm_event->id;
			//@delee - ADD
			new_c_info->cm_client_id->context = (void *)new_c_info;  

			ret = global_connect_client_r_info(s_info, new_c_info);
			if (ret != 0) {
				errorInfoMes("RDMA server failed to conenct client cleanly, ret = %d \n", ret);
				continue;
			}

			pthread_t tid;
			if (pthread_create(&tid, NULL, process_poll_cq_imm, new_c_info)){
				errorInfoMes("Failed to create rdma_sock thread");
			}
			pthread_detach(tid);
			debugInfoMes("Sucess to create rdma_sock thread");			
		}
		else if((*cm_event)->event == RDMA_CM_EVENT_ESTABLISHED){
			struct client_r_info *c_info  = (struct client_r_info *) (*cm_event)->id->context;
			c_info->status = RDMA_CM_EVENT_ESTABLISHED;
			debugInfoMes("c_info->sockfd is %d, STATUS is RDMA_CM_EVENT_ESTABLISHED\n", c_info->sockfd);
			/* important, we acknowledge the event */
			rdma_ack_cm_event(*cm_event);
			
			return 0;
		}
		else if ((*cm_event)->event == RDMA_CM_EVENT_DISCONNECTED){
			struct client_r_info *c_info  = (struct client_r_info *) (*cm_event)->id->context;
			rdma_ack_cm_event(*cm_event);
			c_info->status = RDMA_CM_EVENT_DISCONNECTED;
			warningInfoMes("c_info->sockfd is %d, STATUS is RDMA_CM_EVENT_DISCONNECTED\n", c_info->sockfd);
			/* important, we acknowledge the event */
		}
		else{
			errorInfoMes("Unexpected event received: %s [ expecting: %s ]\n", 
				rdma_event_str((*cm_event)->event),
				rdma_event_str(expected_event));
			/* important, we acknowledge the event */
			rdma_ack_cm_event(*cm_event);
			// return -1; // unexpected event :(
		}
	}
}

int global_connect_client_r_info(struct server_r_info* s_info, struct client_r_info* c_info)
{
	int ret;

	debugInfoMes("Start waiting client\n");
	ret = global_waiting_client_conn(s_info, c_info);
	if (ret) {
		errorInfoMes("Failed to waiting event from client, ret = %d \n", ret);
		return ret;
	}

	if (ret) {
		errorInfoMes("Failed to setup client resources, ret = %d \n", ret);
		return ret;
	}
	debugInfoMes("setup_client_resources\n");

	ret = global_accept_client_connection(s_info, c_info);
	if (ret < 0) {
		errorInfoMes("Failed to handle client cleanly, ret = %d \n", ret);
		return -ret;
	}
	debugInfoMes("Before send_server_metadata_to_client\n");
	ret = send_server_metadata_to_client(c_info);
	if (ret != 0){
		errorInfoMes("Failed to send server metadata to the client, ret = %d \n", ret);
		return ret;
	}

	if(post_recv_req(c_info) < 0){
		errorInfoMes("\n");
	}

	return 0;	// -ret
}

int global_waiting_client_conn(struct server_r_info* s_info, struct client_r_info* c_info) {
	// pthread_rwlock_wrlock(&s_info->rwlock);
	int ret = -1;
	ret = process_rdma_cm_event(s_info->cm_event_channel,
			RDMA_CM_EVENT_CONNECT_REQUEST,
			&s_info->cm_event);

	ret = process_rdma_cm_event_with_global(s_info->cm_event_channel, &s_info->cm_event);
	if (ret) {
		errorInfoMes("Failed to get cm event, ret = %d \n" , ret);
		// pthread_rwlock_unlock(&s_info->rwlock);
		return ret;
	}
	c_info->status = RDMA_CM_EVENT_CONNECT_REQUEST;
	c_info->cm_client_id = s_info->cm_event->id;
	//@delee - ADD
	c_info->cm_client_id->context = (void *)c_info;

	ret = rdma_ack_cm_event(s_info->cm_event);
	if (ret) {
		errorInfoMes("Failed to acknowledge the cm event errno: %d \n", -errno);
		// pthread_rwlock_unlock(&s_info->rwlock);
		return -errno;
	}
	debugInfoMes("A new RDMA client connection id is stored at %p\n", c_info->cm_client_id);

	// pthread_rwlock_unlock(&s_info->rwlock);
	return ret;
}

int global_accept_client_connection(struct server_r_info *s_info, struct client_r_info *c_info) {
	struct rdma_conn_param conn_param;
	struct sockaddr_in remote_sockaddr;
	int ret = -1;

	if(!c_info->cm_client_id || !c_info->client_qp) {
		errorInfoMes("Client resources are not properly setup\n");
		return -EINVAL;
	}

	/* Set rtn_msg_mr */
	c_info->client_metadata_mr = rdma_buffer_register(
									c_info->pd,			/* which protection domain */
									&c_info->client_metadata_attr,			/* what memory */
									sizeof(c_info->client_metadata_attr),	/* what length */
									// (IBV_ACCESS_LOCAL_WRITE) 
									(IBV_ACCESS_LOCAL_WRITE |
									IBV_ACCESS_REMOTE_WRITE |
									IBV_ACCESS_REMOTE_READ) 				/* access permissions */
									);
	if(!c_info->client_metadata_mr){
		errorInfoMes("Failed to register client attr buffer\n");
		return -ENOMEM;
	}

	//post_recv of metadata
	c_info->client_recv_sge.addr = (uint64_t) c_info->client_metadata_mr->addr;	// same as &client_buffer_attr
	c_info->client_recv_sge.length = c_info->client_metadata_mr->length;
	c_info->client_recv_sge.lkey = c_info->client_metadata_mr->lkey;

	/* Now we link this SGE to the work request (WR) */
	memset(&c_info->client_recv_wr, 0, sizeof(c_info->client_recv_wr));
	c_info->client_recv_wr.sg_list = &c_info->client_recv_sge;
	c_info->client_recv_wr.num_sge = 1; // only one SGE
	c_info->client_recv_wr.wr_id = (uint64_t)c_info;

	ret = ibv_post_recv(c_info->client_qp,				/* which QP */
						&c_info->client_recv_wr,		/* receive work request*/
						&c_info->bad_client_recv_wr);	/* error WRs */

	if (ret) {
		errorInfoMes("Failed to pre-post the receive buffer, errno: %d \n", ret);
		return -ret;
	}
	debugInfoMes("Receive buffer pre-posting is successful \n");
	
	
	memset(&conn_param, 0, sizeof(conn_param));
	conn_param.initiator_depth = 3;
	conn_param.responder_resources = 3;
	ret = rdma_accept(c_info->cm_client_id, &conn_param);
	if (ret) {
		errorInfoMes("Failed to accept the connection, errno: %d \n", -errno);
		return -errno;
	}
	debugInfoMes("Going to wait for : RDMA_CM_EVENT_ESTABLISHED event \n");

	ret = rdma_ack_cm_event(s_info->cm_event);
	// pthread_rwlock_unlock(&s_info->rwlock);
	if (ret) {
		errorInfoMes("Failed to acknowledge the cm event %d\n", -errno);
		return -errno;
	}
	debugInfoMes("STATUS is RDMA_CM_EVENT_ESTABLISHED\n");

	struct ibv_qp_attr attr;
	struct ibv_qp_init_attr init_attr;
	ibv_query_qp(c_info->client_qp, &attr, IBV_QP_STATE, &init_attr);
	debugInfoMes("QP state after rdma_accept: %d\n", attr.qp_state); // IBV_QPS_RTS = 4

	/* Just FYI: How to extract connection information */
	memcpy(&remote_sockaddr /* where to save */,
			rdma_get_peer_addr(c_info->cm_client_id) /* gives you remote sockaddr */,
			sizeof(struct sockaddr_in) /* max size */
			);
	debugInfoMes("A new connection is accepted from %s \n", inet_ntoa(remote_sockaddr.sin_addr));

	/* We got client_metadata*/

	c_info->rtn_mr = rdma_buffer_register(
		c_info->pd,						/* which protection domain */
		&c_info->rtn,
		sizeof(c_info->rtn),	/* what size to allocate */
		(IBV_ACCESS_LOCAL_WRITE|
		IBV_ACCESS_REMOTE_READ|
		IBV_ACCESS_REMOTE_WRITE));		/* access permissions */

	return 1;
}

int global_disconnect_and_cleanup(struct server_r_info* s_info, struct client_r_info* c_info)
{
	/*
	<정리순서>
	1. rdma_destroy_qp()              // 먼저 QP 정리
	2. rdma_destroy_id()              // 그 후 ID 정리
	3. usleep(1000~10000) or polling  // 약간 대기 또는 사용 확인
	4. ibv_destroy_cq()               // 이제 CQ 해제
	5. ibv_destroy_comp_channel()     // 그리고 Comp Channel 해제
	6. ibv_dealloc_pd()               // 마지막으로 PD 해제
	*/
	int ret = -1;

	debugInfoMes("Waiting for cm event: RDMA_CM_EVENT_DISCONNECTED\n");
	while(c_info->status != RDMA_CM_EVENT_DISCONNECTED){
		// usleep(1000);
	}

	debugInfoMes("A disconnect event is received from the client...\n");
	// pthread_rwlock_wrlock(&c_info->rwlock);

	rdma_buffer_deregister(c_info->req_mr);
	rdma_buffer_deregister(c_info->rtn_mr);
	rdma_buffer_deregister(c_info->client_metadata_mr);
	rdma_buffer_deregister(c_info->server_metadata_mr);


	rdma_destroy_qp(c_info->cm_client_id->qp);
	ret = rdma_destroy_id(c_info->cm_client_id);
	if (ret ) {
		errorInfoMes("Failed to destroy client id cleanly, %d \n", -errno);
		// we continue anyways;
	}

	// ibv_ack_cq_events(c_info->cq, 1);  // 이벤트 수에 따라 조정
	// ret = ibv_req_notify_cq(c_info->cq, 0); // CQ polling loop에서 빠져나오게 유도
	// if (ret) {
    // 	errorInfoMes("Failed to notify CQ, ret = %d\n", -errno);
	// }
	// usleep(1000);  // QP/ID 정리 완료를 기다림

	// /* Destroy CQ */
	// ret = ibv_destroy_cq(c_info->cq);
	// if (ret) {
	// 	errorInfoMes("Failed to destroy completion queue cleanly, %d \n", -errno);
	// 	// we continue anyways;
	// }

	// /* Destroy completion channel */
	// ret = ibv_destroy_comp_channel(c_info->io_completion_channel);
	// if (ret) {
	// 	errorInfoMes("Failed to destroy completion channel cleanly, %d \n", -errno);
	// 	// we continue anyways;
	// }

	// /* Destroy protection domain */
	// ret = ibv_dealloc_pd(c_info->pd);
	// if (ret ) {
	// 	errorInfoMes("Failed to destroy client protection domain cleanly, %d \n", -errno);
	// 	// we continue anyways;
	// }
	// pthread_rwlock_unlock(&c_info->rwlock);
	// pthread_rwlock_destroy(&c_info->rwlock);

	return 0;
}

int server_disconnect_and_cleanup(struct server_r_info* s_info, struct client_r_info* c_info)
{
	int ret;
	ret = rdma_disconnect(c_info->cm_client_id);
	if (ret) {
		errorInfoMes("rdma_disconnect failed");
		return ret;
	}
	ret = global_disconnect_and_cleanup(s_info, c_info);
	return 0;
}
