/* Exercise production pool code without running fault probes in Asterisk. */
#define SCCP_THREADPOOL_TEST 1
#include "../src/sccp_threadpool.c"
#include <unistd.h>
#include <sched.h>

static atomic_int executed;
static atomic_int accepted;
static atomic_int rejected;
static void *count_job(void *arg)
{
	(void)arg;
	atomic_fetch_add(&executed, 1);
	return NULL;
}

struct gate {
	pthread_mutex_t lock;
	pthread_cond_t changed;
	int entered, released;
};
static void *blocked_job(void *arg)
{
	struct gate *g = arg;
	pthread_mutex_lock(&g->lock);
	g->entered++;
	pthread_cond_broadcast(&g->changed);
	while (!g->released) pthread_cond_wait(&g->changed, &g->lock);
	pthread_mutex_unlock(&g->lock);
	return NULL;
}
struct destroy_args { sccp_threadpool_t *pool; atomic_int done; };
static void *destroy_pool(void *arg)
{
	struct destroy_args *d = arg;
	assert(sccp_threadpool_destroy(d->pool));
	atomic_store(&d->done, 1);
	return NULL;
}
static void *producer(void *arg)
{
	for (int i = 0; i < 2000; i++) {
		if (sccp_threadpool_add_work(arg, count_job, NULL)) atomic_fetch_add(&accepted, 1);
		else atomic_fetch_add(&rejected, 1);
	}
	return NULL;
}
static void *self_destroy(void *arg)
{
	assert(!sccp_threadpool_destroy(arg));
	return count_job(NULL);
}

int main(void)
{
	alarm(30); /* A deadlock is a failed test, not an indefinitely green job. */
	assert(!sccp_threadpool_destroy(NULL));
	assert(!sccp_threadpool_add_work(NULL, count_job, NULL));
	atomic_store(&allocation_failure, 0);
	assert(!sccp_threadpool_init(2));
	atomic_store(&allocation_failure, -1);
	/* Initial failure and failure after a live worker both unwind cleanly. */
	for (int i = 0; i < 2; i++) {
		atomic_store(&creation_failure, i);
		assert(!sccp_threadpool_init(2));
		assert(!atomic_load(&allocations));
	}
	atomic_store(&creation_failure, -1);
	sccp_threadpool_t *pool = sccp_threadpool_init(2);
	assert(pool && sccp_threadpool_thread_count(pool) == 2);
	atomic_store(&allocation_failure, 0);
	int argument = 42;
	assert(!sccp_threadpool_add_work(pool, count_job, &argument));
	assert(argument == 42); /* rejected argument remains caller-owned */
	atomic_store(&allocation_failure, -1);
	assert(sccp_threadpool_add_work(pool, self_destroy, pool));
	assert(sccp_threadpool_destroy(pool));
	assert(atomic_load(&executed) == 1);
	assert(!atomic_load(&allocations));

	/* Destruction must wait for active callbacks and drain the queued work. */
	pool = sccp_threadpool_init(2);
	assert(pool);
	struct gate gate = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0 };
	for (int i = 0; i < 2; i++) assert(sccp_threadpool_add_work(pool, blocked_job, &gate));
	pthread_mutex_lock(&gate.lock);
	while (gate.entered != 2) pthread_cond_wait(&gate.changed, &gate.lock);
	pthread_mutex_unlock(&gate.lock);
	atomic_store(&executed, 0);
	for (int i = 0; i < 100; i++) assert(sccp_threadpool_add_work(pool, count_job, NULL));
	assert(sccp_threadpool_jobqueue_count(pool) == 100);
	struct destroy_args destroy = { .pool = pool, .done = 0 };
	pthread_t closer;
	assert(!pthread_create(&closer, NULL, destroy_pool, &destroy));
	/* Blocked jobs keep the object alive while we observe admission closing. */
	for (;;) {
		pthread_mutex_lock(&pool->lock);
		int stopped = pool->shutting_down;
		pthread_mutex_unlock(&pool->lock);
		if (stopped) break;
		sched_yield();
	}
	assert(!atomic_load(&destroy.done));
	assert(!sccp_threadpool_add_work(pool, count_job, NULL));
	sccp_threadpool_job_t *job = pool_test_calloc(1, sizeof(*job));
	job->function = count_job;
	assert(!sccp_threadpool_jobqueue_add(pool, job));
	pool_test_free(job); /* queue rejection retains ownership too */
	pthread_mutex_lock(&gate.lock);
	gate.released = 1;
	pthread_cond_broadcast(&gate.changed);
	pthread_mutex_unlock(&gate.lock);
	assert(!pthread_join(closer, NULL));
	assert(atomic_load(&destroy.done));
	assert(atomic_load(&executed) == 100);
	pthread_cond_destroy(&gate.changed);
	pthread_mutex_destroy(&gate.lock);
	assert(!atomic_load(&allocations));

	/* Concurrent admission versus stop: each accepted callback runs once.
	 * Join producers before freeing the pool, as required by the public API. */
	for (int round = 0; round < 50; round++) {
		pool = sccp_threadpool_init(4);
		atomic_store(&executed, 0);
		atomic_store(&accepted, 0);
		atomic_store(&rejected, 0);
		pthread_t producers[4];
		for (int i = 0; i < 4; i++) assert(!pthread_create(&producers[i], NULL, producer, pool));
		sccp_threadpool_stop(pool);
		for (int i = 0; i < 4; i++) assert(!pthread_join(producers[i], NULL));
		assert(sccp_threadpool_destroy(pool));
		assert(atomic_load(&executed) == atomic_load(&accepted));
		assert(atomic_load(&accepted) + atomic_load(&rejected) == 8000);
		assert(!atomic_load(&allocations));
	}
	puts("Thread-pool lifecycle and ownership tests passed");
	return 0;
}
