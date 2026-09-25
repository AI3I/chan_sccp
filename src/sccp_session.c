/*!
 * \file	sccp_session.c
 * \brief       SCCP Session Class
 * \author      Sergio Chersovani <mlists [at] c-net.it>
 * \note	Reworked, but based on chan_sccp code.
 *		The original chan_sccp driver that was made by Zozo which itself was derived from the chan_skinny driver.
 *		Modified by Jan Czmok and Julien Goodwin
 * \note	This program is free software and may be modified and distributed under the terms of the GNU General Public License, version 2 or (at your option) any later version.
 *		See the LICENSE file at the top of the source tree.
 */
#include "config.h"
#include "common.h"
#include "sccp_session.h"

SCCP_FILE_VERSION(__FILE__, "");

#include "sccp_actions.h"
#include "sccp_cli.h"
#include "sccp_device.h"
#include "sccp_netsock.h"
#include "sccp_utils.h"
#include "sccp_transport.h"
#include <netinet/in.h>
#include <sys/un.h>

#ifndef CS_USE_POLL_COMPAT
#include <poll.h>
#include <sys/poll.h>
#else
#define AST_POLL_COMPAT 1
#include <asterisk/poll-compat.h>
#endif
#ifdef pbx_poll
#define sccp_netsock_poll pbx_poll
#else
#define sccp_netsock_poll poll
#endif
#ifdef HAVE_PBX_ACL_H
#  include <asterisk/acl.h>
#endif
#include <asterisk/cli.h>
#include <signal.h>

#define WRITE_BACKOFF 500
#define SESSION_DEVICE_CLEANUP_TIME 10										/* wait time before destroying a device on thread exit */
#define KEEPALIVE_ADDITIONAL_PERCENT_SESSION 1.05
#define KEEPALIVE_ADDITIONAL_PERCENT_DEVICE 1.20
#define KEEPALIVE_ADDITIONAL_PERCENT_ON_CALL 2.00
#define SESSION_REQUEST_TIMEOUT              5

#define sccp_session_lock(x)			pbx_mutex_lock(&(x)->lock)
#define sccp_session_unlock(x)			pbx_mutex_unlock(&(x)->lock)
#define sccp_session_trylock(x)			pbx_mutex_trylock(&(x)->lock)
#define SCOPED_SESSION(x)                       SCOPED_MUTEX(sessionlock, (ast_mutex_t *)&(x)->lock);

void sccp_session_device_thread_exit(void *session);
void *sccp_session_device_thread(void *session);
void __sccp_session_stopthread(sessionPtr session, skinny_registrationstate_t newRegistrationState);
gcc_inline void recalc_wait_time(sccp_session_t *s);
static struct ast_sockaddr internip;

struct sccp_servercontext {
	sccp_servercontexttype_t type;
	const sccp_transport_t * transport;
	struct sockaddr_storage boundaddr;
	pthread_t accept_tid;
	sccp_socket_connection_t sc;
	boolean_t (*bind_and_listen)(sccp_servercontext_t * context, struct sockaddr_storage * bindaddr);
	int (*stopListening)(sccp_servercontext_t * context);
};

sccp_servercontext_t * sccp_servercontext_create(struct sockaddr_storage * bindaddr, sccp_servercontexttype_t type)
{
	sccp_servercontext_t * context = NULL;
	if(!(context = (sccp_servercontext_t *)sccp_calloc(sizeof *context, 1))) {
		pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
		return NULL;
	}
	context->type = type;
	switch(type) {
		case SCCP_SERVERCONTEXT_TCP:
			if((context->transport = tcp_init()) == NULL) {
				pbx_log(LOG_ERROR, "SCCP: TCP listener not created: the TCP transport could not be initialized\n");
				sccp_free(context);
				return NULL;
			}
			break;
#ifdef HAVE_LIBSSL
		case SCCP_SERVERCONTEXT_TLS:
			if((context->transport = tls_init()) == NULL) {
				sccp_log(DEBUGCAT_SOCKET)(VERBOSE_PREFIX_2 "SCCP: TLS context could not be initialized\n");
				sccp_free(context);
				return NULL;
			}
			break;
#endif
	}
	context->bind_and_listen = sccp_session_bind_and_listen;
	context->stopListening = sccp_servercontext_stopListening;
	context->sc.fd = -1;
	context->accept_tid = AST_PTHREADT_NULL;
	if (!sccp_servercontext_reload(context, bindaddr)) {
		context->transport->destroy(1);
		sccp_free(context);
		return NULL;
	}
	return context;
}

int sccp_servercontext_stopListening(sccp_servercontext_t * context)
{
	if(context) {
		sccp_session_stop_accept_thread(context);
		return 0;
	}
	return 1;
}

int sccp_servercontext_destroy(sccp_servercontext_t * context)
{
	if(context) {
		sccp_session_stop_accept_thread(context);
		context->transport->destroy(1);
		sccp_free(context);
		return 0;
	}
	return 1;
}

int sccp_servercontext_reload(sccp_servercontext_t * context, struct sockaddr_storage * bindaddr)
{
	if(context->sc.fd > -1 && (sccp_netsock_getPort(&context->boundaddr) != sccp_netsock_getPort(bindaddr) || sccp_netsock_cmp_addr(&context->boundaddr, bindaddr))) {
		sccp_session_stop_accept_thread(context);
	}
	return context->bind_and_listen(context, bindaddr);
}

const struct sockaddr_storage * const sccp_servercontext_getBoundAddr(sccp_servercontext_t * context)
{
	return context ? &context->boundaddr : NULL;
}

struct sccp_session {
	sccp_servercontext_t * srvcontext;
	time_t lastKeepAlive;
	uint16_t keepAlive;
	uint16_t keepAliveInterval;
	SCCP_RWLIST_ENTRY (sccp_session_t) list;
	sccp_device_t *device;
	sccp_socket_connection_t sc;
	struct sockaddr_storage sin;
	uint32_t protocolType;
	volatile boolean_t session_stop;
	sccp_mutex_t write_lock;										/*!< Prevent multiple threads writing to the socket at the same time */
	sccp_mutex_t send_lock;										/*!< Protect in-flight sends until teardown */
	pbx_cond_t sends_drained;
	unsigned int active_sends;
	sccp_mutex_t lock;
	pthread_t session_thread;
	struct sockaddr_storage ourip;
	struct sockaddr_storage ourIPv4;
	char designator[40];
	uint16_t requestsInFlight;
	pbx_cond_t pendingRequest;
};

int sccp_session_getFD(sccp_session_t * s)
{
	sccp_session_lock(s);
	int res = s->sc.fd;
	sccp_session_unlock(s);
	return res;
}

void sccp_session_setFD(sccp_session_t * s, int fd)
{
	sccp_session_lock(s);
	if(s->sc.fd > 0) {
		s->srvcontext->transport->shutdown(&s->sc, SHUT_RDWR);
		s->srvcontext->transport->close(&s->sc);
		s->sc.fd = -1;
	}
	s->sc.fd = fd;
	sccp_session_unlock(s);
}

ssl_t * sccp_session_getSSL(sccp_session_t * s)
{
	ssl_t * res = NULL;
	sccp_session_lock(s);
	res = s->sc.ssl;
	sccp_session_unlock(s);
	return res;
}

void sccp_session_setSSL(sccp_session_t * s, ssl_t * ssl)
{
	sccp_session_lock(s);
	s->sc.ssl = ssl;
	sccp_session_unlock(s);
}

boolean_t sccp_session_getOurIP(constSessionPtr session, struct sockaddr_storage * const sockAddrStorage, int family)
{
	if (session && sockAddrStorage) {
		if (!sccp_netsock_is_any_addr(&session->ourip)) {
			switch (family) {
				case 0:
					memcpy(sockAddrStorage, &session->ourip, sizeof(struct sockaddr_storage));
					break;
				case AF_INET:
					((struct sockaddr_in *) sockAddrStorage)->sin_addr = ((struct sockaddr_in *) &session->ourip)->sin_addr;
					break;
				case AF_INET6:
					((struct sockaddr_in6 *) sockAddrStorage)->sin6_addr = ((struct sockaddr_in6 *) &session->ourip)->sin6_addr;
					break;
			}
			return TRUE;
		}
	}
	return FALSE;
}

boolean_t sccp_session_getSas(constSessionPtr session, struct sockaddr_storage * const sockAddrStorage)
{
	if (session && sockAddrStorage) {
		memcpy(sockAddrStorage, &session->sin, sizeof(struct sockaddr_storage));
		return TRUE;
	}
	return FALSE;
}

gcc_inline int sccp_session_getClientPort(constSessionPtr session)
{
	if(session) {
		return sccp_netsock_getPort(&session->sin);
	}
	return 0;
}

int sccp_session_setOurIP4Address(constSessionPtr session, const struct sockaddr_storage * them)
{
	sessionPtr s = (sessionPtr)session;
	struct sockaddr_storage us = { 0 };
	sccp_log(DEBUGCAT_SOCKET)(VERBOSE_PREFIX_3 "SCCP: client %s\n", sccp_netsock_stringify(them));

	memcpy(&us, &internip.ss, sizeof(struct sockaddr_storage));

	if(s && sccp_netsock_ouraddrfor(them, &us)) {
		memcpy(&s->ourIPv4, &us, sizeof(struct sockaddr_storage));
		sccp_log(DEBUGCAT_SOCKET)(VERBOSE_PREFIX_3 "SCCP: best local address to reach it: %s\n", sccp_netsock_stringify(&s->ourIPv4));
		return 0;
	}
	return -2;
}

int sccp_session_waitForPendingRequests(sccp_session_t * s)
{
	struct timeval relative_timeout = {
		SESSION_REQUEST_TIMEOUT,
	};
	struct timeval absolute_timeout = ast_tvadd(ast_tvnow(), relative_timeout);
	struct timespec timeout_spec = {
		.tv_sec = absolute_timeout.tv_sec,
		.tv_nsec = absolute_timeout.tv_usec * 1000,
	};

	SCOPED_SESSION(s);
	while(s->requestsInFlight) {
		sccp_log(DEBUGCAT_SOCKET)(VERBOSE_PREFIX_3 "%s: waiting for %d pending requests\n", s->designator, s->requestsInFlight);
		if(pbx_cond_timedwait(&s->pendingRequest, &s->lock, &timeout_spec) == ETIMEDOUT) {
			pbx_log(LOG_WARNING, "%s: the phone did not answer %d outstanding request(s) in time; continuing without the answers\n", s->designator, s->requestsInFlight);
			s->requestsInFlight = 0;
			return s->requestsInFlight;
		}
	}
	return 0;
}

uint16_t sccp_session_getPendingRequests(sccp_session_t * s)
{
	SCOPED_SESSION(s);
	return s->requestsInFlight;
}

static void request_pending(sccp_session_t * s)
{
	SCOPED_SESSION(s);
	s->requestsInFlight++;
}

static void response_received(sccp_session_t * s)
{
	SCOPED_SESSION(s);
	if(s->requestsInFlight) {
		s->requestsInFlight--;
	}
	pbx_cond_broadcast(&s->pendingRequest);
}

static void socket_get_error(constSessionPtr s, const char * file, int line, const char * function)
{
	if (errno) {
		if (errno == ECONNRESET) {
			sccp_log(DEBUGCAT_SOCKET)(VERBOSE_PREFIX_3 "%s: connection reset by the phone\n", DEV_ID_LOG(s->device));
		} else {
			sccp_log(DEBUGCAT_SOCKET)(VERBOSE_PREFIX_3 "%s (%s:%d:%s): socket error: %s (%d)\n", DEV_ID_LOG(s->device), file, line, function, strerror(errno), errno);
		}
	} else {
		if(!s || s->sc.fd <= 0) {
			return;
		}
		int mysocket = s->sc.fd;
		int error = 0;
		socklen_t error_len = sizeof(error);
		if ((mysocket && getsockopt(mysocket, SOL_SOCKET, SO_ERROR, &error, &error_len) == 0) && error != 0) {
			sccp_log(DEBUGCAT_SOCKET)(VERBOSE_PREFIX_3 "%s: (%s:%d:%s) socket error (SO_ERROR): %s (%d)\n", DEV_ID_LOG(s->device), file, line, function, strerror(error), error);
		}
	}
}

static int session_dissect_header(sccp_session_t * s, sccp_header_t * header, struct messageinfo ** msgInfoPtr)
{
	int result = -1;
	struct messageinfo * msginfo = *msgInfoPtr;
	unsigned int packetSize = header->length = letohl(header->length);
	int protocolVersion = letohl(header->lel_protocolVer);
	sccp_mid_t messageId = letohl(header->lel_messageId);
	do {
		if (packetSize < 4 || packetSize > SCCP_MAX_PACKET - 8) {
			pbx_log(LOG_ERROR, "%s: received a packet with payload length %u (message 0x%04X, protocol %u); valid lengths are %d-%d, so the connection is closed\n", DEV_ID_LOG(s->device), packetSize, messageId, protocolVersion, 4, (int) (SCCP_MAX_PACKET - 8));
			return -2;
		}

		if (protocolVersion > 0 && !(sccp_protocol_isProtocolSupported(s->protocolType, protocolVersion))) {
			pbx_log(LOG_WARNING, "%s: received a message with protocol version %u, which this session's protocol does not support; message discarded\n", DEV_ID_LOG(s->device), protocolVersion);
			break;
		}

		if((msginfo = lookupMsgInfoStruct(messageId))) {
			if(msginfo->messageId != messageId) {
				pbx_log(LOG_WARNING, "%s: received unknown message ID 0x%04X (closest table entry 0x%04X); message discarded\n", DEV_ID_LOG(s->device), messageId, msginfo->messageId);
				break;
			}
			result = msginfo->size + SCCP_PACKET_HEADER;
			*msgInfoPtr = msginfo;
		}
	} while (0);

	return result;
}

static gcc_inline int session_buffer2msg(sccp_session_t * s, const unsigned char * const buffer, int lenAccordingToPacketHeader, sccp_msg_t * msg)
{
	int res = -5;
	sccp_header_t msg_header = {0};
	struct messageinfo * msginfo = NULL;
	memcpy(&msg_header, buffer, SCCP_PACKET_HEADER);

	int lenAccordingToOurProtocolSpec = session_dissect_header(s, &msg_header, &msginfo);
	if (dont_expect(lenAccordingToOurProtocolSpec < 0)) {
		if (lenAccordingToOurProtocolSpec == -2) {
			return 0;
		}
		lenAccordingToOurProtocolSpec = 0;
	}
	if (dont_expect(lenAccordingToPacketHeader > lenAccordingToOurProtocolSpec)) {					// show out discarded bytes
		pbx_log(LOG_WARNING, "%s: received a %d-byte message where %d bytes are known; the extra bytes are ignored (packet dump follows)\n", DEV_ID_LOG(s->device), lenAccordingToPacketHeader, lenAccordingToOurProtocolSpec);
		sccp_dump_packet(buffer, lenAccordingToPacketHeader);
	}

	if (((unsigned int)lenAccordingToPacketHeader) < ((unsigned int)lenAccordingToOurProtocolSpec)){
		sccp_log_and((DEBUGCAT_SOCKET + DEBUGCAT_MESSAGE)) (VERBOSE_PREFIX_3 "%s: message is shorter (%d) than its known size (%d)\n", DEV_ID_LOG(s->device), lenAccordingToPacketHeader, lenAccordingToOurProtocolSpec);
		lenAccordingToOurProtocolSpec = lenAccordingToPacketHeader;
	}

	memset(msg, 0, SCCP_MAX_PACKET);
	memcpy(msg, buffer, lenAccordingToOurProtocolSpec);
	msg->header.length = lenAccordingToOurProtocolSpec;

	res = sccp_handle_message(msg, s);

	// check response after handling message
	if(msginfo && msginfo->type == SKINNY_MSGTYPE_RESPONSE) {
		response_received(s);
		sccp_log(DEBUGCAT_SOCKET)(VERBOSE_PREFIX_3 "%s: response %s received\n", DEV_ID_LOG(s->device), msginfo->text);
	}
	return res;
}

static gcc_inline int process_buffer(sccp_session_t * s, sccp_msg_t * msg, unsigned char * const buffer, size_t * len)
{
	int res = 0;
	while (*len >= SCCP_PACKET_HEADER && *len <= SCCP_MAX_PACKET * 2) {										// We have at least SCCP_PACKET_HEADER, so we have the payload length
		uint32_t header_len;
		memcpy(&header_len, buffer, 4);
		uint32_t payload_len = letohl(header_len) + (SCCP_PACKET_HEADER - 4);
		if (dont_expect(payload_len < SCCP_PACKET_HEADER || payload_len > SCCP_MAX_PACKET)) {
			pbx_log(LOG_ERROR, "%s: received a packet with payload length %u, outside the valid range %d-%d; the connection is closed\n", DEV_ID_LOG(s->device), payload_len, (int)SCCP_PACKET_HEADER, (int)SCCP_MAX_PACKET);
			res = -1;
			break;
		}
		if (*len < payload_len) {
			break;
		}

		if (dont_expect(session_buffer2msg(s, buffer, payload_len, msg) != 0)) {
			res = -2;
			break;
		}

		*len -= payload_len;
		if (*len > 0) {
			memmove(buffer + 0, buffer + payload_len, *len);
		}
	}
	return res;
}

static boolean_t sccp_session_findBySession(sccp_session_t * s)
{
	sccp_session_t * session = NULL;
	boolean_t res = FALSE;

	SCCP_RWLIST_RDLOCK(&GLOB(sessions));
	SCCP_RWLIST_TRAVERSE(&GLOB(sessions), session, list) {
		if (session == s) {
			res = TRUE;
			break;
		}
	}
	SCCP_RWLIST_UNLOCK(&GLOB(sessions));
	return res;
}

static boolean_t sccp_session_addToGlobals(sccp_session_t * s)
{
	boolean_t res = FALSE;

	if (s) {
		if (!sccp_session_findBySession(s)) {;
			SCCP_RWLIST_WRLOCK(&GLOB(sessions));
			SCCP_LIST_INSERT_HEAD(&GLOB(sessions), s, list);
			res = TRUE;
			SCCP_RWLIST_UNLOCK(&GLOB(sessions));
		}
	}
	return res;
}

static boolean_t sccp_session_removeFromGlobals(sccp_session_t * s)
{
	sccp_session_t * session = NULL;
	boolean_t res = FALSE;

	if (s) {
		SCCP_RWLIST_WRLOCK(&GLOB(sessions));
		SCCP_RWLIST_TRAVERSE_SAFE_BEGIN(&GLOB(sessions), session, list) {
			if (session == s) {
				SCCP_LIST_REMOVE_CURRENT(list);
				res = TRUE;
				break;
			}
		}
		SCCP_RWLIST_TRAVERSE_SAFE_END;
		SCCP_RWLIST_UNLOCK(&GLOB(sessions));
	}
	return res;
}

/* A send takes an in-flight reference while the session is still in the global list. */
static sessionPtr sccp_session_acquireForSend(constSessionPtr requested, constDevicePtr device)
{
	sccp_session_t *current = NULL;
	sccp_session_t *found = NULL;

	SCCP_RWLIST_RDLOCK(&GLOB(sessions));
	SCCP_RWLIST_TRAVERSE(&GLOB(sessions), current, list) {
		pbx_mutex_lock(&current->send_lock);
		if ((requested && current == requested) || (device && current->device == device)) {
			current->active_sends++;
			found = current;
			pbx_mutex_unlock(&current->send_lock);
			break;
		}
		pbx_mutex_unlock(&current->send_lock);
	}
	SCCP_RWLIST_UNLOCK(&GLOB(sessions));
	return found;
}

static void sccp_session_releaseSend(sessionPtr s)
{
	pbx_mutex_lock(&s->send_lock);
	if (--s->active_sends == 0) {
		pbx_cond_signal(&s->sends_drained);
	}
	pbx_mutex_unlock(&s->send_lock);
}

static devicePtr sccp_session_retainSendDevice(sessionPtr s)
{
	sccp_device_t *device;
	pbx_mutex_lock(&s->send_lock);
	device = s->device ? sccp_device_retain(s->device) : NULL;
	pbx_mutex_unlock(&s->send_lock);
	return device;
}

/*
 * Locks: socket_lock, Glob(sessions)
 */
void sccp_session_terminateAll(void)
{
	sccp_session_t *s = NULL;

	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_2 "SCCP: removing sessions\n");
	SCCP_RWLIST_TRAVERSE_SAFE_BEGIN(&GLOB(sessions), s, list) {
		sccp_session_stopthread(s, SKINNY_DEVICE_RS_NONE);
	}
	SCCP_RWLIST_TRAVERSE_SAFE_END;

	int waitloop = 10;
	while (!SCCP_LIST_EMPTY(&GLOB(sessions)) && waitloop-- > 0) {
		usleep(100);
	}

	if (SCCP_LIST_EMPTY(&GLOB(sessions))) {
		SCCP_RWLIST_HEAD_DESTROY(&GLOB(sessions));
	}
}

/* Release device pointer from session */
static sccp_device_t *__sccp_session_removeDevice(sessionPtr session)
{
	sccp_device_t *return_device = NULL;

	if (!session) {
		return NULL;
	}
	pbx_mutex_lock(&session->send_lock);
	return_device = session->device;
	session->device = NULL;
	pbx_mutex_unlock(&session->send_lock);
	if (return_device) {
		if (return_device->session == session) {
			sccp_device_setRegistrationState(return_device, SKINNY_DEVICE_RS_NONE);
			return_device->session = NULL;
		}
	}
	sccp_session_lock(session);
	sccp_copy_string(session->designator, sccp_netsock_stringify(&session->ourip), sizeof(session->designator));
	sccp_session_unlock(session);
	return return_device;
}

/* Retain device pointer in session. */
static int __sccp_session_addDevice(sessionPtr session, constDevicePtr device)
{
	int res = 0;
	sccp_device_t *new_device = NULL;
	if (session && (!device || (device && session->device != device))) {
		sccp_session_lock(session);
		new_device = sccp_device_retain(device);				/* do this before releasing anything, to prevent device cleanup if the same */
		if (session->device) {
			AUTO_RELEASE(sccp_device_t, remDevice, __sccp_session_removeDevice(session));
		}
		if (device) {
			if (new_device) {
				new_device->session = session;			/* update device session pointer while retained */
				pbx_mutex_lock(&session->send_lock);
				session->device = new_device;				/* keep newly retained device */
				pbx_mutex_unlock(&session->send_lock);

				snprintf(session->designator, sizeof(session->designator), "%s:%d", device->id, session->sc.fd);
				res = 1;
			} else {
				res = -1;
			}
		}
		sccp_session_unlock(session);
	}
	return res;
}

/* Retain device pointer in session. */
int sccp_session_retainDevice(constSessionPtr session, constDevicePtr device)
{
	if (session && (!device || (device && session->device != device))) {
		sessionPtr s = (sessionPtr)session;
		sccp_log((DEBUGCAT_DEVICE))(VERBOSE_PREFIX_3 "%s: device attached to session %d from %s\n", DEV_ID_LOG(device), s->sc.fd, sccp_netsock_stringify_addr(&s->sin));
		return __sccp_session_addDevice(s, device);
	}
	return 0;
}

void sccp_session_releaseDevice(constSessionPtr volatile session)
{
	sessionPtr s = (sessionPtr)session;
	if (s) {
		AUTO_RELEASE(sccp_device_t, device, __sccp_session_removeDevice(s));
	}
}

/* Locks: sessions, device */
static void destroy_session(sccp_session_t * s)
{
	if (!s) {
		return;
	}

	/* No new send can acquire this session after removal. Existing sends may
	 * still be using its socket or device, so finish them before cleanup. */
	boolean_t removed = sccp_session_removeFromGlobals(s);
	pbx_mutex_lock(&s->send_lock);
	while (s->active_sends) {
		pbx_cond_wait(&s->sends_drained, &s->send_lock);
	}
	pbx_mutex_unlock(&s->send_lock);

	char addrStr[INET6_ADDRSTRLEN];
	sccp_copy_string(addrStr, sccp_netsock_stringify_addr(&s->sin), sizeof(addrStr));
	AUTO_RELEASE(sccp_device_t, d, sccp_session_retainSendDevice(s));
	if (d && d->session == s) {
		sccp_log((DEBUGCAT_SOCKET)) (VERBOSE_PREFIX_3 "%s: destroying session %s\n", DEV_ID_LOG(d), addrStr);
		d->session = NULL;
		sccp_dev_clean(d, (d->realtime) ? TRUE : FALSE);
	}
	sccp_session_releaseDevice(s);

	if (!removed) {
		sccp_log((DEBUGCAT_SOCKET)) (VERBOSE_PREFIX_3 "%s: session %s not in the session list\n", DEV_ID_LOG(s->device), addrStr);
	}

	if (s) {
		sccp_log((DEBUGCAT_SOCKET)) (VERBOSE_PREFIX_3 "SCCP: destroying session %s\n", addrStr);
		pbx_mutex_lock(&s->write_lock);
		sccp_session_lock(s);
		if(s->sc.fd > 0) {
			sccp_log((DEBUGCAT_SOCKET))(VERBOSE_PREFIX_3 "SCCP: shutting down socket %d\n", s->sc.fd);
			s->srvcontext->transport->shutdown(&s->sc, SHUT_RDWR);
			sccp_log((DEBUGCAT_SOCKET))(VERBOSE_PREFIX_3 "SCCP: closing socket %d\n", s->sc.fd);
			s->srvcontext->transport->close(&s->sc);
			s->sc.fd = -1;
		}
		sccp_session_unlock(s);
		pbx_mutex_unlock(&s->write_lock);

		sccp_mutex_destroy(&s->lock);
		sccp_mutex_destroy(&s->write_lock);
		pbx_cond_destroy(&s->sends_drained);
		sccp_mutex_destroy(&s->send_lock);
		pbx_cond_destroy(&s->pendingRequest);
		sccp_free(s);
		s = NULL;
	}
}

/* Socket Device Thread Exit */
void sccp_session_device_thread_exit(void *session)
{
	sccp_session_t *s = (sccp_session_t *) session;

	if (!s->device) {
		sccp_log(DEBUGCAT_SOCKET) (VERBOSE_PREFIX_3 "SCCP: session has no device\n");
	}

	sccp_log((DEBUGCAT_SOCKET)) (VERBOSE_PREFIX_3 "%s: cleaning up session\n", DEV_ID_LOG(s->device));
	sccp_session_lock(s);
	s->session_stop = TRUE;
	sccp_session_unlock(s);
	s->session_thread = AST_PTHREADT_NULL;
	destroy_session(s);
}

gcc_inline void recalc_wait_time(sccp_session_t *s)
{
	float keepaliveAdditionalTimePercent = KEEPALIVE_ADDITIONAL_PERCENT_SESSION;
	float keepAlive = GLOB(keepalive);
	float keepAliveInterval = GLOB(keepalive);
	sccp_device_t *d = s->device;
	if (d) {
		keepAlive = d->keepalive;
		keepAliveInterval = d->keepaliveinterval;
		if (
			d->skinny_type == SKINNY_DEVICETYPE_CISCO7920 || d->skinny_type == SKINNY_DEVICETYPE_CISCO7921 ||
			d->skinny_type == SKINNY_DEVICETYPE_CISCO7925 || d->skinny_type == SKINNY_DEVICETYPE_CISCO7926 ||
			d->skinny_type == SKINNY_DEVICETYPE_CISCO7970 || d->skinny_type == SKINNY_DEVICETYPE_CISCO7975 ||
			d->skinny_type == SKINNY_DEVICETYPE_CISCO6911
		) {
			keepaliveAdditionalTimePercent = KEEPALIVE_ADDITIONAL_PERCENT_DEVICE;
		}
		if (d->active_channel) {
			keepaliveAdditionalTimePercent = KEEPALIVE_ADDITIONAL_PERCENT_ON_CALL;
		}
	}
       s->keepAlive = (uint16_t)(keepAlive * keepaliveAdditionalTimePercent);
       s->keepAliveInterval = (uint16_t)keepAliveInterval;

	sccp_log((DEBUGCAT_SOCKET)) (VERBOSE_PREFIX_4 "%s: keepalive %d, poll interval %d\n", s->designator, s->keepAlive, s->keepAliveInterval);
	if (!s->keepAlive || !s->keepAliveInterval) {
		pbx_log(LOG_WARNING, "%s: keepalive for this device computed as zero; using the global keepalive=%d\n", s->designator, GLOB(keepalive));
		s->keepAlive = GLOB(keepalive);
		s->keepAliveInterval = GLOB(keepalive);
	}
}

/* Socket Device Thread */
void *sccp_session_device_thread(void *session)
{
	int res = 0;
	sccp_session_t *s = (sccp_session_t *) session;

	if (!s) {
		return NULL;
	}

	pthread_cleanup_push(sccp_session_device_thread_exit, session);
	pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	/* declared after pthread_cleanup_push so they cannot be clobbered by its setjmp/longjmp */
	boolean_t oncall = TRUE;
	boolean_t tokenThread = FALSE;
	unsigned char recv_buffer[SCCP_MAX_PACKET * 2] = "";
	size_t recv_len = 0;
	sccp_msg_t msg = { {0,} };

	struct pollfd fds[1] = { { 0 } };
	fds[0].events = POLLIN | POLLPRI;
	fds[0].revents = 0;
	fds[0].fd = s->sc.fd;

	while(s->sc.fd > 0 && !s->session_stop) {
		if (s->device) {
			sccp_device_t *d = s->device;
			if (d->pendingUpdate || d->pendingDelete) {
				pbx_rwlock_rdlock(&GLOB(lock));
				boolean_t reload_in_progress = GLOB(reload_in_progress);
				pbx_rwlock_unlock(&GLOB(lock));
				if(reload_in_progress == FALSE && sccp_device_check_update(d)) {
					continue;
				}
				sccp_safe_sleep(100);
				if(!s->device) {
					continue;
				}
			}
			if ((d->active_channel ? TRUE : FALSE) != oncall) {
				recalc_wait_time(s);
				oncall = (d->active_channel) ? TRUE : FALSE;
			}
			if (d->status.token == SCCP_TOKEN_STATE_ACK) {
				tokenThread = TRUE;								// only does TCP-Keepalive
			}
		}
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
		sccp_log_and((DEBUGCAT_SOCKET + DEBUGCAT_HIGH))(VERBOSE_PREFIX_4 "%s: poll timeout %d on session %d\n", DEV_ID_LOG(s->device), (int)s->keepAliveInterval, fds[0].fd);

		if (s->srvcontext->transport->pending(&s->sc) > 0) {
			fds[0].revents = POLLIN;
			res = 1;
		} else {
			res = sccp_netsock_poll(fds, 1, s->keepAliveInterval * 1000);
		}
		pthread_testcancel();
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
		if (-1 == res) {
			if (errno > 0 && (errno != EAGAIN) && (errno != EINTR)) {
				pbx_log(LOG_ERROR, "%s: poll() on the device connection failed (errno %d: %s, ip-address: %s); closing the session\n", DEV_ID_LOG(s->device), errno, strerror(errno), s->designator);
				socket_get_error(s, __FILE__, __LINE__, __PRETTY_FUNCTION__);
				__sccp_session_stopthread(s, SKINNY_DEVICE_RS_FAILED);
				break;
			}
		} else if (0 == res) {
			uintmax_t timediff = (uintmax_t)time(0) - (uintmax_t)s->lastKeepAlive;
			if (!tokenThread && timediff >= s->keepAlive) {
				pbx_log(LOG_NOTICE, "%s: no keepalive from the phone for %ju seconds (limit %d); closing the connection %s\n", DEV_ID_LOG(s->device), timediff, s->keepAlive, s->designator);
				__sccp_session_stopthread(s, SKINNY_DEVICE_RS_TIMEOUT);
				break;
			}
		} else if (res > 0) {
			if(fds[0].revents & POLLIN || fds[0].revents & POLLPRI) {
				int result;
				if (recv_len == sizeof(recv_buffer)) {
					pbx_log(LOG_ERROR, "%s: the receive buffer filled up without a complete SCCP message; closing the connection\n", s->designator);
					__sccp_session_stopthread(s, SKINNY_DEVICE_RS_FAILED);
					break;
				}
				result = s->srvcontext->transport->recv(&s->sc, recv_buffer + recv_len, sizeof(recv_buffer) - recv_len, 0);
				if (result < 0) {
					if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
						continue;
					}
					socket_get_error(s, __FILE__, __LINE__, __PRETTY_FUNCTION__);
					__sccp_session_stopthread(s, SKINNY_DEVICE_RS_FAILED);
					break;
				}
				if (result == 0) {
					__sccp_session_stopthread(s, SKINNY_DEVICE_RS_NONE);
					break;
				}
				recv_len += result;
				s->lastKeepAlive = time(0);
				if (process_buffer(s, &msg, recv_buffer, &recv_len) != 0 || recv_len == sizeof(recv_buffer)) {
					pbx_log(LOG_ERROR, "%s: could not parse the data received from the phone (%d bytes); closing the connection (message dump follows)\n", s->designator, result);
					sccp_dump_msg(&msg);
					if (s->device) {
						sccp_device_sendReset(s->device, SKINNY_RESETTYPE_RESTART);
					}
					__sccp_session_stopthread(s, SKINNY_DEVICE_RS_FAILED);
					break;
				}
			} else {
				pbx_log(LOG_NOTICE, "%s: the phone closed the connection or it failed (%s); closing the session\n", s->designator, (fds[0].revents & POLLHUP) ? "hangup" : "socket error");
				__sccp_session_stopthread(s, SKINNY_DEVICE_RS_FAILED);
				break;
			}
		} else {
			pbx_log(LOG_WARNING, "%s: poll() returned unexpected value %d; ignored\n", DEV_ID_LOG(s->device), res);
		}
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
		pthread_testcancel();
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	}
	sccp_log((DEBUGCAT_SOCKET)) (VERBOSE_PREFIX_3 "%s: session thread ending\n", DEV_ID_LOG(s->device));
	pthread_cleanup_pop(1);

	return NULL;
}

/* stop session device thread from the same thread */
void __sccp_session_stopthread(sessionPtr s, skinny_registrationstate_t newRegistrationState)
{
	if(!s) {
		sccp_log((DEBUGCAT_SOCKET))(VERBOSE_PREFIX_3 "SCCP: session stop requested for a session that is already gone\n");
		return;
	}
	AUTO_RELEASE(sccp_device_t, device, sccp_session_retainSendDevice(s));
	sccp_log((DEBUGCAT_SOCKET))(VERBOSE_PREFIX_2 "%s: stopping session thread\n", DEV_ID_LOG(device));

	s->session_stop = TRUE;
	if(device) {
		sccp_device_setRegistrationState(device, newRegistrationState);
	}
	if(AST_PTHREADT_NULL != s->session_thread) {
		s->srvcontext->transport->shutdown(&s->sc, SHUT_RD);                                        // this will also wake up poll
													    // which is waiting for a read event and close down the thread nicely
	}
}

/* cleanup session device thread from another thread */
static void __sccp_netsock_end_device_thread(sccp_session_t *session)
{
	pthread_t session_thread = session->session_thread;
	if (session_thread == AST_PTHREADT_NULL) {
		return;
	}

	/* send thread cancellation (will interrupt poll if necessary) */
	int s = pthread_cancel(session_thread);
	if (s != 0) {
		pbx_log(LOG_WARNING, "SCCP: could not cancel a session thread (%s); waiting for it to exit\n", strerror(s));
	}

	/* join previous session thread, wait for device cleanup */
	void * res = NULL;
	if (pthread_join(session_thread, &res) == 0) {
		if (res != PTHREAD_CANCELED) {
			sccp_log((DEBUGCAT_SOCKET))(VERBOSE_PREFIX_3 "SCCP: session thread had already exited before it was cancelled\n");
		}
	}
}

/* check if same or different thread, choose thread cancel method accordingly */
gcc_inline void sccp_session_stopthread(constSessionPtr session, skinny_registrationstate_t newRegistrationState)
{
	sessionPtr s = (sessionPtr)session;
	if (s) {
		pthread_t ptid = pthread_self();
		if (ptid == s->session_thread) {
			__sccp_session_stopthread(s, newRegistrationState);
		} else {
			__sccp_netsock_end_device_thread(s);
		}
	}
}

static boolean_t sccp_session_new_socket_allowed(struct sockaddr_storage *sin)
{
	char addrStr[INET6_ADDRSTRLEN];
	sccp_copy_string(addrStr, sccp_netsock_stringify(sin), sizeof(addrStr));
	if (GLOB(ha) && sccp_apply_ha(GLOB(ha), sin) != AST_SENSE_ALLOW) {
		struct ast_str *buf = pbx_str_alloca(DEFAULT_PBX_STR_BUFFERSIZE);
		if (buf) {
			sccp_print_ha(buf, DEFAULT_PBX_STR_BUFFERSIZE, GLOB(ha));
			pbx_log(LOG_NOTICE, "SCCP: connection from %s refused by the global deny/permit settings (%s)\n", addrStr, pbx_str_buffer(buf));
		} else {
			pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
		}
		return FALSE;
	}
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: connection from %s accepted\n", addrStr);
	return TRUE;
}

static sccp_session_t * sccp_create_session(sccp_servercontext_t * context, sccp_socket_connection_t * sc)
{
	sccp_session_t * s = NULL;

	if (!(s = (sccp_session_t *)sccp_calloc(sizeof *s, 1))) {
		pbx_log(LOG_ERROR, SS_Memory_Allocation_Error, __func__);
		return NULL;
	}

	sccp_mutex_init(&s->lock);
	pbx_cond_init(&s->pendingRequest, NULL);
	sccp_mutex_init(&s->write_lock);
	sccp_mutex_init(&s->send_lock);
	pbx_cond_init(&s->sends_drained, NULL);

	s->sc.fd = sc->fd;
	s->sc.ssl = sc->ssl;
	s->sc.ssl_lock = sc->ssl_lock;
	s->protocolType = SCCP_PROTOCOL;
	s->srvcontext = context;

	s->lastKeepAlive = time(0);

	return s;
}

static boolean_t sccp_session_set_ourip(sccp_session_t * s)
{
	if (sccp_netsock_is_any_addr(&GLOB(bindaddr))) {
		struct sockaddr_storage them = { 0 };

		if(sccp_netsock_is_mapped_IPv4(&s->sin)) {
			sccp_netsock_ipv4_mapped(&s->sin, &them);
		} else {
			memcpy(&them, &s->sin, sizeof(struct sockaddr_storage));
		}

		memcpy(&s->ourip, &internip.ss, sizeof(struct sockaddr_storage));

		if(!sccp_netsock_ouraddrfor(&them, &s->ourip)) {
			pbx_log(LOG_WARNING, "SCCP: could not determine the local address used to reach %s; RTP may advertise the wrong address\n", sccp_netsock_stringify(&s->sin));
		}
	} else {
		memcpy(&s->ourip, &GLOB(bindaddr), sizeof(s->ourip));
	}
	sccp_copy_string(s->designator, sccp_netsock_stringify(&s->ourip), sizeof(s->designator));
	sccp_log((DEBUGCAT_SOCKET))(VERBOSE_PREFIX_3 "SCCP: connected to the server via %s\n", s->designator);
	return TRUE;
}

/* Accept Thread continuously waits for devices trying to connect, when they do it */
static void * accept_thread(void * data)
{
	sccp_servercontext_t * context = (sccp_servercontext_t *)data;
	sccp_socket_connection_t new_sc = { .fd = -1 };
	struct sockaddr_storage incoming;
	sccp_session_t *s = NULL;
	socklen_t length = (socklen_t)(sizeof(struct sockaddr_storage));

	while (GLOB(module_running)) {
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
		pthread_testcancel();
		new_sc.fd = -1;
		new_sc.ssl = NULL;
		new_sc.ssl_lock = NULL;
		length = (socklen_t)sizeof(incoming);
		if (context->transport->accept(&context->sc, (struct sockaddr *)&incoming, &length, &new_sc) != &new_sc || new_sc.fd < 0) {
			pbx_log(LOG_WARNING, "SCCP: accepting a new phone connection failed: %s; retrying\n", strerror(errno));
			usleep(1000);
			continue;
		}

		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
		sccp_netsock_setoptions(new_sc.fd, -1, 0, -1, -1, 0);

		if (!sccp_session_new_socket_allowed(&incoming)) {
			context->transport->close(&new_sc);
			continue;
		}

		s = sccp_create_session(context, &new_sc);
		if(s == NULL) {
			context->transport->close(&new_sc);
			continue;
		}
		new_sc.fd = -1;
		new_sc.ssl = NULL;
		new_sc.ssl_lock = NULL;
		memcpy(&s->sin, &incoming, sizeof(s->sin));
		sccp_session_set_ourip(s);
		sccp_session_addToGlobals(s);
		recalc_wait_time(s);

		// Create a detached thread, since the sccp_session_device_thread will not be joined from another thread
		// Only detached threads free their stack and control structures after termination, otherwise a pthread_join is mandatory for this to take place (davidded).
		if (pbx_pthread_create_detached(&s->session_thread, NULL, sccp_session_device_thread, s)) {
			destroy_session(s);
		}
	}
	context->transport->close(&new_sc);
	if(context->sc.fd > -1) {
		sccp_log((DEBUGCAT_CORE))(VERBOSE_PREFIX_3 "closing listening socket %d\n", context->sc.fd);
		context->transport->close(&context->sc);
		context->sc.fd = -1;
	}
	return 0;
}

static void sccp_session_start_accept_thread(sccp_servercontext_t * context)
{
	ast_pthread_create_background(&context->accept_tid, NULL, accept_thread, (void *)context);
}

void sccp_session_stop_accept_thread(sccp_servercontext_t * context)
{
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "stopping the accept thread\n");
	pbx_rwlock_wrlock(&GLOB(lock));
	if(context->accept_tid && (context->accept_tid != AST_PTHREADT_STOP)) {
		if (pthread_cancel(context->accept_tid) != 0) {
			pthread_kill(context->accept_tid, SIGURG);
		}
		pthread_join(context->accept_tid, NULL);
	}
	context->accept_tid = AST_PTHREADT_STOP;
	if(context->sc.fd > -1) {
		sccp_log((DEBUGCAT_CORE))(VERBOSE_PREFIX_3 "closing listening socket %d\n", context->sc.fd);
		context->transport->close(&context->sc);
		context->sc.fd = -1;
	}
	pbx_rwlock_unlock(&GLOB(lock));
}

/*
 * The bound accepting socket is stored in a static global variable (see at top) The thread id (tid) is stored in a static global variable (see at top)
 */
boolean_t sccp_session_bind_and_listen(sccp_servercontext_t * context, struct sockaddr_storage * bindaddr)
{
	int result = FALSE;
	static int port = -1;
	char addrStr[INET6_ADDRSTRLEN];
	sccp_copy_string(addrStr, sccp_netsock_stringify_addr(bindaddr), sizeof(addrStr));

	sccp_log((DEBUGCAT_CORE))(VERBOSE_PREFIX_3 "binding and listening on %s\n", addrStr);
	if(context->sc.fd < 0) {
		int status = 0;
		port = sccp_netsock_getPort(bindaddr);
		memcpy(&context->boundaddr, bindaddr, sizeof(struct sockaddr_storage));
		char port_str[15] = "cisco-sccp";

		struct addrinfo hints;

		struct addrinfo * res = NULL;
		memset(&hints, 0, sizeof hints);
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV | AI_PASSIVE;
		if (port) {
			snprintf(port_str, sizeof(port_str), "%d", port);
		}

		sccp_log((DEBUGCAT_CORE))(VERBOSE_PREFIX_3 "looking up %s:%s in /etc/services\n", addrStr, port_str);
		status = getaddrinfo(sccp_netsock_stringify_addr(bindaddr), port_str, &hints, &res);
		if(status != 0) {
			pbx_log(LOG_ERROR, "SCCP: listener not started: could not resolve bindaddr %s port %s: %s\n", sccp_netsock_stringify_addr(bindaddr), port_str, gai_strerror(status));
			return FALSE;
		}
		do {
			context->sc.fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
			if(context->sc.fd < 0) {
				pbx_log(LOG_ERROR, "SCCP: listener not started: could not create a socket: %s\n", strerror(errno));
				break;
			}
			sccp_netsock_setoptions(context->sc.fd, 1, -1, -1, 0, 0);
			if(context->transport->bind(&context->sc, res->ai_addr, res->ai_addrlen) < 0) {
				pbx_log(LOG_ERROR, "SCCP: listener not started: could not bind to %s:%d: %s\n", addrStr, port, strerror(errno));
				context->transport->close(&context->sc);
				context->sc.fd = -1;
				break;
			}

			struct ast_sockaddr tmp_sa;
			ast_sockaddr_copy(&internip, storage2ast_sockaddr(bindaddr, &tmp_sa));
			if(ast_find_ourip(&internip, &tmp_sa, 0)) {
				pbx_log(LOG_ERROR, "SCCP: listener not started: bindaddr is a wildcard and the local IP address could not be determined\n");
				context->transport->close(&context->sc);
				context->sc.fd = -1;
				break;
			}

			if(listen(context->sc.fd, DEFAULT_SCCP_BACKLOG)) {
				pbx_log(LOG_ERROR, "SCCP: listener not started: could not listen on %s:%d: %s\n", addrStr, port, strerror(errno));
				context->transport->close(&context->sc);
				context->sc.fd = -1;
				break;
			}
			sccp_session_start_accept_thread(context);
		} while(0);
		freeaddrinfo(res);
	} else {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "socket unchanged; reusing it\n");
	}

	if(context->sc.fd > -1) {
		sccp_log((DEBUGCAT_CORE))(VERBOSE_PREFIX_3 "SCCP: listening on %s:%d (socket %d)\n", addrStr, port, context->sc.fd);
		sccp_log((DEBUGCAT_SOCKET))(VERBOSE_PREFIX_3 "SCCP: default local address %s\n", ast_sockaddr_stringify_addr(&internip));
		result = TRUE;
	}
	return result;
}

void sccp_session_sendmsg(const sccp_device_t * device, sccp_mid_t t)
{
	if (!device) {
		sccp_log((DEBUGCAT_SOCKET)) (VERBOSE_PREFIX_3 "SCCP: message not sent: no device\n");
		return;
	}

	sccp_msg_t *msg = sccp_build_packet(t, 0);
	if (msg) {
		sccp_session_send(device, msg);
	}
}

static int sccp_session_sendOwned(sessionPtr s, sccp_msg_t *msg);

int sccp_session_send(constDevicePtr device, const sccp_msg_t * msg_in)
{
	sccp_msg_t *msg = (sccp_msg_t *) msg_in;
	sessionPtr s = device ? sccp_session_acquireForSend(NULL, device) : NULL;
	int result = sccp_session_sendOwned(s, msg);

	if (s) {
		sccp_session_releaseSend(s);
	}
	return result;
}

static int sccp_session_sendOwned(sessionPtr s, sccp_msg_t * msg)
{
	ssize_t res = 0;
	uint32_t msgid;
	ssize_t bytesSent = 0;
	ssize_t bufLen = 0;
	uint8_t * bufAddr = NULL;

	if (!msg) {
		return -1;
	}
	msgid = letohl(msg->header.lel_messageId);
	if (s && s->session_stop) {
		sccp_free(msg);
		return -2;
	}

	if(!s || s->sc.fd <= 0) {
		sccp_log((DEBUGCAT_HIGH)) (VERBOSE_PREFIX_3 "SCCP: packet not sent: the device is down\n");
		if (s) {
			__sccp_session_stopthread(s, SKINNY_DEVICE_RS_FAILED);
		}
		sccp_free(msg);
		msg = NULL;
		return -3;
	}
	AUTO_RELEASE(sccp_device_t, send_device, sccp_session_retainSendDevice(s));
	if (msgid == KeepAliveAckMessage || msgid == RegisterAckMessage || msgid == UnregisterAckMessage) {
		msg->header.lel_protocolVer = 0;
	} else if (send_device && send_device->protocol) {
		msg->header.lel_protocolVer = send_device->protocol->version < 10 ? 0 : htolel(send_device->protocol->version);
	}

	uint backoff = WRITE_BACKOFF;
	bytesSent = 0;
	bufAddr = ((uint8_t *) msg);
	bufLen = (ssize_t) (letohl(msg->header.length) + 8);

	struct messageinfo * msginfo = lookupMsgInfoStruct(msgid);
	if(msginfo) {
		if(msginfo->messageId != msgid) {
			pbx_log(LOG_ERROR, "%s: tried to send unknown message ID 0x%04X (closest table entry 0x%04X); not sent (caller bug)\n", DEV_ID_LOG(send_device), msgid, msginfo->messageId);
			sccp_free(msg);
			return -4;
		}
		if(msginfo->type == SKINNY_MSGTYPE_REQUEST) {
			request_pending(s);
			sccp_log(DEBUGCAT_SOCKET)(VERBOSE_PREFIX_3 "%s: request %s pending\n", DEV_ID_LOG(send_device), msginfo->text);
		}
		if((GLOB(debug) & DEBUGCAT_MESSAGE) != 0) {
			pbx_log(LOG_NOTICE, "%s: sending %s (0x%04X), %d bytes\n", DEV_ID_LOG(send_device), msginfo->text, msgid, msg->header.length);
			sccp_dump_msg(msg);
		}
	}
	/* Keep the whole SCCP frame together even when the transport writes only part of it. */
	pbx_mutex_lock(&s->write_lock);
	do {
		res = s->srvcontext->transport->send(&s->sc, bufAddr + bytesSent, bufLen - bytesSent, 0);
		if (res <= 0) {
			if (errno == EINTR) {
				usleep(backoff);
				if (backoff < 8000) {
					backoff *= 2;
				}
				continue;
			}
			socket_get_error(s, __FILE__, __LINE__, __PRETTY_FUNCTION__);
			res = -1;
			break;
		}
		bytesSent += res;
	} while(bytesSent < bufLen && s && !s->session_stop && s->sc.fd > 0);
	pbx_mutex_unlock(&s->write_lock);
	if (res == -1) {
		__sccp_session_stopthread(s, SKINNY_DEVICE_RS_FAILED);
	}

	sccp_free(msg);
	msg = NULL;

	if (bytesSent < bufLen) {
		pbx_log(LOG_ERROR, "%s: only %d of %d bytes of a message were sent to the phone; the phone may be out of sync\n", DEV_ID_LOG(send_device), (int) bytesSent, (int) bufLen);
		res = -1;
	}

	return res;
}

int sccp_session_send2(constSessionPtr session, sccp_msg_t *msg)
{
	sessionPtr s = session ? sccp_session_acquireForSend(session, NULL) : NULL;
	int result = sccp_session_sendOwned(s, msg);

	if (s) {
		sccp_session_releaseSend(s);
	}
	return result;
}

sccp_session_t *sccp_session_reject(constSessionPtr session, char *message)
{
	sccp_msg_t *msg = NULL;
	sessionPtr s = (sessionPtr)session;

	REQ(msg, RegisterRejectMessage);
	if (!msg) {
		return NULL;
	}
	sccp_copy_string(msg->data.RegisterRejectMessage.text, message, sizeof(msg->data.RegisterRejectMessage.text));
	sccp_session_send2(s, msg);
	return NULL;
}

void sccp_session_crossdevice_cleanup(constSessionPtr current_session, sessionPtr previous_session)
{
	if (!current_session || !previous_session) {
		return;
	}
	if (current_session != previous_session && previous_session->session_thread) {
		sccp_log(DEBUGCAT_CORE) (VERBOSE_PREFIX_2 "%s: session %s needs to be closed\n", current_session->designator, previous_session->designator);
		__sccp_netsock_end_device_thread(previous_session);
	}
}

gcc_inline boolean_t sccp_session_check_crossdevice(constSessionPtr session, constDevicePtr device)
{
	if (session && device && ((session->device && session->device != device) || (device->session && device->session != session))) {
		pbx_log(LOG_WARNING, "%s: device and connection disagree: the device is attached to %s, this connection to %s\n", device->id, device->session ? device->session->designator : "none", session->designator);
		return TRUE;
	}
	return FALSE;
}

void sccp_session_tokenReject(constSessionPtr session, uint32_t backoff_time)
{
	sccp_msg_t *msg = NULL;

	REQ(msg, RegisterTokenReject);
	if (!msg) {
		return;
	}
	msg->data.RegisterTokenReject.lel_tokenRejWaitTime = htolel(backoff_time);
	sccp_session_send2(session, msg);
}

void sccp_session_tokenAck(constSessionPtr session)
{
	sccp_msg_t *msg = NULL;

	REQ(msg, RegisterTokenAck);
	if (!msg) {
		return;
	}
	sccp_session_send2(session, msg);
}

void sccp_session_tokenRejectSPCP(constSessionPtr session, uint32_t features)
{
	sccp_msg_t *msg = NULL;

	REQ(msg, SPCPRegisterTokenReject);
	if (!msg) {
		return;
	}
	msg->data.SPCPRegisterTokenReject.lel_features = htolel(features);
	sccp_session_send2(session, msg);
}

void sccp_session_tokenAckSPCP(constSessionPtr session, uint32_t features)
{
	sccp_msg_t *msg = NULL;

	REQ(msg, SPCPRegisterTokenAck);
	if (!msg) {
		return;
	}
	msg->data.SPCPRegisterTokenAck.lel_features = htolel(features);
	sccp_session_send2(session, msg);
}

gcc_inline void sccp_session_setProtocol(constSessionPtr session, uint16_t protocolType)
{
	sessionPtr s = (sessionPtr)session;
	if (s) {
		s->protocolType = protocolType;
	}
}

gcc_inline uint16_t sccp_session_getProtocol(constSessionPtr session)
{
	if (session) {
		return session->protocolType;
	}
	return UNKNOWN_PROTOCOL;
}

gcc_inline void sccp_session_resetLastKeepAlive(constSessionPtr session)
{
	sessionPtr s = (sessionPtr)session;
	if (s) {
		s->lastKeepAlive = time(0);
	}
}

gcc_inline const char * const sccp_session_getDesignator(constSessionPtr session)
{
	return session->designator;
}

/*
 * Get device connected to this session
 * returns retained device
 */
gcc_inline devicePtr sccp_session_getDevice(constSessionPtr session, boolean_t required)
{
	if (!session) {
		return NULL;
	}
	sccp_device_t *device = (session->device) ? sccp_device_retain(session->device) : NULL;
	if (required && !device) {
		sccp_log((DEBUGCAT_SOCKET))(VERBOSE_PREFIX_3 "%s: connection has no registered device\n", session->designator);
		return NULL;
	}
	if (required && sccp_session_check_crossdevice(session, device)) {
		sccp_session_crossdevice_cleanup(session, device->session);
		sccp_device_release(&device);							/* explicit release after error */
		return NULL;
	}
	return device;
}

boolean_t sccp_session_isValid(constSessionPtr session)
{
	if(session && session->sc.fd > 0 && !session->session_stop && !sccp_netsock_is_any_addr(&session->ourip)) {
		return TRUE;
	}
	return FALSE;
}

int sccp_cli_show_sessions(int fd, sccp_cli_totals_t *totals, struct mansession *s, const struct message *m, int argc, char *argv[])
{
	int local_line_total = 0;
	char clientAddress[INET6_ADDRSTRLEN] = "";

#define CLI_AMI_TABLE_NAME Sessions
#define CLI_AMI_TABLE_PER_ENTRY_NAME Session
#define CLI_AMI_TABLE_LIST_ITER_HEAD &GLOB(sessions)
#define CLI_AMI_TABLE_LIST_ITER_TYPE sccp_session_t
#define CLI_AMI_TABLE_LIST_ITER_VAR session
#define CLI_AMI_TABLE_LIST_LOCK SCCP_RWLIST_RDLOCK
#define CLI_AMI_TABLE_LIST_ITERATOR SCCP_RWLIST_TRAVERSE
#define CLI_AMI_TABLE_LIST_UNLOCK SCCP_RWLIST_UNLOCK
#define CLI_AMI_TABLE_BEFORE_ITERATION                                                                      \
	sccp_session_lock(session);                                                                         \
	sccp_copy_string(clientAddress, sccp_netsock_stringify_addr(&session->sin), sizeof(clientAddress)); \
	AUTO_RELEASE(sccp_device_t, d, session->device ? sccp_device_retain(session->device) : NULL);       \
	if(d || (argc == 4 && sccp_strcaseequals(argv[3], "all"))) {
#define CLI_AMI_TABLE_AFTER_ITERATION \
	}                             \
	sccp_session_unlock(session);
#define CLI_AMI_TABLE_FIELDS                                                                                                           \
	CLI_AMI_TABLE_FIELD(Socket, "-6", d, 6, session->sc.fd)                                                                        \
	CLI_AMI_TABLE_FIELD_NAMED(IP, "IP Address", "40.40", s, 40, clientAddress)                                                                         \
	CLI_AMI_TABLE_FIELD_NAMED(Trans, "Transport", "5.5", s, 5, session->srvcontext->transport->name)                                                  \
	CLI_AMI_TABLE_FIELD(Port, "-5", d, 5, sccp_netsock_getPort(&session->sin))                                                     \
	CLI_AMI_TABLE_FIELD_NAMED(KALST, "Last Keepalive (s)", "-5", d, 5, (uint32_t)(time(0) - session->lastKeepAlive))                                           \
	CLI_AMI_TABLE_FIELD_NAMED(KAINT, "Interval (s)", "-5", d, 5, (d ? d->keepaliveinterval : session->keepAliveInterval))                                \
	CLI_AMI_TABLE_FIELD_NAMED(KAMAX, "Timeout (s)", "-5", d, 5, session->keepAlive)                                                                     \
	CLI_AMI_TABLE_FIELD_NAMED(DeviceName, "Device", "15", s, 15, (d) ? d->id : "(none)")                                                               \
	CLI_AMI_TABLE_FIELD(State, "-14.14", s, 14, (d) ? sccp_devicestate2str(sccp_device_getDeviceState(d)) : "(none)")                  \
	CLI_AMI_TABLE_FIELD(Type, "-15.15", s, 15, (d) ? skinny_devicetype2str(d->skinny_type) : "(none)")                                 \
	CLI_AMI_TABLE_FIELD_NAMED(RegState, "Registration", "-10.10", s, 10, (d) ? skinny_registrationstate2str(sccp_device_getRegistrationState(d)) : "(none)") \
	CLI_AMI_TABLE_FIELD(Token, "-10.10", s, 10, d ? sccp_tokenstate2str(d->status.token) : "(none)")                                   \
	CLI_AMI_TABLE_FIELD_NAMED(Req, "Pending Requests", "-3", d, 3, session->requestsInFlight)
#include "sccp_cli_table.h"

	if (s) {
		totals->lines = local_line_total;
		totals->tables = 1;
	}
	return RESULT_SUCCESS;
}
