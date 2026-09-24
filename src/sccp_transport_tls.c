/*!
 * \file	sccp_transport_tls.c
 * \brief       SCCP Session Class
 * \author      Sergio Chersovani <mlists [at] c-net.it>
 * \note	Reworked, but based on chan_sccp code.
 *		The original chan_sccp driver that was made by Zozo which itself was derived from the chan_skinny driver.
 *		Modified by Jan Czmok and Julien Goodwin
 * \note	This program is free software and may be modified and distributed under the terms of the GNU Public License.
 *		See the LICENSE file at the top of the source tree.
 */
#include "config.h"
#include "common.h"

SCCP_FILE_VERSION(__FILE__, "");

#include "sccp_transport.h"
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <time.h>

#ifdef HAVE_LIBSSL
#	include <openssl/err.h> /* for ERR_print_errors_fp */
#	include <openssl/ssl.h> /* for SSL_CTX_free, SSL_get_error, ... */
#	ifdef HAVE_CRYPTO
#		include <openssl/crypto.h> /* for OPENSSL_free */
#	endif
#	define PBX_CERTFILE           "asterisk.pem"                                        // move to config.h (copy from tcptls.h)
#	define REQUEST_RETRY_INTERVAL 5
#	define REQUEST_RETRY_COUNT    2
#	define DUPLICATE_INTERVAL     REQUEST_RETRY_INTERVAL * REQUEST_RETRY_COUNT
#	define TLS_IO_TIMEOUT_MS      5000

/* local variables */
static SSL_CTX * sslctx = NULL;

/* forward declares */
const sccp_transport_t tlstransport;

static void write_openssl_error_to_log(void)
{
	char * buffer = NULL;
	size_t length = 0;

	FILE * fp = open_memstream(&buffer, &length);
	if (!fp) {
		pbx_log(LOG_ERROR, "SCCP: OpenSSL error details could not be collected (open_memstream failed)\n");
		return;
	}

	ERR_print_errors_fp(fp);
	fclose(fp);

	if (length) {
		pbx_log(LOG_ERROR, "SCCP: OpenSSL: %.*s\n", (int)length, buffer);
	}

	ast_free(buffer);
}

static int64_t tls_now_ms(void)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		return -1;
	}
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int tls_wait_for_io(int fd, int ssl_error, int64_t deadline)
{
	struct pollfd pfd = { .fd = fd, .events = ssl_error == SSL_ERROR_WANT_WRITE ? POLLOUT : POLLIN };
	for (;;) {
		int64_t now = tls_now_ms();
		int64_t remaining;
		int result;
		if (now < 0) {
			return -1;
		}
		remaining = deadline - now;
		if (remaining <= 0) {
			errno = ETIMEDOUT;
			return -1;
		}
		result = poll(&pfd, 1, remaining > INT_MAX ? INT_MAX : (int)remaining);
		if (result > 0 && (pfd.revents & pfd.events)) {
			return 0;
		}
		if (result > 0) {
			errno = ECONNRESET;
			return -1;
		}
		if (result == 0) {
			errno = ETIMEDOUT;
			return -1;
		}
		if (errno != EINTR) {
			return -1;
		}
	}
}

static int tls_error_result(int ssl_error, int saved_errno)
{
	if (ssl_error == SSL_ERROR_ZERO_RETURN) {
		return 0;
	}
	errno = ssl_error == SSL_ERROR_SYSCALL && saved_errno ? saved_errno : EPROTO;
	return -1;
}

static SSL_CTX * create_context(void)
{
	sccp_log(DEBUGCAT_SOCKET)(VERBOSE_PREFIX_1 "TLS: creating context\n");
	// const SSL_METHOD * method = TLS_server_method();
	const SSL_METHOD * method = SSLv23_method();
	SSL_CTX *          ctx    = SSL_CTX_new(method);
	if (!ctx) {
		pbx_log(LOG_WARNING, "SCCP: TLS listener not started: OpenSSL could not create a context\n");
		write_openssl_error_to_log();
		return NULL;
	}
	SSL_CTX_set_options(ctx, SSL_OP_SINGLE_DH_USE | SSL_OP_NO_SSLv2);

	return ctx;
}

static boolean_t configure_context(SSL_CTX * ctx)
{
	SSL_CTX_set_ecdh_auto(ctx, 1);
	sccp_log(DEBUGCAT_SOCKET)(VERBOSE_PREFIX_1 "TLS: configuring context\n");

	char * cert_file = NULL;
	if (GLOB(cert_file)) {
		cert_file = ast_strdupa(GLOB(cert_file));
	} else {
		cert_file = ast_strdupa(PBX_CERTFILE);
	}

	if (access(cert_file, F_OK) != 0) {
		pbx_log(LOG_NOTICE, "SCCP: TLS (secure SCCP) listener not started: certfile %s does not exist; phones can only use plain SCCP\n", cert_file);
		return FALSE;
	} else {
		if (SSL_CTX_use_certificate_file(ctx, cert_file, SSL_FILETYPE_PEM) <= 0) {
			pbx_log(LOG_WARNING, "SCCP: TLS listener not started: could not load the certificate from %s\n", cert_file);
			write_openssl_error_to_log();
			return FALSE;
		} else if (SSL_CTX_use_PrivateKey_file(ctx, cert_file, SSL_FILETYPE_PEM) <= 0) {
			pbx_log(LOG_WARNING, "SCCP: TLS listener not started: could not load the private key from %s\n", cert_file);
			write_openssl_error_to_log();
			return FALSE;
		} else if (SSL_CTX_check_private_key(ctx) == 0) {
			pbx_log(LOG_WARNING, "SCCP: TLS listener not started: the private key in %s does not match its certificate\n", cert_file);
			write_openssl_error_to_log();
			return FALSE;
		}
	}

	return TRUE;
}
const sccp_transport_t * const tls_init(void)
{
	sccp_log(DEBUGCAT_SOCKET)(VERBOSE_PREFIX_1 "TLS: initializing\n");
	if (sslctx) {
		return &tlstransport;
	}
	SSL_load_error_strings();
	SSL_library_init();
	sslctx = create_context();
	if (sslctx && configure_context(sslctx)) {
		return &tlstransport;
	}
	SSL_CTX_free(sslctx);
	sslctx = NULL;
	return NULL;
}

static int tls_bind(sccp_socket_connection_t * sc, struct sockaddr * addr, socklen_t addrlen)
{
	// sccp_log(DEBUGCAT_SOCKET)(VERBOSE_PREFIX_1 "TLS Transport bind...\n");
	return bind(sc->fd, addr, addrlen);
}

static int tls_listen(sccp_socket_connection_t * sc, int backlog)
{
	// sccp_log(DEBUGCAT_SOCKET)(VERBOSE_PREFIX_1 "TLS Transport listen...\n");
	return listen(sc->fd, backlog);
}

static sccp_socket_connection_t * tls_accept(sccp_socket_connection_t * in_sc, struct sockaddr * addr, socklen_t * addrlen, sccp_socket_connection_t * out_sc)
{
	int           newfd = -1;
	int           flags;
	int           result;
	int           ssl_error;
	int           saved_errno;
	int           lock_result;
	int64_t       deadline;
	SSL *         ssl   = NULL;
	sccp_mutex_t *ssl_lock = NULL;
	newfd = accept(in_sc->fd, addr, addrlen);
	if (newfd < 0) {
		return NULL;
	}
	/* The caller re-enables cancellation before its next accept. Keep the
	 * handshake and ownership handoff together so cancellation cannot leak
	 * an accepted socket or interrupt an OpenSSL call. */
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	flags = fcntl(newfd, F_GETFL);
	if (flags < 0 || fcntl(newfd, F_SETFL, flags | O_NONBLOCK) < 0) {
		goto failed;
	}
	ssl = SSL_new(sslctx);
	if (!ssl || SSL_set_fd(ssl, newfd) != 1) {
		errno = EPROTO;
		goto failed;
	}
	deadline = tls_now_ms();
	if (deadline < 0) {
		goto failed;
	}
	deadline += TLS_IO_TIMEOUT_MS;
	for (;;) {
		errno = 0;
		result = SSL_accept(ssl);
		if (result == 1) {
			ssl_lock = ast_malloc(sizeof(*ssl_lock));
			if (!ssl_lock) {
				errno = ENOMEM;
				goto failed;
			}
			lock_result = sccp_mutex_init(ssl_lock);
			if (lock_result != 0) {
				errno = lock_result;
				ast_free(ssl_lock);
				ssl_lock = NULL;
				goto failed;
			}
			out_sc->fd = newfd;
			out_sc->ssl = ssl;
			out_sc->ssl_lock = ssl_lock;
			return out_sc;
		}
		saved_errno = errno;
		ssl_error = SSL_get_error(ssl, result);
		if (ssl_error != SSL_ERROR_WANT_READ && ssl_error != SSL_ERROR_WANT_WRITE) {
			if (tls_error_result(ssl_error, saved_errno) == 0) {
				errno = ECONNRESET;
			}
			break;
		}
		if (tls_wait_for_io(newfd, ssl_error, deadline) < 0) {
			break;
		}
	}
failed:
	saved_errno = errno;
	if (ssl) {
		write_openssl_error_to_log();
		SSL_free(ssl);
	}
	close(newfd);
	errno = saved_errno;
	return NULL;
}

static int tls_io_locked(sccp_socket_connection_t * sc, void * buf, size_t buflen, boolean_t writing)
{
	int64_t deadline = tls_now_ms();
	int result;
	int ssl_error;
	int saved_errno;
	if (deadline < 0) {
		return -1;
	}
	deadline += TLS_IO_TIMEOUT_MS;
	if (buflen > INT_MAX) {
		buflen = INT_MAX;
	}
	for (;;) {
		errno = 0;
		result = writing ? SSL_write(sc->ssl, buf, (int)buflen) : SSL_read(sc->ssl, buf, (int)buflen);
		if (result > 0) {
			return result;
		}
		saved_errno = errno;
		ssl_error = SSL_get_error(sc->ssl, result);
		if (ssl_error != SSL_ERROR_WANT_READ && ssl_error != SSL_ERROR_WANT_WRITE) {
			return tls_error_result(ssl_error, saved_errno);
		}
		if (tls_wait_for_io(sc->fd, ssl_error, deadline) < 0) {
			return -1;
		}
	}
}

static int tls_io(sccp_socket_connection_t * sc, void * buf, size_t buflen, boolean_t writing)
{
	int result;
	sccp_mutex_lock(sc->ssl_lock);
	result = tls_io_locked(sc, buf, buflen, writing);
	sccp_mutex_unlock(sc->ssl_lock);
	return result;
}

static int tls_recv(sccp_socket_connection_t * sc, void * buf, size_t buflen, int flags)
{
	return tls_io(sc, buf, buflen, FALSE);
}

static int tls_pending(sccp_socket_connection_t * sc)
{
	int pending;
	sccp_mutex_lock(sc->ssl_lock);
	pending = SSL_pending(sc->ssl);
	sccp_mutex_unlock(sc->ssl_lock);
	return pending;
}

static int tls_send(sccp_socket_connection_t * sc, void * buf, size_t buflen, int flags)
{
	return tls_io(sc, buf, buflen, TRUE);
}

static int tls_shutdown(sccp_socket_connection_t * sc, int how)
{
	// sccp_log(DEBUGCAT_SOCKET)(VERBOSE_PREFIX_1 "TLS Transport shutdown...\n");
	sccp_mutex_lock(sc->ssl_lock);
	SSL_shutdown(sc->ssl);
	sccp_mutex_unlock(sc->ssl_lock);
	return shutdown(sc->fd, how);
}

static int tls_close(sccp_socket_connection_t * sc)
{
	int res = 0;
	if (sc->ssl_lock) {
		sccp_mutex_lock(sc->ssl_lock);
	}
	if (sc->ssl) {
		SSL_free(sc->ssl);
		sc->ssl = NULL;
	}
	if (sc->ssl_lock) {
		sccp_mutex_unlock(sc->ssl_lock);
		sccp_mutex_destroy(sc->ssl_lock);
		ast_free(sc->ssl_lock);
		sc->ssl_lock = NULL;
	}
	if (sc->fd >= 0) {
		res = close(sc->fd);
		sc->fd = -1;
	}
	return res;
}

static const sccp_transport_t * const tls_destroy(uint8_t h)
{
	sccp_log(DEBUGCAT_SOCKET)(VERBOSE_PREFIX_1 "TLS: destroying\n");
	SSL_CTX_free(sslctx);
	sslctx = NULL;
	return NULL;
}

const sccp_transport_t tlstransport = {
	.name           = "TLS",
	.secret_default = "",
	.socktype       = SOCK_STREAM,
	.port_default   = "2443",

	.retrycountdefault        = 0,
	.retrycountmax            = 0,
	.retryintervaldefault     = REQUEST_RETRY_INTERVAL * REQUEST_RETRY_COUNT,
	.retryintervalmax         = 60,
	.duplicateintervaldefault = DUPLICATE_INTERVAL,

	.init     = tls_init,
	.bind     = tls_bind,
	.listen   = tls_listen,
	.accept   = tls_accept,
	.recv     = tls_recv,
	.pending  = tls_pending,
	.send     = tls_send,
	.shutdown = tls_shutdown,
	.close    = tls_close,
	.destroy  = tls_destroy,
};
#endif /* HAVE_LIBSSL */
// kate: indent-width 8; replace-tabs off; indent-mode cstyle; auto-insert-doxygen on; line-numbers on; tab-indents on; keep-extra-spaces off; auto-brackets off;
