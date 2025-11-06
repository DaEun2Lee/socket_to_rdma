# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -g -fPIC -O -DLD_PRELOAD
SHARED_FLAGS = -shared  
LDFLAGS = -ldl -lrdmacm -libverbs -lpthread 

# Targets
TARGET_LIB_RDMA = librdma_client.so
TARGET_LIB_HOOK = libhook.so
TARGET_BIN_CLIENT = rdma_client

# Source files
SRCS_COMMON = rdma_common.c hash_delee.c
SRCS_RDMA = rdma_client.c $(SRCS_COMMON)
SRCS_HOOK = hook.c
SRCS_CLIENT = rdma_client.c

# Object files
OBJS_COMMON = $(SRCS_COMMON:.c=.o)
OBJS_RDMA = $(SRCS_RDMA:.c=.o)
OBJS_HOOK = $(SRCS_HOOK:.c=.o)
OBJS_CLIENT = $(SRCS_CLIENT:.c=.o)

# Library paths
LIB_PATH = .

# Rules
#all: $(TARGET_LIB_RDMA) $(TARGET_LIB_HOOK) $(TARGET_BIN_CLIENT)
all: $(TARGET_LIB_RDMA) $(TARGET_LIB_HOOK)

# Compile librdma_client.so
$(TARGET_LIB_RDMA): $(OBJS_RDMA)
	$(CC) $(SHARED_FLAGS) -o $@ $^ $(LDFLAGS)

# Compile libhook.so (hooking)
$(TARGET_LIB_HOOK): $(OBJS_HOOK) | $(TARGET_LIB_RDMA)
	$(CC) $(SHARED_FLAGS) -o $@ $(OBJS_HOOK) -L$(LIB_PATH) -lrdma_client $(LDFLAGS)

# Compile rdma_client binary (optional standalone binary, not LD_PRELOAD)
#$(TARGET_BIN_CLIENT): rdma_client.o $(OBJS_COMMON)
#	$(CC) -o $@ $^ $(LDFLAGS)

# Compile .o files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean up
clean:
	rm -f *.o $(TARGET_LIB_RDMA) $(TARGET_LIB_HOOK) $(TARGET_BIN_CLIENT)

# Phony targets
.PHONY: all clean
