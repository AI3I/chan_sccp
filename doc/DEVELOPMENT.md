# Development

## Building

```
./configure --with-asterisk=/path/to/asterisk/prefix --with-asterisk-version=22.0
make -j4
make check
```

`--with-asterisk` points at an Asterisk install prefix with headers; without it
configure searches the usual system locations. Useful options:

- `--disable-debug`: leave out debug logging (`sccp debug` then has nothing to
  print). Build both ways before committing; code used only in debug builds
  must not break the other.
- `--enable-experimental-xml`: XML request support.
- `./configure --help` lists everything else.

`configure` and every `Makefile.in` are checked in. After changing
`configure.ac`, `autoconf/*.m4` or a `Makefile.am`, run `sh tools/bootstrap.sh`
(Autoconf 2.72, Automake 1.17, libtool 2.5.4) and commit only the changes
that are not whitespace.

## Tests

- `make check` (or `sh tools/run-tests.sh`) builds and runs the standalone
  tests for the CLI table renderer and the thread pool. It needs no Asterisk.
  For sanitizers:
  `TEST_CFLAGS="-fsanitize=address,undefined" TEST_LDFLAGS="-fsanitize=address,undefined" sh tools/run-tests.sh`
- CI (`.github/workflows/ccpp.yml`) builds default and optional-feature
  configurations against Asterisk 20 to 24, runs the tests with sanitizers,
  builds from a `make dist` archive, and builds once after regenerating the
  build system with `tools/bootstrap.sh`. CodeQL runs separately.
- Calls, registrations and phone text can only be tested against a running
  Asterisk with a phone or a phone simulator; see [STATUS.md](STATUS.md) for
  what has been checked that way.

## Installing on a running PBX

Asterisk maps `chan_sccp.so` into memory. `cp` over the loaded file writes into
that mapping and crashes Asterisk. `make install` is safe: it replaces the file,
and the running module keeps the old copy until it is unloaded. To switch to
the new build without restarting Asterisk:

1. End all SCCP calls, including held calls. A held call keeps the module in
   use, and `module unload` fails.
2. Copy the new module next to the old one under a temporary name.
3. `module unload chan_sccp.so`
4. `mv` the new file over the old one. A rename replaces the directory entry,
   not the file the old process mapped.
5. `module load chan_sccp.so`

After adding or changing an AMI action's documentation, also install the XML
documentation, then reload the docs and the module so the action picks them
up:

```
make chan_sccp-en_US.xml
cp chan_sccp-en_US.xml <asterisk data dir>/documentation/thirdparty/
asterisk -rx 'xmldoc reload'
asterisk -rx 'module reload chan_sccp.so'
```

## Code conventions

- **CLI and AMI.** A command is written once, as a handler taking
  `(fd, totals, s, m, argc, argv)`. `CLI_AMI_ENTRY` registers the CLI command.
  `SCCP_AMI_ACTION` registers the AMI action and maps its headers onto the
  CLI arguments (`"$Device"` becomes the `Device` header). Each action's
  documentation is a `/*** DOCUMENTATION` block in `sccp_cli.c`, which
  `tools/get_documentation` extracts at build time.
- **Console output.** Follow the Asterisk layout:
  - a short section title;
  - `Label:` in sentence case, with values aligned past the longest label;
  - `(not set)` for an unset option and `(none)` for an empty list, in tables too;
  - no banners, arrows, colors or advice.

  Use the `CLI_AMI_OUTPUT_*` macros and the table renderer, so the same code
  produces AMI fields.
- **Log messages.** Use `<device or session>: <what happened>; <what the
  system did>`, for example
  `SEP001122334455: registration refused: no such device in sccp.conf; token refused`.
  - State facts with enough detail to judge whether to investigate; don't
    add instructions.
  - Name the real `sccp.conf` option.
  - Mark internal misuse `(caller bug)`.
  - NOTICE is for normal refusals, WARNING for degraded service and ERROR
    for failures. Routine events go to debug output (`sccp_log`).
  - In prose write "call forward", "line device" and "softkey", not
    CamelCase.
- **Phone text.** Prompts and notifications are 32 bytes (31 characters on
  older phones); XML titles and prompts fit about 32 characters; softkey labels
  show 6 to 8. Label codes (`\200xx`) are translated by the phone. Escape any
  caller-supplied text placed in XML with `sccp_xml_escape()`.
- **Comments.** Comment why, not what: locking rules (`Locks:`),
  non-obvious protocol or Asterisk behavior, reasons for an ordering.
  Don't add tags (`\todo`, `\note`, `\deprecated`), change history or
  comments that restate the code. Keep file headers (authorship and license)
  and the `DOCUMENTATION` blocks.
- **Locking and references.** Release a lock before dropping the last
  reference to the object it protects. `AUTO_RELEASE` handles most
  references; `/*ref_replace*/` marks deliberate reference swaps.
