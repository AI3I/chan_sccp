# chan_sccp-modern Health Audit

Running log of the cleanup effort started 2026-09-22. Not a formal issue tracker —
just a durable record of what's been found and fixed, so work can resume across
sessions without re-deriving everything. Append to this as new issues are found;
move items from "Open" to "Fixed" with the commit/branch that fixed them.

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

## Open — Asterisk version compatibility (the big one)

- `configure.ac` declares `MAX_ASTERISK_VERSION=113`. The real upstream
  (`github.com/chan-sccp/chan-sccp`, `develop` branch) only has genuine
  implementations through `ast119` — nothing for 120/121/122 in any commit, on
  any branch, anywhere in git history.
- The production PBX runs Asterisk 22.8.2 successfully because someone
  hand-created `src/pbx_impl/ast122/` **directly on that one server's disk** —
  untracked by git, not gitignored, not backed up anywhere found. Its entire
  content: `#include "../ast116/ast116.c"` (Asterisk 1.16, ~2018) plus two
  `#define` stubs for `ast_channel_macroexten`/`macrocontext` (removed in
  Asterisk 21+, would otherwise fail to compile). Nothing else that changed
  across ~7 major Asterisk versions was audited or adapted.
- **Risk**: this is a single point of failure. A disk failure or a fresh
  `git clone` of any of the forks loses Asterisk 22 support entirely, with no
  documented way to reconstruct it except redoing this by hand.
- **Recommendation for later**: properly review `ast116.c` against the real
  Asterisk 20-22 API surface (not just what happens to still compile) and commit
  a real `ast120`/`ast122` implementation to `chan_sccp-modern`, not just the
  compile-fix shim.

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

## Open — message quality (broader pass needed, this is a standing concern now)

- **55 separate call sites** log the byte-identical generic string via the
  `SS_Memory_Allocation_Error` macro for any out-of-memory condition — tells an
  operator *that* something failed, never *which* allocation or *where*. Needs a
  design decision: pass context into the macro, or accept `pbx_log`'s own
  file/line and just improve the message text.
- **148 TODO/FIXME/XXX/HACK markers**, concentrated in the hottest paths:
  `sccp_channel.c` (23), `sccp_config.c` (17), `sccp_pbx.c` (16),
  `sccp_actions.c` (16). Not individually triaged yet.
- General standing instruction from the user (2026-09-22): **any message shown to
  a human — phone display text, CLI console output, or log lines — needs real
  content and correct grammar, not boilerplate or copy-paste slop.** Apply this
  standard to every file touched going forward, not just `sccp_webservice.c`.

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
