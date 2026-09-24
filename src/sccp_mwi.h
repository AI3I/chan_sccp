/*!
 * \file	sccp_featureParkingLot.h
 * \brief	SCCP ParkingLot Header
 * \author	Diederik de Groot <ddegroot [at] users.sf.net>
 * \date	2015-Sept-16
 * \note	This program is free software and may be modified and distributed under the terms of the GNU Public License.
 *		See the LICENSE file at the top of the source tree.
 */
#pragma once

struct sccp_mailbox {
	char uniqueid[SCCP_MAX_MAILBOX_UNIQUEID];
	SCCP_LIST_ENTRY (sccp_mailbox_t) list;
};

__BEGIN_C_EXTERN__
typedef struct {
	void (*const startModule)(void);
	void (*const stopModule)(void);
	int (*const showSubscriptions)(int fd, sccp_cli_totals_t * totals, struct mansession * s, const struct message * m, int argc, char * argv[]);
} VoicemailInterface;
extern const VoicemailInterface iVoicemail;

__END_C_EXTERN__
