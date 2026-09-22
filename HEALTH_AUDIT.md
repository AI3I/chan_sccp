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
Whatever replaced it wasn't found in the time spent tonight. **Next session:**
either check Asterisk's GitHub Releases pages directly (`gh release view
<tag>`) for per-version release notes, or diff `include/asterisk/*.h` between
tagged branches directly for the specific APIs `ast116.c` actually calls — that
second approach is more work but doesn't depend on Asterisk's documentation
habits.

### Recommendation

Properly review `ast116.c` against each target version's real API surface (not
just what happens to still compile) and commit real `ast120`/`ast121`/`ast123`/
`ast124` implementations to `chan_sccp-modern` (ast122 already effectively
exists via the hand-hack; formalize it the same way). Given the version count,
this is realistically its own multi-session project, not a single sitting.

## Open — FreeBSD support (researched 2026-09-22)

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

**Recommendation**: before the `bind()` call in `sccp_transport_tcp.c`,
explicitly `setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off))` when
binding to a dual-stack/wildcard address, so behavior is consistent across
platforms instead of silently OS-dependent. This is a small, targeted,
plausible fix — but **cannot be verified without an actual FreeBSD box**, which
isn't available tonight. Don't ship this untested; get access to a FreeBSD VM
first (even a throwaway one) and reproduce the original bug before trusting
the fix.

**Still unknown**: which FreeBSD versions matter today (the original report
was FreeBSD 12.1, now itself EOL) — worth checking FreeBSD's own supported
-RELEASE list before committing to a target, same as the Asterisk version work
above.

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
