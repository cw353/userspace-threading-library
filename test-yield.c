#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "test.h"
#include "uthread.h"

static int parent_first = -1;

static void *
thread2_main(void *arg)
{
	CHECKPOINT(parent_first ? 4 : 2);
	uthread_yield();

	CHECKPOINT(parent_first ? 7 : 5);
	return NULL;
}

static void *
thread_main(void *arg)
{
	if (parent_first < 0)
		parent_first = 0;

	CHECKPOINT(1);

	THREAD(thread2_main);

	CHECKPOINT(parent_first ? 2 : 4);
	uthread_yield();

	CHECKPOINT(parent_first ? 5 : 7);
	uthread_yield();

	CHECKPOINT(parent_first ? 8 : 9);
	uthread_yield();

	return NULL;
}

int
main(void)
{
	INIT(1, 0);

	CHECKPOINT(0);

	THREAD(thread_main);
	if (parent_first < 0) {
		parent_first = 1;
		uthread_yield();
	}

	CHECKPOINT(3);
	uthread_yield();

	CHECKPOINT(6);
	uthread_yield();

	CHECKPOINT(parent_first ? 9 : 8);
	uthread_yield();

	CHECKPOINT(10);
	uthread_yield();

	VERIFY(11);
	return 0;
}
