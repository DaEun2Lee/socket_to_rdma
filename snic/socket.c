#include "socket.h"

struct socket_info * socket_info_init(struct socket_info *s_info) {
	s_info = (struct socket_info*)malloc(sizeof(struct socket_info));
	s_info->addrlen = sizeof(s_info->address);
	memset(s_info->buffer, 0, SO_BUFFER_SIZE);

	return s_info;
}

int sock_socket(struct rtn_msg *rtn_msg_) {
	if ((rtn_msg_->rtn_value = socket(AF_INET, SOCK_STREAM, 0)) <= 0) {
		errorInfoMes("socket failed\n");
		rtn_msg_->errno_ = errno;
	}
	warningInfoMes("Socket created with sockfd: %d\n", rtn_msg_->rtn_value);
	return rtn_msg_->rtn_value;
}

bool sock_bind(struct rtn_msg *rtn_msg_, int sockfd, uint16_t port) {

	int ret = 0;
	struct sockaddr_in address;

	/* Setting IP and Port */
#if defined(_USE_IPV6)
	address.sin6_family = AF_INET6;
	address.sin6_port = htons(port);
	address.sin6_addr = in6addr_any;
	rtn_msg_->rtn_value = bind(sockfd, &address, sizeof(struct sockaddr_in6));
#elif defined(_USE_IPV4)
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = INADDR_ANY;
	rtn_msg_->rtn_value = bind(sockfd, &address, sizeof(struct sockaddr_in));
#endif
	if (rtn_msg_->rtn_value < 0) {
		errorInfoMes("bind failed with sockfd(%d)\n", sockfd);
		rtn_msg_->errno_ = errno;
	}
	return rtn_msg_->rtn_value;
}

bool sock_listen(struct rtn_msg *rtn_msg_, int sockfd) {
	/* listen(socket_fd, backlog); */
	rtn_msg_->rtn_value = listen(sockfd, 1024);

	if (rtn_msg_->rtn_value < 0) {
		errorInfoMes("listen failed with sockfd(%d): %s\n", sockfd, strerror(errno));
		rtn_msg_->errno_ = errno;
	}
	return rtn_msg_->rtn_value;
}

int sock_accept(struct rtn_msg *rtn_msg_, int sockfd, uint16_t port) {
	/* Accept connection request from client */
	int ret = 0;
	struct sockaddr_in address;
	socklen_t addrlen;
	/* Setting IP and Port */
#if defined(_USE_IPV6)
	address.sin6_family = AF_INET6;
	address.sin6_port = htons(port);
	address.sin6_addr = in6addr_any;
	addrlen = sizeof(struct sockaddr_in6);
#elif defined(_USE_IPV4)
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = INADDR_ANY;
	addrlen = sizeof(struct sockaddr_in);
#endif
	rtn_msg_->rtn_value = accept(sockfd, &address, addrlen);

	if (rtn_msg_->rtn_value < 0) {
		errorInfoMes("accept failed with sockfd(%d): %s\n", sockfd, strerror(errno));
		rtn_msg_->errno_ = errno;
    }
	return rtn_msg_->rtn_value;
}

void sock_close(struct rtn_msg *rtn_msg_, int sockfd) {
	/* Close Socket */
	rtn_msg_->rtn_value = close(sockfd);

	if (rtn_msg_->rtn_value < 0) {
		errorInfoMes("close failed with sockfd(%d): %s\n", sockfd, strerror(errno));
		rtn_msg_->errno_ = errno;
	}
}

bool sock_connect(struct rtn_msg *rtn_msg_, int sockfd,  uint16_t port) {
	warningInfoMes("sock_connect called with sockfd: %d, port: %d\n", sockfd, port);
	int ret = 0;
	struct sockaddr_in address;
	socklen_t addrlen;
 
	/* Setting IP and Port */
#if defined(_USE_IPV6)
	address.sin6_family = AF_INET6;
	address.sin6_port = htons(port);
	address.sin6_addr = in6addr_any;
	addrlen = sizeof(struct sockaddr_in6);
#elif defined(_USE_IPV4)
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = inet_addr(DEFAULT_RDMA_SERVER_IP);
	addrlen = sizeof(struct sockaddr_in);
#endif
    debugInfoMes("Connecting to IP: %s, Port: %d\n", DEFAULT_RDMA_SERVER_IP, port);
	warningInfoMes("Connecting to IP: %s, Port: %d, sockfd: %d.\n", DEFAULT_RDMA_SERVER_IP, port, sockfd);
	/* Connect to Server */
	rtn_msg_->rtn_value = connect(sockfd, &address, addrlen);

	if (rtn_msg_->rtn_value < 0) {
		errorInfoMes("connect failed with sockfd(%d): %s\n", sockfd, strerror(errno));
		rtn_msg_->errno_ = errno;
		return rtn_msg_->rtn_value;
	}

	warningInfoMes("Successfully connected with sockfd(%d)\n", sockfd);
	return rtn_msg_->rtn_value;
}

void sock_send_message(struct rtn_msg *rtn_msg_, int sockfd, const char *data, int data_sz) {

	/* Send message */
	debugInfoMes("data_sz = %d\n", data_sz);
	rtn_msg_->rtn_value = send(sockfd, data, data_sz, 0);
	// int ret = write(c_info->server_fd, data, data_sz);

	if(rtn_msg_->rtn_value < 0) {
		errorInfoMes("send failed with sockfd(%d): %s\n", sockfd, strerror(errno));
		rtn_msg_->errno_ = errno;
	}
}

int sock_read_message(struct rtn_msg *rtn_msg_, int sockfd, int buf_sz) {

	warningInfoMes("Reading from socket with sockfd: %d\n", sockfd);
	/* Read message */
	rtn_msg_->rtn_value = read(sockfd, rtn_msg_->data, buf_sz);
	debugInfoMes("rtn_msg_->rtn_value is %ld\n", rtn_msg_->rtn_value);

	warningInfoMes("read %ld bytes from socket with sockfd(%d)\n", rtn_msg_->rtn_value, sockfd);

	if (rtn_msg_->rtn_value < 0) {
		rtn_msg_->errno_ = errno;
		errorInfoMes("read failed with sockfd(%d): %s\n", sockfd, strerror(errno));

	} else if (rtn_msg_->rtn_value == 0) {
		errorInfoMes("Socket closed by peer with sockfd(%d).\n", sockfd);
		rtn_msg_->errno_ = errno;

	} else {
		/* Success to read */
		rtn_msg_->errno_ = 0;
	}

	return rtn_msg_->rtn_value;
}
int sock_select(struct req_msg *req_msg_, struct rtn_msg *rtn_msg_)
{
	if (!req_msg_ || !rtn_msg_) return -1;

	// fd_set *readfds = malloc(sizeof(fd_set));
	// fd_set *writefds = malloc(sizeof(fd_set));
	// fd_set *exceptfds = malloc(sizeof(fd_set));
	// if (!readfds || !writefds || !exceptfds) {
	// 	errorInfoMes("malloc failed");
	// 	return -1;
	// }

	fd_set readfds;
	fd_set writefds;
	fd_set exceptfds;

	struct timeval timeout= {0, 0};
	int nfds;

	// deserialize_select_fd_set(&req_msg_->data, &nfds, readfds, writefds, exceptfds, &timeout);
	deserialize_select_fd_set(&req_msg_->data, &nfds, &readfds, &writefds, &exceptfds, &timeout);
	debugInfoMes("sock_poll called with nfds=%d\n, timeout=%d\n", nfds, timeout);

	// int ret = select(nfds, readfds, writefds, exceptfds, &timeout);
	int ret = select(nfds, &readfds, &writefds, &exceptfds, &timeout);
	warningInfoMes("sock_select: ret is %d\n", ret);
	debugInfoMes("sock_select: ret is %d\n", ret);

	rtn_msg_->rtn_value = ret;
	if (ret == -1) {
		errorInfoMes("select failed: %s\n", strerror(errno));
		rtn_msg_->errno_ = errno;
	} else if (ret > 0) {

		// for (int fd = 0; fd < nfds; ++fd) {
		// 	// warningInfoMes("sock_select: fd %d is ready\n", fd);
		// 	if (FD_ISSET(fd, &readfds)) {
		// 		errorInfoMes("fd %d is ready for read\n", fd);
		// 	}
		// 	if (FD_ISSET(fd, &writefds)) {
		// 		errorInfoMes("fd %d is ready to write\n", fd);
		// 	}
		// 	if (FD_ISSET(fd, &exceptfds)) {
		// 		errorInfoMes("fd %d has an exceptional condition\n", fd);
		// 	}
		// }

		// serialize_select_fd_set(&rtn_msg_->data, &nfds, readfds, writefds, exceptfds, &timeout);
		serialize_select_fd_set(&rtn_msg_->data, &nfds, &readfds, &writefds, &exceptfds, &timeout);
	}

	// free(readfds);
	// free(writefds);
	// free(exceptfds);

	return ret;
}

int sock_epoll_create1(struct req_msg *req_msg_, struct rtn_msg *rtn_msg_) {

	int flag = 0;
	deserialize_epoll_create_dt(&req_msg_->data, &flag);
	int epfd = epoll_create1(flag);
	rtn_msg_->rtn_value = epfd;
	if (epfd == -1) {
		errorInfoMes("epoll_create1 failed: %s\n", strerror(errno));
		rtn_msg_->errno_ = errno;
		return -1;
	}
	debugInfoMes("Epoll instance created with epfd: %d\n", epfd);
	return epfd;
}

int sock_epoll_ctl(struct req_msg *req_msg_, struct rtn_msg *rtn_msg_) {
	// deserialize_epoll_event_struct(&req_msg_->data, &events, 1);

	int epfd;
	int op;
	int fd;
	struct epoll_event events;

	// for (int i = 0; i < 12 + sizeof(struct epoll_event); ++i) {
	// 	printf("%02x ", (unsigned char)req_msg_->data[i]);
	// 	printf("\n");
	// }

	deserialize_epoll_ctl_dt(&req_msg_->data, &epfd, &op, &fd, &events);
	debugInfoMes("epfd is %d, op is %d, fd is %d\n", epfd, op, fd);
	warningInfoMes("epfd is %d, op is %d, fd is %d\n", epfd, op, fd);

	if (fcntl(epfd, F_GETFD) == -1)
    	errorInfoMes("epfd %d is invalid: %s\n", epfd, strerror(errno));
	if (fcntl(fd, F_GETFD) == -1)
    	errorInfoMes("fd %d is invalid: %s\n", fd, strerror(errno));

	int ret = epoll_ctl(epfd, op, fd, &events);
	rtn_msg_->rtn_value = ret;
	if (ret == -1) {
		// if (req_msg_->op == EPOLL_CTL_ADD && errno == EEXIST) {
		// 	debugInfoMes("fd %d already registered in epoll %d — skipping ADD\n", req_msg_->fd, req_msg_->epfd);
		// 	rtn_msg_->rtn_value = 0;     // 성공처럼 처리
		// 	rtn_msg_->errno_ = 0;
		// 	return 0;
		// }
		errorInfoMes("epoll_ctl failed: %s\n", strerror(errno));
		rtn_msg_->errno_ = errno;
		return -1;
	}
	// serialize_epoll_ctl_dt(&rtn_msg_->data, &epfd, &op, &fd, &events);

	debugInfoMes("Epoll control operation %d on fd %d succeeded\n", op, fd);
	return 0;
}

int sock_epoll_wait(struct req_msg *req_msg_, struct rtn_msg *rtn_msg_) {
	struct epoll_event events[100]; 
	int nfds, epfd, maxevents, timeout;

	deserialize_epoll_wait_dt(&req_msg_->data, &nfds, &epfd, &maxevents, &timeout, NULL);
	nfds = epoll_wait(epfd, &events, maxevents, timeout);
	rtn_msg_->rtn_value = nfds;
	debugInfoMes("Epoll wait returned %d events\n", nfds);

	if (nfds == -1) {
		errorInfoMes("epoll_wait failed: %s\n", strerror(errno));
		rtn_msg_->errno_ = errno;
		return -1;
	} else {
		debugInfoMes("event_wait succed: %d events\n", nfds);
		for (int i = 0; i < nfds; i++) {
			debugInfoMes("Event %d: events=%u, u64=%lu\n", i, events[i].events, events[i].data.u64);
		}
		serialize_epoll_wait_dt(&rtn_msg_->data, &nfds, &epfd, &maxevents, &timeout, &events);

		warningInfoMes("sz of epoll_evnet is %zu\n", sizeof(struct epoll_event));
		// for (int i = 0; i < 12 + sizeof(struct epoll_event); i++) {
		// 	printf("%02x ", (unsigned char)rtn_msg_->data[i]);
		// }
		// printf("\n");
	}
	return nfds;
}

int sock_poll(struct req_msg *req_msg_, struct rtn_msg *rtn_msg_)
{
	if (req_msg_ == NULL || rtn_msg_ == NULL) {
		errorInfoMes("Request or return message is NULL\n");
		return -1;
	}

	// if(fd <= 0) {
	// 	errorInfoMes("Invalid file descriptor: %d\n", fd);
	// 	rtn_msg_->errno_ = EBADF; // Bad file descriptor
	// 	return -1;
	// }

	int timeout;
	nfds_t nfds;
	struct pollfd *pfd = NULL;
	debugInfoMes("size of pollfd is %zu\n", sizeof(struct pollfd));
	deserialize_poll_pollfd(&req_msg_->data, &pfd, &nfds, &timeout);
	debugInfoMes("sock_poll called with nfds=%d\n, timeout=%d\n", nfds, timeout);

	int ret = poll(pfd, nfds, timeout);
	rtn_msg_->rtn_value = ret;
	warningInfoMes("poll returned %d\n", ret);

	if (ret < 0) {
		errorInfoMes("poll failed: %s\n", strerror(errno));
		rtn_msg_->errno_ = errno;
		// rtn_msg_->rtn_value = -1;
		return ret;
	}
	serialize_poll_pollfd(&rtn_msg_->data, pfd, &nfds, &timeout);
	// print_poll_revents(pfd, nfds);

	return ret;
}

int sock_fcntl (struct req_msg *req_msg_, struct rtn_msg *rtn_msg_, int sockfd) {
	if (req_msg_ == NULL || rtn_msg_ == NULL) {
		errorInfoMes("Request or return message or socket info is NULL\n");
		return -1;
	}

	int flag, cmd, ret;
	void *arg = NULL;

	deserialize_fcntl_dt(&req_msg_->data, &flag, &cmd, &arg);

	if (flag == 0) {
		ret = fcntl(sockfd, cmd);
	} else {
		ret = fcntl(sockfd, cmd, arg);
	}

	rtn_msg_->rtn_value = ret;

	if (ret < 0) {
		errorInfoMes("fcntl failed: %s\n", strerror(errno));
		rtn_msg_->errno_ = errno;
		return -1;
	}
	
	debugInfoMes("fcntl command %d executed successfully on fd %d\n", cmd, sockfd);
	return ret;
}

int sock_setsockopt (struct req_msg *req_msg_, struct rtn_msg *rtn_msg_, int sockfd) {
	
	int level, optname;
	const void *optval;
	socklen_t optlen;

	deserialize_sockopt_dt(&req_msg_->data, &level, &optname, &optval, &optlen);
	int ret = setsockopt(sockfd, level, optname, optval, optlen);
	if (ret < 0) {
		errorInfoMes("setsockopt failed: %s\n", strerror(errno));
		rtn_msg_->errno_ = errno;
		return -1;
	}
	debugInfoMes("setsockopt succeeded on fd %d\n", sockfd);
	return ret;
}

int sock_getsockopt (struct req_msg *req_msg_, struct rtn_msg *rtn_msg_, int sockfd) {
	
	int level, optname;
	void *optval;
	socklen_t optlen;

	deserialize_sockopt_dt(&req_msg_->data, &level, &optname, &optval, &optlen);

	debugInfoMes("sockfd: %d, level: %d, optname: %d, optlen: %zu\n", sockfd, level, optname, optlen);

	int ret = getsockopt(sockfd, level, optname, optval, &optlen);
	if (ret < 0) {
		errorInfoMes("getsockopt failed: %s\n", strerror(errno));
		rtn_msg_->errno_ = errno;
		return -1;
	}
	serialize_sockopt_dt(&rtn_msg_->data, &level, &optname, optval, &optlen);

	debugInfoMes("getsockopt: level=%d, optname=%d, optlen=%zu\n", level, optname, optlen);
	return ret;
}

int sock_getpeername(struct req_msg *req_msg_, struct rtn_msg *rtn_msg_, int sockfd) {

	if (req_msg_ == NULL || rtn_msg_ == NULL) {
		errorInfoMes("Request or return message or socket info is NULL\n");
		return -1;
	}

	struct sockaddr_in peer_addr;
	socklen_t addrlen = sizeof(peer_addr);

	deserialize_addr_dt(&req_msg_->data, &addrlen, (struct sockaddr *)&peer_addr);
	int ret = getpeername(sockfd, (struct sockaddr *)&peer_addr, &addrlen);

	rtn_msg_->rtn_value = ret;
	warningInfoMes("getpeername returned %d\n", ret);
	if (ret < 0) {
		errorInfoMes("getpeername failed: %s\n", strerror(errno));
		rtn_msg_->errno_ = errno;
		return -1;
	}

	serialize_addr_dt(&rtn_msg_->data, &addrlen, (struct sockaddr *)&peer_addr);

	debugInfoMes("Peer address: %s, Port: %d\n", inet_ntoa(peer_addr.sin_addr), ntohs(peer_addr.sin_port));
	return ret;
}

int sock_getsockname(struct req_msg *req_msg_, struct rtn_msg *rtn_msg_, int sockfd) {

	if (req_msg_ == NULL || rtn_msg_ == NULL) {
		errorInfoMes("Request or return message or socket info is NULL\n");
		return -1;
	}

	struct sockaddr_in local_addr;
	socklen_t addrlen = sizeof(local_addr);

	deserialize_addr_dt(&req_msg_->data, &addrlen, (struct sockaddr *)&local_addr);
	int ret = getsockname(sockfd, &local_addr, &addrlen);

	rtn_msg_->rtn_value = ret;
	warningInfoMes("getsockname returned %d\n", ret);
	if (ret < 0) {
		errorInfoMes("getsockname failed: %s\n", strerror(errno));
		rtn_msg_->errno_ = errno;
		return -1;
	}

	serialize_addr_dt(&rtn_msg_->data, &addrlen, (struct sockaddr *)&local_addr);

	warningInfoMes("Local address: %s, Port: %d\n",	inet_ntoa(local_addr.sin_addr), ntohs(local_addr.sin_port));
	debugInfoMes("Socket name retrieved successfully\n");
	return ret;
}
