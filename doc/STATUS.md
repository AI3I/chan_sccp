# Project status

Updated 2026-09-25 for 5.0.0 (unreleased).

This fork continues [chan-sccp/chan-sccp](https://github.com/chan-sccp/chan-sccp)
for Asterisk 20 to 24. The 5.0 cleanup is finished in code:
- bug fixes;
- the supported-version floor;
- rewritten log, console, AMI and phone text;
- the reworked CLI and AMI command set;
- provisioning and support commands;
- graceful shutdown;
- removal of dead code and comments.

User-visible changes are in [NEWS](../NEWS) and [UPGRADING.md](UPGRADING.md).
The working log of the cleanup is kept in [history/](history/).

## How it was validated

- **Builds:** the hosted CI builds default and optional-feature configurations
  against Asterisk 20, 21, 22, 23 and 24, runs `make check` and sanitizer
  tests, and builds from a source archive. Local builds with `-Wall
  -Wformat=2`, with and without `--disable-debug`, produce no warnings.
- **Static analysis:** CodeQL on every push. GCC `-fanalyzer` findings
  that remain were reviewed and are false positives: the out-of-bounds
  argument reads in the AMI wrapper are guarded by `argc`.
- **Simulated phone:** a script registers as a Cisco 7965 (protocol 17)
  against Asterisk 22. It exercises every CLI command and AMI action, calls,
  hold and resume, answer, hang-up, reload, messages, softkey presses, device
  reset and re-registration, and graceful shutdown. The simulator is not part
  of this repository.
- **Production:** the PBX this fork was made for ran earlier builds. The 5.0
  changes have not been deployed there yet.

## Needs real phones

These parts compile and, where the simulator reaches them, behave as expected.
They still need a handset, a second phone, or a production load:

| Area | What to check |
|---|---|
| TLS (SCCPS) | Bad handshakes, stalled clients, reconnects. |
| Media | Video, dynamic RTP payload types, transcoding, paging, early media. |
| Handset features | Transfer and hold driven from the phone's own keys. |
| Provisioning | A phone booting from a file written by `sccp generate cnf`; `sccp push url` on a phone whose authentication URL allows pushes. |
| Token fallback | A refused phone retrying after the backoff period. |
| Phone text | Rendering of the rewritten prompts and XML screens on older (31-character) and newer displays. |
| Fallback scripts, odd/even fallback | Script output handling and parity enforcement under real registrations. |
| Network | Partial writes and fragmented TCP frames under load; the XML request library during module unload. |

## Known limitations

- Asterisk older than 20 is not supported; configure refuses it.
- Only Linux builds are tested. The socket code builds on FreeBSD, but no full
  install there has been tested.
- Configuration changes made with `sccp set device <device> <option>` and
  `sccp add line`/`remove line` last until the next reload.
- `sccp push url` works only if the phone's authentication URL accepts the
  push; the phone decides.
