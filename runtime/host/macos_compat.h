#pragma once
#ifdef __APPLE__
#define FO2_WEAK_IMPORT __attribute__((weak_import))
#include <pthread.h>
#include <stdint.h>
#include <sys/socket.h>
#include <net/if_dl.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0x10000000
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0x20000000
#endif
static inline int fo2Socket(int domain, int type, int protocol) {
    int fd = socket(domain, type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC), protocol);
    if (fd >= 0) {
        int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
        if (type & SOCK_NONBLOCK) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        if (type & SOCK_CLOEXEC) fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    return fd;
}
static inline int fo2Accept4(int fd, struct sockaddr* address, socklen_t* len, int flags) {
    int client = accept(fd, address, len);
    if (client >= 0) {
        int one = 1; setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
        if (flags & SOCK_NONBLOCK) fcntl(client, F_SETFL, fcntl(client, F_GETFL) | O_NONBLOCK);
        if (flags & SOCK_CLOEXEC) fcntl(client, F_SETFD, FD_CLOEXEC);
    }
    return client;
}
#ifdef __cplusplus
inline int fo2SetThreadName(pthread_t, const char* name) { return pthread_setname_np(name); }
#define pthread_setname_np(thread, name) fo2SetThreadName(thread, name)
#endif
#define st_mtim st_mtimespec
#define st_atim st_atimespec
#endif

#ifndef __APPLE__
#define FO2_WEAK_IMPORT __attribute__((weak))
#endif
