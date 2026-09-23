/*!
 * \file        ast.c
 * \brief       SCCP PBX Asterisk Wrapper Class
 * \author      Diederik de Groot <ddegroot [at] users.sourceforge.net>
 * \note        Reworked, but based on chan_sccp code.
 *              The original chan_sccp driver that was made by Zozo which itself was derived from the chan_skinny driver.
 *              Modified by Jan Czmok and Julien Goodwin
 * \note        This program is free software and may be modified and distributed under the terms of the GNU Public License.
 *              See the LICENSE file at the top of the source tree.
 */
#include "config.h"
#include "common.h"
#include "sccp_channel.h"
#include "sccp_device.h"
#include "sccp_indicate.h"
#include "sccp_netsock.h"
#include "sccp_session.h"
#include "sccp_utils.h"
#include "sccp_pbx.h"
#include "sccp_atomic.h"
#include "sccp_line.h"
#include "sccp_linedevice.h"

SCCP_FILE_VERSION(__FILE__, "");

#include <asterisk.h>
#ifdef HAVE_PBX_ACL_H				// ast_str2cos ast_str2tos
#  include <asterisk/acl.h>
#endif
#include <asterisk/causes.h>
#ifdef HAVE_PBX_APP_H
#  include <asterisk/app.h>
#endif
#include <asterisk/callerid.h>
#include <asterisk/astdb.h>
#include <asterisk/musiconhold.h>
#ifdef HAVE_PBX_FEATURES_H
#  include <asterisk/features.h>
#endif
#include <asterisk/features_config.h>
#include <asterisk/pickup.h>

/*!
 * \brief Skinny Codec Mapping
 */
static const struct pbx2skinny_codec_map {
       pbx_format_enum_type pbx_codec;
       skinny_codec_t skinny_codec;
} pbx2skinny_codec_maps[] = {
       /* *INDENT-OFF* */
       {(pbx_format_enum_type)0,SKINNY_CODEC_NONE},
       {AST_FORMAT_ALAW,       SKINNY_CODEC_G711_ALAW_64K},
       {AST_FORMAT_ALAW,       SKINNY_CODEC_G711_ALAW_56K},
       {AST_FORMAT_ULAW,       SKINNY_CODEC_G711_ULAW_64K},
       {AST_FORMAT_ULAW,       SKINNY_CODEC_G711_ULAW_56K},
       {AST_FORMAT_GSM,        SKINNY_CODEC_GSM},
       {AST_FORMAT_H261,       SKINNY_CODEC_H261},
       {AST_FORMAT_H263,       SKINNY_CODEC_H263},
       {AST_FORMAT_T140,       SKINNY_CODEC_T120},
       {AST_FORMAT_G723,       SKINNY_CODEC_G723_1},
       {AST_FORMAT_SLIN16,     SKINNY_CODEC_WIDEBAND_256K},
       {AST_FORMAT_G729,       SKINNY_CODEC_G729},
       {AST_FORMAT_G729,       SKINNY_CODEC_G729_A},
       {AST_FORMAT_H263P,      SKINNY_CODEC_H263P},
       {AST_FORMAT_G726_AAL2,  SKINNY_CODEC_G726_32K},
       {AST_FORMAT_G726,       SKINNY_CODEC_G726_32K},
       {AST_FORMAT_ILBC,       SKINNY_CODEC_G729_B_LOW},
       {AST_FORMAT_G722,       SKINNY_CODEC_G722_64K},
       {AST_FORMAT_G722,       SKINNY_CODEC_G722_56K},
       {AST_FORMAT_G722,       SKINNY_CODEC_G722_48K},
       {AST_FORMAT_H264,       SKINNY_CODEC_H264},
#    ifdef AST_FORMAT_SIREN7
       {AST_FORMAT_SIREN7,     SKINNY_CODEC_G722_1_24K},                       // should this not be SKINNY_CODEC_G722_1_32K
#    endif
#    ifdef AST_FORMAT_SIREN14
       {AST_FORMAT_SIREN14,    SKINNY_CODEC_G722_1_32K},                       // should this not be SKINNY_CODEC_G722_1_48K
#    endif
#    ifdef AST_FORMAT_OPUS
       {AST_FORMAT_OPUS,       SKINNY_CODEC_OPUS},
#    endif
       /* *INDENT-ON* */
};

/*!
 * \brief AST Device State Structure
 */
static const struct sccp_pbx_devicestate {
	const char *const text;
#ifdef ENUM_AST_DEVICE
	enum ast_device_state devicestate;
#else
	uint8_t devicestate;
#endif
} sccp_pbx_devicestates[] = {
	/* *INDENT-OFF* */
	{"Device is valid but channel doesn't know state",	AST_DEVICE_UNKNOWN},
	{"Device is not in use",				AST_DEVICE_NOT_INUSE},
	{"Device is in use",					AST_DEVICE_INUSE},
	{"Device is busy",					AST_DEVICE_BUSY},
	{"Device is invalid",					AST_DEVICE_INVALID},
	{"Device is unavailable",				AST_DEVICE_UNAVAILABLE},
	{"Device is ringing",					AST_DEVICE_RINGING},
	{"Device is ringing and in use",			AST_DEVICE_RINGINUSE},
	{"Device is on hold",					AST_DEVICE_ONHOLD},
#    ifdef AST_DEVICE_TOTAL
	{"Total num of device states, used for testing",	AST_DEVICE_TOTAL},
#    endif
	/* *INDENT-ON* */
};

gcc_inline const char *pbxsccp_devicestate2str(uint32_t value)
{														/* pbx_impl/ast/ast.h */
	_ARR2STR(sccp_pbx_devicestates, devicestate, value, text);
}

/************************************************************************************************************** CONFIG **/

/*
 * \brief Register a new context
 * \note replacement for ast_context_find_or_create
 *
 * This will first search for a context with your name.  If it exists already, it will not
 * create a new one.  If it does not exist, it will create a new one with the given name
 * and registrar.
 *
 * \param extcontexts pointer to the ast_context structure pointer
 * \param name name of the new context
 * \param registrar registrar of the context
 * \return NULL on failure, and an ast_context structure on success
 */
struct ast_context *pbx_context_find_or_create(struct ast_context **extcontexts, struct ast_hashtab *exttable, const char *name, const char *registrar)
{
	return ast_context_find_or_create(extcontexts, exttable, name, registrar);
}

/*!
 * \brief Load a config file
 *
 * \param filename path of file to open.  If no preceding '/' character,
 * path is considered relative to AST_CONFIG_DIR
 * \param who_asked The module which is making this request.
 * \param flags Optional flags:
 * CONFIG_FLAG_WITHCOMMENTS - load the file with comments intact;
 * CONFIG_FLAG_FILEUNCHANGED - check the file mtime and return CONFIG_STATUS_FILEUNCHANGED if the mtime is the same; or
 * CONFIG_FLAG_NOCACHE - don't cache file mtime (main purpose of this option is to save memory on temporary files).
 *
 * \details
 * Create a config structure from a given configuration file.
 *
 * \return an ast_config data structure on success
 * \retval NULL on error
 */
struct ast_config *pbx_config_load(const char *filename, const char *who_asked, struct ast_flags flags)
{
	return ast_config_load2(filename, who_asked, flags);
}

/******************************************************************************************************** NET / SOCKET **/

/*
 * \brief thread-safe replacement for inet_ntoa()
 * \note replacement for ast_pbx_inet_ntoa
 * \param ia In Address / Source Address
 * \return Address as char
 */
const char *pbx_inet_ntoa(struct in_addr ia)
{
	return ast_inet_ntoa(ia);
}

int pbx_str2tos(const char *value, uint8_t *tos)
{
	uint32_t tos_value = 0;
	ast_str2tos(value, &tos_value);
	*tos = (uint8_t) tos_value;
	return *tos;
}

int pbx_str2cos(const char *value, uint8_t *cos)
{
	uint32_t cos_value = 0;
	ast_str2cos(value, &cos_value);
	*cos = (uint8_t) cos_value;
	return *cos;
}

/************************************************************************************************************* GENERAL **/

/*!   
 * \brief Send ack in manager list transaction
 * \note replacement for astman_send_listack
 * \param s Management Session
 * \param m Management Message
 * \param msg Message
 * \param listflag List Flag
 */
void pbxman_send_listack(struct mansession *s, const struct message *m, char *msg, char *listflag)
{
	astman_send_listack(s, m, msg, listflag);
}

/****************************************************************************************************** CODEC / FORMAT **/

/*!
 * \brief Convert a ast_codec (fmt) to a skinny codec (enum)
 *
 * \param fmt Format as ast_format_type
 *
 * \return skinny_codec 
 */
skinny_codec_t __CONST__ pbx_codec2skinny_codec(ast_format_type fmt)
{
	uint32_t i;

	for (i = 1; i < ARRAY_LEN(pbx2skinny_codec_maps); i++) {
		if (pbx2skinny_codec_maps[i].pbx_codec == (uint64_t) fmt) {
			return pbx2skinny_codec_maps[i].skinny_codec;
		}
	}
	return SKINNY_CODEC_NONE;
}

/*!
 * \brief Convert a skinny_codec (enum) to an ast_codec (fmt)
 *
 * \param codec Skinny Codec (enum)
 *
 * \return fmt Format as ast_format_type
 */
pbx_format_enum_type __CONST__ skinny_codec2pbx_codec(skinny_codec_t codec)
{
	uint32_t i;

	for (i = 1; i < ARRAY_LEN(pbx2skinny_codec_maps); i++) {
		if (pbx2skinny_codec_maps[i].skinny_codec == codec) {
			return pbx2skinny_codec_maps[i].pbx_codec;
		}
	}
	return (pbx_format_enum_type)0;
}

/*!
 * \brief Convert an array of skinny_codecs (enum) to a bit array of ast_codecs (fmt)
 *
 * \param codecs Array of Skinny Codecs
 *
 * \return bit array fmt/Format of ast_format_type (int)
 */
pbx_format_type __PURE__ skinny_codecs2pbx_codecs(const skinny_codec_t * const codecs)
{
	uint32_t i;
	int res_codec = 0;

	for (i = 1; i < SKINNY_MAX_CAPABILITIES; i++) {
		res_codec |= skinny_codec2pbx_codec(codecs[i]);
	}
	return res_codec;
}

/*!
 * \brief Retrieve the SCCP Channel from an Asterisk Channel
 *
 * \param pbx_channel PBX Channel
 * \return SCCP Channel on Success or Null on Fail
 *
 * \todo this code is not pbx independent
 */
sccp_channel_t *get_sccp_channel_from_pbx_channel(const PBX_CHANNEL_TYPE * pbx_channel)
{
	sccp_channel_t *c = NULL;

	if (pbx_channel && CS_AST_CHANNEL_PVT(pbx_channel) && CS_AST_CHANNEL_PVT_CMP_TYPE(pbx_channel, "SCCP")) {
		if ((c = CS_AST_CHANNEL_PVT(pbx_channel))) {
			return sccp_channel_retain(c);
		} 
		pbx_log(LOG_ERROR, "%s: SCCP channel without SCCP call data (refcount bug)\n", pbx_channel_name(pbx_channel));
		return NULL;
	} else {
		return NULL;
	}
}

/*
static void log_hangup_info(const char * hanguptype, constChannelPtr c, PBX_CHANNEL_TYPE * const pbx_channel)
{
	sccp_log(DEBUGCAT_PBX)("%s: (%s):\n"
			       " - pbx_channel: %s\n"
			       " - channel_flags:\n"
			       "   - ZOMBIE: %s\n"
			       "   - BLOCKING: %s\n"
			       " - pbx_channel_hangup_locked: %s\n"
			       " - runningPbxThread: %s\n"
			       " - pbx_channel_is_bridged: %s\n"
			       " - pbx_channel_pbx: %s\n",
			       c->designator, hanguptype, pbx_channel ? pbx_channel_name(pbx_channel) : "", pbx_channel ? pbx_test_flag(pbx_channel_flags(pbx_channel), AST_FLAG_ZOMBIE) ? "YES" : "NO" : "",
			       pbx_channel ? pbx_test_flag(pbx_channel_flags(pbx_channel), AST_FLAG_BLOCKING) ? "YES" : "NO" : "", pbx_channel ? pbx_check_hangup_locked(pbx_channel) ? "YES" : "NO" : "",
			       c->isRunningPbxThread ? "YES" : "NO", iPbx.channel_is_bridged((channelPtr)c) ? "YES" : "NO", pbx_channel ? pbx_channel_pbx(pbx_channel) ? "YES" : "NO" : "");
#if CS_REFCOUNT_DEBUG
	AUTO_RELEASE(sccp_device_t, d, sccp_channel_getDevice(c));
	if(d) {
		pbx_str_t * buf = pbx_str_create(DEFAULT_PBX_STR_BUFFERSIZE * 3);
		sccp_refcount_gen_report(d, &buf);
		pbx_log(LOG_NOTICE, "%s: reference report for the device at %s:\n%s\n", c->designator, hanguptype, pbx_str_buffer(buf));
		sccp_free(buf);
	}
#endif
}
*/
static boolean_t sccp_astgenwrap_handleHangup(constChannelPtr channel, const char * hanguptype)
{
	boolean_t res = FALSE;
	int tries = 10;
	AUTO_RELEASE(sccp_channel_t, c, sccp_channel_retain(channel));
	if (channel) {
		c->isHangingUp = TRUE;
		PBX_CHANNEL_TYPE * pbx_channel = pbx_channel_ref(c->owner);

		if(ATOMIC_FETCH(&c->scheduler.deny, &c->scheduler.lock) == 0) {
			sccp_channel_stop_and_deny_scheduled_tasks(c);
		}
		do {
			// log_hangup_info(hanguptype, c, pbx_channel);
			if(!pbx_channel || pbx_test_flag(pbx_channel_flags(pbx_channel), AST_FLAG_ZOMBIE) || pbx_check_hangup_locked(pbx_channel)) {
				AUTO_RELEASE(sccp_device_t, d, sccp_channel_getDevice(c));
				if(d) {
					sccp_indicate(d, c, SCCP_CHANNELSTATE_ONHOOK);
					sccp_log(DEBUGCAT_PBX)("%s: (%s): Onhook Only\n", c->designator, hanguptype);
				}
				if (iPbx.dumpchan) {
					char * buf = sccp_alloca(sizeof(char) * 2048);
					iPbx.dumpchan(pbx_channel, buf, 2048);
					sccp_log(DEBUGCAT_PBX)("SCCP: (dumpchan) %s", buf);
				}
				res = TRUE;
				break;
			}
			pbx_channel_lock(pbx_channel);
			if(pbx_check_hangup(pbx_channel)) {
				// already being hungup
				sccp_log(DEBUGCAT_PBX)("%s: (%s): Already being hungup, giving up\n", c->designator, hanguptype);
				res = FALSE;
				break;
			}
			if(c->isRunningPbxThread || pbx_channel_pbx(pbx_channel)) {
				// outbound / call initiator (running pbx_pbx_start)
				sccp_log(DEBUGCAT_PBX)("%s: (%s): Hangup Queued\n", c->designator, hanguptype);
				pbx_channel_unlock(pbx_channel);
				res = ast_queue_hangup(pbx_channel) ? FALSE : TRUE;
				res = TRUE;
				break;
			}
			if(SCCP_CHANNELSTATE_IsSettingUp(c->state) || SCCP_CHANNELSTATE_IsConnected(c->state) || iPbx.channel_is_bridged(c)) {
				// inbound / receiving call
				sccp_log(DEBUGCAT_PBX)("%s: (%s): Softhangup\n", c->designator, hanguptype);
				ast_softhangup(pbx_channel, AST_SOFTHANGUP_DEV);
				pbx_channel_unlock(pbx_channel);
				res = TRUE;
				break;
			}
			if(pbx_test_flag(pbx_channel_flags(pbx_channel), AST_FLAG_BLOCKING)) { /* not sure if required */
				// blocking while being the initiator of the call, strange
				sccp_log(DEBUGCAT_PBX)("%s: (%s): Blocker detected, SIGURG signal sent\n", c->designator, hanguptype);
				pthread_kill(pbx_channel_blocker(pbx_channel), SIGURG);
				sched_yield();
				pbx_safe_sleep(pbx_channel, 1000);
				pbx_channel_unlock(pbx_channel);
				res = TRUE;
				break;
			}
			if(SCCP_CHANNELSTATE_Idling(c->state) || SCCP_CHANNELSTATE_IsDialing(c->state) || SCCP_CHANNELSTATE_IsTerminating(c->state)) {
				/* hard hangup for all partially started calls */
				sccp_log(DEBUGCAT_PBX)("%s: (%s): Hard Hangup\n", c->designator, hanguptype);
				pbx_channel_unlock(pbx_channel);
				ast_hangup(pbx_channel);
				res = TRUE;
				break;
			}
		} while(tries--);
		pbx_channel_unref(pbx_channel);
	}
	return res;
}

static boolean_t sccp_astgenwrap_carefullHangup(constChannelPtr c)
{
	boolean_t res = FALSE;
	AUTO_RELEASE(sccp_channel_t, channel , sccp_channel_retain(c));
	if (channel) {
		channel->isHangingUp = TRUE;
		PBX_CHANNEL_TYPE *pbx_channel = pbx_channel_ref(channel->owner);
		sched_yield();
		pbx_safe_sleep(pbx_channel, 1000);
		res = sccp_astgenwrap_handleHangup(channel, "RequestCarefullHangup");
		pbx_channel_unref(pbx_channel);
	}
	return res;
}

boolean_t sccp_astgenwrap_requestQueueHangup(constChannelPtr c)
{
	return sccp_astgenwrap_handleHangup(c, "RequestQueueHangup");
}

boolean_t sccp_astgenwrap_requestHangup(constChannelPtr c)
{
	return sccp_astgenwrap_handleHangup(c, "RequestHangup");
}

/***** database *****/
boolean_t sccp_astwrap_addToDatabase(const char *family, const char *key, const char *value)
{
	int res;

	// pbx_log(LOG_NOTICE, "family:%s, key:%s, value:%s\n", family, key, value);
	if (sccp_strlen_zero(family) || sccp_strlen_zero(key) || sccp_strlen_zero(value)) {
		return FALSE;
	}
	res = ast_db_put(family, key, value);
	return (!res) ? TRUE : FALSE;
}

boolean_t sccp_astwrap_getFromDatabase(const char *family, const char *key, char *out, int outlen)
{
	int res;

	// pbx_log(LOG_NOTICE, "family:%s, key:%s\n", family, key);
	if (sccp_strlen_zero(family) || sccp_strlen_zero(key)) {
		return FALSE;
	}
	res = ast_db_get(family, key, out, outlen);
	return (!res) ? TRUE : FALSE;
}

boolean_t sccp_astwrap_removeFromDatabase(const char *family, const char *key)
{
	int res;

	// pbx_log(LOG_NOTICE, "family:%s, key:%s\n", family, key);
	if (sccp_strlen_zero(family) || sccp_strlen_zero(key)) {
		return FALSE;
	}
	res = ast_db_del(family, key);
	return (!res) ? TRUE : FALSE;
}

boolean_t sccp_astwrap_removeTreeFromDatabase(const char *family, const char *key)
{
	int res;

	// pbx_log(LOG_NOTICE, "family:%s, key:%s\n", family, key);
	if (sccp_strlen_zero(family) || sccp_strlen_zero(key)) {
		return FALSE;
	}
	res = ast_db_deltree(family, key);
	return (!res) ? TRUE : FALSE;
}

/* end - database */

/*!
 * \brief Turn on music on hold on a given channel 
 * \note replacement for ast_moh_start
 *
 * \param pbx_channel The channel structure that will get music on hold
 * \param mclass The class to use if the musicclass is not currently set on the channel structure.
 * \param interpclass The class to use if the musicclass is not currently set on the channel structure or in the mclass argument.
 *
 * \retval Zero on success
 * \retval non-zero on failure
 */
int sccp_astwrap_moh_start(PBX_CHANNEL_TYPE * pbx_channel, const char *mclass, const char *interpclass)
{
	if (!pbx_test_flag(pbx_channel_flags(pbx_channel), AST_FLAG_MOH)) {
		pbx_set_flag(pbx_channel_flags(pbx_channel), AST_FLAG_MOH);
		return ast_moh_start( pbx_channel, mclass, interpclass);
	} 
	return 0;
}

void sccp_astwrap_moh_stop(PBX_CHANNEL_TYPE * pbx_channel)
{
	if (pbx_test_flag(pbx_channel_flags(pbx_channel), AST_FLAG_MOH)) {
		pbx_clear_flag(pbx_channel_flags(pbx_channel), AST_FLAG_MOH);
		ast_moh_stop( pbx_channel);
	}
}

void sccp_astwrap_redirectedUpdate(sccp_channel_t * channel, const void *data, size_t datalen)
{
	PBX_CHANNEL_TYPE *ast = channel->owner;
	int redirectreason = 0;
	sccp_callinfo_t *ci = sccp_channel_getCallInfo(channel);

	iCallInfo.Getter(ci, 
		SCCP_CALLINFO_LAST_REDIRECT_REASON, &redirectreason,
		SCCP_CALLINFO_KEY_SENTINEL);
	struct ast_party_id redirecting_from = pbx_channel_redirecting_effective_from(ast);
	struct ast_party_id redirecting_to = pbx_channel_redirecting_effective_to(ast);

	sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "%s: Got redirecting update. From %s<%s>; To %s<%s>\n", pbx_channel_name(ast),
				(redirecting_from.name.valid && redirecting_from.name.str) ? redirecting_from.name.str : "", 
				(redirecting_from.number.valid && redirecting_from.number.str) ? redirecting_from.number.str : "", 
				(redirecting_to.name.valid && redirecting_to.name.str) ? redirecting_to.name.str : "",
				(redirecting_to.number.valid && redirecting_to.number.str) ? redirecting_to.number.str : "");

	iCallInfo.Setter(ci, 
		SCCP_CALLINFO_LAST_REDIRECTINGPARTY_NAME, redirecting_from.name.valid && redirecting_from.name.str ? redirecting_from.name.str : NULL, 
		SCCP_CALLINFO_LAST_REDIRECTINGPARTY_NUMBER, (redirecting_from.number.valid && redirecting_from.number.str) ? redirecting_from.number.str : "",
		SCCP_CALLINFO_ORIG_CALLEDPARTY_NUMBER, (redirecting_from.number.valid && redirecting_from.number.str) ? redirecting_from.number.str : "",
		SCCP_CALLINFO_ORIG_CALLEDPARTY_NAME, redirecting_from.name.valid && redirecting_from.name.str ? redirecting_from.name.str : NULL,
		SCCP_CALLINFO_ORIG_CALLEDPARTY_REDIRECT_REASON, redirectreason,
		SCCP_CALLINFO_LAST_REDIRECT_REASON, 4,					// need to figure out these codes
		SCCP_CALLINFO_KEY_SENTINEL);

	//sccp_channel_display_callInfo(channel);
	sccp_channel_send_callinfo2(channel);
}

/*!
 * \brief Update Connected Line
 * \param channel Asterisk Channel as ast_channel
 * \param data Asterisk Data
 * \param datalen Asterisk Data Length
 */
void sccp_astwrap_connectedline(sccp_channel_t * channel, const void *data, size_t datalen)
{
	PBX_CHANNEL_TYPE *ast = channel->owner;
	int changes = 0;
	sccp_callinfo_t *const callInfo = sccp_channel_getCallInfo(channel);

	sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "%s: %s call Got connected line update, connected.id.number=%s, connected.id.name=%s, source=%s\n",
		channel->calltype == SKINNY_CALLTYPE_INBOUND ? "INBOUND" : "OUTBOUND",
		pbx_channel_name(ast), 
		pbx_channel_connected_id(ast).number.str ? pbx_channel_connected_id(ast).number.str : "(nil)", 
		pbx_channel_connected_id(ast).name.str ? pbx_channel_connected_id(ast).name.str : "(nil)", 
		pbx_connected_line_source_name(pbx_channel_connected_source(ast))
	);

	char tmpCallingNumber[StationMaxDirnumSize] = {0};
	char tmpCallingName[StationMaxNameSize] = {0};
	char tmpCalledNumber[StationMaxDirnumSize] = {0};
	char tmpCalledName[StationMaxNameSize] = {0};
	char tmpOrigCalledPartyNumber[StationMaxDirnumSize] = {0};
	char tmpOrigCalledPartyName[StationMaxNameSize] = {0};
	int tmpOrigCalledPartyRedirectReason = 0;
	int tmpLastRedirectReason = 4;		/* \todo need to figure out more about these codes */

	iCallInfo.Getter(callInfo,
		SCCP_CALLINFO_CALLINGPARTY_NUMBER, &tmpCallingNumber,
		SCCP_CALLINFO_CALLINGPARTY_NAME, &tmpCallingName,
		SCCP_CALLINFO_CALLEDPARTY_NUMBER, &tmpCalledNumber,
		SCCP_CALLINFO_CALLEDPARTY_NAME, &tmpCalledName,
		SCCP_CALLINFO_ORIG_CALLEDPARTY_NUMBER, &tmpOrigCalledPartyNumber,
		SCCP_CALLINFO_ORIG_CALLEDPARTY_NAME, &tmpOrigCalledPartyName,
		SCCP_CALLINFO_ORIG_CALLEDPARTY_REDIRECT_REASON, &tmpOrigCalledPartyRedirectReason,
		SCCP_CALLINFO_KEY_SENTINEL);

	if (SCCP_CHANNELSTATE_CALLPARK == channel->state && !sccp_strlen_zero (pbx_builtin_getvar_helper (channel->owner, "PARKER"))) {
		channel->calltype = SKINNY_CALLTYPE_OUTBOUND;
		sccp_channel_setChannelstate (channel, SCCP_CHANNELSTATE_RINGOUT);
	}

	/* set the original calling/called party if the reason is a transfer */
	if (channel->calltype == SKINNY_CALLTYPE_INBOUND) {
		if (pbx_channel_connected_source(ast) == AST_CONNECTED_LINE_UPDATE_SOURCE_TRANSFER || pbx_channel_connected_source(ast) == AST_CONNECTED_LINE_UPDATE_SOURCE_TRANSFER_ALERTING) {
			// sccp_log(DEBUGCAT_CHANNEL) ("SCCP: (connectedline) Transfer Destination\n");
			changes = iCallInfo.Setter(callInfo,
				SCCP_CALLINFO_CALLINGPARTY_NUMBER, pbx_channel_connected_id(ast).number.str,
				SCCP_CALLINFO_CALLINGPARTY_NAME, pbx_channel_connected_id(ast).name.str,

				SCCP_CALLINFO_ORIG_CALLINGPARTY_NUMBER, pbx_channel_connected_id(ast).number.str,
				SCCP_CALLINFO_ORIG_CALLINGPARTY_NAME, pbx_channel_connected_id(ast).name.str,

				SCCP_CALLINFO_ORIG_CALLEDPARTY_NUMBER, tmpCallingNumber,
				SCCP_CALLINFO_ORIG_CALLEDPARTY_NAME, tmpCallingName,
				SCCP_CALLINFO_ORIG_CALLEDPARTY_REDIRECT_REASON, tmpOrigCalledPartyRedirectReason,

				SCCP_CALLINFO_LAST_REDIRECTINGPARTY_NUMBER, tmpCallingNumber,
				SCCP_CALLINFO_LAST_REDIRECTINGPARTY_NAME, tmpCallingNumber,
				SCCP_CALLINFO_LAST_REDIRECT_REASON, tmpLastRedirectReason,

				SCCP_CALLINFO_KEY_SENTINEL);
		} else {
			struct ast_party_id redirecting_orig = pbx_channel_redirecting_effective_orig(ast);
			if (redirecting_orig.name.valid || redirecting_orig.number.valid) {
				changes = iCallInfo.Setter(callInfo,
					SCCP_CALLINFO_CALLINGPARTY_NUMBER, pbx_channel_connected_id(ast).number.str,
					SCCP_CALLINFO_CALLINGPARTY_NAME, pbx_channel_connected_id(ast).name.str,
					SCCP_CALLINFO_ORIG_CALLEDPARTY_NAME, redirecting_orig.name.valid ? ast_channel_redirecting(ast)->orig.name.str : "",
					SCCP_CALLINFO_ORIG_CALLEDPARTY_NUMBER, redirecting_orig.number.valid ? ast_channel_redirecting(ast)->orig.number.str : "",
					SCCP_CALLINFO_KEY_SENTINEL);
			} else {
				changes = iCallInfo.Setter(callInfo,
					SCCP_CALLINFO_CALLINGPARTY_NUMBER, pbx_channel_connected_id(ast).number.str,
					SCCP_CALLINFO_CALLINGPARTY_NAME, pbx_channel_connected_id(ast).name.str,
					SCCP_CALLINFO_KEY_SENTINEL);
			}
			sccp_indicate (NULL, channel, channel->state);
		}
	} else /* OUTBOUND CALL */ {
		if (sccp_strlen_zero(pbx_builtin_getvar_helper(ast, "SETCALLEDPARTY"))) {
			struct ast_party_id connected = pbx_channel_connected_id(ast);
			changes = iCallInfo.Setter(callInfo,
				SCCP_CALLINFO_CALLEDPARTY_NUMBER, connected.number.valid ? connected.number.str : tmpCalledNumber,
				SCCP_CALLINFO_CALLEDPARTY_NAME, connected.name.valid ? connected.name.str : tmpCalledName,
				SCCP_CALLINFO_ORIG_CALLEDPARTY_NUMBER, (!sccp_strlen_zero(tmpOrigCalledPartyNumber)) ? tmpOrigCalledPartyNumber : (!sccp_strlen_zero(tmpCalledNumber) ? tmpCalledNumber : connected.number.str),
				SCCP_CALLINFO_ORIG_CALLEDPARTY_NAME, (!sccp_strlen_zero(tmpOrigCalledPartyName)) ? tmpOrigCalledPartyName : (!sccp_strlen_zero(tmpCalledName) ? tmpCalledName : connected.name.str),
				SCCP_CALLINFO_KEY_SENTINEL);
		}
	}
	//sccp_channel_display_callInfo(channel);
	if (changes) {
		sccp_channel_send_callinfo2(channel);

		/* We have a preliminary connected line, indicate RINGOUT_ALERTING */
		if (SKINNY_CALLTYPE_OUTBOUND == channel->calltype && SCCP_CHANNELSTATE_RINGOUT == channel->state) {
			sccp_indicate(NULL, channel, SCCP_CHANNELSTATE_RINGOUT_ALERTING);
		}
#if CS_SCCP_VIDEO
		const char *VideoStr = pbx_builtin_getvar_helper(channel->owner, "SCCP_VIDEO_MODE");
		if (VideoStr && !sccp_strlen_zero(VideoStr)) {
			sccp_channel_setVideoMode(channel, VideoStr);
		}
#endif
        }
	if (SCCP_CHANNELSTATE_CALLPARK == channel->state || !sccp_strlen_zero (pbx_builtin_getvar_helper (channel->owner, "PARK_RETRIEVER"))) {
		sccp_indicate_force (NULL, channel, SCCP_CHANNELSTATE_CONNECTED);
	}
}

void sccp_astwrap_sendRedirectedUpdate(const sccp_channel_t * channel, const char *fromNumber, const char *fromName, const char *toNumber, const char *toName, uint8_t reason)
{
	struct ast_party_redirecting redirecting;
	struct ast_set_party_redirecting update_redirecting;

	sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "%s: Send Redirected Update. From %s<%s>, To: %s<%s>\n", channel->designator, fromName, fromNumber, toName, toNumber);

	ast_party_redirecting_init(&redirecting);
	memset(&update_redirecting, 0, sizeof(update_redirecting));

	/* update redirecting info line for source part */
	if (fromNumber) {
		update_redirecting.from.number = 1;
		redirecting.from.number.valid = 1;
		redirecting.from.number.str = pbx_strdup(fromNumber);
	}

	if (fromName) {
		update_redirecting.from.name = 1;
		redirecting.from.name.valid = 1;
		redirecting.from.name.str = pbx_strdup(fromName);
	}

	if (toNumber) {
		update_redirecting.to.number = 1;
		redirecting.to.number.valid = 1;
		redirecting.to.number.str = pbx_strdup(toNumber);
	}

	if (toName) {
		update_redirecting.to.name = 1;
		redirecting.to.name.valid = 1;
		redirecting.to.name.str = pbx_strdup(toName);
	}
	redirecting.reason.code = reason;

	ast_channel_queue_redirecting_update(channel->owner, &redirecting, &update_redirecting);
	ast_party_redirecting_free(&redirecting);
}


int sccp_parse_alertinfo(PBX_CHANNEL_TYPE *pbx_channel, skinny_ringtype_t *ringermode)
{
	int res = 0;
	const char *alert_info = pbx_builtin_getvar_helper(pbx_channel, "ALERT_INFO");
	if (alert_info && !sccp_strlen_zero(alert_info)) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: Found ALERT_INFO=%s\n", pbx_channel_name(pbx_channel), alert_info);
		if (!strncasecmp(alert_info, "bellcore-dr", 11) && strlen(alert_info) >= 12) {
			switch(alert_info[11]) {
				case '1': 
					*ringermode = SKINNY_RINGTYPE_INSIDE;
					break;
				case '2':
					*ringermode = SKINNY_RINGTYPE_OUTSIDE;
					break;
				case '3':
					*ringermode = SKINNY_RINGTYPE_FEATURE;
					break;
				case '4':
					*ringermode = SKINNY_RINGTYPE_BELLCORE_4;
					break;
				case '5':
					*ringermode = SKINNY_RINGTYPE_URGENT;
					break;
				default:
					pbx_log(LOG_NOTICE, "%s: ALERT_INFO '%s' is not a known ring type (Bellcore-dr1 to dr5); default ring used\n", pbx_channel_name(pbx_channel), alert_info);
					*ringermode = SKINNY_RINGTYPE_SENTINEL;
					res = -1;
					break;
			}
		} else {
			*ringermode = skinny_ringtype_str2val(alert_info);
		}
	}
	if (*ringermode == SKINNY_RINGTYPE_SENTINEL) {
		*ringermode = GLOB(ringtype);
	}
	return res;
}

int sccp_parse_auto_answer(PBX_CHANNEL_TYPE * pbx_channel, sccp_autoanswer_t * autoanswer_type)
{
	int res = 0;
	const char * auto_answer = pbx_builtin_getvar_helper(pbx_channel, "AUTO_ANSWER");
	if (auto_answer && !sccp_strlen_zero(auto_answer)) {
		sccp_log((DEBUGCAT_CORE))(VERBOSE_PREFIX_3 "%s: Found AUTO_ANSWER=%s\n", pbx_channel_name(pbx_channel), auto_answer);
		if (sccp_strcaseequals(auto_answer, "1way") || sccp_strcaseequals(auto_answer, "1w")) {
			*autoanswer_type = SCCP_AUTOANSWER_1W;
		} else if (sccp_strcaseequals(auto_answer, "2way") || sccp_strcaseequals(auto_answer, "2w")) {
			*autoanswer_type = SCCP_AUTOANSWER_2W;
		} else {
			res = -1;
		}
	}
	return res;
}

/*!
 * parse the DIAL options and store results by ref
 */
int sccp_parse_dial_options(char *options, sccp_autoanswer_t *autoanswer_type, uint8_t *autoanswer_cause, skinny_ringtype_t *ringermode)
{
	int res = 0;
	int optc = 0;
	char *optv[5];
	int opti = 0;

	/* parse options */
	if (options && (optc = sccp_app_separate_args(options, '/', optv, sizeof(optv) / sizeof(optv[0])))) {
		for (opti = 0; opti < optc; opti++) {
			if (!strncasecmp(optv[opti], "aa", 2)) {
				/* let's use the old style auto answer aa1w and aa2w */
				if (!strncasecmp(optv[opti], "aa1w", 4)) {
					*autoanswer_type = SCCP_AUTOANSWER_1W;
					optv[opti] += 4;
				} else if (!strncasecmp(optv[opti], "aa2w", 4)) {
					*autoanswer_type = SCCP_AUTOANSWER_2W;
					optv[opti] += 4;
				} else if (!strncasecmp(optv[opti], "aa=", 3)) {
					optv[opti] += 3;
					if (!strncasecmp(optv[opti], "1w", 2)) {
						*autoanswer_type = SCCP_AUTOANSWER_1W;
						optv[opti] += 2;
					} else if (!strncasecmp(optv[opti], "2w", 2)) {
						*autoanswer_type = SCCP_AUTOANSWER_2W;
						optv[opti] += 2;
					}
				}

				/* since the pbx ignores autoanswer_cause unless SCCP_RWLIST_GETSIZE(&l->channels) > 1, it is safe to set it if provided */
				if (!sccp_strlen_zero(optv[opti]) && (autoanswer_cause)) {
					if (!strcasecmp(optv[opti], "b")) {
						*autoanswer_cause = AST_CAUSE_BUSY;
					} else if (!strcasecmp(optv[opti], "u")) {
						*autoanswer_cause = AST_CAUSE_REQUESTED_CHAN_UNAVAIL;
					} else if (!strcasecmp(optv[opti], "c")) {
						*autoanswer_cause = AST_CAUSE_CONGESTION;
					}
				}
				/* check for ringer options */
			} else if (!strncasecmp(optv[opti], "ringer=", 7)) {
				optv[opti] += 7;
				*ringermode = skinny_ringtype_str2val(optv[opti]);
			} else {
				pbx_log(LOG_WARNING, "SCCP: dial option '%s' is not aa=, aa1w, aa2w, ringer=, or a known flag; ignored\n", optv[opti]);
				res = -1;
			}
		}
	}
	if (*ringermode == SKINNY_RINGTYPE_SENTINEL) {
		*ringermode = GLOB(ringtype);
	}
	return res;
}

/*!
 * \brief ACF Channel Read callback
 *
 * \param ast Asterisk Channel
 * \param funcname      functionname as const char *
 * \param preparse      arguments as char *
 * \param buf           buffer as char *
 * \param buflen        bufferlenght as size_t
 * \return result as int
 *
 * \called_from_asterisk
 */
int sccp_astgenwrap_channel_read(PBX_CHANNEL_TYPE * ast, NEWCONST char *funcname, char *preparse, char *buf, size_t buflen)
{
	int res = -1;

	char *parse = pbx_strdupa(preparse);

	AST_DECLARE_APP_ARGS(args, AST_APP_ARG(param); AST_APP_ARG(type); AST_APP_ARG(field););
	AST_STANDARD_APP_ARGS(args, parse);

	/* begin asserts */
	if (!ast || !CS_AST_CHANNEL_PVT_IS_SCCP(ast)) {
		pbx_log(LOG_WARNING, "CHANNEL() read of SCCP fields on %s, which is not an SCCP channel\n", ast ? pbx_channel_name(ast) : "none");
		return -1;
	}

	AUTO_RELEASE(sccp_channel_t, c , get_sccp_channel_from_pbx_channel(ast));
	if (!c) {
		return -1;
	}
	AUTO_RELEASE(sccp_device_t, d , sccp_channel_getDevice(c));
	if (!d) {
		return -1;
	}
	/* end asserts */
	
	/* handle params */
	if (!strcasecmp(args.param, "peerip")) {
		struct sockaddr_storage sas = { 0 };
		if (sccp_session_getOurIP(d->session, &sas, 0)) {
			sccp_copy_string(buf, sccp_netsock_stringify(&sas), buflen);
		} else {
			sccp_copy_string(buf, "--", buflen);
		}
	} else if (!strcasecmp(args.param, "recvip")) {
		struct sockaddr_storage sas = { 0 };
		if (sccp_session_getSas(d->session, &sas)) {
			sccp_copy_string(buf, sccp_netsock_stringify(&sas), buflen);
		} else {
			sccp_copy_string(buf, "--", buflen);
		}
	} else if (sccp_strcaseequals(args.type, "codec")) {
		sccp_codec_multiple2str (buf, buflen - 1, c->capabilities.audio, SKINNY_MAX_CAPABILITIES);
		sccp_codec_multiple2str (buf, buflen - 1, c->preferences.audio, SKINNY_MAX_CAPABILITIES);
#if CS_SCCP_VIDEO
		sccp_codec_multiple2str (buf, buflen - 1, c->capabilities.video, SKINNY_MAX_CAPABILITIES);
		sccp_codec_multiple2str (buf, buflen - 1, c->preferences.video, SKINNY_MAX_CAPABILITIES);
#endif
	} else if (sccp_strcaseequals(args.type, "video")) {
#if CS_SCCP_VIDEO
		sccp_copy_string(buf, sccp_video_mode2str(c->videomode), buflen);
#endif
	} else if (!strcasecmp(args.param, "useragent")) {
		sccp_copy_string(buf, skinny_devicetype2str(d->skinny_type), buflen);
	} else if (!strcasecmp(args.param, "from")) {
		sccp_copy_string(buf, (char *) d->id, buflen);
	} else if (!strcasecmp(args.param, "rtpqos")) {
		PBX_RTP_TYPE *rtp = NULL;

		if (sccp_strlen_zero(args.type)) {
			args.type = pbx_strdupa("audio");
		}

		if (sccp_strcaseequals(args.type, "audio")) {
			rtp = c->rtp.audio.instance;
		} else if (sccp_strcaseequals(args.type, "video")) {
			rtp = c->rtp.video.instance;
		/*
		} else if (sccp_strcaseequals(args.type, "text")) {
			rtp = c->rtp.text.instance;
		*/
		} else {
			return -1;
		}
		ast_channel_lock(ast);
		do {
			if (rtp) {
				if (sccp_strlen_zero(args.field) || sccp_strcaseequals(args.field, "all")) {
					char quality_buf[256 /*AST_MAX_USER_FIELD */ ];

					if (!ast_rtp_instance_get_quality(rtp, AST_RTP_INSTANCE_STAT_FIELD_QUALITY, quality_buf, sizeof(quality_buf))) {
						break;
					}

					sccp_copy_string(buf, quality_buf, buflen);
					res = 0;
					break;
				} 
				struct ast_rtp_instance_stats stats;
				int i;
				enum __int_double { __INT, __DBL };
				struct {
					const char *name;
					enum __int_double type;
					union {
						unsigned int *i4;
						double *d8;
					};
				} lookup[] = {
					/* *INDENT-OFF* */
					{"txcount", 		__INT, {.i4 = &stats.txcount},}, 
					{"rxcount", 		__INT, {.i4 = &stats.rxcount,},}, 
					{"txjitter", 		__DBL, {.d8 = &stats.txjitter,},}, 
					{"rxjitter", 		__DBL, {.d8 = &stats.rxjitter,},},
					{"remote_maxjitter", 	__DBL, {.d8 = &stats.remote_maxjitter,},},
					{"remote_minjitter", 	__DBL, {.d8 = &stats.remote_minjitter,},},
					{"remote_normdevjitter",__DBL, {.d8 = &stats.remote_normdevjitter,},},
					{"remote_stdevjitter", 	__DBL, {.d8 = &stats.remote_stdevjitter,},},
					{"local_maxjitter",	__DBL, {.d8 = &stats.local_maxjitter,},},
					{"local_minjitter", 	__DBL, {.d8 = &stats.local_minjitter,},},
					{"local_normdevjitter", __DBL, {.d8 = &stats.local_normdevjitter,},},
					{"local_stdevjitter", 	__DBL, {.d8 = &stats.local_stdevjitter,},},
					{"txploss", 		__INT, {.i4 = &stats.txploss,},},
					{"rxploss", 		__INT, {.i4 = &stats.rxploss,},},
					{"remote_maxrxploss", 	__DBL, {.d8 = &stats.remote_maxrxploss,},},
					{"remote_minrxploss", 	__DBL, {.d8 = &stats.remote_minrxploss,},},
					{"remote_normdevrxploss",__DBL, {.d8 = &stats.remote_normdevrxploss,},},
					{"remote_stdevrxploss", __DBL, {.d8 = &stats.remote_stdevrxploss,},},
					{"local_maxrxploss", 	__DBL, {.d8 = &stats.local_maxrxploss,},},
					{"local_minrxploss", 	__DBL, {.d8 = &stats.local_minrxploss,},},
					{"local_normdevrxploss",__DBL, {.d8 = &stats.local_normdevrxploss,},},
					{"local_stdevrxploss", 	__DBL, {.d8 = &stats.local_stdevrxploss,},},
					{"rtt", 		__DBL, {.d8 = &stats.rtt,},},
					{"maxrtt", 		__DBL, {.d8 = &stats.maxrtt,},},
					{"minrtt", 		__DBL, {.d8 = &stats.minrtt,},},
					{"normdevrtt", 		__DBL, {.d8 = &stats.normdevrtt,},},
					{"stdevrtt", 		__DBL, {.d8 = &stats.stdevrtt,},},
					{"local_ssrc", 		__INT, {.i4 = &stats.local_ssrc,},},
					{"remote_ssrc", 	__INT, {.i4 = &stats.remote_ssrc,},},
					{NULL,},
					/* *INDENT-ON* */
				};

				if (ast_rtp_instance_get_stats(rtp, &stats, AST_RTP_INSTANCE_STAT_ALL)) {
					break;
				}

				for (i = 0; !sccp_strlen_zero(lookup[i].name); i++) {
					if (sccp_strcaseequals(args.field, lookup[i].name)) {
						if (lookup[i].type == __INT) {
							snprintf(buf, buflen, "%u", *lookup[i].i4);
						} else {
							snprintf(buf, buflen, "%f", *lookup[i].d8);
						}
						res = 0;
						break;
					}
				}
			}
		} while (0);
		ast_channel_unlock(ast);
	} else {
		pbx_log(LOG_WARNING, "%s: %s(%s) is not a known SCCP channel field; returned empty\n", pbx_channel_name(ast), funcname, preparse);
	}
	return res;
}

int sccp_astgenwrap_channel_write(PBX_CHANNEL_TYPE * ast, const char *funcname, char *args, const char *value)
{
	int res = 0;

	AUTO_RELEASE(sccp_channel_t, c , get_sccp_channel_from_pbx_channel(ast));
	if (c) {
		if (!strcasecmp(args, "MaxCallBR")) {
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: set max call bitrate to %s\n", (char *) c->designator, value);

			if (sscanf(value, "%d", &c->maxBitRate) == 1) {
				pbx_builtin_setvar_helper(ast, "_MaxCallBR", value);
			} else {
				res = -1;
			}

		} else if (!strcasecmp(args, "codec")) {
			res = (TRUE == sccp_channel_setPreferredCodec(c, value)) ? 0 : -1;
			
		} else if (!strcasecmp(args, "video")) {
			res = (TRUE == sccp_channel_setVideoMode(c, value)) ? 0 : -1;
			
		} else if (!strcasecmp(args, "CallingParty")) {
			if(!value || sccp_strlen_zero(value)) {
				pbx_log(LOG_WARNING, "%s: CHANNEL(%s) needs a value like \"Name\" <number>; not set\n", pbx_channel_name(ast), "CallingParty");
				return -1;
			}
			char *num, *name;
			pbx_callerid_parse((char *) value, &name, &num);
			sccp_channel_set_callingparty(c, name, num);
			sccp_channel_display_callInfo(c);
			pbx_builtin_setvar_helper(c->owner, "SETCALLINGPARTY", pbx_strdup(value));
		} else if (!strcasecmp(args, "CalledParty")) {
			if(!value || sccp_strlen_zero(value)) {
				pbx_log(LOG_WARNING, "%s: CHANNEL(%s) needs a value like \"Name\" <number>; not set\n", pbx_channel_name(ast), "CalledParty");
				return -1;
			}
			char *num, *name;
			pbx_callerid_parse((char *) value, &name, &num);
			sccp_channel_set_calledparty(c, name, num);
			sccp_channel_display_callInfo(c);
			pbx_builtin_setvar_helper(c->owner, "SETCALLEDPARTY", pbx_strdup(value));
		} else if (!strcasecmp(args, "OriginalCallingParty")) {
			if(!value || sccp_strlen_zero(value)) {
				pbx_log(LOG_WARNING, "%s: CHANNEL(%s) needs a value like \"Name\" <number>; not set\n", pbx_channel_name(ast), "OriginalCallingParty");
				return -1;
			}
			char *num, *name;
			pbx_callerid_parse((char *) value, &name, &num);
			sccp_channel_set_originalCallingparty(c, name, num);
			sccp_channel_display_callInfo(c);
			pbx_builtin_setvar_helper(c->owner, "SETORIGCALLINGPARTY", pbx_strdup(value));
		} else if (!strcasecmp(args, "OriginalCalledParty")) {
			if(!value || sccp_strlen_zero(value)) {
				pbx_log(LOG_WARNING, "%s: CHANNEL(%s) needs a value like \"Name\" <number>; not set\n", pbx_channel_name(ast), "OriginalCalledParty");
				return -1;
			}
			char *num, *name;
			pbx_callerid_parse((char *) value, &name, &num);
			sccp_channel_set_originalCalledparty(c, name, num);
			sccp_channel_display_callInfo(c);
			pbx_builtin_setvar_helper(c->owner, "SETORIGCALLEDPARTY", pbx_strdup(value));
		} else if (!strcasecmp(args, "microphone")) {
			if (!value || sccp_strlen_zero(value) || !sccp_true(value)) {
				c->setMicrophone(c, FALSE);
			} else {
				c->setMicrophone(c, TRUE);
			}
		} else {
			res = -1;
		}
	} else {
		pbx_log(LOG_WARNING, "CHANNEL() write of SCCP field '%s' on %s, which is not an SCCP channel; not set\n", args, ast ? pbx_channel_name(ast) : "none");
		res = -1;
	}
	return res;
}

/*!
 * \brief Call asterisk automon feature
 * \obsolete, using sccp_manager_action2str method instead
 */
boolean_t sccp_astgenwrap_featureMonitor(const sccp_channel_t * channel)
{
	char featexten[SCCP_MAX_EXTENSION] = "";

	if (iPbx.getFeatureExtension(channel, "automon", featexten) && !sccp_strlen_zero(featexten)) {
		sccp_log((DEBUGCAT_FEATURE))(VERBOSE_PREFIX_3 "%s: sending feature code '%s' to toggle recording\n", channel->designator, featexten);
		struct ast_frame f = { AST_FRAME_DTMF, };
		uint j;

		f.len = 100;
		for (j = 0; j < strlen(featexten); j++) {
			f.subclass.integer = featexten[j];
			ast_queue_frame(channel->owner, &f);
		}
		return TRUE;
	}
	pbx_log(LOG_WARNING, "%s: recording not toggled: no automon feature code is set in features.conf\n", channel->designator);
	return FALSE;
}

#if !defined(AST_DEFAULT_EMULATE_DTMF_DURATION)
#define AST_DEFAULT_EMULATE_DTMF_DURATION 100
#endif
int sccp_wrapper_sendDigits(const sccp_channel_t * channel, const char *digits)
{
	uint8_t maxdigits = AST_MAX_EXTENSION;
	if (!channel || !channel->owner) {
		pbx_log(LOG_WARNING, "SCCP: digits not sent: the call or its Asterisk channel is gone\n");
		return 0;
	}
	if (!digits || sccp_strlen_zero(digits)) {
		sccp_log((DEBUGCAT_PBX))(VERBOSE_PREFIX_3 "%s: no digits to send\n", channel->designator);
		return 0;
	}
	//ast_channel_undefer_dtmf(channel->owner);
	PBX_CHANNEL_TYPE *pbx_channel = channel->owner;
	PBX_FRAME_TYPE f = ast_null_frame;

	sccp_log((DEBUGCAT_HIGH)) (VERBOSE_PREFIX_3 "%s: Sending digits '%s'\n", (char *) channel->currentDeviceId, digits);
	// We don't just call sccp_pbx_senddigit due to potential overhead, and issues with locking
	f.src = "SCCP";
	while (maxdigits-- && *digits != '\0') {
		sccp_log((DEBUGCAT_HIGH)) (VERBOSE_PREFIX_3 "%s: Sending digit %c\n", (char *) channel->currentDeviceId, *digits);

		f.frametype = AST_FRAME_DTMF_END;								// Sending only the dmtf will force asterisk to start with DTMF_BEGIN and schedule the DTMF_END
		f.subclass.integer = *digits;
		// f.len = SCCP_MIN_DTMF_DURATION;
		f.len = AST_DEFAULT_EMULATE_DTMF_DURATION;
		f.src = "SEND DIGIT";
		ast_queue_frame(pbx_channel, &f);
		digits++;
	}
	return 1;
}

int sccp_wrapper_sendDigit(const sccp_channel_t * channel, const char digit)
{
	const char digits[] = { digit, '\0' };
	sccp_log((DEBUGCAT_HIGH)) (VERBOSE_PREFIX_3 "%s: got a single digit '%c' -> '%s'\n", channel->currentDeviceId, digit, digits);
	return sccp_wrapper_sendDigits(channel, digits);
}

static void *sccp_astwrap_doPickupThread(void *data)
{
	PBX_CHANNEL_TYPE *pbx_channel = (PBX_CHANNEL_TYPE *)data;

	if (ast_pickup_call(pbx_channel)) {
		pbx_channel_set_hangupcause(pbx_channel, AST_CAUSE_CALL_REJECTED);
	} else {
		pbx_channel_set_hangupcause(pbx_channel, AST_CAUSE_NORMAL_CLEARING);
	}
	ast_hangup(pbx_channel);
	pbx_channel_unref(pbx_channel);
	pbx_channel = NULL;
	return NULL;
}

static int sccp_astwrap_doPickup(PBX_CHANNEL_TYPE * pbx_channel)
{
	pthread_t threadid;

	if (!pbx_channel) {
		return FALSE;
	}
	pbx_channel_ref(pbx_channel);										// released by sccp_astwrap_doPickupThread
	if (ast_pthread_create_detached_background(&threadid, NULL, sccp_astwrap_doPickupThread, pbx_channel)) {
		pbx_log(LOG_ERROR, "%s: group pickup not done: could not start its thread\n", pbx_channel_name(pbx_channel));
		pbx_channel_unref(pbx_channel);
		return FALSE;
	}
	sccp_log((DEBUGCAT_FEATURE))(VERBOSE_PREFIX_3 "%s: group pickup started\n", pbx_channel_name(pbx_channel));
	return TRUE;
}

void sccp_astgenwrap_set_callgroup(sccp_channel_t *channel, ast_group_t value)
{
	if (channel && channel->owner) {
		ast_channel_callgroup_set(channel->owner, value);
	}
}

void sccp_astgenwrap_set_pickupgroup(sccp_channel_t *channel, ast_group_t value)
{
	if (channel && channel->owner) {
		ast_channel_pickupgroup_set(channel->owner, value);
	}
}

#if CS_AST_HAS_NAMEDGROUP
void sccp_astgenwrap_set_named_callgroups(sccp_channel_t *channel, struct ast_namedgroups *value)
{
	if (channel && channel->owner) {
		ast_channel_named_callgroups_set(channel->owner, value);
	}
}

void sccp_astgenwrap_set_named_pickupgroups(sccp_channel_t *channel, struct ast_namedgroups *value)
{
	if (channel && channel->owner) {
		ast_channel_named_pickupgroups_set(channel->owner, value);
	}
}
#endif

enum ast_pbx_result pbx_pbx_start(PBX_CHANNEL_TYPE * pbx_channel)
{
	enum ast_pbx_result res = AST_PBX_FAILED;

	if (!pbx_channel) {
		pbx_log(LOG_ERROR, "SCCP: pbx_pbx_start() was called without a channel (caller bug)\n");
		return res;
	}

	ast_channel_lock(pbx_channel);
	AUTO_RELEASE(sccp_channel_t, channel , get_sccp_channel_from_pbx_channel(pbx_channel));
	if (channel) {
		// check if the pickup extension was entered
		const char *dialedNumber = iPbx.getChannelExten(channel);
		char pickupexten[SCCP_MAX_EXTENSION];

		if (iPbx.getPickupExtension(channel, pickupexten) && sccp_strequals(dialedNumber, pickupexten)) {
			if (sccp_astwrap_doPickup(pbx_channel)) {
				res = AST_PBX_SUCCESS;
			}
			goto EXIT;
		}
		// channel->hangupRequest = sccp_astgenwrap_dummyHangup;
		channel->hangupRequest = sccp_astgenwrap_carefullHangup;
		res = ast_pbx_start(pbx_channel);								// starting ast_pbx_start with a locked ast_channel so we know exactly where we end up when/if the __ast_pbx_run get started
		if (res == 0) {											// thread started successfully
			do {											// wait for thread to become ready
				pbx_safe_sleep(pbx_channel, 10);
			} while (!pbx_channel_pbx(pbx_channel) && !pbx_check_hangup(pbx_channel));

			if (pbx_channel_pbx(pbx_channel) && !pbx_check_hangup(pbx_channel)) {
				sccp_log(DEBUGCAT_PBX) (VERBOSE_PREFIX_3 "%s: (pbx_pbx_start) autoloop has started, set requestHangup = requestQueueHangup\n", channel->designator);
				channel->isRunningPbxThread = TRUE;
				channel->hangupRequest = sccp_astgenwrap_requestQueueHangup;
			} else {
				pbx_log(LOG_NOTICE, "%s: the dialplan thread ended right after starting (the call is hanging up)\n", channel->designator);
				res = AST_PBX_FAILED;
			}
		}
	}
EXIT:
	ast_channel_unlock(pbx_channel);
	return res;
}

// kate: indent-width 4; replace-tabs off; indent-mode cstyle; auto-insert-doxygen on; line-numbers on; tab-indents on; keep-extra-spaces off;
