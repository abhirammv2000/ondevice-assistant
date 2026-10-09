"""The larger model must only ever see the redacted text. A fake server records what it is sent."""
import json
import stat
import sys
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

import pytest

import hybrid


class Recorder(BaseHTTPRequestHandler):
    bodies: list = []

    def do_POST(self):
        length = int(self.headers["Content-Length"])
        Recorder.bodies.append(self.rfile.read(length))
        reply = json.dumps({"message": {"role": "assistant", "content": "  Rome was founded long ago.  "}}).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(reply)))
        self.end_headers()
        self.wfile.write(reply)

    def log_message(self, *args):
        pass


@pytest.fixture()
def server():
    Recorder.bodies = []
    httpd = HTTPServer(("127.0.0.1", 0), Recorder)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{httpd.server_port}/api/chat"
    httpd.shutdown()


@pytest.fixture()
def fake_cli(tmp_path):
    """A stand-in for assist_cli that prints a fixed result, so this test needs no build."""
    script = tmp_path / "fake_cli.py"
    script.write_text(
        "import json, sys\n"
        "utterance = sys.argv[-1]\n"
        "if 'rome' in utterance:\n"
        "    r = {'route': 'escalate', 'reply': '', 'forward_text': 'tell me about <NAME> in rome'}\n"
        "else:\n"
        "    r = {'route': 'on_device', 'reply': 'Done.', 'forward_text': ''}\n"
        "print(json.dumps(r))\n", encoding="utf-8")
    if sys.platform == "win32":
        wrapper = tmp_path / "fake_cli.cmd"
        wrapper.write_text(f'@"{sys.executable}" "{script}" %*\r\n', encoding="utf-8")
        return str(wrapper)
    wrapper = tmp_path / "fake_cli.sh"
    wrapper.write_text(f'#!/bin/sh\nexec "{sys.executable}" "{script}" "$@"\n', encoding="utf-8")
    wrapper.chmod(wrapper.stat().st_mode | stat.S_IEXEC)
    return str(wrapper)


def test_only_the_redacted_text_reaches_the_larger_model(server, fake_cli):
    escalator = hybrid.Escalator(url=server, timeout_s=5)
    out = hybrid.handle("tell me about Dana Whitfield in rome", fake_cli, "unused.pmodel", escalator)
    assert out.route == "escalate" and out.reply == "Rome was founded long ago."
    assert len(Recorder.bodies) == 1
    sent = json.loads(Recorder.bodies[0])
    assert sent["messages"][-1]["content"] == "tell me about <NAME> in rome"
    assert "Dana" not in Recorder.bodies[0].decode() and "Whitfield" not in Recorder.bodies[0].decode()


def test_on_device_requests_send_nothing(server, fake_cli):
    escalator = hybrid.Escalator(url=server, timeout_s=5)
    out = hybrid.handle("set a timer", fake_cli, "unused.pmodel", escalator)
    assert out.route == "on_device" and out.sent_to_cloud == "" and Recorder.bodies == []


def test_unreachable_larger_model_is_an_answer_not_a_crash(fake_cli):
    escalator = hybrid.Escalator(url="http://127.0.0.1:9/api/chat", timeout_s=2)
    out = hybrid.handle("tell me about rome", fake_cli, "unused.pmodel", escalator)
    assert out.route == "escalate" and out.reply == "" and "larger model" in out.error


def test_unreadable_reply_is_reported():
    class Junk(BaseHTTPRequestHandler):
        def do_POST(self):
            self.rfile.read(int(self.headers["Content-Length"]))
            self.send_response(200)
            self.send_header("Content-Length", "2")
            self.end_headers()
            self.wfile.write(b"{}")

        def log_message(self, *args):
            pass

    httpd = HTTPServer(("127.0.0.1", 0), Junk)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    try:
        answer, error = hybrid.Escalator(url=f"http://127.0.0.1:{httpd.server_port}/x", timeout_s=5).ask("hi")
    finally:
        httpd.shutdown()
    assert answer == "" and "cannot read" in error
