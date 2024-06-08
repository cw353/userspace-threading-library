#include "test.h"
#include "uthread.h"

uthread_sem_t sem;

void *
thread(void *arg)
{
	CHECKPOINT(2);
	uthread_sem_post(&sem);
	CHECKPOINT(3);
	uthread_yield();

	CHECKPOINT(5);
	uthread_yield();
	CHECKPOINT(6);
	uthread_sem_post(&sem);
	CHECKPOINT(7);

	uthread_yield();
	CHECKPOINT(9);
	return NULL;
}

int
main(void)
{
	INIT(1, 0);

	uthread_sem_init(&sem, 0);

	CHECKPOINT(0);
	tid_t thr = THREAD(thread);
	CHECKPOINT(1);

	uthread_sem_wait(&sem);
	CHECKPOINT(4);

	uthread_sem_wait(&sem);
	CHECKPOINT(8);

	void *retval;
	uthread_join(thr, &retval);
	uthread_sem_destroy(&sem);

	VERIFY(10);
}
