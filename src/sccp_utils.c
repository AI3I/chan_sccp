/*!
 * \file	sccp_utils.c
 * \brief       SCCP Utils Class
 * \author      Sergio Chersovani <mlists [at] c-net.it>
 * \note	Reworked, but based on chan_sccp code.
 *		The original chan_sccp driver that was made by Zozo which itself was derived from the chan_skinny driver.
 *		Modified by Jan Czmok and Julien Goodwin
 * \note	This program is free software and may be modified and distributed under the terms of the GNU Public License.
 *		See the LICENSE file at the top of the source tree.
 *
 */

#include "config.h"
#include "common.h"
#include "sccp_channel.h"
#include "sccp_device.h"
#include "sccp_line.h"
#include "sccp_linedevice.h"
#include "sccp_session.h"
#include "sccp_utils.h"
#include "sccp_labels.h"

SCCP_FILE_VERSION(__FILE__, "");
#include <locale.h>
#if defined __has_include
#  if __has_include (<xlocale.h>)
#    include <xlocale.h>
#  endif
#elif defined(HAVE_XLOCALE_H)
#  include <xlocale.h>
#endif
#if defined(DEBUG) && defined(HAVE_EXECINFO_H)
#  include <execinfo.h>
#    include <asterisk/backtrace.h>
#endif
#include <asterisk/ast_version.h>
#ifdef HAVE_PBX_ACL_H
#  include <asterisk/acl.h>
#endif

void sccp_dump_packet(const unsigned char * const messagebuffer, int len)
{
	static const int numcolumns = 16;

	if (len <= 0 || !messagebuffer || !sccp_strlen((const char *) messagebuffer)) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_1 "SCCP: packet dump skipped: no message buffer\n");
		return;
	}
	int col = 0;
	int cur = 0;
	int hexcolumnlength = 0;
	const char *hex = "0123456789ABCDEF";
	char hexout[(numcolumns * 3) + (numcolumns / 8) + 1];
	char * hexptr = NULL;
	char chrout[numcolumns + 1];
	char * chrptr = NULL;
	pbx_str_t *output_buf = pbx_str_create(DEFAULT_PBX_STR_BUFFERSIZE);
	unsigned char * bufptr     = (unsigned char *)messagebuffer;

	do {
		memset(hexout, 0, (numcolumns * 3) + (numcolumns / 8) + 1);
		memset(chrout, 0, numcolumns + 1);
		hexptr = hexout;
		chrptr = chrout;
		for (col = 0; col < numcolumns && (cur + col) < len; col++) {
			*hexptr++ = hex[(*bufptr >> 4) & 0xF];
			*hexptr++ = hex[(*bufptr) & 0xF];
			*hexptr++ = ' ';
			if ((col + 1) % 8 == 0) {
				*hexptr++ = ' ';
			}
			*chrptr++ = isprint(*bufptr) ? *bufptr : '.';
			bufptr++;
		}
		hexcolumnlength = (numcolumns * 3) + (numcolumns / 8) - 1;
		pbx_str_append(&output_buf, 0, VERBOSE_PREFIX_1 "%08X - %-*.*s - %s\n", cur, hexcolumnlength, hexcolumnlength, hexout, chrout);
		cur += col;
	} while (cur < (len - 1));
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_1 "SCCP: packet hex dump:\n%s", pbx_str_buffer(output_buf));
	sccp_free(output_buf)
}

void sccp_dump_msg(const sccp_msg_t * const msg)
{
	sccp_dump_packet((unsigned char *) msg, letohl(msg->header.length) + 8);
}

void sccp_addons_clear(devicePtr d)
{
	sccp_addon_t * addon = NULL;

	if (!d) {
		return;
	}
	while ((addon = SCCP_LIST_REMOVE_HEAD(&d->addons, list))) {
		sccp_free(addon);
	}
	d->addons.first = NULL;
	d->addons.last = NULL;
}

void sccp_safe_sleep(int ms)
{
	struct timeval start = pbx_tvnow();

	usleep(1);
	while (ast_tvdiff_ms(pbx_tvnow(), start) < ms) {
		usleep(1);
	}
}

#ifndef HAVE_PBX_STRINGS_H
char *pbx_skip_blanks(char *str)
{
	while (*str && *str < 33)
		str++;

	return str;
}

char *pbx_trim_blanks(char *str)
{
	char *work = str;

	if (work) {
		work += strlen(work) - 1;
		while ((work >= str) && *work < 33)
			*(work--) = '\0';
	}
	return str;
}

/* Returns Only the Non Blank Characters */
char *pbx_skip_nonblanks(char *str)
{
	while (*str && *str > 32)
		str++;

	return str;
}

char *pbx_strip(char *s)
{
	s = pbx_skip_blanks(s);
	if (s) {
		pbx_trim_blanks(s);
	}
	return s;
}
#endif

#ifndef CS_AST_HAS_APP_SEPARATE_ARGS

unsigned int sccp_app_separate_args(char *buf, char delim, char **array, int arraylen)
{
	int argc = 0;
	char * scan = NULL;
	int paren = 0;

	if (!buf || !array || !arraylen) {
		return 0;
	}
	memset(array, 0, arraylen * sizeof(*array));

	scan = buf;

	for (argc = 0; *scan && (argc < arraylen - 1); argc++) {
		array[argc] = scan;
		for (; *scan; scan++) {
			if (*scan == '(') {
				paren++;
			} else if (*scan == ')') {
				if (paren) {
					paren--;
				}
			} else if ((*scan == delim) && !paren) {
				*scan++ = '\0';
				break;
			}
		}
	}

	if (*scan) {
		array[argc++] = scan;
	}
	return argc;
}
#endif

void sccp_util_featureStorageBackend(const sccp_event_t * const event)
{
	char family[25];
	char cfwdDeviceLineStore[60];
	char cfwdLineDeviceStore[60];
	sccp_linedevice_t * ld = NULL;
	sccp_device_t * device = NULL;

	if(!event || !(device = event->featureChanged.device)) {
		return;
	}

	sccp_log((DEBUGCAT_EVENT + DEBUGCAT_FEATURE)) (VERBOSE_PREFIX_3 "%s: saving feature change %s (%d) to the Asterisk database\n", DEV_ID_LOG(device), sccp_feature_type2str(event->featureChanged.featureType), event->featureChanged.featureType);
	snprintf(family, sizeof(family), "SCCP/%s", device->id);

	switch (event->featureChanged.featureType) {
		case SCCP_FEATURE_CFWDNONE:
		case SCCP_FEATURE_CFWDBUSY:
		case SCCP_FEATURE_CFWDALL:
		case SCCP_FEATURE_CFWDNOANSWER:
			if((ld = event->featureChanged.optional_linedevice)) {
				constLinePtr line = ld->line;
				uint8_t instance = ld->lineInstance;
				int res = 0;

				sccp_dev_forward_status(line, instance, device);
				snprintf(cfwdDeviceLineStore, sizeof(cfwdDeviceLineStore), "SCCP/%s/%s", device->id, line->name);
				snprintf(cfwdLineDeviceStore, sizeof(cfwdLineDeviceStore), "SCCP/%s/%s", line->name, device->id);
				if(event->featureChanged.featureType == SCCP_FEATURE_CFWDNONE) {
					for(uint x = SCCP_CFWD_ALL; x < SCCP_CFWD_SENTINEL; x++) {
						char cfwdstr[15] = "";
						snprintf(cfwdstr, 14, "cfwd%s", sccp_cfwd2str((sccp_cfwd_t)x));
						res |= iPbx.feature_removeFromDatabase(cfwdDeviceLineStore, cfwdstr);
						res |= iPbx.feature_removeFromDatabase(cfwdLineDeviceStore, cfwdstr);
					}
					sccp_log((DEBUGCAT_CORE))(VERBOSE_PREFIX_3 "%s: all call forwards removed from the database (result %d)\n", DEV_ID_LOG(device), res);
				} else {
					sccp_cfwd_t cfwd = sccp_feature2cfwd(event->featureChanged.featureType);
					char cfwdstr[15] = "";
					snprintf(cfwdstr, 14, "cfwd%s", sccp_cfwd2str(cfwd));
					res |= iPbx.feature_removeFromDatabase(cfwdDeviceLineStore, cfwdstr);
					res |= iPbx.feature_removeFromDatabase(cfwdLineDeviceStore, cfwdstr);
					sccp_log((DEBUGCAT_CORE))(VERBOSE_PREFIX_3 "%s: database delete %s %s (result %d)\n", DEV_ID_LOG(device), cfwdDeviceLineStore, cfwdstr, res);
					if(ld->cfwd[cfwd].enabled) {
						res |= iPbx.feature_addToDatabase(cfwdDeviceLineStore, cfwdstr, ld->cfwd[cfwd].number);
						res |= iPbx.feature_addToDatabase(cfwdLineDeviceStore, cfwdstr, ld->cfwd[cfwd].number);
						sccp_log((DEBUGCAT_CORE))(VERBOSE_PREFIX_3 "%s: database put %s %s (result %d)\n", DEV_ID_LOG(device), cfwdDeviceLineStore, cfwdstr, res);
					}
				}
			}
			break;
		case SCCP_FEATURE_DND:
			if (device->dndFeature.previousStatus != device->dndFeature.status) {
				if (!device->dndFeature.status) {
					sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: DND off saved\n", DEV_ID_LOG(device));
					iPbx.feature_removeFromDatabase(family, "dnd");
				} else {
					if (device->dndFeature.status == SCCP_DNDMODE_SILENT) {
						sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: DND silent saved\n", DEV_ID_LOG(device));
						iPbx.feature_addToDatabase(family, "dnd", "silent");
					} else {
						sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: DND reject saved\n", DEV_ID_LOG(device));
						iPbx.feature_addToDatabase(family, "dnd", "reject");
					}
				}
				device->dndFeature.previousStatus = device->dndFeature.status;
			}
			break;
		case SCCP_FEATURE_PRIVACY:
			if (device->privacyFeature.previousStatus != device->privacyFeature.status) {
				if (!device->privacyFeature.status) {
					iPbx.feature_removeFromDatabase(family, "privacy");
				} else {
					char data[256];

					snprintf(data, sizeof(data), "%d", device->privacyFeature.status);
					iPbx.feature_addToDatabase(family, "privacy", data);
				}
				device->privacyFeature.previousStatus = device->privacyFeature.status;
			}
			break;
		case SCCP_FEATURE_MONITOR:
			if (device->monitorFeature.previousStatus != device->monitorFeature.status) {
				if (device->monitorFeature.status & SCCP_FEATURE_MONITOR_STATE_REQUESTED) {
					iPbx.feature_addToDatabase(family, "monitor", "on");
				} else {
					iPbx.feature_removeFromDatabase(family, "monitor");
				}
				device->monitorFeature.previousStatus = device->monitorFeature.status;
			}
			break;
		default:
			return;
	}
}

int sccp_parseComposedId(const char *labelString, unsigned int maxLength, sccp_subscription_id_t *subscriptionId, char extension[SCCP_MAX_EXTENSION])
{
	pbx_assert(NULL != labelString && NULL != subscriptionId && NULL != extension);
	int res = 0;
	const char *stringIterator = 0;
	uint32_t i = 0;
	boolean_t endDetected = FALSE;
	enum {EXTENSION, ID, CIDNAME, LABEL, AUX} state = EXTENSION;
	memset(subscriptionId, 0, sizeof(sccp_subscription_id_t));

	for (stringIterator = labelString; stringIterator < labelString + maxLength && !endDetected; stringIterator++) {
		switch (state) {
			case EXTENSION:
				pbx_assert(i < SCCP_MAX_EXTENSION);
				switch (*stringIterator) {
					case '\0':
						endDetected = TRUE;
						extension[i] = '\0';
						res++;
						break;
					case '@':
						extension[i] = '\0';
						i = 0;
						state = ID;
						res++;
						break;
					case '!':
						extension[i] = '\0';
						i = 0;
						state = AUX;
						res++;
						break;
					default:
						extension[i] = *stringIterator;
						i++;
						break;
				}
				break;

			case ID:

                                // 98099 is the linename
                                // @ starts a subscriptionid
                                // = replace the cid of the line with the one of the button
                                // 98041 is the replacement subscriptionid... also used the replacement cidnum
                                // cid_name is the new cid_name to use
                                // label is the new label to use
                                // ! starts the options / AUX
                                // default makes this the default line to dial out on

				pbx_assert(i < sizeof(subscriptionId->number));
				switch (*stringIterator) {
					case '\0':
						subscriptionId->number[i] = '\0';
						endDetected = TRUE;
						res++;
						break;
					case '+':
						if(i == 0) {
							subscriptionId->replaceCid = 0;
						}
						break;
					case '=':
						if(i == 0) {
							subscriptionId->replaceCid = 1;
						}
						break;
					case ':':
						subscriptionId->number[i] = '\0';
						i = 0;
						state = CIDNAME;
						res++;
						break;
					case '#':
						subscriptionId->name[i] = '\0';
						i = 0;
						state = LABEL;
						res++;
						break;
					case '!':
						subscriptionId->number[i] = '\0';
						i = 0;
						state = AUX;
						res++;
						break;
					default:
						subscriptionId->number[i] = *stringIterator;
						i++;
						break;
				}
				break;

			case CIDNAME:
				pbx_assert(i < sizeof(subscriptionId->name));
				switch (*stringIterator) {
					case '\0':
						subscriptionId->name[i] = '\0';
						endDetected = TRUE;
						res++;
						break;
					case '#':
						subscriptionId->name[i] = '\0';
						i = 0;
						state = LABEL;
						res++;
						break;
					case '!':
						subscriptionId->name[i] = '\0';
						i = 0;
						state = AUX ;
						res++;
						break;
					default:
						subscriptionId->name[i] = *stringIterator;
						i++;
						break;
				}
				break;

			case LABEL:
				pbx_assert(i < sizeof(subscriptionId->label));
				switch (*stringIterator) {
					case '\0':
						subscriptionId->label[i] = '\0';
						endDetected = TRUE;
						res++;
						break;
					case '!':
						subscriptionId->label[i] = '\0';
						i = 0;
						state = AUX;
						res++;
						break;
					default:
						subscriptionId->label[i] = *stringIterator;
						i++;
						break;
				}
				break;

			case AUX:
				pbx_assert(i < sizeof(subscriptionId->aux));
				switch (*stringIterator) {
					case '\0':
						subscriptionId->aux[i] = '\0';
						endDetected = TRUE;
						res++;
						break;
					default:
						subscriptionId->aux[i] = *stringIterator;
						i++;
						break;
				}
				break;

			default:
				pbx_assert(FALSE);
				res = 0;
				break;
		}
	}
	return res;
}

boolean_t __PURE__ sccp_util_matchSubscriptionId(constChannelPtr channel, const char * subscriptionIdNum)
{
	boolean_t result = TRUE;

	boolean_t filterPhones = FALSE;

	/* Determine if the phones registered on the shared line shall be filtered at all:
	   only if a non-trivial subscription id is specified with the calling channel,
	   which is not the default subscription id of the shared line denoting all devices,
	   the phones are addressed individually. (-DD) */
	filterPhones = FALSE;

	if (sccp_strlen(channel->subscriptionId.number) != 0) {
		if (0 != strncasecmp(channel->subscriptionId.number, channel->line->defaultSubscriptionId.number, sccp_strlen(channel->subscriptionId.number))) {
			filterPhones = TRUE;
		}
	}

	if (FALSE == filterPhones) {
		result = TRUE;
	} else if (0 != sccp_strlen(subscriptionIdNum) &&
		   (0 != strncasecmp(channel->subscriptionId.number, subscriptionIdNum, sccp_strlen(channel->subscriptionId.number)))) {
		result = FALSE;
	}
	return result;
}

gcc_inline boolean_t sccp_netsock_equals(const struct sockaddr_storage * const s0, const struct sockaddr_storage *const s1)
{
	if ((s0->ss_family == s1->ss_family && sccp_netsock_cmp_addr(s0, s1) == 0) && sccp_netsock_cmp_port(s0, s1) == 0) {
		return TRUE;
	}
	return FALSE;
}

gcc_inline boolean_t sccp_strlen_zero(const char *data)
{
	if (!data || (*data == '\0')) {
		return TRUE;
	}

	return FALSE;
}

gcc_inline size_t sccp_strlen(const char *data)
{
	if (!data || (*data == '\0')) {
		return 0;
	}
	return strlen(data);
}

gcc_inline boolean_t sccp_strequals(const char *data1, const char *data2)
{
	if (sccp_strlen_zero(data1) && sccp_strlen_zero(data2)) {
		return TRUE;
	} if (!sccp_strlen_zero(data1) && !sccp_strlen_zero(data2) && (sccp_strlen(data1) == sccp_strlen(data2))) {
		return !strcmp(data1, data2);
	}
	return FALSE;
}

gcc_inline boolean_t sccp_strcaseequals(const char *data1, const char *data2)
{
	if (sccp_strlen_zero(data1) && sccp_strlen_zero(data2)) {
		return TRUE;
	} if (!sccp_strlen_zero(data1) && !sccp_strlen_zero(data2) && (sccp_strlen(data1) == sccp_strlen(data2))) {
		return !strcasecmp(data1, data2);
	}
	return FALSE;
}

int __PURE__ sccp_strIsNumeric(const char *s)
{
	if (*s) {
		char c = 0;

		while ((c = *s++)) {
			if (!isdigit(c)) {
				return 0;
			}
		}
		return 1;
	}
	return 0;
}

gcc_inline void sccp_camelcase(char * instr)
{
	boolean_t capsNext = TRUE;
	int       depth    = 0;
	int       j        = 0;
	for (int i = 0; instr[i] != '\0'; i++) {
		unsigned char ch = (unsigned char)instr[i];
		if (ch == '(') {
			depth++;
		} else if (ch == ')' && depth > 0) {
			depth--;
		} else if (depth == 0 && isalnum(ch)) {
			instr[j++] = capsNext ? toupper(ch) : ch;
			capsNext   = FALSE;
			continue;
		}
		capsNext = TRUE;
	}
	instr[j] = '\0';
}

void sccp_free_ha(struct sccp_ha *ha)
{
	struct sccp_ha * hal = NULL;

	while (ha) {
		hal = ha;
		ha = ha->next;
		sccp_free(hal);
	}
}

/* Must be in the range 0-3. */
#define V6_WORD(sin6, index) ((uint32_t *)&((sin6)->sin6_addr))[(index)]

/* An IPv4 address may arrive IPv4-mapped and must be converted before the rule is applied. */
static int apply_netmask(const struct sockaddr_storage *netaddr, const struct sockaddr_storage *netmask, struct sockaddr_storage *result)
{
	int res = 0;

	char *straddr = pbx_strdupa(sccp_netsock_stringify_addr(netaddr));
	char *strmask = pbx_strdupa(sccp_netsock_stringify_addr(netmask));

	sccp_log(DEBUGCAT_HIGH) (VERBOSE_PREFIX_2 "SCCP: applying netmask %s/%s\n", straddr, strmask);

	if (netaddr->ss_family == AF_INET) {
		struct sockaddr_in result4 = { 0, };
		struct sockaddr_in *addr4 = (struct sockaddr_in *) netaddr;
		struct sockaddr_in *mask4 = (struct sockaddr_in *) netmask;

		result4.sin_family = AF_INET;
		result4.sin_addr.s_addr = addr4->sin_addr.s_addr & mask4->sin_addr.s_addr;
		memcpy(result, &result4, sizeof(result4));
	} else if (netaddr->ss_family == AF_INET6) {
		struct sockaddr_in6 result6 = { 0, };
		struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *) netaddr;
		struct sockaddr_in6 *mask6 = (struct sockaddr_in6 *) netmask;
		int i = 0;

		result6.sin6_family = AF_INET6;
		for (i = 0; i < 4; ++i) {
			V6_WORD(&result6, i) = V6_WORD(addr6, i) & V6_WORD(mask6, i);
		}
		memcpy(result, &result6, sizeof(result6));
	} else {
		pbx_log(LOG_WARNING, "SCCP: netmask not applied: the address is neither IPv4 nor IPv6\n");
		/* Unsupported address scheme */
		res = -1;
	}
	sccp_log(DEBUGCAT_HIGH) (VERBOSE_PREFIX_2 "SCCP: netmask result %s\n", sccp_netsock_stringify_addr(result));

	return res;
}

int sccp_apply_ha(const struct sccp_ha *ha, const struct sockaddr_storage *addr)
{
	return sccp_apply_ha_default(ha, addr, AST_SENSE_ALLOW);
}

int sccp_apply_ha_default(const struct sccp_ha *ha, const struct sockaddr_storage *addr, int defaultValue)
{
	int res = defaultValue;
	const struct sccp_ha * current_ha = NULL;

	for (current_ha = ha; current_ha; current_ha = current_ha->next) {
		struct sockaddr_storage result;
		struct sockaddr_storage mapped_addr;
		const struct sockaddr_storage * addr_to_use = NULL;

		if (sccp_netsock_is_IPv4(&ha->netaddr)) {
			if (sccp_netsock_is_IPv6(addr)) {
				if (sccp_netsock_is_mapped_IPv4(addr)) {
					if (!sccp_netsock_ipv4_mapped(addr, &mapped_addr)) {
						pbx_log(LOG_ERROR, "SCCP: IPv4-mapped address %s could not be converted to IPv4; ACL entry skipped\n", sccp_netsock_stringify_addr(addr));
						continue;
					}
					addr_to_use = &mapped_addr;
				} else {
					continue;
				}
			} else {
				addr_to_use = addr;
			}
		} else {
			if (sccp_netsock_is_IPv6(addr) && !sccp_netsock_is_mapped_IPv4(addr)) {
				addr_to_use = addr;
			} else {
				continue;
			}
		}

		if (apply_netmask(addr_to_use, &current_ha->netmask, &result)) {
			/* Unlikely to happen since we know the address to be IPv4 or IPv6 */
			continue;
		}
		if (sccp_netsock_cmp_addr(&result, &current_ha->netaddr) == 0) {
			res = current_ha->sense;
		}
	}
	return res;
}

/*
 * addr may be NULL for validity checks only (e.g. ast_parse_arg()).
 * flags: 0 = a port is optional, PARSE_PORT_REQUIRE = a port is required, PARSE_PORT_FORBID = no port allowed.
 */
int sccp_sockaddr_storage_parse(struct sockaddr_storage *addr, const char *str, int flags)
{
	struct addrinfo hints;
	struct addrinfo * res = NULL;
	char * s = NULL;
	char * host = NULL;
	char * port = NULL;
	int e = 0;

	s = pbx_strdupa(str);
	if (!sccp_netsock_split_hostport(s, &host, &port, flags)) {
		return 0;
	}

	memset(&hints, 0, sizeof(hints));
	/* Hint to get only one entry from getaddrinfo */
	hints.ai_socktype = SOCK_DGRAM;

#ifdef AI_NUMERICSERV
	hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
#else
	hints.ai_flags = AI_NUMERICHOST;
#endif
	if ((e = getaddrinfo(host, port, &hints, &res))) {
		if (e != EAI_NONAME) {
			pbx_log(LOG_WARNING, "SCCP: could not resolve '%s' port '%s': %s\n", host, S_OR(port, ""), gai_strerror(e));
		}
		return 0;
	}

	/*
	 * I don't see how this could be possible since we're not resolving host
	 * names. But let's be careful...
	 */
	if (res->ai_next != NULL) {
		pbx_log(LOG_NOTICE, "SCCP: '%s' resolves to several addresses; using the first\n", host);
	}

	if (addr) {
		memcpy(addr, res->ai_addr, (res->ai_family == AF_INET6) ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in));
		sccp_log(DEBUGCAT_HIGH) (VERBOSE_PREFIX_2 "SCCP: parsed address %s\n", sccp_netsock_stringify_addr(addr));
	}

	freeaddrinfo(res);
	return 1;
}

static int parse_cidr_mask(struct sockaddr_storage *addr, int is_v4, const char *mask_str)
{
	int mask = 0;

	if (sscanf(mask_str, "%30d", &mask) != 1) {
		return -1;
	}
	if (is_v4) {
		struct sockaddr_in sin = { 0, };
		if (mask < 0 || mask > 32) {
			return -1;
		}
		sin.sin_family = AF_INET;
		if (mask != 0) {
			sin.sin_addr.s_addr = htonl(0xFFFFFFFF << (32 - mask));
		}
		memcpy(addr, &sin, sizeof(sin));
	} else {
		struct sockaddr_in6 sin6 = { 0, };
		int i = 0;

		if (mask < 0 || mask > 128) {
			return -1;
		}
		sin6.sin6_family = AF_INET6;
		for (i = 0; i < 4; ++i) {
			/* Once mask reaches 0, we don't have
			 * to explicitly set anything anymore
			 * since sin6 was zeroed out already
			 */
			if (mask > 0) {
				V6_WORD(&sin6, i) = htonl(0xFFFFFFFF << (mask < 32 ? (32 - mask) : 0));
				mask -= mask < 32 ? mask : 32;
			}
		}
		memcpy(addr, &sin6, sizeof(sin6));
	}
	return 0;
}

struct sccp_ha *sccp_append_ha(const char *sense, const char *stuff, struct sccp_ha *path, int *error)
{
	struct sccp_ha * ha = NULL;
	struct sccp_ha * prev = NULL;
	struct sccp_ha * ret = NULL;
	char *tmp = pbx_strdupa(stuff);
	char * address = NULL;

	char * mask = NULL;
	int addr_is_v4 = 0;

	ret = path;
	while (path) {
		prev = path;
		path = path->next;
	}

	if (!(ha = (struct sccp_ha *)sccp_calloc(sizeof *ha, 1))) {
		pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
		if (error) {
			*error = 1;
		}
		return ret;
	}

	address = strsep(&tmp, "/");
	if (!address) {
		address = tmp;
	} else {
		mask = tmp;
	}
	if (!sccp_sockaddr_storage_parse(&ha->netaddr, address, PARSE_PORT_FORBID)) {
		pbx_log(LOG_WARNING, "SCCP: deny/permit entry '%s' is not a valid IP address; entry ignored\n", address);
		sccp_free_ha(ha);
		if (error) {
			*error = 1;
		}
		return ret;
	}
	if (sccp_netsock_ipv4_mapped(&ha->netaddr, &ha->netaddr)) {
		sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_3 "SCCP: deny/permit entry %s is IPv4-mapped; treated as an IPv4 entry\n", address);
	}

	addr_is_v4 = sccp_netsock_is_IPv4(&ha->netaddr);

	if (!mask) {
		parse_cidr_mask(&ha->netmask, addr_is_v4, addr_is_v4 ? "32" : "128");
	} else if (strchr(mask, ':') || strchr(mask, '.')) {
		int mask_is_v4 = 0;

		sccp_log(DEBUGCAT_HIGH) (VERBOSE_PREFIX_2 "SCCP: deny/permit mask %s\n", mask);
		if (!sccp_sockaddr_storage_parse(&ha->netmask, mask, PARSE_PORT_FORBID)) {
			pbx_log(LOG_WARNING, "SCCP: deny/permit entry %s: '%s' is not a valid netmask; entry ignored\n", address, mask);
			sccp_free_ha(ha);
			if (error) {
				*error = 1;
			}
			return ret;
		}
		sccp_log(DEBUGCAT_HIGH) (VERBOSE_PREFIX_2 "SCCP: deny/permit mask %s = %s\n", mask, sccp_netsock_stringify_addr(&ha->netmask));
		if (sccp_netsock_ipv4_mapped(&ha->netmask, &ha->netmask)) {
			sccp_log((DEBUGCAT_CONFIG))(VERBOSE_PREFIX_3 "SCCP: deny/permit netmask %s is IPv4-mapped; treated as an IPv4 netmask\n", mask);
		}
		mask_is_v4 = sccp_netsock_is_IPv4(&ha->netmask);
		if (addr_is_v4 ^ mask_is_v4) {
			pbx_log(LOG_WARNING, "SCCP: deny/permit entry %s/%s mixes IPv4 and IPv6; entry ignored\n", address, mask);
			sccp_free_ha(ha);
			if (error) {
				*error = 1;
			}
			return ret;
		}
	} else if (parse_cidr_mask(&ha->netmask, addr_is_v4, mask)) {
		pbx_log(LOG_WARNING, "SCCP: deny/permit entry %s: '/%s' is not a valid prefix length; entry ignored\n", address, mask);
		sccp_free_ha(ha);
		if (error) {
			*error = 1;
		}
		return ret;
	}
	if (apply_netmask(&ha->netaddr, &ha->netmask, &ha->netaddr)) {
		/* This shouldn't happen because ast_sockaddr_parse would
		 * have failed much earlier on an unsupported address scheme
		 */
		char *failaddr = pbx_strdupa(sccp_netsock_stringify_addr(&ha->netaddr));
		char *failmask = pbx_strdupa(sccp_netsock_stringify_addr(&ha->netmask));

		pbx_log(LOG_WARNING, "SCCP: deny/permit entry: netmask %s could not be applied to %s; entry ignored\n", failmask, failaddr);
		sccp_free_ha(ha);
		if (error) {
			*error = 1;
		}
		return ret;
	}

	ha->sense = strncasecmp(sense, "p", 1) ? AST_SENSE_DENY : AST_SENSE_ALLOW;

	ha->next = NULL;
	if (prev) {
		prev->next = ha;
	} else {
		ret = ha;
	}

	sccp_log (DEBUGCAT_HIGH) (VERBOSE_PREFIX_2 "%s/%s (sense %d) added to the access list\n", sccp_netsock_stringify_addr (&ha->netaddr), sccp_netsock_stringify_addr (&ha->netmask), ha->sense);

	return ret;
}

void sccp_print_ha(struct ast_str *buf, int buflen, struct sccp_ha *path)
{
	/* sccp_netsock_stringify_addr() returns one shared per-thread buffer, so copy each result before the next call */
	const char *separator = "";
	while (path) {
		char netaddr[INET6_ADDRSTRLEN] = "";
		char netmask[INET6_ADDRSTRLEN] = "";
		sccp_copy_string(netaddr, sccp_netsock_stringify_addr(&path->netaddr), sizeof(netaddr));
		sccp_copy_string(netmask, sccp_netsock_stringify_addr(&path->netmask), sizeof(netmask));
		pbx_str_append(&buf, buflen, "%s%s %s/%s", separator, AST_SENSE_DENY == path->sense ? "deny" : "permit", netaddr, netmask);
		separator = ", ";
		path = path->next;
	}
}

#if CS_TEST_FRAMEWORK
#include <asterisk/test.h>
AST_TEST_DEFINE(chan_sccp_acl_tests)
{
	struct sccp_ha *ha = NULL;
	struct sockaddr_storage sas10;

	struct sockaddr_storage sas1015;

	struct sockaddr_storage sas172;

	struct sockaddr_storage sas200;

	struct sockaddr_storage sasff;

	struct sockaddr_storage sasffff;
	int error = 0;

	switch (cmd) {
	case TEST_INIT:
		info->name = "permit_deny";
		info->category = "/channels/chan_sccp/acl/";
		info->summary = "chan-sccp-b ha / permit / deny test";
		info->description = "chan-sccp-b ha / permit / deny parsing tests";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	pbx_test_status_update(test, "Executing chan-sccp-b ha path tests...\n");

	pbx_test_status_update(test, "Setting up sockaddr_storage...\n");
	sccp_sockaddr_storage_parse(&sas10, "10.0.0.1", PARSE_PORT_FORBID);
	pbx_test_validate(test, sccp_netsock_is_IPv4(&sas10));

	sccp_sockaddr_storage_parse(&sas1015, "10.15.15.1", PARSE_PORT_FORBID);
	pbx_test_validate(test, sccp_netsock_is_IPv4(&sas1015));

	sccp_sockaddr_storage_parse(&sas172, "172.16.0.1", PARSE_PORT_FORBID);
	pbx_test_validate(test, sccp_netsock_is_IPv4(&sas172));

	sccp_sockaddr_storage_parse(&sas200, "200.200.100.100", PARSE_PORT_FORBID);
	pbx_test_validate(test, sccp_netsock_is_IPv4(&sas200));

	sccp_sockaddr_storage_parse(&sasff, "fe80::ffff:0:0:0", PARSE_PORT_FORBID);
	pbx_test_validate(test, sccp_netsock_is_IPv6(&sasff));

	sccp_sockaddr_storage_parse(&sasffff, "fe80::ffff:0:ffff:0", PARSE_PORT_FORBID);
	pbx_test_validate(test, sccp_netsock_is_IPv6(&sasffff));

	pbx_test_status_update(test, "test 1: ha deny all\n");
	ha = sccp_append_ha("deny", "0.0.0.0/0.0.0.0", ha, &error);
	pbx_test_validate(test, error == 0);
	pbx_test_validate(test, sccp_apply_ha(ha, (struct sockaddr_storage *) &sas10) == AST_SENSE_DENY);
	pbx_test_validate(test, sccp_apply_ha(ha, (struct sockaddr_storage *) &sas1015) == AST_SENSE_DENY);
	pbx_test_validate(test, sccp_apply_ha(ha, (struct sockaddr_storage *) &sas172) == AST_SENSE_DENY);
	pbx_test_validate(test, sccp_apply_ha(ha, (struct sockaddr_storage *) &sas200) == AST_SENSE_DENY);

	pbx_test_status_update(test, "test 2: previous + permit 10.15.15.0/255.255.255.0\n");
	ha = sccp_append_ha("permit", "10.15.15.0/255.255.255.0", ha, &error);
	pbx_test_validate(test, error == 0);
	pbx_test_validate(test, sccp_apply_ha(ha, (struct sockaddr_storage *) &sas10) == AST_SENSE_DENY);
	pbx_test_validate(test, sccp_apply_ha(ha, (struct sockaddr_storage *) &sas1015) != AST_SENSE_DENY);
	pbx_test_validate(test, sccp_apply_ha(ha, (struct sockaddr_storage *) &sas172) == AST_SENSE_DENY);
	pbx_test_validate(test, sccp_apply_ha(ha, (struct sockaddr_storage *) &sas200) == AST_SENSE_DENY);

	pbx_test_status_update(test, "test 3: previous + second permit 10.15.15.0/255.255.255.0\n");
	ha = sccp_append_ha("permit", "10.15.15.0/255.255.255.0", ha, &error);
	pbx_test_validate(test, error == 0);
	pbx_test_validate(test, sccp_apply_ha(ha, (struct sockaddr_storage *) &sas10) == AST_SENSE_DENY);
	pbx_test_validate(test, sccp_apply_ha(ha, (struct sockaddr_storage *) &sas1015) != AST_SENSE_DENY);
	pbx_test_validate(test, sccp_apply_ha(ha, (struct sockaddr_storage *) &sas172) == AST_SENSE_DENY);
	pbx_test_validate(test, sccp_apply_ha(ha, (struct sockaddr_storage *) &sas200) == AST_SENSE_DENY);
	sccp_free_ha(ha);
	ha = NULL;

	pbx_test_status_update(test, "test 4: deny all + permit 10.0.0.0/255.255.255.0\n");
	ha = sccp_append_ha("deny", "0.0.0.0/0.0.0.0", ha, &error);
	pbx_test_validate(test, error == 0);
	ha = sccp_append_ha("permit", "10.0.0.0/255.0.0.0", ha, &error);
	pbx_test_validate(test, error == 0);
	pbx_test_validate(test, sccp_apply_ha(ha, (struct sockaddr_storage *) &sas10) != AST_SENSE_DENY);
	pbx_test_validate(test, sccp_apply_ha(ha, (struct sockaddr_storage *) &sas1015) != AST_SENSE_DENY);
	pbx_test_validate(test, sccp_apply_ha(ha, (struct sockaddr_storage *) &sas172) == AST_SENSE_DENY);
	pbx_test_validate(test, sccp_apply_ha(ha, (struct sockaddr_storage *) &sas200) == AST_SENSE_DENY);

	pbx_test_status_update(test, "test 5: previous + 172.16.0.0/255.255.0.0\n");
	ha = sccp_append_ha("permit", "172.16.0.0/255.0.0.0", ha, &error);
	pbx_test_validate(test, error == 0);
	pbx_test_validate(test, sccp_apply_ha(ha, (struct sockaddr_storage *) &sas10) != AST_SENSE_DENY);
	pbx_test_validate(test, sccp_apply_ha(ha, (struct sockaddr_storage *) &sas1015) != AST_SENSE_DENY);
	pbx_test_validate(test, sccp_apply_ha(ha, (struct sockaddr_storage *) &sas172) != AST_SENSE_DENY);
	pbx_test_validate(test, sccp_apply_ha(ha, (struct sockaddr_storage *) &sas200) == AST_SENSE_DENY);

	pbx_test_status_update(test, "test 6: previous + deny_all at the end\n");
	ha = sccp_append_ha("deny", "0.0.0.0/0.0.0.0", ha, &error);
	pbx_test_validate(test, error == 0);
	pbx_test_validate(test, sccp_apply_ha(ha, (struct sockaddr_storage *) &sas10) == AST_SENSE_DENY);
	pbx_test_validate(test, sccp_apply_ha(ha, (struct sockaddr_storage *) &sas1015) == AST_SENSE_DENY);
	pbx_test_validate(test, sccp_apply_ha(ha, (struct sockaddr_storage *) &sas172) == AST_SENSE_DENY);
	pbx_test_validate(test, sccp_apply_ha(ha, (struct sockaddr_storage *) &sas200) == AST_SENSE_DENY);
	sccp_free_ha(ha);
	ha = NULL;

	pbx_test_status_update(test, "test 7: IPv6: deny 0.0.0.0/0.0.0.0,::,::/0::\n");
	ha = sccp_append_ha("deny", "0.0.0.0/0.0.0.0", ha, &error);
	pbx_test_validate(test, error == 0);
	ha = sccp_append_ha("deny", "::", ha, &error);
	pbx_test_validate(test, error == 0);
	ha = sccp_append_ha("deny", "::/0", ha, &error);
	pbx_test_validate(test, error == 0);
	pbx_test_status_update(test, "      : previous + permit fe80::ffff:0:0:0/80\n");
	ha = sccp_append_ha("permit", "fe80::ffff:0:0:0/80", ha, &error);
	pbx_test_validate(test, error == 0);
	pbx_test_status_update(test, "      : previous + permit fe80::ffff:0:ffff:0/112\n");
	ha = sccp_append_ha("permit", "fe80::ffff:0:ffff:0/112", ha, &error);
	pbx_test_validate(test, error == 0);
	pbx_test_validate(test, sccp_apply_ha(ha, (struct sockaddr_storage *) &sas10) == AST_SENSE_DENY);
	pbx_test_validate(test, sccp_apply_ha(ha, (struct sockaddr_storage *) &sasff) != AST_SENSE_DENY);
	pbx_test_validate(test, sccp_apply_ha(ha, (struct sockaddr_storage *) &sasffff) != AST_SENSE_DENY);
	pbx_test_validate(test, sccp_apply_ha(ha, (struct sockaddr_storage *) &sas200) == AST_SENSE_DENY);
	sccp_free_ha(ha);
	ha = NULL;

	return AST_TEST_PASS;
}

AST_TEST_DEFINE(chan_sccp_acl_invalid_tests)
{
	struct sccp_ha *ha = NULL;
	enum ast_test_result_state res = AST_TEST_PASS;

	switch (cmd) {
	case TEST_INIT:
		info->name = "invalid";
		info->category = "/channels/chan_sccp/acl/";
		info->summary = "Invalid ACL unit test";
		info->description = "Ensures that garbage ACL values are not accepted";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	pbx_test_status_update(test, "Executing invalid acl test tests...\n");

	const char * invalid_acls[] = {
		"1.3.3.7/-1",
		"1.3.3.7/33",
		"1.3.3.7/92342348927389492307420",
		"1.3.3.7/California",
		/* Too many octets in Netmask */
		"1.3.3.7/255.255.255.255.255",
		/* Octets in IP address exceed 255 */
		"57.60.278.900/31",
		/* Octets in IP address exceed 255 and are negative */
		"400.32.201029.-6/24",
		"EGGSOFDEATH/4000",
		/* Too many octets in IP address */
		"33.4.7.8.3/300030",
		/* Too many octets in Netmask */
		"1.2.3.4/6.7.8.9.0",
		/* Too many octets in IP address */
		"3.1.4.1.5.9/3",
		"ff::ff::ff/3",
		"1234:5678:90ab:cdef:1234:5678:90ab:cdef:1234/56",
		"::ffff/129",
		/* IPv4-mapped IPv6 address has too few octets */
		"::ffff:255.255.255/128",
		":1234:/15",
		"fe80::1234/255.255.255.0",
	};
	uint8_t i = 0;
	for (i = 0; i < ARRAY_LEN(invalid_acls); ++i) {
		int error = 0;
		ha = sccp_append_ha("permit", invalid_acls[i], ha, &error);
		if (ha || !error) {
			pbx_test_status_update(test, "ACL %s accepted even though it is total garbage.\n", invalid_acls[i]);
			res = AST_TEST_FAIL;
			break;
		}
	}
	if (ha) {
		sccp_free_ha(ha);
	}
	ha = NULL;

	return res;
}
#endif

void sccp_print_group(struct ast_str *buf, int buflen, sccp_group_t group)
{
	unsigned int i = 0;
	int first = 1;
	uint8_t max = (sizeof(sccp_group_t) * 8) - 1;

	if (!group) {
		return;
	}
	for (i = 0; i <= max; i++) {
		if (group & ((sccp_group_t) 1 << i)) {
			if (!first) {
				pbx_str_append(&buf, buflen, ",");
			} else {
				first = 0;
			}
			pbx_str_append(&buf, buflen, "%d", i);
		}
	}
}

int __PURE__ sccp_strversioncmp(const char *s1, const char *s2)
{
	static const char *digits = "0123456789";
	int ret = 0;

	int lz1 = 0;

	int lz2 = 0;
	size_t p1 = 0;

	size_t p2 = 0;

	p1 = strcspn(s1, digits);
	p2 = strcspn(s2, digits);
	while (p1 == p2 && s1[p1] != '\0' && s2[p2] != '\0') {
		ret = strncmp(s1, s2, p1);
		if(ret != 0) {
			return ret;
		}
		s1 += p1;
		s2 += p2;

		lz1 = lz2 = 0;
		if (*s1 == '0') {
			lz1 = 1;
		}
		if (*s2 == '0') {
			lz2 = 1;
		}
		if (lz1 > lz2) {
			return -1;
		}
		if (lz1 < lz2) {
			return 1;
		}
		if (lz1 == 1) {
			/*
			 * If the common prefix for s1 and s2 consists only of zeros, then the
			 * "longer" number has to compare less. Otherwise the comparison needs
			 * to be numerical (just fallthrough). See
			 */
			while (*s1 == '0' && *s2 == '0') {
				++s1;
				++s2;
			}

			p1 = strspn(s1, digits);
			p2 = strspn(s2, digits);

			if (p1 == 0 && p2 > 0) {
				return 1;
			} if (p2 == 0 && p1 > 0) {
				return -1;
			}
			if (*s1 != *s2 && *s1 != '0' && *s2 != '0') {
				if (p1 < p2) {
					return 1;
				}
				if (p1 > p2) {
					return -1;
				}
			} else {
				if (p1 < p2) {
					ret = strncmp(s1, s2, p1);
				} else if (p1 > p2) {
					ret = strncmp(s1, s2, p2);
				}
				if (ret != 0) {
					return ret;
				}
			}
		}

		p1 = strspn(s1, digits);
		p2 = strspn(s2, digits);

		if (p1 < p2) {
			return -1;
		}
		if (p1 > p2) {
			return 1;
		}
		ret = strncmp(s1, s2, p1);
		if(ret != 0) {
			return ret;
		}
		s1 += p1;
		s2 += p2;
		p1 = strcspn(s1, digits);
		p2 = strcspn(s2, digits);
	}

	return strcmp(s1, s2);
}

char *sccp_dec2binstr(char *buf, size_t size, int value)
{
	char b[33] = { 0 };
	int pos = 0;
	long long z = 0;

	for (z = 1LL << 31, pos = 0; z > 0; z >>= 1, pos++) {
		b[pos] = (((value & z) == z) ? '1' : '0');
	}
	snprintf(buf, size, "%s", b);
	return buf;
}

gcc_inline void sccp_copy_string(char *dst, const char *src, size_t size)
{
	pbx_assert(NULL != dst && NULL != src);
	if (do_expect(size != 0)) {
		while (do_expect(--size != 0)) {
			if (+(*dst++ = *src++) == '\0') {
				break;
			}
		}
	}
	*dst = '\0';
}

/*
 * If the given string was allocated dynamically, the caller must not overwrite that pointer with the returned value, since the original pointer must be deallocated using the same allocator with which it was allocated.
 * The return value must NOT be deallocated using free() etc.
 */
char *sccp_trimwhitespace(char *str)
{
	char * end = NULL;

	while (isspace(*str)) {
		str++;
	}
	if (*str == 0) {
		return str;
	}
	end = str + sccp_strlen(str) - 1;
	while (end > str && isspace(*end)) {
		end--;
	}
	*(end + 1) = 0;
	return str;
}

gcc_inline int sccp_atoi(const char * const buf, size_t buflen)
{
	int result = 0;
	if (buf && buflen > 0) {
		errno = 0;
		char *end = NULL;
		long temp = strtol (buf, &end, 10);
		if (end != buf && errno != ERANGE && temp >= INT_MIN && temp <= INT_MAX) {
			result = (int)temp;
		}
	}
	return result;
}

int sccp_random(void)
{
	return (int)pbx_random();
}

const char * __PURE__ sccp_retrieve_str_variable_byKey(PBX_VARIABLE_TYPE * params, const char * key)
{
	PBX_VARIABLE_TYPE * param = NULL;
	for(param = params;param;param = param->next) {
		if(strcasecmp(key, param->name) == 0) {
			return param->value;
			break;
		}
	}
	return NULL;
}

int sccp_retrieve_int_variable_byKey(PBX_VARIABLE_TYPE *params, const char *key)
{
	const char *value = sccp_retrieve_str_variable_byKey(params, key);
	if (value) {
		return sccp_atoi(value, strlen(value));
	}
	return -1;
}

boolean_t sccp_append_variable(PBX_VARIABLE_TYPE *params, const char *key, const char *value)
{
	boolean_t res = FALSE;
	PBX_VARIABLE_TYPE * newvar = NULL;
	if ((newvar = pbx_variable_new(key, value, ""))) {
		if (params) {
			while(params->next) {
				params = params->next;
			}
			params->next = newvar;
		} else {
			params = newvar;
		}
		res = TRUE;
	} else {
		pbx_log(LOG_ERROR, "SCCP: channel variable not added: out of memory\n");
	}
	return res;
}

gcc_inline int sccp_utf8_columnwidth(int width, const char *const ms)
{
	int res = 0;
	locale_t locale = newlocale(LC_ALL_MASK, "", NULL);
	locale_t old_locale = uselocale(locale);

	if(ms) {
		res = width + (strlen(ms) - mbstowcs(NULL, ms, width));
	}

	uselocale(old_locale);
	if(locale != (locale_t)0) {
		freelocale(locale);
	}

	return res;
}

gcc_inline boolean_t sccp_always_false(void)
{
	return FALSE;
}

gcc_inline boolean_t sccp_always_true(void)
{
	return TRUE;
}

gcc_inline sccp_feature_type_t sccp_cfwd2feature(const sccp_cfwd_t type)
{
	switch(type) {
		case SCCP_CFWD_ALL:
			return SCCP_FEATURE_CFWDALL;
		case SCCP_CFWD_BUSY:
			return SCCP_FEATURE_CFWDBUSY;
		case SCCP_CFWD_NOANSWER:
			return SCCP_FEATURE_CFWDNOANSWER;
		case SCCP_CFWD_NONE:
			return SCCP_FEATURE_CFWDNONE;
		default:
			return SCCP_FEATURE_TYPE_SENTINEL;
	}
}

gcc_inline sccp_cfwd_t sccp_feature2cfwd(const sccp_feature_type_t type)
{
	switch(type) {
		case SCCP_FEATURE_CFWDALL:
			return SCCP_CFWD_ALL;
		case SCCP_FEATURE_CFWDBUSY:
			return SCCP_CFWD_BUSY;
		case SCCP_FEATURE_CFWDNOANSWER:
			return SCCP_CFWD_NOANSWER;
		case SCCP_FEATURE_CFWDNONE:
			return SCCP_CFWD_NONE;
		default:
			return SCCP_CFWD_SENTINEL;
	}
}

gcc_inline skinny_stimulus_t sccp_cfwd2stimulus(const sccp_cfwd_t type)
{
	switch(type) {
		case SCCP_CFWD_ALL:
			return SKINNY_STIMULUS_FORWARDALL;
		case SCCP_CFWD_BUSY:
			return SKINNY_STIMULUS_FORWARDBUSY;
		case SCCP_CFWD_NOANSWER:
			return SKINNY_STIMULUS_FORWARDNOANSWER;
		case SCCP_CFWD_NONE:
		default:
			return SKINNY_STIMULUS_SENTINEL;
	}
}

gcc_inline const char * const sccp_cfwd2disp(const sccp_cfwd_t type)
{
	switch(type) {
		case SCCP_CFWD_ALL:
			return SKINNY_DISP_CFWDALL;
		case SCCP_CFWD_BUSY:
			return SKINNY_DISP_CFWDBUSY;
		case SCCP_CFWD_NOANSWER:
			return SKINNY_DISP_NOANSWER;
		case SCCP_CFWD_NONE:
		case SCCP_CFWD_SENTINEL:
		default:
			return "";
	}
}

#if CS_TEST_FRAMEWORK
static void __attribute__((constructor)) sccp_register_tests(void)
{
	AST_TEST_REGISTER(chan_sccp_acl_tests);
	AST_TEST_REGISTER(chan_sccp_acl_invalid_tests);
}

static void __attribute__((destructor)) sccp_unregister_tests(void)
{
	AST_TEST_UNREGISTER(chan_sccp_acl_tests);
	AST_TEST_UNREGISTER(chan_sccp_acl_invalid_tests);
}
#endif

#if DEBUG
void sccp_do_backtrace(void)
{
	pbx_rwlock_rdlock(&GLOB(lock));
	boolean_t running = GLOB(module_running);
	pbx_rwlock_unlock(&GLOB(lock));
	if (!running) {
		return;
	}

#if defined(HAVE_EXECINFO_H) && defined(HAVE_BKTR)
	void	*addresses[SCCP_BACKTRACE_SIZE];
	size_t size = 0;

	size_t i = 0;
	bt_string_t * strings = NULL;
	struct ast_str * btbuf = NULL;
	if (!(btbuf = pbx_str_alloca(DEFAULT_PBX_STR_BUFFERSIZE * 2))) {
		return;
	}

	pbx_str_append(&btbuf, DEFAULT_PBX_STR_BUFFERSIZE, "================================================================================\n");
	pbx_str_append(&btbuf, DEFAULT_PBX_STR_BUFFERSIZE, "OPERATING SYSTEM: %s, ARCHITECTURE: %s, KERNEL: %s\nASTERISK: %s\nCHAN_SCCP: %s, revision %s, built by %s on %s\n", BUILD_OS, BUILD_MACHINE, BUILD_KERNEL, pbx_get_version(), SCCP_VERSION, SCCP_REVISIONSTR, BUILD_USER, BUILD_DATE);
	pbx_str_append(&btbuf, DEFAULT_PBX_STR_BUFFERSIZE, "--------------------------------------------------------------------------(bt)--\n");
	size = backtrace(addresses, SCCP_BACKTRACE_SIZE);
	strings = ast_bt_get_symbols(addresses, size);

	if (strings) {
		for (i = 1; i < size; i++) {
#ifdef CS_AST_BACKTRACE_VECTOR_STRING
			pbx_str_append(&btbuf, DEFAULT_PBX_STR_BUFFERSIZE, " (bt) > %s\n", AST_VECTOR_GET(strings, i));
#else
			pbx_str_append(&btbuf, DEFAULT_PBX_STR_BUFFERSIZE, " (bt) > %s\n", strings[i]);
#endif
		}
		bt_free(strings);

		pbx_str_append(&btbuf, DEFAULT_PBX_STR_BUFFERSIZE, "================================================================================\n");
		pbx_log(LOG_WARNING, "SCCP: backtrace:\n%s\n", pbx_str_buffer(btbuf));
	}
#endif	// HAVE_EXECINFO_H && HAVE_BKTR
}
#endif // DEBUG
