/*!
 * \file        sccp_devstate.h
 * \brief       SCCP device state Header
 * \author      Marcello Ceschia <marcelloceschia [at] users.sourceforge.net>
 * \note        This program is free software and may be modified and distributed under the terms of the GNU General Public License, version 2 or (at your option) any later version.
 *              See the LICENSE file at the top of the source tree.
 * \since       2013-08-15
 */
#pragma once

__BEGIN_C_EXTERN__
#ifdef CS_DEVSTATE_FEATURE
SCCP_API void SCCP_CALL sccp_devstate_module_start(void);
SCCP_API void SCCP_CALL sccp_devstate_module_stop(void);
SCCP_API enum ast_device_state SCCP_CALL sccp_devstate_getNextDeviceState(constDevicePtr d, sccp_buttonconfig_t * config);
#endif
__END_C_EXTERN__
