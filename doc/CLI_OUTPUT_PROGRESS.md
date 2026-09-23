# SCCP CLI output cleanup — progress and resume notes

Updated: 2026-09-22 (PBX clock crossed into September 23). Base commit: `20ea3fc1`.

## Agreed scope

- Validate and improve all `sccp show` output: headings, column widths,
  truncation, alignment, and leading blank lines.
- Add `sccp show tones`, mapping the driver's tone IDs in hex to their names.
- AMI field compatibility is not a constraint; the user confirmed no current
  AMI consumers. Existing identifiers are retained where a rename adds no value.
- Save progress here for review/resumption. No commit/push requested for this
  batch yet.
- Separate review: `doc/CODE_REVIEW_2026-09-22.md`; supported Asterisk scope is
  20–24, with older support to be retired in later cleanup work.

## Implemented

- New `src/sccp_cli_table_data.[ch]`: snapshots formatted cells, measures the
  longest actual value/header, and renders aligned tables without clipping.
  Uses thread-local UTF-8 locale for wide/combining characters, handles invalid
  bytes, normalizes terminal controls, and reports incomplete/OOM snapshots.
- Shared `sccp_cli_table.h` now collects each row once rather than streaming
  with unrelated hard-coded widths. Removed two leading newlines; empty tables
  explicitly say `(no entries)`.
- Readable headings/titles across device details, sessions, channels, hints,
  MWI, refcounts, conferences, softkeys, and statistics.
- `show lines` converted from an independent fixed-width box to measured
  columns; line settings remain visible in a separate table. Missing labels
  use `--`, and each shared-line row repeats the line ID.
- Larger buffers for IPv6 addresses including ports.
- Removed leading padding from global/device/line settings titles.
- `show tones` enumerates `skinny_tone_exists()` / `skinny_tone2str()` from the
  generated protocol enum, avoiding a duplicate tone list.
- Standalone renderer regression test: `tools/test_cli_tables.c`.

## Validation

- Standalone renderer tests passed with `-Wall -Wextra -Werror` and ASan/UBSan:
  long strings/numbers, wide/combining Unicode, invalid bytes, terminal controls,
  IPv6 with port, empty/incomplete/failed snapshots, and 1,000-row allocation growth.
- Default Asterisk 22.8.2 build passed using the checked-in `Makefile.in` updates.
- Separate build with conference, video, and experimental XML enabled passed.
  Existing unrelated prototype and enum warnings remain. Both CLI and AMI
  branches compile; AMI is disabled on this PBX, so no live AMI protocol test.
- Initial live validation: 18 command variants, 25 tables, no leading blank
  lines or rendering errors. Every one of the 93 hex/name tone rows matched
  `src/sccp_enum.in` exactly. Refcount's `Hash Table Usage:` title initially
  triggered the capture script's overly broad `Usage:` detector; no command failed.
- Four phones re-registered after module replacement; Asterisk was not restarted.
- Follow-up presentation fixes: use `Unknown` for undefined models, `--` for
  absent call parties/codecs, readable attached-device title, corrected hint
  subscription help, and stable registration timestamps.
- Live captures are in `/tmp/sccp-cli-validation` locally and on the PBX.
  They contain configuration details; keep them out of Git.
- No new active-call audio test was performed. The working RTP fix is retained.
- Builds against Asterisk 20, 21, 23, and 24 have not been run in this pass.
  The supported range remains 20–24; legacy retirement is separate work.

## Build and rollback locations

- Working remote build: `/usr/src/chan-sccp-cli-format` on `root@pbx.jdlewis.net`.
  Copied from `/usr/src/chan-sccp-rtp-payload-fix` to preserve earlier builds.
- Final build log: `/tmp/sccp-cli-final-build.log` on PBX.
- Optional-feature build: `/usr/src/chan-sccp-cli-format-options`; logs
  `/tmp/sccp-cli-options-{config,build}.log`.
- Generated template for comparison: `/tmp/sccp-cli-Makefile.in` locally.
  Automake differs (1.16.4 vs 1.17); avoid importing unrelated template churn.
- Known working RTP build: `/usr/src/chan-sccp-rtp-payload-fix`.
- Existing pre-RTP-fix backup:
  `/root/chan_sccp_backups/chan_sccp-before-payload-fix.so`.
  Current working RTP module was separately backed up before CLI deployment:
  `/root/chan_sccp_backups/chan_sccp-before-cli-format.so`
  (SHA-256 `a96b0287b23d085479bf82641e8c45e1181997f960c58480aede23ce073cd924`).
- Never overwrite the loaded `.so` in place: stage a new file and rename it
  atomically, verify zero active channels, unload/load SCCP, and verify phones.

## Status and next steps

- CLI cleanup and `sccp show tones` are implemented and deployed. Final expanded
  validation passed: 23 command captures, 51 tables, 93 exact tone mappings,
  and all four phones registered. No leading blank lines or rendering errors.
- Installed module SHA-256:
  `a191cad78c4905dd2f9972a0708bcf5bb87345e01feb05c64f0486797645bb73`.
- Changes are uncommitted and unpushed. Base/remote commit remains `20ea3fc1`.
- Review this batch, then commit/push when requested.
- Resume the broader review using the ordered handoff below. None of that
  broader cleanup has been bundled into the CLI patch.

Standalone test command (from repository root, where a compiler is available):

```sh
cc -D_GNU_SOURCE -Isrc -Wall -Wextra -Werror -fsanitize=address,undefined \
  tools/test_cli_tables.c src/sccp_cli_table_data.c -o /tmp/test_cli_tables
/tmp/test_cli_tables
```

The local workspace has no C compiler; use the isolated remote build/test
directory. Do not run concurrency/fault-injection probes inside live Asterisk.

## Tone label follow-up

- Normalized 50 tone descriptions in `src/sccp_enum.in`, the source for
  generated name mappings used by CLI, logging, and configuration metadata.
- DTMF symbols now use `*` and `#`; fixed Barge In, Precedence Ringback,
  Preemption Tone, Off-Hook and Camp-On spacing; normalized title case,
  `Hz`, MF spacing, MLPP acronyms, MeetMe and PIN capitalization.
- Updated the matching lists in `conf/sccp.conf.annotated`.
- All 93 numeric protocol values are unchanged. Renamed the misspelled
  `SKINNY_TONE_BARGIN` symbol to `SKINNY_TONE_BARGE_IN` at the user’s request.
- Ask before changing unfamiliar phone terminology; retain protocol acronyms.
- Caught a stale absolute source path in the copied remote build: enum generation
  read `/usr/src/chan-sccp-rtpfix2`. Reconfigure in the actual build directory
  before regenerating enums; a successful build alone did not catch stale names.

- User confirmed `0x66` means **Preemption Warning**; use that description
  and rename `SKINNY_TONE_PREAMPWARN` to `SKINNY_TONE_PREEMPTION_WARNING`.

- Reconfigured and rebuilt successfully; final live output matches all 93
  source mappings exactly, including `0x66 Preemption Warning`.
- Pre-label backup: `/root/chan_sccp_backups/chan_sccp-before-tone-labels.so`.
- Build log: `/tmp/sccp-tone-label-build.log`.

### Cisco references

- [Cisco CME MLPP guide, MLPP Announcements table](https://www.cisco.com/c/en/us/td/docs/voice_ip_comm/cucme/admin/configuration/manual/cmeadm/cmemlpp.html):
  defines BPA, BNEA, ICA, UPA, and VCA and describes precedence/preemption tones.
  It does not establish the complete SCCP hex-ID mapping.
- [Cisco CallManager 4.1 troubleshooting](https://www.cisco.com/en/US/docs/voice_ip_comm/cucm/trouble/4_1_3/tb413b.html):
  traces identify Inside Dial Tone as decimal 33 (`0x21`).
- No complete public Cisco 7900-series hex tone table was located in this search.
  Preserve unresolved terms, including PALA and DT Monitor, until verified.

## Resume handoff — agreed priorities

The user explicitly requested ongoing Markdown records so context survives
between sessions. Update these notes after each meaningful change, validation,
deployment, or decision; distinguish completed work from proposed work.

Current checkpoint: RTP fix committed/pushed at `20ea3fc1`; CLI/table/tone work
is deployed and validated but still uncommitted locally. No broader review
fixes have been implemented yet. Preserve this working tree when resuming.

Next work, in the latest discussed order:

1. Checkpoint the CLI/tone batch, then address build reproducibility and real CI
   (review R4/R10): fresh checkouts and release archives, Asterisk 20–24,
   consistent generated files, correct source paths, and meaningful tests.
2. Thread-pool lifetime and enqueue ownership (R1/R9), including removal of
   process-wide `exit(1)` on queue allocation failure.
3. TCP message serialization and receive handling (R5/R6).
4. Read-format setter and error propagation (R7).
5. Retire pre-20 adapters/configure branches after supported builds are proven.
   The ast116 directory contains the shared modern implementation: do not
   delete it merely because its name looks obsolete.
6. Optional-feature defects: fallback-script overflow/policy (R2/R11), TLS
   handshake/retry handling (R8), and XML global cleanup (R3).

Use `CODE_REVIEW_2026-09-22.md` for evidence and detailed fixes. Keep changes
reviewable and validate concurrency/fault cases outside the running PBX.
For each batch record files/behavior changed, commands and outcomes, remaining
limits, module hash/backup if deployed, commit/push status, and next action.

Standing constraints: retain Asterisk 20–24; AMI field names may change;
ask about uncertain phone terminology; preserve protocol numeric IDs during
label cleanup. The latest deployment was checked for all 93 tone mappings and
four phone registrations, but did not include a new active-call audio test.

## Complete review finding tracker

All findings remain open unless explicitly marked otherwise here. This table
tracks implementation; the linked review contains locations, evidence, limits,
and proposed validation. The short priority list is not the complete backlog.

| ID | Severity | Finding | Status |
|---|---|---|---|
| R1 | High | Thread-pool teardown can free live-worker storage | Open |
| R2 | High, conditional | Fallback-script output stack overflow | Open |
| R3 | High, conditional | XML request/unload cleanup destroys global library state | Open |
| R4 | High | Generated build files and distribution lists contradict sources | Open; copied-build source-path issue corrected locally on PBX, repository policy still unresolved |
| R5 | Medium | Partial writes can interleave SCCP frames | Open |
| R6 | Medium | Recoverable reads/full receive buffers cause disconnects | Open |
| R7 | Medium | Read-format setter modifies write format; failures ignored | Open |
| R8 | Medium, conditional | TLS handshake/accept and retry contracts are broken | Open |
| R9 | Medium | Queue allocation exits Asterisk; rejected jobs report success | Open |
| R10 | Medium | Fake-success test target and stale CI configuration | Open; standalone CLI tests added, general test/CI repair still pending |
| R11 | Medium, conditional | Odd/even fallback policy does not enforce parity | Open |
| R12 | Low | Send error paths leak owned messages | Open; include with session/ownership fixes |

Additional recommendations remain tracked in the review's **What can reasonably
be discarded** table and **Rework order and acceptance checks** section:

- Legacy adapter/configure/packaging removal and modern wrapper consolidation.
- Ineffective answer-time RTP fallback; signed RTP lookup failure handling;
  audio/video API separation and dynamic-codec validation.
- Dead/commented code, legacy XML stylesheet entry, test web handlers, and
  hazardous experimental CLI branches.
- Empty adapter archive, conditional C++ scaffolding retirement, inherited CI
  configuration, and obsolete backport patches.
- Cheap CLI aliases: retain for now, per the review recommendation.

These are recommendations, not completed removals or separately severity-rated
findings. Preserve their constraints and acceptance checks when scheduling work.
