#!/usr/bin/env python3

import argparse
import html
import http.server
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import threading
import unicodedata

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tests"))
from terminal_process import TerminalProcess


BACKGROUND = "#0c101c"
FOREGROUND = "#e0e8f8"
PALETTE = (
    "#131929", "#ff778d", "#62e4b8", "#f9c873", "#6bbbff", "#b097ff", "#73daca", "#e0e8f8",
    "#70829f", "#ff778d", "#62e4b8", "#f9c873", "#6bbbff", "#b097ff", "#73daca", "#ffffff",
)
ESCAPE = re.compile(r"\x1b(?:\[([0-?]*)([ -/]*)([@-~])|([78]))")


class TerminalFrame:
    def __init__(self, columns=100, rows=24):
        self.columns, self.rows = columns, rows
        self.foreground, self.background = FOREGROUND, BACKGROUND
        self.row = self.column = 0
        self.saved = (0, 0)
        self.alternate = None
        self.cursor = True
        self.clear()

    def clear(self):
        self.cells = [[(" ", self.foreground, self.background) for _ in range(self.columns)]
                      for _ in range(self.rows)]

    def colors(self, values):
        index = 0
        while index < len(values):
            value = values[index]
            if value == 0:
                self.foreground, self.background = FOREGROUND, BACKGROUND
            elif 30 <= value <= 37 or 90 <= value <= 97:
                self.foreground = PALETTE[value - (30 if value < 90 else 82)]
            elif 40 <= value <= 47 or 100 <= value <= 107:
                self.background = PALETTE[value - (40 if value < 100 else 92)]
            elif value == 39:
                self.foreground = FOREGROUND
            elif value == 49:
                self.background = BACKGROUND
            elif value in (38, 48) and values[index + 1:index + 2] == [2] and index + 4 < len(values):
                color = "#" + "".join(f"{part:02x}" for part in values[index + 2:index + 5])
                if value == 38:
                    self.foreground = color
                else:
                    self.background = color
                index += 4
            index += 1

    def control(self, parameters, command, simple):
        if simple:
            if simple == "7":
                self.saved = (self.row, self.column)
            else:
                self.row, self.column = self.saved
            return
        values = [int(value or 0) for value in parameters.lstrip("?").split(";")]
        amount = values[0] or 1
        if command == "m":
            self.colors(values)
        elif command in ("H", "f"):
            self.row = min(self.rows - 1, max(0, amount - 1))
            self.column = min(self.columns - 1, max(0, (values[1] or 1) - 1)) if len(values) > 1 else 0
        elif command == "G":
            self.column = min(self.columns - 1, max(0, amount - 1))
        elif command == "J" and values[0] == 2:
            self.clear()
        elif command == "K":
            start = 0 if values[0] in (1, 2) else self.column
            end = self.column + 1 if values[0] == 1 else self.columns
            self.cells[self.row][start:end] = [(" ", self.foreground, self.background)] * (end - start)
        elif command in ("h", "l") and parameters.startswith("?"):
            if 25 in values:
                self.cursor = command == "h"
            if 1049 in values:
                if command == "h":
                    self.alternate = (self.cells, self.row, self.column)
                    self.clear()
                    self.row = self.column = 0
                elif self.alternate is not None:
                    self.cells, self.row, self.column = self.alternate
                    self.alternate = None

    def text(self, text):
        for character in text:
            if character == "\r":
                self.column = 0
            elif character == "\n":
                self.row += 1
                if self.row >= self.rows:
                    self.cells.pop(0)
                    self.cells.append([(" ", self.foreground, self.background)] * self.columns)
                    self.row = self.rows - 1
            elif character == "\b":
                self.column = max(0, self.column - 1)
            elif character.isprintable() and not unicodedata.combining(character):
                width = 2 if unicodedata.east_asian_width(character) in ("W", "F") else 1
                if self.column + width <= self.columns:
                    self.cells[self.row][self.column] = (character, self.foreground, self.background)
                    if width == 2:
                        self.cells[self.row][self.column + 1] = ("", self.foreground, self.background)
                    self.column += width

    def feed(self, output):
        text = output.decode("utf-8", errors="replace")
        start = 0
        for match in ESCAPE.finditer(text):
            self.text(text[start:match.start()])
            self.control(match[1] or "", match[3], match[4])
            start = match.end()
        self.text(text[start:])

    def svg(self, label, visible_rows=None):
        visible_rows = visible_rows or self.rows
        width, height = self.columns * 9 + 48, visible_rows * 19 + 104
        parts = [
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
            f'viewBox="0 0 {width} {height}" role="img" aria-labelledby="title description">',
            f'  <title id="title">proto — {html.escape(label)}</title>',
            '  <desc id="description">Rendered from a real disposable proto terminal session. '
            'Model responses are supplied by a local test fixture.</desc>',
            f'  <rect width="{width}" height="{height}" rx="16" fill="{BACKGROUND}"/>',
            f'  <path d="M16 0h{width - 32}a16 16 0 0 1 16 16v28H0V16A16 16 0 0 1 16 0" fill="#131929"/>',
            '  <circle cx="24" cy="22" r="5" fill="#ff778d"/>',
            '  <circle cx="42" cy="22" r="5" fill="#f9c873"/>',
            '  <circle cx="60" cy="22" r="5" fill="#62e4b8"/>',
            f'  <text x="{width / 2}" y="27" text-anchor="middle" fill="#70829f" '
            'font-family="monospace" font-size="12">proto / terminal</text>',
            '  <g font-family="Noto Sans Mono, DejaVu Sans Mono, monospace" font-size="15" xml:space="preserve">',
        ]
        for row, cells in enumerate(self.cells[:visible_rows]):
            column = 0
            while column < self.columns:
                color = cells[column][2]
                end = column + 1
                while end < self.columns and cells[end][2] == color:
                    end += 1
                if color != BACKGROUND:
                    parts.append(f'    <rect x="{24 + column * 9}" y="{56 + row * 19}" '
                                 f'width="{(end - column) * 9}" height="19" fill="{color}"/>')
                column = end
            column = 0
            while column < self.columns:
                color = cells[column][1]
                end = column + 1
                while end < self.columns and cells[end][1] == color:
                    end += 1
                text = "".join(cell[0] for cell in cells[column:end])
                if text.strip():
                    parts.append(f'    <text x="{24 + column * 9}" y="{71 + row * 19}" '
                                 f'fill="{color}" textLength="{(end - column) * 9}" '
                                 f'lengthAdjust="spacingAndGlyphs">{html.escape(text)}</text>')
                column = end
        parts.append('  </g>')
        if self.cursor and self.column < self.columns:
            parts.append(f'  <path d="M{24 + self.column * 9} {56 + self.row * 19}v18" '
                         'stroke="#62e4b8" stroke-width="2"/>')
        parts.extend([
            f'  <path d="M24 {height - 37}h{width - 48}" stroke="#232d45"/>',
            f'  <text x="24" y="{height - 15}" fill="#62e4b8" font-family="monospace" '
            f'font-size="12">{html.escape(label)}</text>',
            '</svg>',
        ])
        return "\n".join(parts) + "\n"

    def image(self, label, font):
        width, height = self.columns * 9 + 48, self.rows * 19 + 104
        image = Image.new("RGB", (width, height), BACKGROUND)
        draw = ImageDraw.Draw(image)
        draw.rectangle((0, 0, width, 43), fill="#131929")
        for x, color in ((24, "#ff778d"), (42, "#f9c873"), (60, "#62e4b8")):
            draw.ellipse((x - 5, 17, x + 5, 27), fill=color)
        draw.text((width / 2, 22), "proto / terminal", font=font, fill="#70829f", anchor="mm")
        for row, cells in enumerate(self.cells):
            for column, (character, foreground, background) in enumerate(cells):
                x, y = 24 + column * 9, 56 + row * 19
                if background != BACKGROUND:
                    draw.rectangle((x, y, x + 8, y + 18), fill=background)
                if character.strip():
                    draw.text((x, y + 15), character, font=font, fill=foreground, anchor="ls")
        if self.cursor and self.column < self.columns:
            x, y = 24 + self.column * 9, 56 + self.row * 19
            draw.line((x, y, x, y + 18), fill="#62e4b8", width=2)
        draw.line((24, height - 37, width - 24, height - 37), fill="#232d45")
        draw.text((24, height - 15), label, font=font, fill="#62e4b8", anchor="ls")
        return image.convert("P", palette=Image.Palette.ADAPTIVE, colors=96)


def main():
    parser = argparse.ArgumentParser(description="Render README demos using a local fixture, not a real model.")
    parser.add_argument("binary", type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / "docs")
    parser.add_argument("--font", default="DejaVuSansMono.ttf")
    options = parser.parse_args()
    binary = str(options.binary.resolve())
    font = ImageFont.truetype(options.font, 15)
    options.output.mkdir(parents=True, exist_ok=True)

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            context = json.loads(request["messages"][-1]["content"])
            if "task" in context:
                content = json.dumps({"title": "Prepare a demo", "steps": [
                    {"command": "mkdir -p demo", "explanation": "Create a working directory."},
                    {"command": "cd demo"}, {"command": "git init"},
                ]})
            elif context["prefix"] == "git st":
                content = json.dumps({"completions": ["git status --short", "git stash list"]})
            else:
                content = context["prefix"]
            payload = json.dumps({"choices": [{"message": {"content": content}}]}).encode()
            try:
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def log_message(self, *_arguments):
            pass

    base = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None
    with tempfile.TemporaryDirectory(prefix="proto-demo-", dir=base) as directory:
        root = Path(directory).resolve()
        home, cwd = root / "home", root / "home/project"
        cwd.mkdir(parents=True)
        (home / "Documents").mkdir()
        (cwd / "main.cpp").write_text("int main() {}\n")
        (cwd / "old-cache").write_text("keep this file\n")
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        environment = {key: value for key, value in os.environ.items() if not key.startswith("PROTO_")}
        environment.update({
            "HOME": str(home), "XDG_STATE_HOME": str(root / "state"), "XDG_CONFIG_HOME": str(root / "config"),
            "PROTO_MODEL": "qwen2.5-coder:7b", "PROTO_AI": "1", "PROTO_SHELL": "/bin/sh",
            "PROTO_API_BASE": f"http://127.0.0.1:{server.server_port}/v1", "PROTO_TIMEOUT_MS": "1000",
            "PROTO_DEBOUNCE_MS": "80", "TERM": "xterm-256color",
            "LC_ALL": "C.UTF-8" if sys.platform.startswith("linux") else "en_US.UTF-8",
        })
        for command in ("cd ~/Documents", "git diff --stat", "ls -la"):
            subprocess.run([binary, "--record", command], cwd=cwd, env=environment, check=True)
        terminal = TerminalProcess(binary, environment, cwd)
        thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.02})
        thread.start()
        screen = TerminalFrame()
        frames, durations = [], []
        consumed = 0

        def send(text):
            nonlocal consumed
            terminal.send(text)
            consumed = 0

        def capture(label, duration=1400, name=None):
            nonlocal consumed
            terminal.pump(0.08)
            screen.feed(terminal.output[consumed:])
            consumed = len(terminal.output)
            frames.append(screen.image(label, font))
            durations.append(duration)
            if name:
                rows = 5 if name == "terminal" else None
                (options.output / f"{name}.svg").write_text(screen.svg(label, rows), encoding="utf-8")

        try:
            terminal.resize(24, 100)
            terminal.read_until(b" $ ")
            screen.feed(terminal.output)
            consumed = len(terminal.output)
            send("\x0c")
            capture("01 / TYPE A COMMAND", 700)
            for text in ("g", "it", " st"):
                send(text)
                capture("01 / TYPE A COMMAND", 130)
            terminal.read_until(b"atus --short")
            capture("02 / GHOST TEXT - nothing has executed", 1900, "terminal")
            send("\x1b[1;3B")
            terminal.read_until(b"ash list")
            capture("03 / ALT + DOWN - choose another variant", 1700)
            send("\t")
            terminal.read_until(b"git stash list")
            capture("04 / TAB - accept text, not execution", 1400)
            send("\x1bOQ")
            terminal.read_until(b"YOUR TERMINAL")
            capture("05 / F2 - open the control center", 1900, "control-center")
            send("2")
            terminal.read_until(b"Alternatives")
            capture("06 / SETTINGS - model, privacy, timing, theme", 1600, "settings")
            send("3")
            terminal.read_until(b"git diff --stat")
            capture("07 / HISTORY - search and recall without executing", 1700, "history")
            send("\x1b")
            terminal.read_until(b"\x1b[?1049l")
            terminal.read_until(b"git stash list")
            capture("08 / ESC - your draft is still here", 1200)
            send("\x15?? create a demo project\r")
            terminal.read_until(b"CONFIRM")
            capture("09 / PLAN - review every step, then type RUN", 2400, "plan")
            send("\x1b")
            terminal.prompt()
            capture("10 / CANCEL - no plan commands have run", 1000)
            send("rm old-cache\r")
            terminal.read_until(b"DANGER / confirm command")
            capture("11 / DANGER - deletion waits for typed confirmation", 2100, "danger")
            send("\x1b")
            terminal.prompt()
            capture("12 / YOU DECIDE WHAT RUNS", 1200)
            assert not (cwd / "demo").exists()
            assert (cwd / "old-cache").read_text() == "keep this file\n"
            order = (4, 5, 6, 0, 1, 2, 3)
            inline = []
            for index in order:
                source = frames[index].convert("RGB")
                image = Image.new("RGB", (source.width, 199), BACKGROUND)
                image.paste(source.crop((0, 0, source.width, 151)), (0, 0))
                image.paste(source.crop((0, source.height - 48, source.width, source.height)), (0, 151))
                inline.append(image.convert("P", palette=Image.Palette.ADAPTIVE, colors=96))
            inline[0].save(options.output / "demo.gif", save_all=True, append_images=inline[1:],
                           duration=[durations[index] for index in order], loop=0, optimize=True, disposal=2)
            pages = (7, 8, 9, 11, 13)
            frames[pages[0]].save(options.output / "control-center.gif", save_all=True,
                                  append_images=[frames[index] for index in pages[1:]],
                                  duration=2200, loop=0, optimize=True, disposal=2)
        finally:
            terminal.close()
            server.shutdown()
            thread.join()
            server.server_close()
    print(f"Rendered 2 animations and 6 SVG screenshots in {options.output}")


if __name__ == "__main__":
    main()
