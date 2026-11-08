#!/usr/bin/env python3

import http.server
import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest

from terminal_process import TerminalProcess


BINARY = str(Path(sys.argv.pop(1)).resolve())
module_argument = sys.argv.pop(1)
MODULE = Path(module_argument).resolve() if module_argument != "-" else None
ROOT = Path(__file__).resolve().parent.parent
BASH = shutil.which(os.environ.get("PROTO_TEST_BASH", "bash"))
ZSH = shutil.which("zsh")


class ShellTests(unittest.TestCase):
    def setUp(self):
        base = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None
        self.directory = tempfile.TemporaryDirectory(prefix="proto-shell-", dir=base)
        self.addCleanup(self.directory.cleanup)
        self.home = Path(self.directory.name).resolve()
        self.cwd = self.home / "work"
        self.cwd.mkdir()
        self.requests = []
        self.models = []
        self.response = lambda context: context["prefix"]
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                document = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                context = json.loads(document["messages"][-1]["content"])
                owner.requests.append(context)
                owner.models.append(document["model"])
                payload = json.dumps({"choices": [{"message": {"content": owner.response(context)}}]}).encode()
                try:
                    self.send_response(200)
                    self.send_header("Content-Length", str(len(payload)))
                    self.end_headers()
                    self.wfile.write(payload)
                except (BrokenPipeError, ConnectionResetError):
                    pass

            def log_message(self, *_args):
                pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.start_server()
        self.addCleanup(self.stop_server)
        self.environment = {key: value for key, value in os.environ.items() if not key.startswith("PROTO_")}
        self.environment.update({
            "HOME": str(self.home), "ZDOTDIR": str(self.home), "INPUTRC": "/dev/null",
            "HISTFILE": str(self.home / "shell-history"), "PS1": "shell $ ", "PROMPT_COMMAND": "",
            "XDG_CONFIG_HOME": str(self.home / "config"), "XDG_STATE_HOME": str(self.home / "state"),
            "XDG_RUNTIME_DIR": str(self.home / "runtime"),
            "PROTO_BIN": BINARY, "PROTO_AI": "0", "PROTO_MODEL": "shell-fixture",
            "PROTO_DEBOUNCE_MS": "70", "PROTO_TIMEOUT_MS": "1000",
            "PROTO_API_BASE": f"http://127.0.0.1:{self.server.server_port}/v1",
            "LC_ALL": "C.UTF-8" if sys.platform.startswith("linux") else "en_US.UTF-8",
            "TERM": "xterm-256color",
        })

    def start_server(self):
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.02})
        self.thread.start()

    def stop_server(self):
        self.server.shutdown()
        self.thread.join()
        self.server.server_close()

    def shell(self, kind="bash", native=True, setup=""):
        executable = BASH if kind == "bash" else ZSH
        if executable is None:
            self.skipTest(f"{kind} is not installed")
        if kind == "bash" and native and (MODULE is None or not MODULE.is_file()):
            self.skipTest("Bash module was not built")
        self.environment["PROTO_BASH_MODULE"] = str(MODULE) if kind == "bash" and native else ""
        self.server.shutdown()
        self.thread.join()
        try:
            arguments = ("--noprofile", "--norc", "-i") if kind == "bash" else ("-f", "-i")
            terminal = TerminalProcess(executable, self.environment, self.cwd, arguments)
        finally:
            self.start_server()
        self.addCleanup(self.close_shell, terminal)
        terminal.read_until(b"shell $ ")
        terminal.send(setup + "source " + shlex.quote(str(ROOT / "shell" / f"proto.{kind}")) + "\r")
        terminal.read_until(b"shell $ ", after=b"\n")
        terminal.pump(0.1)
        return terminal

    def close_shell(self, terminal):
        if terminal.status is None:
            terminal.send("\x15exit\r")
            try:
                terminal.wait(0.5)
            except AssertionError:
                pass
        terminal.close(signal.SIGHUP)

    def start_daemon(self):
        runtime = self.home / "runtime"
        runtime.mkdir(mode=0o700)
        self.environment["PROTO_DAEMON_SOCKET"] = str(runtime / "proto.sock")
        daemon = subprocess.Popen([BINARY, "--daemon"], cwd=self.home, env=self.environment,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)

        def stop():
            if daemon.poll() is None:
                daemon.terminate()
            try:
                output, error = daemon.communicate(timeout=4)
            except subprocess.TimeoutExpired:
                daemon.kill()
                daemon.communicate()
                self.fail("Shell fixture daemon did not stop")
            self.assertEqual(daemon.returncode, 0, error.decode())

        self.addCleanup(stop)
        deadline = time.monotonic() + 4
        while daemon.poll() is None and time.monotonic() < deadline:
            status = subprocess.run([BINARY, "--daemon-status"], cwd=self.home, env=self.environment,
                                    capture_output=True, text=True, timeout=2)
            if status.returncode == 0 and json.loads(status.stdout)["pid"] == daemon.pid:
                return
            time.sleep(0.01)
        self.fail("Shell fixture daemon did not become ready")

    def test_bash_inline_and_variant_selection(self):
        self.environment["PROTO_AI"] = "1"
        self.response = lambda _context: json.dumps({"completions": [
            "printf first > first.txt", "printf second > second.txt"]})
        terminal = self.shell()
        terminal.send("printf ")
        terminal.read_until(b"first > first.txt")
        self.assertFalse((self.cwd / "first.txt").exists())
        terminal.send("\x1b[1;3B")
        terminal.read_until(b"second > second.txt")
        terminal.send("\t\r")
        terminal.read_until(b"shell $ ")
        self.assertEqual((self.cwd / "second.txt").read_text(), "second")
        self.assertFalse((self.cwd / "first.txt").exists())

    def test_bash_alias_function_and_cd_state(self):
        terminal = self.shell(setup="alias native_alias='printf alias'; native_function() { printf function; }; ")
        terminal.send("native_alias > alias.txt; native_function > function.txt; cd ..\r")
        terminal.read_until(b"shell $ ")
        terminal.send("printf '%s' \"$PWD\" > location.txt\r")
        terminal.read_until(b"shell $ ")
        self.assertEqual((self.cwd / "alias.txt").read_text(), "alias")
        self.assertEqual((self.cwd / "function.txt").read_text(), "function")
        self.assertEqual((self.home / "location.txt").read_text(), str(self.home))

    def assert_enter_does_not_accept_ghost(self, kind):
        self.environment["PROTO_AI"] = "1"
        self.response = lambda _context: "printf untouched > ghost.txt"
        terminal = self.shell(kind)
        terminal.send("printf untouched")
        terminal.read_until(b"> ghost.txt")
        terminal.send("\r")
        terminal.read_until(b"shell $ ")
        self.assertFalse((self.cwd / "ghost.txt").exists())

    def assert_menu_preserves_input(self, kind):
        terminal = self.shell(kind)
        terminal.send("printf draft > draft.txt\x1bOQ")
        terminal.read_until(b"YOUR TERMINAL")
        terminal.send("\x1b")
        terminal.read_until(b"\x1b[?1049l")
        terminal.read_until(b"\x1b[?2004h")
        terminal.read_until(b"draft > draft.txt")
        terminal.send("\r")
        terminal.prompt(b"shell $ ")
        self.assertEqual((self.cwd / "draft.txt").read_text(), "draft")

    def assert_danger_requires_confirmation(self, kind):
        (self.cwd / "keep.txt").write_text("keep")
        terminal = self.shell(kind)
        terminal.send("rm keep.txt\r")
        terminal.read_until(b"DANGER / confirm command")
        terminal.send("\x1b")
        terminal.read_until(b"\x1b[?1049l")
        self.assertTrue((self.cwd / "keep.txt").exists())
        terminal.send("\r")
        terminal.read_until(b"CONFIRM")
        terminal.send("RUN\r")
        terminal.read_until(b"shell $ ")
        self.assertFalse((self.cwd / "keep.txt").exists())

    def assert_plan_inherits_functions_without_leaking_state(self, kind):
        self.environment["PROTO_AI"] = "1"
        self.response = lambda _context: json.dumps({"title": "Native state", "steps": [
            {"command": "cd .."}, {"command": "native_function > plan.txt"}]})
        terminal = self.shell(kind, setup="native_function() { printf inherited; }; ")
        terminal.send("?? use the native function\r")
        terminal.read_until(b"CONFIRM")
        self.assertFalse((self.home / "plan.txt").exists())
        terminal.send("RUN\r")
        terminal.read_until(b"shell $ ")
        self.assertEqual((self.home / "plan.txt").read_text(), "inherited")
        terminal.send("printf '%s' \"$PWD\" > parent.txt\r")
        terminal.read_until(b"shell $ ")
        self.assertEqual((self.cwd / "parent.txt").read_text(), str(self.cwd))

    def assert_debounce_stale_reply_and_privacy(self, kind):
        self.environment["PROTO_AI"] = "1"

        def respond(context):
            if context["prefix"] == "printf old":
                time.sleep(0.3)
                return "printf old-stale > stale.txt"
            return context["prefix"] + "-fresh"

        self.response = respond
        terminal = self.shell(kind)
        terminal.send("printf old")
        deadline = time.monotonic() + 4
        while not self.requests and time.monotonic() < deadline:
            terminal.pump(0.02)
        self.assertEqual(self.requests[0]["prefix"], "printf old")
        terminal.send("\x15printf new")
        terminal.read_until(b"-fresh")
        terminal.pump(0.4)
        self.assertNotIn(b"old-stale", terminal.plain_output)
        terminal.send("\x15password=never-send")
        terminal.pump(0.3)
        self.assertEqual(len(self.requests), 2)
        self.assertNotIn("recent_commands", self.requests[-1])
        self.assertEqual(self.requests[-1]["shell"], kind)

    def test_bash_enter_does_not_accept_ghost(self):
        self.assert_enter_does_not_accept_ghost("bash")

    def test_zsh_enter_does_not_accept_ghost(self):
        self.assert_enter_does_not_accept_ghost("zsh")

    def test_bash_menu_preserves_input(self):
        self.assert_menu_preserves_input("bash")

    def test_zsh_menu_preserves_input(self):
        self.assert_menu_preserves_input("zsh")

    def assert_menu_application_arrows(self, kind, native=True):
        terminal = self.shell(kind, native=native)
        terminal.send("printf menu-draft\x1bOQ")
        terminal.read_until(b"YOUR TERMINAL")
        terminal.send("2\x1bOB\x1bOB\r\x15changed-model\x1bOD\x1bOD\x1bOCX\rs")
        terminal.read_until(b"Saved & applied")
        settings = json.loads((self.home / "config/proto/config.json").read_text())
        self.assertEqual(settings["model"], "changed-modeXl")
        terminal.send("\x1bOA\r")
        terminal.read_until(b"EDIT  /  Endpoint")
        terminal.send("\x1b")
        terminal.read_until(b"Alternatives")
        terminal.send("\x1b")
        terminal.read_until(b"\x1b[?1049l")
        terminal.read_until(b"\x1b[?2004h")
        terminal.read_until(b"printf menu-draft")
        terminal.send("\x03")
        terminal.read_until(b"\x1b[?2004h")

    def test_bash_menu_application_arrows(self):
        self.assert_menu_application_arrows("bash")

    def test_bash_fallback_menu_application_arrows(self):
        self.assert_menu_application_arrows("bash", native=False)

    def test_zsh_menu_application_arrows(self):
        self.assert_menu_application_arrows("zsh")

    def test_zsh_history_search_arrows_keep_the_original_prefix(self):
        self.environment["PROTO_AI"] = "1"
        setup = ("autoload -Uz up-line-or-beginning-search down-line-or-beginning-search; "
                 "zle -N up-line-or-beginning-search; zle -N down-line-or-beginning-search; "
                 "bindkey '^[[A' up-line-or-beginning-search; "
                 "bindkey '^[[B' down-line-or-beginning-search; "
                 "bindkey '^[OA' up-line-or-beginning-search; "
                 "bindkey '^[OB' down-line-or-beginning-search; ")
        terminal = self.shell("zsh", setup=setup)
        terminal.send("fc -p; HISTSIZE=20; print -s -- 'printf history-first'; "
                      "print -s -- 'printf history-second'; print -r -- PROTO_HISTORY_''READY\r")
        terminal.read_until(b"PROTO_HISTORY_READY")
        terminal.prompt(b"shell $ ")
        terminal.send("printf history-\x1b[A")
        terminal.read_until(b"second")
        terminal.pump(0.25)
        terminal.send("\x1bOA")
        terminal.read_until(b"first")
        terminal.pump(0.25)
        terminal.send("\x1b[B")
        terminal.read_until(b"second")
        terminal.send("\x1bOB")
        terminal.pump(0.1)
        self.assertEqual(self.requests, [])
        terminal.send("draft > history-draft.txt\r")
        terminal.prompt(b"shell $ ")
        self.assertEqual((self.cwd / "history-draft.txt").read_text(), "history-draft")

    def test_zsh_custom_history_arrows_do_not_request_or_display_suggestions(self):
        self.environment["PROTO_AI"] = "1"
        self.response = lambda context: context["prefix"] + "-suggested"
        bindings = ("for key in '^[[A' '^[[B' '^[OA' '^[OB'; do "
                    "bindkey -L \"$key\"; done")
        setup = ("native_previous() { zle .up-line-or-history; }; "
                 "native_next() { zle .down-line-or-history; }; "
                 "zle -N native-previous native_previous; zle -N native-next native_next; "
                 "bindkey '^[[A' native-previous; bindkey '^[OA' native-previous; "
                 "bindkey '^[[B' native-next; bindkey '^[OB' native-next; "
                 "{ " + bindings + "; } > before-bindings.txt; ")
        terminal = self.shell("zsh", setup=setup)
        terminal.send("{ " + bindings + "; } > after-bindings.txt; "
                      "fc -p; HISTSIZE=20; print -s -- 'printf browse-first'; "
                      "print -s -- 'printf browse-second'; print -r -- PROTO_HISTORY_''READY\r")
        terminal.read_until(b"PROTO_HISTORY_READY")
        terminal.prompt(b"shell $ ")
        self.assertEqual((self.cwd / "before-bindings.txt").read_text(),
                         (self.cwd / "after-bindings.txt").read_text())
        terminal.send("printf browse-draft")
        terminal.read_until(b"-suggested")
        requests = len(self.requests)
        for arrow, expected in (("\x1bOA", b"second"), ("\x1b[A", b"first"), ("\x1bOB", b"second")):
            terminal.send(arrow)
            terminal.read_until(expected)
            terminal.pump(0.25)
            self.assertNotIn(b"-suggested", terminal.plain_output)
            self.assertEqual(len(self.requests), requests)
        terminal.send("\x1b[B")
        terminal.pump(0.15)
        self.assertNotIn(b"-suggested", terminal.plain_output)
        self.assertEqual(len(self.requests), requests)
        terminal.send("X")
        terminal.read_until(b"-suggested")
        self.assertEqual(self.requests[-1]["prefix"], "printf browse-draftX")
        terminal.send("\x03")
        terminal.read_until(b"\x1b[?2004h")

    def test_zsh_async_replies_preserve_the_last_editing_widget(self):
        self.environment["PROTO_AI"] = "1"
        self.response = lambda context: context["prefix"] + "-suggested"
        setup = ("capture_last_widget() { print -r -- \"$LASTWIDGET\" > last-widget.txt; }; "
                 "zle -N native-capture capture_last_widget; bindkey '^X^L' native-capture; ")
        terminal = self.shell("zsh", setup=setup)
        terminal.send("printf last-widget")
        terminal.read_until(b"-suggested")
        terminal.send("\x18\x0c")
        terminal.pump(0.1)
        self.assertEqual((self.cwd / "last-widget.txt").read_text(), "self-insert\n")
        terminal.send("\x03")
        terminal.read_until(b"\x1b[?2004h")

    def test_bash_danger_requires_confirmation(self):
        self.assert_danger_requires_confirmation("bash")

    def test_zsh_danger_requires_confirmation(self):
        self.assert_danger_requires_confirmation("zsh")

    def test_bash_plan_inherits_functions_without_leaking_state(self):
        self.assert_plan_inherits_functions_without_leaking_state("bash")

    def test_zsh_plan_inherits_functions_without_leaking_state(self):
        self.assert_plan_inherits_functions_without_leaking_state("zsh")

    def test_bash_debounce_stale_reply_and_privacy(self):
        self.assert_debounce_stale_reply_and_privacy("bash")

    def test_zsh_debounce_stale_reply_and_privacy(self):
        self.assert_debounce_stale_reply_and_privacy("zsh")

    def test_bash_disable_restores_custom_tab(self):
        terminal = self.shell(setup="bind '\"\\C-i\": \"restored\"'; ")
        terminal.send("proto-disable\r")
        terminal.read_until(b"shell $ ", after=b"\n")
        terminal.send("printf '\t' > restored.txt\r")
        terminal.read_until(b"shell $ ", after=b"\n")
        self.assertEqual((self.cwd / "restored.txt").read_text(), "restored")

    def test_bash_disable_keeps_later_bindings_and_restores_macros_after_reenable(self):
        terminal = self.shell(setup="bind '\"\\C-i\": \"restored\"'; ")
        terminal.send("bind '\"\\eOQ\": \"new-binding\"'; proto-disable; proto-enable; proto-disable\r")
        terminal.read_until(b"shell $ ", after=b"\n")
        terminal.send("printf '\t' > restored.txt; printf '\x1bOQ' > newer.txt\r")
        terminal.read_until(b"shell $ ", after=b"\n")
        self.assertEqual((self.cwd / "restored.txt").read_text(), "restored")
        self.assertEqual((self.cwd / "newer.txt").read_text(), "new-binding")

    def test_bash_custom_tab_fallback_when_there_is_no_suggestion(self):
        terminal = self.shell(setup="bind '\"\\C-i\": \"restored\"'; ")
        terminal.send("printf '\t' > fallback.txt\r")
        terminal.read_until(b"shell $ ", after=b"\n")
        self.assertEqual((self.cwd / "fallback.txt").read_text(), "restored")

    def test_bash_exit_restores_terminal(self):
        terminal = self.shell()
        terminal.send("exit\r")
        self.assertEqual(terminal.wait(), 0)
        self.assertTrue(terminal.restored())

    def test_bash_menu_settings_apply_without_changing_user_environment(self):
        self.environment["PROTO_AI"] = "1"
        self.response = lambda context: context["prefix"] + "-suggested"
        for native in (True, False):
            with self.subTest(native=native):
                terminal = self.shell(native=native)
                terminal.send("\x1bOQ")
                terminal.read_until(b"YOUR TERMINAL")
                terminal.send("2\x1b[B\x1b[B\r\x15changed-model\r")
                terminal.read_until(b"Changed locally")
                terminal.send("s")
                terminal.read_until(b"Saved & applied")
                terminal.send("\x1b")
                terminal.read_until(b"\x1b[?1049l")
                terminal.send("printf configuration")
                if not native:
                    terminal.send("\x18\x01")
                terminal.read_until(b"-suggested")
                self.assertEqual(self.models[-1], "changed-model")
                terminal.send("\x15printf '%s' \"$PROTO_MODEL\" > environment.txt\r")
                terminal.read_until(b"shell $ ")
                self.assertEqual((self.cwd / "environment.txt").read_text(), "shell-fixture")
                self.close_shell(terminal)

    def assert_plan_function_uses_menu_settings(self, kind, native=True):
        self.environment["PROTO_AI"] = "1"
        self.response = lambda context: json.dumps({"title": "Configured plan", "steps": [
            {"command": "printf configured > plan-settings.txt"}]}) if "task" in context else context["prefix"]
        terminal = self.shell(kind, native=native)
        terminal.send("\x1bOQ")
        terminal.read_until(b"YOUR TERMINAL")
        terminal.send("2\x1b[B\x1b[B\r\x15plan-model\r")
        terminal.read_until(b"Changed locally")
        terminal.send("s")
        terminal.read_until(b"Saved & applied")
        terminal.send("\x1b")
        terminal.read_until(b"\x1b[?1049l")
        terminal.read_until(b"\x1b[?2004h")
        terminal.send("proto-plan 'use the current settings'\r")
        terminal.read_until(b"CONFIRM")
        self.assertEqual(self.models[-1], "plan-model")
        self.assertFalse((self.cwd / "plan-settings.txt").exists())
        terminal.send("RUN\r")
        terminal.read_until(b"shell $ ", after=b"\n")
        self.assertEqual((self.cwd / "plan-settings.txt").read_text(), "configured")
        terminal.send("printf '%s' \"$PROTO_MODEL\" > environment.txt\r")
        terminal.read_until(b"shell $ ", after=b"\n")
        self.assertEqual((self.cwd / "environment.txt").read_text(), "shell-fixture")

    def test_bash_plan_function_uses_menu_settings(self):
        self.assert_plan_function_uses_menu_settings("bash")

    def test_bash_fallback_plan_function_uses_menu_settings(self):
        self.assert_plan_function_uses_menu_settings("bash", native=False)

    def test_zsh_plan_function_uses_menu_settings(self):
        self.assert_plan_function_uses_menu_settings("zsh")

    def test_bash_daemon_plan_uses_menu_settings(self):
        self.start_daemon()
        self.assert_plan_function_uses_menu_settings("bash")

    def test_bash_fallback_daemon_plan_uses_menu_settings(self):
        self.start_daemon()
        self.assert_plan_function_uses_menu_settings("bash", native=False)

    def test_zsh_daemon_plan_uses_menu_settings(self):
        self.start_daemon()
        self.assert_plan_function_uses_menu_settings("zsh")

    def test_bash_fallback_and_restore(self):
        if BASH is None:
            self.skipTest("Bash is not installed")
        version = subprocess.run([BASH, "-c", "printf '%s' \"${BASH_VERSINFO[0]}\""],
                                 capture_output=True, text=True, check=True)
        if int(version.stdout) < 4:
            self.skipTest("Fallback requires Bash 4+")
        (self.home / "Documents").mkdir()
        terminal = self.shell(native=False)
        terminal.send("cd ~/Doc\x18\x01")
        terminal.read_until(b"Documents/")
        terminal.send("\r")
        terminal.read_until(b"shell $ ", after=b"\n")
        terminal.send("proto-disable\r")
        terminal.read_until(b"shell $ ", after=b"\n")
        terminal.send("printf fallback > fallback.txt\r")
        terminal.read_until(b"shell $ ", after=b"\n")
        self.assertEqual((self.home / "Documents/fallback.txt").read_text(), "fallback")

    def test_zsh_local_completion_and_native_state(self):
        (self.home / "Documents").mkdir()
        terminal = self.shell("zsh", setup="alias native_alias='printf alias'; native_function() { printf function; }; ")
        terminal.send("cd ~/Doc")
        terminal.read_until(b"uments/")
        terminal.send("\t\r")
        terminal.read_until(b"shell $ ")
        terminal.send("native_alias > alias.txt; native_function > function.txt\r")
        terminal.read_until(b"shell $ ")
        self.assertEqual((self.home / "Documents/alias.txt").read_text(), "alias")
        self.assertEqual((self.home / "Documents/function.txt").read_text(), "function")

    def test_zsh_inline_and_variant_selection(self):
        self.environment["PROTO_AI"] = "1"
        self.response = lambda _context: json.dumps({"completions": [
            "printf first > first.txt", "printf second > second.txt"]})
        terminal = self.shell("zsh")
        terminal.send("printf ")
        terminal.read_until(b"first > first.txt")
        self.assertFalse((self.cwd / "first.txt").exists())
        terminal.send("\x1b[1;3B")
        terminal.read_until(b"second > second.txt")
        terminal.send("\t\r")
        terminal.read_until(b"shell $ ")
        self.assertEqual((self.cwd / "second.txt").read_text(), "second")
        self.assertFalse((self.cwd / "first.txt").exists())

    def test_zsh_disable_restores_custom_tab_and_keeps_later_bindings(self):
        terminal = self.shell("zsh", setup="bindkey -s '^I' 'restored'; ")
        terminal.send("bindkey -s '^[OQ' 'new-binding'; proto-disable\r")
        terminal.read_until(b"shell $ ")
        terminal.send("printf '\t' > restored.txt\r")
        terminal.read_until(b"shell $ ")
        self.assertEqual((self.cwd / "restored.txt").read_text(), "restored")
        terminal.send("printf '\x1bOQ' > newer.txt\r")
        terminal.read_until(b"shell $ ")
        self.assertEqual((self.cwd / "newer.txt").read_text(), "new-binding")

    def test_zsh_custom_tab_fallback_when_there_is_no_suggestion(self):
        terminal = self.shell("zsh", setup="bindkey -s '^I' 'restored'; ")
        terminal.send("printf '\t' > fallback.txt\r")
        terminal.read_until(b"shell $ ")
        self.assertEqual((self.cwd / "fallback.txt").read_text(), "restored")

    def test_zsh_plan_stops_on_failure_and_returns_to_parent_shell(self):
        self.environment["PROTO_AI"] = "1"
        self.response = lambda _context: json.dumps({"title": "Stop safely", "steps": [
            {"command": "printf first > first.txt"}, {"command": "false"},
            {"command": "touch never.txt"}]})
        terminal = self.shell("zsh")
        terminal.send("?? stop after a failure\r")
        terminal.read_until(b"CONFIRM")
        terminal.send("RUN\r")
        terminal.read_until(b"shell $ ")
        self.assertEqual((self.cwd / "first.txt").read_text(), "first")
        self.assertFalse((self.cwd / "never.txt").exists())
        terminal.send("printf '%s' \"$?\" > status.txt\r")
        terminal.read_until(b"shell $ ")
        self.assertEqual((self.cwd / "status.txt").read_text(), "1")

    def test_zsh_menu_settings_apply_immediately(self):
        self.environment["PROTO_AI"] = "1"
        self.response = lambda context: context["prefix"] + "-suggested"
        terminal = self.shell("zsh")
        terminal.send("\x1bOQ")
        terminal.read_until(b"YOUR TERMINAL")
        terminal.send("2\x1b[B\x1b[B\r\x15changed-model\r")
        terminal.read_until(b"Changed locally")
        terminal.send("s")
        terminal.read_until(b"Saved & applied")
        terminal.send("\x1b")
        terminal.read_until(b"\x1b[?1049l")
        terminal.send("printf configuration")
        terminal.read_until(b"-suggested")
        self.assertEqual(self.models[-1], "changed-model")
        terminal.send("\x15printf '%s' \"$PROTO_MODEL\" > environment.txt\r")
        terminal.read_until(b"shell $ ")
        self.assertEqual((self.cwd / "environment.txt").read_text(), "shell-fixture")

    def test_zsh_local_completion_does_not_wait_for_ai(self):
        self.environment["PROTO_AI"] = "1"
        (self.home / "Documents").mkdir()

        def respond(context):
            time.sleep(0.8)
            return context["prefix"]

        self.response = respond
        terminal = self.shell("zsh")
        terminal.send("cd ~/Doc")
        terminal.read_until(b"uments/", timeout=0.6)
        terminal.send("\x03")
        terminal.read_until(b"shell $ ")

    def test_zsh_utf8_paste_never_executes_on_its_own(self):
        terminal = self.shell("zsh")
        terminal.send("\x1b[200~printf 'Привет' > unicode.txt\x1b[201~")
        terminal.pump(0.2)
        self.assertFalse((self.cwd / "unicode.txt").exists())
        terminal.send("\r")
        terminal.read_until(b"shell $ ")
        self.assertEqual((self.cwd / "unicode.txt").read_text(), "Привет")

    def test_zsh_exit_restores_terminal(self):
        terminal = self.shell("zsh")
        terminal.send("exit\r")
        self.assertEqual(terminal.wait(), 0)
        self.assertTrue(terminal.restored())

    def test_zsh_existing_hooks_and_reenable_are_preserved(self):
        terminal = self.shell("zsh", setup="old_init() { print x >> hooks.txt; }; zle -N zle-line-init old_init; ")
        before = (self.cwd / "hooks.txt").read_text().count("x")
        terminal.send("proto-disable\r")
        terminal.read_until(b"shell $ ")
        self.assertGreater((self.cwd / "hooks.txt").read_text().count("x"), before)
        terminal.send("proto-enable\r")
        terminal.read_until(b"shell $ ")
        terminal.send("pwd\r")
        terminal.read_until(b"shell $ ")
        self.assertGreaterEqual((self.cwd / "hooks.txt").read_text().count("x"), before + 3)
        self.assertFalse((self.home / ".bashrc").exists())
        self.assertFalse((self.home / ".zshrc").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
