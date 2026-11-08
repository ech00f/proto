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
MODULE = Path(module_argument) if module_argument != "-" else None
ROOT = Path(__file__).resolve().parent.parent
BASH = shutil.which("bash")
ZSH = shutil.which("zsh")


class ShellTests(unittest.TestCase):
    def setUp(self):
        base = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None
        self.directory = tempfile.TemporaryDirectory(prefix="proto-shell-", dir=base)
        self.addCleanup(self.directory.cleanup)
        self.home = Path(self.directory.name)
        self.cwd = self.home / "work"
        self.cwd.mkdir()
        self.requests = []
        self.response = lambda context: context["prefix"]
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                document = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                context = json.loads(document["messages"][-1]["content"])
                owner.requests.append(context)
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
        terminal.read_until(b"shell $ ")
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

    def test_bash_enter_does_not_accept_ghost(self):
        self.environment["PROTO_AI"] = "1"
        self.response = lambda _context: "printf untouched > ghost.txt"
        terminal = self.shell()
        terminal.send("printf untouched")
        terminal.read_until(b"> ghost.txt")
        terminal.send("\r")
        terminal.read_until(b"shell $ ")
        self.assertFalse((self.cwd / "ghost.txt").exists())

    def test_bash_menu_preserves_input(self):
        terminal = self.shell()
        terminal.send("printf draft > draft.txt\x1bOQ")
        terminal.read_until(b"YOUR TERMINAL")
        terminal.send("\x1b")
        terminal.read_until(b"\x1b[?1049l")
        terminal.send("\r")
        terminal.read_until(b"shell $ ")
        self.assertEqual((self.cwd / "draft.txt").read_text(), "draft")

    def test_bash_danger_requires_confirmation(self):
        (self.cwd / "keep.txt").write_text("keep")
        terminal = self.shell()
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

    def test_bash_plan_inherits_functions_without_leaking_state(self):
        self.environment["PROTO_AI"] = "1"
        self.response = lambda _context: json.dumps({"title": "Native state", "steps": [
            {"command": "cd .."}, {"command": "native_function > plan.txt"}]})
        terminal = self.shell(setup="native_function() { printf inherited; }; ")
        terminal.send("?? use the native function\r")
        terminal.read_until(b"CONFIRM")
        self.assertFalse((self.home / "plan.txt").exists())
        terminal.send("RUN\r")
        terminal.read_until(b"shell $ ")
        self.assertEqual((self.home / "plan.txt").read_text(), "inherited")
        terminal.send("printf '%s' \"$PWD\" > parent.txt\r")
        terminal.read_until(b"shell $ ")
        self.assertEqual((self.cwd / "parent.txt").read_text(), str(self.cwd))

    def test_bash_debounce_stale_reply_and_privacy(self):
        self.environment["PROTO_AI"] = "1"

        def respond(context):
            if context["prefix"] == "printf old":
                time.sleep(0.3)
                return "printf old-stale > stale.txt"
            return context["prefix"] + "-fresh"

        self.response = respond
        terminal = self.shell()
        terminal.send("printf old")
        deadline = time.monotonic() + 4
        while not self.requests and time.monotonic() < deadline:
            terminal.pump(0.02)
        self.assertEqual(self.requests[0]["prefix"], "printf old")
        terminal.send("\x15printf new")
        terminal.read_until(b"-fresh")
        terminal.pump(0.4)
        self.assertNotIn(b"old-stale", terminal.output)
        terminal.send("\x15password=never-send")
        terminal.pump(0.3)
        self.assertEqual(len(self.requests), 2)
        self.assertNotIn("recent_commands", self.requests[-1])

    def test_bash_disable_restores_custom_tab(self):
        terminal = self.shell(setup="bind '\"\\C-i\": \"restored\"'; ")
        terminal.send("proto-disable\r")
        terminal.read_until(b"shell $ ")
        terminal.send("printf '\t' > restored.txt\r")
        terminal.read_until(b"shell $ ")
        self.assertEqual((self.cwd / "restored.txt").read_text(), "restored")

    def test_bash_exit_restores_terminal(self):
        terminal = self.shell()
        terminal.send("exit\r")
        self.assertEqual(terminal.wait(), 0)
        self.assertTrue(terminal.restored())

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
        terminal.read_until(b"shell $ ")
        terminal.send("proto-disable\r")
        terminal.read_until(b"shell $ ")
        terminal.send("printf fallback > fallback.txt\r")
        terminal.read_until(b"shell $ ")
        self.assertEqual((self.home / "Documents/fallback.txt").read_text(), "fallback")


if __name__ == "__main__":
    unittest.main(verbosity=2)
