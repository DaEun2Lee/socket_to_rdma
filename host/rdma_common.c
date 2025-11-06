/*
 * Implementation of the common RDMA functions. 
 *
 * Authors: Animesh Trivedi
 *          atrivedi@apache.org 
 */

#include "rdma_common.h"

void show_rdma_cmid(struct rdma_cm_id *id)
{
	if(!id){
		errorInfoMes("Passed ptr is NULL\n");
		return;
	}
	debugInfoMes("RDMA cm id at %p \n", id);
	if(id->verbs && id->verbs->device)
		debugInfoMes("dev_ctx: %p (device name: %s) \n", id->verbs, 
				id->verbs->device->name);
	if(id->channel)
		debugInfoMes("cm event channel %p\n", id->channel);
	debugInfoMes("QP: %p, port_space %x, port_num %u \n", id->qp, 
			id->ps,
			id->port_num);
}

void show_rdma_buffer_attr(struct rdma_buffer_attr *attr){
	if(!attr){
		errorInfoMes("Passed attr is NULL\n");
		return;
	}
	debugMes("---------------------------------------------------------\n");
	debugMes("buffer attr, addr: %p , len: %u , local_stag : 0x%x, remote_stag : 0x%x\n", 
			(void*) attr->address, 
			(unsigned int) attr->length,
			attr->stag.local_stag,
			attr->stag.remote_stag);
	debugMes("---------------------------------------------------------\n");
}

struct ibv_mr* rdma_buffer_alloc(struct ibv_pd *pd, uint32_t size,
    enum ibv_access_flags permission) 
{
	struct ibv_mr *mr = NULL;
	if (!pd) {
		errorInfoMes("Protection domain is NULL \n");
		return NULL;
	}
	void *buf = calloc(1, size);
	if (!buf) {
		errorInfoMes("failed to allocate buffer, -ENOMEM\n");
		return NULL;
	}
	debugInfoMes("Buffer allocated: %p , len: %u \n", buf, size);
	mr = rdma_buffer_register(pd, buf, size, permission);
	if(!mr){
		free(buf);
	}
	return mr;
}

struct ibv_mr *rdma_buffer_register(struct ibv_pd *pd, 
		void *addr, uint32_t length, 
		enum ibv_access_flags permission)
{
	struct ibv_mr *mr = NULL;
	if (!pd) {
		errorInfoMes("Protection domain is NULL, ignoring \n");
		return NULL;
	}
	mr = ibv_reg_mr(pd, addr, length, permission);
	if (!mr) {
		errorInfoMes("Failed to create mr on buffer, errno: %d \n", -errno);
		return NULL;
	}
	debugInfoMes("Registered: %p , len: %u , stag: 0x%x \n", 
			mr->addr, 
			(unsigned int) mr->length, 
			mr->lkey);
	return mr;
}

void rdma_buffer_free(struct ibv_mr *mr) 
{
	if (!mr) {
		errorInfoMes("Passed memory region is NULL, ignoring\n");
		return ;
	}
	void *to_free = mr->addr;
	rdma_buffer_deregister(mr);
	debugInfoMes("Buffer %p free'ed\n", to_free);
	free(to_free);
}

void rdma_buffer_deregister(struct ibv_mr *mr) 
{
	if (!mr) { 
		errorInfoMes("Passed memory region is NULL, ignoring\n");
		return;
	}
	debugInfoMes("Deregistered: %p , len: %u , stag : 0x%x \n", 
			mr->addr, 
			(unsigned int) mr->length, 
			mr->lkey);
	ibv_dereg_mr(mr);
}

int process_rdma_cm_event(struct rdma_event_channel *echannel,
		enum rdma_cm_event_type expected_event,
		struct rdma_cm_event **cm_event)
{
	int ret = 1;
	ret = rdma_get_cm_event(echannel, cm_event);
	if (ret) {
		errorInfoMes("Failed to retrieve a cm event, errno: %d \n",
				-errno);
		return -errno;
	}
	/* lets see, if it was a good event */
	if(0 != (*cm_event)->status){
		errorInfoMes("CM event has non zero status: %d\n", (*cm_event)->status);
		ret = -((*cm_event)->status);
		/* important, we acknowledge the event */
		rdma_ack_cm_event(*cm_event);
		return ret;
	}
	/* if it was a good event, was it of the expected type */
	if ((*cm_event)->event != expected_event) {
 		errorInfoMes("Unexpected event received: %s [ expecting: %s ]\n", 
				rdma_event_str((*cm_event)->event),
				rdma_event_str(expected_event));
		/* important, we acknowledge the event */
		rdma_ack_cm_event(*cm_event);
		return -1; // unexpected event :(
	}
	debugInfoMes("A new %s type event is received \n", rdma_event_str((*cm_event)->event));
	/* The caller must acknowledge the event */
	return ret;
}


int process_work_completion_events (struct ibv_comp_channel *comp_channel, struct ibv_wc *wc, int max_wc)
{
	struct ibv_cq *cq_ptr = NULL;
	void *context = NULL;
	int ret = -1, i, total_wc = 0;
	/* We wait for the notification on the CQ channel */
	ret = ibv_get_cq_event(comp_channel, /* IO channel where we are expecting the notification */ 
							&cq_ptr, /* which CQ has an activity. This should be the same as CQ we created before */ 
							&context); /* Associated CQ user context, which we did set */
	if (ret) {
		errorInfoMes("ibv_get_cq_event returned %d, errno=%d (%s)\n", ret, errno, strerror(errno));
		return -errno;
	}
	debugInfoMes("\n");
	/* Request for more notifications. */
	ret = ibv_req_notify_cq(cq_ptr, 0);
	if (ret){
		errorInfoMes("Failed to request further notifications %d \n", -errno);
		return -errno;
	}
	debugInfoMes("\n");
	/* We got notification. We reap the work completion (WC) element. It is 
	* unlikely but a good practice it write the CQ polling code that 
	* can handle zero WCs. ibv_poll_cq can return zero. Same logic as 
	* MUTEX conditional variables in pthread programming.
	*/
	total_wc = 0;
	do {
		ret = ibv_poll_cq(cq_ptr /* the CQ, we got notification for */, 
		max_wc - total_wc /* number of remaining WC elements*/,
		wc + total_wc/* where to store */);
		if (ret < 0) {
			errorInfoMes("Failed to poll cq for wc due to %d \n", ret);
			/* ret is errno here */
			return ret;
		}
		total_wc += ret;
	} while (total_wc < max_wc);
	debugInfoMes("%d WC are completed \n", total_wc);

	/* Now we check validity and status of I/O work completions */
	for( i = 0 ; i < total_wc ; i++) {
		if (wc[i].status != IBV_WC_SUCCESS) {
			errorInfoMes("Work completion (WC) has error status: %s at index %d\n", ibv_wc_status_str(wc[i].status), i);
			/* return negative value */
			return -(wc[i].status);
		}
	}
	/* Similar to connection management events, we need to acknowledge CQ events */
	ibv_ack_cq_events(cq_ptr,
						1 /* we received one event notification. This is not number of WC elements */
					);
	debugInfoMes("\n");
	return total_wc;
}


/* Code acknowledgment: rping.c from librdmacm/examples */
int get_addr(char *dst, struct sockaddr *addr)
{
	struct addrinfo *res;
	int ret = -1;
	ret = getaddrinfo(dst, NULL, NULL, &res);
	if (ret) {
		errorInfoMes("getaddrinfo failed - invalid hostname or IP address\n");
		return ret;
	}
	if (res->ai_family == AF_INET) {
		memcpy(addr, res->ai_addr, sizeof(struct sockaddr_in));
	}
	freeaddrinfo(res);
	return ret;
}

void show_req_msg(struct req_msg *msg){
	if(!msg){
		errorInfoMes("Passed msg is NULL\n");
		return;
	}

	debugMes("---------------------------------------------------------\n");
	debugMes("req_msg, rdma_req: %d , buf_sz: %ld , \n data : %s\n",
		msg->rdma_req, msg->buf_sz,	msg->data);
	debugMes("---------------------------------------------------------\n");
}

void show_rtn_msg(struct rtn_msg *msg){
	if(!msg){
		errorInfoMes("Passed msg is NULL\n");
		return;
	}

	debugMes("---------------------------------------------------------\n");
	debugMes("rtn_msg, rtn_value: %ld , errno: %d , \n data : %s\n",
		msg->rtn_value, msg->errno_,	msg->data);
	debugMes("---------------------------------------------------------\n");
}

/*
epoll 함수 인자 정리 (64bit 시스템 기준)

- create1
|---- int ----|
|	  4B	  |
|	flag	  |
|-------------| 

- ctl
|---- int ----|---- int ----|---- int ----|---- epoll_event ----|
|	  4B	  |		4B		|	  4B	  |		  12B (4+8)		|
|    epfd     |		op		|	  fd	  |	  	  event			|
|-------------|-------------|-------------|---------------------|


- wait
|---- int ----|---- int ----|---- int ----|---- int ----|---- epoll_event ----|
|	  4B	  |	  	4B		|	 4B		  |	 	4B		|		12B (4+8)	  |
|    nfds     |    epfd     |  maxevents  |	  timeout	|	  	  event		  |
|-------------|-------------|-------------|-------------|---------------------|

- epoll_evnet
typedef union epoll_data {
    void     *ptr;
    int      fd;
    uint32_t u32;
    uint64_t u64;
} epoll_data_t;

struct epoll_event {
    uint32_t     events;     // Epoll events 비트마스크
    epoll_data_t data;       // 사용자 데이터 변수
};
*/

// void serialize_epoll_event(struct epoll_event *event, struct epoll_event_dt *events) {
// 	/* struct epoll_event -> epoll_event_dt  */
// 	events->events = event->events;
// 	// events->fd = event->data.fd;
// 	// events->u32 = event->data.u32;
// 	events->u64 = event->data.u64;
// 	debugInfoMes("sizeof(epoll_event_dt) = %zu\n", sizeof(struct epoll_event_dt));
// 	debugInfoMes("serialize_epoll_event: events: %u, u64: %lu\n", event->events, event->data.u64);
// 	debugInfoMes("serialize_epoll_event: events: %u, u64: %lu\n", events->events, events->u64);
// }

// void deserialize_epoll_event(struct epoll_event *event, struct epoll_event_dt *events) {
// 	event->events = events->events;
// 	// event->data.fd = events->fd;
// 	// event->data.u32 = events->u32;
// 	event->data.u64 = events->u64;
// 	debugInfoMes("sizeof(epoll_event_dt) = %zu\n", sizeof(struct epoll_event_dt));
// 	debugInfoMes("deserialize_epoll_event: events: %u, u64: %lu\n", event->events, event->data.u64);
// 	debugInfoMes("deserialize_epoll_event: events: %u, u64: %lu\n", events->events, events->u64);
// }

void serialize_epoll_event_struct(char *data, struct epoll_event *event, int num) {
	/* struct epoll_event -->  data  */
	if (data == NULL || event == NULL) {
		errorInfoMes("serialize_epoll_event_struct: data or event is NULL\n");
		return;
	}
	debugInfoMes("serialize_epoll_event_struct called\n");
	

	int offset = 0;
	int sz = sizeof(struct epoll_event);
	for (int i = 0; i < num; i++) {
		memcpy(data + offset, &event[i], sz);
		offset += sz;		// struct epoll_event

		// memcpy(data + offset, &event[i].events, 4);
		// offset += 4;		// uint32_t events; 
		// memcpy(data + offset, &event[i].data.u64, 8);
		// offset += 8;		// uint64_t u64;
		// debugInfoMes("Serialized event %d: events: %u, u64: %lu\n", i, event[i].events, event[i].data.u64);
	}
}
void deserialize_epoll_event_struct(char *data, struct epoll_event *event, int num) {
	/* data --> struct epoll_event */
	if (data == NULL || event == NULL) {
		errorInfoMes("deserialize_epoll_event_struct: data or event is NULL\n");
		return;
	}
	debugInfoMes("deserialize_epoll_event_struct called\n");
	
	int offset = 0;
	int sz = sizeof(struct epoll_event);
	for (int i = 0; i < num; i++) {
		memcpy(&event[i], data + offset, sz);
		offset += sz;		// struct epoll_event

		// memcpy(&event[i].events, data + offset, 4);
		// offset += 4;		// uint32_t events;
		// memcpy(&event[i].data.u64, data + offset, 8);
		// offset += 8;		// uint64_t u64;

		// debugInfoMes("deerialized event %d: events: %u, u64: %lu\n", i, event[i].events, event[i].data.u64);
	}
}

void serialize_epoll_create_dt(char *msg_data, int *flag) {
	if (msg_data == NULL || flag == NULL) {
		errorInfoMes("msg_data or flag is NULL!");
		return;
	}

	debugInfoMes("serialize_epoll_create_dt called\n");

	// Serialize the flag
	memcpy(msg_data, flag, 4);
	debugInfoMes("Serialized flag: %d\n", *flag);
	// warningInfoMes("Serialized flag: %d\n", *flag);
}
void deserialize_epoll_create_dt(char *msg_data, int *flag) {
	if (msg_data == NULL || flag == NULL) {
		errorInfoMes("msg_data or flag is NULL!");
		return;
	}

	debugInfoMes("deserialize_epoll_create_dt called\n");

	// Deserialize the flag
	memcpy(flag, msg_data, 4);
	debugInfoMes("Deserialized flag: %d\n", *flag);
	// warningInfoMes("Deserialized flag: %d\n", *flag);
}

void serialize_epoll_ctl_dt(char *msg_data, int *epfd, int *op, int *fd, struct epoll_event *event) {
	if (msg_data == NULL || epfd == NULL || op == NULL || fd == NULL) {
		errorInfoMes("msg_data or one of the parameters is NULL!");
		if(msg_data == NULL)
			errorInfoMes("msg_data is NULL!");
		if(epfd == NULL)
			errorInfoMes("epfd is NULL!");
		if(op == NULL)
			errorInfoMes("op is NULL!");
		if(fd == NULL)
			errorInfoMes("fd is NULL!");
		return;
	}

	if(event == NULL) {
		if(op != EPOLL_CTL_DEL){
			warningInfoMes("event is NULL! op is %d\n", *op);
		}
		else { // EPOLL_CTL_DEL 일 때는 event가 NULL이어도 됨
			debugInfoMes("event is NULL, but op is EPOLL_CTL_DEL, so continuing...\n");
			warningInfoMes("event is NULL, but op is EPOLL_CTL_DEL, so continuing...\n");
		}
	} else {
		memcpy(msg_data + 12, &event->events, 4);
		memcpy(msg_data + 16, &event->data, 8);
	}

	debugInfoMes("serialize_epoll_ctl_dt called\n");


	// Serialize the parameters
	memcpy(msg_data, epfd, 4);
	memcpy(msg_data + 4, op, 4);
	memcpy(msg_data + 8, fd, 4);
	// memcpy(msg_data + 12, &event->events, 4);
	// memcpy(msg_data + 16, &event->data, 8);

	debugInfoMes("Serialized epfd: %d, op: %d, fd: %d\n", *epfd, *op, *fd);
}
void deserialize_epoll_ctl_dt(char *msg_data, int *epfd, int *op, int *fd, struct epoll_event *event) {
	if (msg_data == NULL || epfd == NULL || op == NULL || fd == NULL || event == NULL) {
		errorInfoMes("msg_data or one of the parameters is NULL!");
		return;
	}
	// debugInfoMes("deserialize_epoll_ctl_dt called\n");

	// Deserialize the parameters
	memcpy(epfd, msg_data, 4);
	memcpy(op, msg_data + 4, 4);
	memcpy(fd, msg_data + 8, 4);
	// memcpy(event, msg_data + 12, sizeof(struct epoll_event));

	if(op == EPOLL_CTL_DEL) {
		debugInfoMes("Setting event to zero for EPOLL_CTL_DEL\n");
		event->events = 0; // EPOLL_CTL_DEL 일 때는 events를 0으로 설정
	} else {
		debugInfoMes("Deserializing event for op: %d\n", *op);
		memcpy(&event->events, msg_data + 12, 4);
		memcpy(&event->data, msg_data + 16, 8);
		
	}
	debugInfoMes("Deserialized epfd: %d, op: %d, fd: %d\n", *epfd, *op, *fd);
}

void serialize_epoll_wait_dt(char *msg_data, int *nfds, int *epfd, int *maxevents, int *timeout, struct epoll_event *event) {
	if (msg_data == NULL || nfds == NULL || epfd == NULL || maxevents == NULL || timeout == NULL) {
		errorInfoMes("msg_data or one of the parameters is NULL!");
		return;
	}

	debugInfoMes("serialize_epoll_wait_dt called\n");

	// Serialize the parameters
	memcpy(msg_data, nfds, 4);
	memcpy(msg_data + 4, epfd, 4);
	memcpy(msg_data + 8, maxevents, 4);
	memcpy(msg_data + 12, timeout, 4);
	
	int offset = 16; // Start after the first 16 bytes
	int sz = sizeof(struct epoll_event);

	if (event == NULL) {
		event = malloc(*nfds * sz);
		if (event == NULL) {
			errorInfoMes("Failed to allocate memory for epoll_event array of size: %d\n", *nfds);
			return;
		}
		debugInfoMes("Allocated memory for epoll_event array of size: %d\n", *nfds);
	} 


	for (int i = 0; i < *nfds; i++) {
		if (offset + sz > 1024) { // Assuming msg_data has a size limit
			errorInfoMes("msg_data buffer overflow, cannot serialize more events\n");
			return;
		}
		// memcpy(msg_data + offset, event + i, sz);
		memcpy(msg_data + offset, &event[i].events, 4);
		memcpy(msg_data + offset + 4, &event[i].data, 8);
		offset += 12;
	}
	debugInfoMes("Serialized nfds: %d, epfd: %d, maxevents: %d, timeout: %d\n", *nfds, *epfd, *maxevents, *timeout);
}

void deserialize_epoll_wait_dt(char *msg_data, int *nfds, int *epfd, int *maxevents, int *timeout, struct epoll_event *event) {
	if (msg_data == NULL || nfds == NULL || epfd == NULL || maxevents == NULL || timeout == NULL) {
		errorInfoMes("msg_data or one of the parameters is NULL!");
		return;
	}

	debugInfoMes("deserialize_epoll_wait_dt called\n");

	// Deserialize the parameters
	memcpy(nfds, msg_data, 4);
	memcpy(epfd, msg_data + 4, 4);
	memcpy(maxevents, msg_data + 8, 4);
	memcpy(timeout, msg_data + 12, 4);

	int offset = 16; // Start after the first 16 bytes
	int sz = sizeof(struct epoll_event);
	if (event != NULL) {
		for (int i = 0; i < *nfds; i++) {
			if (offset + sz > 1024) { // Assuming msg_data has a size limit
				errorInfoMes("msg_data buffer overflow, cannot deserialize more events\n");
				return;
			}
			// memcpy(event + i, msg_data + offset, sz);
			memcpy(&event[i].events, msg_data + offset, 4);
			memcpy(&event[i].data, msg_data + offset + 4, 8);
			offset += 12;
		}
		debugInfoMes("Deserialized nfds: %d, epfd: %d, maxevents: %d, timeout: %d\n", *nfds, *epfd, *maxevents, *timeout);
	} else {
		debugInfoMes("Deserialized nfds: %d, epfd: %d, maxevents: %d, timeout: %d (event is NULL)\n", *nfds, *epfd, *maxevents, *timeout);
	}
}

/*
select 함수 인자 정리 (64bit 시스템 기준)

|---- int ----|---- int ----|---- fd_set ----|---- fd_set ----|---- fd_set ----|---- struct timeval ----|
|	  4B	  |		1B		|		128B	 |		128B	  |		 128B	   |		   16B			|
|    nfds     |		flag    |	  readfds 	 |	  writefds 	  |		exceptfds  |		timeout			|
|-------------|-------------|----------------|----------------|----------------|------------------------|
*/

void serialize_select_dt(struct select_dt *select_data, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout) {

	int sz = sizeof(fd_set);

	if (select_data == NULL) {
		errorInfoMes("msg_data is NULL!");
		return;
	}

	debugInfoMes("serialize_select_fd_set called\n");

	if (readfds == NULL) {
		errorInfoMes("readfds is NULL!");
		memset(select_data->reqdfds, 0, sz);
	} else {
		debugInfoMes("readfds is not NULL, proceeding with serialization\n");
		memcpy(select_data->reqdfds, readfds, sz);
	}

	if (writefds == NULL) {
		errorInfoMes("writefds is NULL!");
		memset(select_data->writefds, 0, sz);
	} else {
		debugInfoMes("writefds is not NULL, proceeding with serialization\n");
		memcpy(select_data->writefds, writefds, sz);
	}

	if (exceptfds == NULL) {
		errorInfoMes("exceptfds is NULL!");
		memset(select_data->exceptfds, 0, sz);
	} else {
		debugInfoMes("exceptfds is not NULL, proceeding with serialization\n");
		memcpy(select_data->exceptfds, exceptfds, sz);
	}
	debugInfoMes("\n");
	if (timeout != NULL) {
		select_data->tv_sec = timeout->tv_sec;
		select_data->tv_usec = timeout->tv_usec;
		// memcpy(select_data->tv_sec, timeout->tv_sec, sizeof(timeout->tv_sec));
		// memcpy(select_data->tv_usec, timeout->tv_usec, sizeof(timeout->tv_usec));
		debugInfoMes("timeout serialized: %ld sec, %ld usec\n", timeout->tv_sec, timeout->tv_usec);
	} else {
		errorInfoMes("timeout is NULL!");
		select_data->tv_sec = 0; // Set default values if timeout is NULL
		select_data->tv_usec = 0;
	}
	debugInfoMes("\n");
}
void deserialize_select_dt(struct select_dt *select_data, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout) {
	int sz = sizeof(fd_set);

	if (select_data == NULL) {
		errorInfoMes("select_data is NULL!");
		return;
	}

	debugInfoMes("deserialize_select_fd_set called\n");

	if (readfds == NULL) {
		errorInfoMes("readfds is NULL!");
	} else {
		debugInfoMes("readfds is not NULL, proceeding with deserialization\n");
		memcpy(readfds, select_data->reqdfds, sz);
	}

	if (writefds == NULL) {
		errorInfoMes("writefds is NULL!");
	} else {
		debugInfoMes("writefds is not NULL, proceeding with deserialization\n");
		memcpy(writefds, select_data->writefds, sz);
	}

	if (exceptfds == NULL) {
		errorInfoMes("exceptfds is NULL!");
	} else {
		debugInfoMes("exceptfds is not NULL, proceeding with deserialization\n");
		memcpy(exceptfds, select_data->exceptfds, sz);
	}

	if (timeout != NULL) {
		timeout->tv_sec = select_data->tv_sec;
		timeout->tv_usec = select_data->tv_usec;
		// memcpy(timeout->tv_sec, select_data->tv_sec, sizeof(select_data->tv_sec));
		// memcpy(timeout->tv_usec, select_data->tv_usec, sizeof(select_data->tv_usec));
		debugInfoMes("timeout deserialized: %ld sec, %ld usec\n", timeout->tv_sec, timeout->tv_usec);
	} else {
		errorInfoMes("timeout is NULL!");
		timeout->tv_sec = 0;
		timeout->tv_usec = 0;
	}
}

void serialize_select_fd_set(char *msg_data, int *nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds, struct timeval *timeout) {
	int sz = sizeof(fd_set);
	int tv_sz = sizeof(struct timeval);

	if (msg_data == NULL) {
		errorInfoMes("msg_data is NULL!");
		return;
	}

	if (nfds != NULL)
		memcpy(msg_data, nfds, sizeof(int));
	debugInfoMes("nfds: %d\n", *nfds);

	int offset = 5;
	int flag = 0;

	if (readfds != NULL) {
		memcpy(msg_data + offset, readfds, sz);
		flag += 1000;

	} else {
		debugInfoMes("readfds is NULL!\n");
		memset(msg_data + offset, 0, sz); 
	}
	offset += sz;

	if (writefds != NULL) {
		memcpy(msg_data + offset, writefds, sz);
		flag += 100;
	} else {
		debugInfoMes("writefds is NULL!\n");
		memset(msg_data + offset, 0, sz); 
	}
	offset += sz;

	if (exceptfds != NULL) {
		memcpy(msg_data + offset, exceptfds, sz);
		flag += 10;
	} else {
		debugInfoMes("exceptfds is NULL!\n");
		memset(msg_data + offset, 0, sz); 
	}
	offset += sz;

	if (timeout != NULL) {
        memcpy(msg_data + offset, timeout, tv_sz);
		flag += 1;
    } else {
        debugInfoMes("timeout is NULL!\n");
		memset(msg_data + offset, 0, tv_sz);
    }

	memcpy(msg_data + 4, &flag, sizeof(int));
	debugInfoMes("flag: %d\n", flag);
}

void deserialize_select_fd_set(char *msg_data, int *nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout) {
	int sz = sizeof(fd_set);
	int tv_sz = sizeof(struct timeval);

	if (msg_data == NULL) {
		errorInfoMes("msg_data is NULL!");
		return;
	}
	debugInfoMes("msg_data is not NULL, proceeding with deserialization\n");

	if (nfds != NULL) {
		// memcpy(&nfds, msg_data, 4);
		memcpy(nfds, msg_data, 4);
	}

	debugInfoMes("Deserialized nfds: %d\n", *nfds);
	int flag = 0;
	memcpy(&flag, msg_data + 4, 4);
	debugInfoMes("Deserialized nfds: %d\n", *nfds);

	if (flag == 0) {
		debugInfoMes("flag is 0, no data to deserialize\n");
		return;
	}

	int offset = 5;


	if (flag / 1000 != 1 || readfds == NULL) {
		debugInfoMes("readfds is NULL!\n");
		readfds = NULL;
	} else {
		debugInfoMes("readfds is not NULL, proceeding with deserialization\n");
		memcpy(readfds, msg_data + offset, sz);
	}
	offset += sz;
	flag -= 1000;

	if (flag / 100 != 1 || writefds == NULL) {
		debugInfoMes("writefds is NULL!\n");
		writefds = NULL;
	} else {
		debugInfoMes("writefds is not NULL, proceeding with deserialization\n");
		memcpy(writefds, msg_data + offset, sz);
	}
	offset += sz;
	flag -= 100;

	if (flag / 10 != 1 || exceptfds == NULL) {
		debugInfoMes("exceptfds is NULL!\n");
		exceptfds = NULL;
	} else {
		debugInfoMes("exceptfds is not NULL, proceeding with deserialization\n");
		memcpy(exceptfds, msg_data + offset, sz);
	}
	offset += sz;
	flag -= 10;

	if (flag != 1 || timeout == NULL) {
		debugInfoMes("timeout is NULL!\n");
		timeout = NULL;
    } else {
		debugInfoMes("timeout deserialized: %ld sec, %ld usec\n", timeout->tv_sec, timeout->tv_usec);
        memcpy(timeout, msg_data + offset, tv_sz);
    }
}

void print_fd_set(fd_set *fds, int max_fd) {
    debugInfoMes("FD_SET: ");
    for (int i = 0; i <= max_fd; ++i) {
        if (FD_ISSET(i, fds)) {
            debugMes("%d ", i);
        }
    }
    debugMes("\n");
}

/*
poll 함수 인자 정리 (64bit 시스템 기준)

|---- int ----|---- nfds_t ----|---- pollfd ----|
|	   4B	  |   	  8B	   |	8B * nfds	|
|	timetout  |      nfds	   |	 pollfd		|
|-------------|----------------|----------------|

typedef nfds_t; //unsigned integer type

*/

void serialize_pollfd(char *msg_data, struct pollfd *fds, nfds_t *nfds) {
	if (msg_data == NULL || fds == NULL || nfds == NULL) 
		return;	
	/* typedef unsigned long int nfds_t; */
	if (sizeof(nfds_t) == 4) {
		uint64_t nfds_64 = (uint64_t)(*nfds);
		memcpy(msg_data, &nfds_64, 8);
	} else {
		memcpy(msg_data, nfds, 8);
	}
	debugInfoMes("Serialized nfds: %zu\n", *nfds);
	/* pollfd */	
	int n_nfds = (int)(*nfds);
	int offset = 8; // Start after nfds
	for (int i = 0; i < n_nfds; ++i) {
		debugInfoMes("\n");
        memcpy(msg_data + offset, &fds[i].fd, 4);
        memcpy(msg_data + offset + 4, &fds[i].events, 2);
        memcpy(msg_data + offset + 6, &fds[i].revents, 2);

		offset += 8;
    }
	debugInfoMes("Serialized %zu pollfd structures\n", *nfds);
}
void deserialize_pollfd(char *msg_data, struct pollfd **fds, nfds_t *nfds){
	if (msg_data == NULL || fds == NULL || nfds == NULL) 
		return;

	/* typedef unsigned long int nfds_t; */
	
	if (sizeof(nfds_t) == 4) {
		uint64_t nfds_64;
		memcpy(&nfds_64, msg_data, 8);
		*nfds = (nfds_t)nfds_64;
	} else {
		memcpy(nfds, msg_data, 8);
	}
	int n_nfds = (int)(*nfds);

	*fds = malloc(n_nfds * sizeof(struct pollfd));
    if (!(*fds)) {
        perror("malloc failed");
        return NULL;
    }

	/* pollfd */
	int offset = 8; // Start after nfds
	for (int i = 0; i < n_nfds; i++) {
		// memcpy(&fds[i].fd,      msg_data + offset, 4);
        // memcpy(&fds[i].events,  msg_data + offset + 4, 2);
        // memcpy(&fds[i].revents, msg_data + offset + 6, 2);

		memcpy(&(*fds)[i].fd,      msg_data + offset, 4);
        memcpy(&(*fds)[i].events,  msg_data + offset + 4, 2);
        memcpy(&(*fds)[i].revents, msg_data + offset + 6, 2);

		offset += 8; // Move offset for fd
    }
}

void serialize_poll_pollfd(char *msg_data, struct pollfd *fds, nfds_t *nfds, int *timeout){
	if (msg_data == NULL || fds == NULL || nfds == NULL) 
		return;	

	if (timeout != NULL) {
		memcpy(msg_data, timeout, 4);
		debugInfoMes("Serialized timeout: %d\n", *timeout);
	}
	memcpy(msg_data + 4, nfds, 8);
	debugInfoMes("Serialized nfds: %zu\n", *nfds);

	int offset = 12; // Start after nfds
	int sz = sizeof(struct pollfd);

	for (nfds_t i = 0; i < *nfds; ++i) {
		memcpy(msg_data + offset, &fds[i], sz);
		offset += sz;
	}
}

void deserialize_poll_pollfd(char *msg_data, struct pollfd **fds, nfds_t *nfds, int *timeout){
	if (msg_data == NULL || fds == NULL || nfds == NULL) 
		return;

	if (timeout != NULL) {
		memcpy(timeout, msg_data, 4);
		debugInfoMes("Deserialized timeout: %d\n", *timeout);
	}
	memcpy(nfds, msg_data + 4, 8);
	debugInfoMes("Deserialized nfds: %zu\n", *nfds);

	if (*fds == NULL) {
		*fds = malloc((*nfds) * sizeof(struct pollfd));
		if (*fds == NULL) {
			perror("malloc failed");
			return;
		}
	}

	int offset = 12; // Start after nfds
	int sz = sizeof(struct pollfd);

	for (nfds_t i = 0; i < *nfds; ++i) {
		memcpy(&((*fds)[i]), msg_data + offset, sz);
		offset += sz;
	}
	debugInfoMes("Deserialized %zu pollfd structures\n", *nfds);
}

void print_poll_revents(struct pollfd *fds, nfds_t nfds) {
    if (fds == NULL || nfds == 0) {
        debugInfoMes("No pollfd data provided.\n");
        return;
    }

    for (nfds_t i = 0; i < nfds; ++i) {
        printf("fd[%lu] = %d, revents = 0x%x: ", i, fds[i].fd, fds[i].revents);

        if (fds[i].revents == 0) {
            debugMes("No event\n");
            continue;
        }

        if (fds[i].revents & POLLIN)  debugMes("POLLIN ");
        if (fds[i].revents & POLLOUT) debugMes("POLLOUT ");
        if (fds[i].revents & POLLERR) debugMes("POLLERR ");
        if (fds[i].revents & POLLHUP) debugMes("POLLHUP ");
        if (fds[i].revents & POLLNVAL) debugMes("POLLNVAL ");
        if (fds[i].revents & POLLPRI) debugMes("POLLPRI ");
#ifdef POLLRDHUP
        if (fds[i].revents & POLLRDHUP) debugMes("POLLRDHUP ");
#endif
        debugMes("\n");
    }
}

/*
fcntl 함수 인자 정리 (64bit 시스템 기준)

|---- int ----|---- int ----|---- int ----|
|	   4B	  |   	 4B		|	  4B	  |
|	   cmd 	  |     flag	|	 		  |
|-------------|-------------|-------------|

|---- int ----|---- int ----|---- struct flock ----|
|	   4B	  |   	 4B		|		32 - 72B	   |
|	   cmd	  |     flag	|					   |
|-------------|-------------|----------------------|

struct flock {
    short l_type;    // 락 종류
    short l_whence;  // 기준 위치 (SEEK_SET 등)
    off_t l_start;   // 시작 오프셋
    off_t l_len;     // 길이
    pid_t l_pid;     // 락을 소유한 프로세스 ID
};

int flag 
0: None
1: int 
2: struct flock
*/

void serialize_fcntl_dt(char *msg_data, int *flag, int *cmd, void *arg) {
	if (msg_data == NULL || cmd == NULL) {
		errorInfoMes("msg_data or one of the parameters is NULL!");
		return;
	}
	debugInfoMes("serialize_fcntl_dt called\n");

    // cmd에 따라 arg 내용을 복사
    switch (*cmd) {
		case F_GETFD:
		case F_GETFL:
			*flag = 0;
			break;

        case F_SETFD:
        case F_SETFL:
        case F_DUPFD:
        // case F_DUPFD_CLOEXEC:
        // case F_SETOWN:
        // case F_SETPIPE_SZ:
            // int형 인자
			*flag = 1;
			if (arg != NULL) {
                memcpy(msg_data + 8, arg, sizeof(int));
            } else {
                errorInfoMes("arg is NULL for int-type fcntl cmd");
            }
            break;

        case F_GETLK:
        case F_SETLK:
        case F_SETLKW:
            // struct flock 포인터인 경우
			*flag = 2;
            if (arg != NULL) {
                memcpy(msg_data + 8, arg, sizeof(struct flock));  // fallback
            } else {
                errorInfoMes("flock and arg are both NULL for flock-type fcntl cmd");
            }
            break;

        // arg 없는 경우는 아무것도 안 함
        default:
			*flag = 0;
            break;
    }
	memcpy(msg_data, cmd, 4); // cmd를 msg_data에 복사
	memcpy(msg_data + 4, flag, 4); // flag를 msg_data에 복사

	debugInfoMes("Serialized cmd: %d, flag: %d\n", *cmd, *flag);
}

void deserialize_fcntl_dt(char *msg_data, int *flag, int *cmd, void *arg) {
    if (msg_data == NULL || cmd == NULL || flag == NULL) {
        errorInfoMes("msg_data, cmd, or arg is NULL!");
        return;
    }

    debugInfoMes("deserialize_fcntl_dt called\n");
	memcpy(cmd, msg_data, 4); // cmd를 msg_data에 복사
	memcpy(flag, msg_data + 4, 4); // flag를 msg_data에 복사


	switch (*flag) {
		case 0:	{	// None
			debugInfoMes("No argument to deserialize for cmd: %d\n", *cmd);
			return; // No argument to deserialize
		}
		case 1: {	// int
			if (arg != NULL) {
				// arg = (int*)malloc(4);
				memcpy(arg, msg_data + 8, sizeof(int));
				debugInfoMes("Deserialized int arg for cmd: %d\n", *cmd);
			} else {
				errorInfoMes("arg is NULL for int-type fcntl");
			}
			break;
		}
		case 2:	{	//struct flock
			if (arg != NULL) {
				int sz = sizeof(struct flock);
				memcpy(arg, msg_data + 8, sizeof(struct flock));
				debugInfoMes("Deserialized struct flock arg for cmd: %d\n", *cmd);
			} else {
				errorInfoMes("arg is NULL for struct flock-type fcntl");
			}
			break;
		}
		default:
			errorInfoMes("Unknown flag value: %d\n", *flag);
			return;
	
	}
    debugInfoMes("Deserialized cmd: %d\n", *cmd);
}

/*
sockopt 함수 인자 정리 (64bit 시스템 기준)

|---- int ----|---- int ----|---- socklen_t ----|---- void* ----|
|	   4B	  |   	 4B		|	  	4B			|				|
|	level	  |	  optname	|	  optlen		|	  optval	|
|-------------|-------------|-------------------|---------------|

*optval*
- unsigned char (1B)
IP_MULTICAST_TTL
- int (4B)
SO_REUSEADDR, SO_KEEPALIVE, SO_BROADCAST, SO_RCVBUF, SO_SNDBUF, TCP_NODELAY
- struct linger (8B)
SO_LINGER
- 	struct ip_mreq (8B)
IP_ADD_MEMBERSHIP

*/

void serialize_sockopt_dt(char *msg_data, int *level, int *optname, const void *optval, socklen_t *optlen) {
	if (msg_data == NULL || level == NULL || optname == NULL || optlen == NULL) {
		errorInfoMes("msg_data or one of the parameters is NULL!");
		return;
	}
	debugInfoMes("serialize_setsockopt called\n");

	memcpy(msg_data, level, 4); // level
	memcpy(msg_data + 4, optname, 4); // optname
	memcpy(msg_data + 8, optlen, 4); // optlen

	if (*optlen > 0) {
		memcpy(msg_data + 12, optval, *optlen); // optval
		debugInfoMes("Serialized optval of size: %d\n", *optlen);
	} else {
		debugInfoMes("No optval to serialize\n");
	}
	debugInfoMes("\n");
}

void deserialize_sockopt_dt(char *msg_data, int *level, int *optname, void **optval, socklen_t *optlen) {
	if (msg_data == NULL || level == NULL || optname == NULL || optlen == NULL) {
		errorInfoMes("msg_data or one of the parameters is NULL!");
		return;
	}
	debugInfoMes("deserialize_setsockopt called\n");

	memcpy(level, msg_data, 4); // level
	memcpy(optname, msg_data + 4, 4); // optname
	memcpy(optlen, msg_data + 8, 4); // optlen

	if (*optlen > 0) {
		*optval = malloc(*optlen);
		if (*optval == NULL) {
			errorInfoMes("Failed to allocate memory for optval of size: %d\n", *optlen);
			return;
		}
		memcpy(*optval, msg_data + 12, *optlen); // optval
		debugInfoMes("Deserialized optval of size: %d\n", *optlen);
	} else {
		debugInfoMes("No optval to deserialize\n");
	}
	debugInfoMes("\n");
}

/*
getpeername 함수 인자 정리 (64bit 시스템 기준)

|---- socklen_t ----|---- struct sockaddr ----|
|		4B			|   		16B			  |
|		addrlen		|			addr		  |
|-------------------|-------------------------|

*/

void serialize_addr_dt(char *msg_data, socklen_t *addrlen, struct sockaddr *addr) {
	if (msg_data == NULL || addrlen == NULL || addr == NULL) {
		errorInfoMes("msg_data or one of the parameters is NULL!");
		return;
	}
	debugInfoMes("serialize_getpeername_dt called\n");

	memcpy(msg_data, addrlen, 4); // addrlen
	memcpy(msg_data + 4, addr, sizeof(struct sockaddr)); // addr
	debugInfoMes("Serialized addrlen: %d\n", *addrlen);
}

void deserialize_addr_dt(char *msg_data, socklen_t *addrlen, struct sockaddr *addr) {
	if (msg_data == NULL || addrlen == NULL || addr == NULL) {
		errorInfoMes("msg_data or one of the parameters is NULL!");
		return;
	}
	debugInfoMes("deserialize_getpeername_dt called\n");

	memcpy(addrlen, msg_data, 4); // addrlen
	memcpy(addr, msg_data + 4, sizeof(struct sockaddr)); // addr
	debugInfoMes("Deserialized addrlen: %d\n", *addrlen);
}

void print_req_rdma(int req_rdma) {
	switch (req_rdma) {
		case RDMA_REQ_SOCKET:
			debugInfoMes("Request RDMA: SOCKET\n");
			break;
		case RDMA_REQ_CLOSE:
			debugInfoMes("Request RDMA: CLOSE\n");
			break;
		case RDMA_REQ_READ:
			debugInfoMes("Request RDMA: READ\n");
			break;
		case RDMA_REQ_WRITE:
			debugInfoMes("Request RDMA: WRITE\n");
			break;
		case RDMA_REQ_BIND:
			debugInfoMes("Request RDMA: BIND\n");
			break;
		case RDMA_REQ_LISTEN:
			debugInfoMes("Request RDMA: LISTEN\n");
			break;
		case RDMA_REQ_ACCEPT:
			debugInfoMes("Request RDMA: ACCEPT\n");
			break;
		case RDMA_REQ_CONNECT:
			debugInfoMes("Request RDMA: CONNECT\n");
			break;
		case RDMA_REQ_SELECT:
			debugInfoMes("Request RDMA: SELECT\n");
			break;
		case RDMA_REQ_EPOLL_CREATE1:
			debugInfoMes("Request RDMA: EPOLL_CREATE1\n");
			break;
		case RDMA_REQ_EPOLL_CTL:
			debugInfoMes("Request RDMA: EPOLL_CTL\n");
			break;
		case RDMA_REQ_EPOLL_WAIT:
			debugInfoMes("Request RDMA: EPOLL_WAIT\n");
			break;
		case RDMA_REQ_POLL:
			debugInfoMes("Request RDMA: POLL\n");
			break;
		case RDMA_REQ_FCNTL:
			debugInfoMes("Request RDMA: FCNTL\n");
			break;
		case RDMA_REQ_SETSOCKOPT:
			debugInfoMes("Request RDMA: SETSOCKOPT\n");
			break;
		case RDMA_REQ_GETSOCKOPT:
			debugInfoMes("Request RDMA: GETSOCKOPT\n");
			break;
		case RDMA_REQ_GETPEERNAME:
			debugInfoMes("Request RDMA: GETPEERNAME\n");
			break;
		case RDMA_REQ_GETSOCKNAME:
			debugInfoMes("Request RDMA: GETSOCKNAME\n");
			break;
		case RDMA_REQ_NEW_RDMA_CONN:
			debugInfoMes("Request RDMA: NEW_RDMA_CONN\n");
			break;
		case RDMA_REQ_DEL_RDMA_CONN:
			debugInfoMes("Request RDMA: CLOSE_RDMA_CONN\n");
			break;
		default:
			debugInfoMes("Request RDMA: UNKNOWN (%d)\n", req_rdma);
			break;
	}
}

void print_qp_state(struct ibv_qp *qp) {

	warningInfoMes("print_qp_state called\n");
	if(qp == NULL) {
		errorInfoMes("qp is NULL!");
		return;
	}
	warningInfoMes("qp is not NULL, proceeding to query QP state\n");
    struct ibv_qp_attr attr;
    int ret = ibv_query_qp(qp, &attr, IBV_QP_STATE, NULL);
    if (ret) {
        errorInfoMes("ibv_query_qp failed (%d)", ret);
        return;
    }

    const char *state_str[] = {
        "RESET", "INIT", "RTR", "RTS", "SQD", "SQE", "ERR", "UNKNOWN"
    };
    int idx = (attr.qp_state <= 6) ? attr.qp_state : 7;
    debugInfoMes("QP state = %s (%d)\n", state_str[idx], attr.qp_state);
}