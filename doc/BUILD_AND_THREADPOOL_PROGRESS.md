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
- Configure targets 20–24 and configures all supported adapter Makefiles plus
  the shared ast116 implementation for source distribution.
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

- **Open reviewed defect:** R8, TLS accept/handshake and SSL read/write retry
  handling, including connection and `SSL_CTX` ownership. This is the only
  unimplemented R1–R12 finding; see the review for the precise failure paths.
- **Media follow-up:** make RTP payload lookup return signed failure instead of
  wrapping -1 to `uint8_t`; validate dynamic audio/video payload mappings,
  bidirectional transcoding, early media, paging, hold/resume, and transfer.
  The static audio payload fix covers only six standard mappings.
- **High-value cleanup:** remove or test-gate `testhtml`/`testxml` HTTP handlers
  and the hazardous `sccp test` CLI branches; fold away the no-behavior
  `libpbximpl.la` build unit. These remain present in source.
- **Structural cleanup:** rename the shared `ast116` adapter for its actual
  20–24 role, consolidate small per-version wrappers, assess C++ scaffolding,
  and remove unreachable `#if 0` implementations in reviewable batches.
- **Deferred validation:** physical Cisco call behavior and the compile-only
  R2/R3/R5/R6/R7/R11/R12 paths. The user requested code progress now and no
  test cycle after every change. Keep all lab actions recorded in wadsworth's
  `/root/asterisk.txt`; production remains on the previously validated module.
- **Standing quality pass:** CLI/phone/log messages and misleading comments,
  as described in `HEALTH_AUDIT.md`. The older health audit includes historic
  plans and should not override this current status section.
