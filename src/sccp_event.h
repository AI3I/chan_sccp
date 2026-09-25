/*!
 * \file	sccp_event.h
 * \brief       SCCP Event Header
 * \author      Marcello Ceschia <marcelloceschia [at] users.sourceforge.net>
 * \note	This program is free software and may be modified and distributed under the terms of the GNU General Public License, version 2 or (at your option) any later version.
 *		See the LICENSE file at the top of the source tree.
 * \since       2009-09-02
 */
#pragma once

__BEGIN_C_EXTERN__
typedef struct sccp_event {
	union {
		struct {
			sccp_line_t *line;									/*!< SCCP Line (required) */
		} lineInstance;
		struct {
			sccp_device_t *device;									/*!< SCCP Device (required) */
		} deviceRegistered;
		struct {
			sccp_linedevice_t * ld;                                                                 /*!< SCCP device line (required) */
		} deviceAttached;
		struct {
			sccp_device_t *device;									/*!< SCCP device (required) */
			sccp_linedevice_t * optional_linedevice;
			sccp_feature_type_t featureType;							/*!< what feature is changed (required) */
		} featureChanged;
		struct {
			sccp_line_t * line;                                                                     /*!< SCCP line (required) */
			sccp_device_t *optional_device;
			uint8_t state;										/*!< state (required) */
		} lineStatusChanged;
#if CS_TEST_FRAMEWORK
		struct {
			uint32_t value;
			char *str;
		} TestEvent;
#endif
	};
	sccp_event_type_t type;
} sccp_event_t;

typedef void (*sccp_event_callback_t) (const sccp_event_t * event);

SCCP_API void SCCP_CALL sccp_event_module_start(void);
SCCP_API boolean_t SCCP_CALL sccp_event_subscribe(int eventType, sccp_event_callback_t cb, boolean_t allowAsyncExecution);
SCCP_API sccp_event_t * SCCP_CALL sccp_event_allocate(sccp_event_type_t eventType);
SCCP_API boolean_t SCCP_CALL      _sccp_event_fire(sccp_event_t * event, boolean_t forceSync);
#define sccp_event_fire(_event)     _sccp_event_fire(_event, FALSE)
#define sccp_event_syncFire(_event) _sccp_event_fire(_event, TRUE)
SCCP_API boolean_t SCCP_CALL sccp_event_unsubscribe(int eventType, sccp_event_callback_t cb);
SCCP_API void SCCP_CALL sccp_event_module_stop(void);
__END_C_EXTERN__
