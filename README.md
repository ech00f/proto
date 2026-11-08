# proto

AI-powered command autocomplete, written in C++20. Uses an OpenAI-compatible
chat-completions API; defaults to a local Ollama model.

## Build

On Ubuntu / Debian:

```sh
sudo apt install build-essential cmake ninja-build pkg-config libcurl4-openssl-dev nlohmann-json3-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

## Completion API

```sh
ollama pull qwen2.5-coder:7b
# Start `ollama serve` if the Ollama service is not already running.
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
