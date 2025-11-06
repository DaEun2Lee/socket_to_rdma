#ifndef CONFIG_H
#define CONFIG_H

#define ERROR_ON 1      //TRUE
#define WARNING_ON 1    //TRUE
#define DEBUG_ON 1      //TRUE

#define _USE_IPV4

#define RDMA_T_1 1

/* MAX SGE capacity */
#define MAX_SGE (8)
/* MAX work requests */
#define MAX_WR (1024)
/* Capacity of the completion queue (CQ) */
#define CQ_CAPACITY (8*1024)
/* Default port where the RDMA server is listening */
#define DEFAULT_RDMA_PORT (12345)
#define DEFAULT_RDMA_IP "127.0.0.1"
#define DEFAULT_RDMA_SERVER_IP "10.0.10.2"
#define DEFAULT_RDMA_CLIENT_IP "10.0.10.22"

#define HT_SZ 1024
#define HT_SEED 4426
#define FD_SET_SZ 1024
#define TIMEOUT_IN_MS 2000

/* Print Macro */
#define COLOR_RED     "\033[1;31m"
#define COLOR_YELLOW  "\033[1;33m"
#define COLOR_GREEN   "\033[1;32m"
#define COLOR_BLUE    "\033[1;34m"
#define COLOR_RESET   "\033[0m"

/* Error Macro */
#if ERROR_ON
    #define errorInfoMes(msg, ...) do {\
        fprintf(stderr, COLOR_RED "[%s %s %d] ERROR : " msg COLOR_RESET "\n", \
        __FILE__, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
    }while(0);
#else
    #define errorInfoMes(msg, ...)
#endif

/* Warning Macro */
#if WARNING_ON
    #define warningInfoMes(msg, ...) do { \
        fprintf(stderr, COLOR_BLUE "[%s %s %d] WARNING : " msg COLOR_RESET "\n", \
        __FILE__, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
    } while(0)
#else
    #define warningInfoMes(msg, ...)
#endif

/* Debug Macro */
#if DEBUG_ON
    //base printf
    #define debugMes(fmt, ...) printf(fmt, ##__VA_ARGS__)
    // printf include info
    #define debugInfoMes(fmt, ...)    printf("[%s %s %d] " fmt, __FILE__, __FUNCTION__, __LINE__, ##__VA_ARGS__)
#else
    #define debugMes(fmt, ...)
    #define debugInfoMes(fmt, ...)
#endif

#endif // CONFIG_H
