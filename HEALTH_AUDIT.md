# chan_sccp-modern Health Audit

## Changed — text shown on the phones (2026-09-25)

Reviewed every string sent to a phone screen (status line, priority notify,
XML menus); approved before/after by the user. Status-line fields hold 31
characters on older phones; all new strings fit and are plain ASCII. Strings
built from `\200` label codes are translated by the phone and were left alone.

- Status line: `Call in progress`, `No lines registered`, `Transfer not
  possible`, `No line available`, `Number too long`, `No active call to hold`,
  `No call to send to voicemail`, `No line to send to voicemail`, `More than 2
  calls, use Select` (was "More that two calls"), `Privacy is not enabled`,
  `No call to make private`, and `No call for <softkey>` (was "No Channel to
  perform ANSWER on ! Giving Up", 43 characters, cut on the phone). The call
  forward "no answer" type now uses the phone's own translated label, like
  "all" and "busy" already did.
- Conference: `Moderators cannot be removed`, `Make another moderator first`,
  `You are now a moderator` / `You are no longer a moderator`, `Only SCCP
  phones can moderate` (the old text was 33 characters and got cut).
- XML menus: `Choose a parked call`, `Choose a parking lot` (was "Please Choose
  on of the parking lots"), `Select a participant`, `Invite to conference N`,
  `Number to invite`.
- Hotline caller ID name `Hotline` (was lowercase).

Bugs fixed with it: caller names and numbers were put into the conference
list, parking-lot and parked-calls XML without escaping, so a name like
"Smith & Sons" or "<unknown>" made the phone reject the screen (new
`sccp_xml_escape()`); the parked-calls directory put every call inside one
`<DirectoryEntry>` instead of one entry per call.

Validation: both build configurations without warnings, `make check`, full
`alltests.sh all`; the simulated phone received the new status texts. Rendering
on real phones is part of the post-deploy checks. Not deployed.

## Fixed — line codecs, held-call device, text cut off in buffers (2026-09-25)

- A line's device codec list stayed empty ("(none)" in `sccp show line`):
  the phone sends its capabilities before its button request, so the lines
  were not attached yet when the capabilities were copied, and attaching a line
  recomputed only its preferences. Attaching now also recomputes capabilities.
- Shared-line codec preferences kept only the first codec: two `memcpy`/
  `memset` calls used `sizeof *temp` (one element) instead of the array.
- A held call (detached from its phone) showed "SCCP" as its device in `sccp
  show channels` and in `SCCPChannel(device)`; now "(none)" / empty.
- All `-Wformat-truncation` warnings in `--disable-debug` builds are gone
  (24 before). Real bugs among them:
  - Call names were built in 32 bytes, so a line name over 17 characters lost
    the call ID (`SCCP/<line>-<id>`) and calls on that line got the same name;
    also in the call info. Now sized for the longest line name.
  - The session name ("SEP…:fd") was built in 16 bytes and always lost the
    socket number.
  - Call-forward database keys were written in 60 bytes but read back from a
    100-byte key, so forwards on lines with long names were not restored at
    the next registration.
  - The saved last-dialed number could lose its line instance.
  - MeetMe options (number + options) could be cut before being passed to
    the conference application.
  The rest only cut display text; they now use explicit precision or larger
  buffers (caller ID is still cut to the phone's field sizes, deliberately).

Validation: default and `--disable-debug` builds with no warnings, `make
check` in both, full `alltests.sh all` on wadsworth; checked with a temporary
37-character line name (call name and both database keys complete). Not
deployed.

## Changed — code comments cut to the ones that explain why (2026-09-24)

About 4,900 comments (roughly 8,000 lines) were removed from `src/`:
commented-out code (about 800 lines and 50 blocks), Doxygen boilerplate
(`\param X SCCP Device`, `\return Result as int`, `\callgraph`, briefs that
repeat the function name), struct member comments that repeat the field name,
section banners, editor modelines, `\todo`/`\note`/`\warning`/`\since`/
`\deprecated` tags, dated "added since" markers and comments restating the
next line. Kept, as plain sentences without tags: comments that state a
reason or a constraint (locking and reference-count rules, phone and firmware
quirks, protocol limits, workarounds), value meanings on protocol fields,
`Locks:` notes, `/*ref_replace*/` markers, fall-through and formatter
directives, `#endif` annotations, the packet layouts in `sccp_protocol.h`, the
file headers (GPL notice and author attribution) and the `/*** DOCUMENTATION`
blocks that generate the XML docs. About 25 misspellings in the remaining
comments were fixed.

The removal was scripted (a comment tokenizer that skips strings, with
whole-line deletion) and each step was checked by a build; the four
DOCUMENTATION blocks, which one pass damaged, were restored byte-for-byte from
the previous commit.

Validation: default and `--disable-debug` builds (the latter showed that a
debug-only helper was called outside `#if DEBUG`; fixed), `make check` in
both, XML documentation regenerated (42 AMI actions), full `alltests.sh all`
on wadsworth. Not deployed.

## Changed — second sweep of log and CLI text (2026-09-24)

Every `sccp_log` debug message (about 1,300) was rewritten, not just the
`pbx_log` ones from the first pass: no function-name tags ("(handle_keypad)",
"(%s)" + `__func__`/`__PRETTY_FUNCTION__`), "->"/"=>" arrows, "###",
"Handle X Stimulus", ALL-CAPS state words or made-up compound words;
lower-case facts in the form "<device/call>: <what happened>". A script checked
that every rewrite keeps the same printf conversions in the same order, and
argument order was checked by hand; the compiler checks the types.

All 635 `pbx_log` messages were read again; the leftovers fixed include
"SCCP Handle Message … bytes length", "Unhandled SCCP Message", "You need at
least 2 participant", "Call from … rejected because", the refcount self-test
output and `SS_Memory_Allocation_Error` ("%s: out of memory; operation not
done"). CLI usage texts that still named old commands (`show hint
linestates`, `show softkeysets`, `show refcount`) or used tab indentation
were rewritten, and `sccp reload file` no longer prints its own usage line.

Bugs found in the message arguments:
- The OpenReceiveChannel debug line labelled the codec number "payload"; a
  literal rewrite would have added a conversion with no argument.
- The RTP peer debug line printed the direct-media flag in the "ACL allows"
  slot; the connected-line debug line printed call direction and channel name
  swapped; the session-close line printed a string with %p.
- The `CS_ASTOBJ_REFCOUNT` refcount trace used an undefined debug category,
  a misspelled variable and wrong arguments (that variant did not compile).
- Phone prompts were printed raw in debug output (label codes showed as
  garbage); they are now rendered as "[Hold]" etc.

Validation: wadsworth, full `alltests.sh all` with most debug categories on;
build clean with `-Wall -Wformat=2`; `make check` passes.

## Fixed — graceful shutdown and the four open decisions (2026-09-24)

Graceful shutdown (`core stop|restart gracefully`): Asterisk already waits
until no channels are left, and every SCCP call state that matters has an
Asterisk channel (off hook, dialing, ringing, connected, held — a held call
keeps its channel after it is detached from the phone), so SCCP calls do
hold the shutdown back. What was missing: a new call while the shutdown is
pending failed deep in channel allocation with two ERRORs and two WARNINGs
and no sign on the phone. Now `sccp_channel_getEmptyChannel()` checks
`ast_shutting_down()` first: one NOTICE ("new call on line X refused:
Asterisk is shutting down"), "Shutting down: no new calls" on the phone and
the reject tone; calls Asterisk asks chan_sccp to create during the shutdown
log one NOTICE instead of an ERROR. `core stop|restart when convenient` keeps
accepting calls, as in Asterisk. Tested on wadsworth: held call + `core stop
gracefully` → Asterisk kept running, a new call from the phone was refused
cleanly, `core abort shutdown` cancelled; with the held call ended, chan_sccp
unloaded and Asterisk exited.

Decisions from the message pass, all taken as proposed:
- Token backoff: `registrationTime < time(0) + backoff` was always true, so a
  phone refused once was refused forever; now `time(0) < registrationTime +
  backoff`.
- Hotline: the anonymous-device registration path used
  `GLOB(hotline)->line->name` without the NULL check the token paths have.
- Lines: a line section with cid_name/cid_num but no label was skipped, and
  one with no label at all got a "required option" warning. `label` is now
  optional and falls back to cid_name, then the line name.
- `pbx_channel_unref()` came before `pbx_channel_unlock()` on the same channel
  in resume and both answer paths (unlocking a possibly freed channel); order
  swapped.
- `sccp reload force` no longer prints "Force Reading Config file".

Validation: wadsworth, full `alltests.sh all` clean; label fallback checked
with temporary label-less lines (sccp.conf restored); `make check` passes.
Not deployed.

## Added — provisioning and support commands (2026-09-24)

- `sccp show devices [registered | unregistered | model <text> | line <line> |
  firmware <text>]`, AMI `SCCPShowDevices` Filter/Value: filtered list and a
  "N devices, N registered, N not registered" summary. The Type ID column
  (the model again, as a number) is replaced by the firmware the phone
  reports; unregistered devices show "(not connected)" / "(never)".
- `sccp show firmware`, AMI `SCCPShowFirmware`: firmware per model with the
  device count and names.
- `sccp set device <device> debug on|off` (also AMI `SCCPSetDeviceOption`
  Option: debug): while any device is marked, SCCP debug output is limited to
  messages naming a marked device or a call on one of its lines
  (`SCCP/<line>-`). Turning a device on while debug is off enables core,
  device, line, action, channel, indicate and softkey. `sccp debug` lists the
  marked devices. Implemented in `sccp_log2` (`sccp_debug_log_filtered`), so
  there is no cost while no device is marked.
- `sccp show device <device> calls`, AMI `SCCPShowDeviceCalls`: the phone's
  own end-of-call report (packets, loss, jitter, latency, MOS, minimum MOS,
  concealed seconds) for the last 20 calls, newest first. The statistics
  debug dump in `handle_ConnectionStatistics` is two plain lines now.

- `sccp push url <device> <url>`, AMI `SCCPPushURL`: send a CiscoIPPhoneExecute
  (the phone accepts it only if its authentication URL allows pushes).
- `sccp press <device> softkey <name> | digits <digits> | offhook | onhook`,
  AMI `SCCPPress`: feed the same message the phone would send through the
  normal handler. Keys apply to the active call, or to a held call on one of
  the device's lines (so Resume works; a held call is detached from its
  device).

- `sccp generate cnf <device> [file [server-address]]`, AMI
  `SCCPGenerateCnf`: write `<device>.cnf.xml` for the TFTP server with the
  server address and port (the address the phone registered to, else
  bindaddr, else externip, or given), `dateformat`, `imageversion` as
  loadInformation, TOS values and an English locale when `language` is en.
  sccp.conf has no NTP, time zone or service URL settings, so the file says
  it leaves them out. Lines and buttons come from chan_sccp at registration,
  not from the file. Never overwrites an existing file. Checked for valid XML
  only; not yet loaded by a real phone.

Validation: wadsworth lab with the simulated phone, which now answers
ConnectionStatisticsReq; offhook, digits 700, Hold, Resume and EndCall via
`sccp press` drive a real call through; push URL escapes `&`; every filter, invalid filters (usage / AMI error),
firmware list, debug limited to the marked device during a call (only
non-SCCP Asterisk lines otherwise), history after a call on CLI and AMI.
Build clean with `-Wall -Wformat=2`; `make check` passes. Not deployed.

## Changed — console formatting of the SCCP CLI screens (2026-09-24)

Key/value screens (`sccp show globals`, `show device`, `show line`) now use
the Asterisk layout: short section titles (General, Network, Media, Calls,
...), indented sentence-case "  Label:" lines with values aligned per screen,
and long lists (deny/permit, local networks, codecs) wrapped under the value
column. Empty values read "(not set)" for an option and "(none)" for a list;
AMI gets an empty value. Removed: "---"/"???.???.???.???"/"<not set>"/
"Unset"/"NONE"/"(null)" placeholders, "=>" arrows, the "Softkey Set: default
=> NULL ! ((nil))" pointer dump, the External IP advice text, byte order,
made-up labels (PendingUpdate, BTemplate support, Videosupport?,
linesRegistered, Can CFWDALL, AmaFlags, ParkingLot), the conference "---"
banner and the ANSI-coloured refcount advice. Tables: rows indented under the
title, no dash underline, "(none)" for an empty table, "(not set)" for an
empty cell, two decimals for call-quality figures, sentence-case titles.
AMI keys follow the new labels (e.g. `ConfigFile`, `AudioTOSCOS`).

Bugs found and fixed on the way:
- `show line` printed "Pending Delete" from `pendingUpdate` and "Pending
  Update" from `pendingDelete`; "Adhoc Number Assigned" was always "on" (a
  string passed as a boolean). The line PIN was printed in clear text; now
  only "set" / "(not set)".
- `show globals` dereferenced `GLOB(hotline)->line` without a NULL check,
  and computed the video codec list with the audio array length.
- `sccp reload`: a second reload while one was running cleared the running
  reload's in-progress flag; a missing config file name crashed
  (`pbx_strdupa(NULL)`); a failed listener rebind returned 3, which is not a
  CLI result. Messages now name the file that was (not) reloaded and why.
- `sccp reload device/line` without a name printed its own usage line instead
  of the command's usage.

Seen, not changed: a line's combined device codec list reads "(none)" while
a registered phone is attached (line capabilities are not recomputed on
registration).

Validation: wadsworth lab, full `alltests.sh all` — every CLI command and AMI
action behaves as before, no ERROR/WARNING in the log; `make check` (table
renderer test updated) passes; build clean with `-Wall -Wformat=2`. Not
deployed.

## Changed — CLI and AMI command set renamed and repaired (2026-09-24)

Commands are renamed outright (no aliases). CLI: `sccp show {globals,
devices, device, lines, line, channels, sessions, mwi subscriptions, hint line
states, hint subscriptions, softkey sets, references, tones, version}`,
`sccp message all|device`, `sccp system message`, `sccp set
device|line|channel|fallback`, `sccp add|remove line`, `sccp call`, `sccp
answer`, `sccp hangup`, `sccp reset|restart|unregister`, `sccp apply config`,
`sccp refresh device`, `sccp token ack`. AMI actions are `SCCPShow*`,
`SCCPMessageAll`, `SCCPMessageDevice`, `SCCPSystemMessage`,
`SCCPSetDeviceDND`, `SCCPSetDeviceMicrophone`, `SCCPSetDeviceOption`,
`SCCPSetLineForward`, `SCCPSetFallback`, `SCCPAddLine`, `SCCPRemoveLine`,
`SCCPCall`, `SCCPAnswer`, `SCCPHangup`, `SCCPHold`, `SCCPReset`,
`SCCPRestart`, `SCCPApplyConfig`, `SCCPUnregister`, `SCCPRefreshDevice`,
`SCCPTokenAck` and `SCCPConfigMetadata`, all documented in the XML block in
`sccp_cli.c` (loads with no xmldoc warnings). The duplicate actions in
`sccp_management.c` are gone.

AMI framework bugs fixed (`ast120.h` macros):
- Successful actions sent no response at all; errors could send two.
- The generated handlers built argv in one `static` array shared by every
  concurrent AMI session.
- The fixed CLI words of a command were looked up as AMI headers, so most
  actions got empty arguments; `$Header` placeholders are now explicit.
- Missing arguments were answered with Success; now an Error naming
  `manager show command`.
- As a result `SCCPSystemMessage`, call forward and DND always failed.

Behavior bugs fixed:
- **Rebuilding the button template dropped every line.**
  `sccp_make_button_template()` skips buttons that already have an instance,
  so a second ButtonTemplateReq in one registration (from the phone, or from
  `refresh device`) produced a template and line list with no lines: outgoing
  calls failed with "no line" until the phone re-registered. The handler now
  resends the template built at registration; this also stops leaking the line
  references held by the discarded template.
- `sccp_linedevice_createButtonsArray()` freed the line array when one line
  button had no line attached and then kept writing into it (use after free).
  The slot is now left empty.
- Setting a single device option at runtime went through
  `sccp_config_applyDeviceConfiguration()`, which resets every option not
  given to its default (description, buttons, ...). New
  `sccp_config_setDeviceOption()` sets only that option and rejects unknown,
  obsolete and multi-entry options such as `button`.
- `hold off` resumes on the call's own device when none is named.
- Every CLI/AMI command reports what it did or why it did nothing (unknown
  device/line/call, device not registered, DND feature disabled, line already
  on the device, call not ringing, no refused token, ...).

Also cleaned up:
- `sccp show version` and the module description are now "Skinny Client
  Control Protocol (SCCP) 5.0.0" / "... (SCCP)"; the branch, revision and
  builder only appear in backtraces. The old string ended in a newline and
  overflowed the `module show` column.
- `sccp debug` rejects unknown category names on the CLI (with usage) and
  changes nothing; before, typos were logged as NOTICEs and the rest applied.
- AMI keys made from CLI labels: `sccp_camelcase()` read past the end of
  labels ending in ")" and turned "Keepalive (s)" into "Keepalive(s"; units in
  parentheses are now dropped and the rest keeps its case (`IPAddress`, not
  `Ipaddress`). Refcount entries are `SCCPReferenceEntry`, not
  `SCCPEntryEntry`.
- `SCCPConfigMetadata`: string options were typed `" STRING"`, missing
  defaults printed as `"(null)"` (now JSON null), text was not JSON-escaped,
  the ENUM key had a space ("Possible Values", now `PossibleValues`), and two
  `strsep()` loops freed NULL and leaked their copies. Name is `chan_sccp`;
  the archive branch/revision fields are gone. XML docs rewritten.
- `sccp show channels` logged an ERROR per call on builds without video.
- Two NOTICEs on every call from a channel without an SCCP codec (e.g. a
  Local channel) are now codec debug; the requested format reference leaked.
- `sccp config generate`: absolute paths were put under the config
  directory; the result and reason are printed on the CLI; the generated
  header was missing a newline after the date.

Test harness: the simulated phone now drops its connection after Reset,
Restart and RegisterReject, as a real phone does; `alltests.sh` runs every
CLI command and AMI action including the negative cases.

Validation: wadsworth lab, full `alltests.sh all` run — every command gives
the expected result; calls work after refresh, reset, restart and unregister.
Build clean with `-Wall -Wformat=2`. Not deployed.

## Fixed — found by exercising every CLI command and AMI action (2026-09-24)

Method: a simulated SCCP phone (Cisco 7965, protocol 17) registered against
the wadsworth lab Asterisk 22; every `sccp` CLI command and SCCP AMI action
was run and the messages the phone received were checked. Harness:
`~/asterisk-lab/clitest/` on wadsworth (`testphone.py`, `t`, `ami`).

- **SCCP calls could not be hung up by Asterisk** (`sccp onhook`, `channel
  request hangup`, AMI hangup, dialplan timeouts) unless an RTP packet arrived:
  both `ast_channel_alloc()` calls passed `needqueue = 0`, so the channel had no
  alert pipe. On a system without a timing module nothing could wake the
  channel; calls stayed in the dialplan forever (seen stuck in `Echo()`). With
  `res_timing_timerfd` the timer masked it, which is why production looked
  fine. Now `needqueue = 1`, as every Asterisk channel driver does.
- **One failed `sccp reload file` broke every later reload and could crash
  Asterisk.** `sccp_config_getConfig()` freed `GLOB(config_file_name)` and then
  copied from it when callers passed that same name (`reload device/line`), and
  on any load failure it had already destroyed the working config. `sccp
  reload device` then read a NULL config (segfault in `ast_variable_browse`,
  core in the lab) or marked the device `pendingDelete`, removing it (the phone
  was then rejected as "Device Unknown"). The config is now loaded into a local
  and only replaces the current one when usable; reload device/line refuse to
  run without a loaded config and only delete when the loaded file really lacks
  the section. A device/line created by `reload device/line` is now also added
  to the global list.
- **`sccp add line` put the button at position 256**: `sccp_config_addButton()`
  stored index -1 in a `uint8_t`. Index -1 now appends after the last button.
  The same function returned with the list lock held on allocation failure.
- **AMI SCCPDeviceAddLine added the line button twice** and answered with a bare
  "Done" instead of an AMI response.
- **Regression from the message pass:** an empty `privacy` (its default) was
  reported as invalid on every reload; empty now means off again.
- `sccp reload` printed a raw pointer ("SCCP reloading configuration. 0x7f…").

Validation: wadsworth lab — calls now end on `sccp onhook` (plain and after
hold/resume), `sccp reload file nosuch.conf` followed by `reload device` /
`reload force` keeps the config and the device, `add line`/`remove line` place
the button at position 2. Build clean with `-Wall -Wformat=2`; `make check`
passes. Not deployed.

## Fixed — "internal" ACL range, netmask display, load crash (2026-09-23)

- **`permit = internal` / `localnet = internal` allowed 172.0.0.0/11.** The
  expansion used 172.16.0.0 with mask 255.224.0.0, which masks to
  172.0.0.0–172.31.255.255 and so also permitted the public 172.0–172.15
  block. Now 172.16.0.0/255.240.0.0 (RFC 1918). The sample `conf/sccp.conf`
  documented the wrong range too.
- **Every deny/permit/localnet entry displayed its address as its netmask**
  (`sccp show globals`, `sccp show device`, the refused-connection log, and
  config-change detection). `sccp_netsock_stringify*()` return one shared
  per-thread buffer and `sccp_print_ha()` called it twice in one `printf`.
  Same bug in the `sccp_rtp_print()` output used by `sccp show channel`
  debugging. Both now copy each result; the ACL list reads
  "permit 10.0.0.0/255.0.0.0, …". Found only because the display was wrong:
  the netmask bug hid the 172 range bug.
- **Crash on module load introduced by the message pass:** a debug log added at
  the start of `sccp_prePBXLoad()` read `sccp_globals->debug` before the
  globals were allocated. Found by loading the build into the wadsworth lab
  Asterisk (segfault at address 0x4 in `sccp_prePBXLoad`); fixed before any
  deployment. Other startup/shutdown paths checked; none log that early.
- Missing TLS `certfile` is logged once at NOTICE (the TLS listener is always
  attempted when built with OpenSSL, so a WARNING would fire on every normal
  load).

Validation: wadsworth build clean with `-Wall -Wformat=2`, `make check`
passes, module loads in the lab Asterisk and `sccp show globals` shows
correct netmasks.

## In progress — message-quality pass (started 2026-09-23)

Every always-visible message (`pbx_log` ERROR/WARNING/NOTICE) is being
rewritten file by file, after reading the code around it, to the approved
style: `<device or session>: <what happened>; <what the system did>`, naming
the real `sccp.conf` option, labelling internal misuse "(caller bug)", and no
instructions or links to the dead upstream project. Routine or default events
move to debug `sccp_log`. Order: `sccp_actions.c`, `sccp_config.c`,
`sccp_channel.c`, `ast120.c`, `sccp_device.c`, `sccp_feature.c`,
`sccp_conference.c`, `sccp_pbx.c`, `sccp_session.c`, `sccp_cli.c` and every
remaining file, including stray `ast_log` calls (done for all always-visible
log messages). Still to do: CLI/AMI output text and usage strings, phone
prompts, then debug `sccp_log`. User rule added mid-pass: no made-up compound
words in prose ("call forward", not CallForward); interface names (sccp.conf
options, AMI headers, CLI keywords) keep their spelling, AMI headers written
readably (ForwardType) since Asterisk matches them case-insensitively.

Messages that described the wrong outcome (now corrected): token fallback
failures said nothing about the token being refused; "Unable to schedule
dialing" was a hangup; "Call has already been hungup" was the code ending the
call itself; "active channel from a different device, skipping" did not skip;
the answer-failure log passed NULL to `%s`; "Could not match audio codec,
Falling back to ULAW" actually falls back to G.722; the auto-answer "no
channel" warning seen while paging (the call simply ended during the
auto-answer delay) is now debug-level. Conference CLI commands no longer log
user typos as Asterisk warnings; their CLI errors now show usage/values.

Behavior bugs found and fixed along the way:
- Transfer and Conference buttons had no `return` after acting, so every
  press with an active call also logged "no call" and played the reject tone.
- Boolean options and `privacy`: the invalid-value branch was unreachable
  (`sccp_true(v) … else if (!sccp_true(v))`), so typos silently meant "off".
  Now uses `sccp_false()`; invalid values are reported and ignored.
- `sccp_channel_allocate()` leaked a line reference on its two early returns.
- Config/wiki generators closed the file descriptor twice (`fclose` then
  `close(fd)`) on every run.
- Group pickup turned the GROUPCALLPICKUP lamp on but switched CALLPICKUP
  off, leaving the group pickup lamp flashing.
- `sccp_feat_conflist()` read `c->callid` before checking `c` for NULL.
- Four pickup-unsupported logs had a `%s` with no argument (undefined
  behavior; only compiled without Asterisk pickup support).
- AMI `SCCPLineForwardUpdate` could never enable call forwarding: it tested
  `astman_get_header()` results for NULL (they are "" when missing) and set
  `enabled = sccp_true(Disable)`. Now ForwardType is required unless
  `Disable: yes` (which still clears all forwards), Number is required when
  enabling, and unknown types are rejected instead of reporting success.
- A missing TLS certificate was only logged at debug level, so a configured
  TLS listener could silently fail to start; now a warning.
- CLI handlers logged user typos as Asterisk warnings; they now print the
  error on the CLI instead. The reload path logged each config error three
  times; now once plus "devices and lines keep their current settings".

The four decisions found here were resolved on 2026-09-24 (see above).

Validation so far: wadsworth build with `-Wall -Wformat=2` clean, `make
check` passes. Not deployed.

## Fixed — findings from strict-warning and `-fanalyzer` builds (2026-09-23)

Found by building with `-O2 -Wall -Wextra` plus extra checks and with GCC 14
`-fanalyzer` on wadsworth.

- **Crash on a DevState button without options.** A `feature = devstate`
  button with no custom state name is never registered, so pressing it passed
  NULL into `sccp_devstate_getNextDeviceState()` and dereferenced it. The
  button handler now logs a warning and ignores the press, and the function
  returns `AST_DEVICE_UNKNOWN` when no handler exists.
- **Unescaped XML pushed to phones.** `pushTextMessage()` inserted the message
  text and sender, and `pushURL()` (the `SendURL` application) the URL,
  straight into `CiscoIPPhone*` XML. `<`, `&` or `"` broke the message, and
  text from outside (e.g. SIP MESSAGE) could inject XML elements. Text and
  sender now go through `ast_xml_escape()`. URLs use a local escaper that also
  escapes bare `&`, `<`, `>`, `"` and `'` but keeps `&amp;`-style and numeric
  entities, so dialplans that already wrote `&amp;` keep working. Length limits
  still apply to the unescaped text; the escaped text is heap-allocated.
- `sccp_dev_starttone()` was declared with `uint32_t timeout` but defined and
  called with `skinny_toneDirection_t direction`; the header now matches.
- `sccp_astwrap_doPickup()` compared the pointer returned by `ast_channel_ref()`
  with `> 0` (always true); now takes the reference unconditionally.
- Session thread locals are declared after `pthread_cleanup_push()` so they
  cannot be clobbered by its `setjmp`; the `poll()` error log labelled
  `errno` as the return value and now states that the session is closed.
- Old-style `()` definitions (`sccp_codec_getArrayLen`,
  `sccp_session_terminateAll`) and a misplaced `static` in `sccp_callinfo.c`.

Reviewed and left as false positives: the analyzer's NULL `channel` in
`ast120.c` request (a successful request always sets `channel` and
`channel->owner`), fd leaks in the TCP/TLS bind paths (the fd belongs to the
connection) and in the token script runner (every path closes both pipe ends).

Validation on wadsworth against the lab Asterisk 22 prefix: default and
`--disable-debug` builds pass `make check`; the default build has no warnings
(previously 4); the strict build is clean apart from the `*const` return-type
(`-Wignored-qualifiers`) and `SS_Memory_Allocation_Error`
(`-Wformat-nonliteral`) noise; the URL escaper passed a standalone test of
plain, pre-escaped, numeric, bogus and injected entities plus truncation.
The 25 `-Wformat-truncation` warnings in `--disable-debug` builds remain.
Not deployed; no handset test of text/URL push or DevState buttons.

## Fixed — libbfd backtrace support removed; `#ifdef DEBUG` guards (2026-09-23)

`DEBUG` is always defined by configure (1 or 0), so every `#ifdef DEBUG` was
true, including in `--disable-debug` builds. Removed the `CS_CHECK_BFD`
configure block and `LIBBFD` link flag; `sccp_do_backtrace()` now exists only
when `DEBUG` is 1 and uses execinfo/Asterisk `ast_bt_get_symbols`. Its two
callers in `sccp_refcount.c`, the declaration in `sccp_utils.h`, and the
AMI "ConfigureEnabled" `"debug"` entry in `sccp_config.c` now use
`#if DEBUG`. Behavior change: release builds that had libbfd no longer print
a backtrace on refcount errors.

Generated files (`configure`, five `Makefile.in`, `src/config.h.in`) were
regenerated with `tools/bootstrap.sh` on wadsworth (Autoconf 2.72, Automake
1.17, libtool 2.5.4 Debian-2.5.4-4, matching the committed files); only
non-whitespace changes were kept. The larger `configure` diff is autoconf
dropping helpers (`ac_fn_c_check_type`, etc.) only the bfd checks used.

Validation on wadsworth against the lab Asterisk 22 prefix
(`~/asterisk-lab/bfd-cleanup-build`, `bfd-cleanup-nodebug`): default
(`DEBUG 1`) and `--disable-debug` (`DEBUG 0`) builds both configure, build and
pass `make check`. Default build warnings unchanged from the previous lab
build except the `sccp_do_backtrace()` old-style definition, now fixed.
The `--disable-debug` build also shows ~25 pre-existing `-Wformat-truncation`
warnings (sccp_pbx.c, sccp_cli.c, sccp_utils.c, sccp_hint.c, others) that are
not addressed here. Not deployed to the PBX.

## Fixed — sscanf format and return checks (CodeQL, 2026-09-23)

CodeQL `cpp/incorrectly-checked-scanf` flagged 8 calls that tested
`sscanf()` as a boolean; it returns -1 (true) on empty input. Two were also
wrong formats:

- `sccp_config.c`, 2-byte numeric options: `"%ux"` is decimal followed by a
  literal `x`, so any `0x…` value parsed as **0**. Now `"%x"`.
- `ast.c`, `MaxCallBR` channel option: `"%ud"` (with a trailing literal `d`)
  into a signed `int32_t`. Now `"%d"`.

All eight now require a return of exactly 1. Empty config values still
become `"0"` before parsing, so the numeric path's behavior is unchanged
except for the hex fix. An empty group entry (`1,,3`) used to re-add the
previous group; it is now logged as a syntax error and skipped.

Validation: built and tested on wadsworth together with the libbfd removal
above; not deployed to the PBX.

## RTP transmit payload initialization correction (2026-09-22)

The answer-time format changes described below did **not** fix the live
`Don't know how to send format ulaw packets with RTP` warning. On the PBX's
Asterisk 22.8.2, `res_rtp_asterisk` implements neither `set_write_format` nor
`set_read_format`; the instance wrappers return -1 without initializing
payload mappings. `ast_rtp_write` uses the separate transmit payload table,
while `ast_rtp_codecs_payload_code` allocates receive mappings.

The deployed `chan-sccp-rtpfix2` module matched the local ast116 source and
registered telephone-event but no standard audio transmit mappings. RTP
instance creation now explicitly registers static payloads 0 (PCMU), 3
(GSM), 4 (G723), 8 (PCMA), 9 (G722), and 18 (G729), before activation and
early media. Channel codec selection is unchanged. This covers the standard
static audio codecs; dynamic audio/video mappings need separate validation.

Comparison with `/usr/src/chan-sccp` confirms that the older source explicitly
registered these six payloads. Disassembly of its September 12 module also
shows the six registration calls, confirming this behavior existed in the
compiled artifact, not just the source left on disk.

Validation: rebuilt the ast122 wrapper and module against the PBX's headers
in `/usr/src/chan-sccp-rtp-payload-fix`; build succeeded. With zero active
channels, backed up the previous module to
`/root/chan_sccp_backups/chan_sccp-before-payload-fix.so`, unloaded SCCP,
atomically replaced the module, and loaded it successfully. Installed and
built module SHA-256 hashes match. The user subsequently reported that calls
were working after deployment. Separate paging and broader codec coverage
have not been verified.

## Earlier attempt — RTP format setup for auto-answered/paged calls (2026-09-22)

**Correction:** the original diagnosis below incorrectly attributed payload
initialization to the RTP format setters. See the transmit mapping fix above.

Found live, in production: paging multiple SCCP devices produced repeated
`ast_rtp_write: Don't know how to send format ulaw packets with RTP`
warnings plus `(autoanswer_thread) no channel` on several legs. Root cause:
`sccp_astwrap_answer()` in `ast116.c` (the function Asterisk core calls
whenever *it* answers a channel - exactly what Page()/auto-answer does) had
a commented-out `pbx_indicate(pbxchan, AST_CONTROL_PROGRESS)` with nothing
put in its place. That indication used to be the only thing that triggered
`pbx_retrieve_remote_capabilities()`, which is the only code that calls
`ast_rtp_instance_set_write_format()`/`set_read_format()` on the audio RTP
instance. `AST_CONTROL_PROGRESS` is sent by the far end; Page()-style
auto-answered legs typically never receive it, so those calls' RTP
instances never got their payload-type table configured at all. Fixed by
calling `pbx_retrieve_remote_capabilities()` directly at answer time.
Also removed two leftover `pbx_log(LOG_NOTICE, "parsing aa"/"set aa to
2w")` debug statements in `sccp_parse_dial_options` (ast.c) that fired
once per paged device at always-visible NOTICE level - the log spam that
surfaced this in the first place. Deployed to production (commit
`15bb25ce`); Asterisk core stayed up through the module swap (used atomic
temp-file + rename, not in-place overwrite - see the deployment incident
noted below for why that matters).

**Not yet done**: this fix was deployed same-session under real time
pressure. It hasn't been proven via an actual live page/call test yet
(only build + module-load verified) - a full page test with a device
that has actually re-registered should happen next session before
calling this fully closed.

## Incident — in-place `cp` over a loaded module crashed Asterisk (2026-09-22)

While deploying the first build of tonight's work, used `cp` to overwrite
`/usr/lib/asterisk/modules/chan_sccp.so` while the *old* module was still
loaded and memory-mapped by the running Asterisk process. `cp` writes into
the existing file's inode in place rather than atomically replacing it, so
it corrupted the running process's own code pages mid-flight; Asterisk
segfaulted a few seconds later (confirmed in `dmesg`). ~2 minutes of full
outage before `systemctl restart asterisk` recovered it. The file on disk
was fine the whole time (the corruption was only in the already-running
process's memory), so a fresh process start loaded correctly.
**Lesson, now applied for the rest of this session**: any module swap on a
running Asterisk must be unload → replace file via temp+atomic-rename →
load, never `cp` directly over a file a live process may have mapped.

Running log of the cleanup effort started 2026-09-22. Not a formal issue tracker —
just a durable record of what's been found and fixed, so work can resume across
sessions without re-deriving everything. Append to this as new issues are found;
move items from "Open" to "Fixed" with the commit/branch that fixed them.

**Note (2026-09-22, later)**: the user has decided this project's real home going
forward is the public `AI3I/chan_sccp` repo, not this private `chan_sccp-modern`
one (which was only ever a parking/staging area). Plan: push this repo's full
history into `chan_sccp`'s main/master branch, then delete the `chan_sccp-modern`
GitHub repo and its local `~/GitHub/chan_sccp-modern` clone. Not done yet -
pending a natural stopping point in the current cleanup pass. Also: AMI is
confirmed disabled on the production PBX (`manager.conf: enabled=no`, no login
history ever) and SCCP+AMI is a dead combination in practice, so field/identifier
naming in CLI tables no longer needs to preserve any legacy AMI-facing name -
clean them up freely, just don't break compilation or Asterisk's own behavior.

## Fixed — config parameter documentation (`sccp_config_entries.hh`) (2026-09-22)

This file (not the `conf/*.annotated`/`conf/*.conf` files, which are just
generated output - editing those directly would be overwritten) is the real
source of every parameter description shown in the annotated sample configs.
Found and fixed real problems, not just polish:

- **A real functional bug in `autoconf/extra.m4`**: `--enable-park`/
  `--disable-park` had a misplaced m4 quote bracket (`[ac_cv_use_park=$enableva]l,`
  - note "enableva" + stray "l,"), splitting `$enableval` and very likely
  breaking that configure flag. Fixed to `[ac_cv_use_park=$enableval],`.
- **`park`'s own doc was factually wrong** - claimed "not compiled by default";
  the real configure default is enabled. Rewritten to state what it does and
  the correct default.
- **`monitor`'s description was completely empty** (`""`) - the worst case of
  "poorly documented." Traced what it actually does (toggles Asterisk's automon
  call recording) and wrote a real description.
- **Two unbalanced-parenthesis bugs** (3 occurrences) in `pickupgroup`/
  `namedpickupgroup` descriptions - an opened paren that was never closed,
  confusing to read. Also expanded the cryptic internal `(ast111)` shorthand
  (this codebase's own `ASTERISK_VER_GROUP` jargon) to "Requires Asterisk 1.11
  or later" - not something an end user reading sccp.conf should have to
  decode.
- **~15 grammar/typo fixes**: "for for", "seperate", "direcrtp" (a parameter
  described using a misspelling of its own name), duplicated "Options:
  Options:", "will we added"→"will be added" (2x, same bug in two sibling
  entries), duplicated periods, "datebasetable"→"database table" (2x),
  "beused"→"be used", "to registered"→"to register", "whe dialing"→"used when
  dialing", "line if not"→"line is not" (changed actual meaning), "a ip
  address"→"an IP address" (2x), "will be use."→"will be used.", three
  missing-space string-concatenation gaps, "to work correct"→"to work
  correctly", "replaced by in favor of"→"in favor of".
- **Content quality, not just correctness** (per explicit direction: real
  errors not filler, real values not deprecated cruft): `earlyrtp`'s
  description used to enumerate 6 specific "deprecated option" values
  (none/offhook/immediate/dial/ringout/progress) as if they were deliberately
  supported aliases - checked the actual parser (`sccp_config_parse_earlyrtp`
  in `sccp_config.c`) and confirmed only `"none"` is special-cased; everything
  else just falls through to "not recognized as false" = true, same as any
  typo would. Rewrote to lead with the two real values and drop the misleading
  list. Replaced all 4 uses of the word "stuff" (`cfwdall`/`cfwdbusy`/
  `cfwdnoanswer` descriptions, general and device sections) with what's
  actually being enabled. Rewrote `callgroup`/`pickupgroup`'s first-person
  template artifacts ("We are in caller groups 1,3,4" - reads like an example
  site's real config asserted as fact, not a parameter description) into
  proper generic descriptions. Fixed "natted" (informal jargon-as-verb) in the
  `externhost` description and in a live `sccp show globals` CLI message
  (`sccp_cli.c`, which also had its own typo, "IP-addres") to plain "behind
  NAT" phrasing; same fix applied to two comments (`sccp_channel.c`,
  `sccp_actions.c`) for consistency, though those aren't user-facing.

## Fixed

All on branch `feature/parkedcalls-webservice`, in `src/sccp_webservice.c` /
`src/sccp_xml.c`, verified by a real build against a `--enable-experimental-xml`
configured Asterisk 22 tree unless noted:

- **OOB array read** — unrecognized `?outformat=` param let `SCCP_XML_OUTPUTFMT_SENTINEL`
  (8) flow through as an index into the 8-element `outputfmt2contenttype[]` array
  (valid indices 0-7). Fixed in `parse_outputfmt()` by validating with the
  generated `sccp_xml_outputfmt_exists()` helper before accepting the parsed value.
- **Real Cisco phones silently got HTML instead of CXML** — `request_parser()` seeds
  `outputfmt` with `HTML` (not `NULL`) before calling `parse_outputfmt()`, so the
  Allegro-Software-WebClient auto-detect branch (which required `*outputfmt ==
  NULL` to fire) was unreachable. Fixed by making phone detection unconditional —
  it should always win over any caller-seeded default.
- **NULL-deref crash on malformed XSLT** — `applyStyleSheetByName()` never checked
  whether `xsltParseStylesheetFile()` succeeded before passing the result to
  `xsltApplyStylesheet()`. A corrupt `.xsl` file crashed the whole Asterisk process
  on an ordinary HTTP GET. Fixed with an explicit NULL check + early return.
- **Dangling pointer / use-after-realloc** — `get_request_handler()` returned a raw
  pointer into the `SCCP_VECTOR_RW`-protected `handlers` array *after* releasing
  the lock; a concurrent `addHandler()`/`removeHandler()` could realloc/shift the
  backing storage out from under the caller. Fixed by copying the (small,
  trivially-copyable) `handler_t` by value while the lock is held, instead of
  returning a pointer. Signature changed: `boolean_t get_request_handler(params,
  handler_t * const out)`.
- **Wrong HTTP status code** — handler-not-found was reported as `500 Server Error`
  (implies a server bug) instead of `404 Not Found` (it's a malformed/unmatched
  client request). Fixed, and the "URI could not be parsed / Not handler found"
  message (broken grammar, conflated two different failure reasons into one vague
  line) replaced with something specific.
- **Raw pointer logging** — `sccp_webservice_callback()` logged `%p, %p` for
  `get_params`/`headers` — meaningless to an operator reading the log. Replaced
  with actual param/header counts.
- **Redundant boilerplate + wrong function name in error text** — several
  `ast_http_error()` calls repeated "Internal Server Error\n" as a literal first
  line of body text (already implied by the HTTP status); one blamed
  `ast_str_create()` for an OOM when the code actually calls `pbx_str_create()`
  (a different wrapper). Cleaned up across `request_parser()`.
- **Vague stylesheet failure messages** — `xmlPostProcess()`'s "Applying
  Stylesheet failed" / "Stylesheet could not be found" logged with a
  `stylesheetFilename` variable sitting right there, unused. Fixed to name the
  actual file and the handler URI.
- **Race condition, the most serious item in the whole audit** —
  `firewall_holepunch` in `sccp_channel.c` was read/written from two different
  threads (SCCP device-indication thread vs. Asterisk's own RTP-read thread, one
  call site in every `pbx_impl/ast11x.c`) with no lock at all. The code had its
  own unresolved comment admitting this (`sccp_indicate.c:283`: *"Do we need to
  lock the channel here... or is locking done there already iirc?"*). Fixed by
  wrapping `startHolePunch()`/`finishHolePunch()`/`holePunchPending()` with the
  channel's own existing `sccp_channel_lock`/`unlock` — confirmed safe to hold
  across the media-transmission calls since `AST_MUTEX_KIND` is
  `PTHREAD_MUTEX_RECURSIVE` on this build (checked `lock.h` directly, didn't
  assume).
- **Build-breaking argument mismatch**: `ast112.c` and `ast114.c` still called
  `sccp_channel_finishHolePunch(c)` with one argument against the current
  two-argument prototype; every other version file (`ast111`/`113`/`115-119`)
  already called it correctly. Fixed to match. Could not compile-test these two
  specific files directly (no Asterisk 12/14 headers installed here), but the
  change is a one-argument mechanical fix matching a pattern proven in 8 other
  files.

## Open — correctness bugs (from the `/code-review high` pass, not yet fixed)

None remaining from that pass — both were fixed above.

## Fixed — Asterisk version compatibility, 20 through 24 (the big one)

**Starting state**: `configure.ac` declared `MAX_ASTERISK_VERSION=113`. The
real upstream (`github.com/chan-sccp/chan-sccp`, `develop` branch) only had
genuine implementations through `ast119` — nothing for 120+ in any commit, on
any branch, anywhere in git history. The production PBX ran Asterisk 22.8.2
successfully only because someone hand-created `src/pbx_impl/ast122/`
**directly on that one server's disk** — untracked by git, not gitignored,
not backed up anywhere found — a single point of failure: a disk failure or
fresh `git clone` would have lost Asterisk 22 support entirely, with no
documented way to reconstruct it.

**Now resolved** — see the dated subsections below for the full investigation
and fix. Summary: real Asterisk 20/21/22/23/24 source, configured and
compiled/linked against this fork end to end, on real hardware, producing a
working `chan_sccp.so` for all five. `configure.ac`/`extra.m4`/`asterisk.m4`/
`ast.h` all now natively support 120-124 - the working `ast122` hack is
committed into git history instead of living only on one server's disk.

### The real target version matrix (researched 2026-09-22, sourced from
### docs.asterisk.org/About-the-Project/Asterisk-Versions)

| Version | Type | Status |
|---|---|---|
| 16 | LTS | EOL Oct 2023 — not worth new investment |
| 18 | LTS | EOL Oct 2025 — just died, borderline |
| 20 | LTS | Supported to Oct 2027 — **target** |
| 21 | Standard | Maintenance-only, dies Oct 2026 — **target** |
| 22 | LTS | Supported to Oct 2029 (what the PBX runs) — **target** |
| 23 | Standard | Supported to Oct 2027 — **target** |
| 24 | LTS | **Releases Oct 15, 2026** (~3 weeks out), pre-release now — **target, get ahead of it** |

So "as many versions as reasonable" = real per-version review for **20, 21, 22,
23, and 24**, not just a compile-fix shim for whichever one happens to be
installed locally. `ast116` through `ast119` are confirmed byte-identical
(`ast117`/`118`/`119` are literally symlinks to `ast116.c` — zero API drift
across those four versions), so the real work starts at the 20 boundary, not
before it.

### Why `UPGRADE.txt`-diffing didn't pan out as a research method

Asterisk stopped maintaining a single `UPGRADE.txt` after the 16 branch — later
branches (18/20/21/22/23) have no such file at the repo root or under `doc/`.
Whatever replaced it wasn't found in the time spent tonight.

### Real progress: pulled the actual headers and did the diffing directly

Fetched `include/asterisk/*.h` for all 5 target versions straight from
`github.com/asterisk/asterisk` (branches `20`/`21`/`22`/`23`/`24` all exist;
kept locally at `~/GitHub/asterisk-headers/<version>/include/asterisk/`, ~3.4MB
per version, **not** committed into this repo - reference material, not
project source). This produced real, concrete findings, not just a plan:

- **22, 23, and 24 need zero additional work.** `channel.h` is byte-identical
  across all three. The only two files that differ at all between 22→23→24 are
  `manager.h` (confirmed: the only difference is a cosmetic `AMI_VERSION`
  string bump, `"11.0.0"` → `"12.0.0"` — the actual macro `ast116.c` calls,
  `ast_manager_register`, is unchanged) and `musiconhold.h` (confirmed:
  `ast116.c` never references it at all). **The existing `ast122` hand-hack
  already validly covers 22, 23, and 24 as-is** - the real work is only 20 and
  21.
- **20→21 and 21→22 have real churn**: 56 and 40 header files differ,
  respectively (vs. 1-2 files for the 22→23→24 range). Confirmed the known
  `ast_channel_macroexten`/`macrocontext` (+ `_set` variants) removal lands
  exactly at the 20→21 boundary, matching what the existing hack already
  stubs.
- Tried filtering the raw diffs down to only lines mentioning symbols
  `ast116.c` actually references (extracted all 319 `ast_*` identifiers from
  the file, grepped the diffs against that list) - cut 2272 raw diff lines
  down to ~40-56 candidates per version boundary. **This method has a real
  blind spot, found the hard way**: `ast116.c` doesn't call
  `ast_channel_macroexten()` directly even though it's confirmed to use it -
  it goes through this codebase's own macro layer
  (`DECLARE_PBX_CHANNEL_STRGET(macroexten)`), which only expands to the real
  Asterisk symbol elsewhere. A handful of other candidates that grepped as
  "zero references" (`ast_bridge_get_variable`, `ast_channel_endpoint`,
  `ast_channel_monitor`, `ast_channel_tech_hangupcause`, the
  `ast_app_exec_macro`/`ast_channel_*_macro` family - all of which look like
  they're part of the same "legacy Macro() app" removal as macroexten/context)
  could genuinely be unused, or could be hidden behind the same kind of macro
  indirection. Grep alone can't tell the difference reliably.

### Resolved: real compile test against 20 and 21 (2026-09-22)

Ran the decisive test instead of continuing to reason from header diffs.
Cloned `asterisk/asterisk` branches `20` and `21` directly from GitHub into
`/usr/src/ast-versions/` on the PBX, ran `./configure` against each (enough to
generate `autoconfig.h` and every other build-time header, without a full
`make`), then compiled `ast116.c` against each tree using the **exact flag
set** the production `ast122` object is actually built with (pulled via
`make V=1` from the real `/usr/src/chan-sccp/src/pbx_impl/ast122/`), including
`-Werror=implicit -Wfatal-errors` so a silently-hidden implicit-declaration
break couldn't slip past a trimmed-down test command.

**Result: `ast116.c`, completely unmodified, compiles clean against real
Asterisk 20 and 21 headers.** Zero errors, zero warnings beyond one unrelated
cosmetic `-Wmissing-include-dirs` about a stale relative path in the test
harness itself. `nm -u` on the resulting `.o` confirms `ast_channel_macroexten`
/ `ast_channel_macrocontext` aren't even in the undefined-symbol table for the
Asterisk 21 build — the existing `ast122` stub's two macro `#define`s turn out
to be defensive, not load-bearing; this codebase's actual compiled code path
never calls those functions at all. So the previously-suspected 20→21
"macroexten removal" boundary doesn't affect `ast116.c` in practice.

**Bottom line: the C source needs zero changes across the entire 20-24
matrix.** Combined with the earlier header-diff finding for 22→23→24 and the
fact the identical `ast116.c`-via-`ast122` file has run in production for
weeks without incident, this closes the "is chan_sccp-modern far from Asterisk
20-24 support" question definitively - it already isn't, at the C level.

**The one real remaining gap is packaging, not code**: `configure.ac` /
`autoconf/extra.m4` in this repo only defines `AM_CONDITIONAL`/
`AC_CONFIG_FILES` entries through `ASTERISK_VER_GROUP_119` - there's no
`_120`/`_121`/`_122` wiring at all, so `./configure` against a real Asterisk
20/21/22 install doesn't generate `src/pbx_impl/ast120/Makefile` (etc.) and
the build fails at the Makefile-generation step, never reaching the compiler.
This is exactly why the production box's actual `ast122/` (a single
`#include "../ast116/ast116.c"` plus the two now-proven-unnecessary-but-
harmless stub macros) had to be hand-created directly on disk, untracked -
the fork's own build system doesn't know versions past 119 exist.

**Fix is small and mechanical** (not attempted yet, but now fully scoped):
add three `ast120`/`ast121`/`ast122` directories (each just
`#include "../ast116/ast116.c"`, matching the proven-in-production file
verbatim), three trivial `Makefile.am`s copied from `ast119`'s, and three
`AM_CONDITIONAL`/`AC_CONFIG_FILES` blocks in `extra.m4` following the existing
106-119 pattern exactly. Also bump `configure.ac`'s stale
`MAX_ASTERISK_VERSION=113` to `122` (or higher). No new C code, no design
decisions - just committing the working hack into the actual git history so it
survives a disk failure or fresh clone, which was the real risk identified
earlier.

### Done: 20-24 wired up, real build-tested, and committed (2026-09-22)

Went further than 20/21 - the user asked to cover 23 and 24 too, not just
close the gap the header-diff research had already flagged as free (22→23→24
being header-identical doesn't help if the *build system* can't even reach
the compiler for those version numbers, which turned out to be the real
story below). Full real `./configure && make` runs (not just header diffs)
against freshly cloned/configured Asterisk 20, 21, 22 (system-installed),
23, and 24 source trees on the PBX, iterating until all five produced a
working `chan_sccp.so`. Found and fixed four real, independent bugs along
the way - none of them hypothetical, all caught by the compiler or by
`./configure` itself refusing to proceed:

1. **`ASTTERISK_VER_GROUP` (double-T) typo in `autoconf/extra.m4`** - every
   `AM_CONDITIONAL([ASTERISK_VER_GROUP_1xx], [test x${ASTTERISK_VER_GROUP} = x1xx])`
   line (12 of them, for versions 106-119) tested a variable that is never
   assigned anywhere in the codebase; only the correctly-spelled
   `ASTERISK_VER_GROUP` is ever set (in `autoconf/asterisk.m4`). Fixed by
   correcting the variable name at all 15 occurrences (12 pre-existing + 3
   just added for 120-122). Whether this ever caused a real-world failure for
   106-119 wasn't fully run to ground (the currently-shipped, pre-built
   `configure` may predate the typo's introduction), but it's unambiguously
   wrong now and was copied verbatim into the hand-hacked upstream tree's
   own extra.m4 too - worth fixing regardless of whether it was live.
2. **`CS_GET_VERSION` in `autoconf/acinclude.m4`** computed
   `BASE=\`dirname $ac_dir\`` where `$ac_dir` is never actually set by this
   macro - it was silently reusing whatever value a *different*, unrelated
   `for ac_dir in ...` loop inside `libtool.m4` happened to leave behind as
   a shell-global side effect. This "worked" only by the accident of which
   macro autoconf happened to expand last, which is why the currently-shipped
   `configure` (built by an older autoconf) tolerated it but a fresh
   `autoreconf -fi` with a newer autoconf/automake on this box didn't
   (`dirname: missing operand` / `tools/versioncheck: No such file or
   directory`, well before ever reaching Asterisk detection). Fixed by using
   autoconf's own reliably-set `$srcdir` instead. This one blocked *any*
   regeneration of the build system, for any Asterisk version, not just
   20-24 - a real durability bug independent of this task.
3. **`NEWCONST` undefined for any new version group** - `src/pbx_impl/ast/ast.h`
   (the shared wrapper header every compilation unit includes) has a chain of
   `#ifdef ASTERISK_CONF_1_16` / `#ifdef ASTERISK_CONF_1_17` ... conditionally
   including each version's own header, but the chain stopped at
   `ASTERISK_CONF_1_19`. The `ASTERISK_CONF_1_2x` macros themselves also
   weren't defined anywhere - `autoconf/asterisk.m4`'s version-detection
   `case` statement (both the explicit `--with-asterisk-version=` override
   path and the dead legacy auto-detect path, see #4) also stopped at 119.
   Result: building against a detected version >119 compiled `pbx_impl.c`
   with `NEWCONST` completely undefined, a hard compile error. Fixed by
   adding `120`-`124` entries to both `case` statements in `asterisk.m4` and
   the matching `#ifdef ASTERISK_CONF_1_2x` / `#include` blocks in `ast.h`.
4. **Asterisk 20 specifically can't use the `ast122`-style stub wrapper** -
   confirmed by a real compile failure, not a guess:
   `#define ast_channel_macrocontext(chan) ("")` in `ast120.c` collided with
   the *real* `ast_channel_macrocontext()` declaration still present in
   genuine Asterisk 20 headers (`error: expected identifier or '(' before
   string constant`), because the macroexten/macrocontext removal doesn't
   land until 21. This is the direct, compiler-verified confirmation of what
   the earlier header-diff research predicted. Fixed by making `ast120` a
   plain symlink to `ast116.c`/`ast116.h` - identical to how `ast117`/`118`/
   `119` already handle versions that need zero changes - instead of the
   defensive-stub wrapper pattern, which is now used only for `ast121`
   through `ast124` (the versions that actually lack those functions).

**Verified with real `./configure && make` runs producing an actual
`chan_sccp.so`** against: Asterisk 20 (clone, symlink wrapper), 21 (clone,
stub wrapper), 22 (the box's real system-installed 22.8.2, stub wrapper,
matching production), 23 (clone, stub wrapper), 24 (clone, stub wrapper).
All five link cleanly.

**Also found, not yet fixed (separate, lower-priority bug)**: when no
`--with-asterisk-version=` is given, `./configure`'s auto-detection can't
identify Asterisk 20+ at all. `asterisk/version.h` (the header the primary
detection path parses) was removed from Asterisk entirely in favor of
`asterisk/ast_version.h` - including the old header is now a hard
`#error` in modern Asterisk, so `AC_CHECK_HEADER` correctly reports it
missing and falls through to a much cruder fallback heuristic
(`autoconf/asterisk.m4` ~line 197-220) that greps for `AMI_VERSION` /
`res_audiosocket.h` and, finding them present (true for any Asterisk from
118 onward), **always reports "Found 'Asterisk Version 11900'"** regardless
of whether the real install is 19, 20, 22, or 24. Confirmed directly: a
plain `./configure` against this box's real Asterisk 22.8.2 prints "Found
'Asterisk Version 11900'" and builds using the `ast119` symlink - which
happens to be harmless today (proven identical to `ast120`-`124` for this
file) but is a real, misleading operator-facing message and means the new
120-124 code paths are *only* reachable via the explicit
`--with-asterisk-version=` flag, never by auto-detection. Fixing this
properly needs new version-string parsing logic for Asterisk's modern
integer-only scheme (checking `AST_MAJOR_VERSION` or similar from
`ast_version.h`), not a mechanical list extension - scoped as a real,
separate follow-up.

## Fixed — FreeBSD one-way audio / dual-stack bind bug (researched + fixed + validated on real hardware, 2026-09-22)

chan-sccp had a working FreeBSD port at one point per the user's own
recollection, which checks out: Asterisk itself ships a `BSDmakefile` at its
repo root (confirmed on the `22` branch), so FreeBSD is a genuine first-class
Asterisk build target, not an afterthought.

**Found the specific RTP audio bug being recalled**: [chan-sccp/chan-sccp
issue #499](https://github.com/chan-sccp/chan-sccp/issues/499) — a Cisco 6901
on FreeBSD 12.1-RELEASE got one-way audio (phone received audio, but tcpdump
showed zero RTP packets flowing *from* the phone back to Asterisk). The
reporter's own `sockstat -v` output is the smoking gun:

```
asterisk asterisk   10169 13 tcp6   *:2000                *:*
```

— only `tcp6`/`udp6` bound, no `tcp4`/`udp4` counterpart, despite the phones
being plain IPv4 DHCP/TFTP clients.

**Root cause, confirmed against this codebase, not just the old issue thread**:
FreeBSD defaults to `IPV6_V6ONLY=1` (a v6 socket accepts only v6 traffic unless
told otherwise); Linux defaults to `IPV6_V6ONLY=0` (a v6 wildcard socket
transparently also accepts v4 traffic). `grep -rn "IPV6_V6ONLY" src/` returns
**zero results anywhere in this codebase** — the dual-stack bind at
`sccp_transport_tcp.c:35` (`bind(sc->fd, addr, addrlen)`) never sets this
option explicitly, so it silently inherits whatever the OS defaults to. That's
exactly why this worked for essentially every tester (who are on Linux) and
silently broke for the one person on FreeBSD.

**Fixed and validated on real hardware** — the user offered an actual FreeBSD
box (`belvedere`, FreeBSD 15.1-RELEASE-p3) specifically for this. Confirmed
`net.inet6.ip6.v6only=1` as the live system default, then:

1. Reproduced the exact historical bug with a minimal standalone C program:
   bind `::` without clearing `V6ONLY` → IPv4 connection refused at the kernel
   level, matching the old issue's symptom precisely.
2. Fixed in `sccp_netsock_setoptions()` (not `tcp_bind()`/`tls_bind()`
   individually — every transport already calls this function immediately
   before `bind()`, so fixing it here covers both TCP and TLS uniformly).
   Detects the socket's family via `getsockname()` and clears `IPV6_V6ONLY`
   only for `AF_INET6` sockets, in the function's existing cross-platform
   section (ahead of the `#if defined(linux)` block) since this needs to run
   on every OS — it's a no-op on Linux, whose default is already 0.
3. **A real gotcha caught by testing, not assumed away**: `IPV6_V6ONLY` can
   only be changed *before* `bind()` on FreeBSD — an initial test that set it
   *after* binding correctly failed with `EINVAL`. The real code already calls
   `sccp_netsock_setoptions()` before `bind()` (confirmed in `sccp_session.c`),
   so this isn't an issue for the actual fix, but it's exactly the kind of
   assumption that would've been wrong if shipped without checking.
4. Validated the exact real logic (pre-bind `getsockname()` → conditional
   `setsockopt`) in isolation: family correctly detected on an *unbound*
   socket, `V6ONLY` successfully cleared, confirmed 0 after `bind()`+`listen()`.
5. Also compiled clean (full `make`, `CCLD chan_sccp.la`) against the Linux/
   Asterisk 22 build tree this actually runs on tonight — no regression.

**Not done**: no Asterisk/chan-sccp install exists on `belvedere` (offered
purely as a test box) — only the standalone socket mechanism was validated,
not a full build. A real end-to-end test (phone registration, actual RTP flow
on FreeBSD) is still open for whenever a full install is set up there.

**Still unknown**: which FreeBSD versions matter today for broader testing
(the original bug report was FreeBSD 12.1, now itself EOL; `belvedere` runs
15.1) — worth checking FreeBSD's own supported `-RELEASE` list if wider
version coverage is wanted, same as the Asterisk version work above.

## Fixed — message quality, spelling, and file headers (continued)

- **The 55 identical OOM messages** — 45 of the 55 call sites for
  `SS_Memory_Allocation_Error` passed the same hardcoded literal `"SCCP"` as
  context, telling an operator nothing about which allocation failed. Replaced
  with `__func__` at all 45 sites — gives the exact enclosing function name
  automatically, can't go stale. The 10 sites that already passed something
  specific (`"devstate::addSubscriber"`, `c->designator`, etc.) were left as-is.
- **13 misspellings** fixed: `seperated`/`seperate`, `occured`, `paramater`,
  `withing`, `limitted`, across `sccp_debug.c`, `sccp_appfunctions.c`,
  `sccp_line.c`/`.h`, `sccp_netsock.c`, `sccp_refcount.c`,
  `sccp_transport_tls.c`, `sccp_config.c`, `sccp_conference.c`, `sccp_device.c`.
- **Copy-pasted `\file` headers** — **only 5 of the originally-reported 7 were
  real bugs**: `sccp_mwi.c`, `sccp_transport_tcp.c`, `sccp_transport_tls.c`,
  `ast114.c`, `ast_announce.c` were genuine regular files with a stale comment,
  fixed. **`ast117.c`/`ast118.c`/`ast119.c` were never bugs** — they're symlinks
  to `../ast116/ast116.c` (mode `120000`, confirmed via `git ls-tree`), so their
  header correctly said `ast116.c`. Caught a real mistake here: an earlier
  `sed -i` pass in this session silently materialized those three symlinks into
  full standalone 4000-line copies while "fixing" a header that was never
  wrong. Caught before committing (git flagged them as type-changes, `M` → `T`)
  and reverted with `git checkout HEAD --` before anything landed. Lesson for
  future sweeps in this repo: **check `git ls-tree` mode before running `sed -i`
  on any file under `pbx_impl/` — several version directories are legitimately
  symlinked to a shared implementation, not independent copies.**

## Fixed — second typo sweep + TODO/FIXME/XXX/HACK triage (2026-09-22)

Re-scanned the whole tree (not just previously-touched files) for common
English misspellings and fixed everything found:

- `ommitted`→`omitted` (`sccp_appfunctions.c`, `sccp_cli.c`), `begining`→
  `beginning` and `Seperate`→`Separate` (`sccp_utils.c`), `untill`→`until`
  (`sccp_threadpool.c`), `prefered`→`preferred` (`sccp_appfunctions.c`,
  `sccp_codec.c` x2), `Silence supression`→`Silence suppression`
  (`sccp_protocol.c` x3), `transfered`→`transferred` (`sccp_appfunctions.c`,
  `sccp_channel.c` x2), `loosing`→`losing` (`sccp_softkeys.c`),
  `PortReponse`→`PortResponse` (`sccp_actions.c` — the very next case in the
  same `switch` already spells it correctly, confirming this was a typo, not
  a deliberate distinct name), `Succes as int`→`Success as int` (a
  copy-pasted doxygen `\return` line duplicated identically across all 9 real
  `ast106`-`ast116` implementation files).
- **`Priviledge: Command`→`Privilege: Command`** (`sccp_config.c`, 2 AMI
  manager-action response sites) — worth calling out separately from the
  rest: this string is sent over the wire to Asterisk Manager Interface
  clients, not just read by a developer in a log. Any AMI client written to
  expect Asterisk's own standard `Privilege:` header spelling would have
  silently failed to match on this one.
- **`SKINNY_DISP_NO_CHANNEL_TO_PERFORM_XXXXXXX_ON`** (`sccp_labels.h`) — the
  literal string of X's was baked into the `#define`'s own identifier
  (substituted via `%s` at its 2 call sites in `sccp_softkeys.c`/
  `sccp_actions.c`). Renamed to `SKINNY_DISP_NO_CHANNEL_TO_PERFORM_ACTION_ON`
  at all 3 sites - the string value itself (`"No Channel to perform %s on
  !"`) was already fine.

**TODO/FIXME/XXX/HACK triage** (19 real markers in `src/`, not the 148
figure from the initial broad-strokes estimate — that count included
non-source directories/looser matching): one is externally-mandated and
must not change (`_XXX_AST_CONTROL_T38` across 9 `pbx_impl/ast10x-116`
files is Asterisk's *own* upstream enum constant name, marking that value
deprecated in Asterisk's own headers — confirmed it's never defined
anywhere in this codebase, only referenced, so renaming it would break the
`switch` match against the real enum). The `XXXXXXX` one above was the only
genuinely-fixable naming issue found in that set. The rest are real,
substantive TODOs left as-is (need a design decision or deeper testing, not
a text cleanup):
- `sccp_cli.c:2419-2438` — "temporary backward compatible version
  (2020-11-16)" — now 5+ years old; worth revisiting whether it can be
  removed, but that's a compatibility-scope decision, not this pass.
- `sccp_feature.c:524` — voicemail-button workaround for old hint style /
  speeddials before the first line.
- `pbx_impl/ast113,114,115,116/*.c` — "convert format_type to ast_format",
  the same note duplicated across 4 files, a real data-type migration.

## Open — message quality (broader pass needed, this is a standing concern now)

- (The "55 call sites with the generic OOM string" item that used to be here
  is done — verified zero remaining `SS_Memory_Allocation_Error, "SCCP")`
  call sites in `src/`. Removed as stale; see the earlier **Fixed** entry.)
- General standing instruction from the user (2026-09-22): **any message shown to
  a human — phone display text, CLI console output, or log lines — needs real
  content and correct grammar, not boilerplate or copy-paste slop.** Apply this
  standard to every file touched going forward, not just `sccp_webservice.c`.
- **Refined standard (2026-09-22, later the same day)**: the user gave two
  sharper rules for this class of fix, worth calling out because they change
  *what counts as done*, not just *what to touch*:
  1. **Report errors, not instructions.** State the fact, the location, and
     *why it matters* (what it implies) - don't append a "please do X" unless
     X is something the reader can reliably act on themselves. Never point at
     a dead/inactive upstream project's issue tracker as if filing a bug
     there will do anything.
  2. **The bar for a good message**: "If you were reading this, even with a
     few machine-jargon items here and there, what would you want to see in
     the log that would prompt you to go back and analyze a fix?" - i.e. does
     it give enough real technical content to judge severity, without telling
     the reader what to do about it.
  3. Also: **freely rewrite misleading/stale comments in anything actually
     touched**, regardless of original authorship ("to hell with what anyone
     else put in there").
  4. **Scope for this whole effort, restated plainly**: anything that pushes
     a response back to the phone, prints to Asterisk's CLI (`pbx_cli`), or
     logs via `pbx_log` is in scope for review.

## Fixed — refcount ALARM messages, CLI table engine, and CLI labels (2026-09-22)

- **`sccp_refcount.c` retain/release failure paths** - three real problems in
  one small block, not just wording: (1) the `retain` path's log line said
  `"(release)"` - a copy-paste mislabel naming the wrong function; (2) both
  paths logged `obj`, which is *always NULL* on this failure branch (it's
  only ever assigned on the success path) - a real logging bug, the pointer
  value shown was never the one that actually failed; (3) two near-duplicate
  log lines per failure (an "ALARM" line with location context, then a
  vaguer `LOG_ERROR` "Major Logic Error. Please report to developers" line
  with no location and a dead GitHub link). Collapsed to one line per
  function with correct location (`file:line(func)`), the *actual* failing
  pointer (`ptr`/`*ptr`, not `obj`), and named the real bug class this
  pattern indicates - a double-release, use-after-free, or dangling pointer -
  instead of a vague "this should never happen" plus an instruction to
  contact a project that isn't active.
- **`sccp_cli_table.h`, the shared macro engine behind every CLI table**
  (Devices, Buttons, LineButtons, SpeeddialButtons, FeatureButtons, ...) -
  the top/bottom `+--- TableName ---+` frame and its width, computed
  separately from the header/data rows, could drift out of sync with actual
  content (exactly the misalignment the user flagged in `sccp show devices`
  output). Replaced with a simpler, structurally-guaranteed-consistent
  layout: a title line, then header/separator/data rows that all share the
  *same* per-field width logic - nothing to independently drift. Also added
  an opt-in `CLI_AMI_TABLE_FIELD_NAMED` (and UTF8 counterpart) so a table can
  show a friendlier CLI column label without renaming the underlying field
  identifier - that identifier doubles as the AMI manager-interface
  protocol's field name, so renaming it outright would silently break any
  AMI client parsing these tables. Verified by hand-tracing all four
  generation phases (CLI header/separator/data-row, AMI) for macro-alias
  symmetry, since an initial pass accidentally deleted the one line that
  defined `CLI_AMI_TABLE_UTF8_FIELD` for every later phase (a real would-be
  compile break, caught before being called done).
- **`sccp show devices` column labels** - applied the new `_NAMED` mechanism:
  `Descr`→"Description", `Address`→"IP Address", `Mac`→"MAC Address",
  `RegState`→"Status", `RegTime`→"Registered", `Act`→"Active" (widened its
  column from 3 to 6 to fit), `Nat`→"NAT", `Type`→"Model". `Token` and
  `Lines` were already clear, left as-is. **Found a real data bug while doing
  this, not just a naming one**: `LoadInfo` has never shown load/firmware
  info - it prints `d->skinny_type`, the exact same raw numeric device-type
  enum `Type` already shows as a readable string. Nothing in this codebase
  tracks the phone's actual reported firmware/load ID at all (`grep`
  confirms no `loadid`/`loadInformation`/firmware field anywhere in
  `sccp_device.h`/`.c` or the protocol-parsing code) - despite `SEP*.cnf.xml`
  provisioning files (this project's other half) having a real
  `loadInformation` tag, the SCCP session layer never captures what the phone
  reports back. Relabeled to `TypeID` (accurate for what it actually shows)
  rather than inventing fake data; real firmware/load-ID tracking is a
  genuine, separate feature gap - noted here, not attempted.
- **Other `pbx_cli`/`pbx_log` message fixes this pass**: `sccp_hint.c` debug
  log with 13 exclamation marks reduced to a normal sentence that also now
  names which `switch` case it's in (`AST_EXTENSION_INUSE`) instead of just
  noise; `sccp_pbx.c`'s `"!! retrieving parked call !!"` normalized;
  `sccp_cli.c` debug test message's trailing `!!` dropped; `chan_sccp.c`'s
  two config-reload `LOG_ERROR` messages had real broken grammar ("an
  configuration file is not following the sccp format") rewritten as plain,
  correct sentences (these two remain self-actionable - "please update your
  own config file" - unlike the refcount ones, so they keep their guidance).

## Fixed — the formatter tooling itself (item 4)

`.clang-format` had two independent real bugs, not one:

1. Duplicate `BreakBeforeBraces` key (`Custom` at line 29, a stray `Linux` at
   line 51) — invalid YAML, clang-format refused to load the file at all.
   Removed the duplicate; `Custom` was clearly intended (it's immediately
   followed by a 20-line `BraceWrapping` block that only applies under
   `Custom`).
2. Even after #1, a test reformat of `sccp_webservice.c` was still badly
   broken — not just differently-styled, genuinely wrong (closing braces not
   aligned with their own opening statement). Root-caused to
   `BraceWrapping.IndentBraces: true`, a Whitesmiths-style setting that adds a
   *compounding* extra indent at every brace nesting level — inconsistent with
   the WebKit-derived style the rest of the config implements. Flipping it to
   `false` fixed it: that file's diff dropped from 915 lines of garbage to 44
   lines of exactly the sane cleanup you'd want.

Verified against 3 files of increasing size (`sccp_webservice.c`,
`sccp_channel.c`, `sccp_cli.c`) — all produce sane, reviewable diffs now
(comment alignment, `//` spacing, declaration alignment — real accumulated
drift, correctly cleaned up, no more indentation corruption).

**Not done as part of this fix**: an actual tree-wide `clang-format -i` sweep.
The config is now trustworthy, but running it across every file is a separate,
much bigger decision (touches every file, one huge commit) — do that
deliberately, not as a side effect of fixing the tool.
