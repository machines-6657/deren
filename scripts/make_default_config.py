#!/usr/bin/env python3
"""Generate a default config.toml for deren.

Asks only where to put it, then copies config.example.toml verbatim to that
location (renamed config.toml). For a fully interactive setup with per-key
prompts (types + defaults), use make_config.py instead.

Usage:
    python scripts/make_default_config.py
    python scripts/make_default_config.py [output_dir]
"""

from __future__ import annotations

import os
import shutil
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXAMPLE = os.path.join(REPO_ROOT, "config.example.toml")


def ask_output_dir() -> str:
    default = REPO_ROOT  # the cwd the renderer reads config.toml from
    while True:
        raw = input(f"where should config.toml be written? [default: {default}]\n> ").strip()
        path = raw or default
        path = os.path.abspath(path)
        if os.path.isdir(path):
            return path
        try:
            os.makedirs(path, exist_ok=True)
            print(f"(created directory {path})")
            return path
        except OSError as exc:
            print(f"cannot create {path}: {exc}", file=sys.stderr)


def main() -> int:
    if len(sys.argv) > 1:
        out_dir = os.path.abspath(sys.argv[1])
        if not os.path.isdir(out_dir):
            try:
                os.makedirs(out_dir, exist_ok=True)
            except OSError as exc:
                print(f"cannot create {out_dir}: {exc}", file=sys.stderr)
                return 1
    else:
        print("== deren: write a default config.toml ==")
        out_dir = ask_output_dir()

    if not os.path.isfile(EXAMPLE):
        print(f"error: config.example.toml not found at {EXAMPLE}", file=sys.stderr)
        return 1

    dest = os.path.join(out_dir, "config.toml")
    if os.path.exists(dest):
        answer = input(f"{dest} already exists - overwrite? [y/N] ").strip().lower()
        if answer not in ("y", "yes"):
            print("aborted - nothing written.")
            return 1

    shutil.copyfile(EXAMPLE, dest)
    print(f"wrote {dest}")
    print("run the renderer from that directory, or point at it with --config config.toml")
    return 0


if __name__ == "__main__":
    sys.exit(main())
