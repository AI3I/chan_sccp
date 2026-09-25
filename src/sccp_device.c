/*!
 * \file        sccp_device.c
 * \brief       SCCP Device Class
 * \author      Sergio Chersovani <mlists [at] c-net.it>
 * \note        Reworked, but based on chan_sccp code.
 *              The original chan_sccp driver that was made by Zozo which itself was derived from the chan_skinny driver.
 *              Modified by Jan Czmok and Julien Goodwin
 * \note        This program is free software and may be modified and distributed under the terms of the GNU Public License.
 *              See the LICENSE file at the top of the source tree.
 *
 * \remarks
 * Purpose:     SCCP Device
 * When to use: Only methods directly related to sccp devices should be stored in this source file.
 * Relations:   SCCP Device -> SCCP DeviceLine -> SCCP Line
 *              SCCP Line -> SCCP ButtonConfig -> SCCP Device
 */

#include "config.h"
#include "common.h"
#include "sccp_channel.h"
#include "sccp_actions.h"
#include "sccp_config.h"
#include "sccp_device.h"
#include "sccp_feature.h"
#include "sccp_line.h"
#include "sccp_linedevice.h"
#include "sccp_session.h"
#include "sccp_indicate.h"
#include "sccp_utils.h"
#include "sccp_atomic.h"
#include "sccp_featureParkingLot.h"
#include "sccp_labels.h"

SCCP_FILE_VERSION(__FILE__, "");

#ifdef HAVE_PBX_ACL_H
#  include <asterisk/acl.h>
#endif
#if defined(CS_AST_HAS_EVENT) && defined(HAVE_PBX_EVENT_H)
#  include <asterisk/event.h>
#endif
#if HAVE_ICONV
#	include <iconv.h>
int sccp_device_createiconv(devicePtr d);
void sccp_device_destroyiconv(devicePtr d);
#endif

int __sccp_device_destroy(const void *ptr);
void sccp_device_removeFromGlobals(devicePtr device);

struct sccp_private_device_data {
	sccp_mutex_t lock;

	sccp_accessorystate_t accessoryStatus[SCCP_ACCESSORY_SENTINEL + 1];
	sccp_devicestate_t deviceState;

	skinny_registrationstate_t registrationState;

#if HAVE_ICONV
	iconv_t iconv;
	sccp_mutex_t iconv_lock;
#endif
};

#define sccp_private_lock(x) sccp_mutex_lock(&((struct sccp_private_device_data * const)(x))->lock)
#define sccp_private_unlock(x) sccp_mutex_unlock(&((struct sccp_private_device_data * const)(x))->lock)

static void sccp_device_indicate_onhook(constDevicePtr device, const uint8_t lineInstance, uint32_t callid);
static void sccp_device_indicate_offhook(constDevicePtr device, sccp_linedevice_t * ld, uint32_t callid);
static void sccp_device_indicate_dialing(constDevicePtr device, const uint8_t lineInstance, const uint32_t callid, const skinny_calltype_t calltype, sccp_callinfo_t * const callinfo, char dialedNumber[SCCP_MAX_EXTENSION]);
static void sccp_device_indicate_proceed(constDevicePtr device, const uint8_t lineInstance, const uint32_t callid, const skinny_calltype_t calltype, sccp_callinfo_t * const callinfo);
static void sccp_device_indicate_connected(constDevicePtr device, const uint8_t lineInstance, const uint32_t callid, const skinny_calltype_t calltype, sccp_callinfo_t * const callinfo);
static void sccp_device_old_callhistory(constDevicePtr device, const uint8_t lineInstance, const uint32_t callid, const skinny_callHistoryDisposition_t disposition);
static void sccp_device_new_callhistory(constDevicePtr device, const uint8_t lineInstance, const uint32_t callid, const skinny_callHistoryDisposition_t disposition);

static void sccp_device_indicate_onhook_remote(constDevicePtr device, const uint8_t lineInstance, const uint32_t callid);
static void sccp_device_indicate_offhook_remote(constDevicePtr device, const uint8_t lineInstance, const uint32_t callid);
static void sccp_device_indicate_connected_remote(constDevicePtr device, const uint8_t lineInstance, const uint32_t callid, skinny_callinfo_visibility_t visibility);
static void sccp_device_old_indicate_remoteHold(constDevicePtr device, uint8_t lineInstance, uint32_t callid, skinny_callpriority_t callpriority, skinny_callinfo_visibility_t visibility);
static void sccp_device_new_indicate_remoteHold(constDevicePtr device, uint8_t lineInstance, uint32_t callid, skinny_callpriority_t callpriority, skinny_callinfo_visibility_t visibility);

void sccp_dev_postregistration(devicePtr data);

static sccp_push_result_t sccp_device_pushURL(constDevicePtr device, const char *url, uint8_t priority, skinny_tone_t tone);
static sccp_push_result_t sccp_device_pushURLNotSupported(constDevicePtr device, const char *url, uint8_t priority, skinny_tone_t tone)
{
	return SCCP_PUSH_RESULT_NOT_SUPPORTED;
}

static sccp_push_result_t sccp_device_pushTextMessage(constDevicePtr device, const char *messageText, const char *from, uint8_t priority, skinny_tone_t tone);
static sccp_push_result_t sccp_device_pushTextMessageNotSupported(constDevicePtr device, const char *messageText, const char *from, uint8_t priority, skinny_tone_t tone)
{
	return SCCP_PUSH_RESULT_NOT_SUPPORTED;
}

static const struct sccp_device_indication_cb sccp_device_indication_newerDevices = {
	.remoteHold = sccp_device_new_indicate_remoteHold,
	.remoteOffhook = sccp_device_indicate_offhook_remote,
	.remoteOnhook = sccp_device_indicate_onhook_remote,
	.remoteConnected = sccp_device_indicate_connected_remote,
	.offhook = sccp_device_indicate_offhook,
	.onhook = sccp_device_indicate_onhook,
	.dialing = sccp_device_indicate_dialing,
	.proceed = sccp_device_indicate_proceed,
	.connected = sccp_device_indicate_connected,
	.callhistory = sccp_device_new_callhistory,
};

static const struct sccp_device_indication_cb sccp_device_indication_olderDevices = {
	.remoteHold = sccp_device_old_indicate_remoteHold,
	.remoteOffhook = sccp_device_indicate_offhook_remote,
	.remoteOnhook = sccp_device_indicate_onhook_remote,
	.remoteConnected = sccp_device_indicate_connected_remote,
	.offhook = sccp_device_indicate_offhook,
	.onhook = sccp_device_indicate_onhook,
	.dialing = sccp_device_indicate_dialing,
	.proceed = sccp_device_indicate_proceed,
	.connected = sccp_device_indicate_connected,
	.callhistory = sccp_device_old_callhistory,
};

static boolean_t sccp_device_checkACLTrue(constDevicePtr device)
{
	return TRUE;
}

static boolean_t sccp_device_trueResult(void)
{
	return TRUE;
}

static boolean_t sccp_device_falseResult(void)
{
	return FALSE;
}

static void sccp_device_setBackgroundImageNotSupported(constDevicePtr device, const char *url, const char *tn)
{
	sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: background image not set: this model does not support background images\n", device->id);
}

static void sccp_device_setBackgroundImage(constDevicePtr device, const char *url, const char *tn)
{
	if (!url || strncasecmp("http://", url, strlen("http://")) != 0) {
		pbx_log(LOG_WARNING, "%s: background image not set: '%s' is not an http:// URL\n", device->id, url ? url : "");
		return;
	}

	char xmlStr[StationMaxXMLMessage] = { 0 };
	unsigned int transactionID = sccp_random();

	snprintf(xmlStr, sizeof(xmlStr), "<setBackground><background><image>%s</image><icon>%s</icon></background></setBackground>\n", url, tn);

	device->protocol->sendUserToDeviceDataVersionMessage(device, APPID_BACKGROUND, 0, 0, transactionID, xmlStr, 0);
	sccp_log(DEBUGCAT_CORE)(VERBOSE_PREFIX_3 "%s: setting background image %s (transaction %d)\n", device->id, url, transactionID);
}

static void sccp_device_displayBackgroundImagePreviewNotSupported(constDevicePtr device, const char *url)
{
	sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: background image not shown: this model does not support background images\n", device->id);
}

static void sccp_device_displayBackgroundImagePreview(constDevicePtr device, const char *url)
{
	if (!url || strncmp("http://", url, strlen("http://")) != 0) {
		pbx_log(LOG_WARNING, "%s: background image preview not shown: '%s' is not an http:// URL\n", device->id, url ? url : "");
		return;
	}
	char xmlStr[StationMaxXMLMessage] = {0};
	unsigned int transactionID = sccp_random();

	snprintf(xmlStr, sizeof(xmlStr), "<setBackgroundPreview><image>%s</image></setBackgroundPreview>", url);

	device->protocol->sendUserToDeviceDataVersionMessage(device, APPID_BACKGROUND, 0, 0, transactionID, xmlStr, 0);
	sccp_log(DEBUGCAT_CORE)(VERBOSE_PREFIX_3 "%s: showing background image %s (transaction %d)\n", device->id, url, transactionID);
}

static void sccp_device_retrieveDeviceCapabilities(constDevicePtr device)
{
	char *xmlStr = "<getDeviceCaps></getDeviceCaps>";
	unsigned int transactionID = sccp_random();

	device->protocol->sendUserToDeviceDataVersionMessage(device, APPID_DEVICECAPABILITIES, 1, 0, transactionID, xmlStr, 2);
	sccp_log(DEBUGCAT_CORE)(VERBOSE_PREFIX_3 "%s: asking for device capabilities (transaction %d)\n", device->id, transactionID);
}

static void sccp_device_setRingtoneNotSupported(constDevicePtr device, const char *url)
{
	sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: ringtone not set: this model does not support setting the ringtone\n", device->id);
}

static void sccp_device_setRingtone(constDevicePtr device, const char *url)
{
	if (!url || strncmp("http://", url, strlen("http://")) != 0) {
		pbx_log(LOG_WARNING, "%s: ringtone not set: '%s' is not an http:// URL\n", device->id, url ? url : "");
		return;
	}

	char xmlStr[StationMaxXMLMessage] = {0};
	unsigned int transactionID = sccp_random();

	snprintf(xmlStr, sizeof(xmlStr), "<setRingTone><ringTone>%s</ringTone></setRingTone>", url);

	device->protocol->sendUserToDeviceDataVersionMessage(device, APPID_RINGTONE, 0, 0, transactionID, xmlStr, 0);
	sccp_log(DEBUGCAT_CORE)(VERBOSE_PREFIX_3 "%s: setting ringtone %s (transaction %d)\n", device->id, url, transactionID);
}

static sccp_dtmfmode_t sccp_device_getDtfmMode(constDevicePtr device)
{
	sccp_dtmfmode_t res = device->dtmfmode;

	if (device->dtmfmode == SCCP_DTMFMODE_AUTO) {
		if (device->device_features.phoneFeatures[2] & SKINNY_PHONE_FEATURES2_RFC2833) {
			res = SCCP_DTMFMODE_RFC2833;
		} else {
			res = SCCP_DTMFMODE_SKINNY;
		}
	}
	return res;
}

static void sccp_device_copyStr2Locale_UTF8(constDevicePtr d, char *dst, ICONV_CONST char *src, size_t dst_size)
{
	if (!dst || !src) {
		return;
	}
	sccp_copy_string(dst, src, dst_size);
}

#if HAVE_ICONV
int sccp_device_createiconv(devicePtr d)
{
	d->privateData->iconv = iconv_open(d->iconvcodepage, "UTF-8");
	if (d->privateData->iconv == (iconv_t) -1) {
		pbx_log(LOG_WARNING, "%s: phonecodepage=%s is not supported by iconv; text is sent to the phone unconverted (UTF-8)\n", d->id, d->iconvcodepage);
		return 0;
	}
	pbx_mutex_init(&d->privateData->iconv_lock);
	return 1;
}
void sccp_device_destroyiconv(devicePtr d)
{
	if (d->privateData->iconv != (iconv_t) -1) {
		pbx_mutex_destroy(&d->privateData->iconv_lock);
		iconv_close(d->privateData->iconv);
		d->privateData->iconv = (iconv_t) -1;
	}
}

static boolean_t sccp_device_convUtf8toLatin1(constDevicePtr d, ICONV_CONST char *utf8str, char *buf, size_t len)
{
	if (d->privateData->iconv == (iconv_t) -1) {
		// fallback to plain string copy
		sccp_copy_string(buf, utf8str, len);
		return TRUE;
	}
	size_t incount = 0;

	size_t outcount = len;
	incount = sccp_strlen(utf8str);
	if (incount) {
		pbx_mutex_lock(&d->privateData->iconv_lock);
		if (iconv(d->privateData->iconv, &utf8str, &incount, &buf, &outcount) == (size_t) -1) {
			if (errno == E2BIG) {
				pbx_log(LOG_WARNING, "%s: text converted to %s was longer than its %d-byte field; truncated\n", d->id, d->iconvcodepage, (int)len);
			} else if (errno == EILSEQ) {
				pbx_log(LOG_WARNING, "%s: text contains a character with no %s equivalent (or invalid UTF-8); converted only up to that character\n", d->id, d->iconvcodepage);
			} else if (errno == EINVAL) {
				pbx_log(LOG_WARNING, "%s: text ends in an incomplete UTF-8 character; converted only up to that character\n", d->id);
			} else {
				pbx_log(LOG_WARNING, "%s: conversion to %s failed (errno %d: %s); text partially converted\n", d->id, d->iconvcodepage, errno, strerror(errno));
			}
		}
		pbx_mutex_unlock(&d->privateData->iconv_lock);
	}
	return TRUE;
}

static void sccp_device_copyStr2Locale_Convert(constDevicePtr d, char *dst, ICONV_CONST char *src, size_t dst_size)
{
	if (!dst || !src) {
		return;
	}
	char *buf = (char *)sccp_alloca(dst_size);
	size_t buf_len = dst_size;
	memset(buf, 0, dst_size);
	if (sccp_device_convUtf8toLatin1(d, src, buf, buf_len)) {
		sccp_copy_string(dst, buf, dst_size);
		return;
	}
}
#endif

static boolean_t sccp_device_checkACL(constDevicePtr device)
{
	struct sockaddr_storage sas = { 0 };
	boolean_t matchesACL = FALSE;

	if (!device || !device->session) {
		return FALSE;
	}

	sccp_session_getSas(device->session, &sas);

	if (!device->ha) {
		sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: no deny/permit rules; connection allowed\n", device->id);
		return TRUE;
	}

	if (sccp_apply_ha(device->ha, &sas) != AST_SENSE_ALLOW) {
		struct ast_str *ha_buf = pbx_str_alloca(DEFAULT_PBX_STR_BUFFERSIZE);

		sccp_print_ha(ha_buf, DEFAULT_PBX_STR_BUFFERSIZE, GLOB(ha));

		sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: not allowed by deny/permit (%s); checking permithost\n", device->id, pbx_str_buffer(ha_buf));
	} else {
		matchesACL = TRUE;
	}
	sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: access check result: %s\n", device->id, matchesACL ? "yes" : "no");
	return matchesACL;
}

/*
 * run before reload is start on devices
 */
void sccp_device_pre_reload(void)
{
	sccp_device_t *d = NULL;
	sccp_buttonconfig_t *config = NULL;

	SCCP_RWLIST_RDLOCK(&GLOB(devices));
	SCCP_RWLIST_TRAVERSE(&GLOB(devices), d, list) {
		sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: marked for removal\n", d->id);
#ifdef CS_SCCP_REALTIME
		if (!d->realtime) {
			d->pendingDelete = 1;
		}
#endif
		d->pendingUpdate = 0;

		d->softkeyset = NULL;
		d->softKeyConfiguration.modes = NULL;
		d->softKeyConfiguration.size = 0;
		d->isAnonymous=FALSE;

		SCCP_LIST_LOCK(&d->buttonconfig);
		SCCP_LIST_TRAVERSE(&d->buttonconfig, config, list) {
			sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_4 "%s: button %d marked for removal\n", d->id, config->index);
			config->pendingDelete = 1;
			config->pendingUpdate = 0;
		}
		SCCP_LIST_UNLOCK(&d->buttonconfig);
		d->softkeyset = NULL;
		d->softKeyConfiguration.modes = 0;
		d->softKeyConfiguration.size = 0;
	}
	SCCP_RWLIST_UNLOCK(&GLOB(devices));
}

boolean_t sccp_device_check_update(devicePtr device)
{
	AUTO_RELEASE(sccp_device_t, d , device ? sccp_device_retain(device) : NULL);
	boolean_t res = FALSE;

	if (d) {
		if ((d->pendingUpdate || d->pendingDelete)) {
			do {
				if (sccp_device_numberOfChannels(d) > 0) {
					break;
				}

				sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_1 "device %s needs a restart after the sccp.conf change (update %d, delete %d)\n", d->id, d->pendingUpdate, d->pendingDelete);

				d->pendingUpdate = 0;
				sccp_dev_clean_restart(d, (d->pendingDelete) ? TRUE : FALSE);
				res = TRUE;
			} while (0);
		}
	}
	return res;
}

/*
 * run after the new device config is loaded during the reload process
 */
void sccp_device_post_reload(void)
{
	sccp_device_t *d = NULL;
	sccp_log((DEBUGCAT_CONFIG)) (VERBOSE_PREFIX_1 "SCCP: applying device changes after reload\n");

	SCCP_RWLIST_TRAVERSE_SAFE_BEGIN(&GLOB(devices), d, list) {
		if (!d->pendingDelete && !d->pendingUpdate) {
			continue;
		}
		/* Because of the previous check, the only reason that the device hasn't
		 * been updated will be because it is currently engaged in a call.
		 */
		if (!sccp_device_check_update(d)) {
			sccp_log((DEBUGCAT_CONFIG + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "device %s restarts after its current call\n", d->id);
		}
		// make sure preferences only contains the codecs that this device is capable of
		sccp_codec_reduceSet(d->preferences.audio , d->capabilities.audio);
		sccp_codec_reduceSet(d->preferences.video , d->capabilities.video);
		/* should we re-check the device after hangup ? */
	}
	SCCP_LIST_TRAVERSE_SAFE_END;
}

const sccp_accessorystate_t sccp_device_getAccessoryStatus(constDevicePtr d, const sccp_accessory_t accessory)
{
	pbx_assert(d != NULL && d->privateData != NULL);
	sccp_private_lock(d->privateData);
	sccp_accessorystate_t accessoryStatus = d->privateData->accessoryStatus[accessory];
	sccp_private_unlock(d->privateData);
	return accessoryStatus;
}

const sccp_accessory_t sccp_device_getActiveAccessory(constDevicePtr d)
{
	sccp_accessory_t res = SCCP_ACCESSORY_NONE;
	pbx_assert(d != NULL && d->privateData != NULL);
	sccp_accessory_t accessory = SCCP_ACCESSORY_NONE;
	sccp_private_lock(d->privateData);
	for (accessory = SCCP_ACCESSORY_NONE ; accessory < SCCP_ACCESSORY_SENTINEL; enum_incr(accessory)) {
		if (d->privateData->accessoryStatus[accessory] == SCCP_ACCESSORYSTATE_OFFHOOK) {
			res = accessory;
			break;
		}
	}
	sccp_private_unlock(d->privateData);
	return res;
}

int sccp_device_setAccessoryStatus(constDevicePtr d, const sccp_accessory_t accessory, const sccp_accessorystate_t state)
{
	pbx_assert(d != NULL && d->privateData != NULL);
	pbx_assert(accessory > SCCP_ACCESSORY_NONE && accessory < SCCP_ACCESSORY_SENTINEL && state > SCCP_ACCESSORYSTATE_NONE && state < SCCP_ACCESSORYSTATE_SENTINEL);
	int changed = 0;

	sccp_private_lock(d->privateData);
	if (state != d->privateData->accessoryStatus[accessory]) {
		d->privateData->accessoryStatus[accessory] = state;
		if (state == SCCP_ACCESSORYSTATE_ONHOOK) {
			sccp_dev_cleardisplaynotify(d);
			sccp_dev_check_displayprompt(d);
		}
		changed=1;
	}
	sccp_private_unlock(d->privateData);
	sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: accessory %s is %s\n", d->id, sccp_accessory2str(accessory), sccp_accessorystate2str(state));
	return changed;
}

const sccp_devicestate_t sccp_device_getDeviceState(constDevicePtr d)
{
	pbx_assert(d != NULL && d->privateData != NULL);

	sccp_devicestate_t state = SCCP_DEVICESTATE_SENTINEL;

	sccp_private_lock(d->privateData);
	state = d->privateData->deviceState;
	sccp_private_unlock(d->privateData);

	return state;
}

int sccp_device_setDeviceState(constDevicePtr d, const sccp_devicestate_t state)
{
	pbx_assert(d != NULL && d->privateData != NULL);
	int changed = 0;

	sccp_private_lock(d->privateData);
	if (state != d->privateData->deviceState) {
		d->privateData->deviceState = state;
		changed=1;
	}
	sccp_private_unlock(d->privateData);

	sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: device state %s\n", d->id, sccp_devicestate2str(state));
	return changed;
}

const skinny_registrationstate_t sccp_device_getRegistrationState(constDevicePtr d)
{
	pbx_assert(d != NULL && d->privateData != NULL);

	skinny_registrationstate_t state = SKINNY_REGISTRATIONSTATE_SENTINEL;

	sccp_private_lock(d->privateData);
	state = d->privateData->registrationState;
	sccp_private_unlock(d->privateData);

	return state;
}

int sccp_device_setRegistrationState(constDevicePtr d, const skinny_registrationstate_t state)
{
	pbx_assert(d != NULL);
	if (isPointerDead(d) || !d->privateData) {
		return 0;;
	}

	int changed = 0;

	if (!isPointerDead(d->privateData)) {
		sccp_private_lock(d->privateData);
		if (state != d->privateData->registrationState) {
			d->privateData->registrationState = state;
			changed=1;
		}
		sccp_private_unlock(d->privateData);
	}

#ifdef CS_AST_HAS_STASIS_ENDPOINT
	if (iPbx.endpoint_online && iPbx.endpoint_offline) {
		if (SKINNY_DEVICE_RS_OK == state) {
			struct sockaddr_storage ourip = { 0 };
			sccp_session_getOurIP(d->session, &ourip, 0);
			iPbx.endpoint_online(d->endpoint, sccp_netsock_stringify(&ourip));
		} else if (SKINNY_DEVICE_RS_NONE == state) {
			iPbx.endpoint_offline(d->endpoint, "Unreachable");
		} else {
			iPbx.endpoint_offline(d->endpoint, "expired");
		}
	}
#endif

	sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: registration state %s\n", d->id, skinny_registrationstate2str(state));
	return changed;
}

/* Returns retained device with default/global values */
devicePtr sccp_device_create(const char * id)
{
	sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "SCCP: creating device\n");
	struct sccp_private_device_data * private_data = NULL;

	sccp_device_t *d = (sccp_device_t *) sccp_refcount_object_alloc(sizeof(sccp_device_t), SCCP_REF_DEVICE, id, __sccp_device_destroy);

	if (!d) {
		pbx_log(LOG_ERROR, "%s: device not created: out of memory\n", id);
		return NULL;
	}

	private_data = (sccp_private_device_data_t *)sccp_calloc(sizeof *private_data, 1);
	if (!private_data) {
		pbx_log(LOG_ERROR, "%s: device not created: out of memory\n", id);
		sccp_device_release(&d);
		return NULL;
	}
	d->privateData = private_data;
	d->privateData->registrationState = SKINNY_DEVICE_RS_NONE;
	sccp_mutex_init(&d->privateData->lock);

	sccp_copy_string(d->id, id, sizeof(d->id));
	SCCP_LIST_HEAD_INIT(&d->buttonconfig);
	SCCP_LIST_HEAD_INIT(&d->selectedChannels);
	SCCP_LIST_HEAD_INIT(&d->addons);
#ifdef CS_AST_HAS_STASIS_ENDPOINT
	if (iPbx.endpoint_create) {
		d->endpoint = iPbx.endpoint_create("SCCP", id);
	}
#endif
	memset(&d->softKeyConfiguration.activeMask, 0xFF, sizeof d->softKeyConfiguration.activeMask);
	memset(d->call_statistics, 0, ((sizeof *d->call_statistics) * 2));

	sccp_device_setDeviceState(d, SCCP_DEVICESTATE_ONHOOK);
	d->postregistration_thread = AST_PTHREADT_STOP;
	d->defaultLineInstance = SCCP_FIRST_LINEINSTANCE;

	d->protocolversion = SCCP_DRIVER_SUPPORTED_PROTOCOL_LOW;
	d->protocol        = sccp_protocol_getDeviceProtocol(d, SCCP_PROTOCOL);

	sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "initializing message stack\n");

#ifndef SCCP_ATOMIC
	pbx_mutex_init(&d->messageStack.lock);
	sccp_mutex_lock(&d->messageStack.lock);
#endif
	for(uint8_t i = 0; i < ARRAY_LEN(d->messageStack.messages); i++) {
		d->messageStack.messages[i] = NULL;
	}
#ifndef SCCP_ATOMIC
	sccp_mutex_unlock(&d->messageStack.lock);
#endif
#if HAVE_ICONV
	d->privateData->iconv = (iconv_t) -1;
#endif

	d->pushURL = sccp_device_pushURLNotSupported;
	d->pushTextMessage = sccp_device_pushTextMessageNotSupported;
	d->checkACL = sccp_device_checkACL;
	d->useHookFlash = sccp_device_falseResult;
	d->hasDisplayPrompt = sccp_device_trueResult;
	d->hasLabelLimitedDisplayPrompt = sccp_device_falseResult;
	d->hasEnhancedIconMenuSupport = sccp_device_falseResult;
	d->hasMWILight = sccp_device_trueResult;
	d->setBackgroundImage = sccp_device_setBackgroundImageNotSupported;
	d->displayBackgroundImagePreview = sccp_device_displayBackgroundImagePreviewNotSupported;
	d->retrieveDeviceCapabilities = sccp_device_retrieveDeviceCapabilities;
	d->setRingTone = sccp_device_setRingtoneNotSupported;
	d->getDtmfMode = sccp_device_getDtfmMode;
	d->copyStr2Locale = sccp_device_copyStr2Locale_UTF8;
	d->keepalive = d->keepaliveinterval = d->keepalive ? d->keepalive : GLOB(keepalive);
	d->mwiUpdateRequired = TRUE;

	d->pendingUpdate = 0;
	d->pendingDelete = 0;
	return d;
}

/* Returns retained device with default/global values */
devicePtr sccp_device_createAnonymous(const char * name)
{
	sccp_device_t *d = sccp_device_create(name);

	if (!d) {
		pbx_log(LOG_ERROR, "%s: anonymous device not created: out of memory\n", name);
		return NULL;
	}

	d->realtime = TRUE;
	d->isAnonymous = TRUE;
	d->checkACL = sccp_device_checkACLTrue;
	return d;
}

static void __saveLastDialedNumberToDatabase(constDevicePtr device)
{
	char family[25];
	snprintf(family, sizeof(family), "SCCP/%s", device->id);
	if (!sccp_strlen_zero(device->redialInformation.number)) {
		char buffer[SCCP_MAX_EXTENSION + 32] = "\0";
		snprintf (buffer, sizeof(buffer), "%s;lineInstance=%d", device->redialInformation.number, device->redialInformation.lineInstance);
		iPbx.feature_addToDatabase(family, "lastDialedNumber", buffer);
	} else {
		iPbx.feature_removeFromDatabase(family, "lastDialedNumber");
	}
}

void sccp_device_setLastNumberDialed(devicePtr device, const char * lastNumberDialed, const sccp_linedevice_t * ld)
{
	boolean_t ResetNoneLineInstance = FALSE;
	boolean_t redial_active = FALSE;
	boolean_t update_database = FALSE;

	if (device->useRedialMenu) {
		return;
	}

	if (lastNumberDialed && !sccp_strlen_zero(lastNumberDialed)) {
		if (sccp_strlen_zero(device->redialInformation.number)) {
			ResetNoneLineInstance = TRUE;
		}
		if(!sccp_strequals(device->redialInformation.number, lastNumberDialed) || device->redialInformation.lineInstance != ld->lineInstance) {
			sccp_log(DEBUGCAT_DEVICE) (VERBOSE_PREFIX_3 "%s: last dialed number set to %s\n", DEV_ID_LOG(device), lastNumberDialed);
			sccp_copy_string(device->redialInformation.number, lastNumberDialed, sizeof(device->redialInformation.number));
			device->redialInformation.lineInstance = ld->lineInstance;
			update_database = TRUE;
		}
		redial_active = TRUE;
	} else {
		if (!sccp_strlen_zero(device->redialInformation.number) || device->redialInformation.lineInstance != 0) {
			sccp_log(DEBUGCAT_DEVICE) (VERBOSE_PREFIX_3 "%s: last dialed number cleared\n", DEV_ID_LOG(device));
			sccp_copy_string(device->redialInformation.number, "", sizeof(device->redialInformation.number));
			device->redialInformation.lineInstance = 0;
			update_database = TRUE;
		}
	}
	sccp_softkey_setSoftkeyState(device, KEYMODE_ONHOOK, SKINNY_LBL_REDIAL, redial_active);
	sccp_softkey_setSoftkeyState(device, KEYMODE_OFFHOOK, SKINNY_LBL_REDIAL, redial_active);
	sccp_softkey_setSoftkeyState(device, KEYMODE_OFFHOOKFEAT, SKINNY_LBL_REDIAL, redial_active);
	sccp_softkey_setSoftkeyState(device, KEYMODE_ONHOOKSTEALABLE, SKINNY_LBL_REDIAL, redial_active);
	if (ResetNoneLineInstance) {
		sccp_dev_set_keyset(device, 0, 0, KEYMODE_ONHOOK);
	}
	if (update_database) {
		__saveLastDialedNumberToDatabase(device);
	}
}

void sccp_device_preregistration(devicePtr device)
{
	if (!device) {
		return;
	}
	switch (device->skinny_type) {
		case SKINNY_DEVICETYPE_CISCO7906:
		case SKINNY_DEVICETYPE_CISCO7911:
		case SKINNY_DEVICETYPE_CISCO7931:
		case SKINNY_DEVICETYPE_CISCO7941:
		case SKINNY_DEVICETYPE_CISCO7941GE:
		case SKINNY_DEVICETYPE_CISCO7942:
		case SKINNY_DEVICETYPE_CISCO7945:
		case SKINNY_DEVICETYPE_CISCO7921:
		case SKINNY_DEVICETYPE_CISCO7925:
		case SKINNY_DEVICETYPE_CISCO7926:
		case SKINNY_DEVICETYPE_CISCO7961:
		case SKINNY_DEVICETYPE_CISCO7961GE:
		case SKINNY_DEVICETYPE_CISCO7962:
		case SKINNY_DEVICETYPE_CISCO7965:
		case SKINNY_DEVICETYPE_CISCO7970:
		case SKINNY_DEVICETYPE_CISCO7971:
		case SKINNY_DEVICETYPE_CISCO7975:
		case SKINNY_DEVICETYPE_CISCO7985:
		case SKINNY_DEVICETYPE_CISCO_IP_COMMUNICATOR:
		case SKINNY_DEVICETYPE_CISCO6901:
		case SKINNY_DEVICETYPE_CISCO6911:
		case SKINNY_DEVICETYPE_CISCO6921:
		case SKINNY_DEVICETYPE_CISCO6941:
		case SKINNY_DEVICETYPE_CISCO6945:
		case SKINNY_DEVICETYPE_CISCO6961:
		case SKINNY_DEVICETYPE_CISCO8941:
		case SKINNY_DEVICETYPE_CISCO8945:
			device->indicate = &sccp_device_indication_newerDevices;
			break;
		default:
			device->indicate = &sccp_device_indication_olderDevices;
			break;
	}
#if HAVE_ICONV
	if (!(device->device_features.phoneFeatures[1] & SKINNY_PHONE_FEATURES1_UTF8)) {
		sccp_device_createiconv(device);
		device->copyStr2Locale = sccp_device_copyStr2Locale_Convert;
	}
#endif
}

/*
 * Add a device to the global sccp_device list
 * needs to be called with a retained device
 * adds a retained device to the list (refcount + 1)
 */
void sccp_device_addToGlobals(constDevicePtr device)
{
	if (!device) {
		pbx_log(LOG_ERROR, "SCCP: sccp_device_addToGlobals() was called without a device (caller bug)\n");
		return;
	}
	sccp_device_t *d = sccp_device_retain(device);
	if (d) {
		SCCP_RWLIST_WRLOCK(&GLOB(devices));
		SCCP_RWLIST_INSERT_SORTALPHA(&GLOB(devices), d, list, id);
		SCCP_RWLIST_UNLOCK(&GLOB(devices));
		sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "device %s added\n", d->id);
	}
}

/*
 * Returns device or NULL needs to be called with a retained device
 * removes the retained device within the list (refcount - 1)
 */
void sccp_device_removeFromGlobals(devicePtr device)
{
	if (!device) {
		pbx_log(LOG_ERROR, "SCCP: sccp_device_removeFromGlobals() was called without a device (caller bug)\n");
		return;
	}
	sccp_device_t * d = NULL;

	SCCP_RWLIST_WRLOCK(&GLOB(devices));
	d = SCCP_RWLIST_REMOVE(&GLOB(devices), device, list);
	SCCP_RWLIST_UNLOCK(&GLOB(devices));

	if(d) {
		sccp_log((DEBUGCAT_CORE + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "device %s removed\n", DEV_ID_LOG(device));
		sccp_device_release(&d);					/* explicit release of device after removing from list */
	}
}

static uint8_t sccp_addon_build_buttontemplate(constDevicePtr d, sccp_addon_t *addon, btnlist * btn, uint8_t btn_index)
{
	uint8_t i = 0;
	uint8_t start_point = btn_index;
	skinny_devicetype_t type = addon->type;

	sccp_log((DEBUGCAT_CONFIG + DEBUGCAT_BUTTONTEMPLATE + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: building button template for %s (%d)\n", d->id, skinny_devicetype2str(type), type);

	switch (type) {
		case SKINNY_DEVICETYPE_CISCO_ADDON_7914:
			for (i = 0; i < 14; i++) {
				btn[btn_index++].type = SCCP_BUTTONTYPE_MULTI;
			}
			break;
		case SKINNY_DEVICETYPE_CISCO_ADDON_7915_12BUTTON:
		case SKINNY_DEVICETYPE_CISCO_ADDON_7916_12BUTTON:
			for (i = 0; i < 12; i++) {
				btn[btn_index++].type = SCCP_BUTTONTYPE_MULTI;
			}
			break;
		case SKINNY_DEVICETYPE_CISCO_ADDON_7915_24BUTTON:
		case SKINNY_DEVICETYPE_CISCO_ADDON_7916_24BUTTON:
			for (i = 0; i < 24; i++) {
				btn[btn_index++].type = SCCP_BUTTONTYPE_MULTI;
			}
			break;
		case SKINNY_DEVICETYPE_CISCO_ADDON_SPA500S:
		case SKINNY_DEVICETYPE_CISCO_ADDON_SPA500DS:
		case SKINNY_DEVICETYPE_CISCO_ADDON_SPA932DS:
			for (i = 0; i < 32; i++) {
				btn[btn_index++].type = SCCP_BUTTONTYPE_MULTI;
			}
			break;
		default:
			pbx_log(LOG_WARNING, "%s: expansion module type %d has no button layout; its buttons are not added\n", d->id, type);
			break;
	}
	for (i = start_point; i < btn_index; i++) {
		btn[i].devicetype = type;
	}

	sccp_log(DEBUGCAT_DEVICE)(VERBOSE_PREFIX_3 "%s: %d add-on buttons\n", d->id, btn_index - start_point);
	return btn_index;
}

uint8_t sccp_dev_build_buttontemplate(devicePtr d, btnlist * btn)
{
	uint8_t i = 0;
	uint8_t btn_index=0;
	skinny_devicetype_t type = d->skinny_type;

	sccp_log((DEBUGCAT_CONFIG + DEBUGCAT_BUTTONTEMPLATE + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: building button template for %s (%d), configured model %s\n", d->id, skinny_devicetype2str(type), type, d->config_type);

	switch (type) {
		case SKINNY_DEVICETYPE_30SPPLUS:
		case SKINNY_DEVICETYPE_30VIP:
			for (i = 0; i < 4; i++) {
				btn[btn_index++].type = SCCP_BUTTONTYPE_MULTI;
			}
			btn[btn_index++].type = SKINNY_BUTTONTYPE_LASTNUMBERREDIAL;
			btn[btn_index++].type = SKINNY_BUTTONTYPE_VOICEMAIL;
			btn[btn_index++].type = SKINNY_BUTTONTYPE_CALLPARK;
			btn[btn_index++].type = SKINNY_BUTTONTYPE_FORWARDALL;
			btn[btn_index++].type = SKINNY_BUTTONTYPE_CONFERENCE;
			for (i = 0; i < 4; i++) {
				btn[btn_index++].type = SKINNY_BUTTONTYPE_UNDEFINED;
			}
			for (i = 0; i < 13; i++) {
				btn[btn_index++].type = SCCP_BUTTONTYPE_MULTI;
			}
			d->hasMWILight = sccp_device_falseResult;
			break;
		case SKINNY_DEVICETYPE_12SPPLUS:
		case SKINNY_DEVICETYPE_12SP:
		case SKINNY_DEVICETYPE_12:
			for (i = 0; i < 2; i++) {
				btn[btn_index++].type = SCCP_BUTTONTYPE_LINE;
			}
			for (i = 0; i < 4; i++) {
				btn[btn_index++].type = SCCP_BUTTONTYPE_SPEEDDIAL;
			}
			btn[btn_index++].type = SKINNY_BUTTONTYPE_HOLD;
			btn[btn_index++].type = SKINNY_BUTTONTYPE_LASTNUMBERREDIAL;
			btn[btn_index++].type = SKINNY_BUTTONTYPE_TRANSFER;
			btn[btn_index++].type = SKINNY_BUTTONTYPE_FORWARDALL;
			btn[btn_index++].type = SKINNY_BUTTONTYPE_CALLPARK;
			btn[btn_index++].type = SKINNY_BUTTONTYPE_VOICEMAIL;
			d->hasMWILight = sccp_device_falseResult;
			break;
		case SKINNY_DEVICETYPE_CISCO7902:
			btn[btn_index++].type = SCCP_BUTTONTYPE_LINE;
			btn[btn_index++].type = SKINNY_BUTTONTYPE_HOLD;
			btn[btn_index++].type = SKINNY_BUTTONTYPE_TRANSFER;
			btn[btn_index++].type = SKINNY_BUTTONTYPE_DISPLAY;
			btn[btn_index++].type = SKINNY_BUTTONTYPE_VOICEMAIL;
			btn[btn_index++].type = SKINNY_BUTTONTYPE_CONFERENCE;
			btn[btn_index++].type = SKINNY_BUTTONTYPE_FORWARDALL;
			for (i = 0; i < 4; i++) {
				btn[btn_index++].type = SCCP_BUTTONTYPE_SPEEDDIAL;
			}
			btn[btn_index++].type = SKINNY_BUTTONTYPE_LASTNUMBERREDIAL;
			break;
		case SKINNY_DEVICETYPE_CISCO7910:
			btn[btn_index++].type = SCCP_BUTTONTYPE_LINE;
			btn[btn_index++].type = SKINNY_BUTTONTYPE_HOLD;
			btn[btn_index++].type = SKINNY_BUTTONTYPE_TRANSFER;
			btn[btn_index++].type = SKINNY_BUTTONTYPE_DISPLAY;
			btn[btn_index++].type = SKINNY_BUTTONTYPE_VOICEMAIL;
			btn[btn_index++].type = SKINNY_BUTTONTYPE_CONFERENCE;
			btn[btn_index++].type = SKINNY_BUTTONTYPE_FORWARDALL;
			for (i = 0; i < 2; i++) {
				btn[btn_index++].type = SCCP_BUTTONTYPE_SPEEDDIAL;
			}
			btn[btn_index++].type = SKINNY_BUTTONTYPE_LASTNUMBERREDIAL;
			break;
		case SKINNY_DEVICETYPE_CISCO7906:
			btn[btn_index++].type = SCCP_BUTTONTYPE_LINE;
			btn[btn_index++].type = SKINNY_BUTTONTYPE_HOLD;
			for (i = 0; i < 9; i++) {
				btn[btn_index++].type = SCCP_BUTTONTYPE_SPEEDDIAL;
			}
			d->useHookFlash = sccp_device_trueResult;
			break;
		case SKINNY_DEVICETYPE_CISCO7911:
		case SKINNY_DEVICETYPE_CISCO7905:
		case SKINNY_DEVICETYPE_CISCO7912:
			btn[btn_index++].type = SCCP_BUTTONTYPE_LINE;
			btn[btn_index++].type = SKINNY_BUTTONTYPE_HOLD;
			for (i = 0; i < 9; i++) {
				btn[btn_index++].type = SCCP_BUTTONTYPE_SPEEDDIAL;
			}
			d->hasEnhancedIconMenuSupport = sccp_device_trueResult;
			d->useHookFlash = sccp_device_trueResult;
			break;
		case SKINNY_DEVICETYPE_CISCO7920:
			// only four lines are displayed, but scrolling down shows two additional ones
			for(i = 0; i < 6; i++) {
				btn[btn_index++].type = SCCP_BUTTONTYPE_MULTI;
			}
			break;
		case SKINNY_DEVICETYPE_CISCO7931:
			for (i = 0; i < 20; i++) {
				btn[btn_index++].type = SCCP_BUTTONTYPE_MULTI;
			}
			btn[btn_index].type = SKINNY_BUTTONTYPE_MESSAGES;    btn[btn_index].instance = 21; btn_index++;
			btn[btn_index].type = SKINNY_BUTTONTYPE_DIRECTORY;   btn[btn_index].instance = 22; btn_index++;
			btn[btn_index].type = SKINNY_BUTTONTYPE_HEADSET;     btn[btn_index].instance = 23; btn_index++;
			btn[btn_index].type = SKINNY_BUTTONTYPE_APPLICATION; btn[btn_index].instance = 24; btn_index++;
			d->hasEnhancedIconMenuSupport = sccp_device_trueResult;
			break;
		case SKINNY_DEVICETYPE_CISCO7935:
		case SKINNY_DEVICETYPE_CISCO7936:
		case SKINNY_DEVICETYPE_CISCO7937:
			for (i = 0; i < 2; i++) {
				btn[btn_index++].type = SCCP_BUTTONTYPE_LINE;
			}
			break;
		case SKINNY_DEVICETYPE_CISCO7921:
		case SKINNY_DEVICETYPE_CISCO7925:
		case SKINNY_DEVICETYPE_CISCO7926:
			for (i = 0; i < 6; i++) {
				btn[btn_index++].type = SCCP_BUTTONTYPE_MULTI;
			}
			break;
		case SKINNY_DEVICETYPE_CISCO7940:
			d->pushTextMessage = sccp_device_pushTextMessage;
			d->pushURL = sccp_device_pushURL;
			btn[btn_index++].type = SCCP_BUTTONTYPE_MULTI;
			btn[btn_index++].type = SCCP_BUTTONTYPE_MULTI;
			break;
		case SKINNY_DEVICETYPE_CISCO7960:
			d->pushTextMessage = sccp_device_pushTextMessage;
			d->pushURL = sccp_device_pushURL;
			for (i = 6; i > 0; i--) {
				btn[btn_index++].type = SCCP_BUTTONTYPE_MULTI;
			}
			break;
		case SKINNY_DEVICETYPE_CISCO7941:
		case SKINNY_DEVICETYPE_CISCO7941GE:
		case SKINNY_DEVICETYPE_CISCO7942:
		case SKINNY_DEVICETYPE_CISCO7945:
			d->pushTextMessage = sccp_device_pushTextMessage;
			d->pushURL = sccp_device_pushURL;
			d->setBackgroundImage = sccp_device_setBackgroundImage;
			d->displayBackgroundImagePreview = sccp_device_displayBackgroundImagePreview;
			d->setRingTone = sccp_device_setRingtone;
			btn[btn_index++].type = SCCP_BUTTONTYPE_MULTI;
			btn[btn_index++].type = SCCP_BUTTONTYPE_MULTI;
			break;
		case SKINNY_DEVICETYPE_CISCO7961:
		case SKINNY_DEVICETYPE_CISCO7961GE:
		case SKINNY_DEVICETYPE_CISCO7962:
		case SKINNY_DEVICETYPE_CISCO7965:
			d->pushTextMessage = sccp_device_pushTextMessage;
			d->pushURL = sccp_device_pushURL;
			d->setBackgroundImage = sccp_device_setBackgroundImage;
			d->displayBackgroundImagePreview = sccp_device_displayBackgroundImagePreview;
			d->setRingTone = sccp_device_setRingtone;
			d->hasEnhancedIconMenuSupport = sccp_device_trueResult;
			for (i = 6; i > 0; i--) {
				btn[btn_index++].type = SCCP_BUTTONTYPE_MULTI;
			}
			break;
		case SKINNY_DEVICETYPE_CISCO7970:
		case SKINNY_DEVICETYPE_CISCO7971:
		case SKINNY_DEVICETYPE_CISCO7975:
			/* the nokia icc client identifies itself as SKINNY_DEVICETYPE_CISCO7970, but it can only have one line  */
			if (!strcasecmp(d->config_type, "nokia-icc")) {						// this is for nokia icc legacy support (Old releases) -FS
				btn[btn_index++].type = SCCP_BUTTONTYPE_MULTI;
			} else {
				for (i = 8; i > 0; i--) {
					btn[btn_index++].type = SCCP_BUTTONTYPE_MULTI;
				}

				d->pushTextMessage = sccp_device_pushTextMessage;
				d->pushURL = sccp_device_pushURL;
				d->setBackgroundImage = sccp_device_setBackgroundImage;
				d->displayBackgroundImagePreview = sccp_device_displayBackgroundImagePreview;
				d->setRingTone = sccp_device_setRingtone;
				d->hasEnhancedIconMenuSupport = sccp_device_trueResult;
			}
			d->hasMWILight = sccp_device_falseResult;
			break;
		case SKINNY_DEVICETYPE_CISCO_IP_COMMUNICATOR:
			/* the nokia icc client identifies itself as SKINNY_DEVICETYPE_CISCO7970, but it can only have one line  */
			if (!strcasecmp(d->config_type, "nokia-icc")) {						// this is for nokia icc legacy support (Old releases) -FS
				btn[btn_index++].type = SCCP_BUTTONTYPE_MULTI;
			} else {
				for (i = 8; i > 0; i--) {
					btn[btn_index++].type = SCCP_BUTTONTYPE_MULTI;
				}

				d->pushTextMessage = sccp_device_pushTextMessage;
				d->pushURL = sccp_device_pushURL;
				d->setBackgroundImage = sccp_device_setBackgroundImage;
				d->displayBackgroundImagePreview = sccp_device_displayBackgroundImagePreview;
				d->setRingTone = sccp_device_setRingtone;
			}
			break;
		case SKINNY_DEVICETYPE_CISCO7985:
#ifdef CS_SCCP_VIDEO
			sccp_softkey_setSoftkeyState(d, KEYMODE_CONNTRANS, SKINNY_LBL_VIDEO_MODE, TRUE);
#endif
			for (i = 0; i < 1; i++) {
				btn[btn_index++].type = SCCP_BUTTONTYPE_MULTI;
			}
			break;
		case SKINNY_DEVICETYPE_NOKIA_ICC:
			btn[btn_index++].type = SCCP_BUTTONTYPE_MULTI;
			d->useHookFlash = sccp_device_trueResult;
			break;
		case SKINNY_DEVICETYPE_NOKIA_E_SERIES:
			btn[btn_index++].type = SCCP_BUTTONTYPE_LINE;
			btn[btn_index++].type = SCCP_BUTTONTYPE_LINE;
			for (i = 0; i < 5; i++) {
				btn[btn_index++].type = SCCP_BUTTONTYPE_SPEEDDIAL;
			}
			break;
		case SKINNY_DEVICETYPE_VGC:
		case SKINNY_DEVICETYPE_ANALOG_GATEWAY:
			btn[btn_index++].type = SCCP_BUTTONTYPE_LINE;
			d->hasDisplayPrompt = sccp_device_falseResult;
			d->useHookFlash = sccp_device_trueResult;
			d->hasMWILight = sccp_device_falseResult;
			break;
		case SKINNY_DEVICETYPE_ATA188:
		case SKINNY_DEVICETYPE_ATA186:
			btn[btn_index++].type = SCCP_BUTTONTYPE_LINE;
			for (i = 0; i < 4; i++) {
				btn[btn_index++].type = SCCP_BUTTONTYPE_SPEEDDIAL;
			}
			d->hasDisplayPrompt = sccp_device_falseResult;
			d->useHookFlash = sccp_device_trueResult;
			d->hasMWILight = sccp_device_falseResult;
			break;
		case SKINNY_DEVICETYPE_CISCO8941:
		case SKINNY_DEVICETYPE_CISCO8945:
#ifdef CS_SCCP_VIDEO
			sccp_softkey_setSoftkeyState(d, KEYMODE_CONNTRANS, SKINNY_LBL_VIDEO_MODE, TRUE);
#endif
			d->pushTextMessage = sccp_device_pushTextMessage;
			d->pushURL = sccp_device_pushURL;
			d->setBackgroundImage = sccp_device_setBackgroundImage;
			d->displayBackgroundImagePreview = sccp_device_displayBackgroundImagePreview;
			d->setRingTone = sccp_device_setRingtone;
			d->hasDisplayPrompt = sccp_device_falseResult;
			d->hasLabelLimitedDisplayPrompt = sccp_device_trueResult;
			d->dndmode = SCCP_DNDMODE_REJECT;

			for (i = 0; i < 10; i++) {
				btn[btn_index++].type = SCCP_BUTTONTYPE_MULTI;
			}
			btn[btn_index++].type = SKINNY_BUTTONTYPE_CONFERENCE;
			btn[btn_index++].type = SKINNY_BUTTONTYPE_HOLD;
			btn[btn_index++].type = SKINNY_BUTTONTYPE_TRANSFER;
			btn[btn_index++].type = SKINNY_BUTTONTYPE_LASTNUMBERREDIAL;
			break;

		case SKINNY_DEVICETYPE_SPA_502G:
		case SKINNY_DEVICETYPE_SPA_512G:
		case SKINNY_DEVICETYPE_SPA_521S:
			btn[btn_index++].type = SCCP_BUTTONTYPE_MULTI;
			break;
		case SKINNY_DEVICETYPE_SPA_303G:
			for (i = 0; i < 3; i++) {
				btn[btn_index++].type = SCCP_BUTTONTYPE_MULTI;
			}
			break;
		case SKINNY_DEVICETYPE_SPA_504G:
		case SKINNY_DEVICETYPE_SPA_514G:
		case SKINNY_DEVICETYPE_SPA_524SG:
			for (i = 0; i < 4; i++) {
				btn[btn_index++].type = SCCP_BUTTONTYPE_MULTI;
			}
			break;
		case SKINNY_DEVICETYPE_SPA_508G:
			for (i = 0; i < 8; i++) {
				btn[btn_index++].type = SCCP_BUTTONTYPE_MULTI;
			}
			btn[btn_index++].type = SKINNY_BUTTONTYPE_VOICEMAIL;
			btn[btn_index++].type = SKINNY_BUTTONTYPE_HOLD;
			break;
		case SKINNY_DEVICETYPE_SPA_509G:
			for (i = 0; i < 12; i++) {
				btn[btn_index++].type = SCCP_BUTTONTYPE_MULTI;
			}
			btn[btn_index++].type = SKINNY_BUTTONTYPE_VOICEMAIL;
			btn[btn_index++].type = SKINNY_BUTTONTYPE_HOLD;
			break;
		case SKINNY_DEVICETYPE_SPA_525G2:
			for (i = 0; i < 8; i++) {
				btn[btn_index++].type = SCCP_BUTTONTYPE_MULTI;
			}
			break;
		case SKINNY_DEVICETYPE_CISCO6901:
			d->useHookFlash = sccp_device_trueResult;
			d->hasLabelLimitedDisplayPrompt = sccp_device_trueResult;
			btn[btn_index++].type = SCCP_BUTTONTYPE_MULTI;
			break;
		case SKINNY_DEVICETYPE_CISCO6911:
			d->hasDisplayPrompt = sccp_device_falseResult;
			btn[btn_index++].type = SCCP_BUTTONTYPE_MULTI;
			break;
		case SKINNY_DEVICETYPE_CISCO6921:
			d->hasLabelLimitedDisplayPrompt = sccp_device_trueResult;
			for (i = 0; i < 2; i++) {
				btn[btn_index++].type = SCCP_BUTTONTYPE_MULTI;
			}
			for (i = 0; i < 6; i++) {
				btn[btn_index++].type = SCCP_BUTTONTYPE_SPEEDDIAL;
			}
			break;
		case SKINNY_DEVICETYPE_CISCO6941:
		case SKINNY_DEVICETYPE_CISCO6945:
			for (i = 0; i < 4; i++) {
				btn[btn_index++].type = SCCP_BUTTONTYPE_MULTI;
			}
			d->hasLabelLimitedDisplayPrompt = sccp_device_trueResult;
			break;
		case SKINNY_DEVICETYPE_CISCO6961:
			for (i = 0; i < 12; i++) {
				btn[btn_index++].type = SCCP_BUTTONTYPE_MULTI;
			}
			d->hasLabelLimitedDisplayPrompt = sccp_device_trueResult;
			break;
		default:
			pbx_log(LOG_WARNING, "%s: device type %d has no button layout; using a single line button\n", d->id, d->skinny_type);
			/* at least one line */
			btn[btn_index++].type = SCCP_BUTTONTYPE_LINE;
			break;
	}
	for (i = 0; i < btn_index; i++) {
		btn[i].devicetype = type;
	}
	sccp_log(DEBUGCAT_DEVICE)(VERBOSE_PREFIX_3 "%s: %d device buttons\n", d->id, btn_index);

	sccp_addon_t *cur = NULL;
	SCCP_LIST_LOCK(&d->addons);
	SCCP_LIST_TRAVERSE(&d->addons, cur, list) {
		btn_index = sccp_addon_build_buttontemplate(d, cur, btn, btn_index);
	}
	SCCP_LIST_UNLOCK(&d->addons);

	if (d->skinny_type < 6 || sccp_strcaseequals(d->config_type, "kirk")) {
		d->hasDisplayPrompt = sccp_device_falseResult;
		d->hasMWILight = sccp_device_falseResult;
	}

	for (i = btn_index; i< StationMaxButtonTemplateSize; i++) {
		btn[i].type = SCCP_BUTTONTYPE_ABBRDIAL;
		btn[i].devicetype = type;
	}
	sccp_log(DEBUGCAT_DEVICE)(VERBOSE_PREFIX_3 "%s: %d abbreviated dial buttons\n", d->id, StationMaxButtonTemplateSize - btn_index);

	return btn_index;
}

int sccp_dev_send(constDevicePtr d, sccp_msg_t * msg)
{
	int result = -1;

	if (d && msg) {
		sccp_log((DEBUGCAT_MESSAGE))(VERBOSE_PREFIX_3 "%s: sending %s\n", d->id, msginfo2str(letohl(msg->header.lel_messageId)));
		result = sccp_session_send(d, msg);
	} else {
		sccp_free(msg);
	}
	return result;
}

void sccp_dev_sendmsg(constDevicePtr d, sccp_mid_t t)
{
	if (d) {
		sccp_session_sendmsg(d, t);
	}
}

/* adds a retained device to the event.deviceRegistered.device */
void sccp_dev_set_registered(devicePtr d, skinny_registrationstate_t state)
{
	sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: registration state %s changed to %s\n", DEV_ID_LOG(d), skinny_registrationstate2str(sccp_device_getRegistrationState(d)), skinny_registrationstate2str(state));

	if (!sccp_device_setRegistrationState(d, state)) {
		return;
	}

	if (state == SKINNY_DEVICE_RS_OK) {
		if (!d->linesRegistered) {
			sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: model does not send RegisterAvailableLinesMessage; handling it as if it did\n", DEV_ID_LOG(d));
			sccp_handle_AvailableLines(d->session, d, NULL);
		}
		sccp_dev_postregistration(d);
	} else if (state == SKINNY_DEVICE_RS_PROGRESS) {
		sccp_event_t *event = sccp_event_allocate(SCCP_EVENT_DEVICE_PREREGISTERED);
		if (event) {
			event->deviceRegistered.device = sccp_device_retain(d);
			sccp_event_fire(event);
		}
	}
	d->registrationTime = time(0);
}

void sccp_dev_set_keyset(constDevicePtr d, uint8_t lineInstance, uint32_t callid, skinny_keymode_t softKeySetIndex)
{
	sccp_msg_t *msg = NULL;

	if (!d) {
		return;
	}
	if (!d->softkeysupport) {
		return;
	}
	/* 69XX Exception SoftKeySet Mapping */
	if (d->skinny_type == SKINNY_DEVICETYPE_CISCO6901 || d->skinny_type == SKINNY_DEVICETYPE_CISCO6911 || d->skinny_type == SKINNY_DEVICETYPE_CISCO6921 || d->skinny_type == SKINNY_DEVICETYPE_CISCO6941 || d->skinny_type == SKINNY_DEVICETYPE_CISCO6945 || d->skinny_type == SKINNY_DEVICETYPE_CISCO6961) {
		/* 69XX does not like CONNCONF, so let's not set that and keep CONNECTED instead */
		/* while transfer in progress, they like OFFHOOKFEAT, after when we have connected to the destination we need to set CONNTRANS */
		if (d->transfer && d->transferChannels.transferee) {
			if (softKeySetIndex == KEYMODE_OFFHOOK && !d->transferChannels.transferer) {
				softKeySetIndex = KEYMODE_OFFHOOKFEAT;
			}
			if ((softKeySetIndex == KEYMODE_RINGOUT || softKeySetIndex == KEYMODE_CONNECTED) && d->transferChannels.transferer) {
				softKeySetIndex = KEYMODE_CONNTRANS;
			}
		}
	} else {
		if (softKeySetIndex == KEYMODE_CONNECTED) {
			softKeySetIndex = (
#if CS_SCCP_CONFERENCE
						(d->conference) ? KEYMODE_CONNCONF :
#endif
						(d->transfer) ? KEYMODE_CONNTRANS : KEYMODE_CONNECTED
					  );
		}
	}
	REQ(msg, SelectSoftKeysMessage);
	if (!msg) {
		pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
		return;
	}
	msg->data.SelectSoftKeysMessage.lel_lineInstance = htolel(lineInstance);
	msg->data.SelectSoftKeysMessage.lel_callReference = htolel(callid);
	msg->data.SelectSoftKeysMessage.lel_softKeySetIndex = htolel(softKeySetIndex);

	if (softKeySetIndex == KEYMODE_ONHOOK || softKeySetIndex == KEYMODE_OFFHOOK || softKeySetIndex == KEYMODE_OFFHOOKFEAT) {
		sccp_softkey_setSoftkeyState((sccp_device_t *) d, softKeySetIndex, SKINNY_LBL_REDIAL, (sccp_strlen_zero(d->redialInformation.number) && !d->useRedialMenu) ? FALSE : TRUE);
	}
#if CS_SCCP_CONFERENCE
	if (d->allow_conference) {
		if (d->conference) {
			sccp_softkey_setSoftkeyState((sccp_device_t *) d, softKeySetIndex, SKINNY_LBL_CONFRN, FALSE);
			sccp_softkey_setSoftkeyState((sccp_device_t *) d, softKeySetIndex, SKINNY_LBL_JOIN, TRUE);
		} else {
			sccp_softkey_setSoftkeyState((sccp_device_t *) d, softKeySetIndex, SKINNY_LBL_CONFRN, TRUE);
			sccp_softkey_setSoftkeyState((sccp_device_t *) d, softKeySetIndex, SKINNY_LBL_JOIN, FALSE);
		}
		sccp_softkey_setSoftkeyState((sccp_device_t *) d, softKeySetIndex, SKINNY_LBL_CONFLIST, TRUE);
	} else {
		sccp_softkey_setSoftkeyState((sccp_device_t *) d, softKeySetIndex, SKINNY_LBL_CONFRN, FALSE);
		sccp_softkey_setSoftkeyState((sccp_device_t *) d, softKeySetIndex, SKINNY_LBL_CONFLIST, FALSE);
		sccp_softkey_setSoftkeyState((sccp_device_t *) d, softKeySetIndex, SKINNY_LBL_JOIN, FALSE);
	}
#endif

	if (softKeySetIndex != KEYMODE_CONNTRANS && softKeySetIndex != KEYMODE_CONNECTED && softKeySetIndex != KEYMODE_EMPTY) {
		sccp_softkey_setSoftkeyState((sccp_device_t *) d, softKeySetIndex, SKINNY_LBL_MONITOR, FALSE);
	}
	if (softKeySetIndex == KEYMODE_RINGOUT) {
		if (d->transfer && d->transferChannels.transferer) {
			sccp_softkey_setSoftkeyState((sccp_device_t *) d, softKeySetIndex, SKINNY_LBL_TRANSFER, TRUE);
		} else  {
			sccp_softkey_setSoftkeyState((sccp_device_t *) d, softKeySetIndex, SKINNY_LBL_TRANSFER, FALSE);
		}
	}
	msg->data.SelectSoftKeysMessage.les_validKeyMask = htolel(d->softKeyConfiguration.activeMask[softKeySetIndex]);

	sccp_log((DEBUGCAT_SOFTKEY + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: softkey set %s (%d) on line %d, call %d\n", d->id, skinny_keymode2str(softKeySetIndex), softKeySetIndex, lineInstance, callid);
	sccp_log((DEBUGCAT_SOFTKEY + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: valid key mask %u\n", d->id, msg->data.SelectSoftKeysMessage.les_validKeyMask);
	sccp_dev_send(d, msg);
}

void sccp_dev_set_ringer(constDevicePtr d, skinny_ringtype_t ringtype, skinny_ringduration_t duration, uint8_t lineInstance, uint32_t callid)
{
	sccp_msg_t *msg = NULL;

	REQ(msg, SetRingerMessage);
	if (!msg) {
		pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
		return;
	}
	msg->data.SetRingerMessage.lel_ringMode = htolel(ringtype);
	/*
	 * Note that for distinctive ringing to work with the higher protocol versions the following actually needs to be set to 1 as the original comment says.
	 */
	msg->data.SetRingerMessage.lel_ringDuration = htolel(duration);
	msg->data.SetRingerMessage.lel_lineInstance = htolel(lineInstance);
	msg->data.SetRingerMessage.lel_callReference = htolel(callid);
	sccp_dev_send(d, msg);
	sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: ringer mode %s (%d)\n", DEV_ID_LOG(d), skinny_ringtype2str(ringtype), ringtype);
}

void sccp_dev_set_speaker(constDevicePtr d, uint8_t mode)
{
	sccp_msg_t *msg = NULL;

	if (!d || !d->session) {
		return;
	}
	REQ(msg, SetSpeakerModeMessage);
	if (!msg) {
		pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
		return;
	}
	msg->data.SetSpeakerModeMessage.lel_speakerMode = htolel(mode);
	sccp_dev_send(d, msg);
	sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: speaker %s\n", d->id, (mode == SKINNY_STATIONSPEAKER_ON ? "on" : (mode == SKINNY_STATIONSPEAKER_OFF ? "off" : "unknown")));
}

static void sccp_dev_setHookFlashDetect(constDevicePtr d)
{
	sccp_msg_t *msg = NULL;

	if (!d || !d->session || !d->protocol || !d->useHookFlash()) {
		return;												/* only for old phones */
	}
	REQ(msg, SetHookFlashDetectMessage);
	if (!msg) {
		pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
		return;
	}
	sccp_dev_send(d, msg);
	sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: hook flash detection enabled\n", d->id);
}

void sccp_dev_set_microphone(devicePtr d, uint8_t mode)
{
	sccp_msg_t *msg = NULL;

	if (!d || !d->session) {
		return;
	}
	REQ(msg, SetMicroModeMessage);
	if (!msg) {
		pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
		return;
	}
	msg->data.SetMicroModeMessage.lel_micMode = htolel(mode);
	sccp_dev_send(d, msg);
	sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: microphone %s\n", d->id, (mode == SKINNY_STATIONMIC_ON ? "on" : (mode == SKINNY_STATIONMIC_OFF ? "off" : "unknown")));
}

void sccp_dev_set_cplane(constDevicePtr device, uint8_t lineInstance, int status)
{
	sccp_msg_t *msg = NULL;

	if (!device) {
		return;
	}
	REQ(msg, ActivateCallPlaneMessage);
	if (!msg) {
		pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
		return;
	}
	if (status) {
		msg->data.ActivateCallPlaneMessage.lel_lineInstance = htolel(lineInstance);
	}
	sccp_dev_send(device, msg);

	sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: activating call plane on line %d\n", device->id, (status) ? lineInstance : 0);
}

void sccp_dev_deactivate_cplane(constDevicePtr d)
{
	if (!d) {
		sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "call plane not deactivated: no device\n");
		return;
	}

	sccp_dev_sendmsg(d, DeactivateCallPlaneMessage);
	sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: deactivating call plane\n", d->id);
}

void sccp_dev_starttone(constDevicePtr d, skinny_tone_t tone, uint8_t lineInstance, uint32_t callid, skinny_toneDirection_t direction)
{
	sccp_msg_t *msg = NULL;

	if (!d) {
		sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "tone not started: no device\n");
		return;
	}

	REQ(msg, StartToneMessage);
	if (!msg) {
		pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
		return;
	}
	msg->data.StartToneMessage.lel_tone = htolel(tone);
	msg->data.StartToneMessage.lel_toneDirection = htolel(direction);
	msg->data.StartToneMessage.lel_lineInstance = htolel(lineInstance);
	msg->data.StartToneMessage.lel_callReference = htolel(callid);

	sccp_dev_send(d, msg);
	sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: starting tone %s (%d) on line %d, call %d (%s)\n", d->id, skinny_tone2str(tone), tone, lineInstance, callid, skinny_toneDirection2str(direction));
}

void sccp_dev_stoptone(constDevicePtr d, uint8_t lineInstance, uint32_t callid)
{
	sccp_msg_t *msg = NULL;

	if (!d || !d->session) {
		return;
	}
	REQ(msg, StopToneMessage);
	if (!msg) {
		pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
		return;
	}
	msg->data.StopToneMessage.lel_lineInstance = htolel(lineInstance);
	msg->data.StopToneMessage.lel_callReference = htolel(callid);
	if (d->protocolversion >= 11) {
		msg->data.StopToneMessage.lel_tone = htolel(SKINNY_TONE_SILENCE);
	}
	sccp_dev_send(d, msg);
	sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: stopping tone on line %d, call %d\n", d->id, lineInstance, callid);
}

void sccp_dev_set_message(devicePtr d, const char *msg, const int timeout, const boolean_t storedb, const boolean_t beep)
{
	if (storedb) {
		char msgtimeout[10];

		snprintf(msgtimeout, sizeof(msgtimeout), "%d", timeout);
		iPbx.feature_addToDatabase("SCCP/message", "timeout", msgtimeout);
		iPbx.feature_addToDatabase("SCCP/message", "text", msg);
	}

	if (timeout) {
		if (d->skinny_type == SKINNY_DEVICETYPE_CISCO6901 || d->skinny_type == SKINNY_DEVICETYPE_CISCO6921 || d->skinny_type == SKINNY_DEVICETYPE_CISCO6941 || d->skinny_type == SKINNY_DEVICETYPE_CISCO6945 || d->skinny_type == SKINNY_DEVICETYPE_CISCO6961) {
			sccp_dev_displayprompt(d, 0, 0, msg, timeout);
		} else {
			sccp_dev_displayprinotify(d, msg, SCCP_MESSAGE_PRIORITY_TIMEOUT, timeout);
		}
	} else {
		sccp_device_addMessageToStack(d, SCCP_MESSAGE_PRIORITY_IDLE, msg);
	}
	if (beep) {
		sccp_dev_starttone(d, SKINNY_TONE_ZIPZIP, 0, 0, SKINNY_TONEDIRECTION_USER);
	}
}

void sccp_dev_clear_message(devicePtr d, const boolean_t cleardb)
{
	if (cleardb) {
		iPbx.feature_removeTreeFromDatabase("SCCP/message", "timeout");
		iPbx.feature_removeTreeFromDatabase("SCCP/message", "text");
	}

	sccp_device_clearMessageFromStack(d, SCCP_MESSAGE_PRIORITY_IDLE);
	if (d->skinny_type == SKINNY_DEVICETYPE_CISCO6901 || d->skinny_type == SKINNY_DEVICETYPE_CISCO6921 || d->skinny_type == SKINNY_DEVICETYPE_CISCO6941 || d->skinny_type == SKINNY_DEVICETYPE_CISCO6945 || d->skinny_type == SKINNY_DEVICETYPE_CISCO6961) {
		sccp_dev_clearprompt(d, 0, 0);
	} else {
		sccp_dev_cleardisplayprinotify(d, SCCP_MESSAGE_PRIORITY_TIMEOUT);
	}
}

static const char * __attribute__((unused)) sccp_dev_prompt2str(const char * msg, char * buf, size_t size)
{
	size_t len = 0;
	buf[0]     = '\0';
	for (const unsigned char * p = (const unsigned char *)msg; p && *p && len + 1 < size; p++) {
		int n = 0;
		if (*p == 0x80 && p[1]) {
			n = snprintf(buf + len, size - len, "[%s]", label2str(*++p));
		} else if (*p >= 0x20 && *p < 0x7f) {
			n = snprintf(buf + len, size - len, "%c", *p);
		} else {
			n = snprintf(buf + len, size - len, "\\x%02x", *p);
		}
		if (n < 0 || (size_t)n >= size - len) {
			break;
		}
		len += n;
	}
	return buf;
}

void sccp_dev_clearprompt(constDevicePtr d, const uint8_t lineInstance, const uint32_t callid)
{
	sccp_msg_t *msg = NULL;

	if (!d || !d->session || !d->protocol || (!d->hasDisplayPrompt() && !d->hasLabelLimitedDisplayPrompt())) {
		return;												/* only for telecaster and new phones */
	}
	REQ(msg, ClearPromptStatusMessage);
	if (!msg) {
		pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
		return;
	}
	msg->data.ClearPromptStatusMessage.lel_callReference = htolel(callid);
	msg->data.ClearPromptStatusMessage.lel_lineInstance = htolel(lineInstance);
	sccp_dev_send(d, msg);
	sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: clearing prompt on line %d, call %d\n", d->id, lineInstance, callid);
}

void sccp_dev_displayprompt_debug(constDevicePtr d, const uint8_t lineInstance, const uint32_t callid, const char *msg, const int timeout, const char *file, int lineno, const char *pretty_function)
{
#if DEBUG
	char promptbuf[128];
	sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: from %s:%d (%s): prompt '%s' on line %d, timeout %d\n", DEV_ID_LOG(d), file, lineno, pretty_function, sccp_dev_prompt2str(msg, promptbuf, sizeof(promptbuf)), lineInstance, timeout);
#endif
	if (!d || !d->session || !d->protocol || (!d->hasDisplayPrompt() && !d->hasLabelLimitedDisplayPrompt())) {
		return;
	}
	d->protocol->displayPrompt(d, lineInstance, callid, timeout, msg);
}

void sccp_dev_cleardisplay(constDevicePtr d)
{
}

void sccp_dev_cleardisplaynotify(constDevicePtr d)
{
	if (!d || !d->session || !d->protocol || (!d->hasDisplayPrompt() && !d->hasLabelLimitedDisplayPrompt())) {
		return;												/* only for telecaster and new phones */
	}
	sccp_dev_sendmsg(d, ClearNotifyMessage);
	sccp_log((DEBUGCAT_DEVICE + DEBUGCAT_MESSAGE)) (VERBOSE_PREFIX_3 "%s: clearing notify message\n", d->id);
}

void sccp_dev_displaynotify_debug(constDevicePtr d, const char *msg, uint8_t timeout, const char *file, const int lineno, const char *pretty_function)
{
	char promptbuf[128];
	sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: from %s:%d (%s): notify '%s', timeout %d\n", DEV_ID_LOG(d), file, lineno, pretty_function, sccp_dev_prompt2str(msg, promptbuf, sizeof(promptbuf)), timeout);
	if (!d || !d->session || !d->protocol || (!d->hasDisplayPrompt() && !d->hasLabelLimitedDisplayPrompt())) {
		return;
	}
	if (!msg || sccp_strlen_zero(msg)) {
		return;
	}
	d->protocol->displayNotify(d, timeout, msg);
	sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: notify with timeout %d\n", d->id, timeout);
}

void sccp_dev_cleardisplayprinotify(constDevicePtr d, const uint8_t priority)
{
	sccp_msg_t *msg = NULL;
	if (!d || !d->session || !d->protocol || (!d->hasDisplayPrompt() && !d->hasLabelLimitedDisplayPrompt())) {
		return;
	}
	REQ(msg, ClearPriNotifyMessage);
	if (!msg) {
		pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
		return;
	}
	msg->data.ClearPriNotifyMessage.lel_priority = htolel(priority);

	sccp_dev_send(d, msg);
	sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: clearing priority notify message\n", d->id);
}

void sccp_dev_displayprinotify_debug(constDevicePtr d, const char *msg, const sccp_message_priority_t priority, const uint8_t timeout, const char *file, const int lineno, const char *pretty_function)
{
	if (!d || !d->session || !d->protocol || (!d->hasDisplayPrompt() && !d->hasLabelLimitedDisplayPrompt())) {
		return;
	}
	if (!msg || sccp_strlen_zero(msg)) {
		sccp_dev_cleardisplayprinotify(d, priority);
		return;
	}
	d->protocol->displayPriNotify(d, priority, timeout, msg);
	sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: priority notify with timeout %d, priority %d\n", d->id, timeout, priority);
}

void sccp_dev_speed_find_byindex(constDevicePtr d, const uint16_t instance, boolean_t withHint, sccp_speed_t * const k)
{
	sccp_buttonconfig_t *config  = NULL;

	if (!d || !d->session || instance == 0) {
		return;
	}
	memset(k, 0, sizeof(sccp_speed_t));
	sccp_copy_string(k->name, "unknown speeddial", sizeof(k->name));

	SCCP_LIST_LOCK(&(((devicePtr)d)->buttonconfig));
	SCCP_LIST_TRAVERSE(&d->buttonconfig, config, list) {
		if (config->type == SPEEDDIAL && config->instance == instance) {
			if (TRUE == withHint && !sccp_strlen_zero(config->button.speeddial.hint)) {
				k->valid = TRUE;
				k->instance = instance;
				k->type = SCCP_BUTTONTYPE_SPEEDDIAL;
				sccp_copy_string(k->name, config->label, sizeof(k->name));
				sccp_copy_string(k->ext, config->button.speeddial.ext, sizeof(k->ext));
				sccp_copy_string(k->hint, config->button.speeddial.hint, sizeof(k->hint));
			} else if(FALSE == withHint && sccp_strlen_zero(config->button.speeddial.hint)) {
				k->valid = TRUE;
				k->instance = instance;
				k->type = SCCP_BUTTONTYPE_SPEEDDIAL;
				sccp_copy_string(k->name, config->label, sizeof(k->name));
				sccp_copy_string(k->ext, config->button.speeddial.ext, sizeof(k->ext));
			}
		}
	}
	SCCP_LIST_UNLOCK(&(((devicePtr)d)->buttonconfig));
}

linePtr sccp_dev_getActiveLine(constDevicePtr device)
{
	sccp_buttonconfig_t * buttonconfig = NULL;

	if (!device || !device->session) {
		return NULL;
	}
	if (device->currentLine) {
		sccp_log((DEBUGCAT_DEVICE + DEBUGCAT_LINE)) (VERBOSE_PREFIX_3 "%s: active line is %s\n", device->id, device->currentLine->name);
		return sccp_line_retain(device->currentLine);
	}

	devicePtr d = (sccp_device_t * const) device;
	SCCP_LIST_TRAVERSE(&device->buttonconfig, buttonconfig, list) {
		if (buttonconfig->type == LINE && !d->currentLine) {
			if ((d->currentLine = sccp_line_find_byname(buttonconfig->button.line.name, FALSE))) {	// update device->currentLine, returns retained line
				sccp_log((DEBUGCAT_DEVICE + DEBUGCAT_LINE)) (VERBOSE_PREFIX_3 "%s: no active line; using %s\n", d->id, d->currentLine->name);
				return sccp_line_retain(d->currentLine);
			}
		}
	}

	sccp_log((DEBUGCAT_DEVICE + DEBUGCAT_LINE)) (VERBOSE_PREFIX_3 "%s: device has no lines\n", device->id);
	return NULL;
}

void __sccp_dev_setActiveLine(devicePtr device, constLinePtr l, const char *file, uint32_t line, const char *func)
{
	if (!device || !device->session) {
		return;
	}
	sccp_line_refreplace(&device->currentLine, l);

	sccp_log((DEBUGCAT_DEVICE + DEBUGCAT_LINE)) (VERBOSE_PREFIX_3 "%s: active line set to %s\n", device->id, l ? l->name : "(none)");
}

channelPtr sccp_device_getActiveChannel(constDevicePtr device)
{
	sccp_channel_t *channel = NULL;

	if (!device) {
		return NULL;
	}

	sccp_log((DEBUGCAT_CHANNEL + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: looking up the active call\n", device->id);

 	if (device->active_channel && (channel = sccp_channel_retain(device->active_channel))) {
		if (channel && channel->state == SCCP_CHANNELSTATE_DOWN) {
			sccp_log((DEBUGCAT_CHANNEL + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: active call %s is down; none returned\n", device->id, channel->designator);
			sccp_channel_release(&channel);						/* explicit release, when not returning channel because it's DOWN */
		}
	} else {
		sccp_log((DEBUGCAT_CHANNEL + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: no active call\n", device->id);
	}

	return channel;
}

void __sccp_device_setActiveChannel(constDevicePtr d, constChannelPtr channel, const char *file, uint32_t line, const char *func)
{
	AUTO_RELEASE(sccp_device_t, device , sccp_device_retain(d));

	if(device && device->active_channel != channel) {
		sccp_log((DEBUGCAT_CHANNEL + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: active call set to %d\n", DEV_ID_LOG(d), (channel) ? channel->callid : 0);
		if (device->active_channel && device->active_channel->line) {
			device->active_channel->line->statistic.numberOfActiveChannels--;
		}
		if (!channel) {
			sccp_dev_setActiveLine(device, NULL);
		}
		sccp_channel_refreplace(&device->active_channel, channel);
		if (device->active_channel) {
			sccp_dev_setActiveLine(device, device->active_channel->line);
			if (device->active_channel->line) {
				device->active_channel->line->statistic.numberOfActiveChannels++;
			}
		}
	}
}

void sccp_dev_check_displayprompt(constDevicePtr d)
{
	if (!d || !d->session || !d->protocol || (!d->hasDisplayPrompt() && !d->hasLabelLimitedDisplayPrompt())) {
		return;
	}
	boolean_t message_set = FALSE;

	sccp_dev_clearprompt(d, 0, 0);
#ifndef SCCP_ATOMIC
	devicePtr device = (devicePtr) d;
	sccp_mutex_lock(&device->messageStack.lock);
#endif
	for(int i = SCCP_MESSAGE_PRIORITY_SENTINEL - 1; i >= 0; i--) {
		if (d->messageStack.messages[i] != NULL && !sccp_strlen_zero(d->messageStack.messages[i])) {
			sccp_dev_displayprompt(d, 0, 0, d->messageStack.messages[i], 0);
			message_set = TRUE;
			break;
		}
	}
#ifndef SCCP_ATOMIC
	sccp_mutex_unlock(&device->messageStack.lock);
#endif
	if (!message_set) {
		sccp_dev_displayprompt(d, 0, 0, SKINNY_DISP_YOUR_CURRENT_OPTIONS, 0);
		sccp_dev_set_keyset(d, 0, 0, KEYMODE_ONHOOK);
	}
	sccp_log_and((DEBUGCAT_DEVICE + DEBUGCAT_HIGH)) (VERBOSE_PREFIX_3 "%s: prompt finished\n", d->id);
}

void sccp_dev_forward_status(constLinePtr l, uint8_t lineInstance, constDevicePtr device)
{
#ifndef ASTDB_FAMILY_KEY_LEN
#define ASTDB_FAMILY_KEY_LEN 100
#endif
#ifndef ASTDB_RESULT_LEN
#define ASTDB_RESULT_LEN 80
#endif
	if (!l || !device || !device->session) {
		return;
	}
	sccp_log((DEBUGCAT_DEVICE + DEBUGCAT_LINE)) (VERBOSE_PREFIX_3 "%s: sending forward status for line %s\n", device->id, l->name);

	if (sccp_device_getRegistrationState(device) != SKINNY_DEVICE_RS_OK) {
		if (!device->linesRegistered) {
			AUTO_RELEASE(sccp_device_t, d , sccp_device_retain(device));
			if (d) {
				sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: model does not send RegisterAvailableLinesMessage; handling it as if it did\n", DEV_ID_LOG(device));
				sccp_handle_AvailableLines(d->session, d, NULL);
				d->linesRegistered = TRUE;
			}
		}
	}

	AUTO_RELEASE(sccp_linedevice_t, ld, sccp_linedevice_find(device, l));
	if(ld) {
		device->protocol->sendCallForwardStatus(device, ld);
		char buffer[256];
		sccp_log((DEBUGCAT_DEVICE + DEBUGCAT_LINE))(VERBOSE_PREFIX_3 "%s: forward status sent (%s) for line %s (%d)\n", device->id, sccp_linedevice_get_cfwd_string(ld, buffer, sizeof(buffer)), l->name,
							    ld->lineInstance);
	} else {
		pbx_log(LOG_NOTICE, "%s: forward status not sent: line %s is not on this device\n", DEV_ID_LOG(device), l->name);
	}
}

/* adds a retained device to the event.deviceRegistered.device */
void sccp_dev_postregistration(devicePtr d)
{
#ifndef ASTDB_FAMILY_KEY_LEN
#define ASTDB_FAMILY_KEY_LEN 100
#endif
#ifndef ASTDB_RESULT_LEN
#define ASTDB_RESULT_LEN 256
#endif
	char family[ASTDB_FAMILY_KEY_LEN] = { 0 };
	char buffer[ASTDB_RESULT_LEN] = { 0 };
	int instance = 0;

	if (!d) {
		return;
	}
	sccp_log((DEBUGCAT_DEVICE + DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: registered; running post-registration tasks\n", d->id);

	sccp_event_t *event = sccp_event_allocate(SCCP_EVENT_DEVICE_REGISTERED);
	if (event) {
		event->deviceRegistered.device = sccp_device_retain(d);
		sccp_event_fire(event);
	}

	if (iPbx.feature_getFromDatabase) {
		sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: reading saved settings from the Asterisk database\n", d->id);
		for (instance = SCCP_FIRST_LINEINSTANCE; instance < d->lineButtons.size; instance++) {
			if (d->lineButtons.instance[instance]) {
				AUTO_RELEASE(sccp_linedevice_t, ld, sccp_linedevice_retain(d->lineButtons.instance[instance]));
				snprintf(family, sizeof(family), "SCCP/%s/%s", d->id, ld->line->name);
				for(uint x = SCCP_CFWD_ALL; x < SCCP_CFWD_SENTINEL; x++) {
					char cfwdstr[15] = "";
					snprintf(cfwdstr, 14, "cfwd%s", sccp_cfwd2str((sccp_cfwd_t)x));
					if(iPbx.feature_getFromDatabase(family, cfwdstr, buffer, sizeof(buffer)) && strcmp(buffer, "") != 0) {
						ld->cfwd[x].enabled = TRUE;
						sccp_copy_string(ld->cfwd[x].number, buffer, sizeof(ld->cfwd[x].number));
						sccp_feat_changed(d, ld, sccp_cfwd2feature((sccp_cfwd_t)x));
					}
				}
			}
		}

		if (iPbx.feature_getFromDatabase("SCCP/message", "text", buffer, sizeof(buffer))) {
			char timebuffer[ASTDB_RESULT_LEN];
			int timeout = 0;
			if (!sccp_strlen_zero(buffer)) {
				if (iPbx.feature_getFromDatabase("SCCP/message", "timeout", timebuffer, sizeof(timebuffer))) {
					sscanf(timebuffer, "%i", &timeout);
				}
				sccp_dev_set_message(d, buffer, timeout, FALSE, FALSE);
			}
		}

		snprintf(family, sizeof(family), "SCCP/%s", d->id);
		if(iPbx.feature_getFromDatabase(family, "dnd", buffer, sizeof(buffer)) && strcmp(buffer, "") != 0) {
			d->dndFeature.status = sccp_dndmode_str2val(buffer);
			sccp_feat_changed(d, NULL, SCCP_FEATURE_DND);
		}

		if(iPbx.feature_getFromDatabase(family, "privacy", buffer, sizeof(buffer)) && strcmp(buffer, "") != 0) {
			sscanf(buffer,"%d", &d->privacyFeature.status);
			sccp_feat_changed(d, NULL, SCCP_FEATURE_PRIVACY);
		}

		if(iPbx.feature_getFromDatabase(family, "monitor", buffer, sizeof(buffer)) && strcmp(buffer, "") != 0) {
			sccp_feat_monitor(d, NULL, 0, NULL);
			sccp_feat_changed(d, NULL, SCCP_FEATURE_MONITOR);
		}

		char lastNumber[SCCP_MAX_EXTENSION] = "";
		if (iPbx.feature_getFromDatabase(family, "lastDialedNumber", buffer, sizeof(buffer))) {
			sscanf(buffer,"%79[^;];lineInstance=%d", lastNumber, &instance);
			AUTO_RELEASE(sccp_linedevice_t, ld, sccp_linedevice_findByLineinstance(d, instance));
			if(ld) {
				sccp_device_setLastNumberDialed(d, lastNumber, ld);
			}
		}
	}
	if (d->backgroundImage && !sccp_strlen_zero(d->backgroundImage)) {
		d->setBackgroundImage(d, d->backgroundImage, d->backgroundTN ? d->backgroundTN : d->backgroundImage);
	}

	if (d->ringtone && !sccp_strlen_zero(d->ringtone)) {
		d->setRingTone(d, d->ringtone);
	}

	if (d->useRedialMenu && (!d->hasDisplayPrompt() && !d->hasLabelLimitedDisplayPrompt())) {
		pbx_log(LOG_NOTICE, "%s: useRedialMenu is on, but this phone has no display for it; using direct redial\n", d->id);
		d->useRedialMenu = FALSE;
	}

	for (instance = SCCP_FIRST_LINEINSTANCE; instance < d->lineButtons.size; instance++) {
		if (d->lineButtons.instance[instance]) {
			AUTO_RELEASE(sccp_linedevice_t, ld, sccp_linedevice_retain(d->lineButtons.instance[instance]));
			if(ld) {
				sccp_linedevice_indicateMWI(ld);
			}
		}
	}
	sccp_device_setMWI(d);
	sccp_dev_check_displayprompt(d);

#ifdef CS_SCCP_PARK
	sccp_buttonconfig_t *config = NULL;
	SCCP_LIST_LOCK(&d->buttonconfig);
	SCCP_LIST_TRAVERSE(&d->buttonconfig, config, list) {
		if (config->type == FEATURE && config->button.feature.id ==SCCP_FEATURE_PARKINGLOT) {
			if(iParkingLot.attachObserver && iParkingLot.attachObserver(d, config)) {
				iParkingLot.notifyDevice(d, config);
			}
		}
	}
	SCCP_LIST_UNLOCK(&d->buttonconfig);
#endif
	if (d->useHookFlash()) {
		sccp_dev_setHookFlashDetect(d);
	}
	sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: post-registration tasks done\n", d->id);
}

static void sccp_buttonconfig_destroy(sccp_buttonconfig_t *buttonconfig)
{
	if (!buttonconfig) {
		return;
	}
	sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "SCCP: destroying button %d, type %s (%d), delete pending %s, update pending %s\n",
		buttonconfig->index, sccp_config_buttontype2str(buttonconfig->type), buttonconfig->type, buttonconfig->pendingDelete ? "yes" : "no", buttonconfig->pendingUpdate ? "yes" : "no");
	if (buttonconfig->label) {
		sccp_free(buttonconfig->label);
	}
	switch(buttonconfig->type) {
		case LINE:
			if (buttonconfig->button.line.name) {
				sccp_free(buttonconfig->button.line.name);
			}
			if (buttonconfig->button.line.subscriptionId) {
				sccp_free(buttonconfig->button.line.subscriptionId);
			}
			if (buttonconfig->button.line.options) {
				sccp_free(buttonconfig->button.line.options);
			}
			break;
		case SPEEDDIAL:
			if (buttonconfig->button.speeddial.ext) {
				sccp_free(buttonconfig->button.speeddial.ext);
			}
			if (buttonconfig->button.speeddial.hint) {
				sccp_free(buttonconfig->button.speeddial.hint);
			}
			break;
		case SERVICE:
			if (buttonconfig->button.service.url) {
				sccp_free(buttonconfig->button.service.url);
			}
			break;
		case FEATURE:
			if (buttonconfig->button.feature.options) {
				sccp_free(buttonconfig->button.feature.options);
			}
			if(buttonconfig->button.feature.args) {
				sccp_free(buttonconfig->button.feature.args);
			}
			break;
		case EMPTY:
		case SCCP_CONFIG_BUTTONTYPE_SENTINEL:
			break;
	}
	sccp_free(buttonconfig);
	buttonconfig = NULL;
}

/* adds a retained device to the event.deviceRegistered.device */
void _sccp_dev_clean(devicePtr device, boolean_t remove_from_global, boolean_t restart_device)
{
	AUTO_RELEASE(sccp_device_t, d , sccp_device_retain(device));
	sccp_buttonconfig_t *config = NULL;
	sccp_selectedchannel_t *selectedChannel = NULL;
	sccp_channel_t *c = NULL;
	int i = 0;

	if(d) {
		sccp_log((DEBUGCAT_CORE + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_1 "SCCP: cleaning device %s (remove from list: %s, restart: %s)\n", d->id, remove_from_global ? "yes" : "no", restart_device ? "yes" : "no");
		sccp_device_setRegistrationState(d, SKINNY_DEVICE_RS_CLEANING);
		if (remove_from_global) {
			d->id[0] = 'X';
			d->id[1] = 'X';
			d->id[2] = 'X';
			sccp_device_removeFromGlobals(d);
		}

		d->linesRegistered = FALSE;
		__saveLastDialedNumberToDatabase(d);

		if (d->active_channel) {
			sccp_device_setActiveChannel(d, NULL);
		}

		if (d->currentLine) {
			sccp_dev_setActiveLine(d, NULL);
		}
		SCCP_LIST_LOCK(&d->buttonconfig);
		SCCP_LIST_TRAVERSE(&d->buttonconfig, config, list) {
			if (config->type == LINE) {
				sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_2 "%s: checking button %d, type %s (%d) for lines and calls\n",
					d->id, config->index, sccp_config_buttontype2str(config->type), config->type);
				AUTO_RELEASE(sccp_line_t, line , sccp_line_find_byname(config->button.line.name, FALSE));

				if (!line) {
					continue;
				}
				SCCP_LIST_LOCK(&line->channels);
				SCCP_LIST_TRAVERSE_BACKWARDS_SAFE_BEGIN(&line->channels, c, list) {
					AUTO_RELEASE(sccp_channel_t, channel, sccp_channel_retain(c));
					if (channel) {
						AUTO_RELEASE(sccp_device_t, tmpDevice, sccp_channel_getDevice(channel));
						if (tmpDevice && tmpDevice == d) {
							pbx_log(LOG_NOTICE, "%s: device is being cleaned up; ending its open call on line %s\n", d->id, line->name);
							sccp_channel_endcall(channel);
						}
					}
				}
				SCCP_LIST_TRAVERSE_BACKWARDS_SAFE_END;
				SCCP_LIST_UNLOCK(&line->channels);

				sccp_log((DEBUGCAT_CORE + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_2 "SCCP: removing line %s from device %s\n", line->name, d->id);
				sccp_linedevice_remove(d, line);
#ifdef CS_SCCP_PARK
			} else if (iParkingLot.detachObserver && config->type == FEATURE && config->button.feature.id ==SCCP_FEATURE_PARKINGLOT) {
				sccp_log((DEBUGCAT_DEVICE))(VERBOSE_PREFIX_2 "%s: checking button %d, type %s (%d) for observed parking lots\n", d->id, config->index,
							    sccp_config_buttontype2str(config->type), config->type);
				iParkingLot.detachObserver(d, config);
#endif
			}
		}
		SCCP_LIST_TRAVERSE_SAFE_BEGIN(&d->buttonconfig, config, list) {
			sccp_log_and((DEBUGCAT_DEVICE + DEBUGCAT_HIGH)) (VERBOSE_PREFIX_2 "%s: checking button for removal (index %d, type %s (%d), delete pending %s, update pending %s)\n",
				d->id, config->index, sccp_config_buttontype2str(config->type), config->type, config->pendingDelete ? "yes" : "no", config->pendingUpdate ? "yes" : "no");
			config->instance = 0;
			if (config->pendingDelete) {
				SCCP_LIST_REMOVE_CURRENT(list);
				sccp_buttonconfig_destroy(config);
			}
		}
		SCCP_LIST_TRAVERSE_SAFE_END;
		SCCP_LIST_UNLOCK(&d->buttonconfig);

		sccp_log((DEBUGCAT_CORE + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_2 "SCCP: unregistering device %s\n", d->id);

		sccp_event_t *event = sccp_event_allocate(SCCP_EVENT_DEVICE_UNREGISTERED);
		if (event) {
			event->deviceRegistered.device = sccp_device_retain(d);
			sccp_event_fire(event);
		}

		if (SCCP_NAT_AUTO == d->nat || SCCP_NAT_AUTO_OFF == d->nat || SCCP_NAT_AUTO_ON == d->nat) {
			d->nat = SCCP_NAT_AUTO;
		}

		memset(&d->configurationStatistic, 0, sizeof(d->configurationStatistic));

		d->status.token = SCCP_TOKEN_STATE_NOTOKEN;
		d->registrationTime = time(0);

		if (remove_from_global) {
			sccp_addons_clear(d);
		}

		SCCP_LIST_LOCK(&d->selectedChannels);
		while ((selectedChannel = SCCP_LIST_REMOVE_HEAD(&d->selectedChannels, list))) {
			sccp_channel_release(&selectedChannel->channel);
			sccp_free(selectedChannel);
		}
		SCCP_LIST_UNLOCK(&d->selectedChannels);

		/* release line references, refcounted in btnList */
		if (d->buttonTemplate) {
			btnlist *btn = d->buttonTemplate;

			for (i = 0; i < StationMaxButtonTemplateSize; i++) {
				if ((btn[i].type == SKINNY_BUTTONTYPE_LINE) && btn[i].ptr) {
					sccp_line_t * tmp = btn[i].ptr;
					sccp_line_release(&tmp);
					btn[i].ptr = NULL;
				}
			}
			sccp_free(d->buttonTemplate);
			d->buttonTemplate = NULL;
		}

		if (device->lineButtons.size) {
			sccp_linedevice_deleteButtonsArray(d);
		}
		sccp_session_t *s = d->session;
		if (s) {
			if (restart_device) {
				sccp_device_sendReset(d, SKINNY_RESETTYPE_RESTART);
			}
			sccp_session_releaseDevice(s);
			d->session = NULL;
			sccp_session_stopthread(s, SKINNY_DEVICE_RS_NONE);
		}
		sccp_device_setRegistrationState(d, SKINNY_DEVICE_RS_NONE);
	}
}

int __sccp_device_destroy(const void *ptr)
{
	sccp_device_t *d = (sccp_device_t *) ptr;

	if (!d) {
		pbx_log(LOG_ERROR, "SCCP: device destructor was called without a device (refcount bug)\n");
		return -1;
	}

	sccp_log((DEBUGCAT_DEVICE + DEBUGCAT_CONFIG)) (VERBOSE_PREFIX_1 "%s: destroying device\n", d->id);

	sccp_config_cleanup_dynamically_allocated_memory(d, SCCP_CONFIG_DEVICE_SEGMENT);

	// clean button config (only generated on read config, so do not remove during device clean)
	{
		sccp_buttonconfig_t *config = NULL;
		SCCP_LIST_LOCK(&d->buttonconfig);
		while ((config = SCCP_LIST_REMOVE_HEAD(&d->buttonconfig, list))) {
			sccp_buttonconfig_destroy(config);
		}
		SCCP_LIST_UNLOCK(&d->buttonconfig);
		if (!SCCP_LIST_EMPTY(&d->buttonconfig)) {
			pbx_log(LOG_WARNING, "%s: button configurations were still listed after the device was destroyed (list bug); they leak\n", d->id);
		}
		SCCP_LIST_HEAD_DESTROY(&d->buttonconfig);
	}

	{
		sccp_hostname_t *permithost = NULL;
		SCCP_LIST_LOCK(&d->permithosts);
		while ((permithost = SCCP_LIST_REMOVE_HEAD(&d->permithosts, list))) {
			if (permithost) {
				sccp_free(permithost);
			}
		}
		SCCP_LIST_UNLOCK(&d->permithosts);
		if (!SCCP_LIST_EMPTY(&d->permithosts)) {
			pbx_log(LOG_WARNING, "%s: permithost entries were still listed after the device was destroyed (list bug); they leak\n", d->id);
		}
		SCCP_LIST_HEAD_DESTROY(&d->permithosts);
	}

	{
		sccp_selectedchannel_t *selectedChannel = NULL;
		SCCP_LIST_LOCK(&d->selectedChannels);
		while ((selectedChannel = SCCP_LIST_REMOVE_HEAD(&d->selectedChannels, list))) {
			sccp_channel_release(&selectedChannel->channel);
			sccp_free(selectedChannel);
		}
		SCCP_LIST_UNLOCK(&d->selectedChannels);
		if (!SCCP_LIST_EMPTY(&d->selectedChannels)) {
			pbx_log(LOG_WARNING, "%s: selected calls were still listed after the device was destroyed (list bug); they leak\n", d->id);
		}
		SCCP_LIST_HEAD_DESTROY(&d->selectedChannels);
	}

	if (d->ha) {
		sccp_free_ha(d->ha);
		d->ha = NULL;
	}

	{
#ifndef SCCP_ATOMIC
		sccp_mutex_lock(&d->messageStack.lock);
#endif
		for(uint i = 0; i < SCCP_MESSAGE_PRIORITY_SENTINEL; i++) {
			if (d->messageStack.messages[i] != NULL) {
				sccp_free(d->messageStack.messages[i]);
			}
		}
#ifndef SCCP_ATOMIC
		sccp_mutex_unlock(&d->messageStack.lock);
		pbx_mutex_destroy(&d->messageStack.lock);
#endif
	}

	if (d->variables) {
		pbx_variables_destroy(d->variables);
		d->variables = NULL;
	}

	if (d->privateData) {
#if HAVE_ICONV
		if (d->privateData->iconv != (iconv_t) -1) {
			sccp_device_destroyiconv(d);
		}
#endif
		sccp_mutex_destroy(&d->privateData->lock);
		sccp_free(d->privateData);
	}

#ifdef CS_AST_HAS_STASIS_ENDPOINT
	if(iPbx.endpoint_shutdown && d->endpoint) {
		iPbx.endpoint_shutdown(&d->endpoint);
	}
#endif

	sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: device destroyed\n", d->id);
	return 0;
}

boolean_t sccp_device_isVideoSupported(constDevicePtr device)
{
	boolean_t res = FALSE;
#ifdef CS_SCCP_VIDEO
	if (device->capabilities.video[0] != SKINNY_CODEC_NONE) {
		res = TRUE;
	}
	sccp_log((DEBUGCAT_CODEC)) (VERBOSE_PREFIX_3 "%s: video supported: %s\n", device->id, res ? "true" : "false");
#endif
	return res;
}

sccp_buttonconfig_t *sccp_dev_serviceURL_find_byindex(devicePtr device, uint16_t instance)
{
	sccp_buttonconfig_t *config = NULL;

	if (!device || !device->session) {
		return NULL;
	}
	sccp_log((DEBUGCAT_DEVICE + DEBUGCAT_BUTTONTEMPLATE)) (VERBOSE_PREFIX_3 "%s: looking up service with instance %d\n", device->id, instance);
	SCCP_LIST_LOCK(&device->buttonconfig);
	SCCP_LIST_TRAVERSE(&device->buttonconfig, config, list) {
		sccp_log_and((DEBUGCAT_DEVICE + DEBUGCAT_HIGH)) (VERBOSE_PREFIX_3 "%s: instance %d, button type %d\n", device->id, config->instance, config->type);

		if (config->type == SERVICE && config->instance == instance) {
			sccp_log((DEBUGCAT_DEVICE + DEBUGCAT_BUTTONTEMPLATE)) (VERBOSE_PREFIX_3 "%s: service found: %s\n", device->id, config->label);
			break;
		}
	}
	SCCP_LIST_UNLOCK(&device->buttonconfig);

	return config;
}

int sccp_device_sendReset(devicePtr d, skinny_resetType_t reset_type)
{
	sccp_msg_t *msg = NULL;

	if (!d) {
		return 0;
	}

	REQ(msg, Reset);
	if (!msg) {
		pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
		return 0;
	}
	msg->data.Reset.lel_resetType = htolel(reset_type);
	sccp_session_send(d, msg);

	d->pendingUpdate = 0;
	return 1;
}

void sccp_device_sendcallstate(constDevicePtr d, uint8_t instance, uint32_t callid, skinny_callstate_t state, skinny_callpriority_t precedence_level, skinny_callinfo_visibility_t visibility)
{
	sccp_msg_t *msg = NULL;

	if (!d) {
		return;
	}
	REQ(msg, CallStateMessage);
	if (!msg) {
		pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
		return;
	}
	msg->data.CallStateMessage.lel_callState = htolel(state);
	msg->data.CallStateMessage.lel_lineInstance = htolel(instance);
	msg->data.CallStateMessage.lel_callReference = htolel(callid);
	msg->data.CallStateMessage.lel_visibility = htolel(visibility);
	msg->data.CallStateMessage.precedence.lel_level = htolel(precedence_level);
	msg->data.CallStateMessage.precedence.lel_domain = htolel(0);
	sccp_dev_send(d, msg);
	sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: call state %s (%d) on call %d (visibility %s)\n", d->id, skinny_callstate2str(state), state, callid, skinny_callinfo_visibility2str(visibility));
}

/* Only works on a limited set of devices and firmware revisions (more research needed). */
void sccp_device_sendCallHistoryDisposition(constDevicePtr d, uint8_t lineInstance, uint32_t callid, skinny_callHistoryDisposition_t disposition)
{
	sccp_msg_t *msg = NULL;
	if (!d) {
		return;
	}
	REQ(msg, CallHistoryDispositionMessage);
	if (!msg) {
		pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
		return;
	}
	msg->data.CallHistoryDispositionMessage.lel_disposition = htolel(disposition);
	msg->data.CallHistoryDispositionMessage.lel_lineInstance = htolel(lineInstance);
	msg->data.CallHistoryDispositionMessage.lel_callReference = htolel(callid);
	sccp_dev_send(d, msg);
	sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: call history disposition %s on call %d\n", d->id, skinny_callHistoryDisposition2str(disposition), callid);
}

/*
 * Get the number of channels that the device owns
 * device should be locked by parent functions
 */
uint8_t sccp_device_numberOfChannels(constDevicePtr device)
{
	sccp_buttonconfig_t *config = NULL;
	sccp_channel_t *c = NULL;
	uint8_t numberOfChannels = 0;

	if (!device) {
		sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "no device\n");
		return 0;
	}

	SCCP_LIST_TRAVERSE(&device->buttonconfig, config, list) {
		if (config->type == LINE) {
			AUTO_RELEASE(sccp_line_t, l , sccp_line_find_byname(config->button.line.name, FALSE));

			if (!l) {
				continue;
			}
			SCCP_LIST_LOCK(&l->channels);
			SCCP_LIST_TRAVERSE(&l->channels, c, list) {
				AUTO_RELEASE(sccp_device_t, tmpDevice , sccp_channel_getDevice(c));

				if (tmpDevice == device) {
					numberOfChannels++;
				}
			}
			SCCP_LIST_UNLOCK(&l->channels);
		}
	}

	return numberOfChannels;
}

void sccp_dev_keypadbutton(devicePtr d, char digit, uint8_t line, uint32_t callid)
{
	sccp_msg_t *msg = NULL;

	if (!d || !d->session) {
		return;
	}
	if (digit == '*') {
		digit = 0xe;
	} else if (digit == '#') {
		digit = 0xf;
	} else if (digit == '0') {
		digit = 0xa;											/* 0 is not 0 for cisco :-) */
	} else {
		digit -= '0';
	}

	if (digit > 16) {
		sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: phone cannot play this DTMF digit; sending it in band\n", d->id);
		return;
	}

	REQ(msg, KeypadButtonMessage);
	if (!msg) {
		pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
		return;
	}
	msg->data.KeypadButtonMessage.lel_kpButton = htolel(digit);
	msg->data.KeypadButtonMessage.lel_lineInstance = htolel(line);
	msg->data.KeypadButtonMessage.lel_callReference = htolel(callid);

	sccp_dev_send(d, msg);

	sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: sending keypad digit %02X\n", DEV_ID_LOG(d), digit);
}

static void sccp_device_indicate_onhook(constDevicePtr device, const uint8_t lineInstance, uint32_t callid)
{
	sccp_dev_stoptone(device, lineInstance, callid);
	sccp_device_setLamp(device, SKINNY_STIMULUS_LINE, lineInstance, SKINNY_LAMP_OFF);
	sccp_dev_clearprompt(device, lineInstance, callid);

	sccp_device_sendcallstate(device, lineInstance, callid, SKINNY_CALLSTATE_ONHOOK, SKINNY_CALLPRIORITY_LOW, SKINNY_CALLINFO_VISIBILITY_DEFAULT);
	sccp_dev_set_keyset(device, 0, 0, KEYMODE_ONHOOK);
	if (device->session) {
		sccp_handle_time_date_req(device->session, (sccp_device_t *) device, NULL);	/** we need datetime on hangup for 7936 */
	}

	sccp_device_clearMessageFromStack((sccp_device_t *) device, SCCP_MESSAGE_PRIORITY_PRIVACY);
	if (device->active_channel && device->active_channel->callid == callid) {
		sccp_dev_set_speaker(device, SKINNY_STATIONSPEAKER_OFF);
	}
	sccp_dev_set_ringer(device, SKINNY_RINGTYPE_OFF, SKINNY_RINGDURATION_NORMAL, lineInstance, callid);
}
static void sccp_device_indicate_offhook(constDevicePtr device, sccp_linedevice_t * ld, uint32_t callid)
{
	sccp_dev_set_speaker(device, SKINNY_STATIONSPEAKER_ON);
	if (device->dndFeature.status == SCCP_DNDMODE_OFF && device->monitorFeature.status == SCCP_FEATURE_MONITOR_STATE_DISABLED) {
		sccp_device_sendcallstate(device, ld->lineInstance, callid, SKINNY_CALLSTATE_OFFHOOK, SKINNY_CALLPRIORITY_LOW, SKINNY_CALLINFO_VISIBILITY_DEFAULT);
	} else {
		sccp_device_sendcallstate(device, ld->lineInstance, callid, SKINNY_CALLSTATE_CALLREMOTEMULTILINE, SKINNY_CALLPRIORITY_LOW, SKINNY_CALLINFO_VISIBILITY_DEFAULT);
	}
	sccp_dev_set_cplane(device, ld->lineInstance, 1);
	sccp_dev_displayprompt(device, ld->lineInstance, callid, SKINNY_DISP_ENTER_NUMBER, GLOB(digittimeout));
	sccp_dev_set_keyset(device, ld->lineInstance, callid, KEYMODE_OFFHOOK);
	sccp_dev_starttone(device, ld->line->initial_dialtone_tone, ld->lineInstance, callid, SKINNY_TONEDIRECTION_USER);
}

static void sccp_device_indicate_dialing(constDevicePtr device, const uint8_t lineInstance, const uint32_t callid, const skinny_calltype_t calltype, sccp_callinfo_t * const callinfo, char dialedNumber[SCCP_MAX_EXTENSION])
{
	sccp_dev_stoptone(device, lineInstance, callid);
	sccp_device_setLamp(device, SKINNY_STIMULUS_LINE, lineInstance, SKINNY_LAMP_BLINK);
	iCallInfo.Setter(callinfo, SCCP_CALLINFO_CALLEDPARTY_NUMBER, dialedNumber, SCCP_CALLINFO_KEY_SENTINEL);
	iCallInfo.Send(callinfo, callid, calltype, lineInstance, device, FALSE);

	if (device->protocol && device->protocol->sendDialedNumber) {
		device->protocol->sendDialedNumber(device, lineInstance, callid, dialedNumber);
	}
	sccp_device_sendcallstate(device, lineInstance, callid, SKINNY_CALLSTATE_PROCEED, SKINNY_CALLPRIORITY_LOW, SKINNY_CALLINFO_VISIBILITY_DEFAULT);
}

static void sccp_device_indicate_proceed(constDevicePtr device, const uint8_t lineInstance, const uint32_t callid, const skinny_calltype_t calltype, sccp_callinfo_t * const callinfo)
{
	sccp_dev_stoptone(device, lineInstance, callid);
	sccp_device_sendcallstate(device, lineInstance, callid, SKINNY_CALLSTATE_PROCEED, SKINNY_CALLPRIORITY_LOW, SKINNY_CALLINFO_VISIBILITY_DEFAULT);
	iCallInfo.Send(callinfo, callid, calltype, lineInstance, device, FALSE);
	sccp_dev_displayprompt(device, lineInstance, callid, SKINNY_DISP_CALL_PROCEED, GLOB(digittimeout));
}

static void sccp_device_indicate_connected(constDevicePtr device, const uint8_t lineInstance, const uint32_t callid, const skinny_calltype_t calltype, sccp_callinfo_t * const callinfo)
{
	sccp_dev_set_ringer(device, SKINNY_RINGTYPE_OFF, SKINNY_RINGDURATION_NORMAL, lineInstance, callid);
	sccp_dev_set_speaker(device, SKINNY_STATIONSPEAKER_ON);
	sccp_dev_stoptone(device, lineInstance, callid);
	sccp_device_setLamp(device, SKINNY_STIMULUS_LINE, lineInstance, SKINNY_LAMP_ON);
	sccp_device_sendcallstate(device, lineInstance, callid, SKINNY_CALLSTATE_CONNECTED, SKINNY_CALLPRIORITY_LOW, SKINNY_CALLINFO_VISIBILITY_DEFAULT);
	iCallInfo.Send(callinfo, callid, calltype, lineInstance, device, TRUE);
	sccp_dev_set_cplane(device, lineInstance, 1);
	sccp_dev_displayprompt(device, lineInstance, callid, SKINNY_DISP_CONNECTED, GLOB(digittimeout));
}

static void sccp_device_old_callhistory(constDevicePtr device, const uint8_t lineInstance, const uint32_t callid, const skinny_callHistoryDisposition_t disposition)
{
	skinny_callstate_t state=SKINNY_CALLSTATE_CONNECTED;
	skinny_callinfo_visibility_t visibility=SKINNY_CALLINFO_VISIBILITY_HIDDEN;
	sccp_log((DEBUGCAT_CALLINFO)) (VERBOSE_PREFIX_3 "%s: call history entry for call %d on line instance %d, disposition %s\n", device->id, callid, lineInstance, skinny_callHistoryDisposition2str(disposition));
	switch(disposition) {
		case SKINNY_CALL_HISTORY_DISPOSITION_RECEIVED_CALLS:
			state=SKINNY_CALLSTATE_CONNECTED;
			visibility=SKINNY_CALLINFO_VISIBILITY_COLLAPSED;
			break;
		case SKINNY_CALL_HISTORY_DISPOSITION_MISSED_CALLS:
			state=SKINNY_CALLSTATE_RINGIN;
			visibility=SKINNY_CALLINFO_VISIBILITY_COLLAPSED;
			break;
		case SKINNY_CALL_HISTORY_DISPOSITION_IGNORE:
		case SKINNY_CALL_HISTORY_DISPOSITION_PLACED_CALLS:
		case SKINNY_CALL_HISTORY_DISPOSITION_UNKNOWN:
		case SKINNY_CALLHISTORYDISPOSITION_SENTINEL:
			state=SKINNY_CALLSTATE_CONNECTED;
			visibility=SKINNY_CALLINFO_VISIBILITY_HIDDEN;
			break;
	}
	sccp_device_sendcallstate(device, lineInstance, callid, state, SKINNY_CALLPRIORITY_LOW, visibility);
}

static void sccp_device_new_callhistory(constDevicePtr device, const uint8_t lineInstance, const uint32_t callid, const skinny_callHistoryDisposition_t disposition)
{
	sccp_log((DEBUGCAT_CALLINFO)) (VERBOSE_PREFIX_3 "%s: call history entry for call %d on line instance %d, disposition %s\n", device->id, callid, lineInstance, skinny_callHistoryDisposition2str(disposition));
	sccp_device_sendCallHistoryDisposition(device, lineInstance, callid, disposition);
}

static void sccp_device_indicate_onhook_remote(constDevicePtr device, const uint8_t lineInstance, const uint32_t callid)
{
	sccp_device_setLamp(device, SKINNY_STIMULUS_LINE, lineInstance, SKINNY_LAMP_OFF);
	sccp_dev_cleardisplaynotify(device);
	sccp_dev_clearprompt(device, lineInstance, callid);
	sccp_dev_set_ringer(device, SKINNY_RINGTYPE_OFF, SKINNY_RINGDURATION_NORMAL, lineInstance, callid);
	sccp_device_sendcallstate(device, lineInstance, callid, SKINNY_CALLSTATE_ONHOOK, SKINNY_CALLPRIORITY_LOW, SKINNY_CALLINFO_VISIBILITY_DEFAULT);
	sccp_dev_set_keyset(device, lineInstance, callid, KEYMODE_ONHOOK);
	sccp_dev_set_cplane(device, lineInstance, 0);
	sccp_dev_set_keyset(device, lineInstance, callid, KEYMODE_ONHOOK);
	if (device->session) {
		sccp_handle_time_date_req(device->session, (sccp_device_t *) device, NULL);	/** we need datetime on hangup for 7936 */
	}
}

static void sccp_device_indicate_offhook_remote(constDevicePtr device, const uint8_t lineInstance, const uint32_t callid)
{
	sccp_device_sendcallstate(device, lineInstance, callid, SKINNY_CALLSTATE_OFFHOOK, SKINNY_CALLPRIORITY_LOW, SKINNY_CALLINFO_VISIBILITY_DEFAULT);
	sccp_dev_set_keyset(device, lineInstance, callid, KEYMODE_OFFHOOK);
}

static void sccp_device_indicate_connected_remote(constDevicePtr device, const uint8_t lineInstance, const uint32_t callid, skinny_callinfo_visibility_t visibility)
{
	sccp_dev_set_ringer(device, SKINNY_RINGTYPE_OFF, SKINNY_RINGDURATION_NORMAL, lineInstance, callid);
	sccp_dev_clearprompt(device, lineInstance, callid);
	sccp_device_setLamp(device, SKINNY_STIMULUS_LINE, lineInstance, SKINNY_LAMP_ON);
	sccp_device_sendcallstate(device, lineInstance, callid, SKINNY_CALLSTATE_CALLREMOTEMULTILINE, SKINNY_CALLPRIORITY_LOW, visibility);
	sccp_dev_set_keyset(device, lineInstance, callid, KEYMODE_ONHOOKSTEALABLE);
}

static void sccp_device_old_indicate_remoteHold(constDevicePtr device, uint8_t lineInstance, uint32_t callid, skinny_callpriority_t callpriority, skinny_callinfo_visibility_t visibility)
{
	sccp_device_sendcallstate(device, lineInstance, callid, SKINNY_CALLSTATE_HOLD, callpriority, visibility);
	sccp_dev_set_keyset(device, lineInstance, callid, KEYMODE_ONHOLD);
	sccp_dev_displayprompt(device, lineInstance, callid, SKINNY_DISP_HOLD, GLOB(digittimeout));
}

static void sccp_device_new_indicate_remoteHold(constDevicePtr device, uint8_t lineInstance, uint32_t callid, skinny_callpriority_t callpriority, skinny_callinfo_visibility_t visibility)
{
	sccp_device_sendcallstate(device, lineInstance, callid, SKINNY_CALLSTATE_HOLDRED, callpriority, visibility);
	sccp_dev_set_keyset(device, lineInstance, callid, KEYMODE_ONHOLD);
	sccp_dev_displayprompt(device, lineInstance, callid, SKINNY_DISP_HOLD, GLOB(digittimeout));
}

void sccp_device_addMessageToStack(devicePtr device, const uint8_t priority, const char *message)
{
	if (ARRAY_LEN(device->messageStack.messages) <= priority) {
		return;
	}
	char * newValue = NULL;
	char * oldValue = NULL;

	newValue = pbx_strdup(message);

	do {
		oldValue = device->messageStack.messages[priority];
	} while (!CAS_PTR(&device->messageStack.messages[priority], oldValue, newValue, &device->messageStack.lock));

	if (oldValue) {
		sccp_free(oldValue);
	}
	sccp_dev_check_displayprompt(device);
}

void sccp_device_clearMessageFromStack(devicePtr device, const uint8_t priority)
{
	if (ARRAY_LEN(device->messageStack.messages) <= priority) {
		return;
	}

	char * newValue = NULL;
	char * oldValue = NULL;

	sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_4 "%s: clearing message stack level %d\n", DEV_ID_LOG(device), priority);

	do {
		oldValue = device->messageStack.messages[priority];
	} while (!CAS_PTR(&device->messageStack.messages[priority], oldValue, newValue, &device->messageStack.lock));

	if (oldValue) {
		sccp_free(oldValue);
		sccp_dev_check_displayprompt(device);
	}
}

void sccp_device_featureChangedDisplay(const sccp_event_t * event)
{
	sccp_linedevice_t * ld = NULL;
	sccp_device_t * device = NULL;

	char tmp[256] = { 0 };
	size_t len = sizeof(tmp);
	char *s = tmp;

	if (!event || !(device = event->featureChanged.device)) {
		return;
	}
	sccp_log((DEBUGCAT_DEVICE + DEBUGCAT_EVENT + DEBUGCAT_FEATURE)) (VERBOSE_PREFIX_3 "%s: feature changed: %s (%d)\n", DEV_ID_LOG(device), sccp_feature_type2str(event->featureChanged.featureType), event->featureChanged.featureType);
	switch (event->featureChanged.featureType) {
		case SCCP_FEATURE_CFWDNONE:
			sccp_device_clearMessageFromStack(device, SCCP_MESSAGE_PRIORITY_CFWD);
			break;
		case SCCP_FEATURE_CFWDBUSY:
		case SCCP_FEATURE_CFWDALL:
		case SCCP_FEATURE_CFWDNOANSWER:
			if((ld = event->featureChanged.optional_linedevice)) {
				linePtr line = ld->line;
				uint8_t instance = ld->lineInstance;

				sccp_dev_forward_status(line, instance, device);
				for(uint x = SCCP_CFWD_ALL; x < SCCP_CFWD_SENTINEL; x++) {
					if(ld->cfwd[x].enabled) {
						sccp_cfwd_t cfwd_type = (sccp_cfwd_t)x;
						if(sccp_strlen(line->cid_num) + sccp_strlen(ld->cfwd[x].number) > 15) {
							pbx_build_string(&s, &len, "%s:%s", sccp_cfwd2disp(cfwd_type), ld->cfwd[x].number);
						} else {
							pbx_build_string(&s, &len, "%s:%s %s %s", sccp_cfwd2disp(cfwd_type), line->cid_num, SKINNY_DISP_FORWARDED_TO, ld->cfwd[x].number);
						}
					}
				}
			}
			if (!sccp_strlen_zero(tmp)) {
				sccp_device_addMessageToStack(device, SCCP_MESSAGE_PRIORITY_CFWD, tmp);
			} else {
				sccp_device_clearMessageFromStack(device, SCCP_MESSAGE_PRIORITY_CFWD);
			}
			break;
		case SCCP_FEATURE_DND:
			if(device->hasLabelLimitedDisplayPrompt()) {
				sccp_dev_displayprompt(device, 0, 0, SKINNY_DISP_YOUR_CURRENT_OPTIONS, 0);
			}
			if (device->dndFeature.status) {
				char dndmsg[StationMaxDisplayNotifySize];
				if (!device->dndmode) {
					if (device->dndFeature.status == SCCP_DNDMODE_SILENT) {
						snprintf(dndmsg, sizeof(dndmsg), SKINNY_DISP_DND " (" SKINNY_DISP_SILENT ")");
					} else {
						snprintf(dndmsg, sizeof(dndmsg), SKINNY_DISP_DND " (" SKINNY_DISP_BUSY ")");
					}
				} else {
					snprintf(dndmsg, sizeof(dndmsg), SKINNY_DISP_DO_NOT_DISTURB_IS_ACTIVE);
				}
				if (device->hasLabelLimitedDisplayPrompt() && device->hasDisplayPrompt()) {
					sccp_device_addMessageToStack(device, SCCP_MESSAGE_PRIORITY_DND, SKINNY_DISP_DO_NOT_DISTURB_IS_ACTIVE);
					if (!device->dndmode) {
						sccp_dev_displaynotify(device, dndmsg, 3);
					}
				} else {											// 79xx and 89xx series
					sccp_device_addMessageToStack(device, SCCP_MESSAGE_PRIORITY_DND, dndmsg);
				}
			} else {
				sccp_device_clearMessageFromStack(device, SCCP_MESSAGE_PRIORITY_DND);
			}
			break;
		case SCCP_FEATURE_PRIVACY:
			if (TRUE == device->privacyFeature.status) {
				sccp_device_addMessageToStack(device, SCCP_MESSAGE_PRIORITY_PRIVACY, SKINNY_DISP_PRIVATE);
			} else {
				sccp_device_clearMessageFromStack(device, SCCP_MESSAGE_PRIORITY_PRIVACY);
			}
			break;
		case SCCP_FEATURE_MONITOR:
			if (device->monitorFeature.status & (SCCP_FEATURE_MONITOR_STATE_REQUESTED | SCCP_FEATURE_MONITOR_STATE_ACTIVE)) {
				sccp_dev_set_message(device, SKINNY_DISP_RECORDING, SCCP_DISPLAYSTATUS_TIMEOUT, FALSE, FALSE);
			} else if (device->monitorFeature.status & SCCP_FEATURE_MONITOR_STATE_REQUESTED) {
				sccp_device_addMessageToStack(device, SCCP_MESSAGE_PRIORITY_MONITOR, SKINNY_DISP_RECORDING_AWAITING_CALL_TO_BE_ACTIVE);
			} else {
				sccp_device_clearMessageFromStack(device, SCCP_MESSAGE_PRIORITY_MONITOR);
			}
			break;
		case SCCP_FEATURE_PARKINGLOT:
			break;
		default:
			return;
	}
}

/*
 * Bare '&', '<', '>', '"' and '\'' are escaped; an '&' that starts one of the five XML entities or a numeric character reference is kept.
 */
static int sccp_device_escapeXmlUrl(const char *url, char *outbuf, size_t buflen)
{
	size_t used = 0;
	for (const char *p = url; *p; p++) {
		const char *entity = NULL;
		char single[2] = { *p, '\0' };
		switch (*p) {
			case '&': {
				static const char *const known[] = { "amp;", "lt;", "gt;", "quot;", "apos;" };
				boolean_t isEntity = FALSE;
				for (size_t i = 0; i < ARRAY_LEN(known) && !isEntity; i++) {
					isEntity = !strncmp(p + 1, known[i], strlen(known[i]));
				}
				if (!isEntity && p[1] == '#') {
					const char *digits = (p[2] == 'x' || p[2] == 'X') ? p + 3 : p + 2;
					size_t len = strspn(digits, (digits == p + 2) ? "0123456789" : "0123456789abcdefABCDEF");
					isEntity = len > 0 && digits[len] == ';';
				}
				entity = isEntity ? "&" : "&amp;";
				break;
			}
			case '<':
				entity = "&lt;";
				break;
			case '>':
				entity = "&gt;";
				break;
			case '"':
				entity = "&quot;";
				break;
			case '\'':
				entity = "&apos;";
				break;
			default:
				entity = single;
				break;
		}
		size_t len = strlen(entity);
		if (used + len >= buflen) {
			outbuf[used] = '\0';
			return -1;
		}
		memcpy(outbuf + used, entity, len);
		used += len;
	}
	outbuf[used] = '\0';
	return 0;
}

static sccp_push_result_t sccp_device_pushURL(constDevicePtr device, const char *url, uint8_t priority, skinny_tone_t tone)
{
	const char *xmlFormat = "<CiscoIPPhoneExecute><ExecuteItem Priority=\"0\" URL=\"%s\"/></CiscoIPPhoneExecute>";
	unsigned int transactionID = sccp_random();

	if (sccp_strlen(url) > 256) {
		sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: URL not pushed: it is %d characters and phones accept at most 256\n", DEV_ID_LOG(device), (int)sccp_strlen(url));
		return SCCP_PUSH_RESULT_FAIL;
	}
	char escapedUrl[256 * 6 + 1];
	if (sccp_device_escapeXmlUrl(url ? url : "", escapedUrl, sizeof(escapedUrl))) {
		return SCCP_PUSH_RESULT_FAIL;
	}
	size_t msg_length = strlen(xmlFormat) - 2 + strlen(escapedUrl) + 1 ;
	char xmlData[msg_length];

	snprintf(xmlData, msg_length, xmlFormat, escapedUrl);
	device->protocol->sendUserToDeviceDataVersionMessage(device, APPID_PUSH, 0, 1, transactionID, xmlData, priority);
	if (SKINNY_TONE_SILENCE != tone) {
		sccp_dev_starttone(device, tone, 0, 0, SKINNY_TONEDIRECTION_USER);
	}
	return SCCP_PUSH_RESULT_SUCCESS;
}

/*
 * title field can be max 32 characters long protocolversion < 17 allows a maximum of 1024 characters in the text block protocolversion >= 17 allows variable sized messages up to 4000 characters in the text block
 */
static sccp_push_result_t sccp_device_pushTextMessage(constDevicePtr device, const char *messageText, const char *from, uint8_t priority, skinny_tone_t tone)
{
	const char *xmlFormat = "<CiscoIPPhoneText>%s<Text>%s</Text></CiscoIPPhoneText>";
	const char *xmlTitleFormat = "<Title>%s</Title>";
	size_t text_length = sccp_strlen(messageText);
	size_t from_length = sccp_strlen(from);
	char title[sizeof("<Title></Title>") + 32 * 6] = "";
	unsigned int transactionID = sccp_random();

	if (!messageText || from_length > 32) {
		sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: text message not pushed: text missing or sender longer than 32 characters\n", DEV_ID_LOG(device));
		return SCCP_PUSH_RESULT_FAIL;
	}

	if (text_length > (device->protocolversion < 17 ? 1024 : 4000)) {
		sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: text message not pushed: text too long\n", DEV_ID_LOG(device));
		return SCCP_PUSH_RESULT_FAIL;
	}

	if (from_length) {
		char escapedFrom[32 * 6 + 1];
		if (ast_xml_escape(from, escapedFrom, sizeof(escapedFrom))) {
			return SCCP_PUSH_RESULT_FAIL;
		}
		snprintf(title, sizeof(title), xmlTitleFormat, escapedFrom);
	}

	size_t escaped_size = text_length * 6 + 1;
	char *escapedText = sccp_malloc(escaped_size);
	if (!escapedText) {
		pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
		return SCCP_PUSH_RESULT_FAIL;
	}
	if (ast_xml_escape(messageText, escapedText, escaped_size)) {
		sccp_free(escapedText);
		return SCCP_PUSH_RESULT_FAIL;
	}

	size_t msg_length = strlen(xmlFormat) - 4 + strlen(title) + strlen(escapedText) + 1;
	char *xmlData = sccp_malloc(msg_length);
	if (!xmlData) {
		sccp_free(escapedText);
		pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
		return SCCP_PUSH_RESULT_FAIL;
	}

	snprintf(xmlData, msg_length, xmlFormat, title, escapedText);
	sccp_free(escapedText);
	device->protocol->sendUserToDeviceDataVersionMessage(device, APPID_PUSH, 0, 1, transactionID, xmlData, priority);
	sccp_free(xmlData);

	if (SKINNY_TONE_SILENCE != tone) {
		sccp_dev_starttone(device, tone, 0, 0, SKINNY_TONEDIRECTION_USER);
	}
	return SCCP_PUSH_RESULT_SUCCESS;
}

/* device should be locked by parent function */
uint8_t __PURE__ sccp_device_find_index_for_line(constDevicePtr d, const char *lineName)
{
	for(uint8_t instance = SCCP_FIRST_LINEINSTANCE; instance < d->lineButtons.size; instance++) {
		if (d->lineButtons.instance[instance] && d->lineButtons.instance[instance]->line && !strcasecmp(d->lineButtons.instance[instance]->line->name, lineName)) {
			return instance;
		}
	}
	return 0;
}

gcc_inline int16_t sccp_device_buttonIndex2lineInstance(constDevicePtr d, uint16_t buttonIndex)
{
	if (buttonIndex > 0 && buttonIndex < StationMaxButtonTemplateSize && d->buttonTemplate[buttonIndex - 1].instance) {
		return d->buttonTemplate[buttonIndex - 1].instance;
	}
	pbx_log(LOG_WARNING, "%s: button %d is not a line button; request ignored\n", d->id, buttonIndex);
	return -1;
}

devicePtr sccp_device_find_byid(const char * id, boolean_t useRealtime)
{
	sccp_device_t *d = NULL;

	if (sccp_strlen_zero(id)) {
		sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "SCCP: device lookup with an empty name refused\n");
		return NULL;
	}

	SCCP_RWLIST_RDLOCK(&GLOB(devices));
	d = SCCP_RWLIST_FIND(&GLOB(devices), sccp_device_t, tmpd, list, (sccp_strcaseequals(tmpd->id, id)), TRUE, __FILE__, __LINE__, __PRETTY_FUNCTION__);
	SCCP_RWLIST_UNLOCK(&GLOB(devices));

#ifdef CS_SCCP_REALTIME
	if (!d && useRealtime) {
		d = sccp_device_find_realtime_byid(id);
	}
#endif

	return d;
}

#ifdef CS_SCCP_REALTIME
#if DEBUG
devicePtr __sccp_device_find_realtime(const char * name, const char * filename, int lineno, const char * func)
#	else
devicePtr sccp_device_find_realtime(const char * name)
#	endif
{
	sccp_device_t *d = NULL;
	PBX_VARIABLE_TYPE *v = NULL, *variable = NULL;

	if (sccp_strlen_zero(GLOB(realtimedevicetable)) || sccp_strlen_zero(name)) {
		return NULL;
	}
	if ((variable = pbx_load_realtime(GLOB(realtimedevicetable), "name", name, NULL))) {
		v = variable;
		sccp_log((DEBUGCAT_DEVICE + DEBUGCAT_REALTIME)) (VERBOSE_PREFIX_3 "SCCP: device %s found in realtime table %s\n", name, GLOB(realtimedevicetable));

		d = sccp_device_create(name);
		if (!d) {
			pbx_log(LOG_ERROR, "%s: realtime device not created: out of memory\n", name);
			return NULL;
		}

		sccp_config_applyDeviceConfiguration(d, v);

		sccp_device_addToGlobals(d);

		d->realtime = TRUE;
		pbx_variables_destroy(v);

		return d;
	}

	sccp_log((DEBUGCAT_DEVICE + DEBUGCAT_REALTIME)) (VERBOSE_PREFIX_3 "SCCP: device %s not found in realtime table %s\n", name, GLOB(realtimedevicetable));
	return NULL;
}
#endif

void sccp_device_setLamp(constDevicePtr device, skinny_stimulus_t stimulus, uint8_t instance, skinny_lampmode_t mode)
{
	sccp_msg_t *msg = NULL;

	REQ(msg, SetLampMessage);
	if (!msg) {
		pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
		return;
	}
	msg->data.SetLampMessage.lel_stimulus         = htolel(stimulus);
	msg->data.SetLampMessage.lel_stimulusInstance = instance;
	msg->data.SetLampMessage.lel_lampMode         = htolel(mode);
	sccp_dev_send(device, msg);
}

void sccp_device_setMWI(devicePtr device)
{
	device->voicemailStatistic.newmsgs = 0;
	device->voicemailStatistic.oldmsgs = 0;
	for (uint8_t instance = SCCP_FIRST_LINEINSTANCE; instance < device->lineButtons.size; instance++) {
		if(device->lineButtons.instance[instance]) {
			linePtr l = device->lineButtons.instance[instance]->line;
			device->voicemailStatistic.newmsgs += l->voicemailStatistic.newmsgs;
			device->voicemailStatistic.oldmsgs += l->voicemailStatistic.oldmsgs;
		}
	}
	sccp_log((DEBUGCAT_MWI)) (VERBOSE_PREFIX_3 "%s: voicemail: %d new, %d old\n", device->id, device->voicemailStatistic.newmsgs, device->voicemailStatistic.oldmsgs);
	device->mwiUpdateRequired = TRUE;
	sccp_device_indicateMWI(device);
}

void sccp_device_suppressMWI(devicePtr device)
{
	if (!device->mwioncall) {
		sccp_log((DEBUGCAT_MWI)) (VERBOSE_PREFIX_3 "%s: message waiting indication suppressed\n", device->id);
		device->mwiUpdateRequired = TRUE;
		sccp_device_setLamp(device, SKINNY_STIMULUS_VOICEMAIL, 0, SKINNY_LAMP_OFF);
	}
}

void sccp_device_indicateMWI(devicePtr device)
{
	sccp_log((DEBUGCAT_MWI)) (VERBOSE_PREFIX_3 "%s: message waiting update needed: %s\n", device->id, device->mwiUpdateRequired ? "yes" : "no");
	if (device->mwiUpdateRequired) {
		sccp_log((DEBUGCAT_MWI)) (VERBOSE_PREFIX_3 "%s: message lamp %s\n", device->id,
			device->voicemailStatistic.newmsgs ? "on" : "off");
		sccp_device_setLamp(device, SKINNY_STIMULUS_VOICEMAIL, 0, device->voicemailStatistic.newmsgs ? device->mwilamp : SKINNY_LAMP_OFF);

		if (device->voicemailStatistic.newmsgs || device->voicemailStatistic.oldmsgs) {
			sccp_log((DEBUGCAT_MWI)) (VERBOSE_PREFIX_3 "%s: showing voicemail indicator\n", device->id);
			char buffer[StationMaxDisplayTextSize];
			snprintf(buffer, StationMaxDisplayTextSize, "%s: (%u/%u)", SKINNY_DISP_YOU_HAVE_VOICEMAIL, device->voicemailStatistic.newmsgs, device->voicemailStatistic.oldmsgs);
			sccp_device_addMessageToStack(device, SCCP_MESSAGE_PRIORITY_VOICEMAIL, buffer);
		} else {
			sccp_log((DEBUGCAT_MWI)) (VERBOSE_PREFIX_3 "%s: removing voicemail indicator\n", device->id);
			sccp_device_clearMessageFromStack(device, SCCP_MESSAGE_PRIORITY_VOICEMAIL);
		}
	}
}
