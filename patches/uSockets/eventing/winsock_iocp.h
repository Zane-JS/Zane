/*
 * Windows IOCP header for uSockets
 * Native high-performance async I/O for Windows
 */

#ifndef WINSOCK_IOCP_H
#define WINSOCK_IOCP_H

#ifdef LIBUS_USE_IOCP

#include "internal/loop_data.h"
#include <winsock2.h>
#include <windows.h>

/* Define socket event flags compatible with uSockets */
#define LIBUS_SOCKET_READABLE 1
#define LIBUS_SOCKET_WRITABLE 2

/* Forward declaration of operation context */
struct iocp_operation;

/* Loop structure for IOCP */
struct us_loop_t {
    alignas(LIBUS_EXT_ALIGNMENT) struct us_internal_loop_data_t data;
    
    /* IOCP handle */
    HANDLE iocp_handle;
    
    /* Number of active polls */
    int num_polls;
    
    /* Winsock initialized flag */
    int winsock_initialized;
    
    /* Timer queue */
    HANDLE timer_queue;
};

/* Poll structure */
struct us_poll_t {
    alignas(LIBUS_EXT_ALIGNMENT) struct {
        SOCKET socket;
        unsigned int poll_type : 4;
        unsigned int events : 4;
    } state;
    
    /* Pending operations */
    struct iocp_operation *read_op;
    struct iocp_operation *write_op;
};

#endif /* LIBUS_USE_IOCP */

#endif /* WINSOCK_IOCP_H */
