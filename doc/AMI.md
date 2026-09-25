# AMI reference

chan_sccp registers the actions below with the Asterisk Manager Interface.
`manager show command <action>` prints the full description from the running
module. Each action runs the CLI command of the same name, so arguments and
results match [CLI.md](CLI.md).

## Responses

Actions that change something answer with one message:

```
Response: Success
Message: Reset sent to SEP001122334455
```

Errors use `Response: Error` and a `Message` saying what failed, for example
`Device SEP001122334455 does not exist`. A missing required header gives
`Missing or invalid arguments; see 'manager show command <action>'`.

List actions (the `Show` actions) answer `Response: Success` with
`EventList: start`, send one event per item, and end with
`<Action>Complete`:

```
Response: Success
EventList: start
Message: SCCPShowDevices list will follow

Event: TableStart
TableName: Devices

Event: SCCPDeviceEntry
ChannelType: SCCP
ChannelObjectType: Device
MACAddress: SEP001122334455
...

Event: TableEnd
TableName: Devices
TableEntries: 1

Event: SCCPShowDevicesComplete
EventList: Complete
ListItems: 3
ListTableItems: 1
```

`ListItems` counts the events sent before the Complete event, and
`ListTableItems` counts the tables. Tabular data comes as `TableStart`, one
`SCCP<Type>Entry` event per row, and `TableEnd` with `TableEntries`. Add
`TableFormatVersion: 2` to the action to receive `<Type>_Entry` event names
instead. Every event carries the request's `ActionID`.

Field names follow the labels of the console output, without spaces (for
example `IPAddress`, `ConfigFile`, `DNDFeatureEnabled`).

## Permissions

The manager user needs one of the listed write permissions.

| Group | Write permission |
|---|---|
| Show actions | `system` or `reporting` |
| `SCCPConfigMetadata` | `system` or `config` |
| Call control | `call` |
| Phone control and messages | `system` |
| Configuration changes | `system` or `config` |

## Status

| Action | Headers | Events |
|---|---|---|
| `SCCPShowGlobals` | | One response with the global settings. |
| `SCCPShowDevices` | `Filter` (registered, unregistered, model, line, firmware), `Value` | `SCCPDeviceEntry` |
| `SCCPShowDevice` | `Device`* | `SCCPShowDevice`, then tables of `SCCPDeviceButtonEntry`, `SCCPDeviceLineEntry`, `SCCPDeviceSpeeddialEntry`, `SCCPDeviceFeatureEntry`, `SCCPDeviceServiceURLEntry`, `SCCPVariableEntry`, `SCCPDeviceStatisticsEntry` |
| `SCCPShowDeviceCalls` | `Device`* | `SCCPDeviceCallEntry` (quality of each of the last 20 calls) |
| `SCCPShowFirmware` | | `SCCPFirmwareEntry` |
| `SCCPShowLines` | | `SCCPLineEntry` |
| `SCCPShowLine` | `Line`* | `SCCPShowLine`, then `SCCPAttachedDeviceEntry`, `SCCPMailboxEntry`, `SCCPVariableEntry` |
| `SCCPShowChannels` | | `SCCPChannelEntry` |
| `SCCPShowSessions` | | `SCCPSessionEntry` |
| `SCCPShowMWISubscriptions` | | `SCCPMailboxSubscriberEntry` |
| `SCCPShowHintLineStates` | | `SCCPHintLineStateEntry` |
| `SCCPShowHintSubscriptions` | | `SCCPHintSubscriptionEntry` |
| `SCCPShowSoftkeySets` | | `SCCPSoftKeySetEntry` |
| `SCCPShowReferences` | | `SCCPReferenceEntry`, `SCCPFactorEntry` |
| `SCCPShowConferences` | | `SCCPConferenceEntry` (conference support only) |
| `SCCPShowConference` | `Conference`* | `SCCPShowConference` (with `ConfId`), then `SCCPParticipantEntry` |
| `SCCPConfigMetadata` | `Segment`, `ResultFormat`, `JSON` | The `sccp.conf` options as JSON: names, types, defaults and, for enums, `PossibleValues`. |

\* required

## Messages

| Action | Headers |
|---|---|
| `SCCPMessageAll` | `Text`*, `Beep`, `Timeout` |
| `SCCPMessageDevice` | `Device`*, `Text`*, `Beep`, `Timeout` |
| `SCCPSystemMessage` | `Text`, `Beep`, `Timeout` (no `Text` clears the message) |

## Settings at runtime

| Action | Headers |
|---|---|
| `SCCPSetDeviceDND` | `Device`*, `State`* (off, reject, silent) |
| `SCCPSetDeviceMicrophone` | `Device`*, `State`* (on, off) |
| `SCCPSetDeviceOption` | `Device`*, `Option`*, `Value`* |
| `SCCPSetLineForward` | `Line`*, `Device`, `Type`* (all, busy, noanswer, none), `Number` |
| `SCCPSetFallback` | `Fallback`* (true, false, odd, even, or a script path) |
| `SCCPAddLine` | `Device`*, `Line`* |
| `SCCPRemoveLine` | `Device`*, `Line`* |

## Calls

| Action | Headers |
|---|---|
| `SCCPCall` | `Device`*, `Number`, `Line` |
| `SCCPAnswer` | `Call`*, `Device` |
| `SCCPHangup` | `Call`* |
| `SCCPHold` | `Call`*, `State`* (on, off), `Device` |
| `SCCPPress` | `Device`*, `Key`* (softkey, digits, offhook, onhook), `Value` (the softkey name or digits) |
| `SCCPConference` | `Command`* (EndConf, Kick, Mute, Invite, Moderate), `Conference`*, `Participant` |

`Call` is a call ID from `SCCPShowChannels` or a channel name.

## Phone control

| Action | Headers |
|---|---|
| `SCCPReset` | `Device`* |
| `SCCPRestart` | `Device`* |
| `SCCPApplyConfig` | `Device`* |
| `SCCPUnregister` | `Device`* |
| `SCCPRefreshDevice` | `Device`* |
| `SCCPTokenAck` | `Device`* |
| `SCCPPushURL` | `Device`*, `URL`* |
| `SCCPGenerateCnf` | `Device`*, `File`, `Server` |

## Events sent by chan_sccp

These are sent without a request. Events marked *callevents* need
`callevents = yes` in `[general]` (the default). All need a build without
`--disable-manager`. The manager user needs read permission `call`, except
`SCCPConfStart` and `SCCPConfEnd` (`user`) and `ChannelUpdate` (`system`).

| Event | When |
|---|---|
| `DeviceStatus` | A device pre-registers, registers or unregisters (`DeviceStatus: PREREGISTERED \| REGISTERED \| UNREGISTERED`). |
| `PeerStatus` | A line is attached to or detached from a device (`PeerStatus: ATTACHED \| DETACHED`). |
| `DND` | Do-not-disturb changes on a device. |
| `CallForward` | A call-forward changes on a line. |
| `CallAnswered` | *callevents*: a phone answers a call. |
| `Hold` | *callevents*: a phone holds (`Status: On`) or resumes (`Status: Off`) a call. |
| `ChannelUpdate` | *callevents*: a call from a phone enters the dialplan (`SCCPDevice`, `SCCPLine`, `SCCPCallID`, `SCCPCallDesignator`). |
| `SCCPConfStart`, `SCCPConfStarted`, `SCCPConfEnd` | *callevents*: a conference is created, starts, ends. |
| `SCCPConfEntered`, `SCCPConfLeft`, `SCCPConfLeave` | *callevents*: a participant joins or leaves. |
| `SCCPConfParticipantKicked`, `SCCPConfParticipantMute`, `SCCPConfParticipantPromotion` | *callevents*: a participant is kicked, muted or unmuted, promoted or demoted. |

Actions renamed in 5.0.0 are listed in [UPGRADING.md](UPGRADING.md).
