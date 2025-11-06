/*
 * Header file for the common RDMA routines used in the server/client example program.
 */

#ifndef RDMA_COMMON_H
#define RDMA_COMMON_H


#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>

#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#include "config.h"
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/syscall.h>  
#include <poll.h>
#include <fcntl.h>    // F_GETFD 등 fcntl 플래그 정의
#include <time.h>

// #define BUFFER_SIZE 1048576		// 1MB
#define BUFFER_SIZE 600000	//524288	// 512KB

enum RDMA_REQ_MSG{
    //common
    RDMA_REQ_SOCKET = 1,
    RDMA_REQ_CLOSE = 2,
    RDMA_REQ_READ = 3,
    RDMA_REQ_WRITE = 4,
    //Server
    RDMA_REQ_BIND = 5,
    RDMA_REQ_LISTEN = 6,
    RDMA_REQ_ACCEPT = 7,
    //CLIENT
    RDMA_REQ_CONNECT = 8,
    RDMA_REQ_SELECT = 9,
	//epoll
	RDMA_REQ_EPOLL_CREATE1 = 10,
	RDMA_REQ_EPOLL_CTL = 11,
	RDMA_REQ_EPOLL_WAIT = 12,
	//poll
	RDMA_REQ_POLL = 13,
	
	RDMA_REQ_FCNTL = 14,
	RDMA_REQ_SETSOCKOPT = 15,
	RDMA_REQ_GETSOCKOPT = 16,
	RDMA_REQ_GETPEERNAME = 17,
	RDMA_REQ_GETSOCKNAME = 18,

	// thread
	RDMA_REQ_NEW_RDMA_CONN = 19, 
	RDMA_REQ_DEL_RDMA_CONN = 20, 
};

/*
 * We use attribute so that compiler does not step in and try to pad the structure.
 * We use this structure to exchange information between the server and the client.
 *
 * For details see: http://gcc.gnu.org/onlinedocs/gcc/Type-Attributes.html
 */
struct __attribute((packed)) rdma_buffer_attr {
	uint64_t address;
	uint32_t length;
	union stag {
		/* if we send, we call it local stags */
		uint32_t local_stag;
		/* if we receive, we call it remote stag */
		uint32_t remote_stag;
	}stag;

	uint64_t eventfd_addr;
	uint32_t eventfd_rkey;

};

/*
typedef union epoll_data{
	void *ptr;    
	int fd;    
	__uint32_t u32;    
	__uint64_t u64;
} 

epoll_data_tstruct epoll_event {    
	__uint32_t events;	//Epoll events
	epoll_data_t data;	// User data variable
}
*/
// struct __attribute((packed)) epoll_event_dt {
// 	uint32_t events;
// 	uint64_t u64;
// 	// int	fd;
// 	// uint32_t u32;
// };

/*
int nfds;
typedef struct fd_set {
	__fd_mask __fds_bits[__FD_SETSIZE / __NFDBITS];
} fd_set;

struct timeval {
	time_t tv_sec;		// seconds
	suseconds_t tv_usec;		// microseconds
};
*/
struct __attribute((packed)) select_dt {
	unsigned long reqdfds[16];	//16*64 = 1024 bits
	unsigned long writefds[16];	//16*64 = 1024 bits
	unsigned long exceptfds[16];	//16*64 = 1024 bits
	
	/* timeval*/
	long tv_sec;		// seconds
	long tv_usec;		// microseconds
};

/*
struct pollfd {
	int fd;			// file descriptor
	short events;	// 요구된 이벤트
	short revents;	// 반환된 이벤트
};
*/
struct __attribute((packed)) poll_dt {
	int fd;            // file descriptor
	short events;      // requested events
	short revents;     // returned events

};

/* Data for send/recv */
struct __attribute((packed)) req_msg{
	uint32_t rdma_req;  //4B
	uint64_t buf_sz;    //8B
	uint16_t port;      //2B
	char data[BUFFER_SIZE];

	// int flags;
	// int epfd;
	// int op;
	// int fd;
	// int maxevents;
	// int timeout;
	// struct epoll_event event; // for epoll_ctl
	// struct epoll_event_dt event;
	// struct select_dt select_data; // for select
	// struct poll_dt poll_data; // for poll
};

struct __attribute((packed)) rtn_msg{
	uint32_t errno_;    //4B
	int64_t rtn_value;  //8B
	char data[BUFFER_SIZE];

	// struct epoll_event event; // for epoll_ctl
	// struct epoll_event_dt event;//
	// struct select_dt select_data; // for select
	// struct poll_dt poll_data; // for poll
};

/* resolves a given destination name to sin_addr */
int get_addr(char *dst, struct sockaddr *addr);

/* prints RDMA buffer info structure */
void show_rdma_buffer_attr(struct rdma_buffer_attr *attr);

/*
 * Processes an RDMA connection management (CM) event.
 * @echannel: CM event channel where the event is expected.
 * @expected_event: Expected event type
 * @cm_event: where the event will be stored
 */
int process_rdma_cm_event(struct rdma_event_channel *echannel,
		enum rdma_cm_event_type expected_event,
		struct rdma_cm_event **cm_event);

/* Allocates an RDMA buffer of size 'length' with permission permission. This
 * function will also register the memory and returns a memory region (MR)
 * identifier or NULL on error.
 * @pd: Protection domain where the buffer should be allocated
 * @length: Length of the buffer
 * @permission: OR of IBV_ACCESS_* permissions as defined for the enum ibv_access_flags
 */
struct ibv_mr* rdma_buffer_alloc(struct ibv_pd *pd,
		uint32_t length,
		enum ibv_access_flags permission);

/* Frees a previously allocated RDMA buffer. The buffer must be allocated by
 * calling rdma_buffer_alloc();
 * @mr: RDMA memory region to free
 */
void rdma_buffer_free(struct ibv_mr *mr);

/* This function registers a previously allocated memory. Returns a memory region
 * (MR) identifier or NULL on error.
 * @pd: protection domain where to register memory
 * @addr: Buffer address
 * @length: Length of the buffer
 * @permission: OR of IBV_ACCESS_* permissions as defined for the enum ibv_access_flags
 */
struct ibv_mr *rdma_buffer_register(struct ibv_pd *pd,
		void *addr,
		uint32_t length,
		enum ibv_access_flags permission);
/* Deregisters a previously register memory
 * @mr: Memory region to deregister
 */
void rdma_buffer_deregister(struct ibv_mr *mr);

/* Processes a work completion (WC) notification.
 * @comp_channel: Completion channel where the notifications are expected to arrive
 * @wc: Array where to hold the work completion elements
 * @max_wc: Maximum number of expected work completion (WC) elements. wc must be
 *          atleast this size.
 */
int process_work_completion_events(struct ibv_comp_channel *comp_channel,
		struct ibv_wc *wc,
		int max_wc);

/* prints some details from the cm id */
void show_rdma_cmid(struct rdma_cm_id *id);

void show_req_msg(struct req_msg *msg);
void show_rtn_msg(struct rtn_msg *msg);

// void serialize_epoll_event(struct epoll_event *event, struct epoll_event_dt *events);
// void deserialize_epoll_event(struct epoll_event *event, struct epoll_event_dt *events);
void serialize_epoll_event_struct(char *data, struct epoll_event *event, int num);
void deserialize_epoll_event_struct(char *data, struct epoll_event *event, int num);

void serialize_epoll_create_dt(char *msg_data, int *flag) ;
void deserialize_epoll_create_dt(char *msg_data, int *flag);
void serialize_epoll_ctl_dt(char *msg_data, int *epfd, int *op, int *fd, struct epoll_event *event);
void deserialize_epoll_ctl_dt(char *msg_data, int *epfd, int *op, int *fd, struct epoll_event *event);
void serialize_epoll_wait_dt(char *msg_data, int *nfds, int *epfd, int *maxevents, int *timeout, struct epoll_event *event);
void deserialize_epoll_wait_dt(char *msg_data, int *nfds, int *epfd, int *maxevents, int *timeout, struct epoll_event *event);

void serialize_select_dt(struct select_dt *select_data, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);
void deserialize_select_dt(struct select_dt *select_data, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);

void serialize_select_fd_set(char *msg_data,  int *nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds, struct timeval *timeout);
void deserialize_select_fd_set(char *msg_data, int *nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);
void print_fd_set(fd_set *fds, int max_fd);

void serialize_pollfd(char *msg_data, struct pollfd *fds, nfds_t *nfds);
void deserialize_pollfd(char *msg_data, struct pollfd **fds, nfds_t *nfds);
void serialize_poll_pollfd(char *msg_data, struct pollfd *fds, nfds_t *nfds, int *timeout);
void deserialize_poll_pollfd(char *msg_data, struct pollfd **fds, nfds_t *nfds, int *timeout);
void print_poll_revents(struct pollfd *fds, nfds_t nfds);

void serialize_fcntl_dt(char *msg_data, int *flag, int *cmd, void *arg);
void deserialize_fcntl_dt(char *msg_data, int *flag, int *cmd, void **arg);

void serialize_sockopt_dt(char *msg_data, int *level, int *optname, const void *optval, socklen_t *optlen);
void deserialize_sockopt_dt(char *msg_data, int *level, int *optname, void **optval, socklen_t *optlen);

void serialize_addr_dt(char *msg_data, socklen_t *addrlen, struct sockaddr *addr);
void deserialize_addr_dt(char *msg_data, socklen_t *addrlen, struct sockaddr *addr);


#endif /* RDMA_COMMON_H */
