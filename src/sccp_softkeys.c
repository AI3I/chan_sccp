/*!
 * \file        sccp_softkeys.c
 * \brief       SCCP SoftKeys Class
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
#include "sccp_pbx.h"
#include "sccp_channel.h"
#include "sccp_softkeys.h"
#include "sccp_actions.h"
#include "sccp_device.h"
#include "sccp_feature.h"
#include "sccp_line.h"
#include "sccp_linedevice.h"
#include "sccp_session.h"
#include "sccp_utils.h"
#include "sccp_labels.h"

SCCP_FILE_VERSION(__FILE__, "");

const uint8_t softkeysmap[32] = {
	SKINNY_LBL_REDIAL,
	SKINNY_LBL_NEWCALL,
	SKINNY_LBL_HOLD,
	SKINNY_LBL_TRANSFER,
	SKINNY_LBL_CFWDALL,
	SKINNY_LBL_CFWDBUSY,
	SKINNY_LBL_CFWDNOANSWER,
	SKINNY_LBL_BACKSPACE,
	SKINNY_LBL_ENDCALL,
	SKINNY_LBL_RESUME,
	SKINNY_LBL_ANSWER,
	SKINNY_LBL_INFO,
	SKINNY_LBL_CONFRN,
	SKINNY_LBL_PARK,
	SKINNY_LBL_JOIN,
	SKINNY_LBL_MEETME,
	SKINNY_LBL_PICKUP,
	SKINNY_LBL_GPICKUP,
	SKINNY_LBL_MONITOR,
	SKINNY_LBL_CALLBACK,
	SKINNY_LBL_BARGE,
	SKINNY_LBL_DND,
	SKINNY_LBL_CONFLIST,
	SKINNY_LBL_SELECT,
	SKINNY_LBL_PRIVATE,
	SKINNY_LBL_TRNSFVM,
	SKINNY_LBL_DIRTRFR,
	SKINNY_LBL_IDIVERT,
	SKINNY_LBL_VIDEO_MODE,
	SKINNY_LBL_INTRCPT,
	SKINNY_LBL_EMPTY,
	SKINNY_LBL_DIAL,
};

struct softKeySetConfigList softKeySetConfig;

struct sccp_softkeyMap_cb {
	uint32_t event;
	boolean_t channelIsNecessary;
	void (*softkeyEvent_cb) (const sccp_softkeyMap_cb_t * const softkeyMap_cb, constDevicePtr d, constLinePtr l, const uint32_t lineInstance, channelPtr c);
	char *uriactionstr;
};

/* Returns retained line */
static const sccp_line_t * sccp_sk_get_retained_line(constDevicePtr d, constLinePtr l, const uint32_t lineInstance, constChannelPtr c, char *error_str) {
	const sccp_line_t *line = NULL;
	if (l && (line = sccp_line_retain(l))) {
		return line;
	}
	if (c && c->line && (line = sccp_line_retain(c->line))) {
		return line;
	}
	if (d && lineInstance && (line = sccp_line_find_byid(d, lineInstance))) {
		return line;
	}
	if (d && d->currentLine && (line = sccp_dev_getActiveLine(d))) {
		return line;
	}
	if (d && d->defaultLineInstance > 0 && (line = sccp_line_find_byid(d, d->defaultLineInstance))) {
		return line;
	}
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: no line found\n", DEV_ID_LOG(d));
	if(c) {
		c->setTone(c, SKINNY_TONE_ZIPZIP, SKINNY_TONEDIRECTION_USER);
	} else {
		sccp_dev_starttone(d, SKINNY_TONE_ZIPZIP, lineInstance, 0, SKINNY_TONEDIRECTION_USER);
	}
	sccp_dev_displayprompt(d, lineInstance, 0, error_str, SCCP_DISPLAYSTATUS_TIMEOUT);
	return NULL;
}

/* Forces Dialling before timeout */
static void sccp_sk_dial(const sccp_softkeyMap_cb_t * const softkeyMap_cb, constDevicePtr d, constLinePtr l, const uint32_t lineInstance, channelPtr c)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: Dial softkey pressed\n", DEV_ID_LOG(d));
	if (c && !iPbx.getChannelPbx(c)) {									// Prevent dialling if in an inappropriate state.
		/* Only handle this in DIALING state. AFAIK GETDIGITS is used only for call forward and related input functions. (-DD) */
		if (c->state == SCCP_CHANNELSTATE_DIGITSFOLL || c->softswitch_action == SCCP_SOFTSWITCH_GETFORWARDEXTEN) {
			sccp_pbx_softswitch(c);
		}
	}
}

static void sccp_sk_videomode(const sccp_softkeyMap_cb_t * const softkeyMap_cb, constDevicePtr d, constLinePtr l, const uint32_t lineInstance, channelPtr c)
{
#ifdef CS_SCCP_VIDEO
	if (sccp_device_isVideoSupported(d) && c->preferences.video[0] != SKINNY_CODEC_NONE) {
		sccp_log((DEBUGCAT_RTP)) (VERBOSE_PREFIX_3 "%s: video possible; starting video RTP\n", DEV_ID_LOG(d));
		if(!c->rtp.video.instance || sccp_rtp_getState(&c->rtp.video, SCCP_RTP_RECEPTION)) {
			sccp_channel_openMultiMediaReceiveChannel(c);
		}
		if((sccp_rtp_getState(&c->rtp.video, SCCP_RTP_RECEPTION) & SCCP_RTP_STATUS_ACTIVE) && !sccp_rtp_getState(&c->rtp.video, SCCP_RTP_TRANSMISSION)) {
			sccp_channel_startMultiMediaTransmission(c);
		}
		sccp_channel_setVideoMode(c, "user");
	}
#endif
}

static void sccp_sk_redial(const sccp_softkeyMap_cb_t * const softkeyMap_cb, constDevicePtr d, constLinePtr l, const uint32_t lineInstance, channelPtr c)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: Redial softkey pressed\n", DEV_ID_LOG(d));
	if (!d) {
		return;
	}
	char * data = NULL;

	if (d->useRedialMenu) {
		if (d->protocol->type == SCCP_PROTOCOL) {
			if (d->protocolversion < 15) {
				data = "<CiscoIPPhoneExecute><ExecuteItem Priority=\"0\" URL=\"Key:Directories\"/><ExecuteItem Priority=\"0\" URL=\"Key:KeyPad3\"/></CiscoIPPhoneExecute>";
			} else {
				data = "<CiscoIPPhoneExecute><ExecuteItem Priority=\"0\" URL=\"Application:Cisco/PlacedCalls\"/></CiscoIPPhoneExecute>";
			}
		} else {
			data = "<CiscoIPPhoneExecute><ExecuteItem Priority=\"0\" URL=\"Key:Setup\"/><ExecuteItem Priority=\"0\" URL=\"Key:KeyPad1\"/><ExecuteItem Priority=\"0\" URL=\"Key:KeyPad3\"/></CiscoIPPhoneExecute>";
		}

		d->protocol->sendUserToDeviceDataVersionMessage(d, 0, lineInstance, 0, 0, data, 0);
		return;
	}

	if (sccp_strlen_zero(d->redialInformation.number)) {
		sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: no number to redial\n", d->id);
		return;
	}

	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: redialing %s on line instance %d\n", d->id, d->redialInformation.number, d->redialInformation.lineInstance ? d->redialInformation.lineInstance : lineInstance);
	if (c) {
		if (c->state == SCCP_CHANNELSTATE_OFFHOOK) {
			sccp_copy_string(c->dialedNumber, d->redialInformation.number, sizeof(c->dialedNumber));
			sccp_pbx_softswitch(c);
		}
		return;
	}
	AUTO_RELEASE(const sccp_line_t, line,
		     d->redialInformation.lineInstance == 0 ? sccp_line_find_byid(d, d->redialInformation.lineInstance) : sccp_sk_get_retained_line(d, l, lineInstance, c, SKINNY_DISP_NO_LINE_AVAILABLE));
	if(!line) {
		line = sccp_sk_get_retained_line(d, l, lineInstance, c, SKINNY_DISP_NO_LINE_AVAILABLE) /*ref_replace*/;
	}
	if (line) {
		AUTO_RELEASE(sccp_channel_t, new_channel, sccp_channel_newcall(line, d, d->redialInformation.number, SKINNY_CALLTYPE_OUTBOUND, NULL, NULL));
	} else {
		sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: Redial pressed, but the device has no registered line\n", d->id);
	}
}

static void sccp_sk_newcall(const sccp_softkeyMap_cb_t * const softkeyMap_cb, constDevicePtr d, constLinePtr l, const uint32_t lineInstance, channelPtr c)
{
	char *adhocNumber = NULL;
	sccp_speed_t k;
	AUTO_RELEASE(const sccp_line_t, line , sccp_sk_get_retained_line(d, l, lineInstance, c, SKINNY_DISP_NO_LINE_AVAILABLE));
	if (!line) {
		return;
	}

	uint8_t instance = sccp_device_find_index_for_line(d, line->name);
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: NewCall softkey pressed\n", DEV_ID_LOG(d));

	if (!line || instance != lineInstance) {
		sccp_dev_speed_find_byindex(d, lineInstance, TRUE, &k);
		if (sccp_strlen(k.ext) > 0) {
			adhocNumber = pbx_strdupa(k.ext);
		}
	}
	if (!adhocNumber && !sccp_strlen_zero(line->adhocNumber)) {
		adhocNumber = pbx_strdupa(line->adhocNumber);
	}

	/* check if we have an active channel on an other line, that does not have any dialed number
	 * (Can't select line after already off-hook - https://sourceforge.net/p/chan-sccp-b/discussion/652060/thread/878fe455/?limit=25#c06e/6006/a54d)
	 */
	if(!adhocNumber) {
		AUTO_RELEASE(sccp_channel_t, activeChannel, sccp_device_getActiveChannel(d));
		if(activeChannel && activeChannel->line != l && sccp_strlen(activeChannel->dialedNumber) == 0) {
			sccp_channel_endcall(activeChannel);
		}
	}

	AUTO_RELEASE(sccp_channel_t, new_channel, sccp_channel_newcall(line, d, adhocNumber, SKINNY_CALLTYPE_OUTBOUND, NULL, NULL));
}

static void sccp_sk_hold(const sccp_softkeyMap_cb_t * const softkeyMap_cb, constDevicePtr d, constLinePtr l, const uint32_t lineInstance, channelPtr c)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: Hold softkey pressed\n", DEV_ID_LOG(d));
	if (!c) {
		sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: Hold pressed with no call to hold (the softkey set should not offer Hold here)\n", DEV_ID_LOG(d));
		sccp_dev_displayprompt(d, 0, 0, SKINNY_DISP_NO_ACTIVE_CALL_TO_PUT_ON_HOLD, SCCP_DISPLAYSTATUS_TIMEOUT);
		return;
	}
	sccp_channel_hold(c);
}

static void sccp_sk_resume(const sccp_softkeyMap_cb_t * const softkeyMap_cb, constDevicePtr d, constLinePtr l, const uint32_t lineInstance, channelPtr c)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: Resume softkey pressed\n", DEV_ID_LOG(d));
	if (!c) {
		sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: Resume pressed with no call to resume; ignored\n", d->id);
		return;
	}
	sccp_channel_resume(d, c, TRUE);
}

static void sccp_sk_transfer(const sccp_softkeyMap_cb_t * const softkeyMap_cb, constDevicePtr d, constLinePtr l, const uint32_t lineInstance, channelPtr c)
{
	sccp_channel_transfer(c, d);
}

static void sccp_sk_endcall(const sccp_softkeyMap_cb_t * const softkeyMap_cb, constDevicePtr d, constLinePtr l, const uint32_t lineInstance, channelPtr c)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: EndCall softkey pressed\n", DEV_ID_LOG(d));
	if (!c) {
		sccp_log((DEBUGCAT_SOFTKEY))(VERBOSE_PREFIX_3 "%s: EndCall pressed with no call in progress; ignored\n", d->id);
		return;
	}

	if (c->calltype == SKINNY_CALLTYPE_INBOUND && 1 < c->subscribers--) {
		if (d && d->indicate && d->indicate->onhook) {
			d->indicate->onhook(d, lineInstance, c->callid);
		}
	} else {
		sccp_channel_endcall(c);
	}
}

/* Set DND on Current Line if Line is Active otherwise set on Device */
static void sccp_sk_dnd(const sccp_softkeyMap_cb_t * const softkeyMap_cb, constDevicePtr d, constLinePtr l, const uint32_t lineInstance, channelPtr c)
{
	if (!d) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: DND softkey handler called without a device (caller bug)\n");
		return;
	}

	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: DND softkey pressed (status %s, feature enabled %s)\n", DEV_ID_LOG(d), sccp_dndmode2str((sccp_dndmode_t)d->dndFeature.status), d->dndFeature.enabled ? "yes" : "no");

	if (!d->dndFeature.enabled) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: DND softkey ignored: dndFeature is off\n", DEV_ID_LOG(d));
		sccp_dev_displayprompt(d, lineInstance, c ? c->callid : 0, SKINNY_DISP_DND " " SKINNY_DISP_SERVICE_IS_NOT_ACTIVE, SCCP_DISPLAYSTATUS_TIMEOUT);
		sccp_dev_starttone(d, SKINNY_TONE_BEEPBONK, 0, 0, SKINNY_TONEDIRECTION_USER);
		return;
	}

	AUTO_RELEASE(const sccp_line_t, line, l ? sccp_line_retain(l) : NULL);
	AUTO_RELEASE(sccp_device_t, device, sccp_device_retain(d));
	if (device) {
		do {
			if (line) {
				if (line->dndmode == SCCP_DNDMODE_REJECT) {
					if (device->dndFeature.status == SCCP_DNDMODE_OFF) {
						device->dndFeature.status = SCCP_DNDMODE_REJECT;
					} else {
						device->dndFeature.status = SCCP_DNDMODE_OFF;
					}
					break;
				} else if (line->dndmode == SCCP_DNDMODE_SILENT) {
					if (device->dndFeature.status == SCCP_DNDMODE_OFF) {
						device->dndFeature.status = SCCP_DNDMODE_SILENT;
					} else {
						device->dndFeature.status = SCCP_DNDMODE_OFF;
					}
					break;
				}
			} else {
				if (device->dndmode == SCCP_DNDMODE_REJECT) {
					if (device->dndFeature.status == SCCP_DNDMODE_OFF) {
						device->dndFeature.status = SCCP_DNDMODE_REJECT;
					} else {
						device->dndFeature.status = SCCP_DNDMODE_OFF;
					}
					break;
				} else if (device->dndmode == SCCP_DNDMODE_SILENT) {
					if (device->dndFeature.status == SCCP_DNDMODE_OFF) {
						device->dndFeature.status = SCCP_DNDMODE_SILENT;
					} else {
						device->dndFeature.status = SCCP_DNDMODE_OFF;
					}
					break;
				}
			}
			switch (device->dndFeature.status) {
				case SCCP_DNDMODE_OFF:
					device->dndFeature.status = SCCP_DNDMODE_REJECT;
					break;
				case SCCP_DNDMODE_REJECT:
					device->dndFeature.status = SCCP_DNDMODE_SILENT;
					break;
				case SCCP_DNDMODE_SILENT:
					/* fall through */
				default:
					device->dndFeature.status = SCCP_DNDMODE_OFF;
					break;
			}
			sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: DND status now %s (feature enabled %s)\n", DEV_ID_LOG(d), sccp_dndmode2str((sccp_dndmode_t)device->dndFeature.status), device->dndFeature.enabled ? "yes" : "no");
		} while (0);

		sccp_feat_changed(device, NULL, SCCP_FEATURE_DND);
		sccp_dev_check_displayprompt(device);
		sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: DND status now %s (feature enabled %s)\n", DEV_ID_LOG(device), sccp_dndmode2str((sccp_dndmode_t)device->dndFeature.status), device->dndFeature.enabled ? "yes" : "no");
	}
}

static void sccp_sk_backspace(const sccp_softkeyMap_cb_t * const softkeyMap_cb, constDevicePtr d, constLinePtr l, const uint32_t lineInstance, channelPtr c)
{
	if (!d) {
		return;
	}
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: Backspace softkey pressed\n", DEV_ID_LOG(d));

	AUTO_RELEASE(const sccp_line_t, line , sccp_sk_get_retained_line(d, l, lineInstance, c, SKINNY_DISP_NO_LINE_AVAILABLE));
	if (!line) {
		return;
	}
	int len = 0;

	if (((c->state != SCCP_CHANNELSTATE_DIALING) && (c->state != SCCP_CHANNELSTATE_DIGITSFOLL) && (c->state != SCCP_CHANNELSTATE_OFFHOOK) && (c->state != SCCP_CHANNELSTATE_GETDIGITS)) || iPbx.getChannelPbx(c)) {
		return;
	}

	len = sccp_strlen(c->dialedNumber);

	if (!len) {
		sccp_channel_schedule_digittimeout(c, GLOB(firstdigittimeout));
		return;
	}

	if (len >= 1) {
		c->dialedNumber[len - 1] = '\0';
		sccp_channel_schedule_digittimeout(c, GLOB(digittimeout));
	}
	sccp_handle_dialtone(d, line, c);
	sccp_handle_backspace(d, lineInstance, c->callid);
}

static void sccp_sk_answer(const sccp_softkeyMap_cb_t * const softkeyMap_cb, constDevicePtr d, constLinePtr l, const uint32_t lineInstance, channelPtr c)
{
	if (!c) {
		char buf[100];
		pbx_log(LOG_NOTICE, "%s: answer pressed with no call%s%s; reject tone played\n", d->id, l ? " on line " : "", l ? l->name : "");
		snprintf(buf, sizeof(buf), SKINNY_DISP_NO_CHANNEL_TO_PERFORM_ACTION_ON, label2str(SKINNY_LBL_ANSWER));
		sccp_dev_displayprinotify(d, buf, SCCP_MESSAGE_PRIORITY_TIMEOUT, 5);
		sccp_dev_starttone(d, SKINNY_TONE_BEEPBONK, lineInstance, 0, SKINNY_TONEDIRECTION_USER);
		return;
	}
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: Answer softkey pressed on line instance %d\n", DEV_ID_LOG(d), lineInstance);

	/* taking the reference during a locked ast channel allows us to call sccp_channel_answer unlock without the risk of losing the channel */
	if (c->owner) {
		sccp_channel_answer(d, c);
	}
}

static void sccp_sk_dirtrfr(const sccp_softkeyMap_cb_t * const softkeyMap_cb, constDevicePtr d, constLinePtr l, const uint32_t lineInstance, channelPtr c)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: DirTrfr softkey pressed\n", DEV_ID_LOG(d));

	AUTO_RELEASE(sccp_device_t, device , sccp_device_retain(d));

	if (!device) {
		return;
	}

	AUTO_RELEASE(sccp_channel_t, chan1, NULL);
	AUTO_RELEASE(sccp_channel_t, chan2, NULL);
	if ((sccp_device_selectedchannels_count(device)) == 2) {
		sccp_selectedchannel_t *x = NULL;
		SCCP_LIST_LOCK(&device->selectedChannels);
		if((x = SCCP_LIST_FIRST(&device->selectedChannels))) {
			chan1 = sccp_channel_retain(x->channel) /*ref_replace*/;
			sccp_channel_t * tmp = NULL;
			if((tmp = SCCP_LIST_NEXT(x, list)->channel)) {
				chan2 = sccp_channel_retain(tmp) /*ref_replace*/;
			}
		}
		SCCP_LIST_UNLOCK(&device->selectedChannels);
	} else {
		AUTO_RELEASE(sccp_line_t, line , sccp_line_retain(l));
		if (line) {
			if (SCCP_RWLIST_GETSIZE(&line->channels) == 2) {
				SCCP_LIST_LOCK(&line->channels);
				sccp_channel_t *tmp = NULL;
				if ((tmp  = SCCP_LIST_FIRST(&line->channels))) {
					chan1 = sccp_channel_retain(tmp) /*ref_replace*/;
					if ((tmp = SCCP_LIST_NEXT(tmp, list))) {
						chan2 = sccp_channel_retain(tmp) /*ref_replace*/;
					}
				}
				SCCP_LIST_UNLOCK(&line->channels);
			} else if (SCCP_RWLIST_GETSIZE(&line->channels) < 2) {
				sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: direct transfer needs two calls\n", device->id);
				sccp_dev_displayprompt(device, lineInstance, c->callid, SKINNY_DISP_NOT_ENOUGH_CALLS_TO_TRANSFER, SCCP_DISPLAYSTATUS_TIMEOUT);
				return;
			} else {
				sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: direct transfer with more than two calls needs the calls selected first\n", device->id);
				sccp_dev_displayprompt(device, lineInstance, c->callid, SKINNY_DISP_MORE_THAN_TWO_CALLS ", " SKINNY_DISP_USE " " SKINNY_DISP_SELECT, SCCP_DISPLAYSTATUS_TIMEOUT);
				return;
			}
		}
	}

	if (chan1 && chan2) {
		//for using the sccp_channel_transfer_complete function
		//chan2 must be in RINGOUT or CONNECTED state
		sccp_dev_displayprompt(device, lineInstance, c->callid, SKINNY_DISP_CALL_TRANSFER, SCCP_DISPLAYSTATUS_TIMEOUT);
		sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: direct transfer: first call state %d, second call state %d\n", DEV_ID_LOG(device), chan1->state, chan2->state);
		if (chan2->state != SCCP_CHANNELSTATE_CONNECTED && chan1->state == SCCP_CHANNELSTATE_CONNECTED) {
			sccp_channel_t * tmp = chan1;
			chan1 = chan2 /*ref_replace*/;
			chan2 = tmp /*ref_replace*/;
		} else if (chan1->state == SCCP_CHANNELSTATE_HOLD && chan2->state == SCCP_CHANNELSTATE_HOLD) {
			sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: direct transfer: resuming second call (state %d)\n", DEV_ID_LOG(device), chan2->state);
			sccp_channel_resume(device, chan2, TRUE);
			sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: direct transfer: second call resumed (state %d)\n", DEV_ID_LOG(device), chan2->state);
		}
		sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: direct transfer: first call state %d, second call state %d\n", DEV_ID_LOG(device), chan1->state, chan2->state);
		device->transferChannels.transferee = sccp_channel_retain(chan1);
		device->transferChannels.transferer = sccp_channel_retain(chan2);
		if (device->transferChannels.transferee && device->transferChannels.transferer) {
			sccp_channel_transfer_complete(chan2);
		}
	}
}

static void sccp_sk_select(const sccp_softkeyMap_cb_t * const softkeyMap_cb, constDevicePtr d, constLinePtr l, const uint32_t lineInstance, channelPtr c)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: Select softkey pressed\n", DEV_ID_LOG(d));
	sccp_selectedchannel_t * selectedchannel = NULL;
	sccp_msg_t * msg = NULL;
	uint8_t numSelectedChannels = 0;

	uint8_t status = 0;

	if (!d) {
		sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "SCCP: Select handler called without a device (caller bug)\n");
		return;
	}
	if (!c) {
		sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: Select pressed with no call to select\n", DEV_ID_LOG(d));
		return;
	}

	AUTO_RELEASE(sccp_device_t, device , sccp_device_retain(d));
	if (device) {
		if ((selectedchannel = sccp_device_find_selectedchannel(device, c))) {
			SCCP_LIST_LOCK(&device->selectedChannels);
			selectedchannel = SCCP_LIST_REMOVE(&device->selectedChannels, selectedchannel, list);
			SCCP_LIST_UNLOCK(&device->selectedChannels);
			sccp_channel_release(&selectedchannel->channel);
			sccp_free(selectedchannel);
		} else {
			selectedchannel = (sccp_selectedchannel_t *) sccp_calloc(sizeof *selectedchannel, 1);
			if (selectedchannel != NULL) {
				selectedchannel->channel = sccp_channel_retain(c);
				SCCP_LIST_LOCK(&device->selectedChannels);
				SCCP_LIST_INSERT_HEAD(&device->selectedChannels, selectedchannel, list);
				SCCP_LIST_UNLOCK(&device->selectedChannels);
				status = 1;
			} else {
				pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
				return;
			}
		}
		numSelectedChannels = sccp_device_selectedchannels_count(device);

		sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: %d calls selected\n", DEV_ID_LOG(device), numSelectedChannels);

		REQ(msg, CallSelectStatMessage);
		if (!msg) {
			return;
		}
		msg->data.CallSelectStatMessage.lel_status = htolel(status);
		msg->data.CallSelectStatMessage.lel_lineInstance = htolel(lineInstance);
		msg->data.CallSelectStatMessage.lel_callReference = htolel(c->callid);
		sccp_dev_send(d, msg);
	}
}

static void sccp_sk_cfwdall(const sccp_softkeyMap_cb_t * const softkeyMap_cb, constDevicePtr d, constLinePtr l, const uint32_t lineInstance, channelPtr c)
{
	if (!d) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: call forward all handler called without a device (caller bug)\n");
		return;
	}

	AUTO_RELEASE(const sccp_line_t, line , sccp_sk_get_retained_line(d, l, lineInstance, c, SKINNY_DISP_NO_LINE_AVAILABLE));

	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: CFwdALL softkey pressed on line %s, instance %d, call %d\n", DEV_ID_LOG(d), l ? l->name : "(none)", lineInstance, c ? c->callid : 0);

	if (line && d->cfwdall) {
		sccp_feat_handle_callforward(line, d, SCCP_CFWD_ALL, c, lineInstance);
		return;
	}
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: call forward all is off for this device\n", d->id);
	sccp_dev_displayprompt(d, 0, 0, SKINNY_DISP_CFWDALL " " SKINNY_DISP_SERVICE_IS_NOT_ACTIVE, SCCP_DISPLAYSTATUS_TIMEOUT);
	sccp_dev_starttone(d, SKINNY_TONE_BEEPBONK, 0, 0, SKINNY_TONEDIRECTION_USER);
}

static void sccp_sk_cfwdbusy(const sccp_softkeyMap_cb_t * const softkeyMap_cb, constDevicePtr d, constLinePtr l, const uint32_t lineInstance, channelPtr c)
{
	if (!d) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: call forward busy handler called without a device (caller bug)\n");
		return;
	}
	AUTO_RELEASE(const sccp_line_t, line , sccp_sk_get_retained_line(d, l, lineInstance, c, SKINNY_DISP_NO_LINE_AVAILABLE));

	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: CFwdBusy softkey pressed\n", DEV_ID_LOG(d));
	if (line && d->cfwdbusy) {
		sccp_feat_handle_callforward(line, d, SCCP_CFWD_BUSY, c, lineInstance);
		return;
	}
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: call forward busy is off for this device\n", d->id);
	sccp_dev_displayprompt(d, 0, 0, SKINNY_DISP_CFWDBUSY " " SKINNY_DISP_SERVICE_IS_NOT_ACTIVE, SCCP_DISPLAYSTATUS_TIMEOUT);
	sccp_dev_starttone(d, SKINNY_TONE_BEEPBONK, 0, 0, SKINNY_TONEDIRECTION_USER);
}

static void sccp_sk_cfwdnoanswer(const sccp_softkeyMap_cb_t * const softkeyMap_cb, constDevicePtr d, constLinePtr l, const uint32_t lineInstance, channelPtr c)
{
	if (!d) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: call forward no answer handler called without a device (caller bug)\n");
		return;
	}
	AUTO_RELEASE(const sccp_line_t, line , sccp_sk_get_retained_line(d, l, lineInstance, c, SKINNY_DISP_NO_LINE_AVAILABLE));

	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: CFwdNoAnswer softkey pressed\n", DEV_ID_LOG(d));
	if (line && d->cfwdnoanswer) {
		sccp_feat_handle_callforward(line, d, SCCP_CFWD_NOANSWER, c, lineInstance);
		return;
	}
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: call forward no answer is off for this device\n", d->id);
	sccp_dev_displayprompt(d, 0, 0, SKINNY_DISP_CFWDNOANSWER " " SKINNY_DISP_SERVICE_IS_NOT_ACTIVE, SCCP_DISPLAYSTATUS_TIMEOUT);
	sccp_dev_starttone(d, SKINNY_TONE_BEEPBONK, 0, 0, SKINNY_TONEDIRECTION_USER);
}

static void sccp_sk_park(const sccp_softkeyMap_cb_t * const softkeyMap_cb, constDevicePtr d, constLinePtr l, const uint32_t lineInstance, channelPtr c)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: Park softkey pressed\n", DEV_ID_LOG(d));
#ifdef CS_SCCP_PARK
	sccp_channel_park(c);
#else
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "SCCP: Park ignored: chan_sccp was built without park support\n");
#endif
}

static void sccp_sk_trnsfvm(const sccp_softkeyMap_cb_t * const softkeyMap_cb, constDevicePtr d, constLinePtr l, const uint32_t lineInstance, channelPtr c)
{
	AUTO_RELEASE(const sccp_line_t, line , sccp_sk_get_retained_line(d, l, lineInstance, c, SKINNY_DISP_NO_LINE_AVAILABLE));
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: TrnsfVM softkey pressed\n", DEV_ID_LOG(d));
	if (line) {
		sccp_feat_idivert(d, line, c);
	}
}

static void sccp_sk_private(const sccp_softkeyMap_cb_t * const softkeyMap_cb, constDevicePtr device, constLinePtr l, const uint32_t lineInstance, channelPtr c)
{
	if(!device) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: Private handler called without a device (caller bug)\n");
		return;
	}
	AUTO_RELEASE(sccp_device_t, d, sccp_device_retain(device));
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: Private softkey pressed\n", DEV_ID_LOG(d));

	if (!d->privacyFeature.enabled) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: Private ignored: privacy is off for this device\n", d->id);
		sccp_dev_displayprompt(d, lineInstance, 0, SKINNY_DISP_PRIVATE_FEATURE_NOT_ACTIVE, SCCP_DISPLAYSTATUS_TIMEOUT);
		return;
	}

	uint8_t instance = 0;
	AUTO_RELEASE(sccp_channel_t, channel, c ? sccp_channel_retain(c) : NULL);
	if(channel) {
		instance = lineInstance;
	} else {
		AUTO_RELEASE(const sccp_line_t, line , sccp_sk_get_retained_line(d, l, lineInstance, c, SKINNY_DISP_PRIVATE_WITHOUT_LINE_CHANNEL));
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: starting a new private call\n", d->id);
		if(line) {
			instance = sccp_device_find_index_for_line(d, line->name);
			sccp_dev_setActiveLine(d, line);
			sccp_dev_set_cplane(d, instance, 1);
			channel = sccp_channel_newcall(line, d, NULL, SKINNY_CALLTYPE_OUTBOUND, NULL, NULL) /*ref_replace*/;
		}
	}

	if (!channel) {
		sccp_dev_displayprompt(d, lineInstance, 0, SKINNY_DISP_PRIVATE_WITHOUT_LINE_CHANNEL, SCCP_DISPLAYSTATUS_TIMEOUT);
		return;
	}
	// check device->privacyFeature.status before toggling

	channel->privacy = !channel->privacy;
	sccp_softkey_setSoftkeyState(d, KEYMODE_ONHOOKSTEALABLE, SKINNY_LBL_BARGE, channel->privacy);

	// Should actually use the messageStack instead of using displayprompt directly
	if (channel->privacy) {
		sccp_channel_set_calleridPresentation(channel, CALLERID_PRESENTATION_FORBIDDEN);
		pbx_builtin_setvar_helper(channel->owner, "SKINNY_PRIVATE", "1");
		sccp_device_addMessageToStack(d, SCCP_MESSAGE_PRIORITY_PRIVACY, SKINNY_DISP_PRIVATE);
		sccp_dev_displayprompt(d, instance, channel->callid, SKINNY_DISP_PRIVATE, 5);
	} else {
		pbx_builtin_setvar_helper(channel->owner, "SKINNY_PRIVATE", "0");
		sccp_channel_set_calleridPresentation(c, CALLERID_PRESENTATION_ALLOWED);
		sccp_device_clearMessageFromStack(d, SCCP_MESSAGE_PRIORITY_PRIVACY);
	}
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: privacy %s on call %d\n", d->id, channel->privacy ? "enabled" : "disabled", channel->callid);
}

static void sccp_sk_monitor(const sccp_softkeyMap_cb_t * const softkeyMap_cb, constDevicePtr d, constLinePtr l, const uint32_t lineInstance, channelPtr c)
{
	AUTO_RELEASE(const sccp_line_t, line , sccp_sk_get_retained_line(d, l, lineInstance, c, SKINNY_DISP_NO_LINE_AVAILABLE));
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: Monitor softkey pressed\n", DEV_ID_LOG(d));
	if (line) {
		sccp_feat_monitor(d, line, lineInstance, c);
	}
}

static void sccp_sk_conference(const sccp_softkeyMap_cb_t * const softkeyMap_cb, constDevicePtr d, constLinePtr l, const uint32_t lineInstance, channelPtr c)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: Conference softkey pressed\n", DEV_ID_LOG(d));
#ifdef CS_SCCP_CONFERENCE
	AUTO_RELEASE(const sccp_line_t, line , sccp_sk_get_retained_line(d, l, lineInstance, c, SKINNY_DISP_NO_LINE_AVAILABLE));
	if (line) {
		sccp_feat_handle_conference(d, line, lineInstance, c);
	}
#else
	sccp_dev_displayprompt(d, lineInstance, c->callid, SKINNY_DISP_KEY_IS_NOT_ACTIVE, SCCP_DISPLAYSTATUS_TIMEOUT);
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "SCCP: Conference ignored: chan_sccp was built without conference support\n");
#endif
}

static void sccp_sk_conflist(const sccp_softkeyMap_cb_t * const softkeyMap_cb, constDevicePtr d, constLinePtr l, const uint32_t lineInstance, channelPtr c)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: ConfList softkey pressed\n", DEV_ID_LOG(d));
#ifdef CS_SCCP_CONFERENCE
	AUTO_RELEASE(sccp_device_t, device , sccp_device_retain(d));
	if (device) {
		sccp_feat_conflist(device, lineInstance, c);
	}
#else
	sccp_dev_displayprompt(d, lineInstance, c->callid, SKINNY_DISP_KEY_IS_NOT_ACTIVE, SCCP_DISPLAYSTATUS_TIMEOUT);
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "SCCP: ConfList ignored: chan_sccp was built without conference support\n");
#endif
}

static void sccp_sk_join(const sccp_softkeyMap_cb_t * const softkeyMap_cb, constDevicePtr d, constLinePtr l, const uint32_t lineInstance, channelPtr c)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: Join softkey pressed\n", DEV_ID_LOG(d));
#ifdef CS_SCCP_CONFERENCE
	AUTO_RELEASE(const sccp_line_t, line , sccp_sk_get_retained_line(d, l, lineInstance, c, SKINNY_DISP_NO_LINE_AVAILABLE));
	if (line) {
		sccp_feat_join(d, line, lineInstance, c);
	}
#else
	sccp_dev_displayprompt(d, lineInstance, c->callid, SKINNY_DISP_KEY_IS_NOT_ACTIVE, SCCP_DISPLAYSTATUS_TIMEOUT);
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "SCCP: Join ignored: chan_sccp was built without conference support\n");
#endif
}

static void sccp_sk_barge(const sccp_softkeyMap_cb_t * const softkeyMap_cb, constDevicePtr d, constLinePtr l, const uint32_t lineInstance, channelPtr c)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: Barge softkey pressed\n", DEV_ID_LOG(d));
	AUTO_RELEASE(const sccp_line_t, line , sccp_sk_get_retained_line(d, l, lineInstance, c, SKINNY_DISP_NO_LINE_AVAILABLE));
	if (line) {
		sccp_feat_handle_barge(line, lineInstance, d, c);
	}
}

static void sccp_sk_cbarge(const sccp_softkeyMap_cb_t * const softkeyMap_cb, constDevicePtr d, constLinePtr l, const uint32_t lineInstance, channelPtr c)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: cBarge softkey pressed\n", DEV_ID_LOG(d));
	AUTO_RELEASE(const sccp_line_t, line , sccp_sk_get_retained_line(d, l, lineInstance, c, SKINNY_DISP_NO_LINE_AVAILABLE));
	if (line) {
		sccp_feat_handle_cbarge(line, lineInstance, d);
	}
}

static void sccp_sk_meetme(const sccp_softkeyMap_cb_t * const softkeyMap_cb, constDevicePtr d, constLinePtr l, const uint32_t lineInstance, channelPtr c)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: Meetme softkey pressed\n", DEV_ID_LOG(d));

	AUTO_RELEASE(const sccp_line_t, line , sccp_sk_get_retained_line(d, l, lineInstance, c, SKINNY_DISP_NO_LINE_AVAILABLE));
	if (line) {
		sccp_feat_handle_meetme(line, lineInstance, d);
	}
}

static void sccp_sk_pickup(const sccp_softkeyMap_cb_t * const softkeyMap_cb, constDevicePtr d, constLinePtr l, const uint32_t lineInstance, channelPtr c)
{
	assert(d != NULL);
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: PickUp softkey pressed\n", d->id);
#ifndef CS_SCCP_PICKUP
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "SCCP: PickUp ignored: chan_sccp was built without pickup support\n");
#else
	AUTO_RELEASE(const sccp_line_t, line , sccp_sk_get_retained_line(d, l, lineInstance, c, SKINNY_DISP_NO_LINE_AVAILABLE));
	if (line) {
		AUTO_RELEASE(const sccp_device_t, call_assoc_device, (c ? c->getDevice(c) : NULL));
		if (!call_assoc_device || call_assoc_device == d) {
			sccp_feat_handle_directed_pickup(d, line, c);
			return;
		}
		sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: pickup ignored: call %s is already on shared line %s\n", d->id, c ? c->designator : "SCCP", line->name);
	}
#endif
}

static void sccp_sk_gpickup(const sccp_softkeyMap_cb_t * const softkeyMap_cb, constDevicePtr d, constLinePtr l, const uint32_t lineInstance, channelPtr c)
{
	assert(d != NULL);
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: GPickUp softkey pressed\n", d->id);
#ifndef CS_SCCP_PICKUP
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "SCCP: GPickUp ignored: chan_sccp was built without pickup support\n");
#else
	AUTO_RELEASE(const sccp_line_t, line , sccp_sk_get_retained_line(d, l, lineInstance, c, SKINNY_DISP_NO_LINE_AVAILABLE));
	if (line) {
		AUTO_RELEASE(const sccp_device_t, call_assoc_device, (c ? c->getDevice(c) : NULL));
		if (!call_assoc_device || call_assoc_device == d) {
			sccp_feat_grouppickup(d, line, lineInstance, c);
			return;
		}
		sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: group pickup ignored: call %s is already on shared line %s\n", d->id, c ? c->designator : "SCCP", line->name);
	}
#endif
}

static void sccp_sk_info(const sccp_softkeyMap_cb_t * const softkeyMap_cb, constDevicePtr d, constLinePtr l, const uint32_t lineInstance, channelPtr none)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: Info softkey pressed\n", DEV_ID_LOG(d));
	sccp_dev_displayprompt(d, lineInstance, 0, SKINNY_DISP_KEY_IS_NOT_ACTIVE, SCCP_DISPLAYSTATUS_TIMEOUT);
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "SCCP: Info softkey not supported\n");
}

static void sccp_sk_callback(const sccp_softkeyMap_cb_t * const softkeyMap_cb, constDevicePtr d, constLinePtr l, const uint32_t lineInstance, channelPtr c)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: CallBack softkey pressed\n", DEV_ID_LOG(d));
	sccp_dev_displayprompt(d, lineInstance, c->callid, SKINNY_DISP_KEY_IS_NOT_ACTIVE, SCCP_DISPLAYSTATUS_TIMEOUT);
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "SCCP: CallBack softkey not supported\n");
}

static void sccp_sk_empty(const sccp_softkeyMap_cb_t * const softkeyMap_cb, constDevicePtr d, constLinePtr l, const uint32_t lineInstance, channelPtr none)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: empty softkey pressed\n", DEV_ID_LOG(d));
	sccp_dev_displayprompt(d, lineInstance, 0, SKINNY_DISP_KEY_IS_NOT_ACTIVE, SCCP_DISPLAYSTATUS_TIMEOUT);
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "SCCP: empty softkey has no action\n");
}

static void sccp_sk_uriaction(const sccp_softkeyMap_cb_t * const softkeyMap_cb, constDevicePtr d, constLinePtr l, const uint32_t lineInstance, channelPtr c)
{
	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: softkey pressed\n", DEV_ID_LOG(d));
	if (!d) {
		return;
	}
	unsigned int transactionID = sccp_random();

	struct ast_str *paramStr = pbx_str_alloca(DEFAULT_PBX_STR_BUFFERSIZE);
	if (!paramStr) {
		pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
		return;
	}
	ast_str_append(&paramStr, DEFAULT_PBX_STR_BUFFERSIZE, "name=%s", d->id);
	ast_str_append(&paramStr, DEFAULT_PBX_STR_BUFFERSIZE, "&amp;softkey=%s", label2str(softkeyMap_cb->event));
	if (l) {
		ast_str_append(&paramStr, DEFAULT_PBX_STR_BUFFERSIZE, "&amp;line=%s", l->name);
	}
	if (lineInstance) {
		ast_str_append(&paramStr, DEFAULT_PBX_STR_BUFFERSIZE, "&amp;lineInstance=%d", lineInstance);
	}
	if (c) {
		ast_str_append(&paramStr, DEFAULT_PBX_STR_BUFFERSIZE, "&amp;channel=%s", c->designator);
		ast_str_append(&paramStr, DEFAULT_PBX_STR_BUFFERSIZE, "&amp;callid=%d", c->callid);
		if (c->owner) {
			ast_str_append(&paramStr, DEFAULT_PBX_STR_BUFFERSIZE, "&amp;linkedid=%s", iPbx.getChannelLinkedId(c));
			ast_str_append(&paramStr, DEFAULT_PBX_STR_BUFFERSIZE, "&amp;uniqueid=%s", pbx_channel_uniqueid(c->owner));
		}
	}
	ast_str_append(&paramStr, DEFAULT_PBX_STR_BUFFERSIZE, "&amp;appID=%d", APPID_URIHOOK);
	ast_str_append(&paramStr, DEFAULT_PBX_STR_BUFFERSIZE, "&amp;transactionID=%d", transactionID);

	struct ast_str *xmlStr = pbx_str_alloca(DEFAULT_PBX_STR_BUFFERSIZE);
	if (!xmlStr) {
		pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
		return;
	}

	ast_str_append(&xmlStr, DEFAULT_PBX_STR_BUFFERSIZE, "%s", "<CiscoIPPhoneExecute>");

	char delims[] = ",";
	char *uris = pbx_strdupa(softkeyMap_cb->uriactionstr);
	char *tokenrest = NULL;
	char *token = strtok_r(uris, delims, &tokenrest);

	while (token) {
		token = sccp_trimwhitespace(token);
		if (!strncasecmp("http:", token, 5)) {
			if (!strchr(token, '?')) {
				ast_str_append(&xmlStr, DEFAULT_PBX_STR_BUFFERSIZE, "<ExecuteItem Priority=\"0\" URL=\"%s?%s\"/>", token, pbx_str_buffer(paramStr));
			} else {
				ast_str_append(&xmlStr, DEFAULT_PBX_STR_BUFFERSIZE, "<ExecuteItem Priority=\"0\" URL=\"%s&amp;%s\"/>", token, pbx_str_buffer(paramStr));
			}
		} else {
			ast_str_append(&xmlStr, DEFAULT_PBX_STR_BUFFERSIZE, "<ExecuteItem Priority=\"0\" URL=\"%s\"/>", token);
		}
		token = strtok_r(NULL, delims, &tokenrest);
	}
	ast_str_append(&xmlStr, DEFAULT_PBX_STR_BUFFERSIZE, "%s", "</CiscoIPPhoneExecute>");

	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: sending '%s' to the phone\n", DEV_ID_LOG(d), pbx_str_buffer(xmlStr));
	d->protocol->sendUserToDeviceDataVersionMessage(d, APPID_URIHOOK, lineInstance, c ? c->callid : 0, transactionID, pbx_str_buffer(xmlStr), 0);
}

static const struct sccp_softkeyMap_cb softkeyCbMap[] = {
	{SKINNY_LBL_REDIAL, FALSE, sccp_sk_redial, NULL},
	{SKINNY_LBL_NEWCALL, FALSE, sccp_sk_newcall, NULL},
	{SKINNY_LBL_HOLD, TRUE, sccp_sk_hold, NULL},
	{SKINNY_LBL_TRANSFER, TRUE, sccp_sk_transfer, NULL},
	{SKINNY_LBL_CFWDALL, FALSE, sccp_sk_cfwdall, NULL},
	{SKINNY_LBL_CFWDBUSY, FALSE, sccp_sk_cfwdbusy, NULL},
	{SKINNY_LBL_CFWDNOANSWER, FALSE, sccp_sk_cfwdnoanswer, NULL},
	{SKINNY_LBL_BACKSPACE, TRUE, sccp_sk_backspace, NULL},
	{SKINNY_LBL_ENDCALL, TRUE, sccp_sk_endcall, NULL},
	{SKINNY_LBL_RESUME, TRUE, sccp_sk_resume, NULL},
	{SKINNY_LBL_ANSWER, TRUE, sccp_sk_answer, NULL},
	{SKINNY_LBL_INFO, FALSE, sccp_sk_info, NULL},
	{SKINNY_LBL_CONFRN, TRUE, sccp_sk_conference, NULL},
	{SKINNY_LBL_PARK, TRUE, sccp_sk_park, NULL},
	{SKINNY_LBL_JOIN, TRUE, sccp_sk_join, NULL},
	{SKINNY_LBL_MEETME, TRUE, sccp_sk_meetme, NULL},
	{SKINNY_LBL_PICKUP, FALSE, sccp_sk_pickup, NULL},
	{SKINNY_LBL_GPICKUP, FALSE, sccp_sk_gpickup, NULL},
	{SKINNY_LBL_MONITOR, TRUE, sccp_sk_monitor, NULL},
	{SKINNY_LBL_CALLBACK, TRUE, sccp_sk_callback, NULL},
	{SKINNY_LBL_BARGE, TRUE, sccp_sk_barge, NULL},
	{SKINNY_LBL_DND, FALSE, sccp_sk_dnd, NULL},
	{SKINNY_LBL_CONFLIST, TRUE, sccp_sk_conflist, NULL},
	{SKINNY_LBL_SELECT, TRUE, sccp_sk_select, NULL},
	{SKINNY_LBL_PRIVATE, FALSE, sccp_sk_private, NULL},
	{SKINNY_LBL_TRNSFVM, TRUE, sccp_sk_trnsfvm, NULL},
	{SKINNY_LBL_DIRTRFR, TRUE, sccp_sk_dirtrfr, NULL},
	{SKINNY_LBL_IDIVERT, TRUE, sccp_sk_trnsfvm, NULL},
	{SKINNY_LBL_VIDEO_MODE, TRUE, sccp_sk_videomode, NULL},
	{SKINNY_LBL_INTRCPT, TRUE, sccp_sk_resume, NULL},
	{SKINNY_LBL_EMPTY, FALSE, sccp_sk_empty, NULL},
	{SKINNY_LBL_DIAL, TRUE, sccp_sk_dial, NULL},
	{SKINNY_LBL_CBARGE, TRUE, sccp_sk_cbarge, NULL},
};

gcc_inline static const sccp_softkeyMap_cb_t *sccp_getSoftkeyMap_by_SoftkeyEvent(constDevicePtr d, uint32_t event)
{
	uint8_t i = 0;

	const sccp_softkeyMap_cb_t *mySoftkeyCbMap = softkeyCbMap;

	if (d->softkeyset && d->softkeyset->softkeyCbMap) {
		mySoftkeyCbMap = d->softkeyset->softkeyCbMap;
	}
	sccp_log(DEBUGCAT_SOFTKEY) (VERBOSE_PREFIX_3 "%s: softkey map: default %p, set %p, set map %p\n", d->id, softkeyCbMap, d->softkeyset, d->softkeyset ? d->softkeyset->softkeyCbMap : NULL);

	for (i = 0; i < ARRAY_LEN(softkeyCbMap); i++) {
		if (mySoftkeyCbMap[i].event == event) {
			return &mySoftkeyCbMap[i];
		}
	}
	return NULL;
}

void sccp_softkey_pre_reload(void)
{
	sccp_softkey_clear();
}

void sccp_softkey_post_reload(void)
{
	/* only required because softkeys are parsed after devices */
	/* incase softkeysets have changed but device was not reloaded, then d->softkeyset needs to be fixed up */
	sccp_device_t * d = NULL;
	SCCP_RWLIST_RDLOCK(&GLOB(devices));
	SCCP_RWLIST_TRAVERSE(&GLOB(devices), d, list) {
		sccp_softKeySetConfiguration_t * softkeyset = NULL;
		SCCP_LIST_LOCK(&softKeySetConfig);
		SCCP_LIST_TRAVERSE(&softKeySetConfig, softkeyset, list) {
			if(sccp_strcaseequals(d->softkeyDefinition, softkeyset->name)) {
				sccp_log((DEBUGCAT_CONFIG + DEBUGCAT_SOFTKEY))(VERBOSE_PREFIX_3 "softkey set %s attached to %s again\n", softkeyset->name, d->id);
				d->softkeyset = softkeyset;
				d->softKeyConfiguration.modes = softkeyset->modes;
				d->softKeyConfiguration.size = softkeyset->numberOfSoftKeySets;
			}
		}
		SCCP_LIST_UNLOCK(&softKeySetConfig);
	}
	SCCP_RWLIST_UNLOCK(&GLOB(devices));
}

void sccp_softkey_clear(void)
{
	sccp_softKeySetConfiguration_t * k = NULL;
	uint8_t i = 0;

	SCCP_LIST_LOCK(&softKeySetConfig);
	while ((k = SCCP_LIST_REMOVE_HEAD(&softKeySetConfig, list))) {
		for (i = 0; i < StationMaxSoftKeySetDefinition; i++) {
			if (k->modes[i].ptr) {
				sccp_free(k->modes[i].ptr);
				k->modes[i].count = 0;
			}
		}
		if (k->softkeyCbMap) {
			for (i = 0; i < ARRAY_LEN(softkeyCbMap); i++) {
				if (!sccp_strlen_zero(k->softkeyCbMap[i].uriactionstr)) {
					sccp_free(k->softkeyCbMap[i].uriactionstr);
				}
			}
			sccp_free(k->softkeyCbMap);
		}
		sccp_free(k);
	}
	SCCP_LIST_UNLOCK(&softKeySetConfig);
}

sccp_softkeyMap_cb_t __attribute__ ((malloc)) * sccp_softkeyMap_copyStaticallyMapped(void)
{
	sccp_softkeyMap_cb_t *newSoftKeyMap = (sccp_softkeyMap_cb_t *) sccp_malloc((sizeof *newSoftKeyMap) * ARRAY_LEN(softkeyCbMap));
	if (!newSoftKeyMap) {
		pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
		return NULL;
	}
	memcpy(newSoftKeyMap, softkeyCbMap, ARRAY_LEN(softkeyCbMap) * sizeof(sccp_softkeyMap_cb_t));
	sccp_log(DEBUGCAT_SOFTKEY) (VERBOSE_PREFIX_3 "SCCP: copied the static softkey map: %p\n", newSoftKeyMap);
	return newSoftKeyMap;
}

boolean_t sccp_softkeyMap_replaceCallBackByUriAction(sccp_softkeyMap_cb_t * const softkeyMap, uint32_t event, char *uriactionstr)
{
	sccp_log(DEBUGCAT_SOFTKEY) (VERBOSE_PREFIX_3 "SCCP: softkey map %p: %s now runs URI action %s\n", softkeyMap, label2str(event), uriactionstr);
	for(uint i = 0; i < ARRAY_LEN(softkeyCbMap); i++) {
		if (event == softkeyMap[i].event) {
			softkeyMap[i].softkeyEvent_cb = sccp_sk_uriaction;
			softkeyMap[i].uriactionstr = pbx_strdup(sccp_trimwhitespace(uriactionstr));
			return TRUE;
		}
	}
	return FALSE;
}

boolean_t sccp_SoftkeyMap_execCallbackByEvent(devicePtr d, linePtr l, uint32_t lineInstance, channelPtr c, uint32_t event)
{
	if (!d || !event) {
		pbx_log(LOG_ERROR, "SCCP: softkey handler called without a device or event (caller bug)\n");
		return FALSE;
	}
	const sccp_softkeyMap_cb_t *softkeyMap_cb = sccp_getSoftkeyMap_by_SoftkeyEvent(d, event);

	if (!softkeyMap_cb) {
		pbx_log(LOG_WARNING, "%s: softkey %s (%d) has no handler; ignored\n", d->id, label2str(event), event);
		return FALSE;
	}
	if (softkeyMap_cb->channelIsNecessary == TRUE && !c) {
		pbx_log(LOG_NOTICE, "%s: softkey %s pressed with no call; ignored\n", d->id, label2str(event));
		return FALSE;
	}
	sccp_log((DEBUGCAT_SOFTKEY))(VERBOSE_PREFIX_3 "%s: softkey %s on line %s, call %s\n", d->id, label2str(event), l ? l->name : "UNDEF", c ? c->designator : "UNDEF");

	softkeyMap_cb->softkeyEvent_cb(softkeyMap_cb, d, l, lineInstance, c);
	return TRUE;
}

void sccp_softkey_setSoftkeyState(devicePtr device, skinny_keymode_t softKeySet, uint8_t softKey, boolean_t enable)
{
	if (!device || !device->softKeyConfiguration.size) {
		return;
	}

	sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: softkey %s in %s turned %s\n", DEV_ID_LOG(device), label2str(softKey), skinny_keymode2str(softKeySet), enable ? "on" : "off");
	for(uint8_t i = 0; i < device->softKeyConfiguration.modes[softKeySet].count; i++) {
		if (device->softKeyConfiguration.modes[softKeySet].ptr && device->softKeyConfiguration.modes[softKeySet].ptr[i] == softKey) {
			sccp_log((DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_4 "%s: softkey %s found at position %d\n", DEV_ID_LOG(device), label2str(device->softKeyConfiguration.modes[softKeySet].ptr[i]), i);
			if (enable) {
				device->softKeyConfiguration.activeMask[softKeySet] |= (1 << i);
			} else {
				device->softKeyConfiguration.activeMask[softKeySet] &= (~(1 << i));
			}
		}
	}
}

boolean_t __PURE__ sccp_softkey_isSoftkeyInSoftkeySet(constDevicePtr device, const skinny_keymode_t softKeySet, const uint8_t softKey)
{
	if (!device || !device->softKeyConfiguration.size) {
		return FALSE;
	}

	for(uint8_t i = 0; i < device->softKeyConfiguration.modes[softKeySet].count; i++) {
		if (device->softKeyConfiguration.modes[softKeySet].ptr && device->softKeyConfiguration.modes[softKeySet].ptr[i] == softKey) {
			return TRUE;
		}
	}
	return FALSE;
}
