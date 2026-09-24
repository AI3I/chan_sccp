/*!
 * \file        sccp_pbx.c
 * \brief       SCCP PBX Asterisk Wrapper Class
 * \author      Diederik de Groot <ddegroot [at] users.sourceforge.net>
 * \note        Reworked, but based on chan_sccp code.
 *              The original chan_sccp driver that was made by Zozo which itself was derived from the chan_skinny driver.
 *              Modified by Jan Czmok and Julien Goodwin
 * \note        This program is free software and may be modified and distributed under the terms of the GNU Public License.
 *              See the LICENSE file at the top of the source tree.
 *
 */
#include "config.h"
#include "common.h"
#include "sccp_pbx.h"
#include "sccp_channel.h"
#include "sccp_device.h"
#include "sccp_conference.h"
#include "sccp_feature.h"
#include "sccp_line.h"
#include "sccp_utils.h"
#include "sccp_indicate.h"
#include "sccp_linedevice.h"
#include "sccp_rtp.h"
#include "sccp_netsock.h"
#include "sccp_session.h"
#include "sccp_atomic.h"
#include "sccp_labels.h"
#include "sccp_threadpool.h"

SCCP_FILE_VERSION(__FILE__, "");

#include <asterisk/callerid.h>
#include <asterisk/module.h>
#include <asterisk/causes.h>

sccp_channel_request_status_t sccp_requestChannel(const char * lineName, sccp_autoanswer_t autoanswer_type, uint8_t autoanswer_cause, skinny_ringtype_t ringermode, sccp_channel_t * const * channel)
{
	if (!lineName) {
		return SCCP_REQUEST_STATUS_ERROR;
	}

	char mainId[SCCP_MAX_EXTENSION];
	sccp_subscription_id_t subscriptionId;
	if(!sccp_parseComposedId(lineName, 80, &subscriptionId, mainId)) {
		sccp_log((DEBUGCAT_CORE))(VERBOSE_PREFIX_3 "line name %s could not be parsed\n", lineName);
		return SCCP_REQUEST_STATUS_LINEUNKNOWN;
	};

	AUTO_RELEASE(sccp_line_t, l, sccp_line_find_byname(mainId, FALSE));
	if (!l) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP/%s does not exist\n", mainId);
		return SCCP_REQUEST_STATUS_LINEUNKNOWN;
	}
	sccp_log_and((DEBUGCAT_CORE + DEBUGCAT_HIGH)) (VERBOSE_PREFIX_1 "called from %s:%d (%s)\n", __FILE__, __LINE__, __PRETTY_FUNCTION__);
	if (SCCP_RWLIST_GETSIZE(&l->devices) == 0) {
		sccp_log((DEBUGCAT_DEVICE + DEBUGCAT_LINE)) (VERBOSE_PREFIX_3 "SCCP/%s is not registered on any phone\n", l->name);
		return SCCP_REQUEST_STATUS_LINEUNAVAIL;
	}
	sccp_log_and((DEBUGCAT_CORE + DEBUGCAT_HIGH)) (VERBOSE_PREFIX_1 "called from %s:%d (%s)\n", __FILE__, __LINE__, __PRETTY_FUNCTION__);

	AUTO_RELEASE(sccp_channel_t, my_sccp_channel, sccp_channel_allocate(l, NULL));
	if (!my_sccp_channel) {
		return SCCP_REQUEST_STATUS_ERROR;
	}

	if (!sccp_strlen_zero(subscriptionId.number)) {
		sccp_copy_string(my_sccp_channel->subscriptionId.number, subscriptionId.number, sizeof(my_sccp_channel->subscriptionId.number));
		if (!sccp_strlen_zero(subscriptionId.name)) {
			sccp_copy_string(my_sccp_channel->subscriptionId.name, subscriptionId.name, sizeof(my_sccp_channel->subscriptionId.name));
		} else {
		}
	} else {
		sccp_copy_string(my_sccp_channel->subscriptionId.number, l->defaultSubscriptionId.number, sizeof(my_sccp_channel->subscriptionId.number));
		sccp_copy_string(my_sccp_channel->subscriptionId.name, l->defaultSubscriptionId.name, sizeof(my_sccp_channel->subscriptionId.name));
	}

	my_sccp_channel->autoanswer_type = autoanswer_type;
	my_sccp_channel->autoanswer_cause = autoanswer_cause;
	my_sccp_channel->ringermode = ringermode;
	my_sccp_channel->hangupRequest = sccp_astgenwrap_requestQueueHangup;
	(*(sccp_channel_t **)channel) = sccp_channel_retain(my_sccp_channel);
	return SCCP_REQUEST_STATUS_SUCCESS;
}

struct sccp_answer_conveyor_struct {
	sccp_linedevice_t * ld;
	uint32_t callid;
};
/* The Auto Answer thread is started by ref sccp_pbx_call if necessary */
static void *sccp_pbx_call_autoanswer_thread(void *data)
{
	struct sccp_answer_conveyor_struct *conveyor = (struct sccp_answer_conveyor_struct *)data;

	sccp_log((DEBUGCAT_PBX))(VERBOSE_PREFIX_3 "SCCP: autoanswer thread started\n");

	sleep(GLOB(autoanswer_ring_time));
	pthread_testcancel();

	if (!conveyor) {
		pbx_log(LOG_ERROR, "SCCP: auto-answer thread started without its call data (caller bug)\n");
		return NULL;
	}
	if(!conveyor->ld) {
		pbx_log(LOG_WARNING, "SCCP: auto-answer of call %d skipped: its line is no longer on the device\n", conveyor->callid);
		goto FINAL;
	}

	{
		AUTO_RELEASE(sccp_device_t, device, sccp_device_retain(conveyor->ld->device));

		if (!device) {
			pbx_log(LOG_NOTICE, "SCCP: auto-answer of call %d skipped: the device is gone\n", conveyor->callid);
			goto FINAL;
		}

		AUTO_RELEASE(sccp_channel_t, c , sccp_channel_find_byid(conveyor->callid));

		if (!c || c->state != SCCP_CHANNELSTATE_RINGING) {
			sccp_log((DEBUGCAT_PBX))(VERBOSE_PREFIX_3 "%s: auto-answer of call %d skipped: %s\n", device->id, conveyor->callid, c ? "it is no longer ringing" : "it ended during the auto-answer delay");
			goto FINAL;
		}
		if (c->pbx_callid) {
			pbx_callid_threadassoc_add(c->pbx_callid);
		}

		sccp_channel_answer(device, c);
		c->setTone(c, GLOB(autoanswer_tone), SKINNY_TONEDIRECTION_USER);
		if (c->autoanswer_type == SCCP_AUTOANSWER_1W) {
			sccp_dev_set_microphone(device, SKINNY_STATIONMIC_OFF);
		}
		if (c->pbx_callid) {
			pbx_callid_threadassoc_remove();
		}
	}
FINAL:
	if(conveyor->ld) {
		sccp_linedevice_release(&conveyor->ld);                                        // retained in calling thread, explicit release required here
	}
	sccp_free(conveyor);
	return NULL;
}

/* called with c retained */
// - handle dnd and callforward after calculating c->subscribers
int sccp_pbx_call(channelPtr c, const char * dest, int timeout)
{
	if (!c) {
		return -1;
	}
	int res = 0;

	AUTO_RELEASE(sccp_line_t, l , sccp_line_retain(c->line));
	if (!l) {
		pbx_log(LOG_WARNING, "SCCP: incoming call %08X not delivered: it has no line\n", c->callid);
		return -1;
	}

	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: Asterisk calls %s\n", l->name, iPbx.getChannelName(c));

	/* Reinstated this call instead of the following lines */
	char cid_name[StationMaxNameSize] = {0};
	char cid_num[StationMaxDirnumSize] = {0};
	char suffixedNumber[StationMaxDirnumSize] = {0};
	sccp_callerid_presentation_t presentation = CALLERID_PRESENTATION_ALLOWED;

	sccp_callinfo_t *ci = sccp_channel_getCallInfo(c);
	iCallInfo.Getter(ci,
		SCCP_CALLINFO_CALLINGPARTY_NAME, &cid_name,
		SCCP_CALLINFO_CALLINGPARTY_NUMBER, &cid_num,
		SCCP_CALLINFO_PRESENTATION, &presentation,
		SCCP_CALLINFO_KEY_SENTINEL);
	sccp_copy_string(suffixedNumber, cid_num, sizeof(suffixedNumber));
	sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "SCCP: caller ID '%s <%s>'\n", cid_name, cid_num);

	if (GLOB(recorddigittimeoutchar)) {
		/* The hack to add the # at the end of the incoming number
		   is only applied for numbers beginning with a 0,
		   which is appropriate for Germany and other countries using similar numbering plan.
		   The option should be generalized, moved to the dialplan, or otherwise be replaced. */
		/*
		 * Also, we require an option whether to add the timeout suffix to certain enbloc dialed numbers (such as via 7970 enbloc dialing) if they match a certain pattern.
		 */
		int length = sccp_strlen(cid_num);
		if (length && (length + 2  < StationMaxDirnumSize) && ('\0' == cid_num[0])) {
			suffixedNumber[length + 0] = GLOB(digittimeoutchar);
			suffixedNumber[length + 1] = '\0';
		}
	}
	sccp_callerid_presentation_t pbx_presentation = iPbx.get_callerid_presentation ? iPbx.get_callerid_presentation(c->owner) : SCCP_CALLERID_PRESENTATION_SENTINEL;
	if (	(!sccp_strequals(suffixedNumber, cid_num)) ||
		(pbx_presentation != SCCP_CALLERID_PRESENTATION_SENTINEL && pbx_presentation != presentation)
	) {
		iCallInfo.Setter(ci,
			SCCP_CALLINFO_CALLINGPARTY_NUMBER, (!sccp_strlen_zero(suffixedNumber) ? suffixedNumber : NULL),
			SCCP_CALLINFO_PRESENTATION, (pbx_presentation != SCCP_CALLERID_PRESENTATION_SENTINEL) ? pbx_presentation : presentation,
			SCCP_CALLINFO_KEY_SENTINEL);
	}
	sccp_channel_display_callInfo(c);

	if (!c->ringermode) {
		c->ringermode = GLOB(ringtype);
	}
	boolean_t isRinging = FALSE;
	boolean_t isBusy = FALSE;
	boolean_t bypassCallForward = !sccp_strlen_zero(pbx_builtin_getvar_helper(c->owner, "BYPASS_CFWD"));
	sccp_linedevice_t * ForwardingLineDevice = NULL;

	sccp_linedevice_t * ld = NULL;
	sccp_channelstate_t previousstate = c->previousChannelState;

	SCCP_LIST_LOCK(&l->devices);
	int num_devices = SCCP_LIST_GETSIZE(&l->devices);
	c->subscribers = num_devices;

	pbx_str_t * cfwds_all = pbx_str_alloca(DEFAULT_PBX_STR_BUFFERSIZE);
	pbx_str_t * cfwds_busy = pbx_str_alloca(DEFAULT_PBX_STR_BUFFERSIZE);
	pbx_str_t * cfwds_noanswer = pbx_str_alloca(DEFAULT_PBX_STR_BUFFERSIZE);
	int cfwd_all = 0;
	int cfwd_busy = 0;
	int cfwd_noanswer = 0;
	SCCP_LIST_TRAVERSE(&l->devices, ld, list) {
		AUTO_RELEASE(sccp_channel_t, active_channel, sccp_device_getActiveChannel(ld->device));

		if (active_channel && active_channel != c && sccp_strequals(iPbx.getChannelLinkedId(active_channel), iPbx.getChannelLinkedId(c))) {
			sccp_log((DEBUGCAT_PBX))(VERBOSE_PREFIX_3 "SCCP: not ringing %s\n", ld->device->id);
			c->subscribers--;
			continue;
		}

		if(ld->cfwd[SCCP_CFWD_ALL].enabled) {
			ast_str_append(&cfwds_all, DEFAULT_PBX_STR_BUFFERSIZE, "%s%s", cfwd_all++ ? "," : "", ld->cfwd[SCCP_CFWD_ALL].number);
		}
		if(ld->cfwd[SCCP_CFWD_BUSY].enabled) {
			ast_str_append(&cfwds_busy, DEFAULT_PBX_STR_BUFFERSIZE, "%s%s", cfwd_busy++ ? "," : "", ld->cfwd[SCCP_CFWD_BUSY].number);
		}
		if(!bypassCallForward
		   && (ld->cfwd[SCCP_CFWD_ALL].enabled || (ld->cfwd[SCCP_CFWD_BUSY].enabled && (sccp_device_getDeviceState(ld->device) != SCCP_DEVICESTATE_ONHOOK || sccp_device_getActiveAccessory(ld->device))))) {
			if (num_devices == 1) {
				/* when single line -> use asterisk functionality directly, without creating new channel + masquerade */
				sccp_log((DEBUGCAT_CORE))(VERBOSE_PREFIX_3 "%s: call forward is active on line %s\n", ld->device->id, ld->line->name);
				ForwardingLineDevice = ld;
			} else {
				pbx_log(LOG_NOTICE, "%s: forwarding call on shared line %s to %s\n", ld->device->id, l->name, ld->cfwd[SCCP_CFWD_ALL].enabled ? ld->cfwd[SCCP_CFWD_ALL].number : ld->cfwd[SCCP_CFWD_BUSY].number);
				if(sccp_channel_forward(c, ld, ld->cfwd[SCCP_CFWD_ALL].enabled ? ld->cfwd[SCCP_CFWD_ALL].number : ld->cfwd[SCCP_CFWD_BUSY].number) == 0) {
					sccp_device_sendcallstate(ld->device, ld->lineInstance, c->callid, SKINNY_CALLSTATE_INTERCOMONEWAY, SKINNY_CALLPRIORITY_NORMAL, SKINNY_CALLINFO_VISIBILITY_DEFAULT);
					sccp_channel_send_callinfo(ld->device, c);
					isRinging = TRUE;
				}
			};
			c->subscribers--;
			continue;
		}

		if(ld->cfwd[SCCP_CFWD_NOANSWER].enabled && c) {
			sccp_channel_schedule_cfwd_noanswer(c, GLOB(cfwdnoanswer_timeout));
			ast_str_append(&cfwds_noanswer, DEFAULT_PBX_STR_BUFFERSIZE, "%s%s", cfwd_noanswer++ ? "," : "", ld->cfwd[SCCP_CFWD_NOANSWER].number);
		}

		if(!ld->device->session) {
			sccp_log((DEBUGCAT_CORE))(VERBOSE_PREFIX_3 "%s: line device has no session\n", DEV_ID_LOG(ld->device));
			c->subscribers--;
			continue;
		}
		/* This means that we call only those devices on a shared line
		   which match the specified subscription id in the dial parameters. */
		if(!sccp_util_matchSubscriptionId(c, ld->subscriptionId.number)) {
			sccp_log((DEBUGCAT_PBX))(VERBOSE_PREFIX_3 "%s: device skipped: call subscription '%s' does not match the device subscription '%s'\n", DEV_ID_LOG(ld->device),
						 c->subscriptionId.number, ld->subscriptionId.number);
			c->subscribers--;
			continue;
		}
		/* reset channel state (because we are offering the same call to multiple (shared) lines)*/
		c->previousChannelState = previousstate;

		int calls = sccp_getCallCount(ld);
		sccp_log((DEBUGCAT_PBX))(VERBOSE_PREFIX_3 "%s: %d calls, incoming call limit %d\n", l->name, calls, l->incominglimit);
		if(l->incominglimit && calls > l->incominglimit) {
			sccp_log((DEBUGCAT_CORE))(VERBOSE_PREFIX_3 "%s: %d calls is over the incoming call limit %d; busy\n", l->name, calls, l->incominglimit);
			isBusy = TRUE;
			c->subscribers--;
			continue;
		}

		if (active_channel) {
			sccp_indicate(ld->device, c, SCCP_CHANNELSTATE_CALLWAITING);
			AUTO_RELEASE(sccp_linedevice_t, activeChannelLinedevice, active_channel->getLineDevice(active_channel));
			if (activeChannelLinedevice) {
				char caller[100] = {0};
				if(!sccp_strlen_zero(cid_name)) {
					if(!sccp_strlen_zero(cid_num)) {
						snprintf(caller,sizeof(caller), "%s %s <%s>", SKINNY_DISP_CALL_WAITING, cid_name, cid_num);
					} else {
						snprintf(caller,sizeof(caller), "%s %s", SKINNY_DISP_CALL_WAITING, cid_name);
					}
				} else {
					if (!sccp_strlen_zero(cid_num)) {
						snprintf(caller,sizeof(caller), "%s %s", SKINNY_DISP_CALL_WAITING, cid_num);
					} else {
						snprintf(caller,sizeof(caller), "%s %s", SKINNY_DISP_CALL_WAITING, SKINNY_DISP_UNKNOWN_NUMBER);
					}
				}
				sccp_dev_set_message(ld->device, caller, SCCP_DISPLAYSTATUS_TIMEOUT, FALSE, FALSE);
			}
			ForwardingLineDevice = NULL;
			isRinging = TRUE;
		} else {
			if(SKINNY_RINGTYPE_URGENT != c->ringermode && ld->device->dndFeature.enabled && ld->device->dndFeature.status == SCCP_DNDMODE_REJECT) {
				sccp_log((DEBUGCAT_CORE))(VERBOSE_PREFIX_3 "%s: DND is on for line %s; busy\n", ld->device->id, ld->line->name);
				isBusy = TRUE;
				c->subscribers--;
				continue;
			}
			ForwardingLineDevice = NULL;
			sccp_log(DEBUGCAT_PBX)(VERBOSE_PREFIX_3 "%s: ringing %sline %s on %s, call %s, ringer %s\n", ld->device->id, SCCP_LIST_GETSIZE(&l->devices) > 1 ? "Shared" : "", ld->line->name,
					       ld->device->id, c->designator, skinny_ringtype2str(c->ringermode));
			sccp_indicate(ld->device, c, SCCP_CHANNELSTATE_RINGING);
			isRinging = TRUE;
			if (c->autoanswer_type) {
				struct sccp_answer_conveyor_struct *conveyor = (struct sccp_answer_conveyor_struct *)sccp_calloc(1, sizeof(struct sccp_answer_conveyor_struct));
				if (conveyor) {
					sccp_log((DEBUGCAT_CORE))(VERBOSE_PREFIX_3 "%s: starting auto-answer for %s\n", DEV_ID_LOG(ld->device), iPbx.getChannelName(c));
					conveyor->callid = c->callid;
					conveyor->ld = sccp_linedevice_retain(ld);

					if (!sccp_threadpool_add_work(GLOB(general_threadpool), sccp_pbx_call_autoanswer_thread, conveyor)) {
						sccp_linedevice_release(&conveyor->ld);
						sccp_free(conveyor);
					}
				} else {
					pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, c->designator);
				}
			}
		}
	}
	SCCP_LIST_UNLOCK(&l->devices);

	if(cfwd_all) {
		pbx_builtin_setvar_helper(c->owner, "_CFWD_ALL", pbx_str_buffer(cfwds_all));
	}
	if(cfwd_busy) {
		pbx_builtin_setvar_helper(c->owner, "_CFWD_BUSY", pbx_str_buffer(cfwds_busy));
	}
	if(cfwd_noanswer) {
		pbx_builtin_setvar_helper(c->owner, "_CFWD_NOANSWER", pbx_str_buffer(cfwds_noanswer));
	}
	if (isRinging) {
		sccp_channel_setChannelstate(c, SCCP_CHANNELSTATE_RINGING);
		iPbx.set_callstate(c, AST_STATE_RINGING);
		iPbx.queue_control(c->owner, AST_CONTROL_RINGING);
	} else if (ForwardingLineDevice) {
		/* when single line -> use asterisk functionality directly, without creating new channel + masquerade */
		pbx_log(LOG_NOTICE, "%s: forwarding call on line %s to %s\n", ForwardingLineDevice->device->id, l->name,
			ForwardingLineDevice->cfwd[SCCP_CFWD_ALL].enabled ? ForwardingLineDevice->cfwd[SCCP_CFWD_ALL].number : ForwardingLineDevice->cfwd[SCCP_CFWD_BUSY].number);
#if CS_AST_CONTROL_REDIRECTING
		iPbx.queue_control(c->owner, AST_CONTROL_REDIRECTING);
#endif
		pbx_channel_call_forward_set(c->owner, ForwardingLineDevice->cfwd[SCCP_CFWD_ALL].enabled ? ForwardingLineDevice->cfwd[SCCP_CFWD_ALL].number : ForwardingLineDevice->cfwd[SCCP_CFWD_BUSY].number);
		sccp_device_sendcallstate(ForwardingLineDevice->device, ForwardingLineDevice->lineInstance, c->callid, SKINNY_CALLSTATE_INTERCOMONEWAY, SKINNY_CALLPRIORITY_NORMAL, SKINNY_CALLINFO_VISIBILITY_DEFAULT);
		sccp_channel_send_callinfo(ForwardingLineDevice->device, c);
	} else if(isBusy) {
		iPbx.queue_control(c->owner, AST_CONTROL_BUSY);
		pbx_channel_set_hangupcause(c->owner, AST_CAUSE_BUSY);
		res = 0;
	} else {
		iPbx.queue_control(c->owner, AST_CONTROL_CONGESTION);
		res = -1;
	}

	PBX_VARIABLE_TYPE *v = l->variables;
	while (c->owner && !pbx_check_hangup(c->owner) && l && v) {
		pbx_builtin_setvar_helper(c->owner, v->name, v->value);
		v = v->next;
	}

	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: call result %d\n", c->designator, res);
	return res;
}

int sccp_pbx_cfwdnoanswer_cb(const void * data)
{
	AUTO_RELEASE(sccp_channel_t, c, (channelPtr)data);
	if(!c || !c->owner) {
		pbx_log(LOG_NOTICE, "SCCP: forward on no answer skipped: the call or its Asterisk channel is gone\n");
		return -1;
	}

	if((ATOMIC_FETCH(&c->scheduler.deny, &c->scheduler.lock) == 0)) {
		c->scheduler.cfwd_noanswer_id = -3; /* prevent further cfwd_noanswer_id scheduling */
	}

	AUTO_RELEASE(sccp_line_t, l, sccp_line_retain(c->line));
	if(!l) {
		pbx_log(LOG_WARNING, "%s: forward on no answer skipped: the call has no line\n", c->designator);
		return -2;
	}

	if (sccp_channel_isAnswering(c)) {
		sccp_log(DEBUGCAT_CHANNEL)(VERBOSE_PREFIX_2 "%s: no answer forward cancelled: the call is being answered\n", c->designator);
		return -3;
	}

	pbx_channel_lock(c->owner);
	PBX_CHANNEL_TYPE * forwarder = pbx_channel_ref(c->owner);
	pbx_channel_unlock(c->owner);

	if(!forwarder || c->isHangingUp || pbx_check_hangup_locked(forwarder)) {
		sccp_log((DEBUGCAT_PBX))(VERBOSE_PREFIX_3 "%s: forward on no answer skipped: the call was already hung up\n", c->designator);
		pbx_channel_unref(forwarder);
		return -4;
	}

	boolean_t bypassCallForward = !sccp_strlen_zero(pbx_builtin_getvar_helper(forwarder, "BYPASS_CFWD"));

	sccp_linedevice_t * ld = NULL;
	SCCP_LIST_LOCK(&l->devices);
	int num_devices = SCCP_LIST_GETSIZE(&l->devices);
	SCCP_LIST_TRAVERSE(&l->devices, ld, list) {
		if(ld->cfwd[SCCP_CFWD_NOANSWER].enabled && !sccp_strlen_zero(ld->cfwd[SCCP_CFWD_NOANSWER].number) && !bypassCallForward) {
			if(num_devices == 1) {
				/* when single line -> use asterisk functionality directly, without creating new channel + masquerade */
				sccp_log(DEBUGCAT_PBX)("%s: forwarding to no answer destination %s\n", c->designator, ld->cfwd[SCCP_CFWD_NOANSWER].number);
#if CS_AST_CONTROL_REDIRECTING
				iPbx.queue_control(forwarder, AST_CONTROL_REDIRECTING);
#endif
				pbx_channel_call_forward_set(forwarder, ld->cfwd[SCCP_CFWD_NOANSWER].number);
				sccp_device_sendcallstate(ld->device, ld->lineInstance, c->callid, SKINNY_CALLSTATE_INTERCOMONEWAY, SKINNY_CALLPRIORITY_NORMAL, SKINNY_CALLINFO_VISIBILITY_DEFAULT);
				sccp_channel_send_callinfo(ld->device, c);
				iPbx.set_owner(c, NULL);
				break;
			} else {
				pbx_log(LOG_NOTICE, "%s: call on line %s not answered in time; forwarding to %s\n", ld->device->id, l->name, ld->cfwd[SCCP_CFWD_NOANSWER].number);
				sccp_channel_forward(c, ld, ld->cfwd[SCCP_CFWD_NOANSWER].number);
			}
		}
	}
	SCCP_LIST_UNLOCK(&l->devices);
	pbx_channel_unref(forwarder);
	return 0;
}

/*
 * Handle Hangup Request by Asterisk
 * sccp_channel should be retained in calling function
 */

channelPtr sccp_pbx_hangup(constChannelPtr channel)
{
	/* here the ast channel is locked */
	(void) ATOMIC_DECR(&GLOB(usecnt), 1, &GLOB(usecnt_lock));

	pbx_update_use_count();

	AUTO_RELEASE(sccp_channel_t, c , sccp_channel_retain(channel));
	if (!c) {
		sccp_log_and((DEBUGCAT_PBX + DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "SCCP: hangup requested for a channel whose SCCP call is already gone\n");
		return NULL;
	}
	c->isHangingUp = TRUE;
	sccp_log_and((DEBUGCAT_PBX + DEBUGCAT_CHANNEL))(VERBOSE_PREFIX_3 "%s: hangup requested\n", c->designator);

	AUTO_RELEASE(sccp_device_t, d , sccp_channel_getDevice(c));
	if(d && d->session) {
		sccp_session_waitForPendingRequests(d->session);
	}

	AUTO_RELEASE(sccp_line_t, l , sccp_line_retain(c->line));

#ifdef CS_SCCP_CONFERENCE
	if (c && c->conference) {
		sccp_conference_release(&c->conference);								/* explicit release required here */
	}
	if (d && d->conference) {
		sccp_conference_release(&d->conference);								/* explicit release required here */
	}
#endif														// CS_SCCP_CONFERENCE
	if (c->rtp.audio.instance || c->rtp.video.instance) {
		sccp_channel_closeAllMediaTransmitAndReceive(c);
	}

	sccp_channel_stop_schedule_digittimout(c);
	sccp_channel_stop_schedule_cfwd_noanswer(c);

	sccp_log((DEBUGCAT_PBX + DEBUGCAT_CHANNEL))(VERBOSE_PREFIX_3 "%s: current call state %s (%d)\n", c->designator, sccp_channelstate2str(c->state), c->state);

	sccp_channel_end_forwarding_channel(c);

	if(d) {
		sccp_channel_transfer_cancel(d, c);
	}

	if (l) {
		sccp_linedevice_t * ld = NULL;
		SCCP_LIST_LOCK(&l->devices);
		SCCP_LIST_TRAVERSE(&l->devices, ld, list) {
			AUTO_RELEASE(sccp_device_t, tmpDevice, sccp_device_retain(ld->device));
			if(!d && tmpDevice && SKINNY_DEVICE_RS_OK == sccp_device_getRegistrationState(tmpDevice)) {
				d = sccp_device_retain(tmpDevice);
			}
			if (tmpDevice) {
				sccp_channel_transfer_release(tmpDevice, c); /* explicit release required here */
			}
		}
		SCCP_LIST_UNLOCK(&l->devices);
		sccp_line_removeChannel(l, c);
	}

	if (d) {
		if (d->monitorFeature.status & SCCP_FEATURE_MONITOR_STATE_ACTIVE) {
			d->monitorFeature.status &= ~SCCP_FEATURE_MONITOR_STATE_ACTIVE;
			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "%s: recording state reset after hangup\n", DEV_ID_LOG(d));
			sccp_feat_changed(d, NULL, SCCP_FEATURE_MONITOR);
		}

		if (SCCP_CHANNELSTATE_DOWN != c->state && SCCP_CHANNELSTATE_ONHOOK != c->state) {
			sccp_indicate(d, c, SCCP_CHANNELSTATE_ONHOOK);
		}

		sccp_channel_StatisticsRequest(c);
		sccp_channel_clean(c);
		return c;								/* returning unretained so that sccp_wrapper_asterisk113_hangup can clear out the last reference */
	}
	return NULL;
}

int sccp_pbx_remote_answer(constChannelPtr channel)
{
	int res = -1;

	AUTO_RELEASE(sccp_channel_t, c , sccp_channel_retain(channel));
	if(!c || !c->owner) {
		return res;
	}
	sccp_log((DEBUGCAT_PBX))(VERBOSE_PREFIX_2 "%s: remote party answered\n", c->designator);

	sccp_channel_stop_schedule_cfwd_noanswer(c);

	if (c->parentChannel) {										// containing a retained channel, final release at the end
		sccp_log((DEBUGCAT_PBX))(VERBOSE_PREFIX_3 "%s: handling forwarded call\n", c->designator);

		pbx_channel_lock(c->parentChannel->owner);
		PBX_CHANNEL_TYPE * forwarder = pbx_channel_ref(c->parentChannel->owner);
		pbx_channel_unlock(c->parentChannel->owner);

		pbx_channel_lock(c->owner);
		PBX_CHANNEL_TYPE * tmp_channel = pbx_channel_ref(c->owner);
		pbx_channel_unlock(c->owner);

		const char * destinationChannelName = pbx_builtin_getvar_helper(tmp_channel, CS_BRIDGEPEERNAME);

		PBX_CHANNEL_TYPE * destination = NULL;
		if(sccp_strlen_zero(destinationChannelName) || !iPbx.getChannelByName(destinationChannelName, &destination)) {
			pbx_log(LOG_NOTICE, "%s: forwarded call not connected: its destination channel '%s' is gone\n", c->designator, destinationChannelName ? destinationChannelName : "");
			return -2;
		}
		sccp_log(DEBUGCAT_PBX)(VERBOSE_PREFIX_3 "\n"
							"\tendpoint1           | bridge             | endpoint2          | comment\n"
							"\t=================== | ================== | ================== | =================\n"
							"\t%-20.20s| primary_call       |%20.20s| creates tmp_channel in sccp_pbx_call to call forwarding destination\n"
							"\t%-20.20s| temp_bridge        |%20.20s| masquerade on answer of endpoint 2\n"
							"\t------------------- | ------------------ | ------------------ | <-- masquerade temp_bridge:endpoint2 -> primary_call:endpoint2\n"
							"\t%-20.20s| primary call       |%20.20s| after masquerading, hangup temp_bridge:temp_channel\n",
				       "incoming", pbx_channel_name(forwarder), pbx_channel_name(tmp_channel), destinationChannelName, "incoming", destinationChannelName);
		do {
			sccp_channel_release(&c->parentChannel);
			if(destination) {
				sccp_log((DEBUGCAT_PBX))(VERBOSE_PREFIX_3 "%s: handling forwarded call: replacing %s with %s\n", c->designator, pbx_channel_name(forwarder), pbx_channel_name(destination));
				if(!iPbx.masqueradeHelper(destination, forwarder)) {
					pbx_log(LOG_ERROR, "%s: forwarded call not connected: Asterisk could not move %s into the place of %s\n", c->designator, pbx_channel_name(destination), pbx_channel_name(forwarder));
					if(destination) {
						pbx_channel_unref(destination);
					}
					res = -3;
					break;
				}
				pbx_indicate(forwarder, AST_CONTROL_CONNECTED_LINE);
				sccp_log((DEBUGCAT_PBX))(VERBOSE_PREFIX_4 "%s: moved into %s\n", c->designator, pbx_channel_name(forwarder));
				res = 0;
			} else {
				pbx_log(LOG_WARNING, "%s: forwarded call not connected: its destination channel '%s' is gone; hanging up\n", c->designator, destinationChannelName);
				if(pbx_channel_state(tmp_channel) == AST_STATE_RING && pbx_channel_state(forwarder) == AST_STATE_DOWN && iPbx.getChannelPbx(c)) {
					sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_4 "SCCP: receiving side hung up (has PBX: %s)\n", iPbx.getChannelPbx(c) ? "yes" : "no");
					pbx_channel_set_hangupcause(forwarder, AST_CAUSE_CALL_REJECTED);
				} else {
					pbx_log(LOG_WARNING, "%s: forwarded call %s not connected: the forward destination did not answer or is gone; hanging up\n", c->currentDeviceId, c->designator);
					pbx_channel_set_hangupcause(forwarder, AST_CAUSE_REQUESTED_CHAN_UNAVAIL);
					sccp_channel_endcall(c);
				}
				pbx_channel_set_hangupcause(forwarder, AST_CAUSE_REQUESTED_CHAN_UNAVAIL);
				pbx_channel_unref(forwarder);
				if(destination) {
					pbx_channel_unref(destination);
				}
				res = -4;
			}
		} while(0);
		if(tmp_channel) {
			pbx_channel_unref(tmp_channel);
		}
	} else {
		sccp_log((DEBUGCAT_CORE))(VERBOSE_PREFIX_3 "%s: outgoing call %s answered by the remote party\n", c->currentDeviceId, iPbx.getChannelName(c));
		AUTO_RELEASE(sccp_device_t, d , sccp_channel_getDevice(c));
		if (d) {
			const char * application = ast_channel_appl (c->owner);
			if (application && sccp_strequals (application, "ParkedCall")) {
				sccp_log ((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: retrieving parked call\n", c->designator);
				sccp_channel_setChannelstate (c, SCCP_CHANNELSTATE_CALLPARK);
				pbx_builtin_setvar_helper (c->owner, "_PARK_RETRIEVER", c->designator);
			}
#if CS_SCCP_CONFERENCE
			sccp_indicate(d, c, d->conference ? SCCP_CHANNELSTATE_CONNECTEDCONFERENCE : SCCP_CHANNELSTATE_CONNECTED);
#else
			sccp_indicate(d, c, SCCP_CHANNELSTATE_CONNECTED);
#endif
			if((d->monitorFeature.status & SCCP_FEATURE_MONITOR_STATE_REQUESTED) && !(d->monitorFeature.status & SCCP_FEATURE_MONITOR_STATE_ACTIVE)) {
				pbx_log(LOG_NOTICE, "%s: starting the recording requested with the monitor feature on call %s\n", d->id, c->designator);
				sccp_feat_monitor(d, NULL, 0, c);
			}

			sccp_log(DEBUGCAT_PBX)(VERBOSE_PREFIX_3 "%s: call is up\n", c->designator);
			iPbx.set_callstate(c, AST_STATE_UP);
			res = 0;
		}

		if((sccp_rtp_getState(&c->rtp.video, SCCP_RTP_RECEPTION) & SCCP_RTP_STATUS_ACTIVE)) {
			iPbx.queue_control(c->owner, AST_CONTROL_VIDUPDATE);
		}
	}
	return res;
}

/* Locks: usecnt_lock */
boolean_t sccp_pbx_channel_allocate(constChannelPtr channel, const void * ids, const PBX_CHANNEL_TYPE * parentChannel)
{
	PBX_CHANNEL_TYPE * tmp = NULL;
	AUTO_RELEASE(sccp_channel_t, c , sccp_channel_retain(channel));
	AUTO_RELEASE(sccp_device_t, d , NULL);

	if (!c) {
		return FALSE;
	}
	pbx_assert(c->owner == NULL);										// prevent calling this function when the channel already has a pbx channel

#ifndef CS_AST_CHANNEL_HAS_CID
	char cidtmp[256];

	memset(&cidtmp, 0, sizeof(cidtmp));
#endif														// CS_AST_CHANNEL_HAS_CID

	AUTO_RELEASE(sccp_line_t, l , sccp_line_retain(c->line));
	if (!l) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: channel %s not created: no line\n", c->designator);
		pbx_log(LOG_ERROR, "%s: Asterisk channel not created: the call has no line\n", c->designator);
		return FALSE;
	}

	sccp_log((DEBUGCAT_PBX + DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "SCCP: creating %s channel on line %s\n", skinny_calltype2str(c->calltype), l->name);
	char s1[512];

	char s2[512];

	char cid_name[StationMaxNameSize] = {0};
	char cid_num[StationMaxDirnumSize] = {0};
	{
		sccp_linedevice_t * ld = NULL;
		d = sccp_channel_getDevice(c) /*ref_replace*/;
		if(d) {
			SCCP_LIST_LOCK(&l->devices);
			SCCP_LIST_TRAVERSE(&l->devices, ld, list) {
				if(ld->device == d) {
					break;
				}
			}
			SCCP_LIST_UNLOCK(&l->devices);
		} else if(SCCP_LIST_GETSIZE(&l->devices) > 0) {
			SCCP_LIST_LOCK(&l->devices);
			ld = SCCP_LIST_FIRST(&l->devices);
			SCCP_LIST_UNLOCK(&l->devices);
			if(ld && ld->device) {
				d = sccp_device_retain(ld->device) /*ref_replace*/;                                        // ugly hack just picking the first one !
			}
		}

		if(!ld) {
			pbx_log(LOG_NOTICE, "%s: Asterisk channel not created: line %s is not on any registered device\n", c->designator, l->name);
			goto error_exit;
		}

		sccp_callinfo_t *ci = sccp_channel_getCallInfo(c);
		if(ld->subscriptionId.replaceCid) {
			snprintf(cid_num, StationMaxDirnumSize, "%s", sccp_strlen_zero(ld->subscriptionId.number) ? l->cid_num : ld->subscriptionId.number);
			snprintf(cid_name, StationMaxNameSize, "%s", sccp_strlen_zero(ld->subscriptionId.name) ? l->cid_name : ld->subscriptionId.name);
		} else {
			snprintf(cid_num, StationMaxDirnumSize, "%s%s", l->cid_num, sccp_strlen_zero(ld->subscriptionId.number) ? "" : ld->subscriptionId.number);
			snprintf(cid_name, StationMaxNameSize, "%s%s", l->cid_name, sccp_strlen_zero(ld->subscriptionId.name) ? "" : ld->subscriptionId.name);
		}
		switch (c->calltype) {
			case SKINNY_CALLTYPE_INBOUND:
				iCallInfo.Setter(ci, SCCP_CALLINFO_CALLEDPARTY_NAME, &cid_name, SCCP_CALLINFO_CALLEDPARTY_NUMBER, &cid_num, SCCP_CALLINFO_KEY_SENTINEL);
				break;
			case SKINNY_CALLTYPE_FORWARD:
				iCallInfo.Setter(ci,
					SCCP_CALLINFO_CALLINGPARTY_NAME, &cid_name,
					SCCP_CALLINFO_CALLINGPARTY_NUMBER, &cid_num,
					SCCP_CALLINFO_LAST_REDIRECTINGPARTY_NAME, &cid_name,
					SCCP_CALLINFO_LAST_REDIRECTINGPARTY_NUMBER, &cid_num,
					SCCP_CALLINFO_LAST_REDIRECT_REASON, 4,
					SCCP_CALLINFO_KEY_SENTINEL);
				break;
			case SKINNY_CALLTYPE_OUTBOUND:
				iCallInfo.Setter(ci,
					SCCP_CALLINFO_CALLINGPARTY_NAME, &cid_name,
					SCCP_CALLINFO_CALLINGPARTY_NUMBER, &cid_num,
					SCCP_CALLINFO_LAST_REDIRECTINGPARTY_NAME, &cid_name,
					SCCP_CALLINFO_LAST_REDIRECTINGPARTY_NUMBER, &cid_num,
					SCCP_CALLINFO_LAST_REDIRECT_REASON, 0,
					SCCP_CALLINFO_KEY_SENTINEL);
				break;
			case SKINNY_CALLTYPE_SENTINEL:
				break;
		}

		// make sure preferences only contains the codecs that this channel is capable of
		if (SCCP_LIST_GETSIZE(&l->devices) == 1 && d) {
			sccp_codec_reduceSet(c->preferences.audio, d->capabilities.audio);
			sccp_codec_reduceSet(c->preferences.video, d->capabilities.video);
		} else {
			sccp_codec_reduceSet(c->preferences.audio, c->capabilities.audio);
			sccp_codec_reduceSet(c->preferences.video, c->capabilities.video);
		}

		if (c->preferences.audio[0] == SKINNY_CODEC_NONE || c->capabilities.audio[0] == SKINNY_CODEC_NONE) {
			pbx_log(LOG_ERROR, "%s: call ended: no audio codec is both allowed by the configuration (%s) and supported by the %s (%s)\n",
				c->designator,
				sccp_codec_multiple2str(s1, sizeof(s1) - 1, c->preferences.audio, SKINNY_MAX_CAPABILITIES),
				l->preferences_set_on_line_level ? "line's devices" : "phone",
				sccp_codec_multiple2str(s2, sizeof(s2) - 1, c->capabilities.audio, SKINNY_MAX_CAPABILITIES));
			goto error_exit;
		}
	}
	sccp_log((DEBUGCAT_PBX + DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "SCCP: caller ID number: %s\n", cid_num);
	sccp_log((DEBUGCAT_PBX + DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "SCCP: caller ID name: %s\n", cid_name);
	sccp_log((DEBUGCAT_PBX + DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "SCCP: account code: %s\n", S_OR(l->accountcode, "(not set)"));
	sccp_log((DEBUGCAT_PBX + DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "SCCP: extension: %s\n", c->dialedNumber);
	sccp_log((DEBUGCAT_PBX + DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "SCCP: context: %s\n", l->context);
	sccp_log((DEBUGCAT_PBX + DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "SCCP: AMA flags: %d\n", (int)l->amaflags);
	sccp_log((DEBUGCAT_PBX + DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "SCCP: call: %s\n", c->designator);
	sccp_log((DEBUGCAT_PBX + DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "SCCP: combined audio capabilities: %s\n", sccp_codec_multiple2str(s1, sizeof(s1) - 1, c->capabilities.audio, SKINNY_MAX_CAPABILITIES));
	sccp_log((DEBUGCAT_PBX + DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "SCCP: reduced audio preferences: %s\n", sccp_codec_multiple2str(s1, sizeof(s1) - 1, c->preferences.audio, SKINNY_MAX_CAPABILITIES));
	sccp_log((DEBUGCAT_PBX + DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "SCCP: combined video capabilities: %s\n", sccp_codec_multiple2str(s1, sizeof(s1) - 1, c->capabilities.video, SKINNY_MAX_CAPABILITIES));
	sccp_log((DEBUGCAT_PBX + DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "SCCP: reduced video preferences: %s\n", sccp_codec_multiple2str(s1, sizeof(s1) - 1, c->preferences.video, SKINNY_MAX_CAPABILITIES));
	if (!c->pbx_callid && c->calltype != SKINNY_CALLTYPE_INBOUND) {
		c->pbx_callid = pbx_create_callid();
	}
	iPbx.alloc_pbxChannel(c, ids, parentChannel, &tmp);

	if (!tmp || !c->owner) {
		if (!ast_shutting_down()) {
			pbx_log(LOG_ERROR, "%s: Asterisk could not create a channel for line %s\n", c->designator, l->name);
		}
		goto error_exit;
	}
	iPbx.setChannelName(c, c->designator);

	(void) ATOMIC_INCR(&GLOB(usecnt), 1, &GLOB(usecnt_lock));

	pbx_update_use_count();

	if (iPbx.set_callerid_number) {
		iPbx.set_callerid_number(c->owner, cid_num);
	}
	if (iPbx.set_callerid_ani) {
		iPbx.set_callerid_ani(c->owner, cid_num);
	}
	if (iPbx.set_callerid_name) {
		iPbx.set_callerid_name(c->owner, cid_name);
	}

	if (SCCP_LIST_GETSIZE(&l->devices) == 1) {
		sccp_linedevice_t * ld = NULL;

		SCCP_LIST_LOCK(&l->devices);
		SCCP_LIST_TRAVERSE(&l->devices, ld, list) {
			if(ld->line == l) {
				if(ld->cfwd[SCCP_CFWD_ALL].enabled) {
					sccp_log((DEBUGCAT_PBX))(VERBOSE_PREFIX_3 "%s: Asterisk call forward set to %s\n", c->designator, ld->cfwd[SCCP_CFWD_ALL].number);
					iPbx.setChannelCallForward(c, ld->cfwd[SCCP_CFWD_ALL].number);
				} else if(ld->cfwd[SCCP_CFWD_BUSY].enabled && (sccp_device_getDeviceState(ld->device) != SCCP_DEVICESTATE_ONHOOK || sccp_device_getActiveAccessory(ld->device))) {
					sccp_log((DEBUGCAT_PBX))(VERBOSE_PREFIX_3 "%s: Asterisk call forward set to %s\n", c->designator, ld->cfwd[SCCP_CFWD_BUSY].number);
					iPbx.setChannelCallForward(c, ld->cfwd[SCCP_CFWD_BUSY].number);
				}
				break;
			}
		}
		SCCP_LIST_UNLOCK(&l->devices);
	}

#if CS_SCCP_VIDEO
	const char *VideoStr = pbx_builtin_getvar_helper(c->owner, "SCCP_VIDEO_MODE");
	if (VideoStr && !sccp_strlen_zero(VideoStr)) {
		sccp_channel_setVideoMode(c, VideoStr);
	}
#endif
	/* asterisk needs the native formats before dialout, otherwise the next channel gets the whole AUDIO_MASK as requested format
	 * chan_sip don't like this do sdp processing */

	if (d) {
		if (c->calltype == SKINNY_CALLTYPE_OUTBOUND) {
			if (!c->rtp.audio.instance && !sccp_rtp_createServer(d, c, SCCP_RTP_AUDIO)) {
				pbx_log(LOG_WARNING, "%s: could not create the audio RTP instance for call %s\n", d->id, c->designator);
				goto error_exit;
			}
		}
		pbx_builtin_setvar_helper(tmp, "SCCP_DEVICE_MAC", d->id);
		struct sockaddr_storage sas = { 0 };
		sccp_session_getSas(d->session, &sas);
		pbx_builtin_setvar_helper(tmp, "SCCP_DEVICE_IP", d->session ? sccp_netsock_stringify_addr(&sas) : "");
		pbx_builtin_setvar_helper(tmp, "SCCP_DEVICE_TYPE", skinny_devicetype2str(d->skinny_type));
	}
	sccp_log((DEBUGCAT_PBX + DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "%s: Asterisk channel %s created\n", (l) ? l->name : "SCCP", c->designator);

	return TRUE;

error_exit:
	if(c) {
		if (!ast_shutting_down()) {
			pbx_log(LOG_WARNING, "%s: call %s on line %s not set up; hanging it up\n", DEV_ID_LOG(d), c->designator, l->name);
		}
		if(c->owner) {
			if(d) {
				sccp_indicate(d, c, SCCP_CHANNELSTATE_CONGESTION);
			}
			sccp_channel_endcall(c);
		} else {
			if(d) {
				sccp_indicate(d, channel, SCCP_CHANNELSTATE_ONHOOK);
			}
			if(c->line) {
				sccp_line_removeChannel(c->line, c);
			}
			sccp_channel_clean(c);
			sccp_channel_release(&c);
		}
	}
	return FALSE;
}

int sccp_pbx_sched_dial(const void * data)
{
	AUTO_RELEASE(sccp_channel_t, channel, sccp_channel_retain(data));

	if(channel) {
		if ((ATOMIC_FETCH(&channel->scheduler.deny, &channel->scheduler.lock) == 0) && channel->scheduler.hangup_id == -1) {
			channel->scheduler.digittimeout_id = -3;	/* prevent further digittimeout scheduling */
			if (channel->owner && !iPbx.getChannelPbx(channel) && !sccp_strlen_zero(channel->dialedNumber)) {
				sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_1 "SCCP: digit timeout on %s; dialing %s\n", channel->designator, channel->dialedNumber);
				sccp_pbx_softswitch(channel);
			} else {
				sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_1 "SCCP: digit timeout on %s with nothing dialed; invalid number\n", channel->designator);
				channel->dialedNumber[0] = '\0';
				sccp_indicate(NULL, channel, SCCP_CHANNELSTATE_INVALIDNUMBER);
			}
		}
		sccp_channel_release((sccp_channel_t **)&data);	// release channel retained in scheduled event
	}
	return 0;						// return 0 to release schedule !
}

sccp_extension_status_t sccp_pbx_helper(constChannelPtr c)
{
	sccp_extension_status_t extensionStatus = 0;
	int dialedLen = sccp_strlen(c->dialedNumber);

	if (dialedLen > 1) {
		if (GLOB(recorddigittimeoutchar) && GLOB(digittimeoutchar) == c->dialedNumber[dialedLen - 1]) {
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_2 "%s: dialing ended with the dial-now key: %s\n", c->designator, c->dialedNumber);
			return SCCP_EXTENSION_EXACTMATCH;
		}
	}

	if ((c->softswitch_action != SCCP_SOFTSWITCH_GETCBARGEROOM) && (c->softswitch_action != SCCP_SOFTSWITCH_GETMEETMEROOM)
#ifdef CS_SCCP_CONFERENCE
	    && (c->softswitch_action != SCCP_SOFTSWITCH_GETCONFERENCEROOM)
#endif
	    ) {
		extensionStatus = iPbx.extension_status(c);
		AUTO_RELEASE(sccp_device_t, d , sccp_channel_getDevice(c));

		if (d) {
			if (((d->overlapFeature.enabled && !extensionStatus) || (!d->overlapFeature.enabled && !extensionStatus))) {
				sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: %s can match more digits\n", c->designator, c->dialedNumber);
				return SCCP_EXTENSION_MATCHMORE;
			}
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: %s matches %s\n", c->designator, c->dialedNumber, extensionStatus == SCCP_EXTENSION_EXACTMATCH ? "Exactly" : "More");
		}
		return extensionStatus;
	}
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_2 "%s: %s exists\n", c->designator, c->dialedNumber);
	return SCCP_EXTENSION_NOTEXISTS;
}

void * sccp_pbx_softswitch(constChannelPtr channel)
{
	PBX_CHANNEL_TYPE * pbx_channel = NULL;
	PBX_VARIABLE_TYPE * v = NULL;

	{
		AUTO_RELEASE(sccp_channel_t, c , sccp_channel_retain(channel));

		if (!c) {
			pbx_log(LOG_ERROR, "SCCP: dial thread started without a call (caller bug)\n");
			goto EXIT_FUNC;
		}
		sccp_channel_stop_schedule_digittimout(c);

		c->enbloc.deactivate = 0;
		c->enbloc.totaldigittime = 0;
		c->enbloc.totaldigittimesquared = 0;
		c->enbloc.digittimeout = GLOB(digittimeout);

		/* prevent softswitch from being executed twice (Pavel Troller / 15-Oct-2010) */
		if (iPbx.getChannelPbx(c)) {
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: dialplan already running; sending digits instead of starting it\n");
			/* If there are any digits, send them instead of starting the PBX */
			if (!sccp_strlen_zero(c->dialedNumber)) {
				if (iPbx.send_digits) {
					iPbx.send_digits(channel, c->dialedNumber);
				}
				sccp_channel_set_calledparty(c, NULL, c->dialedNumber);
			}
			goto EXIT_FUNC;
		}

		if (!c->owner) {
			pbx_log(LOG_WARNING, "%s: not dialed: the call has no Asterisk channel\n", c->designator);
			goto EXIT_FUNC;
		}
		pbx_channel = pbx_channel_ref(c->owner);

		if (c->calltype != SKINNY_CALLTYPE_OUTBOUND && c->softswitch_action == SCCP_SOFTSWITCH_DIAL) {
			pbx_log(LOG_ERROR, "%s: not dialed: the dial thread was started for an incoming call (caller bug)\n", c->designator);
			goto EXIT_FUNC;
		}

		AUTO_RELEASE(sccp_device_t, d , sccp_channel_getDevice(c));

		if (!d) {
			pbx_log(LOG_WARNING, "%s: not dialed: the call has no device attached\n", c->designator);
			goto EXIT_FUNC;
		}

		if (pbx_check_hangup(pbx_channel)) {
			sccp_log((DEBUGCAT_PBX))(VERBOSE_PREFIX_3 "%s: not dialed: the call was hung up before dialing\n", c->designator);
			sccp_indicate(d, c, SCCP_CHANNELSTATE_ONHOOK);
			goto EXIT_FUNC;
		}

		/* we don't need to check for a device type but just if the device has an id, otherwise back home  -FS */
		if (sccp_strlen_zero(d->id)) {
			pbx_log(LOG_ERROR, "%s: not dialed: the device has no name\n", c->designator);
			goto EXIT_FUNC;
		}

		AUTO_RELEASE(sccp_line_t, l , sccp_line_retain(c->line));

		if (!l) {
			pbx_log(LOG_WARNING, "%s: not dialed: the call has no line\n", c->designator);
			c->hangupRequest(c);
			goto EXIT_FUNC;
		}
		uint8_t instance = sccp_device_find_index_for_line(d, l->name);

		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: new call on line %s\n", DEV_ID_LOG(d), l->name);

		char shortenedNumber[256] = { '\0' };
		sccp_copy_string(shortenedNumber, c->dialedNumber, sizeof(shortenedNumber));
		unsigned int len = sccp_strlen(shortenedNumber);

		pbx_assert(sccp_strlen(c->dialedNumber) == len);

		if (len > 0 && GLOB(digittimeoutchar) == shortenedNumber[len - 1]) {
			shortenedNumber[len - 1] = '\0';

			if (!GLOB(recorddigittimeoutchar)) {
				c->dialedNumber[len - 1] = '\0';
			}
		}

		switch (c->softswitch_action) {
			case SCCP_SOFTSWITCH_GETFORWARDEXTEN:
				{
				sccp_cfwd_t type = (sccp_cfwd_t)c->ss_data;
				sccp_log((DEBUGCAT_PBX))(VERBOSE_PREFIX_3 "%s: collecting the %s forward destination\n", d->id, sccp_cfwd2str(type));
				if(!sccp_strlen_zero(shortenedNumber)) {
					c->setTone(c, SKINNY_TONE_ZIP, SKINNY_TONEDIRECTION_USER);
					sccp_line_cfwd(l, d, type, shortenedNumber);
				}
					sccp_channel_endcall(c);
					goto EXIT_FUNC;
				}
			case SCCP_SOFTSWITCH_ENDCALLFORWARD:
				{
				sccp_cfwd_t type = (sccp_cfwd_t)c->ss_data;
				sccp_log((DEBUGCAT_PBX))(VERBOSE_PREFIX_3 "%s: clearing %s forward\n", d->id, sccp_cfwd2str(type));
				sccp_line_cfwd(l, d, type, NULL);
				switch(type) {
					case SCCP_CFWD_ALL:
						sccp_device_setLamp(d, SKINNY_STIMULUS_FORWARDALL, instance, SKINNY_LAMP_OFF);
						break;
					case SCCP_CFWD_BUSY:
						sccp_device_setLamp(d, SKINNY_STIMULUS_FORWARDBUSY, instance, SKINNY_LAMP_OFF);
						break;
					case SCCP_CFWD_NOANSWER:
						sccp_device_setLamp(d, SKINNY_STIMULUS_FORWARDNOANSWER, instance, SKINNY_LAMP_OFF);
						break;
					case SCCP_CFWD_NONE:
					case SCCP_CFWD_SENTINEL:
					default:
						pbx_log(LOG_ERROR, "%s: forward not cleared: the call carries forward type %d, which is not all, busy or no-answer\n", d->id, (int)type);
				}
					sccp_channel_endcall(c);
					goto EXIT_FUNC;
				}
#ifdef CS_SCCP_PICKUP
			case SCCP_SOFTSWITCH_GETPICKUPEXTEN:
				sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "%s: collecting the pickup extension\n", d->id);
				sccp_dev_clearprompt(d, instance, c->callid);

				if (!sccp_strlen_zero(shortenedNumber)) {
 					sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: picking up extension %s\n", shortenedNumber);
					sccp_dev_displayprompt(d, instance, c->callid, SKINNY_DISP_PICKUP, GLOB(digittimeout));
					if (sccp_feat_directed_pickup(d, c, instance, shortenedNumber) == 0) {
						goto EXIT_FUNC;
					}
				}
				sccp_dev_displayprinotify(d, SKINNY_DISP_NO_CALL_AVAILABLE_FOR_PICKUP, SCCP_MESSAGE_PRIORITY_TIMEOUT, 5);
				if (c->state == SCCP_CHANNELSTATE_ONHOOK || c->state == SCCP_CHANNELSTATE_DOWN) {
					c->setTone(c, SKINNY_TONE_BEEPBONK, SKINNY_TONEDIRECTION_USER);
				}
				sccp_channel_schedule_hangup(c, 500);
				goto EXIT_FUNC;
#endif														// CS_SCCP_PICKUP
#ifdef CS_SCCP_CONFERENCE
			case SCCP_SOFTSWITCH_GETCONFERENCEROOM:
				sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "%s: conference request\n", d->id);
				if (c->owner && !pbx_check_hangup(c->owner)) {
					sccp_channel_setChannelstate(c, SCCP_CHANNELSTATE_PROCEED);
					iPbx.set_callstate(channel, AST_STATE_UP);
					if (!d->conference) {
						if (!(d->conference = sccp_conference_create(d, c))) {
							goto EXIT_FUNC;
						}
					} else {
						pbx_log(LOG_NOTICE, "%s: new conference not started: this device already runs a conference\n", DEV_ID_LOG(d));
						sccp_channel_endcall(c);
						goto EXIT_FUNC;
					}
					sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: starting conference\n", d->id);
					sccp_feat_conference_start(d, instance, c);
				}
				goto EXIT_FUNC;
#endif														// CS_SCCP_CONFERENCE
			case SCCP_SOFTSWITCH_GETMEETMEROOM:
				sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "%s: meetme request\n", d->id);
				if (!sccp_strlen_zero(shortenedNumber) && !sccp_strlen_zero(c->line->meetmenum)) {
					sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: meetme room %s via extension %s\n", d->id, shortenedNumber, c->line->meetmenum);
					if (c->owner && !pbx_check_hangup(c->owner)) {
						pbx_builtin_setvar_helper(c->owner, "SCCP_MEETME_ROOM", shortenedNumber);
					}
					sccp_copy_string(shortenedNumber, c->line->meetmenum, sizeof(shortenedNumber));

					sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: starting meetme thread\n", d->id);
					sccp_feat_meetme_start(c);
					sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: meetme thread started\n", d->id);
					goto EXIT_FUNC;
				} else {
					sccp_channel_endcall(c);
					goto EXIT_FUNC;
				}
				break;
			case SCCP_SOFTSWITCH_GETBARGEEXTEN:
				sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "%s: collecting the barge extension\n", d->id);
				sccp_dev_clearprompt(d, instance, c->callid);
				if (!sccp_strlen_zero(shortenedNumber)) {
 					sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: barging into extension %s\n", shortenedNumber);
					sccp_dev_displayprompt(d, instance, c->callid, SKINNY_DISP_BARGE, GLOB(digittimeout));
					if (sccp_feat_singleline_barge(c, shortenedNumber)) {
						goto EXIT_FUNC;
					}
				}
				sccp_dev_displayprinotify(d, SKINNY_DISP_FAILED_TO_SETUP_BARGE, SCCP_MESSAGE_PRIORITY_TIMEOUT, 5);
				if (c->state == SCCP_CHANNELSTATE_ONHOOK || c->state == SCCP_CHANNELSTATE_DOWN) {
					c->setTone(c, SKINNY_TONE_BEEPBONK, SKINNY_TONEDIRECTION_USER);
				}
				sccp_channel_endcall(c);
				goto EXIT_FUNC;
			case SCCP_SOFTSWITCH_GETCBARGEROOM:
				sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "%s: collecting the conference barge extension\n", d->id);
				sccp_indicate(d, c, SCCP_CHANNELSTATE_DIALING);
				sccp_device_sendcallstate(d, instance, c->callid, SKINNY_CALLSTATE_PROCEED, SKINNY_CALLPRIORITY_LOW, SKINNY_CALLINFO_VISIBILITY_DEFAULT);
				sccp_channel_send_callinfo(d, c);
				sccp_dev_clearprompt(d, instance, c->callid);
				sccp_dev_displayprompt(d, instance, c->callid, SKINNY_DISP_CALL_PROCEED, GLOB(digittimeout));
				if (!sccp_strlen_zero(shortenedNumber)) {
					sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: barging into conference %s\n", d->id, shortenedNumber);
					if (sccp_feat_cbarge(c, shortenedNumber)) {
						sccp_indicate(d, c, SCCP_CHANNELSTATE_INVALIDNUMBER);
					}
				} else {
					sccp_channel_endcall(c);
				}
				goto EXIT_FUNC;
			case SCCP_SOFTSWITCH_SENTINEL:
				sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "%s: unknown dialing action\n", d->id);
				goto EXIT_FUNC;
			case SCCP_SOFTSWITCH_DIAL:
				sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "%s: dialing %s\n", d->id, shortenedNumber);
				sccp_indicate(d, c, SCCP_CHANNELSTATE_DIALING);
				/* fall through */
		}

		if (pbx_channel && !pbx_check_hangup(pbx_channel)) {
			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "SCCP: SKINNY_PRIVATE set to %s\n", c->privacy ? "1" : "0");
			if (c->privacy) {
				sccp_channel_set_calleridPresentation(c, CALLERID_PRESENTATION_FORBIDDEN);
			}

			uint32_t result = d->privacyFeature.status & SCCP_PRIVACYFEATURE_CALLPRESENT;

			result |= c->privacy;
			if (d->privacyFeature.enabled && result) {
				sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "SCCP: SKINNY_PRIVATE set to %s\n", "1");
				pbx_builtin_setvar_helper(pbx_channel, "SKINNY_PRIVATE", "1");
			} else {
				sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "SCCP: SKINNY_PRIVATE set to %s\n", "0");
				pbx_builtin_setvar_helper(pbx_channel, "SKINNY_PRIVATE", "0");
			}
		}

		v = d->variables;
		while (pbx_channel && !pbx_check_hangup(pbx_channel) && d && v) {
			pbx_builtin_setvar_helper(pbx_channel, v->name, v->value);
			v = v->next;
		}

		v = l->variables;
		while (pbx_channel && !pbx_check_hangup(pbx_channel) && l && v) {
			pbx_builtin_setvar_helper(pbx_channel, v->name, v->value);
			v = v->next;
		}

		iPbx.setChannelExten(c, shortenedNumber);

		int extension_exists = SCCP_EXTENSION_NOTEXISTS;

		if (!sccp_strlen_zero(shortenedNumber) && ((extension_exists = iPbx.extension_status(c) != SCCP_EXTENSION_NOTEXISTS))
		    ) {
			if (pbx_channel && !pbx_check_hangup(pbx_channel)) {
				sccp_log((DEBUGCAT_PBX + DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_1 "%s: %s dials %s\n", DEV_ID_LOG(d), c->designator, shortenedNumber);

				/* Answer dialplan command works only when in RINGING OR RING ast_state */
				iPbx.set_callstate(c, AST_STATE_RING);

				enum ast_pbx_result pbxStartResult = pbx_pbx_start(pbx_channel);

				switch (pbxStartResult) {
					case AST_PBX_FAILED:
						pbx_log(LOG_ERROR, "%s: call %s to %s failed: Asterisk could not start the dialplan for it; congestion signalled\n", DEV_ID_LOG(d), c->designator, shortenedNumber);
						sccp_indicate(d, c, SCCP_CHANNELSTATE_CONGESTION);		/* will auto hangup after SCCP_HANGUP_TIMEOUT */
						break;
					case AST_PBX_CALL_LIMIT:
						pbx_log(LOG_WARNING, "%s: call %s to %s refused: Asterisk's maxcalls limit is reached; congestion signalled\n", DEV_ID_LOG(d), c->designator, shortenedNumber);
						sccp_indicate(d, c, SCCP_CHANNELSTATE_CONGESTION);		/* will auto hangup after SCCP_HANGUP_TIMEOUT */
						break;
					case AST_PBX_SUCCESS:
						sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_1 "%s: dialplan started\n", DEV_ID_LOG(d));
#ifdef CS_MANAGER_EVENTS
						if (GLOB(callevents)) {
							manager_event(EVENT_FLAG_SYSTEM, "ChannelUpdate", "Channel: %s\r\nUniqueid: %s\r\nChanneltype: %s\r\nSCCPdevice: %s\r\nSCCPline: %s\r\nSCCPcallid: %08X\r\nSCCPCallDesignator: %s\r\n",
								(pbx_channel) ? pbx_channel_name(pbx_channel) : "(null)",
								(pbx_channel) ? pbx_channel_uniqueid(pbx_channel) : "(null)",
								"SCCP",
								(d) ? d->id : "(null)",
								(l) ? l->name : "(null)",
								(c && c->callid) ? c->callid : 0,
								(c) ? c->designator : "(null)");
						}
#endif														// CS_MANAGER_EVENTS
						break;
				}
				AUTO_RELEASE(sccp_linedevice_t, ld, c->getLineDevice(c));
				if(ld) {
					sccp_device_setLastNumberDialed(d, shortenedNumber, ld);
				}
				if(iPbx.set_dialed_number) {
					iPbx.set_dialed_number(c, shortenedNumber);
				}
			} else {
				sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_1 "%s: hangup check %d on line %s\n", DEV_ID_LOG(d), (pbx_channel && pbx_check_hangup(pbx_channel)), l->name);
			}
		} else {
			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_1 "%s: %s dialed %s, extension exists: %s\n", DEV_ID_LOG(d), c->designator, shortenedNumber, (extension_exists != SCCP_EXTENSION_NOTEXISTS) ? "yes" : "no");
			pbx_log(LOG_NOTICE, "%s: call from line %s to %s refused: the extension does not exist in context %s\n", DEV_ID_LOG(d), l->name, shortenedNumber, pbx_channel ? pbx_channel_context(pbx_channel) : "pbx_channel==NULL");
			if (pbx_channel && !pbx_check_hangup(pbx_channel)) {
				sccp_log((DEBUGCAT_PBX))(VERBOSE_PREFIX_3 "%s: invalid-number indication sent; call %s will be hung up\n", DEV_ID_LOG(d), c->designator);
				sccp_indicate(d, c, SCCP_CHANNELSTATE_INVALIDNUMBER);				/* will auto hangup after SCCP_HANGUP_TIMEOUT */
			}
		}

		sccp_log((DEBUGCAT_PBX + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_1 "%s: dialing done\n", DEV_ID_LOG(d));
	}
EXIT_FUNC:
	sccp_log((DEBUGCAT_PBX + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_1 "SCCP: dial thread done\n");
	if (pbx_channel) {
		pbx_channel_unref(pbx_channel);
	}
	return NULL;
}

