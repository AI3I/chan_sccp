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
