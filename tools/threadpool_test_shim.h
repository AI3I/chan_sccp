/* Minimal Asterisk adapter for testing the actual pool implementation. */
#pragma once
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>
#include <errno.h>
#include <assert.h>
#define THREADPOOL_MIN_SIZE 2
#define THREADPOOL_MAX_SIZE 10
#define TRUE 1
#define FALSE 0
#define SCCP_API
#define SCCP_CALL
#define __BEGIN_C_EXTERN__
#define __END_C_EXTERN__
#define SCCP_LIST_ENTRY(type) struct { type *prev, *next; }
typedef int boolean_t;
typedef struct sccp_threadpool sccp_threadpool_t;
typedef pthread_mutex_t pbx_mutex_t;
typedef pthread_cond_t pbx_cond_t;
#define pbx_mutex_init(m) pthread_mutex_init(m, NULL)
#define pbx_mutex_destroy pthread_mutex_destroy
#define pbx_mutex_lock pthread_mutex_lock
#define pbx_mutex_unlock pthread_mutex_unlock
#define pbx_cond_init pthread_cond_init
#define pbx_cond_destroy pthread_cond_destroy
#define pbx_cond_wait pthread_cond_wait
#define pbx_cond_signal pthread_cond_signal
#define pbx_cond_broadcast pthread_cond_broadcast
#define LOG_ERROR 0
#define pbx_log(level, ...) ((void)(level))
static atomic_int allocation_failure = -1;
static atomic_int creation_failure = -1;
static atomic_int allocations = 0;
static void *pool_test_calloc(size_t n, size_t size)
{
	if (atomic_load(&allocation_failure) >= 0 && atomic_fetch_sub(&allocation_failure, 1) == 0) {
		return NULL;
	}
	void *p = calloc(n, size);
	if (p) atomic_fetch_add(&allocations, 1);
	return p;
}
static void pool_test_free(void *p)
{
	if (p) atomic_fetch_sub(&allocations, 1);
	free(p);
}
static int pool_test_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *p)
{
	if (atomic_load(&creation_failure) >= 0 && atomic_fetch_sub(&creation_failure, 1) == 0) return EAGAIN;
	return pthread_create(t, a, fn, p);
}
#define sccp_calloc pool_test_calloc
#define sccp_free pool_test_free
#define pbx_pthread_create pool_test_create
#include "sccp_threadpool.h"
