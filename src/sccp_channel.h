/*!
 * \file        sccp_channel.h
 * \brief       SCCP Channel Header
 * \author      Sergio Chersovani <mlists [at] c-net.it>
 * \date
 * \note        Reworked, but based on chan_sccp code.
 *              The original chan_sccp driver that was made by Zozo which itself was derived from the chan_skinny driver.
 *              Modified by Jan Czmok and Julien Goodwin
 * \note        This program is free software and may be modified and distributed under the terms of the GNU General Public License, version 2 or (at your option) any later version.
 *              See the LICENSE file at the top of the source tree.
 */

#pragma once

#include "sccp_rtp.h"

#define sccp_channel_retain(_x)		sccp_refcount_retain_type(sccp_channel_t, _x)
#define sccp_channel_release(_x)	sccp_refcount_release_type(sccp_channel_t, _x)
#define sccp_channel_refreplace(_x, _y)	sccp_refcount_refreplace_type(sccp_channel_t, _x, _y)

__BEGIN_C_EXTERN__
struct sccp_channel {
	const uint32_t callid;
	const uint32_t passthrupartyid;
	sccp_channelstate_t state;
	sccp_channelstate_t previousChannelState;
	sccp_channelstatereason_t channelStateReason;								/*!< Reason the new/current state was set (for example to handle HOLD differently for transfer than normal) */
	skinny_calltype_t calltype;
	sccp_mutex_t lock;

	PBX_CHANNEL_TYPE *owner;
	sccp_line_t * const line;
	SCCP_LIST_ENTRY (sccp_channel_t) list;
	char dialedNumber[SCCP_MAX_EXTENSION];
	const char * const designator;
	sccp_subscription_id_t subscriptionId;
	boolean_t answered_elsewhere;
	boolean_t privacy;
	boolean_t peerIsSCCP;											/*!< Indicates that channel-peer is also SCCP */
	sccp_video_mode_t videomode;

	devicePtr (*getDevice)(constChannelPtr channel);
	lineDevicePtr (*getLineDevice)(constChannelPtr channel);
	void (*setDevice)(channelPtr channel, constDevicePtr device, boolean_t activate);
	char currentDeviceId[StationMaxDeviceNameSize];

	sccp_private_channel_data_t * const privateData;

	skinny_capabilities_t capabilities;									/*!< our channel Capability in preference order */
	skinny_capabilities_t preferences;
	skinny_capabilities_t remoteCapabilities;

	struct ast_format_cap *caps;
	struct {
		uint32_t digittimeout;
		uint32_t totaldigittime;
		uint32_t totaldigittimesquared;
		boolean_t deactivate;
	} enbloc;

	struct {
#ifndef SCCP_ATOMIC
		sccp_mutex_t lock;
#endif
		volatile CAS32_TYPE deny;
		int digittimeout_id;
		int hangup_id;											/*!< Automatic hangup after invalid/congested indication */
		int cfwd_noanswer_id;
	} scheduler;

	sccp_dtmfmode_t dtmfmode;
	struct {
		sccp_rtp_t audio;
		sccp_rtp_t video;
		uint8_t peer_dtmfmode;
	} rtp;

	skinny_ringtype_t ringermode;

	sccp_softswitch_t softswitch_action;									/*!< Simple Switch Action. This is used in dial thread to collect numbers for callforward, pickup and so on -FS */
	uint16_t ss_data;
	uint16_t subscribers;

	int32_t maxBitRate;

	sccp_conference_t *conference;
	uint32_t conference_id;											/*!< Conference ID (might be safer to use instead of conference) */
	uint32_t conference_participant_id;

	void (*setMicrophone)(channelPtr c, boolean_t on);
	boolean_t (*hangupRequest) (constChannelPtr channel);
	boolean_t (*isMicrophoneEnabled) (void);
	boolean_t (*wantsEarlyRTP)(void);
	boolean_t (*progressSent)(void);
	void (*setTone)(constChannelPtr c, skinny_tone_t tone, skinny_toneDirection_t direction);
	void (*setEarlyRTP)(channelPtr c, boolean_t state);
	void (*makeProgress)(channelPtr c);
	const char *const musicclass;

	sccp_channel_t *parentChannel;
	boolean_t isBarged;
	boolean_t isBarging;
	boolean_t isHangingUp;
	boolean_t isRunningPbxThread;

	sccp_autoanswer_t autoanswer_type;
	uint16_t autoanswer_cause;
	pbx_event_subscription_t * parking_sub;

	pbx_callid_t pbx_callid;
};

struct sccp_selectedchannel {
	sccp_channel_t *channel;
	SCCP_LIST_ENTRY (sccp_selectedchannel_t) list;
};
SCCP_API channelPtr SCCP_CALL sccp_channel_allocate(constLinePtr l, constDevicePtr device);
SCCP_API PBX_CHANNEL_TYPE * SCCP_CALL sccp_channel_lock_full(channelPtr c, boolean_t retry_indefinitly);
SCCP_API channelPtr SCCP_CALL sccp_channel_getEmptyChannel(constLinePtr l, constDevicePtr d, channelPtr maybe_c, skinny_calltype_t calltype, PBX_CHANNEL_TYPE * parentChannel, const void *ids);
SCCP_API channelPtr SCCP_CALL sccp_channel_newcall(constLinePtr l, constDevicePtr device, const char *dial, skinny_calltype_t calltype, PBX_CHANNEL_TYPE * parentChannel, const void *ids);

SCCP_API int SCCP_CALL sccp_getCallCount(constLineDevicePtr ld);
SCCP_API void SCCP_CALL sccp_channel_updateChannelCapability(channelPtr channel);
SCCP_API sccp_callinfo_t * const SCCP_CALL sccp_channel_getCallInfo(constChannelPtr channel);
SCCP_API void SCCP_CALL sccp_channel_send_callinfo(constDevicePtr device, constChannelPtr channel);
SCCP_API void SCCP_CALL sccp_channel_send_callinfo2(constChannelPtr channel);
SCCP_API void SCCP_CALL sccp_channel_setChannelstate(channelPtr channel, sccp_channelstate_t state);
SCCP_API void SCCP_CALL sccp_channel_display_callInfo(constChannelPtr channel);
SCCP_API void SCCP_CALL sccp_channel_set_callingparty(constChannelPtr channel, const char *name, const char *number);
SCCP_API boolean_t SCCP_CALL sccp_channel_set_originalCallingparty(constChannelPtr channel, char * name, char * number);
SCCP_API void SCCP_CALL sccp_channel_set_calledparty(constChannelPtr channel, const char * name, const char * number);
SCCP_API boolean_t SCCP_CALL sccp_channel_set_originalCalledparty(constChannelPtr channel, char * name, char * number);
SCCP_API void SCCP_CALL sccp_channel_set_calleridPresentation(constChannelPtr channel, sccp_callerid_presentation_t presentation);
SCCP_API boolean_t SCCP_CALL sccp_channel_finishHolePunch(constChannelPtr channel, boolean_t keepChannelOpen);
SCCP_API boolean_t SCCP_CALL sccp_channel_holePunchPending(constChannelPtr c);
SCCP_API void SCCP_CALL sccp_channel_startHolePunch(constChannelPtr c);
SCCP_API boolean_t __PURE__ SCCP_CALL      sccp_channel_isAnswering(constChannelPtr c);
SCCP_API void SCCP_CALL sccp_channel_openReceiveChannel(constChannelPtr channel);
SCCP_API int SCCP_CALL sccp_channel_receiveChannelOpen(devicePtr d, channelPtr c);
SCCP_API void SCCP_CALL sccp_channel_closeReceiveChannel(constChannelPtr channel, boolean_t KeepPortOpen);
SCCP_API void SCCP_CALL sccp_channel_startMediaTransmission(constChannelPtr channel);
SCCP_API int SCCP_CALL sccp_channel_mediaTransmissionStarted(devicePtr d, channelPtr c);
SCCP_API void SCCP_CALL sccp_channel_stopMediaTransmission(constChannelPtr channel, boolean_t KeepPortOpen);
SCCP_API void SCCP_CALL sccp_channel_updateMediaTransmission(constChannelPtr channel);
SCCP_API void SCCP_CALL sccp_channel_openMultiMediaReceiveChannel(constChannelPtr channel);
SCCP_API int SCCP_CALL sccp_channel_receiveMultiMediaChannelOpen(constDevicePtr d, channelPtr c);
SCCP_API void SCCP_CALL sccp_channel_closeMultiMediaReceiveChannel(constChannelPtr channel, boolean_t KeepPortOpen);
SCCP_API void SCCP_CALL sccp_channel_startMultiMediaTransmission(constChannelPtr channel);
SCCP_API int SCCP_CALL sccp_channel_multiMediaTransmissionStarted(constDevicePtr d, channelPtr c);
SCCP_API void SCCP_CALL sccp_channel_stopMultiMediaTransmission(constChannelPtr channel, boolean_t KeepPortOpen);

SCCP_API sccp_rtp_status_t SCCP_CALL sccp_channel_closeAllMediaTransmitAndReceive(constChannelPtr channel);

SCCP_API boolean_t SCCP_CALL sccp_channel_transfer_on_hangup(constChannelPtr channel);
SCCP_INLINE void SCCP_CALL sccp_channel_stop_schedule_digittimout(constChannelPtr channel);
SCCP_INLINE void SCCP_CALL sccp_channel_schedule_hangup(constChannelPtr channel, int timeout);
SCCP_INLINE void SCCP_CALL sccp_channel_schedule_digittimeout(constChannelPtr channel, int timeout);
SCCP_INLINE void SCCP_CALL sccp_channel_schedule_cfwd_noanswer(constChannelPtr channel, int timeout);
SCCP_INLINE void SCCP_CALL sccp_channel_stop_schedule_cfwd_noanswer(constChannelPtr channel);
SCCP_API void SCCP_CALL sccp_channel_end_forwarding_channel(channelPtr orig_channel);
SCCP_API void SCCP_CALL sccp_channel_endcall(channelPtr channel);
SCCP_API void SCCP_CALL sccp_channel_StatisticsRequest(constChannelPtr channel);
SCCP_API void SCCP_CALL sccp_channel_answer(constDevicePtr device, channelPtr channel);
SCCP_API void SCCP_CALL sccp_channel_stop_and_deny_scheduled_tasks(constChannelPtr channel);
SCCP_API void SCCP_CALL sccp_channel_addCleanupJob(channelPtr c, void *(*function_p) (void *), void *arg_p);
SCCP_API void SCCP_CALL sccp_channel_clean(channelPtr channel);
SCCP_API void SCCP_CALL sccp_channel_transfer(channelPtr channel, constDevicePtr device);
SCCP_API void SCCP_CALL sccp_channel_transfer_release(devicePtr d, channelPtr c);
SCCP_API void SCCP_CALL sccp_channel_transfer_cancel(devicePtr d, channelPtr c);
SCCP_API void SCCP_CALL sccp_channel_transfer_complete(channelPtr sccp_destination_local_channel);
SCCP_API int SCCP_CALL sccp_channel_hold(channelPtr channel);
SCCP_API int SCCP_CALL sccp_channel_resume(constDevicePtr device, channelPtr channel, boolean_t swap_channels);
SCCP_API int SCCP_CALL sccp_channel_forward(constChannelPtr sccp_channel_parent, constLineDevicePtr ld, const char * fwdNumber);
SCCP_API devicePtr SCCP_CALL sccp_channel_getDevice(constChannelPtr channel);
SCCP_API lineDevicePtr SCCP_CALL sccp_channel_getLineDevice(constChannelPtr channel);
SCCP_API void SCCP_CALL sccp_channel_setDevice(channelPtr channel, constDevicePtr device, boolean_t activate);

#ifdef CS_SCCP_PARK
SCCP_API void SCCP_CALL sccp_channel_park(constChannelPtr channel);
#endif

SCCP_API boolean_t SCCP_CALL sccp_channel_setPreferredCodec(channelPtr c, const char * data);
SCCP_API sccp_video_mode_t __PURE__ SCCP_CALL sccp_channel_getVideoMode(constChannelPtr c);
SCCP_API boolean_t SCCP_CALL sccp_channel_setVideoMode(channelPtr c, const char *data);
SCCP_API int SCCP_CALL sccp_channel_callwaiting_tone_interval(constDevicePtr device, constChannelPtr channel);

SCCP_API channelPtr SCCP_CALL sccp_channel_find_byid(uint32_t callid);
SCCP_API channelPtr SCCP_CALL sccp_find_channel_on_line_byid(constLinePtr l, uint32_t id);
SCCP_API channelPtr SCCP_CALL sccp_channel_find_bypassthrupartyid(uint32_t passthrupartyid);
SCCP_API channelPtr SCCP_CALL sccp_channel_find_bystate_on_line(constLinePtr l, sccp_channelstate_t state);
SCCP_API channelPtr SCCP_CALL sccp_channel_find_bystate_on_device(constDevicePtr device, sccp_channelstate_t state);
SCCP_API channelPtr SCCP_CALL sccp_find_channel_by_lineInstance_and_callid(constDevicePtr d, const uint32_t lineInstance, const uint32_t callid);
SCCP_API channelPtr SCCP_CALL sccp_find_channel_by_buttonIndex_and_callid(constDevicePtr d, const uint32_t buttonIndex, const uint32_t callid);
SCCP_API channelPtr SCCP_CALL sccp_channel_find_on_device_bypassthrupartyid(constDevicePtr d, uint32_t passthrupartyid);
SCCP_API sccp_selectedchannel_t * SCCP_CALL sccp_device_find_selectedchannel(constDevicePtr d, constChannelPtr channel);
SCCP_API uint8_t SCCP_CALL sccp_device_selectedchannels_count(constDevicePtr device);
__END_C_EXTERN__
