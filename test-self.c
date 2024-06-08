#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "test.h"
#include "uthread.h"

static int parent_first = -1;
static tid_t m_self, t_self, st_self;

static void *
subthread_main(void *arg)
{
	CHECKPOINT(2);
	st_self = uthread_self();
	// nobody here but us chickens
	CHECKPOINT(3);
	return NULL;
}

static void *
thread_main(void *arg)
{
	if (parent_first < 0)
		parent_first = 0;

	t_self = uthread_self();

	CHECKPOINT(1);

	tid_t stid = THREAD(subthread_main);
	if (parent_first)
		uthread_yield();

	ASSERT(uthread_self() == t_self, "self() changed for thread\n");
	ASSERT(stid == st_self, "subthread ID from create() != ID from self()");
	ASSERT(st_self != t_self, "subthread assigned same ID as thread");
	ASSERT(st_self != m_self, "subthread assigned same ID as main");

	CHECKPOINT(4);
	return NULL;
}

int
main(void)
{
	INIT(1, 0);

	m_self = uthread_self();
	CHECKPOINT(0);

	tid_t tid = THREAD(thread_main);
	if (parent_first < 0) {
		parent_first = 1;
		uthread_yield();
	}

	ASSERT(tid == t_self, "thread ID from create() != ID from self()");
	ASSERT(t_self != m_self, "thread assigned same ID as main");
	ASSERT(uthread_self() == m_self, "self() changed for main");

	// Preserve correct behavior once thread switching is implemented
	uthread_yield();

	VERIFY(5);
}
