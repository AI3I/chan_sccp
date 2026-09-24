/*!
 * \file        sccp_debug.c
 * \brief       SCCP Debug Class
 * \author      Diederik de Groot < ddegroot@users.sourceforge.net >
 * \note        This program is free software and may be modified and distributed under the terms of the GNU Public License.
 *              See the LICENSE file at the top of the source tree.
 * \since       2016-02-02
 */
#include "config.h"
#include "common.h"
#include "sccp_debug.h"

SCCP_FILE_VERSION(__FILE__, "");
const char * SS_Memory_Allocation_Error = "%s: Memory Allocation Error.\n";

/*!
 * \brief SCCP Verbose Level Structure
 */
struct sccp_debug_category const sccp_debug_categories[32] = {
	/* clang-format off */
	{"all",			"all debug levels", 			DEBUGCAT_ALL,},
	{"none",		"all debug levels", 			DEBUGCAT_NONE,},
	{"core",		"core debug level", 			DEBUGCAT_CORE},
	{"hint",		"hint debug level", 			DEBUGCAT_HINT},
	{"rtp",			"rtp debug level", 			DEBUGCAT_RTP},
	{"device",		"device debug level", 			DEBUGCAT_DEVICE},
	{"line",		"line debug level", 			DEBUGCAT_LINE},
	{"action",		"action debug level", 			DEBUGCAT_ACTION},
	{"channel",		"channel debug level", 			DEBUGCAT_CHANNEL},
	{"config",		"config debug level", 			DEBUGCAT_CONFIG},
	{"feature",		"feature debug level", 			DEBUGCAT_FEATURE},
	{"feature_button",	"feature_button debug level",		DEBUGCAT_FEATURE_BUTTON},
	{"softkey",		"softkey debug level", 			DEBUGCAT_SOFTKEY},
	{"indicate",		"indicate debug level",	 		DEBUGCAT_INDICATE},
	{"pbx",			"pbx debug level", 			DEBUGCAT_PBX},
	{"socket",		"socket debug level", 			DEBUGCAT_SOCKET},
	{"mwi",			"mwi debug level", 			DEBUGCAT_MWI},
	{"event",		"event debug level", 			DEBUGCAT_EVENT},
	{"conference",		"conference debug level", 		DEBUGCAT_CONFERENCE},
	{"buttontemplate",	"buttontemplate debug level",		DEBUGCAT_BUTTONTEMPLATE},
	{"speeddial",		"speeddial debug level",		DEBUGCAT_SPEEDDIAL},
	{"codec",		"codec debug level", 			DEBUGCAT_CODEC},
	{"realtime",		"realtime debug level",	 		DEBUGCAT_REALTIME},
	{"callinfo",		"callinfo debug level", 		DEBUGCAT_CALLINFO},
	{"refcount",		"refcount lock debug level", 		DEBUGCAT_REFCOUNT},
	{"message",		"message debug level", 			DEBUGCAT_MESSAGE},
	{"parkinglot",		"parkinglot debug level", 		DEBUGCAT_PARKINGLOT},
	{"webservice",		"webservice debug level", 		DEBUGCAT_WEBSERVICE},
	{"threadpool",		"threadpool debug level",	 	DEBUGCAT_THPOOL},
	{"newcode",		"newcode debug level", 			DEBUGCAT_NEWCODE},
	{"filelinefunc",	"add line/file/function to debug output", DEBUGCAT_FILELINEFUNC},
	{"high",		"high debug level", 			DEBUGCAT_HIGH},
	/* clang-format on */
};

/*!
 * \brief Parse a debug categories line to debug int
 * \param arguments Array of Arguments
 * \param startat Start Point in the Arguments Array
 * \param argc Count of Arguments
 * \param new_debug_value as uint32_t
 * \return new_debug_value as uint32_t
 */
int32_t sccp_parse_debugline(char * arguments[], int startat, int argc, int32_t new_debug_value)
{
	int        argi         = 0;
	uint32_t   i            = 0;
	const char delimiters[] = " ,\t";
	boolean_t  subtract     = 0;

	if (sscanf(arguments[startat], "%d", &new_debug_value) != 1) {
		for (argi = startat; argi < argc; argi++) {
			char * argument = arguments[argi];
			if (!strcasecmp(argument, "none") || !strcasecmp(argument, "off")) {
				new_debug_value = 0;
				break;
			} else if (!strcasecmp(argument, "no")) {
				subtract = 1;
			} else if (!strcasecmp(argument, "all")) {
				new_debug_value = subtract ? 0 : DEBUGCAT_ALL;
			} else {
				// parse comma separated debug_var
				boolean_t matched   = FALSE;
				char *    tokenrest = NULL;
				char *    token     = strtok_r(argument, delimiters, &tokenrest);
				while (token != NULL) {
					// match debug level name to enum
					for (i = 0; i < ARRAY_LEN(sccp_debug_categories); i++) {
						if (strcasecmp(token, sccp_debug_categories[i].key) == 0) {
							if (subtract) {
								if ((new_debug_value & sccp_debug_categories[i].category) == sccp_debug_categories[i].category) {
									new_debug_value -= sccp_debug_categories[i].category;
								}
							} else {
								if ((new_debug_value & sccp_debug_categories[i].category) != sccp_debug_categories[i].category) {
									new_debug_value += sccp_debug_categories[i].category;
								}
							}
							matched = TRUE;
						}
					}
					if (!matched) {
						pbx_log(LOG_NOTICE, "SCCP: '%s' is not a debug category; ignored\n", token);
					}
					token = strtok_r(NULL, delimiters, &tokenrest);
				}
			}
		}
	}
	return new_debug_value;
}

/*!
 * \brief Is name a debug category (or one of the keywords none, off, no, all)
 */
boolean_t sccp_debug_is_category(const char * name)
{
	if (!strcasecmp(name, "none") || !strcasecmp(name, "off") || !strcasecmp(name, "no") || !strcasecmp(name, "all")) {
		return TRUE;
	}
	for (uint32_t i = 0; i < ARRAY_LEN(sccp_debug_categories); i++) {
		if (!strcasecmp(name, sccp_debug_categories[i].key)) {
			return TRUE;
		}
	}
	return FALSE;
}

/*!
 * \brief Write the current debug value to debug categories
 * \param debugvalue DebugValue as uint32_t
 * \return string containing list of categories comma separated (you need to free it)
 */
char * sccp_get_debugcategories(int32_t debugvalue)
{
	char * res    = NULL;
	char * tmpres = NULL;
	size_t size   = 0;

	for (uint32_t i = 2; i < ARRAY_LEN(sccp_debug_categories); ++i) {
		if ((debugvalue & sccp_debug_categories[i].category) == sccp_debug_categories[i].category) {
			size_t new_size = size;

			new_size += strlen(sccp_debug_categories[i].key) + 1 /*sizeof(sep) */ + 1;
			tmpres = (char *)sccp_realloc(res, new_size);
			if (tmpres == NULL) {
				pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
				sccp_free(res);
				return NULL;
			}
			res = tmpres;
			if (size == 0) {
				// strlcpy(res, sccp_debug_categories[i].key, new_size);
				snprintf(res, new_size - 1, "%s", sccp_debug_categories[i].key);
			} else {
				snprintf(res + strlen(res), new_size - 1, ",%s", sccp_debug_categories[i].key);
			}

			size = new_size;
		}
	}

	return res;
}

// kate: indent-width 8; replace-tabs off; indent-mode cstyle; auto-insert-doxygen on; line-numbers on; tab-indents on; keep-extra-spaces off; auto-brackets off;

/* ------------------------------------------------------------------------------------------------ per-device debug - */
#define SCCP_DEBUG_FILTER_MAX_DEVICES 32
typedef struct {
	char device[StationMaxDeviceNameSize];
	char matches[SCCP_DEBUG_FILTER_MAX_MATCHES][96];
	int  nmatches;
} sccp_debug_filter_t;

volatile int                sccp_debug_filter_active = 0;
static sccp_debug_filter_t  debug_filters[SCCP_DEBUG_FILTER_MAX_DEVICES];
static int                  debug_nfilters = 0;
static ast_rwlock_t         debug_filter_lock = AST_RWLOCK_INIT_VALUE;

void sccp_debug_log_filtered(const char * file, int line, const char * function, const char * fmt, ...)
{
	char    buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	boolean_t match = FALSE;
	ast_rwlock_rdlock(&debug_filter_lock);
	for (int f = 0; f < debug_nfilters && !match; f++) {
		for (int i = 0; i < debug_filters[f].nmatches && !match; i++) {
			match = strstr(buf, debug_filters[f].matches[i]) != NULL;
		}
	}
	ast_rwlock_unlock(&debug_filter_lock);
	if (!match) {
		return;
	}
	if ((sccp_globals->debug & DEBUGCAT_FILELINEFUNC) == DEBUGCAT_FILELINEFUNC) {
		ast_log(__LOG_NOTICE, file, line, function, "%s", buf);
	} else {
		ast_log(__LOG_VERBOSE, "", 0, "", "%s", buf);
	}
}

/* mark device (or update its match strings); FALSE when the table is full */
boolean_t sccp_debug_filter_set(const char * device, const char * const matches[], int nmatches)
{
	boolean_t res = FALSE;
	ast_rwlock_wrlock(&debug_filter_lock);
	int f = 0;
	for (f = 0; f < debug_nfilters; f++) {
		if (!strcasecmp(debug_filters[f].device, device)) {
			break;
		}
	}
	if (f < SCCP_DEBUG_FILTER_MAX_DEVICES) {
		sccp_debug_filter_t * filter = &debug_filters[f];
		memset(filter, 0, sizeof(*filter));
		snprintf(filter->device, sizeof(filter->device), "%s", device);
		snprintf(filter->matches[0], sizeof(filter->matches[0]), "%s", device);
		filter->nmatches = 1;
		for (int i = 0; i < nmatches && filter->nmatches < SCCP_DEBUG_FILTER_MAX_MATCHES; i++) {
			snprintf(filter->matches[filter->nmatches++], sizeof(filter->matches[0]), "%s", matches[i]);
		}
		if (f == debug_nfilters) {
			debug_nfilters++;
		}
		res = TRUE;
	}
	sccp_debug_filter_active = debug_nfilters > 0;
	ast_rwlock_unlock(&debug_filter_lock);
	return res;
}

boolean_t sccp_debug_filter_remove(const char * device)
{
	boolean_t res = FALSE;
	ast_rwlock_wrlock(&debug_filter_lock);
	for (int f = 0; f < debug_nfilters; f++) {
		if (!strcasecmp(debug_filters[f].device, device)) {
			debug_filters[f] = debug_filters[--debug_nfilters];
			res = TRUE;
			break;
		}
	}
	sccp_debug_filter_active = debug_nfilters > 0;
	ast_rwlock_unlock(&debug_filter_lock);
	return res;
}

void sccp_debug_filter_clear(void)
{
	ast_rwlock_wrlock(&debug_filter_lock);
	debug_nfilters           = 0;
	sccp_debug_filter_active = 0;
	ast_rwlock_unlock(&debug_filter_lock);
}

boolean_t sccp_debug_filter_has(const char * device)
{
	boolean_t res = FALSE;
	ast_rwlock_rdlock(&debug_filter_lock);
	for (int f = 0; f < debug_nfilters && !res; f++) {
		res = !strcasecmp(debug_filters[f].device, device);
	}
	ast_rwlock_unlock(&debug_filter_lock);
	return res;
}

/* marked devices, comma separated (caller frees), or NULL when none */
char * sccp_debug_filter_devices(void)
{
	char * res = NULL;
	ast_rwlock_rdlock(&debug_filter_lock);
	if (debug_nfilters) {
		size_t size = debug_nfilters * (StationMaxDeviceNameSize + 2) + 1;
		res = (char *)sccp_calloc(1, size);
		for (int f = 0; res && f < debug_nfilters; f++) {
			size_t used = strlen(res);
			snprintf(res + used, size - used, "%s%s", f ? ", " : "", debug_filters[f].device);
		}
	}
	ast_rwlock_unlock(&debug_filter_lock);
	return res;
}
