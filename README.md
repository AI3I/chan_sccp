# chan_sccp

**Cisco SCCP (Skinny) phones on Asterisk, without a CallManager.**

[![Build and test](https://github.com/AI3I/chan_sccp/actions/workflows/ccpp.yml/badge.svg)](https://github.com/AI3I/chan_sccp/actions/workflows/ccpp.yml)
[![CodeQL](https://github.com/AI3I/chan_sccp/actions/workflows/codeql-analysis.yml/badge.svg)](https://github.com/AI3I/chan_sccp/actions/workflows/codeql-analysis.yml)
![Asterisk 20-24](https://img.shields.io/badge/Asterisk-20%E2%80%9324-orange)
[![License: GPL v2+](https://img.shields.io/badge/license-GPL%20v2%2B-blue)](COPYING)

chan_sccp is an Asterisk channel driver that lets Cisco 79xx, 89xx and 99xx
phones running SCCP firmware register directly with Asterisk. This is an
actively maintained fork of [chan-sccp/chan-sccp](https://github.com/chan-sccp/chan-sccp),
brought up to date for Asterisk 20 through 24.

> **Upgrading from 4.x?** CLI commands and AMI actions were renamed in 5.0 with
> no aliases. Read [doc/UPGRADING.md](doc/UPGRADING.md) before you install.

## Features

- **Lines and buttons:** multiple and shared lines, speed dials with BLF
  (hint) lamps, feature buttons, service URLs, addon modules.
- **Calling:** hold, transfer (including direct transfer), call forward
  (all, busy, no answer), do-not-disturb, call pickup and group pickup,
  parking, barge, auto-answer, paging, voicemail indication (MWI).
- **Conferencing** (`--enable-conference`): ad hoc conferences with a
  participant list and moderator controls on the phone.
- **Provisioning:** `sccp generate cnf` writes a phone's `SEP<MAC>.cnf.xml`
  from `sccp.conf`; `sccp push url` opens an XML service on a phone.
- **Operations:** a full CLI and AMI command set, per-device debug output,
  per-call quality history reported by the phones, firmware inventory, and
  graceful shutdown that waits for calls in progress.
- **Also:** TLS signalling, realtime configuration, hotline (guest) devices,
  dialplan functions, and video (`--enable-video`, experimental).

## Supported Asterisk versions

| Asterisk | Status |
|---|---|
| 20 (LTS) | Supported |
| 21 | Supported |
| 22 (LTS) | Supported; the version this fork is developed and tested on |
| 23 | Supported |
| 24 (LTS) | Supported |
| 19 and older | Not supported; `configure` refuses them |

Every version is built and tested in CI.

## Quick start

### 1. Install the prerequisites

- Asterisk 20-24 with its development headers (from source, or your
  distribution's `asterisk-dev` / `asterisk-devel` package)
- gcc or clang, GNU make
- libxml2, libxslt, gettext and OpenSSL development packages. On
  Debian/Ubuntu: `libxml2-dev libxslt1-dev gettext libssl-dev`; on
  RHEL/Fedora: `libxml2-devel libxslt-devel gettext-devel openssl-devel`

### 2. Build and install

```sh
git clone https://github.com/AI3I/chan_sccp.git
cd chan_sccp
./configure            # add --with-asterisk=/path/to/prefix if Asterisk is not in /usr
make -j4
make check
sudo make install
```

`make install` puts `chan_sccp.so` in the Asterisk modules directory and the
AMI/CLI documentation in Asterisk's documentation directory. It also installs
an example `sccp.conf`, only if you don't have one. `./configure --help` lists
the build options.

### 3. Prepare Asterisk

In `modules.conf`, stop `chan_skinny` from loading (it listens on the same
port):

```ini
noload => chan_skinny.so
```

chan_sccp needs these modules, which a standard Asterisk loads:
`bridge_simple`, `bridge_native_rtp`, `bridge_softmix`, `bridge_holding`,
`res_stasis` and `res_stasis_device_state`. `app_voicemail` is optional and
enables voicemail indication.

### 4. Configure a phone

A minimal `/etc/asterisk/sccp.conf` with one phone and one line:

```ini
[general]
bindaddr = 0.0.0.0
port = 2000
context = default
deny = 0.0.0.0/0.0.0.0
permit = internal          ; private (RFC 1918) networks

[SEP001122334455]          ; SEP + the phone's MAC address
type = device
devicetype = 7960
description = Front desk
button = line, 1000

[1000]
type = line
cid_name = Front Desk
cid_num = 1000
context = default
```

The phone finds Asterisk through its TFTP configuration file.
`sccp generate cnf SEP001122334455` writes one, and you place it on your TFTP
server. `conf/sccp.conf.annotated` lists every option with its default.

### 5. Load and check

```
asterisk -rx "module load chan_sccp.so"
asterisk -rx "sccp show devices"
```

A registered phone shows status `OK` and its registration time, and the
summary line counts it as registered. `sccp show device SEP001122334455` shows
its buttons, lines and connection.

## Updating a running system

Never `cp` a new `chan_sccp.so` over the loaded one: Asterisk has the file
mapped and will crash. `make install` is safe because it replaces the file. The
running Asterisk keeps the old module until you unload it and load it again
(held calls must end first), or until Asterisk restarts. See
[doc/DEVELOPMENT.md](doc/DEVELOPMENT.md#installing-on-a-running-pbx).

## Documentation

| | |
|---|---|
| [doc/CLI.md](doc/CLI.md) | Every `sccp` console command |
| [doc/AMI.md](doc/AMI.md) | AMI actions, events and permissions |
| [doc/UPGRADING.md](doc/UPGRADING.md) | Moving from 4.x |
| [doc/STATUS.md](doc/STATUS.md) | What is validated, what still needs real-phone testing, known limitations |
| [doc/DEVELOPMENT.md](doc/DEVELOPMENT.md) | Building, tests, conventions for code and messages |
| [NEWS](NEWS) | Release notes |

Inside Asterisk, `help sccp <command>` and `manager show command <action>` show
the same reference.

## Troubleshooting

- **Phone never registers:** check `sccp show sessions all` to see whether it
  connects at all. Then check the Asterisk log: refusals name the reason (the
  device isn't in `sccp.conf`, or the `deny`/`permit` rules block its address).
- **Registered, but no line buttons:** the device's `button = line, ...`
  entries must name existing line sections.
- **One phone's details:** `sccp set device <device> debug on` together with
  `sccp debug device channel` limits debug output to that phone.

## Contributing

Issues and pull requests are welcome at
[AI3I/chan_sccp](https://github.com/AI3I/chan_sccp).
[doc/DEVELOPMENT.md](doc/DEVELOPMENT.md) covers building, the tests, and the
conventions for log and console messages.

## Credits and license

chan_sccp began as Sergio Chersovani's (Zozo) driver, derived from
chan_skinny. It grew into Chan-SCCP-b under Marcello Ceschia, Diederik de Groot
and many contributors; see [AUTHORS](AUTHORS) and the
[legacy project](https://github.com/chan-sccp/chan-sccp).

Released under the GNU General Public License, version 2 or (at your option)
any later version; see [COPYING](COPYING) and the full text in
[LICENSE](LICENSE).
