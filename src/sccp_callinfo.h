/*!
 * \file	sccp_callinfo.h
 * \brief	SCCP CallInfo Header
 * \author	Diederik de Groot <ddegroot [at] users.sf.net>
 * \date	2015-Sept-16
 * \note	This program is free software and may be modified and distributed under the terms of the GNU Public License.
 *		See the LICENSE file at the top of the source tree.
 */
#pragma once

__BEGIN_C_EXTERN__

struct sccp_callinfo;

typedef struct tagCallInfo {
	sccp_callinfo_t * const (*const Constructor)(uint8_t callInstance, const char * const designator);
	sccp_callinfo_t * const (*const Destructor)(sccp_callinfo_t ** const ci);
	sccp_callinfo_t * (*const CopyConstructor)(const sccp_callinfo_t * const src_ci);

	/*
	 * Key/value pairs ending with SCCP_CALLINFO_KEY_SENTINEL. "" clears an entry; NULL leaves it unchanged.
	 * Example: iCallInfo.Setter(ci, SCCP_CALLINFO_LAST_REDIRECTINGPARTY_NUMBER, "123", SCCP_CALLINFO_LAST_REDIRECT_REASON, 4, SCCP_CALLINFO_KEY_SENTINEL);
	 */
	int (*const Setter)(sccp_callinfo_t * const ci, int key, ...);
	int (*const CopyByKey)(const sccp_callinfo_t * const src_ci, sccp_callinfo_t * const dst_ci, int key, ...);
	int (*const Send)(sccp_callinfo_t * const ci, const uint32_t callid, const skinny_calltype_t calltype, const uint8_t lineInstance, constDevicePtr device, boolean_t force);

	/*
	 * Key/destination-pointer pairs ending with SCCP_CALLINFO_KEY_SENTINEL.
	 * Example: iCallInfo.Getter(ci, SCCP_CALLINFO_LAST_REDIRECTINGPARTY_NUMBER, &number, SCCP_CALLINFO_LAST_REDIRECT_REASON, &reason, SCCP_CALLINFO_KEY_SENTINEL);
	 */
	int (*const Getter)(const sccp_callinfo_t * const ci, int key, ...);

	int (*const SetCalledParty)(sccp_callinfo_t * const ci, const char name[StationMaxDirnumSize], const char number[StationMaxDirnumSize], const char voicemail[StationMaxDirnumSize]);
	int (*const SetCallingParty)(sccp_callinfo_t * const ci, const char name[StationMaxDirnumSize], const char number[StationMaxDirnumSize], const char voicemail[StationMaxDirnumSize]);
	int (*const SetOrigCalledParty)(sccp_callinfo_t * const ci, const char name[StationMaxDirnumSize], const char number[StationMaxDirnumSize], const char voicemail[StationMaxDirnumSize], const int reason);
	int (*const SetOrigCallingParty)(sccp_callinfo_t * const ci, const char name[StationMaxDirnumSize], const char number[StationMaxDirnumSize]);
	int (*const SetLastRedirectingParty)(sccp_callinfo_t * const ci, const char name[StationMaxDirnumSize], const char number[StationMaxDirnumSize], const char voicemail[StationMaxDirnumSize], const int reason);

	void (*Print2log)(const sccp_callinfo_t * const ci, const char *const header);
} CallInfoInterface;

extern const CallInfoInterface iCallInfo;

__END_C_EXTERN__
