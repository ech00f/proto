import errno
import fcntl
import os
import pty
import select
import signal
import struct
import termios
import time


class TerminalProcess:
    def __init__(self, executable, environment, cwd, arguments=()):
        gate_read, gate_write = os.pipe()
        self.pid, self.master = pty.fork()
        if self.pid == 0:
            os.close(gate_write)
            os.read(gate_read, 1)
            os.close(gate_read)
            os.chdir(cwd)
            os.execve(executable, [executable, *arguments], environment)
        os.close(gate_read)
        self.original = termios.tcgetattr(self.master)
        fcntl.ioctl(self.master, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 120, 0, 0))
        os.write(gate_write, b"1")
        os.close(gate_write)
        self.output = b""
        self.status = None
        self.closed = False

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
                if error.errno == errno.EIO:
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
            raise AssertionError(f"Terminal did not print {text!r}: {self.output[-1200:]!r}")
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
            raise AssertionError(f"Terminal process did not exit: {self.output[-1200:]!r}")
        self.pump(0.01)
        return self.status

    def restored(self):
        return termios.tcgetattr(self.master) == self.original

    def resize(self, rows, columns):
        fcntl.ioctl(self.master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))

    def close(self, termination_signal=signal.SIGTERM):
        if self.closed:
            return
        if self.status is None:
            try:
                os.kill(self.pid, termination_signal)
            except ProcessLookupError:
                pass
            try:
                self.wait()
            except AssertionError:
                os.kill(self.pid, signal.SIGKILL)
                os.waitpid(self.pid, 0)
        os.close(self.master)
        self.closed = True
