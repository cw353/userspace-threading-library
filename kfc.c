#include <assert.h>
#include <sys/types.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include "kfc.h"
#include "kthread.h"
#include "queue.h"
#include "bitvec.h"
#include "ucontext.h"
#include "valgrind.h"

static int inited = 0;

// synchronized shared data
static struct ready_queue rqueue;

static bitvec_t bitvec; // bit i is 1 if tid i is in use and 0 otherwise
static kthread_mutex_t bitvec_lock;

static kfc_upcb_t *uthread_pcbs[KFC_MAX_THREADS]; // user thread PCBs
static kthread_rwlock_t uthread_pcbs_lock;


// shared data that should go into kthread_pcbs
static ucontext_t sched_ctx; // scheduler context
static tid_t current_tid = KFC_TID_MAIN; // tid of currently executing thread


// shared data that doesn't need to be synchronized
static kfc_kpcb_t **kthread_pcbs;
static size_t num_kthreads;

kfc_kpcb_t *
get_kthread_pcb(kthread_t ktid)
{
  kfc_kpcb_t *kpcb = NULL;
  for (int i = 0; i < num_kthreads; i++) {
    if (kthread_pcbs[i]->ktid == ktid) {
      kpcb = kthread_pcbs[i];
    }
  }
  assert(kpcb);
  return kpcb;
}

void *
kthread_main(void *arg)
{
  assert(get_kthread_pcb(kthread_self())->ktid == kthread_self());
  return 0;
}

int get_next_tid()
{
  int ret;

  if (kthread_mutex_lock(&bitvec_lock)) {
    perror("get_next_tid (kthread_mutex_lock)");
    abort();
  }
  int tid = bitvec_first_cleared_index(&bitvec);
  if (tid < 0) {
    // no tid not in use found
    ret = -1;
  } else {
    // mark tid as in use
    bitvec_set(&bitvec, tid);
    ret = tid + KFC_TID_MAIN;
  }
  if (kthread_mutex_unlock(&bitvec_lock)) {
    perror("get_next_tid (kthread_mutex_unlock)");
    abort();
  }

  return ret;
}

void uthread_pcbs_rdlock()
{
  if (kthread_rwlock_rdlock(&uthread_pcbs_lock)) {
    perror("uthread_pcbs_rdlock");
    abort();
  }
}

void uthread_pcbs_wrlock()
{
  if (kthread_rwlock_wrlock(&uthread_pcbs_lock)) {
    perror("uthread_pcbs_wrlock");
    abort();
  }
}

void uthread_pcbs_unlock()
{
  if (kthread_rwlock_unlock(&uthread_pcbs_lock)) {
    perror("uthread_pcbs_unlock");
    abort();
  }
}

void reclaim_tid(tid_t tid)
{
  if (kthread_mutex_lock(&bitvec_lock)) {
    perror("get_next_tid (kthread_mutex_lock)");
    abort();
  }
  bitvec_clear(&bitvec, tid - KFC_TID_MAIN);
  if (kthread_mutex_unlock(&bitvec_lock)) {
    perror("get_next_tid (kthread_mutex_unlock)");
    abort();
  }
}

void
ready_enqueue(kfc_upcb_t *pcb)
{
  if (kthread_mutex_lock(&rqueue.mutex)) {
    perror("ready_enqueue (kthread_mutex_lock)");
    abort();
  }
  if (queue_enqueue(&rqueue.queue, pcb)) {
    perror("ready_enqueue (queue_enqueue)");
    abort();
  }
  if (kthread_mutex_unlock(&rqueue.mutex)) {
    perror("ready_enqueue (kthread_mutex_unlock)");
    abort();
  }
  if (kthread_cond_signal(&rqueue.not_empty)) {
    perror("ready_enqueue (kthread_cond_signal)");
    abort();
  }
}

kfc_upcb_t *
ready_dequeue()
{
  if (kthread_mutex_lock(&rqueue.mutex)) {
    perror("ready_dequeue (kthread_mutex_lock)");
    abort();
  }
  while (queue_size(&rqueue.queue) == 0) {
    kthread_cond_wait(&rqueue.not_empty, &rqueue.mutex);
  }
  kfc_upcb_t *pcb = queue_dequeue(&rqueue.queue);
  assert(pcb);
  if (queue_size(&rqueue.queue) > 0) {
    if (kthread_cond_signal(&rqueue.not_empty)) {
      perror("ready_dequeue (kthread_cond_signal)");
      abort();
    }
  }
  if (kthread_mutex_unlock(&rqueue.mutex)) {
    perror("ready_dequeue (kthread_mutex_unlock)");
    abort();
  }
  return pcb;
}

/**
 * Schedule the next thread.
 */
void schedule()
{
  // get next thread from ready queue (fcfs)
  kfc_upcb_t *next_pcb = ready_dequeue();

  uthread_pcbs_wrlock();

  // update current_tid
  current_tid = next_pcb->tid;

  // update thread state
  assert(next_pcb->state == READY);
  next_pcb->state = RUNNING;

  uthread_pcbs_unlock();

  // schedule thread
  if (setcontext(&next_pcb->ctx)) {
    perror("schedule (setcontext)");
    abort();
  }
}

/**
 * Allocate the stack for the provided ucontext_t.
 * @param stack       A pointer to the thread's stack
 * @param stack_base  Location of the thread's stack if already allocated, or
 *                    NULL if requesting that the library allocate it
 *                    dynamically
 * @param stack_size  Size (in bytes) of the thread's stack, or 0 to use the
 *                    default thread stack size KFC_DEF_STACK_SIZE
 * POSTCONDITION: The stack has been allocated and registered with valgrind.
 */
void allocate_stack(stack_t *stack, caddr_t stack_base, size_t stack_size)
{
  stack->ss_size = stack_size ? stack_size : KFC_DEF_STACK_SIZE;
  stack->ss_sp = stack_base ? stack_base : malloc(stack->ss_size);
  if (!(stack->ss_sp)) {
    perror("allocate_stack (malloc)");
    abort();
  }
  stack->ss_flags = 0;
  VALGRIND_STACK_REGISTER(stack->ss_sp, stack->ss_sp + stack->ss_size);
}

/**
 * Initializes the kfc library.  Programs are required to call this function
 * before they may use anything else in the library's public interface.
 *
 * @param kthreads    Number of kernel threads (pthreads) to allocate
 * @param quantum_us  Preemption timeslice in microseconds, or 0 for cooperative
 *                    scheduling
 *
 * @return 0 if successful, nonzero on failure
 */
int
kfc_init(int kthreads, int quantum_us)
{
	assert(!inited);

  // initialize uthread_pcbs lock
  if (kthread_rwlock_init(&uthread_pcbs_lock, NULL)) {
    perror("kfc_init (kthread_rwlock_init)");
    abort();
  }

  // initialize bitvector and its lock
  if (bitvec_init(&bitvec, KFC_MAX_THREADS)) {
    perror ("kfc_init (bitvec_init)");
    abort();
  }
  if (kthread_mutex_init(&bitvec_lock)) {
    perror("kfc_init (kthread_mutex_init)");
    abort();
  }

  // initialize ready queue and its synchronization constructs
  if (queue_init(&rqueue.queue)) {
    perror ("kfc_init (queue_init)");
    abort();
  }
  if (kthread_mutex_init(&rqueue.mutex)) {
    perror("kfc_init (kthread_mutex_init)");
    abort();
  }
  if (kthread_cond_init(&rqueue.not_empty)) {
    perror("kfc_init (kthread_cond_init)");
    abort();
  }

  // initialize scheduler context
  if (getcontext(&sched_ctx)) {
    perror("kfc_init (getcontext)");
    abort();
  }
  // XXX successor context is main thread ???
  sched_ctx.uc_link = &uthread_pcbs[KFC_TID_MAIN]->ctx;
  allocate_stack(&sched_ctx.uc_stack, NULL, 0);
  errno = 0;
  makecontext(&sched_ctx, (void (*)(void)) schedule, 0);
  if (errno != 0) {
    perror("kfc_init (makecontext)");
    abort();
  }
  
  // initialize kfc_ctx for main thread
  tid_t tid = get_next_tid();
  uthread_pcbs[tid] = malloc(sizeof(kfc_upcb_t));
  uthread_pcbs[tid]->stack_allocated = 0;
  uthread_pcbs[tid]->tid = tid;
  uthread_pcbs[tid]->state = RUNNING;
  uthread_pcbs[tid]->join_tid = -1;

  // initialize kthread_pcbs
  num_kthreads = kthreads;
  kthread_pcbs = malloc(num_kthreads * sizeof(kfc_kpcb_t *));

  // create kthread_pcbs
  for (int i = 0; i < num_kthreads; i++) {
    kthread_pcbs[i] = malloc(sizeof(kfc_kpcb_t));
    kthread_pcbs[i]->ktid = -1; // XXX remove later?
    // assign current user tid
    kthread_pcbs[i]->current_utid = KFC_TID_MAIN;
    // make scheduler context
    if (getcontext(&kthread_pcbs[i]->sched_ctx)) {
      perror("kfc_init (getcontext)");
      abort();
    }
    kthread_pcbs[i]->sched_ctx.uc_link = &uthread_pcbs[KFC_TID_MAIN]->ctx; // XXX ?
    allocate_stack(&kthread_pcbs[i]->sched_ctx.uc_stack, NULL, 0);
    errno = 0;
    makecontext(&kthread_pcbs[i]->sched_ctx, (void (*)(void)) schedule, 0);
    if (errno != 0) {
      perror("kfc_init (makecontext)");
      abort();
    }
  }

  // create kthreads
  for (int i = 0; i < num_kthreads; i++) {
    if (kthread_create(&kthread_pcbs[i]->ktid, kthread_main, NULL)) {
      perror("kfc_init (kthread_create)");
      abort();
    }
  }
	inited = 1;
	return 0;

}

// Precondition: uthread_pcbs[tid] != NULL
void
destroy_thread(tid_t tid)
{
  if (uthread_pcbs[tid]->stack_allocated) {
    free(uthread_pcbs[tid]->ctx.uc_stack.ss_sp);
  }
  free(uthread_pcbs[tid]);
  uthread_pcbs[tid] = NULL;
}

/**
 * Cleans up any resources which were allocated by kfc_init.  You may assume
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
kfc_teardown(void)
{
	assert(inited);
  
  // destroy ready queue and its synchronization constructs
  queue_destroy(&rqueue.queue);
  kthread_mutex_destroy(&rqueue.mutex);
  kthread_cond_destroy(&rqueue.not_empty);

  // destroy bitvector and its synchronization constructs
  bitvec_destroy(&bitvec);
  kthread_mutex_destroy(&bitvec_lock);

  // destroy other synchronization constructs
  kthread_rwlock_destroy(&uthread_pcbs_lock);

  // free zombie threads
  for (int i = KFC_TID_MAIN + 1; i < KFC_MAX_THREADS; i++) {
    if (uthread_pcbs[i]) {
      destroy_thread(i);
    }
  }

  // join kthreads
  for (int i = 0; i < num_kthreads; i++) {
    kthread_join(kthread_pcbs[i]->ktid, NULL);
  }
  
  // free kthread_pcbs
  for (int i = 0; i < num_kthreads; i++) {
    free(kthread_pcbs[i]->sched_ctx.uc_stack.ss_sp);
    free(kthread_pcbs[i]);
  }
  free(kthread_pcbs);

  // free scheduler stack
  free(sched_ctx.uc_stack.ss_sp);

  // free main thread
  destroy_thread(KFC_TID_MAIN);

	inited = 0;
}

/**
 * Thread trampoline function.
 */
void trampoline(void *(*start_func)(void *), void *arg)
{
  uthread_pcbs_rdlock();
  assert(uthread_pcbs[current_tid]->state == RUNNING);
  uthread_pcbs_unlock();

  // run start_func and pass return value to kfc_exit
  kfc_exit(start_func(arg));

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
 *                    default thread stack size KFC_DEF_STACK_SIZE
 *
 * @return 0 if successful, nonzero on failure
 */
// XXX Reference: https://www.ibm.com/docs/en/zos/2.2.0?topic=functions-makecontext-modify-user-context
int
kfc_create(tid_t *ptid, void *(*start_func)(void *), void *arg,
		caddr_t stack_base, size_t stack_size)
{
	assert(inited);

  // create new context (return early with error if KFC_MAX_THREADS has been reached) (XXX change later to allow tid reuse)
  tid_t new_tid = get_next_tid(); 
  *ptid = new_tid;

  uthread_pcbs_wrlock();

  uthread_pcbs[new_tid] = malloc(sizeof(kfc_upcb_t));
  uthread_pcbs[new_tid]->tid = new_tid;
  uthread_pcbs[new_tid]->state = READY;
  uthread_pcbs[new_tid]->join_tid = -1;
  if (getcontext(&uthread_pcbs[new_tid]->ctx)) {
    perror("kfc_create (getcontext)");
    abort();
  }

  // allocate stack for new context
  allocate_stack(&uthread_pcbs[new_tid]->ctx.uc_stack, stack_base, stack_size);
  uthread_pcbs[new_tid]->stack_allocated = stack_base ? 0 : 1;

  // set scheduler as successor context
  uthread_pcbs[new_tid]->ctx.uc_link = &sched_ctx;
  
  // makecontext (note that current_tid is the tid of the calling context)
  errno = 0;
  makecontext(&uthread_pcbs[new_tid]->ctx, (void (*)(void)) trampoline, 2, start_func, arg);
  if (errno != 0) {
    perror("kfc_create (makecontext)");
    abort();
  }

  // add new context to ready queue
  ready_enqueue(uthread_pcbs[new_tid]);

  uthread_pcbs_unlock();

  return 0;
}

/**
 * Exits the calling thread.  This should be the same thing that happens when
 * the thread's start_func returns.
 *
 * @param ret  Return value from the thread
 */
void
kfc_exit(void *ret)
{
	assert(inited);

  // update thread state and save return value
  uthread_pcbs_wrlock();

  assert(uthread_pcbs[current_tid]->state == RUNNING);
  uthread_pcbs[current_tid]->state = FINISHED;
  uthread_pcbs[current_tid]->retval = ret;
  
  // if another thread is waiting to join on this thread,
  // return it to the ready queue
  int join_tid = uthread_pcbs[current_tid]->join_tid;
  if (join_tid >= 0) {
    assert(join_tid != current_tid);
    assert(uthread_pcbs[join_tid]->state == WAITING_JOIN);
    uthread_pcbs[join_tid]->state = READY;
    ready_enqueue(uthread_pcbs[join_tid]);
    // no thread waiting to join on this thread any more
    uthread_pcbs[current_tid]->join_tid = -1;
  }

  uthread_pcbs_unlock();

  // ask scheduler to schedule next thread
  if (setcontext(&sched_ctx)) {
    perror("kfc_exit (setcontext)");
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
 *                   kfc_exit should be stored, or NULL if the caller does not
 *                   care.
 *
 * @return 0 if successful, nonzero on failure
 */
int
kfc_join(tid_t tid, void **pret)
{
	assert(inited);

  uthread_pcbs_wrlock();
  
  kfc_upcb_t *current_pcb = uthread_pcbs[current_tid];
  kfc_upcb_t *target_pcb = uthread_pcbs[tid]; 

  // Block if the target thread is not finished yet
  if (target_pcb->state != FINISHED) {
    // add caller to waitlist
    //assert(pcb->state == RUNNING);
    current_pcb->state = WAITING_JOIN;
    target_pcb->join_tid = (int) current_tid; // cast to int for comparison with -1

    uthread_pcbs_unlock();

    // block by saving caller state and swapping to scheduler
    if (swapcontext(&current_pcb->ctx, &sched_ctx)) {
      perror("kfc_join (swapcontext)");
      abort();
    }
  } else {
    uthread_pcbs_unlock();
  }

  uthread_pcbs_rdlock();

  // continue once the target thread has finished
  assert(uthread_pcbs[tid]->state == FINISHED);

  // Pass target thread's return value to caller
  if (pret) {
    *pret = uthread_pcbs[tid]->retval;
  }

  uthread_pcbs_unlock();

  // Clean up target thread's resources
  reclaim_tid(tid);
  destroy_thread(tid);

	return 0;
}

/**
 * Returns a small integer which identifies the calling thread.
 *
 * @return Thread ID of the currently executing thread
 */
tid_t
kfc_self(void)
{
	assert(inited);
	return current_tid;
}

/**
 * Causes the calling thread to yield the processor voluntarily.  This may
 * result in another thread being scheduled, but it does not preclude the
 * possibility of the same thread continuing if re-chosen by the scheduling
 * algorithm.
 */
void
kfc_yield(void)
{
	assert(inited);

  uthread_pcbs_wrlock();
  
  // enqueue calling thread
  kfc_upcb_t *pcb = uthread_pcbs[current_tid];
  assert(pcb->state == RUNNING);
  pcb->state = READY;

  uthread_pcbs_unlock();

  ready_enqueue(pcb);
  
  // save caller state and swap to scheduler
  if (swapcontext(&pcb->ctx, &sched_ctx)) {
    perror("kfc_yield (swapcontext)");
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
kfc_sem_init(kfc_sem_t *sem, int value)
{
	assert(inited);
  sem->counter = value;
  return queue_init(&sem->queue);
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
kfc_sem_post(kfc_sem_t *sem)
{
	assert(inited);

  // increase counter
  sem->counter++;

  // if a thread has been waiting to decrement counter, return it to the ready queue
  if (queue_size(&sem->queue) > 0) {
    kfc_upcb_t *pcb = queue_dequeue(&sem->queue);

    uthread_pcbs_wrlock();

    assert(pcb);
    assert(pcb->state == WAITING_SEM);
    pcb->state = READY;
    
    uthread_pcbs_unlock();
    
    ready_enqueue(pcb);
  }
	return 0;
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
kfc_sem_wait(kfc_sem_t *sem)
{
	assert(inited);
  assert(sem->counter >= 0);

  // block if the counter is not above 0
  if (sem->counter == 0) {
    // add to semaphore queue

    uthread_pcbs_wrlock();

    kfc_upcb_t *pcb = uthread_pcbs[current_tid];
    pcb->state = WAITING_SEM;

    uthread_pcbs_unlock();
    
    if (queue_enqueue(&sem->queue, pcb)) {
      perror("kfc_sem_wait (queue_enqueue)");
      abort();
    }


    // block by saving caller state and swapping to scheduler
    if (swapcontext(&pcb->ctx, &sched_ctx)) {
      perror("kfc_sem_wait (swapcontext)");
      abort();
    }
  }

  // decrement the counter once it is above 0
  assert(sem->counter > 0);
  sem->counter--;
	return 0;
}

/**
 * Frees any resources associated with a semaphore.  Destroying a semaphore on
 * which threads are waiting results in undefined behavior.
 *
 * @param sem  Pointer to the semaphore to be destroyed
 */
void
kfc_sem_destroy(kfc_sem_t *sem)
{
	assert(inited);
  queue_destroy(&sem->queue);
}
