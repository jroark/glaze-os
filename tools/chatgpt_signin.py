#!/usr/bin/env python3
"""Create CODEX.AUTH using a dedicated official Codex CLI sign-in.

This is a one-time credential transfer, not a chat proxy or running service.
The temporary CODEX_HOME prevents refresh-token sharing with a desktop login.
"""
import argparse
import base64
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

CLIENT_ID = "app_EMoamEEZ73f0CkXaXp7hrann"
ISSUER = "https://auth.openai.com"


def clean_auth(raw):
    if len(raw) > 32768:
        raise ValueError("Codex login file exceeds the device limit")
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result: raise ValueError("Duplicate credential field")
            result[key] = value
        return result
    value = json.loads(raw, object_pairs_hook=unique)
    if not isinstance(value, dict) or value.get("auth_mode", "chatgpt") != "chatgpt" or value.get("OPENAI_API_KEY") is not None:
        raise ValueError("Sign in with ChatGPT, rather than an API key")
    tokens = value.get("tokens")
    if not isinstance(tokens, dict):
        raise ValueError("Codex did not save a transferable ChatGPT login")
    out = {}
    for key, maximum, required in (("access_token", 8191, True), ("refresh_token", 4095, True),
                                    ("id_token", 8191, False), ("account_id", 128, False)):
        text = tokens.get(key)
        if text is None and not required:
            continue
        if not isinstance(text, str) or not 1 <= len(text) <= maximum or not all(33 <= ord(c) <= 126 for c in text):
            raise ValueError("Invalid or oversized Codex login field: " + key)
        out[key] = text
    provenance = tokens.get("oauth_provenance")
    if provenance is not None and provenance != {"issuer": ISSUER, "client_id": CLIENT_ID}:
        raise ValueError("This Codex OAuth authority is not supported by the Glaze client")
    out["oauth_provenance"] = {"issuer": ISSUER, "client_id": CLIENT_ID}
    try:
        parts = out["access_token"].split(".")
        if len(parts) != 3 or not all(parts): raise ValueError()
        claims = json.loads(base64.urlsafe_b64decode(parts[1] + "=" * (-len(parts[1]) % 4)), object_pairs_hook=unique)
        account = claims["https://api.openai.com/auth"]["chatgpt_account_id"]
        if not isinstance(claims["exp"], int) or not 0 < claims["exp"] < 2**32: raise ValueError()
        if claims["https://api.openai.com/auth"].get("chatgpt_account_is_fedramp"): raise ValueError()
        if not isinstance(account, str) or not 1 <= len(account) <= 128 or not all(33 <= ord(c) <= 126 for c in account): raise ValueError()
        if out.get("account_id", account) != account: raise ValueError()
        out["account_id"] = account
    except (ValueError, KeyError, TypeError, UnicodeError) as exc:
        raise ValueError("Unsupported or inconsistent Codex account metadata") from exc
    return json.dumps({"auth_mode": "chatgpt", "OPENAI_API_KEY": None, "tokens": out}, separators=(",", ":")).encode()


def export(source, output):
    if output.exists():
        raise ValueError("Output already exists; choose another path or remove the old export first")
    payload = clean_auth(source.read_bytes())
    if len(payload) > 32768:
        raise ValueError("Export exceeds the device limit")
    output.parent.mkdir(parents=True, exist_ok=True)
    # Exclusive creation and mode 0600; credentials never go to stdout.
    fd = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as file:
        file.write(payload)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("CODEX.AUTH"))
    parser.add_argument("--codex", default="codex", help="official Codex CLI executable")
    parser.add_argument("--device-code", action="store_true", help="use Codex CLI's device-code login instead of its browser")
    parser.add_argument("--from-auth", type=Path, help="export a specified auth.json instead of starting a dedicated login")
    args = parser.parse_args()
    try:
        if args.output.exists():
            parser.error("output already exists; choose a new path")
        if args.from_auth:
            export(args.from_auth, args.output)
        else:
            executable = shutil.which(args.codex)
            if not executable:
                parser.error("install the official Codex CLI, or provide --codex PATH")
            with tempfile.TemporaryDirectory(prefix="glaze-codex-login-") as home:
                command = [executable, "-c", 'cli_auth_credentials_store="file"', "login"]
                if args.device_code: command.append("--device-auth")
                subprocess.run(command, env=dict(os.environ, CODEX_HOME=home), check=True)
                export(Path(home) / "auth.json", args.output)
        print(f"Saved {args.output}. Transfer it to Glaze, choose Import Codex login, then remove the transfer copy.")
    except (OSError, ValueError, RecursionError, subprocess.CalledProcessError) as exc:
        # No remote response bodies or credential values in errors.
        parser.exit(1, f"Could not export Codex login ({type(exc).__name__}). Check the sign-in and output path.\n")


if __name__ == "__main__":
    main()
