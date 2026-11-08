# proto

Copilot-style inline command autocomplete, written in C++20 for Linux and macOS.
Suggestions appear as gray ghost text while you type; the model never executes
anything. Uses an OpenAI-compatible API and defaults to a local Ollama model.

```text
proto ~/project $ git st│atus --short
                       └─ gray suggestion: Tab accepts, Enter executes your input
```

Network requests run on a background thread, are debounced, and are cancelled
when you edit the line. Stale replies cannot overwrite newer input. Local path,
common-command, and proto-history completions still work when the AI is unavailable.

## Build

On Ubuntu / Debian:

```sh
sudo apt install build-essential cmake ninja-build pkg-config libcurl4-openssl-dev nlohmann-json3-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

## Run

```sh
ollama pull qwen2.5-coder:7b
# Start `ollama serve` if the Ollama service is not already running.
./build/proto

# Non-interactive completion / metadata inspection:
./build/proto --complete 'git st'
./build/proto --no-ai --complete 'cd ~/Doc'
./build/proto --context
```

For another OpenAI-compatible provider, set `PROTO_API_BASE`, `PROTO_MODEL`, and
`PROTO_API_KEY` in your environment. Never commit API keys.

The model receives the typed prefix, shell name, visible current-directory names,
and top-level home-directory names. File contents, dotfiles, symlinks, and obvious
secret/recovery files are excluded. `PROTO_CONTEXT=none` disables directory context;
`PROTO_CONTEXT=cwd` limits it to the current directory. Recent proto commands are
sent only with `PROTO_SEND_HISTORY=1`; existing bash/zsh history is never imported.

`--complete` only prints a suggestion. It never executes a command.

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
replacement: shell aliases/functions and complex `cd ... && ...` expressions run
in child shells and do not change proto's own state.
