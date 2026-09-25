/*!
 * \file	sccp_xml.c
 * \brief	SCCP XML Class
 * \author	Diederik de Groot <ddegroot [at] users.sourceforge.net>
 * \note	This program is free software and may be modified and distributed under the terms of the GNU Public License.
 *		See the LICENSE file at the top of the source tree.
 */
#include <config.h>
#include "common.h"
#include "sccp_xml.h"

SCCP_FILE_VERSION(__FILE__, "")

#if defined(CS_EXPERIMENTAL_XML) && defined(HAVE_LIBXML2) && defined(HAVE_LIBXSLT) && defined(HAVE_LIBEXSLT_EXSLT_H)
#	include "sccp_utils.h"

#	include <asterisk/paths.h>

#	if HAVE_LIBXML2
#		include <libxml/tree.h>
#		include <libxml/xinclude.h>
#	endif

#	if HAVE_LIBXSLT
#		include <libxslt/xslt.h>
#		include <libxslt/xsltInternals.h>
#		include <libxslt/transform.h>
#		include <libxslt/xsltutils.h>
#		include <libxslt/extensions.h>
#		if HAVE_LIBEXSLT_EXSLT_H
#			include <libexslt/exslt.h>
#		endif
#	endif

static __attribute__((malloc)) xmlDoc * createDoc(void)
{
	xmlDoc * doc = xmlNewDoc((const xmlChar *)"1.0");
	sccp_log(DEBUGCAT_WEBSERVICE)(VERBOSE_PREFIX_2 "SCCP: XML document %p created\n", doc);
	return doc;
}

static __attribute__((malloc)) xmlDoc * createDocFromStr(const char * inbuf, int length)
{
	int      options = 0;
	xmlDoc * doc     = xmlReadMemory(inbuf, length, "noname.xml", NULL, options);
	sccp_log(DEBUGCAT_WEBSERVICE)(VERBOSE_PREFIX_2 "SCCP: XML document %p parsed\n", doc);
	return doc;
}

static __attribute__((malloc)) xmlDoc * createDocFromPbxStr(const pbx_str_t * inbuf)
{
	return createDocFromStr(pbx_str_buffer(inbuf), pbx_str_size(inbuf));
}

static xmlNode * createNode(const char * const name)
{
	xmlNode * node = xmlNewNode(NULL, (const xmlChar *)name);
	sccp_log(DEBUGCAT_WEBSERVICE)(VERBOSE_PREFIX_2 "SCCP: XML element %s = %p\n", name, node);
	return node;
}

static xmlNode * addElement(xmlNode * const parentNode, const char * const name, const char * const content)
{
	xmlNode * node = xmlNewChild(parentNode, NULL, (const xmlChar *)name, (const xmlChar *)content);
	sccp_log(DEBUGCAT_WEBSERVICE)(VERBOSE_PREFIX_2 "SCCP: XML element %p/%s '%s' = %p\n", parentNode, name, content, node);
	return node;
}

static void __attribute__((format(printf, 3, 4))) addProperty(xmlNode * const parentNode, const char * const key, const char * const format, ...)
{
	va_list args;
	va_start(args, format);
	char tmp[DEFAULT_PBX_STR_BUFFERSIZE];
	vsnprintf(tmp, DEFAULT_PBX_STR_BUFFERSIZE, format, args);
	va_end(args);

	xmlNewProp(parentNode, (const xmlChar *)key, (const xmlChar *)tmp);
}

static void setRootElement(xmlDoc * const doc, xmlNode * const node)
{
	sccp_log(DEBUGCAT_WEBSERVICE)(VERBOSE_PREFIX_2 "SCCP: XML document %p root %p\n", doc, node);
	xmlDocSetRootElement(doc, node);
}

static __attribute__((malloc)) char * dump(xmlDoc * const doc, boolean_t indent)
{
	xmlChar *xml_output = NULL;
	int    output_len = 0;
	xmlDocDumpFormatMemoryEnc(doc, &xml_output, &output_len, "UTF-8", indent ? 1 : 0);
	char *output = xml_output ? pbx_strdup((const char *)xml_output) : NULL;
	if (xml_output)
		xmlFree(xml_output);

	sccp_log(DEBUGCAT_WEBSERVICE)(VERBOSE_PREFIX_2 "SCCP: XML document %p:\n%s\n", doc, output ? output : "");
	return output;
}

#	if defined(HAVE_LIBXSLT) && defined(HAVE_LIBEXSLT_EXSLT_H)

static boolean_t applyStyleSheetByName(xmlDoc * const doc, const char * const styleSheetFilename, char **result)
{
	boolean_t res = FALSE;
	const char *params[] = { "locales", "en", NULL };
	*result = NULL;

	if (xmlXIncludeProcess(doc) < 0) {
		return res;
	}

	if (styleSheetFilename) {
		xsltStylesheet * const xslt = xsltParseStylesheetFile((const xmlChar *)styleSheetFilename);
		if (!xslt) {
			// malformed/unparseable .xsl file - xsltApplyStylesheet() would otherwise
			// be handed a NULL stylesheet and crash the process on an ordinary request
			pbx_log(LOG_ERROR, "SCCP: XSL stylesheet '%s' could not be parsed; the page was not rendered\n", styleSheetFilename);
			return res;
		}
		xmlDoc * const newdoc = xsltApplyStylesheet(xslt, doc, params);
		if (newdoc) {
			int output_len = 0;
			xmlChar *xml_output = NULL;
			xmlDocDumpFormatMemoryEnc(newdoc, &xml_output, &output_len, "UTF-8", 1);
			if (xml_output) {
				*result = pbx_strdup((const char *)xml_output);
				xmlFree(xml_output);
			}
			if (*result)
				sccp_log(DEBUGCAT_WEBSERVICE)(VERBOSE_PREFIX_3 "stylesheet result: '%s'\n", *result);
			xmlFreeDoc(newdoc);
			res = *result != NULL;
		}
		xsltFreeStylesheet(xslt);
	}

	return res;
}
#	endif

static void destroyDoc(xmlDoc **doc)
{
	if (doc && *doc) {
		xmlFreeDoc(*doc);
		*doc = NULL;
	}
}

static void __attribute__((constructor)) init_xml(void)
{
	xmlInitParser();
	exsltRegisterAll();
}

const XMLInterface iXML = {
	.createDoc           = createDoc,
	.createDocFromStr    = createDocFromStr,
	.createDocFromPbxStr = createDocFromPbxStr,
	.createNode          = createNode,
	.addElement          = addElement,
	.addProperty         = addProperty,
	.setRootElement      = setRootElement,

#	if defined(HAVE_LIBXSLT) && defined(HAVE_LIBEXSLT_EXSLT_H)
	.applyStyleSheetByName = applyStyleSheetByName,
#	endif
	.dump       = dump,
	.destroyDoc = destroyDoc,
};

#	if CS_TEST_FRAMEWORK
#		include <asterisk/test.h>
AST_TEST_DEFINE(sccp_xml_test)
{
	switch (cmd) {
		case TEST_INIT:
			info->name        = "xml";
			info->category    = "/channels/chan_sccp/";
			info->summary     = "xml tests";
			info->description = "chan-sccp-b xml tests";
			return AST_TEST_NOT_RUN;
		case TEST_EXECUTE:
			break;
	}

	xmlDoc * doc = iXML.createDoc();
	pbx_test_validate(test, doc != NULL);

	xmlNode * root = iXML.createNode("root");
	iXML.setRootElement(doc, root);
	pbx_test_validate(test, root != NULL);

	xmlNode * group = iXML.addElement(root, "group", NULL);
	pbx_test_validate(test, group != NULL);

	xmlNode * val1 = iXML.addElement(group, "val1", "content1");
	iXML.addProperty(val1, "type", "%s", "text_value");
	iXML.addElement(group, "val2", NULL);
	iXML.addElement(group, "val3", NULL);

	char * check = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<root><group><val1 type=\"text_value\">content1</val1><val2/><val3/></group></root>\n";

	char * result = iXML.dump(doc, FALSE);
	pbx_test_validate(test, 0 == strcmp(check, result));
	sccp_free(result);

	iXML.destroyDoc(&doc);

	return AST_TEST_PASS;
}

static void __attribute__((constructor)) sccp_register_tests(void)
{
	AST_TEST_REGISTER(sccp_xml_test);
}

static void __attribute__((destructor)) sccp_unregister_tests(void)
{
	AST_TEST_UNREGISTER(sccp_xml_test);
}
#	endif

#else
const XMLInterface iXML = { 0 };
#endif                                        // defined(CS_EXPERIMENTAL_XML)
