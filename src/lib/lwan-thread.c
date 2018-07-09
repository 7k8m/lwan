/*
 * lwan - simple web server
 * Copyright (c) 2012, 2013 Leandro A. F. Pereira <leandro@hardinfo.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>

#if defined(HAVE_EVENTFD)
#include <sys/eventfd.h>
#endif

#include "lwan-private.h"

struct death_queue {
    const struct lwan *lwan;
    struct lwan_connection *conns;
    struct lwan_connection head;
    unsigned time;
    unsigned short keep_alive_timeout;
};

static const uint32_t events_by_write_flag[] = {
    EPOLLOUT | EPOLLRDHUP | EPOLLERR,
    EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLET
};

static inline int death_queue_node_to_idx(struct death_queue *dq,
    struct lwan_connection *conn)
{
    return (conn == &dq->head) ? -1 : (int)(ptrdiff_t)(conn - dq->conns);
}

static inline struct lwan_connection *death_queue_idx_to_node(struct death_queue *dq,
    int idx)
{
    return (idx < 0) ? &dq->head : &dq->conns[idx];
}

static void death_queue_insert(struct death_queue *dq,
    struct lwan_connection *new_node)
{
    new_node->next = -1;
    new_node->prev = dq->head.prev;
    struct lwan_connection *prev = death_queue_idx_to_node(dq, dq->head.prev);
    dq->head.prev = prev->next = death_queue_node_to_idx(dq, new_node);
}

static void death_queue_remove(struct death_queue *dq,
    struct lwan_connection *node)
{
    struct lwan_connection *prev = death_queue_idx_to_node(dq, node->prev);
    struct lwan_connection *next = death_queue_idx_to_node(dq, node->next);
    next->prev = node->prev;
    prev->next = node->next;

    /* FIXME: This shouldn't be required; there may be a bug somewhere when
     * a few million requests are attended to.  */
    node->next = node->prev = -1;
}

static bool death_queue_empty(struct death_queue *dq)
{
    return dq->head.next < 0;
}

static void death_queue_move_to_last(struct death_queue *dq,
    struct lwan_connection *conn)
{
    /*
     * If the connection isn't keep alive, it might have a coroutine that
     * should be resumed.  If that's the case, schedule for this request to
     * die according to the keep alive timeout.
     *
     * If it's not a keep alive connection, or the coroutine shouldn't be
     * resumed -- then just mark it to be reaped right away.
     */
    conn->time_to_die = dq->time;
    if (conn->flags & (CONN_KEEP_ALIVE | CONN_SHOULD_RESUME_CORO))
        conn->time_to_die += dq->keep_alive_timeout;

    death_queue_remove(dq, conn);
    death_queue_insert(dq, conn);
}

static void
death_queue_init(struct death_queue *dq, const struct lwan *lwan)
{
    dq->lwan = lwan;
    dq->conns = lwan->conns;
    dq->time = 0;
    dq->keep_alive_timeout = lwan->config.keep_alive_timeout;
    dq->head.next = dq->head.prev = -1;
}

static ALWAYS_INLINE int
death_queue_epoll_timeout(struct death_queue *dq)
{
    return death_queue_empty(dq) ? -1 : 1000;
}

static ALWAYS_INLINE void
destroy_coro(struct death_queue *dq, struct lwan_connection *conn)
{
    death_queue_remove(dq, conn);
    if (LIKELY(conn->coro)) {
        coro_free(conn->coro);
        conn->coro = NULL;
    }
    if (conn->flags & CONN_IS_ALIVE) {
        conn->flags &= ~CONN_IS_ALIVE;
        close(lwan_connection_get_fd(dq->lwan, conn));
    }
}

static ALWAYS_INLINE int
min(const int a, const int b)
{
    return a < b ? a : b;
}

__attribute__((noreturn)) static int
process_request_coro(struct coro *coro, void *data)
{
    /* NOTE: This function should not return; coro_yield should be used
     * instead.  This ensures the storage for `strbuf` is alive when the
     * coroutine ends and lwan_strbuf_free() is called. */
    struct lwan_connection *conn = data;
    const enum lwan_request_flags flags_filter = (REQUEST_PROXIED | REQUEST_ALLOW_CORS);
    struct lwan_strbuf strbuf;
    struct lwan *lwan = conn->thread->lwan;
    int fd = lwan_connection_get_fd(lwan, conn);
    char request_buffer[DEFAULT_BUFFER_SIZE];
    struct lwan_value buffer = {
        .value = request_buffer,
        .len = 0
    };
    char *next_request = NULL;
    enum lwan_request_flags flags = 0;
    struct lwan_proxy proxy;

    if (UNLIKELY(!lwan_strbuf_init(&strbuf))) {
        coro_yield(coro, CONN_CORO_ABORT);
        __builtin_unreachable();
    }
    coro_defer(coro, CORO_DEFER(lwan_strbuf_free), &strbuf);

    flags |= lwan->config.proxy_protocol << REQUEST_ALLOW_PROXY_REQS_SHIFT |
             lwan->config.allow_cors << REQUEST_ALLOW_CORS_SHIFT;

    while (true) {
        struct lwan_request request = {
            .conn = conn,
            .fd = fd,
            .response = {
                .buffer = &strbuf
            },
            .flags = flags,
            .proxy = &proxy
        };

        assert(conn->flags & CONN_IS_ALIVE);

        size_t generation = coro_deferred_get_generation(coro);
        next_request = lwan_process_request(lwan, &request, &buffer, next_request);
        coro_deferred_run(coro, generation);

        coro_yield(coro, CONN_CORO_MAY_RESUME);

        lwan_strbuf_reset(&strbuf);
        flags = request.flags & flags_filter;
    }
}

static ALWAYS_INLINE void
resume_coro_if_needed(struct death_queue *dq, struct lwan_connection *conn,
    int epoll_fd)
{
    assert(conn->coro);

    if (!(conn->flags & CONN_SHOULD_RESUME_CORO))
        return;

    enum lwan_connection_coro_yield yield_result = coro_resume(conn->coro);
    /* CONN_CORO_ABORT is -1, but comparing with 0 is cheaper */
    if (yield_result < CONN_CORO_MAY_RESUME) {
        destroy_coro(dq, conn);
        return;
    }

    bool write_events;
    if (conn->flags & CONN_MUST_READ) {
        write_events = true;
    } else {
        bool should_resume_coro = (yield_result == CONN_CORO_MAY_RESUME);

        if (should_resume_coro)
            conn->flags |= CONN_SHOULD_RESUME_CORO;
        else
            conn->flags &= ~CONN_SHOULD_RESUME_CORO;

        write_events = (conn->flags & CONN_WRITE_EVENTS);
        if (should_resume_coro == write_events)
            return;
    }

    struct epoll_event event = {
        .events = events_by_write_flag[write_events],
        .data.ptr = conn
    };

    int fd = lwan_connection_get_fd(dq->lwan, conn);
    if (UNLIKELY(epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event) < 0))
        lwan_status_perror("epoll_ctl");

    conn->flags ^= CONN_WRITE_EVENTS;
}

static void
death_queue_kill_waiting(struct death_queue *dq)
{
    dq->time++;

    while (!death_queue_empty(dq)) {
        struct lwan_connection *conn = death_queue_idx_to_node(dq, dq->head.next);

        if (conn->time_to_die > dq->time)
            return;

        destroy_coro(dq, conn);
    }

    /* Death queue exhausted: reset epoch */
    dq->time = 0;
}

static void
death_queue_kill_all(struct death_queue *dq)
{
    while (!death_queue_empty(dq)) {
        struct lwan_connection *conn = death_queue_idx_to_node(dq, dq->head.next);
        destroy_coro(dq, conn);
    }
}

static void
update_date_cache(struct lwan_thread *thread)
{
    time_t now = time(NULL);
    if (now != thread->date.last) {
        thread->date.last = now;

        lwan_format_rfc_time(now, thread->date.date);
        lwan_format_rfc_time(now + (time_t)thread->lwan->config.expires,
                    thread->date.expires);
    }
}

static ALWAYS_INLINE void
spawn_coro(struct lwan_connection *conn,
            struct coro_switcher *switcher, struct death_queue *dq)
{
    assert(!conn->coro);
    assert(!(conn->flags & CONN_IS_ALIVE));
    assert(!(conn->flags & CONN_SHOULD_RESUME_CORO));

    conn->coro = coro_new(switcher, process_request_coro, conn);
    if (UNLIKELY(!conn->coro)) {
        lwan_status_error("Could not create coroutine");
        return;
    }

    conn->flags = CONN_IS_ALIVE | CONN_SHOULD_RESUME_CORO;
    conn->time_to_die = dq->time + dq->keep_alive_timeout;

    death_queue_insert(dq, conn);
}

static void
accept_nudge(int pipe_fd, struct spsc_queue *pending_fds,
    struct lwan_connection *conns, struct death_queue *dq,
    struct coro_switcher *switcher, int epoll_fd)
{
    void *new_fd;
    uint64_t event;

    if (read(pipe_fd, &event, sizeof(event)) < 0) {
        return;
    }

    while ((new_fd = spsc_queue_pop(pending_fds))) {
        int fd = (int)(intptr_t)new_fd;
        struct lwan_connection *conn = &conns[fd];
        struct epoll_event ep_event = { .events = events_by_write_flag[1] };

        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ep_event) < 0) {
            lwan_status_perror("epoll_ctl");
            continue;
        }

        spawn_coro(conn, switcher, dq);
        resume_coro_if_needed(dq, conn, epoll_fd);
    }
}

static void *
thread_io_loop(void *data)
{
    struct lwan_thread *t = data;
    int epoll_fd = t->epoll_fd;
    const int read_pipe_fd = t->pipe_fd[0];
    const int max_events = min((int)t->lwan->thread.max_fd, 1024);
    struct lwan *lwan = t->lwan;
    struct epoll_event *events;
    struct coro_switcher switcher;
    struct death_queue dq;
    int n_fds;

    lwan_status_debug("Starting IO loop on thread #%d",
        (unsigned short)(ptrdiff_t)(t - t->lwan->thread.threads) + 1);

    events = calloc((size_t)max_events, sizeof(*events));
    if (UNLIKELY(!events))
        lwan_status_critical("Could not allocate memory for events");

    death_queue_init(&dq, lwan);

    pthread_barrier_wait(&lwan->thread.barrier);

    for (;;) {
        switch (n_fds = epoll_wait(epoll_fd, events, max_events,
                                   death_queue_epoll_timeout(&dq))) {
        case -1:
            if (errno == EBADF || errno == EINVAL)
                goto epoll_fd_closed;
            continue;

        case 0: /* timeout: shutdown waiting sockets */
            death_queue_kill_waiting(&dq);
            break;

        default: /* activity in some of this poller's file descriptor */
            update_date_cache(t);

            for (struct epoll_event *ep_event = events; n_fds--; ep_event++) {
                struct lwan_connection *conn;

                if (UNLIKELY(!ep_event->data.ptr)) {
                    accept_nudge(read_pipe_fd, &t->pending_fds, lwan->conns,
                        &dq, &switcher, epoll_fd);
                    continue;
                }

                conn = ep_event->data.ptr;

                if (UNLIKELY(ep_event->events & (EPOLLRDHUP | EPOLLHUP))) {
                    destroy_coro(&dq, conn);
                    continue;
                }

                resume_coro_if_needed(&dq, conn, epoll_fd);
                death_queue_move_to_last(&dq, conn);
            }
        }
    }

epoll_fd_closed:
    pthread_barrier_wait(&lwan->thread.barrier);

    death_queue_kill_all(&dq);
    free(events);

    return NULL;
}

static void
create_thread(struct lwan *l, struct lwan_thread *thread)
{
    pthread_attr_t attr;

    memset(thread, 0, sizeof(*thread));
    thread->lwan = l;

    if ((thread->epoll_fd = epoll_create1(EPOLL_CLOEXEC)) < 0)
        lwan_status_critical_perror("epoll_create");

    if (pthread_attr_init(&attr))
        lwan_status_critical_perror("pthread_attr_init");

    if (pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM))
        lwan_status_critical_perror("pthread_attr_setscope");

    if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE))
        lwan_status_critical_perror("pthread_attr_setdetachstate");

#if defined(HAVE_EVENTFD)
    int efd = eventfd(0, EFD_NONBLOCK | EFD_SEMAPHORE);
    if (efd < 0)
        lwan_status_critical_perror("eventfd");

    thread->pipe_fd[0] = thread->pipe_fd[1] = efd;
#else
    if (pipe2(thread->pipe_fd, O_NONBLOCK | O_CLOEXEC) < 0)
        lwan_status_critical_perror("pipe");
#endif

    struct epoll_event event = { .events = EPOLLIN, .data.ptr = NULL };
    if (epoll_ctl(thread->epoll_fd, EPOLL_CTL_ADD, thread->pipe_fd[0], &event) < 0)
        lwan_status_critical_perror("epoll_ctl");

    if (pthread_create(&thread->self, &attr, thread_io_loop, thread))
        lwan_status_critical_perror("pthread_create");

    if (pthread_attr_destroy(&attr))
        lwan_status_critical_perror("pthread_attr_destroy");

    if (spsc_queue_init(&thread->pending_fds, thread->lwan->thread.max_fd) < 0)
        lwan_status_critical("Could not initialize pending fd queue");
}

void
lwan_thread_add_client(struct lwan_thread *t, int fd)
{
    t->lwan->conns[fd] = (struct lwan_connection) { .thread = t };

    if (!spsc_queue_push(&t->pending_fds, (void *)(intptr_t)fd))
        lwan_status_error("spsc_queue_push");
}

void
lwan_thread_nudge(struct lwan_thread *t)
{
    uint64_t event = 1;

    if (UNLIKELY(write(t->pipe_fd[1], &event, sizeof(event)) < 0))
        lwan_status_perror("write");
}

void
lwan_thread_init(struct lwan *l)
{
    if (pthread_barrier_init(&l->thread.barrier, NULL, (unsigned)l->thread.count + 1))
        lwan_status_critical("Could not create barrier");

    lwan_status_debug("Initializing threads");

    l->thread.threads = calloc((size_t)l->thread.count, sizeof(struct lwan_thread));
    if (!l->thread.threads)
        lwan_status_critical("Could not allocate memory for threads");

    for (short i = 0; i < l->thread.count; i++)
        create_thread(l, &l->thread.threads[i]);

    pthread_barrier_wait(&l->thread.barrier);

    lwan_status_debug("IO threads created and ready to serve");
}

void
lwan_thread_shutdown(struct lwan *l)
{
    lwan_status_debug("Shutting down threads");

    for (int i = 0; i < l->thread.count; i++) {
        struct lwan_thread *t = &l->thread.threads[i];

        close(t->epoll_fd);
        lwan_thread_nudge(t);
    }

    pthread_barrier_wait(&l->thread.barrier);
    pthread_barrier_destroy(&l->thread.barrier);

    for (int i = 0; i < l->thread.count; i++) {
        struct lwan_thread *t = &l->thread.threads[i];

        close(t->pipe_fd[0]);
#if !defined(HAVE_EVENTFD)
        close(t->pipe_fd[1]);
#endif

        pthread_join(l->thread.threads[i].self, NULL);
        spsc_queue_free(&t->pending_fds);
    }

    free(l->thread.threads);
}
