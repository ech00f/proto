#!/usr/bin/env python3

import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parent.parent
specification = importlib.util.spec_from_file_location("proto_installer", ROOT / "scripts/install.py")
installer = importlib.util.module_from_spec(specification)
specification.loader.exec_module(installer)


class InstallTests(unittest.TestCase):
    def setUp(self):
        base = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None
        self.directory = tempfile.TemporaryDirectory(prefix="proto-install-", dir=base)
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.prefix = self.root / "local with spaces"
        self.rc = self.root / ".zshrc"

    def test_shell_install_is_opt_in_idempotent_and_preserves_user_content(self):
        for original in (b"", b"export EXISTING=yes\n", b"export EXISTING=yes", b"# keep custom bindings\n\n"):
            with self.subTest(original=original):
                self.rc.write_bytes(original)
                self.rc.chmod(0o640)
                installer.edit_shell(self.rc, self.prefix, "zsh")
                first = self.rc.read_bytes()
                self.assertTrue(first.startswith(original))
                self.assertEqual(first.count(installer.BEGIN), 1)
                self.assertEqual(self.rc.stat().st_mode & 0o777, 0o640)
                installer.edit_shell(self.rc, self.prefix, "zsh")
                self.assertEqual(self.rc.read_bytes(), first)
                installer.edit_shell(self.rc, self.prefix, "zsh", remove=True)
                self.assertEqual(self.rc.read_bytes(), original)

    def test_remove_keeps_content_added_after_installation(self):
        original = b"# initial settings\n"
        later = b"# newer user settings\n"
        self.rc.write_bytes(original)
        installer.edit_shell(self.rc, self.prefix, "zsh")
        with self.rc.open("ab") as file:
            file.write(later)
        installer.edit_shell(self.rc, self.prefix, "zsh", remove=True)
        self.assertEqual(self.rc.read_bytes(), original + later)

    def test_changed_and_duplicate_blocks_are_not_overwritten(self):
        installer.edit_shell(self.rc, self.prefix, "zsh")
        changed = self.rc.read_bytes().replace(b"PROTO_BIN=", b"USER_CHANGED_BIN=")
        self.rc.write_bytes(changed)
        with self.assertRaises(RuntimeError):
            installer.edit_shell(self.rc, self.prefix, "zsh")
        self.assertEqual(self.rc.read_bytes(), changed)
        self.rc.write_bytes(changed + changed)
        with self.assertRaises(RuntimeError):
            installer.edit_shell(self.rc, self.prefix, "zsh", remove=True)

    def test_atomic_write_does_not_discard_concurrent_user_changes(self):
        self.rc.write_bytes(b"original\n")
        original = installer.read_owned(self.rc)
        self.rc.write_bytes(b"new user change\n")
        with self.assertRaises(RuntimeError):
            installer.write_owned(self.rc, b"replacement\n", 0o600, expected=original)
        self.assertEqual(self.rc.read_bytes(), b"new user change\n")

    def test_atomic_write_rechecks_the_file_before_replacing_it(self):
        self.rc.write_bytes(b"original\n")
        original_fsync = installer.os.fsync

        def edit_while_writing(descriptor):
            self.rc.write_bytes(b"concurrent edit\n")
            original_fsync(descriptor)

        with patch.object(installer.os, "fsync", side_effect=edit_while_writing):
            with self.assertRaises(RuntimeError):
                installer.write_owned(self.rc, b"replacement\n", 0o600)
        self.assertEqual(self.rc.read_bytes(), b"concurrent edit\n")
        self.assertEqual(list(self.root.glob(".proto-*")), [])

    def test_rc_symlinks_and_hard_links_are_not_modified(self):
        victim = self.root / "victim"
        victim.write_text("keep me")
        self.rc.symlink_to(victim)
        with self.assertRaises(OSError):
            installer.edit_shell(self.rc, self.prefix, "zsh")
        self.assertEqual(victim.read_text(), "keep me")
        self.rc.unlink()
        os.link(victim, self.rc)
        with self.assertRaises(RuntimeError):
            installer.edit_shell(self.rc, self.prefix, "zsh")
        self.assertEqual(victim.read_text(), "keep me")

    def test_shell_blocks_quote_paths_and_ignore_noninteractive_shells(self):
        for shell in ("bash", "zsh"):
            with self.subTest(shell=shell):
                block = installer.shell_block(self.prefix / "quote' and $value", shell)
                self.rc.write_bytes(block)
                result = subprocess.run(["bash", "-n", str(self.rc)], capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                result = subprocess.run(["bash", "-c", f"source '{self.rc}'; printf '%s' \"${{PROTO_BIN-unset}}\""],
                                        env={key: value for key, value in os.environ.items() if key != "PROTO_BIN"},
                                        capture_output=True, text=True)
                self.assertEqual(result.stdout, "unset")

    def test_service_unit_is_endpoint_independent_and_escapes_systemd_paths(self):
        unit = installer.render_unit(self.prefix / '$cash%percent/bin/proto').decode()
        self.assertIn("--daemon", unit)
        self.assertIn('$cash%%percent', unit)
        self.assertIn('ExecStart=:"', unit)
        self.assertIn("WantedBy=default.target", unit)
        self.assertIn("RuntimeDirectoryMode=0700", unit)
        self.assertNotIn("ollama", unit.lower())
        self.assertNotIn("PROTO_API_KEY", unit)
        for binary in (Path("relative/proto"), self.prefix / 'quote"/proto',
                       self.prefix / "quote'/proto", self.prefix / "back\\slash/proto"):
            with self.subTest(binary=binary):
                with self.assertRaises(RuntimeError):
                    installer.render_unit(binary)

    @unittest.skipUnless(shutil.which("systemd-analyze"), "systemd-analyze is not installed")
    def test_service_unit_passes_systemd_verification_with_spaces_and_literals(self):
        binary = self.prefix / "$cash%literal/bin/proto"
        binary.parent.mkdir(parents=True)
        binary.symlink_to(shutil.which("true"))
        unit = self.root / "proto.service"
        unit.write_bytes(installer.render_unit(binary))
        runtime = self.root / "runtime"
        runtime.mkdir(mode=0o700)
        environment = {key: value for key, value in os.environ.items() if not key.startswith(("XDG_", "DBUS_"))}
        environment.update({"HOME": str(self.root), "XDG_RUNTIME_DIR": str(runtime)})
        result = subprocess.run(["systemd-analyze", "--user", "verify", str(unit)],
                                env=environment, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_installed_files_are_updated_only_if_unchanged(self):
        build = self.root / "build"
        build.mkdir()
        binary = build / "proto"
        binary.write_bytes(b"first binary")
        binary.chmod(0o755)
        installer.install_files(build, self.prefix)
        self.assertFalse(self.rc.exists())
        binary.write_bytes(b"updated binary")
        installer.install_files(build, self.prefix)
        installed = self.prefix / "bin/proto"
        self.assertEqual(installed.read_bytes(), b"updated binary")
        installed.write_bytes(b"user modification")
        binary.write_bytes(b"another binary")
        with self.assertRaises(RuntimeError):
            installer.install_files(build, self.prefix)
        self.assertEqual(installed.read_bytes(), b"user modification")
        manifest = json.loads((self.prefix / "share/proto/install.json").read_text())
        self.assertIn("bin/proto", manifest["files"])

    def test_service_install_does_not_overwrite_modified_units(self):
        unit = self.root / "config/systemd/user/proto.service"
        unit.parent.mkdir(parents=True)
        unit.write_text("user service")
        with patch.dict(os.environ, {"XDG_CONFIG_HOME": str(self.root / "config")}):
            with patch.object(installer.sys, "platform", "linux"), patch.object(installer.shutil, "which", return_value="/fake/systemctl"):
                with self.assertRaises(RuntimeError):
                    installer.install_service(self.prefix, no_start=True)
        self.assertEqual(unit.read_text(), "user service")

    def test_service_updates_only_its_unchanged_recorded_unit(self):
        unit = self.root / "config/systemd/user/proto.service"
        with patch.dict(os.environ, {"XDG_CONFIG_HOME": str(self.root / "config")}):
            with patch.object(installer.sys, "platform", "linux"), patch.object(installer.shutil, "which", return_value="/fake/systemctl"):
                with patch.object(installer.subprocess, "run"):
                    installer.install_service(self.prefix, no_start=True)
                    updated = unit.read_bytes() + b"# updated template\n"
                    with patch.object(installer, "render_unit", return_value=updated):
                        installer.install_service(self.prefix, no_start=True)
                    self.assertEqual(unit.read_bytes(), updated)
                    unit.write_bytes(updated + b"# user change\n")
                    with self.assertRaises(RuntimeError):
                        installer.install_service(self.prefix, no_start=True)
        self.assertTrue(unit.read_bytes().endswith(b"# user change\n"))

    def test_service_enable_and_linger_use_explicit_user_commands(self):
        with patch.dict(os.environ, {"XDG_CONFIG_HOME": str(self.root / "config")}):
            with patch.object(installer.sys, "platform", "linux"), patch.object(installer.shutil, "which", return_value="/fake/command"):
                with patch.object(installer.subprocess, "run") as run:
                    installer.install_service(self.prefix, no_start=True, linger=True)
        commands = [call.args[0] for call in run.call_args_list]
        self.assertEqual(commands[0][:3], ["loginctl", "--no-ask-password", "enable-linger"])
        self.assertEqual(commands[1], ["systemctl", "--user", "daemon-reload"])
        self.assertEqual(commands[2], ["systemctl", "--user", "enable", "proto.service"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
