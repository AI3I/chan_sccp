# CLI reference

Every chan_sccp console command starts with `sccp`. `help sccp <command>` prints
the same usage text shown here. Most commands also have an AMI action with the
same arguments; see [AMI.md](AMI.md).

Arguments:

- `<device>` is the device name from `sccp.conf`, for example `SEP001122334455`.
- `<line>` is the line name (its section name in `sccp.conf`).
- `<call>` is a call ID from `sccp show channels`, or a channel name such as
  `SCCP/2004-00000003`.

Empty values are shown as `(not set)` for settings and `(none)` for lists and
states.

## Status

| Command | What it shows |
|---|---|
| `sccp show version` | The chan_sccp version. |
| `sccp show globals` | Settings from the `[general]` section. |
| `sccp show devices [registered \| unregistered \| model <text> \| line <line> \| firmware <text>]` | Devices, with a registration summary. `model` and `firmware` match part of the name. |
| `sccp show device <device>` | One device: settings, state, buttons, addons and connection statistics. |
| `sccp show device <device> calls` | The quality the phone reported for its last 20 calls: packets, loss, jitter, latency, MOS and concealment. |
| `sccp show firmware` | Each model's reported firmware and the devices running it. |
| `sccp show lines` | Lines with their devices and current calls. |
| `sccp show line <line>` | One line: settings, attached devices, mailboxes and variables. The PIN is masked. |
| `sccp show channels` | SCCP calls. A held call that is not attached to a device shows device `(none)`. |
| `sccp show sessions [all]` | Phone connections with a registered device; `all` also lists the others. |
| `sccp show mwi subscriptions` | Voicemail (MWI) mailbox subscriptions. |
| `sccp show hint line states` | The line states SCCP reports to Asterisk hints. |
| `sccp show hint subscriptions` | Phone buttons (BLF speeddials) subscribed to hints. |
| `sccp show softkey sets` | Softkey sets and their keys for each call state. |
| `sccp show tones` | SCCP tones and their hexadecimal codes. |
| `sccp show references [show \| suppress]` | Reference-counted objects, for debugging leaks. `show` adds an in-use column; `suppress` also hides objects in use. |
| `sccp show conferences` | Running SCCP conferences (conference support only). |
| `sccp show conference <conference>` | A conference and its participants. |

## Messages

| Command | Effect |
|---|---|
| `sccp message all <text> [beep] [timeout]` | Show a message on every registered phone for `timeout` seconds (default 10). `beep` also plays a short tone. |
| `sccp message device <device> <text> [beep] [timeout]` | The same, on one phone. |
| `sccp system message [<text> [beep] [timeout]]` | Set the message every phone shows, including phones that register later. With `timeout` 0 (the default) it is the idle message; 1-255 shows it as a notification for that many seconds. Without text, clear it. |

## Settings at runtime

None of these are saved to `sccp.conf`; a reload replaces them.

| Command | Effect |
|---|---|
| `sccp set device <device> dnd <off\|reject\|silent>` | Do-not-disturb. |
| `sccp set device <device> microphone <on\|off>` | Mute or unmute the device's active call. |
| `sccp set device <device> debug <on\|off>` | Limit debug output to marked devices (see [Debugging](#debugging)). |
| `sccp set device <device> ringtone <url>` | Change the ring tone. |
| `sccp set device <device> backgroundimage <url> [thumbnail-url]` | Change the background image. |
| `sccp set device <device> <option> <value>` | Change any `sccp.conf` device option on the running device. Unknown and obsolete options, and options that take several entries, are refused. |
| `sccp set line <line> [device] forward <all\|busy\|noanswer> [number]` | Set call forwarding; without a number, clear that type. Without a device, apply it to every device with the line. |
| `sccp set line <line> [device] forward none` | Clear all forwards. |
| `sccp set channel <call> hold <on\|off> [device]` | Hold or resume a call. `off` resumes on the given device, or on the call's own device. |
| `sccp set channel <call> park` | Park a call. |
| `sccp set fallback <true\|false\|odd\|even\|/path/to/script>` | Change the token fallback policy. |
| `sccp add line <device> <line>` | Add a line to a device. |
| `sccp remove line <device> <line>` | Remove a line from a device. |

## Calls

| Command | Effect |
|---|---|
| `sccp call <device> [number [line]]` | Start an outgoing call as if the user dialed. Without a number the phone goes off hook; without a line, the phone's default (or active) line is used. |
| `sccp answer <call> [device]` | Answer a ringing call, on the given device if the line is shared. |
| `sccp hangup <call>` | Hang up a call. |
| `sccp press <device> softkey <name>` | Act as if a softkey was pressed on the active call. Names are those in `sccp show softkey sets`. |
| `sccp press <device> digits <digits>` | Dial digits on the active call. |
| `sccp press <device> offhook\|onhook` | Lift or replace the handset. |
| `sccp conference <EndConf\|Kick\|Mute\|Invite\|Moderate> <conference> [participant]` | End a conference, or act on a participant. Every action except `EndConf` needs a participant. `Mute` and `Moderate` toggle. |

## Phone control

`reset`, `restart` and `apply config` are refused while the phone has an
active call.

| Command | Effect |
|---|---|
| `sccp reset <device>` | Reboot the phone; it reloads its firmware and configuration. |
| `sccp restart <device>` | Restart the phone's registration and reload its configuration without rebooting. |
| `sccp apply config <device>` | Make the phone download and apply its configuration file from TFTP. |
| `sccp unregister <device>` | Ask the phone to unregister; it registers again on its own. |
| `sccp refresh device <device>` | Resend the button and softkey layout. |
| `sccp token ack <device>` | Accept a phone that is waiting for a token (fallback mode). |
| `sccp push url <device> <url>` | Make the phone open a URL (a Cisco XML service or page). The phone accepts it only if its authentication URL allows pushes. |

## Configuration

| Command | Effect |
|---|---|
| `sccp reload` | Reload `sccp.conf` if it changed. Devices whose settings changed restart; a device on a call restarts when the call ends. If the new file cannot be loaded, the current configuration stays. |
| `sccp reload force` | Reload even if the file did not change. |
| `sccp reload file <file>` | Load a different file. |
| `sccp reload device <device>` / `sccp reload line <line>` | Reload one section. |
| `sccp config generate [file [wiki]]` | Write every option with its default to `file` (default `sccp.conf.new`). A relative name goes in the Asterisk configuration directory. Existing files are not overwritten. `wiki` writes a wiki page instead. |
| `sccp generate cnf <device> [file [server-address]]` | Write the phone's TFTP file (`<device>.cnf.xml`) from `sccp.conf`: server address and port, date format, firmware (`imageversion`), TOS and locale. `file` may be a directory; a relative name goes in the Asterisk configuration directory. Existing files are not overwritten. The server address defaults to the one the phone is registered to, then `bindaddr`, then `externip`. |

## Debugging

`sccp debug` without arguments shows the current categories.

```
sccp debug [0|off|none|all|<mask>|[no] <categories>]
```

Add categories by name; prefix `no` to remove them. Separate names with spaces
or commas. `0`, `off` or `none` disables debug output.

Categories: core, hint, rtp, device, line, action, channel, config, feature,
feature_button, softkey, indicate, pbx, socket, mwi, event, conference,
buttontemplate, speeddial, codec, realtime, callinfo, refcount, message,
parkinglot, webservice, threadpool, newcode, filelinefunc, high.

To follow one phone on a busy system, mark it and enable the categories you
need:

```
sccp set device SEP001122334455 debug on
sccp debug device channel
```

While any device is marked, only debug lines that name a marked device, or a
call on one of its lines, are printed. Up to 32 devices can be marked.
`sccp set device <device> debug off` on the last marked device returns to
normal output.

Debug output needs a build without `--disable-debug`.
