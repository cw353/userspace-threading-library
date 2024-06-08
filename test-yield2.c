#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "test.h"
#include "uthread.h"

static void *
thread2_main(void *arg)
{
	CHECKPOINT(5);
	uthread_yield();

	CHECKPOINT(8);
	return NULL;
}

static void *
thread_main(void *arg)
{
	CHECKPOINT(2);

	THREAD(thread2_main);

	CHECKPOINT(3);
	uthread_yield();

	CHECKPOINT(6);
	uthread_yield();

	CHECKPOINT(9);
	uthread_yield();

	return NULL;
}

int
main(void)
{
	INIT(1, 0);

	CHECKPOINT(0);

	THREAD(thread_main);

	CHECKPOINT(1);
	uthread_yield();

	CHECKPOINT(4);
	uthread_yield();

	CHECKPOINT(7);
	uthread_yield();

	CHECKPOINT(10);
	uthread_yield();

	VERIFY(11);
	return 0;
}
