/*!
 * \file        sccp_management.c
 * \brief       SCCP Management Class
 * \author      Marcello Ceschia <marcello [at] ceschia.de>
 * \note        This program is free software and may be modified and distributed under the terms of the GNU Public License.
 *              See the LICENSE file at the top of the source tree.
 */
#include "config.h"
#ifdef CS_SCCP_MANAGER
#include "common.h"
#include "sccp_channel.h"
#include "sccp_actions.h"
#include "sccp_config.h"
#include "sccp_device.h"
#include "sccp_feature.h"
#include "sccp_line.h"
#	include "sccp_linedevice.h"
#	include "sccp_management.h"
#	include "sccp_session.h"
#	include "sccp_utils.h"
#	include "sccp_labels.h"
#	include "sccp_featureParkingLot.h"
#	include <asterisk/threadstorage.h>
#	include <asterisk/localtime.h>

SCCP_FILE_VERSION(__FILE__, "");

void sccp_manager_eventListener(const sccp_event_t * event);
static const char * configMetadata_command = "SCCPConfigMetadata";

#if HAVE_PBX_MANAGER_HOOK_H
static int sccp_asterisk_managerHookHelper(int category, const char *event, char *content);
boolean_t  hook_registered = FALSE;

static struct manager_custom_hook sccp_manager_hook = {
	.file = "chan_sccp",
	.helper = sccp_asterisk_managerHookHelper,
};
#	endif

int sccp_register_management(void)
{
	int result = iPbx.register_manager(configMetadata_command, EVENT_FLAG_SYSTEM | EVENT_FLAG_CONFIG, sccp_manager_config_metadata, NULL, NULL);

#	if HAVE_PBX_MANAGER_HOOK_H
#		if CS_AST_MANAGER_CHECK_ENABLED
	if (ast_manager_check_enabled())
#		endif
	{
		ast_manager_register_hook(&sccp_manager_hook);
		hook_registered = TRUE;
	}
#	else
#		warning "manager_custom_hook not found, monitor indication does not work properly"
#	endif
	return result;
}

int sccp_unregister_management(void)
{
	int result = pbx_manager_unregister(configMetadata_command);
#	if HAVE_PBX_MANAGER_HOOK_H
	if (hook_registered) {
		ast_manager_unregister_hook(&sccp_manager_hook);
	}
#	endif
	return result;
}

void sccp_manager_module_start(void)
{
	sccp_event_subscribe(SCCP_EVENT_DEVICE_ATTACHED | SCCP_EVENT_DEVICE_PREREGISTERED | SCCP_EVENT_DEVICE_REGISTERED | SCCP_EVENT_FEATURE_CHANGED, sccp_manager_eventListener, TRUE);
	sccp_event_subscribe(SCCP_EVENT_DEVICE_DETACHED | SCCP_EVENT_DEVICE_UNREGISTERED, sccp_manager_eventListener, FALSE);
}

void sccp_manager_module_stop(void)
{
	sccp_event_unsubscribe(SCCP_EVENT_DEVICE_ATTACHED | SCCP_EVENT_DEVICE_DETACHED | SCCP_EVENT_DEVICE_PREREGISTERED | SCCP_EVENT_DEVICE_REGISTERED | SCCP_EVENT_DEVICE_UNREGISTERED, sccp_manager_eventListener);
}

void sccp_manager_eventListener(const sccp_event_t * event)
{
	sccp_device_t * device = NULL;
	sccp_linedevice_t * ld = NULL;

	if (!event) {
		return;
	}
	switch (event->type) {
		case SCCP_EVENT_DEVICE_REGISTERED:
			device = event->deviceRegistered.device;						// already retained in the event
			manager_event(EVENT_FLAG_CALL, "DeviceStatus", "ChannelType: SCCP\r\nChannelObjectType: Device\r\nDeviceStatus: %s\r\nSCCPDevice: %s\r\n", "REGISTERED", DEV_ID_LOG(device));
			break;

		case SCCP_EVENT_DEVICE_UNREGISTERED:
			device = event->deviceRegistered.device;						// already retained in the event
			manager_event(EVENT_FLAG_CALL, "DeviceStatus", "ChannelType: SCCP\r\nChannelObjectType: Device\r\nDeviceStatus: %s\r\nSCCPDevice: %s\r\n", "UNREGISTERED", DEV_ID_LOG(device));
			break;

		case SCCP_EVENT_DEVICE_PREREGISTERED:
			device = event->deviceRegistered.device;						// already retained in the event
			manager_event(EVENT_FLAG_CALL, "DeviceStatus", "ChannelType: SCCP\r\nChannelObjectType: Device\r\nDeviceStatus: %s\r\nSCCPDevice: %s\r\n", "PREREGISTERED", DEV_ID_LOG(device));
			break;

		case SCCP_EVENT_DEVICE_ATTACHED:
			device = event->deviceAttached.ld->device;                                        // already retained in the event
			ld = event->deviceAttached.ld;                                                    // already retained in the event
			manager_event(EVENT_FLAG_CALL, "PeerStatus",
				      "ChannelType: SCCP\r\nChannelObjectType: DeviceLine\r\nPeerStatus: %s\r\nSCCPDevice: %s\r\nSCCPLine: %s\r\nSCCPLineName: %s\r\nSubscriptionId: %s\r\nSubscriptionName: %s\r\n", "ATTACHED",
				      DEV_ID_LOG(device), ld && ld->line ? ld->line->name : "(null)", (ld && ld->line && ld->line->label) ? ld->line->label : "(null)", ld->subscriptionId.number, ld->subscriptionId.name);
			break;

		case SCCP_EVENT_DEVICE_DETACHED:
			device = event->deviceAttached.ld->device;                                        // already retained in the event
			ld = event->deviceAttached.ld;                                                    // already retained in the event
			manager_event(EVENT_FLAG_CALL, "PeerStatus",
				      "ChannelType: SCCP\r\nChannelObjectType: DeviceLine\r\nPeerStatus: %s\r\nSCCPDevice: %s\r\nSCCPLine: %s\r\nSCCPLineName: %s\r\nSubscriptionId: %s\r\nSubscriptionName: %s\r\n", "DETACHED",
				      DEV_ID_LOG(device), ld && ld->line ? ld->line->name : "(null)", (ld && ld->line && ld->line->label) ? ld->line->label : "(null)", ld->subscriptionId.number, ld->subscriptionId.name);
			break;

		case SCCP_EVENT_FEATURE_CHANGED:
			device = event->featureChanged.device;						// already retained in the event
			ld = event->featureChanged.optional_linedevice;                                        // either NULL or already retained in the event
			sccp_feature_type_t featureType = event->featureChanged.featureType;
			sccp_cfwd_t cfwd_type = SCCP_CFWD_NONE;

			switch(featureType) {
				case SCCP_FEATURE_DND:
					manager_event(EVENT_FLAG_CALL, "DND", "ChannelType: SCCP\r\nChannelObjectType: Device\r\nFeature: %s\r\nStatus: %s\r\nSCCPDevice: %s\r\n", sccp_feature_type2str(SCCP_FEATURE_DND), sccp_dndmode2str((sccp_dndmode_t)device->dndFeature.status), DEV_ID_LOG(device));
					break;
				case SCCP_FEATURE_CFWDALL:
					cfwd_type = SCCP_CFWD_ALL;
					break;
				case SCCP_FEATURE_CFWDBUSY:
					cfwd_type = SCCP_CFWD_BUSY;
					break;
				case SCCP_FEATURE_CFWDNOANSWER:
					cfwd_type = SCCP_CFWD_NOANSWER;
					break;
				case SCCP_FEATURE_CFWDNONE:
					cfwd_type = SCCP_CFWD_NONE;
					manager_event(EVENT_FLAG_CALL, "CallForward", "ChannelType: SCCP\r\nChannelObjectType: DeviceLine\r\nFeature: %s\r\nStatus: Off\r\nSCCPLine: %s\r\nSCCPDevice: %s\r\n",
						      sccp_feature_type2str(featureType), (ld && ld->line) ? ld->line->name : "(null)", DEV_ID_LOG(device));
					break;
				default:
					break;
			}
			if(ld && cfwd_type != SCCP_CFWD_NONE) {
				manager_event(EVENT_FLAG_CALL, "CallForward", "ChannelType: SCCP\r\nChannelObjectType: DeviceLine\r\nFeature: %s\r\nStatus: %s\r\nExtension: %s\r\nSCCPLine: %s\r\nSCCPDevice: %s\r\n",
					      sccp_feature_type2str(featureType), ld->cfwd[cfwd_type].enabled ? "On" : "Off", ld->cfwd[cfwd_type].number, (ld->line) ? ld->line->name : "(null)", DEV_ID_LOG(device));
			}

			break;

		default:
			break;
	}
}

#if HAVE_PBX_MANAGER_HOOK_H
static char * sccp_asterisk_parseStrToAstMessage(char *str, struct message *m)
{
	int x = 0;
	int curlen = 0;

	curlen = sccp_strlen(str);
	for (x = 0; x < curlen; x++) {
		int cr = 0;

		if (str[x] == '\r' && x + 1 < curlen && str[x + 1] == '\n') {
			cr = 2;
		} else if (str[x] == '\n') {
			cr = 1;											/* also accept \n only */
		} else {
			continue;
		}
		if (x && m->hdrcount < ARRAY_LEN(m->headers)) {
			str[x] = '\0';
			m->headers[m->hdrcount++] = str;
		}
		x += cr;
		curlen -= x;
		str += x;
		x = -1;
	}
	return str;
}

static int sccp_asterisk_managerHookHelper(int category, const char *event, char *content)
{
	char * str = NULL;

	char * dupStr = NULL;

	if (EVENT_FLAG_CALL == category) {
		if (!strcasecmp("MonitorStart", event) || !strcasecmp("MonitorStop", event)) {
			AUTO_RELEASE(sccp_channel_t, channel , NULL);
			struct message m = { 0 };

			str = dupStr = pbx_strdupa(content);
			sccp_log(DEBUGCAT_CORE)("SCCP: AMI MonitorStart/MonitorStop received:\n[%s]\n", content);

			sccp_asterisk_parseStrToAstMessage(str, &m);
			const char *channelName = astman_get_header(&m, "Channel");

			PBX_CHANNEL_TYPE *pbxchannel = pbx_channel_get_by_name(channelName);
			if (pbxchannel) {
				PBX_CHANNEL_TYPE *pbxBridge = NULL;
				if ((CS_AST_CHANNEL_PVT_IS_SCCP(pbxchannel))) {
					channel = get_sccp_channel_from_pbx_channel(pbxchannel) /*ref_replace*/;
				} else if ( (pbxBridge = pbx_channel_get_by_name(pbx_builtin_getvar_helper(pbxchannel, "BRIDGEPEER"))) ) {
					if ((CS_AST_CHANNEL_PVT_IS_SCCP(pbxBridge))) {
						channel = get_sccp_channel_from_pbx_channel(pbxBridge) /*ref_replace*/;
					}
					pbxBridge = ast_channel_unref(pbxBridge);
				}
				pbxchannel = ast_channel_unref(pbxchannel);
			}

			if (channel) {
				sccp_log(DEBUGCAT_CORE)("%s: AMI MonitorStart/MonitorStop received\n", channel->designator);
				AUTO_RELEASE(sccp_device_t, d , sccp_channel_getDevice(channel));
				if (d) {
					sccp_log(DEBUGCAT_CORE)("%s: AMI MonitorStart/MonitorStop for %s\n", channel->designator, d->id);
					if (!strcasecmp("MonitorStart", event)) {
						d->monitorFeature.status |= SCCP_FEATURE_MONITOR_STATE_ACTIVE;
					} else {
						d->monitorFeature.status &= ~SCCP_FEATURE_MONITOR_STATE_ACTIVE;
					}
					sccp_msg_t *msg_out = NULL;
					REQ(msg_out, RecordingStatusMessage);
					if (!msg_out) {
						return -1;
					}
					msg_out->data.RecordingStatusMessage.lel_callReference = htolel(channel->callid);
					msg_out->data.RecordingStatusMessage.lel_status = (d->monitorFeature.status & SCCP_FEATURE_MONITOR_STATE_ACTIVE) ? htolel(1) : htolel(0);
					sccp_dev_send(d, msg_out);

					sccp_feat_changed(d, NULL, SCCP_FEATURE_MONITOR);
				}
			}
#ifdef CS_SCCP_PARK
		} else if (sccp_strcaseequals("ParkedCall", event) || sccp_strcaseequals("UnParkedCall", event) || sccp_strcaseequals("ParkedCallGiveUp", event) || sccp_strcaseequals("ParkedCallTimeout", event)) {
			if (iParkingLot.addSlot && iParkingLot.removeSlot) {
				sccp_log_and((DEBUGCAT_PARKINGLOT & DEBUGCAT_HIGH))("SCCP: AMI event %s received:\n[%s]\n", event, content);

				str = dupStr = pbx_strdupa(content);
				struct message m = { 0 };
				sccp_asterisk_parseStrToAstMessage(str, &m);

				const char *parkinglot = astman_get_header(&m, "Parkinglot");
				const char *extension = astman_get_header(&m, PARKING_SLOT);
				int exten = sccp_atoi(extension, strlen(extension));

				if (parkinglot && exten) {
					if (sccp_strcaseequals("ParkedCall", event)) {
						iParkingLot.addSlot(parkinglot, exten, &m);
					} else {
						iParkingLot.removeSlot(parkinglot, exten);
					}
				}
			}
#endif
		}
	}
	return 0;
}

AST_THREADSTORAGE(hookresult_threadbuf);
#define HOOKRESULT_INITSIZE DEFAULT_PBX_STR_BUFFERSIZE*2

static int __sccp_manager_hookresult(int category, const char *event, char *content) {
        struct ast_str *buf = ast_str_thread_get(&hookresult_threadbuf, HOOKRESULT_INITSIZE);;
	if (buf) {
		pbx_str_append(&buf, 0, "%s", content);
	}
	return 0;
}
/* outStr is allocated on success and must be freed by the caller; returns -1 on failure, otherwise the called function's result */
boolean_t sccp_manager_action2str(const char *manager_command, char **outStr)
{
        int failure = 0;
	struct ast_str * buf = NULL;

	if(!outStr || sccp_strlen_zero(manager_command) || !(buf = ast_str_thread_get(&hookresult_threadbuf, HOOKRESULT_INITSIZE))) {
		pbx_log(LOG_ERROR, "SCCP: manager command hook called without a command or output buffer (caller bug)\n");
        	return -2;
	}

	struct manager_custom_hook hook = {__FILE__, __sccp_manager_hookresult};
        failure = ast_hook_send_action(&hook, manager_command);
        if (!failure) {
		sccp_log(DEBUGCAT_CORE)("SCCP: AMI result: %s\n", pbx_str_buffer(buf));
        	*outStr = pbx_strdup(pbx_str_buffer(buf));
        }
       	ast_str_reset(buf);
        return !failure ? TRUE : FALSE;
}

/*
<response type='object' id='(null)'><(null) response='Success' message='Parked calls will follow' /></response>
<response type='object' id='(null)'><(null) event='ParkedCall' parkinglot='default' exten='701' channel='IAX2/iaxuser-2343' from='SCCP/98031-00000001' timeout='41' duration='4' calleridnum='100011' calleridname='Diederik de Groot (10001)' connectedlinenum='' connectedlinename='' /></response>
<response type='object' id='(null)'><(null) event='ParkedCallsComplete' total='1' /></response>
*/

#if defined(CS_EXPERIMENTAL)
char * sccp_manager_retrieve_parkedcalls_cxml(char ** out)
{
	char *parkedcalls_messageStr = NULL;
	char *manager_command = "Action: ParkedCalls\r\n";

	if (sccp_manager_action2str(manager_command, &parkedcalls_messageStr) && parkedcalls_messageStr) {
		pbx_str_t *tmpPbxStr = ast_str_create(DEFAULT_PBX_STR_BUFFERSIZE);
		struct message m = {0};
		const char *event = "";

		sccp_log(DEBUGCAT_CORE)(VERBOSE_PREFIX_2 "SCCP: parked calls: %s\n",  parkedcalls_messageStr);
		pbx_str_append(&tmpPbxStr, 0, "<?xml version=\"1.0\"?>");
		pbx_str_append(&tmpPbxStr, 0, "<CiscoIPPhoneDirectory>");
		pbx_str_append(&tmpPbxStr, 0, "<Title>Parked Calls</Title>");
		pbx_str_append(&tmpPbxStr, 0, "<Prompt>Please Choose on of the parking lots</Prompt>");
		pbx_str_append(&tmpPbxStr, 0, "<DirectoryEntry>");
		char *strptr = parkedcalls_messageStr;
		char *token = NULL;
		char *rest = strptr;
		while (sscanf(strptr, "%[^\r\n]\r\n\r\n%s", token, rest) && token) {
			sccp_log(DEBUGCAT_CORE)(VERBOSE_PREFIX_2 "SCCP: parked calls: token '%s', rest '%s'\n", token, rest);
			usleep(500);

			token = sccp_asterisk_parseStrToAstMessage(token, &m);
			event = astman_get_header(&m, "Event");
			if (sccp_strcaseequals(event, "ParkedCallsComplete")) {
				break;
			} else if(sccp_strcaseequals(event, "ParkedCall")) {
				pbx_str_append(&tmpPbxStr, 0, "<Name>%s (%s) by %s</Name><Telephone>%s</Telephone>",
					astman_get_header((const struct message *)&m, "CallerIdName"),
					astman_get_header((const struct message *)&m, "CallerIdNum"),
					astman_get_header((const struct message *)&m, "ConnectedLineName"),
					astman_get_header((const struct message *)&m, "Exten")
				);
				sccp_log(DEBUGCAT_CORE)(VERBOSE_PREFIX_3 "SCCP: parked call %s on %s@%s\n", astman_get_header((const struct message *)&m, "Channel"), astman_get_header((const struct message *)&m, "Exten"), astman_get_header((const struct message *)&m, "ParkingLot"));
			}
			memset(&m, 0, sizeof(m));
			strptr = rest;
		}
		pbx_str_append(&tmpPbxStr, 0, "</DirectoryEntry>");
		pbx_str_append(&tmpPbxStr, 0, "</CiscoIPPhoneDirectory>");

		*out = pbx_strdup(pbx_str_buffer(tmpPbxStr));

		sccp_free(tmpPbxStr);
		sccp_free(parkedcalls_messageStr);
	}
	sccp_log(DEBUGCAT_CORE)(VERBOSE_PREFIX_2 "SCCP: parked calls XML: %s\n", *out);
	return *out;
}
#endif
#endif														// HAVE_PBX_MANAGER_HOOK_H
#endif														// CS_SCCP_MANAGER
