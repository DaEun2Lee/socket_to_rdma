#include "rdma_server.h"

HashTable* rdma_ht;
SocketNodeHashTable* sock_ht;

void rdma_req_event(struct server_r_info* s_info, struct client_r_info* c_info)
{
	struct socket_info sock_info = sock_ht->searchByKey(c_info->sockfd);

	switch (c_info->req_msg.rdma_req) {
		case RDMA_REQ_SOCKET: {
			debugInfoMes("RDMA_REQ_SOCKET\n");
			struct socket_info *new_sock_info = socket_info_init(new_sock_info);

			socket_socket(&c_info->rtn_msg, new_sock_info);

			if (new_sock_info != NULL && c_info->rtn_msg.rtn_value > 1){
				c_info->sockfd = new_sock_info->server_fd;
				sock_ht->insertByValue(new_sock_info->server_fd, new_sock_info);
			}
			send_msg_to_client(c_info);
			break;
		}
		case RDMA_REQ_ACCEPT: {
			debugInfoMes("RDMA_REQ_ACCEPT\n");

			int new_sockfd = socket_connect_request(&c_info->rtn_msg, sock_info);
			if (!new_sockfd) {
				errorInfoMes("Failed to get sockfd. sockfd is %d\n", new_sockfd);
				return;
			}

			// Return value
			send_msg_to_client(c_info);

			struct socket_info *new_sock_info = socket_info_init(new_sock_info);
			new_sock_info->server_fd = new_sockfd;
			struct rdma_info *new_c_info = malloc(sizeof(struct client_r_info));
			null_client_r_info(new_c_info);

			ret = connect_client_r_info(s_info, new_c_info);
			if (!ret) {
				errorInfoMes("Failed to create client ret = %d \n", ret);
				socket_deleteByKey(sock_ht, new_sock_info->server_fd);
				return;
			}
			socket_insertByValue(sock_ht, new_sock_info->server_fd, new_sock_info);
			new_c_info->sockfd = new_sockfd;
			rdma_ht->insertByValue(new_c_info->sockfd, new_c_info);
			break;
		}
		/* For Client-side */
		case RDMA_REQ_CONNECT: {
			debugInfoMes("RDMA_REQ_CONNECT\n");
			/* Is is same case RDMA_REQ_ACCEPT */
		}
		case RDMA_REQ_CLOSE: {
			debugInfoMes("RDMA_REQ_CLOSE\n");

			socket_end(&c_info->rtn_msg, sock_info);

			send_msg_to_client(c_info);
			socket_deleteByKey(sock_ht, new_sock_info->server_fd);
			deleteByKey(rdma_ht, new_c_info->sockfd);
			disconnect_and_cleanup(s_info, new_c_info);
			break;
		}
		case RDMA_REQ_READ: {
			debugInfoMes("RDMA_REQ_READ\n");
			// socket_read_message(&c_info->rtn_msg, sock_info);
			socket_read_message(&c_info->req_msg, &c_info->rtn_msg, sock_info);
			send_msg_to_client(c_info);
			break;
		}
		case RDMA_REQ_WRITE: {
			debugInfoMes("RDMA_REQ_WRITE\n");
			int remaining_sz = r_info->req_msg_.buf_sz;
			int bytes_to_copy = 0;
			int offset = 0;

			while (remaining_sz > 0) {
				bytes_to_copy = (remaining_sz < BUFFER_SIZE) ? remaining_sz : BUFFER_SIZE;

				//rdma -> socket
				socket_send_message(&r_info->req_msg_ + offset, &r_info->rtn_msg_, c_info, bytes_to_copy);

				offset += bytes_to_copy;
				remaining_sz -= bytes_to_copy;
			}
			send_msg_to_client(c_info);
			break;
		}
		case RDMA_REQ_BIND: {
			debugInfoMes("RDMA_REQ_BIND\n");

			if(c_info == NULL)
				errorInfoMes("c_info is NULL\n");

			socket_bind(&c_info->rtn_msg, sock_info, c_info->req_msg.port);
			debugInfoMes("Binding port is %d\n",(uint16_t)c_info->req_msg.port);
			send_msg_to_client(c_info);
			break;
		}
		default:
			debugInfoMes("Unknown RDMA request: %d\n", rdma_req);
			break;
	}
}

