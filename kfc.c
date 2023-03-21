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
static kthread_sem_t inited_sem;
static kthread_sem_t exitall_sem;

static struct ready_queue rqueue;

static bitvec_t bitvec; // bit i is 1 if tid i is in use and 0 otherwise
static kthread_mutex_t bitvec_lock;

static kfc_pcb_t *pcbs[KFC_MAX_THREADS]; // user thread PCBs
static kthread_mutex_t pcbs_lock;

static ucontext_t abort_ctx;

static kfc_pcb_t exitall;


// shared data that doesn't need to be synchronized
static kfc_kinfo_t **kthread_info;
static size_t num_kthreads;
static unsigned int MAIN_KTHREAD_INDEX = 0;

kfc_kinfo_t *
get_kthread_info(kthread_t ktid)
{
  kfc_kinfo_t *kinfo = NULL;
  for (int i = 0; i < num_kthreads; i++) {
    if (kthread_info[i]->ktid == ktid) {
      kinfo = kthread_info[i];
    }
  }
  assert(kinfo);
  return kinfo;
}

tid_t
get_current_tid()
{
  return get_kthread_info(kthread_self())->current_tid;
}

ucontext_t *
get_sched_ctx()
{
  kfc_kinfo_t *kinfo = get_kthread_info(kthread_self());
  return &kinfo->sched_info.sched_ctx;
}

void *
kthread_func(void *arg)
{
  if (kthread_sem_wait(&inited_sem)) {
		perror("kthread_func (kthread_sem_wait)");
		abort();
	}

  if (setcontext(get_sched_ctx())) {
    perror("kfc_exit (setcontext)");
    abort();
  }
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

void lock_pcbs()
{
	if (kthread_mutex_lock(&pcbs_lock)) {
    perror("pcbs_lock");
    abort();
  }
}

void unlock_pcbs()
{
  if (kthread_mutex_unlock(&pcbs_lock)) {
    perror("pcbs_unlock");
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
ready_enqueue(kfc_pcb_t *pcb)
{
  if (kthread_mutex_lock(&rqueue.lock)) {
    perror("ready_enqueue (kthread_mutex_lock)");
    abort();
  }
  if (queue_enqueue(&rqueue.queue, pcb)) {
    perror("ready_enqueue (queue_enqueue)");
    abort();
  }
  if (kthread_mutex_unlock(&rqueue.lock)) {
    perror("ready_enqueue (kthread_mutex_unlock)");
    abort();
  }
  if (kthread_sem_post(&rqueue.not_empty)) {
    perror("ready_enqueue (kthread_sem_post)");
    abort();
  }
}

kfc_pcb_t *
ready_dequeue()
{
  if (kthread_sem_wait(&rqueue.not_empty)) {
    perror("ready_dequeue (kthread_sem_wait)");
    abort();
  }

  if (kthread_mutex_lock(&rqueue.lock)) {
    perror("ready_dequeue (kthread_mutex_lock)");
    abort();
  }

  assert(queue_size(&rqueue.queue) > 0);
  kfc_pcb_t *pcb = queue_dequeue(&rqueue.queue);
  assert(pcb);

  if (kthread_mutex_unlock(&rqueue.lock)) {
    perror("ready_dequeue (kthread_mutex_lock)");
    abort();
  }

  return pcb;
}

/**
 * Schedule the next thread.
 */
void schedule()
{
  
  kfc_kinfo_t *kinfo = get_kthread_info(kthread_self());
	//tid_t current_tid = kinfo->current_tid;
	int current_tid = get_current_tid();
	kfc_pcb_t *current_pcb = current_tid > -1 ? pcbs[current_tid] : NULL;
	kfc_sem_t *sem = kinfo->sched_info.task_sem;

	assert(kthread_self() == kinfo->ktid);

  switch(kinfo->sched_info.task) {
    case NONE:
      break;
    case YIELD:
			assert(current_pcb);
      assert(current_pcb->state == RUNNING);
      current_pcb->state = READY;
      ready_enqueue(current_pcb);
      unlock_pcbs();
      break;
		case EXIT:
			assert(current_pcb && current_pcb->state == RUNNING);
			current_pcb->state = FINISHED;

			// if another thread is waiting to join on this thread,
			// return it to the ready queue
			if (queue_size(&current_pcb->join_queue) > 0) {
				kfc_pcb_t *join_pcb = queue_dequeue(&current_pcb->join_queue);
				assert(join_pcb && join_pcb->tid != current_tid && join_pcb->state == WAITING_JOIN);
				join_pcb->state = READY;
				ready_enqueue(join_pcb);
			}
			unlock_pcbs();
			break;
    case JOIN:
			assert(current_pcb);

      int target_tid = kinfo->sched_info.task_target;
			assert(pcbs[target_tid]->state != FINISHED);
			assert(&pcbs[target_tid]->join_queue == kinfo->sched_info.queue);
			assert(queue_size(kinfo->sched_info.queue) == 0);

			current_pcb->state = WAITING_JOIN;
			if (queue_enqueue(kinfo->sched_info.queue, current_pcb)) {
				perror("schedule (queue_enqueue)");
				abort();
			}
      unlock_pcbs();
      break;
    case SEM_WAIT:
			assert(current_pcb);
			assert(sem);
			if (kthread_mutex_lock(&sem->lock)) {
				perror("schedule (kthread_mutex_lock)");
				abort();
			}
			assert(sem->counter >= 0);
			lock_pcbs();
			if (sem->counter == 0) {
				// add current pcb to sem waiting queue
				current_pcb->state = WAITING_SEM;
				if (queue_enqueue(&sem->queue, current_pcb)) {
					perror("schedule (queue_enqueue)");
					abort();
				}
			} else {
				current_pcb->state = READY;
				ready_enqueue(current_pcb);
			}
			unlock_pcbs();
			if (kthread_mutex_unlock(&sem->lock)) {
				perror("schedule (kthread_mutex_unlock)");
				abort();
			}
			kinfo->sched_info.task_sem = NULL;
      break;
		case TEARDOWN:
			current_pcb->state = READY;
			ready_enqueue(current_pcb);
			unlock_pcbs();
			break;
    default:
      perror("schedule: invalid task");
      abort();
      break;
  }
  kinfo->sched_info.task = NONE;
	kinfo->sched_info.lock = NULL;
	kinfo->sched_info.queue = NULL;
	kinfo->sched_info.task_target = -1;
	assert(kinfo->sched_info.task_sem == NULL);
	assert(kinfo->sched_info.task_target == -1);

  // get next thread from ready queue (fcfs)
  kfc_pcb_t *next_pcb = ready_dequeue();

  if (next_pcb == &exitall) {
		if (kthread_self() == kthread_info[MAIN_KTHREAD_INDEX]->ktid) {
			DPRINTF("main kthread %d received exitall and is waiting on exitall_sem\n", kthread_self());
			for (int i = 0; i < num_kthreads-1; i++) {
				if (kthread_sem_wait(&exitall_sem)) {
					perror("schedule (kthread_sem_wait)");
					abort();
				}
			}
			schedule();
		} else {
			DPRINTF("kthread %d received exitall and is exiting\n", kthread_self());
			kthread_sem_post(&exitall_sem);
			kthread_exit();
		}
  }

  lock_pcbs();

  // update current tid for this kthread
  get_kthread_info(kthread_self())->current_tid = next_pcb->tid;

  // update thread state
  assert(next_pcb->state == READY);
  next_pcb->state = RUNNING;

  unlock_pcbs();

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

  num_kthreads = kthreads;
  if (kthread_sem_init(&inited_sem, 0)) {
    perror("kthread_sem_init");
    abort();
  }

  if (kthread_sem_init(&exitall_sem, 0)) {
		perror("kthread_sem_init");
    abort();
  }

  // initialize pcbs lock
  if (kthread_mutex_init(&pcbs_lock)) {
    perror("kfc_init (kthread_mutex_init)");
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
  if (kthread_mutex_init(&rqueue.lock)) {
    perror("kfc_init (kthread_mutex_init)");
    abort();
  }
  if (kthread_sem_init(&rqueue.not_empty, 0)) {
    perror("kfc_init (kthread_sem_init)");
    abort();
  }

  // initialize kfc_ctx for main thread
  tid_t tid = get_next_tid();
  pcbs[tid] = malloc(sizeof(kfc_pcb_t));
  pcbs[tid]->stack_allocated = 0;
  pcbs[tid]->tid = tid;
  pcbs[tid]->state = RUNNING;
  if (queue_init(&pcbs[tid]->join_queue)) {
    perror("kfc_init (queue_init)");
    abort();
  }

  // initialize abort_ctx
  if (getcontext(&abort_ctx)) {
    perror("kfc_init (getcontext)");
    abort();
  }
  abort_ctx.uc_link = NULL;
  allocate_stack(&abort_ctx.uc_stack, NULL, 0);
  errno = 0;
  makecontext(&abort_ctx, (void (*)(void)) abort, 0);
  if (errno != 0) {
    perror("kfc_init (makecontext)");
    abort();
  }


  // initialize kthread_info
  kthread_info = malloc(num_kthreads * sizeof(kfc_kinfo_t *));

  // create kthread_info
  for (int i = 0; i < num_kthreads; i++) {
    kthread_info[i] = malloc(sizeof(kfc_kinfo_t));
    kthread_info[i]->ktid = i == 0 ? kthread_self() : -1;
    // assign current user tid
    kthread_info[i]->current_tid = i == 0 ? KFC_TID_MAIN : -1;
    // initialize scheduler info
    kthread_info[i]->sched_info.task = NONE;
    kthread_info[i]->sched_info.task_sem = NULL;
    kthread_info[i]->sched_info.task_target = -1;
		kthread_info[i]->sched_info.lock = NULL;
		kthread_info[i]->sched_info.queue = NULL;
    if (getcontext(&kthread_info[i]->sched_info.sched_ctx)) {
      perror("kfc_init (getcontext)");
      abort();
    }
    // successor context should never be reached (it aborts the program)
    kthread_info[i]->sched_info.sched_ctx.uc_link = &abort_ctx;
    allocate_stack(&kthread_info[i]->sched_info.sched_ctx.uc_stack, NULL, 0);
    errno = 0;
    makecontext(&kthread_info[i]->sched_info.sched_ctx, (void (*)(void)) schedule, 0);
    if (errno != 0) {
      perror("kfc_init (makecontext)");
      abort();
    }
  }

  // create other kthreads
  for (int i = MAIN_KTHREAD_INDEX+1; i < num_kthreads; i++) {
    if (kthread_create(&kthread_info[i]->ktid, kthread_func, NULL)) {
      perror("kfc_init (kthread_create)");
      abort();
    }
  }
	inited = 1;

  for (int i = 0; i < num_kthreads-1; i++) {
    kthread_sem_post(&inited_sem);
  }

	return 0;

}

// Precondition: pcbs[tid] != NULL
void
destroy_thread(tid_t tid)
{
  queue_destroy(&pcbs[tid]->join_queue);
  if (pcbs[tid]->stack_allocated) {
    free(pcbs[tid]->ctx.uc_stack.ss_sp);
  }
  free(pcbs[tid]);
  pcbs[tid] = NULL;
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

	DPRINTF("kthread %d is starting teardown\n", kthread_self());
  for (int i = 0; i < num_kthreads; i++) {
    ready_enqueue(&exitall);
  }
	kfc_kinfo_t *kinfo = get_kthread_info(kthread_self());
	assert(kinfo->sched_info.task == NONE);
	assert(kinfo->sched_info.task_sem == NULL);
	assert(kinfo->sched_info.task_target == -1);

	kinfo->sched_info.task = TEARDOWN;
	lock_pcbs();

	// block by saving caller state and swapping to scheduler
	if (swapcontext(&pcbs[get_current_tid()]->ctx, get_sched_ctx())) {
		perror("kfc_join (swapcontext)");
		abort();
	}

	assert(inited);

	kthread_t self = kthread_self();
	assert(self == kthread_info[MAIN_KTHREAD_INDEX]->ktid);
	DPRINTF("main kthread %d is continuing teardown\n", self);

  // join kthreads except for "main" kthread
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
  while (kthread_mutex_destroy(&pcbs_lock) == EBUSY);

  // free zombie threads (exclude KFC_TID_MAIN)
  for (int i = KFC_TID_MAIN + 1; i < KFC_MAX_THREADS; i++) {
    if (pcbs[i]) {
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
  destroy_thread(KFC_TID_MAIN);

	inited = 0;
}

/**
 * Thread trampoline function.
 */
void trampoline(void *(*start_func)(void *), void *arg)
{
  lock_pcbs();
  assert(pcbs[get_current_tid()]->state == RUNNING);
  unlock_pcbs();

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

  lock_pcbs();

  pcbs[new_tid] = malloc(sizeof(kfc_pcb_t));
  pcbs[new_tid]->tid = new_tid;
  pcbs[new_tid]->state = READY;
  if (queue_init(&pcbs[new_tid]->join_queue)) {
    perror("kfc_create (queue_init)");
    abort();
  }
  if (getcontext(&pcbs[new_tid]->ctx)) {
    perror("kfc_create (getcontext)");
    abort();
  }

  // allocate stack for new context
  allocate_stack(&pcbs[new_tid]->ctx.uc_stack, stack_base, stack_size);
  pcbs[new_tid]->stack_allocated = stack_base ? 0 : 1;

  // successor context should never be reached (it aborts the program)
  pcbs[new_tid]->ctx.uc_link = &abort_ctx;
  
  // makecontext
  errno = 0;
  makecontext(&pcbs[new_tid]->ctx, (void (*)(void)) trampoline, 2, start_func, arg);
  if (errno != 0) {
    perror("kfc_create (makecontext)");
    abort();
  }

  // add new context to ready queue
  ready_enqueue(pcbs[new_tid]);

  unlock_pcbs();

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
  lock_pcbs();

  pcbs[get_current_tid()]->retval = ret;

  // let scheduler know that the current user thread has requested to yield
  kfc_kinfo_t *kinfo = get_kthread_info(kthread_self());
  assert(kinfo->sched_info.task == NONE);
  assert(kinfo->sched_info.task_sem == NULL);
  assert(kinfo->sched_info.task_target == -1);

  kinfo->sched_info.task = EXIT;
  
  // ask scheduler to schedule next thread
  if (setcontext(get_sched_ctx())) {
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

  tid_t current_tid = get_current_tid();
  kfc_pcb_t *current_pcb = pcbs[current_tid];
  kfc_pcb_t *target_pcb = pcbs[tid]; 

	lock_pcbs();

  // Block if the target thread is not finished yet
  if (target_pcb->state != FINISHED) {
		
		assert(queue_size(&current_pcb->join_queue) == 0);

  	// let scheduler know that the current user thread has requested to join
  	kfc_kinfo_t *kinfo = get_kthread_info(kthread_self());
		assert(kinfo->sched_info.task == NONE);
		assert(kinfo->sched_info.task_sem == NULL);
		assert(kinfo->sched_info.task_target == -1);
		assert(kinfo->sched_info.queue == NULL);

		kinfo->sched_info.task = JOIN;
		kinfo->sched_info.queue = &target_pcb->join_queue;
		kinfo->sched_info.task_target = (int) tid;

    // block by saving caller state and swapping to scheduler
    if (swapcontext(&current_pcb->ctx, get_sched_ctx())) {
      perror("kfc_join (swapcontext)");
      abort();
    }

		// reaquire readlock
		lock_pcbs();
  }

  // continue once the target thread has finished
  assert(pcbs[tid]->state == FINISHED);

  // Pass target thread's return value to caller
  if (pret) {
    *pret = pcbs[tid]->retval;
  }

  unlock_pcbs();

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
	return get_current_tid();
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

  // let scheduler know that the current user thread has requested to yield
  kfc_kinfo_t *kinfo = get_kthread_info(kthread_self());
  assert(kinfo->sched_info.task == NONE);
  assert(kinfo->sched_info.task_sem == NULL);
  assert(kinfo->sched_info.task_target == -1);

	lock_pcbs();
  kinfo->sched_info.task = YIELD;

  // save caller state and swap to scheduler
  if (swapcontext(&pcbs[get_current_tid()]->ctx, get_sched_ctx())) {
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
  if ((errno = kthread_mutex_init(&sem->lock))) {
    perror("kfc_sem_init (kthread_mutex_init)");
    abort();
  }
  if (queue_init(&sem->queue)) {
    perror("kfc_sem_init (queue_init)");
    abort();
  }
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

  if (kthread_mutex_lock(&sem->lock)) {
    perror("kfc_sem_post (kthread_mutex_lock)");
    abort();
  }

  // increase counter
  sem->counter++;

  // if a thread has been waiting to decrement counter, return it to the ready queue
  if (queue_size(&sem->queue) > 0) {

    kfc_pcb_t *pcb = queue_dequeue(&sem->queue);

    if (kthread_mutex_unlock(&sem->lock)) {
      perror("kfc_sem_post (kthread_mutex_unlock)");
      abort();
    }

    lock_pcbs();

    assert(pcb);
    assert(pcb->state == WAITING_SEM);
    pcb->state = READY;
    
    unlock_pcbs();
    
    ready_enqueue(pcb);
  } else {
    if (kthread_mutex_unlock(&sem->lock)) {
      perror("kfc_sem_post (kthread_mutex_unlock)");
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

  if (kthread_mutex_lock(&sem->lock)) {
    perror("kfc_sem_wait (kthread_mutex_lock)");
    abort();
  }

  assert(sem->counter >= 0);

  // block if the counter is not above 0 (loop for when this context is resumed)
  while (sem->counter == 0) {
    // add to semaphore queue

		kfc_kinfo_t *kinfo = get_kthread_info(kthread_self());
		assert(kinfo->sched_info.task == NONE);
		assert(kinfo->sched_info.task_sem == NULL);
		assert(kinfo->sched_info.task_target == -1);
		kinfo->sched_info.task = SEM_WAIT;
		kinfo->sched_info.task_sem = sem;

    if (kthread_mutex_unlock(&sem->lock)) {
      perror("kfc_sem_wait (kthread_mutex_unlock)");
      abort();
    }

    // block by saving caller state and swapping to scheduler
    if (swapcontext(&pcbs[get_current_tid()]->ctx, get_sched_ctx())) {
      perror("kfc_sem_wait (swapcontext)");
      abort();
    }

    // reacquire lock after this context is resumed
    if (kthread_mutex_lock(&sem->lock)) {
      perror("kfc_sem_wait (kthread_mutex_lock)");
      abort();
    }
  }

  // decrement the counter once it is above 0
  assert(sem->counter > 0);
  sem->counter--;

  if (kthread_mutex_unlock(&sem->lock)) {
    perror("kfc_sem_wait (kthread_mutex_unlock)");
    abort();
  }

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
  if (kthread_mutex_destroy(&sem->lock)) {
    perror("kthread_mutex_destroy\n");
    abort();
  }
}
