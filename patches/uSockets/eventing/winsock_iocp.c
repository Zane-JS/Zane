/*
 * Windows IOCP (I/O Completion Ports) implementation for uSockets
 * Native high-performance async I/O for Windows without libuv dependency
 * 
 * This implementation provides native Windows support for Z8 runtime,
 * targeting the Windows server market that Bun hasn't conquered yet.
 */

#ifdef LIBUS_USE_IOCP

#include "libusockets.h"
#include "internal/internal.h"

#include <winsock2.h>
#include <windows.h>
#include <mswsock.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")

/* CONTAINING_RECORD macro for getting parent structure from member */
#ifndef CONTAINING_RECORD
#define CONTAINING_RECORD(address, type, field) \
    ((type *)((char *)(address) - (unsigned long long)(&((type *)0)->field)))
#endif

/* IOCP operation types */
enum iocp_operation_type {
    IOCP_OP_ACCEPT,
    IOCP_OP_CONNECT,
    IOCP_OP_READ,
    IOCP_OP_WRITE,
    IOCP_OP_TIMER,
    IOCP_OP_ASYNC
};

/* IOCP operation context - overlapped structure with metadata */
struct iocp_operation {
    OVERLAPPED overlapped;
    enum iocp_operation_type type;
    struct us_poll_t *poll;
    WSABUF wsa_buf;
    char buffer[LIBUS_RECV_BUFFER_LENGTH];
    DWORD bytes_transferred;
    DWORD flags;
};

/* Initialize Winsock */
static int init_winsock(struct us_loop_t *loop) {
    if (loop->winsock_initialized) {
        return 0;
    }
    
    WSADATA wsa_data;
    int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (result != 0) {
        return -1;
    }
    
    loop->winsock_initialized = 1;
    return 0;
}

/* Create IOCP operation context */
static struct iocp_operation *create_iocp_operation(enum iocp_operation_type type, struct us_poll_t *poll) {
    struct iocp_operation *op = (struct iocp_operation *)calloc(1, sizeof(struct iocp_operation));
    if (!op) {
        return NULL;
    }
    
    memset(&op->overlapped, 0, sizeof(OVERLAPPED));
    op->type = type;
    op->poll = poll;
    op->flags = 0;
    
    return op;
}

/* Loop functions */
struct us_loop_t *us_create_loop(void *hint, void (*wakeup_cb)(struct us_loop_t *loop),
                                  void (*pre_cb)(struct us_loop_t *loop),
                                  void (*post_cb)(struct us_loop_t *loop),
                                  unsigned int ext_size) {
    struct us_loop_t *loop = (struct us_loop_t *)malloc(sizeof(struct us_loop_t) + ext_size);
    if (!loop) {
        return NULL;
    }
    
    memset(loop, 0, sizeof(struct us_loop_t));
    
    /* Initialize Winsock */
    if (init_winsock(loop) != 0) {
        free(loop);
        return NULL;
    }
    
    /* Create IOCP handle */
    loop->iocp_handle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (loop->iocp_handle == NULL) {
        free(loop);
        return NULL;
    }
    
    /* Create timer queue */
    loop->timer_queue = CreateTimerQueue();
    if (loop->timer_queue == NULL) {
        CloseHandle(loop->iocp_handle);
        free(loop);
        return NULL;
    }
    
    loop->num_polls = 0;
    
    us_internal_loop_data_init(loop, wakeup_cb, pre_cb, post_cb);
    
    return loop;
}

void us_loop_free(struct us_loop_t *loop) {
    if (!loop) {
        return;
    }
    
    us_internal_loop_data_free(loop);
    
    if (loop->timer_queue) {
        DeleteTimerQueueEx(loop->timer_queue, INVALID_HANDLE_VALUE);
    }
    
    if (loop->iocp_handle) {
        CloseHandle(loop->iocp_handle);
    }
    
    if (loop->winsock_initialized) {
        WSACleanup();
    }
    
    free(loop);
}

void *us_loop_ext(struct us_loop_t *loop) {
    return loop + 1;
}

void us_loop_run(struct us_loop_t *loop) {
    DWORD bytes_transferred;
    ULONG_PTR completion_key;
    LPOVERLAPPED overlapped;
    
    while (loop->num_polls > 0) {
        us_internal_loop_pre(loop);
        
        /* Wait for I/O completion with timeout */
        BOOL result = GetQueuedCompletionStatus(
            loop->iocp_handle,
            &bytes_transferred,
            &completion_key,
            &overlapped,
            100  /* 100ms timeout for timer granularity */
        );
        
        if (overlapped != NULL) {
            struct iocp_operation *op = CONTAINING_RECORD(overlapped, struct iocp_operation, overlapped);
            
            if (result) {
                /* Successful I/O completion */
                op->bytes_transferred = bytes_transferred;
                
                switch (op->type) {
                    case IOCP_OP_READ:
                        us_internal_dispatch_ready_poll(op->poll, 0, LIBUS_SOCKET_READABLE);
                        break;
                    case IOCP_OP_WRITE:
                        us_internal_dispatch_ready_poll(op->poll, 0, LIBUS_SOCKET_WRITABLE);
                        break;
                    case IOCP_OP_ACCEPT:
                    case IOCP_OP_CONNECT:
                        us_internal_dispatch_ready_poll(op->poll, 0, LIBUS_SOCKET_READABLE);
                        break;
                }
            } else {
                /* I/O error */
                DWORD error = GetLastError();
                if (error != ERROR_OPERATION_ABORTED) {
                    us_internal_dispatch_ready_poll(op->poll, error, 0);
                }
            }
            
            free(op);
        }
        
        /* Process timers */
        us_internal_timer_sweep(loop);
        
        /* Free closed sockets */
        us_internal_free_closed_sockets(loop);
        
        us_internal_loop_post(loop);
    }
}

void us_wakeup_loop(struct us_loop_t *loop) {
    /* Post a completion packet to wake up the loop */
    PostQueuedCompletionStatus(loop->iocp_handle, 0, 0, NULL);
}

long long us_loop_iteration_number(struct us_loop_t *loop) {
    return loop->data.iteration_nr;
}

void us_loop_integrate(struct us_loop_t *loop) {
    /* IOCP doesn't need integration like epoll/kqueue */
}

/* Poll functions */
struct us_poll_t *us_create_poll(struct us_loop_t *loop, int fallthrough, unsigned int ext_size) {
    struct us_poll_t *poll = (struct us_poll_t *)malloc(sizeof(struct us_poll_t) + ext_size);
    if (!poll) {
        return NULL;
    }
    
    memset(poll, 0, sizeof(struct us_poll_t));
    poll->state.socket = INVALID_SOCKET;
    poll->read_op = NULL;
    poll->write_op = NULL;
    
    if (!fallthrough) {
        loop->num_polls++;
    }
    
    return poll;
}

void us_poll_free(struct us_poll_t *p, struct us_loop_t *loop) {
    if (!p) {
        return;
    }
    
    if (p->read_op) {
        free(p->read_op);
    }
    if (p->write_op) {
        free(p->write_op);
    }
    
    free(p);
}

void us_poll_init(struct us_poll_t *p, LIBUS_SOCKET_DESCRIPTOR fd, int poll_type) {
    p->state.socket = fd;
    p->state.poll_type = poll_type;
    p->state.events = 0;
}

void us_poll_start(struct us_poll_t *p, struct us_loop_t *loop, int events) {
    /* Associate socket with IOCP */
    if (CreateIoCompletionPort((HANDLE)p->state.socket, loop->iocp_handle, 
                               (ULONG_PTR)p, 0) == NULL) {
        return;
    }
    
    p->state.events = events;
    
    /* Start async read if needed */
    if (events & LIBUS_SOCKET_READABLE) {
        if (!p->read_op) {
            p->read_op = create_iocp_operation(IOCP_OP_READ, p);
        }
        
        if (p->read_op) {
            p->read_op->wsa_buf.buf = p->read_op->buffer;
            p->read_op->wsa_buf.len = sizeof(p->read_op->buffer);
            
            DWORD flags = 0;
            WSARecv(p->state.socket, &p->read_op->wsa_buf, 1, NULL, &flags,
                   &p->read_op->overlapped, NULL);
        }
    }
}

void us_poll_change(struct us_poll_t *p, struct us_loop_t *loop, int events) {
    /* For IOCP, we manage operations dynamically */
    p->state.events = events;
    
    /* Restart read operation if needed */
    if ((events & LIBUS_SOCKET_READABLE) && !p->read_op) {
        us_poll_start(p, loop, events);
    }
}

void us_poll_stop(struct us_poll_t *p, struct us_loop_t *loop) {
    p->state.events = 0;
    
    /* Cancel pending I/O operations */
    if (p->state.socket != INVALID_SOCKET) {
        CancelIo((HANDLE)p->state.socket);
    }
}

int us_poll_events(struct us_poll_t *p) {
    return p->state.events;
}

void *us_poll_ext(struct us_poll_t *p) {
    return p + 1;
}

LIBUS_SOCKET_DESCRIPTOR us_poll_fd(struct us_poll_t *p) {
    return p->state.socket;
}

struct us_poll_t *us_poll_resize(struct us_poll_t *p, struct us_loop_t *loop, unsigned int ext_size) {
    int events = us_poll_events(p);
    SOCKET socket = p->state.socket;
    int poll_type = p->state.poll_type;
    
    /* Stop current poll */
    us_poll_stop(p, loop);
    
    /* Create new poll with larger extension */
    struct us_poll_t *new_p = (struct us_poll_t *)realloc(p, sizeof(struct us_poll_t) + ext_size);
    if (!new_p) {
        return p;
    }
    
    /* Reinitialize */
    us_poll_init(new_p, socket, poll_type);
    us_poll_start(new_p, loop, events);
    
    return new_p;
}

unsigned int us_internal_accept_poll_event(struct us_poll_t *p) {
    return 0;
}

int us_internal_poll_type(struct us_poll_t *p) {
    return p->state.poll_type & 3;
}

void us_internal_poll_set_type(struct us_poll_t *p, int poll_type) {
    p->state.poll_type = poll_type;
}

/* Timer functions */
struct us_timer_t {
    alignas(LIBUS_EXT_ALIGNMENT) struct us_loop_t *loop;
    HANDLE timer_handle;
    void (*cb)(struct us_timer_t *);
};

static VOID CALLBACK timer_callback(PVOID parameter, BOOLEAN timer_or_wait_fired) {
    struct us_timer_t *timer = (struct us_timer_t *)parameter;
    if (timer && timer->cb) {
        timer->cb(timer);
    }
}

struct us_timer_t *us_create_timer(struct us_loop_t *loop, int fallthrough, unsigned int ext_size) {
    struct us_timer_t *timer = (struct us_timer_t *)malloc(sizeof(struct us_timer_t) + ext_size);
    if (!timer) {
        return NULL;
    }
    
    timer->loop = loop;
    timer->timer_handle = NULL;
    timer->cb = NULL;
    
    return timer;
}

void *us_timer_ext(struct us_timer_t *timer) {
    return timer + 1;
}

void us_timer_close(struct us_timer_t *timer) {
    if (timer->timer_handle) {
        DeleteTimerQueueTimer(timer->loop->timer_queue, timer->timer_handle, NULL);
    }
    free(timer);
}

void us_timer_set(struct us_timer_t *timer, void (*cb)(struct us_timer_t *), int ms, int repeat_ms) {
    timer->cb = cb;
    
    if (timer->timer_handle) {
        DeleteTimerQueueTimer(timer->loop->timer_queue, timer->timer_handle, NULL);
        timer->timer_handle = NULL;
    }
    
    if (ms > 0 || repeat_ms > 0) {
        CreateTimerQueueTimer(
            &timer->timer_handle,
            timer->loop->timer_queue,
            timer_callback,
            timer,
            ms,
            repeat_ms,
            WT_EXECUTEDEFAULT
        );
    }
}

struct us_loop_t *us_timer_loop(struct us_timer_t *timer) {
    return timer->loop;
}

/* Async functions (for wakeup support) */
struct us_internal_async {
    alignas(LIBUS_EXT_ALIGNMENT) struct us_loop_t *loop;
    HANDLE event_handle;
    void (*cb)(struct us_internal_async *);
};

struct us_internal_async *us_internal_create_async(struct us_loop_t *loop, int fallthrough, unsigned int ext_size) {
    struct us_internal_async *async = (struct us_internal_async *)malloc(sizeof(struct us_internal_async) + ext_size);
    if (!async) {
        return NULL;
    }
    
    async->loop = loop;
    async->event_handle = CreateEvent(NULL, FALSE, FALSE, NULL);
    async->cb = NULL;
    
    if (!async->event_handle) {
        free(async);
        return NULL;
    }
    
    return async;
}

void us_internal_async_close(struct us_internal_async *async) {
    if (async->event_handle) {
        CloseHandle(async->event_handle);
    }
    free(async);
}

void us_internal_async_set(struct us_internal_async *async, void (*cb)(struct us_internal_async *)) {
    async->cb = cb;
}

void us_internal_async_wakeup(struct us_internal_async *async) {
    if (async->event_handle) {
        SetEvent(async->event_handle);
        /* Post completion to wake up the loop */
        PostQueuedCompletionStatus(async->loop->iocp_handle, 0, (ULONG_PTR)async, NULL);
    }
}

/* Stub SSL functions - not implemented yet */
void us_internal_init_loop_ssl_data(struct us_loop_t *loop) {
    /* TODO: Implement SSL support */
}

void us_internal_free_loop_ssl_data(struct us_loop_t *loop) {
    /* TODO: Implement SSL support */
}

#endif /* LIBUS_USE_IOCP */
