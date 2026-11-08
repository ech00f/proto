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
`-DPROTO_ENABLE_SANITIZERS=ON`.

## Source layout

- `main.cpp`: CLI options and interactive command loop.
- `src/config.cpp`: environment configuration and endpoint validation.
- `src/context.cpp`, `src/text.cpp`: metadata collection, filtering, and UTF-8 utilities.
- `src/api_client.cpp`: bounded, cancellable OpenAI-compatible HTTP requests.
- `src/completion_service.cpp`: background requests, debounce, and generation tracking.
- `src/local_completion.cpp`, `src/history.cpp`: offline suggestions and private local history.
- `src/editor.cpp`: input decoding, line editing, and inline terminal rendering.
- `src/shell.cpp`, `src/signals.cpp`: foreground commands, small builtins, and signal handling.
- `tests/unit_tests.cpp`: C++ unit/regression checks (also active in Release builds).
- `tests/integration_test.py`: fake HTTP server and real PTY integration tests.

## Safety invariants

Never auto-execute a model response, replace a user's typed prefix, render control
characters from a provider, or show stale results after an edit. Do not add recursive
home scanning, file-content uploads, shell-history imports, or opt-out secret sharing.
Changes to outbound context must remain inspectable with `--context` and configurable
with `PROTO_CONTEXT`. API keys and personal history must not enter the repository.
