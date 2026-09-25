/*!
 * \file	forward_declarations.h
 * \brief	Forward Declarations
 * \author	Diederik de Groot <ddegroot [at] users.sourceforge.net>
 * \note	This program is free software and may be modified and distributed under the terms of the GNU General Public License, version 2 or (at your option) any later version.
 *		See the LICENSE file at the top of the source tree.
 */
#pragma once

__BEGIN_C_EXTERN__
#if defined(HAVE_UNALIGNED_BUSERROR)
typedef unsigned long sccp_group_t;
#else
typedef ULONG sccp_group_t;
#endif

typedef struct sccp_session sccp_session_t;
typedef struct sccp_linedevice sccp_linedevice_t;
typedef struct sccp_device sccp_device_t;
typedef struct sccp_line sccp_line_t;
typedef struct sccp_channel sccp_channel_t;
typedef struct sccp_speed sccp_speed_t;
typedef struct sccp_service sccp_service_t;
typedef struct sccp_addon sccp_addon_t;
typedef struct sccp_hint sccp_hint_t;
typedef struct sccp_hostname sccp_hostname_t;
typedef struct sccp_rtp sccp_rtp_t;
typedef struct sccp_header sccp_header_t;                                                //!< Skinny Protocol Structure
typedef struct sccp_msg sccp_msg_t;

#define sessionPtr sccp_session_t *const
#define devicePtr sccp_device_t *const
#define linePtr sccp_line_t *const
#define channelPtr sccp_channel_t *const
#define lineDevicePtr sccp_linedevice_t * const
#define conferencePtr sccp_conference_t *const
#define rtpPtr        sccp_rtp_t * const
#define messagePtr    sccp_msg_t * const
#define constSessionPtr const sccp_session_t *const
#define constDevicePtr const sccp_device_t *const
#define constLinePtr const sccp_line_t *const
#define constChannelPtr const sccp_channel_t *const
#define constLineDevicePtr const sccp_linedevice_t * const
#define constConferencePtr const sccp_conference_t *const
#define constRtpPtr        const sccp_rtp_t * const
#define constMessagePtr const sccp_msg_t * const
#ifdef CS_DEVSTATE_FEATURE
typedef struct sccp_devstate_specifier sccp_devstate_specifier_t;
#endif
typedef struct sccp_feature_configuration sccp_featureConfiguration_t;
typedef struct sccp_selectedchannel sccp_selectedchannel_t;
typedef struct sccp_ast_channel_name sccp_ast_channel_name_t;
typedef struct sccp_buttonconfig sccp_buttonconfig_t;
typedef struct sccp_hotline sccp_hotline_t;
typedef struct sccp_callinfo sccp_callinfo_t;
typedef struct sccp_call_statistics sccp_call_statistics_t;
typedef struct softKeySetConfiguration sccp_softKeySetConfiguration_t;
typedef struct sccp_mailbox sccp_mailbox_t;
typedef struct subscriptionId sccp_subscription_id_t;
typedef struct sccp_conference sccp_conference_t;
typedef struct sccp_private_channel_data sccp_private_channel_data_t;
typedef struct sccp_private_device_data sccp_private_device_data_t;
typedef struct sccp_cfwd_information sccp_cfwd_information_t;
typedef struct sccp_buttonconfig_list sccp_buttonconfig_list_t;
typedef struct sccp_threadpool sccp_threadpool_t;
typedef struct _xmlDoc xmlDoc;
typedef struct _xmlNode xmlNode;

#ifndef SOLARIS
#  if defined __STDC__ && defined __STDC_VERSION__ && __STDC_VERSION__ >= 199901L
typedef _Bool boolean_t;
#    define FALSE false
#    define TRUE true
#  else
#define TRUE 1
#define FALSE 0
typedef bool boolean_t;
#  endif
#else
#  define FALSE B_FALSE
#  define TRUE B_TRUE
#endif

#define do_expect(_x) __builtin_expect(_x,1)
#define dont_expect(_x) __builtin_expect(_x,0)
#if defined(CCC_ANALYZER)
#define NONENULL(...) __attribute__((nonnull(__VA_ARGS__)))
#else
#define NONENULL(...)
#endif

typedef void sk_func(sccp_device_t * d, sccp_line_t * l, sccp_channel_t * c);
typedef int (*sccp_sched_cb) (const void *data);
__END_C_EXTERN__
