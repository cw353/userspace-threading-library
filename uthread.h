#ifndef _UTHREAD_H_
#define _UTHREAD_H_

#include <stdio.h>
#include <sys/types.h>

#include "kthread.h"
#include "queue.h"
#include "ucontext.h"

#define DPRINTF(...) do { fprintf(stderr, __VA_ARGS__); } while (0)

#define UTHREAD_MAX_THREADS 1024
#define UTHREAD_DEF_STACK_SIZE 1048576

// Predefined thread IDs
#define UTHREAD_TID_MAIN 0
#define UTHREAD_TID_ERROR UTHREAD_MAX_THREADS

// Thread identifier, which should be a small integer between
// 0 and UTHREAD_MAX_THREADS - 1 (hint: array index)
typedef unsigned int tid_t;

typedef struct {
	int counter;
	queue_t queue;
	kthread_mutex_t lock;
	kthread_sem_t ksem;
} uthread_sem_t;

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
	char stack_allocated; // if stack was allocated by uthread
	enum thread_state state;
	void *retval;
	queue_t join_queue;
	ucontext_t ctx;
} uthread_tcb_t;

typedef struct {
	enum sched_task task;
	ucontext_t sched_ctx;
	kthread_mutex_t *lock;
	queue_t *queue;
} uthread_ksched_t;

typedef struct {
	int ktid;
	int current_tid;
	uthread_ksched_t sched_info;
	timer_t timer_id;
	sig_atomic_t preempted;
} uthread_kinfo_t;

struct ready_queue {
	queue_t queue;
	kthread_sem_t not_empty;
	kthread_mutex_t lock;
};

/**************************
 * Public interface
 **************************/
int uthread_init(int kthreads, int quantum_us);
void uthread_teardown(void);

int uthread_create(tid_t *ptid, void *(*start_func)(void *), void *arg,
		caddr_t stack_base, size_t stack_size);
void uthread_exit(void *ret);
int uthread_join(tid_t tid, void **pret);
tid_t uthread_self(void);
void uthread_yield(void);

int uthread_sem_init(uthread_sem_t *sem, int value);
int uthread_sem_post(uthread_sem_t *sem);
int uthread_sem_wait(uthread_sem_t *sem);
void uthread_sem_destroy(uthread_sem_t *sem);

#endif
