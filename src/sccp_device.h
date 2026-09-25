/*!
 * \file        sccp_device.h
 * \brief       SCCP Device Header
 * \author      Sergio Chersovani <mlists [at] c-net.it>
 * \note        Reworked, but based on chan_sccp code.
 *              The original chan_sccp driver that was made by Zozo which itself was derived from the chan_skinny driver.
 *              Modified by Jan Czmok and Julien Goodwin
 * \note        This program is free software and may be modified and distributed under the terms of the GNU General Public License, version 2 or (at your option) any later version.
 *              See the LICENSE file at the top of the source tree.
 *
 */
#pragma once

#define sccp_device_retain(_x)		sccp_refcount_retain_type(sccp_device_t, _x)
#define sccp_device_release(_x)		sccp_refcount_release_type(sccp_device_t, _x)
#define sccp_device_refreplace(_x, _y)	sccp_refcount_refreplace_type(sccp_device_t, _x, _y)

__BEGIN_C_EXTERN__

struct sccp_buttonconfig {
	uint8_t instance;
	uint8_t index;
	uint8_t _padding1[2];
	sccp_config_buttontype_t type;
	char *label;
	SCCP_LIST_ENTRY (sccp_buttonconfig_t) list;

	union sccp_button {
		struct {
			char *name;
			sccp_subscription_id_t *subscriptionId;
			char *options;
		} line;

		struct {
			char *ext;
			char *hint;
		} speeddial;

		struct {
			char *url;
		} service;

		struct {
			uint8_t index;										/*!< Button Feature Index */
			sccp_feature_type_t id;
			char *options;
			char *args;
			uint32_t status;
		} feature;
	} button;

	boolean_t pendingDelete;
	boolean_t pendingUpdate;
};

SCCP_LIST_HEAD (sccp_buttonconfig_list, sccp_buttonconfig_t);
struct sccp_speed {
	uint8_t instance;
	uint8_t config_instance;
	uint8_t type;
	boolean_t valid;
	char name[StationMaxNameSize];
	char ext[SCCP_MAX_EXTENSION];
	char hint[SCCP_MAX_EXTENSION];
	SCCP_LIST_ENTRY (sccp_speed_t) list;
};

enum sccp_privacyfeature {
	SCCP_PRIVACYFEATURE_OFF 	= 0,
	SCCP_PRIVACYFEATURE_HINT 	= 1 << 1,
	SCCP_PRIVACYFEATURE_CALLPRESENT	= 1 << 2,
};

#define SCCP_CALL_HISTORY_SIZE 20
typedef struct {
	time_t   ended;
	uint32_t callid;
	uint32_t packets_sent;
	uint32_t packets_received;
	uint32_t packets_lost;
	uint32_t jitter;
	uint32_t latency;
	float    mos_average;
	float    mos_minimum;
	uint32_t concealed_seconds;
	uint32_t severely_concealed_seconds;
} sccp_call_quality_t;

struct sccp_call_statistics {
	uint32_t num;
	uint32_t packets_sent;
	uint32_t packets_received;
	uint32_t packets_lost;
	uint32_t jitter;
	uint32_t latency;
	uint32_t discarded;
	float opinion_score_listening_quality;
	float avg_opinion_score_listening_quality;
	float mean_opinion_score_listening_quality;
	float max_opinion_score_listening_quality;
	float variance_opinion_score_listening_quality;
	float concealement_seconds;
	float cumulative_concealement_ratio;
	float interval_concealement_ratio;
	float max_concealement_ratio;
	uint32_t concealed_seconds;
	uint32_t severely_concealed_seconds;
};

struct sccp_hostname {
	char name[SCCP_MAX_HOSTNAME_LEN];
	SCCP_LIST_ENTRY (sccp_hostname_t) list;
};

struct sccp_device {
	char id[StationMaxDeviceNameSize];
	const sccp_deviceProtocol_t *protocol;									/*!< protocol the device uses */
	skinny_devicetype_t skinny_type;
	StationProtocolFeatures_t device_features;
	boolean_t earlyrtp;
	uint16_t keepalive;
	uint16_t keepaliveinterval;
	uint8_t protocolversion;										/*!< Skinny Supported Protocol Version */
	uint8_t inuseprotocolversion;										/*!< Skinny Used Protocol Version */
	uint16_t directrtp;											/*!< Direct RTP Support (Boolean, default=on) */

	sccp_nat_t nat;												/*!< Network Address Translation Support (Boolean, default=on) */
	sccp_session_t *session;
	SCCP_RWLIST_ENTRY (sccp_device_t) list;

	sccp_private_device_data_t *privateData;

	sccp_channel_t *active_channel;
	sccp_line_t *currentLine;

	struct {
		sccp_linedevice_t ** instance;
		uint8_t size;
	} lineButtons;
	sccp_buttonconfig_list_t buttonconfig;
	SCCP_LIST_HEAD (, sccp_selectedchannel_t) selectedChannels;
	SCCP_LIST_HEAD (, sccp_addon_t) addons;
	SCCP_LIST_HEAD (, sccp_hostname_t) permithosts;

	char *description;											/*!< Internal Description. Skinny protocol does not use it */
	char imageversion[StationMaxImageVersionSize];								/*!< Version to Send to the phone */
	char loadedimageversion[StationMaxImageVersionSize];							/*!< Loaded version on the phone */
	char config_type[SCCP_MAX_DEVICE_CONFIG_TYPE];
	int32_t tz_offset;											/*!< Timezone OffSet */
	uint8_t linesCount;
	uint8_t defaultLineInstance;
	uint8_t maxstreams;											/*!< Maximum number of Stream supported by the device */
	uint8_t _padding1;
	struct {
		char number[SCCP_MAX_EXTENSION];
		uint16_t lineInstance;
	} redialInformation;
	boolean_t linesRegistered;
	boolean_t meetme;
	boolean_t softkeysupport;										/*!< Soft Key Support (Boolean, default=on) */
	boolean_t realtime;
	boolean_t transfer;											/*!< Transfer Support (Boolean, default=on) */

	char *iconvcodepage;											/*!< Iconv Codepage to use during conversion from UTF-8, for old phone models */
	char *backgroundImage;											/*!< backgroundimage we will set after device registered */
	char *backgroundTN;											/*!< background thumbnail we will set after device registered */
	char *ringtone;												/*!< ringtone we will set after device registered */

	skinny_capabilities_t capabilities;
	skinny_capabilities_t preferences;

	time_t registrationTime;

	struct sccp_ha *ha;

	sccp_dtmfmode_t dtmfmode;
	boolean_t park;												/*!< Park Support (Boolean, default=on) */
	boolean_t cfwdall;											/*!< Call Forward All Support (Boolean, default=on) */
	boolean_t cfwdbusy;											/*!< Call Forward on Busy Support (Boolean, default=on) */
	boolean_t cfwdnoanswer;											/*!< Call Forward on No-Answer Support (Boolean, default=on) */
	char *meetmeopts;
	skinny_lampmode_t mwilamp;
	boolean_t mwioncall;											/*!< MWI On Call Support (Boolean, default=on) */
	boolean_t mwiUpdateRequired;

	struct {
		sccp_channel_t *transferee;
		sccp_channel_t *transferer;
	} transferChannels;

	pthread_t postregistration_thread;									/*!< Post Registration Thread */
	PBX_VARIABLE_TYPE *variables;

	sccp_dndmode_t dndmode;
	struct {
		uint8_t numberOfLines;
		uint8_t numberOfSpeeddials;
		uint8_t numberOfFeatures;
		uint8_t numberOfServices;
	} configurationStatistic;

	struct {
		uint16_t newmsgs;
		uint16_t oldmsgs;
	} voicemailStatistic;

	sccp_featureConfiguration_t privacyFeature;
	sccp_featureConfiguration_t overlapFeature;
	sccp_featureConfiguration_t monitorFeature;
	sccp_featureConfiguration_t dndFeature;
	sccp_featureConfiguration_t priFeature;
	sccp_featureConfiguration_t mobFeature;

	uint8_t audio_tos;
	uint8_t video_tos;
	uint8_t audio_cos;
	uint8_t video_cos;
	struct {
		softkey_modes *modes;
		uint32_t activeMask[SCCP_MAX_SOFTKEY_MASK];
		uint8_t size;
	} softKeyConfiguration;

	struct {
		sccp_tokenstate_t token;
	} status;
	boolean_t allowRinginNotification;									/*!< allow ringin notification for hinted extensions (Boolean, default=on) */
	boolean_t trustphoneip;											/*!< Trust Phone IP Support (Boolean, default=off) DEPRECATED */
	boolean_t needcheckringback;										/*!< Need to Check Ring Back Support (Boolean, default=on) */
	boolean_t isAnonymous;											/*!< Device is connected Anonymously (Guest) */

	btnlist *buttonTemplate;

	struct {
		char *action;
		uint32_t transactionID;
	} dtu_softkey;

	boolean_t (*checkACL) (constDevicePtr device);								/*!< check ACL callback function */
	sccp_push_result_t (*pushURL) (constDevicePtr device, const char *url, uint8_t priority, skinny_tone_t tone);
	sccp_push_result_t (*pushTextMessage) (constDevicePtr device, const char *messageText, const char *from, uint8_t priority, skinny_tone_t tone);
	boolean_t (*hasDisplayPrompt) (void);									/*!< has Display Prompt callback function (derived from devicetype and protocol) */
	boolean_t (*hasLabelLimitedDisplayPrompt) (void);							/*!< Can only display very limited selection of label based status bar messages */
	boolean_t (*useHookFlash) (void);
	boolean_t (*hasEnhancedIconMenuSupport) (void);								/*!< has Enhanced IconMenu Support (derived from devicetype and protocol) */
	boolean_t (*hasMWILight) (void);
	void (*retrieveDeviceCapabilities) (constDevicePtr device);
	void (*setBackgroundImage) (constDevicePtr device, const char *url, const char *tn);
	void (*displayBackgroundImagePreview) (constDevicePtr device, const char *url);
	void (*setRingTone) (constDevicePtr device, const char *url);						/*!< set the default Ringtone */
	const struct sccp_device_indication_cb *indicate;

	sccp_dtmfmode_t(*getDtmfMode) (constDevicePtr device);

	struct {
#ifndef SCCP_ATOMIC
		sccp_mutex_t lock;
#endif
		char *(messages[SCCP_MESSAGE_PRIORITY_SENTINEL]);
	} messageStack;

	sccp_call_statistics_t call_statistics[2];
	struct {
		sccp_call_quality_t entry[SCCP_CALL_HISTORY_SIZE];
		uint8_t next;
		uint8_t count;
	} call_history;
	char *softkeyDefinition;
	sccp_softKeySetConfiguration_t *softkeyset;								/*!< Allow for a copy of the softkeyset, if any of the softkeys needs to be redefined, for example for urihook/uriaction */

	void (*copyStr2Locale) (constDevicePtr d, char *dst, ICONV_CONST char *src, size_t dst_size);

#ifdef CS_SCCP_CONFERENCE
	sccp_conference_t *conference;
	char *conf_music_on_hold_class;
	uint32_t conference_id;
	boolean_t conferencelist_active;
	boolean_t allow_conference;
	boolean_t conf_play_general_announce;									/*!< Playback General Announcements (Entering/Leaving) */
	boolean_t conf_play_part_announce;									/*!< Playback Personal Announcements (You have been Kicked/You are muted) */

	boolean_t conf_mute_on_entry;
	boolean_t conf_show_conflist;
#endif
#ifdef CS_SCCP_PICKUP
	boolean_t directed_pickup;										/*!< Directed Pickup Extension Support (Boolean, default=on) */
	char directed_pickup_context[SCCP_MAX_CONTEXT];
	boolean_t pickup_modeanswer;										/*!< Directed Pickup Mode Answer (Boolean, default on). Answer on directed pickup */
#endif
	skinny_callHistoryDisposition_t callhistory_answered_elsewhere;
	boolean_t useRedialMenu;

	uint32_t  rtpPort;
#ifdef CS_AST_HAS_STASIS_ENDPOINT
	PBX_ENDPOINT_TYPE *endpoint;
#endif
	struct sockaddr_storage ipv4;
	struct sockaddr_storage ipv6;

	boolean_t pendingDelete;										/*!< this bit will tell the scheduler to delete this line when unused */
	boolean_t pendingUpdate;
};

struct sccp_addon {
	SCCP_LIST_ENTRY (sccp_addon_t) list;
	skinny_devicetype_t type;
};

struct sccp_device_indication_cb {
	void (*const onhook) (constDevicePtr device, const uint8_t lineInstance, uint32_t callid);
	void (*const offhook)(constDevicePtr device, sccp_linedevice_t * ld, uint32_t callid);
	void (*const dialing) (constDevicePtr device, const uint8_t lineInstance, const uint32_t callid, const skinny_calltype_t calltype, sccp_callinfo_t * const callinfo, char dialedNumber[SCCP_MAX_EXTENSION]);
	void (*const proceed) (constDevicePtr device, const uint8_t lineInstance, const uint32_t callid, const skinny_calltype_t calltype, sccp_callinfo_t * const callinfo);
	void (*const connected)(constDevicePtr device, const uint8_t lineInstance, const uint32_t callid, const skinny_calltype_t calltype, sccp_callinfo_t * const callinfo);
	void (*const callhistory)(constDevicePtr device, const uint8_t lineInstance, const uint32_t callid, const skinny_callHistoryDisposition_t disposition);
	void (*const remoteOnhook) (constDevicePtr device, const uint8_t lineInstance, const uint32_t callid);
	void (*const remoteOffhook) (constDevicePtr device, const uint8_t lineInstance, const uint32_t callid);
	void (*const remoteConnected) (constDevicePtr device, const uint8_t lineInstance, const uint32_t callid, skinny_callinfo_visibility_t visibility);
	void (*const remoteHold) (constDevicePtr device, uint8_t lineInstance, uint32_t callid, skinny_callpriority_t callpriority, skinny_callinfo_visibility_t visibility);
};

#define sccp_dev_displayprompt(p, q, r, s, t) sccp_dev_displayprompt_debug(p, q, r, s, t, __FILE__, __LINE__, __PRETTY_FUNCTION__)
#define sccp_dev_displaynotify(p,q,r) sccp_dev_displaynotify_debug(p,q,r, __FILE__, __LINE__, __PRETTY_FUNCTION__)
#define sccp_dev_displayprinotify(p,q,r,s) sccp_dev_displayprinotify_debug(p,q,r,s,__FILE__, __LINE__, __PRETTY_FUNCTION__)

SCCP_API void SCCP_CALL sccp_device_pre_reload(void);
SCCP_API void SCCP_CALL sccp_device_post_reload(void);

SCCP_API const SCCP_CALL sccp_accessorystate_t sccp_device_getAccessoryStatus(constDevicePtr d, const sccp_accessory_t accessory);
SCCP_API const SCCP_CALL sccp_accessory_t sccp_device_getActiveAccessory(constDevicePtr d);
SCCP_API int SCCP_CALL sccp_device_setAccessoryStatus(constDevicePtr d, const sccp_accessory_t accessory, const sccp_accessorystate_t state);
SCCP_API const SCCP_CALL sccp_devicestate_t sccp_device_getDeviceState(constDevicePtr d);
SCCP_API int SCCP_CALL sccp_device_setDeviceState(constDevicePtr d, const sccp_devicestate_t state);
SCCP_API const SCCP_CALL skinny_registrationstate_t sccp_device_getRegistrationState(constDevicePtr d);
SCCP_API int SCCP_CALL sccp_device_setRegistrationState(constDevicePtr d, const skinny_registrationstate_t state);

SCCP_API devicePtr SCCP_CALL sccp_device_create(const char * id);
SCCP_API devicePtr SCCP_CALL sccp_device_createAnonymous(const char * name);
SCCP_API void SCCP_CALL sccp_device_addToGlobals(constDevicePtr device);
SCCP_API linePtr SCCP_CALL sccp_dev_getActiveLine(constDevicePtr device);
#define sccp_dev_setActiveLine(d, l) __sccp_dev_setActiveLine(d, l, __FILE__, __LINE__, __PRETTY_FUNCTION__)
SCCP_API void SCCP_CALL __sccp_dev_setActiveLine(devicePtr device, constLinePtr l, const char *file, uint32_t line, const char *func);

SCCP_API channelPtr SCCP_CALL sccp_device_getActiveChannel(constDevicePtr device);
#define sccp_device_setActiveChannel(_d,_c) __sccp_device_setActiveChannel(_d, _c, __FILE__, __LINE__, __PRETTY_FUNCTION__)
SCCP_API void SCCP_CALL __sccp_device_setActiveChannel(constDevicePtr d, constChannelPtr channel, const char *file, uint32_t line, const char *func);

SCCP_API sccp_buttonconfig_t * SCCP_CALL sccp_dev_serviceURL_find_byindex(devicePtr device, uint16_t instance);
SCCP_API void SCCP_CALL sccp_dev_check_displayprompt(constDevicePtr d);
SCCP_API void SCCP_CALL sccp_device_setLastNumberDialed(devicePtr device, const char * lastNumberDialed, const sccp_linedevice_t * ld);
SCCP_API void SCCP_CALL sccp_device_preregistration(devicePtr device);
SCCP_API uint8_t SCCP_CALL sccp_dev_build_buttontemplate(devicePtr d, btnlist * btn);
SCCP_API void SCCP_CALL sccp_dev_sendmsg(constDevicePtr d, sccp_mid_t t);
SCCP_API void SCCP_CALL sccp_dev_set_keyset(constDevicePtr d, uint8_t lineInstance, uint32_t callid, skinny_keymode_t softKeySetIndex);
SCCP_API void SCCP_CALL sccp_dev_set_ringer(constDevicePtr d, skinny_ringtype_t ringtype, skinny_ringduration_t duration, uint8_t lineInstance, uint32_t callid);
SCCP_API void SCCP_CALL sccp_dev_cleardisplay(constDevicePtr d);
SCCP_API void SCCP_CALL sccp_dev_set_registered(devicePtr d, skinny_registrationstate_t state);
SCCP_API void SCCP_CALL sccp_dev_set_speaker(constDevicePtr d, uint8_t mode);
SCCP_API void SCCP_CALL sccp_dev_set_microphone(devicePtr d, uint8_t mode);
SCCP_API void SCCP_CALL sccp_dev_set_cplane(constDevicePtr device, uint8_t lineInstance, int status);
SCCP_API void SCCP_CALL sccp_dev_deactivate_cplane(constDevicePtr d);
SCCP_API void SCCP_CALL sccp_dev_starttone(constDevicePtr d, skinny_tone_t tone, uint8_t lineInstance, uint32_t callid, skinny_toneDirection_t direction);
SCCP_API void SCCP_CALL sccp_dev_stoptone(constDevicePtr d, uint8_t lineInstance, uint32_t callid);
SCCP_API void SCCP_CALL sccp_dev_clearprompt(constDevicePtr d, uint8_t lineInstance, uint32_t callid);
SCCP_API void SCCP_CALL sccp_dev_displayprompt_debug(constDevicePtr d, const uint8_t lineInstance, const uint32_t callid, const char *msg, int timeout, const char *file, const int lineno, const char *pretty_function);
SCCP_API void SCCP_CALL sccp_dev_displaynotify_debug(constDevicePtr d, const char *msg, const uint8_t timeout, const char *file, const int lineno, const char *pretty_function);
SCCP_API void SCCP_CALL sccp_dev_displayprinotify_debug(constDevicePtr d, const char *msg, const sccp_message_priority_t priority, const uint8_t timeout, const char *file, const int lineno, const char *pretty_function);
SCCP_API void SCCP_CALL sccp_dev_cleardisplaynotify(constDevicePtr d);
SCCP_API void SCCP_CALL sccp_dev_cleardisplayprinotify(constDevicePtr d, const uint8_t priority);
SCCP_API void SCCP_CALL sccp_dev_speed_find_byindex(constDevicePtr d, const uint16_t instance, boolean_t withHint, sccp_speed_t * const k);
SCCP_API void SCCP_CALL sccp_dev_forward_status(constLinePtr l, uint8_t lineInstance, constDevicePtr device);
SCCP_API void SCCP_CALL _sccp_dev_clean(devicePtr device, boolean_t remove_from_global, boolean_t restart_device);
#define sccp_dev_clean(d, r) _sccp_dev_clean(d, r, FALSE);
#define sccp_dev_clean_restart(d, r) _sccp_dev_clean(d, r, TRUE);
SCCP_API void SCCP_CALL sccp_dev_keypadbutton(devicePtr d, char digit, uint8_t line, uint32_t callid);
SCCP_API void SCCP_CALL sccp_dev_set_message(devicePtr d, const char *msg, const int timeout, const boolean_t storedb, const boolean_t beep);
SCCP_API void SCCP_CALL sccp_dev_clear_message(devicePtr d, const boolean_t cleardb);
SCCP_API void SCCP_CALL sccp_device_addMessageToStack(devicePtr device, const uint8_t priority, const char *message);
SCCP_API void SCCP_CALL sccp_device_clearMessageFromStack(devicePtr device, const uint8_t priority);
SCCP_API void SCCP_CALL sccp_device_featureChangedDisplay(const sccp_event_t * event);
SCCP_API void SCCP_CALL sccp_device_sendcallstate(constDevicePtr d, uint8_t instance, uint32_t callid, skinny_callstate_t state, skinny_callpriority_t precedence_level, skinny_callinfo_visibility_t visibility);
SCCP_API void SCCP_CALL sccp_device_sendCallHistoryDisposition(constDevicePtr d, uint8_t lineInstance, uint32_t callid, skinny_callHistoryDisposition_t disposition);
SCCP_API int SCCP_CALL sccp_dev_send(constDevicePtr d, sccp_msg_t * msg);
SCCP_API int SCCP_CALL sccp_device_sendReset(devicePtr d, skinny_resetType_t reset_type);
SCCP_API uint8_t SCCP_CALL sccp_device_find_index_for_line(constDevicePtr d, const char *lineName);
SCCP_API uint8_t SCCP_CALL sccp_device_numberOfChannels(constDevicePtr device);
SCCP_API boolean_t SCCP_CALL sccp_device_isVideoSupported(constDevicePtr device);
SCCP_API boolean_t SCCP_CALL sccp_device_check_update(devicePtr device);
SCCP_INLINE SCCP_CALL int16_t sccp_device_buttonIndex2lineInstance(constDevicePtr d, uint16_t buttonIndex);

SCCP_API devicePtr SCCP_CALL sccp_device_find_byid(const char * id, boolean_t useRealtime);
#ifdef CS_SCCP_REALTIME
#	if DEBUG
#		define sccp_device_find_realtime(_x) __sccp_device_find_realtime(_x, __FILE__, __LINE__, __PRETTY_FUNCTION__)
SCCP_API devicePtr SCCP_CALL __sccp_device_find_realtime(const char * name, const char * filename, int lineno, const char * func);
#	else
SCCP_API devicePtr SCCP_CALL sccp_device_find_realtime(const char * name);
#	endif
#	define sccp_device_find_realtime_byid(x) sccp_device_find_realtime(x)
#endif

SCCP_API void SCCP_CALL sccp_device_setLamp(constDevicePtr device, skinny_stimulus_t stimulus, uint8_t instance, skinny_lampmode_t mode);
SCCP_API void SCCP_CALL sccp_device_setMWI(devicePtr device);
SCCP_API void SCCP_CALL sccp_device_suppressMWI(devicePtr device);
SCCP_API void SCCP_CALL sccp_device_indicateMWI(devicePtr device);
__END_C_EXTERN__
