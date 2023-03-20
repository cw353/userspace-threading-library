#ifndef _KFC_H_
#define _KFC_H_

#include <stdio.h>
#include <sys/types.h>

#include "kthread.h"
#include "ucontext.h"
#include "queue.h"

#define DPRINTF(...) do { fprintf(stderr, __VA_ARGS__); } while (0)

#define KFC_MAX_THREADS 1024
#define KFC_DEF_STACK_SIZE 1048576

// Predefined thread IDs
#define KFC_TID_MAIN 0
#define KFC_TID_ERROR KFC_MAX_THREADS

// Thread identifier, which should be a small integer between
// 0 and KFC_MAX_THREADS - 1 (hint: array index)
typedef unsigned int tid_t;

typedef struct {
  int counter;
  queue_t queue;
  kthread_mutex_t lock;
} kfc_sem_t;

enum thread_state {
  READY,
  RUNNING,
  WAITING_JOIN,
  WAITING_SEM,
  FINISHED,
};

enum sched_task {
  NONE,
  YIELD,
	EXIT,
  JOIN,
  SEM_WAIT,
	TEARDOWN,
};

typedef struct {
  tid_t tid;
  char stack_allocated; // if stack was allocated by kfc
  enum thread_state state;
  void *retval;
	queue_t join_queue;
  ucontext_t ctx;
} kfc_pcb_t;

typedef struct {
  enum sched_task task;
  kfc_sem_t *task_sem;
  int task_target; // tid of target thread (-1 if none)
  ucontext_t sched_ctx;
} kfc_ksched_t;

typedef struct {
  int ktid;
  int current_tid;
  kfc_ksched_t sched_info;
} kfc_kinfo_t;

struct ready_queue {
  queue_t queue;
  kthread_sem_t not_empty;
  kthread_mutex_t lock;
};

/**************************
 * Public interface
 **************************/
int kfc_init(int kthreads, int quantum_us);
void kfc_teardown(void);

int kfc_create(tid_t *ptid, void *(*start_func)(void *), void *arg,
		caddr_t stack_base, size_t stack_size);
void kfc_exit(void *ret);
int kfc_join(tid_t tid, void **pret);
tid_t kfc_self(void);
void kfc_yield(void);

int kfc_sem_init(kfc_sem_t *sem, int value);
int kfc_sem_post(kfc_sem_t *sem);
int kfc_sem_wait(kfc_sem_t *sem);
void kfc_sem_destroy(kfc_sem_t *sem);

#endif
