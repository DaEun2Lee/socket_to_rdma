#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <time.h>
#include <pthread.h>

#define MAX_EVENTS 1024
#define BUFFER_SIZE 524000   // 512KB
#define SERVER_IP "10.0.10.2"
#define SERVER_PORT 3300

// const char *req_path = "Bible_4.2M.txt";
const char *req_path = "index.html";

typedef struct {
    int fd;
    size_t total_received;   // header + body (raw bytes)
    size_t body_received;    // body only (for throughput/size)
    struct timeval start_time;
    struct timeval end_time;
} conn_t;

typedef struct {
    int conn_id;
    int requests_per_conn;
    int use_keep_alive;
    FILE *log_fp;
    pthread_mutex_t *log_mutex;
    // 통계용
    double total_bytes;    // 누적 받은 바이트 (body 기준)
    double total_latency;  // 누적 latency (ms)
    int    total_reqs;     // 처리한 요청 개수
} thread_arg_t;

static void perror_exit(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static inline double timeval_to_ms(const struct timeval *tv) {
    return tv->tv_sec * 1000.0 + tv->tv_usec / 1000.0;
}

static inline void get_time_diff(const struct timeval *start, const struct timeval *end, struct timeval *diff) {
    timersub(end, start, diff);
}

static int connect_to_server() {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) perror_exit("socket");

    struct sockaddr_in serv_addr = {0};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_IP, &serv_addr.sin_addr) != 1) {
        perror_exit("inet_pton");
    }

    // printf("Connecting to server %s:%d with sockfd(%d) ...\n", SERVER_IP, SERVER_PORT, sockfd);
    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0){
        printf("Failed to connect to server %s:%d\n", SERVER_IP, SERVER_PORT);
        sleep(1);
        perror_exit("connect");
    }

    // printf("Connected to server %s:%d with sockfd(%d)\n", SERVER_IP, SERVER_PORT, sockfd);

    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1) perror_exit("fcntl F_GETFL");
    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) perror_exit("fcntl F_SETFL");
    return sockfd;
}

static void send_request(int fd, int use_keep_alive) {
    char request[512];
    int n = snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "Connection: %s\r\n\r\n",
             req_path,
             SERVER_IP,
             use_keep_alive ? "keep-alive" : "close");
    if (n < 0 || n >= (int)sizeof(request)) {
        fprintf(stderr, "request snprintf truncated/failed\n");
        return;
    }

    printf("Before send\n");
    ssize_t sent = send(fd, request, (size_t)n, 0);
    printf("Sent %zd bytes request on fd %d\n", sent, fd);
    if (sent < 0) {
        perror("send");
    }
}

// Content-Length 추출 함수 (공백/대소문자/콜론 뒤 공백 안전)
static ssize_t parse_content_length(const char *response) {
    const char *p = strcasestr(response, "Content-Length:");
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
    p++; // skip ':'
    while (*p == ' ' || *p == '\t') p++;
    char *endptr = NULL;
    long long v = strtoll(p, &endptr, 10);
    if (endptr == p) return -1; // no digits
    if (v < 0) return -1;
    return (ssize_t)v;
}

static void epoll_add(int epfd, int fd) {
    struct epoll_event ev = {0};
    ev.events = EPOLLIN; // level-triggered
    ev.data.fd = fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == -1) perror_exit("epoll_ctl ADD");
}

static void epoll_del(int epfd, int fd) {
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL) == -1) {
        // If already removed/closed, ignore
        if (errno != EBADF && errno != ENOENT) perror("epoll_ctl DEL");
    }
}

static int reconnect_and_register(int epfd, int oldfd) {
    epoll_del(epfd, oldfd);
    close(oldfd);
    printf("reconnecting...\n");
    int nfd = connect_to_server();
    epoll_add(epfd, nfd);
    return nfd;
}

static void *worker_thread(void *arg) {
    // int restart_request = 0;
    thread_arg_t *targ = (thread_arg_t *)arg;

    printf("[%d] Worker thread started, requests_per_conn=%d, use_keep_alive=%d\n",
        targ->conn_id, targ->requests_per_conn, targ->use_keep_alive);

    int epfd = epoll_create1(0);
    if (epfd == -1) perror_exit("epoll_create1");

    conn_t conn = {0};
    conn.fd = connect_to_server();
    epoll_add(epfd, conn.fd);

    for (int i = 0; i < targ->requests_per_conn; ++i) {
        printf("[%d] connection\n",i);
        conn.total_received = 0;
        conn.body_received = 0;
        gettimeofday(&conn.start_time, NULL);
        printf("[%d] Request %d: Sending request...\n", targ->conn_id, i + 1);
        send_request(conn.fd, targ->use_keep_alive);
        printf("[%d] Request %d: Waiting for response...\n", targ->conn_id, i + 1);

        int header_parsed = 0;
        ssize_t expected_len = -1;
        size_t header_size = 0;
        char response_header[8192] = {0};
        size_t header_len = 0;

        int done = 0;
        while (!done) {
            struct epoll_event events[MAX_EVENTS];
            int n = epoll_wait(epfd, events, MAX_EVENTS, 15000); // 15s timeout to prevent infinite hang
            if (n == 0) {
                // timeout: treat as failure and reconnect if needed
                pthread_mutex_lock(targ->log_mutex);
                fprintf(targ->log_fp,
                        "Conn[%d] Req[%d] %s, TIMEOUT after 15s (received: %.2f KB)\n",
                        targ->conn_id, i + 1, req_path, conn.body_received / 1024.0);
                fflush(targ->log_fp);
                pthread_mutex_unlock(targ->log_mutex);

                // printf("n=0/ epoll_wait timeout, reconnecting...\n");
                conn.fd = reconnect_and_register(epfd, conn.fd);
                // restart_request = 1;
                break; // proceed next request
            }
            if (n < 0) {
                if (errno == EINTR) continue;
                perror_exit("epoll_wait");
            }

            for (int j = 0; j < n; ++j) {
                int fd = events[j].data.fd;
                if (fd != conn.fd) continue;

                for (;;) {
                    char buf[BUFFER_SIZE];
                    ssize_t r = recv(fd, buf, sizeof(buf), 0);
                    if (r > 0) {
                        conn.total_received += (size_t)r;

                        if (!header_parsed) {
                            // Accumulate header text (bounded)
                            size_t copy_len = (size_t)r;
                            if (header_len + copy_len >= sizeof(response_header)) {
                                copy_len = sizeof(response_header) - header_len - 1; // keep NUL
                            }
                            if (copy_len > 0) {
                                memcpy(response_header + header_len, buf, copy_len);
                                header_len += copy_len;
                                response_header[header_len] = '\0';
                            }

                            char *header_end = strstr(response_header, "\r\n\r\n");
                            if (header_end) {
                                header_parsed = 1;
                                expected_len = parse_content_length(response_header);
                                header_size = (size_t)((header_end - response_header) + 4);

                                // Compute how many body bytes already included in this recv batch
                                // size_t already = 0;
                                if ((size_t)r > (header_len - (size_t)(response_header + header_size - response_header))) {
                                    // But easier and exact: total bytes read minus header_size overall
                                    size_t hdr_trunc_safe = header_size;
                                    if (conn.total_received >= hdr_trunc_safe) {
                                        conn.body_received = conn.total_received - hdr_trunc_safe;
                                    } else {
                                        conn.body_received = 0; // shouldn't happen
                                    }
                                }
                            }
                        } else {
                            conn.body_received += (size_t)r;
                        }

                        if (header_parsed && expected_len != -1 && conn.body_received >= (size_t)expected_len) {
                            gettimeofday(&conn.end_time, NULL);
                            struct timeval diff2;
                            get_time_diff(&conn.start_time, &conn.end_time, &diff2);

                            double latency_ms = timeval_to_ms(&diff2);
                            double elapsed_sec = diff2.tv_sec + diff2.tv_usec / 1000000.0;
                            double throughput = (conn.body_received / 1024.0) / (elapsed_sec > 0 ? elapsed_sec : 1.0); // KB/s

                            // === 요청 완료 후 로그 기록 ===
                            pthread_mutex_lock(targ->log_mutex);
                            fprintf(targ->log_fp,
                                    "Conn[%d] Req[%d] %s, Latency: %.2f ms, Total: %.2f KB, Throughput: %.2f KB/s\n",
                                    targ->conn_id, i + 1, req_path,
                                    latency_ms, conn.body_received / 1024.0, throughput);
                            fflush(targ->log_fp);
                            pthread_mutex_unlock(targ->log_mutex);
                            // ==============================

                            // === 스레드별 누적 통계 ===
                            targ->total_bytes   += conn.body_received;
                            targ->total_latency += latency_ms;
                            targ->total_reqs    += 1;
                            // ==============================

                            done = 1;

                            if (!targ->use_keep_alive) {
                                // 다음 요청이 더 남아 있을 때만 재연결합니다.
                                if (i + 1 < targ->requests_per_conn) {
                                    printf("Non-keep-alive: Reconnecting for next request (%d/%d)...\n", 
                                        i + 2, targ->requests_per_conn);
                                    conn.fd = reconnect_and_register(epfd, conn.fd);
                                } else {
                                    // 이것이 마지막 요청이었으므로 재연결하지 않습니다.
                                    printf("Non-keep-alive: Last request completed. Not reconnecting.\n");
                                }
                            }
                            break; // leave inner recv loop and continue outer while(done)
                        }
                    } else if (r == 0) {
                        // 서버가 연결을 닫은 경우 → 요청이 끝났다고 간주 (Content-Length 미제공 케이스)
                        gettimeofday(&conn.end_time, NULL);
                        struct timeval diff2;
                        get_time_diff(&conn.start_time, &conn.end_time, &diff2);
                        double latency_ms = timeval_to_ms(&diff2);
                        double elapsed_sec = diff2.tv_sec + diff2.tv_usec / 1000000.0;
                        double throughput = (conn.body_received / 1024.0) / (elapsed_sec > 0 ? elapsed_sec : 1.0);

                        pthread_mutex_lock(targ->log_mutex);
                        fprintf(targ->log_fp,
                                "Conn[%d] Req[%d] %s, Latency: %.2f ms, Total: %.2f KB, Throughput: %.2f KB/s (EOF)\n",
                                targ->conn_id, i + 1, req_path,
                                latency_ms, conn.body_received / 1024.0, throughput);
                        fflush(targ->log_fp);
                        pthread_mutex_unlock(targ->log_mutex);

                        targ->total_bytes   += conn.body_received;
                        targ->total_latency += latency_ms;
                        targ->total_reqs    += 1;

                        // keep-alive라면 서버가 닫았으므로 재연결
                        printf("Server closed connection (EOF). Reconnecting and retrying request...\n");
                        conn.fd = reconnect_and_register(epfd, conn.fd);

                        // 요청 다시 보내기
                        // restart_request = 1;
                        done = 1;
                        break;
                    } else { // r < 0
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // no more data for now
                            break;
                        }
                        if (errno == EINTR) {
                            continue;
                        }
                        // Fatal recv error → reconnect and count as failed
                        perror("recv");
                        printf("else errno recv error, reconnecting...\n");
                        conn.fd = reconnect_and_register(epfd, conn.fd);

                        // 요청 다시 보내기
                        // restart_request = 1;

                        done = 1; // move on to next request
                        break;
                    }
                } // for(;;) recv loop
            } // for events
        } // while !done
        // if (restart_request) {
        //     restart_request = 0;
        //     i--;  // 같은 요청 재시도
        //     continue;
        // }
    }

    epoll_del(epfd, conn.fd);
    close(conn.fd);
    close(epfd);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <requests_per_conn> <keep_alive: 0|1>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int requests_per_conn = atoi(argv[1]);
    int use_keep_alive = atoi(argv[1]);
    int num_conns = 1; // 현재는 단일 연결만 지원

    char log_file_path[64];
    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    strftime(log_file_path, sizeof(log_file_path), "./log_%Y%m%d_%H%M%S.txt", tm_now);

    FILE *log_fp = fopen(log_file_path, "w");
    if (!log_fp) perror_exit("fopen log file");

    printf("Log file: %s\n", log_file_path);

    pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

    pthread_t *threads = (pthread_t *)calloc(1, sizeof(pthread_t));
    thread_arg_t *args = (thread_arg_t *)calloc(1, sizeof(thread_arg_t));
    if (!threads || !args) perror_exit("calloc");

    for (int i = 0; i < num_conns; ++i) {
        args[i].conn_id = i;
        args[i].requests_per_conn = requests_per_conn;
        args[i].use_keep_alive = use_keep_alive;
        args[i].log_fp = log_fp;
        args[i].log_mutex = &log_mutex;
        args[i].total_bytes = 0;
        args[i].total_latency = 0;
        args[i].total_reqs = 0;


        if (pthread_create(&threads[i], NULL, worker_thread, &args[i]) != 0) {
            perror("pthread_create");
            return 1;
        }
        printf("Thread %d started for %d requests (keep-alive=%d)\n",
               i, requests_per_conn, use_keep_alive);
    }

    for (int i = 0; i < num_conns; ++i) {
        pthread_join(threads[i], NULL);
    }

    // === 전체 평균 latency / throughput 계산 ===
    double grand_bytes = 0;
    double grand_latency = 0;
    int grand_reqs = 0;

    for (int i = 0; i < num_conns; ++i) {
        grand_bytes   += args[i].total_bytes;
        grand_latency += args[i].total_latency;
        grand_reqs    += args[i].total_reqs;
    }

    double avg_latency = (grand_reqs > 0) ? (grand_latency / grand_reqs) : 0.0;
    double avg_throughput = 0.0;

    if (grand_reqs > 0 && avg_latency > 0) {
        // 평균 throughput = 평균 한 요청 사이즈 / 평균 latency
        double avg_latency_sec = avg_latency / 1000.0;
        avg_throughput = ((grand_bytes / grand_reqs) / 1024.0) / avg_latency_sec; // KB/s
    }

    fprintf(log_fp,
            "=== Final Average Latency: %.2f ms, Final Average Throughput: %.2f KB/s ===\n",
            avg_latency, avg_throughput);
    printf("Final Avg Latency: %.2f ms, Final Avg Throughput: %.2f KB/s\n",
           avg_latency, avg_throughput);
    // ===========================================

    fclose(log_fp);
    free(threads);
    free(args);
    return 0;
}
