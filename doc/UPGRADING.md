# Upgrading from 4.x to 5.0

## Checklist

1. Check the Asterisk version: 5.0 builds against Asterisk 20 through 24 only.
   `configure` refuses older versions.
2. Rebuild from a clean tree. Remove configure flags that no longer exist (see
   [Build](#build)); `configure` warns about unknown flags but continues.
3. Update anything that calls chan_sccp CLI commands or AMI actions: scripts,
   dialplan `System()` calls, AMI clients and dashboards. The old names were
   removed without aliases. The tables below list every rename.
4. AMI clients that read the `Show` actions: check field names (see
   [AMI fields](#ami-fields)).
5. `sccp.conf` needs no changes. Option names and defaults are the same as in
   4.3. Lines with `cid_name` but no `label` now appear on phones (see
   [Behavior](#behavior)).
6. Install the module with Asterisk stopped, or unload, replace and load it.
   Never copy over a loaded `chan_sccp.so`: the running process maps the file
   and crashes. See [DEVELOPMENT.md](DEVELOPMENT.md#installing-on-a-running-pbx).

## CLI commands

| 4.x | 5.0 |
|---|---|
| `sccp applyconfig <dev>` | `sccp apply config <dev>` |
| `sccp tokenack <dev>` | `sccp token ack <dev>` |
| `sccp onhook <call>` | `sccp hangup <call>` |
| `sccp message devices <text>` | `sccp message all <text>` |
| `sccp microphone <dev> on\|off` | `sccp set device <dev> microphone on\|off` |
| `sccp dnd device <dev> <mode>` | `sccp set device <dev> dnd <off\|reject\|silent>` |
| `sccp callforward <line> [dev] <type> [number]` | `sccp set line <line> [dev] forward <type> [number]` |
| `sccp set debug ...` | `sccp debug ...` |
| `sccp set variable` | removed (it was never implemented) |
| `sccp show hint linestates` | `sccp show hint line states` |
| `sccp show softkeysets` | `sccp show softkey sets` |
| `sccp show refcount` | `sccp show references` |

`sccp set channel <call> hold off` now also works without naming a device.

## AMI actions

| 4.x | 5.0 (headers) |
|---|---|
| `SCCPListDevices` | `SCCPShowDevices` |
| `SCCPListLines` | `SCCPShowLines` |
| `SCCPMessageDevices` | `SCCPMessageAll` |
| `SCCPDeviceSetDND`, `SCCPDndDevice` | `SCCPSetDeviceDND` (`Device`, `State`) |
| `SCCPMicrophone` | `SCCPSetDeviceMicrophone` (`Device`, `State`) |
| `SCCPLineForwardUpdate`, `SCCPCallforward` | `SCCPSetLineForward` (`Line`, `Device`, `Type`, `Number`) |
| `SCCPDeviceUpdate`, `SCCPDeviceAddLine` | `SCCPAddLine` / `SCCPRemoveLine` (`Device`, `Line`) |
| `SCCPDeviceRestart` | `SCCPRestart` / `SCCPReset` (`Device`) |
| `SCCPStartCall` | `SCCPCall` (`Device`, `Number`, `Line`) |
| `SCCPAnswerCall`, `SCCPAnswerCall1` | `SCCPAnswer` (`Call`, `Device`) |
| `SCCPHangupCall` | `SCCPHangup` (`Call`) |
| `SCCPHoldCall` | `SCCPHold` (`Call`, `State`, `Device`) |
| `SCCPShowRefcount` | `SCCPShowReferences` |
| `SCCPConfigMetaData` | `SCCPConfigMetadata` |
| `SCCPConference` (`Command`, `ConferenceId`, `ParticipantId`) | `SCCPConference` (`Command`, `Conference`, `Participant`) |

New actions: `SCCPApplyConfig`, `SCCPUnregister`, `SCCPRefreshDevice`,
`SCCPTokenAck`, `SCCPSetDeviceOption`, `SCCPSetFallback`, `SCCPShowFirmware`,
`SCCPShowDeviceCalls`, `SCCPPushURL`, `SCCPPress`, `SCCPGenerateCnf`.

Every action answers with `Response`/`Message`; missing required headers are
reported as an error instead of being ignored.

## AMI fields

- Fields of the `Show` actions follow the console labels, without spaces: for
  example `ConfigFile`, `IPAddress`, `DNDFeatureEnabled`, `Keepalive`.
- `SCCPShowDevices` reports `Firmware` instead of `TypeID`.
- Reference list events are `SCCPReferenceEntry`.
- `SCCPShowLines` entries carry `ActionID` (it was `ActionId`, and was sent even
  when empty), and its `TableEnd` has `TableEntries`.
- `ListItems` in `<Action>Complete` is the number of events sent. 4.x counted
  output lines.
- The `ChannelUpdate` event's fields are `ChannelType`, `SCCPDevice`, `SCCPLine`
  and `SCCPCallID` (4.x: `Channeltype`, `SCCPdevice`, `SCCPline`, `SCCPcallid`).
- `SCCPConfigMetadata` returns valid JSON: `null` for options without a
  default, `PossibleValues` for enumerated options.

## Behavior

- **Graceful shutdown.** During `core stop gracefully` or `core restart
  gracefully`, phones cannot start new calls: the phone shows "Shutting down:
  no new calls" and plays a short tone. Calls in progress, including held
  calls, keep Asterisk running until they end.
- **Line labels.** `label` is optional. Without it the button shows
  `cid_name`, then the line name. In 4.x a line with `cid_name` but no
  `label` was skipped.
- **Reload.** `sccp reload` keeps the running configuration when the new file
  cannot be loaded.
- **Token fallback.** A phone refused a token waits for the backoff period and
  can then retry. In 4.x a phone refused once was refused on every later
  attempt.
- **Phone text.** Prompts and XML screens were rewritten to fit the phone
  display, and caller names in XML screens are escaped. Parked calls are
  listed one entry per call.
- **Console output.** Screens use the Asterisk layout: section titles, aligned
  `Label: value` lines, `(not set)` and `(none)` for empty values.
  `sccp show version` prints `Skinny Client Control Protocol (SCCP) 5.0.0`.
  Log messages were rewritten; filters on old log text need updating.

## Build

- Supported Asterisk: 20 to 24.
- The module is C only; C++ builds were removed.
- Removed configure flags: `--enable-distributed-devicestate`,
  `--enable-backtrace-detail` (libbfd backtraces), `--enable-devdoc` and all
  `--enable-doxygen-*` flags, and the bundled libltdl flags
  (`--with-included-ltdl`, `--with-ltdl-include`, `--with-ltdl-lib`,
  `--enable-ltdl-install`), `--with-libevent` (nothing used it) and
  `--disable-feature-monitor`. `--disable-monitor` now leaves out call
  recording by itself; before, both flags were needed.
- The example `conf/sccp.conf`, which `make install` copies when no
  `sccp.conf` exists, was replaced. The old one let any phone register as a
  guest (`hotline_enabled = yes`) and turned on debug output. An existing
  `sccp.conf` is never overwritten.
- `make check` runs standalone regression tests (it used to print `Success`
  without running anything).
