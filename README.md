# Sanguinius

Sanguinius is a deliberately small Discord bot built with modern C++ and
[D++](https://dpp.dev/). It supports only two prefix commands and writes every
message-create event it can see to an append-only text log.

## Commands

| Command | Description |
| --- | --- |
| `!help` | List the supported commands. |
| `!repo` | Link to this source repository. |

Command names are case-insensitive. Messages written by bots are logged but are
not treated as commands.

## Requirements

- A C++20 compiler
- CMake 3.25 or newer
- Ninja (for the supplied presets)
- D++ with its CMake package configuration installed

The locally built D++ 10.1.6 installation under `/usr/local` is discovered by
its exported `dpp::dpp` CMake target. If D++ is installed under a different
prefix, configure with `-DCMAKE_PREFIX_PATH=/path/to/prefix`.

The Discord application must have the **Message Content Intent** enabled in the
Developer Portal. The bot also needs permission to view channels, read message
history, and send messages wherever it is expected to operate. It can only log
messages Discord delivers to it.

## Configure

Provide the bot token through one of these environment variables:

```bash
export SANGUINIUS_TOKEN='your-token'
# Or keep the token in a permission-restricted file:
export SANGUINIUS_TOKEN_FILE="$HOME/.config/sanguinius/bot.token"
```

The start script uses `$HOME/.config/sanguinius/bot.token` automatically when
neither variable is set. The file should be readable only by its owner.

Optional settings are:

| Variable | Default | Purpose |
| --- | --- | --- |
| `SANGUINIUS_LOG_FILE` | `logs/messages.log` | Message log path. |
| `SANGUINIUS_COMMAND_PREFIX` | `!` | Command prefix (1–8 non-space characters). |

Do not commit tokens or environment files. A token exposed in source control
must be regenerated in the Discord Developer Portal.

## Build and run

```bash
./scripts/build_bot.bash --clean
ctest --test-dir build/release --output-on-failure
./build/release/sanguinius
```

The build script performs an incremental release build by default. Pass
`--clean` to clean the selected preset before compiling everything again. It
always prints the complete CMake configuration and verbose compiler/linker
output.

```bash
./scripts/build_bot.bash
./scripts/build_bot.bash --clean
./scripts/build_bot.bash debug
./scripts/build_bot.bash --clean debug
```

For routine background operation, the helper scripts preserve the process ID
and stop the bot cleanly:

```bash
./scripts/start_bot.bash
./scripts/stop_bot.bash
```

`start_bot.bash` expects the release binary and the token environment to
already be available. Console output goes to `logs/console.log`.

Discord gateway callbacks log each message and place command events onto a
dedicated worker queue. Command execution therefore cannot block D++ from
receiving and logging later messages. A future command that needs parallel
execution should manage that concurrency explicitly rather than blocking the
single command worker indefinitely.

## Message log

Each message is flushed as one line containing a UTC receipt timestamp,
message/guild/channel/author IDs, username, bot flag, message content, and any
attachment filenames and URLs. Control characters are escaped so one Discord
message always occupies one physical line. The bot restricts the message log to
its owning user. Example:

```text
2026-07-17T14:22:05Z message_id=123 guild_id=456 channel_id=789 author_id=42 author="user" bot=false content="hello\nworld"
```

The log contains user-generated content and identifiers. Restrict access,
establish a retention policy, and disclose the logging practice to server
members as required by your policies and applicable law.
