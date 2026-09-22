## Chan_SCCP

A Skinny Client Control Protocol (SCCP) channel driver for Asterisk, letting
Cisco 79xx/89xx/99xx-series phones register directly with Asterisk without a
CUCM. This is a fork of [chan-sccp/chan-sccp](https://github.com/chan-sccp/chan-sccp)
(itself descended from the original chan_sccp driver by Zozo/Sergio
Chersovani), maintained here for active use, with an ongoing health/cleanup
pass tracked in [HEALTH_AUDIT.md](HEALTH_AUDIT.md) - real bug fixes, message
and documentation quality, and Asterisk 20-24 support that upstream never
picked up.

Chan_SCCP is free software; see [COPYING](COPYING) for the license.

### Prerequisites

- A C compiler: gcc >= 4.6 or clang >= 3.6 (newer strongly preferred)
- GNU make
- Libraries: libxml2-dev, libxslt1-dev, gettext, libssl-dev (Debian/Ubuntu
  package names; adjust for other distros, e.g. libxml2-devel on RPM-based
  systems)
- Asterisk with source headers and debug symbols installed
  (asterisk-dev/asterisk-dbg or asterisk-devel/asterisk-debug-info depending
  on distro) - see the version matrix below for what's actually supported
- `chan_skinny` disabled in `/etc/asterisk/modules.conf` (it will otherwise
  fight chan_sccp for the same SCCP port)
- Standard POSIX tools: sed, awk, tr

### Asterisk version support

Real per-version support, not just "whatever happens to compile":

| Version | Status |
|---|---|
| 20 | Supported (LTS) |
| 21 | Supported (Standard) |
| 22 | Supported (LTS) - what this fork is primarily developed/tested against |
| 23 | Supported (Standard) |
| 24 | Supported (LTS, released Oct 2026) |
| 16-19 | Not actively maintained here; older `ast1xx` wrapper code is still present but unverified against current headers |

### Building from source

```
git clone https://github.com/AI3I/chan_sccp.git
cd chan_sccp
./configure
make -j2 && make install && make reload
```

Run `./configure --help` for the full list of configure flags. If you edit
`configure.ac`, anything under `autoconf/`, or any `Makefile.am`, regenerate
the build system with `autoreconf -fi` before rebuilding.

### Required Asterisk modules

Make sure these are loaded before loading `chan_sccp`:
- app_voicemail
- bridge_simple
- bridge_native_rtp
- bridge_softmix
- bridge_holding
- res_stasis
- res_stasis_device_state

### License

GPL - see [COPYING](COPYING) / [LICENSE](LICENSE).
