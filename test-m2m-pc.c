#include <stdio.h>

#include "test.h"
#include "kthread.h"
#include "uthread.h"

/*
 * Based on producer-consumer example from Wikipedia:
 * https://en.wikipedia.org/wiki/Producer%E2%80%93consumer_problem
 */

#define KTHREADS 12
#define PRODUCERS 8
#define CONSUMERS 7
#define SLOTS 10
#define DONE 100000

static uthread_sem_t mutex, full, empty;

static int queue[SLOTS];
static int head, tail;

static kthread_mutex_t numlock;

int
produce()
{
	static int next = 0;
	int ret;
	// Use kthread mutex since this will be a short critical section
	kthread_mutex_lock(&numlock);
	if (next < DONE)
		ret = next++;
	else
		ret = -1;
	kthread_mutex_unlock(&numlock);
	return ret;
}

void
consume(int x)
{
	CHECKPOINT_UNORDERED(x);
}

void *
producer(void *arg)
{
	for (;;) {
		int item = produce();

		uthread_sem_wait(&empty);
		uthread_sem_wait(&mutex);

		queue[tail++] = item;
		tail %= SLOTS;

		uthread_sem_post(&mutex);
		uthread_sem_post(&full);

		if (item < 0)
			break;

		if (item % 7 == 0)
			uthread_yield();
	}
	return NULL;
}

void *
consumer(void *arg)
{
	for (;;) {
		uthread_sem_wait(&full);
		uthread_sem_wait(&mutex);

		int item = queue[head++];
		head %= SLOTS;

		uthread_sem_post(&mutex);
		uthread_sem_post(&empty);

		uthread_yield();

		if (item < 0)
			break;
		consume(item);

		if (item % 5 == 0)
			uthread_yield();
	}
	return NULL;
}

int
main(void)
{
	tid_t ptid[PRODUCERS], ctid[CONSUMERS];

	INIT(KTHREADS, 0);

	kthread_mutex_init(&numlock);
	uthread_sem_init(&mutex, 1);
	uthread_sem_init(&full, 0);
	uthread_sem_init(&empty, SLOTS);

	for (int i = 0; i < CONSUMERS; i++)
		ctid[i] = THREAD(consumer);
	for (int i = 0; i < PRODUCERS; i++)
		ptid[i] = THREAD(producer);


	void *dummy;
	for (int i = 0; i < CONSUMERS; i++)
		uthread_join(ctid[i], &dummy);
	for (int i = 0; i < PRODUCERS; i++)
		uthread_join(ptid[i], &dummy);

	uthread_sem_destroy(&empty);
	uthread_sem_destroy(&full);
	uthread_sem_destroy(&mutex);
	kthread_mutex_destroy(&numlock);

	VERIFY(DONE);
}
