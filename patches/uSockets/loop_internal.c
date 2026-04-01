/* Loop internal functions needed by uSockets
 * This file provides the internal loop functions that other uSockets files depend on
 */

#ifdef LIBUS_USE_IOCP

#include "libusockets.h"
#include "internal/internal.h"
#include <stdlib.h>

/* Include only the internal functions from loop.c, not the main loop implementation */

/* The loop has 2 fallthrough polls */
void us_internal_loop_data_init(struct us_loop_t *loop, void (*wakeup_cb)(struct us_loop_t *loop),
    void (*pre_cb)(struct us_loop_t *loop), void (*post_cb)(struct us_loop_t *loop)) {
    loop->data.sweep_timer = us_create_timer(loop, 1, 0);
    loop->data.recv_buf = malloc(LIBUS_RECV_BUFFER_LENGTH + LIBUS_RECV_BUFFER_PADDING * 2);
    loop->data.ssl_data = 0;
    loop->data.head = 0;
    loop->data.iterator = 0;
    loop->data.closed_head = 0;
    loop->data.low_prio_head = 0;
    loop->data.low_prio_budget = 0;

    loop->data.pre_cb = pre_cb;
    loop->data.post_cb = post_cb;
    loop->data.iteration_nr = 0;

    loop->data.wakeup_async = us_internal_create_async(loop, 1, 0);
    us_internal_async_set(loop->data.wakeup_async, (void (*)(struct us_internal_async *)) wakeup_cb);
}

void us_internal_loop_data_free(struct us_loop_t *loop) {
#ifndef LIBUS_NO_SSL
    us_internal_free_loop_ssl_data(loop);
#endif

    free(loop->data.recv_buf);

    us_timer_close(loop->data.sweep_timer);
    us_internal_async_close(loop->data.wakeup_async);
}

void us_internal_loop_link(struct us_loop_t *loop, struct us_socket_context_t *context) {
    /* Insert this context as the head of loop */
    context->next = loop->data.head;
    if (loop->data.head) {
        loop->data.head->prev = context;
    }
    loop->data.head = context;
    context->prev = 0;
}

/* Unlink is called before free */
void us_internal_loop_unlink(struct us_loop_t *loop, struct us_socket_context_t *context) {
    if (loop->data.head == context) {
        loop->data.head = context->next;
        if (loop->data.head) {
            loop->data.head->prev = 0;
        }
    } else {
        context->prev->next = context->next;
        if (context->next) {
            context->next->prev = context->prev;
        }
    }
}

/* This functions should never run recursively */
void us_internal_timer_sweep(struct us_loop_t *loop) {
    struct us_internal_loop_data_t *loop_data = &loop->data;
    /* For all socket contexts in this loop */
    for (struct us_socket_context_t *context = loop_data->head; context; context = context->next) {
        /* Increase timestamp so that we can compare timeouts to this and see if it is triggered or not */
        context->timestamp = (context->timestamp + 1) & 255;
        context->long_timestamp = (context->long_timestamp + 1) & 255;

        /* For all sockets in this context */
        for (struct us_socket_t *s = context->head_sockets; s; s = s->next) {
            /* Check if this socket should timeout */
            if (s->timeout != 255 && s->timeout == context->timestamp) {
                /* Timeout this socket */
                s = context->on_socket_timeout(s);
            }

            /* Check long timeout */
            if (s && s->long_timeout != 255 && s->long_timeout == context->long_timestamp) {
                /* Timeout this socket */
                s = context->on_socket_long_timeout(s);
            }
        }
    }
}

static const int MAX_LOW_PRIO_SOCKETS_PER_LOOP_ITERATION = 5;

void us_internal_handle_low_priority_sockets(struct us_loop_t *loop) {
    struct us_internal_loop_data_t *loop_data = &loop->data;
    struct us_socket_t *s;

    loop_data->low_prio_budget = MAX_LOW_PRIO_SOCKETS_PER_LOOP_ITERATION;
    for (s = loop_data->low_prio_head; s && loop_data->low_prio_budget > 0; s = s->next) {
        if (s->low_prio_state == 2) {
            s->low_prio_state = 1;
        }
    }
}

/* Note: Properly takes the linked list and timeout sweep into account */
void us_internal_free_closed_sockets(struct us_loop_t *loop) {
    /* Free all closed sockets (maybe it is better to reverse order?) */
    if (loop->data.closed_head) {
        for (struct us_socket_t *s = loop->data.closed_head; s; ) {
            struct us_socket_t *next = s->next;
            free(s);
            s = next;
        }
        loop->data.closed_head = 0;
    }
}

/* These may have somewhat different meaning depending on the underlying event library */
void us_internal_loop_pre(struct us_loop_t *loop) {
    loop->data.iteration_nr++;
    us_internal_handle_low_priority_sockets(loop);
    loop->data.pre_cb(loop);
}

void us_internal_loop_post(struct us_loop_t *loop) {
    us_internal_free_closed_sockets(loop);
    loop->data.post_cb(loop);
}

void us_internal_dispatch_ready_poll(struct us_poll_t *p, int error, int events) {
    switch (us_internal_poll_type(p)) {
    case POLL_TYPE_CALLBACK: {
        struct us_internal_callback_t *cb = (struct us_internal_callback_t *) p;
        /* Ignore any event if we are not polling for it */
        if (us_poll_events(p)) {
            cb->cb(cb);
        }
        break;
    }
    case POLL_TYPE_SEMI_SOCKET: {
        /* Semi-sockets are not implemented yet */
        break;
    }
    case POLL_TYPE_SOCKET_SHUT_DOWN:
    case POLL_TYPE_SOCKET: {
        struct us_socket_t *s = (struct us_socket_t *) p;

        /* Such as ECONNRESET, ECONNREFUSED */
        if (error) {
            s = s->context->on_close(s, 0, NULL);
            break;
        }

        if (events & LIBUS_SOCKET_WRITABLE) {
            s = s->context->on_writable(s);
            if (!s) {
                break;
            }
        }

        if (events & LIBUS_SOCKET_READABLE) {
            s = s->context->on_data(s, s->context->loop->data.recv_buf, 0);
        }
        break;
    }
    }
}

#endif /* LIBUS_USE_IOCP */
