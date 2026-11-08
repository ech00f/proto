#!/usr/bin/env python3

from concurrent.futures import ThreadPoolExecutor
import http.server
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest

from terminal_process import TerminalProcess


BINARY = str(Path(sys.argv.pop(1)).resolve())


class DaemonTests(unittest.TestCase):
    def setUp(self):
        base = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None
        self.directory = tempfile.TemporaryDirectory(prefix="proto-daemon-", dir=base)
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.home = self.root / "home"
        self.cwd = self.home / "project"
        self.cwd.mkdir(parents=True)
        (self.home / "Documents").mkdir()
        (self.cwd / "main.cpp").write_text("never upload file contents")
        (self.cwd / ".env").write_text("never upload secrets")
        self.runtime = self.root / "runtime"
        self.runtime.mkdir(mode=0o700)
        self.path = self.runtime / "proto.sock"
        self.requests = []
        self.response = lambda context: context["prefix"] + "-suggested"
        self.http_status = 200
        self.raw_response = None
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                document = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                context = json.loads(document["messages"][-1]["content"])
                owner.requests.append({"context": context, "model": document["model"],
                                       "authorization": self.headers.get("Authorization")})
                content = owner.response(context)
                payload = owner.raw_response
                if payload is None:
                    payload = json.dumps({"choices": [{"message": {"content": content}}]}).encode()
                try:
                    self.send_response(owner.http_status)
                    if owner.http_status == 307:
                        self.send_header("Location", f"http://127.0.0.1:{owner.server.server_port}/leak")
                    self.send_header("Content-Length", str(len(payload)))
                    self.end_headers()
                    self.wfile.write(payload)
                except (BrokenPipeError, ConnectionResetError):
                    pass

            def log_message(self, *_arguments):
                pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.start_server()
        self.addCleanup(self.stop_server)
        self.environment = {key: value for key, value in os.environ.items() if not key.startswith("PROTO_")}
        self.environment.update({
            "HOME": str(self.home), "XDG_STATE_HOME": str(self.root / "state"),
            "XDG_CONFIG_HOME": str(self.root / "config"), "XDG_RUNTIME_DIR": str(self.runtime),
            "PROTO_DAEMON_SOCKET": str(self.path), "PROTO_API_BASE": f"http://127.0.0.1:{self.server.server_port}/v1",
            "PROTO_MODEL": "daemon-fixture", "PROTO_API_KEY": "fixture-key-never-log",
            "PROTO_SHELL": "/bin/sh", "PROTO_TIMEOUT_MS": "1000", "PROTO_DEBOUNCE_MS": "60",
            "TERM": "xterm-256color", "LC_ALL": "C.UTF-8" if sys.platform.startswith("linux") else "en_US.UTF-8",
        })
        self.daemon = None

    def start_server(self):
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.02})
        self.thread.start()

    def stop_server(self):
        self.server.shutdown()
        self.thread.join()
        self.server.server_close()

    def start_daemon(self):
        environment = {**self.environment, "PROTO_AI": "0", "PROTO_API_BASE": "invalid-on-purpose",
                       "PROTO_MODEL": "ambient-model-must-not-override-clients"}
        self.daemon = subprocess.Popen([BINARY, "--daemon"], cwd=self.root, env=environment,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.addCleanup(self.stop_daemon)
        deadline = time.monotonic() + 4
        while self.daemon.poll() is None and time.monotonic() < deadline:
            status = self.run_cli("--daemon-status")
            if status.returncode == 0 and json.loads(status.stdout)["pid"] == self.daemon.pid:
                return
            time.sleep(0.01)
        self.fail("Daemon did not become ready")

    def stop_daemon(self):
        if self.daemon is None:
            return "", ""
        if self.daemon.poll() is None:
            self.daemon.terminate()
        try:
            output = self.daemon.communicate(timeout=4)
        except subprocess.TimeoutExpired:
            self.daemon.kill()
            self.daemon.communicate()
            self.fail("Daemon did not stop promptly")
        self.assertEqual(self.daemon.returncode, 0, output[1])
        self.daemon = None
        return output

    def run_cli(self, *arguments, environment=None):
        return subprocess.run([BINARY, *arguments], cwd=self.cwd,
                              env=environment or self.environment, capture_output=True, text=True, timeout=5)

    def wait_requests(self, count):
        deadline = time.monotonic() + 4
        while len(self.requests) < count and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertGreaterEqual(len(self.requests), count)

    def rpc(self, request=None, body=None, length=None):
        body = body if body is not None else json.dumps(request).encode()
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(3)
            connection.connect(str(self.path))
            connection.sendall(struct.pack("!I", len(body) if length is None else length) + body)
            header = b""
            while len(header) < 4:
                chunk = connection.recv(4 - len(header))
                self.assertTrue(chunk, "Daemon closed before sending a response header")
                header += chunk
            size, = struct.unpack("!I", header)
            response = b""
            while len(response) < size:
                chunk = connection.recv(size - len(response))
                self.assertTrue(chunk, "Daemon closed before sending its response")
                response += chunk
            return json.loads(response)

    def test_idle_daemon_requires_no_backend_and_creates_no_history(self):
        self.start_daemon()
        status = json.loads(self.run_cli("--daemon-status").stdout)
        self.assertEqual(status["pid"], self.daemon.pid)
        self.assertEqual(self.path.stat().st_mode & 0o777, 0o600)
        self.assertEqual(self.requests, [])
        self.assertFalse((self.root / "state").exists())
        self.assertFalse((self.root / "config").exists())
        output, error = self.stop_daemon()
        self.assertIn("daemon ready", output)
        self.assertNotIn("fixture-key-never-log", output + error)
        self.assertFalse(self.path.exists())
        self.assertEqual(self.run_cli("--daemon-status").returncode, 3)

    def test_client_settings_and_filtered_metadata_reach_the_selected_endpoint(self):
        self.start_daemon()
        self.response = lambda _context: "git status --short"
        result = self.run_cli("--complete", "git st")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "git status --short\n")
        request = self.requests[0]
        self.assertEqual(request["model"], "daemon-fixture")
        self.assertEqual(request["authorization"], "Bearer fixture-key-never-log")
        self.assertEqual(request["context"]["cwd"], "~/project")
        self.assertEqual(request["context"]["cwd_entries"], ["main.cpp"])
        self.assertNotIn("recent_commands", request["context"])
        self.assertNotIn("never upload", json.dumps(request))
        self.environment["PROTO_MODEL"] = "changed-client-model"
        self.assertEqual(self.run_cli("--complete", "git st").returncode, 0)
        self.assertEqual(self.requests[-1]["model"], "changed-client-model")

    def test_absent_service_uses_direct_api_and_offline_paths_remain_available(self):
        self.response = lambda _context: "git status"
        self.assertEqual(self.run_cli("--complete", "git st").stdout, "git status\n")
        self.assertEqual(len(self.requests), 1)
        self.start_daemon()
        self.assertEqual(self.run_cli("--no-ai", "--complete", "cd ~/Doc").stdout, "cd ~/Documents/\n")
        self.assertEqual(self.run_cli("--complete", "password=never-send").returncode, 0)
        self.assertEqual(len(self.requests), 1)

    def test_alternatives_and_plan_generation_do_not_execute_commands(self):
        self.start_daemon()
        self.response = lambda _context: json.dumps({"completions": ["git status", "git stash", "rm wrong"]})
        result = self.run_cli("--suggest-json", "git st")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([entry["command"] for entry in json.loads(result.stdout)], ["git status", "git stash"])
        self.response = lambda _context: json.dumps({"title": "Preview only", "steps": [{"command": "touch never.txt"}]})
        result = self.run_cli("--plan", "prepare a file")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["steps"][0]["command"], "touch never.txt")
        self.assertFalse((self.cwd / "never.txt").exists())
        self.assertEqual(self.run_cli("--plan", "password=never-send").returncode, 1)
        self.assertEqual(len(self.requests), 2)

    def test_concurrent_requests_do_not_block_each_other(self):
        self.start_daemon()

        def respond(context):
            if context["prefix"] == "printf slow":
                time.sleep(0.5)
            return context["prefix"] + "-done"

        self.response = respond
        with ThreadPoolExecutor(max_workers=2) as pool:
            slow = pool.submit(self.run_cli, "--complete", "printf slow")
            self.wait_requests(1)
            self.assertEqual(self.run_cli("--daemon-status").returncode, 0)
            fast = pool.submit(self.run_cli, "--complete", "printf fast")
            self.assertEqual(fast.result(timeout=2).stdout, "printf fast-done\n")
            self.assertFalse(slow.done())
            self.assertEqual(slow.result(timeout=2).stdout, "printf slow-done\n")

    def test_timeout_does_not_retry_or_block_the_service(self):
        self.start_daemon()
        self.environment["PROTO_TIMEOUT_MS"] = "100"

        def respond(context):
            time.sleep(0.4)
            return context["prefix"]

        self.response = respond
        result = self.run_cli("--complete", "printf slow")
        self.assertEqual(result.returncode, 2)
        self.assertIn("timed out", result.stderr)
        self.assertEqual(self.run_cli("--daemon-status").returncode, 0)
        self.assertEqual(len(self.requests), 1)

    def test_disconnected_client_and_daemon_shutdown_cancel_pending_work(self):
        self.start_daemon()
        self.environment["PROTO_TIMEOUT_MS"] = "30000"
        release = threading.Event()
        self.addCleanup(release.set)

        def respond(context):
            if context["prefix"] == "printf cancel":
                release.wait(5)
            return context["prefix"]

        self.response = respond
        clients = []
        for _ in range(8):
            client = subprocess.Popen([BINARY, "--complete", "printf cancel"], cwd=self.cwd,
                                      env=self.environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.addCleanup(lambda child=client: child.communicate(timeout=4))
            self.addCleanup(lambda child=client: child.kill() if child.poll() is None else None)
            clients.append(client)
        self.wait_requests(8)
        result = self.run_cli("--complete", "printf overflow")
        self.assertEqual(result.returncode, 2)
        self.assertEqual(len(self.requests), 8)
        clients[0].terminate()
        clients[0].communicate(timeout=3)
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            if self.run_cli("--daemon-status").returncode == 0:
                break
            time.sleep(0.01)
        else:
            self.fail("Disconnected client did not release its daemon worker")
        started = time.monotonic()
        self.stop_daemon()
        self.assertLess(time.monotonic() - started, 2)
        release.set()

    def test_invalid_requests_and_oversized_frames_are_rejected_without_echoing_secrets(self):
        self.start_daemon()
        requests = [self.rpc(body=b"fixture-key-never-log is not JSON"),
                    self.rpc({"version": 1, "op": {"secret": "fixture-key-never-log"}}),
                    self.rpc({"version": 1, "op": "execute", "command": "touch never.txt"}),
                    self.rpc(body=b"", length=256 * 1024 + 1)]
        for response in requests:
            self.assertFalse(response["ok"])
            self.assertNotIn("fixture-key-never-log", json.dumps(response))
        self.assertFalse((self.cwd / "never.txt").exists())
        self.assertEqual(self.requests, [])
        self.assertEqual(self.run_cli("--daemon-status").returncode, 0)

    def test_socket_and_lock_symlinks_never_overwrite_user_files(self):
        victim = self.root / "victim"
        victim.write_text("keep me")
        for path in (self.path, Path(str(self.path) + ".lock")):
            with self.subTest(path=path.name):
                path.symlink_to(victim)
                result = self.run_cli("--daemon")
                self.assertEqual(result.returncode, 1)
                self.assertEqual(victim.read_text(), "keep me")
                path.unlink()
                lock = Path(str(self.path) + ".lock")
                if lock.exists():
                    lock.unlink()

    def test_insecure_directory_and_socket_are_not_used(self):
        self.runtime.chmod(0o755)
        self.assertEqual(self.run_cli("--daemon").returncode, 1)
        self.runtime.chmod(0o700)
        self.start_daemon()
        self.path.chmod(0o666)
        self.assertEqual(self.run_cli("--complete", "git st").returncode, 2)
        self.assertEqual(self.requests, [])
        self.path.chmod(0o600)

    def test_second_daemon_does_not_replace_a_live_socket(self):
        self.start_daemon()
        inode = self.path.stat().st_ino
        result = self.run_cli("--daemon")
        self.assertEqual(result.returncode, 1)
        self.assertIn("already running", result.stderr)
        self.assertEqual(self.path.stat().st_ino, inode)
        self.assertEqual(json.loads(self.run_cli("--daemon-status").stdout)["pid"], self.daemon.pid)

    def test_unrelated_live_socket_is_not_replaced(self):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
            listener.bind(str(self.path))
            self.path.chmod(0o600)
            listener.listen(1)
            inode = self.path.stat().st_ino
            result = self.run_cli("--daemon")
            self.assertEqual(result.returncode, 1)
            self.assertIn("already listening", result.stderr)
            self.assertEqual(self.path.stat().st_ino, inode)

    def test_stale_owned_socket_is_recovered_after_a_crash(self):
        self.start_daemon()
        self.daemon.kill()
        self.daemon.communicate(timeout=3)
        self.daemon = None
        self.assertTrue(self.path.exists())
        self.start_daemon()
        self.assertEqual(self.run_cli("--daemon-status").returncode, 0)

    def test_upstream_errors_do_not_expose_credentials_or_follow_redirects(self):
        self.start_daemon()
        self.raw_response = b"fixture-key-never-log is not JSON"
        result = self.run_cli("--complete", "git st")
        self.assertEqual(result.returncode, 2)
        self.assertNotIn("fixture-key-never-log", result.stderr)
        self.raw_response = None
        self.http_status = 307
        result = self.run_cli("--complete", "git st")
        self.assertEqual(result.returncode, 2)
        self.assertIn("HTTP 307", result.stderr)
        self.assertEqual(len(self.requests), 2)
        output, error = self.stop_daemon()
        self.assertNotIn("fixture-key-never-log", output + error)

    def test_disabled_daemon_transport_bypasses_the_socket(self):
        self.path.symlink_to(self.root / "unrelated")
        self.environment["PROTO_DAEMON"] = "0"
        self.assertEqual(self.run_cli("--complete", "printf direct").stdout, "printf direct-suggested\n")
        self.assertEqual(len(self.requests), 1)

    def test_invalid_socket_paths_are_rejected_before_direct_requests(self):
        for path in ("relative.sock", str(self.runtime / "bad\x1bpath")):
            with self.subTest(path=path):
                self.environment["PROTO_DAEMON_SOCKET"] = path
                result = self.run_cli("--complete", "printf invalid")
                self.assertEqual(result.returncode, 2)
                self.assertIn("printable absolute path", result.stderr)
        self.assertEqual(self.requests, [])

    def test_stale_ghost_reply_cannot_execute_after_daemon_transport(self):
        self.start_daemon()

        def respond(context):
            if context["prefix"] == "printf old":
                time.sleep(0.3)
                return "printf old-stale > never.txt"
            return "printf new-current"

        self.response = respond
        self.server.shutdown()
        self.thread.join()
        try:
            terminal = TerminalProcess(BINARY, self.environment, self.cwd)
        finally:
            self.start_server()
        self.addCleanup(terminal.close)
        terminal.read_until(b" $ ")
        terminal.send("printf old")
        self.wait_requests(1)
        terminal.send("\x15printf new")
        terminal.read_until(b"-current")
        self.assertNotIn(b"old-stale", terminal.output)
        terminal.send("\t\r")
        terminal.prompt()
        self.assertFalse((self.cwd / "never.txt").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
