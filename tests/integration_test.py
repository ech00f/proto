#!/usr/bin/env python3

import errno
import fcntl
import http.server
import json
import os
from pathlib import Path
import pty
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time
import unittest


BINARY = str(Path(sys.argv.pop(1)).resolve())


class TerminalProcess:
    def __init__(self, environment, cwd, arguments=()):
        gate_read, gate_write = os.pipe()
        self.pid, self.master = pty.fork()
        if self.pid == 0:
            os.close(gate_write)
            os.read(gate_read, 1)
            os.close(gate_read)
            os.chdir(cwd)
            os.execve(BINARY, [BINARY, *arguments], environment)
        os.close(gate_read)
        self.original = termios.tcgetattr(self.master)
        fcntl.ioctl(self.master, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 120, 0, 0))
        os.write(gate_write, b"1")
        os.close(gate_write)
        self.output = b""
        self.status = None
        self.read_until(b" $ ")

    def send(self, text):
        self.output = b""
        os.write(self.master, text.encode() if isinstance(text, str) else text)

    def pump(self, duration=0.1):
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            readable, _, _ = select.select([self.master], [], [], max(0, deadline - time.monotonic()))
            if not readable:
                break
            try:
                block = os.read(self.master, 65536)
            except OSError as error:
                if error.errno == errno.EIO:  # A PTY returns EIO after its slave closes.
                    break
                raise
            if not block:
                break
            self.output += block
        return self.output

    def read_until(self, text, timeout=4):
        deadline = time.monotonic() + timeout
        while text not in self.output and time.monotonic() < deadline:
            self.pump(0.03)
        if text not in self.output:
            raise AssertionError(f"Terminal did not print {text!r}: {self.output!r}")
        return self.output

    def prompt(self, text=b" $ "):
        self.read_until(b"\x1b[?2004h")
        return self.read_until(text)

    def foreground(self):
        self.read_until(b"\x1b[?2004l")
        deadline = time.monotonic() + 4
        while os.tcgetpgrp(self.master) == self.pid and time.monotonic() < deadline:
            self.pump(0.01)
        if os.tcgetpgrp(self.master) == self.pid:
            raise AssertionError("Command did not acquire the foreground terminal")

    def wait(self, timeout=4):
        deadline = time.monotonic() + timeout
        while self.status is None and time.monotonic() < deadline:
            pid, status = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                self.status = os.waitstatus_to_exitcode(status)
            else:
                self.pump(0.03)
        if self.status is None:
            raise AssertionError(f"Terminal process did not exit: {self.output!r}")
        self.pump(0.01)
        return self.status

    def restored(self):
        current = termios.tcgetattr(self.master)
        return current == self.original

    def close(self):
        if self.status is None:
            os.kill(self.pid, signal.SIGTERM)
            try:
                self.wait()
            except AssertionError:
                os.kill(self.pid, signal.SIGKILL)
                os.waitpid(self.pid, 0)
        os.close(self.master)


class IntegrationTests(unittest.TestCase):
    def setUp(self):
        base = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None
        self.directory = tempfile.TemporaryDirectory(prefix="proto-integration-", dir=base)
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.home = self.root / "home"
        self.cwd = self.home / "project"
        self.cwd.mkdir(parents=True)
        (self.home / "Documents").mkdir()
        (self.home / ".ssh").mkdir()
        (self.cwd / "main.cpp").write_text("FILE_CONTENT_MUST_NOT_LEAVE_THE_MACHINE")
        (self.cwd / ".env").write_text("DO_NOT_SEND")
        (self.cwd / "private.key").write_text("DO_NOT_SEND")
        self.requests = []
        self.response = lambda context: context["prefix"]
        self.http_status = 200
        self.raw_response = None
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                body = self.rfile.read(int(self.headers["Content-Length"]))
                request = json.loads(body)
                context = json.loads(request["messages"][-1]["content"])
                owner.requests.append({
                    "path": self.path,
                    "authorization": self.headers.get("Authorization"),
                    "request": request,
                    "context": context,
                })
                content = owner.response(context)
                payload = owner.raw_response
                if payload is None:
                    payload = json.dumps({"choices": [{"message": {"content": content}}]}).encode()
                try:
                    self.send_response(owner.http_status)
                    if owner.http_status == 307:
                        self.send_header("Location", f"http://127.0.0.1:{owner.server.server_port}/leak")
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(payload)))
                    self.end_headers()
                    self.wfile.write(payload)
                except (BrokenPipeError, ConnectionResetError):
                    pass  # Cancellation and timeout tests deliberately close the connection.

            def log_message(self, *_arguments):
                pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = False
        self.start_server()
        self.addCleanup(self.stop_server)
        self.environment = {key: value for key, value in os.environ.items() if not key.startswith("PROTO_")}
        self.environment.update({
            "HOME": str(self.home),
            "XDG_STATE_HOME": str(self.root / "state"),
            "PROTO_API_BASE": f"http://127.0.0.1:{self.server.server_port}/v1",
            "PROTO_API_KEY": "test-only-key",
            "PROTO_MODEL": "fixture-model",
            "PROTO_SHELL": "/bin/sh",
            "PROTO_DEBOUNCE_MS": "60",
            "PROTO_TIMEOUT_MS": "1000",
            "LC_ALL": "C.UTF-8" if sys.platform.startswith("linux") else "en_US.UTF-8",
        })

    def stop_server(self):
        self.server.shutdown()
        self.thread.join()
        self.server.server_close()

    def start_server(self):
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.02})
        self.thread.start()

    def run_cli(self, *arguments):
        return subprocess.run([BINARY, *arguments], cwd=self.cwd, env=self.environment,
                              text=True, capture_output=True, timeout=5)

    def terminal(self, *arguments):
        self.server.shutdown()
        self.thread.join()
        try:
            terminal = TerminalProcess(self.environment, self.cwd, arguments)
        finally:
            self.start_server()
        self.addCleanup(terminal.close)
        return terminal

    def wait_requests(self, count):
        deadline = time.monotonic() + 4
        while len(self.requests) < count and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertGreaterEqual(len(self.requests), count)

    def test_cli_help_and_version(self):
        self.assertEqual(self.run_cli("--version").stdout, "proto 0.1.0\n")
        self.assertIn("Tab", self.run_cli("--help").stdout)
        self.assertEqual(self.run_cli("--complete").returncode, 1)

    def test_exact_request_and_private_context(self):
        self.response = lambda _context: "git status --short"
        result = self.run_cli("--complete", "git st")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "git status --short\n")
        request = self.requests[0]
        self.assertEqual(request["path"], "/v1/chat/completions")
        self.assertEqual(request["authorization"], "Bearer test-only-key")
        self.assertEqual(request["request"]["model"], "fixture-model")
        self.assertEqual(request["context"]["prefix"], "git st")
        self.assertEqual(request["context"]["cwd"], "~/project")
        self.assertEqual(request["context"]["cwd_entries"], ["main.cpp"])
        self.assertEqual(request["context"]["home_directories"], ["Documents/", "project/"])
        self.assertNotIn("recent_commands", request["context"])
        self.assertNotIn("FILE_CONTENT_MUST_NOT_LEAVE", json.dumps(request))

    def test_alternative_suggestions_and_risk_labels(self):
        self.response = lambda _context: json.dumps({"completions": ["git status", "git stash", "git status", "rm -rf /"]})
        result = self.run_cli("--suggest-json", "git st")
        self.assertEqual(result.returncode, 0, result.stderr)
        candidates = json.loads(result.stdout)
        self.assertEqual([item["command"] for item in candidates], ["git status", "git stash"])
        self.assertEqual(candidates[0]["risk"]["level"], "none")
        self.assertEqual(self.requests[0]["context"]["suggestion_count"], 3)

    def test_context_preview_does_not_call_api(self):
        result = self.run_cli("--context")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["cwd_entries"], ["main.cpp"])
        self.assertEqual(self.requests, [])

    def test_alt_arrows_cycle_inline_suggestions(self):
        self.response = lambda _context: json.dumps({"completions": ["printf first > first.txt", "printf second > second.txt"]})
        terminal = self.terminal()
        terminal.send("printf ")
        terminal.read_until(b"first > first.txt")
        terminal.send("\x1b[1;3B")
        terminal.read_until(b"second > second.txt")
        terminal.send("\t\r")
        terminal.prompt()
        self.assertFalse((self.cwd / "first.txt").exists())
        self.assertEqual((self.cwd / "second.txt").read_text(), "second")

    def test_prefix_cannot_be_replaced(self):
        self.response = lambda _context: "rm -rf /"
        result = self.run_cli("--complete", "git st")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "")

    def test_multiline_and_terminal_escape_responses_rejected(self):
        for completion in ("git status\nrm -rf /", "git status\x1b[2J", "```sh\ngit status\n```"):
            with self.subTest(completion=completion):
                self.response = lambda _context: completion
                self.assertEqual(self.run_cli("--complete", "git st").stdout, "")

    def test_invalid_json_has_sanitized_error(self):
        self.raw_response = b"test-only-key is not JSON"
        result = self.run_cli("--complete", "git st")
        self.assertEqual(result.returncode, 2)
        self.assertIn("invalid chat-completion", result.stderr)
        self.assertNotIn("test-only-key", result.stderr)

    def test_oversized_response_is_rejected(self):
        self.raw_response = b"x" * (1024 * 1024 + 1)
        result = self.run_cli("--complete", "git st")
        self.assertEqual(result.returncode, 2)
        self.assertIn("exceeded 1 MiB", result.stderr)

    def test_redirect_not_followed(self):
        self.http_status = 307
        result = self.run_cli("--complete", "git st")
        self.assertEqual(result.returncode, 2)
        self.assertIn("HTTP 307", result.stderr)
        self.assertEqual(len(self.requests), 1)

    def test_secret_prefix_not_sent(self):
        for prefix in ("export API_KEY=do-not-send", "curl -u user:abc123 ",
                       "curl https://user:abc123@example.com/", "sshpass -p abc123 ssh "):
            with self.subTest(prefix=prefix):
                result = self.run_cli("--complete", prefix)
                self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.requests, [])

    def test_timeout_is_bounded(self):
        self.environment["PROTO_TIMEOUT_MS"] = "100"

        def slow(context):
            time.sleep(0.4)
            return context["prefix"]

        self.response = slow
        start = time.monotonic()
        result = self.run_cli("--complete", "git st")
        self.assertEqual(result.returncode, 2)
        self.assertIn("timed out", result.stderr)
        self.assertLess(time.monotonic() - start, 1.5)

    def test_remote_plain_http_rejected(self):
        self.environment["PROTO_API_BASE"] = "http://example.com/v1"
        result = self.run_cli("--complete", "git st")
        self.assertEqual(result.returncode, 1)
        self.assertIn("HTTPS", result.stderr)
        self.assertEqual(self.requests, [])

    def test_local_fallback_on_http_error(self):
        self.http_status = 503
        result = self.run_cli("--complete", "cd ~/Doc")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "cd ~/Documents/\n")
        self.assertIn("HTTP 503", result.stderr)

    def test_loopback_bypasses_proxy_environment(self):
        self.environment.update({"http_proxy": "http://127.0.0.1:1", "HTTP_PROXY": "http://127.0.0.1:1",
                                 "ALL_PROXY": "http://127.0.0.1:1", "NO_PROXY": "", "no_proxy": ""})
        self.response = lambda _context: "git status"
        result = self.run_cli("--complete", "git st")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "git status\n")

    def test_ghost_and_tab_do_not_execute_until_enter(self):
        self.response = lambda _context: "printf ai-accepted > accepted.txt"
        terminal = self.terminal()
        terminal.send("printf ai")
        terminal.read_until(b"-accepted > accepted.txt")
        self.assertFalse((self.cwd / "accepted.txt").exists())
        terminal.send("\t")
        terminal.read_until(b"printf ai-accepted > accepted.txt")
        self.assertFalse((self.cwd / "accepted.txt").exists())
        terminal.send("\r")
        terminal.prompt()
        self.assertEqual((self.cwd / "accepted.txt").read_text(), "ai-accepted")
        terminal.send("\x04")
        self.assertEqual(terminal.wait(), 0)
        self.assertTrue(terminal.restored())

    def test_enter_never_accepts_ghost(self):
        self.response = lambda _context: "printf plain > should-not-exist.txt"
        terminal = self.terminal()
        terminal.send("printf plain")
        terminal.read_until(b"> should-not-exist.txt")
        terminal.send("\r")
        terminal.prompt()
        self.assertFalse((self.cwd / "should-not-exist.txt").exists())
        self.assertIn(b"plain", terminal.output)

    def test_debounce_coalesces_edits(self):
        self.environment["PROTO_DEBOUNCE_MS"] = "150"
        terminal = self.terminal()
        for character in "printf hello":
            os.write(terminal.master, character.encode())
        self.wait_requests(1)
        terminal.pump(0.2)
        self.assertEqual(len(self.requests), 1)
        self.assertEqual(self.requests[0]["context"]["prefix"], "printf hello")

    def test_stale_response_cannot_replace_new_input(self):
        def completion(context):
            if context["prefix"] == "printf old":
                time.sleep(0.3)
                return "printf old-stale > stale.txt"
            return "printf new-current"

        self.response = completion
        terminal = self.terminal()
        terminal.send("printf old")
        self.wait_requests(1)
        terminal.send("\x15printf new")
        terminal.read_until(b"-current")
        self.assertNotIn(b"old-stale", terminal.output)
        terminal.send("\t\r")
        terminal.prompt()
        self.assertFalse((self.cwd / "stale.txt").exists())

    def test_unicode_backspace_and_terminal_restore(self):
        terminal = self.terminal("--no-ai")
        terminal.send("printf Приветx\x7f\r")
        terminal.prompt()
        self.assertIn("Привет".encode(), terminal.output)
        self.assertNotIn("Приветx".encode(), terminal.output)
        terminal.send("\x04")
        self.assertEqual(terminal.wait(), 0)
        self.assertTrue(terminal.restored())

    def test_bracketed_paste_does_not_execute(self):
        terminal = self.terminal("--no-ai")
        terminal.send("\x1b[200~printf pasted > pasted.txt\n\x1b[201~")
        terminal.read_until(b"printf pasted > pasted.txt")
        self.assertFalse((self.cwd / "pasted.txt").exists())
        terminal.send("\r")
        terminal.prompt()
        self.assertEqual((self.cwd / "pasted.txt").read_text(), "pasted")

    def test_cd_persists_in_parent(self):
        terminal = self.terminal("--no-ai")
        terminal.send("cd ..\r")
        terminal.prompt(b"proto ~ $ ")
        terminal.send("printf '%s' \"$PWD\" > cwd.txt\r")
        terminal.prompt()
        self.assertEqual((self.home / "cwd.txt").read_text(), str(self.home))
        terminal.send("cd -\r")
        terminal.prompt(b"proto ~/project $ ")

    def test_foreground_ctrl_c_and_job_resume(self):
        terminal = self.terminal("--no-ai")
        terminal.send("sleep 10\r")
        terminal.foreground()
        terminal.send("\x1a")
        terminal.read_until(b"use fg to resume")
        terminal.prompt()
        terminal.send("fg\r")
        terminal.foreground()
        terminal.send("\x03")
        terminal.prompt()
        terminal.send("\x04")
        self.assertEqual(terminal.wait(), 130)
        self.assertTrue(terminal.restored())

    def test_sigterm_restores_terminal(self):
        terminal = self.terminal()
        os.kill(terminal.pid, signal.SIGTERM)
        self.assertEqual(terminal.wait(), 143)
        self.assertTrue(terminal.restored())

    def test_api_key_not_inherited_by_commands(self):
        terminal = self.terminal("--no-ai")
        terminal.send("printf '%s' \"${PROTO_API_KEY-unset}\" > child-key.txt\r")
        terminal.prompt()
        self.assertEqual((self.cwd / "child-key.txt").read_text(), "unset")

    def test_metadata_scope_and_entry_limit(self):
        for index in range(8):
            (self.cwd / f"file-{index}.txt").write_text("private content")
        self.environment.update({"PROTO_CONTEXT": "cwd", "PROTO_MAX_ENTRIES": "3"})
        context = json.loads(self.run_cli("--context").stdout)
        self.assertEqual(len(context["cwd_entries"]), 3)
        self.assertNotIn("home_directories", context)
        self.environment["PROTO_CONTEXT"] = "none"
        context = json.loads(self.run_cli("--context").stdout)
        self.assertEqual(set(context), {"prefix", "shell"})

    def test_history_sharing_requires_explicit_opt_in(self):
        history = self.root / "state" / "proto" / "history"
        history.parent.mkdir(parents=True)
        history.write_text("git status\nexport API_KEY=never-send\n")
        history.chmod(0o600)
        self.assertNotIn("recent_commands", json.loads(self.run_cli("--context").stdout))
        self.environment["PROTO_SEND_HISTORY"] = "1"
        context = json.loads(self.run_cli("--context").stdout)
        self.assertEqual(context["recent_commands"], ["git status"])

    def test_invalid_environment_is_rejected(self):
        for name, value in (("PROTO_MAX_ENTRIES", "0"), ("PROTO_TIMEOUT_MS", "abc"),
                            ("PROTO_CONTEXT", "all"), ("PROTO_SEND_HISTORY", "yes")):
            with self.subTest(name=name):
                self.environment[name] = value
                result = self.run_cli("--context")
                self.assertEqual(result.returncode, 1)
                self.assertIn(name, result.stderr)
                del self.environment[name]

    def test_idle_ctrl_c_and_escape_dismissal(self):
        self.response = lambda _context: "printf discard > discard.txt"
        terminal = self.terminal()
        terminal.send("printf dis")
        terminal.read_until(b"card > discard.txt")
        terminal.send("\x1b")
        terminal.pump(0.2)
        terminal.send("\t\r")
        terminal.prompt()
        self.assertFalse((self.cwd / "discard.txt").exists())
        terminal.send("this-command-will-be-cancelled\x03")
        terminal.read_until(b"^C")
        terminal.prompt()
        terminal.send("\x04")
        self.assertEqual(terminal.wait(), 0)

    def test_up_down_restore_unsubmitted_draft(self):
        terminal = self.terminal("--no-ai")
        terminal.send("printf history > history.txt\r")
        terminal.prompt()
        terminal.send("printf draft > draft.txt\x1b[A\x1b[B\r")
        terminal.prompt()
        self.assertEqual((self.cwd / "draft.txt").read_text(), "draft")

    def test_right_arrow_accepts_and_enter_keeps_exit_status(self):
        self.response = lambda _context: "printf right > right.txt"
        terminal = self.terminal()
        terminal.send("printf ri")
        terminal.read_until(b"ght > right.txt")
        terminal.send("\x1b[C\r")
        terminal.prompt()
        self.assertEqual((self.cwd / "right.txt").read_text(), "right")
        terminal.send("exit 7\r")
        self.assertEqual(terminal.wait(), 7)
        self.assertTrue(terminal.restored())

    def test_ai_offline_indicator_recovers(self):
        terminal = self.terminal()
        self.http_status = 503
        terminal.send("printf unavailable")
        terminal.read_until(b"proto [local]")
        self.http_status = 200
        self.response = lambda context: context["prefix"]
        terminal.send("\x15printf recovered")
        self.wait_requests(2)
        terminal.pump(0.2)
        self.assertIn(b"\x1b[36mproto ~/project", terminal.output)

    def test_non_tty_requires_explicit_completion_mode(self):
        result = self.run_cli()
        self.assertEqual(result.returncode, 1)
        self.assertIn("requires a TTY", result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
