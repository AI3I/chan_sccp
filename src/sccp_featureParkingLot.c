/*!
 * \file	sccp_featureParkingLot.c
 * \brief	SCCP ParkingLot Class
 * \author	Diederik de Groot <ddegroot [at] users.sf.net>
 * \date	2015-Sept-16
 * \note	This program is free software and may be modified and distributed under the terms of the GNU Public License.
 *		See the LICENSE file at the top of the source tree.
 */
#include "config.h"
#include "common.h"

SCCP_FILE_VERSION(__FILE__, "");

#include "sccp_featureParkingLot.h"

#ifdef CS_SCCP_PARK

#include "sccp_utils.h"
#include "sccp_vector.h"
#include "sccp_device.h"
#include "sccp_line.h"
#	include "sccp_linedevice.h"
#	include "sccp_channel.h"
#	include "sccp_feature.h"
#	include "sccp_labels.h"

static const uint32_t appID = APPID_VISUALPARKINGLOT;

#ifdef HAVE_PBX_APP_H
#  include <asterisk/app.h>
#endif

struct parkinglot;
typedef struct parkinglot sccp_parkinglot_t;
static void notifyLocked(sccp_parkinglot_t *pl);

typedef struct plslot plslot_t;
typedef struct plobserver plobserver_t;

struct plslot {
	int slot;
	const char *exten;
	const char *from;
	const char *channel;
	const char *callerid_num;
	const char *callerid_name;
	const char *connectedline_num;
	const char *connectedline_name;
};

struct plobserver {
	sccp_device_t * device;
	uint8_t instance;
	uint8_t transactionId;
};

struct parkinglot {
	pbx_mutex_t lock;
	char *context;
	SCCP_VECTOR(, plobserver_t) observers;
	SCCP_VECTOR(, plslot_t) slots;
	SCCP_RWLIST_ENTRY(sccp_parkinglot_t) list;
};

#define ICONSTATE_NEW_ON 0x020303
#define ICONSTATE_NEW_OFF 0x010000
#define ICONSTATE_OLD_ON 1
#define ICONSTATE_OLD_OFF 0

#define sccp_parkinglot_lock(x)		({pbx_mutex_lock(&((sccp_parkinglot_t * const)(x))->lock);})
#define sccp_parkinglot_unlock(x)	({pbx_mutex_unlock(&((sccp_parkinglot_t * const)(x))->lock);})

SCCP_RWLIST_HEAD(sccp_parkinglot_vector, sccp_parkinglot_t) parkinglots;
#define OBSERVER_CB_CMP(elem, value) ((elem).device == (value).device && (elem).instance == (value).instance)
#define SLOT_CB_CMP(elem, value) ((elem).slot == (value))

#define SLOT_CLEANUP(elem) 							\
	if ((elem).exten) {sccp_free((elem).exten);}				\
	if ((elem).from) {sccp_free((elem).from);}				\
	if ((elem).channel) {sccp_free((elem).channel);}			\
	if ((elem).callerid_num) {sccp_free((elem).callerid_num);}		\
	if ((elem).callerid_name) {sccp_free((elem).callerid_name);}		\
	if ((elem).connectedline_num) {sccp_free((elem).connectedline_num);}	\
	if ((elem).connectedline_name) {sccp_free((elem).connectedline_name);}

static sccp_parkinglot_t * addParkinglot(const char *parkinglot)
{
	pbx_assert(parkinglot != NULL);

	sccp_log(DEBUGCAT_PARKINGLOT)(VERBOSE_PREFIX_1 "SCCP: adding parking lot %s\n", parkinglot);
	sccp_parkinglot_t *pl = (sccp_parkinglot_t *) sccp_calloc(sizeof(sccp_parkinglot_t), 1);

	pl->context = pbx_strdup(parkinglot);
	pbx_mutex_init(&pl->lock);
	SCCP_VECTOR_INIT(&pl->observers,1);
	SCCP_VECTOR_INIT(&pl->slots,1);

	SCCP_RWLIST_WRLOCK(&parkinglots);
	SCCP_RWLIST_INSERT_HEAD(&parkinglots, pl, list);
	SCCP_RWLIST_UNLOCK(&parkinglots);
	return pl;
}

static int removeParkinglot(sccp_parkinglot_t *pl)
{
	pbx_assert(pl != NULL && pl != NULL);

	int res = FALSE;
	sccp_parkinglot_t *removed = NULL;
	sccp_log(DEBUGCAT_PARKINGLOT)(VERBOSE_PREFIX_1 "SCCP: removing parking lot %s\n", pl->context);

	SCCP_RWLIST_WRLOCK(&parkinglots);
	removed = SCCP_RWLIST_REMOVE(&parkinglots, pl, list);
	SCCP_RWLIST_UNLOCK(&parkinglots);
	sccp_parkinglot_unlock(pl);

	if (removed) {
		if (removed->context) {
			sccp_free(removed->context);
		}
		SCCP_VECTOR_RESET(&removed->observers, SCCP_VECTOR_ELEM_CLEANUP_NOOP);
		SCCP_VECTOR_FREE(&removed->observers);
		SCCP_VECTOR_RESET(&removed->slots, SLOT_CLEANUP);
		SCCP_VECTOR_FREE(&removed->slots);
		pbx_mutex_destroy(&removed->lock);
		sccp_free(removed);
		res = TRUE;
	}
	sccp_log(DEBUGCAT_PARKINGLOT)(VERBOSE_PREFIX_1 "SCCP: parking lot removed\n");
	return res;
}

/* returns locked pl */
static sccp_parkinglot_t * const findParkinglotByContext(const char *parkinglot)
{
	pbx_assert(parkinglot != NULL);

	sccp_parkinglot_t *pl = NULL;
	SCCP_RWLIST_RDLOCK(&parkinglots);
	SCCP_RWLIST_TRAVERSE(&parkinglots, pl, list) {
		sccp_parkinglot_lock(pl);
		if (sccp_strcaseequals(pl->context, parkinglot)) {
			// returning parkinglot locked
			break;
		}
		sccp_parkinglot_unlock(pl);
	}
	SCCP_RWLIST_UNLOCK(&parkinglots);
	return pl;
}

/* returns locked pl */
static sccp_parkinglot_t * const findCreateParkinglot(const char *parkinglot, boolean_t create)
{
	pbx_assert(parkinglot != NULL);

	sccp_parkinglot_t *pl = findParkinglotByContext(parkinglot);
	if (!pl && create) {
		if (!(pl = addParkinglot(parkinglot))) {
			return NULL;
		}
		sccp_parkinglot_lock(pl);
	}
	sccp_log(DEBUGCAT_PARKINGLOT)(VERBOSE_PREFIX_1 "SCCP: parking lot found: %s\n", pl ? "yes" : "no");
	return pl;
}

static int attachObserver(sccp_device_t * device, const sccp_buttonconfig_t * const buttonConfig)
{
	pbx_assert(device != NULL && buttonConfig != NULL);
	int res = FALSE;

	if(!sccp_strlen_zero(buttonConfig->button.feature.options)) {
		sccp_log(DEBUGCAT_PARKINGLOT)(VERBOSE_PREFIX_1 "%s: %s watches this parking lot on button instance %d\n", buttonConfig->button.feature.options, device->id, buttonConfig->instance);
		RAII(sccp_parkinglot_t *, pl, findCreateParkinglot(buttonConfig->button.feature.options, TRUE), sccp_parkinglot_unlock);
		if (pl) {
			plobserver_t observer = {
				.device = device,
				.instance = buttonConfig->instance,
				.transactionId = 0,
			};

			if (SCCP_VECTOR_APPEND(&pl->observers, observer) == 0) {
				res = TRUE;
			}
		}
	}
	return res;
}

static int detachObserver(sccp_device_t * device, const sccp_buttonconfig_t * const buttonConfig)
{
	pbx_assert(device != NULL && buttonConfig != NULL);
	int res = FALSE;

	if(!sccp_strlen_zero(buttonConfig->button.feature.options)) {
		sccp_log(DEBUGCAT_PARKINGLOT)(VERBOSE_PREFIX_1 "%s: %s stopped watching this parking lot (instance %d)\n", buttonConfig->button.feature.options, device->id, buttonConfig->instance);
		sccp_parkinglot_t * pl = findCreateParkinglot(buttonConfig->button.feature.options, FALSE);
		if (pl) {
			plobserver_t cmp = {
				.device = device,
				.instance = buttonConfig->instance,
			};
			if (SCCP_VECTOR_REMOVE_CMP_UNORDERED(&pl->observers, cmp, OBSERVER_CB_CMP, SCCP_VECTOR_ELEM_CLEANUP_NOOP) == 0) {
				res = TRUE;
			}
			if (SCCP_VECTOR_SIZE(&pl->observers) == 0) {
				removeParkinglot(pl);	// will destroy pl and unlock pl in the process
			} else {
				sccp_parkinglot_unlock(pl);
			}
		}
	}
	return res;
}

static char * const getParkingLotCXML(sccp_parkinglot_t *pl, int protocolversion, uint8_t instance, uint32_t transactionId, char **const outbuf)
{
	pbx_assert(pl != NULL && outbuf != NULL);

	sccp_log(DEBUGCAT_PARKINGLOT)(VERBOSE_PREFIX_1 "%s: building parking lot display (protocol %d)\n", pl->context, protocolversion);
	*outbuf = NULL;
	if (SCCP_VECTOR_SIZE(&pl->slots)) {
		pbx_str_t *buf = ast_str_create(DEFAULT_PBX_STR_BUFFERSIZE);
		pbx_str_append(&buf, 0, "<?xml version=\"1.0\"?>");
		if (protocolversion < 15) {
			pbx_str_append(&buf, 0, "<CiscoIPPhoneMenu>");
		} else {
			pbx_str_append(&buf, 0, "<CiscoIPPhoneMenu appId='%d' onAppClosed='%d'>", appID, appID);
		}
		pbx_str_append(&buf, 0, "<Title>Parked Calls</Title>");
		pbx_str_append(&buf, 0, "<Prompt>Choose a parked call</Prompt>");
		for (size_t idx = 0; idx < SCCP_VECTOR_SIZE(&pl->slots); idx++) {
			plslot_t *slot = SCCP_VECTOR_GET_ADDR(&pl->slots, idx);
			pbx_str_append(&buf, 0, "<MenuItem>");
			const char *connected_line = !sccp_strcaseequals(slot->connectedline_name, "<unknown>") ? slot->connectedline_name : slot->from;
			char esc_name[SCCP_MAX_EXTENSION * 6];
			char esc_num[SCCP_MAX_EXTENSION * 6];
			char esc_by[SCCP_MAX_EXTENSION * 6];
			sccp_xml_escape(slot->callerid_num, esc_num, sizeof(esc_num));
			sccp_xml_escape(connected_line, esc_by, sizeof(esc_by));
			if (!sccp_strcaseequals(slot->callerid_name, "<unknown>")) {
				pbx_str_append(&buf, 0, "<Name>%s (%s) by %s</Name>", sccp_xml_escape(slot->callerid_name, esc_name, sizeof(esc_name)), esc_num, esc_by);
			} else {
				pbx_str_append(&buf, 0, "<Name>%s by %s</Name>", esc_num, esc_by);
			}
			pbx_str_append(&buf, 0, "<URL>UserCallData:%d:%d:%d:%d:%s/%s</URL>", appID, instance, 0, transactionId, pl->context, slot->exten);
			pbx_str_append(&buf, 0, "</MenuItem>");
		}
		pbx_str_append(&buf, 0, "<SoftKeyItem>");
		pbx_str_append(&buf, 0, "<Name>Dial</Name>");
		pbx_str_append(&buf, 0, "<Position>1</Position>");
		pbx_str_append(&buf, 0, "<URL>UserDataSoftKey:Select:%d:DIAL/%d</URL>", appID, transactionId);
		pbx_str_append(&buf, 0, "</SoftKeyItem>\n");
		pbx_str_append(&buf, 0, "<SoftKeyItem>");
		pbx_str_append(&buf, 0, "<Name>Exit</Name>");
		pbx_str_append(&buf, 0, "<Position>3</Position>");
		pbx_str_append(&buf, 0, "<URL>UserDataSoftKey:Select:%d:EXIT/%d</URL>", appID, transactionId);
		pbx_str_append(&buf, 0, "</SoftKeyItem>\n");

		pbx_str_append(&buf, 0, "</CiscoIPPhoneMenu>");
		*outbuf = pbx_strdup(pbx_str_buffer(buf));
		sccp_log(DEBUGCAT_PARKINGLOT)(VERBOSE_PREFIX_1 "%s: parking lot display (protocol %d):\n[%s]\n", pl->context, protocolversion, *outbuf);
		sccp_free(buf);
	}
	return *outbuf;
}

static void __showVisualParkingLot(sccp_parkinglot_t *pl, constDevicePtr d, plobserver_t * observer)
{
	pbx_assert(pl != NULL && d != NULL && observer != NULL);
	uint32_t transactionId = sccp_random();
	char * xmlStr = NULL;

	sccp_log(DEBUGCAT_PARKINGLOT)(VERBOSE_PREFIX_1 "%s: showing parking lot on %s, instance %d\n", pl->context, observer->device->id, observer->instance);
	if ((xmlStr = getParkingLotCXML(pl, d->protocolversion, observer->instance, transactionId, &xmlStr))) {
		sccp_parkinglot_unlock(pl);
		d->protocol->sendUserToDeviceDataVersionMessage(d, appID, 0, 0, transactionId, xmlStr, 0);
		sccp_free(xmlStr);
		sccp_parkinglot_lock(pl);
	} else {
		sccp_parkinglot_unlock(pl);
		sccp_dev_displayprinotify(d, SKINNY_DISP_CANNOT_RETRIEVE_PARKED_CALL, SCCP_MESSAGE_PRIORITY_TIMEOUT, 5);
		sccp_parkinglot_lock(pl);
	}
	observer->transactionId = transactionId;
}

static void __hideVisualParkingLot(sccp_parkinglot_t *pl, constDevicePtr d, plobserver_t *observer)
{
	pbx_assert(pl != NULL && observer != NULL);

	sccp_log(DEBUGCAT_PARKINGLOT)(VERBOSE_PREFIX_1 "%s: hiding parking lot on %s, instance %d\n", pl->context, d->id, observer->instance);
	uint32_t transactionId = observer->transactionId;
	sccp_parkinglot_unlock(pl);

	char xmlStr[DEFAULT_PBX_STR_BUFFERSIZE];
	if (d->protocolversion < 15) {
		snprintf(xmlStr, DEFAULT_PBX_STR_BUFFERSIZE, "<CiscoIPPhoneExecute><ExecuteItem Priority=\"0\" URL=\"Init:Services\"/></CiscoIPPhoneExecute>");
	} else {
		snprintf(xmlStr, DEFAULT_PBX_STR_BUFFERSIZE, "<CiscoIPPhoneExecute><ExecuteItem Priority=\"0\" URL=\"App:Close:%d\"/></CiscoIPPhoneExecute>", appID);
	}
	d->protocol->sendUserToDeviceDataVersionMessage(observer->device, appID, 0, 0, transactionId, xmlStr, 0);

	sccp_parkinglot_lock(pl);
	observer->transactionId = 0;
}

static void hideVisualParkingLot(const char *parkinglot, constDevicePtr d, uint8_t instance)
{
	pbx_assert(parkinglot != NULL &&  d != NULL);

	RAII(sccp_parkinglot_t *, pl, findCreateParkinglot(parkinglot, TRUE), sccp_parkinglot_unlock);
	if (pl) {
		sccp_log(DEBUGCAT_PARKINGLOT)(VERBOSE_PREFIX_1 "%s: hiding parking lot on %s, instance %d (%d watchers)\n", parkinglot, d->id, instance, (int)SCCP_VECTOR_SIZE(&pl->observers));
		for (size_t idx = 0; idx < SCCP_VECTOR_SIZE(&pl->observers); idx++) {
			plobserver_t *observer = SCCP_VECTOR_GET_ADDR(&pl->observers, idx);
			if (observer->device == d && observer->instance == instance) {
				__hideVisualParkingLot(pl, d, observer);
			}
		}
	}
}

static void _notifyHelper(plobserver_t *observer, sccp_parkinglot_t *pl, constDevicePtr device)
{
	uint32_t iconstate = 0;
	sccp_buttonconfig_t *config = NULL;
	size_t               numslots  = SCCP_VECTOR_SIZE(&pl->slots);
	if (device->protocolversion < 15) {
		sccp_device_setLamp(device, SKINNY_STIMULUS_PARKINGLOT, 0, numslots ? SKINNY_LAMP_ON : SKINNY_LAMP_OFF);
		iconstate = numslots ? ICONSTATE_OLD_ON : ICONSTATE_OLD_OFF;
	} else {
		iconstate = numslots ? ICONSTATE_NEW_ON : ICONSTATE_NEW_OFF;
	}

	SCCP_LIST_LOCK(&device->buttonconfig);
	SCCP_LIST_TRAVERSE(&device->buttonconfig, config, list) {
		if (config->type == FEATURE && config->instance == observer->instance) {
			config->button.feature.status = iconstate;
		}
	}
	SCCP_LIST_UNLOCK(&device->buttonconfig);

	if (observer->transactionId) {
		if (numslots > 0 && !device->active_channel) {
			__showVisualParkingLot(pl, device, observer);
		} else {
			__hideVisualParkingLot(pl, device, observer);
		}
	}
	sccp_feat_changed(device, NULL, SCCP_FEATURE_PARKINGLOT);
}

static void notifyDevice(constDevicePtr device, const sccp_buttonconfig_t * const buttonConfig)
{
	pbx_assert(device != NULL && buttonConfig != NULL);
	plobserver_t *observer = NULL;

	if(!sccp_strlen_zero(buttonConfig->button.feature.options)) {
		sccp_log(DEBUGCAT_PARKINGLOT)(VERBOSE_PREFIX_1 "%s: notifying %s\n", buttonConfig->button.feature.options, device->id);
		RAII(sccp_parkinglot_t *, pl, findCreateParkinglot(buttonConfig->button.feature.options, TRUE), sccp_parkinglot_unlock);
		if (pl) {
			for (size_t idx = 0; idx < SCCP_VECTOR_SIZE(&pl->observers); idx++) {
				observer = SCCP_VECTOR_GET_ADDR(&pl->observers, idx);
				if (observer && observer->device == device) {
					_notifyHelper(observer, pl, device);
				}
			}
		}
	}
}

static void notifyLocked(sccp_parkinglot_t *pl)
{
	pbx_assert(pl != NULL);

	sccp_log(DEBUGCAT_PARKINGLOT)(VERBOSE_PREFIX_1 "%s: notifying watchers\n", pl->context);
	plobserver_t *observer = NULL;

	for (size_t idx = 0; idx < SCCP_VECTOR_SIZE(&pl->observers); idx++) {
		observer = SCCP_VECTOR_GET_ADDR(&pl->observers, idx);
		if (observer) {
			AUTO_RELEASE(sccp_device_t, device , sccp_device_retain(observer->device));
			if (device) {
				_notifyHelper(observer, pl, device);
			}
		}
	}
}

static int addSlot(const char *parkinglot, int slot, struct message *m)
{
	pbx_assert(parkinglot != NULL && m != NULL);

	int res = FALSE;

	sccp_log(DEBUGCAT_PARKINGLOT)(VERBOSE_PREFIX_1 "%s: call parked in slot %d\n", parkinglot, slot);

	RAII(sccp_parkinglot_t *, pl, findCreateParkinglot(parkinglot, TRUE), sccp_parkinglot_unlock);
	if (pl) {
		if (SCCP_VECTOR_GET_CMP(&pl->slots, slot, SLOT_CB_CMP) == NULL) {
			plslot_t new_slot = {
				.slot = slot,
				.exten = pbx_strdup(astman_get_header(m, PARKING_SLOT)),
				.from = pbx_strdup(astman_get_header(m, PARKING_FROM)),
				.channel = pbx_strdup(astman_get_header(m, PARKING_PREFIX "Channel")),
				.callerid_num = pbx_strdup(astman_get_header(m, PARKING_PREFIX "CallerIDNum")),
				.callerid_name = pbx_strdup(astman_get_header(m, PARKING_PREFIX "CallerIDName")),
				.connectedline_num = pbx_strdup(astman_get_header(m, PARKING_PREFIX "ConnectedLineNum")),
				.connectedline_name = pbx_strdup(astman_get_header(m, PARKING_PREFIX "ConnectedLineName")),
			};
			if (SCCP_VECTOR_APPEND(&pl->slots, new_slot) == 0)  {
				notifyLocked(pl);
				res = TRUE;
			}
		} else {
			notifyLocked(pl);
		}
	} else {
		sccp_log(DEBUGCAT_PARKINGLOT)(VERBOSE_PREFIX_1 "SCCP: parking lot %s has no watchers\n", parkinglot);
	}
	return res;
}

static int removeSlot(const char *parkinglot, int slot)
{
	pbx_assert(parkinglot != NULL);

	sccp_log(DEBUGCAT_PARKINGLOT)(VERBOSE_PREFIX_1 "%s: slot %d freed\n", parkinglot, slot);
	int res = FALSE;

	RAII(sccp_parkinglot_t *, pl, findCreateParkinglot(parkinglot, TRUE), sccp_parkinglot_unlock);
	if (pl) {
		if (SCCP_VECTOR_REMOVE_CMP_UNORDERED(&pl->slots, slot, SLOT_CB_CMP, SLOT_CLEANUP) == 0) {
			notifyLocked(pl);
			res = TRUE;
		}
	} else {
		sccp_log(DEBUGCAT_PARKINGLOT)(VERBOSE_PREFIX_1 "SCCP: parking lot %s has no watchers\n", parkinglot);
	}
	return !res;
}

static void handleButtonPress(constDevicePtr d, const sccp_buttonconfig_t * const buttonConfig)
{
	pbx_assert(d != NULL && buttonConfig != NULL);
	sccp_log(DEBUGCAT_PARKINGLOT)(VERBOSE_PREFIX_1 "%s: parking lot button %s pressed (instance %d)\n", d->id, buttonConfig->button.feature.options, buttonConfig->instance);

	AUTO_RELEASE(sccp_channel_t, channel , sccp_device_getActiveChannel(d));
	if (channel && channel->state != SCCP_CHANNELSTATE_OFFHOOK && channel->state != SCCP_CHANNELSTATE_HOLD) {
		sccp_channel_park(channel);
	} else if(!sccp_strlen_zero(buttonConfig->button.feature.options)) {
		RAII(sccp_parkinglot_t *, pl, findCreateParkinglot(buttonConfig->button.feature.options, TRUE), sccp_parkinglot_unlock);
		if (pl) {
			if (SCCP_VECTOR_SIZE(&pl->slots) == 0) {
				sccp_log(DEBUGCAT_PARKINGLOT)(VERBOSE_PREFIX_1 "%s: no parked calls; showing a status message\n", buttonConfig->button.feature.options);
				sccp_dev_displayprinotify(d, SKINNY_DISP_CANNOT_RETRIEVE_PARKED_CALL, SCCP_MESSAGE_PRIORITY_TIMEOUT, 5);
			} else {
				if(sccp_strcaseequals(buttonConfig->button.feature.args, "RetrieveSingle") && SCCP_VECTOR_SIZE(&pl->slots) == 1) {
					sccp_log(DEBUGCAT_PARKINGLOT)(VERBOSE_PREFIX_1 "%s: one parked call; retrieving it\n", buttonConfig->button.feature.options);
					plslot_t *slot = SCCP_VECTOR_GET_ADDR(&pl->slots, 0);
					if (slot) {
						AUTO_RELEASE(sccp_line_t, line , channel ? sccp_line_retain(channel->line) : d->currentLine ? sccp_dev_getActiveLine(d) : sccp_line_find_byid(d, d->defaultLineInstance));
						AUTO_RELEASE(sccp_channel_t, new_channel,
							     sccp_channel_newcall(line, d, slot->exten, SKINNY_CALLTYPE_OUTBOUND, NULL, NULL));
					}
				} else {
					sccp_log(DEBUGCAT_PARKINGLOT)(VERBOSE_PREFIX_1 "%s: several parked calls; showing the parking lot\n", buttonConfig->button.feature.options);
					for (size_t idx = 0; idx < SCCP_VECTOR_SIZE(&pl->observers); idx++) {
						plobserver_t *observer = SCCP_VECTOR_GET_ADDR(&pl->observers, idx);
						if(observer->device == d && observer->instance == buttonConfig->instance) {
							__showVisualParkingLot(pl, d, observer);
						}
					}
				}
			}
		}
	}
}

static void handleDevice2User(const char *parkinglot, constDevicePtr d, const char *slot_exten, uint8_t instance, uint32_t transactionId)
{
	pbx_assert(d != NULL);
	sccp_log(DEBUGCAT_PARKINGLOT)(VERBOSE_PREFIX_1 "%s: parking lot selection: instance %d, transaction %d\n", d->id, instance, transactionId);

	if (d->dtu_softkey.action && d->dtu_softkey.transactionID == transactionId) {
		if (sccp_strequals(d->dtu_softkey.action, "DIAL")) {
			AUTO_RELEASE(sccp_line_t, line , d->currentLine ? sccp_dev_getActiveLine(d) : sccp_line_find_byid(d, d->defaultLineInstance));
			AUTO_RELEASE(sccp_channel_t, new_channel, sccp_channel_newcall(line, d, slot_exten, SKINNY_CALLTYPE_OUTBOUND, NULL, NULL));
		} else if (sccp_strequals(d->dtu_softkey.action, "EXIT")) {
			hideVisualParkingLot(parkinglot, d, instance);
		}
	}
}
const ParkingLotInterface iParkingLot = {
	.attachObserver = attachObserver,
	.detachObserver = detachObserver,
	.addSlot = addSlot,
	.removeSlot = removeSlot,
	.handleButtonPress = handleButtonPress,
	.handleDevice2User = handleDevice2User,
	.notifyDevice = notifyDevice,
};
#else
const ParkingLotInterface iParkingLot = { 0 };
#endif
