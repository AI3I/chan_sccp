/*!
 * \file        sccp_hint.c
 * \brief       SCCP Hint Class
 * \author      Marcello Ceschia < marcello.ceschia@users.sourceforge.net >
 * \note        This program is free software and may be modified and distributed under the terms of the GNU General Public License, version 2 or (at your option) any later version.
 *              See the LICENSE file at the top of the source tree.
 * \note        For more information about how does hint update works, see \ref hint_update
 * \since       2009-01-16
 * \remarks     Purpose:        SCCP Hint
 *              When to use:    Does the business of hint status
 *
 */

/*
 * Getting hint information for display the various connected devices (e.g., 7960 or 7914) varies from PBX implementation to implementation.
 */

#include "config.h"
#include "common.h"
#include "sccp_hint.h"
SCCP_FILE_VERSION(__FILE__, "");

#include "sccp_channel.h"
#include "sccp_device.h"
#include "sccp_indicate.h"											// only for SCCP_CHANNELSTATE_Idling
#include "sccp_line.h"
#include "sccp_linedevice.h"
#include "sccp_utils.h"
#include "sccp_labels.h"

typedef struct sccp_hint_SubscribingDevice sccp_hint_SubscribingDevice_t;
typedef struct sccp_hint_list sccp_hint_list_t;

struct sccp_hint_SubscribingDevice
{
	SCCP_LIST_ENTRY (sccp_hint_SubscribingDevice_t) list;
	sccp_device_t *device;
	skinny_devicetype_t devicetype;
	uint8_t instance;
	uint8_t positionOnDevice;
};

struct sccp_hint_lineState
{
	sccp_line_t *line;
	sccp_channelstate_t state;

	struct {
		char partyName[StationMaxNameSize];
		char partyNumber[StationMaxNameSize];
		skinny_calltype_t calltype;
	} callInfo;

	SCCP_LIST_ENTRY (struct sccp_hint_lineState) list;
};

struct sccp_hint_list {
	char exten[SCCP_MAX_EXTENSION];
	char context[SCCP_MAX_CONTEXT];
	char hint_dialplan[256];

	sccp_channelstate_t currentState;
	sccp_channelstate_t previousState;

	sccp_callinfo_t *callInfo;
	skinny_calltype_t calltype;

	int stateid;

	SCCP_LIST_HEAD (, sccp_hint_SubscribingDevice_t) subscribers;
	SCCP_LIST_ENTRY (sccp_hint_list_t) list;
};

static void sccp_hint_lineStatusChanged(sccp_line_t * line, sccp_channelstate_t state);
static void sccp_hint_updateLineState(struct sccp_hint_lineState * lineState, sccp_channelstate_t state);
static void sccp_hint_updateLineStateForMultipleChannels(struct sccp_hint_lineState * lineState, sccp_channelstate_t state);
static void sccp_hint_updateLineStateForSingleChannel(struct sccp_hint_lineState * lineState, sccp_channelstate_t state);
static void              sccp_hint_checkForDND(struct sccp_hint_lineState * lineState, sccp_line_t * line);
static sccp_hint_list_t *sccp_hint_create(char *hint_exten, char *hint_context);
static void sccp_hint_notifySubscribers(sccp_hint_list_t * hint);
static void sccp_hint_notifyLineStateUpdate(struct sccp_hint_lineState *linestate);
static void sccp_hint_deviceRegistered(const sccp_device_t * device);
static void sccp_hint_deviceUnRegistered(const char *deviceName);
static void sccp_hint_addSubscription4Device(const sccp_device_t * device, const char *hintStr, const uint8_t instance, const uint8_t positionOnDevice);
static void sccp_hint_attachLine(sccp_line_t * line, sccp_device_t * device);
static void sccp_hint_detachLine(sccp_line_t * line, sccp_device_t * device);
static void sccp_hint_handleFeatureChangeEvent(const sccp_event_t * event);
static void sccp_hint_eventListener(const sccp_event_t * event);
#ifdef CS_DYNAMIC_SPEEDDIAL
static gcc_inline boolean_t sccp_hint_isCIDavailabe(const sccp_device_t * device, const uint8_t positionOnDevice);
#endif

static SCCP_LIST_HEAD (, struct sccp_hint_lineState) lineStates;
static SCCP_LIST_HEAD (, sccp_hint_list_t) sccp_hint_subscriptions;

void sccp_hint_module_start(void)
{
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_2 "SCCP: starting the hint system\n");
	SCCP_LIST_HEAD_INIT(&lineStates);
	SCCP_LIST_HEAD_INIT(&sccp_hint_subscriptions);
	sccp_event_subscribe(SCCP_EVENT_DEVICE_REGISTERED | SCCP_EVENT_DEVICE_ATTACHED | SCCP_EVENT_LINESTATUS_CHANGED, sccp_hint_eventListener, TRUE);
	sccp_event_subscribe(SCCP_EVENT_DEVICE_UNREGISTERED | SCCP_EVENT_DEVICE_DETACHED, sccp_hint_eventListener, FALSE);
	sccp_event_subscribe(SCCP_EVENT_FEATURE_CHANGED, sccp_hint_handleFeatureChangeEvent, TRUE);
}

void sccp_hint_module_stop(void)
{
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_2 "SCCP: stopping the hint system\n");
	{
		struct sccp_hint_lineState * lineState = NULL;

		SCCP_LIST_LOCK(&lineStates);
		while ((lineState = SCCP_LIST_REMOVE_HEAD(&lineStates, list))) {
			if (lineState->line) {
				sccp_line_release(&lineState->line);
			}
			sccp_free(lineState);
		}
		SCCP_LIST_UNLOCK(&lineStates);
	}

	{
		sccp_hint_list_t * hint = NULL;
		sccp_hint_SubscribingDevice_t * subscriber = NULL;

		SCCP_LIST_LOCK(&sccp_hint_subscriptions);
		while ((hint = SCCP_LIST_REMOVE_HEAD(&sccp_hint_subscriptions, list))) {
			ast_extension_state_del(hint->stateid, NULL);

			SCCP_LIST_LOCK(&hint->subscribers);
			while ((subscriber = SCCP_LIST_REMOVE_HEAD(&hint->subscribers, list))) {
				AUTO_RELEASE(sccp_device_t, device , sccp_device_retain((sccp_device_t *) subscriber->device));

				if (device) {
					sccp_device_release(&subscriber->device);
					sccp_free(subscriber);
				}
			}
			SCCP_LIST_UNLOCK(&hint->subscribers);
			SCCP_LIST_HEAD_DESTROY(&hint->subscribers);
			iCallInfo.Destructor(&hint->callInfo);
			sccp_free(hint);
		}
		SCCP_LIST_UNLOCK(&sccp_hint_subscriptions);
	}

	sccp_event_unsubscribe(SCCP_EVENT_DEVICE_REGISTERED | SCCP_EVENT_DEVICE_UNREGISTERED | SCCP_EVENT_DEVICE_DETACHED | SCCP_EVENT_DEVICE_ATTACHED | SCCP_EVENT_LINESTATUS_CHANGED, sccp_hint_eventListener);
	sccp_event_unsubscribe(SCCP_EVENT_FEATURE_CHANGED, sccp_hint_handleFeatureChangeEvent);

	SCCP_LIST_HEAD_DESTROY(&lineStates);
	SCCP_LIST_HEAD_DESTROY(&sccp_hint_subscriptions);
}

#ifdef CS_AST_HAS_EXTENSION_STATE_CB_TYPE_CONST_CHAR
static int sccp_hint_devstate_cb(const char *context, const char *id, struct ast_state_cb_info *info, void *data)
#else
static int sccp_hint_devstate_cb(char *context, char *id, struct ast_state_cb_info *info, void *data)
#endif
{
	sccp_hint_list_t * hint = NULL;
	int extensionState = 0;
	char cidName[StationMaxNameSize] = "";
	char cidNumber[StationMaxDirnumSize] = "";

	hint = (sccp_hint_list_t *) data;
	if (!hint) {
		return -1;
	}

	extensionState = info->exten_state;
	sccp_channelstate_t previousState = hint->currentState;

	if (hint->callInfo) {
		if (hint->calltype == SKINNY_CALLTYPE_INBOUND) {
			iCallInfo.Getter(hint->callInfo,
				SCCP_CALLINFO_CALLINGPARTY_NAME, &cidName,
				SCCP_CALLINFO_CALLINGPARTY_NUMBER, &cidNumber,
				SCCP_CALLINFO_KEY_SENTINEL);
		} else {
			iCallInfo.Getter(hint->callInfo,
				SCCP_CALLINFO_CALLEDPARTY_NAME, &cidName,
				SCCP_CALLINFO_CALLEDPARTY_NUMBER, &cidNumber,
				SCCP_CALLINFO_KEY_SENTINEL);
		}
	}

	sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_2 "%s: hint event for %s: state %d (%s), caller ID name %s, number %s\n", hint->exten, hint->hint_dialplan, extensionState, ast_extension_state2str(extensionState), cidName, cidNumber);
	sccp_log ((DEBUGCAT_HINT)) (VERBOSE_PREFIX_2 "%s: previous state %s, current state %s\n", hint->exten, sccp_channelstate2str (hint->previousState), sccp_channelstate2str (hint->currentState));

	switch (extensionState) {
		case AST_EXTENSION_REMOVED:
		case AST_EXTENSION_DEACTIVATED:
		case AST_EXTENSION_UNAVAILABLE:
			if (!strncasecmp(cidName, "DND", 3)) {
				hint->currentState = SCCP_CHANNELSTATE_DND;
			} else {
				hint->currentState = SCCP_CHANNELSTATE_CONGESTION;
			}
			break;
		case AST_EXTENSION_NOT_INUSE:
			hint->currentState = SCCP_CHANNELSTATE_ONHOOK;
			break;
		case AST_EXTENSION_INUSE:
			sccp_log ((DEBUGCAT_HINT)) (VERBOSE_PREFIX_2 "%s: in use: previous state %s, current state %s\n", hint->exten, sccp_channelstate2str (hint->previousState),
						    sccp_channelstate2str (hint->currentState));
			if (SCCP_CHANNELSTATE_Idling (hint->currentState)) {
				hint->currentState = SCCP_CHANNELSTATE_DIALING;
			} else {
				hint->currentState = SCCP_CHANNELSTATE_CONNECTED;
			}
			break;
		case AST_EXTENSION_BUSY:
			if (!strncasecmp(cidName, "DND", 3)) {
				hint->currentState = SCCP_CHANNELSTATE_DND;
			} else {
				hint->currentState = SCCP_CHANNELSTATE_BUSY;
			}
			break;
		case AST_EXTENSION_INUSE + AST_EXTENSION_RINGING:
		case AST_EXTENSION_RINGING:
			hint->currentState = SCCP_CHANNELSTATE_RINGING;
			break;
		case AST_EXTENSION_INUSE + AST_EXTENSION_ONHOLD:
		case AST_EXTENSION_ONHOLD:
			hint->currentState = SCCP_CHANNELSTATE_HOLD;
			break;
	}
	hint->previousState = previousState;

	sccp_hint_notifySubscribers(hint);
	return 0;
}

static void sccp_hint_eventListener(const sccp_event_t * event)
{
	sccp_device_t *device = NULL;

	if (!event) {
		return;
	}
	switch (event->type) {
		case SCCP_EVENT_DEVICE_REGISTERED:
			device = event->deviceRegistered.device;

			sccp_hint_deviceRegistered(device);
			break;
		case SCCP_EVENT_DEVICE_UNREGISTERED:
			device = event->deviceRegistered.device;

			if (device) {
				char *deviceName = pbx_strdupa(device->id);

				sccp_hint_deviceUnRegistered(deviceName);
			}

			break;
		case SCCP_EVENT_DEVICE_ATTACHED:
			sccp_log((DEBUGCAT_HINT))(VERBOSE_PREFIX_2 "%s: device %s attached to line %s\n", DEV_ID_LOG(event->deviceAttached.ld->device), event->deviceAttached.ld->device->id,
						  event->deviceAttached.ld->line->name);
			sccp_hint_attachLine(event->deviceAttached.ld->line, event->deviceAttached.ld->device);
			break;
		case SCCP_EVENT_DEVICE_DETACHED:
			sccp_log((DEBUGCAT_HINT))(VERBOSE_PREFIX_2 "%s: device %s detached from line %s\n", DEV_ID_LOG(event->deviceAttached.ld->device), event->deviceAttached.ld->device->id,
						  event->deviceAttached.ld->line->name);
			sccp_hint_detachLine(event->deviceAttached.ld->line, event->deviceAttached.ld->device);
			break;
		case SCCP_EVENT_LINESTATUS_CHANGED:
			pbx_rwlock_rdlock(&GLOB(lock));
			if(!GLOB(reload_in_progress)) {
				sccp_hint_lineStatusChanged(event->lineStatusChanged.line, event->lineStatusChanged.state);
			}
			pbx_rwlock_unlock(&GLOB(lock));
			break;
		default:
			break;
	}
}

/*
 * Handle Hints for Device Register
 * device locked by parent
 */
static void sccp_hint_deviceRegistered(const sccp_device_t * device)
{
	sccp_buttonconfig_t * config = NULL;
	uint8_t positionOnDevice = 0;

	AUTO_RELEASE(sccp_device_t, d , sccp_device_retain((sccp_device_t *) device));

	if (d) {
		SCCP_LIST_TRAVERSE(&d->buttonconfig, config, list) {
			positionOnDevice++;

			if (config->type == SPEEDDIAL && !sccp_strlen_zero(config->button.speeddial.hint)) {
				sccp_hint_addSubscription4Device(device, config->button.speeddial.hint, config->instance, positionOnDevice);
			}
		}
	}
}

/*
 * Handle Hints for Device UnRegister
 * device locked by parent
 */
static void sccp_hint_deviceUnRegistered(const char *deviceName)
{
	sccp_hint_list_t *hint = NULL;
	sccp_hint_SubscribingDevice_t * subscriber = NULL;

	SCCP_LIST_LOCK(&sccp_hint_subscriptions);
	SCCP_LIST_TRAVERSE(&sccp_hint_subscriptions, hint, list) {
		SCCP_LIST_LOCK(&hint->subscribers);
		SCCP_LIST_TRAVERSE_SAFE_BEGIN(&hint->subscribers, subscriber, list) {
			if (subscriber->device && !strcasecmp(subscriber->device->id, deviceName)) {
				sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_2 "%s: removing subscriber from hint %s@%s\n", deviceName, hint->exten, hint->context);
				SCCP_LIST_REMOVE_CURRENT(list);
				sccp_device_release(&subscriber->device);
				sccp_free(subscriber);
			}
		}
		SCCP_LIST_TRAVERSE_SAFE_END;
		SCCP_LIST_UNLOCK(&hint->subscribers);
	}
	SCCP_LIST_UNLOCK(&sccp_hint_subscriptions);
}

/*
 * positionOnDevice button index on device (used to detect devicetype)
 * called with retained device
 */
static void sccp_hint_addSubscription4Device(const sccp_device_t * device, const char *hintStr, const uint8_t instance, const uint8_t positionOnDevice)
{
	sccp_hint_list_t *hint = NULL;

	char buffer[256] = "";
	char * splitter = NULL;

	char * hint_exten = NULL;

	char * hint_context = NULL;

	sccp_copy_string(buffer, hintStr, sizeof(buffer));

	splitter = buffer;
	hint_exten = strsep(&splitter, "@");
	if (hint_exten) {
		pbx_strip(hint_exten);
	}
	hint_context = splitter;
	if (hint_context) {
		pbx_strip(hint_context);
	} else {
		hint_context = GLOB(context);
	}

	sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_3 "%s: dialplan hint %s for %s@%s\n", DEV_ID_LOG(device), hintStr, hint_exten, hint_context);

	SCCP_LIST_TRAVERSE(&sccp_hint_subscriptions, hint, list) {
		if (sccp_strlen(hint_exten) == sccp_strlen(hint->exten)
		    && sccp_strlen(hint_context) == sccp_strlen(hint->context)
		    && sccp_strequals(hint_exten, hint->exten)
		    && sccp_strequals(hint_context, hint->context)) {
			sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_4 "%s: hint for %s@%s found\n", DEV_ID_LOG(device), hint_exten, hint_context);
			break;
		}
	}

	if (!hint) {
		sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_4 "%s: creating hint for %s@%s\n", DEV_ID_LOG(device), hint_exten, hint_context);
		hint = sccp_hint_create(hint_exten, hint_context);
		if (!hint) {
			pbx_log(LOG_WARNING, "%s: speeddial hint %s@%s not monitored: the hint could not be created\n", DEV_ID_LOG(device), hint_exten, hint_context);
			return;
		}
		SCCP_LIST_LOCK(&sccp_hint_subscriptions);
		SCCP_LIST_INSERT_HEAD(&sccp_hint_subscriptions, hint, list);
		SCCP_LIST_UNLOCK(&sccp_hint_subscriptions);
	}

	sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_4 "%s: subscribing to hint %s@%s\n", DEV_ID_LOG(device), hint->exten, hint->context);
	sccp_hint_SubscribingDevice_t *subscriber = (sccp_hint_SubscribingDevice_t *)sccp_calloc(sizeof *subscriber, 1);
	if (!subscriber) {
		pbx_log(LOG_ERROR, "%s: speeddial hint %s@%s not monitored: out of memory\n", DEV_ID_LOG(device), hint->exten, hint->context);
		return;
	}

	subscriber->device = sccp_device_retain((sccp_device_t *) device);
	subscriber->instance = instance;
	subscriber->positionOnDevice = positionOnDevice;

	int i = 0;
	for (i = 0; i < StationMaxButtonTemplateSize; i++) {
		if (device->buttonTemplate[i].instance == instance) {
			subscriber->devicetype = device->buttonTemplate[i].devicetype;
		}
	}

	sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_4 "%s: subscription added for hint %s@%s\n", DEV_ID_LOG(device), hint->exten, hint->context);
	SCCP_LIST_INSERT_HEAD(&hint->subscribers, subscriber, list);

	sccp_dev_set_keyset(device, subscriber->instance, 0, KEYMODE_ONHOOK);

	sccp_hint_notifySubscribers(hint);
}

static sccp_hint_list_t *sccp_hint_create(char *hint_exten, char *hint_context)
{
	sccp_hint_list_t *hint = NULL;
	char hint_dialplan[256] = "";

	if (sccp_strlen_zero(hint_exten)) {
		return NULL;
	}
	if (sccp_strlen_zero(hint_context)) {
		hint_context = GLOB(context);
	}
	sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_4 "SCCP: creating hint for %s@%s\n", hint_exten, hint_context);

	int res = pbx_get_hint(hint_dialplan, sizeof(hint_dialplan) - 1, NULL, 0, NULL, hint_context, hint_exten);

	if (!res || sccp_strlen_zero(hint_dialplan)) {
		sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_4 "SCCP: no dialplan hint for %s@%s\n", hint_exten, hint_context);
		return NULL;
	}

	int i=0;
	while (hint_dialplan[i++]) {
		if (hint_dialplan[i] == ',') {
			hint_dialplan[i] = '\0';
			break;
		}
	}

	hint = (sccp_hint_list_t *)sccp_calloc(sizeof *hint, 1);
	if (!hint) {
		pbx_log(LOG_ERROR, "SCCP: hint %s@%s not created: out of memory\n", hint_exten, hint_context);
		return NULL;
	}
	if (!(hint->callInfo = iCallInfo.Constructor(0, "hint"))) {
		sccp_free(hint);
		return NULL;
	}
	hint->calltype = SKINNY_CALLTYPE_SENTINEL;

	SCCP_LIST_HEAD_INIT(&hint->subscribers);

	sccp_copy_string(hint->exten, hint_exten, sizeof(hint->exten));
	sccp_copy_string(hint->context, hint_context, sizeof(hint->context));
	sccp_copy_string(hint->hint_dialplan, hint_dialplan, sizeof(hint_dialplan));

	hint->stateid = pbx_extension_state_add(hint->context, hint->exten, sccp_hint_devstate_cb, hint);

	struct ast_state_cb_info info;
	info.exten_state = (enum ast_extension_states)pbx_extension_state(NULL, hint->context, hint->exten);
	sccp_hint_devstate_cb(hint->context, hint->exten, &info, hint);
	return hint;
}

static void sccp_hint_attachLine(sccp_line_t * line, sccp_device_t * device)
{
	struct sccp_hint_lineState *lineState = NULL;

	SCCP_LIST_LOCK(&lineStates);
	SCCP_LIST_TRAVERSE(&lineStates, lineState, list) {
		if (lineState->line == line) {
			break;
		}
	}
	if (!lineState) {
		sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_3 "%s: tracking line state of %s\n", DEV_ID_LOG(device), line->name);
		lineState = (struct sccp_hint_lineState *) sccp_calloc(sizeof *lineState, 1);
		if (!lineState) {
			pbx_log(LOG_ERROR, "%s: line %s state not tracked for hints: out of memory\n", DEV_ID_LOG(device), line->name);
			SCCP_LIST_UNLOCK(&lineStates);
			return;
		}
		SCCP_LIST_INSERT_HEAD(&lineStates, lineState, list);
	}

	if (!lineState->line) {		/* retain one instance of line in lineState->line */
		lineState->line = sccp_line_retain(line);
	}
	SCCP_LIST_UNLOCK(&lineStates);

	sccp_hint_lineStatusChanged(line, SCCP_CHANNELSTATE_ONHOOK);
}

static void sccp_hint_detachLine(sccp_line_t * line, sccp_device_t * device)
{
	AUTO_RELEASE(sccp_line_t, l, sccp_line_retain(line));
	if (l) {
		sccp_hint_lineStatusChanged(line, SCCP_CHANNELSTATE_ZOMBIE);
		struct sccp_hint_lineState *lineState = NULL;

		if (line->statistic.numberOfActiveDevices == 0) {		/* release last instance of lineState->line */
			SCCP_LIST_LOCK(&lineStates);
			SCCP_LIST_TRAVERSE_SAFE_BEGIN(&lineStates, lineState, list) {
				if (lineState->line == line) {
					if (lineState->line) {
						sccp_line_release(&lineState->line);
					}
					SCCP_LIST_REMOVE_CURRENT(list);
					sccp_free(lineState)
					break;
				}
			}
			SCCP_LIST_TRAVERSE_SAFE_END;
			SCCP_LIST_UNLOCK(&lineStates);
		}
	}
}

static void sccp_hint_lineStatusChanged(sccp_line_t * line, sccp_channelstate_t state)
{
	struct sccp_hint_lineState *lineState = NULL;

	SCCP_LIST_LOCK(&lineStates);
	SCCP_LIST_TRAVERSE(&lineStates, lineState, list) {
		if (lineState->line == line) {
			break;
		}
	}
	SCCP_LIST_UNLOCK(&lineStates);

	if (lineState && lineState->line) {
		sccp_hint_updateLineState(lineState, state);
	}
}

static void sccp_hint_updateLineState(struct sccp_hint_lineState * lineState, sccp_channelstate_t state)
{
	AUTO_RELEASE(sccp_line_t, line , sccp_line_retain(lineState->line));

	if (line) {
		sccp_log((DEBUGCAT_HINT))(VERBOSE_PREFIX_4 "%s: line state %s (%d) changed to %s (%d)\n", line->name, sccp_channelstate2str(lineState->state), lineState->state,
		                          sccp_channelstate2str(state), state);

		if (0 == SCCP_LIST_GETSIZE(&line->devices)) {
			lineState->state = SCCP_CHANNELSTATE_CONGESTION;
			lineState->callInfo.calltype = SKINNY_CALLTYPE_SENTINEL;

			sccp_copy_string(lineState->callInfo.partyName, SKINNY_DISP_TEMP_FAIL, sizeof(lineState->callInfo.partyName));
			sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_4 "SCCP: line %s is not registered on any device\n", line->name);
		} else if (SCCP_LIST_GETSIZE(&line->channels) > 1) {
			sccp_hint_updateLineStateForMultipleChannels(lineState, state);
		} else {
			sccp_hint_updateLineStateForSingleChannel(lineState, state);
		}

		sccp_hint_notifyLineStateUpdate(lineState);
	}
}

static void sccp_hint_updateLineStateForMultipleChannels(struct sccp_hint_lineState * lineState, sccp_channelstate_t state)
{
	if (!lineState || !lineState->line) {
		return;
	}
	AUTO_RELEASE(sccp_line_t, line, sccp_line_retain(lineState->line));

	memset(lineState->callInfo.partyName, 0, sizeof(lineState->callInfo.partyName));
	memset(lineState->callInfo.partyNumber, 0, sizeof(lineState->callInfo.partyNumber));
	lineState->callInfo.calltype = SKINNY_CALLTYPE_SENTINEL;
	lineState->state = SCCP_CHANNELSTATE_ONHOOK;

	if (SCCP_LIST_GETSIZE(&line->channels) > 0) {
		sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_4 "%s: %d active calls\n", line->name, SCCP_LIST_GETSIZE(&line->channels));
		if (SCCP_LIST_GETSIZE(&line->channels) == 1) {
			SCCP_LIST_LOCK(&line->channels);
			AUTO_RELEASE(sccp_channel_t, channel , SCCP_LIST_FIRST(&line->channels) ? sccp_channel_retain(SCCP_LIST_FIRST(&line->channels)) : NULL);
			SCCP_LIST_UNLOCK(&line->channels);

			if (channel) {
				lineState->callInfo.calltype = channel->calltype;
				if (channel->state != SCCP_CHANNELSTATE_ONHOOK && channel->state != SCCP_CHANNELSTATE_DOWN) {
					lineState->state = channel->state;
					sccp_callinfo_t *ci = sccp_channel_getCallInfo(channel);
					char cid_name[StationMaxNameSize] = {0};
					char cid_num[StationMaxDirnumSize] = {0};
					sccp_callerid_presentation_t presentation = CALLERID_PRESENTATION_ALLOWED;

					if (SKINNY_CALLTYPE_INBOUND == channel->calltype) {
						iCallInfo.Getter(ci,
							SCCP_CALLINFO_CALLINGPARTY_NAME, &cid_name,
							SCCP_CALLINFO_CALLINGPARTY_NUMBER, &cid_num,
							SCCP_CALLINFO_PRESENTATION, &presentation,
							SCCP_CALLINFO_KEY_SENTINEL);
					} else {
						iCallInfo.Getter(ci,
							SCCP_CALLINFO_CALLEDPARTY_NAME, &cid_name,
							SCCP_CALLINFO_CALLEDPARTY_NUMBER, &cid_num,
							SCCP_CALLINFO_PRESENTATION, &presentation,
							SCCP_CALLINFO_KEY_SENTINEL);
					}
					if (presentation == CALLERID_PRESENTATION_FORBIDDEN) {
						sccp_copy_string(lineState->callInfo.partyName, SKINNY_DISP_PRIVATE, sizeof(lineState->callInfo.partyName));
						sccp_copy_string(lineState->callInfo.partyNumber, SKINNY_DISP_PRIVATE, sizeof(lineState->callInfo.partyNumber));
					} else {
						sccp_copy_string(lineState->callInfo.partyName, cid_name, sizeof(lineState->callInfo.partyName));
						sccp_copy_string(lineState->callInfo.partyNumber, cid_num, sizeof(lineState->callInfo.partyNumber));
					}
				}
			}
		} else {
			sccp_copy_string(lineState->callInfo.partyName, SKINNY_DISP_IN_USE_REMOTE, sizeof(lineState->callInfo.partyName));
			sccp_copy_string(lineState->callInfo.partyNumber, SKINNY_DISP_IN_USE_REMOTE, sizeof(lineState->callInfo.partyNumber));
			lineState->state = state;
		}
	} else {
		sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_4 "%s: no active calls\n", line->name);
		lineState->state = SCCP_CHANNELSTATE_ONHOOK;
	}

	sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_4 "%s: shared line state %s (%d)\n", line->name, sccp_channelstate2str(lineState->state), lineState->state);
}

static void sccp_hint_updateLineStateForSingleChannel (struct sccp_hint_lineState * lineState, sccp_channelstate_t state)
{
	if (!lineState || !lineState->line) {
		return;
	}
	AUTO_RELEASE(sccp_line_t, line, sccp_line_retain(lineState->line));

	memset(lineState->callInfo.partyName, 0, sizeof(lineState->callInfo.partyName));
	memset(lineState->callInfo.partyNumber, 0, sizeof(lineState->callInfo.partyNumber));

	AUTO_RELEASE(sccp_channel_t, channel, NULL);
	if(SCCP_LIST_GETSIZE(&line->channels) > 0) {
		SCCP_LIST_LOCK(&line->channels);
		sccp_channel_t * tmp = SCCP_LIST_LAST(&line->channels);
		channel = sccp_channel_retain (tmp);
		SCCP_LIST_UNLOCK(&line->channels);
	}

	if (channel) {
		lineState->callInfo.calltype = channel->calltype;
		SCCP_LIST_LOCK(&line->devices);
		AUTO_RELEASE(sccp_linedevice_t, lineDevice, SCCP_LIST_FIRST(&line->devices) ? sccp_linedevice_retain(SCCP_LIST_FIRST(&line->devices)) : NULL);
		SCCP_LIST_UNLOCK(&line->devices);

		if(lineDevice) {
			AUTO_RELEASE(sccp_device_t, device, sccp_device_retain(lineDevice->device));

			if (device) {
				if (device->dndFeature.enabled && device->dndFeature.status == SCCP_DNDMODE_REJECT) {
					state = SCCP_CHANNELSTATE_DND;
				}
			}
		}
		switch (state) {
			case SCCP_CHANNELSTATE_DOWN:
				state = SCCP_CHANNELSTATE_ONHOOK;
				break;
			case SCCP_CHANNELSTATE_SPEEDDIAL:
				break;
			case SCCP_CHANNELSTATE_ONHOOK:
				break;
			case SCCP_CHANNELSTATE_DND:
				lineState->callInfo.calltype = SKINNY_CALLTYPE_INBOUND;
				sccp_copy_string(lineState->callInfo.partyName, "DND", sizeof(lineState->callInfo.partyName));
				sccp_copy_string(lineState->callInfo.partyNumber, "DND", sizeof(lineState->callInfo.partyNumber));
				break;
			case SCCP_CHANNELSTATE_CALLPARK:
				sccp_copy_string (lineState->callInfo.partyName, SKINNY_DISP_PARK, sizeof (lineState->callInfo.partyName));
				sccp_copy_string (lineState->callInfo.partyNumber, "", sizeof (lineState->callInfo.partyNumber));
				break;
			case SCCP_CHANNELSTATE_OFFHOOK:
			case SCCP_CHANNELSTATE_GETDIGITS:
			case SCCP_CHANNELSTATE_RINGOUT:
			case SCCP_CHANNELSTATE_RINGOUT_ALERTING:
			case SCCP_CHANNELSTATE_CONNECTED:
			case SCCP_CHANNELSTATE_PROGRESS:
			case SCCP_CHANNELSTATE_PROCEED:
			case SCCP_CHANNELSTATE_RINGING:
			case SCCP_CHANNELSTATE_DIALING:
			case SCCP_CHANNELSTATE_DIGITSFOLL:
			case SCCP_CHANNELSTATE_BUSY:
			case SCCP_CHANNELSTATE_HOLD:
			case SCCP_CHANNELSTATE_CONGESTION:
			case SCCP_CHANNELSTATE_CALLWAITING:
			case SCCP_CHANNELSTATE_CALLREMOTEMULTILINE:
			case SCCP_CHANNELSTATE_INVALIDNUMBER:
			case SCCP_CHANNELSTATE_CALLCONFERENCE:
			case SCCP_CHANNELSTATE_CALLTRANSFER:
			{
				sccp_callinfo_t *ci = sccp_channel_getCallInfo(channel);
				char cid_name[StationMaxNameSize] = {0};
				char cid_num[StationMaxDirnumSize] = {0};
				sccp_callerid_presentation_t presentation = CALLERID_PRESENTATION_ALLOWED;

				switch (channel->calltype) {
					case SKINNY_CALLTYPE_INBOUND:
						iCallInfo.Getter(ci,
							SCCP_CALLINFO_CALLINGPARTY_NAME, &cid_name,
							SCCP_CALLINFO_CALLINGPARTY_NUMBER, &cid_num,
							SCCP_CALLINFO_PRESENTATION, &presentation,
							SCCP_CALLINFO_KEY_SENTINEL);
						sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_4 "%s: speeddial party '%s <%s>' (calling party)\n", line->name, cid_name, cid_num);
						break;
					case SKINNY_CALLTYPE_OUTBOUND:
						iCallInfo.Getter(ci,
							SCCP_CALLINFO_CALLEDPARTY_NAME, &cid_name,
							SCCP_CALLINFO_CALLEDPARTY_NUMBER, &cid_num,
							SCCP_CALLINFO_PRESENTATION, &presentation,
							SCCP_CALLINFO_KEY_SENTINEL);
						sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_4 "%s: speeddial party '%s <%s>' (called party)\n", line->name, cid_name, cid_num);
						break;
					case SKINNY_CALLTYPE_FORWARD:
						sccp_copy_string(cid_name, "cfwd", sizeof(cid_name));
						sccp_copy_string(cid_num, "cfwd", sizeof(cid_num));
						sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_4 "%s: speeddial party: call forward\n", line->name);
						break;
					case SKINNY_CALLTYPE_SENTINEL:
						break;
				}
				if (presentation == CALLERID_PRESENTATION_FORBIDDEN) {
					sccp_copy_string(lineState->callInfo.partyName, SKINNY_DISP_PRIVATE, sizeof(lineState->callInfo.partyName));
					sccp_copy_string(lineState->callInfo.partyNumber, SKINNY_DISP_PRIVATE, sizeof(lineState->callInfo.partyNumber));
				} else {
					sccp_copy_string(lineState->callInfo.partyName, cid_name, sizeof(lineState->callInfo.partyName));
					sccp_copy_string(lineState->callInfo.partyNumber, cid_num, sizeof(lineState->callInfo.partyNumber));
				}
			}
				break;
			case SCCP_CHANNELSTATE_BLINDTRANSFER:
			case SCCP_CHANNELSTATE_INVALIDCONFERENCE:
			case SCCP_CHANNELSTATE_ZOMBIE:
			case SCCP_CHANNELSTATE_CONNECTEDCONFERENCE:
			case SCCP_CHANNELSTATE_SENTINEL:
				break;
		}

		sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_4 "%s: party name %s, number %s\n", line->name, lineState->callInfo.partyName, lineState->callInfo.partyNumber);
		lineState->state = state;
	} else {
		sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_4 "%s: no call\n", line->name);
		lineState->state = SCCP_CHANNELSTATE_ONHOOK;
		lineState->callInfo.calltype = SKINNY_CALLTYPE_SENTINEL;
		sccp_hint_checkForDND(lineState, line);
	}

	sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_4 "%s: single line state %s (%d)\n", line->name, sccp_channelstate2str(lineState->state), lineState->state);
}

static void sccp_hint_handleFeatureChangeEvent(const sccp_event_t * event)
{
	sccp_buttonconfig_t *buttonconfig = NULL;

	switch (event->featureChanged.featureType) {
		case SCCP_FEATURE_DND:
			{
				AUTO_RELEASE(sccp_device_t, d , sccp_device_retain(event->featureChanged.device));

				if (d) {
					SCCP_LIST_LOCK(&d->buttonconfig);
					SCCP_LIST_TRAVERSE(&d->buttonconfig, buttonconfig, list) {
						if (buttonconfig->type == LINE) {
							AUTO_RELEASE(sccp_line_t, line , sccp_line_find_byname(buttonconfig->button.line.name, FALSE));

							if (line) {
								sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: DND %s reported to Asterisk for line %s\n", d->id, d->dndFeature.status ? "on" : "off", line->name);
								sccp_hint_lineStatusChanged(line, SCCP_CHANNELSTATE_DND);
							}
						}
					}
					SCCP_LIST_UNLOCK(&d->buttonconfig);
				}
				break;
			}
		default:
			break;
	}
}

static enum ast_device_state sccp_hint_hint2DeviceState(sccp_channelstate_t state)
{
	enum ast_device_state newDeviceState = AST_DEVICE_UNKNOWN;

	switch (state) {
		case SCCP_CHANNELSTATE_DOWN:
		case SCCP_CHANNELSTATE_ONHOOK:
			newDeviceState = AST_DEVICE_NOT_INUSE;
			break;
		case SCCP_CHANNELSTATE_RINGOUT:
		case SCCP_CHANNELSTATE_RINGOUT_ALERTING:
		case SCCP_CHANNELSTATE_CALLWAITING:
			newDeviceState = AST_DEVICE_RINGINUSE;
			break;
		case SCCP_CHANNELSTATE_RINGING:
			newDeviceState = AST_DEVICE_RINGING;
			break;
		case SCCP_CHANNELSTATE_HOLD:
			newDeviceState = AST_DEVICE_ONHOLD;
			break;
		case SCCP_CHANNELSTATE_DND:
		case SCCP_CHANNELSTATE_BUSY:
			newDeviceState = AST_DEVICE_BUSY;
			break;
		case SCCP_CHANNELSTATE_ZOMBIE:
		case SCCP_CHANNELSTATE_CONGESTION:
			newDeviceState = AST_DEVICE_UNAVAILABLE;
			break;
		case SCCP_CHANNELSTATE_PROCEED:
		case SCCP_CHANNELSTATE_PROGRESS:
		case SCCP_CHANNELSTATE_GETDIGITS:
		case SCCP_CHANNELSTATE_DIALING:
		case SCCP_CHANNELSTATE_DIGITSFOLL:
		case SCCP_CHANNELSTATE_INVALIDNUMBER:
		case SCCP_CHANNELSTATE_CONNECTEDCONFERENCE:
		case SCCP_CHANNELSTATE_OFFHOOK:
		case SCCP_CHANNELSTATE_CONNECTED:
		case SCCP_CHANNELSTATE_BLINDTRANSFER:
		case SCCP_CHANNELSTATE_CALLTRANSFER:
		case SCCP_CHANNELSTATE_CALLCONFERENCE:
		case SCCP_CHANNELSTATE_CALLPARK:
		case SCCP_CHANNELSTATE_CALLREMOTEMULTILINE:
			newDeviceState = AST_DEVICE_INUSE;
			break;
		case SCCP_CHANNELSTATE_SENTINEL:
		case SCCP_CHANNELSTATE_SPEEDDIAL:
		case SCCP_CHANNELSTATE_INVALIDCONFERENCE:
			break;
	}
	return newDeviceState;
}

static void sccp_hint_notifySubscribers(sccp_hint_list_t * hint)
{
	sccp_hint_SubscribingDevice_t *subscriber = NULL;

	if (!hint) {
		pbx_log(LOG_ERROR, "SCCP: hint notification without a hint (caller bug)\n");
		return;
	}

	if (!GLOB(module_running) || SCCP_REF_RUNNING != sccp_refcount_isRunning()) {
		sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_3 "%s: hint not processed: shutting down\n", hint->exten);
		return;
	}

	sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_3 "%s: notifying %u subscribers of %s state %s\n", hint->exten, SCCP_LIST_GETSIZE(&hint->subscribers), hint->hint_dialplan, sccp_channelstate2str(hint->currentState));

	SCCP_LIST_LOCK(&hint->subscribers);
	SCCP_LIST_TRAVERSE(&hint->subscribers, subscriber, list) {
		AUTO_RELEASE(sccp_device_t, d , sccp_device_retain((sccp_device_t *) subscriber->device));

		if (d) {
			sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_4 "%s: notifying %s of %s state %s (%d), device type %s\n", DEV_ID_LOG(d), d->id, hint->hint_dialplan, sccp_channelstate2str(hint->currentState), hint->currentState, skinny_devicetype2str(subscriber->devicetype));
#ifdef CS_DYNAMIC_SPEEDDIAL
			sccp_msg_t *msg = NULL;
			sccp_speed_t k;
			char displayMessage[StationMaxNameSize * 3] = "";	/* cut to the phone field when sent */
			skinny_busylampfield_state_t status = SKINNY_BLF_STATUS_UNKNOWN;
			if (d->inuseprotocolversion >= 15) {
				sccp_dev_speed_find_byindex( d, subscriber->instance, TRUE, &k);
				char cidName[StationMaxNameSize] = "";
				char cidNumber[StationMaxDirnumSize] = "";

				switch (hint->currentState) {
				case SCCP_CHANNELSTATE_DOWN:
					snprintf(displayMessage, sizeof(displayMessage), "%s", k.name);
					status = SKINNY_BLF_STATUS_UNKNOWN;
					break;

				case SCCP_CHANNELSTATE_ONHOOK:
					snprintf(displayMessage, sizeof(displayMessage), "%s", k.name);
					status = SKINNY_BLF_STATUS_IDLE;
					break;

				case SCCP_CHANNELSTATE_DND:
					snprintf(displayMessage, sizeof(displayMessage), "(DND) %s", k.name);
					status = SKINNY_BLF_STATUS_DND;
					break;

				case SCCP_CHANNELSTATE_CONGESTION:
					snprintf(displayMessage, sizeof(displayMessage), "%s", k.name);
					status = SKINNY_BLF_STATUS_UNKNOWN;
					break;

				case SCCP_CHANNELSTATE_RINGING:
					status = SKINNY_BLF_STATUS_ALERTING;
										/* fall through */

				default:
					if (sccp_hint_isCIDavailabe(d, subscriber->positionOnDevice) == TRUE) {
						if (hint->calltype == SKINNY_CALLTYPE_INBOUND) {
							iCallInfo.Getter(hint->callInfo,
								SCCP_CALLINFO_CALLINGPARTY_NAME, &cidName,
								SCCP_CALLINFO_CALLINGPARTY_NUMBER, &cidNumber,
								SCCP_CALLINFO_KEY_SENTINEL);
						} else {
							iCallInfo.Getter(hint->callInfo,
								SCCP_CALLINFO_CALLEDPARTY_NAME, &cidName,
								SCCP_CALLINFO_CALLEDPARTY_NUMBER, &cidNumber,
								SCCP_CALLINFO_KEY_SENTINEL);
						}
						if (strlen(cidName) > 0) {
							snprintf(displayMessage, sizeof(displayMessage), "%s %s %s", cidName, (SCCP_CHANNELSTATE_CONNECTED == hint->currentState) ? "<=>" : ((hint->calltype == SKINNY_CALLTYPE_OUTBOUND) ? "<-" : "->"), k.name);
						} else if (strlen(cidNumber) > 0) {
							snprintf(displayMessage, sizeof(displayMessage), "%s %s %s", cidNumber, (SCCP_CHANNELSTATE_CONNECTED == hint->currentState) ? "<=>" : ((hint->calltype == SKINNY_CALLTYPE_OUTBOUND) ? "<-" : "->"), k.name);
						} else {
							snprintf(displayMessage, sizeof(displayMessage), "%s", k.name);
						}
					} else
					{
						snprintf(displayMessage, sizeof(displayMessage), "%s", k.name);
					}
					if (status == SKINNY_BLF_STATUS_UNKNOWN) {
						status = SKINNY_BLF_STATUS_INUSE;
					}
					break;
				}

				sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_4 "%s: notifying %s instance %d, display %s, state %s changed to %s\n", hint->exten, DEV_ID_LOG(d), subscriber->instance, displayMessage, sccp_channelstate2str(hint->currentState), skinny_busylampfield_state2str(status));

				/*
				 * Older 7914 expansion units attached to newer phones have problems displaying updated TextLabels
				 * Resetting changed content back to the original
				 */
				if (subscriber->devicetype == SKINNY_DEVICETYPE_CISCO_ADDON_7914) {
					snprintf(displayMessage, sizeof(displayMessage), "%s", k.name);
				}
				/* hack to fix the white text without shadow issue -MC */
				REQ(msg, FeatureStatDynamicMessage);
				if (!msg) {
					return;
				}
				sccp_copy_string(msg->data.FeatureStatDynamicMessage.textLabel, displayMessage, sizeof(msg->data.FeatureStatDynamicMessage.textLabel));
				msg->data.FeatureStatDynamicMessage.textLabel[strlen(displayMessage) - 1] = '\0';
				msg->data.FeatureStatDynamicMessage.lel_lineInstance                      = htolel(subscriber->instance);
				msg->data.FeatureStatDynamicMessage.lel_buttonType                        = htolel(SKINNY_BUTTONTYPE_BLFSPEEDDIAL);
				msg->data.FeatureStatDynamicMessage.stateVal.lel_uint32                   = htolel(status);
				sccp_dev_send(d, msg);

				REQ(msg, FeatureStatDynamicMessage);
				if (!msg) {
					return;
				}
				sccp_copy_string(msg->data.FeatureStatDynamicMessage.textLabel, displayMessage, sizeof(msg->data.FeatureStatDynamicMessage.textLabel));
				msg->data.FeatureStatDynamicMessage.lel_lineInstance    = htolel(subscriber->instance);
				msg->data.FeatureStatDynamicMessage.lel_buttonType      = htolel(SKINNY_BUTTONTYPE_BLFSPEEDDIAL);
				msg->data.FeatureStatDynamicMessage.stateVal.lel_uint32 = htolel(status);
				sccp_dev_send(d, msg);
			} else
#endif
			{
				sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_4 "%s: dynamic speeddial not possible; using state %s (%d)\n", DEV_ID_LOG(d), sccp_channelstate2str(hint->currentState), hint->currentState);

				/*
				   With the old hint style we should only use SCCP_CHANNELSTATE_ONHOOK and SCCP_CHANNELSTATE_CALLREMOTEMULTILINE as callstate,
				   otherwise we get a callplane on device -> set all states except onhook to SCCP_CHANNELSTATE_CALLREMOTEMULTILINE -MC
				 */
				skinny_callstate_t iconstate = SKINNY_CALLSTATE_CALLREMOTEMULTILINE;

				switch (hint->currentState) {
					case SCCP_CHANNELSTATE_DOWN:
					case SCCP_CHANNELSTATE_ONHOOK:
						iconstate = SKINNY_CALLSTATE_ONHOOK;
						break;
					case SCCP_CHANNELSTATE_RINGING:
						if (d->allowRinginNotification) {
							iconstate = SKINNY_CALLSTATE_RINGIN;
						}
						break;
					case SCCP_CHANNELSTATE_ZOMBIE:
					case SCCP_CHANNELSTATE_CONGESTION:
					case SCCP_CHANNELSTATE_CONNECTED:
					case SCCP_CHANNELSTATE_OFFHOOK:
					case SCCP_CHANNELSTATE_RINGOUT:
					case SCCP_CHANNELSTATE_RINGOUT_ALERTING:
					case SCCP_CHANNELSTATE_BUSY:
					case SCCP_CHANNELSTATE_HOLD:
					case SCCP_CHANNELSTATE_CALLWAITING:
					case SCCP_CHANNELSTATE_CALLPARK:
					case SCCP_CHANNELSTATE_PROCEED:
					case SCCP_CHANNELSTATE_CALLREMOTEMULTILINE:
					case SCCP_CHANNELSTATE_INVALIDNUMBER:
					case SCCP_CHANNELSTATE_DIALING:
					case SCCP_CHANNELSTATE_PROGRESS:
					case SCCP_CHANNELSTATE_GETDIGITS:
					case SCCP_CHANNELSTATE_SPEEDDIAL:
					case SCCP_CHANNELSTATE_DIGITSFOLL:
					case SCCP_CHANNELSTATE_INVALIDCONFERENCE:
					case SCCP_CHANNELSTATE_CONNECTEDCONFERENCE:
					case SCCP_CHANNELSTATE_BLINDTRANSFER:
					case SCCP_CHANNELSTATE_DND:
					case SCCP_CHANNELSTATE_CALLTRANSFER:
					case SCCP_CHANNELSTATE_CALLCONFERENCE:
						iconstate = SKINNY_CALLSTATE_CALLREMOTEMULTILINE;
						break;
					case SCCP_CHANNELSTATE_SENTINEL:
						break;
				}
				sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_4 "%s: icon state %s (%d)\n", DEV_ID_LOG(d), skinny_callstate2str(iconstate), iconstate);

				if (SCCP_CHANNELSTATE_RINGING == hint->previousState) {
					sccp_device_sendcallstate(d, subscriber->instance, 0, SKINNY_CALLSTATE_CONGESTION, SKINNY_CALLPRIORITY_NORMAL, SKINNY_CALLINFO_VISIBILITY_HIDDEN);
				}

				sccp_device_sendcallstate(d, subscriber->instance, 0, iconstate, SKINNY_CALLPRIORITY_NORMAL, SKINNY_CALLINFO_VISIBILITY_DEFAULT);

				if (hint->currentState == SCCP_CHANNELSTATE_ONHOOK || hint->currentState == SCCP_CHANNELSTATE_CONGESTION) {
					sccp_device_setLamp(d, SKINNY_STIMULUS_LINE, subscriber->instance, SKINNY_LAMP_OFF);
					sccp_dev_set_keyset(d, subscriber->instance, 0, KEYMODE_ONHOOK);
				} else if (hint->currentState == SCCP_CHANNELSTATE_RINGING && d->allowRinginNotification) {
					sccp_device_setLamp(d, SKINNY_STIMULUS_LINE, subscriber->instance, SKINNY_LAMP_BLINK);
					sccp_dev_set_keyset(d, subscriber->instance, 0, KEYMODE_INUSEHINT);
				} else {
					iCallInfo.Send(hint->callInfo, 0 , (hint->calltype == SKINNY_CALLTYPE_OUTBOUND) ? SKINNY_CALLTYPE_OUTBOUND : SKINNY_CALLTYPE_INBOUND, subscriber->instance, d, TRUE);
					sccp_device_setLamp(d, SKINNY_STIMULUS_LINE, subscriber->instance, SKINNY_LAMP_ON);
					sccp_dev_set_keyset(d, subscriber->instance, 0 , KEYMODE_INUSEHINT);
				}
			}
		} else {
			sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_4 "SCCP: subscriber device not found\n");
		}
	}
	SCCP_LIST_UNLOCK(&hint->subscribers);
}

static void sccp_hint_notifySubscribersViaPbx(struct sccp_hint_lineState *lineState, char *lineName, enum ast_device_state newDeviceState)
{
	sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_3 "SCCP: telling Asterisk: SCCP state %s (%d) = Asterisk %s (%d) on SCCP/%s\n", sccp_channelstate2str(lineState->state), lineState->state, pbxsccp_devicestate2str(newDeviceState), newDeviceState, lineState->line->name);
	pbx_devstate_changed_literal(newDeviceState, lineName);
}

/*
 * helper function to parse aggregated hint_dialplan to search for a match with lineName
 * We need to be able to parse a hint like this: exten => 112,hint, SIP/123&Meetme:444&SCCP/98011&SCCP/98031&Custom:DND112,CustomPresence:112,Meetme:444 and match on the lineName we are looking for, i.e.: SCCP/98011 and SCCP/98031
 */
static boolean_t sccp_match_dialplan2lineName(const char * hint_app, char * lineName)
{
	char *rest = pbx_strdupa(hint_app);
	char * cur = NULL;
	char * tmp = NULL;

	if ((tmp = strrchr(rest, ','))) {
                *tmp = '\0';
        }

        while ((cur = strsep(&rest, "&"))) {
        	if (sccp_strcaseequals(cur, lineName)) {
        		return TRUE;
        	}
        }
        return FALSE;
}

/* Notify Line Status Update either directly or via PBX(including distributed devstate) */
static void sccp_hint_notifyLineStateUpdate(struct sccp_hint_lineState *lineState)
{
	sccp_hint_list_t *hint = NULL;
	char lineName[StationMaxNameSize + 5];
	{
		AUTO_RELEASE(sccp_line_t, line , lineState->line ? sccp_line_retain(lineState->line) : NULL);
		if (line) {
			snprintf(lineName, sizeof(lineName), "SCCP/%s", line->name);
		} else {
			return;
		}
	}
	enum ast_device_state newDeviceState = sccp_hint_hint2DeviceState(lineState->state);
	enum ast_device_state oldDeviceState = AST_DEVICE_UNKNOWN;

 	SCCP_LIST_LOCK(&sccp_hint_subscriptions);
	SCCP_LIST_TRAVERSE(&sccp_hint_subscriptions, hint, list) {
		if (!sccp_strlen_zero(hint->hint_dialplan) && sccp_match_dialplan2lineName(hint->hint_dialplan, lineName)) {
			sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_4 "SCCP: line %s matches dialplan hint %s\n", lineName, hint->hint_dialplan);

			hint->calltype = lineState->callInfo.calltype;
			if (hint->calltype == SKINNY_CALLTYPE_INBOUND) {
				iCallInfo.Setter(hint->callInfo,
					SCCP_CALLINFO_CALLINGPARTY_NAME, lineState->callInfo.partyName,
					SCCP_CALLINFO_CALLINGPARTY_NUMBER, lineState->callInfo.partyNumber,
					SCCP_CALLINFO_KEY_SENTINEL);
			} else {
				iCallInfo.Setter(hint->callInfo,
					SCCP_CALLINFO_CALLEDPARTY_NAME, lineState->callInfo.partyName,
					SCCP_CALLINFO_CALLEDPARTY_NUMBER, lineState->callInfo.partyNumber,
					SCCP_CALLINFO_KEY_SENTINEL);
			}
			oldDeviceState = sccp_hint_hint2DeviceState(hint->currentState);

			sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_3 "SCCP: telling Asterisk: SCCP state %s (%d) on line %s\n", sccp_channelstate2str(lineState->state), lineState->state, lineName);
			sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_3 "SCCP: Asterisk state %s (%d) to %s (%d) on line %s\n", pbxsccp_devicestate2str(oldDeviceState), oldDeviceState, pbxsccp_devicestate2str(newDeviceState), newDeviceState, lineName);
			if (newDeviceState == oldDeviceState) {
				hint->previousState = hint->currentState;
				hint->currentState = lineState->state;
				sccp_hint_notifySubscribers(hint);							/* shortcut to inform sccp subscribers about cid update changes only */
			}
		}
	}
	SCCP_LIST_UNLOCK(&sccp_hint_subscriptions);

	sccp_hint_notifySubscribersViaPbx(lineState, lineName, newDeviceState);
	sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_3 "SCCP: told Asterisk: SCCP state %s (%d) = Asterisk %s (%d) on SCCP/%s\n", sccp_channelstate2str(lineState->state), lineState->state, pbxsccp_devicestate2str(newDeviceState), newDeviceState, lineState->line->name);
}

#ifdef CS_DYNAMIC_SPEEDDIAL
static gcc_inline boolean_t sccp_hint_isCIDavailabe(const sccp_device_t * device, const uint8_t positionOnDevice)
{
#ifdef CS_DYNAMIC_SPEEDDIAL_CID
	if (positionOnDevice <= 8 && (device->skinny_type == SKINNY_DEVICETYPE_CISCO7970 || device->skinny_type == SKINNY_DEVICETYPE_CISCO7971 || device->skinny_type == SKINNY_DEVICETYPE_CISCO7975 || device->skinny_type == SKINNY_DEVICETYPE_CISCO7985 || device->skinny_type == SKINNY_DEVICETYPE_CISCO_IP_COMMUNICATOR)) {
		return TRUE;
	}
	if (positionOnDevice <= 6 && (device->skinny_type == SKINNY_DEVICETYPE_CISCO7945 || device->skinny_type == SKINNY_DEVICETYPE_CISCO7961 || device->skinny_type == SKINNY_DEVICETYPE_CISCO7961GE || device->skinny_type == SKINNY_DEVICETYPE_CISCO7962 || device->skinny_type == SKINNY_DEVICETYPE_CISCO7965)) {
		return TRUE;
	}
	if (positionOnDevice <= 2 && (device->skinny_type == SKINNY_DEVICETYPE_CISCO7911 ||
				      device->skinny_type == SKINNY_DEVICETYPE_CISCO7912 ||
				      device->skinny_type == SKINNY_DEVICETYPE_CISCO7931 || device->skinny_type == SKINNY_DEVICETYPE_CISCO7935 || device->skinny_type == SKINNY_DEVICETYPE_CISCO7936 || device->skinny_type == SKINNY_DEVICETYPE_CISCO7937 || device->skinny_type == SKINNY_DEVICETYPE_CISCO7941 || device->skinny_type == SKINNY_DEVICETYPE_CISCO7941GE || device->skinny_type == SKINNY_DEVICETYPE_CISCO7942 || device->skinny_type == SKINNY_DEVICETYPE_CISCO7945)) {
		return TRUE;
	}
#endif

	return FALSE;
}
#endif

static void sccp_hint_checkForDND(struct sccp_hint_lineState * lineState, sccp_line_t * line)
{
	if (!lineState || !lineState->line || !line) {
		return;
	}
	do { /* we have to check if all devices on this line are dnd=SCCP_DNDMODE_REJECT, otherwise do not propagate DND status */
		boolean_t allDevicesInDND = TRUE;

		sccp_linedevice_t * lineDevice = NULL;
		SCCP_LIST_LOCK(&line->devices);
		SCCP_LIST_TRAVERSE(&line->devices, lineDevice, list) {
			if (lineDevice->device && lineDevice->device->dndFeature.status != SCCP_DNDMODE_REJECT) {
				allDevicesInDND = FALSE;
				break;
			}
		}
		SCCP_LIST_UNLOCK(&line->devices);

		if (allDevicesInDND) {
			lineState->callInfo.calltype = SKINNY_CALLTYPE_INBOUND;
			lineState->state = SCCP_CHANNELSTATE_DND;
		}
	} while(0);

	if (lineState->state == SCCP_CHANNELSTATE_DND) {
		sccp_copy_string(lineState->callInfo.partyName, "DND", sizeof(lineState->callInfo.partyName));
		sccp_copy_string(lineState->callInfo.partyNumber, "DND", sizeof(lineState->callInfo.partyNumber));
	}
}

sccp_channelstate_t sccp_hint_getLinestate(const char *linename, const char *deviceId)
{
	struct sccp_hint_lineState *lineState = NULL;
	sccp_channelstate_t state = SCCP_CHANNELSTATE_CONGESTION;

	SCCP_LIST_LOCK(&lineStates);
	SCCP_LIST_TRAVERSE(&lineStates, lineState, list) {
		if (lineState->line && sccp_strcaseequals(lineState->line->name, linename)) {
			sccp_log(DEBUGCAT_HINT)(VERBOSE_PREFIX_3 "%s: line state %s, party %s/%s, call type %s\n", lineState->line->name, sccp_channelstate2str(lineState->state),
                	        lineState->callInfo.partyNumber,lineState->callInfo.partyName,
                	        (!SCCP_CHANNELSTATE_Idling(lineState->state) && lineState->callInfo.calltype) ? skinny_calltype2str(lineState->callInfo.calltype) : "INACTIVE");
                        state = lineState->state;
			break;
		}
	}
	SCCP_LIST_UNLOCK(&lineStates);
	return state;
}

#include <asterisk/cli.h>
int sccp_show_hint_lineStates(int fd, sccp_cli_totals_t *totals, struct mansession *s, const struct message *m, int argc, char *argv[])
{
	int local_line_total = 0;

#define CLI_AMI_TABLE_NAME HintLineStates
#define CLI_AMI_TABLE_TITLE "Hint line states"
#define CLI_AMI_TABLE_PER_ENTRY_NAME HintLineState
#define CLI_AMI_TABLE_LIST_ITER_HEAD &lineStates
#define CLI_AMI_TABLE_LIST_ITER_TYPE struct sccp_hint_lineState
#define CLI_AMI_TABLE_LIST_ITER_VAR lineState
#define CLI_AMI_TABLE_LIST_LOCK SCCP_LIST_LOCK
#define CLI_AMI_TABLE_LIST_ITERATOR SCCP_LIST_TRAVERSE
#define CLI_AMI_TABLE_LIST_UNLOCK SCCP_LIST_UNLOCK
#define CLI_AMI_TABLE_FIELDS 															\
		CLI_AMI_TABLE_FIELD_NAMED(LineName, "Line",		"-10.10",	s,	10,	lineState->line ? lineState->line->name : "")		\
		CLI_AMI_TABLE_FIELD(State,		"-22.22",	s,	22,	sccp_channelstate2str(lineState->state))		\
		CLI_AMI_TABLE_FIELD_NAMED(CallInfoNumber, "Party Number",	"-15.15",	s,	15,	lineState->callInfo.partyNumber)			\
		CLI_AMI_TABLE_FIELD_NAMED(CallInfoName, "Party Name",	"-30.30",	s,	30,	lineState->callInfo.partyName)				\
		CLI_AMI_TABLE_FIELD(Direction,		"-10.10",	s,	10,	(!SCCP_CHANNELSTATE_Idling(lineState->state) && lineState->callInfo.calltype) ? skinny_calltype2str(lineState->callInfo.calltype) : "INACTIVE")

#include "sccp_cli_table.h"

	if (s) {
		totals->lines = local_line_total;
		totals->tables = 1;
	}
	return RESULT_SUCCESS;
}

int sccp_show_hint_subscriptions(int fd, sccp_cli_totals_t *totals, struct mansession *s, const struct message *m, int argc, char *argv[])
{
	int local_line_total = 0;

#define CLI_AMI_TABLE_NAME HintSubscriptions
#define CLI_AMI_TABLE_TITLE "Hint subscriptions"
#define CLI_AMI_TABLE_PER_ENTRY_NAME HintSubscription
#define CLI_AMI_TABLE_LIST_ITER_HEAD &sccp_hint_subscriptions
#define CLI_AMI_TABLE_LIST_ITER_TYPE sccp_hint_list_t
#define CLI_AMI_TABLE_LIST_ITER_VAR subscription
#define CLI_AMI_TABLE_LIST_LOCK SCCP_LIST_LOCK
#define CLI_AMI_TABLE_LIST_ITERATOR SCCP_LIST_TRAVERSE
#define CLI_AMI_TABLE_LIST_UNLOCK SCCP_LIST_UNLOCK
#define CLI_AMI_TABLE_BEFORE_ITERATION														\
	{																	\
		char cidName[StationMaxNameSize];												\
		char cidNumber[StationMaxDirnumSize];												\
		if (subscription->calltype == SKINNY_CALLTYPE_INBOUND) {									\
			iCallInfo.Getter(subscription->callInfo, 										\
				SCCP_CALLINFO_CALLINGPARTY_NAME, &cidName, 									\
				SCCP_CALLINFO_CALLINGPARTY_NUMBER, &cidNumber, 									\
				SCCP_CALLINFO_KEY_SENTINEL);											\
		} else {															\
			iCallInfo.Getter(subscription->callInfo, 											\
				SCCP_CALLINFO_CALLEDPARTY_NAME, &cidName, 									\
				SCCP_CALLINFO_CALLEDPARTY_NUMBER, &cidNumber, 									\
				SCCP_CALLINFO_KEY_SENTINEL);											\
		}
#define CLI_AMI_TABLE_AFTER_ITERATION 														\
	}
#define CLI_AMI_TABLE_FIELDS 															\
		CLI_AMI_TABLE_FIELD_NAMED(Exten, "Extension",		"-10.10",	s,	10,	subscription->exten)					\
		CLI_AMI_TABLE_FIELD(Context,		"-10.10",	s,	10,	subscription->context)					\
		CLI_AMI_TABLE_FIELD(Hint,		"-15.15",	s,	15,	subscription->hint_dialplan)				\
		CLI_AMI_TABLE_FIELD(State,		"-22.22",	s,	22,	sccp_channelstate2str(subscription->currentState))	\
		CLI_AMI_TABLE_FIELD_NAMED(CallInfoNumber, "Party Number",	"-15.15",	s,	15,	cidNumber)			\
		CLI_AMI_TABLE_FIELD_NAMED(CallInfoName, "Party Name",	"-30.30",	s,	30,	cidName)			\
		CLI_AMI_TABLE_FIELD(Direction,		"-10.10",	s,	10,	(subscription->calltype && subscription->calltype != SKINNY_CALLTYPE_SENTINEL) ? skinny_calltype2str(subscription->calltype) : "") \
		CLI_AMI_TABLE_FIELD_NAMED(Subs, "Subscribers",		"-4",		d,	4,	SCCP_LIST_GETSIZE(&subscription->subscribers))

#include "sccp_cli_table.h"

	if (s) {
		totals->lines = local_line_total;
		totals->tables = 1;
	}
	return RESULT_SUCCESS;
}
