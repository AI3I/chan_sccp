# Code review and cleanup plan

Reviewed revision: `20ea3fc1` on `main`.

This is a review, not an implementation change. The deployed RTP fix remains
untouched. Coverage includes the modern Asterisk adapter, media setup, TCP/TLS
transport, session framing, thread-pool lifecycle, event dispatch, selected
reference-count/configuration paths, optional web/XML code, build generation,
packaging, CI, and obsolete compatibility scaffolding. This is not an exhaustive
audit of every SCCP message handler or a full concurrency proof.

The main opportunities are to remove obsolete compatibility implementations,
make builds reproducible, and repair a few well-defined lifecycle boundaries.
A wholesale rewrite of the call state machine would introduce unnecessary risk.

Confirmed support policy from the user: keep Asterisk 20–24 and retire older
versions. The removal recommendations below use that scope. These removals
have not yet been implemented by this review.

## Findings, ordered by urgency

### R1 — High: thread-pool teardown can free storage used by live workers

Locations: `src/sccp_threadpool.c:121-126`, `:183-196`, `:314-348`.

Workers are created detached. The forced shutdown path cancels them, calls
`pthread_join()` anyway, ignores its result, and destroys the pool's conditions,
locks, and storage. A detached thread cannot be joined to establish completion.
Workers disable cancellation while executing jobs, so a long-running callback
can outlive this cleanup and return into freed storage. The cleanup callback
also needs the threads lock held by the forced shutdown path.

Even the normal path has a lifetime gap: a worker removes itself from the list,
unlocks it, and then signals `tp_p->exit`. The destroyer can observe an empty
list and free the pool before that final signal.

Validation: an isolated pthread probe on the PBX returned
`pthread_join(detached live worker)=22, worker_finished=0`. This confirms the
invalid wait mechanism; no live unload race was deliberately triggered.

Rework: use joinable workers with cooperative shutdown; stop producers, drain
or explicitly cancel owned jobs, join outside locks needed by workers, then
destroy storage. Make enqueue ownership/failure explicit. Add tests with a
blocked job and a concurrent shutdown. Evaluate Asterisk's thread-pool/task
facilities against these requirements before retaining a custom implementation.

### R2 — High, when fallback scripts are configured: stack overflow in script output

Location: `src/sccp_actions.c:602-617`.

The output buffer has 21 bytes. The loop advances the destination by
`strlen(output)` but repeatedly gives `snprintf` a capacity of 20, rather than
the capacity remaining from that destination. Two ordinary output chunks are
enough to write beyond the array. A script printing extra diagnostics can crash
or corrupt the PBX process.

Validation: copied the exact buffer sizes and append loop into a standalone
program, supplied two lines of 18 digits, and ran with AddressSanitizer. It
reported `stack-buffer-overflow`, `WRITE of size 19`, at the append operation.
This was not sent to a running Asterisk instance.

Fix: parse one bounded response line, reject excess/truncated output, and
define timeout/exit-status behavior. If multi-line output is required, track
remaining capacity correctly. This path also constructs a shell command from
arguments including the received device name (`:608-612`); prefer an argv-based
process launch. Exposure to untrusted names depends on configured device/ACL
and anonymous-device policy; remote command execution was not tested or assumed.

### R3 — High, with XML enabled: request cleanup tears down global library state

Locations: `src/sccp_xml.c:219-244`, `src/sccp_webservice.c:778`, `:832`.

`destroyDoc()` frees an individual document and then calls `xmlCleanupParser()`.
HTTP handlers call it after each response. Cleanup also runs when the module
unloads, even though other Asterisk components can still use libxml2.

The libxml2 API explicitly states that global cleanup is not thread-safe and
must not run while other threads can use the library. Per-document cleanup
therefore creates a process-wide concurrency hazard, independent of the earlier
stylesheet NULL-check fix. See the
[libxml2 cleanup contract](https://gnome.pages.gitlab.gnome.org/libxml2/html/parser_8h.html).

Fix: only free objects owned by the request/module. Remove per-request parser
cleanup and review `xsltCleanupGlobals()` in transformation and unload paths.
Do not have a loadable channel driver assume ownership of process-wide library
shutdown. Validate concurrent XML requests and module lifecycle in isolation.

### R4 — High for reproducibility: generated build files contradict their sources

Locations: `configure:2877-2878`, `configure.ac:9-10`,
`src/Makefile.am:12`, `.github/workflows/ccpp.yml:17-20`, `README.md:43-52`.

`configure.ac` declares a maximum group of 124, while the shipped `configure`
still declares 113 and lacks the newer version-selection code. The README and
build workflow invoke `./configure` directly. Updating m4 files does not update
that executable script, and maintainer mode is disabled by default.

There is a related packaging defect: explicit `DIST_SUBDIRS` stops at ast118,
omitting ast119 through ast124. New ast123/ast124 directories also have no
checked-in Makefile.in. A successful build from a manually bootstrapped server
tree does not establish that a fresh clone or release archive works.

Fix: choose one policy. Prefer bootstrapping Git checkouts explicitly, with
generated configure/Makefile.in files produced for release archives. Alternatively
commit regenerated files consistently. Test a clean checkout and a dist archive
against each supported header set. Remove the stale generated copies only as
part of that policy change, not in isolation.

### R5 — Medium: partial writes can interleave SCCP messages

Location: `src/sccp_session.c:1247-1265`.

The write mutex protects one transport `send()` call, not the whole message.
After a short write it is unlocked, allowing another thread to send a different
message before the first thread resumes. The resulting stream can contain
`A-prefix, B, A-suffix`, which violates SCCP framing. The comment promises
serialization that the loop does not provide.

Fix: serialize the entire frame, including retries, or use a single outbound
queue per session. Test with a fake transport that forces partial writes and
two concurrent producers; assert complete frames, not merely byte counts.
This is a static interleaving finding, not a reproduced live handset failure.

### R6 — Medium: receive errors and a full buffer trigger unnecessary disconnects

Location: `src/sccp_session.c:807-820`.

`result < 0 || (errno != EINTR || errno != EAGAIN)` always selects the failure
path for nonpositive reads: the errno disjunction is always true, and negative
results short-circuit it. Recoverable interruptions are treated as disconnects.

Separately, when a read exactly fills the receive buffer, the free-space term
in the condition becomes zero before `process_buffer()` is called. A buffer
containing complete valid coalesced frames is rejected without parsing it.

Fix: use explicit EOF/retry/fatal branches, parse received bytes first, and only
reject an unconsumable full buffer afterward. Keep TLS retry status separate
from POSIX errno. Test EINTR/EAGAIN, fragmented headers, and coalesced messages.

### R7 — Medium: read-format changes actually modify the write format

Locations: `src/pbx_impl/ast116/ast116.c:2714-2728`,
`src/sccp_channel.c:501-506`.

`sccp_astwrap_setReadFormat()` calls `ast_set_write_format()`. It never sets the
channel's read format. Its subsequent RTP-instance read-format setter does not
repair this for the `asterisk` RTP engine, which lacks that callback on the
inspected 22.8.2 build. Both format wrappers return success without checking the
channel setter result. Matching/default codecs can conceal this bug.

Fix: call the read setter, propagate failures, and distinguish channel translator
formats from RTP payload mappings. Test codec changes/transcoding in both
directions. Also split media selection from this API: video recalculation at
`sccp_channel.c:559-560` calls wrappers that hardcode the audio RTP instance.

### R8 — Medium, with TLS enabled: handshake failures are treated as success

Locations: `src/sccp_transport_tls.c:148-187`,
`src/sccp_session.c:979-1005`.

`SSL_accept()` returns a signed integer, but its result is assigned to
`unsigned long ssl_err`. A return of -1 becomes a large positive value and
passes `ssl_err <= 0`. A local probe confirmed this conversion and branch.
[OpenSSL documents the signed return and SSL_get_error handling](https://docs.openssl.org/3.5/man3/SSL_accept/).

The accept loop also ignores the transport's returned pointer. On a TLS failure
that returns NULL without assigning `out_sc`, the zero-initialized descriptor
passes `new_sc.fd < 0`, so processing continues with invalid connection state.
TLS read/write simply expose SSL return values to code expecting socket errno.

Rework: define one accept/error contract, initialize descriptors to -1, check
the returned result, and implement signed SSL status plus retry handling. Move
the handshake out of the serial listener or give it a bounded deadline. Review
SSL_CTX ownership: the current destroy path does not free `sslctx`. Test bad
handshakes, clean closure, stalled clients, and reconnects on a test listener.

### R9 — Medium: job allocation failure exits all of Asterisk

Location: `src/sccp_threadpool.c:265-285`.

Failure to allocate one queue entry calls `exit(1)` from the channel module.
This kills the whole PBX rather than rejecting that job. The API already has a
failure return, so unconditional process termination is unnecessary.

There is also a misleading success result during shutdown: jobqueue_add can
reject and free the queue node, but add_work still returns 1. Event dispatch
uses that return as ownership transfer (`sccp_event.c:349`), potentially leaving
the event/argument allocation unconsumed. Auto-answer also ignores enqueue
failure (`sccp_pbx.c:389`).

Fix: enqueue atomically against shutdown, return the actual accepted/rejected
result, and specify that the caller retains argument ownership on rejection.
Test allocator failure and concurrent shutdown without terminating Asterisk.

### R10 — Medium: the default test command is a false success

Locations: `Makefile.am:92-93`, `.github/workflows/ccpp.yml`,
`.github/workflows/codeql-analysis.yml:5-8`.

`make test` only prints `Success`. There are real embedded AST_TEST_DEFINE
tests, but this target runs none of them, and the CI build has test/distcheck
steps commented out. CodeQL push/PR filters target develop/master rather than
main (the scheduled job is separate). The build workflow does not bootstrap
the changed autotools inputs or exercise a header-version matrix.

Fix: replace the fake success with a real isolated test runner or an explicit
unsupported/failure result; wire the embedded tests into an Asterisk instance
with TEST_FRAMEWORK. Exercise default and optional-feature builds, supported
headers, source archives, and sanitizer tests. Update workflow actions and
branch filters as part of that work; this review did not inspect hosted CI logs.

### R11 — Medium, optional feature: odd/even fallback policy cannot work as written

Location: `src/sccp_actions.c:580-598`.

`deviceName[strlen(deviceName)]` reads the terminator, not the last identifier
digit. Moreover, sendAck starts TRUE and the odd/even branches only set it TRUE;
neither rejects a mismatch. Thus both modes default to acknowledging regardless
of intended partition. Merely subtracting one from the index is insufficient.

Fix: parse the last hexadecimal device-ID digit, assign the predicate's actual
result, and cover both parity outcomes plus malformed/empty names. Keep this
separate from the fallback-script memory fix.

### R12 — Low but useful: send error paths leak the owned message

Location: `src/sccp_session.c:1208-1209`, `:1233-1236`.

The function documents that it consumes/frees msg. The stopped-session and
message-ID mismatch early returns skip that cleanup; the ordinary invalid-fd
path correctly frees it. Repeated sends during teardown can leak messages.

Fix: one cleanup exit after ownership transfer, preserving the original error.
Test stopped sessions and invalid message metadata with leak detection.

## What can reasonably be discarded

| Candidate | Recommendation | Constraint |
|---|---|---|
| ast106/108/110/111/112/113/114/115 implementations and headers | Retire with their configure branches under the confirmed floor of 20 | 31,705 physical C/header lines; preserve history or a legacy tag |
| ast117–ast119 aliases | Remove with the obsolete version selectors | These are symlinks, not duplicated implementations; retain ast120 until modern build selection is consolidated |
| ast116 implementation | Keep, then rename to an honest shared modern adapter | All current modern wrappers depend on it; deleting it because of the name would break 20–24 |
| ast121–ast124 wrapper shims | Consolidate into a shared compatibility header/build target | The removed macro-accessor shims remain functional compatibility code |
| Answer-time RTP format fallback at ast116.c:1963-1977 | Remove the ineffective setter-only fallback after a focused call/paging check | Keep capability discovery where required; the static payload fix remains essential |
| Unreachable `#if 0` and commented-out implementations | Delete in mechanical, separately reviewable commits | Preserve useful protocol explanations and license notices; do not equate feature-gated code with dead code |
| `iXML.applyStyleSheet` legacy entry | Remove implementation and interface member after final reference/build check | Its only found call sites are commented out; it also frees a passed-by-value document without updating the caller |
| `testhtml` / `testxml` web handlers | Remove from normal builds or move to test-only code | Registered whenever the optional web service starts, not under TEST_FRAMEWORK; HTML test echoes unescaped request data |
| Experimental `sccp test` playground | Remove hazardous branches or move to a dedicated developer test module | `labels` dereferences d before NULL-checking and indexes argv beyond the entry argc check; it invokes a shell and logs credentials; `remove_reference` bypasses normal ownership |
| `libpbximpl.la` / pbx_impl.c compilation unit | Fold out the no-behavior archive and update build lists | Keep pbx_impl.h interface; source unit contains includes/file metadata, not wrapper logic |
| C++ parallel initializers/toolchain scaffolding | Remove if the project explicitly standardizes on C | Current .c targets use CC; verify no supported downstream C++ build first |
| Upstream-only Travis/Coverity notification configuration | Remove or replace with fork-owned CI | Avoid carrying inherited notification endpoints and project settings |
| Asterisk 1.6/1.8 backport patches and obsolete packaging recipes | Archive/remove with the new version floor | Inventory other contrib tools individually; screenshot/backtrace/config utilities can still be useful |
| Old CLI spelling/compatibility aliases | Low priority; retain cheap aliases until a documented deprecation | Their size is trivial compared with the risk of breaking scripts |

The C source inventory is about 82,066 lines excluding symlinks. Counts that
follow ast117–ast120 symlinks overstate the actual source duplication. The
31,705-line legacy figure includes those old adapters' headers and is not a
claim that all of those lines increase the loaded module's size: the build
selects one adapter.

## Rework order and acceptance checks

1. **Establish trustworthy builds/tests.** Resolve generated-file policy, fix
   distribution lists, correct CI branch selection, run clean checkout/archive
   builds and real tests. Preserve the current working module as a baseline.
2. **Repair contained defects.** Fallback output bounds, read-format setter,
   receive retry logic, message ownership, XML cleanup, and TLS accept contract.
   Add regression tests that fail on the current code. Keep each fix reviewable.
3. **Repair lifecycle and framing.** Thread-pool shutdown/ownership and complete
   message serialization. Use deterministic blocked jobs/short writes and
   sanitizer runs before integration tests.
4. **Prune legacy support.** With the confirmed floor of 20, remove old adapter
   implementations and configure branches together. Keep ast116's shared
   implementation until renamed and all references are migrated.
5. **Consolidate media and adapter APIs.** Give RTP lookup a signed failure
   result (the uint8_t wrapper at ast116.c:1630 currently loses -1), separate
   RX/TX mappings and audio/video selection, and replace silent no-op format
   setters. Extend payload validation to dynamic codecs/video; the deployed
   fix only covers six standard static audio mappings.
6. **Delete abandoned scaffolding.** Disabled implementations, test endpoints,
   developer-only CLI experiments, no-behavior build archives, and old CI files.

Integration checks should include outbound/inbound calls, early media,
ulaw/alaw/G722, transcoding, DTMF, hold/resume, transfer, paging, device reconnect,
and controlled module unload. Test dynamic codecs, video, TLS, and XML only if
they remain advertised features. Preserve Linux and FreeBSD socket behavior
unless platform support is explicitly changed.

Do not start by replacing reference counting, list/vector primitives, all
locks, or the entire call state machine. They are widely coupled. Encapsulate
ownership and add lifetime tests first; visual complexity alone is insufficient
evidence that a construct is dispensable.

## Validation and limits

The standalone probes are `/tmp/sccp-review-token-output.c` and
`/tmp/sccp-review-pthread.c` on the review machine and PBX. They were compiled
and run as independent programs on the PBX because the review workspace has
no C compiler. The token probe used `gcc -O0 -g -fsanitize=address`; its output
is `/tmp/sccp-review-token-output.log` on the PBX. No fault injection, reload,
test call, configuration change, or module replacement was performed during
this review.

Except for the explicitly described probes, findings are source-reviewed,
not newly reproduced in a full Asterisk instance. Prior successful RTP build
and handset validation do not validate optional features or concurrency.
The existing HEALTH_AUDIT.md records earlier work and should not be read as
a guarantee that no correctness bugs remain.


## Implementation handoff

See [CLI output progress and resume notes](CLI_OUTPUT_PROGRESS.md) for current
working-tree/deployment status, user decisions, and the latest ordered next
steps. The CLI/tone work is complete; the correctness findings above remain
open. The latest priority is build reproducibility/CI, followed by thread-pool
lifetime, TCP framing, read-format correction, legacy retirement, and optional
feature repairs. Keep that progress document updated as implementation proceeds.
