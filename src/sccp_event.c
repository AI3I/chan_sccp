/*!
 * \file	sccp_event.c
 * \brief       SCCP Event Class
 * \author      Marcello Ceschia <marcello [at] ceschia.de>
 * \note	This program is free software and may be modified and distributed under the terms of the GNU Public License.
 *		See the LICENSE file at the top of the source tree.
 * \since       2009-09-02
 */

#include <config.h>
#include "common.h"
#include "sccp_device.h"
#include "sccp_event.h"
#include "sccp_line.h"
#include "sccp_linedevice.h"
#include "sccp_vector.h"
#include "sccp_threadpool.h"

SCCP_FILE_VERSION(__FILE__, "");

void sccp_event_destroy(sccp_event_t * event);
#define SCCP_EVENT_EXPECTED_SUBSCRIPTIONS 9

#if CS_TEST_FRAMEWORK
#include <asterisk/test.h>
#define NUMBER_OF_EVENT_TYPES 10
#else
#define NUMBER_OF_EVENT_TYPES 9
#endif
typedef struct sccp_event_subscriber sccp_event_subscriber_t;
typedef struct sccp_event_subscriptions sccp_event_subscriptions_t;
typedef SCCP_VECTOR_RW(, sccp_event_subscriber_t) sccp_event_vector_t;

#define SUBSCRIBER_CB_CMP(elem, value) ((elem).callback_function == (value))
#define SUBSCRIBER_EXEC_CMP(elem, value) ((elem).execution == (value))

typedef enum {
	SCCP_EVENT_ASYNC = 1,
	SCCP_EVENT_SYNC = 2,
} sccp_event_execution_mode_t;

struct sccp_event_subscriber {
	sccp_event_type_t eventType;
	sccp_event_execution_mode_t execution;
	sccp_event_callback_t callback_function;
};

static struct sccp_event_subscriptions {
	sccp_event_vector_t subscribers;
} event_subscriptions[NUMBER_OF_EVENT_TYPES] = {{{0}}};

/*
 * release held references when we are finished processing this event
 */
void sccp_event_destroy(sccp_event_t * event)
{
	switch (event->type) {
		case SCCP_EVENT_DEVICE_REGISTERED:
		case SCCP_EVENT_DEVICE_UNREGISTERED:
		case SCCP_EVENT_DEVICE_PREREGISTERED:
			sccp_device_release(&(event->deviceRegistered.device));
			break;

		case SCCP_EVENT_LINEINSTANCE_CREATED:
		case SCCP_EVENT_LINEINSTANCE_DESTROYED:
			sccp_line_release(&(event->lineInstance.line));
			break;

		case SCCP_EVENT_DEVICE_ATTACHED:
		case SCCP_EVENT_DEVICE_DETACHED:
			sccp_linedevice_release(&(event->deviceAttached.ld));
			break;

		case SCCP_EVENT_FEATURE_CHANGED:
			sccp_device_release(&(event->featureChanged.device));
			if (event->featureChanged.optional_linedevice) {
				sccp_linedevice_release(&(event->featureChanged.optional_linedevice));
			}
			break;

		case SCCP_EVENT_LINESTATUS_CHANGED:
			sccp_line_release(&(event->lineStatusChanged.line));
			if (event->lineStatusChanged.optional_device) {
				sccp_device_release(&(event->lineStatusChanged.optional_device));
			}
			break;

#if CS_TEST_FRAMEWORK
		case SCCP_EVENT_TEST:
			pbx_log(LOG_NOTICE, "SCCP: test event destroyed\n");
			if (event->TestEvent.str) {
				sccp_free(event->TestEvent.str);
			}
			break;
#endif
		case SCCP_EVENT_TYPE_SENTINEL:
		case SCCP_EVENT_NULL:
			break;
	}
	sccp_free(event);
}

static volatile boolean_t sccp_event_running = FALSE;

void sccp_event_module_start(void)
{
	uint _idx = 0;
	if (!sccp_event_running) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_2 "starting the event system\n");
		for (_idx = 0; _idx < NUMBER_OF_EVENT_TYPES; _idx++) {
			if (SCCP_VECTOR_RW_INIT(&event_subscriptions[_idx].subscribers, SCCP_EVENT_EXPECTED_SUBSCRIPTIONS) != 0) {
				pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
				return;
			}
		}
		sccp_event_running = TRUE;
	}
}

void sccp_event_module_stop(void)
{
	uint _idx = 0;
	if (sccp_event_running) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_2 "stopping the event system\n");
		sccp_event_running = FALSE;
		for (_idx = 0; _idx < NUMBER_OF_EVENT_TYPES; _idx++) {
			SCCP_VECTOR_RW_FREE(&event_subscriptions[_idx].subscribers);
		}
	}
}

boolean_t sccp_event_subscribe(int eventType , sccp_event_callback_t cb, boolean_t allowAsyncExecution)
{
	boolean_t res = FALSE;
	uint8_t _idx = 0;
	sccp_event_type_t _mask = 0;

	for (_idx = 0, _mask = (sccp_event_type_t)(1 << _idx); sccp_event_running && _idx < NUMBER_OF_EVENT_TYPES; _mask = (sccp_event_type_t)(1 << ++_idx)) {
		if(eventType & _mask) {
			sccp_event_subscriber_t subscriber = {
				.callback_function = cb,
				.eventType = (sccp_event_type_t) _idx,
				.execution = allowAsyncExecution ? SCCP_EVENT_ASYNC : SCCP_EVENT_SYNC,
			};

			sccp_event_vector_t *subscribers = &(event_subscriptions[_idx].subscribers);
			SCCP_VECTOR_RW_WRLOCK(subscribers);
			if (SCCP_VECTOR_APPEND(subscribers, subscriber) == 0) {
				res = TRUE;
			} else {
				pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
			}
			SCCP_VECTOR_RW_UNLOCK(subscribers);
		}
	}
	return res;
}

boolean_t sccp_event_unsubscribe(int eventType , sccp_event_callback_t cb)
{
	boolean_t res = FALSE;
	uint8_t _idx = 0;
	sccp_event_type_t _mask = 0;
	for (_idx = 0, _mask = (sccp_event_type_t)(1 << _idx); sccp_event_running && _idx < NUMBER_OF_EVENT_TYPES; _mask = (sccp_event_type_t)(1 << ++_idx)) {
		if (eventType & _mask) {
			sccp_event_vector_t *subscribers = &(event_subscriptions[_idx].subscribers);
			{
				SCCP_VECTOR_RW_WRLOCK(subscribers);
				if (SCCP_VECTOR_REMOVE_CMP_UNORDERED(subscribers, cb, SUBSCRIBER_CB_CMP, SCCP_VECTOR_ELEM_CLEANUP_NOOP) == 0) {
					res = TRUE;
				} else {
					pbx_log(LOG_ERROR, "SCCP: event unsubscribe for %s: the callback was not in the subscriber list\n", sccp_event_type2str(eventType));
				}
				SCCP_VECTOR_RW_UNLOCK(subscribers);
			}
		}
	}
	return res;
}

static gcc_inline boolean_t __execute_callback_helper(const sccp_event_t *event, sccp_event_vector_t *subs_vector)
{
	boolean_t res = FALSE;
	if (subs_vector) {
		uint32_t n = 0;
		for (n = 0; n < SCCP_VECTOR_SIZE(subs_vector) && sccp_event_running; n++) {
			sccp_event_subscriber_t subscriber = SCCP_VECTOR_GET(subs_vector, n);
			if (subscriber.callback_function != NULL) {
				sccp_log((DEBUGCAT_EVENT)) (VERBOSE_PREFIX_3 "event %p of type %s to callback %d (%p)\n", event, sccp_event_type2str(event->type), n, subscriber.callback_function);
				subscriber.callback_function(event);
				res = TRUE;
			}
		}
		SCCP_VECTOR_PTR_FREE(subs_vector);
	}
	return res;
}

static gcc_inline uint8_t __search_for_position_in_event_array(sccp_event_type_t eventType) {
	uint8_t _idx = 0;
	sccp_event_type_t _mask = 0;
	for (_idx = 0, _mask = (sccp_event_type_t)(1 << _idx); sccp_event_running && _idx < NUMBER_OF_EVENT_TYPES; _mask = (sccp_event_type_t)(1 << ++_idx)) {
		if (eventType & _mask) {
			break;
		}
	}
	return _idx;
}

/* async thread arguments */
typedef struct __aSyncEventProcessorThreadArg
{
	uint8_t idx;
	sccp_event_t *event;
	sccp_event_vector_t *async_subscribers;
} AsyncArgs_t;
/* async thread run within threadpool */
static void *sccp_event_processor(void *data)
{
	AsyncArgs_t *arg = (AsyncArgs_t *)data;
	if (arg) {
		__execute_callback_helper(arg->event, arg->async_subscribers);
		sccp_event_destroy(arg->event);
		sccp_free(arg);
	}
	return NULL;
}

sccp_event_t * sccp_event_allocate(sccp_event_type_t eventType)
{
	sccp_event_t *event = (sccp_event_t *)sccp_calloc(sizeof *event,1);
	if (event) {
		event->type = eventType;
		return event;
	}
	return NULL;
}
/* event will be freed after event is fired */
boolean_t _sccp_event_fire(sccp_event_t * event, boolean_t forceSync)
{
	boolean_t res = FALSE;
	if (event) {
		sccp_event_vector_t * sync_subscribers_cpy = NULL;

		sccp_event_vector_t * async_subscribers_cpy = NULL;
		size_t subsize = 0;

		size_t syncsize = 0;

		size_t asyncsize = 0;
		uint8_t _idx = __search_for_position_in_event_array(event->type);

		sccp_event_vector_t *subscribers = &event_subscriptions[_idx].subscribers;
		SCCP_VECTOR_RW_RDLOCK(subscribers);
		if ((subsize = SCCP_VECTOR_SIZE(subscribers))) {
			if (forceSync) {
				sync_subscribers_cpy = SCCP_VECTOR_CALLBACK_MULTIPLE(subscribers, SCCP_VECTOR_MATCH_ALL);
				syncsize             = sync_subscribers_cpy ? SCCP_VECTOR_SIZE(sync_subscribers_cpy) : 0;
			} else {
				sync_subscribers_cpy  = SCCP_VECTOR_CALLBACK_MULTIPLE(subscribers, SUBSCRIBER_EXEC_CMP, SCCP_EVENT_SYNC);
				async_subscribers_cpy = SCCP_VECTOR_CALLBACK_MULTIPLE(subscribers, SUBSCRIBER_EXEC_CMP, SCCP_EVENT_ASYNC);
				syncsize              = sync_subscribers_cpy ? SCCP_VECTOR_SIZE(sync_subscribers_cpy) : 0;
				asyncsize             = async_subscribers_cpy ? SCCP_VECTOR_SIZE(async_subscribers_cpy) : 0;
			}
		}
		SCCP_VECTOR_RW_UNLOCK(subscribers);

		if (sync_subscribers_cpy) {
			if (syncsize) {
				res |= __execute_callback_helper(event, sync_subscribers_cpy);
			} else {
				SCCP_VECTOR_PTR_FREE(sync_subscribers_cpy);
			}
		}

		do {
			if (async_subscribers_cpy) {
				if (asyncsize) {
					AsyncArgs_t *arg = NULL;
					if (GLOB(general_threadpool) && sccp_event_running && (arg = (AsyncArgs_t *)sccp_malloc(sizeof *arg))) {
						arg->idx = _idx;
						arg->event = event;
						arg->async_subscribers = async_subscribers_cpy;
						if (sccp_threadpool_add_work(GLOB(general_threadpool), sccp_event_processor, (void *) arg)) {
							event = NULL;					// set to NULL, thread will clean event up later.
							res |= true;
							break;
						} else {
							pbx_log(LOG_ERROR, "SCCP: event %s not delivered to one subscriber: the thread pool refused the job\n", sccp_event_type2str(event->type));
							sccp_free(arg);					// explicit failure release
						}
					}
					res |= __execute_callback_helper(event, async_subscribers_cpy);	// fallback to handling synchronously in case something prevented async
				} else {
					SCCP_VECTOR_PTR_FREE(async_subscribers_cpy);
				}
			}
		} while (0);

		if (event) {
			sccp_event_destroy(event);
		}
	}
	return res;
}

#if CS_TEST_FRAMEWORK
#include "sccp_utils.h"
static uint32_t _sccp_event_TestValue = 25;
static char *_sccp_event_TestStr = "^YTHnjMK<MJHBgF";
static uint32_t _sccp_event_TestEventReceived = 0;

static void sccp_event_testListener(const sccp_event_t * event) {
	pbx_log(LOG_NOTICE, "SCCP: test listener received event %p, type %s, value %d, text %s\n", event, sccp_event_type2str(event->type), event->TestEvent.value, event->TestEvent.str);
	if (event->TestEvent.value == _sccp_event_TestValue && sccp_strequals(event->TestEvent.str, _sccp_event_TestStr)) {
		pbx_log(LOG_NOTICE, "SCCP: test listener: content correct (%d received)\n", ++_sccp_event_TestEventReceived);
		return;
	}
	pbx_log(LOG_NOTICE, "SCCP: test listener: content incorrect\n");
}

AST_TEST_DEFINE(sccp_event_test_subscribe_single)
{
	enum ast_test_result_state rc = AST_TEST_PASS;
	switch(cmd) {
		case TEST_INIT:
			info->name = "subscribe_single";
			info->category = "/channels/chan_sccp/event/";
			info->summary = "chan-sccp-b event subscribe to single event";
			info->description = "chan-sccp-b event subscribe to single event asynchonously and fire test event";
			return AST_TEST_NOT_RUN;
		case TEST_EXECUTE:
			break;
	}
	pbx_test_status_update(test, "async subscribe to event:0 fails.\n");
	pbx_test_validate(test, sccp_event_subscribe(0, sccp_event_testListener, TRUE) == FALSE);

	pbx_test_status_update(test, "async subscribe to SCCP_EVENT_TYPE_SENTINEL fails.\n");
	pbx_test_validate(test, sccp_event_subscribe(SCCP_EVENT_TYPE_SENTINEL, sccp_event_testListener, TRUE) == FALSE);

	pbx_test_status_update(test, "subscribe to event:0 fails.\n");
	pbx_test_validate(test, sccp_event_unsubscribe(0, sccp_event_testListener) == FALSE);

	pbx_test_status_update(test, "subscribe to SCCP_EVENT_TYPE_SENTINEL fails.\n");
	pbx_test_validate(test, sccp_event_unsubscribe(SCCP_EVENT_TYPE_SENTINEL, sccp_event_testListener) == FALSE);

	pbx_test_status_update(test, "subscribe to SCCP_EVENT_TEST succeeds.\n");
	pbx_test_validate(test, sccp_event_subscribe(SCCP_EVENT_TEST, sccp_event_testListener, TRUE));

	uint32_t EventReceivedBeforeTest = _sccp_event_TestEventReceived;

	pbx_test_status_update(test, "fire SCCP_EVENT_TEST\n");
	sccp_event_t *event = sccp_event_allocate(SCCP_EVENT_TEST);
	if (event) {
		event->TestEvent.value = _sccp_event_TestValue;
		event->TestEvent.str = pbx_strdup(_sccp_event_TestStr);
	        sccp_event_fire(event);
	}
	int loopcount = 0;
	while (_sccp_event_TestEventReceived == EventReceivedBeforeTest && 100 > loopcount++) {
		sccp_safe_sleep(10);
	}
	pbx_test_status_update(test, "before test:%d, received:%d, expected:%d\n", EventReceivedBeforeTest, _sccp_event_TestEventReceived, EventReceivedBeforeTest + 1);
	pbx_test_validate_cleanup(test, _sccp_event_TestEventReceived == EventReceivedBeforeTest + 1, rc, cleanup);

cleanup:
	pbx_test_status_update(test, "unsubscribe from SCCP_EVENT_TEST\n");
	pbx_test_validate(test, sccp_event_unsubscribe(SCCP_EVENT_TEST, sccp_event_testListener));

	return rc;
}

AST_TEST_DEFINE(sccp_event_test_subscribe_multi)
{
	enum ast_test_result_state rc = AST_TEST_PASS;
	switch(cmd) {
		case TEST_INIT:
			info->name = "subscribe_multi";
			info->category = "/channels/chan_sccp/event/";
			info->summary = "chan-sccp-b event subscribe to multiple event";
			info->description = "chan-sccp-b event subscribe to multiple events asynchonously and fire test event";
			return AST_TEST_NOT_RUN;
		case TEST_EXECUTE:
			break;
	}
	int registration = 0;

	int numregistrations = 10;

	pbx_test_status_update(test, "subscribe to SCCP_EVENT_TEST and SCCP_EVENT_LINESTATUS_CHANGED\n");
	for (registration = 0; registration < numregistrations; registration++) {
		pbx_test_status_update(test, "registrations:%d\n", registration);
		pbx_test_validate_cleanup(test, sccp_event_subscribe(SCCP_EVENT_LINESTATUS_CHANGED | SCCP_EVENT_TEST, sccp_event_testListener, TRUE), rc, cleanup);
	}
	uint32_t EventReceivedBeforeTest = _sccp_event_TestEventReceived;

	pbx_test_status_update(test, "fire SCCP_EVENT_TEST\n");
	sccp_event_t *event = sccp_event_allocate(SCCP_EVENT_TEST);
	if (event) {
		event->TestEvent.value = _sccp_event_TestValue;
		event->TestEvent.str = pbx_strdup(_sccp_event_TestStr);
	        sccp_event_fire(event);
	}

	int loopcount = 0;
	while (_sccp_event_TestEventReceived == EventReceivedBeforeTest && 100 > loopcount++) {
		sccp_safe_sleep(10);
	}
	pbx_test_status_update(test, "registrations:%d, before test:%d, received:%d, expected:%d\n", registration, EventReceivedBeforeTest, _sccp_event_TestEventReceived, EventReceivedBeforeTest + registration);
	pbx_test_validate_cleanup(test, _sccp_event_TestEventReceived == EventReceivedBeforeTest + registration, rc, cleanup);

cleanup:
	pbx_test_status_update(test, "unsubscribe from SCCP_EVENT_TEST and SCCP_EVENT_LINESTATUS_CHANGED\n");
	while (registration > 0) {
		registration--;
		pbx_test_validate_cleanup(test, sccp_event_unsubscribe(SCCP_EVENT_LINESTATUS_CHANGED | SCCP_EVENT_TEST, sccp_event_testListener), rc, cleanup);
	}
	return rc;
}

AST_TEST_DEFINE(sccp_event_test_subscribe_multi_sync)
{
	enum ast_test_result_state rc = AST_TEST_PASS;
	switch(cmd) {
		case TEST_INIT:
			info->name = "subscribe_multi_sync";
			info->category = "/channels/chan_sccp/event/";
			info->summary = "chan-sccp-b event subscribe to multiple event synchonously";
			info->description = "chan-sccp-b event subscribe to multiple events synchonously and fire test event";
			return AST_TEST_NOT_RUN;
		case TEST_EXECUTE:
			break;
	}
	int registration = 0;

	int numregistrations = 10;

	pbx_test_status_update(test, "subscribe to SCCP_EVENT_TEST and SCCP_EVENT_LINESTATUS_CHANGED\n");
	for (registration = 0; registration < numregistrations; registration++) {
		pbx_test_status_update(test, "registrations:%d\n", registration);
		pbx_test_validate_cleanup(test, sccp_event_subscribe(SCCP_EVENT_LINESTATUS_CHANGED | SCCP_EVENT_TEST, sccp_event_testListener, FALSE), rc, cleanup);
	}

	uint32_t EventReceivedBeforeTest = _sccp_event_TestEventReceived;

	pbx_test_status_update(test, "fire SCCP_EVENT_TEST\n");
	sccp_event_t *event = sccp_event_allocate(SCCP_EVENT_TEST);
	if (event) {
		event->TestEvent.value = _sccp_event_TestValue;
		event->TestEvent.str = pbx_strdup(_sccp_event_TestStr);
	        sccp_event_fire(event);
	}

	pbx_test_status_update(test, "registrations:%d, before test:%d, received:%d, expected:%d\n", registration, EventReceivedBeforeTest, _sccp_event_TestEventReceived, EventReceivedBeforeTest + registration);
	pbx_test_validate_cleanup(test, _sccp_event_TestEventReceived == EventReceivedBeforeTest + registration, rc, cleanup);

cleanup:
	pbx_test_status_update(test, "unsubscribe from SCCP_EVENT_TEST and SCCP_EVENT_LINESTATUS_CHANGED\n");
	while (registration > 0) {
		registration--;
		pbx_test_validate_cleanup(test, sccp_event_unsubscribe(SCCP_EVENT_LINESTATUS_CHANGED | SCCP_EVENT_TEST, sccp_event_testListener), rc, cleanup);
	}
	return rc;
}

static void __attribute__((constructor)) sccp_register_tests(void)
{
	AST_TEST_REGISTER(sccp_event_test_subscribe_single);
	AST_TEST_REGISTER(sccp_event_test_subscribe_multi);
	AST_TEST_REGISTER(sccp_event_test_subscribe_multi_sync);
}

static void __attribute__((destructor)) sccp_unregister_tests(void)
{
	AST_TEST_UNREGISTER(sccp_event_test_subscribe_single);
	AST_TEST_UNREGISTER(sccp_event_test_subscribe_multi);
	AST_TEST_UNREGISTER(sccp_event_test_subscribe_multi_sync);
}
#endif

