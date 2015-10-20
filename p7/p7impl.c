#include    "./p7impl.h"
#include    "./p7_root_alloc.h"
#include    "../include/model_alloc.h"

// XXX WTF I've written. Get rid of these pieces of CRAP whenever possible.
// XXX UPDATE. These are still crap.

#define     atom_fetch_int32(_x_) \
({ \
    __atomic_load_n(&(_x_), __ATOMIC_SEQ_CST); \
})

#define     atom_store_int32(_d_, _x_) \
({ \
    __atomic_store_n(&(_d_), (_x_), __ATOMIC_SEQ_CST); \
})

#define     atom_add_uint32(_x_) \
({ \
    __atomic_add_fetch(&(_x_), 1, __ATOMIC_SEQ_CST); \
})

static int p7_timer_compare(const void *ev1, const void *ev2);

static struct p7_carrier **carriers = NULL;
static unsigned next_carrier = 0, ncarriers = 1;
static __thread struct p7_carrier *self_view = NULL;

static uint8_t active_queue_at[256] = { [0] = 0, [255] = 1 };

static
struct p7_coro_cntx *p7_coro_cntx_new_(void (*entry)(void *), void *arg, size_t stack_size, struct p7_limbo *limbo) {
    __auto_type allocator = local_root_alloc_get_proxy();
    struct p7_coro_cntx *cntx = scraft_allocate(allocator, (sizeof(struct p7_coro_cntx) + sizeof(void *) * stack_size + STACK_RESERVED_SIZE));
    if (cntx != NULL) {
        cntx->stack_size = stack_size;
        if (stack_size > 0)
            (cntx->uc.uc_stack.ss_sp = cntx->stack), (cntx->uc.uc_stack.ss_size = stack_size);
        cntx->uc.uc_link = (limbo != NULL) ? &(limbo->cntx->uc) : NULL;
        // TODO invalidate reserved area of coro's stack
        if (entry != NULL) {
            getcontext(&(cntx->uc));
            makecontext(&(cntx->uc), (void (*)()) entry, 1, arg);
        }
    }
    return cntx;
}

static
void p7_coro_cntx_delete_(struct p7_coro_cntx *cntx) {
    __auto_type allocator = local_root_alloc_get_proxy();
    scraft_deallocate(allocator, cntx);
}

static
struct p7_coro *p7_coro_new_(void (*entry)(void *), void *arg, size_t stack_size, unsigned carrier_id, struct p7_limbo *limbo) {
    struct p7_coro *coro = NULL;
    struct p7_coro_cntx *cntx = p7_coro_cntx_new_(entry, arg, stack_size, limbo);
    if (cntx != NULL) {
        __auto_type allocator = local_root_alloc_get_proxy();
        coro = scraft_allocate(allocator, (sizeof(struct p7_coro)));
        if (coro != NULL) {
            (coro->carrier_id = carrier_id), (coro->cntx = cntx), (coro->following = NULL);
            (coro->func_info.entry = entry), (coro->func_info.arg = arg);
            coro->timedout = 0;
        }
    }
    return coro;
}

static
void p7_coro_delete_(struct p7_coro *coro) {
    p7_coro_cntx_delete_(coro->cntx);
    __auto_type allocator = local_root_alloc_get_proxy();
    scraft_deallocate(allocator, coro);
}

static
struct p7_coro_cntx *p7_coro_cntx_new(void (*entry)(void *), void *arg, size_t stack_size, struct p7_limbo *limbo) {
    return p7_coro_cntx_new_(entry, arg, stack_size, limbo);
}

static
struct p7_coro *p7_coro_new(void (*entry)(void *), void *arg, size_t stack_size, unsigned carrier_id, struct p7_limbo *limbo) {
    if (!list_is_empty(&(self_view->sched_info.coro_pool_tl))) {
        list_ctl_t *cororef = self_view->sched_info.coro_pool_tl.next;
        list_del(cororef);
        self_view->sched_info.coro_pool_size--;
        struct p7_coro *coro = container_of(cororef, struct p7_coro, lctl);
        (coro->func_info.arg = arg), (coro->func_info.entry = entry), (coro->carrier_id = carrier_id);
        if (stack_size <= coro->cntx->stack_size) {
            coro->cntx->uc.uc_stack.ss_sp = coro->cntx->stack;
            coro->cntx->uc.uc_link = (limbo != NULL) ? &(limbo->cntx->uc) : NULL;
            getcontext(&(coro->cntx->uc));
            makecontext(&(coro->cntx->uc), (void (*)()) entry, 1, arg);
        } else {
            p7_coro_cntx_delete_(coro->cntx);
            coro->cntx = p7_coro_cntx_new(entry, arg, stack_size, limbo);
        }
        return coro;
    } else {
        return p7_coro_new_(entry, arg, stack_size, carrier_id, limbo);
    }
}

static
void p7_coro_delete(struct p7_coro *coro) {
    if (self_view->sched_info.coro_pool_size < self_view->sched_info.coro_pool_cap) {
        self_view->sched_info.coro_pool_size++;
        coro->following = NULL;
        list_add_tail(&(coro->lctl), &(self_view->sched_info.coro_pool_tl));
    } else {
        p7_coro_delete_(coro);
    }
}

static
struct p7_limbo *p7_limbo_new(void (*entry)(void *)) {
    struct p7_coro_cntx *cntx = p7_coro_cntx_new(entry, NULL, 1024, NULL);
    struct p7_limbo *limbo = NULL;
    if (cntx != NULL) {
        __auto_type allocator = local_root_alloc_get_proxy();
        limbo = scraft_allocate(allocator, sizeof(struct p7_limbo));
        if (limbo == NULL) {
            p7_coro_cntx_delete_(cntx);
            return NULL;
        }
        (limbo->entry = entry), (limbo->cntx = cntx);
    }
    return limbo;
}

static
struct p7_carrier *p7_carrier_prepare(unsigned carrier_id, unsigned nevents, void (*limbo_entry)(void *), void (*entry)(void *), void *arg) {
    __auto_type allocator = local_root_alloc_get_proxy();
    struct p7_carrier *carrier = scraft_allocate(allocator, sizeof(struct p7_carrier));
    struct epoll_event *evqueue = scraft_allocate(allocator, sizeof(struct epoll_event) * nevents);
    struct p7_limbo *limbo = p7_limbo_new(limbo_entry);
    void local_free(void *ptr) {
        scraft_deallocate(allocator, ptr);
    }
    if ( (carrier != NULL) && (evqueue != NULL) && (limbo != NULL) ) {
        carrier->carrier_id = carrier_id;
        carrier->sched_info.running = NULL;
        (carrier->startup.at_startup = NULL), (carrier->startup.arg_startup = NULL);
        carrier->sched_info.active_idx = P7_ACTIVE_IDX_MAGIC;
        init_list_head(&(carrier->sched_info.coro_queue));
        init_list_head(&(carrier->sched_info.blocking_queue));
        init_list_head(&(carrier->sched_info.rq_pool_tl));
        init_list_head(&(carrier->sched_info.coro_pool_tl));
        init_list_head(&(carrier->sched_info.waitk_pool_tl));
        init_list_head(&(carrier->sched_info.rq_queues[0]));
        init_list_head(&(carrier->sched_info.rq_queues[1]));
        carrier->sched_info.rq_pool_size = carrier->sched_info.coro_pool_size = carrier->sched_info.waitk_pool_size = 0;
        (carrier->sched_info.rq_pool_cap = P7_RQ_POOL_CAP), (carrier->sched_info.coro_pool_cap = P7_CORO_POOL_CAP), (carrier->sched_info.waitk_pool_cap = P7_WAITK_POOL_CAP);
        carrier->sched_info.active_rq_queue = &(carrier->sched_info.rq_queues[0]);
        carrier->sched_info.local_rq_queue = &(carrier->sched_info.rq_queues[1]);
        pthread_spin_init(&(carrier->sched_info.mutex), PTHREAD_PROCESS_PRIVATE);
        pthread_spin_init(&(carrier->sched_info.rq_pool_lock), PTHREAD_PROCESS_PRIVATE);
        pthread_spin_init(&(carrier->sched_info.waitk_pool_lock), PTHREAD_PROCESS_PRIVATE);
        pthread_spin_init(&(carrier->sched_info.rq_queue_lock), PTHREAD_PROCESS_PRIVATE);
        carrier->sched_info.timer_heap = heap_create(p7_timer_compare);
        (carrier->mgr_cntx.limbo = limbo), (carrier->mgr_cntx.sched = p7_coro_cntx_new(entry, arg, 1024 * 2, NULL)), 
        carrier->iomon_info.maxevents = nevents;
        init_list_head(&(carrier->iomon_info.queue));
        carrier->iomon_info.events = evqueue;
        carrier->iomon_info.epfd = epoll_create(nevents);
        carrier->iomon_info.is_blocking = 0;
        pipe(carrier->iomon_info.condpipe);
        int fdflags = fcntl(carrier->iomon_info.condpipe[0], F_GETFL, 0);
        fcntl(carrier->iomon_info.condpipe[0], F_SETFL, fdflags|O_NONBLOCK);
    } else {
#define free_safe(_p_) ({ do { if ((_p_) != NULL) local_free(_p_); } while (0); 0; })
        free_safe(carrier), free_safe(evqueue), free_safe(limbo);
    }
    if ( (carrier != NULL) && (carrier->iomon_info.epfd == -1) ) {
        free_safe(carrier), free_safe(evqueue), free_safe(limbo);
    }
#undef free_safe
    return carrier;
}

static 
void p7_carrier_atexit(struct p7_carrier *self) {
    // stub
    // TODO implementation
}

static
struct p7_coro_rq *p7_coro_rq_new_(void (*entry)(void *), void *arg, size_t stack_size) {
    __auto_type allocator = local_root_alloc_get_proxy();
    struct p7_coro_rq *rq = scraft_allocate(allocator, sizeof(struct p7_coro_rq));
    if (rq != NULL) {
        (rq->func_info.entry = entry), (rq->func_info.arg = arg);
        (rq->stack_info.stack_unit_size = sizeof(void *)), (rq->stack_info.stack_nunits = stack_size);
        rq->from = self_view->carrier_id;
    }
    return rq;
}

static
void p7_coro_rq_delete_(struct p7_coro_rq *rq) {
    __auto_type allocator = local_root_alloc_get_proxy();
    scraft_deallocate(allocator, rq);
}

static
struct p7_coro_rq *p7_coro_rq_new(void (*entry)(void *), void *arg, size_t stack_size) {
    pthread_spin_lock(&(self_view->sched_info.rq_pool_lock));
    if (!list_is_empty(&(self_view->sched_info.rq_pool_tl))) {
        list_ctl_t *rqref = self_view->sched_info.rq_pool_tl.next;
        list_del(rqref);
        self_view->sched_info.rq_pool_size--;
        pthread_spin_unlock(&(self_view->sched_info.rq_pool_lock));
        struct p7_coro_rq *rq = container_of(rqref, struct p7_coro_rq, lctl);
        (rq->stack_info.stack_unit_size = sizeof(void *)), (rq->stack_info.stack_nunits = stack_size);
        (rq->func_info.entry = entry), (rq->func_info.arg = arg);
        return rq;
    } else {
        pthread_spin_unlock(&(self_view->sched_info.rq_pool_lock));
        return p7_coro_rq_new_(entry, arg, stack_size);
    }
}

static
void p7_coro_rq_delete(struct p7_coro_rq *rq) {
    pthread_spin_lock(&(carriers[rq->from]->sched_info.rq_pool_lock));
    if (carriers[rq->from]->sched_info.rq_pool_size <= carriers[rq->from]->sched_info.rq_pool_cap) {
        carriers[rq->from]->sched_info.rq_pool_size++;
        list_add_tail(&(rq->lctl), &(carriers[rq->from]->sched_info.rq_pool_tl));
        pthread_spin_unlock(&(carriers[rq->from]->sched_info.rq_pool_lock));
    } else {
        pthread_spin_unlock(&(carriers[rq->from]->sched_info.rq_pool_lock));
        p7_coro_rq_delete_(rq);
    }
}

static
struct p7_waitk *p7_waitk_new_(void) {
    __auto_type allocator = local_root_alloc_get_proxy();
    struct p7_waitk *k = scraft_allocate(allocator, sizeof(struct p7_waitk));
    (k != NULL) && (k->from = self_view->carrier_id);
    return k;
}

static
void p7_waitk_delete_(struct p7_waitk *k) {
    __auto_type allocator = local_root_alloc_get_proxy();
    scraft_deallocate(allocator, k);
}

static
struct p7_waitk *p7_waitk_new(void) {
    pthread_spin_lock(&(self_view->sched_info.waitk_pool_lock));
    if (!list_is_empty(&(self_view->sched_info.waitk_pool_tl))) {
        list_ctl_t *waitkref = self_view->sched_info.waitk_pool_tl.next;
        list_del(waitkref);
        self_view->sched_info.waitk_pool_size--;
        pthread_spin_unlock(&(self_view->sched_info.waitk_pool_lock));
        return container_of(waitkref, struct p7_waitk, lctl);
    } else {
        pthread_spin_unlock(&(self_view->sched_info.waitk_pool_lock));
        return p7_waitk_new_();
    }
}

static
void p7_waitk_delete(struct p7_waitk *k) {
    pthread_spin_lock(&(carriers[k->from]->sched_info.waitk_pool_lock));
    if (self_view->sched_info.waitk_pool_size <= self_view->sched_info.waitk_pool_cap) {
        self_view->sched_info.waitk_pool_size++;
        list_add_tail(&(k->lctl), &(self_view->sched_info.waitk_pool_tl));
        pthread_spin_unlock(&(carriers[k->from]->sched_info.waitk_pool_lock));
    } else {
        pthread_spin_unlock(&(carriers[k->from]->sched_info.waitk_pool_lock));
        p7_waitk_delete_(k);
    }
}

static
uint64_t get_timeval_current(void) {
    //struct timeval tv;
    //gettimeofday(&tv, NULL);
    struct timespec tv;
    clock_gettime(CLOCK_REALTIME_COARSE, &tv);
    return ((uint64_t) tv.tv_sec * 1000) + ((uint64_t) tv.tv_nsec / 1000000);
}

static
uint64_t get_timeval_by_diff(uint64_t dt) {
    return get_timeval_current() + dt;
}

static
struct p7_timer_event *p7_timer_event_new_(uint64_t dt, unsigned from, struct p7_coro *coro, struct p7_cond_event *cond) {
    __auto_type allocator = local_root_alloc_get_proxy();
    struct p7_timer_event *ev = scraft_allocate(allocator, sizeof(struct p7_timer_event));
    if (ev != NULL)
        (ev->tval = get_timeval_by_diff(dt)), (ev->from = from), (ev->coro = coro), (ev->condref = cond);
    return ev;
}

static
void p7_timer_event_del_(struct p7_timer_event *ev) {
    if (ev->hook.dtor != NULL)
        ev->hook.dtor(ev->hook.arg, ev->hook.func);
    __auto_type allocator = local_root_alloc_get_proxy();
    scraft_deallocate(allocator, ev);
}

static
struct p7_timer_event *p7_timer_event_new(uint64_t dt, unsigned from, struct p7_coro *coro, struct p7_cond_event *cond) {
    return p7_timer_event_new_(dt, from, coro, cond);
}

static
void p7_timer_event_del(struct p7_timer_event *ev) {
    p7_timer_event_del_(ev);
}

static
void p7_timer_event_hook(struct p7_timer_event *ev, void (*func)(void *), void *arg, void (*dtor)(void *, void (*)(void *))) {
    (ev->hook.arg = arg), (ev->hook.func = func), (ev->hook.dtor = dtor);
}

static
int p7_timer_compare(const void *ev1, const void *ev2) {
    const struct p7_timer_event *timers[2] = { (const struct p7_timer_event *) ev1, (const struct p7_timer_event *) ev2 };
    return (timers[0]->tval == timers[1]->tval) ? 0 : ((timers[0]->tval < timers[1]->tval) ? -1 : 1);
}

static
void timer_add_event(struct p7_timer_event *ev, struct p7_minheap *heap) {
    heap_insert(ev, heap);
}

static
void timer_remove_event(struct p7_timer_event *ev, struct p7_minheap *heap) {
    heap_delete(ev, heap);
}

static
struct p7_timer_event *timer_peek_earliest(struct p7_minheap *heap) {
    return (struct p7_timer_event *) heap_peek_min(heap);
}

static
struct p7_timer_event *timer_extract_earliest(struct p7_minheap *heap) {
    return (struct p7_timer_event *) heap_extract_min(heap);
}

static
void limbo_loop(void *unused) {
    struct p7_carrier *self_carrier = self_view;
    while (1) {
        struct p7_coro *current = self_carrier->sched_info.running;
        list_del(&(current->lctl));
        if (current->following != NULL) {
            list_del(&(current->following->lctl));
            list_add_head(&(current->following->lctl), &(self_carrier->sched_info.coro_queue));
        }
        self_carrier->sched_info.running = NULL;
        p7_coro_delete(current);
        swapcontext(&(self_carrier->mgr_cntx.limbo->cntx->uc), &(self_carrier->mgr_cntx.sched->uc));
    }
}

static volatile uint32_t sched_loop_sync = 0;

static
void *sched_loop(void *arg) {
    struct p7_carrier *self = (struct p7_carrier *) arg;
    self_view = self;
    list_ctl_t rq_backlog;
    init_list_head(&(rq_backlog));

    struct p7_waitk local_cond_k;
    local_cond_k.fd = self->iomon_info.condpipe[0];
    struct epoll_event local_cond_ev;
    (local_cond_ev.events = EPOLLIN), (local_cond_ev.data.ptr = &local_cond_k);
    epoll_ctl(self->iomon_info.epfd, EPOLL_CTL_ADD, self->iomon_info.condpipe[0], &local_cond_ev);

    int idx_active = 0;

    // XXX sync
    atom_add_uint32(sched_loop_sync);
    while (sched_loop_sync < ncarriers);

    if (self->startup.at_startup != NULL)
        self->startup.at_startup(self->startup.arg_startup);

    while (1) {
        pthread_spin_lock(&(self->sched_info.mutex));
        int ep_timeout = (list_is_empty(&(self->sched_info.coro_queue)) && list_is_empty(&(self->sched_info.rq_queues[0])) && list_is_empty(&(self->sched_info.rq_queues[1]))) ? -1 : 0;
        pthread_spin_unlock(&(self->sched_info.mutex));
        struct p7_timer_event *ev_earliest = timer_peek_earliest(self->sched_info.timer_heap);
        uint64_t tval_before = get_timeval_current();
        if (ev_earliest != NULL) {
            if ((ev_earliest->tval > tval_before) && (ep_timeout < 0)) {
                ep_timeout = ev_earliest->tval - tval_before;
            }
        }
        atom_store_int32(self->iomon_info.is_blocking, 1);    // XXX slow but safe
        int nactive = epoll_wait(self->iomon_info.epfd, self->iomon_info.events, self->iomon_info.maxevents, ep_timeout);
        atom_store_int32(self->iomon_info.is_blocking, 0);
        // XXX expire ALL available timers now. I know it's slow. 
        uint64_t tval_after = get_timeval_current();
        struct p7_timer_event *ev_timer_itr = NULL;
        while ((ev_timer_itr = timer_peek_earliest(self->sched_info.timer_heap)) != NULL) {
            if (ev_timer_itr->tval > tval_after)
                break;
            else {
                struct p7_timer_event *ev_timer_expired = timer_extract_earliest(self->sched_info.timer_heap);
                if (ev_timer_expired->hook.func != NULL)
                    ev_timer_expired->hook.func(ev_timer_expired->hook.arg);
                if (ev_timer_expired->coro != NULL) {
                    __auto_type old_timedout = ev_timer_expired->coro->timedout;
                    ev_timer_expired->coro->timedout = 1;
                    if (old_timedout == 0)
                        list_add_head(&(ev_timer_expired->coro->lctl), &(self->sched_info.coro_queue));
                }
                if (ev_timer_expired->condref != NULL) {
                    // TODO condref
                }
                p7_timer_event_del(ev_timer_expired);
            }
        }
        int ep_itr;
        for (ep_itr = 0; ep_itr < nactive; ep_itr++) {
            struct p7_waitk *kwrap = (struct p7_waitk *) self->iomon_info.events[ep_itr].data.ptr;
            if (kwrap->fd != self->iomon_info.condpipe[0]) {
                // XXX be it slower when active connections are many.
                if (epoll_ctl(self->iomon_info.epfd, EPOLL_CTL_DEL, kwrap->fd, NULL) != -1) {
                    if (kwrap->coro->timedout == 0)
                        list_add_head(&(kwrap->coro->lctl), &(self->sched_info.coro_queue));
                    else
                        kwrap->coro->timedout = 0;
                    p7_waitk_delete(kwrap);
                }
            } else {
                // XXX just a stub. need dispatching for both remote creating requests & conditional locks.
                char sink[128];
                while (read(self->iomon_info.condpipe[0], &sink, 64) > 0);
            }
        }

        list_ctl_t *p, *t, *h;
        /*
        pthread_spin_lock(&(self->sched_info.mutex));
        self->sched_info.active_rq_queue = &(self->sched_info.rq_queues[idx_active = 1 - idx_active]);
        self->sched_info.local_rq_queue = &(self->sched_info.rq_queues[1 - idx_active]);
        pthread_spin_unlock(&(self->sched_info.mutex));
        */

        h = &(self->sched_info.rq_queues[active_queue_at[__atomic_fetch_xor(&(self->sched_info.active_idx), (uint8_t) -1, __ATOMIC_SEQ_CST)]]);
        if (!list_is_empty(h)) {
            list_foreach_remove(p, h, t) {
                list_del(t);
                struct p7_coro_rq *rq = container_of(t, struct p7_coro_rq, lctl);
                struct p7_coro *coro = p7_coro_new(rq->func_info.entry, rq->func_info.arg, rq->stack_info.stack_nunits, self->carrier_id, self->mgr_cntx.limbo);
                list_add_tail(&(coro->lctl), &(self->sched_info.coro_queue));
                p7_coro_rq_delete(rq);
            }
        }

        // into the fight we leap
        // sched_info.running is only a "view"
        // XXX if we come back from limbo or iowrap, just pick the next coro
        if (!list_is_empty(&(self->sched_info.coro_queue))) {
            if (self->sched_info.running != NULL) {
                list_ctl_t *last_coro = self->sched_info.coro_queue.next;
                list_del(last_coro);
                list_add_tail(last_coro, &(self->sched_info.coro_queue));
            }
            list_ctl_t *next_coro = self->sched_info.coro_queue.next;
            self->sched_info.running = container_of(next_coro, struct p7_coro, lctl);
            swapcontext(&(self->mgr_cntx.sched->uc), &(self->sched_info.running->cntx->uc));
        }
    }

    p7_carrier_atexit(self);
    return NULL;
}

// NOTE this wrapper is used for the main thread, which needs a scheduler but holds no independent pthread
static
void sched_loop_cntx_wraper(void *unused_arg) {
    sched_loop(carriers[0]);
}

static
void coro_create_request(void (*entry)(void *), void *arg, size_t stack_size) {
    atom_add_uint32(next_carrier);
    struct p7_carrier *next_load = carriers[next_carrier % ncarriers];
    if (next_load->carrier_id != self_view->carrier_id) {
        struct p7_coro_rq *rq = p7_coro_rq_new(entry, arg, stack_size);
        if (rq != NULL) {
            /*
            pthread_spin_lock(&(next_load->sched_info.mutex));
            list_add_tail(&(rq->lctl), next_load->sched_info.active_rq_queue);
            pthread_spin_unlock(&(next_load->sched_info.mutex));
            */
            uint8_t active_index = active_queue_at[next_load->sched_info.active_idx];
            // TODO not-so-heavy performace loss
            pthread_spin_lock(&(next_load->sched_info.rq_queue_lock));
            list_add_tail(&(rq->lctl), &(next_load->sched_info.rq_queues[active_index]));
            pthread_spin_unlock(&(next_load->sched_info.rq_queue_lock));
            if (atom_fetch_int32(next_load->iomon_info.is_blocking)) {
                char wake = 'w';    // wwwwwwwwwwwwwwwwwwwwwwww
                write(next_load->iomon_info.condpipe[1], &wake, 1);
            }
        }
    } else {
        struct p7_coro *coro = p7_coro_new(entry, arg, stack_size, self_view->carrier_id, self_view->mgr_cntx.limbo);
        list_add_tail(&(coro->lctl), &(self_view->sched_info.coro_queue));
    }
}

struct p7_carrier *p7_carrier_self_tl(void) {
    return self_view;
}

unsigned p7_get_ncarriers(void) {
    return ncarriers;
}

// APIs begin here

void p7_timed_event(uint64_t dt, void (*func)(void *), void *arg, void (*dtor)(void *, void (*)(void *))) {
    struct p7_timer_event *ev = p7_timer_event_new_(dt, self_view->carrier_id, NULL, NULL);
    p7_timer_event_hook(ev, func, arg, dtor);
    timer_add_event(ev, self_view->sched_info.timer_heap);
}

struct p7_timer_event *p7_timed_event_assoc(uint64_t dt, void (*func)(void *), void *arg, void (*dtor)(void *, void (*)(void *))) {
    struct p7_timer_event *ev = p7_timer_event_new_(dt, self_view->carrier_id, self_view->sched_info.running, NULL);
    p7_timer_event_hook(ev, func, arg, dtor);
    timer_add_event(ev, self_view->sched_info.timer_heap);
    return ev;
}

unsigned p7_timedout_(void) {
    return self_view->sched_info.running->timedout;
}

unsigned p7_timeout_reset(void) {
    self_view->sched_info.running->timedout = 0;
}

void p7_timer_clean_(struct p7_timer_event *ev) {
    timer_remove_event(ev, self_view->sched_info.timer_heap);
    p7_timer_event_del(ev);
}

void p7_coro_yield(void) {
    struct p7_carrier *self = self_view;
    if (!list_is_empty(&(self->sched_info.coro_queue))) {
        list_ctl_t *last_coro = self->sched_info.coro_queue.next;
        list_del(last_coro);
        list_add_tail(last_coro, &(self->sched_info.coro_queue));
        list_ctl_t *next_coro = self->sched_info.coro_queue.next;
        self->sched_info.running = container_of(next_coro, struct p7_coro, lctl);
        swapcontext(&(container_of(last_coro, struct p7_coro, lctl)->cntx->uc), &(container_of(next_coro, struct p7_coro, lctl)->cntx->uc));
    } else {
        // XXX slow but safe
        struct p7_coro *self_coro = self->sched_info.running;
        swapcontext(&(self_coro->cntx->uc), &(self->mgr_cntx.sched->uc));
    }
}

void p7_coro_create(void (*entry)(void *), void *arg, size_t stack_size) {
    coro_create_request(entry, arg, stack_size);
    p7_coro_yield();
}

void p7_coro_concat(void (*entry)(void *), void *arg, size_t stack_size) {
    struct p7_coro *coro = p7_coro_new(entry, arg, stack_size, self_view->carrier_id, self_view->mgr_cntx.limbo), *last = self_view->sched_info.running;
    coro->following = last;
    list_del(&(last->lctl));
    list_add_tail(&(last->lctl), &(self_view->sched_info.blocking_queue));
    list_add_head(&(coro->lctl), &(self_view->sched_info.coro_queue));
    self_view->sched_info.running = NULL;
    swapcontext(&(last->cntx->uc), &(self_view->mgr_cntx.sched->uc));
}

int p7_iowrap_(int fd, int rdwr) {
    struct p7_waitk *k = p7_waitk_new();
    if (k == NULL)
        return -1;
    k->coro = self_view->sched_info.running;
    //k->fd = -1;
    k->fd = fd;
    (k->event.data.ptr = k), (k->event.events = EPOLLONESHOT);
    ((rdwr & P7_IOMODE_READ) && (k->event.events |= EPOLLIN)), ((rdwr & P7_IOMODE_WRITE) && (k->event.events |= EPOLLOUT));
    (rdwr & P7_IOCTL_ET) && (k->event.events |= EPOLLET);
    (rdwr & P7_IOMODE_ERROR) && (k->event.events |= EPOLLERR); // useless
    int ret;
    ret = epoll_ctl(self_view->iomon_info.epfd, EPOLL_CTL_ADD, fd, &(k->event));
    if (ret == -1) {
        int errsv = errno;
        __auto_type allocator = local_root_alloc_get_proxy();
        scraft_deallocate(allocator, k);
        return -1;
    }
    list_del(&(k->coro->lctl));
    self_view->sched_info.running = NULL;
    swapcontext(&(k->coro->cntx->uc), &(self_view->mgr_cntx.sched->uc));
    return ret;
    
}

int p7_init(unsigned nthreads, void (*at_startup)(void *), void *arg) {
    if (nthreads < 1)
        nthreads = 1;
    ncarriers = nthreads;
    __auto_type allocator = local_root_alloc_get_proxy();
    carriers = scraft_allocate(allocator, sizeof(struct p7_carrier *) * ncarriers);
    int carrier_idx;
    carriers[0] = p7_carrier_prepare(0, 1024, limbo_loop, sched_loop_cntx_wraper, NULL);
    self_view = carriers[0];
    for (carrier_idx = 1; carrier_idx < ncarriers; carrier_idx++)
        carriers[carrier_idx] = p7_carrier_prepare(carrier_idx, 1024, limbo_loop, NULL, NULL);
    struct p7_coro *main_ctlflow = p7_coro_new(NULL, NULL, 0, 0, NULL);
    getcontext(&(main_ctlflow->cntx->uc));
    list_add_tail(&(main_ctlflow->lctl), &(carriers[0]->sched_info.coro_queue));
    carriers[0]->sched_info.running = main_ctlflow;
    if (at_startup != NULL)
        (carriers[0]->startup.at_startup = at_startup), (carriers[0]->startup.arg_startup = arg);
    for (carrier_idx = 1; carrier_idx < ncarriers; carrier_idx++) {
        if (at_startup != NULL)
            (carriers[carrier_idx]->startup.at_startup = at_startup), (carriers[carrier_idx]->startup.arg_startup = arg);
        pthread_create(&(carriers[carrier_idx]->tid), NULL, sched_loop, carriers[carrier_idx]);
    }
    swapcontext(&(main_ctlflow->cntx->uc), &(carriers[0]->mgr_cntx.sched->uc));
    return 0;
}
