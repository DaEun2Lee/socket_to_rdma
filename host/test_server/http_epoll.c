// http_epoll_fixed.c
// Compile: gcc -O2 -pthread -o http_epoll_fixed http_epoll_fixed.c
// Run:     ./http_epoll_fixed 3300 /path/to/docroot

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <netinet/in.h>

#define MAXEVENTS       1024
#define READ_BUF_INIT   (16*1024)
#define WRITE_BUF_INIT  (16*1024)

typedef struct {
    int fd;
    char *rbuf; size_t rcap, rlen;
    char *wbuf; size_t wcap, wlen;
    int keep;
    int ffd;
    off_t foff, flen;
} client_t;

static char *docroot;

static client_t *clients[65536];

static int setnonblocking(int fd){
    int f = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, f | O_NONBLOCK);
}

static void ep_add(int ep, int fd, uint32_t ev){
    struct epoll_event e;
    e.events = ev;
    e.data.fd = fd;
    epoll_ctl(ep, EPOLL_CTL_ADD, fd, &e);
}

static void ep_mod(int ep, int fd, uint32_t ev){
    struct epoll_event e;
    e.events = ev;
    e.data.fd = fd;
    epoll_ctl(ep, EPOLL_CTL_MOD, fd, &e);
}

static void ep_del(int ep, int fd){
    epoll_ctl(ep, EPOLL_CTL_DEL, fd, NULL);
}

static void free_client(int ep, client_t *c){
    if (!c) return;
    ep_del(ep, c->fd);
    close(c->fd);
    if (c->rbuf) free(c->rbuf);
    if (c->wbuf) free(c->wbuf);
    if (c->ffd > 0) close(c->ffd);
    clients[c->fd] = NULL;
    free(c);
}

static void wqueue(client_t *c, const char *data, size_t n){
    if (c->wlen + n > c->wcap){
        size_t nc = c->wcap * 2;
        while (nc < c->wlen + n) nc *= 2;
        c->wbuf = realloc(c->wbuf, nc);
        c->wcap = nc;
    }
    memcpy(c->wbuf + c->wlen, data, n);
    c->wlen += n;
}

static void send_404(client_t *c){
    const char *b = "<h1>404</h1>";
    char h[256];
    int n = snprintf(h, sizeof(h),
                     "HTTP/1.1 404 Not Found\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n\r\n", strlen(b));
    wqueue(c, h, n);
    wqueue(c, b, strlen(b));
    c->keep = 0;
}

static void send_file(client_t *c, const char *path, off_t len, int keep){
    const char *ct = "text/plain";
    if (strstr(path,".html")) ct="text/html";
    else if (strstr(path,".jpg")||strstr(path,".jpeg")) ct="image/jpeg";
    else if (strstr(path,".png")) ct="image/png";
    else if (strstr(path,".css")) ct="text/css";
    else if (strstr(path,".js")) ct="application/javascript";

    char h[256];
    int n = snprintf(h,sizeof(h),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %jd\r\n"
             "Connection: %s\r\n\r\n",
             ct, (intmax_t)len, keep?"keep-alive":"close");

    wqueue(c,h,n);

    int fd = open(path,O_RDONLY);
    if (fd<0){ send_404(c); return; }

    c->ffd = fd; c->foff = 0; c->flen = len;
    c->keep = keep;
}

static void process_requests(client_t *c){
    size_t p = 0;
    while (1){
        if (c->rlen - p < 4) break;
        int found=-1;
        for (size_t i=p; i+3<c->rlen; ++i){
            if (c->rbuf[i]=='\r'&&c->rbuf[i+1]=='\n'&&
                c->rbuf[i+2]=='\r'&&c->rbuf[i+3]=='\n'){
                found = i+4;
                break;
            }
        }
        if (found<0) break;

        size_t hlen = found - p;
        char *h = malloc(hlen+1);
        memcpy(h, c->rbuf+p, hlen);
        h[hlen]=0;

        char path[4096];
        snprintf(path,sizeof(path),"%s",docroot);

        char *g = strstr(h,"GET ");
        if (!g){
            strcat(path,"/index.html");
        } else {
            g+=4;
            char *sp = strchr(g,' ');
            if (!sp){ strcat(path,"/index.html"); }
            else{
                size_t plen = sp-g;
                char tmp[4096];
                if (plen>=sizeof(tmp)) plen=sizeof(tmp)-1;
                memcpy(tmp,g,plen);
                tmp[plen]=0;

                if (strstr(tmp,"..")) strcat(path,"/index.html");
                else{
                    strcat(path,"/");
                    strcat(path,tmp);
                }
            }
        }

        int keep = 1;
        if (strcasestr(h,"Connection: close")) keep=0;
        if (strcasestr(h,"Connection: keep-alive")) keep=1;

        struct stat st;
        if (stat(path,&st)==0 && S_ISREG(st.st_mode)){
            send_file(c,path,st.st_size,keep);
        } else {
            send_404(c);
        }

        free(h);
        p = found;
    }

    if (p>0){
        size_t remain = c->rlen - p;
        memmove(c->rbuf, c->rbuf+p, remain);
        c->rlen = remain;
    }
}

static void on_read(int ep, client_t *c){
    while (1){
        if (c->rcap - c->rlen < 4096){
            c->rcap *= 2;
            c->rbuf = realloc(c->rbuf, c->rcap);
        }
        ssize_t n = read(c->fd, c->rbuf+c->rlen, c->rcap-c->rlen);
        if (n>0){
            c->rlen += n;
            process_requests(c);
            continue;
        }
        if (n==0){ c->keep=0; return; }
        if (errno==EAGAIN||errno==EWOULDBLOCK) break;
        c->keep=0; return;
    }

    if (c->wlen>0 || c->ffd>0)
        ep_mod(ep,c->fd,EPOLLIN|EPOLLOUT|EPOLLET);
}

static void on_write(int ep, client_t *c){
    while (c->wlen>0){
        ssize_t n=write(c->fd,c->wbuf,c->wlen);
        if (n>0){
            memmove(c->wbuf,c->wbuf+n,c->wlen-n);
            c->wlen-=n;
            continue;
        }
        if (errno==EAGAIN||errno==EWOULDBLOCK) break;
        c->keep=0; break;
    }

    while (c->wlen==0 && c->ffd>0 && c->foff<c->flen){
        off_t off = c->foff;
        ssize_t n=sendfile(c->fd,c->ffd,&off,c->flen-c->foff);
        if (n>=0){
            c->foff = off;
            continue;
        }
        if (errno==EAGAIN||errno==EWOULDBLOCK) break;
        close(c->ffd); c->ffd=0; c->keep=0;
    }

    if (c->ffd>0 && c->foff>=c->flen){
        close(c->ffd); c->ffd=0;
    }

    if (c->wlen==0 && c->ffd==0){
        while (1){
            char tmp[4096];
            ssize_t n = read(c->fd,tmp,sizeof(tmp));
            if (n>0){
                if (c->rcap - c->rlen < (size_t)n){
                    c->rcap*=2;
                    c->rbuf=realloc(c->rbuf,c->rcap);
                }
                memcpy(c->rbuf+c->rlen,tmp,n);
                c->rlen+=n;
                process_requests(c);
                continue;
            }
            if (n<0 && (errno==EAGAIN||errno==EWOULDBLOCK))
                break;
            break;
        }
    }

    if (!c->keep){
        free_client(ep,c);
        return;
    }

    if (c->wlen>0 || c->ffd>0)
        ep_mod(ep,c->fd,EPOLLIN|EPOLLOUT|EPOLLET);
    else
        ep_mod(ep,c->fd,EPOLLIN|EPOLLET);
}

int main(int argc,char **argv){
    if (argc<3){
        printf("Usage: %s <port> <docroot>\n", argv[0]);
        return 0;
    }

    int port=atoi(argv[1]);
    docroot=argv[2];

    int ls = socket(AF_INET,SOCK_STREAM,0);
    int on=1; setsockopt(ls,SOL_SOCKET,SO_REUSEADDR,&on,sizeof(on));
    setnonblocking(ls);

    struct sockaddr_in a={0};
    a.sin_family=AF_INET;
    a.sin_addr.s_addr=INADDR_ANY;
    a.sin_port=htons(port);
    bind(ls,(struct sockaddr*)&a,sizeof(a));
    listen(ls,512);

    int ep = epoll_create1(0);
    ep_add(ep,ls,EPOLLIN|EPOLLET);

    struct epoll_event evs[MAXEVENTS];

    printf("epoll HTTP server running on port %d, docroot=%s\n",port,docroot);

    while (1){
        int n = epoll_wait(ep,evs,MAXEVENTS,-1);
        for (int i=0;i<n;i++){
            int fd = evs[i].data.fd;
            uint32_t ev = evs[i].events;

            if (fd==ls){
                while (1){
                    struct sockaddr_in caddr;
                    socklen_t l=sizeof(caddr);
                    int cfd=accept(ls,(struct sockaddr*)&caddr,&l);
                    if (cfd<0){
                        if (errno==EAGAIN||errno==EWOULDBLOCK) break;
                        break;
                    }
                    setnonblocking(cfd);
                    client_t *c = calloc(1,sizeof(client_t));
                    c->fd=cfd; c->keep=1;
                    c->rcap=READ_BUF_INIT; c->rbuf=malloc(c->rcap);
                    c->wcap=WRITE_BUF_INIT; c->wbuf=malloc(c->wcap);
                    clients[cfd]=c;
                    ep_add(ep,cfd,EPOLLIN|EPOLLET);
                }
                continue;
            }

            client_t *c = clients[fd];
            if (!c) continue;

            if (ev & (EPOLLHUP|EPOLLERR)){
                free_client(ep,c);
                continue;
            }

            if (ev & EPOLLIN)
                on_read(ep,c);

            if (clients[fd] && (ev & EPOLLOUT))
                on_write(ep,c);
        }
    }
}
