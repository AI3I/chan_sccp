/*!
 * \file        ast120.h
 * \brief       SCCP PBX Asterisk Header
 * \author      Marcello Ceshia
 * \author      Diederik de Groot <ddegroot [at] users.sourceforge.net>
 * \note        This program is free software and may be modified and distributed under the terms of the GNU Public License.
 *              See the LICENSE file at the top of the source tree.
 */
#pragma once

#include "config.h"
#include <asterisk/format_compatibility.h>

#include "pbx_impl/ast_announce/ast_announce.h"

#undef pbx_channel_ref
#define pbx_channel_ref ast_channel_ref
#undef pbx_channel_unref
#define pbx_channel_unref ast_channel_unref
#define sccp_sched_context_destroy sched_context_destroy

#define PBX_ENDPOINT_TYPE struct ast_endpoint
#define PBX_EVENT_SUBSCRIPTION struct stasis_subscription

typedef struct ast_format_cap ast_format_t;

int sccp_wrapper_asterisk_set_rtp_peer(PBX_CHANNEL_TYPE * ast, PBX_RTP_TYPE * rtp, PBX_RTP_TYPE * vrtp, PBX_RTP_TYPE * trtp, int codecs, int nat_active);
const char *pbx_getformatname(const struct ast_format *format);
const char *pbx_getformatname_multiple(char *buf, size_t size, struct ast_format_cap *format);

#define pbx_channel_name(x) ast_channel_name(x)

#undef CS_BRIDGEPEERNAME
#undef pbx_channel_uniqueid
#undef pbx_channel_flags
#undef pbx_channel_call_forward
#undef pbx_channel_appl
#undef pbx_channel_state
#undef pbx_channel_pbx
#undef pbx_channel_hangupcause
#undef pbx_channel_set_hangupcause
#undef pbx_channel_softhangup
#undef pbx_channel_context
#undef pbx_channel_nativeformats
#undef pbx_channel_exten
#undef pbx_channel_priority
#undef pbx_channel_macroexten
#undef pbx_channel_macrocontext
#undef pbx_channel_dialcontext
#undef pbx_channel_callgroup
#undef pbx_channel_masq
#undef pbx_channel_setwhentohangup_tv
#undef pbx_channel_blocker
#undef pbx_channel_blockproc
#undef pbx_channel_tech
#undef pbx_channel_bridge
#undef pbx_channel_set_bridge
#undef pbx_channel_language
#undef pbx_channel_language_set
#undef pbx_channel_cdr
#undef pbx_channel_call_forward_set
#undef pbx_channel_varshead
#undef pbx_channel_redirecting_effective_from
#undef pbx_channel_redirecting_effective_to
#undef pbx_channel_redirecting_effective_orig
#undef pbx_channel_connected_id
#undef pbx_channel_connected_source
#undef pbx_channel_monitor
#undef pbx_channel_string2amaflag
#undef pbx_channel_amaflags2string
#undef pbx_event_subscribe
#undef pbx_event_unsubscribe
#undef pbx_bridge_destroy
#undef pbx_bridge_new
#undef pbx_bridge_change_state

#define CS_BRIDGEPEERNAME "DIALEDPEERNAME"
#define pbx_channel_uniqueid(_a) ast_channel_uniqueid(_a)
#define pbx_channel_flags(_a) ast_channel_flags(_a)
#define pbx_channel_call_forward(_a) ast_channel_call_forward(_a)
#define pbx_channel_appl(_a) ast_channel_appl(_a)
#define pbx_channel_state(_a) ast_channel_state(_a)
#define pbx_channel_pbx(_a) ast_channel_pbx(_a)
#define pbx_channel_hangupcause(_a) ast_channel_hangupcause(_a)
#define pbx_channel_set_hangupcause(_a, _b) ast_channel_hangupcause_set(_a, _b)
#define pbx_channel_softhangup(_a) ast_channel_softhangup_internal_flag(_a)
#define pbx_channel_set_hangupcause(_a, _b) ast_channel_hangupcause_set(_a, _b)
#define pbx_channel_context(_a) ast_channel_context(_a)
#define pbx_channel_nativeformats(_a) ast_channel_nativeformats(_a)
#define pbx_channel_exten(_a) ast_channel_exten(_a)
#define pbx_channel_priority(_a) ast_channel_priority(_a)
#define pbx_channel_macroexten(_a) ast_channel_macroexten(_a)
#define pbx_channel_macrocontext(_a) ast_channel_macrocontext(_a)
#define pbx_channel_dialcontext(_a) ast_channel_dialcontext(_a)
#define pbx_channel_callgroup(_a) ast_channel_callgroup(_a)
#define pbx_channel_masq(_a) ast_channel_masq(_a)
#define pbx_channel_setwhentohangup_tv(_a, _b) ast_channel_setwhentohangup_tv(_a, _b)
#define pbx_channel_blocker(_a) ast_channel_blocker(_a)
#define pbx_channel_blockproc(_a) ast_channel_blockproc(_a)
#define pbx_channel_tech(_a) ast_channel_tech(_a)
#define pbx_channel_bridge(_a) ast_channel_bridge(_a)
#define pbx_channel_set_bridge(_a, _b) ast_channel_internal_bridge_set(_a, _b)
#define pbx_channel_language(_a) ast_channel_language(_a)
#define pbx_channel_language_set(_a,_b) ast_channel_language_set(_a,_b)
#define pbx_channel_cdr(_a) ast_channel_cdr(_a)
#define pbx_channel_call_forward_set ast_channel_call_forward_set
#define pbx_channel_varshead(_a) ast_channel_varshead(_a)
#define pbx_channel_redirecting_effective_from(_a) ast_channel_redirecting_effective_from(_a)
#define pbx_channel_redirecting_effective_to(_a) ast_channel_redirecting_effective_to(_a)
#define pbx_channel_redirecting_effective_orig(_a) ast_channel_redirecting_effective_orig(_a)
#define pbx_channel_connected_id(_a) ast_channel_connected(_a)->id
#define pbx_channel_connected_source(_a) ast_channel_connected(_a)->source
#define pbx_channel_monitor(_a) ast_channel_monitor(_a)
#define pbx_channel_string2amaflag(_a) ast_channel_string2amaflag(_a)
#define pbx_channel_amaflags2string(_a) ast_channel_amaflags2string(_a)
#define pbx_event_subscribe(_a) _a = stasis_subscribe(_a)
#define pbx_event_unsubscribe(_a) _a = stasis_unsubscribe(_a)
#define pbx_bridge_destroy(_x, _y) ast_bridge_destroy(_x, _y)
#define pbx_bridge_new(_a, _b, _c, _d, _e) ast_bridge_base_new(_a, _b, _c, _d, _e)
#define AST_BRIDGE_CHANNEL_STATE_WAIT BRIDGE_CHANNEL_STATE_WAIT
#define pbx_bridge_change_state(_a, _b) ((_a)->state) = (_b)

int pbx_manager_register(const char *action, int authority, int (*func) (struct mansession * s, const struct message * m), const char *synopsis, const char *description);

#undef CS_AST_CHANNEL_PVT
#undef CS_AST_CHANNEL_PVT_TYPE
#undef CS_AST_CHANNEL_PVT_CMP_TYPE

#define CS_AST_CHANNEL_PVT(_a) ((sccp_channel_t*)ast_channel_tech_pvt(_a))
#define CS_AST_CHANNEL_PVT_TYPE(_a) ast_channel_tech(_a)->type
#define CS_AST_CHANNEL_PVT_CMP_TYPE(_a,_b) !strncasecmp(CS_AST_CHANNEL_PVT_TYPE(_a), _b, strlen(_b))

#define NEWCONST const
#define OLDCONST

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#define CLI_AMI_OUTPUT(fd, s, ...) ({ 										\
	if (NULL != (s)) {											\
		astman_append((s), __VA_ARGS__);								\
		local_line_total++;										\
	} else {												\
		ast_cli((fd), __VA_ARGS__);									\
	}													\
})

#	define CLI_AMI_OUTPUT_PARAM(param, width, fmt, ...)                                                                                                                                                                    \
		({                                                                                                                                                                                                              \
			if (NULL != (s)) {                                                                                                                                                                                      \
				char camelParam[] = param;                                                                                                                                                                      \
				sccp_camelcase(camelParam);                                                                                                                                                                     \
				astman_append((s), "%s: " fmt "\r\n", (camelParam), __VA_ARGS__);                                                                                                                               \
				local_line_total++;                                                                                                                                                                             \
			} else {                                                                                                                                                                                                \
				ast_cli((fd), "  %-*s " fmt "\n", (width) + 1, param ":", __VA_ARGS__);                                                                                                              \
			}                                                                                                                                                                                                       \
		})

#	define CLI_AMI_OUTPUT_BOOL(param, width, value)                                                                                                                                                                        \
		({                                                                                                                                                                                                              \
			if (NULL != (s)) {                                                                                                                                                                                      \
				char camelParam[] = param;                                                                                                                                                                      \
				sccp_camelcase(camelParam);                                                                                                                                                                     \
				astman_append((s), "%s: %s\r\n", (camelParam), ((value) ? "on" : "off"));                                                                                                                       \
				local_line_total++;                                                                                                                                                                             \
			} else {                                                                                                                                                                                                \
				ast_cli((fd), "  %-*s %s\n", (width) + 1, param ":", ((value) ? "on" : "off"));                                                                                                      \
			}                                                                                                                                                                                                       \
		})

#	define CLI_AMI_OUTPUT_YES_NO(param, width, value)                                                                                                                                                                      \
		({                                                                                                                                                                                                              \
			if (NULL != (s)) {                                                                                                                                                                                      \
				char camelParam[] = param;                                                                                                                                                                      \
				sccp_camelcase(camelParam);                                                                                                                                                                     \
				astman_append((s), "%s: %s\r\n", (camelParam), ((value) ? "yes" : "no"));                                                                                                                       \
				local_line_total++;                                                                                                                                                                             \
			} else {                                                                                                                                                                                                \
				ast_cli((fd), "  %-*s %s\n", (width) + 1, param ":", ((value) ? "yes" : "no"));                                                                                                      \
			}                                                                                                                                                                                                       \
		})

#	define RESULT_ERROR_REPORTED 100
#	define RESULT_RESPONDED      101

#	define SCCP_AMI_LIST_BY_HANDLER 2
#	define CLI_AMI_LIST_START(s, m, _ACTION)                                                                \
		({                                                                                              \
			if (NULL != (s)) {                                                                      \
				astman_send_listack((s), (m), _ACTION " list will follow", "start");            \
			}                                                                                       \
		})

#	define CLI_AMI_RETURN_ERROR(fd, s, m, fmt, ...)                                                        \
		({                                                                                              \
			char _cli_ami_error[512];                                                               \
			snprintf(_cli_ami_error, sizeof(_cli_ami_error), (fmt), __VA_ARGS__);                   \
			size_t _cli_ami_len = strlen(_cli_ami_error);                                           \
			while (_cli_ami_len && _cli_ami_error[_cli_ami_len - 1] == '\n') {                      \
				_cli_ami_error[--_cli_ami_len] = '\0';                                          \
			}                                                                                       \
			if (NULL != (s)) {                                                                      \
				astman_send_error((s), (m), _cli_ami_error);                                    \
			} else {                                                                                \
				ast_cli((fd), "%s\n", _cli_ami_error);                                          \
			}                                                                                       \
			return RESULT_ERROR_REPORTED;                                                           \
		})

#	define CLI_AMI_RETURN_DONE(fd, s, m, fmt, ...)                                                         \
		({                                                                                              \
			char _cli_ami_done[512];                                                                \
			snprintf(_cli_ami_done, sizeof(_cli_ami_done), (fmt), __VA_ARGS__);                    \
			if (NULL != (s)) {                                                                      \
				const char * _cli_ami_id = astman_get_header((m), "ActionID");                   \
				astman_append((s), "Response: Success\r\n");                                    \
				if (!ast_strlen_zero(_cli_ami_id)) {                                            \
					astman_append((s), "ActionID: %s\r\n", _cli_ami_id);                    \
				}                                                                               \
				astman_append((s), "Message: %s\r\n", _cli_ami_done);                           \
				return RESULT_RESPONDED;                                                        \
			}                                                                                       \
			ast_cli((fd), "%s\n", _cli_ami_done);                                                   \
			return RESULT_SUCCESS;                                                                  \
		})

#	define SCCP_AMI_ACTION(_FUNCTION_NAME, _CALLED_FUNCTION, _ACTION, _EVENTLIST, ...)                     \
		static int manager_##_FUNCTION_NAME(struct mansession * s, const struct message * m)           \
		{                                                                                               \
			static const char * const template[] = { __VA_ARGS__ };                                 \
			char * arguments[ARRAY_LEN(template)];                                                  \
			int argc = 0;                                                                           \
			for (size_t x = 0; x < ARRAY_LEN(template); x++) {                                      \
				if (template[x][0] == '$') {                                                    \
					arguments[x] = (char *)astman_get_header(m, (char *)template[x] + 1);           \
					if (!ast_strlen_zero(arguments[x])) {                                   \
						argc = (int)x + 1;                                              \
					}                                                                       \
				} else {                                                                        \
					arguments[x] = (char *)template[x];                                     \
					argc = (int)x + 1;                                                      \
				}                                                                               \
			}                                                                                       \
			const char * id = astman_get_header(m, "ActionID");                                     \
			sccp_cli_totals_t totals = { 0 };                                                       \
			if ((_EVENTLIST) == TRUE) {                                                             \
				astman_send_listack(s, m, _ACTION " list will follow", "start");                \
			}                                                                                       \
			int res = _CALLED_FUNCTION(-1, &totals, s, m, argc, arguments);                         \
			if ((_EVENTLIST) == SCCP_AMI_LIST_BY_HANDLER && res != RESULT_SUCCESS) {                 \
				/* the handler reported the error before starting its list */                  \
				if (res == RESULT_SHOWUSAGE) {                                                  \
					astman_send_error(s, m, "Missing or invalid arguments; see 'manager show command " _ACTION "'"); \
				}                                                                               \
				return 0;                                                                       \
			}                                                                                       \
			if ((_EVENTLIST) != FALSE) {                                                            \
				astman_append(s, "Event: " _ACTION "Complete\r\nEventList: Complete\r\n"        \
						 "ListItems: %d\r\nListTableItems: %d\r\n",                       \
					      totals.lines, totals.tables);                                     \
				if (!ast_strlen_zero(id)) {                                                     \
					astman_append(s, "ActionID: %s\r\n", id);                               \
				}                                                                               \
				astman_append(s, "\r\n");                                                       \
				if (res == RESULT_SHOWUSAGE) {                                                  \
					astman_send_error(s, m, "Missing or invalid arguments; see 'manager show command " _ACTION "'"); \
				}                                                                               \
				return 0;                                                                       \
			}                                                                                       \
			switch (res) {                                                                          \
				case RESULT_SUCCESS: astman_send_ack(s, m, NULL); break;                        \
				case RESULT_RESPONDED: astman_append(s, "\r\n"); break;                         \
				case RESULT_ERROR_REPORTED: break;                                              \
				case RESULT_SHOWUSAGE:                                                          \
					astman_send_error(s, m, "Missing or invalid arguments; see 'manager show command " _ACTION "'"); \
					break;                                                                  \
				default: astman_send_error(s, m, _ACTION " failed"); break;                     \
			}                                                                                       \
			return 0;                                                                               \
		}

#	define CLI_AMI_ENTRY(_FUNCTION_NAME, _CALLED_FUNCTION, _DESCR, _USAGE, _COMPLETER_REPEAT, _EVENTLIST)   \
		static char * cli_##_FUNCTION_NAME(struct ast_cli_entry * e, int cmd, struct ast_cli_args * a) \
		{                                                                                               \
			const char * cli_command[] = { CLI_COMMAND, NULL };                                     \
			static sccp_cli_completer_t cli_complete[] = { CLI_COMPLETE };                          \
			static char command[80] = "";                                                           \
			if (cmd == CLI_INIT) {                                                                  \
				ast_join(command, sizeof(command), cli_command);                                \
				e->command = command;                                                           \
				e->usage = _USAGE;                                                              \
				return NULL;                                                                    \
			}                                                                                       \
			if (cmd == CLI_GENERATE) {                                                              \
				for (uint8_t completer = 0; completer < ARRAY_LEN(cli_complete); completer++) { \
					if ((unsigned)a->pos == (completer + ARRAY_LEN(cli_command) - 1) || (_COMPLETER_REPEAT)) { \
						return sccp_exec_completer(cli_complete[completer], (char *)a->line, (char *)a->word, a->pos, a->n); \
					}                                                                       \
				}                                                                               \
				return NULL;                                                                    \
			}                                                                                       \
			if (a->argc < (int)(ARRAY_LEN(cli_command) - 1)) {                                      \
				return CLI_SHOWUSAGE;                                                           \
			}                                                                                       \
			switch ((_CALLED_FUNCTION)(a->fd, NULL, NULL, NULL, a->argc, (char **)a->argv)) {       \
				case RESULT_SUCCESS:                                                            \
				case RESULT_RESPONDED: return CLI_SUCCESS;                                      \
				case RESULT_SHOWUSAGE: return CLI_SHOWUSAGE;                                    \
				default: return CLI_FAILURE;                                                    \
			}                                                                                       \
		}

#define CLI_ENTRY(_FUNCTION_NAME,_CALLED_FUNCTION,_DESCR,_USAGE, _COMPLETER_REPEAT)				\
	static char *_FUNCTION_NAME(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a) {			\
		const char *cli_command[] = { CLI_COMMAND, NULL };						\
		static sccp_cli_completer_t cli_complete[] = { CLI_COMPLETE };					\
		static char command[80]="";									\
		if (cmd == CLI_INIT) {										\
			ast_join(command, sizeof(command), cli_command);					\
			e->command = command;									\
			e->usage = _USAGE;									\
			return NULL;										\
		}												\
		if (cmd == CLI_GENERATE) {									\
                        uint8_t completer;									\
			for (completer=0; completer<ARRAY_LEN(cli_complete); completer++) {			\
				if ((unsigned)a->pos == (completer + ARRAY_LEN(cli_command) -1) || (_COMPLETER_REPEAT) ) {\
					return sccp_exec_completer(cli_complete[completer], (char *)a->line, (char *)a->word, a->pos, a->n);\
				}										\
			}											\
			return NULL;										\
		}												\
		if (a->argc < (int)(ARRAY_LEN(cli_command)-1)) {						\
			return CLI_SHOWUSAGE;									\
		}												\
		switch ((_CALLED_FUNCTION)(a->fd, a->argc, (char **) a->argv)) {				\
			case RESULT_SUCCESS: return CLI_SUCCESS;						\
			case RESULT_FAILURE: return CLI_FAILURE;						\
			case RESULT_SHOWUSAGE: return CLI_SHOWUSAGE;						\
			default: return CLI_FAILURE;								\
		}												\
	};
#endif														/* DOXYGEN_SHOULD_SKIP_THIS */
