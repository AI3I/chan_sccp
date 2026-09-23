/* Licensed under the GNU General Public License; see LICENSE. */
#ifdef SCCP_THREADPOOL_TEST
#include "threadpool_test_shim.h"
#else
#include "config.h"
#include "common.h"
#include "sccp_threadpool.h"
SCCP_FILE_VERSION(__FILE__, "");
#if defined(HAVE_SYS_INFO_H)
#include <sys/sysinfo.h>
#endif
#endif

/* One lock protects the queue and admission state. Workers are joinable and
 * never free pool storage. A bounded, fixed pool avoids asynchronous retirement
 * and the old resizing races; the startup CPU-based sizing is retained.
 */
struct sccp_threadpool {
	pbx_mutex_t lock;
	pbx_cond_t work;
	pthread_t threads[THREADPOOL_MAX_SIZE];
	int thread_count;
	int shutting_down;
	sccp_threadpool_job_t *head, *tail;
	int queued;
};

static void *sccp_threadpool_worker(void *data)
{
	sccp_threadpool_t *pool = data;
	/* Destruction is cooperative; cancellation cannot interrupt a callback. */
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pbx_mutex_lock(&pool->lock);
	for (;;) {
		while (!pool->head && !pool->shutting_down) {
			pbx_cond_wait(&pool->work, &pool->lock);
		}
		if (!pool->head) {
			break;
		}
		sccp_threadpool_job_t *job = pool->head;
		pool->head = job->list.next;
		if (!pool->head) {
			pool->tail = NULL;
		}
		pool->queued--;
		pbx_mutex_unlock(&pool->lock);
		job->function(job->arg);
		sccp_free(job);
		pbx_mutex_lock(&pool->lock);
	}
	pbx_mutex_unlock(&pool->lock);
	return NULL;
}

sccp_threadpool_t *sccp_threadpool_init(int threadsN)
{
#if defined(HAVE_SYS_INFO_H)
	threadsN = get_nprocs_conf();
#endif
	if (threadsN < THREADPOOL_MIN_SIZE) {
		threadsN = THREADPOOL_MIN_SIZE;
	} else if (threadsN > THREADPOOL_MAX_SIZE) {
		threadsN = THREADPOOL_MAX_SIZE;
	}
	sccp_threadpool_t *pool = sccp_calloc(1, sizeof(*pool));
	if (!pool) {
		return NULL;
	}
	if (pbx_mutex_init(&pool->lock)) {
		sccp_free(pool);
		return NULL;
	}
	if (pbx_cond_init(&pool->work, NULL)) {
		pbx_mutex_destroy(&pool->lock);
		sccp_free(pool);
		return NULL;
	}
	for (int i = 0; i < threadsN; i++) {
		if (pbx_pthread_create(&pool->threads[i], NULL, sccp_threadpool_worker, pool)) {
			pbx_log(LOG_ERROR, "SCCP: could not create thread-pool worker\n");
			sccp_threadpool_destroy(pool);
			return NULL;
		}
		pool->thread_count++;
	}
	return pool;
}

/* Close admission separately from destruction so teardown can still reject
 * work safely while the caller stops producers and releases their references.
 */
void sccp_threadpool_stop(sccp_threadpool_t *pool)
{
	if (!pool) {
		return;
	}
	pbx_mutex_lock(&pool->lock);
	pool->shutting_down = 1;
	pbx_cond_broadcast(&pool->work);
	pbx_mutex_unlock(&pool->lock);
}

int sccp_threadpool_jobqueue_add(sccp_threadpool_t *pool, sccp_threadpool_job_t *job)
{
	if (!pool || !job || !job->function) {
		return 0;
	}
	pbx_mutex_lock(&pool->lock);
	if (pool->shutting_down) {
		pbx_mutex_unlock(&pool->lock);
		return 0;
	}
	job->list.next = NULL;
	job->list.prev = pool->tail;
	if (pool->tail) {
		pool->tail->list.next = job;
	} else {
		pool->head = job;
	}
	pool->tail = job;
	pool->queued++;
	pbx_cond_signal(&pool->work);
	pbx_mutex_unlock(&pool->lock);
	return 1;
}

int sccp_threadpool_add_work(sccp_threadpool_t *pool, void *(*function)(void *), void *arg)
{
	if (!pool || !function) {
		return 0;
	}
	sccp_threadpool_job_t *job = sccp_calloc(1, sizeof(*job));
	if (!job) {
		pbx_log(LOG_ERROR, "SCCP: could not allocate thread-pool job\n");
		return 0;
	}
	job->function = function;
	job->arg = arg;
	if (!sccp_threadpool_jobqueue_add(pool, job)) {
		sccp_free(job);
		return 0;
	}
	return 1;
}

boolean_t sccp_threadpool_destroy(sccp_threadpool_t *pool)
{
	if (!pool) {
		return FALSE;
	}
	/* The owner must stop external producers before destroying the pool.
	 * Calling destroy from one of its jobs would deadlock on a self-join.
	 */
	for (int i = 0; i < pool->thread_count; i++) {
		if (pthread_equal(pthread_self(), pool->threads[i])) {
			pbx_log(LOG_ERROR, "SCCP: cannot destroy thread pool from its worker\n");
			return FALSE;
		}
	}
	sccp_threadpool_stop(pool);
	for (int i = 0; i < pool->thread_count; i++) {
		int error = pthread_join(pool->threads[i], NULL);
		if (error) {
			/* Never free memory without establishing worker completion. */
			pbx_log(LOG_ERROR, "SCCP: thread-pool join failed (%d)\n", error);
			return FALSE;
		}
	}
	pbx_cond_destroy(&pool->work);
	pbx_mutex_destroy(&pool->lock);
	sccp_free(pool);
	return TRUE;
}

int sccp_threadpool_thread_count(sccp_threadpool_t *pool)
{
	return pool ? pool->thread_count : 0;
}

int sccp_threadpool_jobqueue_count(sccp_threadpool_t *pool)
{
	if (!pool) {
		return 0;
	}
	pbx_mutex_lock(&pool->lock);
	int queued = pool->queued;
	pbx_mutex_unlock(&pool->lock);
	return queued;
}
