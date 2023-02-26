#include <assert.h>
#include <sys/types.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include "kfc.h"
#include "queue.h"
#include "bitvec.h"
#include "ucontext.h"
#include "valgrind.h"

static int inited = 0;

// XXX synchronize access later

static bitvec_t bitvec; // bit i is 1 if tid i is in use and 0 otherwise

static tid_t current_tid = KFC_TID_MAIN; // tid of currently executing thread

static kfc_ctx_t *thread_info[KFC_MAX_THREADS]; // thread info

static ucontext_t sched_ctx; // scheduler context

static queue_t ready_queue; // ready queue

// join_waitlist[i] is the tid of the thread that has called kfc_join
// on thread i (or -1 if there is no such thread)
static int join_waitlist[KFC_MAX_THREADS];

int get_next_tid()
{
  // get the first tid that is not in use
  int tid = bitvec_first_cleared_index(&bitvec);
  assert((tid + KFC_TID_MAIN) < KFC_MAX_THREADS);
  if (tid < 0) {
    return -1;
  }

  // mark tid as in use
  bitvec_set(&bitvec, tid);

  return tid + KFC_TID_MAIN;
}

void reclaim_tid(tid_t tid)
{
  bitvec_clear(&bitvec, tid - KFC_TID_MAIN);
  assert(bitvec_first_cleared_index(&bitvec) <= tid - KFC_TID_MAIN);
}

/**
 * Schedule the next thread.
 */
void schedule()
{
  // get next thread from ready_queue (fcfs)
  kfc_ctx_t *next_ctx;
  if (!(next_ctx = queue_dequeue(&ready_queue))) {
    perror("schedule - ready_queue is empty");
    return;
  }

  // update current_tid
  current_tid = next_ctx->tid;

  // update thread state
  assert(next_ctx->state == READY);
  next_ctx->state = RUNNING;

  // schedule thread
  if (setcontext(&next_ctx->ctx)) {
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

  // initialize bitvector
  if (bitvec_init(&bitvec, KFC_MAX_THREADS)) {
    perror ("kfc_init (bitvec_init)");
    abort();
  }

  // initialize ready_queue
  if (queue_init(&ready_queue)) {
    perror ("kfc_init (queue_init)");
    abort();
  }

  // initialize join waitlist
  memset(&join_waitlist, -1, KFC_MAX_THREADS);

  // initialize scheduler context
  if (getcontext(&sched_ctx)) {
    perror("kfc_init (getcontext)");
    abort();
  }
  // XXX successor context is main thread ???
  sched_ctx.uc_link = &thread_info[KFC_TID_MAIN]->ctx;
  allocate_stack(&sched_ctx.uc_stack, NULL, 0);
  errno = 0;
  makecontext(&sched_ctx, (void (*)(void)) schedule, 0);
  if (errno != 0) {
    perror("kfc_init (makecontext)");
    abort();
  }
  
  // initialize kfc_ctx for main thread
  tid_t tid = get_next_tid();
  thread_info[tid] = malloc(sizeof(kfc_ctx_t));
  thread_info[tid]->stack_allocated = 0;
  thread_info[tid]->tid = tid;
  thread_info[tid]->state = RUNNING;

	inited = 1;
	return 0;
}

// Precondition: thread_info[tid] != NULL
void
destroy_thread(tid_t tid)
{
  if (thread_info[tid]->stack_allocated) {
    free(thread_info[tid]->ctx.uc_stack.ss_sp);
  }
  free(thread_info[tid]);
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

  // destroy queue
  queue_destroy(&ready_queue);

  // destroy bitvector
  bitvec_destroy(&bitvec);

  // free scheduler stack
  free(sched_ctx.uc_stack.ss_sp);

  // free zombie threads
  for (int i = KFC_TID_MAIN + 1; i < KFC_MAX_THREADS; i++) {
    if (thread_info[i]) {
      destroy_thread(i);
    }
  }

  // free main thread
  destroy_thread(KFC_TID_MAIN);

	inited = 0;
}

/**
 * Thread trampoline function.
 */
void trampoline(void *(*start_func)(void *), void *arg)
{
  assert(thread_info[current_tid]->state == RUNNING);

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
  thread_info[new_tid] = malloc(sizeof(kfc_ctx_t));
  thread_info[new_tid]->tid = new_tid;
  thread_info[new_tid]->state = READY;
  if (getcontext(&thread_info[new_tid]->ctx)) {
    perror("kfc_create (getcontext)");
    abort();
  }

  // allocate stack for new context
  allocate_stack(&thread_info[new_tid]->ctx.uc_stack, stack_base, stack_size);
  thread_info[new_tid]->stack_allocated = stack_base ? 0 : 1;

  // set scheduler as successor context
  thread_info[new_tid]->ctx.uc_link = &sched_ctx;
  
  // makecontext (note that current_tid is the tid of the calling context)
  errno = 0;
  makecontext(&thread_info[new_tid]->ctx, (void (*)(void)) trampoline, 2, start_func, arg);
  if (errno != 0) {
    perror("kfc_create (makecontext)");
    abort();
  }

  // add new context to ready_queue
  if (queue_enqueue(&ready_queue, thread_info[new_tid])) {
    perror("kfc_create (queue_enqueue)");
    abort();
  }

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
  assert(thread_info[current_tid]->state == RUNNING);
  thread_info[current_tid]->state = FINISHED;
  thread_info[current_tid]->retval = ret;
  
  // if another thread is waiting to join on this thread,
  // return it to the ready queue
  int join_tid = join_waitlist[current_tid];
  if (join_tid >= 0) {
    assert(join_tid != current_tid);
    assert(thread_info[join_tid]->state == WAITING);
    thread_info[join_tid]->state = READY;
    if (queue_enqueue(&ready_queue, thread_info[join_tid])) {
      perror("kfc_exit (queue_enqueue)");
      abort();
    }
    join_waitlist[current_tid] = -1; // join waitlist for this tid is now empty
  }

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

  // Block if the target thread is not finished yet
  if (thread_info[tid]->state != FINISHED) {
    // add caller to waitlist
    assert(thread_info[current_tid]->state == RUNNING);
    thread_info[current_tid]->state = WAITING;
    join_waitlist[tid] = (int) current_tid; // cast to int for comparison with -1

    // block by saving caller state and swapping to scheduler
    if (swapcontext(&thread_info[current_tid]->ctx, &sched_ctx)) {
      perror("kfc_join (swapcontext)");
      abort();
    }
  }

  // continue once the target thread has finished
  assert(thread_info[tid]->state == FINISHED);

  // Pass target thread's return value to caller
  if (pret) {
    *pret = thread_info[tid]->retval;
  }

  // Clean up target thread's resources
  reclaim_tid(tid);
  if (thread_info[tid]->stack_allocated) {
    free(thread_info[tid]->ctx.uc_stack.ss_sp);
  }
  free(thread_info[tid]);
  thread_info[tid] = NULL;

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
  
  // enqueue calling thread
  assert(thread_info[current_tid]->state == RUNNING);
  thread_info[current_tid]->state = READY;
  if (queue_enqueue(&ready_queue, thread_info[current_tid])) {
    perror("kfc_yield (queue_enqueue)");
    abort();
  }
  
  // save caller state and swap to scheduler
  if (swapcontext(&thread_info[current_tid]->ctx, &sched_ctx)) {
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
    kfc_ctx_t *ctx = queue_dequeue(&sem->queue);
    assert(ctx);
    assert(ctx->state == WAITING);
    ctx->state = READY;
    if (queue_enqueue(&ready_queue, ctx)) {
      perror("kfc_sem_post (queue_enqueue)");
      abort();
    }
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
    thread_info[current_tid]->state = WAITING;
    if (queue_enqueue(&sem->queue, thread_info[current_tid])) {
      perror("kfc_sem_wait (queue_enqueue)");
      abort();
    }

    // block by saving caller state and swapping to scheduler
    if (swapcontext(&thread_info[current_tid]->ctx, &sched_ctx)) {
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
