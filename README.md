# Sanguinius

Sanguinius is a Discord bot built with modern C++ and [D++](https://dpp.dev/).
It supports two prefix commands, answers messages that begin with a bot mention
through the OpenAI Responses API, and writes every visible message-create event
to an append-only text log.

## Commands

| Command | Description |
| --- | --- |
| `!help` | List the supported commands. |
| `!repo` | Link to this source repository. |

Command names are case-insensitive. Messages written by bots are logged but are
not treated as commands.

To talk to the AI persona, mention the bot at the start of a message:

```text
@sanguinius What is your favorite color?
```

The bot supplies the replied-to message, when present, plus up to eight recent
messages from the same channel as optional context. The persona is defined in
[`config/persona.txt`](config/persona.txt). Two AI workers process requests in
parallel, while a bounded queue prevents an influx of mentions from consuming
unlimited memory. Message logging and Discord event ingestion never wait for an
OpenAI request.

## Requirements

- A C++20 compiler
- CMake 3.25 or newer
- Ninja (for the supplied presets)
- D++ with its CMake package configuration installed
- libcurl
- nlohmann-json

On CachyOS/Arch Linux, install the dependencies supplied by the distribution:

```bash
sudo pacman -S --needed cmake ninja gcc curl nlohmann-json
```

D++ must still be installed separately, as described above. CMake links the
well-established libcurl HTTP client and header-only nlohmann-json parser; no
OpenAI-specific third-party SDK is required.

The locally built D++ 10.1.6 installation under `/usr/local` is discovered by
its exported `dpp::dpp` CMake target. If D++ is installed under a different
prefix, configure with `-DCMAKE_PREFIX_PATH=/path/to/prefix`.

The Discord application must have the **Message Content Intent** enabled in the
Developer Portal. The bot also needs permission to view channels, read message
history, and send messages wherever it is expected to operate. It can only log
messages Discord delivers to it.

## Configure

Provide the Discord bot token through one of these environment variables:

```bash
export SANGUINIUS_TOKEN='your-token'
# Or keep the token in a permission-restricted file:
export SANGUINIUS_TOKEN_FILE="$HOME/.config/sanguinius/bot.token"
```

The start script uses `$HOME/.config/sanguinius/bot.token` automatically when
neither variable is set. The file should be readable only by its owner.

### OpenAI API key and billing

ChatGPT subscriptions and OpenAI API usage are
[billed separately](https://help.openai.com/en/articles/8156019); a ChatGPT
Plus/Pro subscription does not fund API calls. Create a secret key from the
[OpenAI API keys page](https://platform.openai.com/api-keys), configure API
billing, and store the key locally:

```bash
install -d -m 700 "$HOME/.config/sanguinius"
read -rsp 'OpenAI API key: ' sanguinius_openai_key
printf '\n'
printf '%s\n' "${sanguinius_openai_key}" > "$HOME/.config/sanguinius/openai.key"
unset sanguinius_openai_key
chmod 600 "$HOME/.config/sanguinius/openai.key"
```

Do not use a ChatGPT browser/session token. The start script reads
`$HOME/.config/sanguinius/openai.key` by default and exports its contents as
`OPENAI_API_KEY` to the bot process. You may instead provide the key directly
for the current process:

```bash
export OPENAI_API_KEY='your-api-key'
```

The default model is `gpt-5.4-nano`, selected for low API cost. It does not have
a free API tier. Review its [current model pricing and rate
limits](https://developers.openai.com/api/docs/models/gpt-5.4-nano) and set usage
limits in the OpenAI platform before running the bot.

Optional settings are:

| Variable | Default | Purpose |
| --- | --- | --- |
| `SANGUINIUS_LOG_FILE` | `logs/messages.log` | Message log path. |
| `SANGUINIUS_COMMAND_PREFIX` | `!` | Command prefix (1–8 non-space characters). |
| `SANGUINIUS_OPENAI_API_KEY_FILE` | Set by start script | OpenAI API key file. |
| `SANGUINIUS_OPENAI_MODEL` | `gpt-5.4-nano` | Responses API model. |
| `SANGUINIUS_PERSONA_FILE` | `config/persona.txt` | Plaintext persona instructions. |

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

`start_bot.bash` expects the release binary and the two default credential files
described above, unless their environment overrides are set. Console output
goes to `logs/console.log`.

Discord gateway callbacks log each message and place command events onto a
dedicated worker queue. Command execution therefore cannot block D++ from
receiving and logging later messages. A future command that needs parallel
execution should manage that concurrency explicitly rather than blocking the
single command worker indefinitely.

AI mentions use a separate two-thread worker pool. Each OpenAI request has a
connection timeout and an overall timeout, and responses are truncated safely
below Discord's message-size limit. Reply-context lookup requires the bot's
**Read Message History** permission. If context retrieval fails, the bot still
answers using the triggering message and does not expose API credentials or raw
API responses. Responses API requests set `store` to `false` so generated
responses are not retained for later retrieval through the API.

## Message log

Each message is flushed as one line containing its receipt time in the
`America/New_York` timezone, the author's username, and the message text. The
timezone automatically follows daylight-saving transitions. Control characters
are escaped so one Discord message always occupies one physical line. The bot
restricts the message log to its owning user. Example:

```text
2026-07-17T10:22:05-04:00 author="user" message="hello\nworld"
```

The log contains user-generated content and usernames, and AI prompts send a
small amount of recent Discord conversation to OpenAI. Restrict access,
establish a retention policy, and disclose both practices to server members as
required by your policies and applicable law.
