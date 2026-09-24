/*!
 * \file	sccp_xml.h
 * \brief	SCCP XML Header
 * \author	Diederik de Groot <ddegroot [at] users.sf.net>
 * \date	2016-Nov-16
 * \note	This program is free software and may be modified and distributed under the terms of the GNU Public License.
 *		See the LICENSE file at the top of the source tree.
 */
#pragma once

#include "sccp_enum.h"
#include "forward_declarations.h"

__BEGIN_C_EXTERN__
typedef boolean_t (*sccp_webservice_callback_t)(const char * const uri, PBX_VARIABLE_TYPE * params, PBX_VARIABLE_TYPE * headers, pbx_str_t ** result);

typedef struct {
	boolean_t (* const isRunning)(void);
	const char * const (* const getBaseURL)(void);
	boolean_t (* const addHandler)(const char * const uri, sccp_webservice_callback_t callback, sccp_xml_outputfmt_t outputfmt);
	boolean_t (* const removeHandler)(const char * const uri);
} WebServiceInterface;

extern const WebServiceInterface iWebService;
__END_C_EXTERN__
