#include <assert.h>
#include <sys/types.h>
#include <stdlib.h>
#include <errno.h>

#include "kfc.h"
#include "queue.h"
#include "ucontext.h"
#include "valgrind.h"

static int inited = 0;

// XXX synchronize access later

static tid_t next_tid = KFC_TID_MAIN;

static tid_t current_tid = KFC_TID_MAIN; // tid of currently executing thread

static kfc_ctx_t *thread_info[KFC_MAX_THREADS - 1];

static ucontext_t sched_ctx;

static queue_t queue;

void schedule()
{
  // get next thread from queue
  kfc_ctx_t *next_ctx;
  if (!(next_ctx = queue_dequeue(&queue))) {
    perror("schedule - queue is empty");
    abort();
  }

  // update current_tid
  current_tid = next_ctx->tid;

  // schedule next thread
  if (setcontext(&next_ctx->ctx)) {
    perror("schedule (setcontext)");
    abort();
  }
}

/**
 * Swap context while updating state.
 * Postcondition: current_tid has been set to new_tid, old_tid
 * has been enqueued at the tail of queue, the thread context
 * corresponding to old_tid has been saved, and the current
 * context is the thread context corresponding to new_tid.
 */
int kfc_swapcontext(tid_t old_tid, tid_t new_tid)
{
  current_tid = new_tid;
  if (queue_enqueue(&queue, thread_info[old_tid])) {
    perror("kfc_swapcontext (queue_enqueue)");
    abort();
  }
  return swapcontext(&thread_info[old_tid]->ctx, &thread_info[new_tid]->ctx);
}

/**
 * Set context while updating state.
 * Postcondition: current_tid has been set to new_tid, and the
 * current context is the thread context corresponding to new_tid.
 */
int kfc_setcontext(tid_t new_tid)
{
  current_tid = new_tid;
  return setcontext(&thread_info[new_tid]->ctx);
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

  // initialize queue
  if (queue_init(&queue)) {
    perror ("kfc_init (queue_init)");
    abort();
  }

  // initialize scheduler context
  if (getcontext(&sched_ctx)) {
    perror("kfc_init (getcontext)");
    abort();
  }

  // allocate stack for scheduler context
  sched_ctx.uc_stack.ss_size = KFC_DEF_STACK_SIZE;
  sched_ctx.uc_stack.ss_sp = malloc(sched_ctx.uc_stack.ss_size);
  sched_ctx.uc_stack.ss_flags = 0;
  VALGRIND_STACK_REGISTER(sched_ctx.uc_stack.ss_sp, sched_ctx.uc_stack.ss_sp + sched_ctx.uc_stack.ss_size);

  // make scheduler context
  errno = 0;
  makecontext(&sched_ctx, (void (*)(void)) schedule, 0);
  if (errno != 0) {
    perror("kfc_init (makecontext)");
    abort();
  }
  
  // initialize kfc_ctx for main thread
  tid_t tid = next_tid++; // XXX synchronize later
  thread_info[tid] = malloc(sizeof(kfc_ctx_t));
  thread_info[tid]->tid = tid;

	inited = 1;
	return 0;
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

  queue_destroy(&queue);

  // XXX move cleanup of non-main threads to kfc_join later
  for (int tid = KFC_TID_MAIN+1; tid < KFC_MAX_THREADS; tid++) {
    if (thread_info[tid]) {
      if (thread_info[tid]->stack_allocated) {
        free(thread_info[tid]->ctx.uc_stack.ss_sp);
      }
      free(thread_info[tid]);
    }
  }
 
  // free scheduler context
  free(sched_ctx.uc_stack.ss_sp);

  // free main thread
  free(thread_info[KFC_TID_MAIN]);

	inited = 0;
}

/**
 * Run the provided thread main function start_func and then set
 * the context of the next thread in the queue as the new context.
 * Precondition: the queue is not empty.
 */
void swap_helper(void *(*start_func)(void *), void *arg)
{
  start_func(arg);
  assert(queue_size(&queue) > 0);
  kfc_ctx_t *ctx;
  if (!(ctx = queue_dequeue(&queue))) {
    perror("swap_helper - queue is empty");
    abort();
  }
  kfc_setcontext(ctx->tid);
}

void trampoline(void *(*start_func)(void *), void *arg)
{
  void *ret = start_func(arg);
  kfc_exit(ret);
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
  if (next_tid == KFC_MAX_THREADS) {
    perror("kfc_create (KFC_MAX_THREADS reached)");
    return -1;
  }
  tid_t new_tid = next_tid++;
  *ptid = new_tid;
  thread_info[new_tid] = malloc(sizeof(kfc_ctx_t));
  thread_info[new_tid]->tid = new_tid;
  if (getcontext(&thread_info[new_tid]->ctx)) {
    perror("kfc_create (getcontext)");
    abort();
  }

  // allocate stack for new context
  thread_info[new_tid]->ctx.uc_stack.ss_size = stack_size ? stack_size : KFC_DEF_STACK_SIZE;
  thread_info[new_tid]->ctx.uc_stack.ss_sp = stack_base ? stack_base : malloc(thread_info[new_tid]->ctx.uc_stack.ss_size);
  thread_info[new_tid]->stack_allocated = stack_base ? 0 : 1;
  thread_info[new_tid]->ctx.uc_stack.ss_flags = 0;
  VALGRIND_STACK_REGISTER(thread_info[new_tid]->ctx.uc_stack.ss_sp, thread_info[new_tid]->ctx.uc_stack.ss_sp + thread_info[new_tid]->ctx.uc_stack.ss_size);

  // set scheduler as successor context
  thread_info[new_tid]->ctx.uc_link = &sched_ctx;
  
  // makecontext (note that current_tid is the tid of the calling context)
  errno = 0;
  makecontext(&thread_info[new_tid]->ctx, (void (*)(void)) trampoline, 2, start_func, arg);
  if (errno != 0) {
    perror("kfc_create (makecontext)");
    abort();
  }

  // add new context to queue
  if (queue_enqueue(&queue, thread_info[new_tid])) {
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
  if (queue_enqueue(&queue, thread_info[current_tid])) {
    perror("kfc_swapcontext (queue_enqueue)");
    abort();
  }
  
  // save caller state and swap to scheduler (XXX add error checking later)
  swapcontext(&thread_info[current_tid]->ctx, &sched_ctx);
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
	return 0;
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
}
