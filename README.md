# proto

[![CI](https://github.com/ech00f/proto/actions/workflows/ci.yml/badge.svg)](https://github.com/ech00f/proto/actions/workflows/ci.yml)

[Документация на русском](README.ru.md)

Copilot-style inline command autocomplete, written in C++20 for Linux and macOS.
Suggestions appear as gray ghost text while you type; the model never executes
anything. Uses an OpenAI-compatible API and defaults to a local Ollama model.

![Example terminal with an inline git status suggestion](docs/terminal.svg)

Network requests run on a background thread, are debounced, and are cancelled
when you edit the line. Stale replies cannot overwrite newer input. Local path,
common-command, and proto-history completions still work when the AI is unavailable.

## Build

Requires a C++20 compiler, CMake 3.20+, libcurl 7.68+, and nlohmann/json 3.10+.
Python 3.9+ enables the HTTP/terminal integration tests; no Python packages are needed.

**Ubuntu / Debian:**

```sh
sudo apt install build-essential cmake ninja-build pkg-config libcurl4-openssl-dev nlohmann-json3-dev
```

**Arch Linux:**

```sh
sudo pacman -S --needed base-devel cmake ninja pkgconf curl nlohmann-json python
```

**macOS (Homebrew):**

```sh
brew install cmake ninja pkg-config curl nlohmann-json
export PKG_CONFIG_PATH="$(brew --prefix curl)/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export CMAKE_PREFIX_PATH="$(brew --prefix)"
```

Then, from the repository:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Optional user-local installation (does not change your shell configuration):

```sh
cmake --install build --prefix "$HOME/.local"
```

## Run

### Local model — no API key

Install [Ollama](https://ollama.com/), then start its server in a separate terminal
if the Ollama service is not already running:

```sh
ollama serve
```

In your working terminal:

```sh
ollama pull qwen2.5-coder:7b
./build/proto
```

For a lighter model, first pull it and set `PROTO_MODEL=qwen2.5-coder:1.5b`.
Cold model loads or CPU-only inference may need `PROTO_TIMEOUT_MS=15000`.
Ollama is a separate dependency: proto does not download models or start servers automatically.

### OpenAI-compatible cloud API

```sh
export PROTO_API_BASE=https://api.openai.com/v1
export PROTO_MODEL=gpt-4.1-mini
# Supply PROTO_API_KEY through your environment or secret manager.
./build/proto
```

Set `PROTO_API_KEY` before starting proto if your provider requires authentication.
Do not put real keys in Git or shell history. The API base is the provider's base
URL (usually ending in `/v1`); proto appends `/chat/completions`.
LM Studio and other compatible local servers work too: configure their base URL
and loaded model name. HTTP is accepted only for `localhost`, `127.0.0.1`, or `[::1]`;
remote providers require HTTPS. Redirects are not followed.

### Offline / scripting

```sh
./build/proto --no-ai
./build/proto --complete 'git st'
./build/proto --no-ai --complete 'cd ~/Doc'
./build/proto --context
```

`--complete` prints a complete command, not just its suffix, and **never executes it**.
No suggestion means empty stdout. Exit codes: `0` success/no suggestion, `1` invalid
arguments/configuration, `2` an AI failure with no local fallback. Interactive mode
requires a TTY and exits with the requested `exit` status or the last command's status.

## Configuration

Settings are read from the environment at startup. `.env.example` documents the
defaults; proto does **not** automatically read `.env` or shell startup files.

| Variable | Default | Meaning |
| --- | --- | --- |
| `PROTO_API_BASE` | `http://127.0.0.1:11434/v1` | OpenAI-compatible API base URL |
| `PROTO_MODEL` | `qwen2.5-coder:7b` | Provider's model identifier |
| `PROTO_API_KEY` | empty | Optional bearer key; not inherited by executed commands |
| `PROTO_CONTEXT` | `home` | `none`: no directories; `cwd`: current directory; `home`: cwd + home directories |
| `PROTO_MAX_ENTRIES` | `80` | Maximum names per directory list; range 1–200 |
| `PROTO_DEBOUNCE_MS` | `250` | Quiet period before a request; range 0–5000 ms |
| `PROTO_TIMEOUT_MS` | `5000` | Request timeout; range 100–30000 ms |
| `PROTO_SEND_HISTORY` | `0` | Opt in to sending up to 8 recent non-sensitive proto commands |
| `PROTO_NO_HISTORY` | `0` | `1` disables disk history; session-local history still works |
| `PROTO_SHELL` | `$SHELL` or `/bin/sh` | Absolute executable path used for commands |

Boolean settings accept `0`/`1` or `false`/`true`.

## Privacy and safety

- The typed prefix and shell name are sent to the **configured** model endpoint.
- Default directory context contains only visible cwd names and top-level home
  directory names. There is no recursive scan, file-content upload, environment
  upload, or bash/zsh history import. Listings are bounded and sorted.
- Hidden/private directories, symlinks, and obvious secret/recovery names are
  excluded from AI metadata. Home paths are abbreviated to `~`.
- Commands containing recognizable keys, passwords, auth headers, URL credentials,
  or credential flags are not sent to the model or persisted in history.
- These filters are **best-effort**, not a guarantee that arbitrary secrets can be
  recognized. Even file names can be private: inspect `--context`, use
  `PROTO_CONTEXT=none`, choose a local model, or use `--no-ai` as appropriate.
- Proto history lives at `$XDG_STATE_HOME/proto/history` (otherwise
  `~/.local/state/proto/history`), with directory mode `0700` and file mode `0600`.
  Symlinked/hard-linked history is refused. Disk history stops growing at 1 MiB;
  up to 500 entries are kept in memory. Busy/unwritable history falls back to memory.
- Model output must preserve your prefix and contain only printable, single-line
  UTF-8. Terminal escapes, multiline commands, and changed prefixes are discarded.
- **Suggestions are untrusted. Review them before pressing Enter.** Tab only inserts
  text; Enter executes only the actual input, never an unaccepted suggestion.
- Cloud providers may charge per request and retain submitted context according
  to their own policies. Nothing contacts a cloud API unless you configure one.

## Keys and commands

| Key | Action |
| --- | --- |
| Tab / Right at end of input | Accept the full suggestion |
| Ctrl+Right / Alt+Right | Accept the next word |
| Enter | Execute the input, **without** accepting ghost text |
| Esc | Dismiss the suggestion until the next edit |
| Up / Down | Browse proto's own history |
| Left / Right, Home / End | Move the cursor |
| Ctrl+A / Ctrl+E | Start / end of line |
| Ctrl+U / Ctrl+K / Ctrl+W | Delete before cursor / after cursor / previous word |
| Ctrl+C | Cancel input or interrupt a running foreground command |
| Ctrl+D | Exit an empty prompt |
| Ctrl+L | Clear the screen |

UTF-8 text, wide characters, window resizing, and bracketed paste are supported.
Newlines in bracketed paste become spaces; pasting never submits the line.

Commands run through `$SHELL -c` (override with `PROTO_SHELL`). Simple `cd`, `cd -`,
`exit [status]`, `jobs`, and `fg [id]` are handled by proto. Ctrl+Z stops a foreground
command; `fg` resumes it. This is a small terminal wrapper, not a full bash/zsh
replacement. Interactive-shell aliases/functions are not imported. Complex
`cd ... && ...` expressions and command substitutions run in child shells and do
not change proto's own working directory. Background-job management is not implemented.

## Testing and development

CI builds with GCC and Clang on Linux and with Clang on macOS. Tests use a local fake
OpenAI-compatible HTTP server and disposable PTYs/directories: no real API keys,
remote model calls, or user-home contents are required.

```sh
cmake -S . -B build-sanitize -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPROTO_ENABLE_SANITIZERS=ON
cmake --build build-sanitize --parallel
ctest --test-dir build-sanitize --output-on-failure
```

See [CONTRIBUTING.md](CONTRIBUTING.md) for the source layout and contribution checks.
