"""Offline tests for direct_report.py against a local mock HTTP server (127.0.0.1 only).

Run:  python tests/test_direct.py
"""
import io
import json
import os
import sys
import tempfile
import threading
from contextlib import redirect_stderr, redirect_stdout
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import direct_report as dr  # noqa: E402
import reading_report as rr  # noqa: E402
from test_offline import KIDS, activity_payload  # noqa: E402

REQUIRED = {"session-id": "s1", "at-main": "a1"}
EXTRA = {"ubid-main": "u1", "csm-hit": "c1", "i18n-prefs": "USD"}
STATE = {"scenario": "ok"}


def household_html():
    members = [{"role": "ADULT", "firstName": "Parent", "directedId": "p"}] + [
        {"role": "CHILD", "firstName": n, "directedId": i} for n, i in KIDS.items()]
    # Embedded as a JSON string literal, like the real page (escaped quotes and slashes)
    # Real page (2026-11-16): JSON.parse('{\\"members\\":...}') -- single-quoted JS literal, escaped quotes
    inner = json.dumps({"members": members, "householdId": "h"}).replace("/", "\\/")
    lit = "'" + inner.replace("\\", "\\\\").replace('"', '\\"').replace("'", "\\'") + "'"
    return f"<html><script type='text/javascript'>window.__PREFETCHED_FEATURE_GATING_MAP = {{}};\n" \
           f"window.__PREFETCHED_HOUSEHOLD = JSON.parse({lit});\n</script></html>"


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _cookies(self):
        c = {}
        for part in (self.headers.get("Cookie") or "").split(";"):
            if "=" in part:
                k, v = part.strip().split("=", 1)
                c[k] = v
        return c

    def _send(self, status, body=b"", ctype="text/html", headers=None):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _gate(self):
        s = STATE["scenario"]
        if s == "captcha":
            self._send(200, b"<form action='/errors/validateCaptcha'><input id='captchacharacters'></form>")
            return False
        if s == "needs_origin" and self.headers.get("Origin") is None:
            self._send(403, b"{}", "application/json")
            return False
        if any(self._cookies().get(k) != v for k, v in REQUIRED.items()):
            self._send(302, headers={"Location": "https://www.amazon.com/ap/signin?openid.return_to=x"})
            return False
        return True

    def do_GET(self):
        if self.path.startswith("/activities/household-summary") and self._gate():
            self._send(200, household_html().encode())

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        if self.path == rr.ACTIVITY_PATH and self._gate():
            payload = activity_payload(body["childDirectedId"], body)
            if STATE["scenario"] == "shape":
                payload = {"data": []}
            self._send(200, json.dumps(payload).encode(), "application/json")


def run(argv, scenario="ok", cookies=None):
    STATE["scenario"] = scenario
    srv = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory() as d:
        cf = Path(d) / "cookie.txt"
        cf.write_text("; ".join(f"{k}={v}" for k, v in (cookies or {**REQUIRED, **EXTRA}).items()))
        os.chmod(cf, 0o600)
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = dr.main(argv + ["--cookie-file", str(cf), "--base-url", f"http://127.0.0.1:{srv.server_port}"])
    srv.shutdown()
    return code, out.getvalue(), err.getvalue()


def test_ok_last_week():
    code, out, err = run(["--week", "last"])
    assert code == 0, err
    rep = json.loads(out)
    assert rep["kids"]["Sam"]["total_minutes"] == 95 and rep["kids"]["Alex"]["total_minutes"] == 87
    assert "Sam 1h 35m (daily avg 14m)" in err
    assert "s1" not in out + err and "a1" not in out + err  # cookie values never printed


def test_child_ids_skip_household():
    code, out, err = run(["--week", "last", "--child-ids", f"Sam={KIDS['Sam']},Alex={KIDS['Alex']}"])
    assert code == 0, err
    assert [c["path"] for c in json.loads(out)["http_calls"]] == [rr.ACTIVITY_PATH] * 2


def test_probe_finds_minimal_cookies():
    code, out, err = run(["--week", "this", "--probe", "--probe-cookies"])
    assert code == 0, err
    p = json.loads(out)["probe"]
    assert p["required_cookie_names"] == sorted(REQUIRED), p
    assert p["works_without_origin_referer"] is True
    assert p["activity_response_bytes"]["last_week"] > 0


def test_probe_reports_origin_needed():
    code, out, err = run(["--week", "this", "--probe"], scenario="needs_origin")
    assert code == 0, err
    assert str(json.loads(out)["probe"]["works_without_origin_referer"]).startswith("no: exit 3")


def test_expired_cookie_exit_3():
    code, _, err = run(["--week", "this"], cookies=EXTRA)
    assert code == rr.EXIT_SIGNED_OUT and "redirect to https://www.amazon.com/ap/signin" in err, err


def test_captcha_exit_4():
    code, _, err = run(["--week", "this"], scenario="captcha")
    assert code == rr.EXIT_BOT_CHECK, err


def test_shape_exit_5():
    code, _, err = run(["--week", "this"], scenario="shape")
    assert code == rr.EXIT_SHAPE, err


def test_missing_cookie_file_exit_6():
    with redirect_stderr(io.StringIO()):
        assert dr.main(["--cookie-file", "/nonexistent/cookie.txt"]) == rr.EXIT_BROWSER


def test_netscape_cookie_format():
    with tempfile.TemporaryDirectory() as d:
        f = Path(d) / "c.txt"
        f.write_text("# Netscape HTTP Cookie File\n.amazon.com\tTRUE\t/\tTRUE\t1800000000\tat-main\ta1\n"
                     "#HttpOnly_.amazon.com\tTRUE\t/\tTRUE\t1800000000\tsession-id\ts1\n.google.com\tTRUE\t/\tTRUE\t0\tx\ty\n")
        os.chmod(f, 0o600)
        assert dr.load_cookies(str(f)) == {"at-main": "a1", "session-id": "s1"}


if __name__ == "__main__":
    failed = 0
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print(f"PASS {name}")
            except Exception as e:
                failed += 1
                print(f"FAIL {name}: {e!r}")
    sys.exit(1 if failed else 0)
