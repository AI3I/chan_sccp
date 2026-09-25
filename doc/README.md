# Documentation

| Document | Contents |
|---|---|
| [CLI.md](CLI.md) | Every `sccp` console command. |
| [AMI.md](AMI.md) | AMI actions, response format, events and permissions. |
| [UPGRADING.md](UPGRADING.md) | Moving from 4.x: renamed commands and actions, changed AMI fields, behavior and build changes. |
| [STATUS.md](STATUS.md) | What has been validated, what still needs real phones, known limitations. |
| [DEVELOPMENT.md](DEVELOPMENT.md) | Building, tests, installing on a running PBX, code and message conventions. |
| [history/](history/) | Working logs and the code review from the 5.0 cleanup, kept as a record. |

Configuration options are documented in the module itself: `sccp config
generate` writes every option with its default and description, and
[conf/sccp.conf.annotated](../conf/sccp.conf.annotated) is a copy of that
output.
