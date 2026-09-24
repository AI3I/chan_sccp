/*!
 * \file        sccp_config.c
 * \brief       SCCP Config Class
 * \author      Sergio Chersovani <mlists [at] c-net.it>
 * \note        Reworked, but based on chan_sccp code.
 *              The original chan_sccp driver that was made by Zozo which itself was derived from the chan_skinny driver.
 *              Modified by Jan Czmok and Julien Goodwin
 * \note        This program is free software and may be modified and distributed under the terms of the GNU Public License.
 *              See the LICENSE file at the top of the source tree.
 * \note        To find out more about the reload function see \ref sccp_config_reload
 * \remarks     Only methods directly related to chan-sccp configuration should be stored in this source file.
 *
 */

/*** DOCUMENTATION
	<manager name="SCCPConfigMetadata" language="en_US">
		<synopsis>Describe the sccp.conf options as JSON.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
			<parameter name="Segment">
				<para>Section to describe. Without it, the response lists the module version, build options and the sections.</para>
				<enumlist>
					<enum name="general"/>
					<enum name="device"/>
					<enum name="line"/>
					<enum name="softkey"/>
				</enumlist>
			</parameter>
			<parameter name="ResultFormat">
				<para>How the JSON is returned.</para>
				<enumlist>
					<enum name="list"><para>In an SCCPConfigMetadata event, followed by SCCPConfigMetadataComplete.</para></enum>
					<enum name="command"><para>As command output, with DataType: JSON.</para></enum>
				</enumlist>
			</parameter>
		</syntax>
		<description>
			<para>Without ResultFormat the JSON is returned in the JSON header of the response.
			For a section, each option has Name, Type (BOOLEAN, INT, UNSIGNED INT, STRING, PARSER, CHAR or ENUM),
			Size, Flags (Required, Deprecated, Obsolete, MultiEntry, RestartRequiredOnUpdate), DefaultValue
			(null when the option has no default of its own), Description, PossibleValues for ENUM options and
			Parser for PARSER options.</para>
		</description>
		<see-also>
			<ref type="managerEvent">SCCPConfigMetadata</ref>
			<ref type="managerEvent">SCCPConfigMetadataComplete</ref>
		</see-also>
		<responses>
			<list-elements>
				<managerEvent language="en_US" name="SCCPConfigMetadata">
					<managerEventInstance class="EVENT_FLAG_COMMAND">
						<synopsis>The requested metadata.</synopsis>
						<syntax>
							<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
							<parameter name="JSON">
								<para>The metadata as JSON.</para>
							</parameter>
						</syntax>
					</managerEventInstance>
				</managerEvent>
			</list-elements>
			<managerEvent language="en_US" name="SCCPConfigMetadataComplete">
				<managerEventInstance class="EVENT_FLAG_COMMAND">
					<synopsis>End of the SCCPConfigMetadata list.</synopsis>
					<syntax>
						<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
					</syntax>
				</managerEventInstance>
			</managerEvent>
		</responses>
	</manager>
***/
#include "config.h"
#include "common.h"
#include "sccp_config.h"
#include "sccp_device.h"
#include "sccp_featureButton.h"
#include "sccp_line.h"
#include "sccp_linedevice.h"
#include "sccp_mwi.h"
#include "sccp_session.h"
#include "sccp_utils.h"
#include "sccp_labels.h"
#include "revision.h"

SCCP_FILE_VERSION(__FILE__, "");

#include <asterisk/paths.h>
#if defined(CS_AST_HAS_EVENT) && defined(HAVE_PBX_EVENT_H) && (defined(CS_DEVICESTATE) || defined(CS_CACHEABLE_DEVICESTATE))
#	include <asterisk/event.h>
#endif

#ifdef HAVE_PBX_APP_H
#	include <asterisk/app.h>
#endif

#ifndef offsetof
#	if defined(__GNUC__) && __GNUC__ > 3
#		define offsetof(type, member) __builtin_offsetof(type, member)
#	else
#		define offsetof(TYPE, MEMBER) ((size_t) & ((TYPE *)0)->MEMBER)
#	endif
#endif
#ifndef offsetof
#endif
#define offsize(TYPE, MEMBER) sizeof(((TYPE *)0)->MEMBER)
#define G_OBJ_REF(x)          offsize(struct sccp_global_vars, x), offsetof(struct sccp_global_vars, x)
#define D_OBJ_REF(x)          offsize(struct sccp_device, x), offsetof(struct sccp_device, x)
#define L_OBJ_REF(x)          offsize(struct sccp_line, x), offsetof(struct sccp_line, x)
#define S_OBJ_REF(x)          offsize(struct softKeySetConfiguration, x), offsetof(struct softKeySetConfiguration, x)
#define H_OBJ_REF(x)          offsize(struct sccp_hotline, x), offsetof(struct sccp_hotline, x)

enum SCCPConfigOptionType
{
	/* clang-format off */
	SCCP_CONFIG_DATATYPE_BOOLEAN			= 1 << 0,
	SCCP_CONFIG_DATATYPE_INT			= 1 << 1,
	SCCP_CONFIG_DATATYPE_UINT			= 1 << 2,
	SCCP_CONFIG_DATATYPE_STRING			= 1 << 3,
	SCCP_CONFIG_DATATYPE_PARSER			= 1 << 4,
	SCCP_CONFIG_DATATYPE_STRINGPTR			= 1 << 5,
	SCCP_CONFIG_DATATYPE_CHAR			= 1 << 6,
	SCCP_CONFIG_DATATYPE_ENUM			= 1 << 7,
	/* clang-format on */
};

enum SCCPConfigOptionFlag
{
	/* clang-format off */
	SCCP_CONFIG_FLAG_IGNORE 			= 1 << 0,
	SCCP_CONFIG_FLAG_NONE	 			= 1 << 1,
	SCCP_CONFIG_FLAG_DEPRECATED			= 1 << 2,		/*< parameter is deprecated and should not be used anymore, warn user and still set variable */
	SCCP_CONFIG_FLAG_OBSOLETE			= 1 << 3,
	SCCP_CONFIG_FLAG_CHANGED			= 1 << 4,
	SCCP_CONFIG_FLAG_REQUIRED			= 1 << 5,		/*< parameter is required */
	SCCP_CONFIG_FLAG_GET_DEVICE_DEFAULT		= 1 << 6,		/*< retrieve default value from device */
	SCCP_CONFIG_FLAG_GET_GLOBAL_DEFAULT		= 1 << 7,		/*< retrieve default value from global */
	SCCP_CONFIG_FLAG_MULTI_ENTRY			= 1 << 8,
	/* clang-format on */
};

typedef struct SCCPConfigOption {
	/* clang-format off */
	const char *name;
	const size_t size;
	const int offset;							/*!< The offset relative to the context structure where the option value is stored. */
	int type; 								/*!< bitfield of enum SCCPConfigOptionType */
	sccp_value_changed_t (*converter_f)(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment);
	sccp_enum_str2intval_t str2intval;
        sccp_enum_all_entries_t all_entries;
        const char *parsername;
	int flags;								/*!< bitfield of enum SCCPConfigOptionFlag */
	sccp_configurationchange_t change;
	const char *defaultValue;
	const char *description;
	/* clang-format on */
} SCCPConfigOption;

sccp_value_changed_t sccp_config_parse_codec_preferences(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment);
sccp_value_changed_t sccp_config_parse_mailbox(void * const dest, const size_t size, PBX_VARIABLE_TYPE * vroot, const sccp_config_segment_t segment);
sccp_value_changed_t sccp_config_parse_tos(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment);
sccp_value_changed_t sccp_config_parse_cos(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment);
sccp_value_changed_t sccp_config_parse_amaflags(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment);
sccp_value_changed_t sccp_config_parse_secondaryDialtoneDigits(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment);
sccp_value_changed_t sccp_config_parse_variables(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment);
sccp_value_changed_t sccp_config_parse_group(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment);
sccp_value_changed_t sccp_config_parse_deny_permit(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment);
sccp_value_changed_t sccp_config_parse_button(void * const dest, const size_t size, PBX_VARIABLE_TYPE * vars, const sccp_config_segment_t segment);
sccp_value_changed_t sccp_config_parse_permithosts(void * const dest, const size_t size, PBX_VARIABLE_TYPE * vroot, const sccp_config_segment_t segment);
sccp_value_changed_t sccp_config_parse_addons(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment);
sccp_value_changed_t sccp_config_parse_privacyFeature(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment);
sccp_value_changed_t sccp_config_parse_debug(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment);
sccp_value_changed_t sccp_config_parse_ipaddress(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment);
sccp_value_changed_t sccp_config_parse_port(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment);
sccp_value_changed_t sccp_config_parse_context(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment);
sccp_value_changed_t sccp_config_parse_hotline_context(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment);
sccp_value_changed_t sccp_config_parse_hotline_exten(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment);
sccp_value_changed_t sccp_config_parse_hotline_label(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment);
sccp_value_changed_t sccp_config_parse_jbflags_enable(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment);
sccp_value_changed_t sccp_config_parse_jbflags_force(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment);
sccp_value_changed_t sccp_config_parse_jbflags_log(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment);
sccp_value_changed_t sccp_config_parse_jbflags_maxsize(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment);
sccp_value_changed_t sccp_config_parse_jbflags_impl(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment);
sccp_value_changed_t sccp_config_parse_jbflags_jbresyncthreshold(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment);
sccp_value_changed_t sccp_config_checkButton(sccp_buttonconfig_list_t * buttonconfigList, int buttonindex, sccp_config_buttontype_t type, const char * name, const char * options, const char * args);
sccp_value_changed_t sccp_config_parse_webdir(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment);
sccp_value_changed_t sccp_config_parse_earlyrtp(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment);

#include "sccp_config_entries.hh"

typedef struct SCCPConfigSegment {
	const char *                name;
	const SCCPConfigOption *    config;
	long unsigned int           config_size;
	const sccp_config_segment_t segment;
} SCCPConfigSegment;

static const SCCPConfigSegment sccpConfigSegments[] = {
	{ "general", sccpGlobalConfigOptions, ARRAY_LEN(sccpGlobalConfigOptions), SCCP_CONFIG_GLOBAL_SEGMENT },
	{ "device", sccpDeviceConfigOptions, ARRAY_LEN(sccpDeviceConfigOptions), SCCP_CONFIG_DEVICE_SEGMENT },
	{ "line", sccpLineConfigOptions, ARRAY_LEN(sccpLineConfigOptions), SCCP_CONFIG_LINE_SEGMENT },
	{ "softkey", sccpSoftKeyConfigOptions, ARRAY_LEN(sccpSoftKeyConfigOptions), SCCP_CONFIG_SOFTKEY_SEGMENT },
};

static const SCCPConfigSegment * sccp_find_segment(const sccp_config_segment_t segment)
{
	uint8_t i = 0;

	for (i = 0; i < ARRAY_LEN(sccpConfigSegments); i++) {
		if (sccpConfigSegments[i].segment == segment) {
			return &sccpConfigSegments[i];
		}
	}
	return NULL;
}

static const SCCPConfigOption * sccp_find_config(const sccp_config_segment_t segment, const char * name)
{
	const SCCPConfigSegment * sccpConfigSegment = sccp_find_segment(segment);
	if (!sccpConfigSegment) {
		pbx_log(LOG_ERROR, "SCCP: config segment %d does not exist (caller bug)\n", segment);
		return NULL;
	}
	const SCCPConfigOption * config = sccpConfigSegment->config;

	char   delims[]    = "|";
	char * token       = NULL;
	char * tokenrest   = NULL;
	char * config_name = NULL;

	for (long unsigned int i = 0; i < sccpConfigSegment->config_size; i++) {
		if (strstr(config[i].name, delims) != NULL) {
			config_name = pbx_strdup(config[i].name);
			token       = strtok_r(config_name, delims, &tokenrest);
			while (token != NULL) {
				if (strcasecmp(token, name) == 0) {
					sccp_free(config_name);
					return &config[i];
				}
				token = strtok_r(NULL, delims, &tokenrest);
			}
			sccp_free(config_name);
		}
		if (strcasecmp(config[i].name, name) == 0) {
			return &config[i];
		}
	}

	return NULL;
}

static PBX_VARIABLE_TYPE * createVariableSetForMultiEntryParameters(PBX_VARIABLE_TYPE * cat_root, const char * configOptionName, PBX_VARIABLE_TYPE * out)
{
	PBX_VARIABLE_TYPE * v   = cat_root;
	PBX_VARIABLE_TYPE * tmp = NULL;

	size_t options_len = strlen(configOptionName) + 3;
	char   options[options_len];
	snprintf(options, options_len, "|%s|", configOptionName);
	for (v = cat_root; v; v = v->next) {
		if (strcasestr(options, v->name)) {
			size_t v_name_len = strlen(v->name) + 3;
			char   v_name[v_name_len];
			snprintf(v_name, v_name_len, "|%s|", v->name);
			if (strcasestr(options, v_name)) {
				if (!tmp) {
					sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_4 "new variable set: %s=%s\n", v->name, v->value);
					if (!(out = pbx_variable_new(v->name, v->value, ""))) {
						pbx_log(LOG_ERROR, "SCCP: could not copy config variable %s (out of memory); option not applied\n", v->name);
						goto EXIT;
					}
					tmp = out;
				} else {
					sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_4 "variable added: %s=%s\n", v->name, v->value);
					if (!(tmp->next = pbx_variable_new(v->name, v->value, ""))) {
						pbx_log(LOG_ERROR, "SCCP: could not copy config variable %s (out of memory); option not applied\n", v->name);
						pbx_variables_destroy(out);
						goto EXIT;
					}
					tmp = tmp->next;
				}
			}
		}
	}
EXIT:
	return out;
}

static PBX_VARIABLE_TYPE * createVariableSetForTokenizedDefault(const char * configOptionName, const char * defaultValue, PBX_VARIABLE_TYPE * out)
{
	PBX_VARIABLE_TYPE * tmp = NULL;

	char   delims[]                    = "|";
	char * option_name_tokens          = pbx_strdupa(configOptionName);
	char * option_value_tokens         = pbx_strdupa(defaultValue);
	char * option_name_tokens_saveptr  = NULL;
	char * option_value_tokens_saveptr = NULL;

	char * option_name  = strtok_r(option_name_tokens, "|", &option_name_tokens_saveptr);
	char * option_value = strtok_r(option_value_tokens, "|", &option_value_tokens_saveptr);

	while (option_name != NULL && option_value != NULL) {
		sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_4 "option %s, value %s\n", option_name, option_value);
		if (!tmp) {
			sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_4 "new variable set: %s=%s\n", option_name, option_value);
			if (!(out = pbx_variable_new(option_name, option_value, ""))) {
				pbx_log(LOG_ERROR, "SCCP: could not copy config variable %s (out of memory); option not applied\n", option_name);
				goto EXIT;
			}
			tmp = out;
		} else {
			sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_4 "variable added: %s=%s\n", option_name, option_value);
			if (!(tmp->next = pbx_variable_new(option_name, option_value, ""))) {
				pbx_log(LOG_ERROR, "SCCP: could not copy config variable %s (out of memory); option not applied\n", option_name);
				pbx_variables_destroy(out);
				goto EXIT;
			}
			tmp = tmp->next;
		}
		option_name  = strtok_r(NULL, delims, &option_name_tokens_saveptr);
		option_value = strtok_r(NULL, delims, &option_value_tokens_saveptr);
	}
EXIT:
	return out;
}

static sccp_configurationchange_t sccp_config_object_setValue(void * const obj, PBX_VARIABLE_TYPE * cat_root, const char * name, const char * value, int lineno, const sccp_config_segment_t segment, boolean_t * SetEntries,
							      boolean_t default_run)
{
	const SCCPConfigSegment * sccpConfigSegment = sccp_find_segment(segment);
	if (!sccpConfigSegment) {
		pbx_log(LOG_ERROR, "SCCP: config segment %d does not exist (caller bug)\n", segment);
		return SCCP_CONFIG_ERROR;
	}
	const SCCPConfigOption *  sccpConfigOption        = sccp_find_config(segment, name);
	void *                    dst                     = NULL;
	enum SCCPConfigOptionType type                    = 0;
	enum SCCPConfigOptionFlag flags                   = 0;

	sccp_value_changed_t       changed = SCCP_CONFIG_CHANGE_NOCHANGE;
	sccp_configurationchange_t changes = SCCP_CONFIG_NOUPDATENEEDED;

	sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_3 "SCCP: [%s] %s %s%s%s (line %d)\n", sccpConfigSegment->name, name, value ? "= '" : "", value ? value : "", value ? "'" : "", lineno);

	short int              int8num   = 0;
	int                    int16num  = 0;
	long int               int32num  = 0;
	long long int          int64num  = 0;
	short unsigned int     uint8num  = 0;
	unsigned int           uint16num = 0;
	long unsigned int      uint32num = 0;
	long long unsigned int uint64num = 0;
	boolean_t              boolean   = 0;
	char *                 str       = NULL;
	char                   oldChar   = 0;
	char *                 tmp_value = NULL;

	if (!sccpConfigOption) {
		if (strlen(name) == 0 || name[0] != '_') {
			pbx_log(LOG_WARNING, "SCCP: sccp.conf line %d: '%s' is not a %s option; ignored\n", lineno, name, sccpConfigSegment->name);
		}
		return SCCP_CONFIG_NOUPDATENEEDED;
	}

	dst   = ((uint8_t *)obj) + sccpConfigOption->offset;
	type  = (enum SCCPConfigOptionType)sccpConfigOption->type;
	flags = (enum SCCPConfigOptionFlag)sccpConfigOption->flags;

	if (SetEntries != NULL && ((flags & SCCP_CONFIG_FLAG_MULTI_ENTRY) == SCCP_CONFIG_FLAG_MULTI_ENTRY)) {
		for (long unsigned int y = 0; y < sccpConfigSegment->config_size; y++) {
			if (sccpConfigOption->offset == sccpConfigSegment->config[y].offset) {
				if (SetEntries[y] == TRUE) {
					sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_3 "SCCP: option %lu (%s) already set; multi-entry value skipped\n", y,
											sccpConfigSegment->config[y].name);
					return SCCP_CONFIG_NOUPDATENEEDED;
				}
			}
		}
	}

	if ((flags & SCCP_CONFIG_FLAG_IGNORE) == SCCP_CONFIG_FLAG_IGNORE) {
		return SCCP_CONFIG_NOUPDATENEEDED;
	}
	if ((flags & SCCP_CONFIG_FLAG_CHANGED) == SCCP_CONFIG_FLAG_CHANGED && !default_run) {
		pbx_log(LOG_NOTICE, "SCCP: sccp.conf line %d: the meaning of '%s' has changed: %s", lineno, name, sccpConfigOption->description);
	} else if ((flags & SCCP_CONFIG_FLAG_DEPRECATED) == SCCP_CONFIG_FLAG_DEPRECATED && lineno > 0 && !default_run) {
		pbx_log(LOG_WARNING, "SCCP: sccp.conf line %d: '%s' is deprecated but still applied: %s", lineno, name, sccpConfigOption->description);
	} else if ((flags & SCCP_CONFIG_FLAG_OBSOLETE) == SCCP_CONFIG_FLAG_OBSOLETE && lineno > 0 && !default_run) {
		pbx_log(LOG_WARNING, "SCCP: sccp.conf line %d: '%s' is obsolete and ignored: %s", lineno, name, sccpConfigOption->description);
		return SCCP_CONFIG_NOUPDATENEEDED;
	} else if ((flags & SCCP_CONFIG_FLAG_REQUIRED) == SCCP_CONFIG_FLAG_REQUIRED) {
		if (NULL == value) {
			pbx_log(LOG_WARNING, "SCCP: required %s option '%s' has no value: %s", sccpConfigSegment->name, name, sccpConfigOption->description);
			return SCCP_CONFIG_WARNING;
		}
	}

	switch (type) {
		case SCCP_CONFIG_DATATYPE_CHAR:
			oldChar = *(char *)dst;

			if (!sccp_strlen_zero(value)) {
				if (oldChar != value[0]) {
					changed      = SCCP_CONFIG_CHANGE_CHANGED;
					*(char *)dst = value[0];
				}
			} else {
				if (oldChar != '\0') {
					changed      = SCCP_CONFIG_CHANGE_CHANGED;
					*(char *)dst = '\0';
				}
			}
			break;

		case SCCP_CONFIG_DATATYPE_STRING:
			str = (char *)dst;

			if (!sccp_strlen_zero(value)) {
				if (sccp_strlen(value) > sccpConfigOption->size - 1) {
					pbx_log(LOG_WARNING, "SCCP: %s option '%s' value '%s' is longer than %d characters; truncated\n", sccpConfigSegment->name, name, value, (int)sccpConfigOption->size - 1);
				}
				if (strncasecmp(str, value, sccpConfigOption->size - 1) != 0) {
					if (GLOB(reload_in_progress)) {
						sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_2 "SCCP: option %s changed from '%s' to '%s'\n", name, str, value);
					}
					changed = SCCP_CONFIG_CHANGE_CHANGED;
					pbx_copy_string((char *)dst, value, sccpConfigOption->size);
				}
			} else if (!sccp_strlen_zero(str)) {
				changed = SCCP_CONFIG_CHANGE_CHANGED;
				pbx_copy_string((char *)dst, "", sccpConfigOption->size);
			}
			break;

		case SCCP_CONFIG_DATATYPE_STRINGPTR:
			changed = SCCP_CONFIG_CHANGE_NOCHANGE;
			str     = *(char **)dst;

			if (!sccp_strequals(str, value)) {
				changed = SCCP_CONFIG_CHANGE_CHANGED;
			}
			if (SCCP_CONFIG_CHANGE_CHANGED == changed) {
				if (*(void **)dst) {
					sccp_free(*(void **)dst);
				}
				if (value) {
					*(void **)dst = pbx_strdup(value);
				} else {
					*(void **)dst = NULL;
				}
			}
			break;

		case SCCP_CONFIG_DATATYPE_INT:
			if (sccp_strlen_zero(value)) {
				tmp_value = pbx_strdupa("0");
			} else {
				tmp_value = pbx_strdupa(value);
			}

			switch (sccpConfigOption->size) {
				case 1:
					if (sscanf(tmp_value, "%hd", &int8num) == 1) {
						if ((*(int8_t *)dst) != int8num) {
							*(int8_t *)dst = int8num;
							changed        = SCCP_CONFIG_CHANGE_CHANGED;
						}
					}
					break;
				case 2:
					if (sscanf(tmp_value, "%d", &int16num) == 1) {
						if ((*(int16_t *)dst) != int16num) {
							*(int16_t *)dst = int16num;
							changed         = SCCP_CONFIG_CHANGE_CHANGED;
						}
					}
					break;
				case 4:
					if (sscanf(tmp_value, "%ld", &int32num) == 1) {
						if ((*(int32_t *)dst) != int32num) {
							*(int32_t *)dst = int32num;
							changed         = SCCP_CONFIG_CHANGE_CHANGED;
						}
					}
					break;
				case 8:
					if (sscanf(tmp_value, "%lld", &int64num) == 1) {
						if ((*(int64_t *)dst) != int64num) {
							*(int64_t *)dst = int64num;
							changed         = SCCP_CONFIG_CHANGE_CHANGED;
						}
					}
					break;
			}
			break;
		case SCCP_CONFIG_DATATYPE_UINT:
			if (sccp_strlen_zero(value)) {
				tmp_value = pbx_strdupa("0");
			} else {
				tmp_value = pbx_strdupa(value);
			}
			switch (sccpConfigOption->size) {
				case 1:
					if ((!strncmp("0x", tmp_value, 2) && sscanf(tmp_value, "%hx", &uint8num) == 1) || (sscanf(tmp_value, "%hu", &uint8num) == 1)) {
						if ((*(uint8_t *)dst) != uint8num) {
							*(uint8_t *)dst = uint8num;
							changed         = SCCP_CONFIG_CHANGE_CHANGED;
						}
					}
					break;
				case 2:
					if ((!strncmp("0x", tmp_value, 2) && sscanf(tmp_value, "%x", &uint16num) == 1) || (sscanf(tmp_value, "%u", &uint16num) == 1)) {
						if ((*(uint16_t *)dst) != uint16num) {
							*(uint16_t *)dst = uint16num;
							changed          = SCCP_CONFIG_CHANGE_CHANGED;
						}
					}
					break;
				case 4:
					if ((!strncmp("0x", tmp_value, 2) && sscanf(tmp_value, "%lx", &uint32num) == 1) || (sscanf(tmp_value, "%lu", &uint32num) == 1)) {
						if ((*(uint32_t *)dst) != uint32num) {
							*(uint32_t *)dst = uint32num;
							changed          = SCCP_CONFIG_CHANGE_CHANGED;
						}
					}
					break;
				case 8:
					if ((!strncmp("0x", tmp_value, 2) && sscanf(tmp_value, "%llx", &uint64num) == 1) || (sscanf(tmp_value, "%llu", &uint64num) == 1)) {
						if ((*(uint64_t *)dst) != uint64num) {
							*(uint64_t *)dst = uint64num;
							changed          = SCCP_CONFIG_CHANGE_CHANGED;
						}
					}
					break;
			}
			break;

		case SCCP_CONFIG_DATATYPE_BOOLEAN:
			if (sccp_strlen_zero(value)) {
				boolean = FALSE;
			} else {
				if (sccp_true(value)) {
					boolean = TRUE;
				} else if (sccp_false(value)) {
					boolean = FALSE;
				} else {
					pbx_log(LOG_WARNING, "SCCP: %s option '%s' value '%s' is not yes/no, on/off or true/false; ignored\n", sccpConfigSegment->name, name, value);
					changed = SCCP_CONFIG_CHANGE_INVALIDVALUE;
					break;
				}
			}

			if (*(boolean_t *)dst != boolean) {
				*(boolean_t *)dst = boolean;
				changed           = SCCP_CONFIG_CHANGE_CHANGED;
			}
			break;

		case SCCP_CONFIG_DATATYPE_PARSER:
			{
				/* MULTI_ENTRY can only be parsed by a specific datatype_parser at this moment */
				if (sccpConfigOption->converter_f) {
					PBX_VARIABLE_TYPE * new_var = NULL;

					if (cat_root) {
						new_var = createVariableSetForMultiEntryParameters(cat_root, sccpConfigOption->name, new_var);
					} else {
						if (strstr(value, "|")) {
							new_var = createVariableSetForTokenizedDefault(name, value, new_var);
						} else {
							new_var = ast_variable_new(name, value, "");
						}
					}
					if (new_var) {
						changed = sccpConfigOption->converter_f(dst, sccpConfigOption->size, new_var, segment);
						pbx_variables_destroy(new_var);
					}
				}
			}
			break;
		case SCCP_CONFIG_DATATYPE_ENUM:
			{
				int enumValue = -1;
				if (!sccp_strlen_zero(value)) {
					const char * all_entries = sccpConfigOption->all_entries();
					if (!strncasecmp(value, "On,Yes,True,Off,No,False", strlen(value))) {
						if (sccp_true(value)) {
							if (strcasestr(all_entries, "On")) {
								enumValue = sccpConfigOption->str2intval("On");
							} else if (strcasestr(all_entries, "Yes")) {
								enumValue = sccpConfigOption->str2intval("Yes");
							} else if (strcasestr(all_entries, "True")) {
								enumValue = sccpConfigOption->str2intval("True");
							}
						} else if (!sccp_true(value)) {
							if (strcasestr(all_entries, "Off")) {
								enumValue = sccpConfigOption->str2intval("Off");
							} else if (strcasestr(all_entries, "No")) {
								enumValue = sccpConfigOption->str2intval("No");
							} else if (strcasestr(all_entries, "False")) {
								enumValue = sccpConfigOption->str2intval("False");
							}
						}
					} else if (!strncmp("0x", value, 2) && sscanf(value, "%x", &enumValue) == 1) {
						sccp_log(DEBUGCAT_HIGH)("SCCP: value %s = %d\n", value, enumValue);
					} else if (sscanf(value, "%d", &enumValue) == 1) {
						sccp_log(DEBUGCAT_HIGH)("SCCP: value %s = %d\n", value, enumValue);
					} else if ((enumValue = sccpConfigOption->str2intval(value)) != -1) {
						sccp_log(DEBUGCAT_HIGH)("SCCP: value %s = %d\n", value, enumValue);
					}
					if (enumValue != -1) {
						switch (sccpConfigOption->size) {
							case 1:
								if (*(int8_t *)dst != (int8_t)enumValue) {
									*(int8_t *)dst = (int8_t)enumValue;
									changed        = SCCP_CONFIG_CHANGE_CHANGED;
								}
								break;
							case 2:
								if ((*(uint16_t *)dst) != (int16_t)enumValue) {
									*(uint16_t *)dst = (int16_t)enumValue;
									changed          = SCCP_CONFIG_CHANGE_CHANGED;
								}
								break;
							default:
								if (*(int *)dst != enumValue) {
									*(int *)dst = enumValue;
									changed     = SCCP_CONFIG_CHANGE_CHANGED;
								}
								break;
						}
					} else {
						pbx_log(LOG_WARNING, "SCCP: %s option '%s' value '%s' is not one of: %s; ignored\n", sccpConfigSegment->name, name, value, sccpConfigOption->all_entries());
						changed = SCCP_CONFIG_CHANGE_INVALIDVALUE;
					}
					break;
				}
				pbx_log(LOG_WARNING, "SCCP: %s option '%s' cannot be empty; expected one of: %s\n", sccpConfigSegment->name, name, sccpConfigOption->all_entries());
				changed = SCCP_CONFIG_CHANGE_INVALIDVALUE;
			}
			break;
	}

	if (SCCP_CONFIG_CHANGE_CHANGED == changed) {
		if (GLOB(reload_in_progress)) {
			sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_2 "SCCP: option %s='%s' on line %d changed %s\n", name, value, lineno,
						    SCCP_CONFIG_NEEDDEVICERESET == sccpConfigOption->change ? "(causes device reset)" : "");
		}
		changes = sccpConfigOption->change;
	}

	if ((SCCP_CONFIG_CHANGE_INVALIDVALUE != changed && SCCP_CONFIG_CHANGE_ERROR != changed)
	    || ((flags & SCCP_CONFIG_FLAG_MULTI_ENTRY) == SCCP_CONFIG_FLAG_MULTI_ENTRY)
	) {
		if (SetEntries != NULL) {
			for (long unsigned int x = 0; x < sccpConfigSegment->config_size; x++) {
				if (sccpConfigOption->offset == sccpConfigSegment->config[x].offset) {
					sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_3 "SCCP: option %lu (%s) set\n", x, sccpConfigSegment->config[x].name);
					SetEntries[x] = TRUE;
				}
			}
		}
	}
	if (SCCP_CONFIG_CHANGE_INVALIDVALUE == changed && !default_run) {
		pbx_log(LOG_NOTICE, "SCCP: %s option '%s': %s", sccpConfigSegment->name, name, sccpConfigOption->description);
	}
	if (SCCP_CONFIG_CHANGE_ERROR == changed) {
		pbx_log(LOG_WARNING, "SCCP: %s option '%s' could not be parsed; ignored\n", sccpConfigSegment->name, name);
	}
	return changes;
}

static void sccp_config_set_defaults(void * const obj, const sccp_config_segment_t segment, boolean_t * SetEntries)
{
	if (!GLOB(cfg)) {
		pbx_log(LOG_WARNING, "SCCP: defaults not applied because sccp.conf is not loaded\n");
		return;
	}
	const SCCPConfigSegment * sccpConfigSegment = sccp_find_segment(segment);
	if (!sccpConfigSegment) {
		pbx_log(LOG_ERROR, "SCCP: config segment %d does not exist (caller bug)\n", segment);
		return;
	}

	const SCCPConfigOption * sccpDstConfig           = sccpConfigSegment->config;
	const SCCPConfigOption * sccpDefaultConfigOption = NULL;
	sccp_device_t *          referral_device         = NULL; /* need to find a way to find the default device to copy */
	char *                   referral_cat            = "";
	sccp_config_segment_t    search_segment_type     = 0;
	boolean_t                referralValueFound      = FALSE;

	boolean_t skip = FALSE;

	for (long unsigned int cur_elem = 0; cur_elem < sccpConfigSegment->config_size; cur_elem++) {
		skip = FALSE;
		for (long unsigned int skip_elem = 0; skip_elem < sccpConfigSegment->config_size; skip_elem++) {
			if (sccpDstConfig[cur_elem].offset == sccpConfigSegment->config[skip_elem].offset && (SetEntries[skip_elem] || sccpConfigSegment->config[cur_elem].flags & (SCCP_CONFIG_FLAG_OBSOLETE))) {
				sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_2 "SCCP: option %lu (%s) already set; default not applied\n", skip_elem,
										sccpConfigSegment->config[skip_elem].name);
				skip = TRUE;
				break;
			}
		}
		if (skip) {
			continue;
		}
		int flags = sccpDstConfig[cur_elem].flags;
		int type  = sccpDstConfig[cur_elem].type;

		if (((flags & SCCP_CONFIG_FLAG_OBSOLETE) != SCCP_CONFIG_FLAG_OBSOLETE)) {
			sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_1 "[%s] %s: looking up the default (flags %d, type %d)\n", sccpConfigSegment->name, sccpDstConfig[cur_elem].name,
									flags, type);

			if ((flags & SCCP_CONFIG_FLAG_GET_DEVICE_DEFAULT) == SCCP_CONFIG_FLAG_GET_DEVICE_DEFAULT) {
				referral_device     = &(*(sccp_device_t *)obj);
				referral_cat        = referral_device->id;
				search_segment_type = SCCP_CONFIG_DEVICE_SEGMENT;
			} else if ((flags & SCCP_CONFIG_FLAG_GET_GLOBAL_DEFAULT) == SCCP_CONFIG_FLAG_GET_GLOBAL_DEFAULT) {
				referral_cat        = "general";
				search_segment_type = SCCP_CONFIG_GLOBAL_SEGMENT;
			} else {
				referral_cat        = NULL;
				search_segment_type = segment;
			}

			sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_2 "option %s: %s default lookup %s%s\n", sccpDstConfig[cur_elem].name, referral_cat ? "referred" : "direct",
									referral_cat ? "via " : "", referral_cat ? referral_cat : "");

			if (referral_cat) {
				PBX_VARIABLE_TYPE * v        = NULL;
				PBX_VARIABLE_TYPE * cat_root = NULL;
				char                option_tokens[sccp_strlen(sccpDstConfig[cur_elem].name) + 2];
				referralValueFound = FALSE;

				snprintf(option_tokens, sizeof(option_tokens), "%s|", sccpDstConfig[cur_elem].name);
				char * option_tokens_saveptr = NULL;
				char * option_name           = strtok_r(option_tokens, "|", &option_tokens_saveptr);
				do {
					/* search for the default values in the referred segment, if found break so we can pass on the cat_root */
					for (cat_root = v = ast_variable_browse(GLOB(cfg), referral_cat); v; v = v->next) {
						if (sccp_strcaseequals((const char *)option_name, v->name)) {
							sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_2 "option %s: using '%s' from the [%s] section\n", option_name,
													v->value, referral_cat);
							referralValueFound = TRUE;
							break;
						}
					}
				} while ((option_name = strtok_r(NULL, "|", &option_tokens_saveptr)) != NULL);

				if (referralValueFound && v) { /* if referred to other segment and a value was found, pass the newly found cat_root directly to setValue */
					sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_3 "option %s: default taken from the [%s] section\n", sccpDstConfig[cur_elem].name, referral_cat);
					sccp_config_object_setValue(obj, cat_root, sccpDstConfig[cur_elem].name, v->value, __LINE__, segment, SetEntries, TRUE);
					continue;
				} else {
					sccpDefaultConfigOption = sccp_find_config(search_segment_type, sccpDstConfig[cur_elem].name);
					if (sccpDefaultConfigOption && !sccp_strlen_zero(sccpDefaultConfigOption->defaultValue)) {
						sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_3 "option %s: set to the section default '%s'\n", sccpDstConfig[cur_elem].name,
												sccpDstConfig[cur_elem].defaultValue);
						sccp_config_object_setValue(obj, NULL, sccpDstConfig[cur_elem].name, sccpDefaultConfigOption->defaultValue, __LINE__, segment, SetEntries, TRUE);
						continue;
					}
				}
			} else if (!sccp_strlen_zero(sccpDstConfig[cur_elem].defaultValue)) {
				sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_3 "option %s: set to its default '%s'\n", sccpDstConfig[cur_elem].name, sccpDstConfig[cur_elem].defaultValue);
				sccp_config_object_setValue(obj, NULL, sccpDstConfig[cur_elem].name, sccpDstConfig[cur_elem].defaultValue, __LINE__, segment, SetEntries, TRUE);
				continue;
			}

			if (type == SCCP_CONFIG_DATATYPE_STRINGPTR || type == SCCP_CONFIG_DATATYPE_PARSER) {
				sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_3 "option %s cleared\n", sccpDstConfig[cur_elem].name);
				sccp_config_object_setValue(obj, NULL, sccpDstConfig[cur_elem].name, "", __LINE__, segment, SetEntries, TRUE);
			}
		}
	}
}

void sccp_config_cleanup_dynamically_allocated_memory(void * const obj, const sccp_config_segment_t segment)
{
	const SCCPConfigSegment * sccpConfigSegment = sccp_find_segment(segment);
	if (!sccpConfigSegment) {
		pbx_log(LOG_ERROR, "SCCP: config segment %d does not exist (caller bug)\n", segment);
		return;
	}

	const SCCPConfigOption * sccpConfigOption = sccpConfigSegment->config;
	void *                   dst              = NULL;
	char *                   str              = NULL;

	for (long unsigned int i = 0; i < sccpConfigSegment->config_size; i++) {
		if (sccpConfigOption[i].type == SCCP_CONFIG_DATATYPE_STRINGPTR) {
			dst = ((uint8_t *)obj) + sccpConfigOption[i].offset;
			str = *(char **)dst;
			if (str) {
				sccp_free(str);
				str = NULL;
			}
		}
	}
}

sccp_value_changed_t sccp_config_parse_ipaddress(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment)
{
	sccp_value_changed_t changed = SCCP_CONFIG_CHANGE_NOCHANGE;
	char *               value   = pbx_strdupa(v->value);

	if (sccp_strlen_zero(value)) {
		value = pbx_strdupa("0.0.0.0");
	}
	struct sockaddr_storage bindaddr_prev = (*(struct sockaddr_storage *)dest);
	struct sockaddr_storage bindaddr_new  = {
                0,
	};

	if (!sccp_sockaddr_storage_parse(&bindaddr_new, value, PARSE_PORT_FORBID)) {
		pbx_log(LOG_WARNING, "SCCP: bindaddr '%s' is not a valid IP address (a port is not allowed here); ignored\n", value);
		changed = SCCP_CONFIG_CHANGE_INVALIDVALUE;
	} else {
		if (sccp_netsock_cmp_addr(&bindaddr_prev, &bindaddr_new)) {
			memcpy(&(*(struct sockaddr_storage *)dest), &bindaddr_new, sizeof(bindaddr_new));
			changed = SCCP_CONFIG_CHANGE_CHANGED;
		}
	}
	return changed;
}

sccp_value_changed_t sccp_config_parse_port(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment)
{
	sccp_value_changed_t changed = SCCP_CONFIG_CHANGE_NOCHANGE;
	char *               value   = pbx_strdupa(v->value);

	int                     new_port              = 0;
	struct sockaddr_storage bindaddr_storage_prev = (*(struct sockaddr_storage *)dest);

	if (sscanf(value, "%i", &new_port) == 1) {
		if (bindaddr_storage_prev.ss_family == AF_INET) {
			struct sockaddr_in bindaddr_prev = (*(struct sockaddr_in *)dest);

			if (bindaddr_prev.sin_port != 0) {
				if (bindaddr_prev.sin_port != htons(new_port)) {
					(*(struct sockaddr_in *)dest).sin_port = htons(new_port);
					changed                                = SCCP_CONFIG_CHANGE_CHANGED;
				}
			} else {
				(*(struct sockaddr_in *)dest).sin_port = htons(new_port);
				changed                                = SCCP_CONFIG_CHANGE_CHANGED;
			}
		} else if (bindaddr_storage_prev.ss_family == AF_INET6) {
			struct sockaddr_in6 bindaddr_prev = (*(struct sockaddr_in6 *)dest);

			if (bindaddr_prev.sin6_port != 0) {
				if (bindaddr_prev.sin6_port != htons(new_port)) {
					(*(struct sockaddr_in6 *)dest).sin6_port = htons(new_port);
					changed                                  = SCCP_CONFIG_CHANGE_CHANGED;
				}
			} else {
				(*(struct sockaddr_in6 *)dest).sin6_port = htons(new_port);
				changed                                  = SCCP_CONFIG_CHANGE_CHANGED;
			}
		} else {
			pbx_log(LOG_WARNING, "SCCP: port=%s ignored because bindaddr is neither IPv4 nor IPv6\n", value);
			changed = SCCP_CONFIG_CHANGE_INVALIDVALUE;
		}
	} else {
		pbx_log(LOG_WARNING, "SCCP: port '%s' is not a valid port number; ignored\n", value);
		changed = SCCP_CONFIG_CHANGE_INVALIDVALUE;
	}

	return changed;
}

sccp_value_changed_t sccp_config_parse_privacyFeature(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment)
{
	sccp_value_changed_t        changed        = SCCP_CONFIG_CHANGE_NOCHANGE;
	char *                      value          = pbx_strdupa(v->value);
	sccp_featureConfiguration_t privacyFeature = { 0 };

	if (sccp_strcaseequals(value, "full")) {
		privacyFeature.status  = ~0;
		privacyFeature.enabled = TRUE;
	} else if (sccp_strlen_zero(value) || sccp_true(value) || sccp_false(value)) {
		privacyFeature.status  = 0;
		privacyFeature.enabled = sccp_true(value);
	} else {
		pbx_log(LOG_WARNING, "SCCP: privacy value '%s' is not full, on or off; ignored\n", value);
		return SCCP_CONFIG_CHANGE_INVALIDVALUE;
	}

	if (privacyFeature.status != (*(sccp_featureConfiguration_t *)dest).status || privacyFeature.enabled != (*(sccp_featureConfiguration_t *)dest).enabled) {
		memcpy(dest, &privacyFeature, sizeof(sccp_featureConfiguration_t));
		changed = SCCP_CONFIG_CHANGE_CHANGED;
	}
	return changed;
}

sccp_value_changed_t sccp_config_parse_tos(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment)
{
	sccp_value_changed_t changed = SCCP_CONFIG_CHANGE_NOCHANGE;
	char *               value   = pbx_strdupa(v->value);
	uint8_t              tos     = 0;

	if (pbx_str2tos(value, &tos)) {
	} else if (sscanf(value, "%" SCNu8, &tos) == 1) {
		tos = tos & 0xff;
	} else if (sccp_strcaseequals(value, "lowdelay")) {
		tos = IPTOS_LOWDELAY;
	} else if (sccp_strcaseequals(value, "throughput")) {
		tos = IPTOS_THROUGHPUT;
	} else if (sccp_strcaseequals(value, "reliability")) {
		tos = IPTOS_RELIABILITY;

#if !defined(__NetBSD__) && !defined(__OpenBSD__) && !defined(SOLARIS)
	} else if (sccp_strcaseequals(value, "mincost")) {
		tos = IPTOS_MINCOST;
#endif
	} else if (sccp_strcaseequals(value, "none")) {
		tos = 0;
	} else {
#if !defined(__NetBSD__) && !defined(__OpenBSD__) && !defined(SOLARIS)
		changed = SCCP_CONFIG_CHANGE_INVALIDVALUE;
#else
		changed = SCCP_CONFIG_CHANGE_INVALIDVALUE;
#endif
		tos = 0x68 & 0xff;
	}

	if ((*(uint8_t *)dest) != tos) {
		*(uint8_t *)dest = tos;
		changed          = SCCP_CONFIG_CHANGE_CHANGED;
	}
	return changed;
}

sccp_value_changed_t sccp_config_parse_cos(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment)
{
	sccp_value_changed_t changed = SCCP_CONFIG_CHANGE_NOCHANGE;
	char *               value   = pbx_strdupa(v->value);
	uint8_t              cos     = 0;

	if (pbx_str2cos(value, &cos)) {
	} else if (sscanf(value, "%" SCNu8, &cos) == 1) {
		if (cos > 7) {
			pbx_log(LOG_WARNING, "SCCP: CoS value %d is outside the 802.1p range 0-7; ignored\n", cos);
			return SCCP_CONFIG_CHANGE_INVALIDVALUE;
		}
	}

	if ((*(uint8_t *)dest) != cos) {
		*(uint8_t *)dest = cos;
		changed          = SCCP_CONFIG_CHANGE_CHANGED;
	}

	return changed;
}

sccp_value_changed_t sccp_config_parse_amaflags(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment)
{
	sccp_value_changed_t changed  = SCCP_CONFIG_CHANGE_NOCHANGE;
	char *               value    = pbx_strdupa(v->value);
	int                  amaflags = 0;

	if (!sccp_strlen_zero(value)) {
		amaflags = pbx_channel_string2amaflag(value);
		if ((*(int *)dest) != amaflags) {
			changed      = SCCP_CONFIG_CHANGE_CHANGED;
			*(int *)dest = amaflags;
		}
	}
	return changed;
}

sccp_value_changed_t sccp_config_parse_secondaryDialtoneDigits(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment)
{
	sccp_value_changed_t changed = SCCP_CONFIG_CHANGE_NOCHANGE;
	char *               value   = pbx_strdupa(v->value);
	char *               str     = (char *)dest;

	if (sccp_strlen(value) <= 9) {
		if (!sccp_strcaseequals(str, value)) {
			sccp_copy_string(str, value, 9);
			changed = SCCP_CONFIG_CHANGE_CHANGED;
		}
	} else {
		changed = SCCP_CONFIG_CHANGE_INVALIDVALUE;
	}

	return changed;
}

sccp_value_changed_t sccp_config_parse_group(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment)
{
	sccp_value_changed_t changed = SCCP_CONFIG_CHANGE_NOCHANGE;
	char *               value   = pbx_strdupa(v->value);

	char * piece = NULL;
	char * c     = NULL;
	int    start = 0;

	int finish = 0;

	int          x     = 0;
	sccp_group_t group = 0;

	if (!sccp_strlen_zero(value)) {
		c = pbx_strdupa(value);

		while ((piece = strsep(&c, ","))) {
			if (sscanf(piece, "%30d-%30d", &start, &finish) == 2) {
			} else if (sscanf(piece, "%30d", &start) == 1) {
				finish = start;
			} else {
				pbx_log(LOG_WARNING, "SCCP: group list '%s': '%s' is not a number or a range like 1-5; that entry ignored\n", value, piece);
				continue;
			}
			for (x = start; x <= finish; x++) {
				if ((x > 63) || (x < 0)) {
					pbx_log(LOG_WARNING, "SCCP: group %d is outside the range 0-63; ignored\n", x);
				} else {
					group |= ((ast_group_t)1 << x);
				}
			}
		}
	}
#if defined(HAVE_UNALIGNED_BUSERROR)
	sccp_group_t group_orig = 0;

	memcpy(&group_orig, dest, sizeof(sccp_group_t));
	if (group_orig != group) {
		changed = SCCP_CONFIG_CHANGE_CHANGED;
		memcpy(dest, &group, sizeof(sccp_group_t));
	}
#else
	if ((*(sccp_group_t *)dest) != group) {
		changed = SCCP_CONFIG_CHANGE_CHANGED;
		*(sccp_group_t *)dest = group;
	}
#endif
	return changed;
}

sccp_value_changed_t sccp_config_parse_context(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment)
{
	sccp_value_changed_t changed = SCCP_CONFIG_CHANGE_INVALIDVALUE;
	if (v->value && !sccp_strlen_zero(v->value)) {
		char * value = pbx_strdupa(v->value);
		char * str   = (char *)dest;
		if (!sccp_strcaseequals(str, value)) {
			changed = SCCP_CONFIG_CHANGE_CHANGED;
			sccp_copy_string((char *)dest, value, size);
		} else {
			changed = SCCP_CONFIG_CHANGE_NOCHANGE;
		}
	}
	return changed;
}

sccp_value_changed_t sccp_config_parse_hotline_context(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment)
{
	sccp_value_changed_t changed = SCCP_CONFIG_CHANGE_NOCHANGE;
	char *               value   = pbx_strdupa(v->value);
	sccp_hotline_t *     hotline = *(sccp_hotline_t **)dest;

	if (hotline->line && !sccp_strcaseequals(hotline->line->context, value)) {
		changed = SCCP_CONFIG_CHANGE_CHANGED;
		if (hotline->line->context) {
			sccp_free(hotline->line->context);
		}
		hotline->line->context = pbx_strdup(value);
	}
	return changed;
}

sccp_value_changed_t sccp_config_parse_hotline_exten(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment)
{
	sccp_value_changed_t changed = SCCP_CONFIG_CHANGE_NOCHANGE;
	char *               value   = pbx_strdupa(v->value);
	sccp_hotline_t *     hotline = *(sccp_hotline_t **)dest;

	if (!sccp_strcaseequals(hotline->exten, value)) {
		changed = SCCP_CONFIG_CHANGE_CHANGED;
		pbx_copy_string(hotline->exten, value, SCCP_MAX_EXTENSION);
		if (hotline->line) {
			if (hotline->line->adhocNumber) {
				sccp_free(hotline->line->adhocNumber);
			}
			hotline->line->adhocNumber = pbx_strdup(value);
		}
	}
	return changed;
}

sccp_value_changed_t sccp_config_parse_hotline_label(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment)
{
	sccp_value_changed_t changed = SCCP_CONFIG_CHANGE_NOCHANGE;
	char *               value   = pbx_strdupa(v->value);
	sccp_hotline_t *     hotline = *(sccp_hotline_t **)dest;

	if (hotline->line && !sccp_strcaseequals(hotline->line->label, value)) {
		changed = SCCP_CONFIG_CHANGE_CHANGED;
		if (hotline->line->label) {
			sccp_free(hotline->line->label);
		}
		hotline->line->label = pbx_strdup(value);
	}
	return changed;
}

static sccp_value_changed_t sccp_config_parse_jbflags(void * const dest, const char * value, const unsigned int flag)
{
	sccp_value_changed_t changed = SCCP_CONFIG_CHANGE_NOCHANGE;

	struct ast_jb_conf * jb = *(struct ast_jb_conf **)dest;

	if (pbx_test_flag(jb, flag) != (unsigned)sccp_true(value)) {
		pbx_set2_flag(jb, sccp_true(value), flag);
		changed = SCCP_CONFIG_CHANGE_CHANGED;
	}
	return changed;
}

sccp_value_changed_t sccp_config_parse_jbflags_enable(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment)
{
	char * value = pbx_strdupa(v->value);

	return sccp_config_parse_jbflags(dest, value, AST_JB_ENABLED);
}

sccp_value_changed_t sccp_config_parse_jbflags_force(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment)
{
	char * value = pbx_strdupa(v->value);

	return sccp_config_parse_jbflags(dest, value, AST_JB_FORCED);
}

sccp_value_changed_t sccp_config_parse_jbflags_log(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment)
{
	char * value = pbx_strdupa(v->value);

	return sccp_config_parse_jbflags(dest, value, AST_JB_LOG);
}

sccp_value_changed_t sccp_config_parse_jbflags_maxsize(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment)
{
	sccp_value_changed_t changed = SCCP_CONFIG_CHANGE_NOCHANGE;
	int                  value   = sccp_atoi(v->value, strlen(v->value));
	struct ast_jb_conf * jb      = *(struct ast_jb_conf **)dest;

	if (jb->max_size != value) {
		jb->max_size = value;
		changed      = SCCP_CONFIG_CHANGE_CHANGED;
	}
	return changed;
}

sccp_value_changed_t sccp_config_parse_jbflags_jbresyncthreshold(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment)
{
	sccp_value_changed_t changed = SCCP_CONFIG_CHANGE_NOCHANGE;
	int                  value   = sccp_atoi(v->value, strlen(v->value));
	struct ast_jb_conf * jb      = *(struct ast_jb_conf **)dest;

	if (jb->resync_threshold != value) {
		jb->resync_threshold = value;
		changed              = SCCP_CONFIG_CHANGE_CHANGED;
	}
	return changed;
}

sccp_value_changed_t sccp_config_parse_jbflags_impl(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment)
{
	sccp_value_changed_t changed = SCCP_CONFIG_CHANGE_NOCHANGE;
	char *               value   = pbx_strdupa(v->value);
	struct ast_jb_conf * jb      = *(struct ast_jb_conf **)dest;

	if (!sccp_strcaseequals(jb->impl, value)) {
		sccp_copy_string(jb->impl, value, sizeof jb->impl);
		changed = SCCP_CONFIG_CHANGE_CHANGED;
	}
	return changed;
}

sccp_value_changed_t sccp_config_parse_webdir(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment)
{
	sccp_value_changed_t changed              = SCCP_CONFIG_CHANGE_NOCHANGE;
	char *               value                = pbx_strdupa(v->value);
	char *               webdir               = (char *)dest;
	char                 new_webdir[PATH_MAX] = "";

	if (sccp_strlen_zero(value)) {
		snprintf(new_webdir, sizeof(new_webdir), "%s/%s", ast_config_AST_DATA_DIR, "static-http/");
	} else {
		snprintf(new_webdir, sizeof(new_webdir), "%s", value);
	}

	if (!sccp_strcaseequals(new_webdir, webdir)) {
		if (access(new_webdir, F_OK) != -1) {
			changed = SCCP_CONFIG_CHANGE_CHANGED;
			pbx_copy_string(webdir, new_webdir, size);
		} else {
			pbx_log(LOG_WARNING, "SCCP: webdir '%s' does not exist; webdir left empty\n", new_webdir);
			pbx_copy_string(webdir, "", size);
			changed = SCCP_CONFIG_CHANGE_INVALIDVALUE;
		}
	} else {
		changed = SCCP_CONFIG_CHANGE_NOCHANGE;
	}
	return changed;
}

sccp_value_changed_t sccp_config_parse_debug(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment)
{
	sccp_value_changed_t changed      = SCCP_CONFIG_CHANGE_NOCHANGE;
	uint32_t             debug_new    = 0;
	char *               debug_arr[1] = { 0 };

	for (; v; v = v->next) {
		debug_arr[0] = pbx_strdup(v->value);
		debug_new    = sccp_parse_debugline(debug_arr, 0, 1, debug_new);
		sccp_free(debug_arr[0]);
	}
	if (*(uint32_t *)dest != debug_new) {
		*(uint32_t *)dest = debug_new;
		changed           = SCCP_CONFIG_CHANGE_CHANGED;
	}
	return changed;
}

sccp_value_changed_t sccp_config_parse_earlyrtp(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment)
{
	sccp_value_changed_t changed = SCCP_CONFIG_CHANGE_NOCHANGE;
	boolean_t            old     = *(boolean_t *)dest;
	boolean_t new                = !sccp_false(v->value);

	if (sccp_strcaseequals(v->value, "none")) {
		new = FALSE;
	}

	if (new != old) {
		*(boolean_t *)dest = new;
		changed            = SCCP_CONFIG_CHANGE_CHANGED;
	}
	return changed;
}

sccp_value_changed_t sccp_config_parse_codec_preferences(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment)
{
	sccp_value_changed_t    changed                             = SCCP_CONFIG_CHANGE_NOCHANGE;
	skinny_capabilities_t * prefs                               = (skinny_capabilities_t *)dest;
	skinny_codec_t          new_codecs[SKINNY_MAX_CAPABILITIES] = { SKINNY_CODEC_NONE };
	int                     errors                              = 0;

	for (; v; v = v->next) {
		sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))("codec preference %s=%s\n", v->name, v->value);
		if (sccp_strcaseequals(v->name, "disallow")) {
			errors += sccp_codec_parseAllowDisallow(new_codecs, v->value, 0);
		} else if (sccp_strcaseequals(v->name, "allow")) {
			errors += sccp_codec_parseAllowDisallow(new_codecs, v->value, 1);
		} else {
			errors += 1;
		}
	}

	skinny_codec_t audio_prefs[SKINNY_MAX_CAPABILITIES] = { SKINNY_CODEC_NONE };
	sccp_get_codecs_bytype(new_codecs, audio_prefs, SKINNY_CODEC_TYPE_AUDIO);
#if CS_SCCP_VIDEO
	skinny_codec_t video_prefs[SKINNY_MAX_CAPABILITIES] = { SKINNY_CODEC_NONE };
	sccp_get_codecs_bytype(new_codecs, video_prefs, SKINNY_CODEC_TYPE_VIDEO);
#endif
	if (errors) {
		pbx_log(LOG_WARNING, "SCCP: allow/disallow list contains a codec name Asterisk does not recognize; codec preferences not changed\n");
		changed = SCCP_CONFIG_CHANGE_INVALIDVALUE;
	} else {
		if (memcmp(prefs->audio, audio_prefs, sizeof prefs->audio) != 0) {
			memcpy(prefs->audio, audio_prefs, sizeof prefs->audio);
			changed = SCCP_CONFIG_CHANGE_CHANGED;
		}
#if CS_SCCP_VIDEO
		if (memcmp(prefs->video, video_prefs, sizeof prefs->video) != 0) {
			memcpy(prefs->video, video_prefs, sizeof prefs->video);
			changed = SCCP_CONFIG_CHANGE_CHANGED;
		}
#endif
	}
	return changed;
}

sccp_value_changed_t sccp_config_parse_deny_permit(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment)
{
	sccp_value_changed_t changed = SCCP_CONFIG_CHANGE_NOCHANGE;
	int                  error   = 0;
	int                  errors  = 0;

	struct sccp_ha * prev_ha = *(struct sccp_ha **)dest;
	struct sccp_ha * ha      = NULL;

	for (; v; v = v->next) {
		if (sccp_strcaseequals(v->name, "deny")) {
			ha = sccp_append_ha("deny", v->value, ha, &error);
		} else if (sccp_strcaseequals(v->name, "permit") || sccp_strcaseequals(v->name, "localnet")) {
			if (sccp_strcaseequals(v->value, "internal")) {
				ha = sccp_append_ha("permit", "127.0.0.0/255.0.0.0", ha, &error);
				errors |= error;
				ha = sccp_append_ha("permit", "10.0.0.0/255.0.0.0", ha, &error);
				errors |= error;
				ha = sccp_append_ha("permit", "172.16.0.0/255.240.0.0", ha, &error);
				errors |= error;
				ha = sccp_append_ha("permit", "192.168.0.0/255.255.0.0", ha, &error);
			} else {
				ha = sccp_append_ha("permit", v->value, ha, &error);
			}
		}
		errors |= error;
	}
	if (errors == 0) {
		struct ast_str * ha_buf      = pbx_str_alloca(DEFAULT_PBX_STR_BUFFERSIZE);
		struct ast_str * prev_ha_buf = pbx_str_alloca(DEFAULT_PBX_STR_BUFFERSIZE);
		if (ha_buf && prev_ha_buf) {
			sccp_print_ha(ha_buf, DEFAULT_PBX_STR_BUFFERSIZE, ha);
			sccp_print_ha(prev_ha_buf, DEFAULT_PBX_STR_BUFFERSIZE, prev_ha);
			if (!sccp_strequals(pbx_str_buffer(ha_buf), pbx_str_buffer(prev_ha_buf))) {
				if (prev_ha) {
					sccp_free_ha(prev_ha);
				}
				*(struct sccp_ha **)dest = ha;
				changed                  = SCCP_CONFIG_CHANGE_CHANGED;
				ha                       = NULL;
			}
		} else {
			pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
			changed = SCCP_CONFIG_CHANGE_ERROR;
		}
	} else {
		sccp_log(DEBUGCAT_CONFIG)(VERBOSE_PREFIX_3 "SCCP: deny/permit entry not valid\n");
		changed = SCCP_CONFIG_CHANGE_INVALIDVALUE;
	}

	if (ha) {
		sccp_free_ha(ha);
	}
	return changed;
}

/*
 * Config Converter/Parser for Permit Hosts
 * order is irrelevant
 */
sccp_value_changed_t sccp_config_parse_permithosts(void * const dest, const size_t size, PBX_VARIABLE_TYPE * vroot, const sccp_config_segment_t segment)
{
	sccp_value_changed_t changed    = SCCP_CONFIG_CHANGE_NOCHANGE;
	sccp_hostname_t *    permithost = NULL;

	SCCP_LIST_HEAD(hostname, sccp_hostname_t) * permithostList = (struct hostname *)dest;

	PBX_VARIABLE_TYPE * v         = NULL;
	int                 listCount = SCCP_LIST_GETSIZE(permithostList);
	int                 varCount  = 0;
	int                 found     = 0;

	for (v = vroot; v; v = v->next) {
		SCCP_LIST_TRAVERSE(permithostList, permithost, list) {
			if (sccp_strcaseequals(permithost->name, v->value)) {
				found++;
				break;
			}
		}
		varCount++;
	}
	if (listCount != varCount || listCount != found) {
		while ((permithost = SCCP_LIST_REMOVE_HEAD(permithostList, list))) {
			sccp_free(permithost);
		}
		for (v = vroot; v; v = v->next) {
			if (!(permithost = (sccp_hostname_t *)sccp_calloc(1, sizeof(sccp_hostname_t)))) {
				pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
				return SCCP_CONFIG_CHANGE_ERROR;
			}
			sccp_copy_string(permithost->name, v->value, sizeof(permithost->name));
			SCCP_LIST_INSERT_TAIL(permithostList, permithost, list);
		}
		changed = SCCP_CONFIG_CHANGE_CHANGED;
	}
	return changed;
}

static skinny_devicetype_t addonstr2enum(const char * addonstr)
{
	if (sccp_strcaseequals(addonstr, "7914")) {
		return SKINNY_DEVICETYPE_CISCO_ADDON_7914;
	}
	if (sccp_strcaseequals(addonstr, "7915")) {
		return SKINNY_DEVICETYPE_CISCO_ADDON_7915_24BUTTON;
	}
	if (sccp_strcaseequals(addonstr, "7916")) {
		return SKINNY_DEVICETYPE_CISCO_ADDON_7916_24BUTTON;
	}
	if (sccp_strcaseequals(addonstr, "500S")) {
		return SKINNY_DEVICETYPE_CISCO_ADDON_SPA500S;
	}
	if (sccp_strcaseequals(addonstr, "500DS")) {
		return SKINNY_DEVICETYPE_CISCO_ADDON_SPA500DS;
	}
	if (sccp_strcaseequals(addonstr, "932DS")) {
		return SKINNY_DEVICETYPE_CISCO_ADDON_SPA932DS;
	}
	sccp_log((DEBUGCAT_CORE))(VERBOSE_PREFIX_3 "SCCP: add-on type %s not known\n", addonstr);
	return SKINNY_DEVICETYPE_SENTINEL;
}

sccp_value_changed_t sccp_config_parse_addons(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment)
{
	unsigned int        changed    = SCCP_CONFIG_CHANGE_NOCHANGE;
	sccp_addon_t *      addon      = NULL;
	skinny_devicetype_t addon_type = 0;

	SCCP_LIST_HEAD(addon, sccp_addon_t) * addonList = (struct addon *)dest;

	SCCP_LIST_TRAVERSE_SAFE_BEGIN(addonList, addon, list) {
		if (v) {
			if (!sccp_strlen_zero(v->value)) {
				if ((addon_type = addonstr2enum(v->value)) && addon_type != SKINNY_DEVICETYPE_SENTINEL) {
					if (addon->type != addon_type) {
						sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))("add-on changed from %s (%d) to %s (%d)\n", skinny_devicetype2str(addon->type), addon->type, skinny_devicetype2str(addon_type),
												addon_type);
						addon->type = addon_type;
						changed |= SCCP_CONFIG_CHANGE_CHANGED;
					}
				} else {
					pbx_log(LOG_WARNING, "SCCP: addon type '%s' is not a known expansion module; ignored\n", v->value);
					changed |= SCCP_CONFIG_CHANGE_INVALIDVALUE;
				}
			}
			v = v->next;
		} else {
			sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))("add-on %d removed\n", addon->type);
			SCCP_LIST_REMOVE_CURRENT(list);
			sccp_free(addon);
			changed |= SCCP_CONFIG_CHANGE_CHANGED;
		}
	}
	SCCP_LIST_TRAVERSE_SAFE_END
		;

	int addon_counter = 0;

	for (; v; v = v->next) {
		if (2 > addon_counter++) {
			if (!sccp_strlen_zero(v->value)) {
				if ((addon_type = addonstr2enum(v->value)) && addon_type != SKINNY_DEVICETYPE_SENTINEL) {
					sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))("add-on added: %s (%d)\n", skinny_devicetype2str(addon_type), addon_type);
					if (!(addon = (sccp_addon_t *)sccp_calloc(1, sizeof(sccp_addon_t)))) {
						pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
						return SCCP_CONFIG_CHANGE_ERROR;
					}
					addon->type = addon_type;
					SCCP_LIST_INSERT_TAIL(addonList, addon, list);
					changed |= SCCP_CONFIG_CHANGE_CHANGED;
				} else {
					pbx_log(LOG_WARNING, "SCCP: addon type '%s' is not a known expansion module; ignored\n", v->value);
					changed |= SCCP_CONFIG_CHANGE_INVALIDVALUE;
				}
			}
		} else {
			pbx_log(LOG_WARNING, "SCCP: addon '%s' ignored: a device supports at most 2 expansion modules\n", v->value);
			changed |= SCCP_CONFIG_CHANGE_INVALIDVALUE;
		}
	}
	return (sccp_value_changed_t)changed;
}

/*
 * Config Converter/Parser for Mailbox Value
 * order is irrelevant
 */
sccp_value_changed_t sccp_config_parse_mailbox(void * const dest, const size_t size, PBX_VARIABLE_TYPE * vroot, const sccp_config_segment_t segment)
{
	sccp_value_changed_t changed = SCCP_CONFIG_CHANGE_NOCHANGE;
	sccp_mailbox_t *     mailbox = NULL;

	SCCP_LIST_HEAD(mailbox, sccp_mailbox_t) * mailboxList = (struct mailbox *)dest;

	PBX_VARIABLE_TYPE * v         = NULL;
	int                 varCount  = 0;
	int                 listCount = 0;

	listCount    = mailboxList->size;
	int notfound = 0;

	for (v = vroot; v; v = v->next) {
		if (!sccp_strlen_zero(v->value)) {
			varCount++;
		}
	}

	if (varCount == listCount) {
		SCCP_LIST_TRAVERSE(mailboxList, mailbox, list) {
			for (v = vroot; v; v = v->next) {
				if (!sccp_strlen_zero(v->value)) {
					char uniqueid[SCCP_MAX_MAILBOX_UNIQUEID];
					snprintf(uniqueid, sizeof(uniqueid), "%s%s", v->value, !strstr(v->value, "@") ? "@default" : "");
					if (sccp_strcaseequals(mailbox->uniqueid, uniqueid)) {
						continue;
					}
					notfound += 1;
				}
			}
		}
	}
	if (varCount != listCount || notfound) {
		while ((mailbox = SCCP_LIST_REMOVE_HEAD(mailboxList, list))) {
			sccp_free(mailbox);
		}
		for (v = vroot; v; v = v->next) {
			if (!sccp_strlen_zero(v->value)) {
				sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_3 "mailbox added: '%s'\n", v->value);
				if (!(mailbox = (sccp_mailbox_t *)sccp_calloc(1, sizeof(sccp_mailbox_t)))) {
					pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
					return SCCP_CONFIG_CHANGE_ERROR;
				}
				snprintf(mailbox->uniqueid, sizeof(mailbox->uniqueid), "%s%s", v->value, !strstr(v->value, "@") ? "@default" : "");
				SCCP_LIST_INSERT_TAIL(mailboxList, mailbox, list);
			}
		}
		changed = SCCP_CONFIG_CHANGE_CHANGED;
	}
	return changed;
}

sccp_value_changed_t sccp_config_parse_variables(void * const dest, const size_t size, PBX_VARIABLE_TYPE * v, const sccp_config_segment_t segment)
{
	sccp_value_changed_t changed = SCCP_CONFIG_CHANGE_NOCHANGE;

	PBX_VARIABLE_TYPE * variableList = *(PBX_VARIABLE_TYPE **)dest;

	if (variableList) {
		pbx_variables_destroy(variableList);
		variableList = NULL;
	}
	PBX_VARIABLE_TYPE * variable  = variableList;
	char *              var_name  = NULL;
	char *              var_value = NULL;

	for (; v; v = v->next) {
		var_name  = pbx_strdup(v->value);
		var_value = NULL;
		if ((var_value = strchr(var_name, '='))) {
			*var_value++ = '\0';
		}
		if (!sccp_strlen_zero(var_name) && !sccp_strlen_zero(var_value)) {
			sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))("channel variable added: %s=%s\n", var_name, var_value);
			if (!variable) {
				if (!(variableList = pbx_variable_new(var_name, var_value, ""))) {
					pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
					variableList = NULL;
					break;
				}
				variable = variableList;
			} else {
				if (!(variable->next = pbx_variable_new(var_name, var_value, ""))) {
					pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
					pbx_variables_destroy(variableList);
					variableList = NULL;
					break;
				}
				variable = variable->next;
			}
		}
		sccp_free(var_name);
	}
	*(PBX_VARIABLE_TYPE **)dest = variableList;

	return changed;
}

sccp_value_changed_t sccp_config_parse_button(void * const dest, const size_t size, PBX_VARIABLE_TYPE * vars, const sccp_config_segment_t segment)
{
	sccp_value_changed_t changed = SCCP_CONFIG_CHANGE_CHANGED;

	char * buttonType = NULL;

	char * buttonName = NULL;

	char * buttonOption = NULL;

	char *                   buttonArgs = NULL;
	char                     k_button[256];
	char *                   splitter    = NULL;
	sccp_config_buttontype_t type        = EMPTY;
	uint                     buttonindex = 0;

	sccp_buttonconfig_list_t * buttonconfigList = (sccp_buttonconfig_list_t *)dest;
	sccp_buttonconfig_t *      config           = NULL;
	PBX_VARIABLE_TYPE *        first_var        = vars;
	PBX_VARIABLE_TYPE *        v                = NULL;

	{
		SCCP_LIST_LOCK(buttonconfigList);
		sccp_log_and((DEBUGCAT_DEVICE + DEBUGCAT_HIGH))(VERBOSE_PREFIX_2 "buttons before the check:\n");
		SCCP_LIST_TRAVERSE(buttonconfigList, config, list) {
			sccp_log_and((DEBUGCAT_DEVICE + DEBUGCAT_HIGH))(VERBOSE_PREFIX_3 "index %d, type %-10.10s (%d), delete pending %s, update pending %s\n", config->index, sccp_config_buttontype2str(config->type),
									config->type, config->pendingDelete ? "yes" : "no", config->pendingUpdate ? "yes" : "no");
		}
		SCCP_LIST_UNLOCK(buttonconfigList);
	}

	if (GLOB(reload_in_progress)) {
		changed = SCCP_CONFIG_CHANGE_NOCHANGE;
		sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_3 "SCCP: checking button configuration\n");
		for (v = first_var; v && !sccp_strlen_zero(v->value); v = v->next) {
			sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_3 "checking button %s\n", v->value);
			sccp_copy_string(k_button, v->value, sizeof(k_button));
			splitter     = k_button;
			buttonType   = strsep(&splitter, ",");
			buttonName   = strsep(&splitter, ",");
			buttonOption = strsep(&splitter, ",");
			buttonArgs   = splitter;

			type = sccp_config_buttontype_str2val(buttonType);
			if (type == SCCP_CONFIG_BUTTONTYPE_SENTINEL) {
				pbx_log(LOG_WARNING, "SCCP: button type '%s' is not line, speeddial, service, feature or empty; an empty button was used\n", buttonType);
				changed = SCCP_CONFIG_CHANGE_INVALIDVALUE;
				type    = EMPTY;
			}
			if ((changed = sccp_config_checkButton(buttonconfigList, buttonindex, type, buttonName ? pbx_strip(buttonName) : NULL, buttonOption ? pbx_strip(buttonOption) : NULL,
							       buttonArgs ? pbx_strip(buttonArgs) : NULL))) {
				sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_3 "button %s changed; reloading all buttons\n", v->value);
				break;
			}
			buttonindex++;
		}
		if (!changed && SCCP_LIST_GETSIZE(buttonconfigList) != buttonindex) {
			sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_3 "number of buttons changed (%d to %d); reloading all buttons\n", SCCP_LIST_GETSIZE(buttonconfigList), buttonindex);
			changed = SCCP_CONFIG_CHANGE_CHANGED;
		}
		/*
		 * Clear/Set pendingDelete and PendingUpdate if button has changed or not Moved here from device_post_reload so that we will know the new state before line_post_reload.
		 */
		if (!changed) {
			sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_3 "no button changes\n");
			SCCP_LIST_LOCK(buttonconfigList);
			SCCP_LIST_TRAVERSE(buttonconfigList, config, list) {
				config->pendingDelete = 0;
				config->pendingUpdate = 0;
			}
			SCCP_LIST_UNLOCK(buttonconfigList);
		}
	}
	{
		SCCP_LIST_LOCK(buttonconfigList);
		sccp_log_and((DEBUGCAT_DEVICE + DEBUGCAT_HIGH))(VERBOSE_PREFIX_2 "buttons after the check:\n");
		SCCP_LIST_TRAVERSE(buttonconfigList, config, list) {
			sccp_log_and((DEBUGCAT_DEVICE + DEBUGCAT_HIGH))(VERBOSE_PREFIX_3 "index %d, type %-10.10s (%d), delete pending %s, update pending %s\n", config->index, sccp_config_buttontype2str(config->type),
									config->type, config->pendingDelete ? "yes" : "no", config->pendingUpdate ? "yes" : "no");
		}
		SCCP_LIST_UNLOCK(buttonconfigList);
	}
	if (changed) {
		buttonindex = 0;
		sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_3 "previous buttons are discarded after the reload\n");
		for (v = first_var; v && !sccp_strlen_zero(v->value); v = v->next) {
			sccp_copy_string(k_button, v->value, sizeof(k_button));
			splitter     = k_button;
			buttonType   = strsep(&splitter, ",");
			buttonName   = strsep(&splitter, ",");
			buttonOption = strsep(&splitter, ",");
			buttonArgs   = splitter;

			type = sccp_config_buttontype_str2val(buttonType);
			if (type == SCCP_CONFIG_BUTTONTYPE_SENTINEL) {
				pbx_log(LOG_WARNING, "SCCP: button type '%s' is not line, speeddial, service, feature or empty; an empty button was used\n", buttonType);
				changed = SCCP_CONFIG_CHANGE_INVALIDVALUE;
				type    = EMPTY;
			}
			sccp_config_addButton(buttonconfigList, buttonindex, type, buttonName ? pbx_strip(buttonName) : NULL, buttonOption ? pbx_strip(buttonOption) : NULL, buttonArgs ? pbx_strip(buttonArgs) : NULL);
			sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_4 "button added: %s\n", v->value);
			buttonindex++;
		}
	}

	{
		SCCP_LIST_LOCK(buttonconfigList);
		sccp_log_and((DEBUGCAT_DEVICE + DEBUGCAT_HIGH))(VERBOSE_PREFIX_2 "buttons after adding the new ones:\n");
		SCCP_LIST_TRAVERSE(buttonconfigList, config, list) {
			sccp_log_and((DEBUGCAT_DEVICE + DEBUGCAT_HIGH))(VERBOSE_PREFIX_3 "index %d, type %-10.10s (%d), delete pending %s, update pending %s\n", config->index, sccp_config_buttontype2str(config->type),
									config->type, config->pendingDelete ? "yes" : "no", config->pendingUpdate ? "yes" : "no");
		}
		SCCP_LIST_UNLOCK(buttonconfigList);
	}

	if (GLOB(reload_in_progress)) {
		sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_3 "buttons %s\n", changed ? "changed" : "remained the same");
	}

	return changed;
}

sccp_value_changed_t sccp_config_checkButton(sccp_buttonconfig_list_t * buttonconfigList, int buttonindex, sccp_config_buttontype_t type, const char * name, const char * options, const char * args)
{
	sccp_buttonconfig_t * config = NULL;
	char *                parse;
	AST_DECLARE_APP_ARGS(elems, AST_APP_ARG(option); AST_APP_ARG(arg););
	if (args && !sccp_strlen_zero(args)) {
		parse = pbx_strdupa(args);
		AST_STANDARD_APP_ARGS(elems, parse);
	}

	sccp_value_changed_t changed = SCCP_CONFIG_CHANGE_NOCHANGE;

	SCCP_LIST_LOCK(buttonconfigList);
	SCCP_LIST_TRAVERSE(buttonconfigList, config, list) {
		if (config->index == buttonindex) {
			sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_4 "button found at index %d:%d\n", config->index, buttonindex);
			break;
		}
	}
	SCCP_LIST_UNLOCK(buttonconfigList);

	changed = SCCP_CONFIG_CHANGE_CHANGED;
	if (config) {
		switch (type) {
			case LINE:
				{
					char                   extension[SCCP_MAX_EXTENSION];
					sccp_subscription_id_t subscriptionId;
					int                    parseRes = sccp_parseComposedId(name, 80, &subscriptionId, extension);
					if (parseRes) {
						sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_4 "SCCP: line button %s, subscription number %s, name %s, label %s, aux %s\n", extension,
												subscriptionId.number, subscriptionId.name, subscriptionId.label, subscriptionId.aux);
						if (LINE == config->type && sccp_strequals(config->label, name) && sccp_strequals(config->button.line.name, extension)
						    && ((!config->button.line.subscriptionId && parseRes == 1)
							|| (config->button.line.subscriptionId
							    && (sccp_strcaseequals(config->button.line.subscriptionId->number, subscriptionId.number)
								&& sccp_strequals(config->button.line.subscriptionId->name, subscriptionId.name)
								&& sccp_strequals(config->button.line.subscriptionId->label, subscriptionId.label)
								&& sccp_strequals(config->button.line.subscriptionId->aux, subscriptionId.aux))))) {
							if (!options || sccp_strequals(config->button.line.options, options)) {
								sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_4 "SCCP: line button unchanged\n");
								changed = SCCP_CONFIG_CHANGE_NOCHANGE;
							} else {
								sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_4 "options: %s / %s (%d)\n", config->button.line.options, options,
														sccp_strequals(config->button.line.options, options));
							}
						}
					} else {
						pbx_log(LOG_WARNING, "SCCP: line button '%s' could not be parsed (expected line[@subscriber]); button skipped\n", name);
					}
					break;
				}
			case SPEEDDIAL:
				if (SPEEDDIAL == config->type && sccp_strequals(config->label, name) && sccp_strequals(config->button.speeddial.ext, options)) {
					if (!args || sccp_strequals(config->button.speeddial.hint, args)) {
						sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_4 "SCCP: speeddial button unchanged\n");
						changed = SCCP_CONFIG_CHANGE_NOCHANGE;
					}
				}
				break;
			case SERVICE:
				if (SERVICE == config->type && sccp_strequals(config->label, name) && sccp_strequals(config->button.service.url, options)) {
					sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_4 "SCCP: service button unchanged\n");
					changed = SCCP_CONFIG_CHANGE_NOCHANGE;
				}
				break;
			case FEATURE:
				if (FEATURE == config->type && buttonindex == config->index && sccp_strequals(config->label, name) && config->button.feature.id == sccp_feature_type_str2val(options)) {
					char * default_option        = "";
					char * default_arg           = "";
					char   combined_args[512]    = "";
					char   combined_current[512] = "";
					snprintf(combined_current, sizeof(combined_current), "%s, %s", config->button.feature.options ? config->button.feature.options : "",
						 config->button.feature.args ? config->button.feature.args : "");
					sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_4 "SCCP: feature button %s,%s changed to %s,%s\n", config->button.feature.options, config->button.feature.args,
											elems.option, elems.arg);
					if (SCCP_FEATURE_PARKINGLOT == config->button.feature.id) {
						default_option = "default";
						default_arg    = "RetrieveSingle";
					} else
#ifdef CS_DEVSTATE_FEATURE
					    if (SCCP_FEATURE_DEVSTATE == config->button.feature.id) {
						default_option = config->label;
						default_arg    = "00001|10012|22321";
					}
#endif
					snprintf(combined_args, sizeof(combined_args), "%s, %s", elems.option ? elems.option : default_option, elems.arg ? elems.arg : default_arg);
					sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_4 "old '%s', new '%s'\n", combined_current, combined_args);
					if ((sccp_strequals(combined_current, combined_args))) {
						sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_4 "SCCP: feature button unchanged\n");
						changed = SCCP_CONFIG_CHANGE_NOCHANGE;
						break;
					}
					sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_4 "SCCP: feature button changed\n");
				}
				break;
			case EMPTY:
				if (EMPTY == config->type) {
					sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_4 "SCCP: button unchanged\n");
					changed = SCCP_CONFIG_CHANGE_NOCHANGE;
				}
				break;
			default:
				sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_4 "SCCP: button type %d not known\n", type);
				break;
		}
	}
	if (changed) {
		sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_4 "SCCP: button template changed\n");
	} else {
		sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_4 "SCCP: button template unchanged\n");
	}
	return changed;
}

sccp_value_changed_t sccp_config_addButton(sccp_buttonconfig_list_t * buttonconfigList, int buttonindex, sccp_config_buttontype_t type, const char * name, const char * options, const char * args)
{
	sccp_buttonconfig_t * config = NULL;

	char * parse;
	AST_DECLARE_APP_ARGS(elems, AST_APP_ARG(option); AST_APP_ARG(arg););
	if (args && !sccp_strlen_zero(args)) {
		parse = pbx_strdupa(args);
		AST_STANDARD_APP_ARGS(elems, parse);
	}

	sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_4 "SCCP: loading new button configuration\n");

	if (!(config = (sccp_buttonconfig_t *)sccp_calloc(1, sizeof(sccp_buttonconfig_t)))) {
		pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
		return SCCP_CONFIG_CHANGE_ERROR;
	}
	SCCP_LIST_LOCK(buttonconfigList);
	if (buttonindex < 0) {
		/* append after the last configured button */
		sccp_buttonconfig_t * existing = NULL;
		buttonindex = 0;
		SCCP_LIST_TRAVERSE(buttonconfigList, existing, list) {
			if (existing->index >= buttonindex) {
				buttonindex = existing->index + 1;
			}
		}
	}
	config->index = buttonindex;
	config->type  = type;
	sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_4 "new %s button '%s' at %d:%d\n", sccp_config_buttontype2str(type), name, buttonindex, config->index);
	SCCP_LIST_INSERT_TAIL(buttonconfigList, config, list);
	SCCP_LIST_UNLOCK(buttonconfigList);

	if (type != EMPTY && (sccp_strlen_zero(name) || (type != LINE && !options))) {
		sccp_log((DEBUGCAT_CORE))(VERBOSE_PREFIX_1 "SCCP: %s button %d (name %s, options %s, args %s) not valid; replaced with an empty button\n", sccp_config_buttontype2str(type),
					  config->index, name, options, args);
		type = EMPTY;
	}

	switch (type) {
		case LINE:
			{
				char                     extension[SCCP_MAX_EXTENSION];
				sccp_subscription_id_t * subscriptionId = (sccp_subscription_id_t *)sccp_calloc(1, sizeof(sccp_subscription_id_t));
				if (!subscriptionId) {
					pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
					return SCCP_CONFIG_CHANGE_INVALIDVALUE;
				}
				if (sccp_parseComposedId(name, 80, subscriptionId, extension)) {
					;
					sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_4 "SCCP: line button\n");
					sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_4 "SCCP: line button %s, subscription number %s, name %s, label %s, aux %s\n", extension,
											subscriptionId->number, subscriptionId->name, subscriptionId->label, subscriptionId->aux);
					config->type             = LINE;
					config->label            = pbx_strdup(name);
					config->button.line.name = pbx_strdup(extension);

					if (!sccp_strlen_zero(subscriptionId->number) || !sccp_strlen_zero(subscriptionId->name) || !sccp_strlen_zero(subscriptionId->label) || !sccp_strlen_zero(subscriptionId->aux)) {
						config->button.line.subscriptionId = subscriptionId;
					} else {
						sccp_free(subscriptionId);
					}
				} else {
					pbx_log(LOG_WARNING, "SCCP: line button '%s' could not be parsed (expected line[@subscriber]); button skipped\n", name);
					sccp_free(subscriptionId);
					return SCCP_CONFIG_CHANGE_INVALIDVALUE;
				}
				if (options) {
					config->button.line.options = pbx_strdup(options);
				} else {
					config->button.line.options = NULL;
				}
				break;
			}
		case SPEEDDIAL:
			sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_4 "SCCP: speeddial button\n");
			config->type                 = SPEEDDIAL;
			config->label                = pbx_strdup(name);
			config->button.speeddial.ext = pbx_strdup(options);
			if (args) {
				config->button.speeddial.hint = pbx_strdup(args);
			} else {
				config->button.speeddial.hint = NULL;
			}
			break;
		case SERVICE:
			sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_4 "SCCP: service button\n");
			config->type               = SERVICE;
			config->label              = pbx_strdup(name);
			config->button.service.url = pbx_strdup(options);
			break;
		case FEATURE:
			sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_4 "SCCP: feature button\n");
			sccp_log_and((DEBUGCAT_FEATURE + DEBUGCAT_FEATURE_BUTTON + DEBUGCAT_BUTTONTEMPLATE))(VERBOSE_PREFIX_4 "feature %s\n", options);
			config->type              = FEATURE;
			config->label             = pbx_strdup(name);
			config->button.feature.id = sccp_feature_type_str2val(options);

			config->button.feature.options = NULL;
			config->button.feature.args    = NULL;

			if (SCCP_FEATURE_PARKINGLOT == config->button.feature.id) {
				if (elems.option && !sccp_strlen_zero(elems.option)) {
					config->button.feature.options = pbx_strdup(elems.option);
				} else {
					config->button.feature.options = pbx_strdup("default");
				}
				if (elems.arg && !sccp_strlen_zero(elems.arg)) {
					config->button.feature.args = pbx_strdup(elems.arg);
				} else {
					config->button.feature.args = pbx_strdup("RetrieveSingle");
				}
			} else
#ifdef CS_DEVSTATE_FEATURE
			    if (SCCP_FEATURE_DEVSTATE == config->button.feature.id) {
				if (elems.option && !sccp_strlen_zero(elems.option)) {
					config->button.feature.options = pbx_strdup(elems.option);
				} else {
					config->button.feature.options = pbx_strdup(config->label);
				}
				if (elems.arg && !sccp_strlen_zero(elems.arg)) {
					config->button.feature.args = pbx_strdup(elems.arg);
				} else {
					config->button.feature.args = pbx_strdup("00001|10012|22321");
				}
			} else
#endif
			{
				if (elems.option && !sccp_strlen_zero(elems.option)) {
					config->button.feature.options = pbx_strdup(elems.option);
				}
				if (elems.arg && !sccp_strlen_zero(elems.arg)) {
					config->button.feature.args = pbx_strdup(elems.arg);
				}
			}
			sccp_log_and((DEBUGCAT_FEATURE + DEBUGCAT_FEATURE_BUTTON + DEBUGCAT_BUTTONTEMPLATE))(VERBOSE_PREFIX_4 "feature button %d: feature %s, args %s\n", config->instance,
													     config->button.feature.options, config->button.feature.args);
			break;
		case EMPTY:
			sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_4 "SCCP: empty button\n");
			config->type  = EMPTY;
			config->label = NULL;
			break;
		default:
			sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_4 "SCCP: button type %d not known\n", type);
			config->type  = EMPTY;
			config->label = NULL;
			break;
	}
	return SCCP_CONFIG_CHANGE_CHANGED;
}

static void sccp_config_buildLine(sccp_line_t * l, PBX_VARIABLE_TYPE * v, boolean_t isRealtime)
{
	sccp_configurationchange_t res = sccp_config_applyLineConfiguration(l, v);
	if (!l) {
		pbx_log(LOG_ERROR, "SCCP: sccp_config_buildLine() was called without a line (caller bug)\n");
		return;
	}

#ifdef CS_SCCP_REALTIME
	l->realtime = isRealtime;
#endif
	if (GLOB(reload_in_progress) && res == SCCP_CONFIG_NEEDDEVICERESET) {
		sccp_log((DEBUGCAT_CORE))(VERBOSE_PREFIX_1 "%s: line settings changed; its devices need a restart\n", l->name);
		l->pendingUpdate = 1;
	} else {
		l->pendingUpdate = 0;
	}
	sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_2 "%s: removal cancelled: the line is still in sccp.conf\n", l->name);
	l->pendingDelete = 0;
}

static void sccp_config_buildDevice(sccp_device_t * d, PBX_VARIABLE_TYPE * variable, boolean_t isRealtime)
{
	PBX_VARIABLE_TYPE * v = variable;
	if (!d) {
		pbx_log(LOG_ERROR, "SCCP: sccp_config_buildDevice() was called without a device (caller bug)\n");
		return;
	}

	sccp_configurationchange_t res = sccp_config_applyDeviceConfiguration(d, v);

#ifdef CS_SCCP_REALTIME
	d->realtime = isRealtime;
#endif
	if (GLOB(reload_in_progress) && res == SCCP_CONFIG_NEEDDEVICERESET && d) {
		sccp_log((DEBUGCAT_CORE))(VERBOSE_PREFIX_1 "%s: device settings changed; restart needed\n", d->id);
		d->pendingUpdate = 1;
	} else {
		d->pendingUpdate = 0;
	}
	d->pendingDelete = 0;
}

sccp_configurationchange_t sccp_config_applyGlobalConfiguration(PBX_VARIABLE_TYPE * v)
{
	unsigned int        res                                            = SCCP_CONFIG_NOUPDATENEEDED;
	boolean_t           SetEntries[ARRAY_LEN(sccpGlobalConfigOptions)] = { FALSE };
	PBX_VARIABLE_TYPE * cat_root                                       = v;

	for (; v; v = v->next) {
		res |= sccp_config_object_setValue(sccp_globals, cat_root, v->name, v->value, v->lineno, SCCP_CONFIG_GLOBAL_SEGMENT, SetEntries, FALSE);
	}
	if (res) {
		sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_2 "changes need an update (%d)\n", res);
	}
	sccp_config_set_defaults(sccp_globals, SCCP_CONFIG_GLOBAL_SEGMENT, SetEntries);

	if (GLOB(keepalive) < SCCP_MIN_KEEPALIVE) {
		GLOB(keepalive) = SCCP_MIN_KEEPALIVE;
	}

	return (sccp_configurationchange_t)res;
}

static void sccp_config_add_default_softkeyset(void)
{
	PBX_VARIABLE_TYPE *      softkeyset_root  = NULL;
	PBX_VARIABLE_TYPE *      tmp              = NULL;
	uint                     cur_elem         = 0;
	const SCCPConfigOption * sccpConfigOption = sccpSoftKeyConfigOptions;
	for (cur_elem = 0; cur_elem < ARRAY_LEN(sccpSoftKeyConfigOptions); cur_elem++) {
		if (sccpConfigOption[cur_elem].defaultValue != NULL) {
			if (!softkeyset_root) {
				softkeyset_root = pbx_variable_new(sccpConfigOption[cur_elem].name, sccpConfigOption[cur_elem].defaultValue, "");
				tmp             = softkeyset_root;
			} else {
				tmp->next = pbx_variable_new(sccpConfigOption[cur_elem].name, sccpConfigOption[cur_elem].defaultValue, "");
				tmp       = tmp->next;
			}
		}
	}
	sccp_config_softKeySet(softkeyset_root, "default");
	pbx_variables_destroy(softkeyset_root);
}

boolean_t sccp_config_general(sccp_readingtype_t readingtype)
{
	PBX_VARIABLE_TYPE * v = NULL;

	if (!GLOB(cfg)) {
		pbx_log(LOG_ERROR, "SCCP: sccp.conf is not loaded; no global settings applied and SCCP stays disabled\n");
		return FALSE;
	}

	v = ast_variable_browse(GLOB(cfg), "general");
	if (!v) {
		pbx_log(LOG_ERROR, "SCCP: sccp.conf has no [general] section; SCCP stays disabled\n");
		return FALSE;
	}

	if (!sccp_netsock_getPort(&GLOB(bindaddr))) {
		struct sockaddr_in * in = (struct sockaddr_in *)&GLOB(bindaddr);

		in->sin_port             = ntohs(DEFAULT_SCCP_PORT);
		GLOB(bindaddr).ss_family = AF_INET;
	}
#ifdef HAVE_OPENSSL
	if (!sccp_netsock_getPort(&GLOB(secbindaddr))) {
		struct sockaddr_in * in = (struct sockaddr_in *)&GLOB(secbindaddr);

		in->sin_port                = ntohs(DEFAULT_SCCP_SECURE_PORT);
		GLOB(secbindaddr).ss_family = AF_INET;
	}
#endif

	sccp_configurationchange_t res = sccp_config_applyGlobalConfiguration(v);

	if (!sccp_netsock_getPort(&GLOB(bindaddr))) {
		sccp_netsock_setPort(&GLOB(bindaddr), DEFAULT_SCCP_PORT);
	}

	if (GLOB(reload_in_progress) && res == SCCP_CONFIG_NEEDDEVICERESET) {
		sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_1 "SCCP: [general] settings changed; all devices need a restart\n");
		GLOB(pendingUpdate) = 1;
	} else {
		GLOB(pendingUpdate) = 0;
	}

	if (GLOB(regcontext)) {
		char   newcontexts[SCCP_MAX_CONTEXT] = "";
		char   oldcontexts[SCCP_MAX_CONTEXT] = "";
		char * stringp                       = NULL;

		char * context = NULL;

		char * oldregcontext = NULL;

		sccp_copy_string(newcontexts, GLOB(regcontext), sizeof(newcontexts));
		stringp = newcontexts;

		sccp_copy_string(oldcontexts, GLOB(used_context), sizeof(oldcontexts));
		oldregcontext = oldcontexts;

		cleanup_stale_contexts(stringp, oldregcontext);

		while ((context = strsep(&stringp, "&"))) {
			sccp_copy_string(GLOB(used_context), context, sizeof(GLOB(used_context)));
			pbx_context_find_or_create(NULL, NULL, context, "SCCP");
		}
	}
	if (GLOB(externhost)) {
		sccp_netsock_flush_externhost();
	}

	return TRUE;
}

void cleanup_stale_contexts(char * new_contexts, char * old_contexts)
{
	char * oldcontext = NULL;

	char * newcontext = NULL;

	char * stalecontext = NULL;

	char * stringp = NULL;

	char newlist[SCCP_MAX_CONTEXT];

	while ((oldcontext = strsep(&old_contexts, "&"))) {
		stalecontext = NULL;
		sccp_copy_string(newlist, new_contexts, sizeof(newlist));
		stringp = newlist;
		while ((newcontext = strsep(&stringp, "&"))) {
			if (sccp_strequals(newcontext, oldcontext)) {
				stalecontext = NULL;
				break;
			} else {
				stalecontext = oldcontext;
			}
		}
		if (stalecontext) {
			ast_context_destroy(ast_context_find(stalecontext), "SCCP");
		}
	}
}

boolean_t sccp_config_readDevicesLines(sccp_readingtype_t readingtype)
{
	char *              cat          = NULL;
	PBX_VARIABLE_TYPE * v            = NULL;
	uint8_t             device_count = 0;
	uint8_t             line_count   = 0;
	sccp_device_t *     d            = NULL;

	sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_1 "loading devices and lines\n");

	sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_1 "read type %s (%d)\n", readingtype == 0 ? "Module load" : "Reload", readingtype);
	if (readingtype == SCCP_CONFIG_READRELOAD) {
		sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_2 "devices: before reload\n");
		sccp_device_pre_reload();
		sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_2 "lines: before reload\n");
		sccp_line_pre_reload();
		sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_2 "softkey sets: before reload\n");
		sccp_softkey_pre_reload();
	}

	if (!GLOB(cfg)) {
		pbx_log(LOG_ERROR, "SCCP: sccp.conf is not loaded; no devices or lines configured\n");
		return FALSE;
	}

	while ((cat = pbx_category_browse(GLOB(cfg), cat))) {
		const char * utype = NULL;

		if (!strcasecmp(cat, "general")) {
			continue;
		}
		utype = pbx_variable_retrieve(GLOB(cfg), cat, "type");
		sccp_log_and((DEBUGCAT_CONFIG + DEBUGCAT_HIGH))(VERBOSE_PREFIX_2 "SCCP: reading section of type %s\n", utype);

		if (!utype) {
			pbx_log(LOG_WARNING, "SCCP: sccp.conf section [%s] has no type= (device, line or softkeyset); section skipped\n", cat);
			continue;
		} else if (!strcasecmp(utype, "device")) {
			sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_2 "reading device [%s]\n", cat);
			v = ast_variable_browse(GLOB(cfg), cat);

			/*
			 * However, do not look into realtime, since we might have been asked to create a device for realtime addition, thus causing an infinite loop / recursion.
			 */
			AUTO_RELEASE(sccp_device_t, device, sccp_device_find_byid(cat, FALSE));
			sccp_nat_t nat = SCCP_NAT_AUTO;

			if (!device) {
				device = sccp_device_create(cat) /*ref_replace*/;
				if (!device) {
					return FALSE;
				}
				sccp_device_addToGlobals(device);
				device_count++;
			} else {
				if (device->pendingDelete) {
					nat                   = device->nat;
					device->pendingDelete = 0;
				}
			}
			sccp_config_buildDevice(device, v, FALSE);
			sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_3 "device %d: %s\n", device_count, cat);

			/* restore current nat status, if device does not get restarted */
			if (0 == device->pendingDelete && sccp_device_getRegistrationState(device) != SKINNY_DEVICE_RS_NONE) {
				if (SCCP_NAT_AUTO == device->nat && (SCCP_NAT_AUTO == nat || SCCP_NAT_AUTO_OFF == nat || SCCP_NAT_AUTO_ON == nat)) {
					device->nat = nat;
				}
			}
		} else if (!strcasecmp(utype, "line")) {
			sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_2 "reading line [%s]\n", cat);

			line_count++;

			v = ast_variable_browse(GLOB(cfg), cat);
			AUTO_RELEASE(sccp_line_t, l, sccp_line_find_byname(cat, FALSE));

			if (l) {
				sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_3 "line %d: %s, updating\n", line_count, cat);
				sccp_config_buildLine(l, v, FALSE);
			} else if ((l = sccp_line_create(cat)) /*ref_replace*/) {
				sccp_config_buildLine(l, v, FALSE);
				sccp_line_addToGlobals(l); /* may find another line instance create by another thread, in that case the newly created line is going to be dropped when l is released */
			} else {
				return FALSE;
			}
		} else if (!strcasecmp(utype, "softkeyset")) {
			sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_2 "reading softkey set [%s]\n", cat);
			if (sccp_strcaseequals(cat, "default")) {
				pbx_log(LOG_WARNING, "SCCP: softkeyset [default] is built in and cannot be redefined; section skipped\n");
			} else {
				v = ast_variable_browse(GLOB(cfg), cat);
				sccp_config_softKeySet(v, cat);
			}
		} else {
			pbx_log(LOG_WARNING, "SCCP: sccp.conf section [%s] has type=%s, which is not device, line or softkeyset; section skipped\n", cat, utype);
		}
	}
	sccp_config_add_default_softkeyset();

#ifdef CS_SCCP_REALTIME
	sccp_configurationchange_t res = SCCP_CONFIG_NOUPDATENEEDED;
	PBX_VARIABLE_TYPE *        rv  = NULL;

	sccp_line_t * l = NULL;
	SCCP_RWLIST_RDLOCK(&GLOB(lines));
	SCCP_RWLIST_TRAVERSE(&GLOB(lines), l, list) {
		AUTO_RELEASE(sccp_line_t, line, sccp_line_retain(l));
		if (line) {
			do {
				if (line->realtime == TRUE && line != GLOB(hotline)->line) {
					sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_3 "%s: reloading realtime line\n", line->name);
					rv = pbx_load_realtime(GLOB(realtimelinetable), "name", line->name, NULL);
					if (!rv) {
						sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_3 "%s: realtime line no longer exists; marked for removal\n", line->name);
						line->pendingDelete = 1;
						break;
					}
					line->pendingDelete = 0;

					res = sccp_config_applyLineConfiguration(line, rv);
					if (GLOB(reload_in_progress) && res & SCCP_CONFIG_NEEDDEVICERESET) {
						line->pendingUpdate = 1;
					} else {
						line->pendingUpdate = 0;
					}
					pbx_variables_destroy(rv);
				}
			} while (0);
		}
	}
	SCCP_RWLIST_UNLOCK(&GLOB(lines));

	SCCP_RWLIST_RDLOCK(&GLOB(devices));
	SCCP_RWLIST_TRAVERSE(&GLOB(devices), d, list) {
		AUTO_RELEASE(sccp_device_t, device, sccp_device_retain(d));
		if (device) {
			do {
				if (device->realtime == TRUE) {
					sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_3 "%s: reloading realtime device\n", device->id);
					rv = pbx_load_realtime(GLOB(realtimedevicetable), "name", device->id, NULL);
					if (!rv) {
						sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_3 "%s: realtime device no longer exists; marked for removal\n", device->id);
						device->pendingDelete = 1;
						break;
					}
					device->pendingDelete = 0;

					res = sccp_config_applyDeviceConfiguration(device, rv);
					if (GLOB(reload_in_progress) && res & SCCP_CONFIG_NEEDDEVICERESET) {
						device->pendingUpdate = 1;
					} else {
						device->pendingUpdate = 0;
					}
					pbx_variables_destroy(rv);
				}
			} while (0);
		}
	}
	SCCP_RWLIST_UNLOCK(&GLOB(devices));
#endif

	if (GLOB(reload_in_progress) && GLOB(pendingUpdate)) {
		sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_2 "[general] setting changed that needs a restart; restarting all devices\n");

		SCCP_RWLIST_RDLOCK(&GLOB(devices));
		SCCP_RWLIST_TRAVERSE(&GLOB(devices), d, list) {
			if (d->realtime) {
				d->pendingDelete = 1;
			} else if (!d->pendingDelete && !d->pendingUpdate) {
				d->pendingUpdate = 1;
			}
		}
		SCCP_RWLIST_UNLOCK(&GLOB(devices));
	} else {
		GLOB(pendingUpdate) = 0;
	}
	GLOB(pendingUpdate) = 0;

	sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_1 "checking read type\n");
	if (readingtype == SCCP_CONFIG_READRELOAD) {
		/* IMPORTANT: The line_post_reload function may change the pendingUpdate field of
		 * devices, so it's really important to call it *before* calling device_post_real().
		 */
		sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_2 "lines: after reload\n");
		sccp_line_post_reload();
		sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_2 "devices: after reload\n");
		sccp_device_post_reload();
		sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_2 "softkey sets: after reload\n");
		sccp_softkey_post_reload();
	}
	return TRUE;
}

sccp_configurationchange_t sccp_config_applyLineConfiguration(linePtr l, PBX_VARIABLE_TYPE * v)
{
	unsigned int        res                                          = SCCP_CONFIG_NOUPDATENEEDED;
	boolean_t           SetEntries[ARRAY_LEN(sccpLineConfigOptions)] = { FALSE };
	PBX_VARIABLE_TYPE * cat_root                                     = v;
	if (!l) {
		pbx_log(LOG_ERROR, "SCCP: sccp_config_applyLineConfiguration() was called without a line (caller bug)\n");
		return SCCP_CONFIG_ERROR;
	}

	for (; v; v = v->next) {
		res |= sccp_config_object_setValue(l, cat_root, v->name, v->value, v->lineno, SCCP_CONFIG_LINE_SEGMENT, SetEntries, FALSE);
	}

	l->preferences_set_on_line_level = (l->preferences.audio[0] != SKINNY_CODEC_NONE) ? TRUE : FALSE;

	sccp_config_set_defaults(l, SCCP_CONFIG_LINE_SEGMENT, SetEntries);

	if (sccp_strlen_zero(l->id)) {
		snprintf(l->id, sizeof(l->id), "%04d", SCCP_LIST_GETSIZE(&GLOB(lines)));
	}
	if (sccp_strlen_zero(l->label)) {
		sccp_free(l->label);
		l->label = pbx_strdup(!sccp_strlen_zero(l->cid_name) ? l->cid_name : l->name);
	}

	return (sccp_configurationchange_t)res;
}

sccp_configurationchange_t sccp_config_applyDeviceConfiguration(devicePtr d, PBX_VARIABLE_TYPE * v)
{
	unsigned int        res                                            = SCCP_CONFIG_NOUPDATENEEDED;
	boolean_t           SetEntries[ARRAY_LEN(sccpDeviceConfigOptions)] = { FALSE };
	PBX_VARIABLE_TYPE * cat_root                                       = v;
	if (!d) {
		pbx_log(LOG_ERROR, "SCCP: sccp_config_applyDeviceConfiguration() was called without a device (caller bug)\n");
		return SCCP_CONFIG_ERROR;
	}

	if (d->pendingDelete) {
		sccp_dev_clean_restart(d, FALSE);
	}
	for (; v; v = v->next) {
		res |= sccp_config_object_setValue(d, cat_root, v->name, v->value, v->lineno, SCCP_CONFIG_DEVICE_SEGMENT, SetEntries, FALSE);
	}

	sccp_config_set_defaults(d, SCCP_CONFIG_DEVICE_SEGMENT, SetEntries);

	if (d->keepalive < SCCP_MIN_KEEPALIVE) {
		d->keepalive = SCCP_MIN_KEEPALIVE;
	}
	return (sccp_configurationchange_t)res;
}

sccp_configurationchange_t sccp_config_setDeviceOption(devicePtr d, const char * name, const char * value)
{
	const SCCPConfigOption * option = sccp_find_config(SCCP_CONFIG_DEVICE_SEGMENT, name);
	if (!d || !option || !value || (option->flags & (SCCP_CONFIG_FLAG_IGNORE | SCCP_CONFIG_FLAG_OBSOLETE | SCCP_CONFIG_FLAG_MULTI_ENTRY))) {
		return SCCP_CONFIG_ERROR;
	}
	PBX_VARIABLE_TYPE * v = ast_variable_new(name, value, "cli");
	if (!v) {
		return SCCP_CONFIG_ERROR;
	}
	sccp_configurationchange_t res = sccp_config_object_setValue(d, v, name, value, 0, SCCP_CONFIG_DEVICE_SEGMENT, NULL, FALSE);
	ast_variables_destroy(v);
	if (d->keepalive < SCCP_MIN_KEEPALIVE) {
		d->keepalive = SCCP_MIN_KEEPALIVE;
	}
	return res;
}

sccp_config_file_status_t sccp_config_getConfig(boolean_t force, const char * const filename)
{
	struct ast_flags config_flags = { force ? 0 : CONFIG_FLAG_FILEUNCHANGED };
	const char * newfilename = "sccp.conf";
	if (filename && !sccp_strlen_zero(filename)) {
		newfilename = pbx_strdupa(filename);
	} else if (GLOB(config_file_name) && !sccp_strlen_zero(GLOB(config_file_name))) {
		newfilename = pbx_strdupa(GLOB(config_file_name));
	}

	/* load into a local config; GLOB(cfg) and the file name are only replaced when the new file is usable */
	struct ast_config * newcfg = pbx_config_load(newfilename, "chan_sccp", config_flags);
	if (newcfg == CONFIG_STATUS_FILEUNCHANGED) {
		if (GLOB(cfg)) {
			sccp_log(DEBUGCAT_CORE)(VERBOSE_PREFIX_3 "%s has not changed; not reloaded\n", newfilename);
			return CONFIG_STATUS_FILE_NOT_CHANGED;
		}
		pbx_clear_flag(&config_flags, CONFIG_FLAG_FILEUNCHANGED);
		newcfg = pbx_config_load(newfilename, "chan_sccp", config_flags);
	}
	if (newcfg == CONFIG_STATUS_FILEMISSING) {
		pbx_log(LOG_ERROR, "SCCP: config file '%s' not found; (re)load aborted, current configuration kept\n", newfilename);
		return CONFIG_STATUS_FILE_NOT_FOUND;
	}
	if (newcfg == CONFIG_STATUS_FILEINVALID || newcfg == CONFIG_STATUS_FILEUNCHANGED) {
		pbx_log(LOG_ERROR, "SCCP: config file '%s' could not be parsed; (re)load aborted, current configuration kept\n", newfilename);
		return CONFIG_STATUS_FILE_INVALID;
	}
	if (ast_variable_browse(newcfg, "devices")) {
		pbx_log(LOG_ERROR, "SCCP: '%s' uses the old format with a [devices] section, which is no longer supported; (re)load aborted, current configuration kept\n", newfilename);
		pbx_config_destroy(newcfg);
		return CONFIG_STATUS_FILE_OLD;
	}
	if (!ast_variable_browse(newcfg, "general")) {
		pbx_log(LOG_ERROR, "SCCP: '%s' has no [general] section; (re)load aborted, current configuration kept\n", newfilename);
		pbx_config_destroy(newcfg);
		return CONFIG_STATUS_FILE_NOT_SCCP;
	}

	if (GLOB(cfg)) {
		pbx_config_destroy(GLOB(cfg));
	}
	GLOB(cfg) = newcfg;
	char * previous_name = GLOB(config_file_name);
	GLOB(config_file_name) = pbx_strdup(newfilename);
	if (previous_name) {
		sccp_free(previous_name);
	}
	sccp_log(DEBUGCAT_CORE)(VERBOSE_PREFIX_3 "%s loaded\n", newfilename);
	return CONFIG_STATUS_FILE_OK;
}

static const struct softkeyConfigurationTemplate {
	const char configVar[16];
	const int  softkey;
} softKeyTemplate[] = {
	/* clang-format off */
	{"redial", 			SKINNY_LBL_REDIAL},
	{"newcall", 			SKINNY_LBL_NEWCALL},
	{"cfwdall", 			SKINNY_LBL_CFWDALL},
	{"cfwdbusy", 			SKINNY_LBL_CFWDBUSY},
	{"cfwdnoanswer",		SKINNY_LBL_CFWDNOANSWER},
	{"dnd", 			SKINNY_LBL_DND},
	{"hold", 			SKINNY_LBL_HOLD},
	{"endcall", 			SKINNY_LBL_ENDCALL},
	{"idivert", 			SKINNY_LBL_IDIVERT},
	{"resume", 			SKINNY_LBL_RESUME},
	{"newcall", 			SKINNY_LBL_NEWCALL},
	{"transfer", 			SKINNY_LBL_TRANSFER},
	{"answer", 			SKINNY_LBL_ANSWER},
	{"transvm", 			SKINNY_LBL_TRNSFVM},
	{"private", 			SKINNY_LBL_PRIVATE},
	{"meetme", 			SKINNY_LBL_MEETME},
	{"barge", 			SKINNY_LBL_BARGE},
	{"back", 			SKINNY_LBL_BACKSPACE},
	{"intrcpt", 			SKINNY_LBL_INTRCPT},
	{"monitor", 			SKINNY_LBL_MONITOR},
	{"dial", 			SKINNY_LBL_DIAL},
#ifndef CS_ADV_FEATURES
	{"callback",			SKINNY_LBL_CALLBACK},
	{"trnsfvm",			SKINNY_LBL_TRNSFVM},
	{"cbarge", 			SKINNY_LBL_CBARGE},
#endif
#ifdef CS_SCCP_VIDEO
	{"vidmode", 			SKINNY_LBL_VIDEO_MODE},
#else
	{"vidmode", 			-1},
#endif
#ifdef CS_SCCP_PICKUP
	{"pickup", 			SKINNY_LBL_PICKUP},
	{"gpickup", 			SKINNY_LBL_GPICKUP},
#else
	{"pickup", 			-1},
	{"gpickup", 			-1},
#endif
#ifdef CS_SCCP_PARK
	{"park", 			SKINNY_LBL_PARK},
#else
	{"park", 			-1},
#endif
#ifdef CS_SCCP_DIRTRFR
	{"select", 			SKINNY_LBL_SELECT},
	{"dirtrfr", 			SKINNY_LBL_DIRTRFR},
#else
	{"select", 			-1},
	{"dirtrfr", 			-1},
#endif
#ifdef CS_SCCP_CONFERENCE
	{"conf", 			SKINNY_LBL_CONFRN},
	{"confrn",			SKINNY_LBL_CONFRN},
	{"join", 			SKINNY_LBL_JOIN},
	{"conflist", 			SKINNY_LBL_CONFLIST},
#else
	{"conf", 			-1},
	{"confrn",			-1},
	{"join",			-1},
	{"conflist", 			-1},
#endif
	{"empty", 			SKINNY_LBL_EMPTY},
	/* clang-format on */
};

static int sccp_config_getSoftkeyLbl(char * key)
{
	size_t i = 0;
	for (i = 0; i < ARRAY_LEN(softKeyTemplate); i++) {
		if (sccp_strcaseequals(softKeyTemplate[i].configVar, key)) {
			return softKeyTemplate[i].softkey;
		}
	}
	sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_3 "softkey %s not defined\n", key);
	return SKINNY_LBL_EMPTY;
}

static uint8_t sccp_config_readSoftKeySet(uint8_t * softkeyset, const char * data)
{
	if (!data) {
		return 0;
	}
	int i = 0;

	int j       = 0;
	int softkey = 0;

	char * labels    = pbx_strdupa(data);
	char * labelrest = NULL;
	char   delims[]  = ",";

	char * label = strtok_r(labels, delims, &labelrest);
	while (label) {
		label = pbx_strip(label);
		if ((softkey = sccp_config_getSoftkeyLbl(label)) != -1 && (i + 1) < StationMaxSoftKeySetDefinition) {
			softkeyset[i++] = softkey;
		}
		label = strtok_r(NULL, delims, &labelrest);
	}
	for (j = i; j < StationMaxSoftKeySetDefinition; j++) {
		softkeyset[j] = SKINNY_LBL_EMPTY;
	}
	return i;
}

void sccp_config_softKeySet(PBX_VARIABLE_TYPE * variable, const char * name)
{
	int                              keySetSize              = 0;
	sccp_softKeySetConfiguration_t * softKeySetConfiguration = NULL;
	skinny_keymode_t                 keyMode                 = SKINNY_KEYMODE_SENTINEL;

	sccp_log((DEBUGCAT_CONFIG + DEBUGCAT_SOFTKEY))(VERBOSE_PREFIX_3 "reading softkey set %s\n", name);

	SCCP_LIST_LOCK(&softKeySetConfig);
	SCCP_LIST_TRAVERSE(&softKeySetConfig, softKeySetConfiguration, list) {
		if (sccp_strcaseequals(softKeySetConfiguration->name, name)) {
			break;
		}
	}
	SCCP_LIST_UNLOCK(&softKeySetConfig);

	if (!softKeySetConfiguration) {
		softKeySetConfiguration = (sccp_softKeySetConfiguration_t *)sccp_calloc(1, sizeof(sccp_softKeySetConfiguration_t));
		memset(softKeySetConfiguration, 0, sizeof(sccp_softKeySetConfiguration_t));

		sccp_copy_string(softKeySetConfiguration->name, name, sizeof(sccp_softKeySetConfiguration_t));
		softKeySetConfiguration->numberOfSoftKeySets = 0;
		softKeySetConfiguration->softkeyCbMap        = NULL;

		SCCP_LIST_LOCK(&softKeySetConfig);
		SCCP_LIST_INSERT_HEAD(&softKeySetConfig, softKeySetConfiguration, list);
		SCCP_LIST_UNLOCK(&softKeySetConfig);
	}

	while (variable) {
		keyMode = SKINNY_KEYMODE_SENTINEL;
		sccp_log((DEBUGCAT_CONFIG + DEBUGCAT_SOFTKEY))(VERBOSE_PREFIX_3 "softkey set: %s = %s\n", variable->name, variable->value);
		if (sccp_strcaseequals(variable->name, "uriaction")) {
			sccp_log(DEBUGCAT_CONFIG)(VERBOSE_PREFIX_3 "SCCP: URI action softkey %s found\n", variable->value);
			if (!softKeySetConfiguration->softkeyCbMap) {
				softKeySetConfiguration->softkeyCbMap = sccp_softkeyMap_copyStaticallyMapped();
			}

			char * uriactionstr = pbx_strdup(variable->value);
			char * event        = strsep(&uriactionstr, ",");
			if (event && !sccp_strlen_zero(uriactionstr)) {
				sccp_softkeyMap_replaceCallBackByUriAction(softKeySetConfiguration->softkeyCbMap, labelstr2int(event), uriactionstr);
			} else {
				sccp_log(DEBUGCAT_CONFIG)(VERBOSE_PREFIX_3 "SCCP: URI action softkey %s not found, or no URIs given (%s)\n", event, uriactionstr);
			}
			sccp_free(uriactionstr);
		} else if (sccp_strcaseequals(variable->name, "onhook")) {
			keyMode = KEYMODE_ONHOOK;
		} else if (sccp_strcaseequals(variable->name, "connected")) {
			keyMode = KEYMODE_CONNECTED;
		} else if (sccp_strcaseequals(variable->name, "onhold")) {
			keyMode = KEYMODE_ONHOLD;
		} else if (sccp_strcaseequals(variable->name, "ringin")) {
			keyMode = KEYMODE_RINGIN;
		} else if (sccp_strcaseequals(variable->name, "offhook")) {
			keyMode = KEYMODE_OFFHOOK;
		} else if (sccp_strcaseequals(variable->name, "conntrans")) {
			keyMode = KEYMODE_CONNTRANS;
		} else if (sccp_strcaseequals(variable->name, "digitsfoll")) {
			keyMode = KEYMODE_DIGITSFOLL;
		} else if (sccp_strcaseequals(variable->name, "connconf")) {
			keyMode = KEYMODE_CONNCONF;
		} else if (sccp_strcaseequals(variable->name, "ringout")) {
			keyMode = KEYMODE_RINGOUT;
		} else if (sccp_strcaseequals(variable->name, "offhookfeat")) {
			keyMode = KEYMODE_OFFHOOKFEAT;
		} else if (sccp_strcaseequals(variable->name, "inusehint") || sccp_strcaseequals(variable->name, "onhint")) {
			keyMode = KEYMODE_INUSEHINT;
		} else if (sccp_strcaseequals(variable->name, "onhookstealable") || sccp_strcaseequals(variable->name, "onstealable")) {
			keyMode = KEYMODE_ONHOOKSTEALABLE;
		} else if (sccp_strcaseequals(variable->name, "holdconf")) {
			keyMode = KEYMODE_HOLDCONF;
		}

		if (keyMode != SKINNY_KEYMODE_SENTINEL) {
			if (softKeySetConfiguration->numberOfSoftKeySets < (keyMode + 1)) {
				softKeySetConfiguration->numberOfSoftKeySets = keyMode + 1;
			}

			if (softKeySetConfiguration->modes[keyMode].ptr) {
				sccp_free(softKeySetConfiguration->modes[keyMode].ptr);
			}

			uint8_t * softkeyset = (uint8_t *)sccp_calloc(StationMaxSoftKeySetDefinition, sizeof(uint8_t));
			keySetSize           = sccp_config_readSoftKeySet(softkeyset, variable->value);
			if (keySetSize > 0) {
				softKeySetConfiguration->modes[keyMode].id    = keyMode;
				softKeySetConfiguration->modes[keyMode].ptr   = softkeyset;
				softKeySetConfiguration->modes[keyMode].count = keySetSize;
			} else {
				softKeySetConfiguration->modes[keyMode].id    = keyMode;
				softKeySetConfiguration->modes[keyMode].ptr   = NULL;
				softKeySetConfiguration->modes[keyMode].count = 0;
				sccp_free(softkeyset);
			}
		}

		variable = variable->next;
	}
}

static void sccp_config_append_json_string(struct mansession * s, const char * str)
{
	char * out = (char *)sccp_alloca(strlen(str) * 6 + 3);
	char * o   = out;
	*o++       = '"';
	for (; *str; str++) {
		unsigned char ch = (unsigned char)*str;
		if (ch == '"' || ch == '\\') {
			*o++ = '\\';
			*o++ = ch;
		} else if (ch < 0x20) {
			o += snprintf(o, 7, "\\u%04x", ch);
		} else {
			*o++ = ch;
		}
	}
	*o++ = '"';
	*o   = '\0';
	astman_append(s, "%s", out);
}

int sccp_manager_config_metadata(struct mansession * s, const struct message * m)
{
	const SCCPConfigSegment * sccpConfigSegment = NULL;
	int                       total             = 0;
	uint                      i                 = 0;
	const char *              id                = astman_get_header(m, "ActionID");
	const char *              req_segment       = astman_get_header(m, "Segment");
	const char *              req_resultformat  = astman_get_header(m, "ResultFormat");
	uint                      comma             = 0;

	if (sccp_strlen_zero(req_segment)) {
		int sccp_config_revision = 0;

		sscanf(SCCP_CONFIG_REVISION,
		       "$"
		       "Revision: %i"
		       "$",
		       &sccp_config_revision);

		if (sccp_strcaseequals(req_resultformat, "list")) {
			astman_send_listack(s, m, "SCCPConfigMetadata Follows", "Start");
			astman_append(s, "Event: SCCPConfigMetadata\r\n");
		} else if (sccp_strcaseequals(req_resultformat, "command")) {
			astman_append(s, "Response: Follows\r\n");
			astman_append(s, "Privilege: Command\r\n");
		} else {
			astman_append(s, "Response: Success\r\n");
		}
		if (!ast_strlen_zero(id)) {
			astman_append(s, "ActionID: %s\r\n", id);
		}

		astman_append(s, "JSON: {");
		astman_append(s, "\"Name\":\"chan_sccp\",");
		astman_append(s, "\"Version\":\"%s\",", SCCP_VERSION);
		astman_append(s, "\"ConfigRevision\":\"%d\",", sccp_config_revision);
		char * conf_enabled_array[] = {
#ifdef CS_SCCP_PARK
			"park",
#endif
#ifdef CS_SCCP_PICKUP
			"pickup",
#endif
#ifdef CS_SCCP_REALTIME
			"realtime",
#endif
#ifdef CS_SCCP_VIDEO
			"video",
#endif
#ifdef CS_SCCP_CONFERENCE
			"conference",
#endif
#ifdef CS_SCCP_DIRTRFR
			"dirtrfr",
#endif
#ifdef CS_SCCP_FEATURE_MONITOR
			"feature_monitor",
#endif
#ifdef CS_SCCP_FUNCTIONS
			"functions",
#endif
#ifdef CS_MANAGER_EVENTS
			"manager_events",
#endif
#ifdef CS_DEVICESTATE
			"devicestate",
#endif
#ifdef CS_DEVSTATE_FEATURE
			"devstate_feature",
#endif
#ifdef CS_DYNAMIC_SPEEDDIAL
			"dynamic_speeddial",
#endif
#ifdef CS_DYNAMIC_SPEEDDIAL_CID
			"dynamic_speeddial_cid",
#endif
#ifdef CS_EXPERIMENTAL
			"experimental",
#endif
#if DEBUG
			"debug",
#endif
		};
		comma = 0;
		astman_append(s, "\"ConfigureEnabled\": [");
		for (i = 0; i < ARRAY_LEN(conf_enabled_array); i++) {
			astman_append(s, "%s\"%s\"", comma ? "," : "", conf_enabled_array[i]);
			comma = 1;
		}
		astman_append(s, "],");

		comma = 0;
		astman_append(s, "\"Segments\":[");
		for (i = 0; i < ARRAY_LEN(sccpConfigSegments); i++) {
			astman_append(s, "%s", comma ? "," : "");
			astman_append(s, "\"%s\"", sccpConfigSegments[i].name);
			comma = 1;
		}
		astman_append(s, "]}\r\n");
		total++;
		if (sccp_strcaseequals(req_resultformat, "list")) {
			astman_append(s,
				      "\r\nEvent: SCCPConfigMetadataComplete\r\n"
				      "EventList: Complete\r\n"
				      "ListItems: %d\r\n\r\n",
				      total);
		} else if (sccp_strcaseequals(req_resultformat, "command")) {
			astman_append(s, "--END COMMAND--\r\n");
		}
		astman_append(s, "\r\n");
	} else {
		/*
		   JSON:
		   {
		   "Segment": "general",
		   "Options": [
		   {
		   Option: config->name,
		   Type: ....,
		   Flags : [Required, Deprecated, Obsolete, MultiEntry, RestartRequiredOnUpdate],
		   DefaultValue: ...,
		   Description: ....
		   },
		   {
		   ...
		   }
		   ]
		   }
		 */
		for (i = 0; i < ARRAY_LEN(sccpConfigSegments); i++) {
			if (sccp_strcaseequals(sccpConfigSegments[i].name, req_segment)) {
				sccpConfigSegment               = &sccpConfigSegments[i];
				const SCCPConfigOption * config = sccpConfigSegment->config;

				if (sccp_strcaseequals(req_resultformat, "list")) {
					astman_send_listack(s, m, "SCCPConfigMetadata Follows", "Start");
					astman_append(s, "Event: SCCPConfigMetadata\r\n");
				} else if (sccp_strcaseequals(req_resultformat, "command")) {
					astman_append(s, "Response: Follows\r\n");
				} else {
					astman_append(s, "Response: Success\r\n");
				}
				if (!ast_strlen_zero(id)) {
					astman_append(s, "ActionID: %s\r\n", id);
				}
				astman_append(s, "JSON: {");
				astman_append(s, "\"Segment\":\"%s\",", sccpConfigSegment->name);
				astman_append(s, "\"Options\":[");
				comma = 0;

				for (long unsigned int cur_elem = 0; cur_elem < sccpConfigSegment->config_size; cur_elem++) {
					if ((config[cur_elem].flags & SCCP_CONFIG_FLAG_IGNORE) != SCCP_CONFIG_FLAG_IGNORE) {
						astman_append(s, "%s", comma ? "," : "");
						astman_append(s, "{");

						{
							astman_append(s, "\"Name\":\"%s\",", config[cur_elem].name);

							switch (config[cur_elem].type) {
								case SCCP_CONFIG_DATATYPE_BOOLEAN:
									astman_append(s, "\"Type\":\"BOOLEAN\",");
									astman_append(s, "\"Size\":%d", (int)config[cur_elem].size - 1);
									break;
								case SCCP_CONFIG_DATATYPE_INT:
									astman_append(s, "\"Type\":\"INT\",");
									astman_append(s, "\"Size\":%d", (int)config[cur_elem].size - 1);
									break;
								case SCCP_CONFIG_DATATYPE_UINT:
									astman_append(s, "\"Type\":\"UNSIGNED INT\",");
									astman_append(s, "\"Size\":%d", (int)config[cur_elem].size - 1);
									break;
								case SCCP_CONFIG_DATATYPE_STRINGPTR:
									astman_append(s, "\"Type\":\"STRING\",");
									astman_append(s, "\"Size\":0");
									break;
								case SCCP_CONFIG_DATATYPE_STRING:
									astman_append(s, "\"Type\":\"STRING\",");
									astman_append(s, "\"Size\":%d", (int)config[cur_elem].size - 1);
									break;
								case SCCP_CONFIG_DATATYPE_PARSER:
									astman_append(s, "\"Type\":\"PARSER\",");
									astman_append(s, "\"Size\":0,");
									astman_append(s, "\"Parser\":\"%s\"", config[cur_elem].parsername);
									break;
								case SCCP_CONFIG_DATATYPE_CHAR:
									astman_append(s, "\"Type\":\"CHAR\",");
									astman_append(s, "\"Size\":1");
									break;
								case SCCP_CONFIG_DATATYPE_ENUM:
									astman_append(s, "\"Type\":\"ENUM\",");
									astman_append(s, "\"Size\":%d,", (int)config[cur_elem].size - 1);
									char * all_entries    = pbx_strdupa(config[cur_elem].all_entries());
									char * possible_entry = "";

									int subcomma = 0;
									astman_append(s, "\"PossibleValues\":[");
									while (all_entries && (possible_entry = strsep(&all_entries, ","))) {
										astman_append(s, "%s", subcomma ? "," : "");
										sccp_config_append_json_string(s, possible_entry);
										subcomma = 1;
									}
									astman_append(s, "]");
									break;
							}
							astman_append(s, ",");

							if ((config[cur_elem].flags & (SCCP_CONFIG_FLAG_REQUIRED | SCCP_CONFIG_FLAG_DEPRECATED | SCCP_CONFIG_FLAG_OBSOLETE | SCCP_CONFIG_FLAG_MULTI_ENTRY)) > 0
							    || (config[cur_elem].change & SCCP_CONFIG_NEEDDEVICERESET) == SCCP_CONFIG_NEEDDEVICERESET) {
								astman_append(s, "\"Flags\":[");
								{
									int comma1 = 0;

									if ((config[cur_elem].flags & SCCP_CONFIG_FLAG_REQUIRED) == SCCP_CONFIG_FLAG_REQUIRED) {
										astman_append(s, "\"Required\"");
										comma1 = 1;
									}
									if ((config[cur_elem].flags & SCCP_CONFIG_FLAG_DEPRECATED) == SCCP_CONFIG_FLAG_DEPRECATED) {
										astman_append(s, "%s", comma1 ? "," : "");
										astman_append(s, "\"Deprecated\"");
										comma1 = 1;
									}
									if ((config[cur_elem].flags & SCCP_CONFIG_FLAG_OBSOLETE) == SCCP_CONFIG_FLAG_OBSOLETE) {
										astman_append(s, "%s", comma1 ? "," : "");
										astman_append(s, "\"Obsolete\"");
										comma1 = 1;
									}
									if ((config[cur_elem].flags & SCCP_CONFIG_FLAG_MULTI_ENTRY) == SCCP_CONFIG_FLAG_MULTI_ENTRY) {
										astman_append(s, "%s", comma1 ? "," : "");
										astman_append(s, "\"MultiEntry\"");
										comma1 = 1;
									}
									if ((config[cur_elem].change & SCCP_CONFIG_NEEDDEVICERESET) == SCCP_CONFIG_NEEDDEVICERESET) {
										astman_append(s, "%s", comma1 ? "," : "");
										astman_append(s, "\"RestartRequiredOnUpdate\"");
										comma1 = 1;
									}
								}
								astman_append(s, "],");
							}

							astman_append(s, "\"DefaultValue\":");
							if (config[cur_elem].defaultValue) {
								sccp_config_append_json_string(s, config[cur_elem].defaultValue);
							} else {
								astman_append(s, "null");
							}

							if (!sccp_strlen_zero(config[cur_elem].description)) {
								char * description      = pbx_strdupa(config[cur_elem].description);
								char * description_part = "";
								int    comma2           = 0;

								astman_append(s, ",\"Description\":[");
								while (description && (description_part = strsep(&description, "\n")) && !sccp_strlen_zero(description_part)) {
									astman_append(s, "%s", comma2++ ? "," : "");
									sccp_config_append_json_string(s, description_part);
								}
								astman_append(s, "]");
							}
						}
						astman_append(s, "}");
						comma = 1;
					}
				}
				astman_append(s, "]}\r\n");
				total++;
				if (sccp_strcaseequals(req_resultformat, "list")) {
					astman_append(s,
						      "\r\nEvent: SCCPConfigMetadataComplete\r\n"
						      "EventList: Complete\r\n"
						      "ListItems: %d\r\n\r\n",
						      total);
				} else if (sccp_strcaseequals(req_resultformat, "command")) {
					astman_append(s, "--END COMMAND--\r\n"
							 "DataType: JSON\r\n"
							 "Privilege: Command\r\n");
				}
				astman_append(s, "\r\n");
			}
		}
	}
	return 0;
}

void sccp_config_generate_path(char * fn, size_t size, const char * filename)
{
	if (filename[0] == '/') {
		snprintf(fn, size, "%s", filename);
	} else {
		snprintf(fn, size, "%s/%s", ast_config_AST_CONFIG_DIR, filename);
	}
}

static int _config_generate_wiki(char * filename)
{
	const SCCPConfigSegment * sccpConfigSegment = NULL;
	const SCCPConfigOption *  config            = NULL;
	long unsigned int         sccp_option       = 0;
	long unsigned int         segment           = 0;
	char *                    description       = "";
	char *                    description_part  = "";
	char                      fn[PATH_MAX];

	sccp_config_generate_path(fn, sizeof(fn), filename);

	int fd = open(fn, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
	if (fd == -1) {
		return -1;
	}
	FILE * f = fdopen(fd, "w+");
	if (!f) {
		int err = errno;
		close(fd);
		errno = err;
		return -2;
	}

	char           date[256] = "";
	struct ast_tm  tm;
	struct timeval now = ast_tvnow();
	ast_strftime(date, sizeof(date), "%b %e %T", ast_localtime(&now, &tm, NULL));

	fprintf(f, "*sccp.conf options*\n\n");
	for (segment = SCCP_CONFIG_GLOBAL_SEGMENT; segment <= SCCP_CONFIG_SOFTKEY_SEGMENT; segment++) {
		sccpConfigSegment = sccp_find_segment((sccp_config_segment_t)segment);
		if (!sccpConfigSegment) {
			pbx_log(LOG_ERROR, "SCCP: config segment %d does not exist (caller bug)\n", (int)segment);
			fclose(f); /* also closes fd */
			errno = EINVAL;
			return -3;
		}

		fprintf(f, "\n**[%s] section**\n\n", sccpConfigSegment->name);
		fprintf(f, "<table>\n");
		fprintf(f, "<tr><td><b>parameter</b></td><td><b>default</b></td><td><b>format</b></td><td><b>required</b></td><td><b>status</b></td></tr>\n");
		fprintf(f, "<tr><td></td><td colspan='4'><b>description</b></td></tr>\n");
		config = sccpConfigSegment->config;
		for (sccp_option = 0; sccp_option < sccpConfigSegment->config_size; sccp_option++) {
			sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_2 "adding %s, default %s\n", config[sccp_option].name, config[sccp_option].defaultValue);
			if (!sccp_strlen_zero(config[sccp_option].name)) {
				char   delims[]            = "|";
				char * option_name_tokens  = pbx_strdup(config[sccp_option].name);
				char * option_value_tokens = NULL;
				if (!sccp_strlen_zero(config[sccp_option].defaultValue)) {
					option_value_tokens = pbx_strdup(config[sccp_option].defaultValue);
				} else {
					option_value_tokens = pbx_strdup("\"\"");
				}
				char * option_name_tokens_saveptr  = NULL;
				char * option_value_tokens_saveptr = NULL;
				char * option_name                 = strtok_r(option_name_tokens, delims, &option_name_tokens_saveptr);
				char * option_value                = strtok_r(option_value_tokens, delims, &option_value_tokens_saveptr);
				while (option_name != NULL) {
					fprintf(f, "<tr class=option_row id='%s'>\n", option_name);
					fprintf(f, "<td class='name'>%s</td><td class='default_value'>%s</td>\n", option_name, option_value);
					option_name  = strtok_r(NULL, delims, &option_name_tokens_saveptr);
					option_value = strtok_r(NULL, delims, &option_value_tokens_saveptr);
					switch (config[sccp_option].type) {
						case SCCP_CONFIG_DATATYPE_STRING:
							fprintf(f, "<td class='format'>max length:%d</td>\n", (int)config[sccp_option].size - 1);
							break;
						case SCCP_CONFIG_DATATYPE_ENUM:
							{
								char * all_entries    = pbx_strdup(config[sccp_option].all_entries());
								char * possible_entry = "";
								int    subcomma       = 0;

								fprintf(f, "<td class='format'><small>potential values:[");
								while (all_entries && (possible_entry = strsep(&all_entries, ","))) {
									fprintf(f, "%s%s", subcomma ? ", " : "", possible_entry);
									subcomma = 1;
								}
								fprintf(f, "]</small></td>\n");
								sccp_free(all_entries);
							}
							break;
						default:
							fprintf(f, "<td class='format'>-</td>\n");
							break;
					}
					fprintf(f, "<td class='required'>%s</td>\n", (config[sccp_option].flags & SCCP_CONFIG_FLAG_REQUIRED) == SCCP_CONFIG_FLAG_REQUIRED ? "yes" : "-");
					fprintf(f, "<td class='status'>%s</td> ",
						(config[sccp_option].flags & SCCP_CONFIG_FLAG_DEPRECATED) == SCCP_CONFIG_FLAG_DEPRECATED ? "deprecated"
						: (config[sccp_option].flags & SCCP_CONFIG_FLAG_OBSOLETE) == SCCP_CONFIG_FLAG_OBSOLETE   ? "obsolete"
																	 : "-");
					fprintf(f, "</tr>\n");
					if (!sccp_strlen_zero(config[sccp_option].description)) {
						fprintf(f, "<tr class='descr_row'><td></td>\n");
						fprintf(f, "<td class='description' id='%s' colspan='4'><small>", config[sccp_option].name);
						description = pbx_strdup(config[sccp_option].description);
						while ((description_part = strsep(&description, "\n"))) {
							if (!sccp_strlen_zero(description_part)) {
								fprintf(f, "%s.<br>", description_part);
							}
						}
						if (description_part) {
							sccp_free(description_part);
						}
						fprintf(f, "</small></td>\n");
						sccp_free(description);
					}
				}
				fprintf(f, "</tr>\n");
				sccp_free(option_name_tokens);
				sccp_free(option_value_tokens);
			} else {
				pbx_log(LOG_ERROR, "SCCP: out of memory while writing option %s; '%s' is incomplete\n", config[sccp_option].name, fn);
				fclose(f); /* also closes fd */
				return 2;
			}
		}
		fprintf(f, "</table><br>\n");
	}
	fclose(f); /* also closes fd */
	sccp_log(DEBUGCAT_CONFIG)(VERBOSE_PREFIX_2 "SCCP: wrote option reference to '%s'\n", fn);

	return 0;
};

int sccp_config_generate(char * filename, int configType)
{
	if (configType == 3) {
		return _config_generate_wiki(filename);
	}
	const SCCPConfigSegment * sccpConfigSegment   = NULL;
	const SCCPConfigOption *  config              = NULL;
	long unsigned int         sccp_option         = 0;
	long unsigned int         segment             = 0;
	char *                    description         = "";
	char *                    description_part    = "";
	char                      name_and_value[100] = "";
	char                      size_str[15]        = "";
	int                       linelen             = 0;
	struct ast_str *          extra_info          = pbx_str_alloca(DEFAULT_PBX_STR_BUFFERSIZE * 3);

	char fn[PATH_MAX];

	sccp_config_generate_path(fn, sizeof(fn), filename);

	int fd = open(fn, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
	if (fd == -1) {
		return -1;
	}
	FILE * f = fdopen(fd, "w+");
	if (!f) {
		int err = errno;
		close(fd);
		errno = err;
		return -2;
	}

	char           date[256] = "";
	struct ast_tm  tm;
	struct timeval now = ast_tvnow();
	ast_strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", ast_localtime(&now, &tm, NULL));

	fprintf(f, ";!\n");
	fprintf(f, ";! Generated by 'sccp config generate' on %s\n", date);
	fprintf(f, ";! File: %s\n", fn);
	fprintf(f, ";!\n");
	fprintf(f, ";! Every sccp.conf option with its default value, as a reference.\n");
	fprintf(f, ";! Empty values are placeholders, not working settings.\n");
	fprintf(f, ";!\n");
	fprintf(f, "\n");

	for (segment = SCCP_CONFIG_GLOBAL_SEGMENT; segment <= SCCP_CONFIG_SOFTKEY_SEGMENT; segment++) {
		sccpConfigSegment = sccp_find_segment((sccp_config_segment_t)segment);
		if (!sccpConfigSegment) {
			pbx_log(LOG_ERROR, "SCCP: config segment %d does not exist (caller bug)\n", (int)segment);
			fclose(f); /* also closes fd */
			errno = EINVAL;
			return -3;
		}
		if (configType == 0 && (segment == SCCP_CONFIG_DEVICE_SEGMENT || segment == SCCP_CONFIG_LINE_SEGMENT)) {
			sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_2 "adding [%s] template section\n", sccpConfigSegment->name);
			fprintf(f, "\n;\n; %s section\n;\n[default_%s](!)\n", sccpConfigSegment->name, sccpConfigSegment->name);
		} else if (configType == 0 && segment == SCCP_CONFIG_SOFTKEY_SEGMENT) {
			sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_2 "adding [%s] section\n", sccpConfigSegment->name);
			fprintf(f, "\n;\n; %s section\n;\n;[mysoftkeyset]\n", sccpConfigSegment->name);
		} else {
			sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_2 "adding [%s] section\n", sccpConfigSegment->name);
			fprintf(f, "\n;\n; %s section\n;\n[%s]\n", sccpConfigSegment->name, sccpConfigSegment->name);
		}

		config = sccpConfigSegment->config;
		for (sccp_option = 0; sccp_option < sccpConfigSegment->config_size; sccp_option++) {
			if ((config[sccp_option].flags & (SCCP_CONFIG_FLAG_IGNORE | SCCP_CONFIG_FLAG_DEPRECATED | SCCP_CONFIG_FLAG_OBSOLETE)) == 0) {
				sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_2 "adding %s, default %s\n", config[sccp_option].name, config[sccp_option].defaultValue);

				if (!sccp_strlen_zero(config[sccp_option].name)) {
					if (!sccp_strlen_zero(config[sccp_option].defaultValue)
					    || (configType != 2
						&& ((config[sccp_option].flags & SCCP_CONFIG_FLAG_REQUIRED) != SCCP_CONFIG_FLAG_REQUIRED
						    && sccp_strlen_zero(config[sccp_option].defaultValue)))                                        // empty but required
					) {
						if (strstr(config[sccp_option].name, "|")) {
							char   delims[]            = "|";
							char * option_name_tokens  = pbx_strdup(config[sccp_option].name);
							char * option_value_tokens = NULL;
							if (!sccp_strlen_zero(config[sccp_option].defaultValue)) {
								option_value_tokens = pbx_strdup(config[sccp_option].defaultValue);
							} else {
								option_value_tokens = pbx_strdup("\"\"");
							}
							char * option_name_tokens_saveptr  = NULL;
							char * option_value_tokens_saveptr = NULL;
							char * option_name                 = strtok_r(option_name_tokens, delims, &option_name_tokens_saveptr);
							char * option_value                = strtok_r(option_value_tokens, delims, &option_value_tokens_saveptr);
							while (option_name != NULL) {
								snprintf(name_and_value, sizeof(name_and_value), "%s = %s", option_name, option_value ? option_value : "\"\"");
								fprintf(f, "%s", name_and_value);
								option_name  = strtok_r(NULL, delims, &option_name_tokens_saveptr);
								option_value = strtok_r(NULL, delims, &option_value_tokens_saveptr);
								if (option_name) {
									fprintf(f, "\n");
								}
							}
							sccp_free(option_name_tokens);
							sccp_free(option_value_tokens);
						} else {
							snprintf(name_and_value, sizeof(name_and_value), "%s%s = %s", !sccp_strlen_zero(config[sccp_option].defaultValue) ? ";" : "", config[sccp_option].name,
								 sccp_strlen_zero(config[sccp_option].defaultValue) ? "\"\"" : config[sccp_option].defaultValue);
							fprintf(f, "%s", name_and_value);
						}
						linelen = (int)strlen(name_and_value);
						switch (config[sccp_option].type) {
							case SCCP_CONFIG_DATATYPE_STRING:
								snprintf(size_str, sizeof(size_str), "(SIZE: %d) ", (int)config[sccp_option].size - 1);
								break;
							case SCCP_CONFIG_DATATYPE_ENUM:
								{
									char * all_entries    = pbx_strdup(config[sccp_option].all_entries());
									char * possible_entry = "";
									int    subcomma       = 0;

									pbx_str_append(&extra_info, 0, "(POSSIBLE VALUES: [");
									while (all_entries && (possible_entry = strsep(&all_entries, ","))) {
										pbx_str_append(&extra_info, 0, "%s\"%s\"", subcomma ? "," : "", possible_entry);
										subcomma = 1;
									}
									pbx_str_append(&extra_info, 0, "])");
									sccp_free(all_entries);
								}
								size_str[0] = '\0';
								break;
							default:
								size_str[0] = '\0';
								break;
						}
						fprintf(f, "%*.s ; %s%s%s%s%s", 81 - linelen, " ", ((config[sccp_option].flags & SCCP_CONFIG_FLAG_REQUIRED) == SCCP_CONFIG_FLAG_REQUIRED) ? "(REQUIRED) " : "",
							((config[sccp_option].flags & SCCP_CONFIG_FLAG_MULTI_ENTRY) == SCCP_CONFIG_FLAG_MULTI_ENTRY) ? "(MULTI-ENTRY) " : "",
							((config[sccp_option].flags & SCCP_CONFIG_FLAG_DEPRECATED) == SCCP_CONFIG_FLAG_DEPRECATED) ? "(DEPRECATED) " : "",
							((config[sccp_option].flags & SCCP_CONFIG_FLAG_OBSOLETE) == SCCP_CONFIG_FLAG_OBSOLETE) ? "(DEPRECATED) " : "", size_str);
						if (!sccp_strlen_zero(config[sccp_option].description)) {
							description = pbx_strdup(config[sccp_option].description);
							while ((description_part = strsep(&description, "\n"))) {
								if (!sccp_strlen_zero(description_part)) {
									if (linelen) {
										fprintf(f, "%s\n", description_part);
									} else {
										fprintf(f, "%*.s ; %s\n", 81, " ", description_part);
									}
									linelen = 0;
								}
							}
							if (description_part) {
								sccp_free(description_part);
							}
							sccp_free(description);
						} else {
							fprintf(f, "\n");
						}
						if (ast_str_strlen(extra_info)) {
							fprintf(f, "%*.s ; %s\n", 81, " ", pbx_str_buffer(extra_info));
							ast_str_reset(extra_info);
						}
					}
				} else {
					pbx_log(LOG_ERROR, "SCCP: out of memory while writing option %s; '%s' is incomplete\n", config[sccp_option].name, fn);
					fclose(f); /* also closes fd */
					return 2;
				}
			}
		}
		sccp_log((DEBUGCAT_CONFIG))("\n");
	}
	fclose(f); /* also closes fd */
	sccp_log(DEBUGCAT_CONFIG)(VERBOSE_PREFIX_2 "SCCP: wrote example configuration to '%s'\n", fn);

	return 0;
};

#if CS_TEST_FRAMEWORK
#	include <asterisk/test.h>
AST_TEST_DEFINE(sccp_config_base_functions)
{
	switch (cmd) {
		case TEST_INIT:
			info->name        = "base_functions";
			info->category    = "/channels/chan_sccp/config/";
			info->summary     = "chan-sccp-b config test";
			info->description = "chan-sccp-b config tests";
			return AST_TEST_NOT_RUN;
		case TEST_EXECUTE:
			break;
	}

	pbx_test_status_update(test, "Executing chan-sccp-b config tests...\n");

	pbx_test_status_update(test, "sccp_find_segment...\n");
	pbx_test_validate(test, sccp_find_segment(SCCP_CONFIG_GLOBAL_SEGMENT) == &sccpConfigSegments[0]);

	pbx_test_status_update(test, "sccp_fine_config...\n");
	const SCCPConfigSegment * sccpConfigSegment = sccp_find_segment(SCCP_CONFIG_GLOBAL_SEGMENT);
	if (sccpConfigSegment) {
		pbx_test_validate(test, sccp_find_config(SCCP_CONFIG_GLOBAL_SEGMENT, "debug") == &sccpConfigSegment->config[0]);
		pbx_test_validate(test, sccp_find_config(SCCP_CONFIG_GLOBAL_SEGMENT, "port") == &sccpConfigSegment->config[6]);
	}

	return AST_TEST_PASS;
}

AST_TEST_DEFINE(sccp_config_multientry)
{
	switch (cmd) {
		case TEST_INIT:
			info->name        = "MultiEntryParameters";
			info->category    = "/channels/chan_sccp/config/";
			info->summary     = "chan-sccp-b config test";
			info->description = "chan-sccp-b config tests";
			return AST_TEST_NOT_RUN;
		case TEST_EXECUTE:
			break;
	}

	pbx_test_status_update(test, "createVariableSetForMultiEntryParameters...\n");
	PBX_VARIABLE_TYPE *varset = NULL, *v = NULL, *root = NULL;
	root       = ast_variable_new("disallow", "0.0.0.0/0.0.0.0", "");
	root->next = ast_variable_new("allow", "10.10.10.0/255.255.255.0", "");

	v = varset = createVariableSetForMultiEntryParameters(root, "disallow|allow", varset);
	pbx_test_validate(test, v != NULL);
	pbx_test_status_update(test, "Test disallow == 0.0.0.0/0.0.0.0\n");
	pbx_test_validate(test, (!strcasecmp((const char *)"disallow", v->name) && !strcasecmp((const char *)"0.0.0.0/0.0.0.0", v->value)));
	v = v->next;
	pbx_test_validate(test, v != NULL);
	pbx_test_status_update(test, "Test allow == 10.10.10.10/255.255.255.255\n");
	pbx_test_validate(test, (!strcasecmp((const char *)"allow", v->name) && !strcasecmp((const char *)"10.10.10.0/255.255.255.0", v->value)));
	v = v->next;
	pbx_test_validate(test, v == NULL);

	pbx_variables_destroy(varset);
	pbx_variables_destroy(root);

	return AST_TEST_PASS;
}

AST_TEST_DEFINE(sccp_config_tokenized_default)
{
	switch (cmd) {
		case TEST_INIT:
			info->name        = "TokenizedDefault";
			info->category    = "/channels/chan_sccp/config/";
			info->summary     = "chan-sccp-b config test";
			info->description = "chan-sccp-b config tests";
			return AST_TEST_NOT_RUN;
		case TEST_EXECUTE:
			break;
	}

	pbx_test_status_update(test, "createVariableSetForTokenizedDefault...\n");
	PBX_VARIABLE_TYPE *varset = NULL, *v = NULL;
	v = varset = createVariableSetForTokenizedDefault("disallow|allow", "0.0.0.0/0.0.0.0|10.10.10.0/255.255.255.0", NULL);

	pbx_test_validate(test, v != NULL);
	pbx_test_status_update(test, "Test disallow == 0.0.0.0\n");
	pbx_test_validate(test, (!strcasecmp((const char *)"disallow", v->name) && !strcasecmp((const char *)"0.0.0.0/0.0.0.0", v->value)));
	v = v->next;
	pbx_test_validate(test, v != NULL);
	pbx_test_status_update(test, "Test allow == 10.10.10.0/255.255.255.0\n");
	pbx_test_validate(test, (!strcasecmp((const char *)"allow", v->name) && !strcasecmp((const char *)"10.10.10.0/255.255.255.0", v->value)));

	pbx_variables_destroy(varset);

	return AST_TEST_PASS;
}

static void __attribute__((constructor)) sccp_register_tests(void)
{
	AST_TEST_REGISTER(sccp_config_base_functions);
	AST_TEST_REGISTER(sccp_config_multientry);
	AST_TEST_REGISTER(sccp_config_tokenized_default);
}

static void __attribute__((destructor)) sccp_unregister_tests(void)
{
	AST_TEST_UNREGISTER(sccp_config_base_functions);
	AST_TEST_UNREGISTER(sccp_config_multientry);
	AST_TEST_UNREGISTER(sccp_config_tokenized_default);
}
#endif

