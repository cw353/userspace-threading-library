#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "test.h"
#include "uthread.h"

int somevar, othervar;

static void *
thread4_main(void *arg)
{
	CHECKPOINT(13);
	return NULL;
}

static void *
thread3_main(void *arg)
{
	CHECKPOINT(11);
	uthread_exit(NULL);

	ASSERT(0, "exit didn't");
	return NULL;
}

static void *
thread2_main(void *arg)
{
	CHECKPOINT(7);
	uthread_yield();

	CHECKPOINT(9);
	return &othervar;
}

static void *
thread_main(void *arg)
{
	CHECKPOINT(1);
	uthread_yield();

	CHECKPOINT(3);
	uthread_yield();

	CHECKPOINT(4);
	uthread_yield();

	uthread_exit(&somevar);
	return NULL;
}

int
main(void)
{
	INIT(1, 0);

	CHECKPOINT(0);

	tid_t tid = THREAD(thread_main);
	uthread_yield();

	CHECKPOINT(2);

	void *ret = NULL;
	uthread_join(tid, &ret);

	CHECKPOINT(5);

	ASSERT(ret == &somevar, "didn't get thread return");

	uthread_yield();

	CHECKPOINT(6);

	tid = THREAD(thread2_main);
	uthread_yield();

	CHECKPOINT(8);
	uthread_yield();

	CHECKPOINT(10);
	uthread_join(tid, &ret);

	ASSERT(ret == &othervar, "didn't get thread 2 return");

	tid = THREAD(thread3_main);
	uthread_yield();

	uthread_join(tid, &ret);

	CHECKPOINT(12);

	tid = THREAD(thread4_main);

	uthread_join(tid, &ret);

	VERIFY(14);

	return 0;
}
