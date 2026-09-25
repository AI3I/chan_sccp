/*!
 * \file        chan_sccp.c
 * \brief       An implementation of Skinny Client Control Protocol (SCCP)
 * \author      Sergio Chersovani <mlists [at] c-net.it>
 * \brief       Main chan_sccp Class
 * \note        Reworked, but based on chan_sccp code.
 *              The original chan_sccp driver that was made by Zozo which itself was derived from the chan_skinny driver.
 *              Modified by Jan Czmok and Julien Goodwin
 * \note        This program is free software and may be modified and distributed under the terms of the GNU General Public License, version 2 or (at your option) any later version.
 *              See the LICENSE file at the top of the source tree.
 * \remarks
 * Purpose:     This source file should be used only for asterisk module related content.
 * When to use: Methods communicating to asterisk about module initialization, status, destruction
 * Relations:   Main hub for all other sourcefiles.
 *
 */
#include "config.h"
#include "common.h"
#include "chan_sccp.h"
#include "sccp_channel.h"
#include "sccp_config.h"
#include "sccp_device.h"
#include "sccp_globals.h"
#include "sccp_line.h"
#include "sccp_linedevice.h"
#include "sccp_netsock.h"
#include "sccp_mwi.h"
#include "sccp_session.h"
#include "sccp_utils.h"
#include "sccp_hint.h"
#include "sccp_conference.h"
#include "revision.h"
#ifdef CS_DEVSTATE_FEATURE
#include "sccp_devstate.h"
#endif
#include "sccp_management.h"
#include "sccp_threadpool.h"
#include "sccp_session.h"
#include <signal.h>

SCCP_FILE_VERSION(__FILE__, "");

static PBX_FRAME_TYPE sccp_null_frame;

int load_config(void)
{
	GLOB(mwiMonitorThread) = AST_PTHREADT_NULL;

	memset(&GLOB(bindaddr), 0, sizeof(GLOB(bindaddr)));
#ifdef HAVE_LIBSSL
	memset(&GLOB(secbindaddr), 0, sizeof(GLOB(secbindaddr)));
#endif
	GLOB(allowAnonymous) = TRUE;

#if defined(SCCP_LITTLE_ENDIAN) && defined(SCCP_BIG_ENDIAN)
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_2 "platform byte order: little and big endian\n");
#elif defined(SCCP_LITTLE_ENDIAN)
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_2 "platform byte order: little endian\n");
#else
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_2 "platform byte order: big endian\n");
#endif
	if(sccp_config_getConfig(TRUE, "sccp.conf") > CONFIG_STATUS_FILE_OK) {
		sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_3 "SCCP: sccp.conf could not be loaded\n");
		return FALSE;
	}

	if (!sccp_config_general(SCCP_CONFIG_READINITIAL)) {
		sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_3 "SCCP: the [general] section of sccp.conf could not be applied\n");
		return FALSE;
	}
	sccp_config_readDevicesLines(SCCP_CONFIG_READINITIAL);
	return TRUE;
}

boolean_t sccp_prePBXLoad(void)
{
	/* no sccp_log() before this point: it reads sccp_globals->debug */
	sccp_globals = (struct sccp_global_vars *) sccp_calloc(sizeof *sccp_globals, 1);
	if (!sccp_globals) {
		pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
		return FALSE;
	}

	memset(&sccp_null_frame, 0, sizeof(sccp_null_frame));
	GLOB(debug) = DEBUGCAT_CORE;

	pbx_rwlock_init(&GLOB(lock));
#ifndef SCCP_ATOMIC
	pbx_mutex_init(&GLOB(usecnt_lock));
#endif
	sccp_refcount_init();

	SCCP_RWLIST_HEAD_INIT(&GLOB(sessions));
	SCCP_RWLIST_HEAD_INIT(&GLOB(devices));
	SCCP_RWLIST_HEAD_INIT(&GLOB(lines));

	GLOB(general_threadpool) = sccp_threadpool_init(THREADPOOL_MIN_SIZE);
	if (!GLOB(general_threadpool)) {
		SCCP_RWLIST_HEAD_DESTROY(&GLOB(lines));
		SCCP_RWLIST_HEAD_DESTROY(&GLOB(devices));
		SCCP_RWLIST_HEAD_DESTROY(&GLOB(sessions));
		sccp_refcount_destroy();
#ifndef SCCP_ATOMIC
		pbx_mutex_destroy(&GLOB(usecnt_lock));
#endif
		pbx_rwlock_destroy(&GLOB(lock));
		sccp_free(sccp_globals);
		sccp_globals = NULL;
		return FALSE;
	}

	sccp_event_module_start();
	iVoicemail.startModule();
#if defined(CS_DEVSTATE_FEATURE)
	sccp_devstate_module_start();
#endif
	sccp_hint_module_start();
	sccp_manager_module_start();
#ifdef CS_SCCP_CONFERENCE
	sccp_conference_module_start();
#endif
	sccp_event_subscribe(SCCP_EVENT_FEATURE_CHANGED, sccp_device_featureChangedDisplay, TRUE);
	sccp_event_subscribe(SCCP_EVENT_FEATURE_CHANGED, sccp_util_featureStorageBackend, TRUE);

	memset(&GLOB(bindaddr), 0, sizeof(GLOB(bindaddr)));
	GLOB(bindaddr).ss_family = AF_INET;
	((struct sockaddr_in *) &GLOB(bindaddr))->sin_port = DEFAULT_SCCP_PORT;

#ifdef HAVE_LIBSSL
	memset(&GLOB(secbindaddr), 0, sizeof(GLOB(secbindaddr)));
	GLOB(secbindaddr).ss_family = AF_INET;
	((struct sockaddr_in *)&GLOB(secbindaddr))->sin_port = DEFAULT_SCCP_SECURE_PORT;
#endif
	GLOB(externrefresh) = 60;
	GLOB(keepalive) = SCCP_MIN_KEEPALIVE;

	GLOB(firstdigittimeout) = 16;
	GLOB(digittimeout) = 8;

	GLOB(debug) = 1;
	GLOB(sccp_tos) = (0x68 & 0xff);
	GLOB(audio_tos) = (0xB8 & 0xff);
	GLOB(video_tos) = (0x88 & 0xff);
	GLOB(sccp_cos) = 4;
	GLOB(audio_cos) = 6;
	GLOB(video_cos) = 5;
	GLOB(echocancel) = TRUE;
	GLOB(silencesuppression) = TRUE;
	GLOB(dndFeature) = TRUE;
	GLOB(autoanswer_tone) = SKINNY_TONE_ZIP;
	GLOB(remotehangup_tone) = SKINNY_TONE_ZIP;
	GLOB(callwaiting_tone) = SKINNY_TONE_CALLWAITINGTONE;
	GLOB(privacy) = TRUE;
	GLOB(mwilamp) = SKINNY_LAMP_ON;
	GLOB(ringtype) = SKINNY_RINGTYPE_OUTSIDE;
	GLOB(amaflags) = pbx_channel_string2amaflag("documentation");
	GLOB(callanswerorder) = SCCP_ANSWER_OLDEST_FIRST;
	GLOB(earlyrtp) = TRUE;
	GLOB(global_jbconf) = (struct ast_jb_conf *) sccp_calloc(sizeof(struct ast_jb_conf),1);
	if (GLOB(global_jbconf)) {
		GLOB(global_jbconf)->max_size = -1;
		GLOB(global_jbconf)->resync_threshold = -1;
#ifdef CS_AST_JB_TARGETEXTRA
		GLOB(global_jbconf)->target_extra = -1;
#endif
	}
	sccp_create_hotline();
	return TRUE;
}

boolean_t sccp_postPBX_load(void)
{
	pbx_rwlock_wrlock(&GLOB(lock));

	/* SCCP_VERSIONSTR is also the module description; revision and build details go in backtraces only */
#ifdef VCS_SHORT_HASH
#if VCS_WC_MODIFIED
	snprintf(SCCP_REVISIONSTR, sizeof(SCCP_REVISIONSTR), "%sM", VCS_SHORT_HASH);
#else
	snprintf(SCCP_REVISIONSTR, sizeof(SCCP_REVISIONSTR), "%s", VCS_SHORT_HASH);
#endif
#else
	snprintf(SCCP_REVISIONSTR, sizeof(SCCP_REVISIONSTR), "%s", SCCP_REVISION);
#endif
	snprintf(SCCP_VERSIONSTR, sizeof(SCCP_VERSIONSTR), "Skinny Client Control Protocol (SCCP) %s", SCCP_VERSION);

	GLOB(module_running) = TRUE;
	pbx_rwlock_unlock(&GLOB(lock));

	if(!GLOB(srvcontexts[SCCP_SERVERCONTEXT_TCP])) {
		GLOB(srvcontexts[SCCP_SERVERCONTEXT_TCP]) = sccp_servercontext_create(&GLOB(bindaddr), SCCP_SERVERCONTEXT_TCP);
		if(GLOB(srvcontexts[SCCP_SERVERCONTEXT_TCP])) {
			sccp_log((DEBUGCAT_CORE))(VERBOSE_PREFIX_3 "bindaddr %s\n", sccp_netsock_stringify(sccp_servercontext_getBoundAddr(GLOB(srvcontexts[SCCP_SERVERCONTEXT_TCP]))));
		}
	}
#ifdef HAVE_LIBSSL
	if(!GLOB(srvcontexts[SCCP_SERVERCONTEXT_TLS])) {
		GLOB(srvcontexts[SCCP_SERVERCONTEXT_TLS]) = sccp_servercontext_create(&GLOB(secbindaddr), SCCP_SERVERCONTEXT_TLS);
		if(GLOB(srvcontexts[SCCP_SERVERCONTEXT_TLS])) {
			sccp_log((DEBUGCAT_CORE))(VERBOSE_PREFIX_3 "secbindaddr %s\n", sccp_netsock_stringify(sccp_servercontext_getBoundAddr(GLOB(srvcontexts[SCCP_SERVERCONTEXT_TLS]))));
		}
	}
#endif
	return TRUE ;
}

/* PBX Independent Function to be called before unloading the module */
int sccp_preUnload(void)
{
	sccp_device_t *d = NULL;
	sccp_line_t *l = NULL;

	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_1 "SCCP: unloading module\n");

	/* copy some of the required global variables */
	pbx_rwlock_wrlock(&GLOB(lock));
	GLOB(module_running) = FALSE;
	pbx_rwlock_unlock(&GLOB(lock));
	sccp_threadpool_stop(GLOB(general_threadpool));

	sccp_event_unsubscribe(SCCP_EVENT_FEATURE_CHANGED, sccp_device_featureChangedDisplay);
	sccp_event_unsubscribe(SCCP_EVENT_FEATURE_CHANGED, sccp_util_featureStorageBackend);

	/* close accept thread by shutdown the socket descriptor read side -> interrupt polling and break accept loop */
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_2 "SCCP: closing the listening socket\n");
	sccp_servercontext_stopListening(GLOB(srvcontexts[SCCP_SERVERCONTEXT_TCP]));
#if HAVE_LIBSSL
	sccp_servercontext_stopListening(GLOB(srvcontexts[SCCP_SERVERCONTEXT_TLS]));
#endif
	sccp_hint_module_stop();

	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_2 "SCCP: hanging up open calls\n");

	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: removing devices\n");
	SCCP_RWLIST_TRAVERSE_SAFE_BEGIN(&GLOB(devices), d, list) {
		sccp_log((DEBUGCAT_CORE + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "SCCP: removing device %s\n", d->id);
		d->realtime = TRUE;
		sccp_dev_clean_restart(d, TRUE);
	}
	SCCP_RWLIST_TRAVERSE_SAFE_END;

	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: removing lines\n");
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_4 "SCCP: removing hotline\n");
	if (GLOB(hotline)) {
		if (GLOB(hotline)->line) {
			sccp_line_removeFromGlobals(GLOB(hotline)->line);
			if (GLOB(hotline)->line) {
				sccp_line_release(&GLOB(hotline)->line);					// explicit release of hotline->line
			}
		}
		sccp_free(GLOB(hotline));
	}

	SCCP_RWLIST_TRAVERSE_SAFE_BEGIN(&GLOB(lines), l, list) {
		sccp_log((DEBUGCAT_CORE + DEBUGCAT_LINE)) (VERBOSE_PREFIX_4 "SCCP: removing line %s\n", l->name);
		sccp_line_clean(l, TRUE);
	}
	SCCP_RWLIST_TRAVERSE_SAFE_END;
	iVoicemail.stopModule();
	usleep(100);

	sccp_event_module_stop();

	sccp_session_terminateAll();
	/* Producers are stopped; callbacks must finish before their services go away. */
	if (!sccp_threadpool_destroy(GLOB(general_threadpool))) {
		pbx_log(LOG_ERROR, "SCCP: unload stopped: the thread-pool workers did not finish; the module stays loaded\n");
		return -1;
	}
	GLOB(general_threadpool) = NULL;
	if (SCCP_RWLIST_EMPTY(&GLOB(devices))) {
		SCCP_RWLIST_HEAD_DESTROY(&GLOB(devices));
	}
	if (SCCP_RWLIST_EMPTY(&GLOB(lines))) {
		SCCP_RWLIST_HEAD_DESTROY(&GLOB(lines));
	}
	sccp_manager_module_stop();
#ifdef CS_DEVSTATE_FEATURE
	sccp_devstate_module_stop();
#endif
#ifdef CS_SCCP_CONFERENCE
	sccp_conference_module_stop();
#endif
	sccp_softkey_clear();
	sccp_refcount_destroy();

	if (GLOB(config_file_name)) {
		sccp_free(GLOB(config_file_name));
	}
	if (GLOB(global_jbconf)) {
		sccp_free(GLOB(global_jbconf));
	}
	if (GLOB(ha)) {
		sccp_free_ha(GLOB(ha));
	}
	if (GLOB(localaddr)) {
		sccp_free_ha(GLOB(localaddr));
	}
	if(GLOB(cfg)) {
		pbx_config_destroy(GLOB(cfg));
		GLOB(cfg) = NULL;
	}
	sccp_config_cleanup_dynamically_allocated_memory(sccp_globals, SCCP_CONFIG_GLOBAL_SEGMENT);
	sccp_servercontext_destroy(GLOB(srvcontexts[SCCP_SERVERCONTEXT_TCP]));
#if HAVE_LIBSSL
	sccp_servercontext_destroy(GLOB(srvcontexts[SCCP_SERVERCONTEXT_TLS]));
#endif

#ifndef SCCP_ATOMIC
	pbx_mutex_destroy(&GLOB(usecnt_lock));
#endif
	pbx_rwlock_destroy(&GLOB(lock));
	return 0;
}

int sccp_reload(void)
{
	sccp_readingtype_t readingtype = 0;
	int returnval = 0;

	pbx_rwlock_wrlock(&GLOB(lock));
	if (GLOB(reload_in_progress) == TRUE) {
		pbx_log(LOG_NOTICE, "SCCP: reload ignored: another reload is still in progress\n");
		returnval = 4;
		goto EXIT;
	}

	sccp_config_file_status_t cfg = sccp_config_getConfig(FALSE, NULL);

	switch (cfg) {
		case CONFIG_STATUS_FILE_NOT_CHANGED:
			returnval = 0;
			break;
		case CONFIG_STATUS_FILE_OK:
			sccp_log(DEBUGCAT_CORE)(VERBOSE_PREFIX_3 "SCCP: reloading configuration\n");
			readingtype = SCCP_CONFIG_READRELOAD;
			GLOB(reload_in_progress) = TRUE;
			if (!sccp_config_general(readingtype)) {
				pbx_log(LOG_ERROR, "SCCP: reload failed while applying [general] (see the previous message)\n");
				returnval = 3;
				break;
			}
			if (!sccp_config_readDevicesLines(readingtype)) {
				pbx_log(LOG_ERROR, "SCCP: reload failed while applying devices and lines (see the previous message)\n");
				returnval = 3;
				break;
			}
			if(GLOB(srvcontexts[SCCP_SERVERCONTEXT_TCP])) {
				returnval = sccp_servercontext_reload(GLOB(srvcontexts[SCCP_SERVERCONTEXT_TCP]), &GLOB(bindaddr)) ? 0 : 3;
			}
#ifdef HAVE_LIBSSL
			if(GLOB(srvcontexts[SCCP_SERVERCONTEXT_TLS])) {
				returnval = sccp_servercontext_reload(GLOB(srvcontexts[SCCP_SERVERCONTEXT_TLS]), &GLOB(secbindaddr)) ? 0 : 3;
			}
#endif
			break;
		case CONFIG_STATUS_FILE_OLD:
			pbx_log(LOG_ERROR, "SCCP: reload of '%s' aborted; devices and lines keep their current settings\n", GLOB(config_file_name));
			returnval = 3;
			break;
		case CONFIG_STATUS_FILE_NOT_SCCP:
			pbx_log(LOG_ERROR, "SCCP: reload of '%s' aborted; devices and lines keep their current settings\n", GLOB(config_file_name));
			returnval = 3;
			break;
		case CONFIG_STATUS_FILE_NOT_FOUND:
			pbx_log(LOG_ERROR, "SCCP: reload of '%s' aborted; devices and lines keep their current settings\n", GLOB(config_file_name));
			returnval = 3;
			break;
		case CONFIG_STATUS_FILE_INVALID:
			pbx_log(LOG_ERROR, "SCCP: reload of '%s' aborted; devices and lines keep their current settings\n", GLOB(config_file_name));
			returnval = 3;
			break;
	}
EXIT:
	GLOB(reload_in_progress) = FALSE;
	pbx_rwlock_unlock(&GLOB(lock));
	return returnval;
}
