#define _GNU_SOURCE
#define _POSIX_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <stdarg.h>
#include "rdma_client.h"
#include "rdma_common.h"
#include "hash_delee.h"
#include "config.h"


// 원래 함수에 대한 포인터
static int (*original_socket)(int, int, int) = NULL;
static int (*original_close)(int) = NULL;
static ssize_t (*original_recv)(int, void *, size_t, int) = NULL;
static ssize_t (*original_read)(int, void *, ssize_t) = NULL;
static ssize_t (*original_write)(int, const void*, ssize_t) = NULL;
static ssize_t (*original_send)(int, const void *, size_t, int) = NULL;
// for server
static int (*original_bind)(int, const struct sockaddr *, socklen_t) = NULL;
static int (*original_listen)(int, int) = NULL;
static int (*original_accept)(int, struct sockaddr *, socklen_t *) = NULL;
//for client
static int (*original_connect)(int, const struct sockaddr *, socklen_t) = NULL;

static int (*original_select)(int, fd_set *, fd_set *, fd_set *, struct timeval *) = NULL;
/* epoll */
static int (*original_epoll_create1)(int flags) = NULL;
static int (*real_epoll_create)(int size) = NULL;
static int (*original_epoll_ctl)(int epfd, int op, int fd, struct epoll_event *event) = NULL;
static int (*original_epoll_wait)(int epfd, struct epoll_event *events, int maxevents, int timeout) = NULL;

static int (*original_poll)(struct pollfd *fds, nfds_t nfds, int timeout) = NULL;

static int (*original_fcntl)(int, int, ...) = NULL;
static int (*original_setsockopt)(int, int, int, const void *, socklen_t);
static int (*original_getsockopt)(int, int, int, void *, socklen_t *);
static int (*original_getpeername)(int, struct sockaddr *, socklen_t *);
static int (*original_getsockname)(int, struct sockaddr *, socklen_t *) = NULL;

/* thread */
typedef void* (*start_routine_t)(void*);
// static int (*original_pthread_create)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *) = NULL;
static void (*original_pthread_exit)(void *) = NULL;

// 초기화 매크로
#define INIT_ORIGINAL_FUNC(name) do { \
    if (!original_##name) { \
        original_##name = dlsym(RTLD_NEXT, #name); \
        if (!original_##name) { \
            fprintf(stderr, "Error loading original " #name ": %s\n", dlerror()); \
            exit(1); \
        } \
    } \
} while(0)


/* Trampoline for the real main() */
typedef int (*libc_start_main_t)(int (*main)(int, char **, char **),
                                 int, char **,
                                 void (*init)(),
                                 void (*fini)(),
                                 void (*rtld_fini)(),
                                 void *);
static libc_start_main_t original_libc_start_main = NULL;
static int (*original_main)(int, char **, char **) = NULL;

struct HashTable* ht;
struct FDHashTable* fd_ht;

struct global_info_struct {
	struct client_r_info *r_info;
	pthread_rwlock_t rwlock;
} global_info_struct;

struct global_info_struct *global_info;

static pthread_key_t r_info_key;
static pthread_once_t key_once = PTHREAD_ONCE_INIT;

static void make_key() {
    pthread_key_create(&r_info_key, free); // 스레드 종료 시 free(new_r_info)
}

static struct client_r_info* get_r_info() {
	
	struct client_r_info* r_info = pthread_getspecific(r_info_key);

	if (!r_info) {
		pthread_rwlock_wrlock(&global_info->rwlock);
		pid_t tid = syscall(SYS_gettid);
		errorInfoMes("get_r_info: Thread %d has no RDMA info — creating...\n", tid);
		warningInfoMes("No RDMA info found for this Thread %d\n", tid);

		/* Create & Connect RDMA connection */
		int ret = set_req_sock(global_info->r_info, 0, RDMA_REQ_NEW_RDMA_CONN, 0, 0, NULL, &errno, NULL);
		
		if(ret < 1) {
			errorInfoMes("set_req_sock failed for %d, ret = %d\n", tid, ret);
			pthread_rwlock_unlock(&global_info->rwlock);
			return NULL;
		}
        debugInfoMes("[HOOK] tid is %d, set_req_sock ret = %d\n", tid, ret);

		r_info = malloc(sizeof(struct client_r_info));
		if (!r_info) {
			errorInfoMes("malloc new_r_info failed\n");
			pthread_rwlock_unlock(&global_info->rwlock);
			return NULL;
		}
		null_client_r_info(r_info);

		int ret_ = connect_server(r_info);

		errorInfoMes("connect_server called for tid %d. connect_server ret is \n", tid, ret);
		if (ret_) {
			errorInfoMes("Error in connect_server for %d\n", tid);
			free(r_info);
			pthread_rwlock_unlock(&global_info->rwlock);
			return NULL;
		} else {
			// 현재 스레드에 TLS 저장
			pthread_setspecific(r_info_key, r_info);                                                                                                                     
		}
		pthread_rwlock_unlock(&global_info->rwlock);
	}
	return r_info;
}


/* Our fake main() that gets called by __libc_start_main() */
int my_main(int argc, char **argv, char **envp) {
	debugInfoMes("[HOOK] main is called!\n");
	int ret = 0;

	pthread_once(&key_once, make_key);
	fd_ht = createFDHashTable(HT_SZ);
	pid_t tid = syscall(SYS_gettid);
    debugInfoMes("[HOOK] Thread %d is creating a new rdma_conn...\n", tid);

	set_sockaddr();

	/* Create & Connect RDMA connection */

	global_info = malloc(sizeof(struct global_info_struct));
	pthread_rwlock_init(&global_info->rwlock, NULL);
	global_info->r_info = malloc(sizeof(struct client_r_info));
	null_client_r_info(global_info->r_info);
	if (!global_info->r_info) {
		errorInfoMes("malloc global_info failed\n");
		return -1;
	}

	ret = connect_server(global_info->r_info);
	if (ret) {
		errorInfoMes("Error in connect_server for global_info\n");
		free(global_info);
		return -1;
	}

	debugInfoMes("Finish rdma_sock_linker_init\n");
	ret = original_main(argc, argv, envp);
	debugInfoMes("[Hooked] main is finished!\n");

	/* Destory RDMA conenction */
	struct client_r_info *r_info = get_r_info();
	int ret_ = set_req_sock(r_info, 0, RDMA_REQ_DEL_RDMA_CONN, 0, 0, NULL, &errno, NULL);
	client_disconnect_and_clean(r_info);

	pthread_rwlock_destroy(&global_info->rwlock);
	return 0;
}

// 후킹된 __libc_start_main 함수
int __libc_start_main(int (*main)(int, char**, char**),
                                 int argc, char** argv,
                                 void (*init)(),

                                     void (*fini)(),
                                 void (*rtld_fini)(),
                                 void* stack_end)
{
    debugInfoMes("[Hooked] __libc_start_main is intercepted!\n");

    // 원래 __libc_start_main 함수 가져오기
    if (!original_libc_start_main) {
        original_libc_start_main = (libc_start_main_t)dlsym(RTLD_NEXT, "__libc_start_main");
        if (!original_libc_start_main) {
            fprintf(stderr, "Error: Failed to find original __libc_start_main!\n");
            _exit(1);
        }
    }

    // 원래 main을 저장 후 후킹된 main 설정
    original_main = main;

    // 후킹된 main을 __libc_start_main에 전달
    return original_libc_start_main(my_main, argc, argv, init, fini, rtld_fini, stack_end);
}

int is_called_from_rdma_client() {
	void *buffer[10];
	char **functions;
	int frames, i;

	frames = backtrace(buffer, 10);
	functions = backtrace_symbols(buffer, frames);
	int result = 0;

	for (i = 0; i < frames; i++) {
		debugInfoMes("[%d]rdma_create_event_channel is called from %s\n", i, functions[i]);
		if (strstr(functions[i], "librdma_client") != NULL) {
			debugInfoMes("librdma_client is called from %s\n", functions[i]);
			result = 1; // 해당 함수로부터 호출됨
			break;
		}
		if (strstr(functions[i], "rda_rdma") != NULL) {
			debugInfoMes("rda_rdma is called from %s\n", functions[i]);
			result = 1; // 해당 함수로부터 호출됨
			break;
		}
		if (strstr(functions[i], "libibverbs") != NULL) {
			debugInfoMes("libibverbs is called from %s\n", functions[i]);
			result = 1; // 해당 함수로부터 호출됨
			break;
		}
		if (strstr(functions[i], "client_prepare_connection") != NULL) {
			debugInfoMes("client_prepare_connection is called from %s\n", functions[i]);
			result = 1; // 해당 함수로부터 호출됨
			break;
		}
	}
	
	free(functions);
    return result;
}

/* req_ func*/
int req_sock (int domain, int type, int protocol) {
	debugInfoMes("Hooked socket called, executing set_req_sock\n");
	struct client_r_info* r_info = get_r_info();

	int sockfd = set_req_sock(r_info, 0, RDMA_REQ_SOCKET, 0, 0, NULL, &errno, NULL);
	warningInfoMes("sockfd = %d, r_info = %p\n", sockfd, r_info);

	if (sockfd <= 0) {
		errorInfoMes("set_req_sock failed ret=%d\n", sockfd);
		return -1; // or handle gracefully
	}
	// if ret encodes sockfd, ensure > 2 (0/1/2 are std fds)
	if (sockfd <= 2) {
		errorInfoMes("set_req_sock returned suspicious fd %d\n", sockfd);
		return -1;
	}

	if(sockfd > 0){
		pid_t tid = syscall(SYS_gettid);
		bool ret = fd_insertByValue(fd_ht, sockfd, tid);
		debugInfoMes("insertByValue called for sockfd %d, tid %d\n", sockfd, tid);
		if (!ret) {
			errorInfoMes("insertByValue failed for sockfd %d\n", sockfd);
			free(r_info);
			return -1;
		}
	} 

	debugInfoMes("insertByValue returned for sockfd %d\n", sockfd);

	return sockfd;
}


int req_connect (int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
	debugInfoMes("Hooked conenct called! sockfd is %d\n", sockfd);
	int tid = searchByKey(fd_ht, sockfd);
	if(tid == NULL) {
		debugInfoMes("h_sockfd is wrong ! Input_sokcfd is %d\n", sockfd);
		warningInfoMes("sockfd %d not found in fd_ht, calling original connect\n", sockfd);
		INIT_ORIGINAL_FUNC(connect);
		return original_connect(sockfd, addr, addrlen);
	}
	debugInfoMes("tid is %d\n", tid);
	struct client_r_info* r_info = get_r_info();
	warningInfoMes("tid is %d, r_info is %p\n", tid, r_info);

	int port;
	if (addr->sa_family == AF_INET) {
		// IPv4
		struct sockaddr_in *addr_in = (struct sockaddr_in *)addr;
		port = ntohs(addr_in->sin_port);
	} else if (addr->sa_family == AF_INET6) {
		// IPv6
		struct sockaddr_in6 *addr_in6 = (struct sockaddr_in6 *)addr;
		port = ntohs(addr_in6->sin6_port);
	} else {
		port = 0;
	}

	warningInfoMes("Connecting to port %d\n", port);
	int ret = set_req_sock(r_info, sockfd, RDMA_REQ_CONNECT, 0, port, NULL, &errno, NULL);

	warningInfoMes("ret is %d, sockfd is %d, port is %d\n", ret, sockfd, port);
	// sleep(1); // for safety
	usleep(500);

	ret = fd_insertByValue(fd_ht, sockfd, tid);
		if (!ret) {
			errorInfoMes("insertByValue failed for sockfd %d\n", sockfd);
			free(r_info);
			return -1;
		}
	return ret;
}

ssize_t req_send (int socket, const void *buffer_, size_t length, int flags) {
	debugInfoMes("Hooked send called! sockfd is %d\n", socket);
	int tid = searchByKey(fd_ht, socket);
	if(tid == NULL) {
		debugInfoMes("h_sockfd is wrong ! Input_sokcfd is %d\n", socket);
		INIT_ORIGINAL_FUNC(send);
		return original_send(socket, buffer_, length, flags);
	}
	debugInfoMes("tid is %d\n", tid);
	struct client_r_info* r_info = get_r_info();

	warningInfoMes("rdma send called for sockfd %d, r_info %p\n", socket, r_info);
	int ret = set_req_sock(r_info, socket, RDMA_REQ_WRITE, length, 0, buffer_, &errno, NULL);
	memset(r_info->req.data, 0, BUFFER_SIZE);
	debugInfoMes("return value is %d\n", ret);
	return ret;
}

ssize_t req_read(int fs, void *buf, size_t N){
	debugInfoMes("Hooked read called!sockfd is %d\n", fs);

	int tid = searchByKey(fd_ht, fs);
	if(tid == NULL) {
		debugInfoMes("h_sockfd is wrong ! Input_sokcfd is %d\n", fs);
		INIT_ORIGINAL_FUNC(read);
		return original_read(fs, buf, N);
	}
	debugInfoMes("tid is %d\n", tid);
	struct client_r_info* r_info = get_r_info();

	debugInfoMes("N is %d and buf is %p\n", N, buf);
	int ret = set_req_sock(r_info, fs, RDMA_REQ_READ, N, 0, buf, &errno, buf);

	debugInfoMes("return value is %d\n", ret);
	return ret;
}

ssize_t req_write (int fs, const void *buf, size_t N) {
	debugInfoMes("Hooked write called!sockfd is %d\n", fs);

	int tid = fd_searchByKey(fd_ht, fs);
	if(tid == NULL) {
		debugInfoMes("h_sockfd is wrong ! Input_sokcfd is %d\n", fs);
		INIT_ORIGINAL_FUNC(write);
		return original_write(fs, buf, N);
	}
	debugInfoMes("tid is %d\n", tid);
	struct client_r_info* r_info = get_r_info();

	debugInfoMes("N is %d and buf is %p\n", N, buf);
	int ret = set_req_sock(r_info, fs, RDMA_REQ_WRITE, N, 0, buf, &errno, NULL);
	memset(r_info->req.data, 0, BUFFER_SIZE);
	debugInfoMes("return value is %d\n", ret);

	return ret;
}

ssize_t req_recv (int fs, void *buf, size_t length, int flags) {
	debugInfoMes("Hooked recv called! sockfd is %d\n", fs);

	int tid = searchByKey(fd_ht, fs);
	if(tid == NULL) {
		debugInfoMes("h_sockfd is wrong ! Input_sokcfd is %d\n", fs);
		INIT_ORIGINAL_FUNC(recv);
		return original_recv(fs, buf, length, flags);
	}
	debugInfoMes("tid is %d\n", tid);
	struct client_r_info* r_info = get_r_info();
	int ret = set_req_sock(r_info, fs, RDMA_REQ_READ, length, 0, NULL, &errno, buf);

	debugInfoMes("return value is %d\n", ret);
	return ret;		// returns the number of bytes actually read(BUFFER_SIZE)
}


int req_close (int socket) {
	debugInfoMes("Hooked close called! sockfd is %d\n", socket);
	int tid = searchByKey(fd_ht, socket);
	if(tid == NULL) {
		debugInfoMes("h_sockfd is wrong ! Input_sokcfd is %d\n", socket);
		INIT_ORIGINAL_FUNC(close);
		return original_close(socket);
	}
	debugInfoMes("tid is %d\n", tid);
	struct client_r_info* r_info = get_r_info();

	usleep(500);; // to avoid segfault

	int ret = set_req_sock(r_info, socket, RDMA_REQ_CLOSE, 0, 0, NULL, &errno, NULL);
	debugInfoMes("return value is %d\n", ret);

	fd_deleteByKey(fd_ht, socket);

	return ret;
}

int req_bind (int socket, const struct sockaddr *address, socklen_t address_len) {
	debugInfoMes("Hooked bind called! sockfd is %d\n", socket);

	int tid = fd_searchByKey(fd_ht, socket);
	if(tid == NULL) {
		debugInfoMes("h_sockfd is wrong ! Input_sokcfd is %d\n", socket);
		INIT_ORIGINAL_FUNC(bind);
		return original_bind(socket, address, address_len);
	}
	debugInfoMes("tid is %d\n", tid);
	struct client_r_info* r_info = get_r_info();

	if(r_info == NULL) {
		if (!original_bind) {
				original_bind = dlsym(RTLD_NEXT, "bind");
		}
		return original_bind(socket, address, address_len);
	}

	debugInfoMes("Hooked bind called! sockfd is %d\n", socket);

	uint16_t port;
	if (address->sa_family == AF_INET) {
		struct sockaddr_in *addr_in = (struct sockaddr_in *)address;
		port = ntohs(addr_in->sin_port); // 네트워크 바이트 오더 -> 호스트 바이트 오더
		debugInfoMes("IPv4 Port: %u\n", port);
	} else if (address->sa_family == AF_INET6) {
		struct sockaddr_in6 *addr_in6 = (struct sockaddr_in6 *)address;
		port = ntohs(addr_in6->sin6_port);
		debugInfoMes("Binding to IPv6 port: %u\n", port);
	} else {
		port = 0;
		debugInfoMes("Unsupported address family: %d\n", address->sa_family);
	}

	int ret = set_req_sock(r_info, socket, RDMA_REQ_BIND, 0, 8080, NULL, &errno, NULL);
	debugInfoMes("return value is %d\n", ret);
    return ret;
}

int req_listen (int socket, int backlog) {
	debugInfoMes("Hooked listen called!\n");

	int tid = fd_searchByKey(fd_ht, socket);
	if(tid == NULL) {
		debugInfoMes("h_sockfd is wrong ! Input_sokcfd is %d\n", socket);
		INIT_ORIGINAL_FUNC(listen);
		return original_listen(socket, backlog);
	}
	debugInfoMes("tid is %d\n", tid);
	struct client_r_info* r_info = get_r_info();

	if(r_info == NULL) {
		if (!original_listen) {
			original_listen = dlsym(RTLD_NEXT, "listen");
		}
		return original_listen(socket, backlog);
	}
	debugInfoMes("Hooked listen called!\n");

	int ret = set_req_sock(r_info, socket, RDMA_REQ_LISTEN, 0, 0, NULL, &errno, NULL);
	debugInfoMes("return value is %d\n", ret);
	return ret;
}

int req_accept(int socket, struct sockaddr *address, socklen_t *address_len){
	debugInfoMes("Hooked accept called!\n");

	int tid = fd_searchByKey(fd_ht, socket);
	if(tid == NULL) {
		debugInfoMes("h_sockfd is wrong ! Input_sokcfd is %d\n", socket);
		INIT_ORIGINAL_FUNC(accept);
		return original_accept(socket, address, address_len);
	}
	debugInfoMes("tid is %d\n", tid);
	struct client_r_info* r_info = get_r_info();


	int new_sockfd = set_req_sock(r_info, socket, RDMA_REQ_ACCEPT, 0, 0, NULL, &errno, NULL);
	warningInfoMes("sockfd = %d, r_info = %p\n", new_sockfd, r_info);

	if(new_sockfd > 0){
		pid_t tid = syscall(SYS_gettid);
		bool ret = fd_insertByValue(fd_ht, new_sockfd, tid);
		if (!ret) {
			errorInfoMes("insertByValue failed for sockfd %d\n", new_sockfd);
			free(r_info);
			return -1;
		}
	}

	return new_sockfd;
}

int req_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout) {
	debugInfoMes("Hooked select called with nfds=%d\n", nfds);

	int tid = fd_searchByKey(fd_ht, nfds-1);
	if(tid == NULL) {
		debugInfoMes("h_sockfd is wrong ! Input_sokcfd is %d\n", nfds-1);
		INIT_ORIGINAL_FUNC(select);
		return original_select(nfds, readfds, writefds, exceptfds, timeout);
	}
	debugInfoMes("tid is %d\n", tid);
	struct client_r_info* r_info = get_r_info();

	serialize_select_fd_set(&r_info->req.data, &nfds, readfds, writefds, exceptfds, timeout);
	int ret = set_req_sock(r_info, 0, RDMA_REQ_SELECT, 0, 0, NULL, &errno, NULL);

	if (ret > 0){
		deserialize_select_fd_set(&r_info->rtn.data, &nfds, readfds, writefds, exceptfds, NULL);
	}
	debugInfoMes("return value is %d\nreadfds is %p, writefds is %p, exceptfds is %p\n",  ret, readfds, writefds, exceptfds);
	
	return ret;
}


int req_epoll_create1(int flags) {
	debugInfoMes("Hooked epoll_create1 called!\n");

	struct client_r_info* r_info = get_r_info();
	if(r_info == NULL) {
		INIT_ORIGINAL_FUNC(epoll_create1);
		return original_epoll_create1(flags);
	}
	warningInfoMes("Hooked epoll_create1 called with flags=%d\n", flags);
	debugInfoMes("Hooked epoll_create1 called!\n");

	serialize_epoll_create_dt(&r_info->req.data, &flags);
	int epfd = set_req_sock(r_info, 0, RDMA_REQ_EPOLL_CREATE1, 0, 0, NULL, &errno, NULL);
	warningInfoMes("epfd = %d, r_info = %p\n", epfd, r_info);

	if(epfd > 0){
		pid_t tid = syscall(SYS_gettid);
		bool ret = fd_insertByValue(fd_ht, epfd, tid);
		if (!ret) {
			errorInfoMes("insertByValue failed for sockfd %d\n", epfd);
			free(r_info);
			return -1;
		}
	}

	return epfd;
}

int req_epoll_create(int size) {
	return original_epoll_create1(0);
}


int req_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event) {
	debugInfoMes("Hooked epoll_ctl called with epfd=%d, op=%d, fd=%d\n", epfd, op, fd);

	int tid = fd_searchByKey(fd_ht, epfd);
	if(tid == NULL) {
		debugInfoMes("h_sockfd is wrong ! Input_sokcfd is %d\n", epfd);
		INIT_ORIGINAL_FUNC(epoll_ctl);
		return original_epoll_ctl(epfd, op, fd, event);
	}
	debugInfoMes("tid is %d\n", tid);
	struct client_r_info* r_info = get_r_info();

	warningInfoMes("Hooked epoll_ctl called with epfd=%d, op=%d, fd=%d\n", epfd, op, fd);

	serialize_epoll_ctl_dt(&r_info->req.data, &epfd, &op, &fd, event);

#ifdef DEBUG_ON
	for (int i = 0; i < 12 + sizeof(struct epoll_event); ++i)
    	printf("%02x ", (unsigned char)r_info->req.data[i]);
	printf("\n");
#endif
	int ret = set_req_sock(r_info, epfd, RDMA_REQ_EPOLL_CTL, 0, 0, NULL, &errno, NULL);
	debugInfoMes(" sz of epoll_evnet is %zu\n", sizeof(struct epoll_event));

	return ret;
}

int req_epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout) {
	debugInfoMes("Hooked epoll_wait called with epfd=%d, maxevents=%d, timeout=%d\n", epfd, maxevents, timeout);

	int tid = fd_searchByKey(fd_ht, epfd);
	if(tid == NULL) {
		debugInfoMes("h_sockfd is wrong ! Input_sokcfd is %d\n", epfd);
		INIT_ORIGINAL_FUNC(epoll_wait);
		return original_epoll_wait(epfd, events, maxevents, timeout);
	}
	debugInfoMes("tid is %d\n", tid);
	struct client_r_info* r_info = get_r_info();

	warningInfoMes("Hooked epoll_wait called with epfd=%d, maxevents=%d, timeout=%d\n", epfd, maxevents, timeout);

	int nfds = 0;
	serialize_epoll_wait_dt(&r_info->req.data, &nfds, &epfd, &maxevents, &timeout, &events);
	int ret = set_req_sock(r_info, epfd, RDMA_REQ_EPOLL_WAIT, 0, 0, NULL, &errno, NULL);
	deserialize_epoll_wait_dt(&r_info->rtn.data, &nfds, &epfd, &maxevents, &timeout, events);
	debugInfoMes("sz of epoll_evnet is %zu\n", sizeof(struct epoll_event));
	warningInfoMes("nfds is %d\n", nfds);

#ifdef DEBUG_ON
	for (int i = 0; i < 12 + sizeof(struct epoll_event); ++i)
    	printf("%02x ", (unsigned char)r_info->rtn.data[i]);
	printf("\n");
#endif
	debugInfoMes("req_epoll_wait return value is %d\n", ret);
    return ret;
}

int req_poll(struct pollfd *fds, nfds_t nfds, int timeout) {
	debugInfoMes("Hooked poll called with nfds=%zu, sockfd=%d, timeout=%d\n", nfds, fds[0].fd, timeout);

	int tid = fd_searchByKey(fd_ht, fds[0].fd);
	if(tid == NULL) {
		debugInfoMes("h_sockfd is wrong ! Input_sokcfd is %d\n", fds[0].fd);
		INIT_ORIGINAL_FUNC(poll);
		return original_poll(fds, nfds, timeout);
	}
	debugInfoMes("tid is %d\n", tid);
	struct client_r_info* r_info = get_r_info();

	debugInfoMes("Hooked poll called with nfds=%zu, sockfd=%d, timeout=%d\n", nfds, fds[0].fd, timeout);

	serialize_poll_pollfd(&r_info->req.data, fds, &nfds, &timeout);
	int ret = set_req_sock(r_info, fds[0].fd , RDMA_REQ_POLL, 0, 0, NULL, &errno, NULL);
	if (ret > 0){
		debugInfoMes("req_poll return value is %d\n", ret);
		deserialize_poll_pollfd(&r_info->rtn.data, &fds, &nfds, &timeout);
	} else {
		debugInfoMes("req_poll return value is %d, errno is %d\n", ret, errno);
	}
	return ret;
}

int req_fcntl(int fd, int cmd, ...) {
	debugInfoMes("Hooked fcntl called with fd=%d, cmd=%d\n", fd, cmd);
	// va_arg로 가변 인자를 추출해서 직접 호출해야 함
	va_list args;
	va_start(args, cmd);

	int tid = fd_searchByKey(fd_ht, fd);
	if(tid == NULL) {
		debugInfoMes("h_sockfd is wrong ! Input_sokcfd is %d\n", fd);
		INIT_ORIGINAL_FUNC(fcntl);
		return original_fcntl(fd, cmd, args);
	}
	debugInfoMes("tid is %d\n", tid);
	struct client_r_info* r_info = get_r_info();

	debugInfoMes("Hooked fcntl called with fd=%d, cmd=%d\n", fd, cmd);
	int ret;

	switch (cmd) {
		case F_SETFD:
		case F_SETFL:
		case F_DUPFD:
		case F_DUPFD_CLOEXEC:
		case F_SETOWN:
		case F_SETPIPE_SZ: {
			int arg = va_arg(args, int);
			serialize_fcntl_dt(&r_info->req.data, &fd, &cmd, &arg);
			ret = set_req_sock(r_info, fd, RDMA_REQ_FCNTL, 0, 0, NULL, &errno, NULL);
			break;
		}

		case F_GETLK:
		case F_SETLK:
		case F_SETLKW: {
			struct flock *fl = va_arg(args, struct flock *);
			serialize_fcntl_dt(&r_info->req.data, &fd, &cmd, fl);
			ret = set_req_sock(r_info, fd, RDMA_REQ_FCNTL, 0, 0, NULL, &errno, NULL);
			break;
		}

		default:{
			int arg = va_arg(args, int);
			serialize_fcntl_dt(&r_info->req.data, &fd, &cmd, &arg);
			ret = set_req_sock(r_info, fd, RDMA_REQ_FCNTL, 0, 0, NULL, &errno, NULL);
			break;
		}
	}

	va_end(args);

	debugInfoMes("return value is %d\n", ret);
	return ret;
}

int req_setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen) {
	debugInfoMes("Hooked setsockopt called! sockfd is %d, level is %d, optname is %d\n", sockfd, level, optname);

	int tid = fd_searchByKey(fd_ht, sockfd);
	if(tid == NULL) {
		debugInfoMes("h_sockfd is wrong ! Input_sokcfd is %d\n", sockfd);
		INIT_ORIGINAL_FUNC(setsockopt);
		return original_setsockopt(sockfd, level, optname, optval, optlen);
	}
	debugInfoMes("tid is %d\n", tid);

	struct client_r_info* r_info = get_r_info();
	
	serialize_sockopt_dt(&r_info->req.data, &level, &optname, optval, &optlen);
	int ret = set_req_sock(r_info, sockfd , RDMA_REQ_SETSOCKOPT, 0, 0, NULL, &errno, NULL);

	debugInfoMes("return value is %d\n", ret);
    return ret;
}

int req_getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen) {
	debugInfoMes("Hooked getsockopt called! sockfd is %d, level is %d, optname is %d\n", sockfd, level, optname);

	int tid = fd_searchByKey(fd_ht, sockfd);
	if(tid == NULL) {
		debugInfoMes("h_sockfd is wrong ! Input_sokcfd is %d\n", sockfd);
		INIT_ORIGINAL_FUNC(getsockopt);
		return original_getsockopt(sockfd, level, optname, optval, optlen);
	}
	struct client_r_info* r_info = get_r_info();

	int ret = set_req_sock(r_info, sockfd, RDMA_REQ_GETSOCKOPT, 0, 0, NULL, &errno, NULL);
	deserialize_sockopt_dt(&r_info->rtn.data, &level, &optname, &optval, optlen);
	debugInfoMes("Hooked getsockopt called! sockfd is %d, level is %d, optname is %d\n", sockfd, level, optname);

	debugInfoMes("return value is %d\n", ret);
    return ret;
}

int req_getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
	debugInfoMes("Hooked getpeername called! sockfd is %d\n", sockfd);

	int tid = fd_searchByKey(fd_ht, sockfd);
	if(tid == NULL) {
		debugInfoMes("h_sockfd is wrong ! Input_sokcfd is %d\n", sockfd);
		INIT_ORIGINAL_FUNC(getpeername);
		return original_getpeername(sockfd, addr, addrlen);
	}
	struct client_r_info* r_info = get_r_info();

    serialize_addr_dt(&r_info->req.data, addrlen, addr);
	int ret = set_req_sock(r_info, sockfd, RDMA_REQ_GETSOCKOPT, 0, 0, NULL, &errno, NULL);
	deserialize_addr_dt(&r_info->rtn.data, addrlen, addr);

	debugInfoMes("return value is %d\n", ret);
    return ret;
}


int req_getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
	debugInfoMes("Hooked getsockname called! sockfd is %d\n", sockfd);
	int tid = fd_searchByKey(fd_ht, sockfd);
	if(tid == NULL) {
		debugInfoMes("h_sockfd is wrong ! Input_sokcfd is %d\n", sockfd);
		INIT_ORIGINAL_FUNC(getsockname);
		return original_getsockname(sockfd, addr, addrlen);
	}

	struct client_r_info* r_info = get_r_info();

	serialize_addr_dt(&r_info->req.data, addrlen, addr);
	int ret = set_req_sock(r_info, sockfd, RDMA_REQ_GETSOCKNAME, 0, 0, NULL, &errno, NULL);
	deserialize_addr_dt(&r_info->rtn.data, addrlen, addr);

	debugInfoMes("Hooked getsockname called! sockfd is %d, return value is %d\n", sockfd, ret);
	if (ret == 0) {
		memcpy(addr, &r_info->rtn.data, sizeof(struct sockaddr_in));
		*addrlen = sizeof(struct sockaddr_in);

		struct sockaddr_in *addr_in = (struct sockaddr_in *)addr;
		warningInfoMes("Local address: %s, Port: %d\n",
			inet_ntoa(addr_in->sin_addr),
			ntohs(addr_in->sin_port));

		inet_pton(AF_INET, DEFAULT_RDMA_CLIENT_IP, &addr_in->sin_addr);

		debugInfoMes("addrlen is %d\n", *addrlen);
		warningInfoMes("Local address: %s, Port: %d\n",
			inet_ntoa(addr_in->sin_addr),
			ntohs(addr_in->sin_port));
	}
	return ret;
}

void req_pthread_exit(void *retval) {
	pid_t tid = syscall(SYS_gettid);
    debugInfoMes("[HOOK] Thread %d is creating a new thread...\n", tid);

	struct client_r_info* r_info = get_r_info();
	
	int ret = set_req_sock(r_info, 0, RDMA_REQ_DEL_RDMA_CONN, 0, 0, NULL, &errno, NULL);
	debugInfoMes("Hooked pthread_create called! tid is %d, ret is %d\n", tid, ret);
	if (ret) {
		warningInfoMes("Deleting rdma conn...");
	}
	/* Destory RDMA conenction */
	ret = client_disconnect_and_clean(r_info);
	if(!ret){
		debugInfoMes("success to delete r_info");
	}
	fd_deleteByValue(fd_ht, tid);

	return original_pthread_exit(retval);
}

/* hooking func */
int socket(int domain, int type, int protocol) {
#ifdef LD_PRELOAD
	if (is_called_from_rdma_client()) {
		INIT_ORIGINAL_FUNC(socket);
		return original_socket(domain, type, protocol);
	}
	else {
		return req_sock(domain, type, protocol);
	}
#else
	INIT_ORIGINAL_FUNC(socket);
	return original_socket(domain, type, protocol);
#endif
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
#ifdef LD_PRELOAD
	return req_connect(sockfd, addr, addrlen);
#else
	INIT_ORIGINAL_FUNC(connect);
	return original_connect(sockfd, addr, addrlen);
#endif
}

ssize_t send(int socket, const void *buffer_, size_t length, int flags) {
#ifdef LD_PRELOAD
	return req_send(socket, buffer_, length, flags);
#else
	INIT_ORIGINAL_FUNC(send);
	return original_send(socket, buffer_, length, flags);
#endif
}

ssize_t read(int fs, void *buf, size_t N) {
#ifdef LD_PRELOAD
	if (is_called_from_rdma_client()) {
		INIT_ORIGINAL_FUNC(read);
		return original_read(fs, buf, N);
	}
	else {
		return req_read(fs, buf, N);
	}
#else
	INIT_ORIGINAL_FUNC(read);
	return original_read(fs, buf, N);
#endif
}

ssize_t write(int fs, const void *buf, size_t N) {
#ifdef LD_PRELOAD
	return req_write(fs, buf, N);
#else
	INIT_ORIGINAL_FUNC(write);
	return original_write(fs, buf, N);
#endif
}

ssize_t recv(int fs, void *buf, size_t length, int flags) {
#ifdef LD_PRELOAD
	return req_recv(fs, buf, length, flags);
#else
	INIT_ORIGINAL_FUNC(recv);
	return original_recv(fs, buf, length, flags);
#endif
}

int close(int socket) {
#ifdef LD_PRELOAD
	return req_close(socket);
	// return 0;
#else
	INIT_ORIGINAL_FUNC(close);
	return original_close(socket);
#endif
}

int bind(int socket, const struct sockaddr *address, socklen_t address_len) {
#ifdef LD_PRELOAD
	return req_bind(socket, address, address_len);
#else
	INIT_ORIGINAL_FUNC(bind);
	return original_bind(socket, address, address_len);
#endif
}

int listen(int socket, int backlog) {
#ifdef LD_PRELOAD
	return req_listen(socket, backlog);
#else
	INIT_ORIGINAL_FUNC(listen);
	return original_listen(socket, backlog);
#endif
}

int accept(int socket, struct sockaddr *address, socklen_t *address_len) {
#ifdef LD_PRELOAD
	return req_accept(socket, address, address_len);
#else
	INIT_ORIGINAL_FUNC(accept);
	return original_accept(socket, address, address_len);
#endif
}

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout) {
#ifdef LD_PRELOAD
	return req_select(nfds, readfds, writefds, exceptfds, timeout);
#else
	INIT_ORIGINAL_FUNC(select);
	return original_select(nfds, readfds, writefds, exceptfds, timeout);
#endif
}

/*epoll*/
int epoll_create1(int flags) {
#ifdef LD_PRELOAD
	return req_epoll_create1(flags);
#else
	INIT_ORIGINAL_FUNC(epoll_create1);
	return original_epoll_create1(flags);
#endif
}
int epoll_create(int size) {
	// debugInfoMes("Hooking epoll_create\n");
#ifdef LD_PRELOAD
	return req_epoll_create1(0);
#else
	INIT_ORIGINAL_FUNC(epoll_create1);
	return original_epoll_create1(0);
#endif
}

int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event) {
#ifdef LD_PRELOAD
	return req_epoll_ctl(epfd, op, fd, event);
#else
	INIT_ORIGINAL_FUNC(epoll_ctl);
	return original_epoll_ctl(epfd, op, fd, event);
#endif
}

int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout) {
#ifdef LD_PRELOAD
	return req_epoll_wait(epfd, events, maxevents, timeout);
#else
	INIT_ORIGINAL_FUNC(epoll_wait);
	return original_epoll_wait(epfd, events, maxevents, timeout);
#endif
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
#ifdef LD_PRELOAD
		return req_poll(fds, nfds, timeout);
#else
	INIT_ORIGINAL_FUNC(poll);
	return original_epoll_wait(fds, nfds, timeout);
#endif
}

int fcntl(int fd, int cmd, ...) {
#ifdef LD_PRELOAD
	return req_fcntl(fd, cmd);
#else
	INIT_ORIGINAL_FUNC(fcntl);
	return original_fcntl(fd, cmd);
#endif
}

int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen) {
#ifdef LD_PRELOAD
	return req_setsockopt(sockfd, level, optname, optval, optlen);
#else
	INIT_ORIGINAL_FUNC(setsockopt);
	return original_setsockopt(sockfd, level, optname, optval, optlen);
#endif
}

int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen) {
#ifdef LD_PRELOAD
	return req_getsockopt(sockfd, level, optname, optval, optlen);
#endif
	INIT_ORIGINAL_FUNC(getsockopt);
	return original_getsockopt(sockfd, level, optname, optval, optlen);
}

int getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
#ifdef LD_PRELOAD
	return req_getpeername(sockfd, addr, addrlen);
#else
	INIT_ORIGINAL_FUNC(getpeername);
	return original_getpeername(sockfd, addr, addrlen);
#endif
}

int getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
#ifdef LD_PRELOAD
	return req_getsockname(sockfd, addr, addrlen);
#else
	INIT_ORIGINAL_FUNC(getsockname);
	return original_getsockname(sockfd, addr, addrlen);
#endif
}

// int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg) {
// #ifdef LD_PRELOAD
// 	return req_pthread_create(thread, attr, start_routine, arg);
// #else
// 	INIT_ORIGINAL_FUNC(pthread_create);
// 	return original_pthread_create(thread, attr, start_routine, arg);
// #endif
// }

void pthread_exit(void *retval) {
#ifdef LD_PRELOAD
	return req_pthread_exit(retval);
#else
	INIT_ORIGINAL_FUNC(pthread_exit);
	return original_pthread_exit(retval);
#endif
}