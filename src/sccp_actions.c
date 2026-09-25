/*!
 * \file        sccp_actions.c
 * \brief       SCCP Actions Class
 * \author      Sergio Chersovani <mlists [at] c-net.it>
 * \author      Federico Santulli <fsantulli [at] users.sourceforge.net>
 * \note        Reworked, but based on chan_sccp code.
 *              The original chan_sccp driver that was made by Zozo which itself was derived from the chan_skinny driver.
 *              Modified by Jan Czmok and Julien Goodwin
 * \note        This program is free software and may be modified and distributed under the terms of the GNU Public License.
 *              See the LICENSE file at the top of the source tree.
 *
 */
#include "config.h"
#include "common.h"
#include "define.h"

SCCP_FILE_VERSION(__FILE__, "");

#include "sccp_actions.h"
#include "sccp_device.h"
#include "sccp_session.h"
#include "sccp_channel.h"
#include "sccp_utils.h"
#include "sccp_pbx.h"
#include "sccp_conference.h"
#include "sccp_config.h"
#include "sccp_feature.h"
#include "sccp_indicate.h"
#include "sccp_line.h"
#include "sccp_linedevice.h"
#include "sccp_labels.h"
#include "sccp_devstate.h"
#include "sccp_featureParkingLot.h"

#if defined(HAVE_UNALIGNED_BUSERROR)
#include <asterisk/unaligned.h>
#endif
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
#ifdef HAVE_PBX_ACL_H
#  include <asterisk/acl.h>
#endif
#include <math.h>
#include <asterisk/localtime.h>

extern char **environ;

#define TOKEN_SCRIPT_TIMEOUT_MS 2000

/* The helper may return only one short line: ACK or a backoff in seconds. */
static int token_script_result(const char *path, const char *device_name, const char *host,
	const char *device_type, int *backoff)
{
	int fds[2] = { -1, -1 };
	posix_spawn_file_actions_t actions;
	pid_t child = -1;
	struct timespec start, now;
	char output[32] = "";
	size_t used = 0;
	int status = 0;
	int result = -1;
	int reaped = 0;
	char *const args[] = { (char *)path, (char *)device_name, (char *)host, (char *)device_type, NULL };

	if (pipe(fds) != 0)
		return -1;
	if (fds[1] <= STDERR_FILENO) {
		int moved = fcntl(fds[1], F_DUPFD, STDERR_FILENO + 1);
		if (moved < 0)
			goto done;
		close(fds[1]);
		fds[1] = moved;
	}
	if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(fds[1], F_SETFD, FD_CLOEXEC) < 0)
		goto done;
	if (posix_spawn_file_actions_init(&actions) != 0)
		goto done;
	int setup = posix_spawn_file_actions_addclose(&actions, fds[0]);
	if (setup == 0)
		setup = posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
	if (setup == 0)
		setup = posix_spawn_file_actions_addclose(&actions, fds[1]);
	if (setup == 0)
		setup = posix_spawn(&child, path, &actions, NULL, args, environ);
	posix_spawn_file_actions_destroy(&actions);
	if (setup != 0)
		goto done;
	close(fds[1]);
	fds[1] = -1;
	if (clock_gettime(CLOCK_MONOTONIC, &start) != 0)
		goto done;

	for (;;) {
		if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
			goto done;
		long long elapsed = (now.tv_sec - start.tv_sec) * 1000LL + (now.tv_nsec - start.tv_nsec) / 1000000;
		if (elapsed >= TOKEN_SCRIPT_TIMEOUT_MS)
			goto done;
		int remaining = TOKEN_SCRIPT_TIMEOUT_MS - (int)elapsed;
		struct pollfd pfd = { .fd = fds[0], .events = POLLIN };
		int ready = poll(&pfd, 1, remaining);
		if (ready < 0) {
			if (errno == EINTR)
				continue;
			goto done;
		}
		if (ready == 0 || (pfd.revents & (POLLERR | POLLNVAL)))
			goto done;
		char chunk[64];
		ssize_t count = read(fds[0], chunk, sizeof(chunk));
		if (count < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			goto done;
		}
		if (count == 0)
			break;
		if ((size_t)count >= sizeof(output) - used)
			goto done;
		memcpy(output + used, chunk, (size_t)count);
		used += (size_t)count;
	}
	close(fds[0]);
	fds[0] = -1;

	for (;;) {
		pid_t waited = waitpid(child, &status, WNOHANG);
		if (waited == child) {
			reaped = 1;
			break;
		}
		if (waited < 0) {
			if (errno == ECHILD)
				reaped = 1;
			if (errno != EINTR)
				goto done;
		}
		if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
			goto done;
		long long elapsed = (now.tv_sec - start.tv_sec) * 1000LL + (now.tv_nsec - start.tv_nsec) / 1000000;
		if (elapsed >= TOKEN_SCRIPT_TIMEOUT_MS)
			goto done;
		usleep(10000);
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		goto done;
	if (memchr(output, '\0', used) != NULL)
		goto done;
	output[used] = '\0';
	if (used && output[used - 1] == '\n')
		output[--used] = '\0';
	if (used && output[used - 1] == '\r')
		output[--used] = '\0';
	if (strcasecmp(output, "ACK") == 0) {
		result = 1;
	} else if (used && isdigit((unsigned char)output[0])) {
		char *end;
		errno = 0;
		long seconds = strtol(output, &end, 10);
		if (errno == 0 && *end == '\0' && seconds > 30 && seconds <= INT_MAX) {
			*backoff = (int)seconds;
			result = 0;
		}
	}

done:
	if (fds[0] >= 0)
		close(fds[0]);
	if (fds[1] >= 0)
		close(fds[1]);
	if (child > 0 && !reaped) {
		kill(child, SIGKILL);
		while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
	}
	return result;
}

void handle_unknown_message(constSessionPtr s, devicePtr d, constMessagePtr msg_in)			__NONNULL(1,2,3);
void handle_dialedphonebook_message(constSessionPtr s, devicePtr d, constMessagePtr msg_in)		__NONNULL(1,2,3);
void handle_alarm(constSessionPtr s, devicePtr d, constMessagePtr msg_in)				__NONNULL(1,3);
void handle_token_request(constSessionPtr s, devicePtr d, constMessagePtr msg_in)			__NONNULL(1,3);
void handle_register(constSessionPtr s, devicePtr maybe_d, constMessagePtr msg_in)			__NONNULL(1,3);
void handle_SPCPTokenReq(constSessionPtr s, devicePtr d, constMessagePtr msg_in)			__NONNULL(1,3);
void handle_accessorystatus_message(constSessionPtr s, devicePtr d, constMessagePtr msg_in)		__NONNULL(1,2,3);
void handle_unregister(constSessionPtr s, devicePtr d, constMessagePtr msg_in)				__NONNULL(1,3);
void handle_line_number(constSessionPtr s, devicePtr d, constMessagePtr msg_in)				__NONNULL(1,2,3);
void handle_speed_dial_stat_req(constSessionPtr s, devicePtr d, constMessagePtr msg_in)			__NONNULL(1,2,3);
void handle_stimulus(constSessionPtr s, devicePtr d, constMessagePtr msg_in)				__NONNULL(1,2,3);
void handle_KeepAliveMessage(constSessionPtr s, devicePtr d, constMessagePtr msg_in) 			__NONNULL(1);
void handle_offhook(constSessionPtr s, devicePtr d, constMessagePtr msg_in)				__NONNULL(1,2,3);
void handle_onhook(constSessionPtr s, devicePtr d, constMessagePtr msg_in)				__NONNULL(1,2,3);
void handle_headset(constSessionPtr s, devicePtr d, constMessagePtr msg_in)				__NONNULL(1,2,3);
void handle_capabilities_res(constSessionPtr s, devicePtr d, constMessagePtr msg_in)			__NONNULL(1,2,3);
void handle_soft_key_set_req(constSessionPtr s, devicePtr d, constMessagePtr msg_in)			__NONNULL(1,2,3);
void handle_keypad_button(constSessionPtr s, devicePtr d, constMessagePtr msg_in)			__NONNULL(1,2,3);
void handle_soft_key_event(constSessionPtr s, devicePtr d, constMessagePtr msg_in) 			__NONNULL(1,2,3);
void handle_port_response(constSessionPtr s, devicePtr d, constMessagePtr msg_in)			__NONNULL(1,2,3);
void handle_openReceiveChannelAck(constSessionPtr s, devicePtr d, constMessagePtr msg_in)		__NONNULL(1,2,3);
void handle_OpenMultiMediaReceiveAck(constSessionPtr s, devicePtr d, constMessagePtr msg_in)		__NONNULL(1,2,3);
void handle_ConnectionStatistics(constSessionPtr s, devicePtr device, constMessagePtr msg_in)		__NONNULL(1,2,3);
void handle_ipport(constSessionPtr s, devicePtr d, constMessagePtr msg_in)				__NONNULL(1,2,3);
void handle_version(constSessionPtr s, devicePtr d, constMessagePtr msg_in)				__NONNULL(1,2,3);
void handle_ServerResMessage(constSessionPtr s, devicePtr d, constMessagePtr msg_in)			__NONNULL(1,2,3);
void handle_ConfigStatMessage(constSessionPtr s, devicePtr d, constMessagePtr msg_in)			__NONNULL(1,2,3);
void handle_EnblocCallMessage(constSessionPtr s, devicePtr d, constMessagePtr msg_in)			__NONNULL(1,2,3);
void handle_forward_stat_req(constSessionPtr s, devicePtr d, constMessagePtr msg_in)			__NONNULL(1,2,3);
void handle_feature_stat_req(constSessionPtr s, devicePtr d, constMessagePtr msg_in)			__NONNULL(1,2,3);
void handle_services_stat_req(constSessionPtr s, devicePtr d, constMessagePtr msg_in)			__NONNULL(1,2,3);
void handle_updatecapabilities_message(constSessionPtr s, devicePtr d, constMessagePtr msg_in)		__NONNULL(1,2,3);
void handle_updatecapabilities_V2_message(constSessionPtr s, devicePtr d, constMessagePtr msg_in)	__NONNULL(1,2,3);
void handle_updatecapabilities_V3_message(constSessionPtr s, devicePtr d, constMessagePtr msg_in)	__NONNULL(1,2,3);
void handle_startMediaTransmissionAck(constSessionPtr s, devicePtr d, constMessagePtr msg_in)		__NONNULL(1,2,3);
void handle_extension_devicecaps(constSessionPtr s, devicePtr d, constMessagePtr msg_in)           	__NONNULL(1,2,3);
void handle_device_to_user(constSessionPtr s, devicePtr d, constMessagePtr msg_in)			__NONNULL(1,2,3);
void handle_device_to_user_response(constSessionPtr s, devicePtr d, constMessagePtr msg_in)		__NONNULL(1,2,3);
void handle_XMLAlarmMessage(constSessionPtr s, devicePtr d, constMessagePtr msg_in)			__NONNULL(1,3);
void handle_LocationInfoMessage(constSessionPtr s, devicePtr d, constMessagePtr msg_in)			__NONNULL(1,3);
void handle_startMultiMediaTransmissionAck(constSessionPtr s, devicePtr d, constMessagePtr msg_in)	__NONNULL(1,2,3);
void handle_mediaTransmissionFailure(constSessionPtr s, devicePtr d, constMessagePtr msg_in)		__NONNULL(1,2,3);
void handle_miscellaneousCommandMessage(constSessionPtr s, devicePtr d, constMessagePtr msg_in)		__NONNULL(1,2,3);
void handle_hookflash(constSessionPtr s, devicePtr d, constMessagePtr msg_in)				__NONNULL(1,2,3);

/* Returns -1 or the device, retained for this message when deviceIsNecessary */
gcc_inline static devicePtr check_session_message_device(constSessionPtr s, constMessagePtr msg, const char * msgtypestr, boolean_t deviceIsNecessary)
{
	int errors = 0;
	if (!msg) {
		pbx_log(LOG_ERROR, "SCCP: %s handler was called without a message (caller bug); not processed\n", msgtypestr);
		errors++;
	}

 	if (!sccp_session_isValid(s)) {
		pbx_log(LOG_ERROR, "SCCP: %s arrived on a session that is closed or being torn down; not processed\n", msgtypestr);
		errors++;
	}

	if (msg && (GLOB(debug) & (DEBUGCAT_MESSAGE)) != 0) {
		sccp_mid_t mid = letohl(msg->header.lel_messageId);
		pbx_log(LOG_NOTICE, "%s: received %s (0x%04X), %d bytes\n", sccp_session_getDesignator(s), msginfo2str(mid), mid, msg->header.length);
		sccp_dump_msg(msg);
	}

	if (!errors) {
		if (deviceIsNecessary) {
			devicePtr device = sccp_session_getDevice(s, deviceIsNecessary);
			if (!device) {
				pbx_log(LOG_WARNING, "%s: %s needs a registered device, but this connection has none (never registered, or already released); ignored\n", sccp_session_getDesignator(s), msgtypestr);
				return NULL;
			}
			skinny_registrationstate_t registrationState = sccp_device_getRegistrationState(device);
			if (registrationState != SKINNY_DEVICE_RS_PROGRESS && registrationState != SKINNY_DEVICE_RS_OK) {
				pbx_log(LOG_WARNING, "%s: %s ignored: it is only valid while registering or registered, but the device is in registration state %s\n", device->id, msgtypestr,
				        skinny_registrationstate2str(registrationState));
				return NULL;
			}
			return device;
		}
	}
	return NULL;
}

struct messageMap_cb {
	void (*const messageHandler_cb)(constSessionPtr s, devicePtr d, constMessagePtr msg);
	boolean_t deviceIsNecessary;
};

static const struct messageMap_cb sccpMessagesCbMap[SCCP_MESSAGE_HIGH_BOUNDARY + 1] = {
	[KeepAliveMessage] = {handle_KeepAliveMessage, FALSE},						// on 7985,6911 phones and tokenmsg, a KeepAliveMessage is send before register/token
	[OffHookMessage] = {handle_offhook, TRUE},
	[OnHookMessage] = {handle_onhook, TRUE},
	[HookFlashMessage] = {handle_hookflash, TRUE},
	[SoftKeyEventMessage] = {handle_soft_key_event, TRUE},
	[PortResponseMessage] = {handle_port_response, TRUE},
	[OpenReceiveChannelAck] = {handle_openReceiveChannelAck, TRUE},
	[OpenMultiMediaReceiveChannelAckMessage] = {handle_OpenMultiMediaReceiveAck, TRUE},
	[StartMediaTransmissionAck] = {handle_startMediaTransmissionAck, TRUE},
	[IpPortMessage] = {handle_ipport, TRUE},
	[VersionReqMessage] = {handle_version, TRUE},
	[CapabilitiesResMessage] = {handle_capabilities_res, TRUE},
	[ButtonTemplateReqMessage] = {sccp_handle_button_template_req, TRUE},
	[SoftKeyTemplateReqMessage] = {sccp_handle_soft_key_template_req, TRUE},
	[SoftKeySetReqMessage] = {handle_soft_key_set_req, TRUE},
	[LineStatReqMessage] = {handle_line_number, TRUE},
	[SpeedDialStatReqMessage] = {handle_speed_dial_stat_req, TRUE},
	[StimulusMessage] = {handle_stimulus, TRUE},
	[HeadsetStatusMessage] = {handle_headset, TRUE},
	[TimeDateReqMessage] = {sccp_handle_time_date_req, TRUE},
	[KeypadButtonMessage] = {handle_keypad_button, TRUE},
	[ConnectionStatisticsRes] = {handle_ConnectionStatistics, TRUE},
	[ServerReqMessage] = {handle_ServerResMessage, TRUE},
	[ConfigStatReqMessage] = {handle_ConfigStatMessage, TRUE},
	[EnblocCallMessage] = {handle_EnblocCallMessage, TRUE},
	[RegisterAvailableLinesMessage] = {sccp_handle_AvailableLines, TRUE},
	[ForwardStatReqMessage] = {handle_forward_stat_req, TRUE},
	[FeatureStatReqMessage] = {handle_feature_stat_req, TRUE},
	[ServiceURLStatReqMessage] = {handle_services_stat_req, TRUE},
	[AccessoryStatusMessage] = {handle_accessorystatus_message, TRUE},
	[SubscriptionStatReqMessage] = {handle_dialedphonebook_message, TRUE},
	[UpdateCapabilitiesMessage] = {handle_updatecapabilities_message, TRUE},
	[UpdateCapabilitiesV2Message] = {handle_updatecapabilities_V2_message, TRUE},
	[UpdateCapabilitiesV3Message] = {handle_updatecapabilities_V3_message, TRUE},
	[MediaPathCapabilityMessage] = {handle_unknown_message, TRUE},
	[DisplayDynamicNotifyMessage] = {handle_unknown_message, TRUE},
	[DisplayDynamicPriNotifyMessage] = {handle_unknown_message, TRUE},
	[ExtensionDeviceCaps] = {handle_extension_devicecaps, TRUE},
	[DeviceToUserDataVersion1Message] = {handle_device_to_user, TRUE},
	[DeviceToUserDataResponseVersion1Message] = {handle_device_to_user_response, TRUE},
	[RegisterTokenRequest] = {handle_token_request, FALSE},
	[UnregisterMessage] = {handle_unregister, FALSE},
	[RegisterMessage] = {handle_register, FALSE},
	[AlarmMessage] = {handle_alarm, FALSE},
	[XMLAlarmMessage] = {handle_XMLAlarmMessage, FALSE},
	[LocationInfoMessage] = {handle_LocationInfoMessage, FALSE},
	[StartMultiMediaTransmissionAck] = {handle_startMultiMediaTransmissionAck, TRUE},
	[MediaTransmissionFailure] = {handle_mediaTransmissionFailure, TRUE},
	[MiscellaneousCommandMessage] = {handle_miscellaneousCommandMessage, TRUE},
	[CallCountReqMessage] = {handle_unknown_message, FALSE},
};

static const struct messageMap_cb spcpMessagesCbMap[SPCP_MESSAGE_HIGH_BOUNDARY + 1- SPCP_MESSAGE_OFFSET] = {
	[SPCPRegisterTokenRequest - SPCP_MESSAGE_OFFSET] = {handle_SPCPTokenReq, FALSE},
};

int sccp_handle_message(constMessagePtr msg, constSessionPtr s)
{
	const struct messageMap_cb *messageMap_cb = NULL;
	sccp_mid_t mid = KeepAliveMessage;

	if (!s) {
		pbx_log(LOG_ERROR, "SCCP: sccp_handle_message() was called without a session (caller bug); message not processed\n");
		return -1;
	}

	if (!msg) {
		pbx_log(LOG_ERROR, "%s: sccp_handle_message() was called without a message (caller bug)\n", sccp_session_getDesignator(s));
		return -2;
	}

	mid = letohl(msg->header.lel_messageId);

	if (mid <= SCCP_MESSAGE_HIGH_BOUNDARY) {
		messageMap_cb = &sccpMessagesCbMap[mid];
	} else if ((mid >= SPCP_MESSAGE_LOW_BOUNDARY && mid <= SPCP_MESSAGE_HIGH_BOUNDARY)) {
		messageMap_cb = &spcpMessagesCbMap[mid - SPCP_MESSAGE_OFFSET];
	} else {
		pbx_log(LOG_WARNING, "%s: received message ID 0x%04X, outside the known SCCP and SPCP ranges; ignored\n", sccp_session_getDesignator(s), mid);
		handle_unknown_message(s, NULL, msg);
		return 0;
	}
	sccp_log((DEBUGCAT_MESSAGE))(VERBOSE_PREFIX_3 "%s: received %s (0x%X)\n", sccp_session_getDesignator(s), msginfo2str(mid), mid);

	AUTO_RELEASE(sccp_device_t, device, check_session_message_device(s, msg, msginfo2str(mid), messageMap_cb->deviceIsNecessary));
	if (messageMap_cb->messageHandler_cb && messageMap_cb->deviceIsNecessary == TRUE && !device) {
		sccp_log((DEBUGCAT_MESSAGE))(VERBOSE_PREFIX_3 "%s: %s (0x%04X) needs a registered device; ignored\n", sccp_session_getDesignator(s), msginfo2str(mid), mid);
		return -3;
	}
	if (messageMap_cb->messageHandler_cb) {
		messageMap_cb->messageHandler_cb(s, device, msg);
	}

	if (device && sccp_device_getRegistrationState(device) == SKINNY_DEVICE_RS_PROGRESS && mid == device->protocol->registrationFinishedMessageId) {
		sccp_dev_set_registered(device, SKINNY_DEVICE_RS_OK);
		char servername[StationMaxDisplayNotifySize];

		snprintf(servername, sizeof(servername), "%s %s", GLOB(servername), SKINNY_DISP_CONNECTED);
		sccp_dev_displaynotify(device, servername, 5);
	}
	return 0;
}

void sccp_handle_backspace(constDevicePtr d, const uint8_t lineInstance, const uint32_t callid)
{
	pbx_assert(d != NULL && d->session != NULL);
	sccp_msg_t *msg_out = NULL;

	REQ(msg_out, BackSpaceResMessage);
	if (!msg_out) {
		return;
	}
	msg_out->data.BackSpaceResMessage.lel_lineInstance = htolel(lineInstance);
	msg_out->data.BackSpaceResMessage.lel_callReference = htolel(callid);
	sccp_dev_send(d, msg_out);

	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: backspace sent on line instance %u, call %u\n", d->id, lineInstance, callid);
}

void sccp_handle_dialtone(constDevicePtr d, constLinePtr l, constChannelPtr channel)
{
	pbx_assert(d != NULL && l != NULL && channel != NULL);

	if (channel->softswitch_action != SCCP_SOFTSWITCH_DIAL || channel->scheduler.hangup_id > -1 || channel->state == SCCP_CHANNELSTATE_DIALING) {
		return;
	}

	/* we check dialtone just in DIALING action
	 * otherwise, you'll get secondary dialtone also
	 * when catching call forward number, meetme room,
	 * etc.
	 * */
	if (sccp_strlen_zero(channel->dialedNumber) && channel->state != SCCP_CHANNELSTATE_OFFHOOK) {
		channel->setTone(channel, l->initial_dialtone_tone, SKINNY_TONEDIRECTION_USER);
	} else if (!sccp_strlen_zero(channel->dialedNumber)) {
		sccp_indicate(d, channel, SCCP_CHANNELSTATE_DIGITSFOLL);
	}
}

static void log_unknown_devicetype(const char *deviceName, uint32_t deviceType)
{
	pbx_log(LOG_WARNING, "%s: device type %u is not in chan_sccp's device table; continuing, but button layout, softkeys and features may not match this phone\n", deviceName, deviceType);
}

void handle_unknown_message(constSessionPtr no_s, devicePtr no_d, constMessagePtr msg_in)
{
	sccp_mid_t mid = letohl(msg_in->header.lel_messageId);
	if ((GLOB(debug) & DEBUGCAT_MESSAGE) != 0) {								// only show when debugging messages
		pbx_log(LOG_WARNING, "SCCP: received %s (0x%04X), %d bytes, which has no handler\n", msginfo2str(mid), mid, msg_in->header.length);
		sccp_dump_msg(msg_in);
	}
}

/*
 * Interesting values for Last =
 * 0 Phone Load Is Rejected
 * 1 Phone Load TFTP Size Error
 * 2 Phone Load Compressor Error
 * 3 Phone Load Version Error
 * 4 Disk Full Error
 * 5 Checksum Error
 * 6 Phone Load Not Found in TFTP Server
 * 7 TFTP Timeout
 * 8 TFTP Access Error
 * 9 TFTP Error
 * 10 CCM TCP Connection timeout
 * 11 CCM TCP Connection Close because of bad Ack
 * 12 CCM Resets TCP Connection
 * 13 CCM Aborts TCP Connection
 * 14 CCM TCP Connection Closed
 * 15 CCM TCP Connection Closed because ICMP Unreachable
 * 16 CCM Rejects TCP Connection
 * 17 Keepalive Time Out
 * 18 Fail Back to Primary CCM
 * 20 User Resets Phone By Keypad
 * 21 Phone Resets because IP configuration
 * 22 CCM Resets Phone
 * 23 CCM Restarts Phone
 * 24 CCM Rejects Phone Registration
 * 25 Phone Initializes
 * 26 CCM TCP Connection Closed With Unknown Reason
 * 27 Waiting For State From CCM
 * 28 Waiting For Response From CCM
 * 29 DSP Alarm
 * 30 Phone Abort CCM TCP Connection
 * 31 File Authorization Failed
 */

void handle_alarm(constSessionPtr s, devicePtr no_d, constMessagePtr msg_in)
{
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: alarm: severity %s (%d), %s [%d/%d]\n",
		skinny_alarm2str(letohl(msg_in->data.AlarmMessage.lel_alarmSeverity)),
		letohl(msg_in->data.AlarmMessage.lel_alarmSeverity),
		msg_in->data.AlarmMessage.text,
		letohl(msg_in->data.AlarmMessage.lel_parm1),
		letohl(msg_in->data.AlarmMessage.lel_parm2)
	);
}

void handle_XMLAlarmMessage(constSessionPtr s, devicePtr no_d, constMessagePtr msg_in)
{
	sccp_mid_t mid = letohl(msg_in->header.lel_messageId);
	char alarmName[101];
	int reasonEnum = 0;
	char lastProtocolEventSent[101];
	char lastProtocolEventReceived[101];

	char *xmlData = pbx_strdupa((char *) &msg_in->data.XMLAlarmMessage);
	char *state = "";
	char *line = "";

	for (line = strtok_r(xmlData, "\n", &state); line != NULL; line = strtok_r(NULL, "\n", &state)) {
		sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s\n", line);

		if (sscanf(line, "<Alarm Name=\"%[a-zA-Z]\">", alarmName) == 1) {
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_2 "alarm type: %s\n", alarmName);
		}
		if (sscanf(line, "<Enum name=\"ReasonForOutOfService\">%d</Enum>>", &reasonEnum) == 1) {
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_2 "reason: %d\n", reasonEnum);
		}
		if (sscanf(line, "<String name=\"LastProtocolEventSent\">%[^<]</String>", lastProtocolEventSent) == 1) {
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_2 "last event sent: %s\n", lastProtocolEventSent);
		}
		if (sscanf(line, "<String name=\"LastProtocolEventReceived\">%[^<]</String>", lastProtocolEventReceived) == 1) {
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_2 "last event received: %s\n", lastProtocolEventReceived);
		}
	}
	if ((GLOB(debug) & DEBUGCAT_MESSAGE) != 0) {								// only show when debugging messages
		pbx_log(LOG_WARNING, "SCCP: received %s (0x%04X), %d bytes\n", msginfo2str(mid), mid, msg_in->header.length);
		sccp_dump_msg(msg_in);
	}
}

void handle_LocationInfoMessage(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	char *xmldata = pbx_strdupa(msg_in->data.LocationInfoMessage.xmldata);
	sccp_log(DEBUGCAT_DEVICE)(VERBOSE_PREFIX_2 "SCCP: location info (Wi-Fi): %s\n", xmldata);

	if ((GLOB(debug) & DEBUGCAT_MESSAGE) != 0) {								// only show when debugging messages
		sccp_dump_msg(msg_in);
        }
}

void handle_token_request(constSessionPtr s, devicePtr no_d, constMessagePtr msg_in)
{
	char deviceName[sizeof(msg_in->data.RegisterTokenRequest.sId.deviceName) + 1];
	uint32_t serverPriority = GLOB(server_priority);
	uint32_t deviceInstance = 0;
	skinny_devicetype_t deviceType = SKINNY_DEVICETYPE_UNDEFINED;

	memcpy(deviceName, msg_in->data.RegisterTokenRequest.sId.deviceName, sizeof(deviceName) - 1);
	deviceName[sizeof(deviceName) - 1] = '\0';
	deviceInstance = letohl(msg_in->data.RegisterTokenRequest.sId.lel_instance);
	deviceType = letohl(msg_in->data.RegisterTokenRequest.lel_deviceType);
	int token_backoff_time = GLOB(token_backoff_time) >= 30 ? GLOB(token_backoff_time) : 60;

	if (GLOB(reload_in_progress)) {
		pbx_log(LOG_NOTICE, "%s: token request refused because a configuration reload is in progress; phone told to retry in 10 seconds\n", deviceName);
		sccp_session_tokenReject(s, 10);
		return;
	}
	if (!skinny_devicetype_exists(deviceType)) {
		log_unknown_devicetype(deviceName, deviceType);
	}

	sccp_log((DEBUGCAT_MESSAGE | DEBUGCAT_ACTION | DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_2 "%s: token request, instance %d, type %s (%d)\n", deviceName, deviceInstance, skinny_devicetype2str(deviceType), deviceType);
	{
		AUTO_RELEASE(sccp_device_t, tmpdevice , sccp_device_find_byid(deviceName, FALSE));
		if (tmpdevice) {
			skinny_registrationstate_t state = sccp_device_getRegistrationState(tmpdevice);
			if (state == SKINNY_DEVICE_RS_TOKEN && time(0) < tmpdevice->registrationTime + token_backoff_time) {
				pbx_log(LOG_NOTICE, "%s: token request refused: the device already has a token request in progress (token %s, last attempt %d s ago); retry in %d seconds\n", deviceName,
					sccp_tokenstate2str(tmpdevice->status.token), (int)(time(0) - tmpdevice->registrationTime), token_backoff_time);
				tmpdevice->registrationTime = time(0);
				sccp_session_tokenReject(s, token_backoff_time);
				return;
			}
			if (sccp_session_check_crossdevice(s, tmpdevice) || (state != SKINNY_DEVICE_RS_FAILED && state != SKINNY_DEVICE_RS_NONE)) {
				pbx_log(LOG_NOTICE, "%s: token request refused: the device still has another connection (registration state %s); closing it, retry in 10 seconds\n", deviceName, skinny_registrationstate2str(state));
				tmpdevice->registrationTime = time(0);
				sccp_session_crossdevice_cleanup(s, tmpdevice->session);
				sccp_session_tokenReject(s, 10);
				sccp_session_stopthread(s, SKINNY_DEVICE_RS_FAILED);
				tmpdevice->session = NULL;
				return;
			}
		}
	}

	AUTO_RELEASE(sccp_device_t, device, sccp_device_find_byid(deviceName, TRUE));
	if (!device && GLOB(allowAnonymous)) {
		device = sccp_device_createAnonymous(deviceName) /*ref_replace*/;
		sccp_config_applyDeviceConfiguration(device, NULL);
		sccp_config_addButton(&device->buttonconfig, 1, LINE, GLOB(hotline)->line ? GLOB(hotline)->line->name : "Hotline", NULL, NULL);
		device->defaultLineInstance = SCCP_FIRST_LINEINSTANCE;
		sccp_device_addToGlobals(device);
	}

	if (!device) {
		pbx_log(LOG_NOTICE, "%s: token request refused: no such device in sccp.conf or realtime, and hotline_enabled is off\n", deviceName);
		sccp_session_tokenReject(s, token_backoff_time);
		return;
	}

	sccp_session_setProtocol(s, SCCP_PROTOCOL);
	if (sccp_session_retainDevice(s, device) < 0) {
		pbx_log(LOG_WARNING, "%s: could not attach the device to this connection because the device is being removed (for example by a reload); refused\n", DEV_ID_LOG(device));
		sccp_session_tokenReject(s, token_backoff_time);
		goto EXIT;
	}
	device->status.token = SCCP_TOKEN_STATE_REJ;
	device->skinny_type = deviceType;

	if (device->checkACL(device) == FALSE) {
		struct sockaddr_storage sas = { 0 };
		sccp_session_getSas(s, &sas);
		pbx_log(LOG_NOTICE, "%s: refused: address %s is not allowed by the device's deny/permit/permithost settings\n", deviceName, sccp_netsock_stringify_addr(&sas));
		sccp_device_setRegistrationState(device, SKINNY_DEVICE_RS_FAILED);
		sccp_session_tokenReject(s, token_backoff_time);
		goto EXIT;
	}

	boolean_t sendAck = TRUE;
	if (!sccp_strlen_zero(GLOB(token_fallback))) {
		if (sccp_false(GLOB(token_fallback))) {
			sendAck = FALSE;
		} else if (sccp_true(GLOB(token_fallback))) {
			sendAck = serverPriority == 1;
		} else if (!strcasecmp("odd", GLOB(token_fallback)) || !strcasecmp("even", GLOB(token_fallback))) {
			size_t len = strlen(deviceName);
			int digit = -1;
			if (len) {
				unsigned char last = (unsigned char)deviceName[len - 1];
				int upper = toupper(last);
				if (last >= '0' && last <= '9')
					digit = last - '0';
				else if (upper >= 'A' && upper <= 'F')
					digit = upper - 'A' + 10;
			}
			sendAck = digit >= 0 && (digit % 2 == 1) == !strcasecmp("odd", GLOB(token_fallback));
			if (digit < 0)
				pbx_log(LOG_WARNING, "%s: fallback=%s needs a device name ending in a hex digit; token refused\n", deviceName, GLOB(token_fallback));
		} else if (strstr(GLOB(token_fallback), "/") != NULL) {
			struct sockaddr_storage sas = { 0 };
			sccp_session_getSas(s, &sas);
			int script_result = token_script_result(GLOB(token_fallback), deviceName,
				sccp_netsock_stringify_host(&sas), skinny_devicetype2str(deviceType), &token_backoff_time);
			sendAck = script_result == 1;
			if (script_result < 0)
				pbx_log(LOG_WARNING, "%s: fallback script '%s' failed, timed out, or did not print ACK or a retry time in seconds; token refused\n", deviceName, GLOB(token_fallback));
		} else {
			pbx_log(LOG_WARNING, "%s: fallback=%s is not a boolean, odd, even or a script path; token granted\n", deviceName, GLOB(token_fallback));
		}
	} else {
		sccp_log((DEBUGCAT_DEVICE))(VERBOSE_PREFIX_3 "%s: fallback is not set; token granted\n", deviceName);
	}

	device->keepalive = device->keepaliveinterval = device->keepalive ? device->keepalive : GLOB(keepalive);

	sccp_device_setRegistrationState(device, SKINNY_DEVICE_RS_TOKEN);
	if (sendAck) {
		sccp_log_and((DEBUGCAT_ACTION + DEBUGCAT_CORE)) (VERBOSE_PREFIX_2 "%s: token granted\n", deviceName);
		sccp_session_tokenAck(s);
	} else {
		sccp_log_and((DEBUGCAT_ACTION + DEBUGCAT_CORE)) (VERBOSE_PREFIX_2 "%s: token refused (fallback=%s, server priority %d); phone asks again in %d seconds\n", deviceName, GLOB(token_fallback), serverPriority, token_backoff_time);
		sccp_session_tokenReject(s, token_backoff_time);
	}

	device->status.token = (sendAck) ? SCCP_TOKEN_STATE_ACK : SCCP_TOKEN_STATE_REJ;
EXIT:
	if(device) {
		device->registrationTime = time(0);
	}
}

/* A phone that has fallen back to a secondary server (fallback server in its cnf.xml) keeps sending token requests to the primary. */
void handle_SPCPTokenReq(constSessionPtr s, devicePtr no_d, constMessagePtr msg_in)
{
	char *deviceName = "";
	uint32_t deviceInstance = 0;
	skinny_devicetype_t deviceType = SKINNY_DEVICETYPE_UNDEFINED;

	deviceInstance = letohl(msg_in->data.SPCPRegisterTokenRequest.sId.lel_instance);
	deviceName = pbx_strdupa(msg_in->data.RegisterTokenRequest.sId.deviceName);
	deviceType = letohl(msg_in->data.SPCPRegisterTokenRequest.lel_deviceType);
	int token_backoff_time = GLOB(token_backoff_time) >= 30 ? GLOB(token_backoff_time) : 60;

	if (GLOB(reload_in_progress)) {
		pbx_log(LOG_NOTICE, "%s: token request refused because a configuration reload is in progress; phone told to retry in 10 seconds\n", deviceName);
		sccp_session_tokenReject(s, 10);
		return;
	}

	if (!skinny_devicetype_exists(deviceType)) {
		log_unknown_devicetype(deviceName, deviceType);
	}
	sccp_log((DEBUGCAT_DEVICE))(VERBOSE_PREFIX_2 "%s: token request, instance %d, type %s (%d)\n", deviceName, deviceInstance, skinny_devicetype2str(deviceType), deviceType);

	struct sockaddr_storage sas = { 0 };
	sccp_session_getSas(s, &sas);
	if (GLOB(ha) && !sccp_apply_ha(GLOB(ha), &sas)) {
		pbx_log(LOG_NOTICE, "%s: token request refused: address %s is not allowed by the global deny/permit settings\n", deviceName, sccp_netsock_stringify_addr(&sas));
		sccp_session_reject(s, "IP not authorized");
		return;
	}

	{
		AUTO_RELEASE(sccp_device_t, tmpdevice , sccp_device_find_byid(deviceName, FALSE));
		if (tmpdevice) {
			skinny_registrationstate_t state = sccp_device_getRegistrationState(tmpdevice);
			if (state == SKINNY_DEVICE_RS_TOKEN && time(0) < tmpdevice->registrationTime + token_backoff_time) {
				pbx_log(LOG_NOTICE, "%s: token request refused: the device already has a token request in progress (token %s, last attempt %d s ago); retry in %d seconds\n", deviceName,
					sccp_tokenstate2str(tmpdevice->status.token), (int)(time(0) - tmpdevice->registrationTime), token_backoff_time);
				tmpdevice->registrationTime = time(0);
				sccp_session_tokenReject(s, token_backoff_time);
				return;
			}
			if (sccp_session_check_crossdevice(s, tmpdevice) || (state != SKINNY_DEVICE_RS_FAILED && state != SKINNY_DEVICE_RS_NONE)) {
				pbx_log(LOG_NOTICE, "%s: token request refused: the device still has another connection (registration state %s); closing it, retry in 10 seconds\n", deviceName, skinny_registrationstate2str(state));
				sccp_session_crossdevice_cleanup(s, tmpdevice->session);
				tmpdevice->registrationTime = time(0);
				sccp_session_tokenRejectSPCP(s, 10);
				sccp_session_stopthread(s, SKINNY_DEVICE_RS_FAILED);
				tmpdevice->session = NULL;
				return;
			}
		}
	}

	AUTO_RELEASE(sccp_device_t, device, sccp_device_find_byid(deviceName, TRUE));
	if (!device && GLOB(allowAnonymous)) {
		device = sccp_device_createAnonymous(msg_in->data.SPCPRegisterTokenRequest.sId.deviceName) /*ref_replace*/;
		sccp_config_applyDeviceConfiguration(device, NULL);
		sccp_config_addButton(&device->buttonconfig, 1, LINE, GLOB(hotline)->line ? GLOB(hotline)->line->name : "Hotline", NULL, NULL);
		device->defaultLineInstance = SCCP_FIRST_LINEINSTANCE;
		sccp_device_addToGlobals(device);
	}

	if (!device) {
		pbx_log(LOG_NOTICE, "%s: token request refused: no such device in sccp.conf or realtime, and hotline_enabled is off\n", deviceName);
		sccp_session_tokenRejectSPCP(s, 60);
		return;
	}

	sccp_session_setProtocol(s, SPCP_PROTOCOL);
	if (sccp_session_retainDevice(s, device) < 0) {
		pbx_log(LOG_WARNING, "%s: could not attach the device to this connection because the device is being removed (for example by a reload); refused\n", DEV_ID_LOG(device));
		sccp_session_tokenRejectSPCP(s, token_backoff_time);
		goto EXIT;
	}
	device->status.token = SCCP_TOKEN_STATE_REJ;
	device->skinny_type = deviceType;

	if (device->checkACL(device) == FALSE) {
		pbx_log(LOG_NOTICE, "%s: refused: address %s is not allowed by the device's deny/permit/permithost settings\n", deviceName, sccp_netsock_stringify_addr(&sas));
		sccp_device_setRegistrationState(device, SKINNY_DEVICE_RS_FAILED);
		sccp_session_tokenRejectSPCP(s, token_backoff_time);
		goto EXIT;
	}

	if (device->session && device->session != s) {
		pbx_log(LOG_NOTICE, "%s: token request refused: the device is already registered on another connection; both connections are closed\n", device->id);
		sccp_device_setRegistrationState(device, SKINNY_DEVICE_RS_FAILED);
		sccp_session_tokenRejectSPCP(s, token_backoff_time);
		device->session = sccp_session_reject(device->session, "Crossover session not allowed");
		goto EXIT;
	}

	device->keepalive = device->keepaliveinterval = device->keepalive ? device->keepalive : GLOB(keepalive);
	sccp_device_setRegistrationState(device, SKINNY_DEVICE_RS_TOKEN);
	device->status.token = SCCP_TOKEN_STATE_ACK;

	sccp_session_tokenAckSPCP(s, 65535);
EXIT:
	if(device) {
		device->registrationTime = time(0);
	}
}

void handle_register(constSessionPtr s, devicePtr maybe_d, constMessagePtr msg_in)
{
	char * phone_ipv4 = NULL;
	char * phone_ipv6 = NULL;

	uint32_t deviceInstance = letohl(msg_in->data.RegisterMessage.sId.lel_instance);
	uint32_t userid = letohl(msg_in->data.RegisterMessage.sId.lel_userid);
	char deviceName[StationMaxDeviceNameSize];

	sccp_copy_string(deviceName, msg_in->data.RegisterMessage.sId.deviceName, StationMaxDeviceNameSize);
	skinny_devicetype_t deviceType = letohl(msg_in->data.RegisterMessage.lel_deviceType);
	StationProtocolFeatures_t protocolFeatures = msg_in->data.RegisterMessage.protocolFeatures;
	uint8_t protocolVer = protocolFeatures.protocolVersion;
	uint8_t macAddress[12];

	memcpy(macAddress, msg_in->data.RegisterMessage.macAddress, 12);

	if (GLOB(reload_in_progress)) {
		pbx_log(LOG_NOTICE, "%s: registration refused because a configuration reload is in progress\n", deviceName);
		sccp_session_reject(s, "Reload in progress");
		return;
	}

	if (!skinny_devicetype_exists(deviceType)) {
		log_unknown_devicetype(deviceName, deviceType);
	}
	sccp_log((DEBUGCAT_MESSAGE | DEBUGCAT_ACTION | DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_1 "%s: registering, instance %d, user ID %d, type %s (%d), protocol %d, firmware '%s'\n", deviceName, deviceInstance, userid, skinny_devicetype2str(deviceType), deviceType, protocolVer, msg_in->data.RegisterMessage.loadInfo);

	AUTO_RELEASE(sccp_device_t, device, maybe_d ? sccp_device_retain(maybe_d) : sccp_device_find_byid(deviceName, TRUE));
	if (device) {
		skinny_registrationstate_t state = sccp_device_getRegistrationState(device);
		if (
			sccp_session_check_crossdevice(s, device) ||
			state == SKINNY_DEVICE_RS_PROGRESS || state == SKINNY_DEVICE_RS_OK ||
			(state == SKINNY_DEVICE_RS_TOKEN && time(0) - device->registrationTime > 60)
		) {
			pbx_log(LOG_NOTICE, "%s: registration refused: the device still has another connection (registration state %s); closing it so the phone can retry\n", DEV_ID_LOG(device), skinny_registrationstate2str(state));
			sccp_session_crossdevice_cleanup(s, device->session);
			sccp_session_reject(s, "Crossover session");
			sccp_device_setRegistrationState(device, SKINNY_DEVICE_RS_FAILED);
			device->session = NULL;
			goto FUNC_EXIT;
		}
	}

	if (!device && GLOB(allowAnonymous)) {
		device = sccp_device_createAnonymous(deviceName) /*ref_replace*/;
		if(device) {
			sccp_config_applyDeviceConfiguration(device, NULL);
			sccp_config_addButton(&device->buttonconfig, 1, LINE, GLOB(hotline)->line ? GLOB(hotline)->line->name : "Hotline", NULL, NULL);
			device->defaultLineInstance = SCCP_FIRST_LINEINSTANCE;
			sccp_device_addToGlobals(device);
		} else {
			pbx_log(LOG_ERROR, "%s: registration refused: could not create an anonymous (hotline) device, out of memory\n", deviceName);
			sccp_session_reject(s, "hotline failed");
			goto FUNC_EXIT;
		}
	}

	if (device) {
		if (sccp_session_retainDevice(s, device) < 0) {
			pbx_log(LOG_WARNING, "%s: could not attach the device to this connection because the device is being removed (for example by a reload); refused\n", DEV_ID_LOG(device));
			sccp_session_reject(s, "register failed");
			goto FUNC_EXIT;
		}

		if (device->checkACL(device) == FALSE) {
			struct sockaddr_storage sas = { 0 };
			sccp_session_getSas(s, &sas);
			pbx_log(LOG_NOTICE, "%s: refused: address %s is not allowed by the device's deny/permit/permithost settings\n", deviceName, sccp_netsock_stringify_addr(&sas));
			sccp_device_setRegistrationState(device, SKINNY_DEVICE_RS_FAILED);
			sccp_session_reject(s, "IP Not Authorized");
			goto FUNC_EXIT;
		}
	} else {
		pbx_log(LOG_NOTICE, "%s: registration refused: no such device in sccp.conf or realtime, and hotline_enabled is off\n", deviceName);
		sccp_session_reject(s, "Device Unknown");
		return;
	}

	device->device_features = protocolFeatures;
	device->linesRegistered = FALSE;

	if (!sccp_strlen_zero(msg_in->data.RegisterMessage.ipv6Address)) {
		device->ipv6.ss_family = AF_INET6;
		struct sockaddr_in6 * sin6 = (struct sockaddr_in6 *)&device->ipv6;
		memcpy(&sin6->sin6_addr, &msg_in->data.RegisterMessage.ipv6Address, sizeof(sin6->sin6_addr));
		sin6->sin6_port = htons(sccp_session_getClientPort(s));
		phone_ipv6 = pbx_strdupa(sccp_netsock_stringify_host(&device->ipv6));
	}

	if(msg_in->data.RegisterMessage.stationIpAddr != 0) {
		device->ipv4.ss_family = AF_INET;
		struct sockaddr_in * sin4 = (struct sockaddr_in *)&device->ipv4;
		memcpy(&sin4->sin_addr, &msg_in->data.RegisterMessage.stationIpAddr, sizeof(sin4->sin_addr));
		sin4->sin_port = htons(sccp_session_getClientPort(s));
		phone_ipv4 = pbx_strdupa(sccp_netsock_stringify_host(&device->ipv4));
		sccp_session_setOurIP4Address(s, &device->ipv4);
	}

	/* auto NAT detection if NAT is not set as device configuration */
	if (SCCP_NAT_AUTO == device->nat || SCCP_NAT_AUTO_OFF == device->nat || SCCP_NAT_AUTO_ON == device->nat) {
		device->nat = SCCP_NAT_AUTO_OFF;
		struct sockaddr_storage session_sas = { 0 };
		sccp_session_getSas(s, &session_sas);
		sccp_netsock_ipv4_mapped(&session_sas, &session_sas);

		struct ast_str *ha_localnet_buf = pbx_str_alloca(DEFAULT_PBX_STR_BUFFERSIZE);
		sccp_print_ha(ha_localnet_buf, DEFAULT_PBX_STR_BUFFERSIZE, GLOB(localaddr));

		if (session_sas.ss_family == AF_INET) {
			char *session_ipv4 = pbx_strdupa(sccp_netsock_stringify_host(&session_sas));
			if (GLOB(localaddr) && sccp_apply_ha_default(GLOB(localaddr), &session_sas, AST_SENSE_DENY) != AST_SENSE_ALLOW) {
				sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: NAT detected: session address %s (phone reports %s) is outside localnet (%s); RTP uses externip/externhost\n", deviceName, session_ipv4, phone_ipv4, pbx_str_buffer(ha_localnet_buf));
				device->nat = SCCP_NAT_AUTO_ON;
			} else if(sccp_netsock_cmp_addr(&session_sas, &device->ipv4)) {
				sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: NAT detected: session address %s differs from the address the phone reports (%s); RTP uses externip/externhost\n", deviceName, session_ipv4, phone_ipv4);
				device->nat = SCCP_NAT_AUTO_ON;
			}
		} else {
			char *session_ipv6 = pbx_strdupa(sccp_netsock_stringify_host(&session_sas));
			if(sccp_netsock_cmp_addr(&session_sas, &device->ipv6)) {
				sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: NAT detected: session address %s differs from the address the phone reports (%s); RTP uses externip/externhost\n", deviceName, session_ipv6, phone_ipv6);
				device->nat = SCCP_NAT_AUTO_ON;
			}
		}
	} else {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: nat=%s set in sccp.conf\n", deviceName, sccp_nat2str(device->nat));
	}

	device->skinny_type = deviceType;

	sccp_session_resetLastKeepAlive(s);
	device->protocolversion = protocolVer;
	device->status.token = SCCP_TOKEN_STATE_NOTOKEN;
	sccp_copy_string(device->loadedimageversion, msg_in->data.RegisterMessage.loadInfo, StationMaxImageVersionSize);

	/** workaround to fix the protocol version issue for ata devices */
	/*
	 * MAC-Address        : ATA00215504e821
	 * Protocol Version   : Supported '33', In Use '17'
	 */
	if (device->skinny_type == SKINNY_DEVICETYPE_ATA188 || device->skinny_type == SKINNY_DEVICETYPE_ATA186) {
		device->protocolversion = SCCP_DRIVER_SUPPORTED_PROTOCOL_LOW;
	}

	device->protocol = sccp_protocol_getDeviceProtocol(device, sccp_session_getProtocol(s));

	device->keepalive = device->keepalive ? device->keepalive : GLOB(keepalive);
	device->keepaliveinterval = ((device->keepalive / 4) * 3) + (sccp_random() % (device->keepalive / 4)) + 1;

	device->inuseprotocolversion = device->protocol->version;
	sccp_device_preregistration(device);

	device->protocol->sendRegisterAck(device, device->keepaliveinterval, device->keepaliveinterval, GLOB(dateformat));

	sccp_dev_set_registered(device, SKINNY_DEVICE_RS_PROGRESS);

	sccp_dev_sendmsg(device, CapabilitiesReqMessage);
	return;

FUNC_EXIT:
	sccp_session_stopthread(s, SKINNY_DEVICE_RS_FAILED);
	if (device) {
		device->session = NULL;
	}
}

static btnlist *sccp_make_button_template(devicePtr d)
{
	int i = 0;
	btnlist * btn = NULL;
	sccp_buttonconfig_t * buttonconfig = NULL;

	if (!d) {
		return NULL;
	}
	if (!(btn = (btnlist *)sccp_calloc(sizeof *btn, StationMaxButtonTemplateSize))) {
		return NULL;
	}
	sccp_dev_build_buttontemplate(d, btn);

	uint16_t speeddialInstance = SCCP_FIRST_SPEEDDIALINSTANCE;
	uint16_t lineInstance = SCCP_FIRST_LINEINSTANCE;
	uint16_t serviceInstance = SCCP_FIRST_SERVICEINSTANCE;
	boolean_t defaultLineSet = FALSE;

	if (!d->isAnonymous) {
		SCCP_LIST_LOCK(&d->buttonconfig);
		SCCP_LIST_TRAVERSE(&d->buttonconfig, buttonconfig, list) {
			if (buttonconfig->instance > 0) {
				continue;
			}
			for (i = 0; i < StationMaxButtonTemplateSize; i++) {
				if (!(btn[i].type >= SCCP_BUTTONTYPE_MULTI && btn[i].type <= SCCP_BUTTONTYPE_ABBRDIAL)) {
					continue;
				}

				if (buttonconfig->type == LINE && !sccp_strlen_zero(buttonconfig->button.line.name)) {
					if (btn[i].type == SCCP_BUTTONTYPE_MULTI || btn[i].type == SCCP_BUTTONTYPE_LINE) {
						btn[i].type = SKINNY_BUTTONTYPE_LINE;

						/*! retains new line in btn[i].ptr, finally released in sccp_dev_clean */
						if ((btn[i].ptr = sccp_line_find_byname(buttonconfig->button.line.name, TRUE))) {
							buttonconfig->instance = btn[i].instance = lineInstance++;
							sccp_linedevice_create(d, btn[i].ptr, btn[i].instance, buttonconfig->button.line.subscriptionId);
							if (FALSE == defaultLineSet && !d->defaultLineInstance) {
								d->defaultLineInstance = buttonconfig->instance;
								defaultLineSet = TRUE;
							}
						} else {
							btn[i].type = SKINNY_BUTTONTYPE_UNUSED;
							buttonconfig->instance = btn[i].instance = 0;
							pbx_log(LOG_WARNING, "%s: button %d refers to line '%s', which is not defined; button left unused\n", DEV_ID_LOG(d), i + 1, buttonconfig->button.line.name);
						}
					} else {
						sccp_log((DEBUGCAT_BUTTONTEMPLATE)) (VERBOSE_PREFIX_3 "%s: line button %d (%s) skipped: no line buttons left on this model\n", d->id, buttonconfig->index + 1, buttonconfig->label);
						btn[i].type = SKINNY_BUTTONTYPE_UNUSED;
					}
					break;
				} else if (buttonconfig->type == EMPTY) {
					if (btn[i].type == SCCP_BUTTONTYPE_MULTI || btn[i].type == SCCP_BUTTONTYPE_LINE || btn[i].type == SCCP_BUTTONTYPE_SPEEDDIAL) {
						btn[i].type = SKINNY_BUTTONTYPE_UNUSED;
						buttonconfig->instance = btn[i].instance = 0;
					} else {
						sccp_log((DEBUGCAT_BUTTONTEMPLATE)) (VERBOSE_PREFIX_3 "%s: empty button %d (%s) skipped: no buttons left on this model\n", d->id, buttonconfig->index + 1, buttonconfig->label);
						btn[i].type = SKINNY_BUTTONTYPE_UNUSED;
					}
					break;
				} else if (buttonconfig->type == SERVICE) {
					if (btn[i].type == SCCP_BUTTONTYPE_MULTI) {
						btn[i].type = SKINNY_BUTTONTYPE_SERVICEURL;
						buttonconfig->instance = btn[i].instance = serviceInstance++;
					} else {
						sccp_log((DEBUGCAT_BUTTONTEMPLATE)) (VERBOSE_PREFIX_3 "%s: service URL button %d (%s) skipped: no buttons left on this model\n", d->id, buttonconfig->index + 1, buttonconfig->label);
						btn[i].type = SKINNY_BUTTONTYPE_UNUSED;
					}
					break;
				} else if (buttonconfig->type == SPEEDDIAL && !sccp_strlen_zero(buttonconfig->label)) {
					if ((btn[i].type == SCCP_BUTTONTYPE_MULTI || btn[i].type == SCCP_BUTTONTYPE_SPEEDDIAL)) {
						if (!sccp_strlen_zero(buttonconfig->button.speeddial.hint)
						    && btn[i].type == SCCP_BUTTONTYPE_MULTI
						    ) {
#ifdef CS_DYNAMIC_SPEEDDIAL
							if (d->inuseprotocolversion >= 15) {
								btn[i].type = SKINNY_BUTTONTYPE_BLFSPEEDDIAL;
								buttonconfig->instance = btn[i].instance = speeddialInstance++;
							} else
#endif
							{
								btn[i].type = SKINNY_BUTTONTYPE_LINE;
								buttonconfig->instance = btn[i].instance = lineInstance++;
							}
						} else {
							btn[i].type = SKINNY_BUTTONTYPE_SPEEDDIAL;
							buttonconfig->instance = btn[i].instance = speeddialInstance++;
						}
					} else if (btn[i].type == SCCP_BUTTONTYPE_ABBRDIAL) {
						btn[i].type = SKINNY_BUTTONTYPE_SPEEDDIAL;
						buttonconfig->instance = btn[i].instance = speeddialInstance++;
					} else {
						sccp_log((DEBUGCAT_BUTTONTEMPLATE)) (VERBOSE_PREFIX_3 "%s: speeddial button %d (%s) skipped: no buttons left on this model\n", d->id, buttonconfig->index + 1, buttonconfig->label);
						btn[i].type = SKINNY_BUTTONTYPE_UNUSED;
					}
 					break;
				} else if (buttonconfig->type == FEATURE && !sccp_strlen_zero(buttonconfig->label)) {
					if (btn[i].type == SCCP_BUTTONTYPE_MULTI) {
						buttonconfig->instance = btn[i].instance = speeddialInstance++;

						switch (buttonconfig->button.feature.id) {
							case SCCP_FEATURE_HOLD:
								btn[i].type = SKINNY_BUTTONTYPE_HOLD;
								break;

							case SCCP_FEATURE_TRANSFER:
								btn[i].type = SKINNY_BUTTONTYPE_TRANSFER;
								break;
#ifdef CS_DEVSTATE_FEATURE
							case SCCP_FEATURE_DEVSTATE:
								/* fall through */
#endif

							case SCCP_FEATURE_MONITOR:
								/* fall through */

							case SCCP_FEATURE_MULTIBLINK:
								if(d->inuseprotocolversion >= 15 && d->skinny_type != SKINNY_DEVICETYPE_CISCO8941 && d->skinny_type != SKINNY_DEVICETYPE_CISCO8945) {
									btn[i].type = SKINNY_BUTTONTYPE_MULTIBLINKFEATURE;
								} else {
									btn[i].type = SKINNY_BUTTONTYPE_FEATURE;
								}
								break;

							case SCCP_FEATURE_DND:
								if (sccp_strlen_zero(buttonconfig->button.feature.options) && d->inuseprotocolversion >= 15) {
									btn[i].type = SKINNY_BUTTONTYPE_MULTIBLINKFEATURE;
								} else {
									btn[i].type = SKINNY_BUTTONTYPE_FEATURE;
								}
								break;

							case SCCP_FEATURE_PARKINGLOT:
#ifdef CS_SCCP_PARK
								if (iParkingLot.attachObserver) {
									if (d->inuseprotocolversion > 15) {
										btn[i].type = SKINNY_BUTTONTYPE_MULTIBLINKFEATURE;
										buttonconfig->button.feature.status = 0x010000;
									} else {
										btn[i].type = SKINNY_BUTTONTYPE_FEATURE;
										buttonconfig->button.feature.status = 0;
									}
								} else {
									btn[i].type = SKINNY_BUTTONTYPE_PARKINGLOT;
								}
#endif
								break;

							case SCCP_FEATURE_MOBILITY:
								btn[i].type = SKINNY_BUTTONTYPE_MOBILITY;
								break;

							case SCCP_FEATURE_CONFERENCE:
								btn[i].type = SKINNY_BUTTONTYPE_CONFERENCE;
								break;

							case SCCP_FEATURE_PICKUP:
								btn[i].type = SKINNY_BUTTONTYPE_GROUPCALLPICKUP;
								break;

							case SCCP_FEATURE_DO_NOT_DISTURB:
								btn[i].type = SKINNY_BUTTONTYPE_DO_NOT_DISTURB;
								break;

							case SCCP_FEATURE_CONF_LIST:
								btn[i].type = SKINNY_BUTTONTYPE_CONF_LIST;
								break;

							case SCCP_FEATURE_REMOVE_LAST_PARTICIPANT:
								btn[i].type = SKINNY_BUTTONTYPE_REMOVE_LAST_PARTICIPANT;
								break;

							case SCCP_FEATURE_HUNT_GROUP_LOG_IN_OUT:
								btn[i].type = SKINNY_BUTTONTYPE_HUNT_GROUP_LOG_IN_OUT;
								break;

							case SCCP_FEATURE_QUALITY_REPORT_TOOL:
								btn[i].type = SKINNY_BUTTONTYPE_QUALITY_REPORT_TOOL;
								break;

							case SCCP_FEATURE_CALLBACK:
								btn[i].type = SKINNY_BUTTONTYPE_CALLBACK;
								break;

							case SCCP_FEATURE_OTHER_PICKUP:
								btn[i].type = SKINNY_BUTTONTYPE_OTHER_PICKUP;
								break;

							case SCCP_FEATURE_VIDEO_MODE:
								btn[i].type = SKINNY_BUTTONTYPE_VIDEO_MODE;
								break;

							case SCCP_FEATURE_NEW_CALL:
								btn[i].type = SKINNY_BUTTONTYPE_NEW_CALL;
								break;

							case SCCP_FEATURE_END_CALL:
								btn[i].type = SKINNY_BUTTONTYPE_END_CALL;
								break;

							case SCCP_FEATURE_TESTF:
								btn[i].type = SKINNY_BUTTONTYPE_TESTF;
								break;

							case SCCP_FEATURE_TESTI:
								btn[i].type = SKINNY_BUTTONTYPE_TESTI;
								break;

							case SCCP_FEATURE_TESTG:
								btn[i].type = SKINNY_BUTTONTYPE_MESSAGES;
								break;

							case SCCP_FEATURE_TESTH:
								btn[i].type = SKINNY_BUTTONTYPE_DIRECTORY;
								break;

							case SCCP_FEATURE_TESTJ:
								btn[i].type = SKINNY_BUTTONTYPE_APPLICATION;
								break;

							default:
								btn[i].type = SKINNY_BUTTONTYPE_FEATURE;
								break;
						}
					} else {
						sccp_log(DEBUGCAT_BUTTONTEMPLATE)(VERBOSE_PREFIX_3 "%s: feature button %d (%s) skipped: no buttons left on this model\n", d->id, buttonconfig->index + 1, buttonconfig->label);
						btn[i].type = SKINNY_BUTTONTYPE_UNUSED;
					}
					break;
				}
			}
		}
		SCCP_LIST_UNLOCK(&d->buttonconfig);
	} else {
		buttonconfig = SCCP_LIST_FIRST(&d->buttonconfig);
		btn[i].type = SKINNY_BUTTONTYPE_LINE;
		btn[i].ptr = sccp_line_retain(GLOB(hotline)->line);
		buttonconfig->instance = btn[i].instance = d->defaultLineInstance = SCCP_FIRST_LINEINSTANCE;
		sccp_linedevice_create(d, btn[i].ptr, btn[i].instance, buttonconfig->button.line.subscriptionId);
	}

	for (i = 0; i < StationMaxButtonTemplateSize; i++) {
		if (btn[i].type == SCCP_BUTTONTYPE_MULTI || btn[i].type == SCCP_BUTTONTYPE_ABBRDIAL) {
			btn[i].type = SKINNY_BUTTONTYPE_UNUSED;
		}
	}

	SCCP_LIST_LOCK(&d->buttonconfig);
	SCCP_LIST_TRAVERSE(&d->buttonconfig, buttonconfig, list) {
		if (buttonconfig->type == LINE && buttonconfig->button.line.options && strcasestr(buttonconfig->button.line.options, "default")) {
			d->defaultLineInstance = buttonconfig->instance;
			sccp_log((DEBUGCAT_LINE))(VERBOSE_PREFIX_3 "default line instance set to %u\n", buttonconfig->instance);
			break;
		}
	}
	SCCP_LIST_UNLOCK(&d->buttonconfig);
	return btn;
}

void sccp_handle_AvailableLines(constSessionPtr s, devicePtr d, constMessagePtr none)
{
	uint8_t i = 0;

	uint8_t line_count = 0;
	btnlist * btn = NULL;

	line_count = 0;

	if (d->linesRegistered) {
		return;
	}
	btn = d->buttonTemplate;

	if (!btn) {
		pbx_log(LOG_WARNING, "%s: line status requested before a button template was built; phone told to restart\n", DEV_ID_LOG(d));
		sccp_device_sendReset(d, SKINNY_RESETTYPE_RESTART);
		return;
	}

	for (i = 0; i < StationMaxButtonTemplateSize; i++) {
		if ((btn[i].type == SKINNY_BUTTONTYPE_LINE) || (btn[i].type == SCCP_BUTTONTYPE_MULTI)) {
			line_count++;
		} else if (btn[i].type == SKINNY_BUTTONTYPE_UNUSED) {
			break;
}
	}

	d->linesRegistered = TRUE;
}

void handle_accessorystatus_message(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	sccp_accessory_t accessory = letohl(msg_in->data.AccessoryStatusMessage.lel_AccessoryID);
	sccp_accessorystate_t state = letohl(msg_in->data.AccessoryStatusMessage.lel_AccessoryStatus);

	sccp_device_setAccessoryStatus(d, accessory, state);

	// these devices don't generate an offhook stimulus
	// use <alwaysUsePrimeLineVoiceMail>true</alwaysUsePrimeLineVoiceMail> in sep-file instead
}

void handle_unregister(constSessionPtr s, devicePtr device, constMessagePtr msg_in)
{
	sccp_msg_t *msg_out = NULL;
	AUTO_RELEASE(sccp_device_t, d , device ? sccp_device_retain(device) : NULL);
	int reason = letohl(msg_in->data.UnregisterMessage.lel_UnregisterReason);

	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: unregister request (reason: %s)\n", DEV_ID_LOG(d), reason ? "Unknown" : "Powersave");

	/* we don't need to look for active channels. the phone does send unregister only when there are no channels */
	REQ(msg_out, UnregisterAckMessage);
	if (!msg_out) {
		return;
	}

	if (d && d->active_channel) {
		msg_out->data.UnregisterAckMessage.lel_status = SKINNY_UNREGISTERSTATUS_NAK;
		sccp_session_send2(s, msg_out);							// send directly to session, skipping device check
		pbx_log(LOG_NOTICE, "%s: unregister refused: call %s is still active\n", DEV_ID_LOG(d), d->active_channel->designator);
		return;
	}

	msg_out->data.UnregisterAckMessage.lel_status = SKINNY_UNREGISTERSTATUS_OK;
	sccp_session_send2(s, msg_out);								// send directly to session, skipping device check
	sccp_log_and((DEBUGCAT_MESSAGE + DEBUGCAT_ACTION)) (VERBOSE_PREFIX_3 "%s: unregister acknowledged\n", DEV_ID_LOG(d));

	sched_yield();
	if (s) {
		sccp_session_stopthread(s, SKINNY_DEVICE_RS_NONE);
	} else {
		sccp_device_setRegistrationState(d, SKINNY_DEVICE_RS_NONE);
	}
}

void sccp_handle_button_template_req(constSessionPtr s, devicePtr d, constMessagePtr none)
{
	btnlist * btn = NULL;
	int i = 0;
	uint8_t buttonCount = 0;

	uint8_t lastUsedButtonPosition = 0;

	sccp_msg_t *msg_out = NULL;

	skinny_registrationstate_t registrationState=sccp_device_getRegistrationState(d);
	if (registrationState != SKINNY_DEVICE_RS_PROGRESS && registrationState != SKINNY_DEVICE_RS_OK) {
		pbx_log(LOG_WARNING, "%s: button template requested while not registering or registered (state %s); connection closed\n", d->id, skinny_registrationstate2str(registrationState));
		sccp_session_stopthread(s, SKINNY_DEVICE_RS_FAILED);
		return;
	}

	/* the template is built once per registration (sccp_dev_clean frees it); a repeated request
	 * resends it, because a rebuild skips the buttons that already have an instance and loses the lines */
	btn = d->buttonTemplate;
	if (!btn) {
		btn = d->buttonTemplate = sccp_make_button_template(d);
		sccp_linedevice_createButtonsArray(d);
	}

	if (!btn) {
		pbx_log(LOG_ERROR, "%s: could not allocate the button template (out of memory); connection closed\n", d->id);
		sccp_session_stopthread(s, SKINNY_DEVICE_RS_FAILED);
		return;
	}

	REQ(msg_out, ButtonTemplateMessage);
	if (!msg_out) {
		return;
	}
	for (i = 0; i < StationMaxButtonTemplateSize; i++) {
		msg_out->data.ButtonTemplateMessage.definition[i].instanceNumber = btn[i].instance;

		if (SKINNY_BUTTONTYPE_UNUSED != btn[i].type) {
			buttonCount = i + 1;
			lastUsedButtonPosition = i;
		}

		switch (btn[i].type) {
			case SCCP_BUTTONTYPE_HINT:
			case SCCP_BUTTONTYPE_LINE:
				if (msg_out->data.ButtonTemplateMessage.definition[i].instanceNumber == 0) {
					msg_out->data.ButtonTemplateMessage.definition[i].buttonDefinition = SKINNY_BUTTONTYPE_UNDEFINED;
				} else {
					msg_out->data.ButtonTemplateMessage.definition[i].buttonDefinition = SKINNY_BUTTONTYPE_LINE;
				}
				break;

			case SCCP_BUTTONTYPE_MULTI:
				/* fall through */

			case SKINNY_BUTTONTYPE_UNUSED:
				msg_out->data.ButtonTemplateMessage.definition[i].buttonDefinition = SKINNY_BUTTONTYPE_UNDEFINED;
				break;

			default:
				msg_out->data.ButtonTemplateMessage.definition[i].buttonDefinition = btn[i].type;
				break;
		}
		if(msg_out->data.ButtonTemplateMessage.definition[i].buttonDefinition != SKINNY_BUTTONTYPE_UNDEFINED) {
			sccp_log((DEBUGCAT_BUTTONTEMPLATE + DEBUGCAT_FEATURE_BUTTON))(VERBOSE_PREFIX_3 "%s: button %.2d = %s (%d), instance %d\n", d->id, i + 1,
										      skinny_buttontype2str(msg_out->data.ButtonTemplateMessage.definition[i].buttonDefinition),
										      msg_out->data.ButtonTemplateMessage.definition[i].buttonDefinition, msg_out->data.ButtonTemplateMessage.definition[i].instanceNumber);
		}
	}

	msg_out->data.ButtonTemplateMessage.lel_buttonOffset = 0;
	msg_out->data.ButtonTemplateMessage.lel_buttonCount = htolel(buttonCount);

	/* buttonCount is already in a little endian format so don't need to convert it now */
	msg_out->data.ButtonTemplateMessage.lel_totalButtonCount = htolel(lastUsedButtonPosition + 1);

	/* set speeddial for older devices like 7912 */
	uint32_t speeddialInstance = 0;
	sccp_buttonconfig_t * config = NULL;

	sccp_log((DEBUGCAT_BUTTONTEMPLATE + DEBUGCAT_SPEEDDIAL))(VERBOSE_PREFIX_3 "%s: assigning instances to unconfigured speeddials\n", d->id);
	SCCP_LIST_TRAVERSE(&d->buttonconfig, config, list) {
		if (config->type == SPEEDDIAL && config->instance == 0) {
			config->instance = speeddialInstance++;
		} else if (config->type == SPEEDDIAL && config->instance != 0) {
			speeddialInstance = config->instance + 1;
		}
	}

	sccp_dev_send(d, msg_out);
}

void handle_line_number(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	sccp_speed_t k;
	sccp_buttonconfig_t * config = NULL;
	uint8_t lineNumber = letohl(msg_in->data.LineStatReqMessage.lel_lineNumber);

	char * dirNumber = "<undef>";
	char * fullyQualifiedDisplayName = "";
	sccp_log((DEBUGCAT_LINE)) (VERBOSE_PREFIX_3 "%s: configuring line %d\n", d->id, lineNumber);

	AUTO_RELEASE(sccp_line_t, l , sccp_line_find_byid(d, lineNumber));
	if(l) {
		dirNumber = l->name;
		if(d->defaultLineInstance == lineNumber && !sccp_strlen_zero(d->description))
			fullyQualifiedDisplayName = d->description;
		else if(!sccp_strlen_zero(l->description))
			fullyQualifiedDisplayName = l->description;
	} else {
		sccp_dev_speed_find_byindex(d, lineNumber, TRUE, &k);
		if(!k.valid) {
			pbx_log(LOG_WARNING, "%s: phone asked for button %d, which is neither a line nor a speeddial with a hint; sent an empty line status\n", sccp_session_getDesignator(s), lineNumber);
			if (d->protocol) {
				d->protocol->sendLineStatResp(d, lineNumber, "", "", "");
			}
			return;
		}
		fullyQualifiedDisplayName = dirNumber = k.name;
	}

	char displayName[SCCP_MAX_LABEL + 1];
	if (l) {
		SCCP_LIST_LOCK(&d->buttonconfig);
		SCCP_LIST_TRAVERSE(&d->buttonconfig, config, list) {
			if (config->type == LINE && config->instance == lineNumber) {
				if (config->button.line.subscriptionId && !sccp_strlen_zero(config->button.line.subscriptionId->label)) {
					if (config->button.line.subscriptionId->replaceCid) {
						snprintf(displayName, SCCP_MAX_LABEL, "%s", config->button.line.subscriptionId->label);
					} else {
						snprintf(displayName, SCCP_MAX_LABEL, "%s%s", l->label, config->button.line.subscriptionId->label);
					}
				} else {
					snprintf(displayName, SCCP_MAX_LABEL, "%s", l->label);
				}
				break;
			}
		}
		SCCP_LIST_UNLOCK(&d->buttonconfig);
	} else {
		snprintf(displayName, SCCP_MAX_LABEL, "%s", k.name);
	}

	d->protocol->sendLineStatResp(d, lineNumber, dirNumber, fullyQualifiedDisplayName, displayName);
}

void handle_speed_dial_stat_req(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	sccp_speed_t k;
	sccp_msg_t *msg_out = NULL;

	int wanted = letohl(msg_in->data.SpeedDialStatReqMessage.lel_speedDialNumber);

	sccp_log((DEBUGCAT_ACTION)) (VERBOSE_PREFIX_3 "%s: speeddial request for button %d\n", sccp_session_getDesignator(s), wanted);

	REQ(msg_out, SpeedDialStatMessage);
	if (!msg_out) {
		return;
	}
	msg_out->data.SpeedDialStatMessage.lel_speedDialNumber = htolel(wanted);

	sccp_dev_speed_find_byindex(d, wanted, FALSE, &k);
	if (k.valid) {
		d->copyStr2Locale(d, msg_out->data.SpeedDialStatMessage.speedDialDirNumber, k.ext, sizeof(msg_out->data.SpeedDialStatMessage.speedDialDirNumber));
		d->copyStr2Locale(d, msg_out->data.SpeedDialStatMessage.speedDialDisplayName, k.name, sizeof(msg_out->data.SpeedDialStatMessage.speedDialDisplayName));
	} else {
		sccp_log((DEBUGCAT_ACTION | DEBUGCAT_BUTTONTEMPLATE)) (VERBOSE_PREFIX_3 "%s: speeddial %d not configured\n", sccp_session_getDesignator(s), wanted);
	}

	sccp_dev_send(d, msg_out);
}

static void handle_stimulus_lastnumberredial(constDevicePtr d, constLinePtr l, const uint16_t instance, const uint32_t callId, const uint32_t stimulusstatus)
{
	sccp_log_and((DEBUGCAT_CORE + DEBUGCAT_ACTION)) (VERBOSE_PREFIX_3 "%s: redial button pressed\n", d->id);

	if (sccp_strlen_zero(d->redialInformation.number)) {
		sccp_log((DEBUGCAT_ACTION))(VERBOSE_PREFIX_3 "%s: redial pressed, but no number has been dialed yet; ignored\n", d->id);
		return;
	}
	AUTO_RELEASE(sccp_channel_t, channel , sccp_device_getActiveChannel(d));
	if (channel) {
		if (channel->state == SCCP_CHANNELSTATE_OFFHOOK) {
			sccp_channel_stop_schedule_digittimout(channel);
			sccp_copy_string(channel->dialedNumber, d->redialInformation.number, sizeof(d->redialInformation.number));
			sccp_pbx_softswitch(channel);
		} else {
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: redial ignored: a call is in progress\n", d->id);
		}
	} else {
		channel = sccp_channel_newcall(l, d, d->redialInformation.number, SKINNY_CALLTYPE_OUTBOUND, NULL, NULL) /*ref_replace*/;
		sccp_channel_stop_schedule_digittimout(channel);
	}
}

static void handle_speeddial(constDevicePtr d, const sccp_speed_t * k)
{
	int len = 0;

	if (!k || !d || !d->session) {
		return;
	}
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: speeddial button %d pressed, number %s\n", d->id, k->instance, k->ext);

	AUTO_RELEASE(sccp_channel_t, channel , sccp_device_getActiveChannel(d));
	if (channel) {
		if (channel->state == SCCP_CHANNELSTATE_OFFHOOK || channel->state == SCCP_CHANNELSTATE_GETDIGITS || channel->state == SCCP_CHANNELSTATE_SPEEDDIAL) {
			sccp_channel_stop_schedule_digittimout(channel);
			len = sccp_strlen(channel->dialedNumber);
			sccp_copy_string(channel->dialedNumber + len, k->ext, sizeof(channel->dialedNumber) - len);
			sccp_pbx_softswitch(channel);
			return;
		}
		if (channel->state >= SCCP_CHANNELSTATE_DIALING && channel->state <= SCCP_CHANNELSTATE_CONNECTEDCONFERENCE) {
			if (!sccp_channel_hold(channel)) {
				pbx_log(LOG_WARNING, "%s: speeddial not dialed: could not put active call %s on hold\n", d->id, channel->designator);
				return;
			}
			/* fall through to start new call */
		} else if (channel->state == SCCP_CHANNELSTATE_HOLD || channel->state == SCCP_CHANNELSTATE_ONHOOK || channel->state == SCCP_CHANNELSTATE_DOWN) {
			/* fall through to start new call */
		} else {
			pbx_log(LOG_NOTICE, "%s: speeddial ignored: the active call is in state %s, which cannot be put on hold for a new call\n", d->id, sccp_channelstate2str(channel->state));
			return;
		}
	}

	AUTO_RELEASE(sccp_line_t, l, d->defaultLineInstance > 0 ? sccp_line_find_byid(d, d->defaultLineInstance) : sccp_dev_getActiveLine(d));
	if (!l) {
		l = sccp_line_find_byid(d, SCCP_FIRST_LINEINSTANCE) /*ref_replace*/;
	}
	if (l) {
		AUTO_RELEASE(sccp_channel_t, new_channel, sccp_channel_newcall(l, d, k->ext, SKINNY_CALLTYPE_OUTBOUND, NULL, NULL));
	}
}

static void handle_stimulus_speeddial(constDevicePtr d, constLinePtr l, const uint16_t instance, const uint32_t callId, const uint32_t stimulusstatus)
{
	sccp_log_and((DEBUGCAT_CORE + DEBUGCAT_ACTION)) (VERBOSE_PREFIX_3 "%s: speeddial button pressed\n", d->id);

	sccp_speed_t k;

	sccp_dev_speed_find_byindex(d, instance, FALSE, &k);
	if (k.valid) {
		handle_speeddial(d, &k);
		return;
	}
	pbx_log(LOG_NOTICE, "%s: speeddial button %d has no number configured; reject tone played\n", d->id, instance);
	sccp_dev_starttone(d, SKINNY_TONE_BEEPBONK, 0, 0, SKINNY_TONEDIRECTION_USER);
}

static void handle_stimulus_blfspeeddial(constDevicePtr d, constLinePtr l, const uint16_t instance, const uint32_t callId, const uint32_t stimulusstatus)
{
	sccp_log_and((DEBUGCAT_CORE + DEBUGCAT_ACTION)) (VERBOSE_PREFIX_3 "%s: BLF speeddial button pressed\n", d->id);

	sccp_speed_t k;

	sccp_dev_speed_find_byindex(d, instance, TRUE, &k);
	if (k.valid) {
		handle_speeddial(d, &k);
		return;
	}
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: BLF speeddial %d has no number\n", d->id, instance);
	sccp_dev_starttone(d, SKINNY_TONE_BEEPBONK, 0, 0, SKINNY_TONEDIRECTION_USER);
}

static void handle_stimulus_line(constDevicePtr d, constLinePtr l, const uint16_t instance, const uint32_t callId, const uint32_t stimulusstatus)
{
	sccp_log_and((DEBUGCAT_CORE + DEBUGCAT_ACTION)) (VERBOSE_PREFIX_3 "%s: line button pressed\n", d->id);

	if (d->isAnonymous) {
		sccp_feat_adhocDial(d, GLOB(hotline)->line);
		return;
	}

	/* for 7960's we use line keys to display hinted speeddials (Trick), without a hint it would have been a speeddial */
	if (!l) {
		sccp_speed_t k;
		sccp_dev_speed_find_byindex(d, instance, TRUE, &k);
		if (k.valid) {
			handle_speeddial(d, &k);
			return;
		}
		pbx_log(LOG_NOTICE, "%s: speeddial button %d has no number configured; reject tone played\n", d->id, instance);
		sccp_dev_starttone(d, SKINNY_TONE_BEEPBONK, 0, 0, SKINNY_TONEDIRECTION_USER);

		return;
	}

	/*
	 * Check speeddial before handling adHoc allows speeddials to be used and makes adHoc Non-Mandatory (This is a personal Preference - DdG).
	 */
	if (!sccp_strlen_zero(l->adhocNumber)) {
		sccp_feat_adhocDial(d, l);
		return;
	}

	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: line button pressed on line %s\n", d->id, l->name);
	{
		AUTO_RELEASE(sccp_channel_t, channel, (instance && callId) ? sccp_find_channel_by_lineInstance_and_callid(d, instance, callId) : sccp_device_getActiveChannel(d));
		if (channel) {
			AUTO_RELEASE(sccp_device_t, check_device , sccp_channel_getDevice(channel));
			if (check_device == d) {							// check to see if we own the channel (otherwise it would be a shared line owned by another device)
				if (SCCP_CHANNELSTATE_IsConnected(channel->state)) {
					if (!sccp_channel_hold(channel)) {
						pbx_log(LOG_WARNING, "%s: line button ignored: could not put active call %s on hold\n", d->id, channel->designator);
						return;
					}
				} else {
					sccp_channel_endcall(channel);
					sccp_dev_deactivate_cplane(d);
					if (l == channel->line) {
						sccp_log((DEBUGCAT_ACTION))(VERBOSE_PREFIX_3 "%s: line button pressed on the same line as unconnected call %s; call ended\n", d->id, channel->designator);
						return;
					}
				}
			} else {
				sccp_log((DEBUGCAT_ACTION))(VERBOSE_PREFIX_3 "%s: active call %s belongs to %s (shared line); handling the line button for this device only\n", d->id, channel->designator, check_device->id);
			}
		}
	}
	/* fall through to inactive & shared line handler */
	{
		sccp_log_and((DEBUGCAT_CORE + DEBUGCAT_ACTION)) (VERBOSE_PREFIX_3 "%s: line button pressed on an idle line\n", d->id);
		AUTO_RELEASE(sccp_channel_t, channel , NULL);
		AUTO_RELEASE(sccp_device_t, device , sccp_device_retain(d));

		if (!SCCP_LIST_GETSIZE(&l->channels)) {
			sccp_dev_setActiveLine(device, l);
			sccp_dev_set_cplane(device, instance, 1);
			channel = sccp_channel_newcall(l, device, NULL, SKINNY_CALLTYPE_OUTBOUND, NULL, NULL) /*ref_replace*/;
		} else if((channel = sccp_channel_find_bystate_on_line(l, SCCP_CHANNELSTATE_RINGING)) /*ref_replace*/) {
			sccp_channel_answer(device, channel);
			sccp_dev_set_cplane(device, instance, 1);
		} else if(l->statistic.numberOfHeldChannels >= 1 && (channel = sccp_channel_find_bystate_on_line(l, SCCP_CHANNELSTATE_HOLD)) /*ref_replace*/) {
			if (l->statistic.numberOfHeldChannels == 1) {
				sccp_dev_setActiveLine(device, l);
				sccp_channel_resume(device, channel, FALSE);
			} else {
				if (d->useHookFlash() && d->transfer && d->transferChannels.transferer == channel) {
					// 6901 is cancelling the transfer by pressing the line key (see cisco manual for 6901)
					AUTO_RELEASE(sccp_channel_t, resumeChannel, sccp_channel_retain(d->transferChannels.transferee));
					if (resumeChannel) {
						sccp_channel_endcall(d->transferChannels.transferer);
						sccp_channel_resume(d, resumeChannel, FALSE);
					}
				} else {
					sccp_dev_setActiveLine(device, l);
					sccp_device_sendcallstate(d, instance, channel->callid, SKINNY_CALLSTATE_HOLD, SKINNY_CALLPRIORITY_NORMAL, SKINNY_CALLINFO_VISIBILITY_DEFAULT);
				}
			}
			sccp_dev_set_cplane(device, instance, 1);
		} else if((channel = sccp_channel_find_bystate_on_line(l, SCCP_CHANNELSTATE_CONNECTED)) /*ref_replace*/) {
			sccp_device_sendcallstate(d, instance, channel->callid, SKINNY_CALLSTATE_CONNECTED, SKINNY_CALLPRIORITY_LOW, SKINNY_CALLINFO_VISIBILITY_HIDDEN);
			if (d->currentLine == NULL) {
				sccp_dev_setActiveLine(device, l);
				sccp_device_sendcallstate(d, instance, channel->callid, SKINNY_CALLSTATE_CONNECTED, SKINNY_CALLPRIORITY_NORMAL, SKINNY_CALLINFO_VISIBILITY_DEFAULT);
				sccp_softkey_setSoftkeyState((sccp_device_t *)d, KEYMODE_ONHOOKSTEALABLE, SKINNY_LBL_BARGE, TRUE);
				sccp_dev_set_keyset(device, instance, channel->callid, KEYMODE_ONHOOKSTEALABLE);
			} else {
				sccp_dev_setActiveLine(device, NULL);
				sccp_device_sendcallstate(d, instance, channel->callid, SKINNY_CALLSTATE_CONNECTED, SKINNY_CALLPRIORITY_LOW, SKINNY_CALLINFO_VISIBILITY_DEFAULT);
			}
		} else {
			sccp_log((DEBUGCAT_ACTION)) (VERBOSE_PREFIX_3 "%s: line button pressed with no call to act on; switching to line %d\n", device->id, instance);
			sccp_dev_setActiveLine(device, l);
			sccp_dev_set_cplane(device, instance, 1);
		}
	}
}

static void handle_stimulus_hold(constDevicePtr d, constLinePtr l, const uint16_t instance, const uint32_t callId, const uint32_t stimulusstatus)
{
	sccp_log_and((DEBUGCAT_CORE + DEBUGCAT_ACTION)) (VERBOSE_PREFIX_3 "%s: hold/resume button pressed on line %d\n", d->id, instance);

	AUTO_RELEASE(sccp_channel_t, channel1 , NULL);

	if((channel1 = sccp_channel_find_bystate_on_line(l, SCCP_CHANNELSTATE_CONNECTED)) && channel1 /*ref_replace*/) {
		sccp_channel_hold(channel1);
		return;
	}
	if((channel1 = sccp_channel_find_bystate_on_line(l, SCCP_CHANNELSTATE_HOLD)) && channel1 /*ref_replace*/) {
		AUTO_RELEASE(sccp_channel_t, channel2 , sccp_device_getActiveChannel(d));
		if (channel2 && channel2->state == SCCP_CHANNELSTATE_OFFHOOK) {
			if (channel2->calltype == SKINNY_CALLTYPE_OUTBOUND) {
				sccp_channel_endcall(channel2);
			} else {
				return;										/* new since 2014-6-5: Prevent accidental resume when we have in inbound call we are trying to answer */
			}
		}
		sccp_channel_resume(d, channel1, TRUE);
		return;
	}
	pbx_log(LOG_NOTICE, "%s: hold/resume pressed on line instance %d, which has no call to hold or resume; reject tone played\n", d->id, instance);
	sccp_dev_starttone(d, SKINNY_TONE_BEEPBONK, 0, 0, SKINNY_TONEDIRECTION_USER);
}

static void handle_stimulus_transfer(constDevicePtr d, constLinePtr l, const uint16_t instance, const uint32_t callId, const uint32_t stimulusstatus)
{
	sccp_log_and((DEBUGCAT_CORE + DEBUGCAT_ACTION)) (VERBOSE_PREFIX_3 "%s: transfer button pressed\n", d->id);
	if (!d->transfer) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: transfer ignored: transfer is off for this device\n", d->id);
		return;
	}
	AUTO_RELEASE(sccp_channel_t, channel , sccp_device_getActiveChannel(d));

	if (channel) {
		sccp_channel_transfer(channel, d);
		return;
	}
	pbx_log(LOG_NOTICE, "%s: transfer pressed on line instance %d with no active call; reject tone played\n", d->id, instance);
	sccp_dev_starttone(d, SKINNY_TONE_BEEPBONK, 0, 0, SKINNY_TONEDIRECTION_USER);
}

static void handle_stimulus_voicemail(constDevicePtr d, constLinePtr l, const uint16_t instance, const uint32_t callId, const uint32_t stimulusstatus)
{
	sccp_log_and((DEBUGCAT_CORE + DEBUGCAT_ACTION)) (VERBOSE_PREFIX_3 "%s: voicemail button pressed\n", d->id);
	sccp_feat_voicemail(d, instance);
}

static void handle_stimulus_conference(constDevicePtr d, constLinePtr l, const uint16_t instance, const uint32_t callId, const uint32_t stimulusstatus)
{
	sccp_log_and((DEBUGCAT_CORE + DEBUGCAT_ACTION)) (VERBOSE_PREFIX_3 "%s: conference button pressed\n", d->id);
	AUTO_RELEASE(sccp_channel_t, channel , sccp_device_getActiveChannel(d));

	if (channel) {
		sccp_feat_handle_conference(d, l, instance, channel);
		return;
	}
	pbx_log(LOG_NOTICE, "%s: conference pressed on line instance %d with no active call; reject tone played\n", d->id, instance);
	sccp_dev_starttone(d, SKINNY_TONE_BEEPBONK, 0, 0, SKINNY_TONEDIRECTION_USER);
}

static void handle_stimulus_forwardAll(constDevicePtr d, constLinePtr l, const uint16_t instance, const uint32_t callId, const uint32_t stimulusstatus)
{
	sccp_log_and((DEBUGCAT_CORE + DEBUGCAT_ACTION)) (VERBOSE_PREFIX_3 "%s: call forward all button pressed\n", d->id);
	AUTO_RELEASE(sccp_channel_t, maybe_c , sccp_device_getActiveChannel(d));
	if (d->cfwdall) {
		sccp_feat_handle_callforward(l, d, SCCP_CFWD_ALL, maybe_c, instance);
		return;
	}
	pbx_log(LOG_NOTICE, "%s: call forward all pressed, but cfwdall is off for this device; reject tone played\n", d->id);
	sccp_dev_displayprompt(d, 0, 0, SKINNY_DISP_CFWDALL " " SKINNY_DISP_SERVICE_IS_NOT_ACTIVE, SCCP_DISPLAYSTATUS_TIMEOUT);
	sccp_dev_starttone(d, SKINNY_TONE_BEEPBONK, 0, 0, SKINNY_TONEDIRECTION_USER);
}

static void handle_stimulus_forwardBusy(constDevicePtr d, constLinePtr l, const uint16_t instance, const uint32_t callId, const uint32_t stimulusstatus)
{
	sccp_log_and((DEBUGCAT_CORE + DEBUGCAT_ACTION)) (VERBOSE_PREFIX_3 "%s: call forward busy button pressed\n", d->id);
	AUTO_RELEASE(sccp_channel_t, maybe_c , sccp_device_getActiveChannel(d));
	if (d->cfwdbusy) {
		sccp_feat_handle_callforward(l, d, SCCP_CFWD_BUSY, maybe_c, instance);
		return;
	}
	pbx_log(LOG_NOTICE, "%s: call forward busy pressed, but cfwdbusy is off for this device; reject tone played\n", d->id);
	sccp_dev_displayprompt(d, 0, 0, SKINNY_DISP_CFWDBUSY " " SKINNY_DISP_SERVICE_IS_NOT_ACTIVE, SCCP_DISPLAYSTATUS_TIMEOUT);
	sccp_dev_starttone(d, SKINNY_TONE_BEEPBONK, 0, 0, SKINNY_TONEDIRECTION_USER);
}

static void handle_stimulus_forwardNoAnswer(constDevicePtr d, constLinePtr l, const uint16_t instance, const uint32_t callId, const uint32_t stimulusstatus)
{
	sccp_log_and((DEBUGCAT_CORE + DEBUGCAT_ACTION)) (VERBOSE_PREFIX_3 "%s: call forward no answer button pressed\n", d->id);
	AUTO_RELEASE(sccp_channel_t, maybe_c , sccp_device_getActiveChannel(d));
	if (d->cfwdnoanswer) {
		sccp_feat_handle_callforward(l, d, SCCP_CFWD_NOANSWER, maybe_c, instance);
		return;
	}
	pbx_log(LOG_NOTICE, "%s: call forward no answer pressed, but cfwdnoanswer is off for this device; reject tone played\n", d->id);
	sccp_dev_displayprompt(d, 0, 0, SKINNY_DISP_CFWDNOANSWER " " SKINNY_DISP_SERVICE_IS_NOT_ACTIVE, SCCP_DISPLAYSTATUS_TIMEOUT);
	sccp_dev_starttone(d, SKINNY_TONE_BEEPBONK, 0, 0, SKINNY_TONEDIRECTION_USER);
}

static void handle_stimulus_callpark(constDevicePtr d, constLinePtr l, const uint16_t instance, const uint32_t callId, const uint32_t stimulusstatus)
{
	sccp_log_and((DEBUGCAT_CORE + DEBUGCAT_ACTION)) (VERBOSE_PREFIX_3 "%s: park button pressed\n", d->id);
#ifdef CS_SCCP_PARK
	AUTO_RELEASE(sccp_channel_t, channel , sccp_device_getActiveChannel(d));

	if (channel) {
		sccp_channel_park(channel);
		return;
	}
	pbx_log(LOG_NOTICE, "%s: park pressed with no active call; ignored\n", d->id);
#else
	sccp_log((DEBUGCAT_BUTTONTEMPLATE + DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: park button ignored: chan_sccp was built without park support\n");
#endif
	sccp_dev_starttone(d, SKINNY_TONE_BEEPBONK, 0, 0, SKINNY_TONEDIRECTION_USER);
}

static void handle_stimulus_groupcallpickup(constDevicePtr d, constLinePtr l, const uint16_t instance, const uint32_t callId, const uint32_t stimulusstatus)
{
	sccp_log_and((DEBUGCAT_CORE + DEBUGCAT_ACTION)) (VERBOSE_PREFIX_3 "%s: group pickup button pressed\n", d->id);
#ifdef CS_SCCP_PICKUP
	AUTO_RELEASE(sccp_channel_t, maybe_c , sccp_find_channel_by_lineInstance_and_callid(d, instance, callId));
	AUTO_RELEASE(sccp_channel_t, channel , sccp_channel_getEmptyChannel(l, d, maybe_c, SKINNY_CALLTYPE_OUTBOUND, NULL, NULL));
	if (channel) {
		channel->softswitch_action = SCCP_SOFTSWITCH_DIAL;
		channel->ss_data = 0;
		iPbx.getPickupExtension(channel, channel->dialedNumber);
		sccp_indicate(d, channel, SCCP_CHANNELSTATE_SPEEDDIAL);
		iPbx.set_callstate(channel, AST_STATE_OFFHOOK);
		sccp_pbx_softswitch(channel);
	}

#else
	sccp_log((DEBUGCAT_FEATURE + DEBUGCAT_LINE)) (VERBOSE_PREFIX_3 "SCCP: group pickup button ignored: chan_sccp was built without pickup support\n");
#endif
}

static void handle_feature_action(constDevicePtr d, const int instance, const boolean_t toggleState)
{
	sccp_buttonconfig_t *config = NULL;
	sccp_cfwd_t status = SCCP_CFWD_NONE;

	if (!d) {
		return;
	}

	sccp_log((DEBUGCAT_FEATURE_BUTTON + DEBUGCAT_FEATURE)) (VERBOSE_PREFIX_3 "%s: instance %d, toggle %s\n", d->id, instance, (toggleState) ? "yes" : "no");

	SCCP_LIST_TRAVERSE(&d->buttonconfig, config, list) {
		if (config->instance == instance && config->type == FEATURE) {
			break;
		}
	}

	if (!config || !config->type || config->type != FEATURE) {
		pbx_log(LOG_WARNING, "%s: phone reported a press on button instance %d, which is not a feature button; ignored\n", d->id, instance);
		return;
	}

	char featureOption[255] = "";

	if (config->button.feature.options && !sccp_strlen_zero(config->button.feature.options)) {
		sccp_copy_string(featureOption, config->button.feature.options, sizeof(featureOption));
	}

	switch (config->button.feature.id) {
		case SCCP_FEATURE_PRIVACY:
			{
				uint32_t res = 0;
				if (!d->privacyFeature.enabled) {
					break;
				}

				if (sccp_strcaseequals(config->button.feature.options, "callpresent")) {
					res = d->privacyFeature.status & SCCP_PRIVACYFEATURE_CALLPRESENT;
					sccp_featureConfiguration_t *privacyFeature = (sccp_featureConfiguration_t * const)&d->privacyFeature;

					if (res) {
						privacyFeature->status &= ~SCCP_PRIVACYFEATURE_CALLPRESENT;
						config->button.feature.status = 0;
					} else {
						privacyFeature->status |= SCCP_PRIVACYFEATURE_CALLPRESENT;
						config->button.feature.status = 1;
					}
				} else {
					pbx_log(LOG_WARNING, "%s: privacy button option '%s' is not supported (only 'callpresent'); button press ignored\n", d->id, config->button.feature.options ? config->button.feature.options : "");
				}
			}
			break;
		case SCCP_FEATURE_CFWDALL:
			status = SCCP_CFWD_NONE;
			if (TRUE == toggleState) {
				config->button.feature.status = (config->button.feature.status == 0) ? 1 : 0;
			}
			if (!sccp_strlen_zero(config->button.feature.options)) {
				if (config->button.feature.status) {
					status = SCCP_CFWD_ALL;
				}
			}

			SCCP_LIST_TRAVERSE(&d->buttonconfig, config, list) {
				if (config->type == LINE) {
					AUTO_RELEASE(sccp_line_t, line , sccp_line_find_byname(config->button.line.name, FALSE));

					if (line) {
						sccp_line_cfwd(line, d, status, featureOption);
					}
				}
			}

			break;

		case SCCP_FEATURE_DND:
			if (TRUE == toggleState) {
				config->button.feature.status = (config->button.feature.status == 0) ? 1 : 0;
			}

			sccp_featureConfiguration_t *dndFeature = (sccp_featureConfiguration_t *const)&d->dndFeature;
			if (sccp_strcaseequals(config->button.feature.options, "silent")) {
				dndFeature->status = (config->button.feature.status) ? SCCP_DNDMODE_SILENT : SCCP_DNDMODE_OFF;
			} else if (sccp_strcaseequals(config->button.feature.options, "busy")) {
				dndFeature->status = (config->button.feature.status) ? SCCP_DNDMODE_REJECT : SCCP_DNDMODE_OFF;
			} else {
				switch (dndFeature->status) {
					case SCCP_DNDMODE_OFF:
						dndFeature->status = SCCP_DNDMODE_REJECT;
						break;
					case SCCP_DNDMODE_REJECT:
						dndFeature->status = SCCP_DNDMODE_SILENT;
						break;
					case SCCP_DNDMODE_SILENT:
						/* fall through */
					default:
						dndFeature->status = SCCP_DNDMODE_OFF;
						break;
				}
 			}
			sccp_dev_check_displayprompt(d);
			break;
#ifdef CS_SCCP_FEATURE_MONITOR
		case SCCP_FEATURE_MONITOR:
			if (TRUE == toggleState) {
				AUTO_RELEASE(sccp_channel_t, maybe_channel , sccp_device_getActiveChannel(d));
				sccp_feat_monitor(d, NULL, 0, maybe_channel);
			}

			break;
#endif

#ifdef CS_DEVSTATE_FEATURE

		case SCCP_FEATURE_DEVSTATE:
			sccp_log((DEBUGCAT_CORE + DEBUGCAT_FEATURE_BUTTON))(VERBOSE_PREFIX_3 "%s: devstate feature %s is now %s\n", DEV_ID_LOG(d),
									    config->button.feature.options ? config->button.feature.options : "", config->button.feature.status ? "On" : "Off");
			if (TRUE == toggleState) {
				if (sccp_strlen_zero(config->button.feature.options)) {
					pbx_log(LOG_WARNING, "%s: devstate feature button %d has no custom device state name configured; button press ignored\n", DEV_ID_LOG(d), config->instance);
					return;
				}
				enum ast_device_state newDeviceState = sccp_devstate_getNextDeviceState(d, config);
				pbx_devstate_changed(newDeviceState, "Custom:%s", config->button.feature.options);
				return;
			}
			break;
#endif
		case SCCP_FEATURE_PARKINGLOT:
#ifdef CS_SCCP_PARK
			if (TRUE == toggleState && iParkingLot.handleButtonPress) {
				iParkingLot.handleButtonPress(d, config);
			}
#endif
			break;
		case SCCP_FEATURE_MULTIBLINK:
			{
				uint32_t rithm = 0;
				uint32_t color = 0;
				uint32_t icon = 0;
				rithm = (d->priFeature.status & 0xf) - 1;
				color = ((d->priFeature.status & 0xf00) >> 8) - 1;
				icon = ((d->priFeature.status & 0xf0000) >> 16) - 1;

				if (2 == color && 6 == rithm) {
					icon = (icon + 1) % 3;
				}
				if (6 == rithm) {
					color = (color + 1) % 3;
				}
				rithm = (rithm + 1) % 7;

				sccp_featureConfiguration_t *priFeature = (sccp_featureConfiguration_t *const)&d->priFeature;

				priFeature->status = ((icon + 1) << 16) | ((color + 1) << 8) | (rithm + 1);
				sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: priority feature status: icon %d, color %d, rhythm %d, total %d\n", d->id, icon, color, rithm, priFeature->status);
			}
			break;

		default:
			pbx_log(LOG_WARNING, "%s: feature button %d has feature id %d, which has no handler; button press ignored\n", d->id, config->instance, config->button.feature.id);
			break;
	}

	if (config) {
		sccp_feat_changed(d, NULL, config->button.feature.id);
	}
}

static void handle_stimulus_feature(constDevicePtr d, constLinePtr l, const uint16_t instance, const uint32_t callId, const uint32_t stimulusstatus)
{
	sccp_log_and((DEBUGCAT_CORE + DEBUGCAT_ACTION)) (VERBOSE_PREFIX_3 "%s: feature button pressed (status %d)\n", d->id, stimulusstatus);
	handle_feature_action(d, instance, TRUE);
}

static const struct _skinny_stimulusMap_cb {
	void (*const handler_cb) (constDevicePtr d, constLinePtr l, const uint16_t instance, const uint32_t callId, const uint32_t stimulusstatus);
	boolean_t lineRequired;
} skinny_stimulusMap_cb[] = {
	/* clang-format off */
	[SKINNY_STIMULUS_UNUSED] 			= {NULL, TRUE},
	[SKINNY_STIMULUS_LASTNUMBERREDIAL] 		= {handle_stimulus_lastnumberredial, TRUE},
	[SKINNY_STIMULUS_SPEEDDIAL] 			= {handle_stimulus_speeddial, FALSE},
	[SKINNY_STIMULUS_BLFSPEEDDIAL] 			= {handle_stimulus_blfspeeddial, FALSE},
	[SKINNY_STIMULUS_LINE] 				= {handle_stimulus_line, FALSE},
	[SKINNY_STIMULUS_HOLD] 				= {handle_stimulus_hold, TRUE},
	[SKINNY_STIMULUS_TRANSFER] 			= {handle_stimulus_transfer, TRUE},
	[SKINNY_STIMULUS_VOICEMAIL] 			= {handle_stimulus_voicemail, TRUE},
	[SKINNY_STIMULUS_CONFERENCE] 			= {handle_stimulus_conference, TRUE},
	[SKINNY_STIMULUS_FORWARDALL] 			= {handle_stimulus_forwardAll, TRUE},
	[SKINNY_STIMULUS_FORWARDBUSY] 			= {handle_stimulus_forwardBusy, TRUE},
	[SKINNY_STIMULUS_FORWARDNOANSWER] 		= {handle_stimulus_forwardNoAnswer, TRUE},
	[SKINNY_STIMULUS_CALLPARK] 			= {handle_stimulus_callpark, TRUE},
	[SKINNY_STIMULUS_GROUPCALLPICKUP] 		= {handle_stimulus_groupcallpickup, TRUE},
	[SKINNY_STIMULUS_FEATURE] 			= {handle_stimulus_feature, FALSE},
	[SKINNY_STIMULUS_MOBILITY] 			= {handle_stimulus_feature, FALSE},
	[SKINNY_STIMULUS_MULTIBLINKFEATURE] 		= {handle_stimulus_feature, FALSE},
	[SKINNY_STIMULUS_DO_NOT_DISTURB] 		= {handle_stimulus_feature, FALSE},
	[SKINNY_STIMULUS_QUALITY_REPORT_TOOL]		= {handle_stimulus_feature, FALSE},
	[SKINNY_STIMULUS_CALLBACK] 			= {handle_stimulus_feature, FALSE},
	[SKINNY_STIMULUS_OTHER_PICKUP] 			= {handle_stimulus_feature, FALSE},
	[SKINNY_STIMULUS_VIDEO_MODE] 			= {handle_stimulus_feature, FALSE},
	[SKINNY_STIMULUS_NEW_CALL] 			= {handle_stimulus_feature, FALSE},
	[SKINNY_STIMULUS_END_CALL] 			= {handle_stimulus_feature, FALSE},
	[SKINNY_STIMULUS_HUNT_GROUP_LOG_IN_OUT] 	= {handle_stimulus_feature, FALSE},
	[SKINNY_STIMULUS_PARKINGLOT] 			= {handle_stimulus_feature, FALSE},
	[SKINNY_STIMULUS_TESTF] 			= {handle_stimulus_feature, FALSE},
	[SKINNY_STIMULUS_TESTI] 			= {handle_stimulus_feature, FALSE},
	[SKINNY_STIMULUS_MESSAGES] 			= {handle_stimulus_feature, FALSE},
	[SKINNY_STIMULUS_DIRECTORY] 			= {handle_stimulus_feature, FALSE},
	[SKINNY_STIMULUS_APPLICATION] 			= {handle_stimulus_feature, FALSE},
	[SKINNY_STIMULUS_DISPLAY] 			= {NULL, FALSE},
	[SKINNY_STIMULUS_T120CHAT] 			= {NULL, FALSE},
	[SKINNY_STIMULUS_T120WHITEBOARD] 		= {NULL, FALSE},
	[SKINNY_STIMULUS_T120APPLICATIONSHARING]	= {NULL, FALSE},
	[SKINNY_STIMULUS_T120FILETRANSFER] 		= {NULL, FALSE},
	[SKINNY_STIMULUS_VIDEO] 			= {handle_stimulus_feature, FALSE},
	[SKINNY_STIMULUS_ANSWERRELEASE] 		= {NULL, FALSE},
	[SKINNY_STIMULUS_AUTOANSWER] 			= {NULL, FALSE},
	[SKINNY_STIMULUS_SELECT] 			= {NULL, FALSE},
	[SKINNY_STIMULUS_SERVICEURL] 			= {NULL, FALSE},
	[SKINNY_STIMULUS_MALICIOUSCALL] 		= {NULL, FALSE},
	[SKINNY_STIMULUS_GENERICAPPB1] 			= {NULL, FALSE},
	[SKINNY_STIMULUS_GENERICAPPB2] 			= {NULL, FALSE},
	[SKINNY_STIMULUS_GENERICAPPB3] 			= {NULL, FALSE},
	[SKINNY_STIMULUS_GENERICAPPB4] 			= {NULL, FALSE},
	[SKINNY_STIMULUS_GENERICAPPB5] 			= {NULL, FALSE},
	[SKINNY_STIMULUS_MEETMECONFERENCE] 		= {NULL, FALSE},
	[SKINNY_STIMULUS_CALLPICKUP] 			= {NULL, FALSE},
	[SKINNY_STIMULUS_CONF_LIST] 			= {NULL, FALSE},
	[SKINNY_STIMULUS_REMOVE_LAST_PARTICIPANT]	= {NULL, FALSE},
	[SKINNY_STIMULUS_QUEUING] 			= {NULL, FALSE},
	[SKINNY_STIMULUS_HEADSET] 			= {NULL, FALSE},
	[SKINNY_STIMULUS_KEYPAD] 			= {NULL, FALSE},
	[SKINNY_STIMULUS_AEC] 				= {NULL, FALSE},
	[SKINNY_STIMULUS_UNDEFINED] 			= {NULL, FALSE},
	/* clang-format on */
};

void handle_stimulus(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	AUTO_RELEASE(sccp_line_t, l , NULL);
	uint32_t callId = 0;
	uint32_t stimulusStatus = 0;

	skinny_stimulus_t stimulus = letohl(msg_in->data.StimulusMessage.lel_stimulus);
	uint8_t instance = letohl(msg_in->data.StimulusMessage.lel_stimulusInstance);

	if (msg_in->header.length > 12) {
		callId = letohl(msg_in->data.StimulusMessage.lel_callReference);
		stimulusStatus = letohl(msg_in->data.StimulusMessage.lel_stimulusStatus);
	}

	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: stimulus %s (%d), instance %d, call %d, status %d\n", d->id, skinny_stimulus2str(stimulus), stimulus, instance, callId, stimulusStatus);

	if(!instance && stimulus == SKINNY_STIMULUS_LASTNUMBERREDIAL && d->redialInformation.lineInstance > 0) {
		instance = d->redialInformation.lineInstance;
	}
	/* SPA phones always send instance=1 when the hard hold button is pressed, instead of the active lineinstance. */
	if (stimulus == SKINNY_STIMULUS_HOLD && sccp_session_getProtocol(s) == SPCP_PROTOCOL) {
		AUTO_RELEASE(sccp_channel_t, c, sccp_channel_find_byid(callId));
		if (c) {
			l = sccp_line_retain(c->line) /*ref_replace*/;
			for (instance = SCCP_FIRST_LINEINSTANCE; instance < d->lineButtons.size; instance++) {
				if (d->lineButtons.instance[instance] && d->lineButtons.instance[instance]->line == l) {
					break;
				}
			}
		}
	}
	if (!instance) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: stimulus for instance 0; using active line %d\n", d->id, instance);
		if((l = sccp_dev_getActiveLine(d)) /*ref_replace*/) {
			instance = sccp_device_find_index_for_line(d, l->name);
		} else {
			instance = (d->defaultLineInstance > 0) ? d->defaultLineInstance : SCCP_FIRST_LINEINSTANCE;
		}
	}
	if (!l) {
		l = sccp_line_find_byid(d, instance) /*ref_replace*/;
	}

	if (stimulus > SKINNY_STIMULUS_UNUSED && stimulus < SKINNY_STIMULUS_UNDEFINED && skinny_stimulusMap_cb[stimulus].handler_cb) {
		if (!skinny_stimulusMap_cb[stimulus].lineRequired || (skinny_stimulusMap_cb[stimulus].lineRequired && l)) {
			skinny_stimulusMap_cb[stimulus].handler_cb(d, l, instance, callId, stimulusStatus);
		} else {
			pbx_log(LOG_WARNING, "%s: %s (stimulus %d) needs a line, but button instance %d has none; ignored\n", d->id, skinny_stimulus2str(stimulus), stimulus, instance);
			return;
		}
	} else {
		pbx_log(LOG_NOTICE, "%s: %s (stimulus %d) is not implemented; ignored\n", d->id, skinny_stimulus2str(stimulus), stimulus);
	}
}

void handle_offhook(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	if (d->isAnonymous) {
		sccp_feat_adhocDial(d, GLOB(hotline)->line);
		return;
	}

	AUTO_RELEASE(sccp_channel_t, active_channel, sccp_device_getActiveChannel(d));
	if(active_channel) {
		sccp_log((DEBUGCAT_CORE))(VERBOSE_PREFIX_3 "%s: off hook ignored: call %d is in progress\n", d->id, active_channel->callid);
		return;
	}

	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: off hook\n", d->id);
	sccp_device_setDeviceState(d, SCCP_DEVICESTATE_OFFHOOK);

	if (!d->configurationStatistic.numberOfLines) {
		pbx_log(LOG_NOTICE, "%s: phone went off-hook, but it has no lines registered; reject tone played\n", sccp_session_getDesignator(s));
		sccp_dev_displayprompt(d, 0, 0, SKINNY_DISP_NO_LINES_REGISTERED, SCCP_DISPLAYSTATUS_TIMEOUT);
		sccp_dev_starttone(d, SKINNY_TONE_BEEPBONK, 0, 0, SKINNY_TONEDIRECTION_USER);
		return;
	}

	AUTO_RELEASE(sccp_channel_t, ringing_channel, sccp_channel_find_bystate_on_device(d, SCCP_CHANNELSTATE_RINGING));
	if(ringing_channel) {
		sccp_channel_answer(d, ringing_channel);
	} else {
		AUTO_RELEASE(sccp_line_t, l, d->defaultLineInstance > 0 ? sccp_line_find_byid(d, d->defaultLineInstance) : sccp_dev_getActiveLine(d));
		if (!l) {
			l = sccp_line_find_byid(d, SCCP_FIRST_LINEINSTANCE) /*ref_replace*/;
		}
		if (l) {
			AUTO_RELEASE(sccp_channel_t, new_channel,
				     sccp_channel_newcall(l, d, (!sccp_strlen_zero(l->adhocNumber) ? l->adhocNumber : NULL), SKINNY_CALLTYPE_OUTBOUND, NULL, NULL));
		}
	}
}

/*
 * Handle On Hook Event for Session
 * protocolversion < 15 phones send buttonIndex instead of lineInstance protocolversion >= 15 phones don't send lineInstance nor callif on onhook (more like device state)
 */
void handle_onhook(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	pbx_assert(d != NULL);
	uint32_t buttonIndex = letohl(msg_in->data.OnHookMessage.lel_buttonIndex);
	uint32_t callid = letohl(msg_in->data.OnHookMessage.lel_callReference);

	if (!(d->lineButtons.size > SCCP_FIRST_LINEINSTANCE)) {
		pbx_log(LOG_NOTICE, "%s: phone went on-hook, but it has no lines registered; reject tone played\n", DEV_ID_LOG(d));
		sccp_dev_displayprompt(d, 0, 0, SKINNY_DISP_NO_LINES_REGISTERED, SCCP_DISPLAYSTATUS_TIMEOUT);
		sccp_dev_starttone(d, SKINNY_TONE_BEEPBONK, 0, 0, SKINNY_TONEDIRECTION_USER);
		return;
	}

	sccp_device_setDeviceState(d, SCCP_DEVICESTATE_ONHOOK);
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: on hook (button %d, call %d)\n", DEV_ID_LOG(d), buttonIndex, callid);

	AUTO_RELEASE(sccp_channel_t, channel, buttonIndex && callid ? sccp_find_channel_by_buttonIndex_and_callid(d, buttonIndex, callid) : sccp_device_getActiveChannel(d));
	if (channel) {
		if (!GLOB(transfer_on_hangup) || !sccp_channel_transfer_on_hangup(channel)) {
			sccp_channel_endcall(channel);
		}
	} else {
		sccp_dev_set_speaker(d, SKINNY_STATIONSPEAKER_OFF);
		sccp_dev_stoptone(d, 0, 0);
	}
}

void handle_hookflash(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	pbx_assert(d != NULL);
	uint32_t lineInstance = letohl(msg_in->data.HookFlashMessage.lel_lineInstance);
	uint32_t callid = letohl(msg_in->data.HookFlashMessage.lel_callReference);

	if (lineInstance && callid) {
		AUTO_RELEASE(sccp_line_t, l , sccp_line_find_byid(d, lineInstance));
		if (l) {
			handle_stimulus_transfer(d, l, lineInstance, callid, 0);
		} else {
			pbx_log(LOG_WARNING, "%s: hook flash on line instance %d, which has no line; ignored\n", d->id, lineInstance);
		}
	} else {
		pbx_log(LOG_WARNING, "%s: hook flash without a line instance or call reference (line %d, call %d); ignored\n", d->id, lineInstance, callid);
		sccp_dump_msg(msg_in);
	}
}

void handle_headset(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	sccp_accessorystate_t headsetmode = letohl(msg_in->data.HeadsetStatusMessage.lel_hsMode);
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: accessory %s is %s (%u)\n", sccp_session_getDesignator(s), sccp_accessory2str(SCCP_ACCESSORY_HEADSET), sccp_accessorystate2str(headsetmode), 0);
}

void handle_capabilities_res(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	pbx_assert(d != NULL);
	uint8_t numAudioCodecs = 0;
#ifdef CS_SCCP_VIDEO
	uint8_t numVideoCodecs = 0;
#endif
	skinny_codec_t codec = 0;

	uint8_t n = letohl(msg_in->data.CapabilitiesResMessage.lel_count);

	sccp_log((DEBUGCAT_CORE + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: phone reports %d capabilities\n", DEV_ID_LOG(d), n);
	for(uint i = 0; i < n; i++) {
		codec = letohl(msg_in->data.CapabilitiesResMessage.caps[i].lel_payloadCapability);
		if (codec2type(codec) == SKINNY_CODEC_TYPE_AUDIO) {
			d->capabilities.audio[numAudioCodecs++] = codec;
		} else
#ifdef CS_SCCP_VIDEO
		if (codec2type(codec) == SKINNY_CODEC_TYPE_VIDEO) {
			d->capabilities.video[numVideoCodecs++] = codec;
		} else
#endif
		{
		}
	}

	if ((SKINNY_CODEC_NONE == d->preferences.audio[0])) {
		memcpy(&d->preferences.audio, &d->capabilities.audio, sizeof(d->preferences.audio));
	}

#ifdef CS_SCCP_VIDEO
#endif
	sccp_line_updateLineCapabilitiesByDevice(d);
}

void sccp_handle_soft_key_template_req(constSessionPtr s, devicePtr d, constMessagePtr none)
{
	sccp_msg_t *msg_out = NULL;

	d->softkeysupport = 1;

	int arrayLen = ARRAY_LEN(softkeysmap);
	int dummy_len = arrayLen * (sizeof(StationSoftKeyDefinition));
	int hdr_len = sizeof(msg_out->data.SoftKeyTemplateResMessage);

	msg_out = sccp_build_packet(SoftKeyTemplateResMessage, hdr_len + dummy_len);
	if (!msg_out) {
		return;
	}

	msg_out->data.SoftKeyTemplateResMessage.lel_softKeyOffset = 0;

	for(uint8_t i = 0; i < arrayLen; i++) {
		switch (softkeysmap[i]) {
			case SKINNY_LBL_EMPTY:
				break;
			case SKINNY_LBL_DIAL:
				/* fall through */
			case SKINNY_LBL_MONITOR:
				sccp_copy_string(msg_out->data.SoftKeyTemplateResMessage.definition[i].softKeyLabel, label2str(softkeysmap[i]), StationMaxSoftKeyLabelSize);
				break;
			case SKINNY_LBL_VIDEO_MODE:
				msg_out->data.SoftKeyTemplateResMessage.definition[i].softKeyLabel[0] = (char)128;
				msg_out->data.SoftKeyTemplateResMessage.definition[i].softKeyLabel[1] = softkeysmap[i];	/* this works on 7970 */
				break;
#ifdef CS_SCCP_CONFERENCE
			case SKINNY_LBL_CONFRN:
				/* fall through */
			case SKINNY_LBL_JOIN:
				/* fall through */
			case SKINNY_LBL_CONFLIST:
				if (!d->allow_conference) {
					break;
				}
#endif
				/* fall through */
			default:
				msg_out->data.SoftKeyTemplateResMessage.definition[i].softKeyLabel[0] = (char)128;
				msg_out->data.SoftKeyTemplateResMessage.definition[i].softKeyLabel[1] = softkeysmap[i];
		}
		msg_out->data.SoftKeyTemplateResMessage.definition[i].lel_softKeyEvent = htolel(i + 1);
	}

	msg_out->data.SoftKeyTemplateResMessage.lel_softKeyCount = htolel(arrayLen);
	msg_out->data.SoftKeyTemplateResMessage.lel_totalSoftKeyCount = htolel(arrayLen);
	sccp_dev_send(d, msg_out);
}

void handle_soft_key_set_req(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	int iKeySetCount = 0;
	sccp_msg_t *msg_out = NULL;
	uint8_t i = 0;
	uint8_t trnsfvm = 0;
	uint8_t meetme = 0;

#ifdef CS_SCCP_PICKUP
	uint8_t pickupgroup = 0;
	uint8_t directed_pickup = 0;
#endif

	sccp_softKeySetConfiguration_t * softkeyset = NULL;
	d->softkeyset = NULL;

	if (!sccp_strlen_zero(d->softkeyDefinition)) {
		sccp_log((DEBUGCAT_DEVICE + DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: looking up softkey set %s\n", d->id, d->softkeyDefinition);
		SCCP_LIST_LOCK(&softKeySetConfig);
		SCCP_LIST_TRAVERSE(&softKeySetConfig, softkeyset, list) {
			if (sccp_strcaseequals(d->softkeyDefinition, softkeyset->name)) {
				d->softkeyset = softkeyset;
				d->softKeyConfiguration.modes = softkeyset->modes;
				d->softKeyConfiguration.size = softkeyset->numberOfSoftKeySets;
			}
		}
		SCCP_LIST_UNLOCK(&softKeySetConfig);
	}

	if (!d->softkeyset) {
		pbx_log(LOG_WARNING, "%s: softkeyset=%s is not defined in sccp.conf; using the 'default' softkey set\n", d->id, d->softkeyDefinition);
		SCCP_LIST_LOCK(&softKeySetConfig);
		SCCP_LIST_TRAVERSE(&softKeySetConfig, softkeyset, list) {
			if (sccp_strcaseequals("default", softkeyset->name)) {
				d->softkeyset = softkeyset;
				d->softKeyConfiguration.modes = softkeyset->modes;
				d->softKeyConfiguration.size = softkeyset->numberOfSoftKeySets;
			}
		}
		SCCP_LIST_UNLOCK(&softKeySetConfig);
	}

	const softkey_modes *v = d->softKeyConfiguration.modes;
	const uint8_t v_count = d->softKeyConfiguration.size;
	const uint8_t * b = NULL;

	REQ(msg_out, SoftKeySetResMessage);
	if (!msg_out) {
		return;
	}
	msg_out->data.SoftKeySetResMessage.lel_softKeySetOffset = htolel(0);

	sccp_buttonconfig_t * buttonconfig = NULL;

	SCCP_LIST_TRAVERSE(&d->buttonconfig, buttonconfig, list) {
		if (buttonconfig->type == LINE) {
			AUTO_RELEASE(sccp_line_t, l , sccp_line_find_byname(buttonconfig->button.line.name, FALSE));

			if (l) {
				if (!sccp_strlen_zero(l->trnsfvm)) {
					trnsfvm = 1;
				}
				if (l->meetme) {
					meetme = 1;
				}
				if (!sccp_strlen_zero(l->meetmenum)) {
					meetme = 1;
				}
#ifdef CS_SCCP_PICKUP
				if (l->pickupgroup) {
					pickupgroup = 1;
				}
				if (l->directed_pickup) {
					directed_pickup = 1;
				}
#ifdef CS_AST_HAS_NAMEDGROUP
				if (!sccp_strlen_zero(l->namedpickupgroup)) {
					pickupgroup = 1;
				}
#endif
#endif
			}
		}
	}

#ifdef CS_SCCP_PARK
#endif
#ifdef CS_SCCP_PICKUP
#endif
	size_t buffersize = 20 + (15 * sizeof(softkeysmap));
	pbx_str_t *outputStr = pbx_str_create(buffersize);

	for (i = 0; i < v_count; i++) {
		b = v->ptr;
		uint8_t c = 0;

		uint8_t j = 0;

		uint8_t cp = 0;

		pbx_str_append(&outputStr, buffersize, "%-15s => |", skinny_keymode2str(v->id));

		for (c = 0, cp = 0; c < v->count; c++, cp++) {
			msg_out->data.SoftKeySetResMessage.definition[v->id].softKeyTemplateIndex[cp] = 0;
			if ((b[c] == SKINNY_LBL_PARK) && (!d->park)) {
				continue;
			}
			if ((b[c] == SKINNY_LBL_TRANSFER) && (!d->transfer)) {
				continue;
			}
			if ((b[c] == SKINNY_LBL_DND) && (!d->dndFeature.enabled)) {
				continue;
			}
			if ((b[c] == SKINNY_LBL_CFWDALL) && (!d->cfwdall)) {
				continue;
			}
			if ((b[c] == SKINNY_LBL_CFWDBUSY) && (!d->cfwdbusy)) {
				continue;
			}
			if ((b[c] == SKINNY_LBL_CFWDNOANSWER) && (!d->cfwdnoanswer)) {
				continue;
			}
			if ((b[c] == SKINNY_LBL_TRNSFVM) && (!trnsfvm)) {
				continue;
			}
			if ((b[c] == SKINNY_LBL_IDIVERT) && (!trnsfvm)) {
				continue;
			}
			if ((b[c] == SKINNY_LBL_MEETME) && (!meetme)) {
				continue;
			}
#ifndef CS_ADV_FEATURES
			if (b[c] == SKINNY_LBL_CALLBACK) {
				continue;
			}
			if (b[c] == SKINNY_LBL_CBARGE) {
				continue;
			}
#endif
#ifndef CS_SCCP_CONFERENCE
			if (b[c] == SKINNY_LBL_JOIN) {
				continue;
			}
			if (b[c] == SKINNY_LBL_CONFRN) {
				continue;
			}
#endif
#ifdef CS_SCCP_PICKUP
			if ((b[c] == SKINNY_LBL_PICKUP) && (!directed_pickup)) {
				continue;
			}
			if ((b[c] == SKINNY_LBL_GPICKUP) && (!pickupgroup)) {
				continue;
			}
#endif
			if ((b[c] == SKINNY_LBL_PRIVATE) && (!d->privacyFeature.enabled)) {
				continue;
			}
#ifndef CS_SCCP_VIDEO
			if (b[c] == SKINNY_LBL_VIDEO_MODE) {
				continue;
			}
#endif
			if(b[c] == SKINNY_LBL_EMPTY) {
				continue;
			}
			for (j = 0; j < sizeof(softkeysmap); j++) {
				if (b[c] == softkeysmap[j]) {
					ast_str_append(&outputStr, buffersize, "%-2d:%-9s|", c, label2str(softkeysmap[j]));
					msg_out->data.SoftKeySetResMessage.definition[v->id].softKeyTemplateIndex[cp] = (j + 1);
					msg_out->data.SoftKeySetResMessage.definition[v->id].les_softKeyInfoIndex[cp] = htoles(j + 301);
					break;
				}
			}
		}

		sccp_log((DEBUGCAT_DEVICE | DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: %s\n", d->id, ast_str_buffer(outputStr));
		ast_str_reset(outputStr);
		v++;
		iKeySetCount++;
	};
	sccp_free(outputStr);

	for (i = 0; i < KEYMODE_ONHOOKSTEALABLE; i++) {
		sccp_softkey_setSoftkeyState(d, (skinny_keymode_t) i, SKINNY_LBL_VIDEO_MODE, FALSE);
		sccp_softkey_setSoftkeyState(d, (skinny_keymode_t) i, SKINNY_LBL_JOIN, FALSE);
	}

	msg_out->data.SoftKeySetResMessage.lel_softKeySetCount = htolel(iKeySetCount);
	msg_out->data.SoftKeySetResMessage.lel_totalSoftKeySetCount = htolel(iKeySetCount);

	sccp_dev_send(d, msg_out);
	sccp_dev_set_keyset(d, 0, 0, KEYMODE_ONHOOK);
}

void handle_dialedphonebook_message(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	sccp_msg_t *msg_out = NULL;

	uint32_t transactionID = letohl(msg_in->data.SubscriptionStatReqMessage.lel_transactionID);
	uint32_t featureID = letohl(msg_in->data.SubscriptionStatReqMessage.lel_featureID);
	uint32_t timer = letohl(msg_in->data.SubscriptionStatReqMessage.lel_timer);
	char *subscriptionID = pbx_strdupa(msg_in->data.SubscriptionStatReqMessage.subscriptionID);

	REQ(msg_out, SubscriptionStatMessage);
	if (!msg_out) {
		return;
	}
	msg_out->data.SubscriptionStatMessage.lel_transactionID = htolel(transactionID);
	msg_out->data.SubscriptionStatMessage.lel_featureID = htolel(featureID);
	msg_out->data.SubscriptionStatMessage.lel_timer = htolel(timer);
	msg_out->data.SubscriptionStatMessage.lel_cause = 0;						/*!< Cause (Enum):
														OK: 0x00,
														RouteFail:0x01,
														AuthFail:0x02,
														Timeout:0x03,
														TrunkTerm:0x04,
														TrunkForbidden:0x05,
														Throttle:0x06
													*/
	sccp_dev_send(d, msg_out);

	if (sccp_strlen(subscriptionID) <= 1) {
		return;
	}

	AUTO_RELEASE(sccp_line_t, line , sccp_line_find_byid(d, featureID));

	if (line) {
		REQ(msg_out, NotificationMessage);
		if (!msg_out) {
			return;
		}
		skinny_busylampfield_state_t status = iPbx.getExtensionState(subscriptionID, line->context);

		msg_out->data.NotificationMessage.lel_transactionID = htolel(transactionID);
		msg_out->data.NotificationMessage.lel_featureID = htolel(featureID);
		if(status == SKINNY_BLF_STATUS_ALERTING) {
			msg_out->data.NotificationMessage.lel_status = htolel(SKINNY_BLF_STATUS_INUSE);
		} else {
			msg_out->data.NotificationMessage.lel_status = htolel(status);
		}
		sccp_dev_send(d, msg_out);
		sccp_log((DEBUGCAT_HINT + DEBUGCAT_ACTION))(VERBOSE_PREFIX_3 "%s: sending notification for %s@%s, state %s\n", DEV_ID_LOG(d), subscriptionID,
							    line->context ? line->context : "<not set>", skinny_busylampfield_state2str(status));

		// only used in debug logging below
	}
}

void sccp_handle_time_date_req(constSessionPtr s, devicePtr d, constMessagePtr none)
{
	pbx_assert(s != NULL);
	sccp_msg_t * msg_out = NULL;
	REQ(msg_out, DefineTimeDate);
	if (!msg_out) {
		return;
	}

	/* modulate the timezone by full hours only */
	time_t timer = time(0) + (d->tz_offset * 3600);
	struct timeval when = { timer, 0 };
	struct ast_tm tm;
	ast_localtime(&when, &tm, NULL);

	msg_out->data.DefineTimeDate.lel_year = htolel(tm.tm_year + 1900);
	msg_out->data.DefineTimeDate.lel_month = htolel(tm.tm_mon + 1);
	msg_out->data.DefineTimeDate.lel_dayOfWeek = htolel(tm.tm_wday);
	msg_out->data.DefineTimeDate.lel_day = htolel(tm.tm_mday);
	msg_out->data.DefineTimeDate.lel_hour = htolel(tm.tm_hour);
	msg_out->data.DefineTimeDate.lel_minute = htolel(tm.tm_min);
	msg_out->data.DefineTimeDate.lel_seconds = htolel(tm.tm_sec);
	msg_out->data.DefineTimeDate.lel_milliseconds = htolel(0);
	msg_out->data.DefineTimeDate.lel_systemTime = htolel(timer);
	sccp_dev_send(d, msg_out);
}

void handle_keypad_button(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	pbx_assert(d != NULL);
	char resp = '\0';
	int len = 0;

	enum sccp_cili {
		SCCP_CILI_HAS_NEITHER,
		SCCP_CILI_HAS_CALLID,
		SCCP_CILI_HAS_LINEINSTANCE,
	};

	int digit = letohl(msg_in->data.KeypadButtonMessage.lel_kpButton);
	switch (digit) {
		case 0 ... 9:
			resp = '0' + digit;
			break;
		case 14:
			resp = '*';
			break;
		case 15:
			resp = '#';
			break;
		case 16:
			resp = '+';
			break;
		default:
			pbx_log(LOG_WARNING, "%s: keypad sent code %d, which is not a dialable key; ignored\n", DEV_ID_LOG(d), digit);
			return;
	}

	uint8_t CallIdAndLineInstance = SCCP_CILI_HAS_NEITHER;
	uint8_t lineInstance = 0;
	uint32_t callid = 0;
	if (msg_in->header.length >= 16) {
		lineInstance = letohl(msg_in->data.KeypadButtonMessage.lel_lineInstance);
		CallIdAndLineInstance |= lineInstance ? SCCP_CILI_HAS_LINEINSTANCE : 0;
		if (msg_in->header.length >= 20) {
			callid = letohl(msg_in->data.KeypadButtonMessage.lel_callReference);
			CallIdAndLineInstance |= callid ? SCCP_CILI_HAS_CALLID : 0;
		}
	}

	/* old devices (like 7906) send buttonIndex instead of lineInstance, convert buttonIndex to lineInstance */
	if (d->protocolversion < 15 && (CallIdAndLineInstance & SCCP_CILI_HAS_LINEINSTANCE)) {
		int16_t tmpLineInstance = 0;

		int16_t buttonIndex = lineInstance;
		tmpLineInstance = sccp_device_buttonIndex2lineInstance(d, buttonIndex);
		if(tmpLineInstance >= 0) {
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: keypad digit %08x, call %d, button %d = line instance %d\n", DEV_ID_LOG(d), digit, callid, buttonIndex, tmpLineInstance);
			lineInstance = tmpLineInstance;
			CallIdAndLineInstance |= lineInstance ? SCCP_CILI_HAS_LINEINSTANCE : 0;
		}
	}
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: keypad digit %08x, call %d, line instance %d\n", DEV_ID_LOG(d), digit, callid, lineInstance);

	AUTO_RELEASE(sccp_channel_t, channel , NULL);
	AUTO_RELEASE(sccp_line_t, l , NULL);
	switch(CallIdAndLineInstance) {
		case SCCP_CILI_HAS_CALLID | SCCP_CILI_HAS_LINEINSTANCE:
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: keypad: phone sent call and line instance\n", DEV_ID_LOG(d));
			if((channel = sccp_find_channel_by_lineInstance_and_callid(d, lineInstance, callid)) /*ref_replace*/) {
				break;
			}
			// fallthrough to lineInstance only method (channel could not be found on lineInstance), reported in issue #340
			/* fall through */
		case SCCP_CILI_HAS_LINEINSTANCE:
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: keypad: phone sent line instance only\n", DEV_ID_LOG(d));
			if((l = sccp_line_find_byid(d, lineInstance)) /*ref_replace*/) {
				SCCP_LIST_LOCK(&l->channels);
				channel = SCCP_LIST_FIND(&l->channels, sccp_channel_t, tmpc, list,
							 (tmpc->state == SCCP_CHANNELSTATE_OFFHOOK || tmpc->state == SCCP_CHANNELSTATE_GETDIGITS || tmpc->state == SCCP_CHANNELSTATE_DIGITSFOLL), TRUE, __FILE__, __LINE__,
							 __PRETTY_FUNCTION__);
				SCCP_LIST_UNLOCK(&l->channels);
			}
			break;
		case SCCP_CILI_HAS_CALLID:
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: keypad: phone sent call only\n", DEV_ID_LOG(d));
			channel = sccp_channel_find_byid(callid) /*ref_replace*/;
			break;
		case SCCP_CILI_HAS_NEITHER:
			/* Old phones like 7912 never uses callid so we would have trouble finding the right channel */
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: keypad: phone sent neither call nor line instance; using the active line and call\n", DEV_ID_LOG(d));
			channel = sccp_device_getActiveChannel(d) /*ref_replace*/;
			break;
	}
	if (!l && channel && channel->line) {
		l = sccp_line_retain(channel->line) /*ref_replace*/;
	}

	{ /* check if we have all required structures and states for error conditions */
		if (!channel) {
			sccp_log((DEBUGCAT_ACTION))(VERBOSE_PREFIX_3 "%s: key pressed with no active call (often while a call is ending); ignored\n", DEV_ID_LOG(d));
			return;
		}
		if (!channel->owner) {
			pbx_log(LOG_ERROR, "%s: key pressed on call %s, which has no Asterisk channel; call ended\n", DEV_ID_LOG(d), channel->designator);
			sccp_channel_endcall(channel);
			return;
		}
		if (!l) {
			pbx_log(LOG_ERROR, "%s: key pressed on call %s, which has no line; ignored\n", DEV_ID_LOG(d), channel->designator);
			return;
		}
		if (channel->scheduler.hangup_id > -1) {
			sccp_log((DEBUGCAT_ACTION)) (VERBOSE_PREFIX_1 "%s: digit %d dropped: the call is about to hang up\n", DEV_ID_LOG(d), digit);
			return;
		}
		if (channel->state == SCCP_CHANNELSTATE_INVALIDNUMBER || channel->state == SCCP_CHANNELSTATE_CONGESTION || channel->state == SCCP_CHANNELSTATE_BUSY || channel->state == SCCP_CHANNELSTATE_ZOMBIE || channel->state == SCCP_CHANNELSTATE_DND) {
			sccp_log((DEBUGCAT_ACTION)) (VERBOSE_PREFIX_1 "%s: digit %d dropped: the call has ended\n", DEV_ID_LOG(d), digit);
			return;
		}
	}

	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: digit %08x (%d) on line %s, call %d, state %d, DTMF mode %s\n", DEV_ID_LOG(d), digit, digit, l->name, channel->callid, channel->state, sccp_dtmfmode2str(channel->dtmfmode));

	if (channel->state == SCCP_CHANNELSTATE_CONNECTED || channel->state == SCCP_CHANNELSTATE_CONNECTEDCONFERENCE || channel->state == SCCP_CHANNELSTATE_PROCEED || channel->state == SCCP_CHANNELSTATE_RINGOUT) {
		/* we have to unlock 'cause the senddigit lock the channel */
		if (channel->dtmfmode == SCCP_DTMFMODE_SKINNY && iPbx.send_digit) {
			sccp_log((DEBUGCAT_ACTION)) (VERBOSE_PREFIX_1 "%s: sending digit %c to %s as an Asterisk DTMF frame\n", DEV_ID_LOG(d), resp, l->name);
			iPbx.send_digit(channel, resp);
		} else {
			sccp_log((DEBUGCAT_ACTION)) (VERBOSE_PREFIX_1 "%s: phone sent digit %c to %s as RFC 2833\n", DEV_ID_LOG(d), resp, l->name);
		}
		return;
	}

	len = sccp_strlen(channel->dialedNumber);
	if (len + 1 >= (SCCP_MAX_EXTENSION)) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_2 "%s: digit dropped: the dialed number is at its maximum length\n", channel->designator);
		sccp_dev_displayprompt(d, lineInstance, channel->callid, SKINNY_DISP_NO_MORE_DIGITS, SCCP_DISPLAYSTATUS_TIMEOUT);
	} else if (((channel->state == SCCP_CHANNELSTATE_OFFHOOK) || (channel->state == SCCP_CHANNELSTATE_GETDIGITS) || (channel->state == SCCP_CHANNELSTATE_DIGITSFOLL)) && !iPbx.getChannelPbx(channel)) {
		int max_time_per_digit = SCCP_SIM_ENBLOC_MAX_PER_DIGIT;
		double variance = 0;
		double std_deviation = 0;
		int minimum_digit_before_check = SCCP_SIM_ENBLOC_MIN_DIGIT;
		int lpbx_digit_usecs = 0;
		int number_of_digits = len;
		int timeout_if_enbloc = SCCP_SIM_ENBLOC_TIMEOUT;

		sccp_log((DEBUGCAT_ACTION)) (VERBOSE_PREFIX_1 "SCCP: enbloc detection: digit timeout %d s, scheduler wait %d ms\n", channel->enbloc.digittimeout, iPbx.sched_wait(channel->scheduler.digittimeout_id));
		if (GLOB(simulate_enbloc) && !channel->enbloc.deactivate && number_of_digits >= 1) {
			if ((int)channel->enbloc.digittimeout < (iPbx.sched_wait(channel->scheduler.digittimeout_id))) {
				lpbx_digit_usecs = (channel->enbloc.digittimeout * 1000) - (iPbx.sched_wait(channel->scheduler.digittimeout_id));
			} else {
				sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_1 "SCCP: enbloc detection stopped: past the digit timeout\n");
				channel->enbloc.deactivate = 1;
			}
			channel->enbloc.totaldigittime += lpbx_digit_usecs;
			channel->enbloc.totaldigittimesquared += pow(lpbx_digit_usecs, 2);
			sccp_log((DEBUGCAT_ACTION)) (VERBOSE_PREFIX_1 "SCCP: enbloc detection: digit time %d ms, total dial time %d ms, %d digits\n", lpbx_digit_usecs, channel->enbloc.totaldigittime, number_of_digits);
			if (number_of_digits >= 2) {
				if (number_of_digits >= minimum_digit_before_check) {				// minimal number of digits before checking
					if (lpbx_digit_usecs < max_time_per_digit) {
						double mean = (double)channel->enbloc.totaldigittime / (double)number_of_digits;
						variance = ( (double) channel->enbloc.totaldigittimesquared - ((double)number_of_digits * pow(mean, 2)) ) / ((double)number_of_digits - 1);
						std_deviation = sqrt(variance);
						sccp_log((DEBUGCAT_ACTION)) (VERBOSE_PREFIX_1 "SCCP: enbloc detection: sqrt((%d - (%d * (%d/%d)^2)/(%d-1)) = %2.2f\n", channel->enbloc.totaldigittimesquared, number_of_digits, channel->enbloc.totaldigittime, number_of_digits, number_of_digits, std_deviation);
						sccp_log((DEBUGCAT_ACTION)) (VERBOSE_PREFIX_1 "SCCP: enbloc detection: time squared %d, total time %d, %d digits, standard deviation %2.2f, variance %2.2f\n", channel->enbloc.totaldigittimesquared, channel->enbloc.totaldigittime, number_of_digits, std_deviation, variance);

						if (fabs(lpbx_digit_usecs - mean) <= std_deviation) {
							if ((int)channel->enbloc.digittimeout > timeout_if_enbloc) {	// only display message and change timeout once
								sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_1 "SCCP: enbloc detection: fast dialing, digit timeout now 2 s\n");
								channel->enbloc.digittimeout = timeout_if_enbloc;
							}
						} else {
							sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_1 "SCCP: enbloc detection stopped: more than one standard deviation (%2.2f) from the mean (%2.2f)\n", std_deviation, mean);
							channel->enbloc.deactivate = 1;
						}
					} else {
						sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_1 "SCCP: enbloc detection stopped: time per digit %d over the maximum %d\n", lpbx_digit_usecs, max_time_per_digit);
						channel->enbloc.deactivate = 1;
					}
				}
			}
		}

		channel->dialedNumber[len++] = resp;
		channel->dialedNumber[len] = '\0';
		sccp_channel_schedule_digittimeout(channel, channel->enbloc.digittimeout);

		if (GLOB(digittimeoutchar) == resp) {
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: dial-now key '%c' pressed; dialing\n", GLOB(digittimeoutchar));
			channel->dialedNumber[len] = '\0';
			sccp_channel_stop_schedule_digittimout(channel);
			sccp_safe_sleep(100);									// we would hear last keypad stroke before starting all
			sccp_pbx_softswitch(channel);
		}
		if (sccp_pbx_helper(channel) == SCCP_EXTENSION_EXACTMATCH) {
			sccp_channel_stop_schedule_digittimout(channel);
			sccp_safe_sleep(100);									// we would hear last keypad stroke before starting all
			sccp_pbx_softswitch(channel);								// channel will be released by hangup
		}
		sccp_handle_dialtone(d, l, channel);
	} else if (iPbx.getChannelPbx(channel) || channel->state == SCCP_CHANNELSTATE_DIALING) {
		channel->dialedNumber[len++] = resp;
		channel->dialedNumber[len] = '\0';
		if (channel->dtmfmode == SCCP_DTMFMODE_SKINNY && iPbx.send_digit) {
			sccp_log((DEBUGCAT_ACTION)) (VERBOSE_PREFIX_1 "%s: sending digit %c to %s as an Asterisk DTMF frame (forced)\n", DEV_ID_LOG(d), resp, l->name);
			iPbx.send_digit(channel, resp);
		}
	} else {
		pbx_log(LOG_WARNING, "%s: key '%c' ignored: call %d on line %s is in state %s, which accepts no digits\n", DEV_ID_LOG(d), resp, channel->callid, l->name, sccp_channelstate2str(channel->state));
	}
}

void handle_soft_key_event(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	pbx_assert(d != NULL);

	uint32_t event = letohl(msg_in->data.SoftKeyEventMessage.lel_softKeyEvent);
	uint32_t lineInstance = letohl(msg_in->data.SoftKeyEventMessage.lel_lineInstance);
	uint32_t callid = letohl(msg_in->data.SoftKeyEventMessage.lel_callReference);

	if ((int)event - 1 < 0 || (int)event - 1 > (int)ARRAY_LEN(softkeysmap) - 1) {
		pbx_log(LOG_WARNING, "%s: softkey event %u is outside the known range 1-%ld; ignored\n", DEV_ID_LOG(d), event, (long)ARRAY_LEN(softkeysmap));
		return;
	}
	event = softkeysmap[event - 1];

	/* correct events for nokia icc client (Legacy Support -FS) */
	if(strcasecmp(d->config_type, "nokia-icc") == 0) {
		switch (event) {
			case SKINNY_LBL_DIRTRFR:
				event = SKINNY_LBL_ENDCALL;
				break;
		}
	}

	sccp_log((DEBUGCAT_MESSAGE + DEBUGCAT_ACTION + DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: softkey %s (%d), line %d, call %d\n", d->id, label2str(event), event, lineInstance, callid);

	AUTO_RELEASE(sccp_line_t, l, NULL);
	AUTO_RELEASE(sccp_channel_t, c, NULL);
	if (!lineInstance && !callid && (event == SKINNY_LBL_NEWCALL || event == SKINNY_LBL_REDIAL)) {
		if (d->defaultLineInstance > 0) {
			lineInstance = d->defaultLineInstance;
		} else {
			l = sccp_dev_getActiveLine(d) /*ref_replace*/;
		}
	}

	if (!l && lineInstance) {
		l = sccp_line_find_byid(d, lineInstance) /*ref_replace*/;
	}

	if (l && callid) {
		c = sccp_find_channel_by_lineInstance_and_callid(d, lineInstance, callid) /*ref_replace*/;
	}

#ifdef CS_EXPERIMENTAL
	if (lineInstance && callid) {
		AUTO_RELEASE(sccp_channel_t, check_channel, sccp_device_getActiveChannel(d));
		if (check_channel && check_channel->callid != callid && check_channel->state <= SCCP_CHANNELSTATE_OFFHOOK) {
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: call %s is not in progress; ending it\n", d->id, check_channel->designator);
			if (d->transfer && d->transferChannels.transferer == check_channel && event == SKINNY_LBL_ENDCALL) {
				// intervene with ENDCALL send by a line button press (7970)
				AUTO_RELEASE(sccp_channel_t, resumeChannel, sccp_channel_retain(d->transferChannels.transferee));
				if (resumeChannel) {
					sccp_channel_endcall(d->transferChannels.transferer);
					sccp_channel_resume(d, resumeChannel, FALSE);
				}
				return;
			} else {
				sccp_channel_endcall(check_channel);
				sccp_dev_deactivate_cplane(d);
			}
		}
		if (c) {
			sccp_dev_setActiveLine(d, c->line);
		}
		sccp_dev_set_cplane(d, lineInstance, 1);
	}
#endif

	if (!sccp_SoftkeyMap_execCallbackByEvent(d, l, lineInstance, c, event)) {
		char buf[100];

		/* skipping message if event is endcall, because they can coincide when both parties hangup around the same time */
		if (event != SKINNY_LBL_ENDCALL) {
			snprintf(buf, sizeof(buf), SKINNY_DISP_NO_CHANNEL_TO_PERFORM_ACTION_ON, label2str(event));
			sccp_dev_displayprinotify(d, buf, SCCP_MESSAGE_PRIORITY_TIMEOUT, 5);
			sccp_dev_starttone(d, SKINNY_TONE_BEEPBONK, lineInstance, callid, SKINNY_TONEDIRECTION_USER);
			pbx_log(LOG_NOTICE, "%s: softkey %s pressed with no call (line %d, call %d); reject tone played\n", d->id, label2str(event), lineInstance, callid);
		}

		if (d->indicate && d->indicate->onhook) {
			d->indicate->onhook(d, lineInstance, callid);
		}
	}
}

static channelPtr __get_channel_from_callReference_or_passThruParty(devicePtr d, uint32_t callReference, uint32_t callReference1, uint32_t passThruPartyId)
{
	sccp_channel_t * channel = NULL;

	if ((channel = sccp_device_getActiveChannel(d))) {
		if (										// make sure this is the intended channel
			(passThruPartyId && channel->passthrupartyid != passThruPartyId) ||
			(callReference && channel->callid != callReference) ||
			(callReference1 && channel->callid != callReference1)
		) {
			sccp_channel_release(&channel);
		}
	}

	if (!channel && passThruPartyId) {
		channel = sccp_channel_find_on_device_bypassthrupartyid(d, passThruPartyId);
	}

	if (!channel && (callReference || callReference1)) {
		channel = sccp_channel_find_byid(callReference ? callReference : callReference1);
	}

	if (!channel) {
		pbx_log(LOG_NOTICE, "%s: media response for a call that no longer exists (call %d/%d, party %d); ignored\n", DEV_ID_LOG(d), callReference, callReference1, passThruPartyId);
	}

	return channel;
}

void handle_port_response(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	uint32_t conferenceId = 0;

	uint32_t callReference = 0;

	uint32_t passThruPartyId = 0;

	uint32_t RTCPPortNumber = 0;
	skinny_mediaType_t mediaType = SKINNY_MEDIATYPE_SENTINEL;
	struct sockaddr_storage sas = { 0 };

	d->protocol->parsePortResponse(msg_in, &conferenceId, &callReference, &passThruPartyId, &sas, &RTCPPortNumber, &mediaType);

	if (sccp_netsock_is_any_addr(&sas)) {
		pbx_log(LOG_WARNING, "%s: phone returned RTP address 0.0.0.0:0, meaning it has no free RTP ports; this call will have no audio\n", d->id);
		return;
	}
	sccp_log(DEBUGCAT_RTP) (VERBOSE_PREFIX_3 "%s: port response: remote RTP %s, conference %d, party %u, call %u, RTCP port %d, media %s\n", d->id,
		sccp_netsock_stringify(&sas), conferenceId, passThruPartyId, callReference, RTCPPortNumber, skinny_mediaType2str(mediaType));

	AUTO_RELEASE(sccp_channel_t, channel , __get_channel_from_callReference_or_passThruParty(d, callReference, 0, passThruPartyId));
	if (channel) {
		sccp_rtp_t *rtp = NULL;
		switch(mediaType) {
			case SKINNY_MEDIA_TYPE_AUDIO:
				rtp = &(channel->rtp.audio);
				break;
			case SKINNY_MEDIA_TYPE_MAIN_VIDEO:
				rtp = &(channel->rtp.video);
				break;
			case SKINNY_MEDIA_TYPE_INVALID:
				pbx_log(LOG_WARNING, "%s: port response carries an invalid media type; ignored\n", d->id);
				return;
			default:
				pbx_log(LOG_WARNING, "%s: port response for media type %s, which is not supported; ignored\n", d->id, skinny_mediaType2str(mediaType));
				return;
		}

		if (channel && !sccp_netsock_equals(&sas, &rtp->phone_remote)) {
			sccp_log(DEBUGCAT_RTP) (VERBOSE_PREFIX_3 "%s: port response passed to RTP\n", channel->designator);
			rtp->RTCPPortNumber=RTCPPortNumber;
			sccp_rtp_set_phone(channel, rtp, &sas);
		}
	}
}

void handle_openReceiveChannelAck(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	skinny_mediastatus_t mediastatus = SKINNY_MEDIASTATUS_Unknown;
	uint32_t callReference = 0;

	uint32_t passThruPartyId = 0;
	int resultingChannelState = SCCP_RTP_STATUS_ERROR;

	struct sockaddr_storage sas = { 0 };
	d->protocol->parseOpenReceiveChannelAck(msg_in, &mediastatus, &sas, &passThruPartyId, &callReference);

	sccp_log(DEBUGCAT_RTP) (VERBOSE_PREFIX_3 "%s: open receive channel ack: status %s (%d), remote RTP %s, type %s, party %u, call %u\n", d->id, skinny_mediastatus2str(mediastatus), mediastatus, sccp_netsock_stringify(&sas), (d->directrtp ? "DirectRTP" : "Indirect RTP"), passThruPartyId, callReference);

	AUTO_RELEASE(sccp_channel_t, channel , __get_channel_from_callReference_or_passThruParty(d, callReference, 0, passThruPartyId));
	if(do_expect(channel != NULL && sccp_rtp_getState(&channel->rtp.audio, SCCP_RTP_RECEPTION) & SCCP_RTP_STATUS_PROGRESS)) {
		sccp_rtp_t * audio = &(channel->rtp.audio);
		switch (mediastatus) {
			case SKINNY_MEDIASTATUS_Ok:
				sccp_rtp_set_phone(channel, audio, &sas);
				resultingChannelState = sccp_channel_receiveChannelOpen(d, channel);
				break;
			case SKINNY_MEDIASTATUS_DeviceOnHook:
				sccp_log((DEBUGCAT_RTP))(VERBOSE_PREFIX_3 "%s: open receive channel ack ignored: the call has ended\n", d->id);
				resultingChannelState = sccp_channel_closeAllMediaTransmitAndReceive(channel) | SCCP_RTP_STATUS_ERROR;
				break;
			case SKINNY_MEDIASTATUS_OutOfChannels:
			case SKINNY_MEDIASTATUS_OutOfSockets:
				pbx_log(LOG_WARNING, "%s: phone refused a media channel because it is out of %s; call ended (the phone usually needs a restart to recover)\n", d->id, mediastatus == SKINNY_MEDIASTATUS_OutOfSockets ? "sockets" : "media channels");
				resultingChannelState = sccp_channel_closeAllMediaTransmitAndReceive(channel) | SCCP_RTP_STATUS_ERROR;
				sccp_channel_endcall(channel);
				break;
			default:
				pbx_log(LOG_ERROR, "%s: phone refused a media channel with status '%s' (%d); call ended\n", d->id, skinny_mediastatus2str(mediastatus), mediastatus);
				resultingChannelState = sccp_channel_closeAllMediaTransmitAndReceive(channel) | SCCP_RTP_STATUS_ERROR;
				sccp_channel_endcall(channel);
				break;
		}
		sccp_rtp_setState(audio, SCCP_RTP_RECEPTION, resultingChannelState);
	} else {
		if (mediastatus == SKINNY_MEDIASTATUS_Ok) {
			callReference = callReference ? callReference : passThruPartyId ^ 0xFFFFFFFF;
			sccp_msg_t *msg = NULL;

			REQ(msg, CloseReceiveChannel);
			if (!msg) {
				return;
			}
			msg->data.CloseReceiveChannel.lel_conferenceId = htolel(callReference);
			msg->data.CloseReceiveChannel.lel_passThruPartyId = htolel(passThruPartyId);
			msg->data.CloseReceiveChannel.lel_callReference = htolel(callReference);
			sccp_dev_send(d, msg);
		}
	}
}

void handle_startMediaTransmissionAck(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	skinny_mediastatus_t mediastatus = SKINNY_MEDIASTATUS_Unknown;
	uint32_t callReference = 0;

	uint32_t passThruPartyId = 0;

	uint32_t callReference1 = 0;
	int resultingChannelState = SCCP_RTP_STATUS_ERROR;

	struct sockaddr_storage sas = { 0 };
	d->protocol->parseStartMediaTransmissionAck(msg_in, &passThruPartyId, &callReference, &callReference1, &mediastatus, &sas);

	sccp_log(DEBUGCAT_RTP) (VERBOSE_PREFIX_3 "%s: start media transmission ack: status %s (%d), remote RTP %s, type %s, party %u, call %u, call1 %u\n", d->id, skinny_mediastatus2str(mediastatus), mediastatus, sccp_netsock_stringify(&sas), (d->directrtp ? "DirectRTP" : "Indirect RTP"), passThruPartyId, callReference, callReference1);

	AUTO_RELEASE(sccp_channel_t, channel , __get_channel_from_callReference_or_passThruParty(d, callReference, callReference1, passThruPartyId));
	if(do_expect(channel != NULL && sccp_rtp_getState(&channel->rtp.audio, SCCP_RTP_TRANSMISSION) & SCCP_RTP_STATUS_PROGRESS)) {
		sccp_rtp_t * audio = &(channel->rtp.audio);
		switch (mediastatus) {
			case SKINNY_MEDIASTATUS_Ok:
				resultingChannelState = sccp_channel_mediaTransmissionStarted(d, channel);
				break;
			case SKINNY_MEDIASTATUS_DeviceOnHook:
				sccp_log((DEBUGCAT_RTP))(VERBOSE_PREFIX_3 "%s: start media transmission ack ignored: the call has ended\n", d->id);
				resultingChannelState = sccp_channel_closeAllMediaTransmitAndReceive(channel) | SCCP_RTP_STATUS_ERROR;
				break;
			case SKINNY_MEDIASTATUS_OutOfChannels:
			case SKINNY_MEDIASTATUS_OutOfSockets:
				pbx_log(LOG_WARNING, "%s: phone refused a media channel because it is out of %s; call ended (the phone usually needs a restart to recover)\n", d->id, mediastatus == SKINNY_MEDIASTATUS_OutOfSockets ? "sockets" : "media channels");
				resultingChannelState = sccp_channel_closeAllMediaTransmitAndReceive(channel) | SCCP_RTP_STATUS_ERROR;
				sccp_channel_endcall(channel);
				break;
			default:
				pbx_log(LOG_ERROR, "%s: phone refused a media channel with status '%s' (%d); call ended\n", d->id, skinny_mediastatus2str(mediastatus), mediastatus);
				resultingChannelState = sccp_channel_closeAllMediaTransmitAndReceive(channel) | SCCP_RTP_STATUS_ERROR;
				sccp_channel_endcall(channel);
				break;
		}
		sccp_rtp_setState(audio, SCCP_RTP_TRANSMISSION, resultingChannelState);
	} else {
		if (mediastatus == SKINNY_MEDIASTATUS_Ok) {
			callReference = callReference ? callReference : (callReference1 ? callReference1 : passThruPartyId ^ 0xFFFFFFFF);
			sccp_msg_t *msg = NULL;

			REQ(msg, CloseReceiveChannel);
			if (!msg) {
				return;
			}
			msg->data.CloseReceiveChannel.lel_conferenceId = htolel(callReference);
			msg->data.CloseReceiveChannel.lel_passThruPartyId = htolel(passThruPartyId);
			msg->data.CloseReceiveChannel.lel_callReference = htolel(callReference);
			sccp_dev_send(d, msg);

			REQ(msg, StopMediaTransmission);
			if (!msg) {
				return;
			}
			msg->data.StopMediaTransmission.lel_conferenceId = htolel(callReference);
			msg->data.StopMediaTransmission.lel_passThruPartyId = htolel(passThruPartyId);
			msg->data.StopMediaTransmission.lel_callReference = htolel(callReference);
			sccp_dev_send(d, msg);
		}
	}
}

void handle_OpenMultiMediaReceiveAck(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	skinny_mediastatus_t mediastatus = SKINNY_MEDIASTATUS_Unknown;
	uint32_t callReference = 0;

	uint32_t passThruPartyId = 0;
	int resultingChannelState = SCCP_RTP_STATUS_ERROR;

	struct sockaddr_storage sas = { 0 };
	d->protocol->parseOpenMultiMediaReceiveChannelAck(msg_in, &mediastatus, &sas, &passThruPartyId, &callReference);

	sccp_log(DEBUGCAT_RTP) (VERBOSE_PREFIX_3 "%s: open multimedia channel ack: status %s (%d), remote RTP %s, type %s, party %u, call %u\n", d->id, skinny_mediastatus2str(mediastatus), mediastatus, sccp_netsock_stringify(&sas), (d->directrtp ? "DirectRTP" : "Indirect RTP"), passThruPartyId, callReference);

	AUTO_RELEASE(sccp_channel_t, channel , __get_channel_from_callReference_or_passThruParty(d, callReference, 0, passThruPartyId));
	if(do_expect(channel != NULL && sccp_rtp_getState(&channel->rtp.video, SCCP_RTP_RECEPTION) & SCCP_RTP_STATUS_PROGRESS)) {
		sccp_rtp_t * video = &(channel->rtp.video);
		switch (mediastatus) {
			case SKINNY_MEDIASTATUS_Ok:
				sccp_rtp_set_phone(channel, &channel->rtp.video, &sas);
				resultingChannelState = sccp_channel_receiveMultiMediaChannelOpen(d, channel);
				break;
			case SKINNY_MEDIASTATUS_DeviceOnHook:
				sccp_log((DEBUGCAT_RTP))(VERBOSE_PREFIX_3 "%s: open multimedia channel ack ignored: the call has ended\n", d->id);
				sccp_channel_closeMultiMediaReceiveChannel(channel, FALSE);
				sccp_channel_stopMultiMediaTransmission(channel, FALSE);
				break;
			case SKINNY_MEDIASTATUS_OutOfChannels:
			case SKINNY_MEDIASTATUS_OutOfSockets:
				pbx_log(LOG_WARNING, "%s: phone refused a media channel because it is out of %s; call ended (the phone usually needs a restart to recover)\n", d->id, mediastatus == SKINNY_MEDIASTATUS_OutOfSockets ? "sockets" : "media channels");
				sccp_channel_closeMultiMediaReceiveChannel(channel, FALSE);
				sccp_channel_stopMultiMediaTransmission(channel, FALSE);
				sccp_channel_endcall(channel);
				break;
			default:
				pbx_log(LOG_ERROR, "%s: phone refused a media channel with status '%s' (%d); call ended\n", d->id, skinny_mediastatus2str(mediastatus), mediastatus);
				sccp_channel_closeMultiMediaReceiveChannel(channel, FALSE);
				sccp_channel_stopMultiMediaTransmission(channel, FALSE);
				sccp_channel_endcall(channel);
				break;
		}
		sccp_rtp_setState(video, SCCP_RTP_RECEPTION, resultingChannelState);
	} else {
		if (mediastatus == SKINNY_MEDIASTATUS_Ok) {
			callReference = callReference ? callReference : passThruPartyId ^ 0xFFFFFFFF;
			sccp_msg_t *msg = NULL;

			REQ(msg, CloseMultiMediaReceiveChannel);
			if (!msg) {
				return;
			}
			msg->data.CloseMultiMediaReceiveChannel.lel_conferenceId = htolel(callReference);
			msg->data.CloseMultiMediaReceiveChannel.lel_passThruPartyId = htolel(passThruPartyId);
			msg->data.CloseMultiMediaReceiveChannel.lel_callReference = htolel(callReference);
			sccp_dev_send(d, msg);
		}
	}
}

void handle_startMultiMediaTransmissionAck(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	struct sockaddr_storage sas = { 0 };
	skinny_mediastatus_t mediastatus = SKINNY_MEDIASTATUS_Unknown;
	uint32_t passThruPartyId = 0;

	uint32_t callReference = 0;

	uint32_t callReference1 = 0;
	int resultingChannelState = SCCP_RTP_STATUS_ERROR;

	d->protocol->parseStartMultiMediaTransmissionAck(msg_in, &passThruPartyId, &callReference, &callReference1, &mediastatus, &sas);

	sccp_log(DEBUGCAT_RTP) (VERBOSE_PREFIX_3 "%s: start multimedia transmission ack: status %s (%d), remote RTP %s, type %s, party %u, call %u/%u\n", d->id, skinny_mediastatus2str(mediastatus), mediastatus, sccp_netsock_stringify(&sas), (d->directrtp ? "DirectRTP" : "Indirect RTP"), passThruPartyId, callReference, callReference1);

	AUTO_RELEASE(sccp_channel_t, channel , __get_channel_from_callReference_or_passThruParty(d, callReference, callReference1, passThruPartyId));
	if(do_expect(channel != NULL && sccp_rtp_getState(&channel->rtp.video, SCCP_RTP_TRANSMISSION) & SCCP_RTP_STATUS_PROGRESS)) {
		sccp_rtp_t * video = &(channel->rtp.video);
		switch (mediastatus) {
			case SKINNY_MEDIASTATUS_Ok:
				resultingChannelState = sccp_channel_multiMediaTransmissionStarted(d, channel);
				iPbx.queue_control(channel->owner, AST_CONTROL_VIDUPDATE);
				break;
			case SKINNY_MEDIASTATUS_DeviceOnHook:
				sccp_log((DEBUGCAT_RTP))(VERBOSE_PREFIX_3 "%s: start multimedia transmission ack ignored: the call has ended\n", d->id);
				sccp_channel_closeMultiMediaReceiveChannel(channel, FALSE);
				sccp_channel_stopMultiMediaTransmission(channel, FALSE);
				break;
			case SKINNY_MEDIASTATUS_OutOfChannels:
			case SKINNY_MEDIASTATUS_OutOfSockets:
				pbx_log(LOG_WARNING, "%s: phone refused a media channel because it is out of %s; call ended (the phone usually needs a restart to recover)\n", d->id, mediastatus == SKINNY_MEDIASTATUS_OutOfSockets ? "sockets" : "media channels");
				sccp_channel_closeMultiMediaReceiveChannel(channel, FALSE);
				sccp_channel_stopMultiMediaTransmission(channel, FALSE);
				sccp_channel_endcall(channel);
				break;
			default:
				pbx_log(LOG_ERROR, "%s: phone refused a media channel with status '%s' (%d); call ended\n", d->id, skinny_mediastatus2str(mediastatus), mediastatus);
				sccp_channel_closeMultiMediaReceiveChannel(channel, FALSE);
				sccp_channel_stopMultiMediaTransmission(channel, FALSE);
				sccp_channel_endcall(channel);
				break;
		}
		sccp_rtp_setState(video, SCCP_RTP_TRANSMISSION, resultingChannelState);
	} else {
		if (mediastatus == SKINNY_MEDIASTATUS_Ok) {
			callReference = callReference ? callReference : passThruPartyId ^ 0xFFFFFFFF;
			sccp_msg_t *msg = NULL;

			REQ(msg, CloseMultiMediaReceiveChannel);
			if (!msg) {
				return;
			}
			msg->data.CloseMultiMediaReceiveChannel.lel_conferenceId = htolel(callReference);
			msg->data.CloseMultiMediaReceiveChannel.lel_passThruPartyId = htolel(passThruPartyId);
			msg->data.CloseMultiMediaReceiveChannel.lel_callReference = htolel(callReference);
			sccp_dev_send(d, msg);

			REQ(msg, StopMultiMediaTransmission);
			if (!msg) {
				return;
			}
			msg->data.StopMultiMediaTransmission.lel_conferenceId = htolel(callReference);
			msg->data.StopMultiMediaTransmission.lel_passThruPartyId = htolel(passThruPartyId);
			msg->data.StopMultiMediaTransmission.lel_callReference = htolel(callReference);
			sccp_dev_send(d, msg);
		}
	}
}

void handle_mediaTransmissionFailure(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	sccp_dump_msg(msg_in);

	sccp_log((DEBUGCAT_RTP)) (VERBOSE_PREFIX_3 "%s: media transmission failure reported by the phone (not handled)\n", DEV_ID_LOG(d));
}

void handle_ipport(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	d->rtpPort = letohl(msg_in->data.IpPortMessage.lel_rtpMediaPort);
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: phone wants RTP port %d for media\n", d->id, d->rtpPort);
}

void handle_version(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	sccp_msg_t *msg_out = NULL;

	REQ(msg_out, VersionMessage);
	if (!msg_out) {
		return;
	}
	sccp_copy_string(msg_out->data.VersionMessage.requiredVersion, d->imageversion, sizeof(msg_out->data.VersionMessage.requiredVersion));
	sccp_dev_send(d, msg_out);
}

/*
 * Handle Connection Statistics for Session MOS LQK = Mean Opinion Score for listening Quality (5=Excellent -> 1=BAD) Max MOS LQK = Baseline or highest MOS LQK score observed from start of the voice stream.
 * These codecs provide the following maximum MOS LQK score under normal conditions with no frame loss: (G.711 gives 4.5, G.729 A /AB gives 3.7) MOS LQK Version = Version of the Cisco proprietary algorithm used to calculate MOS LQK scores.
 * If using voice activity detection (VAD), a longer interval might be required to accumulate 3 seconds of active speech.
 * Max Conceal Ratio = Highest interval concealment ratio from start of the voice stream.
 * Max Jitter = Maximum value of instantaneous jitter, in milliseconds.
 */
void handle_ConnectionStatistics(constSessionPtr s, devicePtr device, constMessagePtr msg_in)
{
#define CALC_AVG(_newval, _mean, _numval) ( ( ((_mean) * (_numval) ) + (_newval) ) / ((_numval) + 1))

	size_t buffersize = 2048;
	struct ast_str *output_buf = pbx_str_alloca(buffersize);
	char QualityStats[600] = "";
	uint32_t QualityStatsSize = 0;
	const uint32_t protocol_version = letohl(msg_in->header.lel_protocolVer);

	AUTO_RELEASE(sccp_device_t, d , sccp_device_retain(device));

	if (d) {
		sccp_call_statistics_t *call_stats = d->call_statistics;

		if (protocol_version < 20) {
			call_stats[SCCP_CALLSTATISTIC_LAST].num = letohl(msg_in->data.ConnectionStatisticsRes.v3.lel_CallIdentifier);
			call_stats[SCCP_CALLSTATISTIC_LAST].packets_sent = letohl(msg_in->data.ConnectionStatisticsRes.v3.lel_SentPackets);
			call_stats[SCCP_CALLSTATISTIC_LAST].packets_received = letohl(msg_in->data.ConnectionStatisticsRes.v3.lel_RecvdPackets);
			call_stats[SCCP_CALLSTATISTIC_LAST].packets_lost = letohl(msg_in->data.ConnectionStatisticsRes.v3.lel_LostPkts);
			call_stats[SCCP_CALLSTATISTIC_LAST].jitter = letohl(msg_in->data.ConnectionStatisticsRes.v3.lel_Jitter);
			call_stats[SCCP_CALLSTATISTIC_LAST].latency = letohl(msg_in->data.ConnectionStatisticsRes.v3.lel_latency);
			QualityStatsSize = letohl(msg_in->data.ConnectionStatisticsRes.v3.lel_QualityStatsSize);
			QualityStatsSize = QualityStatsSize < sizeof(QualityStats) ? QualityStatsSize + 1 : sizeof(QualityStats);
			if (QualityStatsSize) {
				sccp_copy_string(QualityStats, msg_in->data.ConnectionStatisticsRes.v3.QualityStats, QualityStatsSize);
			}
		} else if (protocol_version < 22) {
			call_stats[SCCP_CALLSTATISTIC_LAST].num = letohl(msg_in->data.ConnectionStatisticsRes.v20.lel_CallIdentifier);
			call_stats[SCCP_CALLSTATISTIC_LAST].packets_sent = letohl(msg_in->data.ConnectionStatisticsRes.v20.lel_SentPackets);
			call_stats[SCCP_CALLSTATISTIC_LAST].packets_received = letohl(msg_in->data.ConnectionStatisticsRes.v20.lel_RecvdPackets);
			call_stats[SCCP_CALLSTATISTIC_LAST].packets_lost = letohl(msg_in->data.ConnectionStatisticsRes.v20.lel_LostPkts);
			call_stats[SCCP_CALLSTATISTIC_LAST].jitter = letohl(msg_in->data.ConnectionStatisticsRes.v20.lel_Jitter);
			call_stats[SCCP_CALLSTATISTIC_LAST].latency = letohl(msg_in->data.ConnectionStatisticsRes.v20.lel_latency);
			QualityStatsSize = letohl(msg_in->data.ConnectionStatisticsRes.v20.lel_QualityStatsSize);
			QualityStatsSize = QualityStatsSize < sizeof(QualityStats) ? QualityStatsSize + 1 : sizeof(QualityStats);
			if (QualityStatsSize) {
				sccp_copy_string(QualityStats, msg_in->data.ConnectionStatisticsRes.v20.QualityStats, QualityStatsSize);
			}
		} else {
			// ConnectionStatisticsRes_V22 has irregular packing (single byte packing), need to access unaligned data (using get_unaligned_uint32 for sparc62 / buserror machines
#if defined(HAVE_UNALIGNED_BUSERROR)
			call_stats[SCCP_CALLSTATISTIC_LAST].num = letohl(get_unaligned_uint32((const void *) &msg_in->data.ConnectionStatisticsRes.v22.lel_CallIdentifier));
			call_stats[SCCP_CALLSTATISTIC_LAST].packets_sent = letohl(get_unaligned_uint32((const void *) &msg_in->data.ConnectionStatisticsRes.v22.lel_SentPackets));
			call_stats[SCCP_CALLSTATISTIC_LAST].packets_received = letohl(get_unaligned_uint32((const void *) &msg_in->data.ConnectionStatisticsRes.v22.lel_RecvdPackets));
			call_stats[SCCP_CALLSTATISTIC_LAST].packets_lost = letohl(get_unaligned_uint32((const void *) &msg_in->data.ConnectionStatisticsRes.v22.lel_LostPkts));
			call_stats[SCCP_CALLSTATISTIC_LAST].jitter = letohl(get_unaligned_uint32((const void *) &msg_in->data.ConnectionStatisticsRes.v22.lel_Jitter));
			call_stats[SCCP_CALLSTATISTIC_LAST].latency = letohl(get_unaligned_uint32((const void *) &msg_in->data.ConnectionStatisticsRes.v22.lel_latency));
			QualityStatsSize = letohl(get_unaligned_uint32((const void *) &msg_in->data.ConnectionStatisticsRes.v22.lel_QualityStatsSize));
#else
			call_stats[SCCP_CALLSTATISTIC_LAST].num = letohl(msg_in->data.ConnectionStatisticsRes.v22.lel_CallIdentifier);
			call_stats[SCCP_CALLSTATISTIC_LAST].packets_sent = letohl(msg_in->data.ConnectionStatisticsRes.v22.lel_SentPackets);
			call_stats[SCCP_CALLSTATISTIC_LAST].packets_received = letohl(msg_in->data.ConnectionStatisticsRes.v22.lel_RecvdPackets);
			call_stats[SCCP_CALLSTATISTIC_LAST].packets_lost = letohl(msg_in->data.ConnectionStatisticsRes.v22.lel_LostPkts);
			call_stats[SCCP_CALLSTATISTIC_LAST].jitter = letohl(msg_in->data.ConnectionStatisticsRes.v22.lel_Jitter);
			call_stats[SCCP_CALLSTATISTIC_LAST].latency = letohl(msg_in->data.ConnectionStatisticsRes.v22.lel_latency);
			QualityStatsSize = letohl(msg_in->data.ConnectionStatisticsRes.v22.lel_QualityStatsSize);
#endif
			QualityStatsSize = QualityStatsSize < sizeof(QualityStats) ? QualityStatsSize + 1 : sizeof(QualityStats);
			if (QualityStatsSize) {
				sccp_copy_string(QualityStats, msg_in->data.ConnectionStatisticsRes.v22.QualityStats, QualityStatsSize);
			}
		}
		sccp_log(DEBUGCAT_CORE) (VERBOSE_PREFIX_3 "quality statistics: %s\n", QualityStats);
		if (!sccp_strlen_zero(QualityStats)) {
			if (protocol_version < 20) {
				sscanf(QualityStats, "MLQK=%f;MLQKav=%f;MLQKmn=%f;MLQKmx=%f;MLQKvr=%f;CCR=%f;ICR=%f;ICRmx=%f;CS=%d;SCS=%d",
				       &call_stats[SCCP_CALLSTATISTIC_LAST].opinion_score_listening_quality, &call_stats[SCCP_CALLSTATISTIC_LAST].avg_opinion_score_listening_quality,
				       &call_stats[SCCP_CALLSTATISTIC_LAST].mean_opinion_score_listening_quality, &call_stats[SCCP_CALLSTATISTIC_LAST].max_opinion_score_listening_quality,
				       &call_stats[SCCP_CALLSTATISTIC_LAST].variance_opinion_score_listening_quality, &call_stats[SCCP_CALLSTATISTIC_LAST].cumulative_concealement_ratio, &call_stats[SCCP_CALLSTATISTIC_LAST].interval_concealement_ratio, &call_stats[SCCP_CALLSTATISTIC_LAST].max_concealement_ratio, &call_stats[SCCP_CALLSTATISTIC_LAST].concealed_seconds, &call_stats[SCCP_CALLSTATISTIC_LAST].severely_concealed_seconds);
			} else if (protocol_version < 22) {
				int Log = 0;

				sscanf(QualityStats, "Log %d: mos %f, avgMos %f, maxMos %f, minMos %f, CS %d, SCS %d, CCR %f, ICR %f, maxCR %f",
				       &Log,
				       &call_stats[SCCP_CALLSTATISTIC_LAST].opinion_score_listening_quality, &call_stats[SCCP_CALLSTATISTIC_LAST].avg_opinion_score_listening_quality,
				       &call_stats[SCCP_CALLSTATISTIC_LAST].max_opinion_score_listening_quality, &call_stats[SCCP_CALLSTATISTIC_LAST].mean_opinion_score_listening_quality,
				       &call_stats[SCCP_CALLSTATISTIC_LAST].concealed_seconds, &call_stats[SCCP_CALLSTATISTIC_LAST].severely_concealed_seconds, &call_stats[SCCP_CALLSTATISTIC_LAST].cumulative_concealement_ratio, &call_stats[SCCP_CALLSTATISTIC_LAST].interval_concealement_ratio, &call_stats[SCCP_CALLSTATISTIC_LAST].max_concealement_ratio);
			} else {
				sscanf(QualityStats, "MLQK=%f;MLQKav=%f;MLQKmn=%f;MLQKmx=%f;ICR=%f;CCR=%f;ICRmx=%f;CS=%d;SCS=%d;MLQKvr=%f",
				       &call_stats[SCCP_CALLSTATISTIC_LAST].opinion_score_listening_quality, &call_stats[SCCP_CALLSTATISTIC_LAST].avg_opinion_score_listening_quality,
				       &call_stats[SCCP_CALLSTATISTIC_LAST].mean_opinion_score_listening_quality, &call_stats[SCCP_CALLSTATISTIC_LAST].max_opinion_score_listening_quality,
				       &call_stats[SCCP_CALLSTATISTIC_LAST].interval_concealement_ratio, &call_stats[SCCP_CALLSTATISTIC_LAST].cumulative_concealement_ratio, &call_stats[SCCP_CALLSTATISTIC_LAST].max_concealement_ratio, &call_stats[SCCP_CALLSTATISTIC_LAST].concealed_seconds, &call_stats[SCCP_CALLSTATISTIC_LAST].severely_concealed_seconds, &call_stats[SCCP_CALLSTATISTIC_LAST].variance_opinion_score_listening_quality);
			}
		}
		sccp_call_quality_t * entry = &d->call_history.entry[d->call_history.next];
		entry->ended                      = time(NULL);
		entry->callid                     = call_stats[SCCP_CALLSTATISTIC_LAST].num;
		entry->packets_sent               = call_stats[SCCP_CALLSTATISTIC_LAST].packets_sent;
		entry->packets_received           = call_stats[SCCP_CALLSTATISTIC_LAST].packets_received;
		entry->packets_lost               = call_stats[SCCP_CALLSTATISTIC_LAST].packets_lost;
		entry->jitter                     = call_stats[SCCP_CALLSTATISTIC_LAST].jitter;
		entry->latency                    = call_stats[SCCP_CALLSTATISTIC_LAST].latency;
		entry->mos_average                = call_stats[SCCP_CALLSTATISTIC_LAST].avg_opinion_score_listening_quality;
		entry->mos_minimum                = call_stats[SCCP_CALLSTATISTIC_LAST].mean_opinion_score_listening_quality;
		entry->concealed_seconds          = call_stats[SCCP_CALLSTATISTIC_LAST].concealed_seconds;
		entry->severely_concealed_seconds = call_stats[SCCP_CALLSTATISTIC_LAST].severely_concealed_seconds;
		d->call_history.next              = (d->call_history.next + 1) % SCCP_CALL_HISTORY_SIZE;
		if (d->call_history.count < SCCP_CALL_HISTORY_SIZE) {
			d->call_history.count++;
		}
		pbx_str_append(&output_buf, buffersize, "%s: call %d: sent %d, received %d, lost %d packets; jitter %d ms, latency %d ms; MOS %.2f (min %.2f); concealed %d s (severely %d s)\n", d->id,
			       entry->callid, entry->packets_sent, entry->packets_received, entry->packets_lost, entry->jitter, entry->latency, entry->mos_average, entry->mos_minimum,
			       (int)entry->concealed_seconds, (int)entry->severely_concealed_seconds);

		call_stats[SCCP_CALLSTATISTIC_AVG].packets_sent = CALC_AVG(call_stats[SCCP_CALLSTATISTIC_LAST].packets_sent, call_stats[SCCP_CALLSTATISTIC_AVG].packets_sent, call_stats[SCCP_CALLSTATISTIC_AVG].num);
		call_stats[SCCP_CALLSTATISTIC_AVG].packets_received = CALC_AVG(call_stats[SCCP_CALLSTATISTIC_LAST].packets_received, call_stats[SCCP_CALLSTATISTIC_AVG].packets_received, call_stats[SCCP_CALLSTATISTIC_AVG].num);
		call_stats[SCCP_CALLSTATISTIC_AVG].packets_lost = CALC_AVG(call_stats[SCCP_CALLSTATISTIC_LAST].packets_lost, call_stats[SCCP_CALLSTATISTIC_AVG].packets_lost, call_stats[SCCP_CALLSTATISTIC_AVG].num);
		call_stats[SCCP_CALLSTATISTIC_AVG].jitter = CALC_AVG(call_stats[SCCP_CALLSTATISTIC_LAST].jitter, call_stats[SCCP_CALLSTATISTIC_AVG].jitter, call_stats[SCCP_CALLSTATISTIC_AVG].num);
		call_stats[SCCP_CALLSTATISTIC_AVG].latency = CALC_AVG(call_stats[SCCP_CALLSTATISTIC_LAST].latency, call_stats[SCCP_CALLSTATISTIC_AVG].latency, call_stats[SCCP_CALLSTATISTIC_AVG].num);
		call_stats[SCCP_CALLSTATISTIC_AVG].opinion_score_listening_quality = CALC_AVG(call_stats[SCCP_CALLSTATISTIC_LAST].opinion_score_listening_quality, call_stats[SCCP_CALLSTATISTIC_AVG].opinion_score_listening_quality, call_stats[SCCP_CALLSTATISTIC_AVG].num);
		call_stats[SCCP_CALLSTATISTIC_AVG].avg_opinion_score_listening_quality = CALC_AVG(call_stats[SCCP_CALLSTATISTIC_LAST].avg_opinion_score_listening_quality, call_stats[SCCP_CALLSTATISTIC_AVG].avg_opinion_score_listening_quality, call_stats[SCCP_CALLSTATISTIC_AVG].num);
		call_stats[SCCP_CALLSTATISTIC_AVG].mean_opinion_score_listening_quality = CALC_AVG(call_stats[SCCP_CALLSTATISTIC_LAST].mean_opinion_score_listening_quality, call_stats[SCCP_CALLSTATISTIC_AVG].mean_opinion_score_listening_quality, call_stats[SCCP_CALLSTATISTIC_AVG].num);
		if (call_stats[SCCP_CALLSTATISTIC_AVG].max_opinion_score_listening_quality < call_stats[SCCP_CALLSTATISTIC_LAST].max_opinion_score_listening_quality) {
			call_stats[SCCP_CALLSTATISTIC_AVG].max_opinion_score_listening_quality = call_stats[SCCP_CALLSTATISTIC_LAST].max_opinion_score_listening_quality;
		}
		call_stats[SCCP_CALLSTATISTIC_AVG].interval_concealement_ratio = CALC_AVG(call_stats[SCCP_CALLSTATISTIC_LAST].interval_concealement_ratio, call_stats[SCCP_CALLSTATISTIC_AVG].interval_concealement_ratio, call_stats[SCCP_CALLSTATISTIC_AVG].num);
		call_stats[SCCP_CALLSTATISTIC_AVG].cumulative_concealement_ratio = CALC_AVG(call_stats[SCCP_CALLSTATISTIC_LAST].cumulative_concealement_ratio, call_stats[SCCP_CALLSTATISTIC_AVG].cumulative_concealement_ratio, call_stats[SCCP_CALLSTATISTIC_AVG].num);
		if (call_stats[SCCP_CALLSTATISTIC_AVG].max_concealement_ratio < call_stats[SCCP_CALLSTATISTIC_LAST].max_concealement_ratio) {
			call_stats[SCCP_CALLSTATISTIC_AVG].max_concealement_ratio = call_stats[SCCP_CALLSTATISTIC_LAST].max_concealement_ratio;
		}
		call_stats[SCCP_CALLSTATISTIC_AVG].concealed_seconds = CALC_AVG(call_stats[SCCP_CALLSTATISTIC_LAST].concealed_seconds, call_stats[SCCP_CALLSTATISTIC_AVG].concealed_seconds, call_stats[SCCP_CALLSTATISTIC_AVG].num);
		call_stats[SCCP_CALLSTATISTIC_AVG].severely_concealed_seconds = CALC_AVG(call_stats[SCCP_CALLSTATISTIC_LAST].severely_concealed_seconds, call_stats[SCCP_CALLSTATISTIC_AVG].severely_concealed_seconds, call_stats[SCCP_CALLSTATISTIC_AVG].num);
		call_stats[SCCP_CALLSTATISTIC_AVG].variance_opinion_score_listening_quality = CALC_AVG(call_stats[SCCP_CALLSTATISTIC_LAST].variance_opinion_score_listening_quality, call_stats[SCCP_CALLSTATISTIC_AVG].variance_opinion_score_listening_quality, call_stats[SCCP_CALLSTATISTIC_AVG].num);

		call_stats[SCCP_CALLSTATISTIC_AVG].num++;
		pbx_str_append(&output_buf, buffersize, "%s: average over %d calls: lost %d packets; jitter %d ms, latency %d ms; MOS %.2f\n", d->id, call_stats[SCCP_CALLSTATISTIC_AVG].num,
			       call_stats[SCCP_CALLSTATISTIC_AVG].packets_lost, call_stats[SCCP_CALLSTATISTIC_AVG].jitter, call_stats[SCCP_CALLSTATISTIC_AVG].latency, call_stats[SCCP_CALLSTATISTIC_AVG].avg_opinion_score_listening_quality);
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s", pbx_str_buffer(output_buf));
	}
}

void handle_ServerResMessage(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	pbx_assert(d != NULL);
	sccp_msg_t *msg_out = NULL;

	if (!sccp_session_isValid(s) || sccp_session_check_crossdevice(s, d)) {
		pbx_log(LOG_WARNING, "%s: server list request on connection %s, which is closed or belongs to another device; ignored\n", DEV_ID_LOG(d), sccp_session_getDesignator(s));
		return;
	}
	sccp_log(DEBUGCAT_CORE) (VERBOSE_PREFIX_3 "%s: sending server list (%s)\n", DEV_ID_LOG(d), sccp_session_getDesignator(s));

	REQ(msg_out, ServerResMessage);
	if (!msg_out) {
		return;
	}
	if (d->protocolversion < 17) {
		struct sockaddr_storage sas = { 0 };
		sccp_session_getOurIP(s, &sas, 0);
		sccp_copy_string(msg_out->data.ServerResMessage.v3.server[0].serverName, GLOB(servername), sizeof(msg_out->data.ServerResMessage.v3.server[0].serverName));
		msg_out->data.ServerResMessage.v3.serverListenPort[0] = sccp_netsock_getPort(&GLOB(bindaddr));
		struct sockaddr_in *in = (struct sockaddr_in *) &sas;
		memcpy(&msg_out->data.ServerResMessage.v3.serverIpAddr[0], &in->sin_addr, 4);
	} else {
		struct sockaddr_storage sas = { 0 };
		sccp_session_getOurIP(s, &sas, 0);
		sccp_copy_string(msg_out->data.ServerResMessage.v17.server[0].serverName, GLOB(servername), sizeof(msg_out->data.ServerResMessage.v17.server[0].serverName));
		msg_out->data.ServerResMessage.v17.serverListenPort[0] = sccp_netsock_getPort(&GLOB(bindaddr));
		msg_out->data.ServerResMessage.v17.serverIpAddr[0].lel_ipv46 = htolel(sas.ss_family == AF_INET6 ? 1 : 0);
		struct sockaddr_in6 *in6 = (struct sockaddr_in6 *) &sas;
		memcpy(&msg_out->data.ServerResMessage.v17.serverIpAddr[0].bel_ipAddr, &in6->sin6_addr, 16);
	}
	sccp_dev_send(d, msg_out);
}

void handle_ConfigStatMessage(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	sccp_msg_t *msg_out = NULL;
	sccp_buttonconfig_t *config = NULL;
	uint8_t lines = 0;
	uint8_t speeddials = 0;

	SCCP_LIST_LOCK(&d->buttonconfig);
	SCCP_LIST_TRAVERSE(&d->buttonconfig, config, list) {
		if (config->type == SPEEDDIAL) {
			speeddials++;
		} else if (config->type == LINE) {
			lines++;
		}
	}
	SCCP_LIST_UNLOCK(&d->buttonconfig);

	REQ(msg_out, ConfigStatMessage);
	if (!msg_out) {
		return;
	}
	sccp_copy_string(msg_out->data.ConfigStatMessage.station_identifier.deviceName, d->id, sizeof(msg_out->data.ConfigStatMessage.station_identifier.deviceName));
	msg_out->data.ConfigStatMessage.station_identifier.lel_stationUserId = htolel(0);
	msg_out->data.ConfigStatMessage.station_identifier.lel_stationInstance = htolel(1);
	sccp_copy_string(msg_out->data.ConfigStatMessage.userName, d->id, sizeof(msg_out->data.ConfigStatMessage.userName));
	sccp_copy_string(msg_out->data.ConfigStatMessage.serverName, GLOB(servername), sizeof(msg_out->data.ConfigStatMessage.serverName));
	msg_out->data.ConfigStatMessage.lel_numberLines = htolel(lines);
	msg_out->data.ConfigStatMessage.lel_numberSpeedDials = htolel(speeddials);

	sccp_dev_send(d, msg_out);
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: sending config status: %d lines, %d speeddials\n", DEV_ID_LOG(d), lines, speeddials);
}

/* Handle Enbloc Call Message (Dial in one block, instead of number by number) */
void handle_EnblocCallMessage(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	int len = 0;

	char calledParty[25] = { 0 };
	uint32_t lineInstance = 0;

	if (d->protocol->parseEnblocCall) {
		d->protocol->parseEnblocCall(msg_in, calledParty, &lineInstance);
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: enbloc call to %s on line instance %d\n", DEV_ID_LOG(d), calledParty, lineInstance);

		if (!sccp_strlen_zero(calledParty)) {
			AUTO_RELEASE(sccp_channel_t, channel , sccp_device_getActiveChannel(d));

			if (channel) {
				if ((channel->state == SCCP_CHANNELSTATE_DIALING) || (channel->state == SCCP_CHANNELSTATE_OFFHOOK)) {
					if (d->isAnonymous) {
						return;
					}

					sccp_channel_stop_schedule_digittimout(channel);
					len = sccp_strlen(channel->dialedNumber);
					sccp_copy_string(channel->dialedNumber + len, calledParty, sizeof(channel->dialedNumber) - len);
					sccp_pbx_softswitch(channel);
					return;
				}
				if (iPbx.send_digits) {
					iPbx.send_digits(channel, calledParty);
				}
				return;
			}
			if (!lineInstance) {
				lineInstance = d->defaultLineInstance ? d->defaultLineInstance : SCCP_FIRST_LINEINSTANCE;
			}

			AUTO_RELEASE(sccp_linedevice_t, ld, sccp_linedevice_findByLineinstance(d, lineInstance));
			if(ld) {
				AUTO_RELEASE(sccp_channel_t, new_channel, sccp_channel_newcall(ld->line, d, calledParty, SKINNY_CALLTYPE_OUTBOUND, NULL, NULL));
				sccp_channel_stop_schedule_digittimout(new_channel);
			}
		}
	}
}
void handle_forward_stat_req(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	sccp_msg_t *msg_out = NULL;

	uint32_t instance = letohl(msg_in->data.ForwardStatReqMessage.lel_lineNumber);

	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: forward status request for line %d\n", d->id, instance);

	AUTO_RELEASE(sccp_line_t, l , sccp_line_find_byid(d, instance));

	if (l) {
		sccp_dev_forward_status(l, instance, d);
		return;
	}

	REQ(msg_out, ForwardStatMessage);
	if (!msg_out) {
		return;
	}
	msg_out->data.ForwardStatMessage.v3.lel_lineNumber = msg_in->data.ForwardStatReqMessage.lel_lineNumber;
	sccp_dev_send(d, msg_out);
}

void handle_feature_stat_req(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	sccp_buttonconfig_t *config = NULL;

	int featureIndex = letohl(msg_in->data.FeatureStatReqMessage.lel_featureIndex);
	int capabilities = letohl(msg_in->data.FeatureStatReqMessage.lel_featureCapabilities);

	sccp_log((DEBUGCAT_FEATURE)) (VERBOSE_PREFIX_3 "%s: feature status request: index %d, capabilities %d\n", d->id, featureIndex, capabilities);

#ifdef CS_DYNAMIC_SPEEDDIAL
	sccp_speed_t k;

	if ((capabilities == 1 && d->inuseprotocolversion >= 15)) {
		sccp_dev_speed_find_byindex(d, featureIndex, TRUE, &k);

		if (k.valid) {
			sccp_msg_t * msg = NULL;

			REQ(msg, FeatureStatDynamicMessage);
			if (!msg) {
				return;
			}
			msg->data.FeatureStatDynamicMessage.lel_lineInstance = htolel(featureIndex);
			msg->data.FeatureStatDynamicMessage.lel_buttonType = htolel(SKINNY_BUTTONTYPE_BLFSPEEDDIAL);
			msg->data.FeatureStatDynamicMessage.stateVal.lel_uint32 = htolel(0);
			d->copyStr2Locale(d, msg->data.FeatureStatDynamicMessage.textLabel, k.name, sizeof(msg->data.FeatureStatDynamicMessage.textLabel));
			sccp_dev_send(d, msg);
			return;
		}
	}
#endif

	SCCP_LIST_TRAVERSE(&d->buttonconfig, config, list) {
		if (config->instance == featureIndex && config->type == FEATURE) {
			sccp_feat_changed(d, NULL, config->button.feature.id);
		}
	}
}

void handle_services_stat_req(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	sccp_msg_t * msg_out = NULL;
	sccp_buttonconfig_t * config = NULL;

	int urlIndex = letohl(msg_in->data.ServiceURLStatReqMessage.lel_serviceURLIndex);

	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: service URL status request: index %d\n", d->id, urlIndex);

	if ((config = sccp_dev_serviceURL_find_byindex(d, urlIndex))) {
		if (d->inuseprotocolversion < 7) {
			REQ(msg_out, ServiceURLStatMessage);
			if (!msg_out) {
				return;
			}
			msg_out->data.ServiceURLStatMessage.lel_serviceURLIndex = htolel(urlIndex);
			sccp_copy_string(msg_out->data.ServiceURLStatMessage.URL, config->button.service.url, sccp_strlen(config->button.service.url) + 1);
			d->copyStr2Locale(d, msg_out->data.ServiceURLStatMessage.label, config->label, sccp_strlen(config->label) + 1);
		} else {
			int URL_len = sccp_strlen(config->button.service.url);
			int label_len = sccp_strlen(config->label);
			int dummy_len = URL_len + label_len;

			int hdr_len = sizeof(msg_in->data.ServiceURLStatDynamicMessage) - 1;

			msg_out = sccp_build_packet(ServiceURLStatDynamicMessage, hdr_len + dummy_len);
			if (!msg_out) {
				return;
			}
			msg_out->data.ServiceURLStatDynamicMessage.lel_serviceURLIndex = htolel(urlIndex);

			if (dummy_len) {
				char buffer[dummy_len + 2];

				memset(&buffer[0], 0, dummy_len + 2);
				if (URL_len) {
					memcpy(&buffer[0], config->button.service.url, URL_len);
				}
				if (label_len) {
					memcpy(&buffer[URL_len + 1], config->label, label_len);
				}
				memcpy(&msg_out->data.ServiceURLStatDynamicMessage.dummy, &buffer[0], dummy_len + 2);
			}
		}
		sccp_dev_send(d, msg_out);
	} else {
		pbx_log(LOG_NOTICE, "%s: phone asked for service URL %d, which is not configured; ignored\n", sccp_session_getDesignator(s), urlIndex);
	}
}

#if defined(CS_SCCP_VIDEO) && defined(DEBUG) && DEBUG == 1
static void handle_updatecapabilities_dissect_customPictureFormat(constDevicePtr d, uint32_t customPictureFormatCount, const customPictureFormat_t customPictureFormat[MAX_CUSTOM_PICTURES]) {
	uint8_t video_customPictureFormat = 0;
	if (customPictureFormatCount <= MAX_CUSTOM_PICTURES) {
		for (video_customPictureFormat = 0; video_customPictureFormat < customPictureFormatCount; video_customPictureFormat++) {
			int width = letohl(customPictureFormat[video_customPictureFormat].lel_width);
			int height = letohl(customPictureFormat[video_customPictureFormat].lel_height);
			int pixelAspectRatio = letohl(customPictureFormat[video_customPictureFormat].lel_pixelAspectRatio);
			int pixelClockConversion = letohl(customPictureFormat[video_customPictureFormat].lel_pixelclockConversionCode);
			int pixelClockDivisor = letohl(customPictureFormat[video_customPictureFormat].lel_pixelclockDivisor);

			sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %7s %-5s customPictureFormat %d: width=%d, height=%d, pixelAspectRatio=%d, pixelClockConversion=%d, pixelClockDivisor=%d\n", DEV_ID_LOG(d), "", "", video_customPictureFormat, width, height, pixelAspectRatio, pixelClockConversion, pixelClockDivisor);
		}
	} else {
		pbx_log(LOG_WARNING, "%s: phone reported %d custom video picture formats, more than the %d supported; video capabilities ignored\n", DEV_ID_LOG(d), customPictureFormatCount, MAX_CUSTOM_PICTURES);
	}
}

static void handle_updatecapabilities_dissect_levelPreference(constDevicePtr d, uint32_t levelPreferenceCount, const levelPreference_t levelPreference[MAX_LEVEL_PREFERENCE])
{
	uint8_t level = 0;
	if (levelPreferenceCount <= MAX_LEVEL_PREFERENCE) {
		sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %7s %d level preferences:\n", DEV_ID_LOG(d), "", levelPreferenceCount);
		for (level = 0; level < levelPreferenceCount; level++) {
			int transmitPreference = letohl(levelPreference[level].lel_transmitPreference);
			skinny_videoformat_t video_format = letohl(levelPreference[level].lel_format);
			int maxBitRate = letohl(levelPreference[level].lel_maxBitRate);
			int minBitRate = letohl(levelPreference[level].lel_minBitRate);
			int MPI = letohl(levelPreference[level].lel_MPI);
			int serviceNumber = letohl(levelPreference[level].lel_serviceNumber);

			sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %6s %2d: %-3s transmitPreference: %d\n", DEV_ID_LOG(d), "", level, "", transmitPreference);
			sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %14s format: %d: %s\n", DEV_ID_LOG(d), "", video_format, skinny_videoformat2str(video_format));
			sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %14s maxBitRate: %d\n", DEV_ID_LOG(d), "", maxBitRate);
			sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %14s minBitRate: %d\n", DEV_ID_LOG(d), "", minBitRate);
			sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %14s MPI: %d\n", DEV_ID_LOG(d), "", MPI);
			sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %14s serviceNumber: %d\n", DEV_ID_LOG(d), "", serviceNumber);
		}
	} else {
		pbx_log(LOG_WARNING, "%s: phone reported %d video level preferences, more than the %d supported; video capabilities ignored\n", DEV_ID_LOG(d), levelPreferenceCount, MAX_LEVEL_PREFERENCE);
	}
}

static void handle_updatecapabilities_dissect_videocapabiltyunion(constDevicePtr d, uint32_t video_codec, const videoCapabilityUnionV2_t *capability) {
	switch (video_codec) {
		case SKINNY_CODEC_H261:
			{
				int temporalSpatialTradeOffCapability = letohl(capability->h261.lel_temporalSpatialTradeOffCapability);
				int stillImageTransmission = letohl(capability->h261.lel_stillImageTransmission);

				sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %-7s temporalSpatialTradeOff: %d\n", DEV_ID_LOG(d), "", temporalSpatialTradeOffCapability);
				sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %-7s stillImageTransmission: %d\n", DEV_ID_LOG(d), "", stillImageTransmission);
			}
			break;
		case SKINNY_CODEC_H263:
			{
				int capabilityBitfield = letohl(capability->h263.lel_capabilityBitfield);
				int annexNandW = letohl(capability->h263.lel_annexNandWFutureUse);

				sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %-7s capabilityBitfield: %d\n", DEV_ID_LOG(d), "", capabilityBitfield);
				sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %-7s annexNandW: %d\n", DEV_ID_LOG(d), "", annexNandW);
			}
			break;
		case SKINNY_CODEC_H263P:
			{
				int modelNumber= letohl(capability->h263P.lel_modelNumber);
				int bandwidth = letohl(capability->h263P.lel_bandwidth);

				sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %-7s model: %d\n", DEV_ID_LOG(d), "", modelNumber);
				sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %-7s bandwidth: %d\n", DEV_ID_LOG(d), "", bandwidth);
			}
			break;
		case SKINNY_CODEC_H264:
			{
				int level = letohl(capability->h264.lel_level);
				int profile = letohl(capability->h264.lel_profile);
				int customMaxMBPS = letohl(capability->h264.lel_customMaxMBPS);
				int customMaxFS = letohl(capability->h264.lel_customMaxFS);
				int customMaxDPB = letohl(capability->h264.lel_customMaxDPB);
				int customMaxBRandCPB = letohl(capability->h264.lel_customMaxBRandCPB);

				sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %-7s level: %d\n", DEV_ID_LOG(d), "", level);
				sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %-7s profile: %d\n", DEV_ID_LOG(d), "", profile);
				sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %-7s customMaxMBPS: %d\n", DEV_ID_LOG(d), "", customMaxMBPS);
				sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %-7s customMaxFS: %d\n", DEV_ID_LOG(d), "", customMaxFS);
				sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %-7s customMaxDPB: %d\n", DEV_ID_LOG(d), "", customMaxDPB);
				sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %-7s customMaxBRandCPB: %d\n", DEV_ID_LOG(d), "", customMaxBRandCPB);
			}
			break;
	}
}
#endif

/*
 * Will be better to store audio codec max packet size and video bandwidth and size.
 * In future we will parse also data caps to support T.38 and NSE with ATA186/188 devices.
 */
void handle_updatecapabilities_message(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	pbx_assert(d != NULL && s != NULL && msg_in != NULL);
	if (letohl(msg_in->header.lel_protocolVer) >= 16) {
		handle_updatecapabilities_V2_message(s, d, msg_in);
	} else {
		uint8_t audio_capability = 0;

		uint8_t audio_capabilities = 0;
		skinny_codec_t audio_codec = SKINNY_CODEC_NONE;
		uint32_t maxFramesPerPacket = 0;
		audio_capabilities = letohl(msg_in->data.UpdateCapabilitiesMessage.v3.lel_audioCapCount);
		int RTPPayloadFormat = letohl(msg_in->data.UpdateCapabilitiesMessage.v3.lel_RTPPayloadFormat);
		sccp_log((DEBUGCAT_CORE + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %d audio capabilities, RTP payload format %d\n", DEV_ID_LOG(d), audio_capabilities, RTPPayloadFormat);

		if (audio_capabilities > 0 && audio_capabilities <= SKINNY_MAX_CAPABILITIES) {
			sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %7s %-25s %-9s\n", DEV_ID_LOG(d), "#", "codec", "maxFrames");
			for (audio_capability = 0; audio_capability < audio_capabilities; audio_capability++) {
				audio_codec = letohl(msg_in->data.UpdateCapabilitiesMessage.v3.audioCaps[audio_capability].lel_payloadCapability);
				if (codec2type(audio_codec) == SKINNY_CODEC_TYPE_AUDIO) {
					maxFramesPerPacket = letohl(msg_in->data.UpdateCapabilitiesMessage.v3.audioCaps[audio_capability].lel_maxFramesPerPacket);
					d->capabilities.audio[audio_capability] = audio_codec;
					sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %7d %-25s %-6d\n", DEV_ID_LOG(d), audio_codec, codec2str(audio_codec), maxFramesPerPacket);
				} else {
					sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %7d %-25s (skipped)\n", DEV_ID_LOG(d), audio_codec, codec2str(audio_codec));
				}

				if (audio_codec == SKINNY_CODEC_G723_1) {
					sccp_log_and((DEBUGCAT_DEVICE + DEBUGCAT_HIGH))(VERBOSE_PREFIX_3 "%s: %7s bitRate: %d\n", DEV_ID_LOG(d), "", letohl(msg_in->data.UpdateCapabilitiesMessage.v3.audioCaps[audio_capability].payloads.lel_g723BitRate));
				} else {
					sccp_log_and((DEBUGCAT_DEVICE + DEBUGCAT_HIGH))(VERBOSE_PREFIX_3 "%s: %7s codecMode: %d, dynamicPayload: %d, codecParam1: %d, codecParam2: %d\n", DEV_ID_LOG(d), "", msg_in->data.UpdateCapabilitiesMessage.v3.audioCaps[audio_capability].payloads.codecParams.codecMode, msg_in->data.UpdateCapabilitiesMessage.v3.audioCaps[audio_capability].payloads.codecParams.dynamicPayload, msg_in->data.UpdateCapabilitiesMessage.v3.audioCaps[audio_capability].payloads.codecParams.codecParam1, msg_in->data.UpdateCapabilitiesMessage.v3.audioCaps[audio_capability].payloads.codecParams.codecParam2);
				}
			}
			sccp_codec_reduceSet(d->preferences.audio , d->capabilities.audio);
		}
#ifdef CS_SCCP_VIDEO
		uint8_t video_customPictureFormat = 0;

		uint8_t video_customPictureFormats = 0;
		video_customPictureFormats = letohl(msg_in->data.UpdateCapabilitiesMessage.v3.lel_customPictureFormatCount);
		for (video_customPictureFormat = 0; video_customPictureFormat < video_customPictureFormats; video_customPictureFormat++) {
			int width = letohl(msg_in->data.UpdateCapabilitiesMessage.v3.customPictureFormat[video_customPictureFormat].lel_width);
			int height = letohl(msg_in->data.UpdateCapabilitiesMessage.v3.customPictureFormat[video_customPictureFormat].lel_height);
			int pixelAspectRatio = letohl(msg_in->data.UpdateCapabilitiesMessage.v3.customPictureFormat[video_customPictureFormat].lel_pixelAspectRatio);
			int pixelClockConversion = letohl(msg_in->data.UpdateCapabilitiesMessage.v3.customPictureFormat[video_customPictureFormat].lel_pixelclockConversionCode);
			int pixelClockDivisor = letohl(msg_in->data.UpdateCapabilitiesMessage.v3.customPictureFormat[video_customPictureFormat].lel_pixelclockDivisor);

			sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %6s %-5s customPictureFormat %d: width=%d, height=%d, pixelAspectRatio=%d, pixelClockConversion=%d, pixelClockDivisor=%d\n", DEV_ID_LOG(d), "", "", video_customPictureFormat, width, height, pixelAspectRatio, pixelClockConversion, pixelClockDivisor);
		}
		sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %6s %-5s %s\n", DEV_ID_LOG(d), "", "", "--");
		uint8_t video_capabilities = 0;

		uint8_t video_capability = 0;
		skinny_codec_t video_codec = SKINNY_CODEC_NONE;
		boolean_t previousVideoSupport = sccp_device_isVideoSupported(d);

		video_capabilities = letohl(msg_in->data.UpdateCapabilitiesMessage.v3.lel_videoCapCount);

		if (video_capabilities > 0 && video_capabilities <= SKINNY_MAX_VIDEO_CAPABILITIES) {
			sccp_log((DEBUGCAT_CORE + DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: video softkey enabled\n", DEV_ID_LOG(d));

			sccp_log((DEBUGCAT_CORE + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %d video capabilit%s\n", DEV_ID_LOG(d), video_capabilities, video_capabilities == 1 ? "y" : "ies");
			for (video_capability = 0; video_capability < video_capabilities; video_capability++) {
				video_codec = letohl(msg_in->data.UpdateCapabilitiesMessage.v3.videoCaps[video_capability].lel_payloadCapability);
				if (codec2type(video_codec) == SKINNY_CODEC_TYPE_VIDEO) {
					d->capabilities.video[video_capability] = video_codec;
#if DEBUG
					char transmitReceiveStr[5];
					snprintf(transmitReceiveStr, sizeof(transmitReceiveStr), "%c-%c", (letohl(msg_in->data.UpdateCapabilitiesMessage.v3.videoCaps[video_capability].lel_transmitOrReceive) & SKINNY_TRANSMITRECEIVE_RECEIVE) ? '<' : ' ', (letohl(msg_in->data.UpdateCapabilitiesMessage.v3.videoCaps[video_capability].lel_transmitOrReceive) & SKINNY_TRANSMITRECEIVE_TRANSMIT) ? '>' : ' ');
					sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %2d: %-3s %3d %-25s\n", DEV_ID_LOG(d), video_capability, transmitReceiveStr, video_codec, codec2str(video_codec));
					handle_updatecapabilities_dissect_videocapabiltyunion(d, video_codec, (videoCapabilityUnionV2_t *)&msg_in->data.UpdateCapabilitiesMessage.v3.videoCaps[video_capability].capability);

					uint8_t levelPreferences = letohl(msg_in->data.UpdateCapabilitiesMessage.v3.videoCaps[video_capability].lel_levelPreferenceCount);
					handle_updatecapabilities_dissect_levelPreference(d, levelPreferences, msg_in->data.UpdateCapabilitiesMessage.v3.videoCaps[video_capability].levelPreference);
#endif
				} else {
					sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %7d %-25s (skipped)\n", DEV_ID_LOG(d), video_codec, codec2str(video_codec));
				}
			}
			sccp_codec_reduceSet(d->preferences.video , d->capabilities.video);
			sccp_softkey_setSoftkeyState(d, KEYMODE_CONNTRANS, SKINNY_LBL_VIDEO_MODE, TRUE);
			sccp_softkey_setSoftkeyState(d, KEYMODE_CONNECTED, SKINNY_LBL_VIDEO_MODE, TRUE);
			if (previousVideoSupport == FALSE) {
				sccp_dev_set_message(d, "Video support enabled", 5, FALSE, FALSE);
			}
		} else {
			d->capabilities.video[0] = SKINNY_CODEC_NONE;
			sccp_softkey_setSoftkeyState(d, KEYMODE_CONNTRANS, SKINNY_LBL_VIDEO_MODE, FALSE);
			sccp_softkey_setSoftkeyState(d, KEYMODE_CONNECTED, SKINNY_LBL_VIDEO_MODE, FALSE);
			sccp_log((DEBUGCAT_CORE + DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: video softkey disabled\n", DEV_ID_LOG(d));
			if (previousVideoSupport == TRUE) {
				sccp_dev_set_message(d, "Video support disabled", 5, FALSE, FALSE);
			}
		}
#endif
		sccp_line_updateLineCapabilitiesByDevice(d);
	}
}
/*
 * Will be better to store audio codec max packet size and video bandwidth and size.
 * In future we will parse also data caps to support T.38 and NSE with ATA186/188 devices.
 */
void handle_updatecapabilities_V2_message(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	pbx_assert(d != NULL && s != NULL && msg_in != NULL);
	uint8_t audio_capability = 0;

	uint8_t audio_capabilities = 0;
	skinny_codec_t audio_codec = SKINNY_CODEC_NONE;
	uint32_t maxFramesPerPacket = 0;

	audio_capabilities = letohl(msg_in->data.UpdateCapabilitiesV2Message.lel_audioCapCount);
	int RTPPayloadFormat = letohl(msg_in->data.UpdateCapabilitiesV2Message.lel_RTPPayloadFormat);
	sccp_log((DEBUGCAT_CORE + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %d audio capabilities, RTP payload format %d (v2)\n", DEV_ID_LOG(d), audio_capabilities, RTPPayloadFormat);

	if (audio_capabilities > 0 && audio_capabilities <= SKINNY_MAX_CAPABILITIES) {
		sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %7s %-25s %-9s\n", DEV_ID_LOG(d), "#", "codec", "maxFrames");
		for (audio_capability = 0; audio_capability < audio_capabilities; audio_capability++) {
			audio_codec = letohl(msg_in->data.UpdateCapabilitiesV2Message.audioCaps[audio_capability].lel_payloadCapability);
			if (codec2type(audio_codec) == SKINNY_CODEC_TYPE_AUDIO) {
				maxFramesPerPacket = letohl(msg_in->data.UpdateCapabilitiesV2Message.audioCaps[audio_capability].lel_maxFramesPerPacket);
				d->capabilities.audio[audio_capability] = audio_codec;
				sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %7d %-25s %-6d\n", DEV_ID_LOG(d), audio_codec, codec2str(audio_codec), maxFramesPerPacket);
			} else {
				sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %7d %-25s (skipped)\n", DEV_ID_LOG(d), audio_codec, codec2str(audio_codec));
			}
			if (audio_codec == SKINNY_CODEC_G723_1) {
				sccp_log_and((DEBUGCAT_DEVICE + DEBUGCAT_HIGH))(VERBOSE_PREFIX_3 "%s: %7s bitRate: %d\n", DEV_ID_LOG(d), "", letohl(msg_in->data.UpdateCapabilitiesV2Message.audioCaps[audio_capability].payloads.lel_g723BitRate));
			} else {
				sccp_log_and((DEBUGCAT_DEVICE + DEBUGCAT_HIGH))(VERBOSE_PREFIX_3 "%s: %7s codecMode: %d, dynamicPayload: %d, codecParam1: %d, codecParam2: %d\n", DEV_ID_LOG(d), "", msg_in->data.UpdateCapabilitiesV2Message.audioCaps[audio_capability].payloads.codecParams.codecMode, msg_in->data.UpdateCapabilitiesV2Message.audioCaps[audio_capability].payloads.codecParams.dynamicPayload, msg_in->data.UpdateCapabilitiesV2Message.audioCaps[audio_capability].payloads.codecParams.codecParam1, msg_in->data.UpdateCapabilitiesV2Message.audioCaps[audio_capability].payloads.codecParams.codecParam2);
			}
		}
		sccp_codec_reduceSet(d->preferences.audio , d->capabilities.audio);
	}
#ifdef CS_SCCP_VIDEO
#if DEBUG
	uint8_t video_customPictureFormats = letohl(msg_in->data.UpdateCapabilitiesV2Message.lel_customPictureFormatCount);
	handle_updatecapabilities_dissect_customPictureFormat(d, video_customPictureFormats, msg_in->data.UpdateCapabilitiesV2Message.customPictureFormat);
#endif

	uint8_t video_capabilities = 0;

	uint8_t video_capability = 0;
	skinny_codec_t video_codec = SKINNY_CODEC_NONE;
	boolean_t previousVideoSupport = sccp_device_isVideoSupported(d);

	video_capabilities = letohl(msg_in->data.UpdateCapabilitiesV2Message.lel_videoCapCount);

	if (video_capabilities > 0 && video_capabilities <= SKINNY_MAX_VIDEO_CAPABILITIES) {
		sccp_log((DEBUGCAT_CORE + DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: video softkey enabled\n", DEV_ID_LOG(d));

		sccp_log((DEBUGCAT_CORE + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %d video capabilit%s\n", DEV_ID_LOG(d), video_capabilities, video_capabilities == 1 ? "y" : "ies");
		for (video_capability = 0; video_capability < video_capabilities; video_capability++) {
			video_codec = letohl(msg_in->data.UpdateCapabilitiesV2Message.videoCaps[video_capability].lel_payloadCapability);
			if (codec2type(video_codec) == SKINNY_CODEC_TYPE_VIDEO) {
				d->capabilities.video[video_capability] = video_codec;
#if DEBUG
				char transmitReceiveStr[5];
				snprintf(transmitReceiveStr, sizeof(transmitReceiveStr), "%c-%c", (letohl(msg_in->data.UpdateCapabilitiesV2Message.videoCaps[video_capability].lel_transmitOrReceive) & SKINNY_TRANSMITRECEIVE_RECEIVE) ? '<' : ' ', (letohl(msg_in->data.UpdateCapabilitiesV2Message.videoCaps[video_capability].lel_transmitOrReceive) & SKINNY_TRANSMITRECEIVE_TRANSMIT) ? '>' : ' ');
				sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %2d: %-3s %3d %-25s\n", DEV_ID_LOG(d), video_capability, transmitReceiveStr, video_codec, codec2str(video_codec));
				handle_updatecapabilities_dissect_videocapabiltyunion(d, video_codec, &msg_in->data.UpdateCapabilitiesV2Message.videoCaps[video_capability].capability);

				uint8_t levelPreferences = letohl(msg_in->data.UpdateCapabilitiesV2Message.videoCaps[video_capability].lel_levelPreferenceCount);
				handle_updatecapabilities_dissect_levelPreference(d, levelPreferences, msg_in->data.UpdateCapabilitiesV2Message.videoCaps[video_capability].levelPreference);
#endif
			} else {
				sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %7d %-25s (skipped)\n", DEV_ID_LOG(d), video_codec, codec2str(video_codec));
			}
		}
		sccp_codec_reduceSet(d->preferences.video , d->capabilities.video);
		sccp_softkey_setSoftkeyState(d, KEYMODE_CONNTRANS, SKINNY_LBL_VIDEO_MODE, TRUE);
		sccp_softkey_setSoftkeyState(d, KEYMODE_CONNECTED, SKINNY_LBL_VIDEO_MODE, TRUE);
		if (previousVideoSupport == FALSE) {
			sccp_dev_set_message(d, "Video support enabled", 5, FALSE, FALSE);
		}
	} else {
		d->capabilities.video[0] = SKINNY_CODEC_NONE;
		sccp_softkey_setSoftkeyState(d, KEYMODE_CONNTRANS, SKINNY_LBL_VIDEO_MODE, FALSE);
		sccp_softkey_setSoftkeyState(d, KEYMODE_CONNECTED, SKINNY_LBL_VIDEO_MODE, FALSE);
		sccp_log((DEBUGCAT_CORE + DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: video softkey disabled\n", DEV_ID_LOG(d));
		if (previousVideoSupport == TRUE) {
			sccp_dev_set_message(d, "Video support disabled", 5, FALSE, FALSE);
		}
	}
#endif
	sccp_line_updateLineCapabilitiesByDevice(d);
}

/*
 * Will be better to store audio codec max packet size and video bandwidth and size.
 * In future we will parse also data caps to support T.38 and NSE with ATA186/188 devices.
 */
void handle_updatecapabilities_V3_message(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	pbx_assert(d != NULL && s != NULL && msg_in != NULL);
	uint8_t audio_capability = 0;

	uint8_t audio_capabilities = 0;
	skinny_codec_t audio_codec = SKINNY_CODEC_NONE;
	uint32_t maxFramesPerPacket = 0;

	audio_capabilities = letohl(msg_in->data.UpdateCapabilitiesV3Message.lel_audioCapCount);
	int RTPPayloadFormat = letohl(msg_in->data.UpdateCapabilitiesV3Message.lel_RTPPayloadFormat);
	sccp_log((DEBUGCAT_CORE + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %d audio capabilities, RTP payload format %d (v3)\n", DEV_ID_LOG(d), audio_capabilities, RTPPayloadFormat);

	if (audio_capabilities > 0 && audio_capabilities <= SKINNY_MAX_CAPABILITIES) {
		sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %7s %-25s %-9s\n", DEV_ID_LOG(d), "#", "codec", "maxFrames");
		for (audio_capability = 0; audio_capability < audio_capabilities; audio_capability++) {
			audio_codec = letohl(msg_in->data.UpdateCapabilitiesV3Message.audioCaps[audio_capability].lel_payloadCapability);
			if (codec2type(audio_codec) == SKINNY_CODEC_TYPE_AUDIO) {
				maxFramesPerPacket = letohl(msg_in->data.UpdateCapabilitiesV3Message.audioCaps[audio_capability].lel_maxFramesPerPacket);
				d->capabilities.audio[audio_capability] = audio_codec;
				sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %7d %-25s %-6d\n", DEV_ID_LOG(d), audio_codec, codec2str(audio_codec), maxFramesPerPacket);
			} else {
				sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %7d %-25s (skipped)\n", DEV_ID_LOG(d), audio_codec, codec2str(audio_codec));
			}
			if (audio_codec == SKINNY_CODEC_G723_1) {
				sccp_log_and((DEBUGCAT_DEVICE + DEBUGCAT_HIGH))(VERBOSE_PREFIX_3 "%s: %7s bitRate: %d\n", DEV_ID_LOG(d), "", letohl(msg_in->data.UpdateCapabilitiesV3Message.audioCaps[audio_capability].payloads.lel_g723BitRate));
			} else {
				sccp_log_and((DEBUGCAT_DEVICE + DEBUGCAT_HIGH))(VERBOSE_PREFIX_3 "%s: %7s codecMode: %d, dynamicPayload: %d, codecParam1: %d, codecParam2: %d\n", DEV_ID_LOG(d), "", msg_in->data.UpdateCapabilitiesV3Message.audioCaps[audio_capability].payloads.codecParams.codecMode, msg_in->data.UpdateCapabilitiesV3Message.audioCaps[audio_capability].payloads.codecParams.dynamicPayload, msg_in->data.UpdateCapabilitiesV3Message.audioCaps[audio_capability].payloads.codecParams.codecParam1, msg_in->data.UpdateCapabilitiesV3Message.audioCaps[audio_capability].payloads.codecParams.codecParam2);
			}
		}
		sccp_codec_reduceSet(d->preferences.audio , d->capabilities.audio);
	}

#ifdef CS_SCCP_VIDEO
#if DEBUG
	uint8_t video_customPictureFormats = letohl(msg_in->data.UpdateCapabilitiesV2Message.lel_customPictureFormatCount);
	handle_updatecapabilities_dissect_customPictureFormat(d, video_customPictureFormats, msg_in->data.UpdateCapabilitiesV3Message.customPictureFormat);
#endif

	uint8_t video_capabilities = 0;

	uint8_t video_capability = 0;
	skinny_codec_t video_codec = SKINNY_CODEC_NONE;
	boolean_t previousVideoSupport = sccp_device_isVideoSupported(d);

	video_capabilities = letohl(msg_in->data.UpdateCapabilitiesV3Message.lel_videoCapCount);

	if (video_capabilities > 0 && video_capabilities <= SKINNY_MAX_VIDEO_CAPABILITIES) {
		sccp_log((DEBUGCAT_CORE + DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: video softkey enabled\n", DEV_ID_LOG(d));

		sccp_log((DEBUGCAT_CORE + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %d video capabilit%s\n", DEV_ID_LOG(d), video_capabilities, video_capabilities == 1 ? "y" : "ies");
		for (video_capability = 0; video_capability < video_capabilities; video_capability++) {
			video_codec = letohl(msg_in->data.UpdateCapabilitiesV3Message.videoCaps[video_capability].lel_payloadCapability);
			if (codec2type(video_codec) == SKINNY_CODEC_TYPE_VIDEO) {
				d->capabilities.video[video_capability] = video_codec;
#if DEBUG
				char transmitReceiveStr[5];
				snprintf(transmitReceiveStr, sizeof(transmitReceiveStr), "%c-%c", (letohl(msg_in->data.UpdateCapabilitiesV3Message.videoCaps[video_capability].lel_transmitOrReceive) & SKINNY_TRANSMITRECEIVE_RECEIVE) ? '<' : ' ', (letohl(msg_in->data.UpdateCapabilitiesV3Message.videoCaps[video_capability].lel_transmitOrReceive) & SKINNY_TRANSMITRECEIVE_TRANSMIT) ? '>' : ' ');
				sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %2d: %-3s %3d %-25s\n", DEV_ID_LOG(d), video_capability, transmitReceiveStr, video_codec, codec2str(video_codec));
				handle_updatecapabilities_dissect_videocapabiltyunion(d, video_codec, &msg_in->data.UpdateCapabilitiesV3Message.videoCaps[video_capability].capability);

				uint8_t levelPreferences = letohl(msg_in->data.UpdateCapabilitiesV3Message.videoCaps[video_capability].lel_levelPreferenceCount);
				handle_updatecapabilities_dissect_levelPreference(d, levelPreferences, msg_in->data.UpdateCapabilitiesV3Message.videoCaps[video_capability].levelPreference);

				int encryptionCapability = letohl(msg_in->data.UpdateCapabilitiesV3Message.videoCaps[video_capability].lel_encryptionCapability);
				sccp_log((DEBUGCAT_CORE + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: encryption capability: %s\n", DEV_ID_LOG(d), encryptionCapability ? "yes" : "no");

				int ipv46 = letohl(msg_in->data.UpdateCapabilitiesV3Message.videoCaps[video_capability].lel_ipv46);
				sccp_log((DEBUGCAT_CORE + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: IP addressing: %s\n", DEV_ID_LOG(d), ipv46 == 0 ? "IPv4" : ipv46 == 1 ? "IPv6" : "Mixed Mode");
#endif
			} else {
				sccp_log((DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: %7d %-25s (skipped)\n", DEV_ID_LOG(d), video_codec, codec2str(video_codec));
			}
		}
		sccp_codec_reduceSet(d->preferences.video , d->capabilities.video);
		sccp_softkey_setSoftkeyState(d, KEYMODE_CONNTRANS, SKINNY_LBL_VIDEO_MODE, TRUE);
		sccp_softkey_setSoftkeyState(d, KEYMODE_CONNECTED, SKINNY_LBL_VIDEO_MODE, TRUE);
		if (previousVideoSupport == FALSE) {
			sccp_dev_set_message(d, "Video support enabled", 5, FALSE, FALSE);
		}
	} else {
		d->capabilities.video[0] = SKINNY_CODEC_NONE;
		sccp_softkey_setSoftkeyState(d, KEYMODE_CONNTRANS, SKINNY_LBL_VIDEO_MODE, FALSE);
		sccp_softkey_setSoftkeyState(d, KEYMODE_CONNECTED, SKINNY_LBL_VIDEO_MODE, FALSE);
		sccp_log((DEBUGCAT_CORE + DEBUGCAT_SOFTKEY)) (VERBOSE_PREFIX_3 "%s: video softkey disabled\n", DEV_ID_LOG(d));
		if (previousVideoSupport == TRUE) {
			sccp_dev_set_message(d, "Video support disabled", 5, FALSE, FALSE);
		}
	}
#endif
	sccp_line_updateLineCapabilitiesByDevice(d);
}

void handle_KeepAliveMessage(constSessionPtr s, devicePtr maybe_d, constMessagePtr msg_in)
{
	sccp_msg_t *msg_out = sccp_build_packet(KeepAliveAckMessage, 0);
	if (!msg_out) {
		return;
	}
	sccp_session_send2(s, msg_out);
}

void handle_extension_devicecaps(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	uint32_t instance = letohl(msg_in->data.ExtensionDeviceCaps.lel_instance);
	uint32_t type = letohl(msg_in->data.ExtensionDeviceCaps.lel_type);
	uint32_t maxAllowed = letohl(msg_in->data.ExtensionDeviceCaps.lel_maxAllowed);
	const char * text = msg_in->data.ExtensionDeviceCaps.text;

	sccp_log(DEBUGCAT_ACTION + DEBUGCAT_DEVICE)(VERBOSE_PREFIX_3 "%s: add-on: instance %d, type %d, max allowed %d\n", d->id, instance, type, maxAllowed);
	sccp_log(DEBUGCAT_ACTION + DEBUGCAT_DEVICE)(VERBOSE_PREFIX_3 "%s: add-on text '%s'\n", d->id, text);
	SCCP_LIST_LOCK(&d->addons);
	if (SCCP_LIST_GETSIZE(&d->addons) < instance) {
		pbx_log(LOG_NOTICE, "%s: phone reports expansion module %d, which has no addon= entry in its sccp.conf device section; added from the phone's report for this registration\n", d->id, instance);
		sccp_addon_t *addon = (sccp_addon_t *)sccp_calloc(1, sizeof(sccp_addon_t));
		if (!addon) {
			pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
			return;
		}
		addon->type = SKINNY_DEVICETYPE_UNDEFINED;
		if (sccp_session_getProtocol(s) == SCCP_PROTOCOL) {
			switch(type) {
				case 1:
					addon->type = SKINNY_DEVICETYPE_CISCO_ADDON_7914;
					break;
				case 2:
					addon->type = SKINNY_DEVICETYPE_CISCO_ADDON_7915_24BUTTON;
					break;
				case 3:
					addon->type = SKINNY_DEVICETYPE_CISCO_ADDON_7916_24BUTTON;
					break;
				default:
					addon->type = SKINNY_DEVICETYPE_UNDEFINED;
					break;
			}
		}
		SCCP_LIST_INSERT_TAIL(&d->addons, addon, list);
	}
	SCCP_LIST_UNLOCK(&d->addons);
}

void handle_device_to_user(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	uint32_t appID = 0;
	uint32_t callReference = 0;
	uint32_t lineInstance = 0;
	uint32_t transactionID = 0;
	uint32_t dataLength = 0;
	char data[StationMaxXMLMessage] = "";

#ifdef CS_SCCP_CONFERENCE
	uint32_t conferenceID = 0;
	uint32_t participantID = 0;
#endif

	appID = letohl(msg_in->data.DeviceToUserDataVersion1Message.lel_appID);
	callReference = letohl(msg_in->data.DeviceToUserDataVersion1Message.lel_callReference);
	lineInstance = letohl(msg_in->data.DeviceToUserDataVersion1Message.lel_lineInstance);
	transactionID = letohl(msg_in->data.DeviceToUserDataVersion1Message.lel_transactionID);

	dataLength = letohl(msg_in->data.DeviceToUserDataVersion1Message.lel_dataLength);
	if (dataLength) {
		memset(data, 0, dataLength);
		memcpy(data, msg_in->data.DeviceToUserDataVersion1Message.data, dataLength);
	}

	if (lineInstance == 0 && callReference == 0) {
		if (dataLength) {
			char str_action[11] = "";

			char str_transactionID[11] = "";
			if (sscanf(data, "%10[^/]/%10s", str_action, str_transactionID) > 0) {
				sccp_log((DEBUGCAT_CONFERENCE + DEBUGCAT_MESSAGE + DEBUGCAT_ACTION)) (VERBOSE_PREFIX_3 "%s: device-to-user softkey %s, %s\n", d->id, str_action, str_transactionID);
				d->dtu_softkey.action = pbx_strdup(str_action);
				d->dtu_softkey.transactionID = sccp_atoi(str_transactionID, sizeof(str_transactionID));
			} else {
				pbx_log(LOG_NOTICE, "%s: could not parse softkey application data '%s' (expected action/transaction number); ignored\n", d->id, data);
			}
		}
	} else {
		sccp_log((DEBUGCAT_ACTION + DEBUGCAT_MESSAGE + DEBUGCAT_DEVICE + DEBUGCAT_CONFERENCE)) (VERBOSE_PREFIX_3 "%s: device-to-user data for app %d: '%s' (%d bytes)\n", d->id, appID, data, dataLength);
		switch (appID) {
			case APPID_CONFERENCE:
#ifdef CS_SCCP_CONFERENCE
				conferenceID = lineInstance;
				participantID = sccp_atoi(data, sizeof(data));
				sccp_conference_handle_device_to_user(d, callReference, transactionID, conferenceID, participantID);
#endif
				break;
			case APPID_CONFERENCE_INVITE:
#ifdef CS_SCCP_CONFERENCE
				conferenceID = lineInstance;
				participantID = sccp_atoi(data, sizeof(data));
#endif
				break;
			case APPID_VISUALPARKINGLOT:
#ifdef CS_SCCP_PARK
				{
					char parkinglot[11] = "";

					char slot_exten[11] = "";
					if (sscanf(data, "%10[^/]/%10s", parkinglot, slot_exten) > 0) {
						iParkingLot.handleDevice2User(parkinglot, d, slot_exten, lineInstance, transactionID);
					}
				}
#endif
				break;
			case APPID_PROVISION:
				break;
			case APPID_INPUT:
				sccp_log((DEBUGCAT_ACTION))(VERBOSE_PREFIX_3 "%s: input application data ignored (appid %d, call %d, line %d, transaction %d, %d bytes)\n", d->id, appID, callReference, lineInstance, transactionID, dataLength);
				break;
		}
	}
}

void handle_device_to_user_response(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	if ((GLOB(debug) & DEBUGCAT_MESSAGE) != 0) {
		uint32_t appID = 0;
		uint32_t lineInstance = 0;
		uint32_t callReference = 0;
		uint32_t transactionID = 0;
		uint32_t dataLength = 0;
		char data[StationMaxXMLMessage] = { 0 };

		appID = letohl(msg_in->data.DeviceToUserDataVersion1Message.lel_appID);
		lineInstance = letohl(msg_in->data.DeviceToUserDataVersion1Message.lel_lineInstance);
		callReference = letohl(msg_in->data.DeviceToUserDataVersion1Message.lel_callReference);
		transactionID = letohl(msg_in->data.DeviceToUserDataVersion1Message.lel_transactionID);
		dataLength = letohl(msg_in->data.DeviceToUserDataVersion1Message.lel_dataLength) + 1;

		if (dataLength) {
			sccp_copy_string(data, msg_in->data.DeviceToUserDataVersion1Message.data, dataLength);
		}

		sccp_log((DEBUGCAT_ACTION + DEBUGCAT_MESSAGE)) (VERBOSE_PREFIX_3 "%s: device-to-user response: app %d, line instance %d, call %d, transaction %d\n", d->id, appID, lineInstance, callReference, transactionID);
		sccp_log((DEBUGCAT_ACTION + DEBUGCAT_MESSAGE + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: device-to-user response data:\n%s\n", d->id, data);

		if (appID == APPID_DEVICECAPABILITIES) {
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: device capabilities response '%s'\n", d->id, data);
		}
	}
}

void handle_miscellaneousCommandMessage(constSessionPtr s, devicePtr d, constMessagePtr msg_in)
{
	skinny_miscCommandType_t commandType = 0;
	uint32_t conferenceId = letohl(msg_in->data.MiscellaneousCommandMessage.lel_conferenceId);
	uint32_t callReference = letohl(msg_in->data.MiscellaneousCommandMessage.lel_callReference);
	uint32_t passThruPartyId = letohl(msg_in->data.MiscellaneousCommandMessage.lel_passThruPartyId);
	commandType = letohl(msg_in->data.MiscellaneousCommandMessage.lel_miscCommandType);

	AUTO_RELEASE(sccp_channel_t, channel , __get_channel_from_callReference_or_passThruParty(d, conferenceId, callReference, passThruPartyId));
	if (channel) {
		switch (commandType) {
			case SKINNY_MISCCOMMANDTYPE_VIDEOFREEZEPICTURE:
				break;
			case SKINNY_MISCCOMMANDTYPE_VIDEOFASTUPDATEPICTURE:
				iPbx.queue_control(channel->owner, AST_CONTROL_VIDUPDATE);
				break;
			case SKINNY_MISCCOMMANDTYPE_VIDEOFASTUPDATEGOB:
				sccp_log((DEBUGCAT_RTP)) (VERBOSE_PREFIX_3 "%s: video fast update GOB: first %d, count %d\n",
							  channel ? channel->currentDeviceId : "--",
							  letohl(msg_in->data.MiscellaneousCommandMessage.data.videoFastUpdateGOB.lel_firstGOB),
							  letohl(msg_in->data.MiscellaneousCommandMessage.data.videoFastUpdateGOB.lel_numberOfGOBs)
				    );
				break;
			case SKINNY_MISCCOMMANDTYPE_VIDEOFASTUPDATEMB:
				sccp_log((DEBUGCAT_RTP)) (VERBOSE_PREFIX_3 "%s: video fast update MB: first GOB %d, first MB %d, count %d\n",
							  channel ? channel->currentDeviceId : "--",
							  letohl(msg_in->data.MiscellaneousCommandMessage.data.videoFastUpdateMB.lel_firstGOB),
							  letohl(msg_in->data.MiscellaneousCommandMessage.data.videoFastUpdateMB.lel_firstMB),
							  letohl(msg_in->data.MiscellaneousCommandMessage.data.videoFastUpdateMB.lel_numberOfMBs)
				    );
				break;
			case SKINNY_MISCCOMMANDTYPE_LOSTPICTURE:
				sccp_log((DEBUGCAT_RTP)) (VERBOSE_PREFIX_3 "%s: lost picture: picture %d, long-term index %d\n",
							  channel ? channel->currentDeviceId : "--",
							  letohl(msg_in->data.MiscellaneousCommandMessage.data.lostPicture.lel_pictureNumber),
							  letohl(msg_in->data.MiscellaneousCommandMessage.data.lostPicture.lel_longTermPictureIndex)
				    );
				break;
			case SKINNY_MISCCOMMANDTYPE_LOSTPARTIALPICTURE:
				sccp_log((DEBUGCAT_RTP)) (VERBOSE_PREFIX_3 "%s: lost partial picture: picture %d, long-term index %d, first MB %d, count %d\n",
							  channel ? channel->currentDeviceId : "--",
							  letohl(msg_in->data.MiscellaneousCommandMessage.data.lostPartialPicture.pictureReference.lel_pictureNumber),
							  letohl(msg_in->data.MiscellaneousCommandMessage.data.lostPartialPicture.pictureReference.lel_longTermPictureIndex),
							  letohl(msg_in->data.MiscellaneousCommandMessage.data.lostPartialPicture.lel_firstMB),
							  letohl(msg_in->data.MiscellaneousCommandMessage.data.lostPartialPicture.lel_numberOfMBs)
				    );
				break;
			case SKINNY_MISCCOMMANDTYPE_RECOVERYREFERENCEPICTURE:
				{
					int curPic = 0;
					int pictureCount = letohl(msg_in->data.MiscellaneousCommandMessage.data.recoveryReferencePicture.lel_PictureCount);
					sccp_log((DEBUGCAT_RTP)) (VERBOSE_PREFIX_3 "%s: recovery reference pictures: %d\n",
								  channel ? channel->currentDeviceId : "--",
								  pictureCount);
					for (curPic = 0; curPic < pictureCount; curPic++) {
						sccp_log((DEBUGCAT_RTP)) (VERBOSE_PREFIX_3 "%s: recovery reference picture %d: picture %d, long-term index %d\n",
									channel ? channel->currentDeviceId : "--", curPic,
									letohl(msg_in->data.MiscellaneousCommandMessage.data.recoveryReferencePicture.pictureReference[curPic].lel_pictureNumber),
									letohl(msg_in->data.MiscellaneousCommandMessage.data.recoveryReferencePicture.pictureReference[curPic].lel_longTermPictureIndex)
						    );
					}
				}
				break;
			case SKINNY_MISCCOMMANDTYPE_TEMPORALSPATIALTRADEOFF:
				sccp_log((DEBUGCAT_RTP)) (VERBOSE_PREFIX_3 "%s: recovery reference: temporal/spatial trade-off %d\n",
							  channel ? channel->currentDeviceId : "--",
							   letohl(msg_in->data.MiscellaneousCommandMessage.data.lel_temporalSpatialTradeOff));
				break;
			default:
				break;
		}
		if (channel->owner) {
			iPbx.queue_control(channel->owner, AST_CONTROL_VIDUPDATE);
		}
	}
}
