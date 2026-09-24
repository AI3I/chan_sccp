/*!
 * \file        sccp_cli.c
 * \brief       SCCP CLI Class
 * \author      Sergio Chersovani <mlists [at] c-net.it>
 * \note        Reworked, but based on chan_sccp code.
 *              The original chan_sccp driver that was made by Zozo which itself was derived from the chan_skinny driver.
 *              Modified by Jan Czmok and Julien Goodwin
 * \note        This program is free software and may be modified and distributed under the terms of the GNU Public License.
 *              See the LICENSE file at the top of the source tree.
 *
 */

/*!
 * \remarks
 * Purpose:     SCCP CLI
 * When to use: Only methods directly related to the asterisk cli interface should be stored in this source file.
 * Relations:   Calls ???
 *
 * how to use the cli macro's
 * /code
 * static char cli_message_device_usage[] = "Usage: sccp message device <device> <text> [beep] [timeout]\n" ...;
 * \#define CLI_COMMAND "sccp", "message", "device"      // the CLI words before the arguments
 * \#define CLI_COMPLETE SCCP_CLI_DEVICE_COMPLETER        // tab completion helpers, one per argument position
 * CLI_AMI_ENTRY(message_device, sccp_message_device, "Show a message on one phone", cli_message_device_usage, FALSE, FALSE)
 *                                                       // generates cli_message_device(); elements: name, handler,
 *                                                       // description, usage, completer repeats, AMI event list
 * SCCP_AMI_ACTION(message_device, sccp_message_device, "SCCPMessageDevice", FALSE,
 *                 "sccp", "message", "device", "$Device", "$Text", "$Beep", "$Timeout")
 *                                                       // generates manager_message_device(); the variadic part is
 *                                                       // the argv passed to the handler, "$Name" = AMI header Name
 * \#undef CLI_COMPLETE
 * \#undef CLI_COMMAND
 * /endcode
 *
 * Handlers return RESULT_SUCCESS / RESULT_SHOWUSAGE, or finish with CLI_AMI_RETURN_DONE (success text) or
 * CLI_AMI_RETURN_ERROR (error text); both print on the CLI or send the matching AMI response.
 * 
 * Inside the function that which is called on execution:
 *  - If s!=NULL we know it is an AMI calls, if m!=NULL it is a CLI call.
 *  - we need to add local_total which get's set to the number of lines returned (for ami calls).
 *  - We need to return RESULT_SUCCESS (for cli calls) at the end. If we set CLI_AMI_RETURN_ERROR, we will exit the function immediately and return RESULT_FAILURE. We need to make sure that all references are released before sending CLI_AMI_RETURN_ERROR.
 *  .
 */

#include "config.h"
#include "common.h"
#include "sccp_channel.h"
#include "sccp_cli.h"
#include "sccp_actions.h"
#include "sccp_softkeys.h"

SCCP_FILE_VERSION(__FILE__, "");

#include "sccp_device.h"
#include "sccp_line.h"
#include "sccp_linedevice.h"
#include "sccp_session.h"
#include "sccp_conference.h"
#include "sccp_utils.h"
#include "sccp_config.h"
#include "sccp_feature.h"
#include "sccp_mwi.h"
#include "sccp_hint.h"
#include "sccp_labels.h"
#include "sccp_threadpool.h"
#include "sccp_indicate.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <asterisk/cli.h>
#include <asterisk/paths.h>
#include <asterisk/localtime.h>

static void cli_table_write(void *context, const char *text)
{
	pbx_cli(*(int *)context, "%s", text);
}

void sccp_cli_table_print(sccp_cli_table_data_t *table, int fd, const char *title)
{
	sccp_cli_table_render(table, title, cli_table_write, &fd);
	sccp_cli_table_destroy(table);
}

/*** DOCUMENTATION
	<manager name="SCCPShowGlobals" language="en_US">
		<synopsis>Show the global SCCP settings.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
		</syntax>
		<description>
			<para>Returns the [general] settings of sccp.conf as they are in effect, one header per setting. Same as the CLI command <literal>sccp show globals</literal>.</para>
		</description>
	</manager>
	<manager name="SCCPShowDevices" language="en_US">
		<synopsis>List SCCP devices.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
			<parameter name="Filter">
				<para>Only list some devices.</para>
				<enumlist>
					<enum name="registered"/>
					<enum name="unregistered"/>
					<enum name="model"><para>Model name contains Value.</para></enum>
					<enum name="line"><para>Device has line Value.</para></enum>
					<enum name="firmware"><para>Reported firmware contains Value.</para></enum>
				</enumlist>
			</parameter>
			<parameter name="Value">
				<para>Text for the model, line and firmware filters.</para>
			</parameter>
		</syntax>
		<description>
			<para>Returns one SCCPDeviceEntry event per matching device, with its address, registration state, model and firmware, followed by SCCPShowDevicesComplete.</para>
		</description>
	</manager>
	<manager name="SCCPShowDeviceCalls" language="en_US">
		<synopsis>Show the quality of a device's last calls.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
			<parameter name="Device" required="true">
				<para>Device name.</para>
			</parameter>
		</syntax>
		<description>
			<para>Returns one SCCPDeviceCallEntry event per call, newest first (the last 20 calls since the module loaded), with the packet counts, loss, jitter, latency, MOS and concealment the phone reported at the end of the call, followed by SCCPShowDeviceCallsComplete.</para>
		</description>
	</manager>
	<manager name="SCCPGenerateCnf" language="en_US">
		<synopsis>Write a phone's TFTP configuration file.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
			<parameter name="Device" required="true"><para>Device name.</para></parameter>
			<parameter name="File"><para>File or directory to write to; default &lt;device&gt;.cnf.xml in the Asterisk configuration directory.</para></parameter>
			<parameter name="Server"><para>Server address for the phone; default the address it is registered to, then bindaddr, then externip.</para></parameter>
		</syntax>
		<description>
			<para>Writes &lt;device&gt;.cnf.xml from sccp.conf (server address and port, date format, firmware, TOS, locale). An existing file is not overwritten.</para>
		</description>
	</manager>
	<manager name="SCCPPushURL" language="en_US">
		<synopsis>Make a phone open a URL.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
			<parameter name="Device" required="true"><para>Device name.</para></parameter>
			<parameter name="URL" required="true"><para>Cisco XML service or page, at most 256 characters.</para></parameter>
		</syntax>
		<description>
			<para>The phone accepts the URL only if its authentication URL allows pushes.</para>
		</description>
	</manager>
	<manager name="SCCPPress" language="en_US">
		<synopsis>Press a key on a phone.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
			<parameter name="Device" required="true"><para>Device name.</para></parameter>
			<parameter name="Key" required="true">
				<enumlist>
					<enum name="softkey"><para>Value is the softkey name, e.g. NewCall, EndCall, Hold.</para></enum>
					<enum name="digits"><para>Value is the digits (0-9, *, #, +).</para></enum>
					<enum name="offhook"/>
					<enum name="onhook"/>
				</enumlist>
			</parameter>
			<parameter name="Value"><para>Softkey name or digits.</para></parameter>
		</syntax>
		<description>
			<para>Acts as if the key was pressed on the phone; softkeys and digits apply to the phone's active call.</para>
		</description>
	</manager>
	<manager name="SCCPShowFirmware" language="en_US">
		<synopsis>List phone firmware.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
		</syntax>
		<description>
			<para>Returns one SCCPFirmwareEntry event per model and firmware pair, with Model, Firmware (empty when the phone has not reported one), Devices (count) and DeviceNames, followed by SCCPShowFirmwareComplete.</para>
		</description>
	</manager>
	<manager name="SCCPShowDevice" language="en_US">
		<synopsis>Show one SCCP device.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
			<parameter name="Device" required="true">
				<para>Device name, for example SEP001122334455.</para>
			</parameter>
		</syntax>
		<description>
			<para>Returns the device's settings and its button, line, speed dial, feature and service URL tables. Same as <literal>sccp show device</literal>.</para>
		</description>
	</manager>
	<manager name="SCCPShowLines" language="en_US">
		<synopsis>List SCCP lines.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
		</syntax>
		<description>
			<para>Returns one SCCPLineEntry event per line and device the line is on.</para>
		</description>
	</manager>
	<manager name="SCCPShowLine" language="en_US">
		<synopsis>Show one SCCP line.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
			<parameter name="Line" required="true">
				<para>Line name as defined in sccp.conf.</para>
			</parameter>
		</syntax>
		<description>
			<para>Returns the line's settings, the devices it is on and its mailboxes. Same as <literal>sccp show line</literal>.</para>
		</description>
	</manager>
	<manager name="SCCPShowChannels" language="en_US">
		<synopsis>List SCCP calls.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
		</syntax>
		<description>
			<para>Returns one event per SCCP call. The call ID in the first column is what the Call parameter of other SCCP actions expects.</para>
		</description>
	</manager>
	<manager name="SCCPShowSessions" language="en_US">
		<synopsis>List phone connections.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
		</syntax>
		<description>
			<para>Returns one event per TCP or TLS connection from a phone, with its keepalive timing and registration state.</para>
		</description>
	</manager>
	<manager name="SCCPShowMWISubscriptions" language="en_US">
		<synopsis>List voicemail (MWI) subscriptions.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
		</syntax>
		<description>
			<para>Returns one event per mailbox that SCCP lines watch, with the new and old message counts.</para>
		</description>
	</manager>
	<manager name="SCCPShowHintLineStates" language="en_US">
		<synopsis>List line states used by hints.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
		</syntax>
		<description>
			<para>Returns the state SCCP reports to Asterisk for each line, as used by BLF and hint subscribers.</para>
		</description>
	</manager>
	<manager name="SCCPShowHintSubscriptions" language="en_US">
		<synopsis>List hint subscriptions.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
		</syntax>
		<description>
			<para>Returns the extensions whose hints SCCP phones watch (BLF speed dials) and their current state.</para>
		</description>
	</manager>
	<manager name="SCCPShowSoftkeySets" language="en_US">
		<synopsis>List softkey sets.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
		</syntax>
		<description>
			<para>Returns the softkeys of every softkey set for each call state.</para>
		</description>
	</manager>
	<manager name="SCCPShowReferences" language="en_US">
		<synopsis>List reference-counted objects.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
		</syntax>
		<description>
			<para>Returns every device, line and call object with its reference count. Used to track down reference leaks.</para>
		</description>
	</manager>
	<manager name="SCCPMessageAll" language="en_US">
		<synopsis>Show a message on all phones.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
			<parameter name="Text" required="true">
				<para>Text to show.</para>
			</parameter>
			<parameter name="Beep">
				<para>yes to also play a short tone.</para>
			</parameter>
			<parameter name="Timeout">
				<para>Seconds to show the message (default 10).</para>
			</parameter>
		</syntax>
		<description>
			<para>Shows the message on every registered phone.</para>
		</description>
		<see-also>
			<ref type="manager">SCCPMessageDevice</ref>
			<ref type="manager">SCCPSystemMessage</ref>
		</see-also>
	</manager>
	<manager name="SCCPMessageDevice" language="en_US">
		<synopsis>Show a message on one phone.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
			<parameter name="Device" required="true">
				<para>Device name.</para>
			</parameter>
			<parameter name="Text" required="true">
				<para>Text to show.</para>
			</parameter>
			<parameter name="Beep">
				<para>yes to also play a short tone.</para>
			</parameter>
			<parameter name="Timeout">
				<para>Seconds to show the message (default 10).</para>
			</parameter>
		</syntax>
		<description>
			<para>Shows the message on one registered phone.</para>
		</description>
	</manager>
	<manager name="SCCPSystemMessage" language="en_US">
		<synopsis>Set or clear the system message.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
			<parameter name="Text">
				<para>Message text. Leave it out to clear the system message.</para>
			</parameter>
			<parameter name="Beep">
				<para>yes to also play a short tone on registered phones.</para>
			</parameter>
			<parameter name="Timeout">
				<para>0 (default) shows the text as the idle message; 1-255 shows it as a notification for that many seconds.</para>
			</parameter>
		</syntax>
		<description>
			<para>The system message is saved in the Asterisk database and shown on every phone, including phones that register later.</para>
		</description>
	</manager>
	<manager name="SCCPSetDeviceDND" language="en_US">
		<synopsis>Set do-not-disturb on a device.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
			<parameter name="Device" required="true">
				<para>Device name.</para>
			</parameter>
			<parameter name="State" required="true">
				<enumlist>
					<enum name="off"/>
					<enum name="reject"><para>Callers get busy.</para></enum>
					<enum name="silent"><para>The phone does not ring.</para></enum>
				</enumlist>
			</parameter>
		</syntax>
		<description>
			<para>Requires dndFeature to be enabled for the device.</para>
		</description>
	</manager>
	<manager name="SCCPSetDeviceMicrophone" language="en_US">
		<synopsis>Mute or unmute a device's active call.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
			<parameter name="Device" required="true">
				<para>Device name.</para>
			</parameter>
			<parameter name="State" required="true">
				<para>on or off.</para>
			</parameter>
		</syntax>
		<description>
			<para>Turns the phone's microphone on or off for its active call.</para>
		</description>
	</manager>
	<manager name="SCCPSetDeviceOption" language="en_US">
		<synopsis>Change a device option at runtime.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
			<parameter name="Device" required="true">
				<para>Device name.</para>
			</parameter>
			<parameter name="Option" required="true">
				<para>Any sccp.conf device option; ringtone or backgroundimage (value is a URL); or debug (on/off: limit SCCP debug output to marked devices).</para>
			</parameter>
			<parameter name="Value" required="true">
				<para>New value.</para>
			</parameter>
		</syntax>
		<description>
			<para>Applies to the running device only; the change is not saved to sccp.conf. Options that need a restart take effect when the phone restarts.</para>
		</description>
	</manager>
	<manager name="SCCPSetLineForward" language="en_US">
		<synopsis>Set or clear call forwarding on a line.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
			<parameter name="Line" required="true">
				<para>Line name.</para>
			</parameter>
			<parameter name="Device">
				<para>Device to change. Without it, every device that has the line is changed.</para>
			</parameter>
			<parameter name="Type" required="true">
				<enumlist>
					<enum name="all"/>
					<enum name="busy"/>
					<enum name="noanswer"/>
					<enum name="none"><para>Clears all forward types.</para></enum>
				</enumlist>
			</parameter>
			<parameter name="Number">
				<para>Forward destination. Leave it out to clear this forward type.</para>
			</parameter>
		</syntax>
		<description>
			<para>Same as <literal>sccp set line &lt;line&gt; [device] forward &lt;type&gt; [number]</literal>.</para>
		</description>
	</manager>
	<manager name="SCCPSetFallback" language="en_US">
		<synopsis>Change the token fallback setting.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
			<parameter name="Fallback" required="true">
				<para>true, false, odd, even, or the absolute path of a script.</para>
			</parameter>
		</syntax>
		<description>
			<para>Same values as the fallback option in sccp.conf. Not saved to sccp.conf.</para>
		</description>
	</manager>
	<manager name="SCCPAddLine" language="en_US">
		<synopsis>Add a line to a device.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
			<parameter name="Device" required="true">
				<para>Device name.</para>
			</parameter>
			<parameter name="Line" required="true">
				<para>Line name.</para>
			</parameter>
		</syntax>
		<description>
			<para>Adds a line button after the device's last button; a registered phone restarts to load it. Not saved to sccp.conf.</para>
		</description>
	</manager>
	<manager name="SCCPRemoveLine" language="en_US">
		<synopsis>Remove a line from a device.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
			<parameter name="Device" required="true">
				<para>Device name.</para>
			</parameter>
			<parameter name="Line" required="true">
				<para>Line name.</para>
			</parameter>
		</syntax>
		<description>
			<para>Removes the line's buttons from the device; a registered phone restarts. Not saved to sccp.conf.</para>
		</description>
	</manager>
	<manager name="SCCPCall" language="en_US">
		<synopsis>Start a call from a phone.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
			<parameter name="Device" required="true">
				<para>Device that places the call.</para>
			</parameter>
			<parameter name="Number">
				<para>Number to dial. Without it the phone only goes off hook.</para>
			</parameter>
			<parameter name="Line">
				<para>Line to call from. Default: the device's default or active line.</para>
			</parameter>
		</syntax>
		<description>
			<para>The phone dials as if the user had dialed; its speaker opens.</para>
		</description>
	</manager>
	<manager name="SCCPAnswer" language="en_US">
		<synopsis>Answer a ringing call.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
			<parameter name="Call" required="true">
				<para>Call ID from SCCPShowChannels, or a channel name such as SCCP/2004-00000003.</para>
			</parameter>
			<parameter name="Device">
				<para>Device that answers. Required when the call rings on a shared line.</para>
			</parameter>
		</syntax>
		<description>
			<para>The call must be ringing.</para>
		</description>
	</manager>
	<manager name="SCCPHangup" language="en_US">
		<synopsis>Hang up a call.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
			<parameter name="Call" required="true">
				<para>Call ID from SCCPShowChannels, or a channel name such as SCCP/2004-00000003.</para>
			</parameter>
		</syntax>
		<description>
			<para>Ends the call as if the user had hung up.</para>
		</description>
	</manager>
	<manager name="SCCPHold" language="en_US">
		<synopsis>Put a call on hold or resume it.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
			<parameter name="Call" required="true">
				<para>Call ID from SCCPShowChannels, or a channel name such as SCCP/2004-00000003.</para>
			</parameter>
			<parameter name="State" required="true">
				<para>on to hold, off to resume.</para>
			</parameter>
			<parameter name="Device">
				<para>Device that resumes the call. Default: the call's own device; required for a held call on a shared line.</para>
			</parameter>
		</syntax>
		<description>
			<para>Same as <literal>sccp set channel &lt;call&gt; hold on|off [device]</literal>.</para>
		</description>
	</manager>
	<manager name="SCCPReset" language="en_US">
		<synopsis>Reset a phone.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
			<parameter name="Device" required="true">
				<para>Device name.</para>
			</parameter>
		</syntax>
		<description>
			<para>The phone reboots and reloads its firmware and configuration. Refused while the phone has an active call.</para>
		</description>
	</manager>
	<manager name="SCCPRestart" language="en_US">
		<synopsis>Restart a phone.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
			<parameter name="Device" required="true">
				<para>Device name.</para>
			</parameter>
		</syntax>
		<description>
			<para>The phone re-registers and reloads its configuration without rebooting. Refused while the phone has an active call.</para>
		</description>
	</manager>
	<manager name="SCCPApplyConfig" language="en_US">
		<synopsis>Make a phone reload its configuration file.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
			<parameter name="Device" required="true">
				<para>Device name.</para>
			</parameter>
		</syntax>
		<description>
			<para>The phone downloads its configuration file from TFTP and applies it.</para>
		</description>
	</manager>
	<manager name="SCCPUnregister" language="en_US">
		<synopsis>Unregister a phone.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
			<parameter name="Device" required="true">
				<para>Device name.</para>
			</parameter>
		</syntax>
		<description>
			<para>The phone unregisters and then registers again on its own.</para>
		</description>
	</manager>
	<manager name="SCCPRefreshDevice" language="en_US">
		<synopsis>Resend a phone's button and softkey layout.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
			<parameter name="Device" required="true">
				<para>Device name.</para>
			</parameter>
		</syntax>
		<description>
			<para>Sends the button template and softkey template to the registered phone again.</para>
		</description>
	</manager>
	<manager name="SCCPTokenAck" language="en_US">
		<synopsis>Acknowledge a phone's token request.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
			<parameter name="Device" required="true">
				<para>Device name.</para>
			</parameter>
		</syntax>
		<description>
			<para>Grants a token request this server refused because of the fallback setting, so the phone registers here. Used when servers are clustered.</para>
		</description>
	</manager>
	<manager name="SCCPShowConferences" language="en_US">
		<synopsis>List SCCP conferences.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
		</syntax>
		<description>
			<para>Returns one event per running conference. Only available when chan_sccp is built with conference support.</para>
		</description>
	</manager>
	<manager name="SCCPShowConference" language="en_US">
		<synopsis>Show one SCCP conference.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
			<parameter name="Conference" required="true">
				<para>Conference ID from SCCPShowConferences.</para>
			</parameter>
		</syntax>
		<description>
			<para>Returns the conference's participants.</para>
		</description>
	</manager>
	<manager name="SCCPConference" language="en_US">
		<synopsis>Control a conference participant.</synopsis>
		<syntax>
			<xi:include href="../core-en_US.xml" parse="xml" xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])"/>
			<parameter name="Action" required="true">
				<enumlist>
					<enum name="EndConf"/>
					<enum name="Kick"/>
					<enum name="Mute"/>
					<enum name="Invite"/>
					<enum name="Moderate"/>
				</enumlist>
			</parameter>
			<parameter name="Conference" required="true">
				<para>Conference ID.</para>
			</parameter>
			<parameter name="Participant">
				<para>Participant ID; required for every action except EndConf.</para>
			</parameter>
		</syntax>
		<description>
			<para>Same as <literal>sccp conference &lt;action&gt; &lt;conference&gt; [participant]</literal>.</para>
		</description>
	</manager>
***/

typedef enum sccp_cli_completer {
	SCCP_CLI_NULL_COMPLETER,
	SCCP_CLI_DEVICE_COMPLETER,
	SCCP_CLI_CONNECTED_DEVICE_COMPLETER,
	SCCP_CLI_LINE_COMPLETER,
	SCCP_CLI_CONNECTED_LINE_COMPLETER,
	SCCP_CLI_CHANNEL_COMPLETER,
	SCCP_CLI_RINGING_CHANNEL_COMPLETER,
	SCCP_CLI_CONNECTED_CHANNEL_COMPLETER,
	SCCP_CLI_CONFERENCE_COMPLETER,
	SCCP_CLI_DEBUG_COMPLETER,
	SCCP_CLI_SET_COMPLETER,
} sccp_cli_completer_t;

static char *sccp_exec_completer(sccp_cli_completer_t completer, OLDCONST char *line, OLDCONST char *word, int pos, int state);

/* --- CLI Tab Completion ---------------------------------------------------------------------------------------------- */
/*!
 * \brief Complete Device
 * \param line Line as char
 * \param word Word as char
 * \param pos Pos as int
 * \param state State as int
 * \return Result as char
 * 
 * \called_from_asterisk
 */
static char * sccp_complete_device(OLDCONST char * word, int state)
{
	sccp_device_t *d = NULL;
	int wordlen = strlen(word);

	int which = 0;
	char *ret = NULL;

	SCCP_RWLIST_RDLOCK(&GLOB(devices));
	SCCP_RWLIST_TRAVERSE(&GLOB(devices), d, list) {
		if (!strncasecmp(word, d->id, wordlen) && ++which > state) {
			ret = pbx_strdup(d->id);
			break;
		}
	}
	SCCP_RWLIST_UNLOCK(&GLOB(devices));

	return ret;
}

static char * sccp_complete_connected_device(OLDCONST char * word, int state)
{
	sccp_device_t *d = NULL;
	int wordlen = strlen(word);

	int which = 0;
	char *ret = NULL;

	SCCP_RWLIST_RDLOCK(&GLOB(devices));
	SCCP_RWLIST_TRAVERSE(&GLOB(devices), d, list) {
		if (!strncasecmp(word, d->id, wordlen) && sccp_device_getRegistrationState(d) != SKINNY_DEVICE_RS_NONE && ++which > state) {
			ret = pbx_strdup(d->id);
			break;
		}
	}
	SCCP_RWLIST_UNLOCK(&GLOB(devices));

	return ret;
}

/*!
 * \brief Complete Line
 * \param line Line as char
 * \param word Word as char
 * \param pos Pos as int
 * \param state State as int
 * \return Result as char
 * 
 * \called_from_asterisk
 * 
 */
static char * sccp_complete_line(OLDCONST char * word, int state)
{
	sccp_line_t *l = NULL;
	int wordlen = strlen(word);

	int which = 0;
	char *ret = NULL;

	SCCP_RWLIST_RDLOCK(&GLOB(lines));
	SCCP_RWLIST_TRAVERSE(&GLOB(lines), l, list) {
		if (!strncasecmp(word, l->name, wordlen) && ++which > state) {
			ret = pbx_strdup(l->name);
			break;
		}
	}
	SCCP_RWLIST_UNLOCK(&GLOB(lines));

	return ret;
}

static char * sccp_complete_connected_line(OLDCONST char * word, int state)
{
	sccp_line_t *l = NULL;
	int wordlen = strlen(word);

	int which = 0;
	char *ret = NULL;

	SCCP_RWLIST_RDLOCK(&GLOB(lines));
	SCCP_RWLIST_TRAVERSE(&GLOB(lines), l, list) {
		if (!strncasecmp(word, l->name, wordlen) && SCCP_LIST_GETSIZE(&l->devices) > 0 && ++which > state) {
			ret = pbx_strdup(l->name);
			break;
		}
	}
	SCCP_RWLIST_UNLOCK(&GLOB(lines));

	return ret;
}

/*!
 * \brief Complete Channel
 * \param line Line as char
 * \param word Word as char
 * \param pos Pos as int
 * \param state State as int
 * \return Result as char
 * 
 * \called_from_asterisk
 * 
 */
static char * sccp_complete_channel(OLDCONST char * word, int state)
{
	sccp_line_t *l = NULL;
	sccp_channel_t *c = NULL;
	int wordlen = strlen(word);

	int which = 0;
	char *ret = NULL;

	SCCP_RWLIST_RDLOCK(&GLOB(lines));
	SCCP_RWLIST_TRAVERSE(&GLOB(lines), l, list) {
		SCCP_LIST_LOCK(&l->channels);
		SCCP_LIST_TRAVERSE(&l->channels, c, list) {
			if (!strncasecmp(word, c->designator, wordlen) && ++which > state) {
				ret = pbx_strdup(c->designator);
				break;
			}
		}
		SCCP_LIST_UNLOCK(&l->channels);
		if (ret) {
			break;								// break out of outer look, prevent leaking memory by strdup
		}
	}
	SCCP_RWLIST_UNLOCK(&GLOB(lines));

	return ret;
}

/*!
 * \brief Complete Ringing Channel
 * \param line Line as char
 * \param word Word as char
 * \param pos Pos as int
 * \param state State as int
 * \return Result as char
 *
 * \called_from_asterisk
 *
 */
static char * sccp_complete_ringing_channel(OLDCONST char * word, int state)
{
	sccp_line_t * l = NULL;
	sccp_channel_t * c = NULL;
	int wordlen = strlen(word);

	int which = 0;
	char * ret = NULL;

	SCCP_RWLIST_RDLOCK(&GLOB(lines));
	SCCP_RWLIST_TRAVERSE(&GLOB(lines), l, list) {
		SCCP_LIST_LOCK(&l->channels);
		SCCP_LIST_TRAVERSE(&l->channels, c, list) {
			if(c->state == SCCP_CHANNELSTATE_RINGING && !strncasecmp(word, c->designator, wordlen) && ++which > state) {
				ret = pbx_strdup(c->designator);
				break;
			}
		}
		SCCP_LIST_UNLOCK(&l->channels);
		if(ret) {
			break;                                        // break out of outer look, prevent leaking memory by strdup
		}
	}
	SCCP_RWLIST_UNLOCK(&GLOB(lines));

	return ret;
}

/*!
 * \brief Complete Ringing Channel
 * \param line Line as char
 * \param word Word as char
 * \param pos Pos as int
 * \param state State as int
 * \return Result as char
 *
 * \called_from_asterisk
 *
 */
static char * sccp_complete_connected_channel(OLDCONST char * word, int state)
{
	sccp_line_t * l = NULL;
	sccp_channel_t * c = NULL;
	int wordlen = strlen(word);

	int which = 0;
	char * ret = NULL;

	SCCP_RWLIST_RDLOCK(&GLOB(lines));
	SCCP_RWLIST_TRAVERSE(&GLOB(lines), l, list) {
		SCCP_LIST_LOCK(&l->channels);
		SCCP_LIST_TRAVERSE(&l->channels, c, list) {
			if(SCCP_CHANNELSTATE_IsConnected(c->state) && !strncasecmp(word, c->designator, wordlen) && ++which > state) {
				ret = pbx_strdup(c->designator);
				break;
			}
		}
		SCCP_LIST_UNLOCK(&l->channels);
		if(ret) {
			break;                                        // break out of outer look, prevent leaking memory by strdup
		}
	}
	SCCP_RWLIST_UNLOCK(&GLOB(lines));

	return ret;
}

/*!
 * \brief Complete Debug
 * \param line Line as char
 * \param word Word as char
 * \param pos Pos as int
 * \param state State as int
 * \return Result as char
 * 
 * \called_from_asterisk
 */
static char * sccp_complete_debug(OLDCONST char * line, OLDCONST char * word, int state)
{
	uint8_t i = 0;
	int wordlen = strlen(word);
	int which = 0;
	char *ret = NULL;
	boolean_t debugno = 0;
	char *extra_cmds[] = { "no", "none", "off", "all" };

	// check if the sccp debug line contains no before the categories
	if(strncasecmp(line, "sccp debug no ", strlen("sccp debug no ")) == 0) {
		debugno = 1;
	}
	// check extra_cmd
	for (i = 0; i < ARRAY_LEN(extra_cmds); i++) {
		if(strncasecmp(word, extra_cmds[i], wordlen) == 0) {
			// skip "no" and "none" if in debugno mode
			if (debugno && !strncasecmp("no", extra_cmds[i], strlen("no"))) {
				continue;
			}
			if (++which > state) {
				return pbx_strdup(extra_cmds[i]);
			}
		}
	}
	// check categories
	for (i =0; i < ARRAY_LEN(sccp_debug_categories); i++) {
		// if in debugno mode
		if (debugno) {
			// then skip the categories which are not currently active
			if ((GLOB(debug) & sccp_debug_categories[i].category) != sccp_debug_categories[i].category) {
				continue;
			}
		} else {
			// not debugno then skip the categories which are already active
			if ((GLOB(debug) & sccp_debug_categories[i].category) == sccp_debug_categories[i].category) {
				continue;
			}
		}
		// find a match with partial category
		if(strncasecmp(word, sccp_debug_categories[i].key, wordlen) == 0) {
			if (++which > state) {
				return pbx_strdup(sccp_debug_categories[i].key);
			}
		}
	}
	return ret;
}

/*!
 * \brief Complete Debug
 * \param line Line as char
 * \param word Word as char
 * \param pos Pos as int
 * \param state State as int
 * \return Result as char
 * 
 * \called_from_asterisk
 */
static char *sccp_complete_set(OLDCONST char *line, OLDCONST char *word, int pos, int state)
{
	uint8_t i = 0;
	sccp_device_t *d = NULL;
	sccp_channel_t *c = NULL;
	sccp_line_t *l = NULL;

	int wordlen = strlen(word);

	int which = 0;
	char tmpname[80];
	char *ret = NULL;

	char *types[] = { "device", "channel", "line", "fallback", "debug" };

	char * properties_channel[] = { "hold",
#ifdef CS_SCCP_PARK
					"park"
#endif
	};
	char * properties_device[] = { "ringtone", "backgroundimage" };
	char *properties_fallback[] = { "true", "false", "odd", "even", "path" };

	char *values_hold[] = { "on", "off" };

	switch (pos) {
		case 2:											// type
			for (i = 0; i < ARRAY_LEN(types); i++) {
				if (!strncasecmp(word, types[i], wordlen) && ++which > state) {
					return pbx_strdup(types[i]);
				}
			}
			break;
		case 3:											// device / channel / line / fallback
			if (strstr(line, "device") != NULL) {
				SCCP_RWLIST_RDLOCK(&GLOB(devices));
				SCCP_RWLIST_TRAVERSE(&GLOB(devices), d, list) {
					if (!strncasecmp(word, d->id, wordlen) && ++which > state) {
						ret = pbx_strdup(d->id);
						break;
					}
				}
				SCCP_RWLIST_UNLOCK(&GLOB(devices));

			} else if (strstr(line, "channel") != NULL) {
				SCCP_RWLIST_RDLOCK(&GLOB(lines));
				SCCP_RWLIST_TRAVERSE(&GLOB(lines), l, list) {
					SCCP_LIST_LOCK(&l->channels);
					SCCP_LIST_TRAVERSE(&l->channels, c, list) {
						snprintf(tmpname, sizeof(tmpname), "%s", c->designator);
						if (!strncasecmp(word, tmpname, wordlen) && ++which > state) {
							ret = pbx_strdup(tmpname);
							break;
						}
					}
					SCCP_LIST_UNLOCK(&l->channels);
					if (ret) {
						break;							// break out of outer look, prevent leaking memory by strdup
					}
				}
				SCCP_RWLIST_UNLOCK(&GLOB(lines));
			} else if (strstr(line, "fallback") != NULL) {
				for (i = 0; i < ARRAY_LEN(properties_fallback); i++) {
					if (!strncasecmp(word, properties_fallback[i], wordlen) && ++which > state) {
						return pbx_strdup(properties_fallback[i]);
					}
				}
			} else if (strstr(line, "debug") != NULL) {
				return sccp_complete_debug(line, word, state);
			}
			break;
		case 4:											// properties

			if (strstr(line, "device") != NULL) {
				for (i = 0; i < ARRAY_LEN(properties_device); i++) {
					if (!strncasecmp(word, properties_device[i], wordlen) && ++which > state) {
						return pbx_strdup(properties_device[i]);
					}
				}

			} else if (strstr(line, "channel") != NULL) {
				for (i = 0; i < ARRAY_LEN(properties_channel); i++) {
					if (!strncasecmp(word, properties_channel[i], wordlen) && ++which > state) {
						return pbx_strdup(properties_channel[i]);
					}
				}
			} else if (strstr(line, "debug") != NULL) {
				return sccp_complete_debug(line, word, state);
			}
			break;
		case 5:											// values_hold

			if (strstr(line, "channel") != NULL && strstr(line, "hold") != NULL) {
				for (i = 0; i < ARRAY_LEN(values_hold); i++) {
					if (!strncasecmp(word, values_hold[i], wordlen) && ++which > state) {
						return pbx_strdup(values_hold[i]);
					}
				}
			} else if (strstr(line, "debug") != NULL) {
				return sccp_complete_debug(line, word, state);
			}
			break;
		case 6:											// values_hold off device
			if (strstr(line, "channel") != NULL && strstr(line, "hold off") != NULL) {
				if (!strncasecmp(word, "device", wordlen) && ++which > state) {
					return pbx_strdup("device");
				}
			} else if (strstr(line, "debug") != NULL) {
				return sccp_complete_debug(line, word, state);
			}
			break;
		case 7:											// values_hold off device
			if (strstr(line, "channel") != NULL && strstr(line, "hold off") != NULL && strstr(line, "device") != NULL) {
				SCCP_RWLIST_RDLOCK(&GLOB(devices));
				SCCP_RWLIST_TRAVERSE(&GLOB(devices), d, list) {
					if (!strncasecmp(word, d->id, wordlen) && ++which > state) {
						ret = pbx_strdup(d->id);
						break;
					}
				}
				SCCP_RWLIST_UNLOCK(&GLOB(devices));
			} else if (strstr(line, "debug") != NULL) {
				return sccp_complete_debug(line, word, state);
			}
			break;
		default:
			if (strstr(line, "debug") != NULL) {
				return sccp_complete_debug(line, word, state);
			}
			break;
	}
	return ret;
}

static char *sccp_exec_completer(sccp_cli_completer_t completer, OLDCONST char *line, OLDCONST char *word, int pos, int state)
{
	char * completerStr = NULL;

	completerStr = NULL;
	switch (completer) {
		case SCCP_CLI_NULL_COMPLETER:
			completerStr = NULL;
			break;
		case SCCP_CLI_DEVICE_COMPLETER:
			completerStr = sccp_complete_device(word, state);
			break;
		case SCCP_CLI_CONNECTED_DEVICE_COMPLETER:
			completerStr = sccp_complete_connected_device(word, state);
			break;
		case SCCP_CLI_LINE_COMPLETER:
			completerStr = sccp_complete_line(word, state);
			break;
		case SCCP_CLI_CONNECTED_LINE_COMPLETER:
			completerStr = sccp_complete_connected_line(word, state);
			break;
		case SCCP_CLI_CHANNEL_COMPLETER:
			completerStr = sccp_complete_channel(word, state);
			break;
		case SCCP_CLI_RINGING_CHANNEL_COMPLETER:
			completerStr = sccp_complete_ringing_channel(word, state);
			break;
		case SCCP_CLI_CONNECTED_CHANNEL_COMPLETER:
			completerStr = sccp_complete_connected_channel(word, state);
			break;
		case SCCP_CLI_CONFERENCE_COMPLETER:
#ifdef CS_SCCP_CONFERENCE
			completerStr = sccp_complete_conference(line, word, pos, state);
#endif
			break;
		case SCCP_CLI_DEBUG_COMPLETER:
			completerStr = sccp_complete_debug(line, word, state);
			break;
		case SCCP_CLI_SET_COMPLETER:
			completerStr = sccp_complete_set(line, word, pos, state);
			break;
	}
	return completerStr;
}

/* --- Support Functions ---------------------------------------------------------------------------------------------- */

/* Key/value screens (Asterisk style): a short section title, then "  Label:" lines with the values
 * aligned just past the longest label of the screen (CLI_AMI_LIST_WIDTH, set per screen).
 * Empty values read "(not set)" for an option and "(none)" for a list on the CLI; AMI gets "". */
#define CLI_NOT_SET(_x) (!sccp_strlen_zero(_x) ? (_x) : (s ? "" : "(not set)"))
#define CLI_NONE(_x)    (!sccp_strlen_zero(_x) ? (_x) : (s ? "" : "(none)"))
#define CLI_TITLE(_title)                                                                                       \
	({                                                                                                      \
		if (!s) {                                                                                       \
			pbx_cli(fd, "%s:\n", (_title));                                                        \
		}                                                                                               \
	})
#define CLI_SECTION(_title)                                                                                     \
	({                                                                                                      \
		if (!s) {                                                                                       \
			pbx_cli(fd, "\n%s:\n", (_title));                                                      \
		}                                                                                               \
	})
/* a ", "-separated list, wrapped under the value column on the CLI */
#define CLI_AMI_OUTPUT_LIST(param, width, text)                                                                 \
	({                                                                                                      \
		if (s) {                                                                                        \
			CLI_AMI_OUTPUT_PARAM(param, width, "%s", (text));                                       \
		} else {                                                                                        \
			sccp_cli_print_list(fd, param ":", (width) + 1, CLI_NONE(text));                         \
		}                                                                                               \
	})

/* codec list without the brackets sccp_codec_multiple2str() adds, for CLI_AMI_OUTPUT_LIST */
static void sccp_cli_codec_list(char * buf, size_t size, const skinny_codec_t * codecs, int length)
{
	sccp_codec_multiple2str(buf, size - 1, codecs, length);
	size_t len = strlen(buf);
	if (len >= 2 && buf[0] == '[' && buf[len - 1] == ']') {
		memmove(buf, buf + 1, len - 2);
		buf[len - 2] = '\0';
	}
}

#define CLI_LINE_LENGTH 79
static void sccp_cli_print_list(int fd, const char * label, int width, const char * text)
{
	const int indent = 2 + width + 1;
	int       column = indent;
	char *    copy   = pbx_strdupa(text);
	char *    rest   = copy;
	char *    item   = NULL;
	pbx_cli(fd, "  %-*s ", width, label);
	while ((item = strsep(&rest, ","))) {
		while (*item == ' ') {
			item++;
		}
		int len = (int)strlen(item) + (rest ? 1 : 0);
		if (column > indent && column + 1 + len > CLI_LINE_LENGTH) {
			pbx_cli(fd, "\n%*s", indent, "");
			column = indent;
		} else if (column > indent) {
			pbx_cli(fd, " ");
			column++;
		}
		pbx_cli(fd, "%s%s", item, rest ? "," : "");
		column += len;
	}
	pbx_cli(fd, "\n");
}
/* -------------------------------------------------------------------------------------------------------SHOW GLOBALS- */

/*!
 * \brief Show Globals
 * \param fd Fd as int
 * \param totals Total number of lines as int
 * \param s AMI Session
 * \param m Message
 * \param argc Argc as int
 * \param argv[] Argv[] as char
 * \return Result as int
 * 
 * \called_from_asterisk
 * 
 */
static int sccp_show_globals(int fd, sccp_cli_totals_t *totals, struct mansession *s, const struct message *m, int argc, char *argv[])
{
	char apref_buf[256];
#if CS_SCCP_VIDEO
	char vpref_buf[256];
#endif
	pbx_str_t *callgroup_buf = pbx_str_alloca(DEFAULT_PBX_STR_BUFFERSIZE);

#ifdef CS_SCCP_PICKUP
	pbx_str_t *pickupgroup_buf = pbx_str_alloca(DEFAULT_PBX_STR_BUFFERSIZE);
#endif
	pbx_str_t *ha_buf = pbx_str_alloca(DEFAULT_PBX_STR_BUFFERSIZE);
	pbx_str_t *ha_localnet_buf = pbx_str_alloca(DEFAULT_PBX_STR_BUFFERSIZE);
	char * debugcategories = NULL;
	int local_line_total = 0;
	const char *actionid = "";

	pbx_rwlock_rdlock(&GLOB(lock));

	sccp_cli_codec_list(apref_buf, sizeof(apref_buf), GLOB(global_preferences).audio, ARRAY_LEN(GLOB(global_preferences).audio));
#if CS_SCCP_VIDEO
	sccp_cli_codec_list(vpref_buf, sizeof(vpref_buf), GLOB(global_preferences).video, ARRAY_LEN(GLOB(global_preferences).video));
#endif
	debugcategories = sccp_get_debugcategories(GLOB(debug));
	sccp_print_ha(ha_buf, DEFAULT_PBX_STR_BUFFERSIZE, GLOB(ha));
	sccp_print_ha(ha_localnet_buf, DEFAULT_PBX_STR_BUFFERSIZE, GLOB(localaddr));

	if (s) {
		astman_append(s, "Response: Success\r\n");
		astman_append(s, "Message: SCCPGlobalSettings\r\n");
		actionid = astman_get_header(m, "ActionID");
		if (!pbx_strlen_zero(actionid)) {
			astman_append(s, "ActionID: %s\r\n", actionid);
		}
		local_line_total++;
	}
	const char * bindaddr = GLOB(srvcontexts[SCCP_SERVERCONTEXT_TCP]) ? pbx_strdupa(sccp_netsock_stringify(sccp_servercontext_getBoundAddr(GLOB(srvcontexts[SCCP_SERVERCONTEXT_TCP])))) : "not listening";
	char         externip[INET6_ADDRSTRLEN + 32] = "";
	if (!sccp_netsock_is_any_addr(&GLOB(externip))) {
		snprintf(externip, sizeof(externip), "%s", sccp_netsock_stringify_addr(&GLOB(externip)));
	}
	const char * context_state = pbx_context_find(GLOB(context)) ? "" : " (context does not exist)";

#undef CLI_AMI_LIST_WIDTH
#define CLI_AMI_LIST_WIDTH 34
	CLI_TITLE("General");
	CLI_AMI_OUTPUT_PARAM("Config file", CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(GLOB(config_file_name)));
	CLI_AMI_OUTPUT_PARAM("Server name", CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(GLOB(servername)));
	CLI_AMI_OUTPUT_PARAM("Context", CLI_AMI_LIST_WIDTH, "%s%s", CLI_NOT_SET(GLOB(context)), context_state);
	CLI_AMI_OUTPUT_PARAM("Registration context", CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(GLOB(regcontext)));
	CLI_AMI_OUTPUT_PARAM("Language", CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(GLOB(language)));
	CLI_AMI_OUTPUT_PARAM("Account code", CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(GLOB(accountcode)));
	CLI_AMI_OUTPUT_PARAM("Music on hold class", CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(GLOB(musicclass)));
	CLI_AMI_OUTPUT_PARAM("AMA flags", CLI_AMI_LIST_WIDTH, "%s", pbx_channel_amaflags2string((pbx_ama_flags_type)GLOB(amaflags)));
	CLI_AMI_OUTPUT_PARAM("Date format", CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(GLOB(dateformat)));
	CLI_AMI_OUTPUT_PARAM("Debug", CLI_AMI_LIST_WIDTH, "%s", debugcategories ? debugcategories : "none");
	CLI_AMI_OUTPUT_PARAM("Thread pool", CLI_AMI_LIST_WIDTH, "%d threads, %d jobs queued", sccp_threadpool_thread_count(GLOB(general_threadpool)), sccp_threadpool_jobqueue_count(GLOB(general_threadpool)));

	CLI_SECTION("Network");
	CLI_AMI_OUTPUT_PARAM("Bind address", CLI_AMI_LIST_WIDTH, "%s", bindaddr);
#ifdef HAVE_LIBSSL
	CLI_AMI_OUTPUT_PARAM("TLS bind address", CLI_AMI_LIST_WIDTH, "%s",
			     GLOB(srvcontexts[SCCP_SERVERCONTEXT_TLS]) ? sccp_netsock_stringify(sccp_servercontext_getBoundAddr(GLOB(srvcontexts[SCCP_SERVERCONTEXT_TLS]))) : "not listening");
	CLI_AMI_OUTPUT_PARAM("TLS certificate file", CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(GLOB(cert_file)));
#endif
	CLI_AMI_OUTPUT_PARAM("External IP", CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(externip));
	if (GLOB(externhost)) {
		struct sockaddr_storage externhost_addr;
		boolean_t               resolved = sccp_netsock_getExternalAddr(&externhost_addr, sccp_netsock_is_IPv6(&GLOB(bindaddr)) ? AF_INET6 : AF_INET);
		CLI_AMI_OUTPUT_PARAM("External host", CLI_AMI_LIST_WIDTH, "%s (%s%s)", GLOB(externhost), resolved ? "resolves to " : "does not resolve", resolved ? sccp_netsock_stringify_addr(&externhost_addr) : "");
		CLI_AMI_OUTPUT_PARAM("External host refresh (s)", CLI_AMI_LIST_WIDTH, "%d", GLOB(externrefresh));
	} else {
		CLI_AMI_OUTPUT_PARAM("External host", CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(""));
	}
	CLI_AMI_OUTPUT_LIST("Local networks", CLI_AMI_LIST_WIDTH, pbx_str_buffer(ha_localnet_buf));
	CLI_AMI_OUTPUT_LIST("Deny/permit", CLI_AMI_LIST_WIDTH, pbx_str_buffer(ha_buf));
	CLI_AMI_OUTPUT_PARAM("NAT", CLI_AMI_LIST_WIDTH, "%s", sccp_nat2str(GLOB(nat)));
	CLI_AMI_OUTPUT_PARAM("Keepalive (s)", CLI_AMI_LIST_WIDTH, "%d", GLOB(keepalive));
	CLI_AMI_OUTPUT_BOOL("Direct RTP", CLI_AMI_LIST_WIDTH, GLOB(directrtp));
	CLI_AMI_OUTPUT_BOOL("Early RTP", CLI_AMI_LIST_WIDTH, GLOB(earlyrtp));
	CLI_AMI_OUTPUT_BOOL("Trust phone IP (deprecated)", CLI_AMI_LIST_WIDTH, GLOB(trustphoneip));
	CLI_AMI_OUTPUT_PARAM("Signaling TOS/COS", CLI_AMI_LIST_WIDTH, "%d/%d", GLOB(sccp_tos), GLOB(sccp_cos));
	CLI_AMI_OUTPUT_PARAM("Audio TOS/COS", CLI_AMI_LIST_WIDTH, "%d/%d", GLOB(audio_tos), GLOB(audio_cos));
	CLI_AMI_OUTPUT_PARAM("Video TOS/COS", CLI_AMI_LIST_WIDTH, "%d/%d", GLOB(video_tos), GLOB(video_cos));

	CLI_SECTION("Media");
	CLI_AMI_OUTPUT_LIST("Audio codecs", CLI_AMI_LIST_WIDTH, apref_buf);
#if CS_SCCP_VIDEO
	CLI_AMI_OUTPUT_LIST("Video codecs", CLI_AMI_LIST_WIDTH, vpref_buf);
#endif
	CLI_AMI_OUTPUT_BOOL("Echo cancellation", CLI_AMI_LIST_WIDTH, GLOB(echocancel));
	CLI_AMI_OUTPUT_BOOL("Silence suppression", CLI_AMI_LIST_WIDTH, GLOB(silencesuppression));
	CLI_AMI_OUTPUT_BOOL("Jitter buffer", CLI_AMI_LIST_WIDTH, pbx_test_flag(GLOB(global_jbconf), AST_JB_ENABLED));
	CLI_AMI_OUTPUT_BOOL("Jitter buffer forced", CLI_AMI_LIST_WIDTH, pbx_test_flag(GLOB(global_jbconf), AST_JB_FORCED));
	CLI_AMI_OUTPUT_PARAM("Jitter buffer max size (ms)", CLI_AMI_LIST_WIDTH, "%ld", GLOB(global_jbconf)->max_size);
	CLI_AMI_OUTPUT_PARAM("Jitter buffer resync threshold", CLI_AMI_LIST_WIDTH, "%ld", GLOB(global_jbconf)->resync_threshold);
	CLI_AMI_OUTPUT_PARAM("Jitter buffer implementation", CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(GLOB(global_jbconf)->impl));
	CLI_AMI_OUTPUT_BOOL("Jitter buffer log", CLI_AMI_LIST_WIDTH, pbx_test_flag(GLOB(global_jbconf), AST_JB_LOG));
#ifdef CS_AST_JB_TARGET_EXTRA
	CLI_AMI_OUTPUT_PARAM("Jitter buffer target extra (ms)", CLI_AMI_LIST_WIDTH, "%ld", GLOB(global_jbconf)->target_extra);
#endif

	CLI_SECTION("Dialing");
	CLI_AMI_OUTPUT_PARAM("First digit timeout (s)", CLI_AMI_LIST_WIDTH, "%d", GLOB(firstdigittimeout));
	CLI_AMI_OUTPUT_PARAM("Digit timeout (s)", CLI_AMI_LIST_WIDTH, "%d", GLOB(digittimeout));
	CLI_AMI_OUTPUT_PARAM("Dial now key", CLI_AMI_LIST_WIDTH, "%c", GLOB(digittimeoutchar));

	CLI_SECTION("Calls");
	CLI_AMI_OUTPUT_PARAM("Ring type", CLI_AMI_LIST_WIDTH, "%s", skinny_ringtype2str(GLOB(ringtype)));
	CLI_AMI_OUTPUT_PARAM("Auto-answer ring time (s)", CLI_AMI_LIST_WIDTH, "%d", GLOB(autoanswer_ring_time));
	CLI_AMI_OUTPUT_PARAM("Auto-answer tone", CLI_AMI_LIST_WIDTH, "%s", skinny_tone2str(GLOB(autoanswer_tone)));
	CLI_AMI_OUTPUT_PARAM("Remote hangup tone", CLI_AMI_LIST_WIDTH, "%s", skinny_tone2str(GLOB(remotehangup_tone)));
	CLI_AMI_OUTPUT_PARAM("Call waiting tone", CLI_AMI_LIST_WIDTH, "%s", skinny_tone2str(GLOB(callwaiting_tone)));
	CLI_AMI_OUTPUT_PARAM("Call waiting tone interval (s)", CLI_AMI_LIST_WIDTH, "%d", GLOB(callwaiting_interval));
	CLI_AMI_OUTPUT_BOOL("Transfer", CLI_AMI_LIST_WIDTH, GLOB(transfer));
	CLI_AMI_OUTPUT_PARAM("Transfer tone", CLI_AMI_LIST_WIDTH, "%s", skinny_tone2str(GLOB(transfer_tone)));
	CLI_AMI_OUTPUT_BOOL("Transfer on hangup", CLI_AMI_LIST_WIDTH, GLOB(transfer_on_hangup));
	CLI_AMI_OUTPUT_BOOL("Call forward all", CLI_AMI_LIST_WIDTH, GLOB(cfwdall));
	CLI_AMI_OUTPUT_BOOL("Call forward busy", CLI_AMI_LIST_WIDTH, GLOB(cfwdbusy));
	CLI_AMI_OUTPUT_BOOL("Call forward no answer", CLI_AMI_LIST_WIDTH, GLOB(cfwdnoanswer));
	CLI_AMI_OUTPUT_PARAM("Call forward no answer timeout (s)", CLI_AMI_LIST_WIDTH, "%d", GLOB(cfwdnoanswer_timeout));
	CLI_AMI_OUTPUT_BOOL("Do not disturb", CLI_AMI_LIST_WIDTH, GLOB(dndFeature));
	CLI_AMI_OUTPUT_BOOL("Privacy softkey", CLI_AMI_LIST_WIDTH, GLOB(privacy));
	CLI_AMI_OUTPUT_PARAM("Answered elsewhere in call history", CLI_AMI_LIST_WIDTH, "%s", skinny_callHistoryDisposition2str(GLOB(callhistory_answered_elsewhere)));
#ifdef CS_MANAGER_EVENTS
	CLI_AMI_OUTPUT_BOOL("Call events", CLI_AMI_LIST_WIDTH, GLOB(callevents));
#else
	CLI_AMI_OUTPUT_BOOL("Call events", CLI_AMI_LIST_WIDTH, FALSE);
#endif
#ifdef CS_SCCP_PARK
	CLI_AMI_OUTPUT_BOOL("Park support", CLI_AMI_LIST_WIDTH, TRUE);
#else
	CLI_AMI_OUTPUT_BOOL("Park support", CLI_AMI_LIST_WIDTH, FALSE);
#endif

	CLI_SECTION("Pickup");
	sccp_print_group(callgroup_buf, DEFAULT_PBX_STR_BUFFERSIZE, GLOB(callgroup));
	CLI_AMI_OUTPUT_PARAM("Call group", CLI_AMI_LIST_WIDTH, "%s", CLI_NONE(pbx_str_buffer(callgroup_buf)));
#ifdef CS_SCCP_PICKUP
	sccp_print_group(pickupgroup_buf, DEFAULT_PBX_STR_BUFFERSIZE, GLOB(pickupgroup));
	CLI_AMI_OUTPUT_PARAM("Pickup group", CLI_AMI_LIST_WIDTH, "%s", CLI_NONE(pbx_str_buffer(pickupgroup_buf)));
#ifdef CS_AST_HAS_NAMEDGROUP
	CLI_AMI_OUTPUT_PARAM("Named call group", CLI_AMI_LIST_WIDTH, "%s", CLI_NONE(GLOB(namedcallgroup)));
	CLI_AMI_OUTPUT_PARAM("Named pickup group", CLI_AMI_LIST_WIDTH, "%s", CLI_NONE(GLOB(namedpickupgroup)));
#endif
	CLI_AMI_OUTPUT_BOOL("Directed pickup", CLI_AMI_LIST_WIDTH, GLOB(directed_pickup));
	CLI_AMI_OUTPUT_PARAM("Directed pickup context", CLI_AMI_LIST_WIDTH, "%s%s", CLI_NOT_SET(GLOB(directed_pickup_context)),
			     sccp_strlen_zero(GLOB(directed_pickup_context)) || pbx_context_find(GLOB(directed_pickup_context)) ? "" : " (context does not exist)");
	CLI_AMI_OUTPUT_BOOL("Pickup answers the call", CLI_AMI_LIST_WIDTH, GLOB(pickup_modeanswer));
#endif

	CLI_SECTION("Registration");
	CLI_AMI_OUTPUT_PARAM("Token fallback", CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(GLOB(token_fallback)));
	CLI_AMI_OUTPUT_PARAM("Token backoff time (s)", CLI_AMI_LIST_WIDTH, "%d", GLOB(token_backoff_time));
	CLI_AMI_OUTPUT_BOOL("Hotline", CLI_AMI_LIST_WIDTH, GLOB(allowAnonymous));
	CLI_AMI_OUTPUT_PARAM("Hotline extension", CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(GLOB(hotline)->exten));
	CLI_AMI_OUTPUT_PARAM("Hotline context", CLI_AMI_LIST_WIDTH, "%s", GLOB(hotline)->line ? CLI_NOT_SET(GLOB(hotline)->line->context) : CLI_NOT_SET(""));
	CLI_AMI_OUTPUT_PARAM("Hotline label", CLI_AMI_LIST_WIDTH, "%s", GLOB(hotline)->line ? CLI_NOT_SET(GLOB(hotline)->line->label) : CLI_NOT_SET(""));
#undef CLI_AMI_LIST_WIDTH
#define CLI_AMI_LIST_WIDTH 46

	sccp_free(debugcategories);
	pbx_rwlock_unlock(&GLOB(lock));

	if (s) {
		totals->lines = local_line_total;
		return RESULT_RESPONDED;								/* the response header was written above */
	}

	return RESULT_SUCCESS;
}

static char cli_globals_usage[] =  "Usage: sccp show globals\n       Show the SCCP settings from the [general] section of sccp.conf.\n";

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#define CLI_COMMAND "sccp", "show", "globals"
#define CLI_COMPLETE SCCP_CLI_NULL_COMPLETER
CLI_AMI_ENTRY(show_globals, sccp_show_globals, "List defined SCCP global settings", cli_globals_usage, FALSE, FALSE)
SCCP_AMI_ACTION(show_globals, sccp_show_globals, "SCCPShowGlobals", FALSE, "sccp", "show", "globals")
#undef CLI_COMPLETE
#undef CLI_COMMAND
#endif														/* DOXYGEN_SHOULD_SKIP_THIS */
/* sccp show devices filters: registered | unregistered | model <text> | line <line> | firmware <text> */
static boolean_t sccp_cli_device_matches(constDevicePtr d, const char * filter, const char * value)
{
	if (sccp_strlen_zero(filter)) {
		return TRUE;
	}
	boolean_t registered = d->session && sccp_device_getRegistrationState(d) == SKINNY_DEVICE_RS_OK;
	if (sccp_strcaseequals(filter, "registered")) {
		return registered;
	}
	if (sccp_strcaseequals(filter, "unregistered")) {
		return !registered;
	}
	if (sccp_strlen_zero(value)) {
		return FALSE;
	}
	if (sccp_strcaseequals(filter, "model")) {
		return strcasestr(skinny_devicetype2str(d->skinny_type), value) || strcasestr(d->config_type, value);
	}
	if (sccp_strcaseequals(filter, "firmware")) {
		return strcasestr(d->loadedimageversion, value) != NULL;
	}
	if (sccp_strcaseequals(filter, "line")) {
		boolean_t              found  = FALSE;
		sccp_buttonconfig_t * config = NULL;
		SCCP_LIST_LOCK(&((devicePtr)d)->buttonconfig);
		SCCP_LIST_TRAVERSE(&d->buttonconfig, config, list) {
			if (config->type == LINE && sccp_strcaseequals(config->button.line.name, value)) {
				found = TRUE;
				break;
			}
		}
		SCCP_LIST_UNLOCK(&((devicePtr)d)->buttonconfig);
		return found;
	}
	return FALSE;
}

static boolean_t sccp_cli_device_filter_valid(const char * filter, const char * value)
{
	if (sccp_strlen_zero(filter)) {
		return TRUE;
	}
	if (sccp_strcaseequals(filter, "registered") || sccp_strcaseequals(filter, "unregistered")) {
		return sccp_strlen_zero(value);
	}
	return (sccp_strcaseequals(filter, "model") || sccp_strcaseequals(filter, "line") || sccp_strcaseequals(filter, "firmware")) && !sccp_strlen_zero(value);
}

    /* --------------------------------------------------------------------------------------------------------SHOW DEVICES- */
    /*!
     * \brief Show Devices
     * \param fd Fd as int
     * \param totals Total number of lines as int
     * \param s AMI Session
     * \param m Message
     * \param argc Argc as int
     * \param argv[] Argv[] as char
     * \return Result as int
     * 
     * \called_from_asterisk
     * 
     */
    //static int sccp_show_devices(int fd, int argc, char *argv[])
static int sccp_show_devices(int fd, sccp_cli_totals_t *totals, struct mansession *s, const struct message *m, int argc, char *argv[])
{
	char regtime[32];
	int local_line_total = 0;
	const char * filter = argc > 3 ? argv[3] : "";
	const char * value  = argc > 4 ? argv[4] : "";
	if (argc > 5 || !sccp_cli_device_filter_valid(filter, value)) {
		return RESULT_SHOWUSAGE;
	}
	CLI_AMI_LIST_START(s, m, "SCCPShowDevices");
	int shown = 0, registered = 0;
	char addrStr[INET6_ADDRSTRLEN + 8];
	struct ast_tm tm;

	// table definition
#define CLI_AMI_TABLE_NAME Devices
#define CLI_AMI_TABLE_PER_ENTRY_NAME Device

#define CLI_AMI_TABLE_LIST_ITER_TYPE sccp_device_t
#define CLI_AMI_TABLE_LIST_ITER_HEAD &GLOB(devices)
#define CLI_AMI_TABLE_LIST_ITER_VAR list_dev
#define CLI_AMI_TABLE_LIST_LOCK SCCP_RWLIST_RDLOCK
#define CLI_AMI_TABLE_LIST_ITERATOR SCCP_RWLIST_TRAVERSE
#define CLI_AMI_TABLE_BEFORE_ITERATION                                                                       \
	{                                                                                                    \
		AUTO_RELEASE (sccp_device_t, d, sccp_device_retain (list_dev));                              \
		if (d && sccp_cli_device_matches(d, filter, value)) {                                        \
			shown++;                                                                             \
			if (d->session && sccp_device_getRegistrationState(d) == SKINNY_DEVICE_RS_OK) {     \
				registered++;                                                                \
			}                                                                                    \
			if (d->session) {                                                                    \
				struct sockaddr_storage sas = { 0 };                                         \
				struct timeval when = { d->registrationTime, 0 };                            \
				ast_localtime (&when, &tm, NULL);                                            \
				ast_strftime (regtime, sizeof (regtime), "%Y-%m-%d %H:%M:%S", &tm);                        \
				sccp_session_getSas (d->session, &sas);                                      \
				sccp_copy_string (addrStr, sccp_netsock_stringify (&sas), sizeof (addrStr)); \
			} else {                                                                             \
				addrStr[0] = '\0';                                                           \
				regtime[0] = '\0';                                                           \
			}

#define CLI_AMI_TABLE_AFTER_ITERATION 																\
		}																		\
	}
#define CLI_AMI_TABLE_LIST_UNLOCK SCCP_RWLIST_UNLOCK

// Human-readable CLI headings are separate from the AMI field identifiers.
#define CLI_AMI_TABLE_FIELDS 																	\
		CLI_AMI_TABLE_FIELD_NAMED(MACAddress,	"Device",	"-16.16",	s,	16,	d->id)								\
		CLI_AMI_TABLE_UTF8_FIELD_NAMED(Description, "Description",	"-25.25",	s,	25,	d->description ? d->description : "")		\
		CLI_AMI_TABLE_FIELD_NAMED(IPAddress,	"Address",	"44.44",	s,	44,	addrStr[0] || s ? addrStr : "(not connected)")			\
		CLI_AMI_TABLE_FIELD(Status,		"-10.10",	s,	10, 	skinny_registrationstate2str(sccp_device_getRegistrationState(d)))		\
		CLI_AMI_TABLE_FIELD(Token,		"-5.5",		s,	5,	sccp_tokenstate2str(d->status.token)) 					\
		CLI_AMI_TABLE_FIELD(Registered,		"25.25",	s,	25, 	regtime[0] || s ? regtime : "(never)")					\
		CLI_AMI_TABLE_FIELD_NAMED(Active, "In Call",	"6.6",		s,	6, 	(d->active_channel) ? "yes" : "no")					\
		CLI_AMI_TABLE_FIELD(Lines, 		"-5",		d,	5, 	d->configurationStatistic.numberOfLines)				\
		CLI_AMI_TABLE_FIELD(NAT,		"9.9",		s, 	9,	sccp_nat2str(d->nat))							\
		CLI_AMI_TABLE_FIELD(Model,		"10.10",	s, 	10,	d->skinny_type == SKINNY_DEVICETYPE_UNDEFINED ? (d->config_type[0] ? d->config_type : "Unknown") : skinny_devicetype2str(d->skinny_type))	\
		CLI_AMI_TABLE_FIELD(Firmware,		"-20.20",	s, 	20,	d->loadedimageversion)
#include "sccp_cli_table.h"

	// end of table definition
	if (s) {
		totals->lines = local_line_total;
		totals->tables = 1;
	} else {
		pbx_cli(fd, "%d device%s, %d registered, %d not registered\n", shown, shown == 1 ? "" : "s", registered, shown - registered);
	}
	return RESULT_SUCCESS;
}

static char cli_devices_usage[] = "Usage: sccp show devices [registered | unregistered | model <text> | line <line> | firmware <text>]\n"
				  "       List SCCP devices, optionally only those registered, not registered, of a\n"
				  "       model, with a line, or running a firmware (model and firmware match part of\n"
				  "       the name).\n";

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#define CLI_COMMAND "sccp", "show", "devices"
#define CLI_COMPLETE SCCP_CLI_NULL_COMPLETER
CLI_AMI_ENTRY(show_devices, sccp_show_devices, "List defined SCCP devices", cli_devices_usage, FALSE, TRUE)
SCCP_AMI_ACTION(show_devices, sccp_show_devices, "SCCPShowDevices", SCCP_AMI_LIST_BY_HANDLER, "sccp", "show", "devices", "$Filter", "$Value")
#undef CLI_COMPLETE
#undef CLI_COMMAND
#endif														/* DOXYGEN_SHOULD_SKIP_THIS */

/* ------------------------------------------------------------------------------------------------------ SHOW FIRMWARE - */
/* sccp show firmware: which firmware each model runs, with how many devices and which */
typedef struct {
	char   model[40];
	char   firmware[StationMaxImageVersionSize];
	int    count;
	char   devices[160];
	size_t more;
} sccp_cli_firmware_row_t;

static int sccp_cli_firmware_row_cmp(const void * a, const void * b)
{
	const sccp_cli_firmware_row_t * ra = (const sccp_cli_firmware_row_t *)a;
	const sccp_cli_firmware_row_t * rb = (const sccp_cli_firmware_row_t *)b;
	int res = strcasecmp(ra->model, rb->model);
	return res ? res : strcasecmp(ra->firmware, rb->firmware);
}

static int sccp_show_firmware(int fd, sccp_cli_totals_t * totals, struct mansession * s, const struct message * m, int argc, char * argv[])
{
	if (argc != 3) {
		return RESULT_SHOWUSAGE;
	}
	sccp_cli_firmware_row_t * rows  = NULL;
	size_t                    nrows = 0;
	sccp_device_t *           d     = NULL;
	int                       local_line_total = 0;

	SCCP_RWLIST_RDLOCK(&GLOB(devices));
	SCCP_RWLIST_TRAVERSE(&GLOB(devices), d, list) {
		char model[40];
		snprintf(model, sizeof(model), "%s", d->skinny_type != SKINNY_DEVICETYPE_UNDEFINED ? skinny_devicetype2str(d->skinny_type) : (d->config_type[0] ? d->config_type : "Unknown"));
		const char * firmware = d->loadedimageversion[0] ? d->loadedimageversion : "";
		size_t       r        = 0;
		for (r = 0; r < nrows; r++) {
			if (!strcasecmp(rows[r].model, model) && !strcmp(rows[r].firmware, firmware)) {
				break;
			}
		}
		if (r == nrows) {
			sccp_cli_firmware_row_t * tmp = (sccp_cli_firmware_row_t *)sccp_realloc(rows, (nrows + 1) * sizeof(*rows));
			if (!tmp) {
				break;
			}
			rows = tmp;
			memset(&rows[r], 0, sizeof(rows[r]));
			sccp_copy_string(rows[r].model, model, sizeof(rows[r].model));
			sccp_copy_string(rows[r].firmware, firmware, sizeof(rows[r].firmware));
			nrows++;
		}
		rows[r].count++;
		size_t used = strlen(rows[r].devices);
		if (used + strlen(d->id) + 3 < sizeof(rows[r].devices)) {
			snprintf(rows[r].devices + used, sizeof(rows[r].devices) - used, "%s%s", used ? ", " : "", d->id);
		} else {
			rows[r].more++;
		}
	}
	SCCP_RWLIST_UNLOCK(&GLOB(devices));
	if (nrows) {
		qsort(rows, nrows, sizeof(*rows), sccp_cli_firmware_row_cmp);
	}

	CLI_AMI_LIST_START(s, m, "SCCPShowFirmware");
	const char * actionid = s ? astman_get_header(m, "ActionID") : "";
	const char * const headers[] = { "Model", "Firmware", "Devices", "Device names" };
	sccp_cli_table_data_t table = { .headers = headers, .columns = ARRAY_LEN(headers) };
	for (size_t r = 0; r < nrows; r++) {
		char names[200];
		if (rows[r].more) {
			snprintf(names, sizeof(names), "%s and %zu more", rows[r].devices, rows[r].more);
		} else {
			snprintf(names, sizeof(names), "%s", rows[r].devices);
		}
		if (!s) {
			sccp_cli_table_add(&table, "%s", rows[r].model);
			sccp_cli_table_add(&table, "%s", rows[r].firmware[0] ? rows[r].firmware : "(not reported)");
			sccp_cli_table_add(&table, "%d", rows[r].count);
			sccp_cli_table_add(&table, "%s", names);
		} else {
			astman_append(s, "Event: SCCPFirmwareEntry\r\n");
			if (!sccp_strlen_zero(actionid)) {
				astman_append(s, "ActionID: %s\r\n", actionid);
			}
			astman_append(s, "Model: %s\r\nFirmware: %s\r\nDevices: %d\r\nDeviceNames: %s\r\n\r\n", rows[r].model, rows[r].firmware, rows[r].count, names);
			local_line_total += 6;
		}
	}
	sccp_free(rows);
	if (!s) {
		sccp_cli_table_print(&table, fd, "Firmware");
	} else {
		totals->lines  = local_line_total;
		totals->tables = 1;
	}
	return RESULT_SUCCESS;
}

static char cli_show_firmware_usage[] = "Usage: sccp show firmware\n"
					"       List the firmware each phone model reports, with the devices running it.\n";

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#define CLI_COMMAND "sccp", "show", "firmware"
#define CLI_COMPLETE SCCP_CLI_NULL_COMPLETER
CLI_AMI_ENTRY(show_firmware, sccp_show_firmware, "List phone firmware", cli_show_firmware_usage, FALSE, TRUE)
SCCP_AMI_ACTION(show_firmware, sccp_show_firmware, "SCCPShowFirmware", SCCP_AMI_LIST_BY_HANDLER, "sccp", "show", "firmware")
#undef CLI_COMPLETE
#undef CLI_COMMAND
#endif														/* DOXYGEN_SHOULD_SKIP_THIS */
/* sccp show device <device> calls: quality of the last calls, newest first */
static int sccp_show_device_calls(int fd, sccp_cli_totals_t * totals, struct mansession * s, const struct message * m, constDevicePtr d)
{
	int          local_line_total = 0;
	const char * actionid         = s ? astman_get_header(m, "ActionID") : "";
	const char * const headers[]  = { "Ended", "Call", "Sent", "Received", "Lost", "Loss", "Jitter (ms)", "Latency (ms)", "MOS", "Min MOS", "Concealed (s)", "Severe (s)" };
	sccp_cli_table_data_t table   = { .headers = headers, .columns = ARRAY_LEN(headers) };

	CLI_AMI_LIST_START(s, m, "SCCPShowDeviceCalls");
	for (int i = 0; i < d->call_history.count; i++) {
		const sccp_call_quality_t * q = &d->call_history.entry[(d->call_history.next + SCCP_CALL_HISTORY_SIZE - 1 - i) % SCCP_CALL_HISTORY_SIZE];
		char                        ended[32];
		struct ast_tm               tm;
		struct timeval              when = { q->ended, 0 };
		ast_localtime(&when, &tm, NULL);
		ast_strftime(ended, sizeof(ended), "%Y-%m-%d %H:%M:%S", &tm);
		uint32_t expected = q->packets_received + q->packets_lost;
		double   loss     = expected ? 100.0 * q->packets_lost / expected : 0.0;
		if (!s) {
			sccp_cli_table_add(&table, "%s", ended);
			sccp_cli_table_add(&table, "%u", q->callid);
			sccp_cli_table_add(&table, "%u", q->packets_sent);
			sccp_cli_table_add(&table, "%u", q->packets_received);
			sccp_cli_table_add(&table, "%u", q->packets_lost);
			sccp_cli_table_add(&table, "%.1f%%", loss);
			sccp_cli_table_add(&table, "%u", q->jitter);
			sccp_cli_table_add(&table, "%u", q->latency);
			sccp_cli_table_add(&table, "%.2f", q->mos_average);
			sccp_cli_table_add(&table, "%.2f", q->mos_minimum);
			sccp_cli_table_add(&table, "%u", q->concealed_seconds);
			sccp_cli_table_add(&table, "%u", q->severely_concealed_seconds);
		} else {
			astman_append(s, "Event: SCCPDeviceCallEntry\r\n");
			if (!sccp_strlen_zero(actionid)) {
				astman_append(s, "ActionID: %s\r\n", actionid);
			}
			astman_append(s,
				      "Device: %s\r\nEnded: %s\r\nCallID: %u\r\nPacketsSent: %u\r\nPacketsReceived: %u\r\nPacketsLost: %u\r\nLossPercent: %.1f\r\n"
				      "Jitter: %u\r\nLatency: %u\r\nMOS: %.2f\r\nMinMOS: %.2f\r\nConcealedSeconds: %u\r\nSeverelyConcealedSeconds: %u\r\n\r\n",
				      d->id, ended, q->callid, q->packets_sent, q->packets_received, q->packets_lost, loss, q->jitter, q->latency, q->mos_average, q->mos_minimum,
				      q->concealed_seconds, q->severely_concealed_seconds);
			local_line_total += 16;
		}
	}
	if (!s) {
		char title[64];
		snprintf(title, sizeof(title), "Calls on %s (newest first, last %d kept)", d->id, SCCP_CALL_HISTORY_SIZE);
		sccp_cli_table_print(&table, fd, title);
	} else {
		totals->lines  = local_line_total;
		totals->tables = 1;
	}
	return RESULT_SUCCESS;
}

    /* --------------------------------------------------------------------------------------------------------SHOW DEVICE- */
    /*!
     * \brief Show Device
     * \param fd Fd as int
     * \param totals Total number of lines as int
     * \param s AMI Session
     * \param m Message
     * \param argc Argc as int
     * \param argv[] Argv[] as char
     * \return Result as int
     * 
     * \called_from_asterisk
     * 
     * \warning
     *   - device->buttonconfig is not always locked
     */
static int sccp_show_device(int fd, sccp_cli_totals_t *totals, struct mansession *s, const struct message *m, int argc, char *argv[])
{
	pbx_str_t *ha_buf = pbx_str_alloca(DEFAULT_PBX_STR_BUFFERSIZE);
	pbx_str_t *permithost_buf = pbx_str_alloca(DEFAULT_PBX_STR_BUFFERSIZE);
	PBX_VARIABLE_TYPE *v = NULL;
	int local_line_total = 0;
	int local_table_total = 0;
	const char *actionid = "";
	char clientAddress[INET6_ADDRSTRLEN + 8];
	char serverAddress[INET6_ADDRSTRLEN + 8];

	const char * dev = NULL;

	if (argc < 4) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "A device name is required%s\n", "");		/* explicit return */
	}
	dev = pbx_strdupa(argv[3]);
	if (pbx_strlen_zero(dev)) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "A device name is required%s\n", "");		/* explicit return */
	}
	AUTO_RELEASE(sccp_device_t, d , sccp_device_find_byid(dev, FALSE));

	if (!d) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "Device %s does not exist\n", dev);		/* explicit return */
	}
	if (argc > 5 || (argc == 5 && !sccp_strcaseequals(argv[4], "calls"))) {
		return RESULT_SHOWUSAGE;
	}
	if (argc == 5) {
		return sccp_show_device_calls(fd, totals, s, m, d);
	}
	CLI_AMI_LIST_START(s, m, "SCCPShowDevice");
	char apref_buf[256];
	char acap_buf[512];
	sccp_cli_codec_list(apref_buf, sizeof(apref_buf), d->preferences.audio, ARRAY_LEN(d->preferences.audio));
	sccp_cli_codec_list(acap_buf, sizeof(acap_buf), d->capabilities.audio, ARRAY_LEN(d->capabilities.audio));
#if CS_SCCP_VIDEO
	char vpref_buf[256];
	char vcap_buf[512];
	sccp_cli_codec_list(vpref_buf, sizeof(vpref_buf), d->preferences.video, ARRAY_LEN(d->preferences.video));
	sccp_cli_codec_list(vcap_buf, sizeof(vcap_buf), d->capabilities.video, ARRAY_LEN(d->capabilities.video));
#endif
	sccp_print_ha(ha_buf, DEFAULT_PBX_STR_BUFFERSIZE, d->ha);

	if (d->session) {
		struct sockaddr_storage sas = { 0 };
		sccp_session_getSas(d->session, &sas);
		sccp_copy_string(clientAddress, sccp_netsock_stringify(&sas), sizeof(clientAddress));
		struct sockaddr_storage ourip = { 0 };
		sccp_session_getOurIP(d->session, &ourip, 0);
		sccp_copy_string(serverAddress, sccp_netsock_stringify(&ourip), sizeof(serverAddress));
	} else {
		snprintf(clientAddress, sizeof(clientAddress), "%s", s ? "" : "(not connected)");
		snprintf(serverAddress, sizeof(serverAddress), "%s", s ? "" : "(not connected)");
	}

	sccp_hostname_t * hostname = NULL;
	sccp_accessory_t activeAccessory = sccp_device_getActiveAccessory(d);
	sccp_accessorystate_t activeAccessoryState = sccp_device_getAccessoryStatus(d, activeAccessory);

	SCCP_LIST_TRAVERSE(&d->permithosts, hostname, list) {
		ast_str_append(&permithost_buf, DEFAULT_PBX_STR_BUFFERSIZE, "%s%s", ast_str_strlen(permithost_buf) ? ", " : "", hostname->name);
	}

	if (s) {
		astman_append(s, "Event: SCCPShowDevice\r\n");
		actionid = astman_get_header(m, "ActionID");
		if (!pbx_strlen_zero(actionid)) {
			astman_append(s, "ActionID: %s\r\n", actionid);
		}
		local_line_total++;
	}

	pbx_str_t *addons_buf = pbx_str_alloca(DEFAULT_PBX_STR_BUFFERSIZE);
	SCCP_LIST_LOCK(&d->addons);
	if (SCCP_LIST_GETSIZE(&d->addons)) {
		sccp_addon_t *addon = NULL;
		int comma = 0;
		SCCP_LIST_TRAVERSE(&d->addons, addon, list) {
			pbx_str_append(&addons_buf, DEFAULT_PBX_STR_BUFFERSIZE, "%s%s", comma++ ? ", " : "", skinny_devicetype2str(addon->type));
		}
	}
	SCCP_LIST_UNLOCK(&d->addons);

	char protocol[64] = "";
	if (d->protocol) {
		snprintf(protocol, sizeof(protocol), "%s %d (phone supports %d)", d->protocol->type == SCCP_PROTOCOL ? "SCCP" : "SPCP", d->inuseprotocolversion, d->protocolversion);
	}
	char redial[SCCP_MAX_EXTENSION + 16] = "";
	if (!sccp_strlen_zero(d->redialInformation.number)) {
		snprintf(redial, sizeof(redial), "%s (line instance %d)", d->redialInformation.number, d->redialInformation.lineInstance);
	}
	char accessory[64] = "";
	if (activeAccessory) {
		snprintf(accessory, sizeof(accessory), "%s, %s", sccp_accessory2str(activeAccessory), sccp_accessorystate2str(activeAccessoryState));
	}
	char softkeyset[64];
	snprintf(softkeyset, sizeof(softkeyset), "%s%s", d->softkeyDefinition, d->softkeyset ? "" : " (not loaded)");
	char ipv4[INET6_ADDRSTRLEN + 8];
	char ipv6[INET6_ADDRSTRLEN + 8];
	sccp_copy_string(ipv4, sccp_netsock_stringify(&d->ipv4), sizeof(ipv4));
	sccp_copy_string(ipv6, sccp_netsock_stringify(&d->ipv6), sizeof(ipv6));
	int features = (d->device_features.phoneFeatures[0] << 16) + (d->device_features.phoneFeatures[1] << 8) + d->device_features.phoneFeatures[2];

	/* clang-format off */
#undef CLI_AMI_LIST_WIDTH
#define CLI_AMI_LIST_WIDTH 35
	CLI_TITLE("Device");
	CLI_AMI_OUTPUT_PARAM("Name",				CLI_AMI_LIST_WIDTH, "%s", d->id);
	CLI_AMI_OUTPUT_PARAM("Description",			CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(d->description));
	CLI_AMI_OUTPUT_PARAM("Configured model",		CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(d->config_type));
	CLI_AMI_OUTPUT_PARAM("Reported model",			CLI_AMI_LIST_WIDTH, "%s (type %d)", skinny_devicetype2str(d->skinny_type), d->skinny_type);
	CLI_AMI_OUTPUT_PARAM("Firmware",			CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(d->loadedimageversion));
	CLI_AMI_OUTPUT_LIST("Add-ons",				CLI_AMI_LIST_WIDTH, pbx_str_buffer(addons_buf));
	CLI_AMI_OUTPUT_PARAM("Registration state",		CLI_AMI_LIST_WIDTH, "%s", skinny_registrationstate2str(sccp_device_getRegistrationState(d)));
	CLI_AMI_OUTPUT_PARAM("Device state",			CLI_AMI_LIST_WIDTH, "%s", sccp_devicestate2str(sccp_device_getDeviceState(d)));
	CLI_AMI_OUTPUT_PARAM("Token state",			CLI_AMI_LIST_WIDTH, "%s", sccp_tokenstate2str(d->status.token));
	CLI_AMI_OUTPUT_PARAM("Protocol",			CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(protocol));
	CLI_AMI_OUTPUT_PARAM("Phone feature flags",		CLI_AMI_LIST_WIDTH, "0x%06x", features);
	CLI_AMI_OUTPUT_YES_NO("Button template sent",		CLI_AMI_LIST_WIDTH, d->buttonTemplate);
	CLI_AMI_OUTPUT_YES_NO("Lines registered",		CLI_AMI_LIST_WIDTH, d->linesRegistered);
	CLI_AMI_OUTPUT_YES_NO("Softkeys supported",		CLI_AMI_LIST_WIDTH, d->softkeysupport);
	CLI_AMI_OUTPUT_PARAM("Softkey set",			CLI_AMI_LIST_WIDTH, "%s", softkeyset);
	CLI_AMI_OUTPUT_PARAM("Default line instance",		CLI_AMI_LIST_WIDTH, "%d", d->defaultLineInstance);
	CLI_AMI_OUTPUT_PARAM("Accessory",			CLI_AMI_LIST_WIDTH, "%s", CLI_NONE(accessory));
	CLI_AMI_OUTPUT_PARAM("Time zone offset",		CLI_AMI_LIST_WIDTH, "%d", d->tz_offset);
	CLI_AMI_OUTPUT_YES_NO("Restart pending",		CLI_AMI_LIST_WIDTH, d->pendingUpdate);
	CLI_AMI_OUTPUT_YES_NO("Removal pending",		CLI_AMI_LIST_WIDTH, d->pendingDelete);

	CLI_SECTION("Network");
	CLI_AMI_OUTPUT_PARAM("Phone address",			CLI_AMI_LIST_WIDTH, "%s", clientAddress);
	CLI_AMI_OUTPUT_PARAM("Server address",			CLI_AMI_LIST_WIDTH, "%s", serverAddress);
	CLI_AMI_OUTPUT_PARAM("Reported IPv4 address",		CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(ipv4));
	CLI_AMI_OUTPUT_PARAM("Reported IPv6 address",		CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(ipv6));
	CLI_AMI_OUTPUT_PARAM("NAT",				CLI_AMI_LIST_WIDTH, "%s", sccp_nat2str(d->nat));
	CLI_AMI_OUTPUT_PARAM("Keepalive (s)",			CLI_AMI_LIST_WIDTH, "%d", d->keepalive);
	CLI_AMI_OUTPUT_BOOL("Direct RTP",			CLI_AMI_LIST_WIDTH, d->directrtp);
	CLI_AMI_OUTPUT_BOOL("Early RTP",			CLI_AMI_LIST_WIDTH, d->earlyrtp);
	CLI_AMI_OUTPUT_BOOL("Trust phone IP (deprecated)",	CLI_AMI_LIST_WIDTH, d->trustphoneip);
	CLI_AMI_OUTPUT_LIST("Deny/permit",			CLI_AMI_LIST_WIDTH, pbx_str_buffer(ha_buf));
	CLI_AMI_OUTPUT_LIST("Permitted hosts",			CLI_AMI_LIST_WIDTH, pbx_str_buffer(permithost_buf));
	CLI_AMI_OUTPUT_PARAM("Audio TOS/COS",			CLI_AMI_LIST_WIDTH, "%d/%d", d->audio_tos, d->audio_cos);
	CLI_AMI_OUTPUT_PARAM("Video TOS/COS",			CLI_AMI_LIST_WIDTH, "%d/%d", d->video_tos, d->video_cos);

	CLI_SECTION("Media");
	CLI_AMI_OUTPUT_LIST("Audio codecs (phone)",		CLI_AMI_LIST_WIDTH, acap_buf);
	CLI_AMI_OUTPUT_LIST("Audio codecs (preference)",	CLI_AMI_LIST_WIDTH, apref_buf);
#if CS_SCCP_VIDEO
	CLI_AMI_OUTPUT_LIST("Video codecs (phone)",		CLI_AMI_LIST_WIDTH, vcap_buf);
	CLI_AMI_OUTPUT_LIST("Video codecs (preference)",	CLI_AMI_LIST_WIDTH, vpref_buf);
#endif
	CLI_AMI_OUTPUT_YES_NO("Video supported",		CLI_AMI_LIST_WIDTH, sccp_device_isVideoSupported(d));
	CLI_AMI_OUTPUT_PARAM("DTMF mode",			CLI_AMI_LIST_WIDTH, "%s", sccp_dtmfmode2str(d->getDtmfMode(d)));

	CLI_SECTION("Features");
	CLI_AMI_OUTPUT_BOOL("Do not disturb",			CLI_AMI_LIST_WIDTH, d->dndFeature.enabled);
	CLI_AMI_OUTPUT_PARAM("Do not disturb status",		CLI_AMI_LIST_WIDTH, "%s", d->dndFeature.status ? sccp_dndmode2str((sccp_dndmode_t)d->dndFeature.status) : "off");
	CLI_AMI_OUTPUT_PARAM("Do not disturb softkey",		CLI_AMI_LIST_WIDTH, "%s", d->dndmode == SCCP_DNDMODE_REJECT ? "toggles reject" : d->dndmode == SCCP_DNDMODE_SILENT ? "toggles silent" : "cycles reject, silent, off");
	CLI_AMI_OUTPUT_BOOL("Transfer",				CLI_AMI_LIST_WIDTH, d->transfer);
	CLI_AMI_OUTPUT_BOOL("Park",				CLI_AMI_LIST_WIDTH, d->park);
	CLI_AMI_OUTPUT_BOOL("Call forward all",			CLI_AMI_LIST_WIDTH, d->cfwdall);
	CLI_AMI_OUTPUT_BOOL("Call forward busy",		CLI_AMI_LIST_WIDTH, d->cfwdbusy);
	CLI_AMI_OUTPUT_BOOL("Call forward no answer",		CLI_AMI_LIST_WIDTH, d->cfwdnoanswer);
	CLI_AMI_OUTPUT_BOOL("Privacy softkey",			CLI_AMI_LIST_WIDTH, d->privacyFeature.enabled);
	CLI_AMI_OUTPUT_BOOL("Ringing notification",		CLI_AMI_LIST_WIDTH, d->allowRinginNotification);
	CLI_AMI_OUTPUT_BOOL("Redial opens placed calls",	CLI_AMI_LIST_WIDTH, d->useRedialMenu);
	CLI_AMI_OUTPUT_PARAM("Last dialed number",		CLI_AMI_LIST_WIDTH, "%s", CLI_NONE(redial));
	CLI_AMI_OUTPUT_PARAM("Answered elsewhere in call history", CLI_AMI_LIST_WIDTH, "%s", skinny_callHistoryDisposition2str(d->callhistory_answered_elsewhere));
	CLI_AMI_OUTPUT_PARAM("Voicemail messages",		CLI_AMI_LIST_WIDTH, "%d new, %d old", d->voicemailStatistic.newmsgs, d->voicemailStatistic.oldmsgs);
	CLI_AMI_OUTPUT_PARAM("Message lamp",			CLI_AMI_LIST_WIDTH, "%s", skinny_lampmode2str(d->mwilamp));
	CLI_AMI_OUTPUT_PARAM("Message lamp during calls",	CLI_AMI_LIST_WIDTH, "%s", d->mwioncall ? "stays on" : "turned off");
	CLI_AMI_OUTPUT_PARAM("Background image",		CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(d->backgroundImage));
	CLI_AMI_OUTPUT_PARAM("Background thumbnail",		CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(d->backgroundTN));
	CLI_AMI_OUTPUT_PARAM("Ringtone",			CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(d->ringtone));
#ifdef CS_SCCP_CONFERENCE

	CLI_SECTION("Conference");
	CLI_AMI_OUTPUT_BOOL("Conference",			CLI_AMI_LIST_WIDTH, d->allow_conference);
	CLI_AMI_OUTPUT_BOOL("Conference announcement",		CLI_AMI_LIST_WIDTH, d->conf_play_general_announce);
	CLI_AMI_OUTPUT_BOOL("Conference participant announcement", CLI_AMI_LIST_WIDTH, d->conf_play_part_announce);
	CLI_AMI_OUTPUT_BOOL("Conference mute on entry",		CLI_AMI_LIST_WIDTH, d->conf_mute_on_entry);
	CLI_AMI_OUTPUT_PARAM("Conference music on hold class",	CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(d->conf_music_on_hold_class));
	CLI_AMI_OUTPUT_BOOL("Conference participant list",	CLI_AMI_LIST_WIDTH, d->conf_show_conflist);
	CLI_AMI_OUTPUT_BOOL("Conference list open",		CLI_AMI_LIST_WIDTH, d->conferencelist_active);
#endif
#undef CLI_AMI_LIST_WIDTH
#define CLI_AMI_LIST_WIDTH 46
	if (!s) {
		pbx_cli(fd, "\n");
	}
	if (s) {
		astman_append(s, "\r\n");
	}

	/* clang-format on */
	if (SCCP_LIST_FIRST(&d->buttonconfig)) {
		// BUTTONS
#define CLI_AMI_TABLE_NAME Buttons
#define CLI_AMI_TABLE_PER_ENTRY_NAME DeviceButton
#define CLI_AMI_TABLE_LIST_ITER_TYPE sccp_buttonconfig_t
#define CLI_AMI_TABLE_LIST_ITER_HEAD &d->buttonconfig
#define CLI_AMI_TABLE_LIST_ITER_VAR buttonconfig
#define CLI_AMI_TABLE_LIST_LOCK SCCP_LIST_LOCK
#define CLI_AMI_TABLE_LIST_ITERATOR SCCP_LIST_TRAVERSE
#define CLI_AMI_TABLE_LIST_UNLOCK SCCP_LIST_UNLOCK
// pendDel used to read buttonconfig->pendingUpdate a second time (copy-paste from pendUpdt) -
// pendingDelete is a real, distinct field (sccp_device.h) that was never actually shown.
#define CLI_AMI_TABLE_FIELDS 																\
			CLI_AMI_TABLE_FIELD_NAMED(Id, "ID",			"-4",	d,		4,	buttonconfig->index + 1)				\
			CLI_AMI_TABLE_FIELD(Instance,		"-9",	d,		9,	buttonconfig->instance)					\
			CLI_AMI_TABLE_FIELD(Type,		"-40",	s,		40,	sccp_config_buttontype2str(buttonconfig->type))		\
			CLI_AMI_TABLE_FIELD_NAMED(TypeID, "Type ID",		"-37",	d,		37,	buttonconfig->type)					\
			CLI_AMI_TABLE_FIELD_NAMED(PendingUpdate, "Update Pending", "-14",	s,		14, 	buttonconfig->pendingUpdate ? "yes" : "no")	\
			CLI_AMI_TABLE_FIELD_NAMED(PendingDelete, "Delete Pending", "-14",	s,		14, 	buttonconfig->pendingDelete ? "yes" : "no")	\
			CLI_AMI_TABLE_FIELD(Default,		"-9",	s,		9,	(0!=buttonconfig->instance && d->defaultLineInstance == buttonconfig->instance && LINE==buttonconfig->type) ? "yes" : "no")
#include "sccp_cli_table.h"
			local_table_total++;
		// LINES
#define CLI_AMI_TABLE_NAME LineButtons
#define CLI_AMI_TABLE_TITLE "Line buttons"
#define CLI_AMI_TABLE_PER_ENTRY_NAME DeviceLine
#define CLI_AMI_TABLE_LIST_ITER_HEAD &d->buttonconfig
#define CLI_AMI_TABLE_LIST_ITER_VAR buttonconfig
#define CLI_AMI_TABLE_LIST_LOCK SCCP_LIST_LOCK
#define CLI_AMI_TABLE_LIST_ITERATOR SCCP_LIST_TRAVERSE
#define CLI_AMI_TABLE_BEFORE_ITERATION                                                                                                                                                   \
	if(buttonconfig->type == LINE) {                                                                                                                                                 \
		AUTO_RELEASE(sccp_line_t, l, sccp_line_find_byname(buttonconfig->button.line.name, FALSE));                                                                              \
		char subscriptionIdBuf[21] = "";                                                                                                                                         \
		if(buttonconfig->button.line.subscriptionId) {                                                                                                                           \
			snprintf(subscriptionIdBuf, 21, "(%s)%s:%s", buttonconfig->button.line.subscriptionId->replaceCid ? "=" : "+", buttonconfig->button.line.subscriptionId->number, \
				 buttonconfig->button.line.subscriptionId->name);                                                                                                        \
		}                                                                                                                                                                        \
		if(l) {                                                                                                                                                                  \
			AUTO_RELEASE(sccp_linedevice_t, ld, sccp_linedevice_find(d, l));                                                                                                 \
			char cfwd_str_buf[256] = "";                                                                                                                                     \
			sccp_linedevice_get_cfwd_string(ld, cfwd_str_buf, sizeof(cfwd_str_buf));
#define CLI_AMI_TABLE_AFTER_ITERATION 															\
				}															\
			}
#define CLI_AMI_TABLE_LIST_UNLOCK SCCP_LIST_UNLOCK
#define CLI_AMI_TABLE_FIELDS                                                                                                                                                      \
	CLI_AMI_TABLE_FIELD_NAMED(Id, "ID", "-4", d, 4, buttonconfig->index + 1)                                                                                                              \
	CLI_AMI_TABLE_UTF8_FIELD(Name, "-23.23", s, 23, l->name)                                                                                                                  \
	CLI_AMI_TABLE_FIELD_NAMED(SubId, "Subscription ID", "-21.21", s, 21, subscriptionIdBuf)                                                                                                            \
	CLI_AMI_TABLE_UTF8_FIELD(Label, "-18.18", s, 18, buttonconfig->button.line.subscriptionId ? buttonconfig->button.line.subscriptionId->label : (l->label ? l->label : "")) \
	CLI_AMI_TABLE_FIELD_NAMED(CallForward, "Call Forward", "46.46", s, 46, cfwd_str_buf)
#include "sccp_cli_table.h"
			local_table_total++;
		// SPEEDDIALS
#define CLI_AMI_TABLE_NAME SpeeddialButtons
#define CLI_AMI_TABLE_TITLE "Speed dial buttons"
#define CLI_AMI_TABLE_PER_ENTRY_NAME DeviceSpeeddial
#define CLI_AMI_TABLE_LIST_ITER_HEAD &d->buttonconfig
#define CLI_AMI_TABLE_LIST_ITER_VAR buttonconfig
#define CLI_AMI_TABLE_LIST_LOCK SCCP_LIST_LOCK
#define CLI_AMI_TABLE_LIST_ITERATOR SCCP_LIST_TRAVERSE
#define CLI_AMI_TABLE_BEFORE_ITERATION 														\
			if (buttonconfig->type == SPEEDDIAL) {											
			
#define CLI_AMI_TABLE_AFTER_ITERATION 														\
			}
#define CLI_AMI_TABLE_LIST_UNLOCK SCCP_LIST_UNLOCK

#define CLI_AMI_TABLE_FIELDS 															\
			CLI_AMI_TABLE_FIELD_NAMED(Id, "ID",			"-4",		d,	4,	buttonconfig->index + 1)			\
			CLI_AMI_TABLE_UTF8_FIELD(Name,		"-23.23",	s,	23,	buttonconfig->label ? buttonconfig->label : "")				\
			CLI_AMI_TABLE_FIELD(Number,		"-22.22",	s,	22,	buttonconfig->button.speeddial.ext ? buttonconfig->button.speeddial.ext : "")		\
			CLI_AMI_TABLE_FIELD(Hint,		"-64.64",	s,	64, 	buttonconfig->button.speeddial.hint ? buttonconfig->button.speeddial.hint : "")
#include "sccp_cli_table.h"
			local_table_total++;

		// FEATURES
#define CLI_AMI_TABLE_NAME FeatureButtons
#define CLI_AMI_TABLE_TITLE "Feature buttons"
#define CLI_AMI_TABLE_PER_ENTRY_NAME DeviceFeature
#define CLI_AMI_TABLE_LIST_ITER_HEAD &d->buttonconfig
#define CLI_AMI_TABLE_LIST_ITER_VAR buttonconfig
#define CLI_AMI_TABLE_LIST_LOCK SCCP_LIST_LOCK
#define CLI_AMI_TABLE_LIST_ITERATOR SCCP_LIST_TRAVERSE
#define CLI_AMI_TABLE_BEFORE_ITERATION 														\
			if (buttonconfig->type == FEATURE) {
#define CLI_AMI_TABLE_AFTER_ITERATION 														\
			}
#define CLI_AMI_TABLE_LIST_UNLOCK SCCP_LIST_UNLOCK

#define CLI_AMI_TABLE_FIELDS                                                                                                            \
	CLI_AMI_TABLE_FIELD_NAMED(Id, "ID", "-4", d, 4, buttonconfig->index + 1)                                                                    \
	CLI_AMI_TABLE_UTF8_FIELD(Name, "-23.23", s, 23, buttonconfig->label ? buttonconfig->label : "")                                 \
	CLI_AMI_TABLE_FIELD(Status, "-22", d, 22, buttonconfig->button.feature.status)                                                  \
	CLI_AMI_TABLE_FIELD(Options, "-14.14", s, 14, buttonconfig->button.feature.options ? buttonconfig->button.feature.options : "") \
	CLI_AMI_TABLE_FIELD(Args, "-49.49", s, 49, buttonconfig->button.feature.args ? buttonconfig->button.feature.args : "")
#include "sccp_cli_table.h"
			local_table_total++;

		// SERVICEURL
#define CLI_AMI_TABLE_NAME ServiceURLButtons
#define CLI_AMI_TABLE_TITLE "Service URL buttons"
#define CLI_AMI_TABLE_PER_ENTRY_NAME DeviceServiceURL
#define CLI_AMI_TABLE_LIST_ITER_HEAD &d->buttonconfig
#define CLI_AMI_TABLE_LIST_ITER_VAR buttonconfig
#define CLI_AMI_TABLE_LIST_LOCK SCCP_LIST_LOCK
#define CLI_AMI_TABLE_LIST_ITERATOR SCCP_LIST_TRAVERSE
#define CLI_AMI_TABLE_BEFORE_ITERATION 														\
			if (buttonconfig->type == SERVICE) {
#define CLI_AMI_TABLE_AFTER_ITERATION 														\
			}
#define CLI_AMI_TABLE_LIST_UNLOCK SCCP_LIST_UNLOCK

#define CLI_AMI_TABLE_FIELDS 															\
			CLI_AMI_TABLE_FIELD_NAMED(Id, "ID",			"-4",		d,	4,	buttonconfig->index + 1)			\
			CLI_AMI_TABLE_UTF8_FIELD(Name,		"-22.22",	s,	22,	buttonconfig->label ? buttonconfig->label : "")				\
			CLI_AMI_TABLE_FIELD(URL,		"-88.88",	s,	88,	buttonconfig->button.service.url ? buttonconfig->button.service.url : "")
#include "sccp_cli_table.h"
			local_table_total++;
	}

	if (d->variables) {
		// VARIABLES
#define CLI_AMI_TABLE_NAME Variables
#define CLI_AMI_TABLE_PER_ENTRY_NAME Variable
#define CLI_AMI_TABLE_ITERATOR for(v = d->variables;v;v = v->next)
#define CLI_AMI_TABLE_FIELDS 															\
			CLI_AMI_TABLE_FIELD(Name,		"-28.28",	s,	28,	v->name)					\
			CLI_AMI_TABLE_FIELD(Value,		"-87.87",	s,	87,	v->value)
#include "sccp_cli_table.h"
			local_table_total++;
	}

	sccp_call_statistics_type_t callstattype = 0;
	sccp_call_statistics_t *stats = NULL;

#define CLI_AMI_TABLE_NAME CallStatistics
#define CLI_AMI_TABLE_TITLE "Call statistics"
#define CLI_AMI_TABLE_PER_ENTRY_NAME DeviceStatistics
#define CLI_AMI_TABLE_ITERATOR for(callstattype = SCCP_CALLSTATISTIC_LAST; callstattype <= SCCP_CALLSTATISTIC_AVG; enum_incr(callstattype))
#define CLI_AMI_TABLE_BEFORE_ITERATION stats = &d->call_statistics[callstattype];
#define CLI_AMI_TABLE_FIELDS																	\
			CLI_AMI_TABLE_FIELD(Type,		"-8.8",		s,	8,	(callstattype == SCCP_CALLSTATISTIC_LAST) ? "Last call" : "Average")	\
			CLI_AMI_TABLE_FIELD(Calls,		"-8",		d,	8,	stats->num)							\
			CLI_AMI_TABLE_FIELD_NAMED(PcktSnt, "Packets Sent",		"-8",		d,	8,	stats->packets_sent)						\
			CLI_AMI_TABLE_FIELD_NAMED(PcktRcvd, "Packets Received",		"-8",		d,	8,	stats->packets_received)					\
			CLI_AMI_TABLE_FIELD(Lost,		"-8",		d,	8,	stats->packets_lost)						\
			CLI_AMI_TABLE_FIELD(Jitter,		"-8",		d,	8,	stats->jitter)							\
			CLI_AMI_TABLE_FIELD(Latency,		"-8",		d,	8,	stats->latency)							\
			CLI_AMI_TABLE_FIELD(Quality,		"1.6",		f,	8,	stats->opinion_score_listening_quality)				\
			CLI_AMI_TABLE_FIELD_NAMED(avgQual, "Avg Quality",		"1.6",		f,	8,	stats->avg_opinion_score_listening_quality)			\
			CLI_AMI_TABLE_FIELD_NAMED(meanQual, "Mean Quality",		"1.6",		f,	8,	stats->mean_opinion_score_listening_quality)			\
			CLI_AMI_TABLE_FIELD_NAMED(maxQual, "Max Quality",		"1.6",		f,	8,	stats->max_opinion_score_listening_quality)			\
			CLI_AMI_TABLE_FIELD_NAMED(rConceal, "Concealment Ratio",		"1.6",		f,	8,	stats->cumulative_concealement_ratio)				\
			CLI_AMI_TABLE_FIELD_NAMED(sConceal, "Concealed Seconds",		"-8",		d,	8,	stats->concealed_seconds)
#include "sccp_cli_table.h"
			local_table_total++;

	if (s) {
		totals->lines = local_line_total;
		totals->tables = local_table_total;
	}
	return RESULT_SUCCESS;
}

static char cli_device_usage[] = "Usage: sccp show device <device> [calls]\n"
				 "       Show a device's settings and state, or with calls, the quality the phone\n"
				 "       reported for its last calls.\n";

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#define CLI_COMMAND "sccp", "show", "device"
#define CLI_COMPLETE SCCP_CLI_DEVICE_COMPLETER
CLI_AMI_ENTRY(show_device, sccp_show_device, "Lists device settings", cli_device_usage, FALSE, TRUE)
SCCP_AMI_ACTION(show_device, sccp_show_device, "SCCPShowDevice", SCCP_AMI_LIST_BY_HANDLER, "sccp", "show", "device", "$Device")
SCCP_AMI_ACTION(show_device_calls, sccp_show_device, "SCCPShowDeviceCalls", SCCP_AMI_LIST_BY_HANDLER, "sccp", "show", "device", "$Device", "calls")
#undef CLI_COMPLETE
#undef CLI_COMMAND
#endif														/* DOXYGEN_SHOULD_SKIP_THIS */
    /* ---------------------------------------------------------------------------------------------------------SHOW LINES- */
    /*!
     * \brief Show Lines
     * \param fd Fd as int
     * \param totals Total number of lines as int
     * \param s AMI Session
     * \param m Message
     * \param argc Argc as int
     * \param argv[] Argv[] as char
     * \return Result as int
     * 
     * \called_from_asterisk
     * 
     */
static int sccp_show_lines(int fd, sccp_cli_totals_t *totals, struct mansession *s, const struct message *m, int argc, char *argv[])
{
	sccp_line_t *l = NULL;
	boolean_t found_linedevice = 0;
	sccp_linedevice_t * ld = NULL;
	sccp_channel_t *channel = NULL;
	char cap_buf[512] = {0};
	PBX_VARIABLE_TYPE * v = NULL;
	int local_line_total = 0;
	const char *actionid = "";

	const char *const headers[] = { "Line", "Subscription", "Label", "Description", "Device", "MWI", "Channels", "State", "Direction", "Party", "Codecs" };
	sccp_cli_table_data_t table = { .headers = headers, .columns = ARRAY_LEN(headers) };
	const char *const detail_headers[] = { "Line", "Setting", "Value" };
	sccp_cli_table_data_t details = { .headers = detail_headers, .columns = ARRAY_LEN(detail_headers) };
	if (s) {
		astman_append(s, "Event: TableStart\r\n");
		local_line_total++;
		astman_append(s, "TableName: Lines\r\n");
		local_line_total++;
		actionid = astman_get_header(m, "ActionID");
		if (!pbx_strlen_zero(actionid)) {
			astman_append(s, "ActionID: %s\r\n", actionid);
		}
		astman_append(s, "\r\n");
		local_line_total++;
	}
	SCCP_RWLIST_RDLOCK(&GLOB(lines));
	SCCP_RWLIST_TRAVERSE(&GLOB(lines), l, list) {
		found_linedevice = 0;
		channel = NULL;
		SCCP_LIST_LOCK(&l->devices);
		SCCP_LIST_TRAVERSE(&l->devices, ld, list) {
			AUTO_RELEASE(sccp_device_t, d, sccp_device_retain(ld->device));
			if (d) {
				memset(&cap_buf, 0, sizeof(cap_buf));
				char cid_name[StationMaxNameSize] = { 0 };
				skinny_calltype_t calltype = SKINNY_CALLTYPE_SENTINEL;
				sccp_channelstate_t state = SCCP_CHANNELSTATE_SENTINEL;
				
				SCCP_LIST_LOCK(&l->channels);
				SCCP_LIST_TRAVERSE(&l->channels, channel, list) {
					//if (channel && (channel->state != SCCP_CHANNELSTATE_CONNECTED || sccp_strequals(channel->currentDeviceId, d->id))) {
					if (channel && (channel->state == SCCP_CHANNELSTATE_HOLD || sccp_strequals(channel->currentDeviceId, d->id))) {
						if (channel->owner) {
							pbx_getformatname_multiple(cap_buf, sizeof(cap_buf), pbx_channel_nativeformats(channel->owner));
						}
						if (channel->calltype == SKINNY_CALLTYPE_OUTBOUND) {
							iCallInfo.Getter(sccp_channel_getCallInfo(channel), 
								SCCP_CALLINFO_CALLEDPARTY_NAME, &cid_name,
								SCCP_CALLINFO_KEY_SENTINEL);
						} else {
							iCallInfo.Getter(sccp_channel_getCallInfo(channel), 
								SCCP_CALLINFO_CALLINGPARTY_NAME, &cid_name,
								SCCP_CALLINFO_KEY_SENTINEL);
						}
						calltype = channel->calltype;
						state = channel->state;
						break;
					}
				}
				SCCP_LIST_UNLOCK(&l->channels);
				if (!s) {
					sccp_cli_table_add(&table, "%s", l->name);
					if (sccp_strlen_zero(ld->subscriptionId.number)) {
						sccp_cli_table_add(&table, "%s", "(none)");
					} else {
						sccp_cli_table_add(&table, "%s%s", ld->subscriptionId.replaceCid ? "(=)" : "(+)", ld->subscriptionId.number);
					}
					sccp_cli_table_add(&table, "%s", sccp_strlen_zero(ld->subscriptionId.label) ? (l->label ? l->label : "") : ld->subscriptionId.label);
					sccp_cli_table_add(&table, "%s", CLI_NOT_SET(l->description));
					sccp_cli_table_add(&table, "%s", d->id);
					sccp_cli_table_add(&table, "%s", l->voicemailStatistic.newmsgs ? "on" : "off");
					sccp_cli_table_add(&table, "%d", SCCP_LIST_GETSIZE(&l->channels));
					sccp_cli_table_add(&table, "%s", state != SCCP_CHANNELSTATE_SENTINEL ? sccp_channelstate2str(state) : "(none)");
					sccp_cli_table_add(&table, "%s", calltype != SKINNY_CALLTYPE_SENTINEL ? skinny_calltype2str(calltype) : "(none)");
					sccp_cli_table_add(&table, "%s", cid_name[0] ? cid_name : "(none)");
					sccp_cli_table_add(&table, "%s", CLI_NONE(cap_buf));
				} else {
					astman_append(s, "Event: SCCPLineEntry\r\n");
					astman_append(s, "ChannelType: SCCP\r\n");
					astman_append(s, "ChannelObjectType: Line\r\n");
					astman_append(s, "ActionId: %s\r\n", actionid);
					astman_append(s, "Exten: %s\r\n", l->name);
					astman_append(s, "SubscriptionNumber: %s\r\n", ld->subscriptionId.number);
					astman_append(s, "Label: %s\r\n", sccp_strlen_zero(ld->subscriptionId.label) ? l->label : ld->subscriptionId.label);
					astman_append(s, "Description: %s\r\n", l->description ? l->description : "");
					astman_append(s, "Device: %s\r\n", d->id);
					astman_append(s, "MWI: %s\r\n", (l->voicemailStatistic.newmsgs) ? "ON" : "OFF");
					astman_append(s, "ActiveChannels: %d\r\n", SCCP_LIST_GETSIZE(&l->channels));
					astman_append(s, "ChannelState: %s\r\n", (state != SCCP_CHANNELSTATE_SENTINEL) ? sccp_channelstate2str(state) : "");
					astman_append(s, "CallType: %s\r\n", (calltype != SKINNY_CALLTYPE_SENTINEL) ? skinny_calltype2str(calltype) : "");
					astman_append(s, "PartyName: %s\r\n", cid_name);
					astman_append(s, "Capabilities: %s\r\n", cap_buf);
					astman_append(s, "\r\n");
				}
				found_linedevice = 1;
			}
		}
		SCCP_LIST_UNLOCK(&l->devices);

		if (found_linedevice == 0) {
			if (!s) {
				sccp_cli_table_add(&table, "%s", l->name);
				sccp_cli_table_add(&table, "%s", "(none)");
				sccp_cli_table_add(&table, "%s", CLI_NOT_SET(l->label));
				sccp_cli_table_add(&table, "%s", CLI_NOT_SET(l->description));
				sccp_cli_table_add(&table, "%s", "(none)");
				sccp_cli_table_add(&table, "%s", l->voicemailStatistic.newmsgs ? "on" : "off");
				sccp_cli_table_add(&table, "%d", SCCP_LIST_GETSIZE(&l->channels));
				for (unsigned int column = 0; column < 4; column++) {
					sccp_cli_table_add(&table, "%s", "(none)");
				}
			} else {
				astman_append(s, "Event: SCCPLineEntry\r\n");
				astman_append(s, "ChannelType: SCCP\r\n");
				astman_append(s, "ChannelObjectType: Line\r\n");
				astman_append(s, "ActionId: %s\r\n", actionid);
				astman_append(s, "Exten: %s\r\n", l->name);
				astman_append(s, "Label: %s\r\n", l->label ? l->label : "");
				astman_append(s, "Description: %s\r\n", l->description ? l->description : "");
				astman_append(s, "Device: \r\n");
				astman_append(s, "MWI: %s\r\n", (l->voicemailStatistic.newmsgs) ? "ON" : "OFF");
				astman_append(s, "\r\n");
			}
		}
		if (!s) {
			for (v = l->variables; v; v = v->next) {
				sccp_cli_table_add(&details, "%s", l->name);
				sccp_cli_table_add(&details, "%s", v->name);
				sccp_cli_table_add(&details, "%s", v->value);
			}
			if (!sccp_strlen_zero(l->defaultSubscriptionId.number) || !sccp_strlen_zero(l->defaultSubscriptionId.name)) {
				sccp_cli_table_add(&details, "%s", l->name);
				sccp_cli_table_add(&details, "%s", "Default Subscription");
				sccp_cli_table_add(&details, "%s %s", l->defaultSubscriptionId.number, l->defaultSubscriptionId.name);
			}
		}
	}
	SCCP_RWLIST_UNLOCK(&GLOB(lines));
	if (!s) {
		sccp_cli_table_print(&table, fd, "Lines");
		if (details.cells || details.failed) {
			sccp_cli_table_print(&details, fd, "Line settings");
		}
	} else {
		astman_append(s, "Event: TableEnd\r\n");
		local_line_total++;
		astman_append(s, "TableName: Lines\r\n");
		local_line_total++;
		if (!pbx_strlen_zero(actionid)) {
			astman_append(s, "ActionID: %s\r\n", actionid);
		} else {
			astman_append(s, "\r\n");
		}
		local_line_total++;
	}
	if (s) {
		totals->lines = local_line_total;
		totals->tables = 1;
		astman_append(s, "\r\n");
	}

	return RESULT_SUCCESS;
}

static char cli_lines_usage[] =  "Usage: sccp show lines\n       List all SCCP lines with their devices and calls.\n";

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#define CLI_COMMAND "sccp", "show", "lines"
#define CLI_COMPLETE SCCP_CLI_NULL_COMPLETER
CLI_AMI_ENTRY(show_lines, sccp_show_lines, "List defined SCCP Lines", cli_lines_usage, FALSE, TRUE)
SCCP_AMI_ACTION(show_lines, sccp_show_lines, "SCCPShowLines", TRUE, "sccp", "show", "lines")
#undef CLI_COMPLETE
#undef CLI_COMMAND
#endif														/* DOXYGEN_SHOULD_SKIP_THIS */
    /* -----------------------------------------------------------------------------------------------------------SHOW LINE- */
    /*!
     * \brief Show Line
     * \param fd Fd as int
     * \param totals Total number of lines as int
     * \param s AMI Session
     * \param m Message
     * \param argc Argc as int
     * \param argv[] Argv[] as char
     * \return Result as int
     * 
     * \called_from_asterisk
     * 
     */
    //static int sccp_show_line(int fd, int argc, char *argv[])
static int sccp_show_line(int fd, sccp_cli_totals_t *totals, struct mansession *s, const struct message *m, int argc, char *argv[])
{
	sccp_linedevice_t * ld = NULL;
	sccp_mailbox_t * mailbox = NULL;
	PBX_VARIABLE_TYPE * v = NULL;
	pbx_str_t * callgroup_buf = pbx_str_alloca(DEFAULT_PBX_STR_BUFFERSIZE);
	const char * actionid = "";

#ifdef CS_SCCP_PICKUP
	pbx_str_t *pickupgroup_buf = pbx_str_alloca(DEFAULT_PBX_STR_BUFFERSIZE);
#endif
	int local_line_total = 0;
	int local_table_total = 0;

	const char * line = NULL;

	if (argc < 4) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "A line name is required%s\n", "");		/* explicit return */
	}
	line = pbx_strdupa(argv[3]);
	AUTO_RELEASE(sccp_line_t, l , sccp_line_find_byname(line, FALSE));

	if (!l) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "Line %s does not exist\n", line);		/* explicit return */
	}
	CLI_AMI_LIST_START(s, m, "SCCPShowLine");

	char apref_buf[256];
	char acap_buf[512];
	sccp_cli_codec_list(apref_buf, sizeof(apref_buf), l->preferences.audio, ARRAY_LEN(l->preferences.audio));
	sccp_cli_codec_list(acap_buf, sizeof(acap_buf), l->capabilities.audio, ARRAY_LEN(l->capabilities.audio));
#if CS_SCCP_VIDEO
	char vpref_buf[256];
	char vcap_buf[512];
	sccp_cli_codec_list(vpref_buf, sizeof(vpref_buf), l->preferences.video, ARRAY_LEN(l->preferences.video));
	sccp_cli_codec_list(vcap_buf, sizeof(vcap_buf), l->capabilities.video, ARRAY_LEN(l->capabilities.video));
#endif

	if (s) {
		astman_append(s, "Event: SCCPShowLine\r\n");
		actionid = astman_get_header(m, "ActionID");
		if (!pbx_strlen_zero(actionid)) {
			astman_append(s, "ActionID: %s\r\n", actionid);
		}
		local_line_total++;
	}
	const char * context_state = sccp_strlen_zero(l->context) || pbx_context_find(l->context) ? "" : " (context does not exist)";

	/* clang-format off */
#undef CLI_AMI_LIST_WIDTH
#define CLI_AMI_LIST_WIDTH 31
	CLI_TITLE("Line");
	CLI_AMI_OUTPUT_PARAM("Name",				CLI_AMI_LIST_WIDTH, "%s", l->name);
	CLI_AMI_OUTPUT_PARAM("Description",			CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(l->description));
	CLI_AMI_OUTPUT_PARAM("Label",				CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(l->label));
	CLI_AMI_OUTPUT_PARAM("ID",				CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(l->id));
	CLI_AMI_OUTPUT_PARAM("PIN",				CLI_AMI_LIST_WIDTH, "%s", sccp_strlen_zero(l->pin) ? CLI_NOT_SET("") : "set");
	CLI_AMI_OUTPUT_PARAM("Caller ID name",			CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(l->cid_name));
	CLI_AMI_OUTPUT_PARAM("Caller ID number",		CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(l->cid_num));
	CLI_AMI_OUTPUT_PARAM("Context",				CLI_AMI_LIST_WIDTH, "%s%s", CLI_NOT_SET(l->context), context_state);
	CLI_AMI_OUTPUT_PARAM("Registration extension",		CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(l->regexten));
	CLI_AMI_OUTPUT_PARAM("Registration context",		CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(l->regcontext));
	CLI_AMI_OUTPUT_PARAM("Language",			CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(l->language));
	CLI_AMI_OUTPUT_PARAM("Account code",			CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(l->accountcode));
	CLI_AMI_OUTPUT_PARAM("Music on hold class",		CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(l->musicclass));
	CLI_AMI_OUTPUT_PARAM("AMA flags",			CLI_AMI_LIST_WIDTH, "%s", pbx_channel_amaflags2string(l->amaflags));
	CLI_AMI_OUTPUT_PARAM("Parking lot",			CLI_AMI_LIST_WIDTH, "%s", !sccp_strlen_zero(l->parkinglot) ? l->parkinglot : "default");
	CLI_AMI_OUTPUT_PARAM("Hotline number",			CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(l->adhocNumber));
#ifdef CS_SCCP_REALTIME
	CLI_AMI_OUTPUT_YES_NO("Realtime",			CLI_AMI_LIST_WIDTH, l->realtime);
#endif
	CLI_AMI_OUTPUT_YES_NO("Update pending",			CLI_AMI_LIST_WIDTH, l->pendingUpdate);
	CLI_AMI_OUTPUT_YES_NO("Removal pending",		CLI_AMI_LIST_WIDTH, l->pendingDelete);

	CLI_SECTION("Calls");
	CLI_AMI_OUTPUT_PARAM("Incoming call limit",		CLI_AMI_LIST_WIDTH, "%d", l->incominglimit);
	CLI_AMI_OUTPUT_PARAM("Calls",				CLI_AMI_LIST_WIDTH, "%d", SCCP_RWLIST_GETSIZE(&l->channels));
	CLI_AMI_OUTPUT_PARAM("Active calls",			CLI_AMI_LIST_WIDTH, "%i", l->statistic.numberOfActiveChannels);
	CLI_AMI_OUTPUT_PARAM("Held calls",			CLI_AMI_LIST_WIDTH, "%i", l->statistic.numberOfHeldChannels);
	CLI_AMI_OUTPUT_PARAM("Devices in use",			CLI_AMI_LIST_WIDTH, "%i", l->statistic.numberOfActiveDevices);
	CLI_AMI_OUTPUT_BOOL("Transfer",				CLI_AMI_LIST_WIDTH, l->transfer);
	CLI_AMI_OUTPUT_PARAM("Transfer to voicemail extension",	CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(l->trnsfvm));
	CLI_AMI_OUTPUT_PARAM("Do not disturb softkey",		CLI_AMI_LIST_WIDTH, "%s", l->dndmode == SCCP_DNDMODE_REJECT ? "toggles reject" : l->dndmode == SCCP_DNDMODE_SILENT ? "toggles silent" : "cycles reject, silent, off");
	CLI_AMI_OUTPUT_PARAM("Voicemail number",		CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(l->vmnum));
	CLI_AMI_OUTPUT_PARAM("Voicemail messages",		CLI_AMI_LIST_WIDTH, "%i new, %i old", l->voicemailStatistic.newmsgs, l->voicemailStatistic.oldmsgs);
	CLI_AMI_OUTPUT_BOOL("MeetMe",				CLI_AMI_LIST_WIDTH, l->meetme);
	CLI_AMI_OUTPUT_PARAM("MeetMe number",			CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(l->meetmenum));
	CLI_AMI_OUTPUT_PARAM("MeetMe options",			CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(l->meetmeopts));
	CLI_AMI_OUTPUT_PARAM("Second dial tone digits",		CLI_AMI_LIST_WIDTH, "%s", CLI_NOT_SET(l->secondary_dialtone_digits));
	CLI_AMI_OUTPUT_PARAM("Second dial tone",		CLI_AMI_LIST_WIDTH, "%s", skinny_tone2str(l->secondary_dialtone_tone));

	CLI_SECTION("Media");
	CLI_AMI_OUTPUT_LIST("Audio codecs (devices)",		CLI_AMI_LIST_WIDTH, acap_buf);
	CLI_AMI_OUTPUT_LIST("Audio codecs (preference)",	CLI_AMI_LIST_WIDTH, apref_buf);
#if CS_SCCP_VIDEO
	CLI_AMI_OUTPUT_LIST("Video codecs (devices)",		CLI_AMI_LIST_WIDTH, vcap_buf);
	CLI_AMI_OUTPUT_LIST("Video codecs (preference)",	CLI_AMI_LIST_WIDTH, vpref_buf);
	CLI_AMI_OUTPUT_PARAM("Video mode",			CLI_AMI_LIST_WIDTH, "%s", sccp_video_mode2str(l->videomode));
#endif
	CLI_AMI_OUTPUT_YES_NO("Codecs set on the line",		CLI_AMI_LIST_WIDTH, l->preferences_set_on_line_level);
	CLI_AMI_OUTPUT_BOOL("Echo cancellation",		CLI_AMI_LIST_WIDTH, l->echocancel);
	CLI_AMI_OUTPUT_BOOL("Silence suppression",		CLI_AMI_LIST_WIDTH, l->silencesuppression);

	CLI_SECTION("Pickup");
	sccp_print_group(callgroup_buf, DEFAULT_PBX_STR_BUFFERSIZE, l->callgroup);
	CLI_AMI_OUTPUT_PARAM("Call group",			CLI_AMI_LIST_WIDTH, "%s", CLI_NONE(pbx_str_buffer(callgroup_buf)));
#ifdef CS_SCCP_PICKUP
	sccp_print_group(pickupgroup_buf, DEFAULT_PBX_STR_BUFFERSIZE, l->pickupgroup);
	CLI_AMI_OUTPUT_PARAM("Pickup group",			CLI_AMI_LIST_WIDTH, "%s", CLI_NONE(pbx_str_buffer(pickupgroup_buf)));
#ifdef CS_AST_HAS_NAMEDGROUP
	CLI_AMI_OUTPUT_PARAM("Named call group",		CLI_AMI_LIST_WIDTH, "%s", CLI_NONE(l->namedcallgroup));
	CLI_AMI_OUTPUT_PARAM("Named pickup group",		CLI_AMI_LIST_WIDTH, "%s", CLI_NONE(l->namedpickupgroup));
#endif
	CLI_AMI_OUTPUT_BOOL("Directed pickup",			CLI_AMI_LIST_WIDTH, l->directed_pickup);
	CLI_AMI_OUTPUT_PARAM("Directed pickup context",		CLI_AMI_LIST_WIDTH, "%s%s", CLI_NOT_SET(l->directed_pickup_context),
			     sccp_strlen_zero(l->directed_pickup_context) || pbx_context_find(l->directed_pickup_context) ? "" : " (context does not exist)");
	CLI_AMI_OUTPUT_BOOL("Pickup answers the call",		CLI_AMI_LIST_WIDTH, l->pickup_modeanswer);
#endif
#undef CLI_AMI_LIST_WIDTH
#define CLI_AMI_LIST_WIDTH 46
	/* clang-format on */
	if (s) {
		astman_append(s, "\r\n");
	} else {
		pbx_cli(fd, "\n");
	}
	// Line attached to these devices
#define CLI_AMI_TABLE_NAME AttachedDevices
#define CLI_AMI_TABLE_TITLE "Attached devices"
#define CLI_AMI_TABLE_PER_ENTRY_NAME AttachedDevice
#define CLI_AMI_TABLE_LIST_ITER_HEAD &l->devices
#define CLI_AMI_TABLE_LIST_ITER_VAR  ld
#define CLI_AMI_TABLE_LIST_LOCK SCCP_LIST_LOCK
#define CLI_AMI_TABLE_LIST_ITERATOR SCCP_LIST_TRAVERSE
#define CLI_AMI_TABLE_LIST_UNLOCK SCCP_LIST_UNLOCK

#define CLI_AMI_TABLE_BEFORE_ITERATION \
	char cfwd_str_buf[256] = "";   \
	sccp_linedevice_get_cfwd_string(ld, cfwd_str_buf, sizeof(cfwd_str_buf));
#define CLI_AMI_TABLE_AFTER_ITERATION

#define CLI_AMI_TABLE_FIELDS                                             \
	CLI_AMI_TABLE_FIELD_NAMED(DeviceName, "Device", "-15.15", s, 15, ld->device->id) \
	CLI_AMI_TABLE_FIELD_NAMED(CallForward, "Call Forward", "55.55", s, 55, cfwd_str_buf)
#include "sccp_cli_table.h"
		local_table_total++;
	// Mailboxes connected to this line
#define CLI_AMI_TABLE_NAME Mailboxes
#define CLI_AMI_TABLE_PER_ENTRY_NAME Mailbox
#define CLI_AMI_TABLE_LIST_ITER_HEAD &l->mailboxes
#define CLI_AMI_TABLE_LIST_ITER_VAR mailbox
#define CLI_AMI_TABLE_LIST_LOCK SCCP_LIST_LOCK
#define CLI_AMI_TABLE_LIST_ITERATOR SCCP_LIST_TRAVERSE
#define CLI_AMI_TABLE_LIST_UNLOCK SCCP_LIST_UNLOCK

#define CLI_AMI_TABLE_FIELDS 												\
		CLI_AMI_TABLE_FIELD(Mailbox,		"30.30",	s,	30,	mailbox->uniqueid)
#include "sccp_cli_table.h"
		local_table_total++;

	if (l->variables) {
		// LINE VARIABLES
#define CLI_AMI_TABLE_NAME Variables
#define CLI_AMI_TABLE_PER_ENTRY_NAME Variable
#define CLI_AMI_TABLE_ITERATOR for(v = l->variables;v;v = v->next)
#define CLI_AMI_TABLE_FIELDS 											\
		CLI_AMI_TABLE_FIELD(Name,		"15.15",	s,	15,	v->name)		\
		CLI_AMI_TABLE_FIELD(Value,		"-29.29",	s,	29,	v->value)
#include "sccp_cli_table.h"
		local_table_total++;
	}
	if (s) {
		totals->lines = local_line_total;
		totals->tables = local_table_total;
	}
	return RESULT_SUCCESS;
}

static char cli_line_usage[] =  "Usage: sccp show line <line>\n       Show a line's settings and state.\n";

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#define CLI_COMMAND "sccp", "show", "line"
#define CLI_COMPLETE SCCP_CLI_LINE_COMPLETER
CLI_AMI_ENTRY(show_line, sccp_show_line, "List defined SCCP line settings", cli_line_usage, FALSE, TRUE)
SCCP_AMI_ACTION(show_line, sccp_show_line, "SCCPShowLine", SCCP_AMI_LIST_BY_HANDLER, "sccp", "show", "line", "$Line")
#undef CLI_COMPLETE
#undef CLI_COMMAND
#endif														/* DOXYGEN_SHOULD_SKIP_THIS */
    /* --------------------------------------------------------------------------------------------------------SHOW CHANNELS- */
    /*!
     * \brief Show Channels
     * \param fd Fd as int
     * \param totals Total number of lines as int
     * \param s AMI Session
     * \param m Message
     * \param argc Argc as int
     * \param argv[] Argv[] as char
     * \return Result as int
     * 
     * \called_from_asterisk
     * 
     */
    //static int sccp_show_channels(int fd, int argc, char *argv[])
static int sccp_show_channels(int fd, sccp_cli_totals_t *totals, struct mansession *s, const struct message *m, int argc, char *argv[])
{
	sccp_channel_t * channel = NULL;
	sccp_line_t * line = NULL;
	int local_line_total = 0;
	char tmpname[25];
	char addrStr[INET6_ADDRSTRLEN] = "";
	pbx_str_t * buf = pbx_str_alloca(DEFAULT_PBX_STR_BUFFERSIZE);

#define CLI_AMI_TABLE_NAME Channels
#define CLI_AMI_TABLE_PER_ENTRY_NAME Channel
#define CLI_AMI_TABLE_LIST_ITER_HEAD &GLOB(lines)
#define CLI_AMI_TABLE_LIST_ITER_VAR line
#define CLI_AMI_TABLE_LIST_LOCK SCCP_RWLIST_RDLOCK
#define CLI_AMI_TABLE_LIST_ITERATOR SCCP_RWLIST_TRAVERSE
#define CLI_AMI_TABLE_LIST_UNLOCK SCCP_RWLIST_UNLOCK
#define CLI_AMI_TABLE_BEFORE_ITERATION                                                                                                        \
	AUTO_RELEASE(sccp_line_t, l, sccp_line_retain(line));                                                                                 \
	SCCP_LIST_LOCK(&l->channels);                                                                                                         \
	SCCP_LIST_TRAVERSE(&l->channels, channel, list) {                                                                                     \
		if(channel->conference_id) {                                                                                                  \
			snprintf(tmpname, sizeof(tmpname), "SCCPCONF/%03d/%03d", channel->conference_id, channel->conference_participant_id); \
		} else {                                                                                                                      \
			snprintf(tmpname, sizeof(tmpname), "%s", channel->designator);                                                        \
		}                                                                                                                             \
		if(&channel->rtp) {                                                                                                           \
			sccp_copy_string(addrStr, sccp_netsock_stringify(&channel->rtp.audio.phone), sizeof(addrStr));                        \
		}                                                                                                                             \
		sccp_rtp_print(channel, SCCP_RTP_AUDIO, buf, DEFAULT_PBX_STR_BUFFERSIZE);                                                     \
		sccp_log(DEBUGCAT_RTP)("%s: %s\n", channel->designator, pbx_str_buffer(buf));                                                 \
		sccp_rtp_print(channel, SCCP_RTP_VIDEO, buf, DEFAULT_PBX_STR_BUFFERSIZE);                                                     \
		sccp_log(DEBUGCAT_RTP)("%s: %s\n", channel->designator, pbx_str_buffer(buf));

#define CLI_AMI_TABLE_AFTER_ITERATION 												\
		}														\
		SCCP_LIST_UNLOCK(&l->channels);											\

#if !CS_SCCP_VIDEO
#	define CLI_AMI_TABLE_FIELDS                                                                                                           \
		CLI_AMI_TABLE_FIELD (ID, "-5", d, 5, channel->callid)                                                                          \
		CLI_AMI_TABLE_FIELD (Name, "-25.25", s, 25, tmpname)                                                                           \
		CLI_AMI_TABLE_UTF8_FIELD_NAMED(LineName, "Line", "-10.10", s, 10, channel->line->name)                                                      \
		CLI_AMI_TABLE_UTF8_FIELD_NAMED(DeviceName, "Device", "-16", s, 16, channel->currentDeviceId)                                                  \
		CLI_AMI_TABLE_FIELD_NAMED(NumCalled, "Dialed Number", "-10.10", s, 10, channel->dialedNumber)                                                        \
		CLI_AMI_TABLE_FIELD_NAMED (PBXState, "PBX State", "-10.10", s, 10, (channel->owner) ? pbx_state2str (iPbx.getChannelState (channel)) : "(none)") \
		CLI_AMI_TABLE_FIELD_NAMED (SCCPState, "SCCP State", "-10.10", s, 10, sccp_channelstate2str (channel->state))                                      \
		CLI_AMI_TABLE_FIELD_NAMED(AudioR, "Audio Read", "-6.6", s, 6, codec2name (channel->rtp.audio.transmission.format))                                \
		CLI_AMI_TABLE_FIELD_NAMED(AudioW, "Audio Write", "-6.6", s, 6, codec2name (channel->rtp.audio.reception.format))                                   \
		CLI_AMI_TABLE_FIELD_NAMED(RTPPeer, "RTP Peer", "22.22", s, 22, addrStr)                                                                         \
		CLI_AMI_TABLE_FIELD (Direct, "-6.6", s, 6, channel->rtp.audio.directMedia ? "yes" : "no")                                      \
		CLI_AMI_TABLE_FIELD_NAMED(DTMFmode, "DTMF Mode", "-8.8", s, 8, sccp_dtmfmode2str (channel->dtmfmode))
#else
#	define CLI_AMI_TABLE_FIELDS                                                                                                           \
		CLI_AMI_TABLE_FIELD (ID, "-5", d, 5, channel->callid)                                                                          \
		CLI_AMI_TABLE_FIELD (Name, "-25.25", s, 25, tmpname)                                                                           \
		CLI_AMI_TABLE_UTF8_FIELD_NAMED(LineName, "Line", "-10.10", s, 10, channel->line->name)                                                      \
		CLI_AMI_TABLE_UTF8_FIELD_NAMED(DeviceName, "Device", "-16", s, 16, channel->currentDeviceId)                                                  \
		CLI_AMI_TABLE_FIELD_NAMED(NumCalled, "Dialed Number", "-10.10", s, 10, channel->dialedNumber)                                                        \
		CLI_AMI_TABLE_FIELD_NAMED (PBXState, "PBX State", "-10.10", s, 10, (channel->owner) ? pbx_state2str (iPbx.getChannelState (channel)) : "(none)") \
		CLI_AMI_TABLE_FIELD_NAMED (SCCPState, "SCCP State", "-10.10", s, 10, sccp_channelstate2str (channel->state))                                      \
		CLI_AMI_TABLE_FIELD_NAMED(AudioR, "Audio Read", "-6.6", s, 6, codec2name (channel->rtp.audio.transmission.format))                                \
		CLI_AMI_TABLE_FIELD_NAMED(AudioW, "Audio Write", "-6.6", s, 6, codec2name (channel->rtp.audio.reception.format))                                   \
		CLI_AMI_TABLE_FIELD_NAMED(VideoR, "Video Read", "-6.6", s, 6, codec2name (channel->rtp.video.transmission.format))                                \
		CLI_AMI_TABLE_FIELD_NAMED(VideoW, "Video Write", "-6.6", s, 6, codec2name (channel->rtp.video.reception.format))                                   \
		CLI_AMI_TABLE_FIELD_NAMED(RTPPeer, "RTP Peer", "22.22", s, 22, addrStr)                                                                         \
		CLI_AMI_TABLE_FIELD (Direct, "-6.6", s, 6, channel->rtp.audio.directMedia ? "yes" : "no")                                      \
		CLI_AMI_TABLE_FIELD_NAMED(DTMFmode, "DTMF Mode", "-8.8", s, 8, sccp_dtmfmode2str (channel->dtmfmode))                                            \
		CLI_AMI_TABLE_FIELD (Video, "-5.5", s, 5, sccp_video_mode2str (l->videomode))
#endif
#include "sccp_cli_table.h"
	if (s) {
		totals->lines = local_line_total;
		totals->tables = 1;
	}
	return RESULT_SUCCESS;
}

static char cli_channels_usage[] =  "Usage: sccp show channels\n       List SCCP calls.\n";

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#define CLI_COMMAND "sccp", "show", "channels"
#define CLI_COMPLETE SCCP_CLI_NULL_COMPLETER
CLI_AMI_ENTRY(show_channels, sccp_show_channels, "Lists active SCCP channels", cli_channels_usage, FALSE, TRUE)
SCCP_AMI_ACTION(show_channels, sccp_show_channels, "SCCPShowChannels", TRUE, "sccp", "show", "channels")
#undef CLI_COMPLETE
#undef CLI_COMMAND
#endif														/* DOXYGEN_SHOULD_SKIP_THIS */

/* -------------------------------------------------------------------------------------------------------SHOW SESSIONS- */
static char cli_sessions_usage[] =  "Usage: sccp show sessions [all]\n       List phone connections with a registered device; all also lists the others.\n";

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#define CLI_COMMAND "sccp", "show", "sessions"
#define CLI_COMPLETE SCCP_CLI_NULL_COMPLETER
CLI_AMI_ENTRY(show_sessions, sccp_cli_show_sessions, "Show all SCCP sessions", cli_sessions_usage, FALSE, TRUE)
SCCP_AMI_ACTION(show_sessions, sccp_cli_show_sessions, "SCCPShowSessions", TRUE, "sccp", "show", "sessions")
#undef CLI_COMPLETE
#undef CLI_COMMAND
#endif														/* DOXYGEN_SHOULD_SKIP_THIS */
    /* ---------------------------------------------------------------------------------------------SHOW_MWI_SUBSCRIPTIONS- */
    // sccp_show_mwi_subscriptions implementation moved to sccp_mwi.c, because of access to private struct
static char cli_mwi_subscriptions_usage[] =  "Usage: sccp show mwi subscriptions\n       List voicemail (MWI) mailbox subscriptions.\n";

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#define CLI_COMMAND "sccp", "show", "mwi", "subscriptions"
#define CLI_COMPLETE SCCP_CLI_NULL_COMPLETER
CLI_AMI_ENTRY(show_mwi_subscriptions, iVoicemail.showSubscriptions, "Show all SCCP MWI subscriptions", cli_mwi_subscriptions_usage, FALSE, TRUE)
SCCP_AMI_ACTION(show_mwi_subscriptions, iVoicemail.showSubscriptions, "SCCPShowMWISubscriptions", TRUE, "sccp", "show", "mwi", "subscriptions")
#undef CLI_COMPLETE
#undef CLI_COMMAND
#endif														/* DOXYGEN_SHOULD_SKIP_THIS */

    /* ---------------------------------------------------------------------------------------------CONFERENCE FUNCTIONS- */
#ifdef CS_SCCP_CONFERENCE
static char cli_conferences_usage[] =  "Usage: sccp show conferences\n       List running SCCP conferences.\n";

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#define CLI_COMMAND "sccp", "show", "conferences"
#define CLI_COMPLETE SCCP_CLI_NULL_COMPLETER
CLI_AMI_ENTRY(show_conferences, sccp_cli_show_conferences, "List running SCCP Conferences", cli_conferences_usage, FALSE, TRUE)
SCCP_AMI_ACTION(show_conferences, sccp_cli_show_conferences, "SCCPShowConferences", TRUE, "sccp", "show", "conferences")
#undef CLI_COMPLETE
#undef CLI_COMMAND
#endif														/* DOXYGEN_SHOULD_SKIP_THIS */
static char cli_conference_usage[] =  "Usage: sccp show conference <conference>\n       Show a conference and its participants.\n";

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#define CLI_COMMAND "sccp", "show", "conference"
#define CLI_COMPLETE SCCP_CLI_NULL_COMPLETER
CLI_AMI_ENTRY(show_conference, sccp_cli_show_conference, "List running SCCP Conference", cli_conference_usage, FALSE, FALSE)
SCCP_AMI_ACTION(show_conference, sccp_cli_show_conference, "SCCPShowConference", FALSE, "sccp", "show", "conference", "$Conference")
#undef CLI_COMPLETE
#undef CLI_COMMAND
#endif
    /* DOXYGEN_SHOULD_SKIP_THIS */
static char cli_conference_command_usage[] =  "Usage: sccp conference <EndConf|Kick|Mute|Invite|Moderate> <conference> [participant]\n       End a conference, or act on one of its participants (every action but\n       EndConf needs a participant).\n";

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#define CLI_COMMAND "sccp", "conference"
#define CLI_COMPLETE SCCP_CLI_CONFERENCE_COMPLETER
CLI_AMI_ENTRY(conference_command, sccp_cli_conference_command, "Conference Action", cli_conference_command_usage, TRUE, FALSE)
SCCP_AMI_ACTION(conference_command, sccp_cli_conference_command, "SCCPConference", FALSE, "sccp", "conference", "$Action", "$Conference", "$Participant")
#undef CLI_COMPLETE
#undef CLI_COMMAND
#endif														/* DOXYGEN_SHOULD_SKIP_THIS */

#endif														/* CS_SCCP_CONFERENCE */
    /* ---------------------------------------------------------------------------------------------SHOW_HINT LINESTATES - */
static char cli_show_hint_lineStates_usage[] =  "Usage: sccp show hint line states\n       List the line states SCCP reports to Asterisk hints.\n";

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#define CLI_COMMAND "sccp", "show", "hint", "line", "states"
#define CLI_COMPLETE SCCP_CLI_NULL_COMPLETER
CLI_AMI_ENTRY(show_hint_lineStates, sccp_show_hint_lineStates, "Show all SCCP Hint Line States", cli_show_hint_lineStates_usage, FALSE, TRUE)
SCCP_AMI_ACTION(show_hint_lineStates, sccp_show_hint_lineStates, "SCCPShowHintLineStates", TRUE, "sccp", "show", "hint", "line", "states")
#undef CLI_COMPLETE
#undef CLI_COMMAND
#endif														/* DOXYGEN_SHOULD_SKIP_THIS */
    /* ---------------------------------------------------------------------------------------------SHOW_HINT LINESTATES - */
static char cli_show_hint_subscriptions_usage[] =  "Usage: sccp show hint subscriptions\n       List the phone buttons (BLF speeddials) subscribed to hints.\n";

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#define CLI_COMMAND "sccp", "show", "hint", "subscriptions"
#define CLI_COMPLETE SCCP_CLI_NULL_COMPLETER
CLI_AMI_ENTRY(show_hint_subscriptions, sccp_show_hint_subscriptions, "Show all SCCP Hint Subscriptions", cli_show_hint_subscriptions_usage, FALSE, TRUE)
SCCP_AMI_ACTION(show_hint_subscriptions, sccp_show_hint_subscriptions, "SCCPShowHintSubscriptions", TRUE, "sccp", "show", "hint", "subscriptions")
#undef CLI_COMPLETE
#undef CLI_COMMAND
#endif														/* DOXYGEN_SHOULD_SKIP_THIS */
    /* ---------------------------------------------------------------------------------------------SHOW_REFCOUNT - */
static char cli_show_refcount_usage[] =  "Usage: sccp show references [show|suppress]\n       List reference-counted objects. show adds an in-use column; suppress also\n       hides the objects in use.\n";

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#define CLI_COMMAND "sccp", "show", "references"
#define CLI_COMPLETE SCCP_CLI_NULL_COMPLETER
CLI_AMI_ENTRY(show_refcount, sccp_show_refcount, "Show all Refcount Entries", cli_show_refcount_usage, FALSE, TRUE)
SCCP_AMI_ACTION(show_refcount, sccp_show_refcount, "SCCPShowReferences", TRUE, "sccp", "show", "references")
#undef CLI_COMPLETE
#undef CLI_COMMAND
#endif														/* DOXYGEN_SHOULD_SKIP_THIS */

    /* --------------------------------------------------------------------------------------------------SHOW_SOKFTKEYSETS- */
    /*!
     * \brief Show Sessions
     * \param fd Fd as int
     * \param totals Total number of lines as int
     * \param s AMI Session
     * \param m Message
     * \param argc Argc as int
     * \param argv[] Argv[] as char
     * \return Result as int
     * 
     * \called_from_asterisk
     * 
     */
    //static int sccp_show_softkeysets(int fd, int argc, char *argv[])
static int sccp_show_softkeysets(int fd, sccp_cli_totals_t *totals, struct mansession *s, const struct message *m, int argc, char *argv[])
{
	uint8_t i = 0;
	uint8_t c = 0;
	int local_line_total = 0;

#define CLI_AMI_TABLE_NAME SoftKeySets
#define CLI_AMI_TABLE_TITLE "Softkey sets"
#define CLI_AMI_TABLE_PER_ENTRY_NAME SoftKeySet
#define CLI_AMI_TABLE_LIST_ITER_HEAD &softKeySetConfig
#define CLI_AMI_TABLE_LIST_ITER_TYPE sccp_softKeySetConfiguration_t
#define CLI_AMI_TABLE_LIST_ITER_VAR softkeyset
#define CLI_AMI_TABLE_LIST_LOCK SCCP_LIST_LOCK
#define CLI_AMI_TABLE_LIST_ITERATOR SCCP_LIST_TRAVERSE
#define CLI_AMI_TABLE_LIST_UNLOCK SCCP_LIST_UNLOCK
#define CLI_AMI_TABLE_BEFORE_ITERATION                                                                                                                                                                                          \
	for (i = 0; i < sizeof(softkeyset->modes) / sizeof(softkey_modes); i++) {                                                                                                                                               \
		for (c = 0; c < softkeyset->modes[i].count; c++) {                                                                                                                                                              \
			const char * label = label2str(softkeyset->modes[i].ptr[c]);

#define CLI_AMI_TABLE_AFTER_ITERATION                                                                                                                                                                                           \
	}                                                                                                                                                                                                                       \
	}
#define CLI_AMI_TABLE_FIELDS                                                                                                                                                                                                    \
	CLI_AMI_TABLE_FIELD(Set, "-15.15", s, 15, softkeyset->name)                                                                                                                                                             \
	CLI_AMI_TABLE_FIELD(Mode, "-12.12", s, 12, skinny_keymode2str((skinny_keymode_t)i))                                                                                                                                     \
	CLI_AMI_TABLE_FIELD(Description, "-40.40", s, 40, skinny_keymode2longstr((skinny_keymode_t)i))                                                                                                                          \
	CLI_AMI_TABLE_FIELD_NAMED(Lbl_ID, "Position", "-5", d, 5, c)                                                                                                                                                                              \
	CLI_AMI_TABLE_UTF8_FIELD(Label, "-15.15", s, 15, label)
#include "sccp_cli_table.h"

	if (s) {
		totals->lines = local_line_total;
		totals->tables = 1;
	}
	return RESULT_SUCCESS;
}

static char cli_show_softkeysets_usage[] =  "Usage: sccp show softkey sets\n       List the softkey sets and their keys for each call state.\n";

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#define CLI_COMMAND "sccp", "show", "softkey", "sets"
#	define CLI_COMPLETE   SCCP_CLI_NULL_COMPLETER
CLI_AMI_ENTRY(show_softkeysets, sccp_show_softkeysets, "Show configured SoftKeySets", cli_show_softkeysets_usage, FALSE, TRUE)
SCCP_AMI_ACTION(show_softkeysets, sccp_show_softkeysets, "SCCPShowSoftkeySets", TRUE, "sccp", "show", "softkey", "sets")
#undef CLI_COMPLETE
#undef CLI_COMMAND
#endif														/* DOXYGEN_SHOULD_SKIP_THIS */



/* ------------------------------------------------------------------------------------------------------------ HELPERS - */
/*
 * The handlers below are shared by the CLI and AMI. On AMI the arguments come from an argv template
 * (see SCCP_AMI_ACTION), so an absent optional header arrives as an empty string; treat "" as absent.
 */
#define ARG_PRESENT(_n) (argc > (_n) && !sccp_strlen_zero(argv[(_n)]))

/* Find a call by its number ("3") or its channel name ("SCCP/2004-00000003") */
static sccp_channel_t * sccp_cli_find_call(const char * id)
{
	if (sccp_strlen_zero(id)) {
		return NULL;
	}
	if (!strncasecmp("SCCP/", id, 5)) {
		const char * dash = strrchr(id, '-');
		if (!dash || sccp_strlen_zero(dash + 1)) {
			return NULL;
		}
		return sccp_channel_find_byid((uint32_t)strtoul(dash + 1, NULL, 16)); /*ref_replace*/
	}
	return sccp_channel_find_byid(sccp_atoi(id, strlen(id))); /*ref_replace*/
}

/* Parse the optional "[beep] [timeout]" arguments of the message commands, in either order */
static boolean_t sccp_cli_parse_message_options(int argc, char * argv[], int first, boolean_t * beep, int * timeout)
{
	for (int x = first; x < argc; x++) {
		if (sccp_strlen_zero(argv[x])) {
			continue;
		}
		if (sccp_strIsNumeric(argv[x])) {
			*timeout = sccp_atoi(argv[x], strlen(argv[x]));
		} else if (sccp_strcaseequals(argv[x], "beep") || sccp_true(argv[x])) {
			*beep = TRUE;
		} else if (sccp_strcaseequals(argv[x], "nobeep") || sccp_false(argv[x])) {
			*beep = FALSE;
		} else {
			return FALSE;
		}
	}
	return TRUE;
}

/* ------------------------------------------------------------------------------------------------------------ MESSAGES - */
/* sccp message all <text> [beep] [timeout] */
static int sccp_message_all(int fd, sccp_cli_totals_t * totals, struct mansession * s, const struct message * m, int argc, char * argv[])
{
	sccp_device_t * d = NULL;
	int timeout = 10;
	boolean_t beep = FALSE;
	int count = 0;

	if (!ARG_PRESENT(3) || !sccp_cli_parse_message_options(argc, argv, 4, &beep, &timeout)) {
		return RESULT_SHOWUSAGE;
	}
	SCCP_RWLIST_RDLOCK(&GLOB(devices));
	SCCP_RWLIST_TRAVERSE(&GLOB(devices), d, list) {
		if (d->session) {
			sccp_dev_set_message(d, argv[3], timeout, FALSE, beep);
			count++;
		}
	}
	SCCP_RWLIST_UNLOCK(&GLOB(devices));
	CLI_AMI_RETURN_DONE(fd, s, m, "Message sent to %d registered device%s", count, count == 1 ? "" : "s");
}

static char cli_message_all_usage[] =
	"Usage: sccp message all <text> [beep] [timeout]\n"
	"       Show a message on every registered phone for <timeout> seconds (default 10).\n"
	"       'beep' also plays a short tone.\n";

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#define CLI_COMMAND "sccp", "message", "all"
#	define CLI_COMPLETE SCCP_CLI_NULL_COMPLETER
CLI_AMI_ENTRY(message_all, sccp_message_all, "Show a message on all phones", cli_message_all_usage, FALSE, FALSE)
SCCP_AMI_ACTION(message_all, sccp_message_all, "SCCPMessageAll", FALSE, "sccp", "message", "all", "$Text", "$Beep", "$Timeout")
#	undef CLI_COMPLETE
#	undef CLI_COMMAND
#endif

/* sccp message device <device> <text> [beep] [timeout] */
static int sccp_message_device(int fd, sccp_cli_totals_t * totals, struct mansession * s, const struct message * m, int argc, char * argv[])
{
	int timeout = 10;
	boolean_t beep = FALSE;

	if (!ARG_PRESENT(3) || !ARG_PRESENT(4) || !sccp_cli_parse_message_options(argc, argv, 5, &beep, &timeout)) {
		return RESULT_SHOWUSAGE;
	}
	AUTO_RELEASE(sccp_device_t, d, sccp_device_find_byid(argv[3], FALSE));
	if (!d) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "Device %s does not exist", argv[3]);
	}
	if (!d->session) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "Device %s is not registered", d->id);
	}
	sccp_dev_set_message(d, argv[4], timeout, FALSE, beep);
	CLI_AMI_RETURN_DONE(fd, s, m, "Message sent to %s", d->id);
}

static char cli_message_device_usage[] =
	"Usage: sccp message device <device> <text> [beep] [timeout]\n"
	"       Show a message on one phone for <timeout> seconds (default 10).\n"
	"       'beep' also plays a short tone.\n";

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#define CLI_COMMAND "sccp", "message", "device"
#	define CLI_COMPLETE SCCP_CLI_CONNECTED_DEVICE_COMPLETER
CLI_AMI_ENTRY(message_device, sccp_message_device, "Show a message on one phone", cli_message_device_usage, FALSE, FALSE)
SCCP_AMI_ACTION(message_device, sccp_message_device, "SCCPMessageDevice", FALSE, "sccp", "message", "device", "$Device", "$Text", "$Beep", "$Timeout")
#	undef CLI_COMPLETE
#	undef CLI_COMMAND
#endif

/* sccp system message [<text> [beep] [timeout]] */
static int sccp_system_message(int fd, sccp_cli_totals_t * totals, struct mansession * s, const struct message * m, int argc, char * argv[])
{
	sccp_device_t * d = NULL;
	int timeout = 0;
	boolean_t beep = FALSE;
	char timeout_text[12];
	int count = 0;

	if (!ARG_PRESENT(3)) {
		iPbx.feature_removeTreeFromDatabase("SCCP/message", "timeout");
		iPbx.feature_removeTreeFromDatabase("SCCP/message", "text");
		SCCP_RWLIST_RDLOCK(&GLOB(devices));
		SCCP_RWLIST_TRAVERSE(&GLOB(devices), d, list) {
			sccp_dev_clear_message(d, FALSE);
		}
		SCCP_RWLIST_UNLOCK(&GLOB(devices));
		CLI_AMI_RETURN_DONE(fd, s, m, "%s", "System message cleared");
	}
	if (!sccp_cli_parse_message_options(argc, argv, 4, &beep, &timeout) || timeout < 0 || timeout > 255) {
		return RESULT_SHOWUSAGE;
	}
	snprintf(timeout_text, sizeof(timeout_text), "%d", timeout);
	if (!iPbx.feature_addToDatabase("SCCP/message", "timeout", timeout_text) || !iPbx.feature_addToDatabase("SCCP/message", "text", argv[3])) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "%s", "System message not saved: the Asterisk database refused the write");
	}
	SCCP_RWLIST_RDLOCK(&GLOB(devices));
	SCCP_RWLIST_TRAVERSE(&GLOB(devices), d, list) {
		sccp_dev_set_message(d, argv[3], timeout, FALSE, beep);
		if (d->session) {
			count++;
		}
	}
	SCCP_RWLIST_UNLOCK(&GLOB(devices));
	CLI_AMI_RETURN_DONE(fd, s, m, "System message saved and shown on %d registered device%s", count, count == 1 ? "" : "s");
}

static char cli_system_message_usage[] =
	"Usage: sccp system message [<text> [beep] [timeout]]\n"
	"       Set the message every phone shows, including phones that register later.\n"
	"       timeout 0 (default) shows it as the idle message; 1-255 shows it as a\n"
	"       notification for that many seconds. Prompts with a higher priority can\n"
	"       hide the idle message. 'beep' plays a short tone on registered phones.\n"
	"       Without arguments, the system message is cleared.\n"
	"       Example: sccp system message \"Maintenance at 18:00\" 30\n";

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#define CLI_COMMAND "sccp", "system", "message"
#	define CLI_COMPLETE SCCP_CLI_NULL_COMPLETER
CLI_AMI_ENTRY(system_message, sccp_system_message, "Set or clear the system message", cli_system_message_usage, FALSE, FALSE)
SCCP_AMI_ACTION(system_message, sccp_system_message, "SCCPSystemMessage", FALSE, "sccp", "system", "message", "$Text", "$Beep", "$Timeout")
#	undef CLI_COMPLETE
#	undef CLI_COMMAND
#endif

/* ------------------------------------------------------------------------------------------------------ LINE BUTTONS - */
/* sccp add line <device> <line> / sccp remove line <device> <line> */
static int sccp_add_remove_line(int fd, sccp_cli_totals_t * totals, struct mansession * s, const struct message * m, int argc, char * argv[])
{
	boolean_t add = sccp_strcaseequals(argv[1], "add");

	if (argc != 5 || !ARG_PRESENT(3) || !ARG_PRESENT(4)) {
		return RESULT_SHOWUSAGE;
	}
	AUTO_RELEASE(sccp_device_t, d, sccp_device_find_byid(argv[3], FALSE));
	if (!d) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "Device %s does not exist", argv[3]);
	}
	AUTO_RELEASE(sccp_line_t, l, sccp_line_find_byname(argv[4], FALSE));
	if (!l) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "Line %s does not exist", argv[4]);
	}

	int changed = 0;
	sccp_buttonconfig_t * config = NULL;
	SCCP_LIST_LOCK(&d->buttonconfig);
	SCCP_LIST_TRAVERSE(&d->buttonconfig, config, list) {
		if (config->type == LINE && sccp_strequals(config->button.line.name, l->name) && !config->pendingDelete) {
			if (add) {
				changed = -1;
				break;
			}
			config->pendingDelete = 1;
			changed++;
		}
	}
	SCCP_LIST_UNLOCK(&d->buttonconfig);

	if (add) {
		if (changed < 0) {
			CLI_AMI_RETURN_ERROR(fd, s, m, "Line %s is already on device %s", l->name, d->id);
		}
		if (sccp_config_addButton(&d->buttonconfig, -1, LINE, l->name, NULL, NULL) != SCCP_CONFIG_CHANGE_CHANGED) {
			CLI_AMI_RETURN_ERROR(fd, s, m, "Line %s not added to device %s (see the log)", l->name, d->id);
		}
	} else if (!changed) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "Line %s is not on device %s", l->name, d->id);
	}
	d->pendingUpdate = 1;
	sccp_device_check_update(d);
	CLI_AMI_RETURN_DONE(fd, s, m, "Line %s %s device %s%s", l->name, add ? "added to" : "removed from", d->id, d->session ? "; the phone restarts to load its new buttons" : "");
}

static char cli_add_line_usage[] =
	"Usage: sccp add line <device> <line>\n"
	"       Add a line button after the device's last button. A registered phone\n"
	"       restarts to load it. The change is not saved to sccp.conf.\n";
static char cli_remove_line_usage[] =
	"Usage: sccp remove line <device> <line>\n"
	"       Remove the line's buttons from the device. A registered phone restarts\n"
	"       to apply it. The change is not saved to sccp.conf.\n";

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#define CLI_COMMAND "sccp", "add", "line"
#	define CLI_COMPLETE SCCP_CLI_DEVICE_COMPLETER, SCCP_CLI_LINE_COMPLETER
CLI_AMI_ENTRY(add_line, sccp_add_remove_line, "Add a line to a device", cli_add_line_usage, FALSE, FALSE)
SCCP_AMI_ACTION(add_line, sccp_add_remove_line, "SCCPAddLine", FALSE, "sccp", "add", "line", "$Device", "$Line")
#	undef CLI_COMPLETE
#	undef CLI_COMMAND
#define CLI_COMMAND "sccp", "remove", "line"
#	define CLI_COMPLETE SCCP_CLI_DEVICE_COMPLETER, SCCP_CLI_LINE_COMPLETER
CLI_AMI_ENTRY(remove_line, sccp_add_remove_line, "Remove a line from a device", cli_remove_line_usage, FALSE, FALSE)
SCCP_AMI_ACTION(remove_line, sccp_add_remove_line, "SCCPRemoveLine", FALSE, "sccp", "remove", "line", "$Device", "$Line")
#	undef CLI_COMPLETE
#	undef CLI_COMMAND
#endif

    /* ------------------------------------------------------------------------------------------------------------DO DEBUG- */
    /*!
     * \brief Do Debug
     * \param fd Fd as int
     * \param argc Argc as int
     * \param argv[] Argv[] as char
     * \return Result as int
     * 
     * \called_from_asterisk
     */
static int sccp_do_debug(int fd, int argc, char *argv[])
{
	int32_t new_debug = GLOB(debug);
	int     mask      = 0;

	/* check every name first, so a typo changes nothing */
	if (argc > 2 && sscanf(argv[2], "%d", &mask) != 1) {
		for (int argi = 2; argi < argc; argi++) {
			char * copy = pbx_strdupa(argv[argi]);
			char * rest = NULL;
			for (char * token = strtok_r(copy, " ,\t", &rest); token; token = strtok_r(NULL, " ,\t", &rest)) {
				if (!sccp_debug_is_category(token)) {
					pbx_cli(fd, "'%s' is not a debug category; debug not changed\n", token);
					return RESULT_SHOWUSAGE;
				}
			}
		}
	}
	if (argc > 2) {
		new_debug = sccp_parse_debugline(argv, 2, argc, new_debug);
	}

	char * old_categories = sccp_get_debugcategories(GLOB(debug));
	char * new_categories = sccp_get_debugcategories(new_debug);

	if (argc > 2) {
		pbx_cli(fd, "SCCP debug: %s (was %s)\n", new_categories ? new_categories : "none", old_categories ? old_categories : "none");
	} else {
		pbx_cli(fd, "SCCP debug: %s\n", old_categories ? old_categories : "none");
	}
	char * devices = sccp_debug_filter_devices();
	if (devices) {
		pbx_cli(fd, "Limited to devices: %s\n", devices);
		sccp_free(devices);
	}
	sccp_free(old_categories);
	sccp_free(new_categories);

	GLOB(debug) = new_debug;
	return RESULT_SUCCESS;
}

static char do_debug_usage[] = "Usage: sccp debug [0|off|none|all|<mask>|[no] <categories>]\n"
	"       Without arguments, show the current settings. Use 0 to disable debugging.\n"
	"       Add categories by name; use no to remove them. Separate names with spaces or commas.\n"
	"       Categories: core, hint, rtp, device, line, action, channel, config, feature,\n"
	"       feature_button, softkey, indicate, pbx, socket, mwi, event, conference,\n"
	"       buttontemplate, speeddial, codec, realtime, callinfo, refcount, message,\n"
	"       parkinglot, webservice, threadpool, newcode, filelinefunc, high.\n";

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#define CLI_COMMAND "sccp", "debug"
#define CLI_COMPLETE SCCP_CLI_DEBUG_COMPLETER
CLI_ENTRY(cli_do_debug, sccp_do_debug, "Set SCCP Debugging Types", do_debug_usage, TRUE)
#undef CLI_COMPLETE
#undef CLI_COMMAND
#endif														/* DOXYGEN_SHOULD_SKIP_THIS */
/* --------------------------------------------------------------------------------------------------------------RELOAD- */
/*!
 * \brief Do Reload
 * \param fd Fd as int
 * \param argc Argc as int
 * \param argv[] Argv[] as char
 * \return Result as int
 * 
 * \called_from_asterisk
 * 
 * \note To find out more about the reload function see \ref sccp_config_reload
 */
static int sccp_cli_reload(int fd, int argc, char *argv[])
{
	boolean_t force_reload = FALSE;
	int returnval = RESULT_FAILURE;
	//sccp_configurationchange_t change;
	unsigned int change = 0;
	sccp_buttonconfig_t *config = NULL;
	char * filename = GLOB(config_file_name) ? pbx_strdupa(GLOB(config_file_name)) : (char *)"sccp.conf";

	if (argc < 2 || argc > 4) {
		return RESULT_SHOWUSAGE;
	}
	pbx_rwlock_wrlock(&GLOB(lock));
	if (GLOB(reload_in_progress) == TRUE) {
		pbx_cli(fd, "Reload not done: another SCCP reload is in progress\n");
		pbx_rwlock_unlock(&GLOB(lock));
		return RESULT_FAILURE;								/* the running reload owns reload_in_progress */
	}

	if (!GLOB(config_file_name) && !(argc > 2 && sccp_strequals("file", argv[2]))) {
		pbx_cli(fd, "Reload not done: no configuration file name is set\n");
		pbx_rwlock_unlock(&GLOB(lock));
		return RESULT_FAILURE;
	}
	GLOB(reload_in_progress) = TRUE;
	pbx_rwlock_unlock(&GLOB(lock));

	if (argc > 2) {
		if (sccp_strequals("device", argv[2])) {
			if (argc == 4) {
				AUTO_RELEASE(sccp_device_t, device , sccp_device_find_byid(argv[3], FALSE));
				PBX_VARIABLE_TYPE *v = NULL;

				if (CONFIG_STATUS_FILE_OK != sccp_config_getConfig(TRUE, GLOB(config_file_name)) || !GLOB(cfg)) {
					pbx_cli(fd, "Device %s not reloaded: %s could not be loaded (see the log)\n", argv[3], GLOB(config_file_name) ? GLOB(config_file_name) : "sccp.conf");
					goto EXIT;
				}
				if (!device) {
					const char * utype = pbx_variable_retrieve(GLOB(cfg), argv[3], "type");
					if (utype && !strcasecmp(utype, "device")) {
						device = sccp_device_create(argv[3]) /*ref_replace*/;
					}
					if (!device) {
						pbx_cli(fd, "Device %s does not exist and is not defined in %s\n", argv[3], GLOB(config_file_name));
						goto EXIT;
					}
					sccp_device_addToGlobals(device);
					pbx_cli(fd, "Device %s added from %s\n", argv[3], GLOB(config_file_name));
				}
#ifdef CS_SCCP_REALTIME
				if (device->realtime) {
					v = pbx_load_realtime(GLOB(realtimedevicetable), "name", argv[3], NULL);
				} else
#endif
				{
					v = ast_variable_browse(GLOB(cfg), argv[3]);
				}
				if (v) {
					SCCP_LIST_LOCK(&device->buttonconfig);
					SCCP_LIST_TRAVERSE(&device->buttonconfig, config, list) {
						sccp_log((DEBUGCAT_CONFIG + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_4 "%s: button %d marked for removal\n", device->id, config->index);
						config->pendingDelete = 1;
					}
					SCCP_LIST_UNLOCK(&device->buttonconfig);

					change = sccp_config_applyDeviceConfiguration(device, v);
					sccp_log((DEBUGCAT_CORE)) ("%s: reloaded; %s\n", device->id, change ? "restarting it to apply the changes" : "changes applied without a restart");
					pbx_cli(fd, "Device %s reloaded%s\n", device->id, change ? "; restarting it to apply the changes" : "");
					if (change == SCCP_CONFIG_NEEDDEVICERESET) {
						device->pendingUpdate = 1;
						sccp_device_check_update(device);				// Will cleanup after reload and restart the device if necessary
					}
#ifdef CS_SCCP_REALTIME
					if (device->realtime) {
						pbx_variables_destroy(v);
					}
#endif
				} else {
					/* the loaded configuration no longer defines this device */
					device->pendingDelete = 1;
					pbx_cli(fd, "Device %s is no longer defined in %s; it will be removed\n", device->id, GLOB(config_file_name));
				}

				returnval = RESULT_SUCCESS;
			} else {
				returnval = RESULT_SHOWUSAGE;
			}
			goto EXIT;
		} else if (sccp_strequals("line", argv[2])) {
			if (argc == 4) {
				AUTO_RELEASE(sccp_line_t, line , sccp_line_find_byname(argv[3], FALSE));
				PBX_VARIABLE_TYPE *v = NULL;

#ifdef CS_SCCP_REALTIME
				PBX_VARIABLE_TYPE *dv = NULL;
#endif
				if (CONFIG_STATUS_FILE_OK != sccp_config_getConfig(TRUE, GLOB(config_file_name)) || !GLOB(cfg)) {
					pbx_cli(fd, "Line %s not reloaded: %s could not be loaded (see the log)\n", argv[3], GLOB(config_file_name) ? GLOB(config_file_name) : "sccp.conf");
					goto EXIT;
				}
				if (!line) {
					const char * utype = pbx_variable_retrieve(GLOB(cfg), argv[3], "type");
					if (utype && !strcasecmp(utype, "line")) {
						line = sccp_line_create(argv[3]) /*ref_replace*/;
					}
					if (!line) {
						pbx_cli(fd, "Line %s does not exist and is not defined in %s\n", argv[3], GLOB(config_file_name));
						goto EXIT;
					}
					sccp_line_addToGlobals(line);
					pbx_cli(fd, "Line %s added from %s\n", argv[3], GLOB(config_file_name));
				}
#ifdef CS_SCCP_REALTIME
				if (line->realtime) {
					v = pbx_load_realtime(GLOB(realtimelinetable), "name", argv[3], NULL);
				} else
#endif
				{
					v = ast_variable_browse(GLOB(cfg), argv[3]);
				}
				if (v) {
					change = sccp_config_applyLineConfiguration(line, v);
					sccp_log((DEBUGCAT_CORE)) ("%s: reloaded; %s\n", line->name, change ? "restarting its devices to apply the changes" : "changes applied without a restart");
					pbx_cli(fd, "Line %s reloaded%s\n", line->name, change ? "; restarting its devices to apply the changes" : "");
					if (change == SCCP_CONFIG_NEEDDEVICERESET) {
						sccp_linedevice_t * lineDevice = NULL;

						SCCP_LIST_LOCK(&line->devices);
						SCCP_LIST_TRAVERSE(&line->devices, lineDevice, list) {
							AUTO_RELEASE(sccp_device_t, device, sccp_device_retain(lineDevice->device));
							if (device) {
								SCCP_LIST_LOCK(&device->buttonconfig);
								SCCP_LIST_TRAVERSE(&device->buttonconfig, config, list) {
									sccp_log((DEBUGCAT_CONFIG + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_4 "%s: button %d marked for removal\n", device->id, config->index);
									config->pendingDelete = 1;
								}
								SCCP_LIST_UNLOCK(&device->buttonconfig);
#ifdef CS_SCCP_REALTIME
								if (device->realtime) {
									if ((dv = pbx_load_realtime(GLOB(realtimedevicetable), "name", argv[3], NULL))) {
										change |= sccp_config_applyDeviceConfiguration(device, dv);
									}
								} else
#endif
								{
									if (GLOB(cfg)) {
										v = ast_variable_browse(GLOB(cfg), device->id);
										change |= sccp_config_applyDeviceConfiguration(device, v);
									}
								}
								device->pendingUpdate = 1;
								sccp_device_check_update(device);				// Will cleanup after reload and restart the device if necessary
#ifdef CS_SCCP_REALTIME
								if (device->realtime && dv) {
									pbx_variables_destroy(dv);
								}
#endif
							}
						}
						SCCP_LIST_UNLOCK(&line->devices);
					}
#ifdef CS_SCCP_REALTIME
					if (line->realtime) {
						pbx_variables_destroy(v);
					}
#endif
				} else {
					/* the loaded configuration no longer defines this line */
					line->pendingDelete = 1;
					pbx_cli(fd, "Line %s is no longer defined in %s; it will be removed\n", line->name, GLOB(config_file_name));
				}

				returnval = RESULT_SUCCESS;
			} else {
				returnval = RESULT_SHOWUSAGE;
			}
			goto EXIT;
		} else if (sccp_strequals("force", argv[2]) && argc == 3) {
			force_reload = TRUE;
		} else if (sccp_strequals("file", argv[2])) {
			if (argc == 4) {
				// build config file path
				int filename_len = 0;
				if(argv[3][0] != '/') {
					filename_len = strlen(ast_config_AST_CONFIG_DIR) + strlen(argv[3]) + 2;
					filename = (char *)alloca(filename_len);
					snprintf(filename, filename_len, "%s/%s", ast_config_AST_CONFIG_DIR, argv[3]);
				} else {
					filename = pbx_strdupa(argv[3]);
				}
				// check file exists
				struct stat sb = { 0 };
				int stat_res = stat(filename, &sb);
				if (stat_res != 0 || !S_ISREG(sb.st_mode)) {
					pbx_cli(fd, "%s not reloaded: %s\n", filename, stat_res != 0 ? (errno == ENOENT ? "file not found" : strerror(errno)) : "not a regular file");
					goto EXIT;
				}

				if(!sccp_strequals(GLOB(config_file_name), filename)) {
					force_reload = TRUE;
				}
			} else {
				returnval = RESULT_SHOWUSAGE;
				goto EXIT;
			}
		} else {
			returnval = RESULT_SHOWUSAGE;
			goto EXIT;
		}
	}
	sccp_config_file_status_t cfg = sccp_config_getConfig(force_reload, filename);

	switch (cfg) {
		case CONFIG_STATUS_FILE_NOT_CHANGED:
			pbx_cli(fd, "%s has not changed; nothing reloaded\n", filename);
			returnval = RESULT_SUCCESS;
			break;
		case CONFIG_STATUS_FILE_OK:
			if (GLOB(cfg)) {
				if (!sccp_config_general(SCCP_CONFIG_READRELOAD)) {
					pbx_cli(fd, "%s not fully reloaded: the [general] section could not be applied (see the log)\n", filename);
					goto EXIT;
				}
				if (!sccp_config_readDevicesLines(SCCP_CONFIG_READRELOAD)) {
					pbx_cli(fd, "%s not fully reloaded: the devices and lines could not be applied (see the log)\n", filename);
					goto EXIT;
				}
				returnval = RESULT_SUCCESS;
				if (GLOB(srvcontexts[SCCP_SERVERCONTEXT_TCP]) && !sccp_servercontext_reload(GLOB(srvcontexts[SCCP_SERVERCONTEXT_TCP]), &GLOB(bindaddr))) {
					pbx_cli(fd, "%s reloaded, but the SCCP listener could not move to bindaddr %s\n", filename, sccp_netsock_stringify(&GLOB(bindaddr)));
					returnval = RESULT_FAILURE;
				}
#if HAVE_LIBSSL
				if (GLOB(srvcontexts[SCCP_SERVERCONTEXT_TLS]) && !sccp_servercontext_reload(GLOB(srvcontexts[SCCP_SERVERCONTEXT_TLS]), &GLOB(secbindaddr))) {
					pbx_cli(fd, "%s reloaded, but the TLS listener could not move to secbindaddr %s\n", filename, sccp_netsock_stringify(&GLOB(secbindaddr)));
					returnval = RESULT_FAILURE;
				}
#endif
				if (returnval == RESULT_SUCCESS) {
					pbx_cli(fd, "%s reloaded\n", filename);
				}
			}
			break;
		case CONFIG_STATUS_FILE_OLD:
			pbx_cli(fd, "%s not reloaded: it uses the old format with a [devices] section; current configuration kept\n", filename);
			break;
		case CONFIG_STATUS_FILE_NOT_SCCP:
			pbx_cli(fd, "%s not reloaded: it is not an sccp.conf file; current configuration kept\n", filename);
			break;
		case CONFIG_STATUS_FILE_NOT_FOUND:
			pbx_cli(fd, "%s not reloaded: file not found; current configuration kept\n", filename);
			break;
		case CONFIG_STATUS_FILE_INVALID:
			pbx_cli(fd, "%s not reloaded: the file could not be parsed; current configuration kept\n", filename);
			break;
	}
EXIT:
	pbx_rwlock_wrlock(&GLOB(lock));
	GLOB(reload_in_progress) = FALSE;
	pbx_rwlock_unlock(&GLOB(lock));
	return returnval;
}

static char reload_usage[] = "Usage: sccp reload [force | file <file> | device <device> | line <line>]\n"
			     "       Reload sccp.conf (or <file>), only if it changed unless force is given.\n"
			     "       device and line reload one section. Devices whose settings changed are\n"
			     "       restarted; a device on a call restarts when the call ends.\n";

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#define CLI_COMPLETE SCCP_CLI_NULL_COMPLETER
#define CLI_COMMAND "sccp", "reload"
CLI_ENTRY(cli_reload, sccp_cli_reload, "Reload the SCCP configuration", reload_usage, FALSE)
#undef CLI_COMMAND
#define CLI_COMMAND "sccp", "reload", "file"
CLI_ENTRY(cli_reload_file, sccp_cli_reload, "Reload the SCCP configuration", reload_usage, FALSE)
#undef CLI_COMMAND
#define CLI_COMMAND "sccp", "reload", "force"
    CLI_ENTRY(cli_reload_force, sccp_cli_reload, "Reload the SCCP configuration", reload_usage, FALSE)
#undef CLI_COMMAND
#undef CLI_COMPLETE
#define CLI_COMPLETE SCCP_CLI_DEVICE_COMPLETER
#define CLI_COMMAND "sccp", "reload", "device"
    CLI_ENTRY(cli_reload_device, sccp_cli_reload, "Reload the SCCP configuration", reload_usage, FALSE)
#undef CLI_COMMAND
#undef CLI_COMPLETE
#define CLI_COMPLETE SCCP_CLI_LINE_COMPLETER
#define CLI_COMMAND "sccp", "reload", "line"
    CLI_ENTRY(cli_reload_line, sccp_cli_reload, "Reload the SCCP configuration", reload_usage, FALSE)
#undef CLI_COMMAND
#undef CLI_COMPLETE
#endif														/* DOXYGEN_SHOULD_SKIP_THIS */
    /*!
     * \brief Generare sccp.conf
     * \param fd Fd as int
     * \param argc Argc as int
     * \param argv[] Argv[] as char
     * \return Result as int
     * 
     * \called_from_asterisk
     */
static int sccp_cli_config_generate(int fd, int argc, char *argv[])
{
	const char * config_file = argc >= 4 ? argv[3] : "sccp.conf.new";
	int          option      = 0;
	char         fn[PATH_MAX];

	if (argc < 3 || argc > 5 || (argc == 5 && !sccp_strcaseequals(argv[4], "wiki"))) {
		return RESULT_SHOWUSAGE;
	}
	if (argc == 5) {
		option = 3;
	}
	sccp_config_generate_path(fn, sizeof(fn), config_file);
	if (sccp_config_generate(pbx_strdupa(config_file), option) != 0) {
		pbx_cli(fd, "%s not written: %s\n", fn, errno == EEXIST ? "the file already exists" : strerror(errno));
		return RESULT_FAILURE;
	}
	pbx_cli(fd, "%s written\n", fn);
	return RESULT_SUCCESS;
}

static char config_generate_usage[] = "Usage: sccp config generate [file [wiki]]\n"
				      "       Write every sccp.conf option with its default value to file (default\n"
				      "       sccp.conf.new). A relative name is placed in the Asterisk configuration\n"
				      "       directory. An existing file is not overwritten. With wiki, the options\n"
				      "       are written as a wiki page.\n";

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#define CLI_COMMAND "sccp", "config", "generate"
#define CLI_COMPLETE SCCP_CLI_NULL_COMPLETER
CLI_ENTRY(cli_config_generate, sccp_cli_config_generate, "Generate a SCCP configuration file", config_generate_usage, FALSE)
#undef CLI_COMMAND
#undef CLI_COMPLETE
#endif														/* DOXYGEN_SHOULD_SKIP_THIS */

/* ------------------------------------------------------------------------------------------------ GENERATE CNF - */
/* sccp generate cnf <device> [file [server-address]]: write the phone's TFTP configuration (SEPxxxx.cnf.xml)
 * from sccp.conf. Lines and buttons are not in the file: the phone gets them from chan_sccp when it registers. */
static int sccp_generate_cnf(int fd, sccp_cli_totals_t * totals, struct mansession * s, const struct message * m, int argc, char * argv[])
{
	if (argc < 4 || argc > 6 || !ARG_PRESENT(3)) {
		return RESULT_SHOWUSAGE;
	}
	AUTO_RELEASE(sccp_device_t, d, sccp_device_find_byid(argv[3], FALSE));
	if (!d) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "Device %s does not exist", argv[3]);
	}

	/* the server address the phone should register to */
	char server[INET6_ADDRSTRLEN + 2] = "";
	const char * source = "";
	if (ARG_PRESENT(5)) {
		sccp_copy_string(server, argv[5], sizeof(server));
		source = "the command";
	} else {
		struct sockaddr_storage ourip = { 0 };
		if (d->session && sccp_session_getOurIP(d->session, &ourip, 0) && !sccp_netsock_is_any_addr(&ourip)) {
			sccp_copy_string(server, sccp_netsock_stringify_addr(&ourip), sizeof(server));
			source = "the address the phone is registered to";
		} else if (!sccp_netsock_is_any_addr(&GLOB(bindaddr))) {
			sccp_copy_string(server, sccp_netsock_stringify_addr(&GLOB(bindaddr)), sizeof(server));
			source = "bindaddr";
		} else if (!sccp_netsock_is_any_addr(&GLOB(externip))) {
			sccp_copy_string(server, sccp_netsock_stringify_addr(&GLOB(externip)), sizeof(server));
			source = "externip";
		}
	}
	if (sccp_strlen_zero(server)) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "No server address for %s: it is not registered, bindaddr is a wildcard address and externip is not set; give the address as the last argument", d->id);
	}
	/* IPv6 literals from stringify_addr come bracketed; processNodeName wants the bare address */
	if (server[0] == '[') {
		size_t len = strlen(server);
		memmove(server, server + 1, len);
		char * end = strchr(server, ']');
		if (end) {
			*end = '\0';
		}
	}
	uint16_t port = sccp_netsock_getPort(&GLOB(bindaddr));

	char fn[PATH_MAX];
	char name[StationMaxDeviceNameSize + 16];
	snprintf(name, sizeof(name), "%s.cnf.xml", d->id);
	if (ARG_PRESENT(4)) {
		struct stat sb;
		if (stat(argv[4], &sb) == 0 && S_ISDIR(sb.st_mode)) {
			snprintf(fn, sizeof(fn), "%s/%s", argv[4], name);
		} else {
			sccp_config_generate_path(fn, sizeof(fn), argv[4]);
		}
	} else {
		sccp_config_generate_path(fn, sizeof(fn), name);
	}

	int fdout = open(fn, O_CREAT | O_EXCL | O_WRONLY, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (fdout == -1) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "%s not written: %s", fn, errno == EEXIST ? "the file already exists" : strerror(errno));
	}
	FILE * f = fdopen(fdout, "w");
	if (!f) {
		int err = errno;
		close(fdout);
		CLI_AMI_RETURN_ERROR(fd, s, m, "%s not written: %s", fn, strerror(err));
	}

	char date_buf[64];
	if (sccp_strlen_zero(GLOB(dateformat)) || ast_xml_escape(GLOB(dateformat), date_buf, sizeof(date_buf))) {
		sccp_copy_string(date_buf, "M/D/YY", sizeof(date_buf));
	}
	char load_buf[StationMaxImageVersionSize * 6 + 1] = "";
	if (!sccp_strlen_zero(d->imageversion) && ast_xml_escape(d->imageversion, load_buf, sizeof(load_buf))) {
		load_buf[0] = '\0';
	}
	boolean_t english = sccp_strlen_zero(GLOB(language)) || !strncasecmp(GLOB(language), "en", 2);

	fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
	fprintf(f, "<!-- %s: written by chan_sccp 'sccp generate cnf' from sccp.conf; server address from %s.\n", name, source);
	fprintf(f, "     sccp.conf has no NTP, time zone or phone service URL settings, so none are included. -->\n");
	fprintf(f, "<device>\n");
	fprintf(f, "  <deviceProtocol>SCCP</deviceProtocol>\n");
	fprintf(f, "  <devicePool>\n");
	fprintf(f, "    <dateTimeSetting>\n");
	fprintf(f, "      <dateTemplate>%s</dateTemplate>\n", date_buf);
	fprintf(f, "    </dateTimeSetting>\n");
	fprintf(f, "    <callManagerGroup>\n");
	fprintf(f, "      <members>\n");
	fprintf(f, "        <member priority=\"0\">\n");
	fprintf(f, "          <callManager>\n");
	fprintf(f, "            <ports>\n");
	fprintf(f, "              <ethernetPhonePort>%u</ethernetPhonePort>\n", port ? port : 2000);
	fprintf(f, "            </ports>\n");
	fprintf(f, "            <processNodeName>%s</processNodeName>\n", server);
	fprintf(f, "          </callManager>\n");
	fprintf(f, "        </member>\n");
	fprintf(f, "      </members>\n");
	fprintf(f, "    </callManagerGroup>\n");
	fprintf(f, "  </devicePool>\n");
	if (load_buf[0]) {
		fprintf(f, "  <loadInformation>%s</loadInformation>\n", load_buf);
	}
	if (english) {
		fprintf(f, "  <userLocale>\n");
		fprintf(f, "    <name>English_United_States</name>\n");
		fprintf(f, "    <langCode>en_US</langCode>\n");
		fprintf(f, "  </userLocale>\n");
		fprintf(f, "  <networkLocale>United_States</networkLocale>\n");
	}
	fprintf(f, "  <dscpForSCCPPhoneConfig>%d</dscpForSCCPPhoneConfig>\n", GLOB(sccp_tos));
	fprintf(f, "  <dscpForCm2Dvce>%d</dscpForCm2Dvce>\n", d->audio_tos);
	fprintf(f, "</device>\n");
	if (fclose(f) != 0) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "%s not written completely: %s", fn, strerror(errno));
	}
	CLI_AMI_RETURN_DONE(fd, s, m, "%s written (server %s port %u, from %s)", fn, server, port ? port : 2000, source);
}

static char cli_generate_cnf_usage[] = "Usage: sccp generate cnf <device> [file [server-address]]\n"
				       "       Write the phone's TFTP configuration file (<device>.cnf.xml) from sccp.conf:\n"
				       "       server address and port, date format, firmware (imageversion), TOS and\n"
				       "       locale. file may be a directory; a relative name is placed in the Asterisk\n"
				       "       configuration directory; an existing file is not overwritten. The server\n"
				       "       address defaults to the one the phone is registered to, then bindaddr,\n"
				       "       then externip.\n";

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#define CLI_COMMAND "sccp", "generate", "cnf"
#define CLI_COMPLETE SCCP_CLI_DEVICE_COMPLETER
CLI_AMI_ENTRY(generate_cnf, sccp_generate_cnf, "Write a phone's TFTP configuration", cli_generate_cnf_usage, FALSE, FALSE)
SCCP_AMI_ACTION(generate_cnf, sccp_generate_cnf, "SCCPGenerateCnf", FALSE, "sccp", "generate", "cnf", "$Device", "$File", "$Server")
#undef CLI_COMPLETE
#undef CLI_COMMAND
#endif														/* DOXYGEN_SHOULD_SKIP_THIS */
    /* -------------------------------------------------------------------------------------------------------SHOW VERSION- */
    /*!
     * \brief Show Version
     * \param fd Fd as int
     * \param argc Argc as int
     * \param argv[] Argv[] as char
     * \return Result as int
     * 
     * \called_from_asterisk
     */
static int sccp_show_version(int fd, int argc, char *argv[])
{
	pbx_cli(fd, "%s\n", SCCP_VERSIONSTR);
	return RESULT_SUCCESS;
}

static char show_version_usage[] =  "Usage: sccp show version\n       Show the chan_sccp version.\n";

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#define CLI_COMMAND "sccp", "show", "version"
#define CLI_COMPLETE SCCP_CLI_NULL_COMPLETER
CLI_ENTRY(cli_show_version, sccp_show_version, "Show SCCP version details", show_version_usage, FALSE)
#undef CLI_COMPLETE
#undef CLI_COMMAND
#endif														/* DOXYGEN_SHOULD_SKIP_THIS */
/* --------------------------------------------------------------------------------------------------------------- SET - */
/* Parse user input without the enum converter's error-log side effects. */
static sccp_cfwd_t sccp_cli_forward_type(const char * value)
{
	if (sccp_strcaseequals(value, "none")) {
		return SCCP_CFWD_NONE;
	}
	if (sccp_strcaseequals(value, "all")) {
		return SCCP_CFWD_ALL;
	}
	if (sccp_strcaseequals(value, "busy")) {
		return SCCP_CFWD_BUSY;
	}
	if (sccp_strcaseequals(value, "noanswer")) {
		return SCCP_CFWD_NOANSWER;
	}
	return SCCP_CFWD_SENTINEL;
}

/* sccp set device <device> dnd|microphone|ringtone|backgroundimage|<option> <value> [...] */
static int sccp_set_device(int fd, sccp_cli_totals_t * totals, struct mansession * s, const struct message * m, int argc, char * argv[])
{
	if (!ARG_PRESENT(3) || !ARG_PRESENT(4) || !ARG_PRESENT(5)) {
		return RESULT_SHOWUSAGE;
	}
	AUTO_RELEASE(sccp_device_t, d, sccp_device_find_byid(argv[3], FALSE));
	if (!d) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "Device %s does not exist", argv[3]);
	}
	const char * what = argv[4];
	const char * value = argv[5];

	if (sccp_strcaseequals(what, "dnd")) {
		sccp_dndmode_t state;
		if (sccp_strcaseequals(value, "off")) {
			state = SCCP_DNDMODE_OFF;
		} else if (sccp_strcaseequals(value, "reject")) {
			state = SCCP_DNDMODE_REJECT;
		} else if (sccp_strcaseequals(value, "silent")) {
			state = SCCP_DNDMODE_SILENT;
		} else {
			return RESULT_SHOWUSAGE;
		}
		if (!d->dndFeature.enabled) {
			CLI_AMI_RETURN_ERROR(fd, s, m, "DND is not enabled on device %s (dndFeature = no)", d->id);
		}
		if (d->dndFeature.status == state) {
			CLI_AMI_RETURN_DONE(fd, s, m, "DND on %s is already %s", d->id, sccp_dndmode2str(state));
		}
		d->dndFeature.status = state;
		sccp_feat_changed(d, NULL, SCCP_FEATURE_DND);
		sccp_dev_check_displayprompt(d);
		CLI_AMI_RETURN_DONE(fd, s, m, "DND on %s set to %s", d->id, sccp_dndmode2str(state));
	}
	if (sccp_strcaseequals(what, "debug")) {
		if (!sccp_true(value) && !sccp_false(value)) {
			return RESULT_SHOWUSAGE;
		}
		if (sccp_false(value)) {
			if (!sccp_debug_filter_remove(d->id)) {
				CLI_AMI_RETURN_DONE(fd, s, m, "Debug for %s was not on", d->id);
			}
			CLI_AMI_RETURN_DONE(fd, s, m, "Debug for %s turned off%s", d->id, sccp_debug_filter_active ? "" : "; debug output is no longer limited to marked devices");
		}
		/* match the device name and the call names (SCCP/<line>-) of its lines */
		const char *          matches[SCCP_DEBUG_FILTER_MAX_MATCHES];
		char                  names[SCCP_DEBUG_FILTER_MAX_MATCHES][96];
		int                   nmatches = 0;
		sccp_buttonconfig_t * config   = NULL;
		SCCP_LIST_LOCK(&d->buttonconfig);
		SCCP_LIST_TRAVERSE(&d->buttonconfig, config, list) {
			if (config->type == LINE && !sccp_strlen_zero(config->button.line.name) && nmatches < SCCP_DEBUG_FILTER_MAX_MATCHES - 1) {
				snprintf(names[nmatches], sizeof(names[nmatches]), "SCCP/%s-", config->button.line.name);
				matches[nmatches] = names[nmatches];
				nmatches++;
			}
		}
		SCCP_LIST_UNLOCK(&d->buttonconfig);
		if (!sccp_debug_filter_set(d->id, matches, nmatches)) {
			CLI_AMI_RETURN_ERROR(fd, s, m, "Debug for %s not turned on: too many devices are already marked", d->id);
		}
		if (!GLOB(debug)) {
			GLOB(debug) = DEBUGCAT_CORE | DEBUGCAT_DEVICE | DEBUGCAT_LINE | DEBUGCAT_ACTION | DEBUGCAT_CHANNEL | DEBUGCAT_INDICATE | DEBUGCAT_SOFTKEY;
			CLI_AMI_RETURN_DONE(fd, s, m, "Debug for %s turned on with categories core, device, line, action, channel, indicate, softkey; other devices' debug output is suppressed", d->id);
		}
		char * categories = sccp_get_debugcategories(GLOB(debug));
		char   msg[512];
		snprintf(msg, sizeof(msg), "Debug for %s turned on with categories %s; other devices' debug output is suppressed", d->id, categories ? categories : "none");
		sccp_free(categories);
		CLI_AMI_RETURN_DONE(fd, s, m, "%s", msg);
	}
	if (sccp_strcaseequals(what, "microphone")) {
		if (!sccp_true(value) && !sccp_false(value)) {
			return RESULT_SHOWUSAGE;
		}
		AUTO_RELEASE(sccp_channel_t, c, sccp_device_getActiveChannel(d));
		if (!c) {
			CLI_AMI_RETURN_ERROR(fd, s, m, "Device %s has no active call", d->id);
		}
		sccp_dev_set_microphone(d, sccp_true(value) ? SKINNY_STATIONMIC_ON : SKINNY_STATIONMIC_OFF);
		CLI_AMI_RETURN_DONE(fd, s, m, "Microphone on %s turned %s", d->id, sccp_true(value) ? "on" : "off");
	}
	if (sccp_strcaseequals(what, "ringtone")) {
		if (!d->session) {
			CLI_AMI_RETURN_ERROR(fd, s, m, "Device %s is not registered", d->id);
		}
		d->setRingTone(d, value);
		CLI_AMI_RETURN_DONE(fd, s, m, "Ringtone %s sent to %s", value, d->id);
	}
	if (sccp_strcaseequals(what, "backgroundimage")) {
		if (!d->session) {
			CLI_AMI_RETURN_ERROR(fd, s, m, "Device %s is not registered", d->id);
		}
		d->setBackgroundImage(d, value, ARG_PRESENT(6) ? argv[6] : value);
		CLI_AMI_RETURN_DONE(fd, s, m, "Background image %s sent to %s", value, d->id);
	}

	/* any other sccp.conf device option, applied to the running device only */
	int res = sccp_config_setDeviceOption(d, what, value);
	if (res & SCCP_CONFIG_ERROR) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "%s is not a device option that can be changed at runtime", what);
	}
	if (res & SCCP_CONFIG_NEEDDEVICERESET) {
		d->pendingUpdate = 1;
		CLI_AMI_RETURN_DONE(fd, s, m, "%s set to %s on %s; takes effect after the phone restarts (not saved to sccp.conf)", what, value, d->id);
	}
	CLI_AMI_RETURN_DONE(fd, s, m, "%s set to %s on %s (not saved to sccp.conf)", what, value, d->id);
}

/* sccp set line <line> [device] forward <all|busy|noanswer> [number] | forward none */
static int sccp_set_line(int fd, sccp_cli_totals_t * totals, struct mansession * s, const struct message * m, int argc, char * argv[])
{
	int next = 4;
	const char * device = NULL;

	if (!ARG_PRESENT(3)) {
		return RESULT_SHOWUSAGE;
	}
	/* optional device before "forward"; an absent AMI Device header arrives as "" */
	if (argc > next && !sccp_strcaseequals(argv[next], "forward")) {
		if (!sccp_strlen_zero(argv[next])) {
			device = argv[next];
		}
		next++;
	}
	if (argc <= next + 1 || !sccp_strcaseequals(argv[next], "forward")) {
		return RESULT_SHOWUSAGE;
	}
	sccp_cfwd_t type = sccp_cli_forward_type(argv[next + 1]);
	const char * number = ARG_PRESENT(next + 2) ? argv[next + 2] : NULL;
	if (type == SCCP_CFWD_SENTINEL || argc > next + 3 || (type == SCCP_CFWD_NONE && number)) {
		return RESULT_SHOWUSAGE;
	}

	AUTO_RELEASE(sccp_line_t, l, sccp_line_find_byname(argv[3], FALSE));
	if (!l) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "Line %s does not exist", argv[3]);
	}
	int count = 0;
	if (device) {
		AUTO_RELEASE(sccp_device_t, d, sccp_device_find_byid(device, FALSE));
		if (!d) {
			CLI_AMI_RETURN_ERROR(fd, s, m, "Device %s does not exist", device);
		}
		AUTO_RELEASE(sccp_linedevice_t, ld, sccp_linedevice_find(d, l));
		if (!ld) {
			CLI_AMI_RETURN_ERROR(fd, s, m, "Line %s is not on device %s", l->name, d->id);
		}
		sccp_linedevice_cfwd(ld, type, (char *)number);
		count = 1;
	} else {
		sccp_linedevice_t * ld = NULL;
		SCCP_LIST_LOCK(&l->devices);
		SCCP_LIST_TRAVERSE(&l->devices, ld, list) {
			sccp_linedevice_cfwd(ld, type, (char *)number);
			count++;
		}
		SCCP_LIST_UNLOCK(&l->devices);
	}
	if (!count) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "Line %s is not on any registered device", l->name);
	}
	if (type == SCCP_CFWD_NONE) {
		CLI_AMI_RETURN_DONE(fd, s, m, "All call forwards on line %s cleared (%d device%s)", l->name, count, count == 1 ? "" : "s");
	}
	CLI_AMI_RETURN_DONE(fd, s, m, "Call forward %s on line %s %s%s (%d device%s)", sccp_cfwd2str(type), l->name, number ? "set to " : "cleared", number ? number : "", count, count == 1 ? "" : "s");
}

/* sccp set channel <call> hold on|off [device] | park */
static int sccp_set_channel(int fd, sccp_cli_totals_t * totals, struct mansession * s, const struct message * m, int argc, char * argv[])
{
	if (!ARG_PRESENT(3) || !ARG_PRESENT(4)) {
		return RESULT_SHOWUSAGE;
	}
	AUTO_RELEASE(sccp_channel_t, c, sccp_cli_find_call(argv[3]));
	if (!c) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "Call %s does not exist", argv[3]);
	}
	if (sccp_strcaseequals("hold", argv[4])) {
		if (!ARG_PRESENT(5)) {
			return RESULT_SHOWUSAGE;
		}
		if (sccp_true(argv[5])) {
			if (!sccp_channel_hold(c)) {
				CLI_AMI_RETURN_ERROR(fd, s, m, "Call %s was not put on hold (see the log)", c->designator);
			}
			CLI_AMI_RETURN_DONE(fd, s, m, "Call %s put on hold", c->designator);
		}
		if (!sccp_false(argv[5])) {
			return RESULT_SHOWUSAGE;
		}
		/* resuming needs the device that takes the call back; a held call is detached from its device,
		 * so default to the only device of a line that is not shared */
		AUTO_RELEASE(sccp_device_t, d, ARG_PRESENT(6) ? sccp_device_find_byid(argv[6], FALSE) : sccp_channel_getDevice(c));
		if (!d && !ARG_PRESENT(6) && c->line && !c->line->isShared) {
			sccp_linedevice_t * ld = NULL;
			SCCP_LIST_LOCK(&c->line->devices);
			ld = SCCP_LIST_FIRST(&c->line->devices);
			d = ld ? sccp_device_retain(ld->device) : NULL /*ref_replace*/;
			SCCP_LIST_UNLOCK(&c->line->devices);
		}
		if (!d) {
			if (ARG_PRESENT(6)) {
				CLI_AMI_RETURN_ERROR(fd, s, m, "Device %s does not exist", argv[6]);
			}
			CLI_AMI_RETURN_ERROR(fd, s, m, "Call %s is on a shared line; name the device that should resume it", c->designator);
		}
		if (!sccp_channel_resume(d, c, FALSE)) {
			CLI_AMI_RETURN_ERROR(fd, s, m, "Call %s was not resumed on %s (see the log)", c->designator, d->id);
		}
		CLI_AMI_RETURN_DONE(fd, s, m, "Call %s resumed on %s", c->designator, d->id);
	}
#ifdef CS_SCCP_PARK
	if (sccp_strcaseequals("park", argv[4])) {
		sccp_channel_park(c);
		CLI_AMI_RETURN_DONE(fd, s, m, "Parking call %s", c->designator);
	}
#endif
	return RESULT_SHOWUSAGE;
}

/* sccp set fallback <true|false|odd|even|/path/to/script> */
static int sccp_set_fallback(int fd, sccp_cli_totals_t * totals, struct mansession * s, const struct message * m, int argc, char * argv[])
{
	if (argc != 4 || !ARG_PRESENT(3)) {
		return RESULT_SHOWUSAGE;
	}
	const char * value = argv[3];
	if (value[0] == '/') {
		struct stat sb;
		if (stat(value, &sb) != 0 || !(sb.st_mode & S_IXUSR)) {
			CLI_AMI_RETURN_ERROR(fd, s, m, "Fallback script %s does not exist or is not executable", value);
		}
	} else if (!sccp_strcaseequals(value, "odd") && !sccp_strcaseequals(value, "even") && !sccp_true(value) && !sccp_false(value)) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "Fallback '%s' is not true, false, odd, even or an absolute script path", value);
	}
	if (GLOB(token_fallback)) {
		sccp_free(GLOB(token_fallback));
	}
	GLOB(token_fallback) = pbx_strdup(value);
	CLI_AMI_RETURN_DONE(fd, s, m, "Fallback set to %s (not saved to sccp.conf)", GLOB(token_fallback));
}

static int sccp_set_object(int fd, sccp_cli_totals_t * totals, struct mansession * s, const struct message * m, int argc, char * argv[])
{
	if (argc < 4 || sccp_strlen_zero(argv[2])) {
		return RESULT_SHOWUSAGE;
	}
	if (sccp_strcaseequals("device", argv[2])) {
		return sccp_set_device(fd, totals, s, m, argc, argv);
	}
	if (sccp_strcaseequals("line", argv[2])) {
		return sccp_set_line(fd, totals, s, m, argc, argv);
	}
	if (sccp_strcaseequals("channel", argv[2])) {
		return sccp_set_channel(fd, totals, s, m, argc, argv);
	}
	if (sccp_strcaseequals("fallback", argv[2])) {
		return sccp_set_fallback(fd, totals, s, m, argc, argv);
	}
	return RESULT_SHOWUSAGE;
}

static char cli_set_usage[] =
	"Usage: sccp set device <device> dnd <off|reject|silent>\n"
	"       sccp set device <device> debug <on|off>\n"
	"       sccp set device <device> microphone <on|off>\n"
	"       sccp set device <device> ringtone <url>\n"
	"       sccp set device <device> backgroundimage <url> [thumbnail-url]\n"
	"       sccp set device <device> <option> <value>\n"
	"       sccp set line <line> [device] forward <all|busy|noanswer> [number]\n"
	"       sccp set line <line> [device] forward none\n"
	"       sccp set channel <call> hold <on|off> [device]\n"
	"       sccp set channel <call> park\n"
	"       sccp set fallback <true|false|odd|even|/path/to/script>\n"
	"\n"
	"       microphone mutes or unmutes the device's active call.\n"
	"       <option> is any sccp.conf device option; the value applies to the\n"
	"       running device only and is not saved to sccp.conf.\n"
	"       forward without a number clears that forward type; 'none' clears all.\n"
	"       Without a device, forward applies to every device that has the line.\n"
	"       <call> is the ID from 'sccp show channels' or a name like SCCP/2004-00000003.\n"
	"       hold off resumes the call on [device], or on the call's own device.\n";

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#define CLI_COMMAND "sccp", "set"
#	define CLI_COMPLETE SCCP_CLI_NULL_COMPLETER
CLI_AMI_ENTRY(set_object, sccp_set_object, "Change device, line, call or fallback settings", cli_set_usage, FALSE, FALSE)
SCCP_AMI_ACTION(set_device_dnd, sccp_set_object, "SCCPSetDeviceDND", FALSE, "sccp", "set", "device", "$Device", "dnd", "$State")
SCCP_AMI_ACTION(set_device_microphone, sccp_set_object, "SCCPSetDeviceMicrophone", FALSE, "sccp", "set", "device", "$Device", "microphone", "$State")
SCCP_AMI_ACTION(set_device_option, sccp_set_object, "SCCPSetDeviceOption", FALSE, "sccp", "set", "device", "$Device", "$Option", "$Value")
SCCP_AMI_ACTION(set_line_forward, sccp_set_object, "SCCPSetLineForward", FALSE, "sccp", "set", "line", "$Line", "$Device", "forward", "$Type", "$Number")
SCCP_AMI_ACTION(hold, sccp_set_object, "SCCPHold", FALSE, "sccp", "set", "channel", "$Call", "hold", "$State", "$Device")
SCCP_AMI_ACTION(set_fallback, sccp_set_object, "SCCPSetFallback", FALSE, "sccp", "set", "fallback", "$Fallback")
#	undef CLI_COMPLETE
#	undef CLI_COMMAND
#endif

/* ----------------------------------------------------------------------------------------------------- DEVICE CONTROL - */
/* sccp push url <device> <url>: make the phone open url (needs the phone's authentication URL to accept it) */
static int sccp_push_url(int fd, sccp_cli_totals_t * totals, struct mansession * s, const struct message * m, int argc, char * argv[])
{
	if (argc != 5 || !ARG_PRESENT(3) || !ARG_PRESENT(4)) {
		return RESULT_SHOWUSAGE;
	}
	AUTO_RELEASE(sccp_device_t, d, sccp_device_find_byid(argv[3], FALSE));
	if (!d) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "Device %s does not exist", argv[3]);
	}
	if (!d->session) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "Device %s is not registered", d->id);
	}
	switch (d->pushURL(d, argv[4], 1, SKINNY_TONE_ZIP)) {
		case SCCP_PUSH_RESULT_SUCCESS:
			CLI_AMI_RETURN_DONE(fd, s, m, "URL sent to %s", d->id);
		case SCCP_PUSH_RESULT_NOT_SUPPORTED:
			CLI_AMI_RETURN_ERROR(fd, s, m, "URL not sent: %s (%s, protocol %d) does not support pushed URLs", d->id, skinny_devicetype2str(d->skinny_type), d->inuseprotocolversion);
		default:
			CLI_AMI_RETURN_ERROR(fd, s, m, "URL not sent to %s: it is longer than 256 characters or could not be encoded", d->id);
	}
}

/* feed a message to the normal message handler as if the phone had sent it */
static void sccp_cli_inject(constDevicePtr d, sccp_msg_t * msg)
{
	msg->header.lel_protocolVer = htolel(d->inuseprotocolversion);
	sccp_handle_message(msg, d->session);
	sccp_free(msg);
}

/* sccp press <device> softkey <label> | digits <digits> | offhook | onhook */
static int sccp_press(int fd, sccp_cli_totals_t * totals, struct mansession * s, const struct message * m, int argc, char * argv[])
{
	if (argc < 4 || argc > 5 || !ARG_PRESENT(2) || !ARG_PRESENT(3)) {
		return RESULT_SHOWUSAGE;
	}
	AUTO_RELEASE(sccp_device_t, d, sccp_device_find_byid(argv[2], FALSE));
	if (!d) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "Device %s does not exist", argv[2]);
	}
	if (!d->session || sccp_device_getRegistrationState(d) != SKINNY_DEVICE_RS_OK) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "Device %s is not registered", d->id);
	}
	const char * what  = argv[3];
	const char * value = argc == 5 ? argv[4] : "";

	/* the key applies to the active call, or else to a call held on this device (for Resume) */
	uint32_t lineInstance = 0;
	uint32_t callid       = 0;
	{
		AUTO_RELEASE(sccp_channel_t, c, sccp_device_getActiveChannel(d));
		if (!c) {
			/* a held call is detached from the device, so look on the device's lines */
			sccp_buttonconfig_t * config = NULL;
			SCCP_LIST_LOCK(&d->buttonconfig);
			SCCP_LIST_TRAVERSE(&d->buttonconfig, config, list) {
				if (config->type == LINE && !c) {
					AUTO_RELEASE(sccp_line_t, l, sccp_line_find_byname(config->button.line.name, FALSE));
					if (l) {
						c = sccp_channel_find_bystate_on_line(l, SCCP_CHANNELSTATE_HOLD) /*ref_replace*/;
					}
				}
			}
			SCCP_LIST_UNLOCK(&d->buttonconfig);
		}
		if (c && c->line) {
			lineInstance = sccp_device_find_index_for_line(d, c->line->name);
			callid       = c->callid;
		}
	}
	sccp_msg_t * msg = NULL;

	if (sccp_strcaseequals(what, "offhook") || sccp_strcaseequals(what, "onhook")) {
		if (argc != 4) {
			return RESULT_SHOWUSAGE;
		}
		if (sccp_strcaseequals(what, "offhook")) {
			REQ(msg, OffHookMessage);
		} else {
			REQ(msg, OnHookMessage);
		}
		if (!msg) {
			CLI_AMI_RETURN_ERROR(fd, s, m, "Key not pressed on %s: out of memory", d->id);
		}
		sccp_cli_inject(d, msg);
		CLI_AMI_RETURN_DONE(fd, s, m, "%s pressed on %s", sccp_strcaseequals(what, "offhook") ? "Off hook" : "On hook", d->id);
	}
	if (sccp_strcaseequals(what, "softkey")) {
		if (argc != 5) {
			return RESULT_SHOWUSAGE;
		}
		uint32_t event = 0;
		for (uint32_t i = 0; i < ARRAY_LEN(softkeysmap) && !event; i++) {
			const char * label = label2str(softkeysmap[i]);
			if (label && !strcasecmp(label, value)) {
				event = i + 1;
			}
		}
		if (!event) {
			CLI_AMI_RETURN_ERROR(fd, s, m, "%s is not a softkey name (names as in 'sccp show softkey sets', e.g. NewCall, EndCall, Hold, Resume, Transfer)", value);
		}
		REQ(msg, SoftKeyEventMessage);
		if (!msg) {
			CLI_AMI_RETURN_ERROR(fd, s, m, "Softkey not pressed on %s: out of memory", d->id);
		}
		msg->data.SoftKeyEventMessage.lel_softKeyEvent  = htolel(event);
		msg->data.SoftKeyEventMessage.lel_lineInstance  = htolel(lineInstance);
		msg->data.SoftKeyEventMessage.lel_callReference = htolel(callid);
		sccp_cli_inject(d, msg);
		CLI_AMI_RETURN_DONE(fd, s, m, "Softkey %s pressed on %s", label2str(softkeysmap[event - 1]), d->id);
	}
	if (sccp_strcaseequals(what, "digits")) {
		if (argc != 5) {
			return RESULT_SHOWUSAGE;
		}
		for (const char * p = value; *p; p++) {
			if (!isdigit((unsigned char)*p) && *p != '*' && *p != '#' && *p != '+') {
				CLI_AMI_RETURN_ERROR(fd, s, m, "Digits not pressed: '%c' is not a keypad key (0-9, *, #, +)", *p);
			}
		}
		for (const char * p = value; *p; p++) {
			REQ(msg, KeypadButtonMessage);
			if (!msg) {
				CLI_AMI_RETURN_ERROR(fd, s, m, "Digits not pressed on %s: out of memory", d->id);
			}
			uint32_t key = *p == '*' ? 14 : *p == '#' ? 15 : *p == '+' ? 16 : (uint32_t)(*p - '0');
			msg->data.KeypadButtonMessage.lel_kpButton      = htolel(key);
			msg->data.KeypadButtonMessage.lel_lineInstance  = htolel(lineInstance);
			msg->data.KeypadButtonMessage.lel_callReference = htolel(callid);
			sccp_cli_inject(d, msg);
			if (!callid) {
				/* the first digit may have opened a call; the rest go to it */
				AUTO_RELEASE(sccp_channel_t, c, sccp_device_getActiveChannel(d));
				if (c && c->line) {
					lineInstance = sccp_device_find_index_for_line(d, c->line->name);
					callid       = c->callid;
				}
			}
		}
		CLI_AMI_RETURN_DONE(fd, s, m, "Digits %s pressed on %s", value, d->id);
	}
	return RESULT_SHOWUSAGE;
}

static char cli_push_url_usage[] = "Usage: sccp push url <device> <url>\n"
				   "       Make the phone open url (a Cisco XML service or page). The phone accepts\n"
				   "       it only if its authentication URL allows pushes.\n";
static char cli_press_usage[] = "Usage: sccp press <device> softkey <name>\n"
				"       sccp press <device> digits <digits>\n"
				"       sccp press <device> offhook|onhook\n"
				"       Act as if the key was pressed on the phone; softkeys and digits apply to\n"
				"       the phone's active call. Softkey names are those in 'sccp show softkey sets'.\n";

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#define CLI_COMPLETE SCCP_CLI_CONNECTED_DEVICE_COMPLETER
#define CLI_COMMAND "sccp", "push", "url"
CLI_AMI_ENTRY(push_url, sccp_push_url, "Make a phone open a URL", cli_push_url_usage, FALSE, FALSE)
SCCP_AMI_ACTION(push_url, sccp_push_url, "SCCPPushURL", FALSE, "sccp", "push", "url", "$Device", "$URL")
#undef CLI_COMMAND
#define CLI_COMMAND "sccp", "press"
CLI_AMI_ENTRY(press, sccp_press, "Press a key on a phone", cli_press_usage, FALSE, FALSE)
SCCP_AMI_ACTION(press, sccp_press, "SCCPPress", FALSE, "sccp", "press", "$Device", "$Key", "$Value")
#undef CLI_COMMAND
#undef CLI_COMPLETE
#endif														/* DOXYGEN_SHOULD_SKIP_THIS */

/* sccp reset|restart|unregister <device>, sccp apply config <device>, sccp refresh device <device>, sccp token ack <device> */
static int sccp_device_control(int fd, sccp_cli_totals_t * totals, struct mansession * s, const struct message * m, int argc, char * argv[])
{
	int word_count = (sccp_strcaseequals(argv[1], "apply") || sccp_strcaseequals(argv[1], "refresh") || sccp_strcaseequals(argv[1], "token")) ? 3 : 2;
	if (argc != word_count + 1 || !ARG_PRESENT(word_count)) {
		return RESULT_SHOWUSAGE;
	}
	const char * name = argv[word_count];
	AUTO_RELEASE(sccp_device_t, d, sccp_device_find_byid(name, FALSE));
	if (!d) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "Device %s does not exist", name);
	}

	if (sccp_strcaseequals(argv[1], "token")) {
		if (!d->session) {
			CLI_AMI_RETURN_ERROR(fd, s, m, "Device %s is not connected", d->id);
		}
		if (d->status.token != SCCP_TOKEN_STATE_REJ) {
			CLI_AMI_RETURN_ERROR(fd, s, m, "%s has no refused token request to acknowledge", d->id);
		}
		sccp_session_tokenAck(d->session);
		CLI_AMI_RETURN_DONE(fd, s, m, "Token acknowledged for %s; the phone can now register here", d->id);
	}
	if (!d->session || sccp_device_getRegistrationState(d) != SKINNY_DEVICE_RS_OK) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "Device %s is not registered", d->id);
	}
	if (sccp_strcaseequals(argv[1], "refresh")) {
		sccp_handle_soft_key_template_req(d->session, d, NULL);
		sccp_handle_button_template_req(d->session, d, NULL);
		CLI_AMI_RETURN_DONE(fd, s, m, "Button and softkey layout sent to %s", d->id);
	}
	if (sccp_strcaseequals(argv[1], "unregister")) {
		sccp_msg_t * msg = NULL;
		REQ(msg, RegisterRejectMessage);
		if (!msg) {
			CLI_AMI_RETURN_ERROR(fd, s, m, "Unregister of %s not sent: out of memory", d->id);
		}
		sccp_copy_string(msg->data.RegisterRejectMessage.text, "Unregister user request", StationMaxDisplayTextSize);
		sccp_dev_send(d, msg);
		CLI_AMI_RETURN_DONE(fd, s, m, "Unregister sent to %s; the phone registers again on its own", d->id);
	}

	skinny_resetType_t type = SKINNY_RESETTYPE_RESTART;
	const char * action = "restart";
	if (sccp_strcaseequals(argv[1], "reset")) {
		type = SKINNY_RESETTYPE_RESET;
		action = "reset";
	} else if (sccp_strcaseequals(argv[1], "apply")) {
		type = SKINNY_RESETTYPE_APPLYCONFIG;
		action = "apply config";
	}
	if (d->active_channel) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "Device %s not %s: it has an active call", d->id, sccp_strcaseequals(action, "reset") ? "reset" : sccp_strcaseequals(action, "restart") ? "restarted" : "told to apply its config");
	}
	if (sccp_device_sendReset(d, type) <= 0 && type != SKINNY_RESETTYPE_APPLYCONFIG) {
		sccp_session_stopthread(d->session, SKINNY_DEVICE_RS_NONE);
	}
	CLI_AMI_RETURN_DONE(fd, s, m, "%s sent to %s", type == SKINNY_RESETTYPE_RESET ? "Reset" : type == SKINNY_RESETTYPE_RESTART ? "Restart" : "Apply config", d->id);
}

static char cli_reset_usage[] =
	"Usage: sccp reset <device>\n"
	"       Hard-reset the phone: it reboots and reloads its firmware and configuration.\n";
static char cli_restart_usage[] =
	"Usage: sccp restart <device>\n"
	"       Restart the phone's SCCP registration and reload its configuration\n"
	"       without rebooting.\n";
static char cli_apply_config_usage[] =
	"Usage: sccp apply config <device>\n"
	"       Tell the phone to download and apply its configuration file from TFTP.\n";
static char cli_unregister_usage[] =
	"Usage: sccp unregister <device>\n"
	"       Ask the phone to unregister; it registers again on its own.\n";
static char cli_refresh_device_usage[] =
	"Usage: sccp refresh device <device>\n"
	"       Send the button and softkey layout to the phone again.\n";
static char cli_token_ack_usage[] =
	"Usage: sccp token ack <device>\n"
	"       Acknowledge a token request this server refused (see the fallback setting),\n"
	"       letting the phone register here. Used when servers are clustered.\n";

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#	define CLI_COMPLETE SCCP_CLI_CONNECTED_DEVICE_COMPLETER
#define CLI_COMMAND "sccp", "reset"
CLI_AMI_ENTRY(reset, sccp_device_control, "Reset a phone", cli_reset_usage, FALSE, FALSE)
SCCP_AMI_ACTION(reset, sccp_device_control, "SCCPReset", FALSE, "sccp", "reset", "$Device")
#	undef CLI_COMMAND
#define CLI_COMMAND "sccp", "restart"
CLI_AMI_ENTRY(restart, sccp_device_control, "Restart a phone", cli_restart_usage, FALSE, FALSE)
SCCP_AMI_ACTION(restart, sccp_device_control, "SCCPRestart", FALSE, "sccp", "restart", "$Device")
#	undef CLI_COMMAND
#define CLI_COMMAND "sccp", "apply", "config"
CLI_AMI_ENTRY(apply_config, sccp_device_control, "Make a phone reload its configuration", cli_apply_config_usage, FALSE, FALSE)
SCCP_AMI_ACTION(apply_config, sccp_device_control, "SCCPApplyConfig", FALSE, "sccp", "apply", "config", "$Device")
#	undef CLI_COMMAND
#define CLI_COMMAND "sccp", "unregister"
CLI_AMI_ENTRY(unregister, sccp_device_control, "Unregister a phone", cli_unregister_usage, FALSE, FALSE)
SCCP_AMI_ACTION(unregister, sccp_device_control, "SCCPUnregister", FALSE, "sccp", "unregister", "$Device")
#	undef CLI_COMMAND
#define CLI_COMMAND "sccp", "refresh", "device"
CLI_AMI_ENTRY(refresh_device, sccp_device_control, "Resend a phone's button layout", cli_refresh_device_usage, FALSE, FALSE)
SCCP_AMI_ACTION(refresh_device, sccp_device_control, "SCCPRefreshDevice", FALSE, "sccp", "refresh", "device", "$Device")
#	undef CLI_COMMAND
#define CLI_COMMAND "sccp", "token", "ack"
CLI_AMI_ENTRY(token_ack, sccp_device_control, "Acknowledge a phone's token request", cli_token_ack_usage, FALSE, FALSE)
SCCP_AMI_ACTION(token_ack, sccp_device_control, "SCCPTokenAck", FALSE, "sccp", "token", "ack", "$Device")
#	undef CLI_COMMAND
#	undef CLI_COMPLETE
#endif

/* -------------------------------------------------------------------------------------------------------------- CALLS - */
/* sccp call <device> [number [line]] */
static int sccp_start_call(int fd, sccp_cli_totals_t * totals, struct mansession * s, const struct message * m, int argc, char * argv[])
{
	if (!ARG_PRESENT(2) || argc > 5) {
		return RESULT_SHOWUSAGE;
	}
	AUTO_RELEASE(sccp_device_t, d, sccp_device_find_byid(argv[2], FALSE));
	if (!d) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "Device %s does not exist", argv[2]);
	}
	if (!d->session) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "Device %s is not registered", d->id);
	}
	AUTO_RELEASE(sccp_line_t, line, NULL);
	if (ARG_PRESENT(4)) {
		line = sccp_line_find_byname(argv[4], FALSE) /*ref_replace*/;
		if (!line) {
			CLI_AMI_RETURN_ERROR(fd, s, m, "Line %s does not exist", argv[4]);
		}
	} else if (d->defaultLineInstance > 0) {
		line = sccp_line_find_byid(d, d->defaultLineInstance) /*ref_replace*/;
	} else {
		line = sccp_dev_getActiveLine(d) /*ref_replace*/;
	}
	if (!line) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "Device %s has no line to call from", d->id);
	}
	const char * number = ARG_PRESENT(3) ? argv[3] : NULL;
	AUTO_RELEASE(sccp_channel_t, channel, sccp_channel_newcall(line, d, number, SKINNY_CALLTYPE_OUTBOUND, NULL, NULL));
	if (!channel) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "Call not started on %s (see the log)", d->id);
	}
	if (number) {
		CLI_AMI_RETURN_DONE(fd, s, m, "Call %s from %s line %s to %s started", channel->designator, d->id, line->name, number);
	}
	CLI_AMI_RETURN_DONE(fd, s, m, "%s taken off hook on line %s (call %s)", d->id, line->name, channel->designator);
}

static char cli_call_usage[] =
	"Usage: sccp call <device> [number [line]]\n"
	"       Start an outgoing call from the phone, as if the user dialed.\n"
	"       Without a number the phone goes off hook. Without a line the\n"
	"       phone's default (or active) line is used.\n";

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#define CLI_COMMAND "sccp", "call"
#	define CLI_COMPLETE SCCP_CLI_CONNECTED_DEVICE_COMPLETER
CLI_AMI_ENTRY(call, sccp_start_call, "Start a call from a phone", cli_call_usage, FALSE, FALSE)
SCCP_AMI_ACTION(call, sccp_start_call, "SCCPCall", FALSE, "sccp", "call", "$Device", "$Number", "$Line")
#	undef CLI_COMPLETE
#	undef CLI_COMMAND
#endif

/* sccp answer <call> [device] */
static int sccp_answercall(int fd, sccp_cli_totals_t * totals, struct mansession * s, const struct message * m, int argc, char * argv[])
{
	if (!ARG_PRESENT(2) || argc > 4) {
		return RESULT_SHOWUSAGE;
	}
	AUTO_RELEASE(sccp_channel_t, c, sccp_cli_find_call(argv[2]));
	if (!c || !c->line) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "Call %s does not exist", argv[2]);
	}
	if (c->state != SCCP_CHANNELSTATE_RINGING) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "Call %s is not ringing (state %s)", c->designator, sccp_channelstate2str(c->state));
	}
	AUTO_RELEASE(sccp_device_t, d, NULL);
	if (ARG_PRESENT(3)) {
		d = sccp_device_find_byid(argv[3], FALSE) /*ref_replace*/;
		if (!d) {
			CLI_AMI_RETURN_ERROR(fd, s, m, "Device %s does not exist", argv[3]);
		}
	} else if (c->line->isShared) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "Call %s rings on a shared line; name the device that should answer it", c->designator);
	} else {
		sccp_linedevice_t * ld = NULL;
		SCCP_LIST_LOCK(&c->line->devices);
		ld = SCCP_LIST_FIRST(&c->line->devices);
		d = ld ? sccp_device_retain(ld->device) : NULL /*ref_replace*/;
		SCCP_LIST_UNLOCK(&c->line->devices);
		if (!d) {
			CLI_AMI_RETURN_ERROR(fd, s, m, "Line %s is not on any registered device", c->line->name);
		}
	}
	sccp_channel_answer(d, c);
	CLI_AMI_RETURN_DONE(fd, s, m, "Call %s answered on %s", c->designator, d->id);
}

static char cli_answer_usage[] =
	"Usage: sccp answer <call> [device]\n"
	"       Answer a ringing incoming call, as if the user picked up. On a\n"
	"       shared line, name the device that answers.\n"
	"       <call> is the ID from 'sccp show channels' or a name like SCCP/2004-00000003.\n";

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#define CLI_COMMAND "sccp", "answer"
#	define CLI_COMPLETE SCCP_CLI_RINGING_CHANNEL_COMPLETER, SCCP_CLI_CONNECTED_DEVICE_COMPLETER
CLI_AMI_ENTRY(answer, sccp_answercall, "Answer a ringing call", cli_answer_usage, FALSE, FALSE)
SCCP_AMI_ACTION(answer, sccp_answercall, "SCCPAnswer", FALSE, "sccp", "answer", "$Call", "$Device")
#	undef CLI_COMPLETE
#	undef CLI_COMMAND
#endif

/* sccp hangup <call> */
static int sccp_hangup_call(int fd, sccp_cli_totals_t * totals, struct mansession * s, const struct message * m, int argc, char * argv[])
{
	if (argc != 3 || !ARG_PRESENT(2)) {
		return RESULT_SHOWUSAGE;
	}
	AUTO_RELEASE(sccp_channel_t, c, sccp_cli_find_call(argv[2]));
	if (!c) {
		CLI_AMI_RETURN_ERROR(fd, s, m, "Call %s does not exist", argv[2]);
	}
	sccp_channel_endcall(c);
	CLI_AMI_RETURN_DONE(fd, s, m, "Call %s is being hung up", c->designator);
}

static char cli_hangup_usage[] =
	"Usage: sccp hangup <call>\n"
	"       Hang up a call, as if the user hung up the phone.\n"
	"       <call> is the ID from 'sccp show channels' or a name like SCCP/2004-00000003.\n";

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#define CLI_COMMAND "sccp", "hangup"
#	define CLI_COMPLETE SCCP_CLI_CHANNEL_COMPLETER
CLI_AMI_ENTRY(hangup, sccp_hangup_call, "Hang up a call", cli_hangup_usage, FALSE, FALSE)
SCCP_AMI_ACTION(hangup, sccp_hangup_call, "SCCPHangup", FALSE, "sccp", "hangup", "$Call")
#	undef CLI_COMPLETE
#	undef CLI_COMMAND
#endif

/* --- Register Cli Entries-------------------------------------------------------------------------------------------- */
/*!
 * \brief Asterisk Cli Entry
 *
 * structure for cli functions including short description.
 *
 * \return Result as struct
 */
static int sccp_show_tones(int fd, int argc, char *argv[])
{
	if (argc != 3) {
		return RESULT_SHOWUSAGE;
	}
	const char *const headers[] = { "Hex", "Name" };
	sccp_cli_table_data_t table = { .headers = headers, .columns = ARRAY_LEN(headers) };
	for (unsigned int tone = 0; tone < SKINNY_TONE_SENTINEL; tone++) {
		if (skinny_tone_exists(tone)) {
			sccp_cli_table_add(&table, "0x%02X", tone);
			sccp_cli_table_add(&table, "%s", skinny_tone2str((skinny_tone_t)tone));
		}
	}
	sccp_cli_table_print(&table, fd, "Tones");
	return RESULT_SUCCESS;
}

static char cli_show_tones_usage[] =  "Usage: sccp show tones\n       List the SCCP tones with their hexadecimal codes, including call progress\n       and DTMF tones.\n";
#define CLI_COMMAND "sccp", "show", "tones"
#define CLI_COMPLETE SCCP_CLI_NULL_COMPLETER
CLI_ENTRY(cli_show_tones, sccp_show_tones, "Show SCCP tone identifiers", cli_show_tones_usage, FALSE)
#undef CLI_COMPLETE
#undef CLI_COMMAND

static struct pbx_cli_entry cli_entries[] = {
	AST_CLI_DEFINE(cli_show_globals, "Show global SCCP settings"),
	AST_CLI_DEFINE(cli_show_devices, "List SCCP devices"),
	AST_CLI_DEFINE(cli_show_device, "Show one SCCP device"),
	AST_CLI_DEFINE(cli_show_firmware, "List phone firmware"),
	AST_CLI_DEFINE(cli_show_lines, "List SCCP lines"),
	AST_CLI_DEFINE(cli_show_line, "Show one SCCP line"),
	AST_CLI_DEFINE(cli_show_channels, "List SCCP calls"),
	AST_CLI_DEFINE(cli_show_sessions, "List phone connections"),
	AST_CLI_DEFINE(cli_show_mwi_subscriptions, "List voicemail (MWI) subscriptions"),
	AST_CLI_DEFINE(cli_show_hint_lineStates, "List line states used by hints"),
	AST_CLI_DEFINE(cli_show_hint_subscriptions, "List hint subscriptions"),
	AST_CLI_DEFINE(cli_show_softkeysets, "List softkey sets"),
	AST_CLI_DEFINE(cli_show_refcount, "List reference-counted objects"),
	AST_CLI_DEFINE(cli_show_tones, "List phone tones"),
	AST_CLI_DEFINE(cli_show_version, "Show the chan_sccp version"),
	AST_CLI_DEFINE(cli_message_all, "Show a message on all phones"),
	AST_CLI_DEFINE(cli_message_device, "Show a message on one phone"),
	AST_CLI_DEFINE(cli_system_message, "Set or clear the system message"),
	AST_CLI_DEFINE(cli_set_object, "Change device, line, call or fallback settings"),
	AST_CLI_DEFINE(cli_add_line, "Add a line to a device"),
	AST_CLI_DEFINE(cli_remove_line, "Remove a line from a device"),
	AST_CLI_DEFINE(cli_call, "Start a call from a phone"),
	AST_CLI_DEFINE(cli_answer, "Answer a ringing call"),
	AST_CLI_DEFINE(cli_hangup, "Hang up a call"),
	AST_CLI_DEFINE(cli_reset, "Reset a phone"),
	AST_CLI_DEFINE(cli_restart, "Restart a phone"),
	AST_CLI_DEFINE(cli_apply_config, "Make a phone reload its configuration"),
	AST_CLI_DEFINE(cli_unregister, "Unregister a phone"),
	AST_CLI_DEFINE(cli_refresh_device, "Resend a phone's button layout"),
	AST_CLI_DEFINE(cli_token_ack, "Acknowledge a phone's token request"),
	AST_CLI_DEFINE(cli_push_url, "Make a phone open a URL"),
	AST_CLI_DEFINE(cli_press, "Press a key on a phone"),
	AST_CLI_DEFINE(cli_do_debug, "Show or change SCCP debug logging"),
	AST_CLI_DEFINE(cli_reload, "Reload sccp.conf"),
	AST_CLI_DEFINE(cli_reload_file, "Reload SCCP from another file"),
	AST_CLI_DEFINE(cli_reload_force, "Reload sccp.conf even if unchanged"),
	AST_CLI_DEFINE(cli_reload_device, "Reload one device from sccp.conf"),
	AST_CLI_DEFINE(cli_reload_line, "Reload one line from sccp.conf"),
	AST_CLI_DEFINE(cli_config_generate, "Write an example sccp.conf"),
	AST_CLI_DEFINE(cli_generate_cnf, "Write a phone's TFTP configuration"),
#ifdef CS_SCCP_CONFERENCE
	AST_CLI_DEFINE(cli_show_conferences, "List SCCP conferences"),
	AST_CLI_DEFINE(cli_show_conference, "Show one SCCP conference"),
	AST_CLI_DEFINE(cli_conference_command, "Control a conference participant"),
#endif
};

/* AMI actions; descriptions come from the XML documentation (chan_sccp-en_US.xml) */
#define _MAN_SHOW   (EVENT_FLAG_SYSTEM | EVENT_FLAG_REPORTING)
#define _MAN_CALL   (EVENT_FLAG_CALL)
#define _MAN_SYSTEM (EVENT_FLAG_SYSTEM)
#define _MAN_CONFIG (EVENT_FLAG_SYSTEM | EVENT_FLAG_CONFIG)
static const struct {
	const char * action;
	int authority;
	int (*func)(struct mansession * s, const struct message * m);
} ami_actions[] = {
	{ "SCCPShowGlobals", _MAN_SHOW, manager_show_globals },
	{ "SCCPShowDevices", _MAN_SHOW, manager_show_devices },
	{ "SCCPShowDevice", _MAN_SHOW, manager_show_device },
	{ "SCCPShowDeviceCalls", _MAN_SHOW, manager_show_device_calls },
	{ "SCCPShowFirmware", _MAN_SHOW, manager_show_firmware },
	{ "SCCPShowLines", _MAN_SHOW, manager_show_lines },
	{ "SCCPShowLine", _MAN_SHOW, manager_show_line },
	{ "SCCPShowChannels", _MAN_SHOW, manager_show_channels },
	{ "SCCPShowSessions", _MAN_SHOW, manager_show_sessions },
	{ "SCCPShowMWISubscriptions", _MAN_SHOW, manager_show_mwi_subscriptions },
	{ "SCCPShowHintLineStates", _MAN_SHOW, manager_show_hint_lineStates },
	{ "SCCPShowHintSubscriptions", _MAN_SHOW, manager_show_hint_subscriptions },
	{ "SCCPShowSoftkeySets", _MAN_SHOW, manager_show_softkeysets },
	{ "SCCPShowReferences", _MAN_SHOW, manager_show_refcount },
	{ "SCCPMessageAll", _MAN_SYSTEM, manager_message_all },
	{ "SCCPMessageDevice", _MAN_SYSTEM, manager_message_device },
	{ "SCCPSystemMessage", _MAN_SYSTEM, manager_system_message },
	{ "SCCPSetDeviceDND", _MAN_SYSTEM, manager_set_device_dnd },
	{ "SCCPSetDeviceMicrophone", _MAN_CALL, manager_set_device_microphone },
	{ "SCCPSetDeviceOption", _MAN_CONFIG, manager_set_device_option },
	{ "SCCPSetLineForward", _MAN_SYSTEM, manager_set_line_forward },
	{ "SCCPSetFallback", _MAN_CONFIG, manager_set_fallback },
	{ "SCCPAddLine", _MAN_CONFIG, manager_add_line },
	{ "SCCPRemoveLine", _MAN_CONFIG, manager_remove_line },
	{ "SCCPCall", _MAN_CALL, manager_call },
	{ "SCCPAnswer", _MAN_CALL, manager_answer },
	{ "SCCPHangup", _MAN_CALL, manager_hangup },
	{ "SCCPHold", _MAN_CALL, manager_hold },
	{ "SCCPReset", _MAN_SYSTEM, manager_reset },
	{ "SCCPRestart", _MAN_SYSTEM, manager_restart },
	{ "SCCPApplyConfig", _MAN_SYSTEM, manager_apply_config },
	{ "SCCPUnregister", _MAN_SYSTEM, manager_unregister },
	{ "SCCPRefreshDevice", _MAN_SYSTEM, manager_refresh_device },
	{ "SCCPTokenAck", _MAN_SYSTEM, manager_token_ack },
	{ "SCCPPushURL", _MAN_SYSTEM, manager_push_url },
	{ "SCCPGenerateCnf", _MAN_CONFIG, manager_generate_cnf },
	{ "SCCPPress", _MAN_CALL, manager_press },
#ifdef CS_SCCP_CONFERENCE
	{ "SCCPShowConferences", _MAN_SHOW, manager_show_conferences },
	{ "SCCPShowConference", _MAN_SHOW, manager_show_conference },
	{ "SCCPConference", _MAN_CALL, manager_conference_command },
#endif
};
#undef _MAN_SHOW
#undef _MAN_CALL
#undef _MAN_SYSTEM
#undef _MAN_CONFIG

/*!
 * register CLI commands and AMI actions with asterisk
 */
int sccp_register_cli(void)
{
	uint res = 0;

	for (uint i = 0; i < ARRAY_LEN(cli_entries); i++) {
		res |= pbx_cli_register(cli_entries + i);
	}
	for (uint i = 0; i < ARRAY_LEN(ami_actions); i++) {
		res |= iPbx.register_manager(ami_actions[i].action, ami_actions[i].authority, ami_actions[i].func, NULL, NULL);
	}
	return res;
}

/*!
 * unregister CLI commands and AMI actions from asterisk
 */
int sccp_unregister_cli(void)
{
	uint res = 0;

	for (uint i = 0; i < ARRAY_LEN(cli_entries); i++) {
		res |= pbx_cli_unregister(cli_entries + i);
	}
	for (uint i = 0; i < ARRAY_LEN(ami_actions); i++) {
		res |= pbx_manager_unregister(ami_actions[i].action);
	}
	return res;
}

// kate: indent-width 8; replace-tabs off; indent-mode cstyle; auto-insert-doxygen on; line-numbers on; tab-indents on; keep-extra-spaces off; auto-brackets off;
