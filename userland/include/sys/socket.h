#pragma once

#include <sys/types.h>
#include <sys/uio.h>

#define AF_UNSPEC 0
#define AF_UNIX   1
#define AF_INET   2
#define AF_INET6  10

#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3

#define SOL_SOCKET 1
#define SOL_IP 0
#define SO_REUSEADDR 2
#define SO_RCVTIMEO 20
#define SO_SNDTIMEO 21
#define SO_MARK 36

#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

#define MSG_PEEK 0x2

struct sockaddr {
    sa_family_t sa_family;
    char sa_data[14];
};

struct sockaddr_storage {
    sa_family_t ss_family;
    char __ss_padding[126];
};

struct msghdr {
    void* msg_name;
    socklen_t msg_namelen;
    struct iovec* msg_iov;
    size_t msg_iovlen;
    void* msg_control;
    size_t msg_controllen;
    int msg_flags;
};

struct cmsghdr {
    size_t cmsg_len;
    int cmsg_level;
    int cmsg_type;
};

#define CMSG_ALIGN(len) (((len) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))
#define CMSG_SPACE(len) (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(len))
#define CMSG_LEN(len) (CMSG_ALIGN(sizeof(struct cmsghdr)) + (len))
#define CMSG_DATA(cmsg) ((unsigned char*)(cmsg) + CMSG_ALIGN(sizeof(struct cmsghdr)))

#define CMSG_FIRSTHDR(msg) \
    (((msg) && (msg)->msg_controllen >= sizeof(struct cmsghdr)) ? \
        (struct cmsghdr*)((msg)->msg_control) : (struct cmsghdr*)0)

#define CMSG_NXTHDR(msg, cmsg) \
    ({ \
        struct cmsghdr* __c = (cmsg); \
        unsigned char* __next = (unsigned char*)__c + CMSG_ALIGN(__c->cmsg_len); \
        unsigned char* __end = (unsigned char*)(msg)->msg_control + (msg)->msg_controllen; \
        (__next + sizeof(struct cmsghdr) <= __end) ? (struct cmsghdr*)__next : (struct cmsghdr*)0; \
    })

int socket(int domain, int type, int protocol);
int bind(int sockfd, const struct sockaddr* addr, socklen_t addrlen);
int connect(int sockfd, const struct sockaddr* addr, socklen_t addrlen);
int listen(int sockfd, int backlog);
int accept(int sockfd, struct sockaddr* addr, socklen_t* addrlen);
ssize_t send(int sockfd, const void* buf, size_t len, int flags);
ssize_t recv(int sockfd, void* buf, size_t len, int flags);
ssize_t sendmsg(int sockfd, const struct msghdr* msg, int flags);
ssize_t recvmsg(int sockfd, struct msghdr* msg, int flags);
ssize_t sendto(int sockfd, const void* buf, size_t len, int flags, const struct sockaddr* dest_addr, socklen_t addrlen);
ssize_t recvfrom(int sockfd, void* buf, size_t len, int flags, struct sockaddr* src_addr, socklen_t* addrlen);
int setsockopt(int sockfd, int level, int optname, const void* optval, socklen_t optlen);
int getsockopt(int sockfd, int level, int optname, void* optval, socklen_t* optlen);
int getsockname(int sockfd, struct sockaddr* addr, socklen_t* addrlen);
int getpeername(int sockfd, struct sockaddr* addr, socklen_t* addrlen);
int shutdown(int sockfd, int how);
