/*!
 * \file        sccp_linedevice.c
 * \brief       SCCP LineDevice
 * \author      Sergio Chersovani <mlists [at] c-net.it>
 * \note        Reworked, but based on chan_sccp code.
 *              The original chan_sccp driver that was made by Zozo which itself was derived from the chan_skinny driver.
 *              Modified by Jan Czmok and Julien Goodwin
 * \note        This program is free software and may be modified and distributed under the terms of the GNU Public License.
 *              See the LICENSE file at the top of the source tree.
 *
 */

#include "config.h"
#include "common.h"
#include "sccp_device.h"
#include "sccp_line.h"
#include "sccp_linedevice.h"
#include "sccp_utils.h"

SCCP_FILE_VERSION(__FILE__, "");

static int __sccp_lineDevice_destroy(const void * ptr)
{
	sccp_linedevice_t * ld = (sccp_linedevice_t *)ptr;

	sccp_log((DEBUGCAT_DEVICE + DEBUGCAT_LINE + DEBUGCAT_CONFIG))(VERBOSE_PREFIX_1 "%s: line device %p freed\n", DEV_ID_LOG(ld->device), ld);
	if(ld->line) {
		sccp_line_release(&ld->line); /* explicit release of line retained in ld */
	}
	if(ld->device) {
		sccp_device_release(&ld->device); /* explicit release of device retained in ld */
	}
	return 0;
}

static void regcontext_exten(constLineDevicePtr ld, int onoff)
{
	char multi[256] = "";
	char * stringp = NULL;

	char cntxt[SCCP_MAX_CONTEXT];
	struct pbx_context * con = NULL;
	struct pbx_find_info q = { .stacklen = 0 };

	if(sccp_strlen_zero(GLOB(regcontext))) {
		return;
	}

	if(!ld || !ld->line) {
		return;
	}
	sccp_line_t * l = ld->line;

	sccp_copy_string(multi, S_OR(l->regexten, l->name), sizeof(multi));
	stringp = multi;

	char * ext;
	while((ext = strsep(&stringp, "&"))) {
		char * context;
		if((context = strchr(ext, '@'))) {
			*context++ = '\0';
			if(!pbx_context_find(context)) {
				pbx_log(LOG_WARNING, "SCCP: regcontext entry %s@%s skipped: context %s does not exist\n", ext, context, context);
				continue;
			}
		} else {
			sccp_copy_string(cntxt, GLOB(regcontext), sizeof(cntxt));
			context = cntxt;
		}
		con = pbx_context_find_or_create(NULL, NULL, context, "SCCP");
		if(con) {
			if(onoff) {
				if(!pbx_exists_extension(NULL, context, ext, 1, NULL) && pbx_add_extension(context, 0, ext, 1, NULL, NULL, "Noop", pbx_strdup(l->name), sccp_free_ptr, "SCCP")) {
					sccp_log((DEBUGCAT_LINE + DEBUGCAT_CONFIG))(VERBOSE_PREFIX_1 "registered in context %s: extension %s for line %s\n", context, ext, l->name);
				}
			} else {
				if(SCCP_LIST_GETSIZE(&l->devices) == 1) {                                        // only remove entry if it is the last one (shared line)
					if(pbx_find_extension(NULL, NULL, &q, context, ext, 1, NULL, "", E_MATCH)) {
						ast_context_remove_extension(context, ext, 1, NULL);
						sccp_log((DEBUGCAT_LINE + DEBUGCAT_CONFIG))(VERBOSE_PREFIX_1 "unregistered from context %s: extension %s\n", context, ext);
					}
				}
			}
		} else {
			pbx_log(LOG_ERROR, "SCCP: context '%s' does not exist and could not be created\n", context);
		}
	}
}

void sccp_linedevice_cfwd(lineDevicePtr ld, sccp_cfwd_t type, char * number)
{
	if (!ld || !ld->line || type < SCCP_CFWD_NONE || type >= SCCP_CFWD_SENTINEL) {
		return;
	}

	if(type == SCCP_CFWD_NONE) {
		for(uint x = SCCP_CFWD_ALL; x < SCCP_CFWD_SENTINEL; x++) {
			ld->cfwd[x].enabled = FALSE;
			ld->cfwd[x].number[0] = '\0';
		}
		sccp_log((DEBUGCAT_CORE))(VERBOSE_PREFIX_3 "%s: all call forwards turned off on line %s\n", DEV_ID_LOG(ld->device), ld->line->name);
	} else {
		if(!number || sccp_strlen_zero(number)) {
			ld->cfwd[type].enabled = FALSE;
			ld->cfwd[type].number[0] = '\0';
			sccp_log((DEBUGCAT_CORE))(VERBOSE_PREFIX_3 "%s: call forward not set: no number\n", DEV_ID_LOG(ld->device));
		} else {
			ld->cfwd[type].enabled = TRUE;
			sccp_copy_string(ld->cfwd[type].number, number, sizeof(ld->cfwd[type].number));
			sccp_log((DEBUGCAT_CORE))(VERBOSE_PREFIX_3 "%s: call forward %s on line %s to %s\n", DEV_ID_LOG(ld->device), sccp_cfwd2str(type), ld->line->name, number);
		}
	}
	sccp_feat_changed(ld->device, ld, sccp_cfwd2feature(type));
	sccp_dev_forward_status(ld->line, ld->lineInstance, ld->device);
}

const char * const sccp_linedevice_get_cfwd_string(constLineDevicePtr ld, char * const buffer, size_t size)
{
	if(!ld) {
		buffer[0] = '\0';
		return NULL;
	}
	snprintf(buffer, size, "All:%s, Busy:%s, NoAnswer:%s", ld->cfwd[SCCP_CFWD_ALL].enabled ? ld->cfwd[SCCP_CFWD_ALL].number : "off", ld->cfwd[SCCP_CFWD_BUSY].enabled ? ld->cfwd[SCCP_CFWD_BUSY].number : "off",
		 ld->cfwd[SCCP_CFWD_NOANSWER].enabled ? ld->cfwd[SCCP_CFWD_NOANSWER].number : "off");
	return buffer;
}

void sccp_linedevice_create(constDevicePtr d, constLinePtr l, uint8_t lineInstance, sccp_subscription_id_t * subscriptionId)
{
	AUTO_RELEASE(sccp_line_t, line, sccp_line_retain(l));
	AUTO_RELEASE(sccp_device_t, device, sccp_device_retain(d));

	if(!device || !line) {
		pbx_log(LOG_ERROR, "SCCP: sccp_linedevice_create() was called without a line or device (caller bug)\n");
		return;
	}
	sccp_linedevice_t * ld = NULL;

	if((ld = sccp_linedevice_find(device, l))) {
		sccp_log((DEBUGCAT_LINE))(VERBOSE_PREFIX_3 "%s: already registered on line %s\n", DEV_ID_LOG(device), l->name);
		sccp_linedevice_release(&ld); /* explicit release of found ld */
		return;
	}

	sccp_log((DEBUGCAT_LINE))(VERBOSE_PREFIX_3 "%s: adding device to line %s\n", DEV_ID_LOG(device), line->name);
#if CS_REFCOUNT_DEBUG
	sccp_refcount_addRelationship(device, line);
#endif
	char ld_id[REFCOUNT_INDENTIFIER_SIZE];

	snprintf(ld_id, REFCOUNT_INDENTIFIER_SIZE, "%s/%s", device->id, line->name);
	ld = (sccp_linedevice_t *)sccp_refcount_object_alloc(sizeof(sccp_linedevice_t), SCCP_REF_LINEDEVICE, ld_id, __sccp_lineDevice_destroy);
	if(!ld) {
		pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, ld_id);
		return;
	}
	memset(ld, 0, sizeof *ld);
#if CS_REFCOUNT_DEBUG
	sccp_refcount_addRelationship(l, ld);
	sccp_refcount_addRelationship(device, ld);
#endif
	*(sccp_device_t **)&(ld->device) = sccp_device_retain(device);
	*(sccp_line_t **)&(ld->line) = sccp_line_retain(line);
	ld->lineInstance = lineInstance;
	if(NULL != subscriptionId) {
		memcpy(&ld->subscriptionId, subscriptionId, sizeof(ld->subscriptionId));
	}

	SCCP_LIST_LOCK(&line->devices);
	SCCP_LIST_INSERT_HEAD(&line->devices, ld, list);
	SCCP_LIST_UNLOCK(&line->devices);

	ld->line->statistic.numberOfActiveDevices++;
	ld->device->configurationStatistic.numberOfLines++;

	sccp_line_updatePreferencesFromDevicesToLine(line);
	sccp_line_updateCapabilitiesFromDevicesToLine(line);

	sccp_event_t * event = sccp_event_allocate(SCCP_EVENT_DEVICE_ATTACHED);
	if(event) {
		event->deviceAttached.ld = sccp_linedevice_retain(ld);
		sccp_event_fire(event);
	}
	regcontext_exten(ld, 1);
	sccp_log((DEBUGCAT_LINE))(VERBOSE_PREFIX_3 "%s: line device %p added for %s\n", line->name, ld, DEV_ID_LOG(device));
}

void sccp_linedevice_remove(constDevicePtr d, linePtr l)
{
	sccp_linedevice_t * ld = NULL;

	if(!l) {
		return;
	}
	sccp_log_and((DEBUGCAT_HIGH + DEBUGCAT_LINE))(VERBOSE_PREFIX_3 "%s: removing device from line %s\n", DEV_ID_LOG(d), l->name);

	SCCP_LIST_LOCK(&l->devices);
	SCCP_LIST_TRAVERSE_SAFE_BEGIN(&l->devices, ld, list) {
		if(d == NULL || ld->device == d) {
#if CS_REFCOUNT_DEBUG
			sccp_refcount_removeRelationship(d ? d : ld->device, l);
#endif
			regcontext_exten(ld, 0);
			SCCP_LIST_REMOVE_CURRENT(list);
			l->statistic.numberOfActiveDevices--;
			sccp_event_t * event = sccp_event_allocate(SCCP_EVENT_DEVICE_DETACHED);
			if(event) {
				event->deviceAttached.ld = sccp_linedevice_retain(ld);
				sccp_event_fire(event);
			}
			sccp_linedevice_release(&ld); /* explicit release of list retained ld */
#ifdef CS_SCCP_REALTIME
			if(l->realtime && SCCP_LIST_GETSIZE(&l->devices) == 0 && SCCP_LIST_GETSIZE(&l->channels) == 0) {
				sccp_line_clean(l, TRUE);
			}
#endif
			if(d)
				break ;
		}
	}
	SCCP_LIST_TRAVERSE_SAFE_END;
	SCCP_LIST_UNLOCK(&l->devices);

	if(GLOB(module_running) == TRUE && d) {
		sccp_line_updatePreferencesFromDevicesToLine(l);
		sccp_line_updateCapabilitiesFromDevicesToLine(l);
	}
}

void sccp_linedevice_indicateMWI(constLineDevicePtr ld)
{
	AUTO_RELEASE(sccp_device_t, d, sccp_device_retain(ld->device));
	AUTO_RELEASE(sccp_line_t, l, sccp_line_retain(ld->line));
	if(l && d) {
		sccp_log((DEBUGCAT_MWI))(VERBOSE_PREFIX_3 "%s: message lamp %s on %s\n", l->name, l->voicemailStatistic.newmsgs ? "on" : "off", d->id);
		sccp_device_setLamp(d, SKINNY_STIMULUS_VOICEMAIL, ld->lineInstance, l->voicemailStatistic.newmsgs ? d->mwilamp : SKINNY_LAMP_OFF);
	}
}

lineDevicePtr __sccp_linedevice_find(constDevicePtr device, constLinePtr line, const char * filename, int lineno, const char * func)
{
	sccp_linedevice_t * ld = NULL;
	sccp_line_t * l = NULL;
	if(!line) {
		pbx_log(LOG_WARNING, "SCCP: line-device lookup without a line (caller bug at %s:%d)\n", filename, lineno);
		return NULL;
	}
	l = (sccp_line_t *)line;

	if(!device) {
		pbx_log(LOG_WARNING, "SCCP: line-device lookup on line %s without a device (caller bug at %s:%d)\n", line->name, filename, lineno);
		return NULL;
	}

	SCCP_LIST_LOCK(&l->devices);
	ld = SCCP_LIST_FIND(&l->devices, sccp_linedevice_t, tmplinedevice, list, (device == tmplinedevice->device), TRUE, filename, lineno, func);
	SCCP_LIST_UNLOCK(&l->devices);

	if(!ld) {
		sccp_log_and((DEBUGCAT_LINE + DEBUGCAT_HIGH))(VERBOSE_PREFIX_3 "%s: (%s:%d) no line device for line %s\n", DEV_ID_LOG(device), filename, lineno, line->name);
	}
	return ld;
}

lineDevicePtr __sccp_linedevice_findByLineinstance(constDevicePtr device, uint16_t instance, const char * filename, int lineno, const char * func)
{
	sccp_linedevice_t * ld = NULL;

	if(instance < 1) {
		pbx_log(LOG_WARNING, "%s: line-device lookup with line instance %d, which is not valid (caller bug at %s:%d)\n", DEV_ID_LOG(device), instance, filename, lineno);
		return NULL;
	}
	if(!device) {
		pbx_log(LOG_WARNING, "SCCP: line-device lookup for line instance %d without a device (caller bug at %s:%d)\n", instance, filename, lineno);
		return NULL;
	}

	if (instance < device->lineButtons.size && device->lineButtons.instance[instance]) {
		ld = sccp_linedevice_retain(device->lineButtons.instance[instance]);
	}

	if(!ld) {
		sccp_log_and((DEBUGCAT_LINE + DEBUGCAT_HIGH))(VERBOSE_PREFIX_3 "%s: (%s:%d) no line device for line instance %d\n", DEV_ID_LOG(device), filename, lineno, instance);
	}
	return ld;
}

void sccp_linedevice_createButtonsArray(devicePtr device)
{
	sccp_linedevice_t * ld = NULL;
	uint8_t lineInstances = 0;
	btnlist * btn = NULL;
	uint8_t i = 0;

	if(device->lineButtons.size) {
		sccp_linedevice_deleteButtonsArray(device);
	}

	btn = device->buttonTemplate;

	for(i = 0; i < StationMaxButtonTemplateSize; i++) {
		if(btn[i].type == SKINNY_BUTTONTYPE_LINE && btn[i].instance > lineInstances && btn[i].ptr) {
			lineInstances = btn[i].instance;
		}
	}

	device->lineButtons.instance = (sccp_linedevice_t **)sccp_calloc(lineInstances + SCCP_FIRST_LINEINSTANCE, sizeof(sccp_linedevice_t *));
	if(!device->lineButtons.instance) {
		pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, device->id);
		return;
	}
	device->lineButtons.size = lineInstances + SCCP_FIRST_LINEINSTANCE;

	for(i = 0; i < StationMaxButtonTemplateSize; i++) {
		if(btn[i].type == SKINNY_BUTTONTYPE_LINE && btn[i].ptr) {
			ld = sccp_linedevice_find(device, (sccp_line_t *)btn[i].ptr);
			if(!(device->lineButtons.instance[btn[i].instance] = ld)) {
				pbx_log(LOG_ERROR, "%s: line button %d has no line attached; button left out of the line list\n", device->id, btn[i].instance);
			}
		}
	}
}

void sccp_linedevice_deleteButtonsArray(devicePtr device)
{
	uint8_t i = 0;

	if(device->lineButtons.instance) {
		for(i = SCCP_FIRST_LINEINSTANCE; i < device->lineButtons.size; i++) {
			if(device->lineButtons.instance[i]) {
				sccp_linedevice_t * tmpld = device->lineButtons.instance[i];
				sccp_linedevice_release(&tmpld);                             /* explicit release of retained ld */
				device->lineButtons.instance[i] = NULL;
			}
		}
		device->lineButtons.size = 0;
		sccp_free(device->lineButtons.instance);
	}
}
