/*!
 * \file        sccp_conference.c
 * \brief       SCCP conference support
 * \author      Marcello Ceschia <marcelloceschia [at] users.sorceforge.net>
 * \note        Reworked, but based on chan_sccp code.
 *
 */

#include "config.h"
#include "common.h"
#include "sccp_conference.h"
#include "sccp_channel.h"
#include "sccp_atomic.h"
#include "sccp_device.h"
#include "sccp_indicate.h"
#include "sccp_line.h"
#include "sccp_linedevice.h"
#include "sccp_utils.h"
#include "sccp_labels.h"
#include "sccp_threadpool.h"
#include <asterisk/say.h>

/*** DOCUMENTATION
	<manager name="SCCPConference" language="en_US">
		<synopsis>Control sccp conference and it's participants.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml"
				xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
			<parameter name="ConferenceId" required="true">
				<para>Id of the conference</para>
			</parameter>
			<parameter name="ParticipantId">
				<para>Id of the participant</para>
			</parameter>
			<parameter name="Command" required="true">
				<para>Command to be executed on this <replaceable>ConferenceId</replaceable> and <replaceable>ParticipantId</replaceable> combination</para>
				<enumlist>
					<enum name="EndConf">
						<para>End the conference specified via <replaceable>ConferenceId</replaceable>.</para>
					</enum>
					<enum name="Kick">
						<para>Kick a <replaceable>ParticipantId</replaceable> out of <replaceable>ConferenceId</replaceable>.</para>
					</enum>
					<enum name="Mute">
						<para>Mute <replaceable>ParticipantId</replaceable> on <replaceable>ConferenceId</replaceable>.</para>
					</enum>
					<enum name="Moderate">
						<para>Promote <replaceable>ParticipantId</replaceable> on <replaceable>ConferenceId</replaceable> to moderator status.</para>
					</enum>
					<enum name="Invite">
						<para>Invite a new participant to this <replaceable>ConferenceId</replaceable> (Not implemented).</para>
					</enum>
				</enumlist>
			</parameter>
		</syntax>
		<description>
			<para>Control sccp conference and it's participants.</para>
		</description>
	</manager>
***/

#ifdef CS_SCCP_CONFERENCE

#include <asterisk/bridge.h>
#include <asterisk/bridge_channel.h>
#include <asterisk/bridge_features.h>
#include <asterisk/bridge_technology.h>
#ifdef HAVE_PBX_BRIDGING_ROLES_H
#include <asterisk/bridging_roles.h>
#endif
#include <asterisk/callerid.h>
#include <asterisk/causes.h>
#include <asterisk/format_cap.h>

#define sccp_participant_retain(_x)		sccp_refcount_retain_type(sccp_participant_t, _x)
#define sccp_participant_release(_x)		sccp_refcount_release_type(sccp_participant_t, _x)
#define sccp_participant_refreplace(_x, _y)	sccp_refcount_refreplace_type(sccp_participant_t, _x, _y)

SCCP_FILE_VERSION(__FILE__, "");
static uint32_t lastConferenceID = 99;
static const uint32_t appID = APPID_CONFERENCE;
typedef struct sccp_participant sccp_participant_t;

struct sccp_conference {
	ast_mutex_t lock;
	uint32_t id;
	int32_t num_moderators;
	const char *linkedid;
	struct ast_bridge *bridge;										/*!< Shared Ast_Bridge used by this conference */
	struct {
		ast_mutex_t lock;
		char language[SCCP_MAX_LANGUAGE];
		PBX_CHANNEL_TYPE *channel;
	} playback;

	SCCP_RWLIST_HEAD (, sccp_participant_t) participants;
	SCCP_LIST_ENTRY (sccp_conference_t) list;

	volatile int finishing;
	boolean_t isLocked;
	boolean_t isOnHold;
	boolean_t mute_on_entry;
	boolean_t playback_announcements;
};

struct sccp_participant {
	boolean_t pendingRemoval;
	uint32_t id;
	sccp_channel_t *channel;
	sccp_device_t *device;
	PBX_CHANNEL_TYPE *conferenceBridgePeer;
	struct ast_bridge_channel *bridge_channel;
	pthread_t joinThread;											/*!< Running in this Thread */
	sccp_conference_t *conference;
	char *final_announcement;										/*!< Announcement played back to participant after leaving the bridge */
	boolean_t isModerator;
	boolean_t onMusicOnHold;
	boolean_t playback_announcements;
	uint32_t callReference;
	uint32_t lineInstance;
	uint32_t transactionID;

	SCCP_RWLIST_ENTRY (sccp_participant_t) list;

	char PartyName[StationMaxNameSize];
	char PartyNumber[StationMaxDirnumSize];

	struct ast_bridge_features features;
};

static SCCP_LIST_HEAD (, sccp_conference_t) conferences;

#define participantPtr sccp_participant_t *const
#define constParticipantPtr const sccp_participant_t *const

static void *sccp_conference_thread(void *data);
void sccp_conference_update_callInfo(constChannelPtr channel, PBX_CHANNEL_TYPE * pbxChannel, constParticipantPtr participant, uint32_t conferenceID);
int playback_to_channel(participantPtr participant, const char *filename, int say_number);
int playback_to_conference(conferencePtr conference, const char *filename, int say_number);
sccp_conference_t *sccp_conference_findByID(uint32_t identifier);
sccp_participant_t *sccp_participant_findByID(constConferencePtr conference, uint32_t identifier);
sccp_participant_t *sccp_participant_findByChannel(constConferencePtr conference, constChannelPtr channel);
sccp_participant_t *sccp_participant_findByDevice(constConferencePtr conference, constDevicePtr device);
sccp_participant_t *sccp_participant_findByPBXChannel(constConferencePtr conference, PBX_CHANNEL_TYPE * channel);
void sccp_conference_play_music_on_hold_to_participant(constConferencePtr conference, participantPtr participant, boolean_t start);
static sccp_participant_t *sccp_conference_createParticipant(constConferencePtr conference);
static void sccp_conference_addParticipant_toList(constConferencePtr conference, constParticipantPtr participant);
void pbx_builtin_setvar_int_helper(PBX_CHANNEL_TYPE * channel, const char *var_name, int intvalue);
static void sccp_conference_update_conflist(conferencePtr conference);
void __sccp_conference_hide_list(participantPtr participant);
void sccp_conference_invite_participant(constConferencePtr conference, constParticipantPtr moderator);
void sccp_conference_kick_participant(constConferencePtr conference, participantPtr participant);
void *sccp_participant_kicker(void *data);
void sccp_conference_toggle_mute_participant(constConferencePtr conference, participantPtr participant);
void sccp_conference_promote_demote_participant(conferencePtr conference, participantPtr participant, constParticipantPtr moderator);

void sccp_conference_module_start(void)
{
	SCCP_LIST_HEAD_INIT(&conferences);
}

void sccp_conference_module_stop(void)
{
	SCCP_LIST_HEAD_DESTROY(&conferences);
}

/*
 * Cleanup after conference refcount goes to zero (refcount destroy)
 */
static int __sccp_conference_destroy(const void *data)
{
	conferencePtr conference = (conferencePtr) data;
	if (!conference) {
		return -1;
	}

	if (conference->playback.channel) {
		sccp_log_and((DEBUGCAT_CONFERENCE + DEBUGCAT_HIGH)) (VERBOSE_PREFIX_4 "SCCPCONF/%04d: destroying the playback channel\n", conference->id);
		sccpconf_announce_channel_depart(conference->playback.channel);
		pbx_hangup(conference->playback.channel);
		conference->playback.channel = NULL;
	}
	sccp_log((DEBUGCAT_CONFERENCE + DEBUGCAT_HIGH)) (VERBOSE_PREFIX_4 "SCCPCONF/%04d: destroying conference\n", conference->id);
	sccp_free(conference->linkedid);
	if (conference->bridge) {
		pbx_bridge_destroy(conference->bridge, AST_CAUSE_NORMAL_CLEARING);
	}
	SCCP_RWLIST_HEAD_DESTROY(&conference->participants);
	pbx_mutex_destroy(&conference->playback.lock);

#ifdef CS_MANAGER_EVENTS
	if (GLOB(callevents)) {
		manager_event(EVENT_FLAG_USER, "SCCPConfEnd", "ConfId: %d\r\n", conference->id);
	}
#endif
	return 0;
}

/*
 * Cleanup after participant refcount goes to zero (refcount destroy)
 */
static int __sccp_participant_destroy(const void *data)
{
	sccp_participant_t * participant = (sccp_participant_t *)data;
	sccp_log_and((DEBUGCAT_CONFERENCE + DEBUGCAT_HIGH)) (VERBOSE_PREFIX_4 "SCCPCONF/%04d: destroying participant %d (%p)\n", participant->conference->id, participant->id, participant);

	if (participant->isModerator && participant->conference) {
		participant->conference->num_moderators--;
	}
	pbx_bridge_features_cleanup(&participant->features);
#ifdef CS_MANAGER_EVENTS
	if (GLOB(callevents)) {
		manager_event(EVENT_FLAG_CALL, "SCCPConfLeave", "ConfId: %d\r\n" "PartId: %d\r\n", participant->conference ? participant->conference->id : 0, participant->id);
	}
#endif
	if (participant->channel) {
#if CS_REFCOUNT_DEBUG
		sccp_refcount_removeRelationship(participant->channel, participant);
#endif
		participant->channel->conference_id = 0;
		participant->channel->conference_participant_id = 0;
		if (participant->channel->conference) {
			sccp_conference_release(&participant->channel->conference);
		}
		sccp_channel_release(&participant->channel);
	}
	if (participant->device) {
#if CS_REFCOUNT_DEBUG
		sccp_refcount_removeRelationship(participant->device, participant);
#endif
		participant->device->conferencelist_active = FALSE;
		if (participant->device->conference) {
			sccp_conference_release(&participant->device->conference);
		}
		sccp_device_release(&participant->device);
	}
	sccp_conference_release(&participant->conference);
	return 0;
}

sccp_conference_t *sccp_conference_create(devicePtr device, channelPtr channel)
{
	sccp_conference_t *conference = NULL;
	char conferenceIdentifier[REFCOUNT_INDENTIFIER_SIZE];
	int conferenceID = ++lastConferenceID;
	uint32_t bridgeCapabilities = 0;

	sccp_log((DEBUGCAT_CORE + DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_3 "SCCP: creating conference SCCPCONF/%04d\n", conferenceID);

	snprintf(conferenceIdentifier, REFCOUNT_INDENTIFIER_SIZE, "SCCPCONF/%04d", conferenceID);
	conference = (conferencePtr) sccp_refcount_object_alloc(sizeof(sccp_conference_t), SCCP_REF_CONFERENCE, conferenceIdentifier, __sccp_conference_destroy);
	if (!conference) {
		pbx_log(LOG_ERROR, "SCCPCONF/%04d: conference not created: out of memory\n", conferenceID);
		return NULL;
	}
	memset(conference, 0, sizeof(sccp_conference_t));
	conference->id = conferenceID;
	conference->finishing = FALSE;
	conference->isLocked = FALSE;
	conference->isOnHold = FALSE;
	conference->linkedid = pbx_strdup(iPbx.getChannelLinkedId(channel));
	if (device->conf_mute_on_entry) {
		sccp_log((DEBUGCAT_CORE + DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_3 "SCCP: %s has conf_mute_on_entry on; participants of SCCPCONF/%04d join muted\n", DEV_ID_LOG(device), conferenceID);
		conference->mute_on_entry = device->conf_mute_on_entry;
	}
	conference->playback_announcements = device->conf_play_general_announce;
	sccp_copy_string(conference->playback.language, pbx_channel_language(channel->owner), sizeof(conference->playback.language));
	SCCP_RWLIST_HEAD_INIT(&conference->participants);

	bridgeCapabilities = AST_BRIDGE_CAPABILITY_MULTIMIX;
#ifdef CS_BRIDGE_CAPABILITY_MULTITHREADED
	bridgeCapabilities |= AST_BRIDGE_CAPABILITY_MULTITHREADED;
#endif
	conference->bridge = pbx_bridge_new(bridgeCapabilities, AST_BRIDGE_FLAG_DISSOLVE_EMPTY | AST_BRIDGE_FLAG_MASQUERADE_ONLY | AST_BRIDGE_FLAG_TRANSFER_PROHIBITED, channel->designator, conferenceIdentifier, NULL);

	if (!conference->bridge) {
		pbx_log(LOG_WARNING, "%s: conference not created: Asterisk could not create the bridge\n", channel->designator);
		sccp_conference_release(&conference);
		return NULL;
	}

#if defined(CS_SCCP_VIDEO)
	ast_bridge_set_talker_src_video_mode(conference->bridge);
#endif

	{
		sccp_conference_t *tmpConference = NULL;
		SCCP_LIST_LOCK(&conferences);
		if ((tmpConference = sccp_conference_retain(conference))) {
			SCCP_RWLIST_INSERT_HEAD(&conferences, tmpConference, list);
		}
		SCCP_LIST_UNLOCK(&conferences);
	}

	pbx_mutex_init(&conference->playback.lock);

	sccp_log((DEBUGCAT_CORE + DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_3 "SCCP: adding the moderator to SCCPCONF/%04d\n", conferenceID);

	AUTO_RELEASE(sccp_participant_t, participant , sccp_conference_createParticipant(conference));

	if (participant) {
#if CS_REFCOUNT_DEBUG
		sccp_refcount_addRelationship(device, participant);
		sccp_refcount_addRelationship(channel, participant);
#endif
		conference->num_moderators = 1;
		participant->channel = sccp_channel_retain(channel);
		participant->device = sccp_device_retain(device);
		participant->conferenceBridgePeer = channel->owner;
		sccp_conference_update_callInfo(channel, participant->conferenceBridgePeer, participant, conference->id);

		if (pbx_pthread_create_background(&participant->joinThread, NULL, sccp_conference_thread, participant) < 0) {
			channel->hangupRequest(channel);
			return NULL;
		}
		channel->hangupRequest = sccp_astgenwrap_requestHangup;					// moderator channel not running in an ast_pbx_start thread, but in a local thread => use hard hangup
		sccp_conference_addParticipant_toList(conference, participant);
		participant->channel->conference = sccp_conference_retain(conference);
		participant->channel->conference_id = conference->id;
		participant->channel->conference_participant_id = participant->id;
		participant->playback_announcements = device->conf_play_part_announce;

		sccp_conference_update_callInfo(channel, participant->conferenceBridgePeer, participant, conference->id);
		channel->calltype=SKINNY_CALLTYPE_INBOUND;
		iPbx.setChannelLinkedId(participant->channel, conference->linkedid);
		channel->calltype=SKINNY_CALLTYPE_OUTBOUND;
		participant->isModerator = TRUE;
		device->conferencelist_active = device->conf_show_conflist;
		pbx_builtin_setvar_int_helper(channel->owner, "__SCCP_CONFERENCE_ID", conference->id);
		pbx_builtin_setvar_int_helper(channel->owner, "__SCCP_CONFERENCE_PARTICIPANT_ID", participant->id);
		sccp_log((DEBUGCAT_CORE + DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_4 "SCCPCONF/%04d: moderator %d added (%s)\n", conference->id, participant->id, pbx_channel_name(participant->conferenceBridgePeer));
	}

	/* the caller keeps the returned conference retained (d->conference), which limits a phone to one conference */
#ifdef CS_MANAGER_EVENTS
	if (GLOB(callevents)) {
		manager_event(EVENT_FLAG_USER, "SCCPConfStart", "ConfId: %d\r\n" "SCCPDevice: %s\r\n", conferenceID, DEV_ID_LOG(device));
	}
#endif
	return conference;
}

static sccp_participant_t *sccp_conference_createParticipant(constConferencePtr conference)
{
	if (!conference) {
		pbx_log(LOG_ERROR, "SCCPCONF: participant requested without a conference (caller bug)\n");
		return NULL;
	}

	sccp_participant_t *participant = NULL;
	int participantID = SCCP_RWLIST_GETSIZE(&conference->participants) + 1;
	char participantIdentifier[REFCOUNT_INDENTIFIER_SIZE];

	sccp_log((DEBUGCAT_CORE + DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_3 "SCCPCONF/%04d: creating participant %d\n", conference->id, participantID);

	snprintf(participantIdentifier, REFCOUNT_INDENTIFIER_SIZE, "SCCPCONF/%04d/PART/%04d", conference->id, participantID);
	participant = (sccp_participant_t *) sccp_refcount_object_alloc(sizeof(sccp_participant_t), SCCP_REF_PARTICIPANT, participantIdentifier, __sccp_participant_destroy);
	if (!participant) {
		pbx_log(LOG_ERROR, "SCCPCONF/%04d: participant not added: out of memory\n", conference->id);
		return NULL;
	}
#if CS_REFCOUNT_DEBUG
	sccp_refcount_addRelationship(conference, participant);
#	endif
	pbx_bridge_features_init(&participant->features);

	participant->id = participantID;
	participant->conference = sccp_conference_retain(conference);
	participant->conferenceBridgePeer = NULL;
	participant->playback_announcements = conference->playback_announcements;
	participant->onMusicOnHold = FALSE;
	if (conference->mute_on_entry) {
		sccp_log((DEBUGCAT_CORE + DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_3 "SCCP: participant %d joins muted\n", participant->id);
		participant->features.mute = 1;
		participant->features.dtmf_passthrough = 1;
	}

	return participant;
}

static void sccp_conference_connect_bridge_channels_to_participants(constConferencePtr conference)
{
	struct ast_bridge *bridge = conference->bridge;
	struct ast_bridge_channel *bridge_channel = NULL;

#  ifndef CS_BRIDGE_BASE_NEW
	sccp_log((DEBUGCAT_HIGH + DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_4 "SCCPCONF/%04d: looking for the bridge channel (%d channels)\n", conference->id, conference->bridge->num);
#  else
	sccp_log((DEBUGCAT_HIGH + DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_4 "SCCPCONF/%04d: looking for the bridge channel (%d channels)\n", conference->id, conference->bridge->num_channels);
#  endif
	ao2_lock(bridge);
	AST_LIST_TRAVERSE(&bridge->channels, bridge_channel, entry) {
		sccp_log((DEBUGCAT_HIGH + DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_4 "SCCPCONF/%04d: bridge channel %p\n", conference->id, bridge_channel);
		AUTO_RELEASE(sccp_participant_t, participant , sccp_participant_findByPBXChannel(conference, bridge_channel->chan));

		if (participant && participant->bridge_channel != bridge_channel) {
			sccp_log((DEBUGCAT_CORE + DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_4 "SCCPCONF/%04d: connecting bridge channel %p to participant %d\n", conference->id, bridge_channel, participant->id);
			participant->bridge_channel = bridge_channel;
			if(participant->isModerator) {
				sccp_device_t * device = participant->device;
				sccp_channel_t * channel = participant->channel;
				if(device && channel) {
					sccp_indicate(device, channel, SCCP_CHANNELSTATE_CONNECTEDCONFERENCE);
					sccp_dev_set_keyset(device, sccp_device_find_index_for_line(device, channel->line->name), channel->callid, KEYMODE_CONNCONF);
				}
			}
		}
	}
	ao2_unlock(bridge);
}

static boolean_t sccp_conference_masqueradeChannel(PBX_CHANNEL_TYPE * participant_ast_channel, sccp_conference_t * conference, sccp_participant_t * participant)
{
	if (participant && participant_ast_channel) {
		if (!(iPbx.allocTempPBXChannel(participant_ast_channel, &participant->conferenceBridgePeer))) {
			pbx_log(LOG_ERROR, "SCCPCONF/%04d: participant %s not added: the helper channel could not be created\n", conference->id, pbx_channel_name(participant_ast_channel));
			pbx_hangup(participant->conferenceBridgePeer);
			pbx_channel_unref(participant_ast_channel);
			return FALSE;
		}
		if (!iPbx.masqueradeHelper(participant_ast_channel, participant->conferenceBridgePeer)) {
			pbx_log(LOG_ERROR, "SCCPCONF/%04d: participant %s not added: Asterisk could not move the call into the conference\n", conference->id, pbx_channel_name(participant_ast_channel));
			pbx_hangup(participant->conferenceBridgePeer);
			pbx_channel_unref(participant_ast_channel);
			return FALSE;
		}
		pbx_channel_ref(participant->conferenceBridgePeer);
		if (pbx_pthread_create_background(&participant->joinThread, NULL, sccp_conference_thread, participant) < 0) {
			pbx_hangup(participant->conferenceBridgePeer);
			pbx_channel_unref(participant->conferenceBridgePeer);
			return FALSE;
		}
		sccp_log((DEBUGCAT_CORE + DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_4 "SCCPCONF/%04d: participant %d added (%s)\n", conference->id, participant->id, pbx_channel_name(participant->conferenceBridgePeer));

		return TRUE;
	}
	return FALSE;
}

static void sccp_conference_addParticipant_toList(constConferencePtr conference, constParticipantPtr participant)
{
	sccp_participant_t *tmpParticipant = NULL;

	SCCP_RWLIST_WRLOCK(&(((conferencePtr)conference)->participants));
	if ((tmpParticipant = sccp_participant_retain(participant))) {
		SCCP_RWLIST_INSERT_TAIL(&(((conferencePtr)conference)->participants), tmpParticipant, list);
	}
	SCCP_RWLIST_UNLOCK(&(((conferencePtr)conference)->participants));
}

void sccp_conference_update_callInfo(constChannelPtr channel, PBX_CHANNEL_TYPE * pbxChannel, constParticipantPtr participant, uint32_t conferenceID)
{
	char moderator_cidname[StationMaxNameSize] = "";
	char moderator_cidnum[StationMaxNameSize] = "";
	sccp_callinfo_t *ci = sccp_channel_getCallInfo(channel);

	switch (channel->calltype) {
		case SKINNY_CALLTYPE_INBOUND:
			iCallInfo.Getter(ci,
				SCCP_CALLINFO_CALLEDPARTY_NAME, moderator_cidname,
				SCCP_CALLINFO_CALLEDPARTY_NUMBER, moderator_cidnum,
				SCCP_CALLINFO_CALLINGPARTY_NAME, &participant->PartyName,
				SCCP_CALLINFO_CALLINGPARTY_NUMBER, &participant->PartyNumber,
				SCCP_CALLINFO_KEY_SENTINEL);
			iCallInfo.Setter(ci,
				SCCP_CALLINFO_ORIG_CALLINGPARTY_NAME, participant->PartyName,
				SCCP_CALLINFO_ORIG_CALLINGPARTY_NUMBER, participant->PartyNumber,
				SCCP_CALLINFO_CALLINGPARTY_NAME, moderator_cidname,
				SCCP_CALLINFO_CALLINGPARTY_NUMBER, moderator_cidnum,
				SCCP_CALLINFO_KEY_SENTINEL);
			break;
		case SKINNY_CALLTYPE_OUTBOUND:
		case SKINNY_CALLTYPE_FORWARD:
			iCallInfo.Getter(ci,
				SCCP_CALLINFO_CALLINGPARTY_NAME, moderator_cidname,
				SCCP_CALLINFO_CALLINGPARTY_NUMBER, moderator_cidnum,
				SCCP_CALLINFO_CALLEDPARTY_NAME, &participant->PartyName,
				SCCP_CALLINFO_CALLEDPARTY_NUMBER, &participant->PartyNumber,
				SCCP_CALLINFO_KEY_SENTINEL);
			iCallInfo.Setter(ci,
				SCCP_CALLINFO_ORIG_CALLEDPARTY_NAME, participant->PartyName,
				SCCP_CALLINFO_ORIG_CALLEDPARTY_NUMBER, participant->PartyNumber,
				SCCP_CALLINFO_CALLEDPARTY_NAME, moderator_cidname,
				SCCP_CALLINFO_CALLEDPARTY_NUMBER, moderator_cidnum,
				SCCP_CALLINFO_KEY_SENTINEL);
			break;
		case SKINNY_CALLTYPE_SENTINEL:
			break;
	}

	/* this is just a workaround to update sip and other channels also -MC */
	struct ast_party_connected_line connected;
	struct ast_set_party_connected_line update_connected;

	memset(&update_connected, 0, sizeof(update_connected));
	ast_party_connected_line_init(&connected);

	update_connected.id.number = 1;
	connected.id.number.valid = 1;
	connected.id.number.str = moderator_cidnum;
	connected.id.number.presentation = AST_PRES_ALLOWED_NETWORK_NUMBER;

	update_connected.id.name = 1;
	connected.id.name.valid = 1;
	connected.id.name.str = moderator_cidname;
	connected.id.name.presentation = AST_PRES_ALLOWED_NETWORK_NUMBER;
	ast_set_party_id_all(&update_connected.priv);
	connected.source = AST_CONNECTED_LINE_UPDATE_SOURCE_TRANSFER;
	if (pbxChannel) {
		ast_channel_set_connected_line(pbxChannel, &connected, &update_connected);
	}
	iPbx.set_connected_line(channel, moderator_cidnum, moderator_cidname, AST_CONNECTED_LINE_UPDATE_SOURCE_TRANSFER);
}

void pbx_builtin_setvar_int_helper(PBX_CHANNEL_TYPE * channel, const char *var_name, int intvalue)
{
	char valuestr[8] = "";

	snprintf(valuestr, 8, "%d", intvalue);
	pbx_builtin_setvar_helper(channel, var_name, valuestr);
}

#if HAVE_PBX_CEL_H
#include <asterisk/cel.h>
#endif
boolean_t sccp_conference_addParticipatingChannel(conferencePtr conference, constChannelPtr conferenceSCCPChannel, constChannelPtr originalSCCPChannel, PBX_CHANNEL_TYPE * pbxChannel)
{
	boolean_t res = FALSE;
	pbx_assert(conference != NULL);
	if (!conference->isLocked) {
		AUTO_RELEASE(sccp_participant_t, participant , sccp_conference_createParticipant(conference));

		if (participant) {
			sccp_log((DEBUGCAT_CORE + DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_4 "SCCPCONF/%04d: adding participant %d (%s)\n", conference->id, participant->id, pbx_channel_name(pbxChannel));

			sccp_conference_update_callInfo(originalSCCPChannel, pbxChannel, participant, conference->id);	// Update CallerId on originalChannel before masquerade

			// if peer is sccp then retain peer_sccp_channel
			AUTO_RELEASE(sccp_channel_t, channel, get_sccp_channel_from_pbx_channel(pbxChannel));
			AUTO_RELEASE(sccp_device_t, device, channel ? sccp_channel_getDevice(channel) : NULL);
			if(device) {
				participant->playback_announcements = device->conf_play_part_announce;
				iPbx.setChannelLinkedId(channel, conference->linkedid);
#if CS_REFCOUNT_DEBUG
				sccp_refcount_addRelationship(device, participant);
				sccp_refcount_addRelationship(channel, participant);
#endif
			} else {
				participant->playback_announcements = conference->playback_announcements;
			}
			pbx_channel_ref(pbxChannel);
			if (sccp_conference_masqueradeChannel(pbxChannel, conference, participant)) {
				sccp_conference_addParticipant_toList(conference, participant);
				if (channel && device) {
					participant->channel = sccp_channel_retain(channel);
					participant->device = sccp_device_retain(device);
					participant->channel->conference = sccp_conference_retain(conference);
					participant->channel->conference_id = conference->id;
					participant->channel->conference_participant_id = participant->id;
					participant->playback_announcements = device->conf_play_part_announce;
				} else {
					iPbx.setPBXChannelLinkedId(participant->conferenceBridgePeer, conference->linkedid);
				}
				pbx_builtin_setvar_int_helper(participant->conferenceBridgePeer, "__SCCP_CONFERENCE_ID", conference->id);
				pbx_builtin_setvar_int_helper(participant->conferenceBridgePeer, "__SCCP_CONFERENCE_PARTICIPANT_ID", participant->id);
				pbx_indicate(participant->conferenceBridgePeer, AST_CONTROL_CONNECTED_LINE);
				res = TRUE;
			} else {
			}
		}
	} else {
		sccp_log((DEBUGCAT_CORE + DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_4 "SCCPCONF/%04d: participant refused: the conference is locked\n", conference->id);
		if (pbxChannel) {
			pbx_stream_and_wait(pbxChannel, "conf-locked", "");
		}
	}
	return res;
}

static void sccp_conference_removeParticipant(conferencePtr conference, participantPtr participant)
{
	int num_participants = 0;

	if (!conference || !participant) {
		return;
	}

	sccp_log((DEBUGCAT_CORE + DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_4 "SCCPCONF/%04d: removing participant %d\n", conference->id, participant->id);

	SCCP_RWLIST_RDLOCK(&(((conferencePtr)conference)->participants));
	AUTO_RELEASE(sccp_participant_t, tmp_participant, SCCP_RWLIST_REMOVE(&conference->participants, (sccp_participant_t *)participant, list));
	num_participants = SCCP_RWLIST_GETSIZE(&conference->participants);
	SCCP_RWLIST_UNLOCK(&(((conferencePtr)conference)->participants));

	if (!ATOMIC_FETCH(&conference->finishing, &conference->lock)) {
		if ((tmp_participant->isModerator && conference->num_moderators <= 1) || num_participants <= 1) {
			sccp_conference_end(conference);
		} else if (!tmp_participant->isModerator) {
			playback_to_conference(conference, "conf-hasleft", participant->id);
		}
	}
	sccp_conference_update_conflist(conference);
	sccp_log((DEBUGCAT_CORE + DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_4 "SCCPCONF/%04d: hanging up participant %d\n", conference->id, tmp_participant->id);
}

static void *sccp_conference_thread(void *data)
{
	AUTO_RELEASE(sccp_participant_t, participant , sccp_participant_retain(data));

	if (participant && participant->conference && participant->conference->bridge) {
		sccp_log_and((DEBUGCAT_CONFERENCE + DEBUGCAT_HIGH)) (VERBOSE_PREFIX_4 "SCCPCONF/%04d: join thread started\n", participant->conference->id);
#ifdef CS_MANAGER_EVENTS
		if (GLOB(callevents)) {
			manager_event(EVENT_FLAG_CALL, "SCCPConfEntered", "ConfId: %d\r\n" "PartId: %d\r\n" "Channel: %s\r\n" "Uniqueid: %s\r\n", participant->conference ? participant->conference->id : 0, participant->id, participant->conferenceBridgePeer ? pbx_channel_name(participant->conferenceBridgePeer) : "NULL", participant->conferenceBridgePeer ? pbx_channel_uniqueid(participant->conferenceBridgePeer) : "NULL");
		}
#endif
		sccp_log_and((DEBUGCAT_CONFERENCE + DEBUGCAT_HIGH)) (VERBOSE_PREFIX_4 "SCCPCONF/%04d: %s joins the bridge as participant %d\n", participant->conference->id, pbx_channel_name(participant->conferenceBridgePeer), participant->id);

		enum ast_bridge_join_flags flags = (enum ast_bridge_join_flags) 0;
		pbx_bridge_join(participant->conference->bridge, participant->conferenceBridgePeer, NULL, &participant->features, NULL, flags);
		participant->pendingRemoval = TRUE;

		sccp_log_and((DEBUGCAT_CONFERENCE + DEBUGCAT_HIGH)) (VERBOSE_PREFIX_4 "SCCPCONF/%04d: %s left the bridge as participant %d\n", participant->conference->id, pbx_channel_name(participant->conferenceBridgePeer), participant->id);
#ifdef CS_MANAGER_EVENTS
		if (GLOB(callevents)) {
			manager_event(EVENT_FLAG_CALL, "SCCPConfLeft", "ConfId: %d\r\n" "PartId: %d\r\n" "Channel: %s\r\n" "Uniqueid: %s\r\n", participant->conference ? participant->conference->id : 0, participant->id, participant->conferenceBridgePeer ? pbx_channel_name(participant->conferenceBridgePeer) : "NULL", participant->conferenceBridgePeer ? pbx_channel_uniqueid(participant->conferenceBridgePeer) : "NULL");
		}
#endif
		if (participant->channel && participant->device) {
			__sccp_conference_hide_list(participant);
		}

		if (participant->conferenceBridgePeer) {
			if (participant->final_announcement) {
				pbx_stream_and_wait(participant->conferenceBridgePeer, participant->final_announcement, "");
				sccp_free(participant->final_announcement);
			}
			if (pbx_test_flag(pbx_channel_flags(participant->conferenceBridgePeer), AST_FLAG_BLOCKING)) {
				ast_softhangup(participant->conferenceBridgePeer, AST_SOFTHANGUP_DEV);
			} else {
				pbx_hangup(participant->conferenceBridgePeer);
			}
			participant->conferenceBridgePeer = NULL;
		}
		sccp_conference_removeParticipant(participant->conference, participant);
		participant->joinThread = AST_PTHREADT_NULL;
	} else {
		pbx_log(LOG_WARNING, "SCCP: conference join thread not started: conference %d, participant %d or the bridge is missing\n", (participant && participant->conference) ? participant->conference->id : 0, participant ? participant->id : 0);
	}
	return NULL;
}

void sccp_conference_update(constConferencePtr conference)
{
	usleep(500); /* need time to settle into bridge, before updating links */
	sccp_conference_connect_bridge_channels_to_participants(conference);
}

void sccp_conference_start(conferencePtr conference)
{
	pbx_assert(conference != NULL);
	sccp_conference_update_conflist(conference);
	playback_to_conference(conference, "conf-placeintoconf", -1);
	sccp_conference_update(conference);
#ifdef CS_MANAGER_EVENTS
	if (GLOB(callevents)) {
		manager_event(EVENT_FLAG_CALL, "SCCPConfStarted", "ConfId: %d\r\n", conference->id);
	}
#endif
}

void sccp_conference_end(sccp_conference_t * conference)
{
	sccp_participant_t *participant = NULL;

	if (ATOMIC_INCR(&conference->finishing, TRUE, &conference->lock)) {
		return;
	}

	sccp_log((DEBUGCAT_CORE + DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_4 "SCCPCONF/%04d: ending conference\n", conference->id);

	SCCP_RWLIST_RDLOCK(&conference->participants);
	int num_participants = SCCP_RWLIST_GETSIZE(&conference->participants);
	if (num_participants > 2) {
		playback_to_conference(conference, "conf-leaderhasleft", -1);
	}
	if (num_participants > 0) {
		SCCP_RWLIST_TRAVERSE_SAFE_BEGIN(&conference->participants, participant, list) {
			if (!participant->isModerator && !participant->pendingRemoval) {
				if (pbx_bridge_remove(participant->conference->bridge, participant->conferenceBridgePeer)) {
					pbx_log(LOG_WARNING, "SCCPCONF/%04d: Asterisk could not remove %s from the conference bridge while ending the conference\n", conference->id, pbx_channel_name(participant->conferenceBridgePeer));
				}
			}
		}
		SCCP_RWLIST_TRAVERSE_SAFE_END;
		SCCP_RWLIST_TRAVERSE_SAFE_BEGIN(&conference->participants, participant, list) {
			if (participant->isModerator && !participant->pendingRemoval) {
				pbx_bridge_remove(participant->conference->bridge, participant->conferenceBridgePeer);
			}
		}
		SCCP_RWLIST_TRAVERSE_SAFE_END;
	}
	SCCP_RWLIST_UNLOCK(&conference->participants);

	sccp_conference_t *tmp_conference = NULL;
	int conference_id = conference->id;

	SCCP_LIST_LOCK(&conferences);
	tmp_conference = SCCP_RWLIST_REMOVE(&conferences, conference, list);
	sccp_conference_release(&tmp_conference);
	SCCP_LIST_UNLOCK(&conferences);
	sccp_log((DEBUGCAT_CORE + DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_3 "SCCPCONF/%04d: conference ended\n", conference_id);
}

void sccp_conference_hold(conferencePtr conference)
{
	sccp_participant_t *participant = NULL;

	if (!conference || conference->isOnHold) {
		return;
	}
	sccp_log((DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_3 "SCCPCONF/%04d: putting the conference on hold\n", conference->id);

	if (conference->num_moderators >= 1) {
		conference->isOnHold = TRUE;
		SCCP_RWLIST_RDLOCK(&(((conferencePtr)conference)->participants));
		SCCP_RWLIST_TRAVERSE(&conference->participants, participant, list) {
			if (participant->isModerator == FALSE) {
				sccp_conference_play_music_on_hold_to_participant(conference, participant, TRUE);
			} else {
				participant->device->conferencelist_active = FALSE;
			}
		}
		SCCP_RWLIST_UNLOCK(&(((conferencePtr)conference)->participants));
	}
}

void sccp_conference_resume(conferencePtr conference)
{
	sccp_participant_t *participant = NULL;

	sccp_log((DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_3 "SCCPCONF/%04d: resuming the conference\n", conference->id);
	if (!conference) {
		return;
	}

	if (conference->isOnHold) {
		SCCP_RWLIST_RDLOCK(&(((conferencePtr)conference)->participants));
		SCCP_RWLIST_TRAVERSE(&conference->participants, participant, list) {
			if (participant->isModerator == FALSE) {
				sccp_conference_play_music_on_hold_to_participant(conference, participant, FALSE);
			}
		}
		SCCP_RWLIST_UNLOCK(&(((conferencePtr)conference)->participants));
		conference->isOnHold = FALSE;
		sccp_conference_update_conflist(conference);
	}
}

static int stream_and_wait(PBX_CHANNEL_TYPE * playback_channel, const char *filename, int say_number)
{
	if (!sccp_strlen_zero(filename) && !pbx_fileexists(filename, NULL, NULL)) {
		pbx_log(LOG_WARNING, "SCCPCONF: announcement '%s' not played: no sound file with that name exists in any format\n", filename);
		return 0;
	}
	if (playback_channel) {
		if (!sccp_strlen_zero(filename)) {
			sccp_log_and((DEBUGCAT_CONFERENCE + DEBUGCAT_HIGH)) (VERBOSE_PREFIX_4 "playing '%s' to the conference\n", filename);
			pbx_stream_and_wait(playback_channel, filename, "");
		} else if (say_number >= 0) {
			sccp_log_and((DEBUGCAT_CONFERENCE + DEBUGCAT_HIGH)) (VERBOSE_PREFIX_4 "saying %d to the conference\n", say_number);
			pbx_say_number(playback_channel, say_number, "", pbx_channel_language(playback_channel), NULL);
		}
	}
	return 1;
}

/* This function is used to playback either a file or number sequence to a specific participant only */
int playback_to_channel(participantPtr participant, const char *filename, int say_number)
{
	int res = 0;

	if (!participant->playback_announcements) {
		sccp_log((DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_4 "SCCPCONF/%04d: announcement for participant %d suppressed\n", participant->conference->id, participant->id);
		return 1;
	}
	if (participant->bridge_channel) {
		sccp_log((DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_4 "SCCPCONF/%04d: playing %s %d to participant %d\n", participant->conference->id, filename, say_number, participant->id);
		pbx_bridge_lock(participant->conference->bridge);
		res = pbx_bridge_suspend(participant->conference->bridge, participant->conferenceBridgePeer);
		pbx_bridge_unlock(participant->conference->bridge);
		if (!res) {
			if (stream_and_wait(participant->bridge_channel->chan, filename, say_number)) {
				res = 1;
			} else {
				pbx_log(LOG_WARNING, "SCCPCONF/%04d: announcement %s%s not played to a participant\n", participant->conference->id, filename ? filename : "", say_number >= 0 ? " (number)" : "");
			}
			pbx_bridge_lock(participant->conference->bridge);
			pbx_bridge_unsuspend(participant->conference->bridge, participant->conferenceBridgePeer);
			pbx_bridge_unlock(participant->conference->bridge);
		}
	} else {
		sccp_log((DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_4 "SCCPCONF/%04d: announcement not played: no bridge channel\n", participant->conference->id);
	}
	return res;
}

int playback_to_conference(conferencePtr conference, const char *filename, int say_number)
{
	pbx_assert(conference != NULL);
	if (!conference->playback_announcements) {
		sccp_log((DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_4 "SCCPCONF/%04d: conference announcement suppressed\n", conference->id);
		return 1;
	}

	pbx_mutex_lock(&conference->playback.lock);

	if (filename && !sccp_strlen_zero(filename) && !pbx_fileexists(filename, NULL, NULL)) {
		pbx_log(LOG_WARNING, "SCCPCONF/%04d: announcement '%s' not played: no sound file with that name exists in any format\n", conference->id, filename);
		pbx_mutex_unlock(&conference->playback.lock);
		return 1;
	}

	if (!(conference->playback.channel)) {
		char data[14];

		snprintf(data, sizeof(data), "SCCPCONF/%04d", conference->id);
		if (!(conference->playback.channel = iPbx.requestAnnouncementChannel(AST_FORMAT_ALAW, NULL, data))) {
			pbx_mutex_unlock(&conference->playback.lock);
			return 1;
		}
		if (!sccp_strlen_zero(conference->playback.language)) {
			iPbx.set_language(conference->playback.channel, conference->playback.language);
		}
	}
	sccp_log_and((DEBUGCAT_CONFERENCE + DEBUGCAT_HIGH)) (VERBOSE_PREFIX_4 "SCCPCONF/%04d: attaching the announcer\n", conference->id);
	if (sccpconf_announce_channel_push(conference->playback.channel, conference->bridge)) {
		pbx_mutex_unlock(&conference->playback.lock);
		return 1;
	}

	if (say_number >= 0) {
		pbx_say_number(conference->playback.channel, say_number, 0, conference->playback.language, "n");
	}
	if (filename && !sccp_strlen_zero(filename)) {
		pbx_stream_and_wait(conference->playback.channel, filename, "");
	}

	sccp_log_and((DEBUGCAT_CONFERENCE + DEBUGCAT_HIGH)) (VERBOSE_PREFIX_4 "SCCPCONF/%04d: detaching the announcer\n", conference->id);
	sccpconf_announce_channel_depart(conference->playback.channel);

	pbx_mutex_unlock(&conference->playback.lock);

	return 0;
}

sccp_conference_t *sccp_conference_findByID(uint32_t identifier)
{
	sccp_conference_t *conference = NULL;

	if (identifier == 0) {
		return NULL;
	}
	SCCP_LIST_LOCK(&conferences);
	SCCP_LIST_TRAVERSE(&conferences, conference, list) {
		if (conference->id == identifier) {
			conference = sccp_conference_retain(conference);
			break;
		}
	}
	SCCP_LIST_UNLOCK(&conferences);
	return conference;
}

sccp_participant_t *sccp_participant_findByID(constConferencePtr conference, uint32_t identifier)
{
	sccp_participant_t *participant = NULL;

	if (!conference || identifier == 0) {
		return NULL;
	}
	SCCP_RWLIST_RDLOCK(&(((conferencePtr)conference)->participants));
	SCCP_RWLIST_TRAVERSE(&conference->participants, participant, list) {
		if (participant->id == identifier) {
			participant = sccp_participant_retain(participant);
			break;
		}
	}
	SCCP_RWLIST_UNLOCK(&(((conferencePtr)conference)->participants));
	return participant;
}

sccp_participant_t *sccp_participant_findByChannel(constConferencePtr conference, constChannelPtr channel)
{
	sccp_participant_t *participant = NULL;

	if (!conference || !channel) {
		return NULL;
	}
	SCCP_RWLIST_RDLOCK(&(((conferencePtr)conference)->participants));
	SCCP_RWLIST_TRAVERSE(&conference->participants, participant, list) {
		if (participant->channel == channel) {
			participant = sccp_participant_retain(participant);
			break;
		}
	}
	SCCP_RWLIST_UNLOCK(&(((conferencePtr)conference)->participants));
	return participant;
}

sccp_participant_t *sccp_participant_findByDevice(constConferencePtr conference, constDevicePtr device)
{
	sccp_participant_t *participant = NULL;

	if (!conference || !device) {
		return NULL;
	}
	SCCP_RWLIST_RDLOCK(&(((conferencePtr)conference)->participants));
	SCCP_RWLIST_TRAVERSE(&conference->participants, participant, list) {
		if (participant->device == device) {
			participant = sccp_participant_retain(participant);
			break;
		}
	}
	SCCP_RWLIST_UNLOCK(&(((conferencePtr)conference)->participants));
	return participant;
}

sccp_participant_t *sccp_participant_findByPBXChannel(constConferencePtr conference, PBX_CHANNEL_TYPE * channel)
{
	sccp_participant_t *participant = NULL;

	if (!conference || !channel) {
		return NULL;
	}
	SCCP_RWLIST_RDLOCK(&(((conferencePtr)conference)->participants));
	SCCP_RWLIST_TRAVERSE(&conference->participants, participant, list) {
		if (participant->conferenceBridgePeer == channel) {
			participant = sccp_participant_retain(participant);
			break;
		}
	}

	SCCP_RWLIST_UNLOCK(&(((conferencePtr)conference)->participants));
	return participant;
}

/*
 * Cisco URL reference UserData:INTEGER:STRING UserDataSoftKey:STRING:INTEGER:STRING UserCallDataSoftKey:STRING:INTEGER0:INTEGER1:INTEGER2:INTEGER3:STRING UserCallData:INTEGER0:INTEGER1:INTEGER2:INTEGER3:STRING
 */
void sccp_conference_show_list(constConferencePtr conference, constChannelPtr channel)
{
	int use_icon = 0;

	if (!conference) {
		pbx_log(LOG_WARNING, "SCCPCONF: conference list requested without a conference (caller bug)\n");
		return;
	}

	if (!channel) {												// only send this list to sccp phones
		pbx_log(LOG_WARNING, "SCCPCONF/%04d: conference list not shown: the call is missing\n", conference->id);
		return;
	}

	AUTO_RELEASE(sccp_participant_t, participant , sccp_participant_findByChannel(conference, channel));

	if (!participant) {
		pbx_log(LOG_NOTICE, "SCCPCONF/%04d: conference list not shown: %s is not a participant\n", conference->id, pbx_channel_name(channel->owner));
		return;
	}
	if (SCCP_RWLIST_GETSIZE(&conference->participants) < 1) {
		sccp_log((DEBUGCAT_CONFERENCE))(VERBOSE_PREFIX_3 "SCCPCONF/%04d: conference list not shown: the conference has no participants\n", conference->id);
		return;
	}
	if (participant->device) {
		participant->device->conferencelist_active = TRUE;
		if (!participant->callReference) {
			participant->callReference = channel->callid;
			participant->lineInstance = conference->id;
			participant->transactionID = sccp_random() % 1000;
		}

		pbx_str_t *xmlStr = pbx_str_alloca(2048);

		if (participant->device->protocolversion >= 15) {
			if (participant->device->hasEnhancedIconMenuSupport()) {
				pbx_str_append(&xmlStr, 0, "<CiscoIPPhoneIconFileMenu appId=\"%d\" onAppClosed=\"%d\">", appID, appID);
				if (conference->isLocked) {
					pbx_str_append(&xmlStr, 0, "<Title IconIndex=\"5\">Conference %d</Title>\n", conference->id);
				} else {
					pbx_str_append(&xmlStr, 0, "<Title IconIndex=\"4\">Conference %d</Title>\n", conference->id);
				}
			} else {
				pbx_str_append(&xmlStr, 0, "<CiscoIPPhoneIconFileMenu>");
				pbx_str_append(&xmlStr, 0, "<Title>Conference %d</Title>\n", conference->id);
			}
		} else {
			pbx_str_append(&xmlStr, 0, "<CiscoIPPhoneIconMenu>");
			pbx_str_append(&xmlStr, 0, "<Title>Conference %d</Title>\n", conference->id);
		}
		pbx_str_append(&xmlStr, 0, "<Prompt>Make Your Selection</Prompt>\n");

		sccp_participant_t *part = NULL;

		SCCP_RWLIST_RDLOCK(&(((conferencePtr)conference)->participants));
		SCCP_RWLIST_TRAVERSE(&conference->participants, part, list) {
			if (part->pendingRemoval) {
				continue;
			}
			pbx_str_append(&xmlStr, 0, "<MenuItem>");

			if (part->isModerator) {
				use_icon = 0;
			} else {
				use_icon = 2;
			}
			if (part->features.mute) {
				++use_icon;
			}
			pbx_str_append(&xmlStr, 0, "<IconIndex>");
			pbx_str_append(&xmlStr, 0, "%d", use_icon);
			pbx_str_append(&xmlStr, 0, "</IconIndex>");

			pbx_str_append(&xmlStr, 0, "<Name>");
			pbx_str_append(&xmlStr, 0, "%d:%s", part->id, part->PartyName);
			if (!sccp_strlen_zero(part->PartyNumber)) {
				pbx_str_append(&xmlStr, 0, " (%s)", part->PartyNumber);
			}
			pbx_str_append(&xmlStr, 0, "</Name>");
			pbx_str_append(&xmlStr, 0, "<URL>UserCallData:%d:%d:%d:%d:%d</URL>", appID, participant->lineInstance, participant->callReference, participant->transactionID, part->id);
			pbx_str_append(&xmlStr, 0, "</MenuItem>\n");
		}
		SCCP_RWLIST_UNLOCK(&(((conferencePtr)conference)->participants));

		if (participant->isModerator) {
			pbx_str_append(&xmlStr, 0, "<SoftKeyItem>");
			pbx_str_append(&xmlStr, 0, "<Name>EndConf</Name>");
			pbx_str_append(&xmlStr, 0, "<Position>1</Position>");
			pbx_str_append(&xmlStr, 0, "<URL>UserDataSoftKey:Select:%d:ENDCONF/%d</URL>", appID, participant->transactionID);
			pbx_str_append(&xmlStr, 0, "</SoftKeyItem>\n");
			pbx_str_append(&xmlStr, 0, "<SoftKeyItem>");
			pbx_str_append(&xmlStr, 0, "<Name>Mute</Name>");
			pbx_str_append(&xmlStr, 0, "<Position>2</Position>");
			pbx_str_append(&xmlStr, 0, "<URL>UserDataSoftKey:Select:%d:MUTE/%d</URL>", appID, participant->transactionID);
			pbx_str_append(&xmlStr, 0, "</SoftKeyItem>\n");

			pbx_str_append(&xmlStr, 0, "<SoftKeyItem>");
			pbx_str_append(&xmlStr, 0, "<Name>Kick</Name>");
			pbx_str_append(&xmlStr, 0, "<Position>3</Position>");
			pbx_str_append(&xmlStr, 0, "<URL>UserDataSoftKey:Select:%d:KICK/%d</URL>", appID, participant->transactionID);
			pbx_str_append(&xmlStr, 0, "</SoftKeyItem>\n");
		}
		pbx_str_append(&xmlStr, 0, "<SoftKeyItem>");
		pbx_str_append(&xmlStr, 0, "<Name>Exit</Name>");
		pbx_str_append(&xmlStr, 0, "<Position>4</Position>");
		pbx_str_append(&xmlStr, 0, "<URL>SoftKey:Exit</URL>");
		pbx_str_append(&xmlStr, 0, "</SoftKeyItem>\n");
		if (participant->isModerator) {
			pbx_str_append(&xmlStr, 0, "<SoftKeyItem>");
			pbx_str_append(&xmlStr, 0, "<Name>Moderate</Name>");
			pbx_str_append(&xmlStr, 0, "<Position>5</Position>");
			pbx_str_append(&xmlStr, 0, "<URL>UserDataSoftKey:Select:%d:MODERATE/%d</URL>", appID, participant->transactionID);
			pbx_str_append(&xmlStr, 0, "</SoftKeyItem>\n");
		}
		if (participant->device->protocolversion >= 15) {
			if (participant->device->hasEnhancedIconMenuSupport()) {
				pbx_str_append(&xmlStr, 0, "<IconItem><Index>0</Index><URL>Resource:Icon.Connected</URL></IconItem>");
				pbx_str_append(&xmlStr, 0, "<IconItem><Index>1</Index><URL>Resource:AnimatedIcon.Hold</URL></IconItem>");
				pbx_str_append(&xmlStr, 0, "<IconItem><Index>2</Index><URL>Resource:AnimatedIcon.StreamRxTx</URL></IconItem>");
				pbx_str_append(&xmlStr, 0, "<IconItem><Index>3</Index><URL>Resource:AnimatedIcon.Hold</URL></IconItem>");
				pbx_str_append(&xmlStr, 0, "<IconItem><Index>4</Index><URL>Resource:Icon.Speaker</URL></IconItem>");
				pbx_str_append(&xmlStr, 0, "<IconItem><Index>5</Index><URL>Resource:Icon.SecureCall</URL></IconItem>\n");
			} else {
				pbx_str_append(&xmlStr, 0, "<IconItem><Index>0</Index><URL>TFTP:Icon.Connected.png</URL></IconItem>");
				pbx_str_append(&xmlStr, 0, "<IconItem><Index>1</Index><URL>TFTP:AnimatedIcon.Hold.png</URL></IconItem>");
				pbx_str_append(&xmlStr, 0, "<IconItem><Index>2</Index><URL>TFTP:AnimatedIcon.StreamRxTx.png</URL></IconItem>");
				pbx_str_append(&xmlStr, 0, "<IconItem><Index>3</Index><URL>TFTP:AnimatedIcon.Hold.png</URL></IconItem>");
				pbx_str_append(&xmlStr, 0, "<IconItem><Index>4</Index><URL>TFTP:Icon.Speaker.png</URL></IconItem>");
				pbx_str_append(&xmlStr, 0, "<IconItem><Index>5</Index><URL>TFTP:Icon.SecureCall.png</URL></IconItem>\n");
			}
		} else {
			pbx_str_append(&xmlStr, 0, "<IconItem><Index>0</Index><Height>10</Height><Width>16</Width><Depth>2</Depth><Data>C3300000FF0F0000F3F30000F3FC0300F3FC0300FFF30000F30F0000FCF30300F0FC0F0000FF3F00</Data></IconItem>");
			pbx_str_append(&xmlStr, 0, "<IconItem><Index>1</Index><Height>10</Height><Width>16</Width><Depth>2</Depth><Data>C3300C00FF0F3C30F3F3F03CF3FCC333F3FC330FFFF3F03CF30FF0F3FCF333CFF0FC0F3C00FF3F30</Data></IconItem>");
			pbx_str_append(&xmlStr, 0, "<IconItem><Index>2</Index><Height>10</Height><Width>16</Width><Depth>2</Depth><Data>000000000000000000F30000C0FC0300C0FC030000F300000000000000F30300C0FC0F0030FF3F00</Data></IconItem>");
			pbx_str_append(&xmlStr, 0, "<IconItem><Index>3</Index><Height>10</Height><Width>16</Width><Depth>2</Depth><Data>00000C0000003C3000F3F03CC0FCC333C0FC330F00F3F03C0000F0F300F333CFC0FC0F3C30FF3F30</Data></IconItem>\n");
		}

		if (participant->device->protocolversion >= 15) {
			pbx_str_append(&xmlStr, 0, "</CiscoIPPhoneIconFileMenu>\n");
		} else {
			pbx_str_append(&xmlStr, 0, "</CiscoIPPhoneIconMenu>\n");
		}
		sccp_log((DEBUGCAT_CONFERENCE + DEBUGCAT_HIGH)) (VERBOSE_PREFIX_4 "SCCPCONF/%04d: showing the participant list: app %d, call %d, line instance %d, transaction %d\n", conference->id, appID, participant->callReference, participant->lineInstance, participant->transactionID);
		sccp_log((DEBUGCAT_CONFERENCE + DEBUGCAT_HIGH)) (VERBOSE_PREFIX_4 "SCCPCONF/%04d: participant list XML:\n%s\n", conference->id, pbx_str_buffer(xmlStr));

		participant->device->protocol->sendUserToDeviceDataVersionMessage(participant->device, appID, participant->callReference, participant->lineInstance, participant->transactionID, pbx_str_buffer(xmlStr), 2);
	}
}

void __sccp_conference_hide_list(participantPtr participant)
{
	if (participant->channel && participant->device && participant->conference) {
		if (participant->device->conferencelist_active) {
			sccp_log((DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_4 "SCCPCONF/%04d: hiding the participant list for participant %d\n", participant->conference->id, participant->id);
			char xmlData[512] = "";
			if (participant->device->protocolversion >= 15 ) {
				snprintf(xmlData, sizeof(xmlData), "<CiscoIPPhoneExecute><ExecuteItem Priority=\"0\" URL=\"App:Close:0\"/></CiscoIPPhoneExecute>");
			} else {
				snprintf(xmlData, sizeof(xmlData), "<CiscoIPPhoneExecute><ExecuteItem Priority=\"0\" URL=\"Init:Services\"/></CiscoIPPhoneExecute>");
			}

			participant->device->protocol->sendUserToDeviceDataVersionMessage(participant->device, appID, participant->callReference, participant->lineInstance, participant->transactionID, xmlData, 2);
			participant->device->conferencelist_active = FALSE;
		}
	}
}

void sccp_conference_hide_list_ByDevice(constDevicePtr device)
{
	sccp_conference_t *conference = NULL;

	SCCP_LIST_LOCK(&conferences);
	SCCP_LIST_TRAVERSE(&conferences, conference, list) {
		if (device) {
			AUTO_RELEASE(sccp_participant_t, participant , sccp_participant_findByDevice(conference, device));

			if (participant && participant->channel && participant->device) {
				__sccp_conference_hide_list(participant);
			}
		}
	}
	SCCP_LIST_UNLOCK(&conferences);
}

static void sccp_conference_update_conflist(conferencePtr conference)
{
	sccp_participant_t *participant = NULL;

	if (!conference || ATOMIC_FETCH(&(conference)->finishing, &conference->lock)) {
		return;
	}
	SCCP_RWLIST_RDLOCK(&(conference->participants));
	SCCP_RWLIST_TRAVERSE(&(conference->participants), participant, list) {
		if (participant->channel && participant->device && (participant->device->conferencelist_active || (participant->isModerator && !conference->isOnHold))) {
			sccp_conference_show_list(conference, participant->channel);
		}
	}
	SCCP_RWLIST_UNLOCK(&(conference->participants));
}

void sccp_conference_handle_device_to_user(devicePtr d, uint32_t callReference, uint32_t transactionID, uint32_t conferenceID, uint32_t participantID)
{
	if (d && d->dtu_softkey.transactionID == transactionID) {
		sccp_log((DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_4 "%s: conference list softkey on call %d, transaction %d, conference %d, participant %d, action %s\n", d->id, callReference, transactionID, conferenceID, participantID, d->dtu_softkey.action);

		AUTO_RELEASE(sccp_conference_t, conference , sccp_conference_findByID(conferenceID));

		if (!conference) {
			pbx_log(LOG_NOTICE, "%s: conference list action ignored: conference %d no longer exists\n", DEV_ID_LOG(d), conferenceID);
			goto EXIT;
		}
		AUTO_RELEASE(sccp_participant_t, participant , sccp_participant_findByID(conference, participantID));

		if (!participant) {
			pbx_log(LOG_NOTICE, "SCCPCONF/%04d: %s: conference list action ignored: participant %d is no longer in the conference\n", conference->id, DEV_ID_LOG(d), participantID);
			goto EXIT;
		}
		AUTO_RELEASE(sccp_participant_t, moderator , sccp_participant_findByDevice(conference, d));

		if (!moderator) {
			pbx_log(LOG_NOTICE, "SCCPCONF/%04d: %s: conference list action ignored: this device is not a moderator of the conference\n", conference->id, DEV_ID_LOG(d));
			goto EXIT;
		}
		sccp_log((DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_3 "SCCPCONF/%04d: list softkey action %s (%s)\n", conference->id, d->dtu_softkey.action, DEV_ID_LOG(d));
		if (!strcmp(d->dtu_softkey.action, "ENDCONF")) {
			sccp_conference_end(conference);
		} else if (!strcmp(d->dtu_softkey.action, "MUTE")) {
			sccp_conference_toggle_mute_participant(conference, participant);
		} else if (!strcmp(d->dtu_softkey.action, "KICK")) {
			if (participant->isModerator) {
				sccp_log((DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_3 "SCCPCONF/%04d: moderators cannot be kicked (%s)\n", conference->id, DEV_ID_LOG(d));
				sccp_dev_set_message(d, "cannot kick a moderator", 5, FALSE, FALSE);
			} else {
				sccp_participant_t *owned = sccp_participant_retain(participant);
				if (owned && !sccp_threadpool_add_work(GLOB(general_threadpool), sccp_participant_kicker, owned)) {
					sccp_participant_release(&owned);
				}
			}
		} else if (!strcmp(d->dtu_softkey.action, "EXIT")) {
			d->conferencelist_active = FALSE;
		} else if(strcmp(d->dtu_softkey.action, "MODERATE") == 0) {
			sccp_conference_promote_demote_participant(conference, participant, moderator);
		}
	} else {
		pbx_log(LOG_NOTICE, "%s: conference list action ignored: transaction %d does not match the list currently shown\n", DEV_ID_LOG(d), transactionID);
	}
EXIT:
	if (d) {
		if (d->dtu_softkey.action) {
			sccp_free(d->dtu_softkey.action);
		}
		d->dtu_softkey.transactionID = 0;
	}
}

void sccp_conference_kick_participant(constConferencePtr conference, participantPtr participant)
{
	sccp_log((DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_3 "SCCPCONF/%04d: kicking participant %d\n", conference->id, participant->id);
	participant->pendingRemoval = TRUE;

	pbx_bridge_lock(participant->conference->bridge);
	pbx_bridge_suspend(participant->conference->bridge, participant->conferenceBridgePeer);
	pbx_bridge_unlock(participant->conference->bridge);

	participant->final_announcement = pbx_strdup("conf-kicked");
	if (pbx_bridge_remove(participant->conference->bridge, participant->conferenceBridgePeer)) {
		pbx_log(LOG_WARNING, "SCCPCONF/%04d: Asterisk could not remove %s from the conference bridge\n", conference->id, pbx_channel_name(participant->conferenceBridgePeer));
		participant->pendingRemoval = FALSE;
		return;
	}
#ifdef CS_MANAGER_EVENTS
	if (GLOB(callevents)) {
		manager_event(EVENT_FLAG_CALL, "SCCPConfParticipantKicked", "ConfId: %d\r\n" "PartId: %d\r\n", conference->id, participant->id);
	}
#endif
}

void *sccp_participant_kicker(void *data)
{
	AUTO_RELEASE(sccp_participant_t, participant, (sccp_participant_t *)data);
	if (participant) {
		sccp_conference_kick_participant(participant->conference, participant);
	}
	return NULL;
}

void sccp_conference_toggle_mute_participant(constConferencePtr conference, participantPtr participant)
{
	sccp_log((DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_3 "SCCPCONF/%04d: muting participant %d\n", conference->id, participant->id);
	if (!participant->features.mute) {
		participant->features.mute = 1;
		participant->features.dtmf_passthrough = 1;
		playback_to_channel(participant, "conf-muted", -1);
	} else {
		participant->features.mute = 0;
		participant->features.dtmf_passthrough = 0;
		playback_to_channel(participant, "conf-unmuted", -1);
	}
	if (participant->channel && participant->device) {
		sccp_dev_set_message(participant->device, participant->features.mute ? "You are muted" : "You are unmuted", 5, FALSE, FALSE);
	}
#ifdef CS_MANAGER_EVENTS
	if (GLOB(callevents)) {
		manager_event(EVENT_FLAG_CALL, "SCCPConfParticipantMute", "ConfId: %d\r\n" "PartId: %d\r\n" "Mute: %s\r\n", conference->id, participant->id, participant->features.mute ? "Yes" : "No");
	}
#endif
	sccp_conference_update_conflist((conferencePtr)conference);
}

void sccp_conference_play_music_on_hold_to_participant(constConferencePtr conference, participantPtr participant, boolean_t start)
{
	if (!participant->channel || !participant->device) {
		return;
	}
	if (start) {
		sccp_log((DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_3 "SCCPCONF/%04d: starting music on hold for participant %d\n", conference->id, participant->id);
		if (participant->onMusicOnHold == FALSE) {
			if (!sccp_strlen_zero(participant->device->conf_music_on_hold_class)) {
				pbx_bridge_lock(participant->conference->bridge);
				int res = pbx_bridge_suspend(participant->conference->bridge, participant->conferenceBridgePeer);
				pbx_bridge_unlock(participant->conference->bridge);
				if (!res) {
					iPbx.moh_start(participant->conferenceBridgePeer, participant->device->conf_music_on_hold_class, NULL);
					participant->onMusicOnHold = TRUE;
					pbx_bridge_lock(((conferencePtr)conference)->bridge);
					pbx_bridge_unsuspend(((conferencePtr)conference)->bridge, participant->conferenceBridgePeer);
					pbx_bridge_unlock(((conferencePtr)conference)->bridge);
				}
			} else {
				sccp_conference_toggle_mute_participant(conference, participant);
			}
		}
	} else {
		sccp_log((DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_3 "SCCPCONF/%04d: stopping music on hold for participant %d\n", conference->id, participant->id);
		if (!sccp_strlen_zero(participant->device->conf_music_on_hold_class)) {
			pbx_bridge_lock(participant->conference->bridge);
			int res = pbx_bridge_suspend(participant->conference->bridge, participant->conferenceBridgePeer);
			pbx_bridge_unlock(participant->conference->bridge);
			if (!res) {
				iPbx.moh_stop(participant->conferenceBridgePeer);
				participant->onMusicOnHold = FALSE;
				pbx_bridge_lock(((conferencePtr)conference)->bridge);
				pbx_bridge_unsuspend(((conferencePtr)conference)->bridge, participant->conferenceBridgePeer);
				pbx_bridge_unlock(((conferencePtr)conference)->bridge);
			}
		} else {
			sccp_conference_toggle_mute_participant(conference, participant);
		}
	}
	if (!conference->isOnHold) {
		sccp_conference_update_conflist((conferencePtr)conference);
	}
}

void sccp_conference_promote_demote_participant(conferencePtr conference, participantPtr participant, constParticipantPtr moderator)
{
	if (participant->device && participant->channel) {
		if (!participant->isModerator) {
			participant->isModerator = TRUE;
			conference->num_moderators++;
			participant->device->conferencelist_active = TRUE;
			participant->device->conference = sccp_conference_retain(conference);
			sccp_softkey_setSoftkeyState(participant->device, KEYMODE_CONNCONF, SKINNY_LBL_JOIN, TRUE);
			sccp_softkey_setSoftkeyState(participant->device, KEYMODE_CONNTRANS, SKINNY_LBL_JOIN, TRUE);
			sccp_indicate(participant->device, participant->channel, SCCP_CHANNELSTATE_CONNECTEDCONFERENCE);
		} else {
			if (conference->num_moderators > 1) {
				participant->isModerator = FALSE;
				conference->num_moderators++;
				sccp_conference_release(&participant->device->conference);
				sccp_softkey_setSoftkeyState(participant->device, KEYMODE_CONNCONF, SKINNY_LBL_JOIN, FALSE);
				sccp_softkey_setSoftkeyState(participant->device, KEYMODE_CONNTRANS, SKINNY_LBL_JOIN, FALSE);
				sccp_indicate(participant->device, participant->channel, SCCP_CHANNELSTATE_CONNECTED);
			} else {
				sccp_log((DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_3 "SCCPCONF/%04d: not demoted: the conference needs at least one other moderator first\n", conference->id);
				if (moderator) {
					sccp_dev_set_message(moderator->device, "Promote someone first", 5, FALSE, FALSE);
				}
			}
		}
		sccp_dev_set_message(participant->device, participant->isModerator ? "You have been Promoted" : "You have been Demoted", 5, FALSE, FALSE);
#ifdef CS_MANAGER_EVENTS
		if (GLOB(callevents)) {
			manager_event(EVENT_FLAG_CALL, "SCCPConfParticipantPromotion", "ConfId: %d\r\n" "PartId: %d\r\n" "Moderator: %s\r\n", conference->id, participant->id, participant->isModerator ? "Yes" : "No");
		}
#endif
	} else {
		sccp_log((DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_3 "SCCPCONF/%04d: only SCCP calls can be moderators\n", conference->id);
		if (moderator) {
			sccp_dev_set_message(moderator->device, "Only sccp phones can be moderator", 5, FALSE, FALSE);
		}
	}
	sccp_conference_update_conflist(conference);
}

void sccp_conference_invite_participant(constConferencePtr conference, constParticipantPtr moderator)
{
	if (!conference) {
		pbx_log(LOG_WARNING, "SCCPCONF: invite requested without a conference (caller bug)\n");
		return;
	}
	if (!moderator) {
		pbx_log(LOG_WARNING, "SCCPCONF/%04d: invite requested without a moderator (caller bug)\n", conference->id);
		return;
	}
	if (conference->isLocked) {
		pbx_log(LOG_NOTICE, "SCCPCONF/%04d: invite refused: the conference is locked\n", conference->id);
		if (moderator->device) {
			sccp_dev_set_message(moderator->device, "Conference is locked", 5, FALSE, FALSE);
		}
		return;
	}

	if (moderator->channel && moderator->device) {
		pbx_str_t *xmlStr = pbx_str_alloca(2048);

		if (moderator->device->protocolversion >= 15) {
			pbx_str_append(&xmlStr, 0, "<CiscoIPPhoneInput appId=\"%d\">\n", appID);
		} else {
			pbx_str_append(&xmlStr, 0, "<CiscoIPPhoneInput>\n");
		}
		pbx_str_append(&xmlStr, 0, "<Title>Conference %d Invite</Title>\n", conference->id);
		pbx_str_append(&xmlStr, 0, "<Prompt>Enter the phone number to invite</Prompt>\n");
		pbx_str_append(&xmlStr, 0, "<URL>UserData:%d:%s</URL>\n", appID, "invite");

		pbx_str_append(&xmlStr, 0, "<InputItem>\n");
		pbx_str_append(&xmlStr, 0, "  <DisplayName>Phone Number</DisplayName>\n");
		pbx_str_append(&xmlStr, 0, "  <QueryStringParam>NUMBER</QueryStringParam>\n");
		pbx_str_append(&xmlStr, 0, "  <InputFlags>T</InputFlags>\n");
		pbx_str_append(&xmlStr, 0, "</InputItem>\n");

		pbx_str_append(&xmlStr, 0, "</CiscoIPPhoneInput>\n");

		sccp_log((DEBUGCAT_CONFERENCE + DEBUGCAT_HIGH)) (VERBOSE_PREFIX_4 "SCCPCONF/%04d: showing the participant list: app %d, call %d, line instance %d, transaction %d\n", conference->id, appID, moderator->callReference, moderator->lineInstance, moderator->transactionID);
		sccp_log((DEBUGCAT_CONFERENCE + DEBUGCAT_HIGH)) (VERBOSE_PREFIX_4 "SCCPCONF/%04d: participant list XML:\n%s\n", conference->id, pbx_str_buffer(xmlStr));

		moderator->device->protocol->sendUserToDeviceDataVersionMessage(moderator->device, APPID_CONFERENCE_INVITE, moderator->callReference, moderator->lineInstance, moderator->transactionID, pbx_str_buffer(xmlStr), 2);
	}
}

#include <asterisk/cli.h>
char *sccp_complete_conference(OLDCONST char *line, OLDCONST char *word, int pos, int state)
{
	int conference_id = 0;
	int wordlen = strlen(word);

	int which = 0;
	uint i = 0;
	char *ret = NULL;
	char tmpname[21];
	char *actions[5] = { "endconf", "kick", "mute", "invite", "moderate" };

	switch (pos) {
		case 2:
			{
				for (i = 0; i < ARRAY_LEN(actions); i++) {
					if (!strncasecmp(word, actions[i], wordlen) && ++which > state) {
						return pbx_strdup(actions[i]);
					}
				}
				break;
			}
		case 3:
			{
				sccp_conference_t *conference = NULL;

				SCCP_LIST_LOCK(&conferences);
				SCCP_LIST_TRAVERSE(&conferences, conference, list) {
					snprintf(tmpname, sizeof(tmpname), "%d", conference->id);
					if (!strncasecmp(word, tmpname, wordlen) && ++which > state) {
						ret = pbx_strdup(tmpname);
						break;
					}
				}
				SCCP_LIST_UNLOCK(&conferences);
				break;
			}
		case 4:
			{
				sccp_participant_t *participant = NULL;

				if (sscanf(line, "sccp conference %20s %d", tmpname, &conference_id) > 0) {
					AUTO_RELEASE(sccp_conference_t, conference , sccp_conference_findByID(conference_id));

					if (conference) {
						SCCP_RWLIST_RDLOCK(&(((conferencePtr)conference)->participants));
						SCCP_RWLIST_TRAVERSE(&conference->participants, participant, list) {
							snprintf(tmpname, sizeof(tmpname), "%d", participant->id);
							if (!strncasecmp(word, tmpname, wordlen) && ++which > state) {
								ret = pbx_strdup(tmpname);
								break;
							}
						}
						SCCP_RWLIST_UNLOCK(&(((conferencePtr)conference)->participants));
					}
				}
				break;
			}
		default:
			break;
	}
	return ret;
}

int sccp_cli_show_conferences(int fd, sccp_cli_totals_t *totals, struct mansession *s, const struct message *m, int argc, char *argv[])
{
	int local_line_total = 0;
	sccp_conference_t *conference = NULL;

#define CLI_AMI_TABLE_NAME Conferences
#define CLI_AMI_TABLE_PER_ENTRY_NAME Conference

#define CLI_AMI_TABLE_LIST_ITER_HEAD &conferences
#define CLI_AMI_TABLE_LIST_ITER_VAR conference
#define CLI_AMI_TABLE_LIST_LOCK SCCP_LIST_LOCK
#define CLI_AMI_TABLE_LIST_ITERATOR SCCP_LIST_TRAVERSE
#define CLI_AMI_TABLE_LIST_UNLOCK SCCP_LIST_UNLOCK

#define CLI_AMI_TABLE_FIELDS 																			\
		CLI_AMI_TABLE_FIELD_NAMED(Id, "ID",			"3.3",		d,	3,	conference->id)										\
		CLI_AMI_TABLE_FIELD(Participants,	"-12.12",	d,	12,	SCCP_RWLIST_GETSIZE(&conference->participants))						\
		CLI_AMI_TABLE_FIELD(Moderators,		"-12.12",	d,	12,	conference->num_moderators)								\
		CLI_AMI_TABLE_FIELD(Announce,		"-12.12",	s,	12,	conference->playback_announcements ? "Yes" : "No")					\
		CLI_AMI_TABLE_FIELD_NAMED(MuteOnEntry, "Mute on Entry",	"-12.12",	s,	12,	conference->mute_on_entry ? "Yes" : "No")						\

#include "sccp_cli_table.h"
	if (s) {
		totals->lines = local_line_total;
		totals->tables = 1;
	}
	return RESULT_SUCCESS;
}

int sccp_cli_show_conference(int fd, sccp_cli_totals_t *totals, struct mansession *s, const struct message *m, int argc, char *argv[])
{
	int local_line_total = 0;
	int confid = 0;

	if (argc < 4 || argc > 5 || sccp_strlen_zero(argv[3])) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "Usage: sccp show conference <conference id>\n%s", "");
	}
	if (!sccp_strIsNumeric(argv[3]) || (confid = sccp_atoi(argv[3], strlen(argv[3]))) <= 0) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "'%s' is not a conference id (a positive number)\n", argv[3]);
	}

	AUTO_RELEASE(sccp_conference_t, conference , sccp_conference_findByID(confid));

	if (conference) {
		sccp_participant_t *participant = NULL;

		if (!s) {
			CLI_AMI_OUTPUT(fd, s, "Conference %d:\n", conference->id);
		} else {
			astman_send_listack(s, m, argv[0], "start");
			CLI_AMI_OUTPUT_PARAM("Event", CLI_AMI_LIST_WIDTH, "%s", argv[0]);
			CLI_AMI_OUTPUT_PARAM("ConfId", CLI_AMI_LIST_WIDTH, "%d", conference->id);
		}
#define CLI_AMI_TABLE_NAME Participants
#define CLI_AMI_TABLE_PER_ENTRY_NAME Participant
#define CLI_AMI_TABLE_LIST_ITER_HEAD &conference->participants
#define CLI_AMI_TABLE_LIST_ITER_VAR participant
#define CLI_AMI_TABLE_LIST_LOCK SCCP_RWLIST_RDLOCK
#define CLI_AMI_TABLE_LIST_ITERATOR SCCP_RWLIST_TRAVERSE
#define CLI_AMI_TABLE_LIST_UNLOCK SCCP_RWLIST_UNLOCK
#define CLI_AMI_TABLE_FIELDS 																						\
			CLI_AMI_TABLE_FIELD_NAMED(Id, "ID",			"3.3",		d,	3,	participant->id)											\
			CLI_AMI_TABLE_FIELD_NAMED(ChannelName, "Channel",	"-20.20",	s,	20,	participant->conferenceBridgePeer ? pbx_channel_name(participant->conferenceBridgePeer) : "")	\
			CLI_AMI_TABLE_FIELD(Moderator,		"-11.11",	s,	11,	participant->isModerator ? "Yes" : "No")								\
			CLI_AMI_TABLE_FIELD(Muted,		"-5.5",		s,	5,	participant->features.mute ? "Yes" : "No")								\
			CLI_AMI_TABLE_FIELD(Announce,		"-8.8",		s,	8,	participant->playback_announcements ? "Yes" : "No")							\
			CLI_AMI_TABLE_FIELD_NAMED(ConfList, "Conference List",		"-8.8",		s,	8,	(participant->device && participant->device->conferencelist_active) ? "YES" : "NO")

#include "sccp_cli_table.h"
	} else {
		CLI_AMI_RETURN_ERROR(fd, s, m, "Conference %d does not exist\n", confid);
	}
	if (s) {
		totals->lines = local_line_total;
		totals->tables = 1;
	}
	return RESULT_SUCCESS;
}

int sccp_cli_conference_command(int fd, sccp_cli_totals_t *totals, struct mansession *s, const struct message *m, int argc, char *argv[])
{
	int confid = 0;

	int partid = 0;
	int local_line_total = 0;
	int res = RESULT_SUCCESS;
	char error[100];

	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_2 "conference command %s, conference %s, participant %s\n", argv[2], argv[3], argc >= 5 ? argv[4] : "");

	if (argc < 4 || argc > 5) {
		return RESULT_SHOWUSAGE;
	}

	if (sccp_strlen_zero(argv[2]) || sccp_strlen_zero(argv[3])) {
		return RESULT_SHOWUSAGE;
	}

	if (sccp_strIsNumeric(argv[3]) && (confid = sccp_atoi(argv[3], strlen(argv[3]))) > 0) {
		AUTO_RELEASE(sccp_conference_t, conference , sccp_conference_findByID(confid));

		if (conference) {
			if (!strncasecmp(argv[2], "EndConf", 7)) {
				sccp_conference_end(conference);
			} else if (argc >= 5) {
				if (sccp_strIsNumeric(argv[4]) && (partid = sccp_atoi(argv[4], strlen(argv[4]))) > 0) {
					AUTO_RELEASE(sccp_participant_t, participant , sccp_participant_findByID(conference, partid));

					if (participant) {
						if (!strncasecmp(argv[2], "Kick", 4)) {
							sccp_participant_t *owned = sccp_participant_retain(participant);
							if (owned && !sccp_threadpool_add_work(GLOB(general_threadpool), sccp_participant_kicker, owned)) {
								sccp_participant_release(&owned);
							}
						} else if (!strncasecmp(argv[2], "Mute", 4)) {
							sccp_conference_toggle_mute_participant(conference, participant);
						} else if (!strncasecmp(argv[2], "Invite", 5)) {
							sccp_conference_invite_participant(conference, participant);
						} else if (!strncasecmp(argv[2], "Moderate", 8)) {
							sccp_conference_promote_demote_participant(conference, participant, NULL);
						} else {
							snprintf(error, sizeof(error), "Unknown conference action '%s'\n", argv[2]);
							res = RESULT_FAILURE;
						}
					} else {
						snprintf(error, sizeof(error), "Participant %s is not in conference %s\n", argv[4], argv[3]);
						res = RESULT_FAILURE;
					}
				} else {
					snprintf(error, sizeof(error), "'%s' is not a participant id (a positive number)\n", argv[4]);
					res = RESULT_FAILURE;
				}
			} else {
				snprintf(error, sizeof(error), "Action %s needs a participant id\n", argv[2]);
				res = RESULT_FAILURE;
			}
			if (res == RESULT_SUCCESS) {
				sccp_conference_update_conflist(conference);
			}
		} else {
			snprintf(error, sizeof(error), "Conference %s does not exist\n", argv[3]);
			res = RESULT_FAILURE;
		}
	} else {
		snprintf(error, sizeof(error), "'%s' is not a conference id (a positive number)\n", argc > 3 ? argv[3] : "");
		res = RESULT_FAILURE;
	}

	if (res == RESULT_FAILURE && !sccp_strlen_zero(error)) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "%s\n", error);
	}
	if (s) {
		totals->lines = local_line_total;
	}
	return res;
}

#endif

