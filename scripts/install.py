#!/usr/bin/env python3

import argparse
import hashlib
import json
import os
from pathlib import Path
import pwd
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parent.parent
BEGIN = b"# >>> proto >>>"
END = b"# <<< proto <<<"


def read_owned(path, maximum=256 * 1024 * 1024):
    try:
        descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
    except FileNotFoundError:
        return b"", None
    with os.fdopen(descriptor, "rb") as file:
        info = os.fstat(file.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_nlink != 1 or info.st_size > maximum:
            raise RuntimeError(f"Refusing to change an unsafe or unowned file: {path}")
        contents = file.read(maximum + 1)
        if len(contents) > maximum:
            raise RuntimeError(f"Refusing to change an oversized file: {path}")
        return contents, stat.S_IMODE(info.st_mode)


def write_owned(path, contents, mode, expected=None):
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    info = path.parent.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o022:
        raise RuntimeError(f"Refusing to write through an unsafe directory: {path.parent}")
    current = read_owned(path)
    if expected is not None and current != expected:
        raise RuntimeError(f"File changed during installation; leaving it unchanged: {path}")
    descriptor, temporary = tempfile.mkstemp(prefix=".proto-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as file:
            os.fchmod(file.fileno(), mode)
            file.write(contents)
            file.flush()
            os.fsync(file.fileno())
        if read_owned(path) != current:
            raise RuntimeError(f"File changed during installation; leaving it unchanged: {path}")
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def managed_span(contents):
    starts, ends = [], []
    offset = 0
    for line in contents.splitlines(keepends=True):
        if line.rstrip(b"\r\n") == BEGIN:
            starts.append(offset)
        if line.rstrip(b"\r\n") == END:
            ends.append(offset + len(line))
        offset += len(line)
    if not starts and not ends:
        return None
    if len(starts) != 1 or len(ends) != 1 or starts[0] >= ends[0]:
        raise RuntimeError("Malformed or duplicate proto shell block; leaving the file unchanged")
    start = starts[0]
    if start > 0 and contents[start - 1:start] == b"\n":
        start -= 1
    return start, ends[0]


def shell_block(prefix, shell):
    binary = shlex.quote(str(prefix / "bin/proto"))
    script = shlex.quote(str(prefix / f"share/proto/proto.{shell}"))
    return (f"{BEGIN.decode()}\n"
            f"if [[ $- == *i* && -r {script} ]]; then\n"
            "    if [[ -z ${PROTO_BIN:-} ]]; then\n"
            f"        PROTO_BIN={binary}\n"
            "    fi\n"
            "    if [[ -x $PROTO_BIN ]]; then\n"
            f"        source {script}\n"
            "    fi\n"
            "fi\n"
            f"{END.decode()}\n").encode()


def shell_path(shell):
    home = Path.home()
    if shell == "zsh" and os.environ.get("ZDOTDIR"):
        home = Path(os.environ["ZDOTDIR"]).expanduser()
        if not home.is_absolute():
            raise RuntimeError("ZDOTDIR must be absolute to install automatic Zsh integration")
    return home / f".{shell}rc"


def edit_shell(path, prefix, shell, remove=False):
    contents, mode = read_owned(path, 4 * 1024 * 1024)
    span = managed_span(contents)
    block = shell_block(prefix, shell)
    if remove:
        if span is not None:
            write_owned(path, contents[:span[0]] + contents[span[1]:], mode if mode is not None else 0o600,
                        expected=(contents, mode))
        return
    if span is not None:
        existing = contents[span[0]:span[1]].lstrip(b"\n")
        if existing != block:
            raise RuntimeError(f"Proto's block was changed in {path}; remove it explicitly before reinstalling")
        return
    separator = b"\n" if contents else b""
    write_owned(path, contents + separator + block, mode if mode is not None else 0o600,
                expected=(contents, mode))


def render_unit(binary):
    value = str(binary)
    if not binary.is_absolute() or not value.isprintable() or any(character in value for character in "'\"\\"):
        raise RuntimeError("The service binary must have a printable absolute path without quotes or backslashes")
    value = value.replace("%", "%%")
    return (ROOT / "systemd/proto.service.in").read_text().replace("@PROTO_BIN@", value).encode()


def install_files(build, prefix):
    binary = build / "proto"
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise RuntimeError(f"Build proto first; executable not found at {binary}")
    cache = build / "CMakeCache.txt"
    if cache.is_file() and "PROTO_ENABLE_SANITIZERS:BOOL=ON" in cache.read_text():
        raise RuntimeError("Install a Release build, not a sanitizer build")
    targets = {"bin/proto": (binary.read_bytes(), 0o755),
               "share/proto/proto.bash": ((ROOT / "shell/proto.bash").read_bytes(), 0o644),
               "share/proto/proto.zsh": ((ROOT / "shell/proto.zsh").read_bytes(), 0o644)}
    module = build / "proto-bash.so"
    if module.is_file():
        targets["lib/proto/proto-bash.so"] = (module.read_bytes(), 0o755)
    manifest_path = prefix / "share/proto/install.json"
    data, manifest_mode = read_owned(manifest_path, 65536)
    try:
        manifest = json.loads(data) if data else {"version": 1, "files": {}}
        if manifest["version"] != 1 or not isinstance(manifest["files"], dict):
            raise ValueError
    except (KeyError, ValueError, TypeError):
        raise RuntimeError("Invalid proto installation manifest; leaving installed files unchanged") from None
    originals = {}
    for name, (contents, _mode) in targets.items():
        current, mode = read_owned(prefix / name)
        originals[name] = current, mode
        if mode is not None and current != contents and hashlib.sha256(current).hexdigest() != manifest["files"].get(name):
            raise RuntimeError(f"Refusing to overwrite a changed or unrelated file: {prefix / name}")
    for name, (contents, mode) in targets.items():
        write_owned(prefix / name, contents, mode, expected=originals[name])
        manifest["files"][name] = hashlib.sha256(contents).hexdigest()
    write_owned(manifest_path, (json.dumps(manifest, indent=2) + "\n").encode(), 0o600,
                expected=(data, manifest_mode))


def systemctl(*arguments):
    if not sys.platform.startswith("linux") or shutil.which("systemctl") is None:
        raise RuntimeError("The systemd installer requires Linux and systemctl")
    subprocess.run(["systemctl", "--user", *arguments], check=True)


def install_service(prefix, no_start=False, linger=False):
    if not sys.platform.startswith("linux") or shutil.which("systemctl") is None:
        raise RuntimeError("The systemd installer requires Linux and systemctl")
    configuration = os.environ.get("XDG_CONFIG_HOME", "")
    directory = Path(configuration) if configuration and Path(configuration).is_absolute() else Path.home() / ".config"
    unit = directory / "systemd/user/proto.service"
    contents = render_unit(prefix / "bin/proto")
    current, mode = read_owned(unit, 65536)
    manifest_path = prefix / "share/proto/install.json"
    data, manifest_mode = read_owned(manifest_path, 65536)
    try:
        manifest = json.loads(data) if data else {"version": 1, "files": {}}
        previous = manifest.get("systemd_unit", {})
        if not isinstance(previous, dict):
            raise ValueError
    except (ValueError, TypeError, AttributeError):
        raise RuntimeError("Invalid proto installation manifest") from None
    recorded = previous.get("path") == str(unit) and previous.get("sha256") == hashlib.sha256(current).hexdigest()
    if mode is not None and current != contents and not recorded:
        raise RuntimeError(f"Refusing to overwrite a modified systemd unit: {unit}")
    write_owned(unit, contents, 0o644, expected=(current, mode))
    manifest["systemd_unit"] = {"path": str(unit), "sha256": hashlib.sha256(contents).hexdigest()}
    write_owned(manifest_path, (json.dumps(manifest, indent=2) + "\n").encode(), 0o600,
                expected=(data, manifest_mode))
    if linger:
        if shutil.which("loginctl") is None:
            raise RuntimeError("loginctl is required to enable boot startup without login")
        user = pwd.getpwuid(os.getuid()).pw_name
        subprocess.run(["loginctl", "--no-ask-password", "enable-linger", user], check=True)
    systemctl("daemon-reload")
    systemctl("enable", "proto.service")
    if not no_start:
        systemctl("restart", "proto.service")
        environment = {**os.environ}
        runtime = environment.get("XDG_RUNTIME_DIR")
        if runtime:
            environment["PROTO_DAEMON_SOCKET"] = str(Path(runtime) / "proto/proto.sock")
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            result = subprocess.run([str(prefix / "bin/proto"), "--daemon-status"], env=environment,
                                    capture_output=True, text=True, timeout=2)
            if result.returncode == 0:
                print("proto.service is enabled and ready")
                return
            time.sleep(0.05)
        raise RuntimeError("proto.service did not become ready; inspect systemctl --user status proto.service")
    print("proto.service is enabled; it will start with the user manager")


def main():
    parser = argparse.ArgumentParser(description="Install proto for the current user. Shell and systemd changes require explicit flags.")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--prefix", type=Path, default=Path.home() / ".local")
    parser.add_argument("--shell", choices=("bash", "zsh", "both"), help="opt in to an automatic shell startup block")
    parser.add_argument("--systemd", action="store_true", help="install and enable the user daemon")
    parser.add_argument("--linger", action="store_true", help="allow the user service to start at boot, before login")
    parser.add_argument("--no-start", action="store_true", help="enable the service without starting it now")
    parser.add_argument("--remove-shell", choices=("bash", "zsh", "both"), help="remove only proto's managed startup block")
    parser.add_argument("--disable-service", action="store_true", help="stop and disable the service; keep installed files")
    options = parser.parse_args()
    if (options.linger or options.no_start) and not options.systemd:
        parser.error("--linger and --no-start require --systemd")
    removing = options.remove_shell is not None or options.disable_service
    if removing and (options.shell or options.systemd):
        parser.error("removal options cannot be combined with installation options")
    if options.systemd and (not sys.platform.startswith("linux") or shutil.which("systemctl") is None):
        parser.error("--systemd requires Linux and systemctl")
    prefix = options.prefix.expanduser().absolute()
    if options.systemd:
        render_unit(prefix / "bin/proto")
    if removing:
        if options.disable_service:
            systemctl("disable", "--now", "proto.service")
        selected = options.remove_shell
    else:
        install_files(options.build_dir.expanduser().absolute(), prefix)
        if options.systemd:
            install_service(prefix, options.no_start, options.linger)
        selected = options.shell
    for shell in (("bash", "zsh") if selected == "both" else (selected,) if selected else ()):
        path = shell_path(shell)
        edit_shell(path, prefix, shell, remove=removing)
        print(f'{"Removed" if removing else "Enabled"} automatic {shell} integration: {path}')
    if not removing:
        print(f"Installed proto in {prefix}. Open a new terminal to load automatic integration.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"proto installer: {error}", file=sys.stderr)
        sys.exit(1)
