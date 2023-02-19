#include <assert.h>
#include <sys/types.h>
#include <stdlib.h>
#include <errno.h>

#include "kfc.h"
#include "ucontext.h"
#include "valgrind.h"

static int inited = 0;

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

	inited = 0;
}

void swap_helper(void *(*start_func)(void *), void *arg, struct ucontext *calling_ctx)
{
  start_func(arg);
  setcontext(calling_ctx);
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

  // create new context
  ucontext_t calling_ctx, new_ctx;
  getcontext(&new_ctx); // XXX why?
  *ptid = 0; // XXX change later

  // allocate stack for new context
  new_ctx.uc_stack.ss_size = stack_size ? stack_size : KFC_DEF_STACK_SIZE;
  new_ctx.uc_stack.ss_sp = stack_base ? stack_base : malloc(new_ctx.uc_stack.ss_size);
  new_ctx.uc_stack.ss_flags = 0;
  VALGRIND_STACK_REGISTER(new_ctx.uc_stack.ss_sp, new_ctx.uc_stack.ss_sp + new_ctx.uc_stack.ss_size);
  //DPRINTF("stack size: asked for %d, got %d\n", (int) stack_size, (int) new_ctx.uc_stack.ss_size);
  //DPRINTF("stack base: asked for %p, got %p\n", stack_base, &new_ctx.uc_stack.ss_sp);

  // assign calling_ctx as successor context
  new_ctx.uc_link = NULL;
  
  // makecontext
  errno = 0;
  //makecontext(&new_ctx, (void (*)(void)) start_func, 1, arg);
  makecontext(&new_ctx, (void (*)(void)) swap_helper, 3, start_func, arg, &calling_ctx);
  if (errno != 0) {
    perror("kfc_create (makecontext)");
    return -1;
  }

  // swap context
  if (swapcontext(&calling_ctx, &new_ctx)) {
  //if (setcontext(&new_ctx)) {
    perror("kfc_create (setcontext)");
    return -1;
  }

  // clean up (move to kfc_exit?)
  //free(new_ctx.uc_stack.ss_sp);
  //free(calling_ctx);
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
  DPRINTF("kfc_exit called\n");
  ucontext_t ctx;
  getcontext(&ctx);
  free(ctx.uc_stack.ss_sp);
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

	return 0;
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
