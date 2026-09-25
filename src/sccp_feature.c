/*!
 * \file        sccp_feature.c
 * \brief       SCCP Feature Class
 * \author      Federico Santulli <fsantulli [at] users.sourceforge.net >
 * \author	Diederik de Groot <ddegroot [at] users.sourceforge.net >
 * \note        This program is free software and may be modified and distributed under the terms of the GNU General Public License, version 2 or (at your option) any later version.
 *              See the LICENSE file at the top of the source tree.
 * \since       2009-01-16
 *
 */

#include "config.h"
#include "common.h"
#include "sccp_channel.h"
#include "sccp_device.h"
#include "sccp_featureButton.h"
#include "sccp_feature.h"
#include "sccp_line.h"
#include "sccp_linedevice.h"
#include "sccp_pbx.h"
#include "sccp_conference.h"
#include "sccp_indicate.h"
#include "sccp_management.h"
#include "sccp_utils.h"
#include "sccp_labels.h"
#include "sccp_threadpool.h"

SCCP_FILE_VERSION(__FILE__, "");

#include <asterisk/causes.h>

#if CS_SCCP_PICKUP
#  if defined(CS_AST_DO_PICKUP) && defined(HAVE_PBX_FEATURES_H)
#    include <asterisk/features.h>
#      include <asterisk/pickup.h>
#  endif
#endif

static int sccp_feat_sharedline_barge(constLineDevicePtr ld, channelPtr bargedChannel);

void sccp_feat_handle_callforward(constLinePtr l, constDevicePtr d, sccp_cfwd_t type, channelPtr maybe_c, uint32_t lineInstance)
{
	if (!l) {
		pbx_log(LOG_ERROR, "SCCP: sccp_feat_handle_callforward() was called without a line (caller bug)\n");
		return;
	}

	if (!d) {
		pbx_log(LOG_ERROR, "SCCP: sccp_feat_handle_callforward() was called without a device (caller bug)\n");
		return;
	}

	AUTO_RELEASE(sccp_linedevice_t, ld, sccp_linedevice_find(d, l));
	if(!ld) {
		pbx_log(LOG_WARNING, "%s: call forward not changed: line %s is not on this device\n", DEV_ID_LOG(d), l->name);
		return;
	}

	AUTO_RELEASE(sccp_channel_t, c , sccp_channel_getEmptyChannel(l, d, maybe_c, SKINNY_CALLTYPE_OUTBOUND, NULL, NULL));
	if (c) {
		sccp_softswitch_t ss_action = c->softswitch_action ? c->softswitch_action : SCCP_SOFTSWITCH_GETFORWARDEXTEN;
		if((ld->cfwd[SCCP_CFWD_ALL].enabled && type == SCCP_CFWD_ALL) || (ld->cfwd[SCCP_CFWD_BUSY].enabled && type == SCCP_CFWD_BUSY) || (ld->cfwd[SCCP_CFWD_NOANSWER].enabled && type == SCCP_CFWD_NOANSWER)) {
			sccp_log(DEBUGCAT_PBX)("%s: removing call forward\n", d->id);
			ss_action = SCCP_SOFTSWITCH_ENDCALLFORWARD;
		} else {
			sccp_log(DEBUGCAT_PBX)("%s: adding call forward\n", d->id);
		}

		if (ss_action == SCCP_SOFTSWITCH_GETFORWARDEXTEN) {
			if (c->state == SCCP_CHANNELSTATE_RINGOUT || c->state == SCCP_CHANNELSTATE_CONNECTED || c->state == SCCP_CHANNELSTATE_PROCEED || c->state == SCCP_CHANNELSTATE_BUSY || c->state == SCCP_CHANNELSTATE_CONGESTION) {
				if (c->calltype == SKINNY_CALLTYPE_OUTBOUND && !sccp_strlen_zero(c->dialedNumber)) {
					sccp_line_cfwd(l, d, type, c->dialedNumber);
					c->setTone(c, SKINNY_TONE_ZIP, SKINNY_TONEDIRECTION_USER);
					sccp_channel_endcall(c);
					return;
				} else if(iPbx.channel_is_bridged(c)) {
					char *number = NULL;
					if (iPbx.get_callerid_name) {
						iPbx.get_callerid_number(c->owner, &number);
					}
					if(number && !sccp_strlen_zero(number)) {
						sccp_line_cfwd(l, d, type, number);
						// we are on call, so no tone has been played until now :)
						c->setTone(c, SKINNY_TONE_ZIP, SKINNY_TONEDIRECTION_USER);
						sccp_channel_endcall(c);
						sccp_free(number);
						return;
					}
				}
			}
		}
		c->softswitch_action = ss_action;
		c->ss_data = type;
		sccp_indicate(d, c, SCCP_CHANNELSTATE_GETDIGITS);
		sccp_dev_set_message((devicePtr)d, SKINNY_DISP_ENTER_NUMBER_TO_FORWARD_TO, SCCP_DISPLAYSTATUS_TIMEOUT, FALSE, FALSE);
		sccp_device_setLamp(d, sccp_cfwd2stimulus(type), ld->lineInstance, SKINNY_LAMP_FLASH);
		if(ss_action == SCCP_SOFTSWITCH_ENDCALLFORWARD) {
			sccp_pbx_softswitch(c);
		}
	}
}

#ifdef CS_SCCP_PICKUP
/*
 * sccp pickup helper function
 * function is called with target locked
 */
static int sccp_feat_perform_pickup(constDevicePtr d, channelPtr c, PBX_CHANNEL_TYPE *target, boolean_t answer)
{
	int res = 0;
	pbx_assert(c != NULL);

#if CS_AST_DO_PICKUP
	PBX_CHANNEL_TYPE *original = c->owner;
	char * target_name = NULL;
	char * target_number = NULL;
	if (iPbx.get_callerid_name) {
		iPbx.get_callerid_name(target, &target_name);
	}
	if (iPbx.get_callerid_number) {
		iPbx.get_callerid_number(target, &target_number);
	}

	sccp_channel_stop_schedule_digittimout(c);
	c->calltype = SKINNY_CALLTYPE_INBOUND;
	c->state = SCCP_CHANNELSTATE_RINGING;
	c->ringermode = answer ? SKINNY_RINGTYPE_SILENT : SKINNY_RINGTYPE_FEATURE;
	int lineInstance = sccp_device_find_index_for_line(d, c->line->name);
	if(c->line->pickup_modeanswer) {
		sccp_dev_set_keyset(d, lineInstance, c->callid, KEYMODE_RINGIN);                                        // setting early to prevent getting multiple pickup button presses
	}

	char called_number[StationMaxDirnumSize] = { 0 };
	char called_name[StationMaxNameSize] = { 0 };

	sccp_callinfo_t * callinfo_orig = NULL;
	callinfo_orig = sccp_channel_getCallInfo(c);
	iCallInfo.Getter(callinfo_orig,
			 SCCP_CALLINFO_CALLEDPARTY_NAME, &called_name,
			 SCCP_CALLINFO_CALLEDPARTY_NUMBER, &called_number, SCCP_CALLINFO_KEY_SENTINEL);

	{
	}

	res = ast_do_pickup(original, target);
	pbx_channel_unlock(target);
	if(!res) {
		sccp_log((DEBUGCAT_FEATURE))(VERBOSE_PREFIX_3 "%s: pickup of %s succeeded\n", DEV_ID_LOG(d), c->designator);
		sccp_channel_setDevice(c, NULL, FALSE);
		pbx_channel_set_hangupcause(original, AST_CAUSE_ANSWERED_ELSEWHERE);

		pbx_channel_set_hangupcause(c->owner, AST_CAUSE_NORMAL_CLEARING);
		pbx_setstate(c->owner, AST_STATE_RINGING);

		callinfo_orig = sccp_channel_getCallInfo(c);
		iCallInfo.Setter(callinfo_orig,
				 SCCP_CALLINFO_CALLEDPARTY_NAME, called_name,
				 SCCP_CALLINFO_CALLEDPARTY_NUMBER, called_number, SCCP_CALLINFO_ORIG_CALLEDPARTY_NAME, target_name, SCCP_CALLINFO_ORIG_CALLEDPARTY_NUMBER, target_number,
				 SCCP_CALLINFO_ORIG_CALLEDPARTY_REDIRECT_REASON, 5, SCCP_CALLINFO_LAST_REDIRECTINGPARTY_NAME, called_name, SCCP_CALLINFO_LAST_REDIRECTINGPARTY_NUMBER, called_number,
				 SCCP_CALLINFO_HUNT_PILOT_NAME, target_name,
				 SCCP_CALLINFO_HUNT_PILOT_NUMBER, target_number,
				 SCCP_CALLINFO_LAST_REDIRECT_REASON, 5, SCCP_CALLINFO_KEY_SENTINEL);

		sccp_event_t * event = sccp_event_allocate(SCCP_EVENT_LINESTATUS_CHANGED);
		if(event) {
			event->lineStatusChanged.line = sccp_line_retain(c->line);
			event->lineStatusChanged.optional_device = sccp_device_retain(d);
			event->lineStatusChanged.state = SCCP_CHANNELSTATE_PROCEED;
			sccp_event_fire(event);
		}
		sccp_log((DEBUGCAT_FEATURE))(VERBOSE_PREFIX_3 "%s: picking up %s (answer: %s)\n", DEV_ID_LOG(d), c->designator, answer ? "yes" : "no");
		if(answer) {
			/* emulate previous indications, before signalling connected */
			sccp_device_sendcallstate(d, lineInstance, c->callid, SKINNY_CALLSTATE_RINGIN, SKINNY_CALLPRIORITY_LOW, SKINNY_CALLINFO_VISIBILITY_DEFAULT);
			sccp_device_sendcallstate(d, lineInstance, c->callid, SKINNY_CALLSTATE_OFFHOOK, SKINNY_CALLPRIORITY_LOW, SKINNY_CALLINFO_VISIBILITY_DEFAULT);
			sccp_channel_answer(d, c);
		} else {
			sccp_dev_deactivate_cplane(d);
			sccp_parse_alertinfo(c->owner, &c->ringermode);
			sccp_indicate(d, c, SCCP_CHANNELSTATE_RINGING);
			sccp_dev_set_cplane(d, lineInstance, 1);
		}

		if(pbx_test_flag(pbx_channel_flags(original), AST_FLAG_ZOMBIE)) {
			pbx_hangup(original);
		}
	} else {
		sccp_log((DEBUGCAT_CORE))(VERBOSE_PREFIX_3 "SCCP: pickup not done\n");
		sccp_dev_displayprompt(d, lineInstance, c->callid, SKINNY_DISP_TEMP_FAIL " " SKINNY_DISP_OPICKUP, SCCP_DISPLAYSTATUS_TIMEOUT);
		c->setTone(c, SKINNY_TONE_BEEPBONK, SKINNY_TONEDIRECTION_USER);
		sccp_channel_schedule_hangup(c, 5000);
	}
#else
	pbx_log(LOG_NOTICE, "SCCP: pickup not done: this Asterisk build has no call pickup support\n");
#endif
	return res;
}

void sccp_feat_handle_directed_pickup(constDevicePtr d, constLinePtr l, channelPtr maybe_c)
{
#if CS_AST_DO_PICKUP
	if (!l || !d) {
		pbx_log(LOG_ERROR, "SCCP: sccp_feat_handle_directed_pickup() was called without a line or device (caller bug)\n");
		return;
	}
	AUTO_RELEASE(sccp_channel_t, c , sccp_channel_getEmptyChannel(l, d, maybe_c, SKINNY_CALLTYPE_INBOUND, NULL, NULL));
	if(c) {
		if(!sccp_strlen_zero(pbx_builtin_getvar_helper(c->owner, "PICKINGUP"))) {
			pbx_log(LOG_NOTICE, "%s: pickup ignored on line %s: pickup was already pressed on this call\n", d->id, c->line->name);
			return;
		}
		pbx_builtin_setvar_helper(c->owner, "PICKINGUP", "PROGRESS");
		c->softswitch_action = SCCP_SOFTSWITCH_GETPICKUPEXTEN;
		c->ss_data = 0;
		sccp_indicate(d, c, SCCP_CHANNELSTATE_GETDIGITS);
		iPbx.set_callstate(c, AST_STATE_OFFHOOK);
		sccp_channel_stop_schedule_digittimout(c);
	}
#else
	pbx_log(LOG_NOTICE, "SCCP: pickup not done: this Asterisk build has no call pickup support\n");
#endif
}

/* Locks: asterisk channel */
int sccp_feat_directed_pickup(constDevicePtr d, channelPtr c, uint32_t lineInstance, const char *exten)
{
	int res = -1;
#if CS_AST_DO_PICKUP

	pbx_assert(c && c->line && c->owner && d);
	if (!c->line->pickupgroup
#if CS_AST_HAS_NAMEDGROUP
	    && sccp_strlen_zero(c->line->namedpickupgroup)
#endif
	    ) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: directed pickup not done: pickupgroup is not set in sccp.conf\n", d->id);
		return -1;
	}

	char * context = NULL;
	if (sccp_strlen_zero(exten)) {
		pbx_log(LOG_NOTICE, "%s: directed pickup not done: no extension was dialed\n", c->designator);
		return -1;
	}

	if (!iPbx.findPickupChannelByExtenLocked) {
		pbx_log(LOG_WARNING, "%s: directed pickup not done: this Asterisk build has no pickup-by-extension support\n", c->designator);
		return -1;
	}

	if ((context = strchr(exten, '@'))) {
		*context++ = '\0';
	} else {
		if (!sccp_strlen_zero(c->line->directed_pickup_context)) {
			context = pbx_strdupa(c->line->directed_pickup_context);
		} else {
			context = pbx_strdupa(pbx_channel_context(c->owner));
		}
	}
	if (sccp_strlen_zero(context)) {
		pbx_log(LOG_WARNING, "%s: directed pickup of %s not done: no directed_pickup_context and the call has no context\n", c->designator, exten);
		return -1;
	}

	PBX_CHANNEL_TYPE *target = NULL;
	PBX_CHANNEL_TYPE *original = c->owner;
	if (pbx_channel_ref(original)) {
		sccp_log((DEBUGCAT_FEATURE))(VERBOSE_PREFIX_3 "%s: directed pickup of %s@%s requested\n", c->designator, exten, context);

		pbx_str_t * buf = pbx_str_alloca(DEFAULT_PBX_STR_BUFFERSIZE);
		ast_print_namedgroups(&buf, ast_channel_named_pickupgroups(original));
		sccp_log((DEBUGCAT_FEATURE))(VERBOSE_PREFIX_3 "%s: %s looking for calls with pickupgroup %lld, namedpickupgroup '%s'\n", d->id, c->designator, ast_channel_pickupgroup(original), pbx_str_buffer(buf));

		if (iPbx.set_callgroup) {
			iPbx.set_callgroup(c, 0);
		}
		if (iPbx.set_named_callgroups) {
			iPbx.set_named_callgroups(c, NULL);
		}

		target = iPbx.findPickupChannelByExtenLocked(original, exten, context);
		if (target) {
			pbx_builtin_setvar_helper(c->owner, "PICKINGUP", ast_channel_name(target));
			pbx_str_reset(buf);
			ast_print_namedgroups(&buf, ast_channel_named_pickupgroups(target));
			pbx_log(LOG_NOTICE, "%s: picking up %s for %s@%s (callgroup %lld, namedcallgroup '%s')\n", d->id, ast_channel_name(target), exten, context, ast_channel_callgroup(target), pbx_str_buffer(buf));
			iPbx.queue_control(target, AST_CONTROL_REDIRECTING);
			sccp_device_setLamp(d, SKINNY_STIMULUS_CALLPICKUP, lineInstance, SKINNY_LAMP_FLASH);
			res = sccp_feat_perform_pickup(d, c, target, c->line->pickup_modeanswer);
			target = pbx_channel_unref(target);
			sccp_device_setLamp(d, SKINNY_STIMULUS_CALLPICKUP, lineInstance, SKINNY_LAMP_OFF);
		} else {
			pbx_log(LOG_NOTICE, "%s: directed pickup found no ringing call at %s@%s that this line's pickup groups may answer\n", DEV_ID_LOG(d), exten, context);
			pbx_builtin_setvar_helper(c->owner, "PICKINGUP", "FAILED");
			sccp_dev_displayprinotify(d, SKINNY_DISP_NO_CALL_AVAILABLE_FOR_PICKUP, SCCP_MESSAGE_PRIORITY_TIMEOUT, 5);
			if (c->state == SCCP_CHANNELSTATE_ONHOOK || c->state == SCCP_CHANNELSTATE_DOWN) {
				sccp_dev_starttone(d, SKINNY_TONE_BEEPBONK, 0, 0, SKINNY_TONEDIRECTION_USER);
			} else {
				c->setTone(c, SKINNY_TONE_BEEPBONK, SKINNY_TONEDIRECTION_USER);
			}
			sccp_channel_schedule_hangup(c, 500);
		}
		pbx_channel_unref(original);
	} else {
		pbx_log(LOG_WARNING, "%s: directed pickup not done: the call's Asterisk channel is gone\n", c->designator);
	}
#else
	pbx_log(LOG_NOTICE, "SCCP: pickup not done: this Asterisk build has no call pickup support\n");
#endif
	return res;
}

/* Locks: asterisk channel */
int sccp_feat_grouppickup(constDevicePtr d, constLinePtr l, uint32_t lineInstance, channelPtr maybe_c)
{
	int res = -1;

	pbx_assert(d != NULL && l != NULL);
#if CS_AST_DO_PICKUP
	if (!iPbx.findPickupChannelByGroupLocked) {
		pbx_log(LOG_WARNING, "SCCP: group pickup not done: this Asterisk build has no pickup-by-group support\n");
		return -1;
	}

	if (!l->pickupgroup
#if CS_AST_HAS_NAMEDGROUP
	    && sccp_strlen_zero(l->namedpickupgroup)
#endif
	    ) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: group pickup not done: pickupgroup is not set in sccp.conf\n", d->id);
		return -1;
	}
	AUTO_RELEASE(sccp_channel_t, c , sccp_channel_getEmptyChannel(l, d, maybe_c, SKINNY_CALLTYPE_INBOUND, NULL, NULL));
	if (c) {
		if(!sccp_strlen_zero(pbx_builtin_getvar_helper(c->owner, "PICKINGUP"))) {
			pbx_log(LOG_NOTICE, "%s: group pickup ignored on line %s: pickup was already pressed on this call\n", d->id, l->name);
			return -1;
		}
		pbx_builtin_setvar_helper(c->owner, "PICKINGUP", "PROGRESS");
		if (iPbx.set_callgroup) {
			iPbx.set_callgroup(c, 0);
		}
		if (iPbx.set_named_callgroups) {
			iPbx.set_named_callgroups(c, NULL);
		}

		PBX_CHANNEL_TYPE *target = NULL;
		PBX_CHANNEL_TYPE *original = c->owner;
		if (pbx_channel_ref(original)) {
			pbx_str_t * buf = pbx_str_alloca(DEFAULT_PBX_STR_BUFFERSIZE);
			ast_print_namedgroups(&buf, ast_channel_named_pickupgroups(original));
			sccp_log((DEBUGCAT_FEATURE))(VERBOSE_PREFIX_3 "%s: %s (%s@%s) looking for calls with pickupgroup %lld, namedpickupgroup '%s'\n", d->id, c->designator, pbx_channel_exten(original), pbx_channel_context(original),
				ast_channel_pickupgroup(original), pbx_str_buffer(buf));
			sccp_channel_stop_schedule_digittimout(c);
			if ((target = iPbx.findPickupChannelByGroupLocked(c->owner))) {
				pbx_builtin_setvar_helper(c->owner, "PICKINGUP", ast_channel_name(target));
				pbx_str_reset(buf);
				ast_print_namedgroups(&buf, ast_channel_named_pickupgroups(target));
				pbx_log(LOG_NOTICE, "%s: group pickup of %s (callgroup %lld, namedcallgroup '%s')\n", d->id, ast_channel_name(target), ast_channel_callgroup(target), pbx_str_buffer(buf));
				sccp_device_setLamp(d, SKINNY_STIMULUS_GROUPCALLPICKUP, lineInstance, SKINNY_LAMP_FLASH);
				res = sccp_feat_perform_pickup(d, c, target, l->pickup_modeanswer);
				target = pbx_channel_unref(target);
				sccp_device_setLamp(d, SKINNY_STIMULUS_GROUPCALLPICKUP, lineInstance, SKINNY_LAMP_OFF);
			} else {
				pbx_log(LOG_NOTICE, "%s: group pickup found no ringing call in this line's pickup groups\n", DEV_ID_LOG(d));
				pbx_builtin_setvar_helper(c->owner, "PICKINGUP", "FAILED");
				sccp_dev_displayprinotify(d, SKINNY_DISP_NO_CALL_AVAILABLE_FOR_PICKUP, SCCP_MESSAGE_PRIORITY_TIMEOUT, 5);
				if (c->state == SCCP_CHANNELSTATE_ONHOOK || c->state == SCCP_CHANNELSTATE_DOWN) {
					sccp_dev_starttone(d, SKINNY_TONE_BEEPBONK, 0, 0, SKINNY_TONEDIRECTION_USER);
				} else {
					c->setTone(c, SKINNY_TONE_BEEPBONK, SKINNY_TONEDIRECTION_USER);
				}
				sccp_channel_schedule_hangup(c, 500);
			}
			pbx_channel_unref(original);
		} else {
			pbx_log(LOG_WARNING, "%s: group pickup not done: the call's Asterisk channel is gone\n", c->designator);
		}
	}
#else
	pbx_log(LOG_NOTICE, "SCCP: pickup not done: this Asterisk build has no call pickup support\n");
#endif
	return res;
}
#endif														// CS_SCCP_PICKUP

void sccp_feat_voicemail(constDevicePtr d, uint8_t lineInstance)
{
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: voicemail button pressed on line %d\n", d->id, lineInstance);

	{
		AUTO_RELEASE(sccp_channel_t, c , sccp_device_getActiveChannel(d));

		if (c) {
			if (!c->line || sccp_strlen_zero(c->line->vmnum)) {
				sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: no voicemail number set on line %d\n", d->id, lineInstance);
				return;
			}
			if (c->state == SCCP_CHANNELSTATE_OFFHOOK || c->state == SCCP_CHANNELSTATE_DIALING) {
				sccp_copy_string(c->dialedNumber, c->line->vmnum, sizeof(c->dialedNumber));
				sccp_channel_stop_schedule_digittimout(c);
				sccp_pbx_softswitch(c);
				return;
			}

			sccp_dev_displayprompt(d, lineInstance, c->callid, SKINNY_DISP_KEY_IS_NOT_ACTIVE, SCCP_DISPLAYSTATUS_TIMEOUT);
			return;
		}
	}

	if (!lineInstance) {
		if (d->defaultLineInstance) {
			lineInstance = d->defaultLineInstance;
		} else {
			lineInstance = 1;
		}
	}

	AUTO_RELEASE(sccp_line_t, l , sccp_line_find_byid(d, lineInstance));

	if (!l) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: no line with instance %d\n", d->id, lineInstance);

		if (d->defaultLineInstance) {
			l = sccp_line_find_byid(d, d->defaultLineInstance) /*ref_replace*/;
		}
	}
	if (l) {
		if (!sccp_strlen_zero(l->vmnum)) {
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: dialing voicemail %s\n", d->id, l->vmnum);
			AUTO_RELEASE(sccp_channel_t, new_channel, sccp_channel_newcall(l, d, l->vmnum, SKINNY_CALLTYPE_OUTBOUND, NULL, NULL));
		} else {
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: no voicemail number set on line %d\n", d->id, lineInstance);
		}
	} else {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: voicemail not dialed: no line with the default instance %d\n", d->id, d->defaultLineInstance);
	}
}

void sccp_feat_idivert(constDevicePtr d, constLinePtr l, constChannelPtr c)
{
	int instance = 0;

	if (!l) {
		sccp_log((DEBUGCAT_FEATURE)) (VERBOSE_PREFIX_3 "%s: TrnsfVM pressed, but no line found\n", d->id);
		sccp_dev_displayprompt(d, 0, 0, SKINNY_DISP_TRANSVM_WITH_NO_LINE, SCCP_DISPLAYSTATUS_TIMEOUT);
		return;
	}
	if (!l->trnsfvm) {
		sccp_log((DEBUGCAT_FEATURE)) (VERBOSE_PREFIX_3 "%s: TrnsfVM pressed, but trnsfvm is not set in sccp.conf\n", d->id);
		return;
	}
	if (!c || !c->owner) {
		sccp_log((DEBUGCAT_FEATURE)) (VERBOSE_PREFIX_3 "%s: TrnsfVM pressed with no active call\n", d->id);
		sccp_dev_displayprompt(d, 0, 0, SKINNY_DISP_TRANSVM_WITH_NO_CHANNEL, SCCP_DISPLAYSTATUS_TIMEOUT);
		return;
	}

	if (c->state != SCCP_CHANNELSTATE_RINGING && c->state != SCCP_CHANNELSTATE_CALLWAITING) {
		sccp_log((DEBUGCAT_FEATURE)) (VERBOSE_PREFIX_3 "%s: TrnsfVM pressed on a call that is not ringing\n", d->id);
		return;
	}

	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: transferring to voicemail %s\n", d->id, l->trnsfvm);
	iPbx.setChannelCallForward(c, l->trnsfvm);
	instance = sccp_device_find_index_for_line(d, l->name);
	sccp_device_sendcallstate(d, instance, c->callid, SKINNY_CALLSTATE_PROCEED, SKINNY_CALLPRIORITY_LOW, SKINNY_CALLINFO_VISIBILITY_DEFAULT);	/* send connected, so it is not listed as missed call */
	pbx_setstate(c->owner, AST_STATE_BUSY);
	iPbx.queue_control(c->owner, AST_CONTROL_BUSY);
}

void sccp_feat_handle_conference(constDevicePtr d, constLinePtr l, uint8_t lineInstance, channelPtr channel)
{
#ifdef CS_SCCP_CONFERENCE
	if (!l || !d || sccp_strlen_zero(d->id)) {
		pbx_log(LOG_ERROR, "SCCP: sccp_feat_handle_conference() was called without a line or device (caller bug)\n");
		return;
	}

	if (!d->allow_conference) {
		if (lineInstance && channel && channel->callid) {
			sccp_dev_displayprompt(d, lineInstance, channel->callid, SKINNY_DISP_KEY_IS_NOT_ACTIVE, SCCP_DISPLAYSTATUS_TIMEOUT);
		} else {
			sccp_dev_displayprompt(d, 0, 0, SKINNY_DISP_KEY_IS_NOT_ACTIVE, SCCP_DISPLAYSTATUS_TIMEOUT);
		}
		pbx_log(LOG_NOTICE, "%s: conference pressed, but conf_allow is off for this device\n", DEV_ID_LOG(d));
		return;
	}

	AUTO_RELEASE(sccp_channel_t, c , sccp_channel_getEmptyChannel(l, d, channel, SKINNY_CALLTYPE_OUTBOUND, NULL, NULL));
	if (c) {
		c->softswitch_action = SCCP_SOFTSWITCH_GETCONFERENCEROOM;
		c->ss_data = 0;
		c->calltype = SKINNY_CALLTYPE_OUTBOUND;
		sccp_device_sendcallstate(d, lineInstance, c->callid, SKINNY_CALLSTATE_OFFHOOK, SKINNY_CALLPRIORITY_LOW, SKINNY_CALLINFO_VISIBILITY_DEFAULT);
		sccp_channel_set_calledparty(c, "Conferencing...", "100");
		sccp_indicate(d, c, SCCP_CHANNELSTATE_DIALING);
		iPbx.set_callstate(c, AST_STATE_OFFHOOK);
		sccp_channel_stop_schedule_digittimout(c);
		sccp_pbx_softswitch(c);
	} else {
		pbx_log(LOG_WARNING, "%s: conference not started on line %s: the call could not be created (see the previous message)\n", DEV_ID_LOG(d), l->name);
		return;
	}
#endif
}

void sccp_feat_conference_start(constDevicePtr device, const uint32_t lineInstance, channelPtr c)
{
	AUTO_RELEASE(sccp_device_t, d , sccp_device_retain(device));

	if (!d || !c) {
		pbx_log(LOG_WARNING, "%s: conference not started: the device or call is missing\n", DEV_ID_LOG(device));
		return;
	}
#ifdef CS_SCCP_CONFERENCE
	sccp_selectedchannel_t *selectedChannel = NULL;
	boolean_t selectedFound = FALSE;
	PBX_CHANNEL_TYPE *bridged_channel = NULL;

	uint8_t num = sccp_device_numberOfChannels(d);

	sccp_log_and((DEBUGCAT_CONFERENCE + DEBUGCAT_FEATURE)) (VERBOSE_PREFIX_3 "%s: %d calls on the device\n", DEV_ID_LOG(d), num);

	if (d->conference ) {
		SCCP_LIST_LOCK(&d->selectedChannels);
		SCCP_LIST_TRAVERSE(&d->selectedChannels, selectedChannel, list) {
			sccp_channel_t * channel = selectedChannel->channel;
			if (channel && channel != c) {
				if (channel != d->active_channel && channel->state == SCCP_CHANNELSTATE_HOLD) {
					if ((bridged_channel = iPbx.get_bridged_channel(channel->owner))) {
						sccp_log((DEBUGCAT_CONFERENCE + DEBUGCAT_FEATURE)) (VERBOSE_PREFIX_3 "%s: conference: call %s, state %s\n", DEV_ID_LOG(d), pbx_channel_name(bridged_channel), sccp_channelstate2str(channel->state));
						if (!sccp_conference_addParticipatingChannel(d->conference, c, channel, bridged_channel)) {
							sccp_dev_displayprompt(device, lineInstance, c->callid, SKINNY_DISP_INVALID_CONFERENCE_PARTICIPANT, SCCP_DISPLAYSTATUS_TIMEOUT);
						}
						pbx_channel_unref(bridged_channel);
					} else {
						pbx_log(LOG_WARNING, "%s: call %s not added to the conference: it is not bridged to another party\n", DEV_ID_LOG(d), pbx_channel_name(channel->owner));
					}
				} else {
					sccp_log(DEBUGCAT_CONFERENCE) (VERBOSE_PREFIX_3 "%s: conference: %s is active on the shared line on another device; skipped\n", DEV_ID_LOG(d), channel->designator);
				}
				selectedFound = TRUE;
			}
		}
		SCCP_LIST_UNLOCK(&d->selectedChannels);

		if (FALSE == selectedFound) {
			uint8_t i = 0;

			for (i = 0; i < StationMaxButtonTemplateSize; i++) {
				if (d->buttonTemplate[i].type == SKINNY_BUTTONTYPE_LINE && d->buttonTemplate[i].ptr) {
					AUTO_RELEASE(sccp_line_t, line , sccp_line_retain(d->buttonTemplate[i].ptr));

					if (line) {
						sccp_channel_t * channel = NULL;
						SCCP_LIST_LOCK(&line->channels);
						SCCP_LIST_TRAVERSE(&line->channels, channel, list) {
							if (channel != d->active_channel && channel->state == SCCP_CHANNELSTATE_HOLD) {
								if ((bridged_channel = iPbx.get_bridged_channel(channel->owner))) {
									sccp_log((DEBUGCAT_CONFERENCE + DEBUGCAT_FEATURE)) (VERBOSE_PREFIX_3 "%s: conference: call %s, state %s\n", DEV_ID_LOG(d), pbx_channel_name(bridged_channel), sccp_channelstate2str(channel->state));
									if (!sccp_conference_addParticipatingChannel(d->conference, c, channel, bridged_channel)) {
										sccp_dev_displayprompt(device, lineInstance, c->callid, SKINNY_DISP_INVALID_CONFERENCE_PARTICIPANT, SCCP_DISPLAYSTATUS_TIMEOUT);
									}
									pbx_channel_unref(bridged_channel);
								} else {
									pbx_log(LOG_WARNING, "%s: call %s not added to the conference: it is not bridged to another party\n", DEV_ID_LOG(d), pbx_channel_name(channel->owner));
								}
							} else {
								sccp_log(DEBUGCAT_CONFERENCE) (VERBOSE_PREFIX_3 "%s: conference: %s is active on the shared line on another device; skipped\n", DEV_ID_LOG(d), channel->designator);
							}
						}
						SCCP_LIST_UNLOCK(&line->channels);
					}
				}
			}
		}
		sccp_conference_start(d->conference);
	} else {
		sccp_dev_displayprompt(d, lineInstance, c->callid, SKINNY_DISP_CAN_NOT_COMPLETE_CONFERENCE, SCCP_DISPLAYSTATUS_TIMEOUT);
		pbx_log(LOG_WARNING, "%s: conference not started: the conference bridge could not be created\n", DEV_ID_LOG(d));
	}
#else
	sccp_log((DEBUGCAT_CONFERENCE + DEBUGCAT_FEATURE)) (VERBOSE_PREFIX_3 "%s: conference is off for this device\n", DEV_ID_LOG(d));
	sccp_dev_displayprompt(d, lineInstance, c->callid, SKINNY_DISP_KEY_IS_NOT_ACTIVE, SCCP_DISPLAYSTATUS_TIMEOUT);
#endif
}

void sccp_feat_join(constDevicePtr device, constLinePtr l, uint8_t lineInstance, channelPtr c)
{
	AUTO_RELEASE(sccp_device_t, d , sccp_device_retain(device));

	if (!c || !d) {
		pbx_log(LOG_WARNING, "%s: join ignored: the device or call is missing\n", DEV_ID_LOG(device));
		return;
	}
#if CS_SCCP_CONFERENCE
	AUTO_RELEASE(sccp_channel_t, newparticipant_channel , sccp_device_getActiveChannel(d));
	sccp_channel_t *moderator_channel = NULL;
	PBX_CHANNEL_TYPE *bridged_channel = NULL;

	if (!d->allow_conference) {
		pbx_log(LOG_NOTICE, "%s: join pressed, but conf_allow is off for this device\n", DEV_ID_LOG(d));
		sccp_dev_displayprompt(d, lineInstance, c->callid, SKINNY_DISP_SERVICE_IS_NOT_ACTIVE, SCCP_DISPLAYSTATUS_TIMEOUT);
	} else if (!d->conference) {
		pbx_log(LOG_NOTICE, "%s: join pressed, but this device has no active conference\n", DEV_ID_LOG(d));
		sccp_dev_displayprompt(d, lineInstance, c->callid, SKINNY_DISP_NO_CONFERENCE_BRIDGE, SCCP_DISPLAYSTATUS_TIMEOUT);
	} else if (!newparticipant_channel) {
		pbx_log(LOG_NOTICE, "%s: join pressed, but there is no active call to add to the conference\n", DEV_ID_LOG(d));
		sccp_dev_displayprompt(d, lineInstance, c->callid, SKINNY_DISP_CAN_NOT_COMPLETE_CONFERENCE, SCCP_DISPLAYSTATUS_TIMEOUT);
	} else if (newparticipant_channel->conference) {
		pbx_log(LOG_NOTICE, "%s: join ignored: the active call is already in a conference\n", DEV_ID_LOG(d));
		sccp_dev_displayprompt(d, lineInstance, c->callid, SKINNY_DISP_IN_CONFERENCE_ALREADY, SCCP_DISPLAYSTATUS_TIMEOUT);
	} else {
		AUTO_RELEASE(sccp_conference_t, conference , sccp_conference_retain(d->conference));

		SCCP_LIST_LOCK(&(((sccp_line_t *const)l)->channels));
		SCCP_LIST_TRAVERSE(&l->channels, moderator_channel, list) {
			if (conference == moderator_channel->conference ) {
				break;
			}
		}
		SCCP_LIST_UNLOCK(&(((sccp_line_t *const)l)->channels));
		sccp_conference_hold(conference);
		if (moderator_channel) {
			if (newparticipant_channel && moderator_channel != newparticipant_channel) {
				sccp_channel_hold(newparticipant_channel);
				sccp_log((DEBUGCAT_CONFERENCE))(VERBOSE_PREFIX_3 "%s: adding %s to the conference\n", DEV_ID_LOG(d), newparticipant_channel->designator);
				if ((bridged_channel = iPbx.get_bridged_channel(newparticipant_channel->owner))) {
					sccp_log((DEBUGCAT_CONFERENCE + DEBUGCAT_FEATURE)) (VERBOSE_PREFIX_3 "%s: conference: call %s, state %s\n", DEV_ID_LOG(d), pbx_channel_name(bridged_channel), sccp_channelstate2str(newparticipant_channel->state));
					if (!sccp_conference_addParticipatingChannel(conference, moderator_channel, newparticipant_channel, bridged_channel)) {
						sccp_dev_displayprompt(device, lineInstance, c->callid, SKINNY_DISP_INVALID_CONFERENCE_PARTICIPANT, SCCP_DISPLAYSTATUS_TIMEOUT);
					}
					pbx_channel_unref(bridged_channel);
				} else {
					pbx_log(LOG_WARNING, "%s: call %s not added to the conference: it is not bridged to another party\n", DEV_ID_LOG(d), pbx_channel_name(newparticipant_channel->owner));
				}
			} else {
				pbx_log(LOG_NOTICE, "%s: join not done: the call to add is the conference moderator's own call\n", DEV_ID_LOG(d));
				sccp_dev_displayprompt(d, lineInstance, c->callid, SKINNY_DISP_INVALID_CONFERENCE_PARTICIPANT, SCCP_DISPLAYSTATUS_TIMEOUT);
			}
			sccp_conference_update(conference);
			sccp_channel_resume(d, moderator_channel, FALSE);
		} else {
			pbx_log(LOG_NOTICE, "%s: join not done: no call on line %s belongs to this device's conference\n", DEV_ID_LOG(d), l->name);
			sccp_dev_displayprompt(d, lineInstance, c->callid, SKINNY_DISP_KEY_IS_NOT_ACTIVE, SCCP_DISPLAYSTATUS_TIMEOUT);
		}
	}
#else
	pbx_log(LOG_NOTICE, "%s: join pressed, but this build has no conference support\n", DEV_ID_LOG(d));
	sccp_dev_displayprompt(d, lineInstance, c->callid, SKINNY_DISP_SERVICE_IS_NOT_ACTIVE, SCCP_DISPLAYSTATUS_TIMEOUT);
#endif
}

void sccp_feat_conflist(devicePtr d, uint8_t lineInstance, constChannelPtr c)
{
	if (d) {
#ifdef CS_SCCP_CONFERENCE
		if (!d->allow_conference) {
			sccp_dev_displayprompt(d, lineInstance, c ? c->callid : 0, SKINNY_DISP_KEY_IS_NOT_ACTIVE, SCCP_DISPLAYSTATUS_TIMEOUT);
			pbx_log(LOG_NOTICE, "%s: conference list requested, but conf_allow is off for this device\n", DEV_ID_LOG(d));
			return;
		}
		if (c && c->conference) {
			d->conferencelist_active = TRUE;
			sccp_conference_show_list(c->conference, c);
		}
#else
		sccp_dev_displayprompt(d, lineInstance, c->callid, SKINNY_DISP_KEY_IS_NOT_ACTIVE, SCCP_DISPLAYSTATUS_TIMEOUT);
#endif
	}
}

void sccp_feat_handle_meetme(constLinePtr l, uint8_t lineInstance, constDevicePtr d)
{
	if (!l || !d || sccp_strlen_zero(d->id)) {
		pbx_log(LOG_ERROR, "SCCP: sccp_feat_handle_meetme() was called without a line or device (caller bug)\n");
		return;
	}

	{
		AUTO_RELEASE(sccp_channel_t, c , sccp_device_getActiveChannel(d));

		if (c) {
			if (c->state == SCCP_CHANNELSTATE_OFFHOOK && sccp_strlen_zero(c->dialedNumber)) {
				c->setTone(c, SKINNY_TONE_SILENCE, SKINNY_TONEDIRECTION_USER);
				c->softswitch_action = SCCP_SOFTSWITCH_GETMEETMEROOM;
				c->ss_data = 0;									/* this should be found in thread */
				sccp_indicate(d, c, SCCP_CHANNELSTATE_GETDIGITS);
				iPbx.set_callstate(c, AST_STATE_OFFHOOK);
				return;
			}
			if (!sccp_channel_hold(c)) {
				sccp_dev_displayprompt(d, lineInstance, c->callid, SKINNY_DISP_TEMP_FAIL, SCCP_DISPLAYSTATUS_TIMEOUT);
				return;
			}
		}
	}

	AUTO_RELEASE(sccp_channel_t, c , sccp_channel_allocate(l, d));

	if (!c) {
		pbx_log(LOG_WARNING, "%s: meetme not started on line %s: the call could not be created (see the previous message)\n", DEV_ID_LOG(d), l->name);
		return;
	}

	c->softswitch_action = SCCP_SOFTSWITCH_GETMEETMEROOM;
	c->ss_data = 0;

	c->calltype = SKINNY_CALLTYPE_OUTBOUND;

	sccp_indicate(d, c, SCCP_CHANNELSTATE_GETDIGITS);
	iPbx.set_callstate(c, AST_STATE_OFFHOOK);

	if(sccp_pbx_channel_allocate(c, NULL, NULL)) {
		iPbx.set_callstate(c, AST_STATE_OFFHOOK);

		sccp_channel_stop_schedule_digittimout(c);
	}
}

static struct meetmeAppConfig {
	const char *appName;
	const char *defaultMeetmeOption;
} meetmeApps[] = {
	/* clang-format off */
	{"MeetMe", 	"qd"},
	{"ConfBridge", 	"Mac"},
	{"Konference", 	"MTV"}
	/* clang-format on */
};

static void *sccp_feat_meetme_thread(void *data)
{
	struct meetmeAppConfig *app = NULL;

	char ext[SCCP_MAX_EXTENSION];
	char context[SCCP_MAX_CONTEXT];

	char meetmeopts[SCCP_MAX_EXTENSION * 3];

#define SCCP_CONF_SPACER ','

	unsigned int eid = sccp_random();
	AUTO_RELEASE(sccp_channel_t, c, (sccp_channel_t *)data);

	if (!c) {
		pbx_log(LOG_ERROR, "SCCP: meetme thread started without a call (caller bug)\n");
		return NULL;
	}
	AUTO_RELEASE(sccp_device_t, d , sccp_channel_getDevice(c));

	if (!d) {
		pbx_log(LOG_WARNING, "%s: meetme not started: the call has no device attached\n", c->designator);
		return NULL;
	}
	for(uint32_t i = 0; i < sizeof(meetmeApps) / sizeof(struct meetmeAppConfig); i++) {
		if (pbx_findapp(meetmeApps[i].appName)) {
			app = &(meetmeApps[i]);
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: using %s for meetme\n", meetmeApps[i].appName);
			break;
		}
	}

	if (!app) {
		pbx_log(LOG_WARNING, "%s: meetme not started: none of MeetMe, ConfBridge or Konference is loaded in Asterisk\n", c->designator);
		sccp_indicate(d, c, SCCP_CHANNELSTATE_DIALING);
		sccp_channel_set_calledparty(c, SKINNY_DISP_CONFERENCE, c->dialedNumber);
		sccp_channel_setChannelstate(c, SCCP_CHANNELSTATE_PROCEED);
		sccp_channel_send_callinfo(d, c);
		sccp_indicate(d, c, SCCP_CHANNELSTATE_INVALIDCONFERENCE);
		return NULL;
	}
	if (c && c->owner) {
		if (c->pbx_callid) {
			pbx_callid_threadassoc_add(c->pbx_callid);
		}
		if (!pbx_channel_context(c->owner) || sccp_strlen_zero(pbx_channel_context(c->owner))) {
			return NULL;
		}
		if (!sccp_strlen_zero(c->line->meetmeopts)) {
			snprintf(meetmeopts, sizeof(meetmeopts), "%s%c%s", c->dialedNumber, SCCP_CONF_SPACER, c->line->meetmeopts);
		} else if (!sccp_strlen_zero(d->meetmeopts)) {
			snprintf(meetmeopts, sizeof(meetmeopts), "%s%c%s", c->dialedNumber, SCCP_CONF_SPACER, d->meetmeopts);
		} else if (!sccp_strlen_zero(GLOB(meetmeopts))) {
			snprintf(meetmeopts, sizeof(meetmeopts), "%s%c%s", c->dialedNumber, SCCP_CONF_SPACER, GLOB(meetmeopts));
		} else {
			snprintf(meetmeopts, sizeof(meetmeopts), "%s%c%s", c->dialedNumber, SCCP_CONF_SPACER, app->defaultMeetmeOption);
		}

		sccp_copy_string(context, pbx_channel_context(c->owner), sizeof(context));

		snprintf(ext, sizeof(ext), "sccp_meetme_temp_conference_%ud", eid);

		if (!pbx_exists_extension(NULL, context, ext, 1, NULL)) {
			pbx_add_extension(context, 1, ext, 1, NULL, NULL, app->appName, meetmeopts, NULL, "sccp_feat_meetme_thread");
			sccp_log((DEBUGCAT_FEATURE))(VERBOSE_PREFIX_3 "%s: temporary extension %s@%s runs %s(%s)\n", c->designator, ext, context, app->appName, meetmeopts);
		}
		iPbx.setChannelExten(c, ext);

		if(sccp_channel_retain(c)) {
			sccp_indicate(d, c, SCCP_CHANNELSTATE_DIALING);
			sccp_channel_set_calledparty(c, SKINNY_DISP_CONFERENCE, c->dialedNumber);
			sccp_channel_setChannelstate(c, SCCP_CHANNELSTATE_PROCEED);
			sccp_channel_send_callinfo(d, c);
			sccp_indicate(d, c, SCCP_CHANNELSTATE_CONNECTED);

			if(pbx_pbx_run(c->owner)) {
				sccp_indicate(d, c, SCCP_CHANNELSTATE_INVALIDCONFERENCE);
				pbx_log(LOG_WARNING, "%s: meetme failed: Asterisk could not run %s for the temporary extension %s\n", c->designator, app->appName, ext);
			}
			ast_context_remove_extension(context, ext, 1, NULL);
		}
		if (c->pbx_callid) {
			pbx_callid_threadassoc_remove();
		}
	}
	return NULL;
}

void sccp_feat_meetme_start(channelPtr c)
{
	sccp_channel_t *owned = sccp_channel_retain(c);
	if (owned && !sccp_threadpool_add_work(GLOB(general_threadpool), sccp_feat_meetme_thread, owned)) {
		sccp_channel_release(&owned);
	}
}

#define BASE_REGISTRAR "chan_sccp"

typedef struct sccp_barge_info_t {
	PBX_CONTEXT_TYPE *context;
	sccp_channel_t *bargedChannel;
	sccp_channel_t *bargingChannel;
} sccp_barge_info_t;

static void *cleanupTempExtensionContext(void *ptr)
{
	sccp_barge_info_t *barge_info= (struct sccp_barge_info_t *)ptr;
	sccp_channel_t *bdc = barge_info->bargedChannel;
	sccp_channel_t *bgc = barge_info->bargingChannel;

	sccp_log(DEBUGCAT_FEATURE)(VERBOSE_PREFIX_2 "SCCP: destroying temporary context %p\n", barge_info->context);
	pbx_context_destroy(barge_info->context, BASE_REGISTRAR);

	bgc->isBarging = FALSE;
	if (bdc) {
		bdc->isBarged = FALSE;
		bdc->channelStateReason = SCCP_CHANNELSTATEREASON_NORMAL;
		sccp_log(DEBUGCAT_FEATURE)(VERBOSE_PREFIX_2 "%s: sending connected again to reset the remote display\n", bdc->designator);
		bdc->state = bdc->previousChannelState;
		sccp_indicate(NULL, bdc, SCCP_CHANNELSTATE_CONNECTED);
		sccp_channel_release(&barge_info->bargedChannel);
	}

	sccp_channel_release(&barge_info->bargingChannel);
	sccp_free(barge_info);
	return 0;
}

static sccp_barge_info_t * createTempExtensionContext(channelPtr c, const char *context_name, const char *ext, const char *app, const char *opts)
{
	if (c) {
		sccp_barge_info_t *barge_info = (sccp_barge_info_t *) sccp_calloc(1, sizeof(sccp_barge_info_t));
		if (barge_info) {
			if ((barge_info->context = pbx_context_find_or_create(NULL, NULL, context_name, BASE_REGISTRAR))) {
				barge_info->bargingChannel = sccp_channel_retain(c);
				pbx_add_extension(context_name, 1, ext, 1, NULL, NULL, "Answer", NULL, NULL, BASE_REGISTRAR);
				pbx_add_extension(context_name, 1, ext, 2, NULL, NULL, app, pbx_strdup(opts), sccp_free_ptr, BASE_REGISTRAR);
				pbx_add_extension(context_name, 1, ext, 3, NULL, NULL, "Hangup", NULL, NULL, BASE_REGISTRAR);
				sccp_log(DEBUGCAT_FEATURE)(VERBOSE_PREFIX_2 "SCCP: temporary context %s, extension %s runs %s with options '%s'\n", context_name, ext, app, opts);
				sccp_channel_addCleanupJob(c, &cleanupTempExtensionContext, barge_info);
				return barge_info;
			}
			sccp_free(barge_info);
		}
	}
	return NULL;
}

void sccp_feat_handle_barge(constLinePtr l, uint8_t lineInstance, constDevicePtr d, channelPtr maybe_c)
{
	if (!l || !d || sccp_strlen_zero(d->id)) {
		pbx_log(LOG_ERROR, "SCCP: sccp_feat_handle_barge() was called without a line or device (caller bug)\n");
		return;
	}
	if (maybe_c) {
		AUTO_RELEASE(sccp_device_t, remoted, maybe_c->getDevice(maybe_c));
		if (l->isShared && d != remoted) {
			sccp_log(DEBUGCAT_FEATURE)(VERBOSE_PREFIX_2 "%s: barge on a shared line\n", maybe_c->designator);
			AUTO_RELEASE(sccp_channel_t, bargedChannel, sccp_channel_retain(maybe_c));
			AUTO_RELEASE(sccp_linedevice_t, bargingLineDevice, sccp_linedevice_find(d, l));
			if (!sccp_feat_sharedline_barge(bargingLineDevice, bargedChannel)) {
				sccp_dev_starttone(d, SKINNY_TONE_BEEPBONK, lineInstance, 0, SKINNY_TONEDIRECTION_USER);
			}
			return;
		}
		// fall through
	}
	AUTO_RELEASE(sccp_channel_t, c, sccp_channel_getEmptyChannel(l, d, maybe_c, SKINNY_CALLTYPE_OUTBOUND, NULL, NULL));
	if (c) {
		sccp_log(DEBUGCAT_FEATURE)(VERBOSE_PREFIX_2 "%s: barge on line %s\n", d->id, l->name);
		c->softswitch_action = SCCP_SOFTSWITCH_GETBARGEEXTEN;
		c->ss_data = 0;
		sccp_indicate(d, c, SCCP_CHANNELSTATE_GETDIGITS);
		iPbx.set_callstate(c, AST_STATE_OFFHOOK);
		sccp_channel_stop_schedule_digittimout(c);
		if (!maybe_c) {
			sccp_pbx_softswitch(c);
		}
	} else {
		pbx_log(LOG_WARNING, "%s: barge not started on line %s: the call could not be created (see the previous message)\n", DEV_ID_LOG(d), l->name);
		sccp_dev_displayprompt(d, lineInstance, 0, SKINNY_DISP_FAILED_TO_SETUP_BARGE, SCCP_DISPLAYSTATUS_TIMEOUT);
	       	sccp_dev_starttone(d, SKINNY_TONE_BEEPBONK, lineInstance, 0, SKINNY_TONEDIRECTION_USER);
	}
}

int sccp_feat_singleline_barge(channelPtr c, const char * const exten)
{
	if (!c) {
		pbx_log(LOG_ERROR, "SCCP: sccp_feat_singleline_barge() was called without a call (caller bug)\n");
		return FALSE;
	}
	AUTO_RELEASE(sccp_linedevice_t, bargingLD, sccp_channel_getLineDevice(c));
	sccp_barge_info_t *barge_info = NULL;

	if(!bargingLD) {
		pbx_log(LOG_WARNING, "%s: barge not done: the call has no line on a device\n", c->designator);
		return FALSE;
	}
	if (!bargingLD->line) {
		pbx_log(LOG_WARNING, "%s: barge not done: the call's line is missing\n", c->designator);
		sccp_dev_displayprompt(bargingLD->device, bargingLD->lineInstance, 0, SKINNY_DISP_FAILED_TO_SETUP_BARGE, SCCP_DISPLAYSTATUS_TIMEOUT);
		sccp_dev_starttone(bargingLD->device, SKINNY_TONE_BEEPBONK, bargingLD->lineInstance, 0, SKINNY_TONEDIRECTION_USER);
		return FALSE;
	}
	sccp_device_t *d = bargingLD->device;
	AUTO_RELEASE(sccp_line_t, l, sccp_line_retain(bargingLD->line));
	uint16_t lineInstance = bargingLD->lineInstance;

	sccp_log(DEBUGCAT_FEATURE)(VERBOSE_PREFIX_2 "%s: barging in on %s\n", c->designator, exten);
	char ext[SCCP_MAX_EXTENSION];
	char context[SCCP_MAX_CONTEXT];
	char opts[SCCP_MAX_CONTEXT];

	snprintf(context, sizeof(context), "sccp_barge_%s_%s", d->id, l->name);
	snprintf(ext, sizeof(ext), "%s", l->cid_num);
	snprintf(opts, sizeof(opts), "SCCP/%s:SIP/%s:IAX2/%s%c%s", exten, exten, exten, SCCP_CONF_SPACER, "sbBE");
	if ((barge_info = createTempExtensionContext(c, context, ext, "ExtenSpy", opts))) {
		c->softswitch_action = SCCP_SOFTSWITCH_DIAL;
		c->ss_data = 0;

		iPbx.setChannelContext(c, context);
		sccp_copy_string(c->dialedNumber, ext, sizeof(c->dialedNumber));

		c->isBarging = TRUE;
		sccp_channel_setDevice(c, d, TRUE);
		sccp_indicate(d, c, SCCP_CHANNELSTATE_OFFHOOK);
		c->channelStateReason = SCCP_CHANNELSTATEREASON_BARGE;
		sccp_channel_setChannelstate(c, SCCP_CHANNELSTATE_PROCEED);

		sccp_channel_set_callingparty(c, "barger", !sccp_strlen_zero(c->subscriptionId.name) ? c->subscriptionId.name : c->subscriptionId.number);

		sccp_pbx_softswitch(c);

		sccp_log(DEBUGCAT_FEATURE)(VERBOSE_PREFIX_2 "%s: barged in on by %s\n", c->designator, exten);
	} else {
		pbx_log(LOG_ERROR, "SCCP: barge not done: could not find or create dialplan context "
			"'%s'\n", context);
		sccp_dev_displayprompt(d, lineInstance, 0, SKINNY_DISP_FAILED_TO_SETUP_BARGE, SCCP_DISPLAYSTATUS_TIMEOUT);
		return FALSE;
	}
	return TRUE;
}

int sccp_feat_sharedline_barge(constLineDevicePtr bargingLD, channelPtr bargedChannel)
{
	if (!bargingLD) {
		pbx_log(LOG_ERROR, "SCCP: sccp_feat_sharedline_barge() was called without a barging line device (caller bug)\n");
		return FALSE;
	}
	if (!bargingLD->line || !bargedChannel) {
		pbx_log(LOG_WARNING, "%s: barge not done: the barging line or the call to barge in on is missing\n", DEV_ID_LOG(bargingLD->device));
		sccp_dev_displayprompt(bargingLD->device, bargingLD->lineInstance, 0, SKINNY_DISP_FAILED_TO_SETUP_BARGE, SCCP_DISPLAYSTATUS_TIMEOUT);
		return FALSE;
	}
	sccp_device_t *d = bargingLD->device;
	AUTO_RELEASE(sccp_line_t, l, sccp_line_retain(bargingLD->line));
	sccp_barge_info_t *barge_info = NULL;
	uint16_t lineInstance = bargingLD->lineInstance;

	if (bargedChannel->privacy) {
		sccp_dev_displayprompt(d, lineInstance,  0, SKINNY_DISP_PRIVATE, SCCP_DISPLAYSTATUS_TIMEOUT);
		return FALSE;
	}
	if (bargedChannel->isBarged) {
		sccp_dev_displayprompt(d, lineInstance, 0, SKINNY_DISP_ANOTHER_BARGE_EXISTS, SCCP_DISPLAYSTATUS_TIMEOUT);
		return FALSE;
	}
	bargedChannel->isBarged = TRUE;

	AUTO_RELEASE(sccp_linedevice_t, bargedLineDevice, bargedChannel->getLineDevice(bargedChannel));
	if (!bargedLineDevice || !bargedLineDevice->device) {
		sccp_dev_displayprompt(d, lineInstance, 0, SKINNY_DISP_FAILED_TO_SETUP_BARGE, SCCP_DISPLAYSTATUS_TIMEOUT);
		return FALSE;
	}

	AUTO_RELEASE(sccp_channel_t, c , sccp_channel_getEmptyChannel(l, d, NULL, SKINNY_CALLTYPE_OUTBOUND, NULL, NULL));
	if (c) {
		sccp_log(DEBUGCAT_FEATURE)(VERBOSE_PREFIX_2 "%s: barging in on %s\n", c->designator, bargedChannel->designator);
		char ext[SCCP_MAX_EXTENSION];
		char context[SCCP_MAX_CONTEXT];
		char opts[SCCP_MAX_CONTEXT];
		char statusmsg[40];

		snprintf(context, sizeof(context), "sccp_barge_%s_%s", d->id, l->name);
		snprintf(ext, sizeof(ext), "%s", l->cid_num);
		snprintf(opts, sizeof(opts), "SCCP/%s%c%s", bargedChannel->line->name, SCCP_CONF_SPACER, "qbBE");
		if ((barge_info = createTempExtensionContext(c, context, ext, "ChanSpy", opts))) {
			c->softswitch_action = SCCP_SOFTSWITCH_DIAL;
			c->ss_data = 0;

			iPbx.setChannelContext(c, context);
			sccp_copy_string(c->dialedNumber, ext, sizeof(c->dialedNumber));

			c->isBarging = TRUE;
			sccp_channel_setDevice(c, d, TRUE);
			barge_info->bargedChannel = sccp_channel_retain(bargedChannel);
			sccp_indicate(d, c, SCCP_CHANNELSTATE_OFFHOOK);
			c->channelStateReason = SCCP_CHANNELSTATEREASON_BARGE;
			sccp_channel_setChannelstate(c, SCCP_CHANNELSTATE_PROCEED);

			sccp_channel_set_calledparty(c, "barged", !sccp_strlen_zero(bargedChannel->subscriptionId.name) ? bargedChannel->subscriptionId.name : bargedChannel->subscriptionId.number);
			sccp_channel_set_callingparty(c, "barger", !sccp_strlen_zero(c->subscriptionId.name) ? c->subscriptionId.name : c->subscriptionId.number);

			sccp_pbx_softswitch(c);

			pbx_builtin_setvar_helper(c->owner, "BARGED", bargedChannel->designator);
			pbx_builtin_setvar_helper(bargedChannel->owner, "BARGED_BY", c->designator);

			d->indicate->remoteConnected(d, lineInstance, bargedChannel->callid, SKINNY_CALLINFO_VISIBILITY_HIDDEN);

			snprintf(statusmsg, sizeof(statusmsg), SKINNY_DISP_BARGE " " SKINNY_DISP_FROM " %.*s", (int)sizeof(statusmsg) - 8, l->cid_num);
			sccp_dev_set_message(d, statusmsg, SCCP_DISPLAYSTATUS_TIMEOUT, FALSE, FALSE);
			bargedChannel->setTone(bargedChannel, SKINNY_TONE_ZIP, SKINNY_TONEDIRECTION_BOTH);

			sccp_log(DEBUGCAT_FEATURE)(VERBOSE_PREFIX_2 "%s: barged in on by %s\n", c->designator, bargedChannel->designator);
		} else {
			pbx_log(LOG_ERROR, "SCCP: barge not done: could not find or create dialplan context "
				"'%s'\n", context);
			sccp_dev_displayprompt(d, lineInstance, 0, SKINNY_DISP_FAILED_TO_SETUP_BARGE, SCCP_DISPLAYSTATUS_TIMEOUT);
			return FALSE;
		}
	}
	return TRUE;
}

void sccp_feat_handle_cbarge(constLinePtr l, uint8_t lineInstance, constDevicePtr d)
{
	if (!l || !d || sccp_strlen(d->id) < 3) {
		pbx_log(LOG_ERROR, "SCCP: sccp_feat_handle_cbarge() was called without a line or device (caller bug)\n");
		return;
	}

	{
		AUTO_RELEASE(sccp_channel_t, c , sccp_device_getActiveChannel(d));

		if (c) {
			if (c->state == SCCP_CHANNELSTATE_OFFHOOK && sccp_strlen_zero(c->dialedNumber)) {
				c->setTone(c, SKINNY_TONE_SILENCE, SKINNY_TONEDIRECTION_USER);
				c->softswitch_action = SCCP_SOFTSWITCH_GETBARGEEXTEN;
				c->ss_data = 0;									/* this should be found in thread */
				sccp_indicate(d, c, SCCP_CHANNELSTATE_GETDIGITS);
				iPbx.set_callstate(c, AST_STATE_OFFHOOK);
				return;
			} if (!sccp_channel_hold(c)) {
				sccp_dev_displayprompt(d, lineInstance, c->callid, SKINNY_DISP_TEMP_FAIL, SCCP_DISPLAYSTATUS_TIMEOUT);
				return;
			}
		}
	}

	AUTO_RELEASE(sccp_channel_t, c , sccp_channel_allocate(l, d));

	if (!c) {
		pbx_log(LOG_WARNING, "%s: conference barge not started on line %s: the call could not be created (see the previous message)\n", d->id, l->name);
		return;
	}

	c->softswitch_action = SCCP_SOFTSWITCH_GETCBARGEROOM;
	c->ss_data = 0;

	c->calltype = SKINNY_CALLTYPE_OUTBOUND;

	sccp_indicate(d, c, SCCP_CHANNELSTATE_GETDIGITS);
	iPbx.set_callstate(c, AST_STATE_OFFHOOK);

	if(sccp_pbx_channel_allocate(c, NULL, NULL)) {
		iPbx.set_callstate(c, AST_STATE_OFFHOOK);
	}
}

int sccp_feat_cbarge(constChannelPtr c, const char * const conferencenum)
{
	if (!c) {
		return -1;
	}
	AUTO_RELEASE(sccp_device_t, d , sccp_channel_getDevice(c));

	if (!d) {
		return -1;
	}
	uint8_t instance = sccp_device_find_index_for_line(d, c->line->name);

	sccp_dev_displayprompt(d, instance, c->callid, SKINNY_DISP_KEY_IS_NOT_ACTIVE, SCCP_DISPLAYSTATUS_TIMEOUT);
	return 1;
}

void sccp_feat_adhocDial(constDevicePtr d, constLinePtr line)
{
	if (!d || !d->session || !line) {
		return;
	}
	sccp_log((DEBUGCAT_FEATURE + DEBUGCAT_LINE)) (VERBOSE_PREFIX_3 "%s: hotline call\n", d->id);

	AUTO_RELEASE(sccp_channel_t, c , sccp_device_getActiveChannel(d));

	if (c) {
		if ((c->state == SCCP_CHANNELSTATE_DIALING) || (c->state == SCCP_CHANNELSTATE_OFFHOOK)) {
			sccp_copy_string(c->dialedNumber, line->adhocNumber, sizeof(c->dialedNumber));
			sccp_channel_stop_schedule_digittimout(c);

			sccp_pbx_softswitch(c);
			return;
		}
		if (iPbx.send_digits) {
			iPbx.send_digits(c, line->adhocNumber);
		}
	} else {
		if (GLOB(hotline)->line) {
			AUTO_RELEASE(sccp_channel_t, new_channel, sccp_channel_newcall(line, d, line->adhocNumber, SKINNY_CALLTYPE_OUTBOUND, NULL, NULL));
		}
	}
}

void sccp_feat_changed(constDevicePtr device, constLineDevicePtr maybe_ld, sccp_feature_type_t featureType)
{
	if (device) {
		sccp_featButton_changed(device, featureType);
		sccp_event_t *event = sccp_event_allocate(SCCP_EVENT_FEATURE_CHANGED);
		if (event) {
			event->featureChanged.device = sccp_device_retain(device);
			event->featureChanged.optional_linedevice = maybe_ld ? sccp_linedevice_retain(maybe_ld) : NULL;
			event->featureChanged.featureType = featureType;
			sccp_event_fire(event);
		}
		sccp_log(DEBUGCAT_FEATURE)(VERBOSE_PREFIX_3 "%s: feature %s change scheduled\n", device->id, sccp_feature_type2str(featureType));
	}
}

void sccp_feat_monitor(constDevicePtr device, constLinePtr no_line, uint32_t no_lineInstance, constChannelPtr maybe_channel)
{
	sccp_featureConfiguration_t *monitorFeature = (sccp_featureConfiguration_t *const)&device->monitorFeature;
	if (!maybe_channel) {
		if (monitorFeature->status & SCCP_FEATURE_MONITOR_STATE_REQUESTED) {
			monitorFeature->status &= ~SCCP_FEATURE_MONITOR_STATE_REQUESTED;
		} else {
			monitorFeature->status |= SCCP_FEATURE_MONITOR_STATE_REQUESTED;
		}
	} else {
		constChannelPtr channel = maybe_channel;
		pbx_str_t *amiCommandStr = pbx_str_alloca(DEFAULT_PBX_STR_BUFFERSIZE);
		char * outStr = NULL;
		if (!(monitorFeature->status & SCCP_FEATURE_MONITOR_STATE_ACTIVE)) {
			pbx_str_append(&amiCommandStr,0 ,"Action: Monitor\r\n");
			pbx_str_append(&amiCommandStr,0 ,"Channel: %s\r\n", pbx_channel_name(channel->owner));
			pbx_str_append(&amiCommandStr,0 ,"File: mixmonitor-%s-%d_%s.wav\r\n", channel->line->name, channel->callid, iPbx.getChannelUniqueID(channel));
			pbx_str_append(&amiCommandStr,0 ,"Format: wav\r\n");
			pbx_str_append(&amiCommandStr,0 ,"Mix: true\r\n");
			pbx_str_append(&amiCommandStr,0 ,"\r\n");
		} else {
			pbx_str_append(&amiCommandStr,0 ,"Action: StopMonitor\r\n");
			pbx_str_append(&amiCommandStr,0 ,"Channel: %s\r\n", pbx_channel_name(channel->owner));
			pbx_str_append(&amiCommandStr,0 ,"\r\n");
		}
		if (sccp_manager_action2str(pbx_str_buffer(amiCommandStr), &outStr) && outStr) {
			if (
				sccp_strequals(outStr, "Response: Success\r\nMessage: Started monitoring channel\r\n\r\n") ||
				sccp_strequals(outStr, "Response: Success\r\nMessage: Stopped monitoring channel\r\n\r\n")
			) {
				sccp_log((DEBUGCAT_FEATURE)) (VERBOSE_PREFIX_3 "%s: recording request sent to Asterisk\n", DEV_ID_LOG(device));
			} else {
				sccp_dev_displayprinotify(device, SKINNY_DISP_RECORDING_FAILED, SCCP_MESSAGE_PRIORITY_MONITOR, SCCP_DISPLAYSTATUS_TIMEOUT*3);
				pbx_log(LOG_WARNING, "%s: recording not toggled: Asterisk answered the Monitor request with '%s'\n", DEV_ID_LOG(device), outStr);
				monitorFeature->status = SCCP_FEATURE_MONITOR_STATE_DISABLED;
			}
			sccp_free(outStr);
		} else {
			pbx_log(LOG_WARNING, "%s: recording not toggled: the Monitor request to Asterisk failed\n", DEV_ID_LOG(device));
			monitorFeature->status = SCCP_FEATURE_MONITOR_STATE_DISABLED;
		}
	}
	sccp_log((DEBUGCAT_FEATURE)) (VERBOSE_PREFIX_3 "%s: recording state now %s (%d)\n", device->id, sccp_feature_monitor_state2str(monitorFeature->status), monitorFeature->status);
}
