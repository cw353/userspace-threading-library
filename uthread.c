#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "uthread.h"
#include "bitvec.h"
#include "kthread.h"
#include "queue.h"
#include "ucontext.h"
#include "valgrind.h"

// reference: https://www.openwall.com/lists/musl/2019/08/01/8
#define sigev_notify_thread_id _sigev_un._tid

// synchronized shared data
static kthread_sem_t inited_sem;
static kthread_sem_t exitall_sem;

static struct ready_queue rqueue;

static bitvec_t bitvec; // bit i is 1 if tid i is in use and 0 otherwise
static kthread_mutex_t bitvec_lock;

static uthread_tcb_t *tcbs[UTHREAD_MAX_THREADS]; // user thread PCBs
static kthread_mutex_t tcbs_lock;

static ucontext_t abort_ctx;

static uthread_tcb_t exitall; // to signal teardown


// shared data that doesn't need to be synchronized
static int inited = 0;
static sig_atomic_t quantum;
static uthread_kinfo_t **kthread_info;
static size_t num_kthreads;
static unsigned int MAIN_KTHREAD_INDEX = 0;
static sigset_t sigrtmin_mask;

uthread_kinfo_t *
get_kthread_info(kthread_t ktid)
{
	uthread_kinfo_t *kinfo = NULL;
	for (int i = 0; i < num_kthreads; i++) {
		if (kthread_info[i]->ktid == ktid) {
			kinfo = kthread_info[i];
		}
	}
	assert(kinfo);
	return kinfo;
}

ucontext_t *
get_sched_ctx()
{
	uthread_kinfo_t *kinfo = get_kthread_info(kthread_self());
	return &kinfo->sched_info.sched_ctx;
}

void
block_sigrtmin()
{
	if (quantum) {
		if (kthread_sigmask(SIG_BLOCK, &sigrtmin_mask, NULL)) {
			perror("kthread_sigmask");
			abort();
		}
	}
}

void
unblock_sigrtmin()
{
	if (quantum) {
		if (kthread_sigmask(SIG_UNBLOCK, &sigrtmin_mask, NULL)) {
			perror("kthread_sigmask");
			abort();
		}
	}
}

void
sigrtmin_handler(int sig)
{
	if (inited && quantum) {
		uthread_kinfo_t *kinfo = get_kthread_info(kthread_self());
		kinfo->preempted = 1;
	}
}

/*
 * Check if the current thread has been preempted and handle if so.
 */
void
check_preempted()
{
	uthread_kinfo_t *kinfo = get_kthread_info(kthread_self());
	if (quantum && kinfo->preempted) {
		DPRINTF("kthread %d has been preempted\n", kthread_self());
		kinfo->preempted = 0;
		uthread_yield();
	}
}

void
set_timer_interrupt(kthread_t kid)
{
	uthread_kinfo_t *kinfo = get_kthread_info(kid);

	struct sigevent sev;
	struct itimerspec its;

	// set up timer signal handler
	sev.sigev_notify = SIGEV_THREAD_ID;
	sev.sigev_notify_thread_id = kid;
	sev.sigev_signo = SIGRTMIN;
	sev.sigev_value.sival_ptr = &kinfo->timer_id;

	if (signal(SIGRTMIN, sigrtmin_handler) == SIG_ERR) {
		perror("signal");
		abort();
	}

	// create timer
	if (timer_create(CLOCK_REALTIME, &sev, &kinfo->timer_id)) {
		perror("timer_create");
		abort();
	}

	// start timer
	its.it_value.tv_sec = 0;
	its.it_value.tv_nsec = quantum;
	its.it_interval.tv_sec = its.it_value.tv_sec;
	its.it_interval.tv_nsec = its.it_value.tv_nsec;

	if (timer_settime(kinfo->timer_id, 0, &its, NULL) < 0) {
		perror("timer_settime");
		abort();
	}
}

void *
kthread_main(void *arg)
{
	if (kthread_sem_wait(&inited_sem)) {
		perror("kthread_sem_wait");
		abort();
	}

	block_sigrtmin();

	assert(inited);
	if (quantum) {
		set_timer_interrupt(kthread_self());
	}
	if (setcontext(get_sched_ctx())) {
		perror("setcontext");
		abort();
	}
	return NULL;
}

int
get_next_tid()
{
	int ret;

	if (kthread_mutex_lock(&bitvec_lock)) {
		perror("kthread_mutex_lock");
		abort();
	}
	int tid = bitvec_first_cleared_index(&bitvec);
	if (tid < 0) {
		// no tid not in use found
		ret = 1;
	} else {
		// mark tid as in use
		bitvec_set(&bitvec, tid);
		ret = tid + UTHREAD_TID_MAIN;
	}
	if (kthread_mutex_unlock(&bitvec_lock)) {
		perror("kthread_mutex_unlock");
		abort();
	}
	return ret;
}

void
lock_tcbs()
{
	if (kthread_mutex_lock(&tcbs_lock)) {
		perror("tcbs_lock");
		abort();
	}
}

void
unlock_tcbs()
{
	if (kthread_mutex_unlock(&tcbs_lock)) {
		perror("tcbs_unlock");
		abort();
	}
}

void
reclaim_tid(tid_t tid)
{
	if (kthread_mutex_lock(&bitvec_lock)) {
		perror("kthread_mutex_lock");
		abort();
	}
	bitvec_clear(&bitvec, tid - UTHREAD_TID_MAIN);
	if (kthread_mutex_unlock(&bitvec_lock)) {
		perror("kthread_mutex_unlock");
		abort();
	}
}

void
ready_enqueue(uthread_tcb_t *tcb)
{
	if (kthread_mutex_lock(&rqueue.lock)) {
		perror("kthread_mutex_lock");
		abort();
	}
	if (queue_enqueue(&rqueue.queue, tcb)) {
		perror("queue_enqueue");
		abort();
	}
	if (kthread_mutex_unlock(&rqueue.lock)) {
		perror("kthread_mutex_unlock");
		abort();
	}
	if (kthread_sem_post(&rqueue.not_empty)) {
		perror("kthread_sem_post");
		abort();
	}
}

uthread_tcb_t *
ready_dequeue()
{
	if (kthread_sem_wait(&rqueue.not_empty)) {
		perror("kthread_sem_wait");
		abort();
	}

	if (kthread_mutex_lock(&rqueue.lock)) {
		perror("kthread_mutex_lock");
		abort();
	}

	assert(queue_size(&rqueue.queue) > 0);
	uthread_tcb_t *tcb = queue_dequeue(&rqueue.queue);
	assert(tcb);

	if (kthread_mutex_unlock(&rqueue.lock)) {
		perror("kthread_mutex_lock");
		abort();
	}

	return tcb;
}

/**
 * Allocate the stack for the provided ucontext_t.
 * @param stack       A pointer to the thread's stack
 * @param stack_base  Location of the thread's stack if already allocated, or
 *                    NULL if requesting that the library allocate it
 *                    dynamically
 * @param stack_size  Size (in bytes) of the thread's stack, or 0 to use the
 *                    default thread stack size UTHREAD_DEF_STACK_SIZE
 * POSTCONDITION: The stack has been allocated and registered with valgrind.
 */
int
allocate_stack(stack_t *stack, caddr_t stack_base, size_t stack_size)
{
	stack->ss_size = stack_size ? stack_size : UTHREAD_DEF_STACK_SIZE;
	stack->ss_sp = stack_base ? stack_base : malloc(stack->ss_size);
	if (!(stack->ss_sp)) {
		perror("malloc");
		return 1;
	}
	stack->ss_flags = 0;
	VALGRIND_STACK_REGISTER(stack->ss_sp, stack->ss_sp + stack->ss_size);
	return 0;
}

/* 
 * Destroy a tcb.
 * Precondition: tcbs[tid] != NULL.
 */
void
destroy_thread(tid_t tid)
{
	queue_destroy(&tcbs[tid]->join_queue);
	if (tcbs[tid]->stack_allocated) {
		free(tcbs[tid]->ctx.uc_stack.ss_sp);
	}
	free(tcbs[tid]);
	tcbs[tid] = NULL;
}

/**
 * Thread trampoline function.
 */
void
trampoline(void *(*start_func)(void *), void *arg)
{
	lock_tcbs();
	assert(tcbs[uthread_self()]->state == RUNNING);
	unlock_tcbs();
	// run start_func and pass return value to uthread_exit
	uthread_exit(start_func(arg));
}

/**
 * Schedule the next thread.
 * Precondition: if sched_info.task != NONE, then this kernel thread
 * is already holding tcbs_lock.
 */
void
schedule()
{

	block_sigrtmin();

	uthread_kinfo_t *kinfo = get_kthread_info(kthread_self());
	int current_tid = uthread_self();
	uthread_tcb_t *current_tcb = current_tid >= 0 ? tcbs[current_tid] : NULL;

	assert(kthread_self() == kinfo->ktid);

	// handle any tasks assigned to the scheduler (if any)
	// (to avoid race conditions)
	switch(kinfo->sched_info.task) {
		case NONE:
			break;
		case YIELD:
			ready_enqueue(current_tcb);
			unlock_tcbs();
			break;
		case EXIT:
			// if another thread is waiting to join on this thread,
			// return it to the ready queue
			if (queue_size(&current_tcb->join_queue) > 0) {
				uthread_tcb_t *join_tcb = queue_dequeue(&current_tcb->join_queue);
				assert(join_tcb && join_tcb->tid != current_tid && join_tcb->state == WAITING_JOIN);
				join_tcb->state = READY;
				ready_enqueue(join_tcb);
			}
			unlock_tcbs();
			break;
		case JOIN:
			if (queue_enqueue(kinfo->sched_info.queue, current_tcb)) {
				perror("queue_enqueue");
				abort();
			}
			unlock_tcbs();
			break;
		case SEM_WAIT:
			if (queue_enqueue(kinfo->sched_info.queue, current_tcb)) {
				perror("queue_enqueue");
				abort();
			}
			if (kthread_mutex_unlock(kinfo->sched_info.lock)) {
				perror("kthread_mutex_unlock");
				abort();
			}
			unlock_tcbs();
			break;
		case TEARDOWN:
			current_tcb->state = READY;
			ready_enqueue(current_tcb);
			unlock_tcbs();
			break;
		default:
			DPRINTF("schedule: invalid task");
			abort();
			break;
	}
	kinfo->sched_info.task = NONE;
	kinfo->sched_info.lock = NULL;
	kinfo->sched_info.queue = NULL;

	// get next thread from ready queue (fcfs)
	uthread_tcb_t *next_tcb = ready_dequeue();

	// handle teardown
	if (next_tcb == &exitall) {
		// if this is the main kthread, wait for the other kthreads to exit,
		// then call schedule() to schedule the tcb that is in the middle of
		// teardown and has been placed in the ready queue
		if (kthread_self() == kthread_info[MAIN_KTHREAD_INDEX]->ktid) {
			for (int i = 0; i < num_kthreads-1; i++) {
				if (kthread_sem_wait(&exitall_sem)) {
					perror("kthread_sem_wait");
					abort();
				}
			}
			schedule();
		} else {
			// otherwise, exit
			if (kthread_sem_post(&exitall_sem)) {
				perror("kthread_sem_post");
				abort();
			}
			kthread_exit();
		}
	}

	lock_tcbs();

	// update current tid for this kthread
	get_kthread_info(kthread_self())->current_tid = next_tcb->tid;

	// update thread state
	assert(next_tcb->state == READY);
	next_tcb->state = RUNNING;

	unlock_tcbs();

	kinfo->preempted = 0;
	unblock_sigrtmin();

	// schedule thread
	if (setcontext(&next_tcb->ctx)) {
		perror("setcontext");
		abort();
	}
}

/**
 * Initializes the uthread library.  Programs are required to call this function
 * before they may use anything else in the library's public interface.
 *
 * @param kthreads    Number of kernel threads (pthreads) to allocate
 * @param quantum_us  Preemption timeslice in microseconds, or 0 for cooperative
 *                    scheduling
 *
 * @return 0 if successful, nonzero on failure
 */
int
uthread_init(int kthreads, int quantum_us)
{
	assert(!inited);

	num_kthreads = kthreads;
	quantum = quantum_us;

	if (sigemptyset(&sigrtmin_mask)) {
		perror("sigemptyset");
		abort();
	}
	if (sigaddset(&sigrtmin_mask, SIGRTMIN)) {
		perror("sigaddset");
		abort();
	}

	block_sigrtmin();

	if (kthread_sem_init(&inited_sem, 0)) {
		perror("kthread_sem_init");
		abort();
	}

	if (kthread_sem_init(&exitall_sem, 0)) {
		perror("kthread_sem_init");
		abort();
	}

	if (kthread_mutex_init(&tcbs_lock)) {
		perror("kthread_mutex_init");
		abort();
	}

	if (bitvec_init(&bitvec, UTHREAD_MAX_THREADS)) {
		perror ("bitvec_init");
		abort();
	}
	if (kthread_mutex_init(&bitvec_lock)) {
		perror("kthread_mutex_init");
		abort();
	}

	if (queue_init(&rqueue.queue)) {
		perror ("queue_init");
		abort();
	}
	if (kthread_mutex_init(&rqueue.lock)) {
		perror("kthread_mutex_init");
		abort();
	}
	if (kthread_sem_init(&rqueue.not_empty, 0)) {
		perror("kthread_sem_init");
		abort();
	}

	// initialize uthread_ctx for main thread
	int tid = get_next_tid();
	assert(tid >= 0);
	tcbs[tid] = malloc(sizeof(uthread_tcb_t));
	assert(tcbs[tid]);
	tcbs[tid]->stack_allocated = 0;
	tcbs[tid]->tid = tid;
	tcbs[tid]->state = RUNNING;
	if (queue_init(&tcbs[tid]->join_queue)) {
		perror("queue_init");
		abort();
	}

	// initialize abort_ctx
	if (getcontext(&abort_ctx)) {
		perror("getcontext");
		abort();
	}
	abort_ctx.uc_link = NULL;
	assert(!allocate_stack(&abort_ctx.uc_stack, NULL, 0));
	errno = 0;
	makecontext(&abort_ctx, (void (*)(void)) abort, 0);
	if (errno != 0) {
		perror("makecontext");
		abort();
	}

	// initialize kthread_info
	kthread_info = malloc(num_kthreads * sizeof(uthread_kinfo_t *));
	assert(kthread_info);

	// create kthread_info
	for (int i = 0; i < num_kthreads; i++) {
		kthread_info[i] = malloc(sizeof(uthread_kinfo_t));
		assert(kthread_info[i]);
		kthread_info[i]->ktid = i == 0 ? kthread_self() : -1;
		// assign current user tid
		kthread_info[i]->current_tid = i == 0 ? UTHREAD_TID_MAIN : -1;
		// initialize scheduler info
		kthread_info[i]->sched_info.task = NONE;
		kthread_info[i]->sched_info.lock = NULL;
		kthread_info[i]->sched_info.queue = NULL;
		if (getcontext(&kthread_info[i]->sched_info.sched_ctx)) {
			perror("getcontext");
			abort();
		}
		// successor context should never be reached (it aborts the program)
		kthread_info[i]->sched_info.sched_ctx.uc_link = &abort_ctx;
		assert(!allocate_stack(&kthread_info[i]->sched_info.sched_ctx.uc_stack, NULL, 0));
		errno = 0;
		makecontext(&kthread_info[i]->sched_info.sched_ctx, (void (*)(void)) schedule, 0);
		if (errno != 0) {
			perror("makecontext");
			abort();
		}
		kthread_info[i]->timer_id = 0;	
		kthread_info[i]->preempted = 0;
	}

	// create other kthreads
	for (int i = MAIN_KTHREAD_INDEX+1; i < num_kthreads; i++) {
		if (kthread_create(&kthread_info[i]->ktid, kthread_main, NULL)) {
			perror("kthread_create");
			abort();
		}
	}
	inited = 1;

	for (int i = 0; i < num_kthreads-1; i++) {
		if (kthread_sem_post(&inited_sem)) {
			perror("kthread_sem_post");
			abort();
		}
	}

	// set timer interrupt for main kthread
	if (quantum) {
		set_timer_interrupt(kthread_self());
	}

	unblock_sigrtmin();
	return 0;
}

/**
 * Cleans up any resources which were allocated by uthread_init.  You may assume
 * that this function is called only from the main thread, that any other
 * threads have terminated and been joined, and that threading will not be
 * needed again.  (In other words, just clean up and don't worry about the
 * consequences.)
 *
 * I won't be testing this function specifically, but it is provided as a
 * convenience to you if you are using Valgrind to check your code, which I
 * always encourage.
 */
void
uthread_teardown(void)
{
	assert(inited);
	block_sigrtmin();

	// if preemption is enabled, delete timers
	if (quantum) {
		for (int i = 0; i < num_kthreads; i++) {
			if (timer_delete(kthread_info[i]->timer_id)) {
				perror("timer_delete");
			}
		}
	}

	// signal all kthreads that teardown has been called
	for (int i = 0; i < num_kthreads; i++) {
		ready_enqueue(&exitall);
	}

	// enqueue this context into the ready queue so that the rest of 
	// this function will be executed by the main kthread
	uthread_kinfo_t *kinfo = get_kthread_info(kthread_self());
	kinfo->sched_info.task = TEARDOWN;
	lock_tcbs();
	if (swapcontext(&tcbs[uthread_self()]->ctx, get_sched_ctx())) {
		perror("swapcontext");
		abort();
	}

	// resume teardown (on the main kthread)
	kthread_t self = kthread_self();
	assert(self == kthread_info[MAIN_KTHREAD_INDEX]->ktid);

	// join kthreads except for main kthread
	for (int i = MAIN_KTHREAD_INDEX+1; i < num_kthreads; i++) {
		assert(kthread_info[i]->ktid != self);
		kthread_join(kthread_info[i]->ktid, NULL);
	}

	// destroy ready queue and its synchronization constructs
	while (kthread_mutex_destroy(&rqueue.lock) == EBUSY);
	do {
		errno = 0;
		kthread_sem_destroy(&rqueue.not_empty);
	} while (errno != 0);
	queue_destroy(&rqueue.queue);

	// destroy bitvector and its synchronization constructs
	bitvec_destroy(&bitvec);
	while (kthread_mutex_destroy(&bitvec_lock) == EBUSY);

	// destroy other synchronization constructs
	while (kthread_mutex_destroy(&tcbs_lock) == EBUSY);

	// free zombie threads (excluding UTHREAD_TID_MAIN, the context
	// in which this function is running)
	for (int i = UTHREAD_TID_MAIN + 1; i < UTHREAD_MAX_THREADS; i++) {
		if (tcbs[i]) {
			destroy_thread(i);
		}
	}

	// free abort_ctx
	free(abort_ctx.uc_stack.ss_sp);

	// free kthread_info
	for (int i = 0; i < num_kthreads; i++) {
		free(kthread_info[i]->sched_info.sched_ctx.uc_stack.ss_sp);
		free(kthread_info[i]);
	}
	free(kthread_info);

	if (kthread_sem_destroy(&inited_sem)) {
		perror("kthread_sem_destroy");
		abort();
	}
	if (kthread_sem_destroy(&exitall_sem)) {
		perror("kthread_sem_destroy");
		abort();
	}

	// free main thread
	destroy_thread(UTHREAD_TID_MAIN);

	unblock_sigrtmin();

	inited = 0;
}

/**
 * Creates a new user thread which executes the provided function concurrently.
 * It is left up to the implementation to decide whether the calling thread
 * continues to execute or the new thread takes over immediately.
 *
 * @param ptid[out]   Pointer to a tid_t variable in which to store the new
 *                    thread's ID
 * @param start_func  Thread main function
 * @param arg         Argument to be passed to the thread main function
 * @param stack_base  Location of the thread's stack if already allocated, or
 *                    NULL if requesting that the library allocate it
 *                    dynamically
 * @param stack_size  Size (in bytes) of the thread's stack, or 0 to use the
 *                    default thread stack size UTHREAD_DEF_STACK_SIZE
 *
 * @return 0 if successful, nonzero on failure
 */
int
uthread_create(tid_t *ptid, void *(*start_func)(void *), void *arg,
		caddr_t stack_base, size_t stack_size)
{
	assert(inited);
	block_sigrtmin();
	check_preempted();

	int ret = 0;

	// create new context
	tid_t new_tid = get_next_tid(); 
	if (new_tid < 0) {
		errno = EAGAIN;
		perror("get_next_tid");
		ret = 1;
		goto unblock_sigrtmin;
	}
	*ptid = new_tid;

	lock_tcbs();

	tcbs[new_tid] = malloc(sizeof(uthread_tcb_t));
	if (!tcbs[new_tid]) {
		perror("malloc");
		ret = 1;
		goto unblock_sigrtmin;
	}
	tcbs[new_tid]->tid = new_tid;
	tcbs[new_tid]->state = READY;
	if (queue_init(&tcbs[new_tid]->join_queue)) {
		perror("queue_init");
		abort();
	}
	if (getcontext(&tcbs[new_tid]->ctx)) {
		perror("getcontext");
		abort();
	}

	// allocate stack for new context
	if (allocate_stack(&tcbs[new_tid]->ctx.uc_stack, stack_base, stack_size)) {
		free(tcbs[new_tid]);
		ret = 1;
		goto unblock_sigrtmin;
	}
	tcbs[new_tid]->stack_allocated = stack_base ? 0 : 1;

	// successor context should never be reached (it aborts the program)
	tcbs[new_tid]->ctx.uc_link = &abort_ctx;

	// makecontext
	errno = 0;
	makecontext(&tcbs[new_tid]->ctx, (void (*)(void)) trampoline, 2, start_func, arg);
	if (errno != 0) {
		perror("makecontext");
		abort();
	}

	// add new context to ready queue
	ready_enqueue(tcbs[new_tid]);

	unlock_tcbs();

unblock_sigrtmin:
	unblock_sigrtmin();

	return ret;
}

/**
 * Exits the calling thread.  This should be the same thing that happens when
 * the thread's start_func returns.
 *
 * @param ret  Return value from the thread
 */
void
uthread_exit(void *ret)
{
	assert(inited);
	block_sigrtmin();
	check_preempted();

	// update thread state and save return value
	lock_tcbs();

	uthread_tcb_t *current_tcb = tcbs[uthread_self()];
	current_tcb->retval = ret;
	assert(current_tcb->state == RUNNING);
	current_tcb->state = FINISHED;

	// let scheduler know that the current user thread has requested to exit
	uthread_kinfo_t *kinfo = get_kthread_info(kthread_self());
	kinfo->sched_info.task = EXIT;

	// ask scheduler to schedule next thread
	if (setcontext(get_sched_ctx())) {
		perror("setcontext");
		abort();
	}
}

/**
 * Waits for the thread specified by tid to terminate, retrieving that threads
 * return value.  Returns immediately if the target thread has already
 * terminated, otherwise blocks.  Attempting to join a thread which already has
 * another thread waiting to join it, or attempting to join a thread which has
 * already been joined, results in undefined behavior.
 *
 * @param pret[out]  Pointer to a void * in which the thread's return value from
 *                   uthread_exit should be stored, or NULL if the caller does not
 *                   care.
 *
 * @return 0 if successful, nonzero on failure
 */
int
uthread_join(tid_t tid, void **pret)
{
	assert(inited);
	block_sigrtmin();
	check_preempted();

	tid_t current_tid = uthread_self();
	uthread_tcb_t *current_tcb = tcbs[current_tid];
	uthread_tcb_t *target_tcb = tcbs[tid]; 

	lock_tcbs();

	// Block if the target thread is not finished yet
	if (target_tcb->state != FINISHED) {

		assert(queue_size(&current_tcb->join_queue) == 0);

		// let scheduler know that the current user thread has requested to join
		uthread_kinfo_t *kinfo = get_kthread_info(kthread_self());

		kinfo->sched_info.task = JOIN;
		kinfo->sched_info.queue = &target_tcb->join_queue;

		current_tcb->state = WAITING_JOIN;

		// block by saving caller state and swapping to scheduler
		if (swapcontext(&current_tcb->ctx, get_sched_ctx())) {
			perror("swapcontext");
			abort();
		}

		// reaquire readlock and re-block sigrtmin
		block_sigrtmin();
		lock_tcbs();
	}

	// continue once the target thread has finished
	assert(tcbs[tid]->state == FINISHED);

	// Pass target thread's return value to caller
	if (pret) {
		*pret = tcbs[tid]->retval;
	}

	unlock_tcbs();

	// Clean up target thread's resources
	reclaim_tid(tid);
	destroy_thread(tid);
	unblock_sigrtmin();

	return 0;
}

/**
 * Returns a small integer which identifies the calling thread.
 *
 * @return Thread ID of the currently executing thread
 */
tid_t
uthread_self(void)
{
	assert(inited);
	return get_kthread_info(kthread_self())->current_tid;
}

/**
 * Causes the calling thread to yield the processor voluntarily.  This may
 * result in another thread being scheduled, but it does not preclude the
 * possibility of the same thread continuing if re-chosen by the scheduling
 * algorithm.
 */
void
uthread_yield(void)
{
	assert(inited);
	block_sigrtmin();

	// let scheduler know that the current user thread has requested to yield
	uthread_kinfo_t *kinfo = get_kthread_info(kthread_self());
	kinfo->sched_info.task = YIELD;

	lock_tcbs();
	uthread_tcb_t *current_tcb = tcbs[uthread_self()];
	assert(current_tcb->state == RUNNING);
	current_tcb->state = READY;

	// save caller state and swap to scheduler
	if (swapcontext(&current_tcb->ctx, get_sched_ctx())) {
		perror("swapcontext");
		abort();
	}
}

/**
 * Initializes a user-level counting semaphore with a specific value.
 *
 * @param sem    Pointer to the semaphore to be initialized
 * @param value  Initial value for the semaphore's counter
 *
 * @return 0 if successful, nonzero on failure
 */
int
uthread_sem_init(uthread_sem_t *sem, int value)
{
	assert(inited);
	block_sigrtmin();
	check_preempted();

	sem->counter = value;
	int ret = 0;
	if ((errno = kthread_mutex_init(&sem->lock))) {
		perror("kthread_mutex_init");
		ret =  1;
		goto unblock_sigrtmin;
	}
	if (queue_init(&sem->queue)) {
		perror("queue_init");
		ret = 1;
		goto unblock_sigrtmin;
	}

unblock_sigrtmin:
	unblock_sigrtmin();
	return ret;
}

/**
 * Increments the value of the semaphore.  This operation is also known as
 * up, signal, release, and V (Dutch verhoog, "increase").
 *
 * @param sem  Pointer to the semaphore which the thread is releasing
 *
 * @return 0 if successful, nonzero on failure
 */
int
uthread_sem_post(uthread_sem_t *sem)
{
	assert(inited);
	block_sigrtmin();
	check_preempted();

	int ret = 0;

	if (kthread_mutex_lock(&sem->lock)) {
		perror("kthread_mutex_lock");
		ret = 1;
		goto unblock_sigrtmin;
	}

	// increase counter
	sem->counter++;

	// if a thread has been waiting to decrement counter,
	// return it to the ready queue
	if (queue_size(&sem->queue) > 0) {

		uthread_tcb_t *tcb = queue_dequeue(&sem->queue);

		lock_tcbs();

		assert(tcb);
		assert(tcb->state == WAITING_SEM);
		tcb->state = READY;

		unlock_tcbs();

		ready_enqueue(tcb);
	}
	if (kthread_mutex_unlock(&sem->lock)) {
		perror("kthread_mutex_unlock");
		ret = 1;
		goto unblock_sigrtmin;
	}

unblock_sigrtmin:
	unblock_sigrtmin();
	return ret;
}

/**
 * Attempts to decrement the value of the semaphore.  This operation is also
 * known as down, acquire, and P (Dutch probeer, "try").  This operation should
 * block when the counter is not above 0.
 *
 * @param sem  Pointer to the semaphore which the thread wishes to acquire
 *
 * @return 0 if successful, nonzero on failure
 */
int
uthread_sem_wait(uthread_sem_t *sem)
{
	assert(inited);
	block_sigrtmin();
	check_preempted();

	int ret = 0;

	if (kthread_mutex_lock(&sem->lock)) {
		perror("kthread_mutex_lock");
		ret = 1;
		goto unblock_sigrtmin;
	}

	uthread_tcb_t *current_tcb = tcbs[uthread_self()];

	assert(sem->counter >= 0);

	// block if the counter is not above 0 (loop to recheck condition
	// once this context is resumed)
	while (sem->counter == 0) {
		uthread_kinfo_t *kinfo = get_kthread_info(kthread_self());
		kinfo->sched_info.task = SEM_WAIT;
		kinfo->sched_info.lock = &sem->lock;
		kinfo->sched_info.queue = &sem->queue;

		lock_tcbs();
		current_tcb->state = WAITING_SEM;

		// block by saving caller state and swapping to scheduler
		if (swapcontext(&current_tcb->ctx, get_sched_ctx())) {
			perror("swapcontext");
			abort();
		}

		// reacquire lock and re-block sigrtmin after this context is resumed
		block_sigrtmin();
		if (kthread_mutex_lock(&sem->lock)) {
			perror("kthread_mutex_lock");
			ret = 1;
			goto unblock_sigrtmin;
		}
	}

	// decrement the counter once it is above 0
	assert(sem->counter > 0);
	sem->counter--;

	if (kthread_mutex_unlock(&sem->lock)) {
		perror("kthread_mutex_unlock");
		ret = 1;
		goto unblock_sigrtmin;
	}

unblock_sigrtmin:
	unblock_sigrtmin();
	return ret;
}

/**
 * Frees any resources associated with a semaphore.  Destroying a semaphore on
 * which threads are waiting results in undefined behavior.
 *
 * @param sem  Pointer to the semaphore to be destroyed
 */
void
uthread_sem_destroy(uthread_sem_t *sem)
{
	assert(inited);
	check_preempted();
	block_sigrtmin();

	queue_destroy(&sem->queue);
	if (kthread_mutex_destroy(&sem->lock)) {
		perror("kthread_mutex_destroy\n");
	}
	unblock_sigrtmin();
}
