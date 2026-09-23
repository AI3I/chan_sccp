# Build reproducibility and thread-pool repair

Started 2026-09-22/23. User authorized priorities #1 and #2 from the review.
Parent notes: [CLI/tone checkpoint](CLI_OUTPUT_PROGRESS.md),
[detailed findings](CODE_REVIEW_2026-09-22.md).

## Checkpoint and production

- CLI/tone checkpoint `28aceacd` and follow-up commits `7ff0436d`,
  `b6036b24`, `f4737956` were pushed to `main` on 2026-09-23.
- Production still runs the validated CLI/tone module, SHA-256
  `a191cad78c4905dd2f9972a0708bcf5bb87345e01feb05c64f0486797645bb73`.
- Build work ran in `/usr/src/chan-sccp-build-review` on the PBX. The new
  module was loaded only in the private isolated Asterisk process.

## Build work (R4/R10) — implemented, validation recorded below

- Retain generated configure/Makefile.in files; regenerate and commit them with
  their inputs. Git checkouts and archives should build without bootstrapping.
- Bootstrap script now fails on errors and only runs autoreconf; it no longer
  hides errors or implicitly runs make in an existing configured directory.
- Configure targets 20–24 and configures all supported adapter Makefiles. The
  shared implementation now lives in `ast120` (see relocation checkpoint).
- DIST_SUBDIRS includes modern wrappers and shared sources. Legacy adapters
  were retired in the later checkpoint below.
- Serialize enum generation through a stamp; preserve actual source paths.
- `make check`/`make test` run real standalone tests.
- CI matrix uses actual Asterisk 20–24 branch headers, default and optional
  feature builds, sanitizer tests, an archive build, and a bootstrap build.
- CodeQL targets main and uses an explicit modern build.
- Asterisk headers are installed in private prefixes, never over production.
- Asterisk 22 default build and `make check` pass. Clean 20 default and 24 optional
  source archives also compile and pass tests against private matching headers.
- Generated files are checked in; final `src/Makefile.in` includes the archive
  header and indent-file fixes. The full 20–24 CI matrix later passed on GitHub.

## Thread-pool work (R1/R9) — implemented, validation recorded below

- Replace detached workers/forced cancellation with joinable workers and a
  single queue/admission lock. Accepted jobs drain before workers are joined;
  pool storage is released only after successful joins.
- Keep bounded CPU-based startup size; discard racy runtime resizing. A blocked
  callback delays shutdown rather than allowing unsafe module unload.
- Separate stop-admission from destruction. Owner must stop external producers
  before destroy; callbacks must return normally and cannot destroy their pool.
- Allocation/creation failures return failure and unwind; no process `exit(1)`.
- Both enqueue APIs report real acceptance. Caller owns rejected arguments;
  direct queue callers also retain rejected queue nodes.
- Auto-answer rejection releases its conveyor/reference; channel cleanup jobs
  execute synchronously on rejection. MeetMe/conference jobs retain references
  before enqueue and release them on rejection/completion.
- Module teardown closes admission early and joins before remaining services
  and reference-count storage are destroyed. Modern adapter propagates failure.
- Standalone tests compile the production pool with a minimal pthread adapter.
  Cases: blocked workers/draining, concurrent producers/stop, injected allocation
  and thread-start failures, rejected-job ownership, self-destroy rejection.
- Pool code no longer embeds the old resize-specific Asterisk tests; standalone
  tests exercise its replacement through `make check`.
- AddressSanitizer/UBSan tests passed. In an isolated Asterisk 22 process, ten
  unload/load cycles completed and CLI commands still worked each time.
  This confirms idle teardown; call/media load and blocked real callbacks are
  covered by standalone tests, not a live phone run.

## Logs and resume

- `/tmp/sccp-review-{bootstrap,config,build,check,dist}.log` on PBX.
- Asterisk 20 header preparation: `/tmp/sccp-headers-20.log`, private prefix
  `/tmp/sccp-headers-20/prefix`.
- Keep this document current with results, limitations, commits and deployment.
- GitHub Actions was enabled on 2026-09-23. The first hosted run found two
  workflow setup issues: the bootstrap lane needed Ubuntu `gettext` for
  `AM_ICONV`, and Python CodeQL needed no-build mode. Both were fixed.
- [Build run 35804033178](https://github.com/AI3I/chan_sccp/actions/runs/35804033178)
  passed all ten Asterisk 20–24/default/optional jobs. This includes the
  sanitizer, source archive, and bootstrap lanes.
- [CodeQL run 35804033083](https://github.com/AI3I/chan_sccp/actions/runs/35804033083)
  passed both C/C++ and Python analysis jobs.
- A genuine Cisco handset call remains for later validation. The older
  adapters were retired in the separate change documented below.

## Additional user decisions during this work

- Canonical hint command is lowercase `sccp show hint linestates`.
- Remove the misspelled `sccp show softkeyssets` alias; keep `softkeysets`.
- Release version is `5.0.0`; `.version` is the sole release-version source.
  Repeated configuration/builds must not rewrite or derive it from Git branches
  or tags. Git provenance is separate, optional diagnostic metadata.
- Use `Reference Counts` as the refcount output title (command unchanged).
- These changes were committed; they have not been deployed to production.

## Command-surface follow-up

- User requested lowercase CLI names and removal of `sccp no debug`.
- Preserve `sccp debug 0` (disable all), bare `sccp debug` (show), and category
  controls; updated help to list real categories and fixed the `all` mask logic.
- Lowercase `backgroundimage` and conference action completions. Keep phone
  protocol identifiers and AMI names separate from CLI command spelling.
- Found missing-line NULL dereference in `sccp add line` and missing argument
  guard in `sccp set channel ... hold off device`; fixed both during the audit.
- Isolated CLI assertions passed for `debug 0`, removed aliases, command help,
  malformed callforward input, system-message persistence and clearing. Ten
  unload/load cycles passed. Production phones were not exercised.

## September 23 CLI findings and test environment

- `sccp call` printed an internal argc diagnostic and its usage falsely made the
  number and line mandatory. It now accepts `<device-id> [number [line-name]]`,
  rejects excess arguments, and reports call setup failure.
- `sccp callforward` is functional: `all`, `busy`, and `noanswer` map to the
  live line-device forwarding state. The original parser logged multiple
  lookup errors and could pass the sentinel into an array index. It now
  validates the mode, device, and line attachment before any mutation. Omitted
  number disables one mode; `none` disables all. The reported command needs
  the mode: `sccp callforward 2004 SEPB8621F6C90A2 all 2003`.
- `sccp system message` saves a global message in the Asterisk database and
  replays it at phone registration. A default timeout of zero places it in the
  idle prompt slot, where a higher-priority prompt can hide it. A 1–255 second
  timeout uses a transient notification. The old parser could dereference a
  missing argument after `beep`, silently accept invalid timeouts, and wrote
  the same database keys once per device. New parsing, single persistence,
  meaningful feedback, and a clear command were verified. The device helper's
  duplicated timeout-string allocation was removed.
- The isolated instance uses `/tmp/sccp-asterisk-test` on pbx.jdlewis.net,
  with private config/database/socket/module directory and port 22000 bound
  only to loopback. No production module or phone was changed. CLI assertion
  log: `/tmp/sccp-cli-smoke.log`; all checks and ten reloads passed.
- Wadsworth (`192.168.0.62`) is reachable as `jdlewis` with passwordless sudo,
  and now hosts a private Asterisk 22 build under
  `/home/jdlewis/asterisk-lab/prefix`. The complete installation and removal
  inventory is in `/root/asterisk.txt` on wadsworth. Its SCCP module was
  copied only into that private prefix; production was not touched.
- Current changes are not deployed to production. The isolated process was
  stopped after checks. Production remains at the previous CLI/tone hash noted
  above.

## September 23 legacy retirement checkpoint

- Asterisk 20–24 remains the supported range. Removed adapter directories
  `ast106`, `ast108`, `ast110`–`ast115`, and `ast117`–`ast119`; the first eight
  contain about 31,705 physical C/header lines. `ast116` stays because all
  modern wrappers share its implementation. Removed obsolete adapter selectors
  and version probing from the Autoconf inputs.
- Wadsworth's private Asterisk 22 compiled the current cleanup source;
  `make check` and `make dist` passed. A synthetic SCCP client also registered
  and received 576 G.711 u-law RTP packets from local extension 701 on the
  *previous* committed module. That probe is not a physical-handset test and
  does not validate the current cleanup build at runtime.
- The first automatic version-detection attempt found a bug in the rewritten
  macro: `AC_COMPILE_IFELSE` removed `conftest.c` before the macro tried to
  preprocess it. Asterisk's header also split the resulting value across a
  preprocessor line marker. The corrected probe reads the final expansion;
  configure without a manual version now detects 22 and selects group 122.
  The earlier compile/check/archive used an explicit version; no compile or
  runtime test was repeated after this probe correction.
- User requested code work now and no per-change test cycle. Defer further
  synthetic calls and physical Cisco handset checks. Keep lab cleanup details
  in `/root/asterisk.txt`.

## Repository metadata and packaging cleanup

- Removed unused Mercurial, LGTM, and Travis metadata. The Travis file held
  upstream Coverity settings and notification endpoints. Removed upstream
  GitHub funding, contribution, issue, and pull-request templates that pointed
  contributors to the old project. The active GitHub Actions build and CodeQL
  workflows remain.
- Removed stale Debian and RPM packaging, their `make deb`/`make rpm`/old
  release recipes, old Asterisk patch files, and the SVN-era release helper.
  Debian packaging still declared Asterisk 11 and hard-coded an old module
  path, so retaining it would misrepresent current 20–24 support. Source
  archive, build, and test targets remain.
- Build templates were regenerated after removing the obsolete RPM configure
  probe. Per user request, no new compile or live call test is planned for
  this metadata-only cleanup; hosted CI can check the pushed commit.

## Token fallback repair

- Replaced the shell-based fallback script invocation with direct argument
  execution. Device ID, host address, and phone type are passed as three argv
  values. Script output is limited to one short line, the process must exit
  successfully within two seconds, and invalid/long/multiple-line output
  rejects the token using the configured backoff. `ACK` acknowledges; an
  integer greater than 30 seconds sets the rejection backoff.
- `odd` and `even` now use the numeric value of the final hexadecimal device-ID
  digit and reject malformed IDs. `true` acknowledges only on server priority
  1. Removed the duplicate early rejection for `fallback=no` and bounded the
  wire device ID before using it as a C string.
- One compile-only build of this batch passed against wadsworth's private
  Asterisk 22 headers. No module was installed and no phone/client test was
  run. The configured-script, parity, and live registration paths remain to be
  exercised later in the dedicated lab; the running module is unchanged.

## XML ownership cleanup

- Removed per-request and module-unload calls that clean up process-wide
  libxml2/libxslt state. A request now frees only its own XML documents and
  stylesheet. The EXSLT registration remains because shipped translation
  stylesheets use it; the shared registry is left intact across module unload.
- Removed the unused `applyStyleSheet` interface and commented call sites. It
  could free a document without updating its caller. XML serialization now
  copies the result into Asterisk-owned memory before releasing libxml's
  buffer, matching the webservice's existing `sccp_free` ownership contract.
  Document destruction now takes a mutable pointer and clears it without a
  cast. Removed the process-wide external-DTD default assignment.
- The optional XML configuration compiled against wadsworth's private
  Asterisk 22 headers. No module was installed; concurrent HTTP requests,
  stylesheet behavior, and unload/reload under XML traffic remain untested.

## Documentation, contrib, and configuration inventory

- Removed the unused Doxygen integration (`amdoxygen.am`, Autoconf macros,
  `doc/Makefile`, generated templates, and Doxygen assets). The Markdown
  progress/review notes remain in `doc/`.
- Removed `contrib/`, including the manually invoked, outdated config
  generator and standalone diagnostics. Nothing in the module's normal build,
  install, or runtime paths uses these files. Git history retains them if a
  specific diagnostic is needed later. The build no longer configures or
  advertises `gen_sccpconf` targets.
- Removed old SQL/LDAP schema files, FreePBX and alternate SCCP config
  examples, and bundled TFTP wallpaper images. Kept the installed
  `conf/sccp.conf`, its annotated option reference, and the Cisco SEP TFTP
  templates: the latter can still assist handset provisioning, including
  796x phones. Kept the optional XML service's XSLT files and updated their
  installation instructions to match its actual `sccpxslt/` data path.
- Removed `src/sccp_xml_embedded.h` and its generator; the runtime XML path
  loads stylesheets from disk and did not reference the embedded array.
- Regenerated `configure`, `Makefile.in`, and `src/Makefile.in`. Wadsworth's
  private Asterisk 22 build tree configured with experimental XML and `make
  dist` passed. The archive includes `conf/sccp.conf` and XSLT files and
  excludes Doxygen/contrib. No compile, module installation, or phone test
  was run for this file/build metadata cleanup.
- The test-tree sync, bootstrap, configure, and archive check were logged in
  wadsworth's `/root/asterisk.txt`. Production was untouched.

## Session TCP framing and receive cleanup

- Serialize a whole outbound SCCP frame with `write_lock`, including partial
  writes and EINTR retries, so concurrent senders cannot interleave frames.
  Cap retry backoff at 8 ms. Defer session-failure teardown until after
  releasing that lock.
- Enforce the send API's message ownership on stopped sessions, missing
  sessions, and mismatched message metadata. The device wrapper now delegates
  all paths to the owned-message sender. A NULL message returns an error.
- Receive handling now distinguishes retryable EINTR/EAGAIN, peer EOF, and
  fatal errors. It parses data before judging a completely filled buffer, so
  valid coalesced messages are consumed. An unconsumable full buffer is still
  rejected. Reject impossible payload lengths immediately after the header.
- A single compile of the batch passed against wadsworth's private Asterisk
  22 headers with experimental XML enabled. No module was installed, and no
  live client/phone test was run. Wadsworth actions and build log location are
  recorded in `/root/asterisk.txt` there.
- The receive fix addresses the TCP/POSIX result contract. TLS still needs the
  separate R8 review of `SSL_get_error`, handshake failure, and retry direction;
  no TLS behavior claim is made here. Concurrent partial-write behavior and
  fragmented/coalesced frames still need runtime or focused transport tests.

## Codec read/write format correction (R7)

- The read-format wrapper now calls `ast_set_read_format`; the write wrapper
  keeps `ast_set_write_format`. Both reject missing channel owners and non-audio
  codecs and return failure if Asterisk rejects the channel format change.
- RTP-engine format callbacks remain best effort: Asterisk's default RTP engine
  does not implement them, and their failure is not a channel-format failure.
  Video codec recalculation no longer routes video codecs through wrappers that
  address the audio RTP instance. Native video capabilities remain selected.
- One private Asterisk 22 compile passed on wadsworth; the source sync, build
  log, and absence of install/runtime testing were recorded in
  `/root/asterisk.txt` there. No physical-phone or bidirectional transcoding
  test was run. Production was untouched.

## Remaining work inventory (2026-09-23)

- **TLS follow-up (R8):** the accept/handshake, bounded I/O retry, and ownership
  fixes below are compile-only. Bad handshakes, clean closure, stalled clients,
  and reconnects still need focused runtime validation before closing R8.
- **Media follow-up:** validate dynamic audio/video mappings, bidirectional
  transcoding, early media, paging, hold/resume, and transfer on a handset.
  The signed RX/TX lookup and conservative mappings below are compile-only.
- **High-value cleanup:** HTTP/CLI test handlers and `libpbximpl.la` were
  removed in the batch below.
- **Structural cleanup:** the shared adapter was moved into `ast120`, unused
  C++ build scaffolding and disabled `#if 0` blocks were retired, and the four
  identical 21–24 wrappers were consolidated below.
- **Deferred validation:** physical Cisco call behavior and the compile-only
  R2/R3/R5/R6/R7/R11/R12 paths. The user requested code progress now and no
  test cycle after every change. Keep all lab actions recorded in wadsworth's
  `/root/asterisk.txt`; production remains on the previously validated module.
- **Standing quality pass:** CLI/phone/log messages and misleading comments,
  as described in `HEALTH_AUDIT.md`. The older health audit includes historic
  plans and should not override this current status section.

### Additional source-audit findings (2026-09-23)

These are code-review findings, not runtime-validated repairs. No build or live
test was run for this read-only audit.

- **High — conference playback mutex:** In `src/sccp_conference.c`,
  `playback_to_conference()` returns after a missing-file check without
  unlocking `conference->playback.lock` (both the current Asterisk branch and
  the legacy branch). A bad announcement filename can leave later conference
  playback blocked. Fix the current branch; remove the unreachable pre-20
  branch as part of the already planned version cleanup.
- **High — phone text messages:** `sccp_device_pushTextMessage()` in
  `src/sccp_device.c` rejects messages *shorter* than 1024 bytes for protocol
  versions below 17, the reverse of its documented limit. An empty or NULL
  sender leaves the `title` buffer uninitialized, then passes it to `%s` in
  `snprintf()`. The Asterisk message callback passes its `from` argument
  directly, so this needs a safe empty-sender path and a corrected size check.
- **Medium — conference announcement format:**
  `sccp_astwrap_requestAnnouncementChannel()` in `src/pbx_impl/ast120/ast120.c`
  ignores `format_type` and always requests A-law. The conference caller
  currently asks for A-law on the supported branch, so no current mismatch is
  proven; remove the unused argument or honor it when simplifying the adapter.
- **Low — connection statistics byte order:** `handle_ConnectionStatistics()`
  in `src/sccp_actions.c` applies `letohl()` to the result of a comparison
  instead of decoding `lel_protocolVer` before comparing it. This works by
  accident on little-endian hosts but can select the wrong statistics layout
  on big-endian hosts. The repeated version checks should share one decoded
  value. Review the v22 quality-statistics length handling in the same pass.

**Repair checkpoint (`258191af`, 2026-09-23):** The missing-file returns in
both conference playback branches now release the playback mutex. Phone text
messages now accept the documented 1024/4000-character limits, initialize an
empty title when the sender is absent, and reject a NULL body. The supported
announcement request now rejects formats other than A-law instead of silently
creating an A-law channel. Connection-statistics layout selection uses one
decoded protocol version; all three quality-statistics copies clamp the wire
length before adding space for a terminator, including the unaligned v22 path.
The source diff passed `git diff --check`. The hosted
[Asterisk 20–24 build and test matrix](https://github.com/AI3I/chan_sccp/actions/runs/35893496608)
and [CodeQL](https://github.com/AI3I/chan_sccp/actions/runs/35893496696)
passed for the source commit. No live handset test was run.

## Media payload and test-surface cleanup (2026-09-23)

- Changed RTP payload lookup through the PBX interface and SCCP helper to
  return signed `int`, with `-1` for absent mappings. Lookups now distinguish
  Asterisk transmit payloads (phone receive) from Asterisk receive payloads
  (phone transmit). A video channel rejects absent/out-of-range payloads
  before sending Skinny instructions or marking RTP progress.
- On RTP instance creation, retained the six proven static audio transmit
  mappings and registered the existing codec table's fixed dynamic values:
  iLBC 97, G.722.1 102/115 when compiled in, H.263+ 98, and H.264 103.
  Added static video H.261 31 and H.263 34 and corrected the H.261 metadata
  from 34 to 31. TX and RX maps are assigned separately. G.726, Opus, H.265,
  and other codecs with absent or ambiguous SCCP payload values were not
  assigned a guessed number; runtime video/codec behavior is still untested.
- Removed the always-registered `testhtml` and `testxml` web handlers, including
  the HTML echo of request fields. Removed the experimental `sccp test` CLI
  playground, which contained unchecked arguments, shell command execution,
  and direct reference-count manipulation. Real web handlers and the
  experimental feature flag remain.
- Removed the metadata-only `src/pbx_impl/pbx_impl.c` compilation unit and its
  `libpbximpl.la` link dependency. The build still distributes `pbx_impl.h`.
  Regenerated the affected Makefile.in files.
- On wadsworth, one private Asterisk 22 compile of the media batch passed.
  After the test/build cleanup, bootstrap, configure, compile, and `make dist`
  passed. No module was installed and no handset test was run. All lab actions
  and log paths are in `/root/asterisk.txt`; production remains unchanged.
- Remaining: real calls for dynamic
  RTP, video, codec changes, paging/early media, and bidirectional transcoding.
  Adapter wrapper consolidation and dead code remain cleanup items.

## Shared adapter relocation (2026-09-23)

- `ast120.c/.h` were symbolic links to the real `ast116.c/.h` files, not
  duplicate implementations. Moved the real files into `ast120` and updated
  the Asterisk 21–24 wrappers to include them. The Asterisk 20 build now uses
  ordinary source files in its own adapter directory.
- Removed the unselected `ast116` Makefile and configure/archive entries.
  The source archive now contains the canonical `ast120.c/.h` files without an
  obsolete version directory. This changes file placement, not adapter logic.
- On wadsworth, private Asterisk 22 bootstrap, configure, compile, and
  `make dist` passed. The generated `configure` and `src/Makefile.in` were
  copied back and staged. Log paths and all lab actions are recorded in
  `/root/asterisk.txt`. No module installation or live phone test was done.
- The 21–24 wrappers were later consolidated below. The completed adapter
  simplification passed the Asterisk 20–24 CI matrix.

## C-only build configuration (2026-09-23)

- All compiled targets are C. Removed the unused C++ compiler probe and
  per-target C++ flags, including `-fpermissive`. The `.hh` configuration
  entries file is included by C source and does not require a C++ compiler.
- `--enable-lto` previously tested `-flto` but restored CFLAGS later, so the
  option did not affect the build. It now checks a C link and adds the flag to
  the C compile and link flags when supported. Its help text now matches the
  disabled-by-default behavior; invalid values report the supplied value.
- Regenerated the checked-in Autotools files. On wadsworth, private Asterisk 22
  bootstrap, normal configure/build, and `make dist` passed; an LTO configure
  and clean LTO build passed as well. Logs and lab actions are recorded in
  `/root/asterisk.txt`. No module installation or runtime phone test was done.
- Local `autoreconf` is unavailable, so generation was done in the private lab.
  The large generated `configure` diff is primarily removal of the Autoconf
  C++ compiler probe and libtool C++ tag. The later Asterisk 20–24 matrix passed.

## Disabled code removal (2026-09-23)

- Removed 12 outer `#if 0` regions (roughly 300 lines) containing unused RTP bridge and
  set-option implementations, a dead transfer function, reference-count macros
  and examples, conference invite/lock stubs, a socket comparison, and debug
  snippets. The nested disabled reference-count example was removed with its
  enclosing block. In the transfer path, preserved the active `#else` branch.
- Removed a stale transfer prototype and comments that still named the
  discarded implementations. This
  is preprocessor-dead code only; no live branch was changed. Source diff and
  `git diff --check` were reviewed. Per the user's request to avoid repeated
  builds, no additional compile or runtime test was run for this deletion.
- The 21–24 wrappers were tiny copies of the same macro definitions and
  included `ast120`; they were removed in the next checkpoint.

## Adapter wrapper consolidation (2026-09-23)

- All supported Asterisk 20–24 builds now select `ast120` directly. The four
  21–24 wrapper source/header/Makefile directories were removed. The shared
  `ast.h` defines the Asterisk 21+ macro extension/context compatibility stubs
  once, guarded by the existing version defines, before including `ast120.h`.
- Configure and distribution lists now contain only the actual adapter. The
  regenerated `configure` and `src/Makefile.in` were copied back from the lab.
- Wadsworth private Asterisk 22 bootstrap, configure, compile, and source
  archive passed. The archive contains `ast120` and omits `ast121`–`ast124`.
  No module was installed or live phone test run. Logs and lab activity are
  recorded in `/root/asterisk.txt`. The hosted
  [Build and test run 35864571157](https://github.com/AI3I/chan_sccp/actions/runs/35864571157)
  passed the Asterisk 20–24 default and optional matrix, including sanitizer,
  archive, and bootstrap jobs.

## TLS transport repair (2026-09-23)

- The listener now initializes each descriptor to `-1`, checks the transport's
  returned pointer, resets the address length before each accept, and transfers
  accepted socket/TLS ownership to the session explicitly. TCP accept uses the
  same pointer-or-NULL contract. A failed server-context bind also releases its
  allocated context and transport.
- TLS handshake uses the signed `SSL_accept` result and `SSL_get_error`, waits
  for the requested socket direction on a nonblocking socket, and has a five
  second deadline. Read/write likewise use `SSL_get_error`, retry the same
  operation after readiness, and expose closure/errno consistently to the
  session code. Buffered plaintext is checked before the session polls again.
  The retry behavior follows the OpenSSL
  [accept](https://docs.openssl.org/3.0/man3/SSL_accept/),
  [error](https://docs.openssl.org/3.0/man3/SSL_get_error/), and
  [write](https://docs.openssl.org/3.0/man3/SSL_write/) contracts.
- TLS context creation failures and transport destruction now release
  `SSL_CTX`; TLS close clears the socket and SSL pointers. Module teardown no
  longer calls OpenSSL-wide cleanup that could affect Asterisk's other users.
- Private Asterisk 22 with `HAVE_LIBSSL` compiled the batch on wadsworth,
  including the final context/error-path edits. No TLS listener or handset
  test was run at the user's request. All lab activity and build logs are in
  `/root/asterisk.txt` there.
- Follow-up: OpenSSL
  [requires serial access to each SSL object](https://docs.openssl.org/master/man7/openssl-threads/).
  A per-connection mutex now serializes TLS read, write, pending, shutdown,
  and free; session teardown also waits for in-flight sends. The lock is
  allocated after the handshake and transferred with the accepted connection.
  Listener cancellation is disabled from accepted fd through the TLS handoff,
  with a five-second handshake bound; the accept loop re-enables it afterward.
  A private Asterisk 22 compile passed after using the project's mutex type;
  the first attempt with raw `pthread_mutex_t` failed because Asterisk headers
  prohibit that type. The change has not been exercised with a live TLS client.
  Runtime handshake/closure/reconnect and teardown races remain for focused
  validation before claiming safe live TLS behavior.
- The final source commit `0799a6d8` passed the hosted
  [Asterisk 20–24 Build and test matrix](https://github.com/AI3I/chan_sccp/actions/runs/35865377426)
  and [CodeQL](https://github.com/AI3I/chan_sccp/actions/runs/35865377415).
  These are build/static checks, not live TLS or handset validation.

## Session send lifetime repair (2026-09-23)

- Source commit `9f219ce7` makes device sends find a live session through the
  global session list instead of dereferencing `device->session`. Direct
  `send2` calls likewise validate the pointer against the list before using
  it. Each send holds an in-flight reference from lookup through completion;
  teardown removes the session from the list, waits for active sends, then
  closes the socket and frees the session. The send path retains the associated
  device while using its protocol and log identity.
- Device/session detachment is synchronized with send lookup. Releasing an old
  session no longer removes a newer session from the global list or cleans a
  device that has re-registered onto the newer session. The device backpointer
  is cleared when its owning session is released.
- `git diff --check`, the hosted
  [Asterisk 20–24 build and test matrix](https://github.com/AI3I/chan_sccp/actions/runs/35894774590),
  and [CodeQL](https://github.com/AI3I/chan_sccp/actions/runs/35894774699)
  passed. No module installation, reconnect/send stress test, or handset call
  was run. Those runtime checks remain open.

## Retired Asterisk branch cleanup (2026-09-23)

- The supported conference implementation no longer carries its pre-12 bridge
  includes, playback implementation, capability flags, or version-conditioned
  wrappers. The Asterisk 20–24 behavior is unchanged apart from checking that
  bridge creation succeeded before setting optional video mode.
- Removed pre-11 monitor fields/initialization, the pre-12 distributed device
  state event path, and pre-16 forwarding state adjustments. The supported
  connected-line indication paths now compile directly.
- This batch removes roughly 160 lines of unreachable code. `git diff --check`,
  the hosted [Asterisk 20–24 build and test matrix](https://github.com/AI3I/chan_sccp/actions/runs/35895520802),
  and [CodeQL](https://github.com/AI3I/chan_sccp/actions/runs/35895520780)
  passed. No live call was run.
- Other pre-20 version branches remain, notably in `sccp_hint.c` and the PBX
  compatibility headers. They should be removed in focused batches so the
  Asterisk 20–24 build matrix can catch dependency mistakes.

### Hint callback and obsolete distributed-state option

- `sccp_hint.c` now keeps only the Asterisk 20–24 extension-state callback
  signature and state extraction. Removed its pre-11 variants and the optional
  distributed-state subscription: the supported-version callback assigned
  empty caller ID fields, so it could not update hint call information.
- Removed `--enable-distributed-devicestate`, whose help text targeted Asterisk
  1.8–12, plus its configure macro, generated configuration entry, and an
  empty adapter preprocessor block. About 160 further lines were removed.
- Autotools files were regenerated in a fresh wadsworth scratch directory.
  The local `configure` diff was reduced to the option removal and compared
  equal to generated output with whitespace ignored; unrelated generator
  whitespace was discarded. Scratch was removed. All wadsworth actions are
  recorded in `/root/asterisk.txt` there.
- `bash -n configure` and `git diff --check` passed; the option is absent from
  `./configure --help`. The hosted
  [Asterisk 20–24 build and test matrix](https://github.com/AI3I/chan_sccp/actions/runs/35896589547)
  and [CodeQL](https://github.com/AI3I/chan_sccp/actions/runs/35896589504)
  passed. No module installation or live handset test was run.

### Shared Asterisk wrapper cleanup

- Removed pre-20 API branches from the shared wrapper headers and active
  adapter functions: bridge and AMA type mappings, codec constants, call group
  setters, redirecting/connected-line handling, configuration, networking,
  and RTP quality reads. Feature probes that can still differ across Asterisk
  20–24 remain. The selected supported-version code paths are unchanged.
- Source commit `8f46a28c` removes about 200 lines of retired compatibility
  code. `git diff --check`, the hosted
  [Asterisk 20–24 build and test matrix](https://github.com/AI3I/chan_sccp/actions/runs/35897444609),
  and [CodeQL](https://github.com/AI3I/chan_sccp/actions/runs/35897444602)
  passed. No module installation or live handset test was run.
- Dead `UNUSEDCODE` blocks and other PBX adapter files still contain retired
  branches and can be cleaned in later focused batches.

### Disabled Asterisk adapter code

- Removed all five `UNUSEDCODE` blocks from `src/pbx_impl/ast/ast.c` and their
  four matching declarations in `ast.h`. The flag has no definition in the
  repository, and no callers of the removed functions exist outside those
  disabled blocks. They included an unused extension-state map, a malformed
  cause map, and abandoned channel-walk, ACL, and extension-removal wrappers.
- Source commit `e956fa4d` removes about 210 lines. `git diff --check`, the
  hosted [Asterisk 20–24 build and test matrix](https://github.com/AI3I/chan_sccp/actions/runs/35897985288),
  and [CodeQL](https://github.com/AI3I/chan_sccp/actions/runs/35897985379)
  passed. No live module or handset test was run. Other disabled blocks
  elsewhere in the project remain for subsequent review.

### Remaining disabled core blocks

- Removed the remaining `UNUSEDCODE` blocks from the core source and headers:
  abandoned audio/video media-update helpers, scheduler-free helper, device
  display helper, and call-info copy method. Their declarations, interface
  entry, and display macro are removed too; no active callers were found.
  The commented-out guard around active `CopyByKey` was removed without
  changing that function.
- Source commit `f07bd7cd` removes about 140 lines. `git diff --check`, the
  hosted [Asterisk 20–24 build and test matrix](https://github.com/AI3I/chan_sccp/actions/runs/35898528592),
  and [CodeQL](https://github.com/AI3I/chan_sccp/actions/runs/35898528632)
  passed. No module installation or live handset test was run.

### Channel and PBX interface version guards

- Removed pre-20 branches around channel capabilities and call IDs, call-group
  restoration, transfer notifications, PBX application callbacks, and AMI
  registration flags. The Asterisk 20–24 paths remain the same. The named-group
  feature probe remains because it can differ by build configuration.
- Removed a commented-out redirected-update call exposed by this cleanup.
  Other older-version guards remain in management, feature, and utility code.
  Build and static validation are pending; no live call was run.
