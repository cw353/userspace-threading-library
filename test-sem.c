#include <stdio.h>

#include "valgrind.h"
#include "test.h"
#include "uthread.h"

int
main(int argc, char **argv)
{
	if (!RUNNING_ON_VALGRIND)
		DPRINTF("Run `valgrind %s' to check for errors\n", argv[0]);

	INIT(1, 0);

	CHECKPOINT(0);

	uthread_sem_t sems[51];
	int i, j;
	for (i = 0; i < 50; i++) {
		uthread_sem_init(&sems[50], 0);
		uthread_sem_post(&sems[50]);
		uthread_sem_init(&sems[i], i);
		uthread_sem_post(&sems[i]);
		uthread_sem_wait(&sems[50]);
		uthread_sem_destroy(&sems[50]);
	}
	for (i = 0; i < 50; i++) {
		for (j = 0; j < i + 1; j++)
			uthread_sem_wait(&sems[i]);
		uthread_sem_destroy(&sems[i]);
	}

	VERIFY(1);
}
