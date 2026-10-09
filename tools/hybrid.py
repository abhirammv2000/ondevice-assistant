"""Run requests through the on-device engine and send only what it cannot handle to a larger model.

    python tools/hybrid.py --cli build/assist_cli --model models/clinc150.pmodel "tell me about the history of rome"
    python tools/hybrid.py --cli build/assist_cli --model models/clinc150.pmodel --file utterances.txt

The larger model is any server with an Ollama-style /api/chat endpoint (a local one by default, so nothing leaves the
machine in this demo). What matters is what is sent to it: the engine's `forward_text`, which has had emails, phone
numbers, card numbers, social security numbers and contact names replaced. The original utterance is never put in the
request. `Escalator.last_request` keeps the exact body that was sent, so a test can check this.
"""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path

SYSTEM_PROMPT = (
    "You are the cloud part of a voice assistant. The user's request may contain placeholders such as <NAME>, "
    "<PHONE> and <EMAIL> where personal details were removed on the device. Answer briefly, in one or two sentences, "
    "and do not ask for the removed details."
)


@dataclass
class Escalator:
    """Sends text to a larger model. It never raises: a down server is an expected case for an assistant."""

    url: str = "http://127.0.0.1:11434/api/chat"
    model: str = "qwen2.5-coder:7b"
    timeout_s: float = 60.0
    last_request: bytes = field(default=b"", init=False)

    def ask(self, text: str) -> tuple[str, str]:
        """Returns (answer, error). Exactly one of them is empty."""
        body = json.dumps({
            "model": self.model,
            "stream": False,
            "messages": [{"role": "system", "content": SYSTEM_PROMPT}, {"role": "user", "content": text}],
        }).encode("utf-8")
        self.last_request = body
        request = urllib.request.Request(self.url, data=body, headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(request, timeout=self.timeout_s) as response:
                payload = json.loads(response.read().decode("utf-8"))
            return str(payload["message"]["content"]).strip(), ""
        except urllib.error.URLError as error:
            return "", f"larger model not reachable: {error.reason}"
        except (TimeoutError, OSError) as error:
            return "", f"larger model failed: {error}"
        except (KeyError, ValueError, TypeError):
            return "", "larger model sent a reply this demo cannot read"


def run_engine(cli: str, model: str, utterance: str, extra: list[str]) -> dict:
    cli = shutil.which(cli) or str(Path(cli).resolve())  # Windows does not accept "dir/prog.exe" as a relative path
    completed = subprocess.run([cli, "--model", model, *extra, utterance], capture_output=True, text=True, timeout=30,
                               check=False)
    if completed.returncode != 0:
        raise RuntimeError(f"{cli} failed: {completed.stderr.strip()}")
    return json.loads(completed.stdout)


@dataclass
class Outcome:
    utterance: str
    route: str
    reply: str
    sent_to_cloud: str
    seconds_on_device: float
    seconds_in_cloud: float
    error: str = ""


def handle(utterance: str, cli: str, model: str, escalator: Escalator, extra: list[str] | None = None) -> Outcome:
    started = time.perf_counter()
    result = run_engine(cli, model, utterance, extra or [])
    local_seconds = time.perf_counter() - started
    if result["route"] != "escalate":
        return Outcome(utterance, result["route"], result["reply"], "", local_seconds, 0.0)

    started = time.perf_counter()
    answer, error = escalator.ask(result["forward_text"])
    return Outcome(utterance, "escalate", answer, result["forward_text"], local_seconds, time.perf_counter() - started, error)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("utterance", nargs="?")
    parser.add_argument("--file", help="a text file with one utterance per line")
    parser.add_argument("--cli", required=True, help="path to the assist_cli program")
    parser.add_argument("--model", required=True, help="path to the .pmodel file")
    parser.add_argument("--contacts", help="a TSV of id and name, passed to the engine")
    parser.add_argument("--url", default=Escalator.url)
    parser.add_argument("--llm", default=Escalator.model)
    parser.add_argument("--timeout", type=float, default=Escalator.timeout_s, help="seconds to wait for the larger model")
    args = parser.parse_args()
    if bool(args.utterance) == bool(args.file):
        parser.error("give either one utterance or --file")

    lines = [args.utterance] if args.utterance else [x.strip() for x in open(args.file, encoding="utf-8") if x.strip()]
    extra = ["--contacts", args.contacts] if args.contacts else []
    escalator = Escalator(url=args.url, model=args.llm, timeout_s=args.timeout)
    local = cloud = 0
    for line in lines:
        out = handle(line, args.cli, args.model, escalator, extra)
        where = "device" if out.route != "escalate" else "cloud "
        local += out.route != "escalate"
        cloud += out.route == "escalate"
        print(f"[{where}] {line}")
        if out.sent_to_cloud:
            print(f"         sent to the larger model: {out.sent_to_cloud!r}")
        print(f"         {out.error or out.reply}   ({out.seconds_on_device * 1000:.0f} ms engine"
              + (f", {out.seconds_in_cloud:.1f} s larger model)" if out.sent_to_cloud else ")"))
    print(f"\n{local} answered on the device, {cloud} sent on.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
