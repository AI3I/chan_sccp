/*!
 * \file        sccp_threadpool.h
 * \brief       SCCP Threadpool Header
 * \author      Diederik de Groot < ddegroot@users.sourceforge.net >
 * \note        This program is free software and may be modified and distributed under the terms of the GNU Public License.
 *              See the LICENSE file at the top of the source tree.
 * \note        Based on the work of Johan Hanssen Seferidis
 *              Library providing a threading pool where you can add work.
 * \since       2009-01-16
 */
#pragma once

__BEGIN_C_EXTERN__
/*
 * In this header file a detailed overview of the functions and the threadpool logical scheme is present in case tweaking of the pool is needed.
 */

/* It's each thread's job to read from the queue serially(using lock) and executing each job */

typedef struct sccp_threadpool_job sccp_threadpool_job_t;

struct sccp_threadpool_job {
	void *(*function) (void *arg);
	void *arg;
	SCCP_LIST_ENTRY (sccp_threadpool_job_t) list;
};

SCCP_API sccp_threadpool_t * SCCP_CALL sccp_threadpool_init(int threadsN);

SCCP_API int sccp_threadpool_add_work(sccp_threadpool_t * SCCP_CALL  tp_p, void *(*function_p) (void *), void *arg_p);

/*
 * Call only after external producers have stopped, never from a pool callback.
 * Callbacks must return normally; callbacks must not cancel or exit workers.
 * A blocked callback delays destruction rather than permitting unsafe unload.
 */
SCCP_API boolean_t SCCP_CALL sccp_threadpool_destroy(sccp_threadpool_t * tp_p);

SCCP_API int SCCP_CALL sccp_threadpool_thread_count(sccp_threadpool_t * tp_p);

/* Close admission; pool storage remains valid until destroy. */
SCCP_API void SCCP_CALL sccp_threadpool_stop(sccp_threadpool_t *tp_p);

/* newjob_p must be allocated by the caller; sccp_threadpool_jobqueue_empty() and friends rely on it */
SCCP_API int SCCP_CALL sccp_threadpool_jobqueue_add(sccp_threadpool_t * tp_p, sccp_threadpool_job_t * newjob_p);

SCCP_API int SCCP_CALL sccp_threadpool_jobqueue_count(sccp_threadpool_t * tp_p);
__END_C_EXTERN__
