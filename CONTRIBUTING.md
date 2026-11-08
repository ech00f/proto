# Contributing

## Local checks

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
git diff --check
```

Use C++20, the project's `.clang-format`, and the existing `proto` namespace.
Keep changes focused; add regression tests for completion, privacy, or terminal
behavior changes. Tests must use disposable directories and fake local APIs,
never a contributor's real home contents or credentials.

For memory/undefined-behavior checks, configure a separate build with
`-DPROTO_ENABLE_SANITIZERS=ON`. This omits the Bash module; cover native Bash in
an unsanitized Release build. Install Bash/Readline headers and Zsh 5.9+ for all
shell tests, and use `PROTO_TEST_BASH` to select a compatible Bash on macOS.
Optional integrations that are unavailable are reported as skipped tests.

## Source layout

- `main.cpp`: CLI options and interactive command loop.
- `src/config.cpp`: environment configuration and endpoint validation.
- `src/settings.cpp`: private saved settings and non-secret configuration data.
- `src/context.cpp`, `src/text.cpp`: metadata collection, filtering, and UTF-8 utilities.
- `src/api_client.cpp`: bounded, cancellable OpenAI-compatible HTTP requests.
- `src/daemon.cpp`: same-user Unix-socket requests, bounded workers, and disconnect cancellation.
- `src/completion_service.cpp`: background requests, debounce, and generation tracking.
- `src/local_completion.cpp`, `src/history.cpp`: offline suggestions and private local history.
- `src/editor.cpp`: input decoding, line editing, and inline terminal rendering.
- `src/shell.cpp`, `src/signals.cpp`: foreground commands, small builtins, and signal handling.
- `src/plan.cpp`, `src/tui.cpp`: validated plans, typed approvals, and settings/history screens.
- `src/bash_bridge.c`, `src/bash_integration.cpp`: Bash loadable builtin and native Readline hooks.
- `shell/proto.bash`, `shell/proto.zsh`: opt-in integration and binding/hook restoration.
- `tests/unit_tests.cpp`: C++ unit/regression checks (also active in Release builds).
- `tests/integration_test.py`: fake HTTP server and real PTY integration tests.
- `tests/shell_integration_test.py`, `tests/terminal_process.py`: native-shell regressions and shared PTY helpers.
- `tests/daemon_test.py`, `tests/install_test.py`: private daemon protocols and opt-in startup installation.
- `scripts/install.py`, `systemd/proto.service.in`: user-local installation and optional systemd autostart.

## Safety invariants

Never auto-execute a model response, replace a user's typed prefix, render control
characters from a provider, or show stale results after an edit. Do not add recursive
home scanning, file-content uploads, shell-history imports, or opt-out secret sharing.
Changes to outbound context must remain inspectable with `--context` and configurable
with `PROTO_CONTEXT`. API keys and personal history must not enter the repository.
Plans and dangerous commands require explicit approval. Plan steps share a
subshell, stop on failure, and do not change parent-shell state. Never rewrite a
user's shell startup files implicitly or discard bindings they changed after enabling proto.
The installer may add/remove only its marked startup block after an explicit
`--shell` / `--remove-shell` request. Never overwrite changed installed files.
Daemon RPC must not execute commands, scan directories, log keys/context, or
accept other users; review and approvals belong in the client terminal.
