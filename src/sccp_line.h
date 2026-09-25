/*!
 * \file        sccp_line.h
 * \brief       SCCP Line Header
 * \author      Sergio Chersovani <mlists [at] c-net.it>
 * \note        Reworked, but based on chan_sccp code.
 *              The original chan_sccp driver that was made by Zozo which itself was derived from the chan_skinny driver.
 *              Modified by Jan Czmok and Julien Goodwin
 * \note        This program is free software and may be modified and distributed under the terms of the GNU General Public License, version 2 or (at your option) any later version.
 *              See the LICENSE file at the top of the source tree.
 *
 */
#pragma once

#define sccp_line_retain(_x)			sccp_refcount_retain_type(sccp_line_t, _x)
#define sccp_line_release(_x)			sccp_refcount_release_type(sccp_line_t, _x)
#define sccp_line_refreplace(_x, _y)		sccp_refcount_refreplace_type(sccp_line_t, _x, _y)
__BEGIN_C_EXTERN__
struct sccp_line {
	char id[SCCP_MAX_LINE_ID];
	char name[StationMaxNameSize];										/*!< The name of the line, so use in asterisk (i.e SCCP/[name]) */
	uint32_t configurationStatus;
#ifdef CS_SCCP_REALTIME
	boolean_t realtime;
	uint8_t _padding1[3];
#endif
	SCCP_RWLIST_ENTRY (sccp_line_t) list;
	struct {
		uint8_t numberOfActiveDevices;
		uint8_t numberOfActiveChannels;
		uint8_t numberOfHeldChannels;
		uint8_t numberOfDNDDevices;
	} statistic;

	uint8_t incominglimit;											/*!< max incoming calls limit */
	skinny_tone_t initial_dialtone_tone;
	skinny_tone_t secondary_dialtone_tone;
	char secondary_dialtone_digits[SCCP_MAX_SECONDARY_DIALTONE_DIGITS];

	char *trnsfvm;
	sccp_group_t callgroup;
#ifdef CS_SCCP_PICKUP
	sccp_group_t pickupgroup;

	boolean_t directed_pickup;										/*!< Directed Pickup Extension Support (Boolean, default=on) */
	char directed_pickup_context[SCCP_MAX_CONTEXT];
	boolean_t pickup_modeanswer;										/*!< Directed PickUp Mode Answer (boolean, default" on) */
#ifdef CS_AST_HAS_NAMEDGROUP
	char *namedcallgroup;
	char *namedpickupgroup;
#endif
#endif
	skinny_capabilities_t capabilities;									/*!< (shared)line level preferences (overrules device level) */
	skinny_capabilities_t preferences;									/*!< (shared)line level preferences (overrules device level) */
	boolean_t preferences_set_on_line_level;

	char cid_num[SCCP_MAX_EXTENSION];
	char cid_name[SCCP_MAX_EXTENSION];

	pbx_ama_flags_type amaflags;
	sccp_dndmode_t dndmode;

	SCCP_LIST_HEAD (, sccp_mailbox_t) mailboxes;
	SCCP_LIST_HEAD (, sccp_channel_t) channels;
	SCCP_LIST_HEAD(, sccp_linedevice_t) devices;                                                            /*!< The device this line is currently registered to. */

	PBX_VARIABLE_TYPE *variables;
	char pin[SCCP_MAX_LINE_PIN];
	char *adhocNumber;
	char *regexten;
	char *regcontext;
	char *description;											/*!< A description for the line, displayed on in header (on the 7960/40) or on main  screen on 7910 */
	char *label;												/*!< A name for the line, displayed next to the button (7960/40). */
	char *vmnum;
	char *meetmenum;
	char *meetmeopts;
	char *context;
	char *language;
	char *accountcode;
	char *musicclass;
	char *parkinglot;

	sccp_subscription_id_t defaultSubscriptionId;								/*!< default subscription id for shared lines */
	boolean_t echocancel;
	boolean_t silencesuppression;
	boolean_t meetme;
	boolean_t isShared;
	struct {
		int newmsgs;
		int oldmsgs;
	} voicemailStatistic;
	boolean_t transfer;

	sccp_video_mode_t videomode;
	boolean_t pendingDelete;										/*!< this bit will tell the scheduler to delete this line when unused */
	boolean_t pendingUpdate;										/*!< this bit will tell the scheduler to update this line when unused */
};

struct sccp_hotline {
	linePtr line;
	char exten[SCCP_MAX_EXTENSION];
};

SCCP_API void SCCP_CALL sccp_line_pre_reload(void);
SCCP_API void SCCP_CALL sccp_line_post_reload(void);
SCCP_API void * SCCP_CALL sccp_create_hotline(void);
SCCP_API linePtr SCCP_CALL sccp_line_create(const char * name);
SCCP_API void SCCP_CALL sccp_line_addToGlobals(constLinePtr line);
SCCP_API void SCCP_CALL sccp_line_removeFromGlobals(sccp_line_t * line);
SCCP_API void SCCP_CALL sccp_line_addChannel(constLinePtr line, constChannelPtr channel);
SCCP_API void SCCP_CALL sccp_line_removeChannel(constLinePtr line, sccp_channel_t * channel);
SCCP_API void SCCP_CALL sccp_line_clean(linePtr l, boolean_t remove_from_global);
SCCP_API void SCCP_CALL sccp_line_kill_channels(linePtr l);
SCCP_API void SCCP_CALL sccp_line_copyCodecSetsFromLineToChannel(constLinePtr l, constDevicePtr maybe_d, channelPtr c);
SCCP_API void SCCP_CALL sccp_line_updatePreferencesFromDevicesToLine(linePtr l);
SCCP_API void SCCP_CALL sccp_line_updateCapabilitiesFromDevicesToLine(linePtr l);
SCCP_API void SCCP_CALL sccp_line_updateLineCapabilitiesByDevice(constDevicePtr d);
SCCP_API void SCCP_CALL sccp_line_cfwd(constLinePtr line, constDevicePtr device, sccp_cfwd_t type, char * number);
SCCP_API void SCCP_CALL sccp_line_setMWI(constLinePtr l, int newlinemsgs, int oldlinemsgs);

SCCP_API linePtr SCCP_CALL sccp_line_find_byname(const char * name, uint8_t useRealtime);
#if DEBUG
#	define sccp_line_find_byid(_x, _y) __sccp_line_find_byid(_x, _y, __FILE__, __LINE__, __PRETTY_FUNCTION__)
SCCP_API linePtr SCCP_CALL __sccp_line_find_byid(constDevicePtr d, uint16_t instance, const char * filename, int lineno, const char * func);

#	define sccp_line_find_byButtonIndex(_x, _y) __sccp_line_find_byButtonIndex(_x, _y, __FILE__, __LINE__, __PRETTY_FUNCTION__)
SCCP_API linePtr SCCP_CALL __sccp_line_find_byButtonIndex(constDevicePtr d, uint16_t buttonIndex, const char * filename, int lineno, const char * func);
#	ifdef CS_SCCP_REALTIME
#		define sccp_line_find_realtime_byname(_x) __sccp_line_find_realtime_byname(_x, __FILE__, __LINE__, __PRETTY_FUNCTION__)
SCCP_API linePtr SCCP_CALL __sccp_line_find_realtime_byname(const char * name, const char * filename, int lineno, const char * func);
#	endif                                                                                                  // CS_SCCP_REALTIME
#else														// DEBUG
SCCP_API linePtr SCCP_CALL sccp_line_find_byid(constDevicePtr d, uint16_t instance);
SCCP_API linePtr SCCP_CALL sccp_line_find_byButtonIndex(constDevicePtr d, uint16_t buttonIndex);

#	ifdef CS_SCCP_REALTIME
SCCP_API linePtr SCCP_CALL sccp_line_find_realtime_byname(const char * name);
#	endif                                                                                                  // CS_SCCP_REALTIME
#endif														// DEBUG

__END_C_EXTERN__
