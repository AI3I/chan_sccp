/*!
 * \file        sccp_globals.h
 * \brief       SCCP Globals Header
 * \author      Diederik de Groot < ddegroot@users.sourceforge.net >
 * \note        This program is free software and may be modified and distributed under the terms of the GNU General Public License, version 2 or (at your option) any later version.
 *              See the LICENSE file at the top of the source tree.
 * \since       2016-02-02
 */
#pragma once
#include "config.h"
#include "define.h"
#include "sccp_codec.h"
#include "sccp_netsock.h"

__BEGIN_C_EXTERN__
SCCP_API char SCCP_VERSIONSTR[300];
SCCP_API char SCCP_REVISIONSTR[30];

struct sccp_servercontext;

struct subscriptionId {
	char number[SCCP_MAX_EXTENSION];									/*!< will be added to cid */
	char name[SCCP_MAX_EXTENSION];										/*!< will be added to cidName */
	char label[SCCP_MAX_LABEL];										/*!< will be added to cidName */
	char aux[SCCP_MAX_AUX];
	boolean_t replaceCid;											/*!< Should cidnumber be replaced instead of appended to, controlled by the '=' subscription flag */
};

struct sccp_global_vars {
	int keepalive;
	int32_t debug;
	int module_running;
	pbx_rwlock_t lock;

	sccp_threadpool_t *general_threadpool;

	SCCP_RWLIST_HEAD (, sccp_session_t) sessions;
	SCCP_RWLIST_HEAD (, sccp_device_t) devices;
	SCCP_RWLIST_HEAD (, sccp_line_t) lines;

	sccp_mutex_t socket_lock;
#ifndef SCCP_ATOMIC
	sccp_mutex_t usecnt_lock;
#endif
	int usecnt;
	long int amaflags;
	pthread_t mwiMonitorThread;

	char dateformat[SCCP_MAX_DATE_FORMAT];

	struct sccp_servercontext * srvcontexts[2];

	struct sccp_ha *ha;
	struct sockaddr_storage bindaddr;
	struct sockaddr_storage secbindaddr;
	char * cert_file;
	struct sccp_ha *localaddr;

	struct sockaddr_storage externip;
	time_t externexpire;
	uint16_t externrefresh;

	boolean_t recorddigittimeoutchar;
	uint8_t firstdigittimeout;										/*!< First Digit Timeout. Wait up to 16 seconds for first digit */

	uint8_t digittimeout;
	char digittimeoutchar;
	boolean_t simulate_enbloc;
	uint8_t autoanswer_ring_time;
	skinny_tone_t autoanswer_tone;
	skinny_tone_t remotehangup_tone;
	skinny_tone_t transfer_tone;
	skinny_tone_t dnd_tone;
	skinny_tone_t callwaiting_tone;

	uint8_t callwaiting_interval;
	uint8_t sccp_tos;
	uint8_t audio_tos;
	uint8_t video_tos;
	uint8_t sccp_cos;
	uint8_t audio_cos;
	uint8_t video_cos;
	boolean_t dndFeature;

	boolean_t transfer_on_hangup;
#ifdef CS_MANAGER_EVENTS
	boolean_t callevents;
#endif
	boolean_t echocancel;											/*!< Echo Cancel Support (Boolean, default=on) */
	boolean_t silencesuppression;										/*!< Silence Suppression Support (Boolean, default=on)  */
	boolean_t trustphoneip;											/*!< Trust Phone IP Support (Boolean, default=on) */
	boolean_t privacy;
	boolean_t mwioncall;											/*!< MWI On Call Support (Boolean, default=on) */
	boolean_t directrtp;
	boolean_t useoverlap;
	boolean_t transfer;
	boolean_t cfwdall;                                                                                      /*!< Call Forward All Support (Boolean, default=on) */
	boolean_t cfwdbusy;                                                                                     /*!< Call Forward on Busy Support (Boolean, default=on) */
	boolean_t cfwdnoanswer;                                                                                 /*!< Call Forward on No-Answer Support (Boolean, default=on) */
	uint16_t cfwdnoanswer_timeout;
	char *meetmeopts;
#if HAVE_ICONV
	char *iconvcodepage;											/*!< Iconv Codepage to use during conversion from UTF-8, for old phone models */
#endif
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
#else
	uint8_t _padding1[1];
#endif
	skinny_callHistoryDisposition_t callhistory_answered_elsewhere;
	skinny_ringtype_t ringtype;
	boolean_t meetme;
	boolean_t allowAnonymous;										/*!< Allow Anonymous/Guest Devices */
	boolean_t earlyrtp;

	skinny_lampmode_t mwilamp;
	sccp_blindtransferindication_t blindtransferindication;							/*!< Blind Transfer Indication Support (Boolean, default=on = SCCP_BLINDTRANSFER_MOH) */
	sccp_nat_t nat;
	sccp_call_answer_order_t callanswerorder;								/*!< Call Answer Order */

	struct ast_jb_conf *global_jbconf;
	char *servername;
	char *context;
	skinny_capabilities_t global_preferences;
	char *externhost;
	char *musicclass;
	char *language;
	char *accountcode;
	char *regcontext;
#ifdef CS_SCCP_REALTIME
	char *realtimedevicetable;
	char *realtimelinetable;
#endif
	char used_context[SCCP_MAX_EXTENSION];

	char *config_file_name;
	struct ast_config *cfg;
	sccp_hotline_t *hotline;

	char *token_fallback;											/*!< TokenReq fallback policy: true/false/odd/even/script */
	int token_backoff_time;
	int server_priority;											/*!< Server Priority to fallback to */

	boolean_t reload_in_progress;
	boolean_t pendingUpdate;
};

#define SCCP_SCHED_DEL(id) 												\
({															\
	int _count = 0; 												\
	int _sched_res = -1; 												\
	while ((id) > -1 && (_sched_res = iPbx.sched_del((id))) && ++_count < 10) 					\
		usleep(1); 												\
	if (_count == 10) { 												\
		sccp_log((DEBUGCAT_CORE))(VERBOSE_PREFIX_3 "SCCP: Unable to cancel schedule ID %d.\n", (id)); 		\
	} 														\
	(id) = -1; \
	(_sched_res); 	 												\
})

SCCP_API struct sccp_global_vars *sccp_globals;

__END_C_EXTERN__
