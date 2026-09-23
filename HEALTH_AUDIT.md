# chan_sccp-modern Health Audit

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
previous group; it is now logged as a syntax error and skipped. Validation: CI build (`ccpp.yml` and the CodeQL c-cpp
build); not rebuilt or deployed to the PBX.

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
