CC = gcc
CFLAGS = -Wall -O
LDFLAGS = -lpthread -lrdmacm -libverbs

SRCS_COMMON = rdma_common.c hash_delee.c
OBJS_COMMON = $(SRCS_COMMON:.c=.o)

SRCS_SERVER_ONLY = socket.c
OBJS_SERVER_ONLY = $(SRCS_SERVER_ONLY:.c=.o)

#all: rdma_client rdma_server
all: rdma_server

#rdma_client: rdma_client.o $(OBJS_COMMON)
#	$(CC) -o $@ $^ $(LDFLAGS)

rdma_server: rdma_server.o rdma_server_main.o $(OBJS_COMMON) $(OBJS_SERVER_ONLY)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f *.o rdma_client rdma_server
