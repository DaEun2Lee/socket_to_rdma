#include "rdma_client.h"

struct sockaddr_in *server_sockaddr;

int null_client_r_info(struct client_r_info* r_info)
{
	if (!r_info) return -1;

	r_info->cm_event_channel = NULL;
	r_info->cm_client_id = NULL;
	r_info->pd = NULL;
	r_info->io_completion_channel = NULL;
	r_info->client_cq = NULL;
	r_info->client_qp = NULL;
	r_info->cm_event = NULL;

	// 메모리 관련
	r_info->client_metadata_mr = NULL;
	r_info->server_metadata_mr = NULL;
	r_info->req_mr = NULL;
	r_info->rtn_mr = NULL;

    // Send/Recv WR 및 SGE
    memset(&r_info->client_send_wr, 0, sizeof(r_info->client_send_wr));
    r_info->bad_client_send_wr = NULL;
    memset(&r_info->server_recv_wr, 0, sizeof(r_info->server_recv_wr));
    r_info->bad_server_recv_wr = NULL;
    memset(&r_info->client_send_sge, 0, sizeof(r_info->client_send_sge));
    memset(&r_info->server_recv_sge, 0, sizeof(r_info->server_recv_sge));

    // eventfd
    r_info->eventfd = -1;

    // buffer attr 및 실제 데이터 구조체는 필요 시 memset 가능
    memset(&r_info->client_metadata_attr, 0, sizeof(r_info->client_metadata_attr));
    memset(&r_info->server_metadata_attr, 0, sizeof(r_info->server_metadata_attr));
    memset(&r_info->req, 0, sizeof(r_info->req));
    memset(&r_info->rtn, 0, sizeof(r_info->rtn));

	return 0;
}
void set_sockaddr()
{
	int ret;
	server_sockaddr = malloc(sizeof(struct sockaddr_in));
	memset(server_sockaddr, 0, sizeof(struct sockaddr_in));
	server_sockaddr->sin_family = AF_INET;
	server_sockaddr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	ret = get_addr(DEFAULT_RDMA_SERVER_IP, (struct sockaddr*) server_sockaddr);
	if (ret) {
		errorInfoMes("Invalid IP \n");
		return;
	}
	/* passed port to listen on */
	server_sockaddr->sin_port = htons(DEFAULT_RDMA_PORT);
}

/* This function prepares client side connection resources for an RDMA connection */
int client_prepare_connection(struct sockaddr_in *s_addr, struct client_r_info* r_info)
{
	int ret = -1;
	debugInfoMes("\n");
	/*  Open a channel used to report asynchronous communication event */
	r_info->cm_event_channel = rdma_create_event_channel();
	debugInfoMes("\n");

	if (!r_info->cm_event_channel) {
		errorInfoMes("Creating cm event channel failed, errno: %d \n", -errno);
		return -errno;
	}
	debugInfoMes("RDMA CM event channel is created at : %p \n", r_info->cm_event_channel);

	ret = rdma_create_id(r_info->cm_event_channel, 
							&r_info->cm_client_id,
							r_info,
							RDMA_PS_TCP);	// RDMA_PS_IB

	if (ret) {
		errorInfoMes("Creating cm id failed with errno: %d \n", -errno);
		return -errno;
	}

	/* Resolve destination and optional source addresses from IP addresses  to
	 * an RDMA address.  If successful, the specified rdma_cm_id will be bound
	 * to a local device. */
	ret = rdma_resolve_addr(r_info->cm_client_id, NULL, (struct sockaddr*) s_addr, TIMEOUT_IN_MS);		 
	if (ret) {
		errorInfoMes("Failed to resolve address, errno: %d \n", -errno);
		return -errno;
	}

	debugInfoMes("waiting for cm event: RDMA_CM_EVENT_ADDR_RESOLVED\n");
	ret  = process_rdma_cm_event(r_info->cm_event_channel,
									RDMA_CM_EVENT_ADDR_RESOLVED,
									&r_info->cm_event);
	if (ret) {
		errorInfoMes("Failed to receive a valid event, ret = %d \n", ret);
		return ret;
	}

	/* we ack the event */
	ret = rdma_ack_cm_event(r_info->cm_event);
	if (ret) {
		errorInfoMes("Failed to acknowledge the CM event, errno: %d\n", -errno);
		return -errno;
	}
	debugInfoMes("RDMA address is resolved \n");

	 /* Resolves an RDMA route to the destination address in order to
	  * establish a connection */
	ret = rdma_resolve_route(r_info->cm_client_id, 2000);
	if (ret) {
		errorInfoMes("Failed to resolve route, erno: %d \n", -errno);
	       return -errno;
	}

	debugInfoMes("waiting for cm event: RDMA_CM_EVENT_ROUTE_RESOLVED\n");
	ret = process_rdma_cm_event(r_info->cm_event_channel,
			RDMA_CM_EVENT_ROUTE_RESOLVED,
			&r_info->cm_event);
	if (ret) {
		errorInfoMes("Failed to receive a valid event, ret = %d \n", ret);
		return ret;
	}
	/* we ack the event */
	ret = rdma_ack_cm_event(r_info->cm_event);
	if (ret) {
		errorInfoMes("Failed to acknowledge the CM event, errno: %d \n", -errno);
		return -errno;
	}
	debugInfoMes("Trying to connect to server at : %s port: %d \n",
			inet_ntoa(s_addr->sin_addr),
			ntohs(s_addr->sin_port));

	/* Protection Domain (PD) is similar to a "process abstraction"
	 * in the operating system. All resources are tied to a particular PD.
	 * And accessing recourses across PD will result in a protection fault.
	 */
	r_info->pd = ibv_alloc_pd(r_info->cm_client_id->verbs);
	if (!r_info->pd) {
		errorInfoMes("Failed to alloc pd, errno: %d \n", -errno);
		return -errno;
	}
	debugInfoMes("pd allocated at %p \n", r_info->pd);

	/* Now we need a completion channel, were the I/O completion
	 * notifications are sent. Remember, this is different from connection
	 * management (CM) event notifications.
	 * A completion channel is also tied to an RDMA device, hence we will
	 * use cm_client_id->verbs.
	 */
	r_info->io_completion_channel = ibv_create_comp_channel(r_info->cm_client_id->verbs);
	if (!r_info->io_completion_channel) {
		errorInfoMes("Failed to create IO completion event channel, errno: %d\n", -errno);
		return -errno;
	}
	debugInfoMes("completion event channel created at : %p \n", r_info->io_completion_channel);

	/* Now we create a completion queue (CQ) where actual I/O
	 * completion metadata is placed. The metadata is packed into a structure
	 * called struct ibv_wc (wc = work completion). ibv_wc has detailed
	 * information about the work completion. An I/O request in RDMA world
	 * is called "work".
	 */
	r_info->client_cq = ibv_create_cq(r_info->cm_client_id->verbs, /* which device*/
										CQ_CAPACITY, /* maximum capacity*/
										NULL, /* user context, not used here */
										r_info->io_completion_channel, /* which IO completion channel */
										0 /* signaling vector, not used here*/
										);
	if (!r_info->client_cq) {
		errorInfoMes("Failed to create CQ, errno: %d \n", -errno);
		return -errno;
	}
	debugInfoMes("CQ created at %p with %d elements \n", r_info->client_cq, r_info->client_cq->cqe);

	ret = ibv_req_notify_cq(r_info->client_cq, 0);
	if (ret) {
		errorInfoMes("Failed to request notifications, errno: %d\n", -errno);
		return -errno;
	}

	/* Now the last step, set up the queue pair (send, recv) queues and their capacity.
	* The capacity here is define statically but this can be probed from the
	* device. We just use a small number as defined in rdma_common.h */
	struct ibv_qp_init_attr qp_init_attr;
	memset(&qp_init_attr, 0, sizeof qp_init_attr);
	qp_init_attr.cap.max_recv_sge = MAX_SGE; /* Maximum SGE per receive posting */
	qp_init_attr.cap.max_recv_wr = MAX_WR; /* Maximum receive posting capacity */
	qp_init_attr.cap.max_send_sge = MAX_SGE; /* Maximum SGE per send posting */
	qp_init_attr.cap.max_send_wr = MAX_WR; /* Maximum send posting capacity */
	qp_init_attr.qp_type = IBV_QPT_RC; /* QP type, RC = Reliable connection */
	/* We use same completion queue, but one can use different queues */
	qp_init_attr.recv_cq = r_info->client_cq; /* Where should I notify for receive completion operations */
	qp_init_attr.send_cq = r_info->client_cq; /* Where should I notify for send completion operations */
	/*Lets create a QP */
	ret = rdma_create_qp(r_info->cm_client_id /* which connection id */,
								r_info->pd /* which protection domain*/,
								&qp_init_attr /* Initial attributes */);
	if (ret) {
		errorInfoMes("Failed to create QP, errno: %d \n", -errno);
		return -errno;
	}
	warningInfoMes("QP's MAX_WR is %d \n", MAX_WR);
	r_info->client_qp = r_info->cm_client_id->qp;
	debugInfoMes("QP created at %p \n", r_info->client_qp);
	return NULL;
}

int client_pre_post_req_buffer(struct client_r_info* r_info)
{
	int ret = -1;
	// alloc
	//@delee
	r_info->req_mr = rdma_buffer_register(
		r_info->pd,
		&r_info->req,
		sizeof(r_info->req),
		(IBV_ACCESS_LOCAL_WRITE|
		IBV_ACCESS_REMOTE_READ|
		IBV_ACCESS_REMOTE_WRITE));
	if(!r_info->req_mr) {
		errorInfoMes("Failed to register the first buffer, ret = %d \n", ret);
		return ret;
	}

	r_info->server_metadata_mr = rdma_buffer_register(
		r_info->pd,
		&r_info->server_metadata_attr,
		sizeof(r_info->server_metadata_attr),
		(IBV_ACCESS_LOCAL_WRITE |
		IBV_ACCESS_REMOTE_WRITE |
		IBV_ACCESS_REMOTE_READ));
	if(!r_info->server_metadata_mr){
		errorInfoMes("Failed to setup the server metadata mr , -ENOMEM\n");
		return -ENOMEM;
	}

	r_info->server_metadata_attr.address = (uint64_t) r_info->req_mr->addr;
	r_info->server_metadata_attr.length = (uint32_t) r_info->req_mr->length;
	r_info->server_metadata_attr.stag.local_stag = r_info->req_mr->lkey;

	r_info->server_recv_sge.addr = (uint64_t) r_info->server_metadata_mr->addr;
	r_info->server_recv_sge.length = (uint32_t) r_info->server_metadata_mr->length;
	r_info->server_recv_sge.lkey = (uint32_t) r_info->server_metadata_mr->lkey;

	/* now we link it to the request */
	memset(&r_info->server_recv_wr, 0, sizeof(r_info->server_recv_wr));
	r_info->server_recv_wr.sg_list = &r_info->server_recv_sge;
	r_info->server_recv_wr.num_sge = 1;

	ret = ibv_post_recv(r_info->client_qp,				/* which QP */
						&r_info->server_recv_wr,		/* receive work request*/
						&r_info->bad_server_recv_wr);	/* error WRs */
	if (ret) {
		errorInfoMes("Failed to pre-post the receive buffer, errno: %d \n", ret);
		return ret;
	}
	debugInfoMes("Receive buffer pre-posting is successful\n");

	return 0;
}

/* Connects to the RDMA server */
int client_connect_to_server(struct client_r_info *r_info)
{
	struct rdma_conn_param conn_param;
	int ret = -1;
	memset(&conn_param, 0, sizeof(conn_param));
	conn_param.initiator_depth = 3;
	conn_param.responder_resources = 3;
	conn_param.retry_count = 3; 	// if fail, then how many times to retry

	debugInfoMes("Connecting to server at %s port %d \n",
					inet_ntoa(server_sockaddr->sin_addr),
					ntohs(server_sockaddr->sin_port));
	ret = rdma_connect(r_info->cm_client_id, &conn_param);
	if (ret) {
		errorInfoMes("Failed to connect to remote host , errno: %d\n", -errno);
		return -errno;
	}

	debugInfoMes("waiting for cm event: RDMA_CM_EVENT_ESTABLISHED\n");
	ret = process_rdma_cm_event(r_info->cm_event_channel,
								RDMA_CM_EVENT_ESTABLISHED,
								&r_info->cm_event);
	if (ret) {
		errorInfoMes("Failed to get cm event, ret = %d \n", ret);
 	    return ret;
	}

	ret = rdma_ack_cm_event(r_info->cm_event);
	if (ret) {
		errorInfoMes("Failed to acknowledge cm event, errno: %d\n", -errno);
		return -errno;
	}

	debugInfoMes("The client is connected successfully \n");
	return 0;
}

/* Exchange buffer metadata with the server. The client sends its, and then receives
 * from the server. The client-side metadata on the server is _not_ used because
 * this program is client driven. But it shown here how to do it for the illustration
 * purposes
 */
int client_xchange_metadata_with_server(struct client_r_info *r_info)
{
	struct ibv_wc wc[2];
	int ret = -1;

	debugInfoMes("\n");
	r_info->rtn_mr = rdma_buffer_register(
						r_info->pd,
						&r_info->rtn,
						sizeof(r_info->rtn),
						(IBV_ACCESS_LOCAL_WRITE|
						IBV_ACCESS_REMOTE_READ|
						IBV_ACCESS_REMOTE_WRITE));
	if(!r_info->rtn_mr) {
		errorInfoMes("Failed to register the first buffer, ret = %d \n", ret);
		return ret;
	}

	r_info->client_metadata_mr = rdma_buffer_register(
									r_info->pd,
									&r_info->client_metadata_attr,
									sizeof(r_info->client_metadata_attr),
									(IBV_ACCESS_LOCAL_WRITE |
									IBV_ACCESS_REMOTE_WRITE |
									IBV_ACCESS_REMOTE_READ));
	if(!r_info->client_metadata_mr){
		errorInfoMes("Failed to setup the server metadata mr , -ENOMEM\n");
		return -ENOMEM;
	}

	r_info->client_metadata_attr.address = (uint64_t) r_info->rtn_mr->addr;
	r_info->client_metadata_attr.length = (uint32_t) r_info->rtn_mr->length;
	r_info->client_metadata_attr.stag.local_stag = r_info->rtn_mr->lkey;
	r_info->client_metadata_attr.stag.remote_stag = r_info->rtn_mr->rkey;

	/* now we fill up SGE */
	r_info->client_send_sge.addr = (uintptr_t) r_info->client_metadata_mr->addr;
	r_info->client_send_sge.length = (uint32_t) r_info->client_metadata_mr->length;
	r_info->client_send_sge.lkey = (uint32_t) r_info->client_metadata_mr->lkey;

	/* now we link to the send work request */
	memset(&r_info->client_send_wr, 0, sizeof(r_info->client_send_wr));
	r_info->client_send_wr.sg_list = &r_info->client_send_sge;
	r_info->client_send_wr.num_sge = 1;
	r_info->client_send_wr.opcode = IBV_WR_SEND;
	r_info->client_send_wr.send_flags = IBV_SEND_SIGNALED;

	/* Now we post it */
	ret = ibv_post_send(r_info->client_qp,
						&r_info->client_send_wr,
						&r_info->bad_client_send_wr);
	if (ret) {
		errorInfoMes("Failed to send client metadata, errno: %d \n", -errno);
		return -errno;
	}

	/* at this point we are expecting 2 work completion. One for our
	 * send and one for recv that we will get from the server for
	 * its buffer information */
	ret = process_work_completion_events(r_info->io_completion_channel,	wc, 2);
	if(ret != 2) {
		errorInfoMes("We failed to get 2 work completions , ret = %d \n", ret);
		return ret;
	}

	debugInfoMes("Server sent us its buffer location and credentials, showing \n");
	debugInfoMes("c_info->client_metadata_attr\n");
	show_rdma_buffer_attr(&r_info->client_metadata_attr);
	debugInfoMes("c_info->server_metadata_attr\n");
	show_rdma_buffer_attr(&r_info->server_metadata_attr);

	return 0;
}

/* This function disconnects the RDMA connection from the server and cleans up
 * all the resources.
 */
int client_disconnect_and_clean(struct client_r_info* r_info)
{
	struct rdma_cm_event *cm_event = NULL;
	int ret = -1;

	/* active disconnect from the client side */
	ret = rdma_disconnect(r_info->cm_client_id);
	if (ret) {
		errorInfoMes("Failed to disconnect, errno: %d \n", -errno);
		//continuing anyways
	}

	ret = process_rdma_cm_event(r_info->cm_event_channel,
			RDMA_CM_EVENT_DISCONNECTED,
			&r_info->cm_event);
	if (ret) {
		errorInfoMes("Failed to get RDMA_CM_EVENT_DISCONNECTED event, ret = %d\n",
				ret);
		//continuing anyways
	}

	ret = rdma_ack_cm_event(r_info->cm_event);
	if (ret) {
		errorInfoMes("Failed to acknowledge cm event, errno: %d\n",
			       -errno);
		//continuing anyways
	}

	/* Destroy QP */
	rdma_destroy_qp(r_info->cm_client_id);
	/* Destroy client cm id */
	ret = rdma_destroy_id(r_info->cm_client_id);
	if (ret) {
		errorInfoMes("Failed to destroy client id cleanly, %d \n", -errno);
		// we continue anyways;
	}

	/* Destroy CQ */
	ret = ibv_destroy_cq(r_info->client_cq);
	if (ret) {
		errorInfoMes("Failed to destroy completion queue cleanly, %d \n", -errno);
		// we continue anyways;
	}

	/* Destroy completion channel */
	ret = ibv_destroy_comp_channel(r_info->io_completion_channel);
	if (ret) {
		errorInfoMes("Failed to destroy completion channel cleanly, %d \n", -errno);
		// we continue anyways;
	}

	/* Destroy memory buffers */
	rdma_buffer_deregister(r_info->server_metadata_mr);
	rdma_buffer_deregister(r_info->client_metadata_mr);
	rdma_buffer_deregister(r_info->rtn_mr);
	rdma_buffer_deregister(r_info->req_mr);
	
	/* Destroy protection domain */
	ret = ibv_dealloc_pd(r_info->pd);
	if (ret) {
		errorInfoMes("Failed to destroy client protection domain cleanly, %d \n", -errno);
		// we continue anyways;
	}

	rdma_destroy_event_channel(r_info->cm_event_channel);
	debugInfoMes("Client resource clean up is complete \n");
	return 0;
}

int client_recv_disconnect_and_clean(struct client_r_info* r_info)
{
	int ret;
	ret = process_rdma_cm_event(r_info->cm_event_channel,
								RDMA_CM_EVENT_DISCONNECTED,
								&r_info->cm_event);
	if (ret) {
		errorInfoMes("Failed to get RDMA_CM_EVENT_DISCONNECTED event, ret = %d\n",ret);
		//continuing anyways
	}

	ret = rdma_ack_cm_event(r_info->cm_event);
	if (ret) {
		errorInfoMes("Failed to acknowledge cm event, errno: %d\n", -errno);
		//continuing anyways
	}

	/* Destroy QP */
	rdma_destroy_qp(r_info->cm_client_id);
	/* Destroy client cm id */
	ret = rdma_destroy_id(r_info->cm_client_id);
	if (ret) {
		errorInfoMes("Failed to destroy client id cleanly, %d \n", -errno);
		// we continue anyways;
	}

	/* Destroy CQ */
	ret = ibv_destroy_cq(r_info->client_cq);
	if (ret) {
		errorInfoMes("Failed to destroy completion queue cleanly, %d \n", -errno);
		// we continue anyways;
	}

	/* Destroy completion channel */
	ret = ibv_destroy_comp_channel(r_info->io_completion_channel);
	if (ret) {
		errorInfoMes("Failed to destroy completion channel cleanly, %d \n", -errno);
		// we continue anyways;
	}
	
	/* Destroy memory buffers */
	rdma_buffer_deregister(r_info->server_metadata_mr);
	rdma_buffer_deregister(r_info->client_metadata_mr);
	rdma_buffer_deregister(r_info->rtn_mr);
	rdma_buffer_deregister(r_info->req_mr);
	
	/* Destroy protection domain */
	ret = ibv_dealloc_pd(r_info->pd);
	if (ret) {
		errorInfoMes("Failed to destroy client protection domain cleanly, %d \n", -errno);
		// we continue anyways;
	}

	rdma_destroy_event_channel(r_info->cm_event_channel);
	debugInfoMes("Client resource clean up is complete \n");
	return 0;
}


int post_write_req(struct client_r_info* r_info) {
	int ret;
	debugInfoMes("\n");
	// /* now we fill up SGE */
	r_info->client_send_sge.addr = (uint64_t) r_info->req_mr->addr;
	r_info->client_send_sge.length = r_info->req_mr->length;
	r_info->client_send_sge.lkey = r_info->req_mr->lkey;

	/* now we link to the send work request */
	memset(&r_info->client_send_wr, 0, sizeof(r_info->client_send_wr));
	r_info->client_send_wr.sg_list = &r_info->client_send_sge;
	r_info->client_send_wr.num_sge = 1;

	r_info->client_send_wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
	r_info->client_send_wr.send_flags = IBV_SEND_SIGNALED;
	r_info->client_send_wr.imm_data = htonl(r_info->sockfd);
	
	r_info->client_send_wr.wr.rdma.remote_addr = r_info->server_metadata_attr.address;
	r_info->client_send_wr.wr.rdma.rkey = r_info->server_metadata_attr.stag.remote_stag;

	debugInfoMes("sockfd is %d and req_rdma = %d\n", r_info->sockfd, r_info->req.rdma_req);

	/* 1) QP 상태 확인: RTS가 아니면 보내지 않음 */
 	struct ibv_qp_attr qp_attr;
    int qp_attr_mask = IBV_QP_STATE;
    const int max_retries = 3;      /* 재시도 횟수 (환경에 맞게 조정) */
	const int cq_poll_attempts = 32;/* CQ에서 최대 몇 개의 WC를 폴링할지 제한 */
	debugInfoMes("QP at %p \n", r_info->client_qp);

	// print_qp_state(r_info->client_qp);
	/* 2) 실제 post_send 시도 + ENOMEM 완화 루프 */
	for (int attempt = 0; attempt <= max_retries; attempt++) {
        ret = ibv_post_send(r_info->client_qp,
                            &r_info->client_send_wr,
                            &r_info->bad_client_send_wr);

        if (ret == 0) {
            /* 성공 */ 
            debugInfoMes("ibv_post_send succeeded (attempt=%d)\n", attempt);
			if(r_info->sockfd == 13 && r_info->req.rdma_req == RDMA_REQ_CONNECT) {
				warningInfoMes("Posted RDMA WRITE WITH IMM: sockfd=%d, req_rdma=%d, imm_data=%u\n",
				r_info->sockfd, r_info->req.rdma_req, ntohl(r_info->client_send_wr.imm_data));
			}
            return 1;
        }
        /* 실패: ret != 0 */
        errorInfoMes("ibv_post_send failed (attempt=%d): ret=%d (%s)\n", attempt, ret, strerror(ret));

        /* ENOMEM(12)인 경우: CQ에 쌓인 completion이 원인일 가능성 있으므로 CQ 소진을 시도 */
        if (ret == ENOMEM || errno == ENOMEM) {
			// exit(EXIT_FAILURE);
			usleep(500); /* 약간의 지연 */

            int polled = 0, ne;
            struct ibv_wc wc;
            debugInfoMes("Detected ENOMEM; try to poll CQ to consume completions (attempt=%d)\n", attempt);

            /* 짧은 루프: CQ를 최대 cq_poll_attempts 번 폴링 */
            for (int i = 0; i < cq_poll_attempts; ++i) {
                ne = ibv_poll_cq(r_info->client_cq, 1, &wc);
                if (ne < 0) {
                    errorInfoMes("ibv_poll_cq error while handling ENOMEM: %d\n", ne);
                    break;
                } else if (ne == 0) {
                    /* 더 이상 completion 없음 */
                    break;
                } else {
                    /* 하나의 WC 처리(로그 또는 필요 작업) */
                    ++polled;
                    if (wc.status != IBV_WC_SUCCESS) {
                        errorInfoMes("WC error while draining CQ: status=%d opcode=%d vendor_err=%d\n",
                                     wc.status, wc.opcode, wc.vendor_err);
						
						if(wc.status ==5){ //REMOTE ACCESS ERROR
							errorInfoMes("REMOTE ACCESS ERROR detected while draining CQ -> aborting\n");

							// exit(EXIT_FAILURE);
						}
						/* 아주 짧은 백오프 후 재시도 */
            			usleep(1000 * (cq_poll_attempts + 1)); /* 시도 횟수에 따라 조금씩 늘림 */
                    } else {
                        debugInfoMes("Drained one WC: opcode=%d\n", wc.opcode);
                        /* IMM이면 처리할 로직이 있으면 여기서 호출 가능 */
                    }
                }
            }

            debugInfoMes("drained %d completions from CQ (attempt=%d)\n", polled, attempt);

            /* 아주 짧은 백오프 후 재시도 */
            usleep(1000 * (attempt + 1)); /* 시도 횟수에 따라 조금씩 늘림 */
			warningInfoMes("Retrying ibv_post_send after handling ENOMEM (attempt=%d)\n", attempt);
        } else {
			/* ENOMEM 이외의 오류는 재시도 의미 없음: 바로 실패 반환 */
			errorInfoMes("ibv_post_send non-ENOMEM error -> aborting\n");

			exit(EXIT_FAILURE);
			return -ret;
		}
    }

    /* 재시도 횟수 초과: 실패로 간주 */
    errorInfoMes("ibv_post_send failed after %d retries, giving up\n", max_retries);
    return -1;
}

int post_recv_rtn(struct client_r_info* r_info)
{
	if(r_info == NULL || r_info->rtn_mr == NULL) {
		errorInfoMes("r_info or r_info->rtn_mr is NULL\n");
		return -1;
	}
	debugInfoMes("\n");
	r_info->server_recv_sge.addr = (uint64_t) r_info->rtn_mr->addr;
	debugInfoMes("\n");
	r_info->server_recv_sge.length = r_info->rtn_mr->length;
	debugInfoMes("\n");
	r_info->server_recv_sge.lkey =  r_info->rtn_mr->lkey;
	debugInfoMes("\n");

	memset(&r_info->server_recv_wr, 0, sizeof(r_info->server_recv_wr));
	r_info->server_recv_wr.sg_list = &r_info->server_recv_sge;
	r_info->server_recv_wr.num_sge = 1;
	debugInfoMes("\n");
	ibv_post_recv(r_info->client_qp, &r_info->server_recv_wr, &r_info->bad_server_recv_wr); 
	debugInfoMes("\n");
	return 0;
}

/* main */
int connect_server(struct client_r_info *r_info)
{
	int ret, option;
	debugInfoMes("\n");

	ret = client_prepare_connection(server_sockaddr, r_info);
	if (ret) {
		errorInfoMes("Failed to setup client connection , ret = %d \n", ret);
		return ret;
	}
	debugInfoMes("\n");

	ret = client_pre_post_req_buffer(r_info);
	if (ret) {
		errorInfoMes("Failed to setup client connection , ret = %d \n", ret);
		return ret;
	}
	debugInfoMes("\n");

	ret = client_connect_to_server(r_info);
	if (ret) {
		errorInfoMes("Failed to setup client connection , ret = %d \n", ret);
		return ret;
	}
	debugInfoMes("\n");

	ret = client_xchange_metadata_with_server(r_info);
	if (ret) {
		errorInfoMes("Failed to setup client connection , ret = %d \n", ret);
		return ret;
	}
	debugInfoMes("\n");

	return 0;
}

int set_req_sock(struct client_r_info *r_info, int sockfd, int req_rdma, uint64_t buf_sz, uint16_t port, const char *message, int *p_errno, void *buf){
	
	struct timespec start, end;
	int ret;
	debugInfoMes("sockfd = %d and req_rdma = %d\n", sockfd, req_rdma);

	if(req_rdma < RDMA_REQ_SOCKET || req_rdma > RDMA_REQ_DEL_RDMA_CONN  )
		return -1;

	warningInfoMes("sockfd = %d, req_rdma = %d, buf_sz = %lu, port = %d\n",
					sockfd, req_rdma, buf_sz, port);

	// if(req_rdma ==3){
	// 	sleep(1);
	// }
	// if(req_rdma ==4){
	// 	warningInfoMes("req_rdma ==4-> sleep 3s\n");
	// 	sleep(3);
	// }

	r_info->req.rdma_req = req_rdma;
	r_info->req.buf_sz = buf_sz;
	r_info->req.port = port;
	r_info->sockfd = sockfd;

	debugInfoMes("port is %d\n", r_info->req.port);

	//write
	if(req_rdma == RDMA_REQ_WRITE) {
		debugInfoMes("req_rdma == RDMA_REQ_WRITE\n");
		uint64_t offset = 0;
		uint64_t header_sz = 12;
		uint64_t total_size = header_sz + buf_sz;

		uint64_t bytes_to_copy = (total_size - offset < BUFFER_SIZE)
									? total_size - offset
									: BUFFER_SIZE - header_sz;

		memcpy(&r_info->req.data, message, (bytes_to_copy - header_sz));

	//read
	} else if (req_rdma == RDMA_REQ_READ){
		debugInfoMes("req_rdma == RMDA_REQ_READ\n");
		// warningInfoMes("req_rdma == RMDA_REQ_READ, buf_sz = %lu\n", buf_sz);
	} else if (req_rdma == RDMA_REQ_EPOLL_CREATE1){
		debugInfoMes("req_rdma == RDMA_REQ_EPOLL_CREATE1\n");
	} else if (req_rdma == RDMA_REQ_EPOLL_CTL){
		debugInfoMes("req_rdma == RDMA_REQ_EPOLL_CTL\n");
		// debugInfoMes("serialize_epoll_event: events: %u, u64: %lu\n", event->events, event->data.u64);
	} else if (req_rdma == RDMA_REQ_EPOLL_WAIT){
		debugInfoMes("req_rdma == RDMA_REQ_EPOLL_WAIT\n");
		// serialize_epoll_event(event, &r_info->req.event);
	} else if (req_rdma == RDMA_REQ_SELECT){
		debugInfoMes("req_rdma == RDMA_REQ_SELECT\n");

	} else if (req_rdma == RDMA_REQ_CLOSE){
		debugInfoMes("req_rdma == RDMA_REQ_CLOSE\n");
		// r_info->req.fd = fd;
	} else if (req_rdma == RDMA_REQ_POLL){
		debugInfoMes("req_rdma == RDMA_REQ_POLL\n");
		// r_info->req.fd = sockfd;
		// r_info->req.timeout = timeout;
	}

	show_req_msg(&r_info->req);

	// clock_gettime(CLOCK_MONOTONIC, &start);
	ret = post_recv_rtn(r_info);
	if(ret != 0){
		errorInfoMes("Fail post_recv_rtn\n");
		return -1;
	}

	// clock_gettime(CLOCK_MONOTONIC, &end);
	// double elapsed = (end.tv_sec - start.tv_sec) + 
    //                  (end.tv_nsec - start.tv_nsec) / 1e9;

    // printf("[set_req_sick] show req_msg time: %.9f seconds\n", elapsed);

	// clock_gettime(CLOCK_MONOTONIC, &start);

	usleep(500); // 서버 post_recv 등록 대기 (RNR 방지)

	ret = post_write_req(r_info);
	if(ret < 0) {
		errorInfoMes("post_write_req failed, ret = %d\n", ret);
		return -1;
	}

	// clock_gettime(CLOCK_MONOTONIC, &end);
	//  elapsed = (end.tv_sec - start.tv_sec) + 
    //                  (end.tv_nsec - start.tv_nsec) / 1e9;

    // printf("[set_req_sick] post_write_req Elapsed time: %.9f seconds\n", elapsed);
	// clock_gettime(CLOCK_MONOTONIC, &start);

	ret = process_poll_cq_imm(r_info);
	if (ret < 0 ) {
		errorInfoMes("process_poll_cq_imm failed, ret = %d\n", ret);
		return -1;
	}

	// clock_gettime(CLOCK_MONOTONIC, &end);
	// elapsed = (end.tv_sec - start.tv_sec) + 
    //                  (end.tv_nsec - start.tv_nsec) / 1e9;

    // printf("[set_req_sick] process_poll_cq_imm Elapsed time: %.9f seconds\n", elapsed);
	// clock_gettime(CLOCK_MONOTONIC, &start);

	// warningInfoMes("ret = %d and req_rdma = %d\n", ret, req_rdma);

	if(req_rdma == RDMA_REQ_READ){
		debugInfoMes("req_rdma == RDMA_REQ_READ\n");
		if(buf == NULL){
			errorInfoMes("buf is NULL\n");
			return -1;
		}

		if(r_info->rtn.rtn_value < 0){
			debugInfoMes("r_info->rtn.rtn_value < 0\n");
			if(p_errno != NULL)
				*p_errno = r_info->rtn.errno_;
				return r_info->rtn.rtn_value;
		}
		debugInfoMes("buf = %p, src = %p, size = %d\n", buf, &r_info->rtn.data, r_info->rtn.rtn_value);
		memcpy(buf, &r_info->rtn.data, (int)r_info->rtn.rtn_value);
		// show_rtn_msg(&r_info->rtn);
		// debugInfoMes("r_info->rtn.data = %s\n", buf);
		// r_info->rtn.rtn_value = 1023;
		// memcpy(buf, &r_info->rtn.data, 1023);
		debugInfoMes("rtn_value is %d\n", r_info->rtn.rtn_value);
		return r_info->rtn.rtn_value;

	} else if(req_rdma == RDMA_REQ_WRITE){
		// if(buf == NULL){
		// 	debugInfoMes("buf is NULL\n");
		// 	return -1;
		// }
		// memset(&r_info->rtn.data, 0, sizeof(r_info->rtn.data));

		if (r_info->rtn.rtn_value < 0){
			if(p_errno != NULL)
				*p_errno = r_info->rtn.errno_;
			// return -1;
		}
		return r_info->rtn.rtn_value;
	} else if(req_rdma == RDMA_REQ_CLOSE){
		if (r_info->rtn.rtn_value < 0){
			if(p_errno != NULL)
				*p_errno = r_info->rtn.errno_;
			return -1;
		}
	} else if (req_rdma == RDMA_REQ_SELECT){
		if (r_info->rtn.rtn_value < 0){
			if(p_errno != NULL)
				*p_errno = r_info->rtn.errno_;
			return -1;
		}
		if (r_info->rtn.rtn_value == 0) {
			debugInfoMes("select timeout\n");
			return 0;
		}
		if (r_info->rtn.rtn_value > 0) {
			debugInfoMes("select success, r_info->rtn.rtn_value = %d\n", r_info->rtn.rtn_value);
			return r_info->rtn.rtn_value;
		}
	} else if(req_rdma == RDMA_REQ_EPOLL_CREATE1){
		if (r_info->rtn.rtn_value < 0){
			if(p_errno != NULL)
				*p_errno = r_info->rtn.errno_;
		}
		return r_info->rtn.rtn_value;
	} else if(req_rdma == RDMA_REQ_EPOLL_CTL){
		if (r_info->rtn.rtn_value < 0){
			if(p_errno != NULL)
				*p_errno = r_info->rtn.errno_;
		}
		return r_info->rtn.rtn_value;
	} else if(req_rdma == RDMA_REQ_EPOLL_WAIT){
		if (r_info->rtn.rtn_value < 0){
			if(p_errno != NULL)
				*p_errno = r_info->rtn.errno_;
		} else {
			// deserialize_epoll_event(event, &r_info->rtn.event); 
			// deserialize_epoll_event_struct(&r_info->rtn.data, event, r_info->rtn.rtn_value);
			// deserialize_epoll_event_struct(&r_info->rtn.data, event, r_info->rtn.rtn_value);
		}
		return r_info->rtn.rtn_value;
	} else if(req_rdma == RDMA_REQ_POLL){
		if (r_info->rtn.rtn_value < 0){
			if(p_errno != NULL)
				*p_errno = r_info->rtn.errno_;
		}
	} else if(req_rdma == RDMA_REQ_GETSOCKNAME){
		if (r_info->rtn.rtn_value < 0){
			if(p_errno != NULL)
				*p_errno = r_info->rtn.errno_;
			return -1;
		}

	} else {
		if (r_info->rtn.rtn_value < 0){
			if(p_errno != NULL)
				*p_errno = r_info->rtn.errno_;
			return -1;
		}
	} 
	return r_info->rtn.rtn_value;
}


int process_rdma_get_cq_event(struct client_r_info* r_info)
{
    // struct ibv_cq *cq_ptr = NULL;
    void *context = NULL;
    int ret = -1;

    ret = ibv_get_cq_event(r_info->io_completion_channel,   /* IO channel where we are expecting the notification */
                            &r_info->client_cq,        /* which CQ has an activity. This should be the same as CQ we created befor */
							&context);      /* Associated CQ user context, which we did set */
    if(ret < 0) {
        errorInfoMes("Failed to get next CQ event due to %d \n", -errno);

		// sleep(100);

        return -errno;
    }

    /* Similar to connection management events, we need to acknowledge CQ events */
    ibv_ack_cq_events(r_info->client_cq,
                        1 /* we received one event notification. This is not number of WC elements */);
 
	/* Request for more notifications. */
    ret = ibv_req_notify_cq(r_info->client_cq, 0);
    if (ret){
        errorInfoMes("Failed to request further notifications %d \n", -errno);
        return -errno;
    }

	return 1;
}

int process_poll_cq_imm(struct client_r_info* r_info)
{
    debugInfoMes("\n");

    struct ibv_wc *wc = malloc(sizeof(struct ibv_wc) * 1);
	int ret, ret_c;

    //polling
    while(true){

		ret = process_rdma_get_cq_event(r_info);
		/* process_rdma_get_cq_event() 호출하여 이벤트 가져오기
         * 반환값 > 0 : 이벤트 받음
         * 반환값 <= 0 : 오류 또는 이벤트 없음 (음수는 오류 코드)
         */
        if(ret > 0){
            while (ret_c = ibv_poll_cq(r_info->client_cq, 1, wc)) {
                if (wc->status != IBV_WC_SUCCESS) {
					errorInfoMes("WC error: %s, wr_id=%lu, opcode=%d\n",
                		ibv_wc_status_str(wc->status), wc->wr_id, wc->opcode);
                    /* return negative value */
                    return -(wc->status);
                }
				//  show_rtn_msg(&r_info->rtn);

				if (wc->opcode == IBV_WC_RECV_RDMA_WITH_IMM) {
					debugInfoMes("Received IBV_WC_RECV_RDMA_WITH_IMM, imm_data=%u\n", ntohl(wc->imm_data));
					show_rtn_msg(&r_info->rtn);
                    if (wc->wc_flags & IBV_WC_WITH_IMM) {
                        debugInfoMes("sockfd is %d\n", r_info->sockfd);
                        return 1;
                    } else {
                        errorInfoMes("Sockfd is not same. r_info's sockfd %d & recv sockfd %d\n", r_info->sockfd, ntohl(wc->imm_data));
                    }
                    break;
                }
            } if (ret_c < 0) {
				errorInfoMes("ibv_poll_cq failed with %d\n", ret);
				return ret;
			} else if (ret_c == 0) {
				// no WC
            	usleep(100);	// 아직 완료 안 됨 → 잠시 대기 후 다시 poll
				continue;
			}

		} else if (ret == 0) {
			// no event
			warningInfoMes("process_rdma_get_cq_event returned 0\n");
			// continue;
			return 0;
		} else {
			errorInfoMes("process_rdma_get_cq_event failed with ret = %d\n", ret);
			return -1;
		}
    }
    return 1;
}