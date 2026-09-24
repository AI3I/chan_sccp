/*!
 * \file	sccp_webservice.c
 * \brief	SCCP WebService Class
 * \author	Diederik de Groot <ddegroot [at] users.sf.net>
 * \date	2016-Nov-16
 * \note	This program is free software and may be modified and distributed under the terms of the GNU Public License.
 *		See the LICENSE file at the top of the source tree.
 */
#include "config.h"
#include "common.h"

SCCP_FILE_VERSION(__FILE__, "");

#include "sccp_webservice.h"

#if defined(HAVE_PBX_HTTP_H) && defined(CS_EXPERIMENTAL_XML) && defined(HAVE_LIBXML2) && defined(HAVE_LIBXSLT) && defined(HAVE_LIBEXSLT_EXSLT_H)

#	include "sccp_utils.h"
#	include "sccp_vector.h"
#	include "sccp_xml.h"
#	include <asterisk/tcptls.h>
#	include <asterisk/http.h>
#	include <asterisk/paths.h>
#	include <asterisk/file.h>
#	include <asterisk/channel.h>                                 // sccp_webservice_parkedcalls: channel iteration
#	include <sys/stat.h>                                        // sccp_webservice_xslt_callback:stat

/* forward declarations */

/* private variables */
#	define MAX_PREFIX            80
#	define DEFAULT_PORT          8088
#	define DEFAULT_SESSION_LIMIT 100

static boolean_t running = FALSE;
static int       cookie_timeout;
static char      baseURL[StationMaxServiceURLSize] = { 0 };

typedef struct handler {
	char                       uri[StationMaxServiceURLSize];
	sccp_webservice_callback_t callback;
	sccp_xml_outputfmt_t       outputfmt;
} handler_t;

char * outputfmt2contenttype[] = {
	[SCCP_XML_OUTPUTFMT_NULL] = "text/plain",
	//[SCCP_XML_OUTPUTFMT_HTML] = "application/xhtml+xml",
	[SCCP_XML_OUTPUTFMT_HTML]  = "text/html",
	[SCCP_XML_OUTPUTFMT_XHTML] = "application/xhtml+xml",
	[SCCP_XML_OUTPUTFMT_XML]   = "application/xml",
	[SCCP_XML_OUTPUTFMT_CXML]  = "application/xml",
	[SCCP_XML_OUTPUTFMT_AJAX]  = "application/json",
	[SCCP_XML_OUTPUTFMT_JSON]  = "application/json",
	[SCCP_XML_OUTPUTFMT_TXT]   = "text/plain",
};

/*! \brief Limit the kinds of files we're willing to serve up */
static struct {
	const char * ext;
	const char * mtype;
} mimetypes[] = {
	//        { "html", "application/xhtml+xml" },
	//        { "htm", "application/xhtml+xml" },
	{ "html", "text/html" },        { "htm", "text/html" }, { "xml", "text/xml" },  { "xslt", "text/xsl" },  { "xsl", "text/xsl" },  { "js", "application/x-javascript" },
	{ "json", "application/json" }, { "css", "text/css" },  { "png", "image/png" }, { "jpg", "image/jpeg" }, { "gif", "image/gif" },
	//        { "wav", "audio/x-wav" },
	//        { "mp3", "audio/mpeg" },
	//        { "cnf", "text/plain" },
	//        { "cfg", "text/plain" },
	//        { "bin", "application/octet-stream" },
	//        { "sbn", "application/octet-stream" },
	//        { "ld", "application/octet-stream" },
};

static const char * ftype2mtype(const char * ftype)
{
	uint8_t x = 0;
	if (ftype) {
		for (x = 0; x < ARRAY_LEN(mimetypes); x++) {
			if (strcasecmp(ftype, mimetypes[x].ext) == 0) {
				return mimetypes[x].mtype;
			}
		}
	}
	return NULL;
}

SCCP_VECTOR_RW(sccp_uri_handler, handler_t) handlers;

#	define HANDLER_CB_CMP(elem, value) (sccp_strcaseequals((elem).uri, (value)))

/* private functions */
static boolean_t parse_manager_conf(void)
{
	boolean_t             result       = FALSE;
	struct ast_config *   cfg          = NULL;
	struct ast_variable * v            = NULL;
	struct ast_flags      config_flags = { 0 };

	int manager_enabled    = 0;
	int webmanager_enabled = 0;

	cfg = ast_config_load2("manager.conf", "http", config_flags);
	if (cfg && cfg != CONFIG_STATUS_FILEINVALID) {
		v = ast_variable_browse(cfg, "general");
		for (; v; v = v->next) {
			if (!strcasecmp(v->name, "enabled")) {
				manager_enabled = ast_true(v->value);
				if (!manager_enabled) {
					pbx_log(LOG_NOTICE, "SCCP: web service not started: manager.conf has enabled=no\n");
					break;
				}
			} else if (!strcasecmp(v->name, "webenabled")) {
				webmanager_enabled = ast_true(v->value);
				if (!webmanager_enabled) {
					pbx_log(LOG_NOTICE, "SCCP: web service not started: manager.conf has webenabled=no\n");
					break;
				}
			}
		}
		ast_config_destroy(cfg);
	} else {
		pbx_log(LOG_NOTICE, "SCCP: web service not started: manager.conf is missing or could not be parsed\n");
	}
	if (manager_enabled && webmanager_enabled) {
		result = TRUE;
	}
	return result;
}

static boolean_t parse_http_conf(char * const uri_str)
{
	boolean_t             result       = FALSE;
	struct ast_config *   cfg          = NULL;
	struct ast_variable * v            = NULL;
	struct ast_flags      config_flags = { 0 };

	int                   http_enabled       = 0;
	int                   bindport           = 0;
	struct ast_sockaddr * addr               = NULL;
	int                   num_addrs          = 0;
	char                  prefix[MAX_PREFIX] = "";

	cfg = ast_config_load2("http.conf", "http", config_flags);
	if (cfg && cfg != CONFIG_STATUS_FILEINVALID) {
		v = ast_variable_browse(cfg, "general");
		for (; v; v = v->next) {
			if (!strcasecmp(v->name, "enabled")) {
				http_enabled = ast_true(v->value);
				if (!http_enabled) {
					pbx_log(LOG_NOTICE, "SCCP: web service not started: http.conf has enabled=no\n");
					break;
				}
			} else if (!strcasecmp(v->name, "bindport")) {
				if (ast_parse_arg(v->value, (enum ast_parse_flags)(PARSE_UINT32 | PARSE_IN_RANGE | PARSE_DEFAULT), &bindport, DEFAULT_PORT, 0, 65535)) {
					pbx_log(LOG_WARNING, "SCCP: http.conf bindport '%s' is not a valid port; the web service assumes %" PRId32 "\n", v->value, DEFAULT_PORT);
					break;
				}
			} else if (!strcasecmp(v->name, "bindaddr") && !num_addrs) {
				if (!(num_addrs = ast_sockaddr_resolve(&addr, v->value, 0, AST_AF_UNSPEC))) {
					pbx_log(LOG_WARNING, "SCCP: http.conf bindaddr '%s' could not be resolved; web service not started\n", v->value);
					break;
				}
			} else if (!strcasecmp(v->name, "prefix")) {
				if (!ast_strlen_zero(v->value)) {
					prefix[0] = '/';
					ast_copy_string(prefix + 1, v->value, sizeof(prefix) - 1);
				} else {
					prefix[0] = '\0';
				}
			} else if (strcasecmp(v->name, "sessionlimit") == 0) {
				if (ast_parse_arg(v->value, (enum ast_parse_flags)(PARSE_UINT32 | PARSE_IN_RANGE | PARSE_DEFAULT), &cookie_timeout, DEFAULT_SESSION_LIMIT, 1, INT_MAX)) {
					pbx_log(LOG_WARNING, "SCCP: http.conf line %d: %s '%s' is not a valid number; default used\n", v->lineno, v->name, v->value);
				}
			}
		}
		ast_config_destroy(cfg);
		if (http_enabled && num_addrs) {
			snprintf(uri_str, StationMaxServiceURLSize, "http://%s:%d%s", ast_sockaddr_stringify_host(&addr[0]), bindport, prefix);
			result = TRUE;
		}
	} else {
		pbx_log(LOG_NOTICE, "SCCP: web service not started: http.conf is missing or could not be parsed\n");
	}
	if (addr) {
		sccp_free(addr);
	}
	return result;
}

typedef enum
{
	ServerSide,
	ClientSide,
} Process_XSLT_t;

static Process_XSLT_t parse_useragent(PBX_VARIABLE_TYPE * request_headers)
{
	Process_XSLT_t res        = ServerSide;
	char *         pos        = NULL;
	const char *   user_agent = sccp_retrieve_str_variable_byKey(request_headers, "User-Agent");
	sccp_log(DEBUGCAT_WEBSERVICE)(VERBOSE_PREFIX_3 "SCCP: web user agent %s\n", user_agent);
	do {
		sccp_log(DEBUGCAT_WEBSERVICE)(VERBOSE_PREFIX_3 "SCCP: matching the user agent\n");
		if (sccp_strlen_zero(user_agent)) {
			sccp_log(DEBUGCAT_WEBSERVICE)(VERBOSE_PREFIX_3 "SCCP: no user agent\n");
			break;
		}
		if (strcasestr(user_agent, "Allegro") || strcasestr(user_agent, "XSI-HTTPClient") || strcasestr(user_agent, "Cisco/SPA")) {
			sccp_log(DEBUGCAT_WEBSERVICE)(VERBOSE_PREFIX_3 "SCCP: user agent is a Cisco phone\n");
			break;
		}
		if (strcasestr(user_agent, "Firefox/") || strcasestr(user_agent, "SeaMonkey/")) {
			sccp_log(DEBUGCAT_WEBSERVICE)(VERBOSE_PREFIX_3 "SCCP: user agent is Firefox or SeaMonkey\n");
			res = ClientSide;
			break;
		}
		if ((pos = strcasestr(user_agent, "MSIE"))) {
			sccp_log(DEBUGCAT_WEBSERVICE)(VERBOSE_PREFIX_3 "SCCP: user agent is Internet Explorer\n");
			pos += 4;
			if (pos[0] == '/' && !(pos[1] == '1' || pos[1] == '2' || pos[1] == '3' || pos[1] == '4' || pos[1] == '5')) {
				sccp_log(DEBUGCAT_WEBSERVICE)(VERBOSE_PREFIX_3 "SCCP: Internet Explorer version is new enough\n");
				res = ClientSide;
			}
			break;
		}
		if ((pos = strcasestr(user_agent, "Safari/"))) {
			sccp_log(DEBUGCAT_WEBSERVICE)(VERBOSE_PREFIX_3 "SCCP: user agent is Internet Explorer\n");
			pos += 7;
			if (!(pos[1] == '1' || pos[1] == '2' || pos[1] == '3')) {
				sccp_log(DEBUGCAT_WEBSERVICE)(VERBOSE_PREFIX_3 "SCCP: Internet Explorer version is new enough\n");
				res = ClientSide;
			}
			break;
		}
		if ((pos = strcasestr(user_agent, "Opera"))) {
			sccp_log(DEBUGCAT_WEBSERVICE)(VERBOSE_PREFIX_3 "SCCP: user agent is Internet Explorer\n");
			pos += 5;
			if ((pos[0] == '/' || pos[0] == ' ') && !(pos[1] > '8')) {
				sccp_log(DEBUGCAT_WEBSERVICE)(VERBOSE_PREFIX_3 "SCCP: Internet Explorer version is new enough\n");
				res = ClientSide;
			}
			break;
		}
	} while (0);
	sccp_log(DEBUGCAT_WEBSERVICE)(VERBOSE_PREFIX_3 "SCCP: user agent %s: stylesheets applied on the %s\n", user_agent, res == ServerSide ? "server" : "client");
	return res;
}

/*!
 * \brief Look up the handler for this request's "handler" URI param.
 *
 * Returns a copy of the matched handler_t, not a pointer into &handlers'
 * backing storage - the vector is only protected by its lock while we hold
 * it here, and handler_t (fixed-size uri[], a function pointer, an enum) is
 * cheap to copy. Returning a live pointer instead would let a concurrent
 * iWebService.addHandler()/removeHandler() call (SCCP_VECTOR_APPEND can
 * realloc the backing array on growth) invalidate it while the caller is
 * still dereferencing handler->callback/uri/outputfmt after we've already
 * released the lock.
 *
 * \retval TRUE  *out was filled in with a valid handler copy
 * \retval FALSE no matching handler (or no "handler" param); *out untouched
 */
static boolean_t get_request_handler(PBX_VARIABLE_TYPE * request_params, handler_t * const out)
{
	const char * uri = sccp_retrieve_str_variable_byKey(request_params, "handler");

	if (!uri) {
		pbx_log(LOG_NOTICE, "SCCP: web request has no 'handler' parameter\n");
		return FALSE;
	}

	SCCP_VECTOR_RW_RDLOCK(&handlers);
	handler_t * found = (handler_t *)SCCP_VECTOR_GET_CMP(&handlers, uri, HANDLER_CB_CMP);
	if (found) {
		*out = *found;
	}
	SCCP_VECTOR_RW_UNLOCK(&handlers);

	if (!found) {
		pbx_log(LOG_NOTICE, "SCCP: web request asks for handler '%s', which does not exist\n", uri);
		return FALSE;
	}
	return TRUE;
}

static int parse_request_headers(PBX_VARIABLE_TYPE * request_headers, const char * locale)
{
	//locale = sccp_retrieve_str_variable_byKey(request_headers, "Accept-Language");

	return 0;
}

static int parse_outputfmt(PBX_VARIABLE_TYPE * request_params, PBX_VARIABLE_TYPE * request_headers, sccp_xml_outputfmt_t * outputfmt)
{
	// A real Cisco phone identifies itself this way regardless of any caller-supplied
	// default (e.g. request_parser() seeds *outputfmt with HTML for browser clients) -
	// detecting one always takes priority over that default.
	const char * user_agent = sccp_retrieve_str_variable_byKey(request_headers, "User-Agent");
	if (!sccp_strlen_zero(user_agent) && !strcasecmp(user_agent, "Allegro-Software-WebClient")) {
		*outputfmt = SCCP_XML_OUTPUTFMT_CXML;
		return 0;
	}

	const char * requested_outputfmt = sccp_retrieve_str_variable_byKey(request_params, "outformat");
	if (!sccp_strlen_zero(requested_outputfmt)) {
		sccp_xml_outputfmt_t parsed = sccp_xml_outputfmt_str2val(requested_outputfmt);
		if (!sccp_xml_outputfmt_exists(parsed)) {
			// unrecognized ?outformat=... value - do not let the SENTINEL flow on to
			// be used as an outputfmt2contenttype[] index (out of bounds: valid
			// indices are 0..SCCP_XML_OUTPUTFMT_SENTINEL-1)
			pbx_log(LOG_NOTICE, "SCCP: web request asks for outformat '%s', which is not supported\n", requested_outputfmt);
			return -1;
		}
		*outputfmt = parsed;
		return 0;
	}
	return (*outputfmt == SCCP_XML_OUTPUTFMT_NULL) ? -1 : 0;
}

/*
static int parse_request_params()
{
	return 0;
}

static int parse_request_uri()
{
	return 0;
}
*/

static __attribute__((malloc)) char * searchWebDirForFile(const char * filename, sccp_xml_outputfmt_t outputfmt, const char * extension)
{
	char filepath[PATH_MAX] = "";
	if (outputfmt && sccp_xml_outputfmt_exists(outputfmt)) {
		snprintf(filepath, sizeof(filepath), PBX_VARLIB "/sccpxslt/%s2%s.%s", filename, sccp_xml_outputfmt2str(outputfmt), extension);
	} else {
		snprintf(filepath, sizeof(filepath), PBX_VARLIB "/sccpxslt/%s.%s", filename, extension);
	}
	sccp_log(DEBUGCAT_WEBSERVICE)("SCCP: looking for %s\n", filepath);
	if (access(filepath, F_OK) == -1) {
		pbx_log(LOG_NOTICE, "SCCP: web file '%s' does not exist\n", filepath);
		filepath[0] = '\0';
		return NULL;
	}
	return pbx_strdup(filepath);
}

/*
static int addTranslation(PBX_VARIABLE_TYPE *request_params)
{
	int res = -1;
	char *translationFilename = searchWebDirForFile("translations", SCCP_XML_OUTPUTFMT_NULL, "xml");
	if (translationFilename) {
		//append_variable(request_params, "translationFile", pbx_strdup(translationFilename));
		//sccp_free(translationFilename);
		sccp_append_variable(request_params, "translationFile", translationFilename);
	}
	return res;
}
*/

static __attribute__((malloc)) char * findStylesheet(const char * const uri, sccp_xml_outputfmt_t outputfmt)
{
	return searchWebDirForFile(uri, outputfmt, "xsl");
}

static int request_parser(struct ast_tcptls_session_instance * ser, enum ast_http_method method, const char * request_uri, PBX_VARIABLE_TYPE * request_params, PBX_VARIABLE_TYPE * request_headers)
{
	int         result      = 0;
	pbx_str_t * http_header = NULL;
	pbx_str_t * out         = NULL;
	sccp_log(DEBUGCAT_WEBSERVICE)(VERBOSE_PREFIX_1 "SCCP: handling web request\n");

	handler_t handler;
	if (!get_request_handler(request_params, &handler)) {
		// get_request_handler() already logged the specific reason (missing
		// 'handler' param, or no registered handler matches it); this line adds
		// the request context (uri/remote address) that log line doesn't have.
		pbx_log(LOG_NOTICE, "SCCP: web request for '%s' from %s refused (reason in the previous message)\n",
			request_uri, ast_sockaddr_stringify(&ser->remote_address));
		// this is a malformed/unrecognized client request, not a server-side failure
		ast_http_error(ser, 404, "Not Found", "No SCCP XML service is registered for the requested 'handler' parameter.\n");
		return -1;
	}
	// Process_XSLT_t process_side = parse_useragent(request_headers);

	// sccp_xml_outputfmt_t outputfmt = handler->outputfmt;
	sccp_xml_outputfmt_t outputfmt = SCCP_XML_OUTPUTFMT_HTML;
	result |= parse_outputfmt(request_params, request_headers, &outputfmt);
	// const char *locale = NULL;
	// result |= parse_request_headers(request_headers, locale);
	// result |= parse_request_params();
	// result |= parse_request_uri();

	// if (DEBUG) {
	PBX_VARIABLE_TYPE * header = NULL;
	sccp_log(DEBUGCAT_WEBSERVICE)(VERBOSE_PREFIX_3 "request headers:\n");
	for (header = request_headers; header; header = header->next) {
		sccp_log(DEBUGCAT_WEBSERVICE)(VERBOSE_PREFIX_3 "SCCP: header %s: %s\n", header->name, header->value);
	}
	PBX_VARIABLE_TYPE * param = NULL;
	sccp_log(DEBUGCAT_WEBSERVICE)(VERBOSE_PREFIX_3 "request parameters:\n");
	for (param = request_params; param; param = param->next) {
		sccp_log(DEBUGCAT_WEBSERVICE)(VERBOSE_PREFIX_3 "SCCP: parameter %s: %s\n", param->name, param->value);
	}
	//}
	sccp_log(DEBUGCAT_WEBSERVICE)(VERBOSE_PREFIX_3 "SCCP: handler %p, result %d\n", handler, result);
	do {
		http_header = pbx_str_create(80);
		out         = pbx_str_create(4196);
		if (!http_header || !out) {
			pbx_log(LOG_ERROR, "SCCP: web request not answered: out of memory for the response\n");
			ast_http_error(ser, 500, "Server Error", "The server ran out of memory building this response. Try again; if this persists, check Asterisk's memory usage.\n");
			break;
		}

		if (result != 0) {
			pbx_log(LOG_NOTICE, "SCCP: web request for '%s' refused: its URI, headers or outformat could not be parsed (details in the previous message)\n", request_uri);
			ast_http_request_close_on_completion(ser);
			ast_http_error(ser, 400, "Bad Request", "The request's URI, headers, or 'outformat' parameter could not be parsed.\n");
			break;
		}
		if (!handler.callback(handler.uri, request_params, request_headers, &out)) {
			pbx_log(LOG_ERROR, "SCCP: web handler '%s' failed while building the response for '%s'\n", handler.uri, request_uri);
			ast_http_request_close_on_completion(ser);
			ast_http_error(ser, 500, "Server Error", "The matched SCCP XML service handler failed while building its response.\n");
			break;
		}
		sccp_log(DEBUGCAT_WEBSERVICE)(VERBOSE_PREFIX_3 "SCCP: handling %s from %s\n", request_uri, ast_sockaddr_stringify(&ser->remote_address));
		char           timebuf[80];
		struct timeval nowtv = ast_tvnow();
		struct ast_tm  now;
		ast_strftime(timebuf, sizeof(timebuf), "%a, %d %b %Y %H:%M:%S GMT", ast_localtime(&nowtv, &now, "GMT"));
		ast_str_set(&http_header, 0,
			    "Content-type: %s\r\n"
			    "Cache-Control: no-cache;\r\n"
			    "Set-Cookie: sccp_id=\"%08x\"; Version=1; Max-Age=%d\r\n"
			    "Pragma: SuppressEvents\r\n"
			    "Last-Modified: %s\r\n",
			    outputfmt2contenttype[outputfmt], 1, cookie_timeout, timebuf);
		// sccp_log(DEBUGCAT_WEBSERVICE) (VERBOSE_PREFIX_3 "SCCP: (request_parser) Returning Header:'%s'\n", pbx_str_buffer(http_header));

		ast_http_send(ser, method, 200, NULL, http_header, out, 0, 0);
		http_header = out = NULL;
		break;
	} while (0);

	if (http_header) {
		ast_free(http_header);
	}
	if (out) {
		ast_free(out);
	}
	return result;
}

static int sccp_webservice_callback(struct ast_tcptls_session_instance * ser, const struct ast_http_uri * urih, const char * uri, enum ast_http_method method, PBX_VARIABLE_TYPE * get_params, PBX_VARIABLE_TYPE * headers)
{
	struct ast_sockaddr   ser_remote_address_tmp;
	int                   retval = -1;
	struct ast_variable * params = get_params;

	if (running) {
		int param_count = 0, header_count = 0;
		for (PBX_VARIABLE_TYPE * v = get_params; v; v = v->next) {
			param_count++;
		}
		for (PBX_VARIABLE_TYPE * v = headers; v; v = v->next) {
			header_count++;
		}
		sccp_log((DEBUGCAT_WEBSERVICE))(VERBOSE_PREFIX_3 "SCCP: web %s request for '%s' (%d param%s, %d header%s)\n",
			method == AST_HTTP_POST ? "POST" : "GET", uri,
			param_count, param_count == 1 ? "" : "s", header_count, header_count == 1 ? "" : "s");
		if (method == AST_HTTP_POST) {
			params = ast_http_get_post_vars(ser, headers);
		}

		ast_sockaddr_copy(&ser_remote_address_tmp, &ser->remote_address);
		retval = request_parser(ser, method, uri, params, headers);
		ast_sockaddr_copy(&ser->remote_address, &ser_remote_address_tmp);
	}
	return retval;
}

static struct ast_http_uri sccp_webservice_uri = {
	.description = "SCCP Cisco IP HTTP Event Interface",
	.uri         = "sccp",
	.callback    = sccp_webservice_callback,
	.data        = NULL,
	.key         = __FILE__,
};

static int sccp_webservice_xslt_callback(struct ast_tcptls_session_instance * ser, const struct ast_http_uri * urih, const char * uri, enum ast_http_method method, struct ast_variable * get_vars,
					 struct ast_variable * headers)
{
	char *           path  = NULL;
	const char *     ftype = NULL;
	const char *     mtype = NULL;
	char             wkspace[80];
	struct stat      st;
	int              len         = 0;
	int              fd          = 0;
	struct ast_str * http_header = NULL;
	struct timeval   tv;
	struct ast_tm    tm;
	char             timebuf[80];

	char                  etag[23];
	struct ast_variable * v            = NULL;
	int                   not_modified = 0;

	if (method != AST_HTTP_GET && method != AST_HTTP_HEAD) {
		ast_http_error(ser, 501, "Not Implemented", "Attempt to use unimplemented / unsupported method");
		return 0;
	}

	/* Disallow any funny filenames at all (checking first character only??) */
	if ((uri[0] < 33) || strchr("./|~@#$%^&*() \t", uri[0])) {
		goto out403;
	}

	if (strstr(uri, "/..")) {
		goto out403;
	}

	if ((ftype = strrchr(uri, '.'))) {
		ftype++;
	}

	if (!(mtype = ftype2mtype(ftype))) {
		snprintf(wkspace, sizeof(wkspace), "text/%s", S_OR(ftype, "plain"));
		mtype = wkspace;
	}

	/* Cap maximum length */
	len = strlen(uri) + strlen(ast_config_AST_DATA_DIR) + strlen("/sccpxslt/") + 5;
	if (len > 1024) {
		goto out403;
	}

	path = (char *)sccp_alloca(len);
	snprintf(path, len, "%s/sccpxslt/%s", ast_config_AST_DATA_DIR, uri);

	if (strstr(path, "/private/") && !astman_is_authed(ast_http_manid_from_vars(headers))) {
		goto out403;
	}

	fd = open(path, O_RDONLY);
	if (fd == -1) {
		/* open file before checking failure cause, to prevent TOCTOU */
		if (stat(path, &st) == -1 || !S_ISREG(st.st_mode)) {
			goto out404;
		}
		goto out403;
	}

	if (fstat(fd, &st) == -1) {
		close(fd);
		goto out403;
	}

	/* make "Etag:" http header value */
	snprintf(etag, sizeof(etag), "\"%ld\"", (long)st.st_mtime);

	/* make "Last-Modified:" http header value */
	tv.tv_sec  = st.st_mtime;
	tv.tv_usec = 0;
	ast_strftime(timebuf, sizeof(timebuf), "%a, %d %b %Y %H:%M:%S GMT", ast_localtime(&tv, &tm, "GMT"));

	/* check received "If-None-Match" request header and Etag value for file */
	for (v = headers; v; v = v->next) {
		if (strcasecmp(v->name, "If-None-Match") == 0) {
			if (strcasecmp(v->value, etag) == 0) {
				not_modified = 1;
			}
			break;
		}
	}

	http_header = ast_str_create(255);
	if (!http_header) {
		ast_http_request_close_on_completion(ser);
		ast_http_error(ser, 500, "Server Error", "Out of memory");
		close(fd);
		return 0;
	}

	ast_str_set(&http_header, 0,
		    "Content-type: %s\r\n"
		    "ETag: %s\r\n"
		    "Last-Modified: %s\r\n",
		    mtype, etag, timebuf);

	/* ast_http_send() frees http_header, so we don't need to do it before returning */
	sccp_log(DEBUGCAT_WEBSERVICE)(VERBOSE_PREFIX_3 "service %s = %s (%s)\n", uri, path, mtype);
	if (not_modified) {
		ast_http_send(ser, method, 304, "Not Modified", http_header, NULL, 0, 1);
	} else {
		ast_http_send(ser, method, 200, NULL, http_header, NULL, fd, 1); /* static content flag is set */
	}
	close(fd);
	return 0;

out404:
	pbx_log(LOG_NOTICE, "SCCP: web request for '%s' answered 404: not found\n", uri);
	ast_http_error(ser, 404, "Not Found", "The requested URL was not found on this server.");
	return 0;

out403:
	ast_http_request_close_on_completion(ser);
	pbx_log(LOG_NOTICE, "SCCP: web request for '%s' answered 403: access denied\n", uri);
	ast_http_error(ser, 403, "Access Denied", "You do not have permission to access the requested URL.");
	return 0;
}

static struct ast_http_uri sccp_webservice_xslt_uri = {
	.description = "SCCP Cisco IP HTTP Event Interface",
	.uri         = "sccpxslt",
	.callback    = sccp_webservice_xslt_callback,
	.has_subtree = 1,
	.data        = NULL,
	.key         = __FILE__,
};

static boolean_t xmlPostProcess(xmlDoc * const doc, const char * const uri, PBX_VARIABLE_TYPE * params, PBX_VARIABLE_TYPE * headers, char ** resultstr)
{
	boolean_t            res          = TRUE;
	Process_XSLT_t       process_side = parse_useragent(headers);
	sccp_xml_outputfmt_t outputfmt    = SCCP_XML_OUTPUTFMT_XHTML;
	const char *         locale       = NULL;
	parse_outputfmt(params, headers, &outputfmt);
	parse_request_headers(headers, locale);

	if (outputfmt && iXML.applyStyleSheetByName) {
		// addTranslation(params);
		// sccp_append_variable(params, "locales", locale ? pbx_strdup(locale) : "en");
		if (process_side == ServerSide) {
			sccp_log(DEBUGCAT_WEBSERVICE)(VERBOSE_PREFIX_3 "SCCP: applying stylesheet on the server\n");
			char * stylesheetFilename = findStylesheet(uri, outputfmt);
			if (stylesheetFilename) {
				if (!iXML.applyStyleSheetByName(doc, stylesheetFilename, resultstr)) {
					pbx_log(LOG_ERROR, "SCCP: web handler '%s': applying stylesheet '%s' failed (details in the previous message)\n", uri, stylesheetFilename);
					res = FALSE;
				}
				sccp_log(DEBUGCAT_WEBSERVICE)(VERBOSE_PREFIX_3 "SCCP: result %s\n", *resultstr ? *resultstr : "");
				sccp_free(stylesheetFilename);
			} else {
				pbx_log(LOG_ERROR, "SCCP: web handler '%s': stylesheet %s2%s.xsl not found in " PBX_VARLIB "/sccpxslt/\n",
					uri, uri, sccp_xml_outputfmt2str(outputfmt));
				res = FALSE;
			}
		} else {
			sccp_log(DEBUGCAT_WEBSERVICE)(VERBOSE_PREFIX_3 "SCCP: stylesheet left to the client\n");
			*resultstr = iXML.dump(doc, TRUE);
			res = *resultstr != NULL;
		}
	}
	return res;
}

/*!
 * \brief Webservice handler for the "parkedcalls" URI.
 *
 * Builds a <response><generic event="ParkedCall" .../></response> document
 * matching the shape parkedcalls2cxml.xsl already expects (mirrors what an
 * AMI ParkedCalls action would return), by iterating currently active
 * channels and filtering to the "parkedcalls" context. There is no core
 * Asterisk API to query current parking-lot occupancy directly (parking.h
 * only exposes the park action and an event-based Stasis topic, not a
 * point-in-time snapshot), so channel iteration + context filtering is the
 * same technique used by AMI/CLI parking introspection internally.
 */
static boolean_t sccp_webservice_parkedcalls(const char * const uri, PBX_VARIABLE_TYPE * params, PBX_VARIABLE_TYPE * headers, pbx_str_t ** result)
{
	sccp_log(DEBUGCAT_WEBSERVICE)(VERBOSE_PREFIX_3 "SCCP: parked calls web service\n");

	xmlDoc *  doc  = iXML.createDoc();
	xmlNode * root = iXML.createNode("response");
	iXML.setRootElement(doc, root);

	struct ast_channel_iterator * iter = ast_channel_iterator_all_new();
	if (iter) {
		struct ast_channel * chan;
		while ((chan = ast_channel_iterator_next(iter))) {
			ast_channel_lock(chan);
			if (sccp_strcaseequals(ast_channel_context(chan), "parkedcalls")) {
				struct ast_party_caller *          caller    = ast_channel_caller(chan);
				struct ast_party_connected_line *  connected = ast_channel_connected(chan);

				xmlNode * entry = iXML.addElement(root, "generic", NULL);
				iXML.addProperty(entry, "event", "%s", "ParkedCall");
				iXML.addProperty(entry, "exten", "%s", ast_channel_exten(chan));
				iXML.addProperty(entry, "calleridname", "%s", (caller && caller->id.name.str) ? caller->id.name.str : "");
				iXML.addProperty(entry, "calleridnum", "%s", (caller && caller->id.number.str) ? caller->id.number.str : "");
				iXML.addProperty(entry, "connectedlinename", "%s", (connected && connected->id.name.str) ? connected->id.name.str : "");
			}
			ast_channel_unlock(chan);
			chan = ast_channel_unref(chan);
		}
		ast_channel_iterator_destroy(iter);
	}

	char *     resultstr = NULL;
	boolean_t  res        = xmlPostProcess(doc, uri, params, headers, &resultstr);
	if (res && resultstr) {
		pbx_str_append(result, 0, "%s", resultstr);
		sccp_free(resultstr);
	}
	iXML.destroyDoc(&doc);

	return res;
}

static void __attribute__((constructor)) init_webservice(void)
{
	if (!running && parse_manager_conf() && parse_http_conf(baseURL)) {
		SCCP_VECTOR_RW_INIT(&handlers, 1);

		iWebService.addHandler("parkedcalls", sccp_webservice_parkedcalls, SCCP_XML_OUTPUTFMT_CXML);

		ast_http_uri_link(&sccp_webservice_uri);
		ast_http_uri_link(&sccp_webservice_xslt_uri);
		running = TRUE;
	}
}

static void __attribute__((destructor)) destroy_webservice(void)
{
	if (running) {
		ast_http_uri_unlink(&sccp_webservice_xslt_uri);
		ast_http_uri_unlink(&sccp_webservice_uri);
		SCCP_VECTOR_RW_WRLOCK(&handlers);
		SCCP_VECTOR_RESET(&handlers, SCCP_VECTOR_ELEM_CLEANUP_NOOP);
		SCCP_VECTOR_RW_UNLOCK(&handlers);
		SCCP_VECTOR_RW_FREE(&handlers);
		running = FALSE;
	}
}

/* exported functions */
static boolean_t isRunning(void)
{
	return running;
}

static const char * const getBaseURL(void)
{
	return baseURL;
}

static boolean_t addHandler(const char * const uri, sccp_webservice_callback_t callback, sccp_xml_outputfmt_t outputfmt)
{
	boolean_t result  = FALSE;
	handler_t handler = {
		.uri       = "",
		.callback  = callback,
		.outputfmt = outputfmt,
	};
	sccp_copy_string(handler.uri, uri, sizeof(handler.uri));

	SCCP_VECTOR_RW_WRLOCK(&handlers);
	if (SCCP_VECTOR_APPEND(&handlers, handler) == 0) {
		result = TRUE;
	}
	SCCP_VECTOR_RW_UNLOCK(&handlers);
	return result;
}

static boolean_t removeHandler(const char * const uri)
{
	boolean_t result = FALSE;
	SCCP_VECTOR_RW_WRLOCK(&handlers);
	if (SCCP_VECTOR_REMOVE_CMP_UNORDERED(&handlers, uri, HANDLER_CB_CMP, SCCP_VECTOR_ELEM_CLEANUP_NOOP) == 0) {
		sccp_log(DEBUGCAT_WEBSERVICE)(VERBOSE_PREFIX_1 "SCCP: web handler for %s removed\n", uri);
		result = TRUE;
	}
	SCCP_VECTOR_RW_UNLOCK(&handlers);

	return result;
}

/* Assign to interface */
const WebServiceInterface iWebService = {
	.isRunning     = isRunning,
	.getBaseURL    = getBaseURL,
	.addHandler    = addHandler,
	.removeHandler = removeHandler,
};
#else
const WebServiceInterface iWebService = { 0 };
#endif
