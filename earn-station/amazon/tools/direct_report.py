#!/usr/bin/env python3
"""Browser-free variant: call the Amazon Parent Dashboard endpoint directly.

Standard library only (urllib), to mirror what a microcontroller would have to do:
send the session cookie, POST /ajax/get-weekly-activities-v2 per child, parse JSON.

It never signs in and never stores a password. It needs a session cookie that
*you* copy from a signed-in browser into a local file (chmod 600). See
README-direct.md. The cookie value is never printed.

Usage:
  python direct_report.py --week this|last|YYYY-MM-DD --out report.json
  python direct_report.py --probe            # evaluation: sizes, timings, needed cookies/headers
"""
from __future__ import annotations

import argparse
import json
import os
import stat
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

import reading_report as rr
from reading_report import ReportError, log

DEFAULT_COOKIE_FILE = "~/.config/reading-report/cookie.txt"
COOKIE_ENV = "READING_REPORT_COOKIE_FILE"
USER_AGENT = "reading-report-direct/1.0 (personal script; python-urllib)"
HOUSEHOLD_MARKER = "window.__PREFETCHED_HOUSEHOLD"


# ------------------------------------------------------------------------ cookies


def load_cookies(path: str) -> dict[str, str]:
    """Accepts either a raw Cookie header value ("a=1; b=2") or a Netscape cookies.txt."""
    p = Path(os.path.expanduser(path))
    if not p.is_file():
        raise ReportError(rr.EXIT_BROWSER, f"cookie file not found: {p} (see README-direct.md)")
    mode = p.stat().st_mode
    if mode & (stat.S_IRWXG | stat.S_IRWXO):
        log(f"warning: {p} is readable by others; run: chmod 600 {p}")
    text = p.read_text(encoding="utf-8").strip()
    cookies: dict[str, str] = {}
    if "\t" in text:  # Netscape format: domain flag path secure expiry name value
        for line in text.splitlines():
            if line.startswith("#HttpOnly_"):
                line = line[len("#HttpOnly_"):]
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) >= 7 and "amazon.com" in parts[0]:
                cookies[parts[5]] = parts[6]
    else:
        if text.lower().startswith("cookie:"):
            text = text[7:]
        for part in text.split(";"):
            if "=" in part:
                k, v = part.strip().split("=", 1)
                cookies[k.strip()] = v.strip()
    if not cookies:
        raise ReportError(rr.EXIT_BROWSER, f"no cookies found in {p}")
    return cookies


def cookie_header(cookies: dict[str, str]) -> str:
    return "; ".join(f"{k}={v}" for k, v in cookies.items())


# --------------------------------------------------------------------------- http


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *a, **k):
        return None  # surface 3xx so a sign-in redirect is detected, never followed


_opener = urllib.request.build_opener(_NoRedirect)


class Http:
    def __init__(self, base: str, cookies: dict[str, str], browser_headers: bool = True, timeout: float = 30):
        self.base = base.rstrip("/")
        self.cookies = dict(cookies)
        self.browser_headers = browser_headers
        self.timeout = timeout
        self.calls: list[dict] = []

    def request(self, method: str, path: str, body: dict | None = None, cookies: dict | None = None):
        url = self.base + path
        headers = {"User-Agent": USER_AGENT, "Cookie": cookie_header(self.cookies if cookies is None else cookies)}
        data = None
        if body is not None:
            data = json.dumps(body).encode()
            headers["Content-Type"] = "application/json"
            headers["Accept"] = "application/json"
        if self.browser_headers:  # what any same-origin browser request carries (no UA spoofing)
            headers["Origin"] = self.base
            headers["Referer"] = self.base + "/activities/household-summary"
        req = urllib.request.Request(url, data=data, headers=headers, method=method)
        t0 = time.monotonic()
        try:
            with _opener.open(req, timeout=self.timeout) as r:
                status, rheaders, raw = r.status, r.headers, r.read()
        except urllib.error.HTTPError as e:
            status, rheaders, raw = e.code, e.headers, e.read() or b""
        except urllib.error.URLError as e:
            raise ReportError(rr.EXIT_UNEXPECTED, f"network error calling {path}: {e.reason}")
        ms = int((time.monotonic() - t0) * 1000)
        info = {"method": method, "path": path, "status": status, "bytes": len(raw), "ms": ms,
                "content_type": rheaders.get("Content-Type", ""), "location": rheaders.get("Location", "")}
        self.calls.append(info)
        return status, info, raw


def classify_failure(status: int, info: dict, raw: bytes, path: str) -> ReportError | None:
    loc = info["location"].lower()
    text = raw[:20000].decode("utf-8", "replace").lower()
    if "validatecaptcha" in loc or "/ap/cvf" in loc or "captcha" in text and "json" not in info["content_type"]:
        return ReportError(rr.EXIT_BOT_CHECK, f"{path}: Amazon answered with a captcha / bot check (HTTP {status}). Not attempting it.")
    if status in (401, 403) or "/ap/signin" in loc or "ap_password" in text or 'name="signin"' in text:
        return ReportError(rr.EXIT_SIGNED_OUT, f"{path}: session cookie rejected (HTTP {status}"
                           + (f", redirect to {info['location'].split('?')[0]}" if loc else "") + "). Copy a fresh cookie.")
    if 300 <= status < 400:
        return ReportError(rr.EXIT_SIGNED_OUT, f"{path}: unexpected redirect to {info['location'].split('?')[0]}; session probably expired.")
    if status != 200:
        return ReportError(rr.EXIT_UNEXPECTED, f"{path}: HTTP {status}")
    return None


# ------------------------------------------------------------------------ calls


def _js_string_literal(src: str, start: int) -> str:
    """Decode the JS string literal ('...' or "...") starting at src[start]."""
    quote, out, i = src[start], ['"'], start + 1
    while i < len(src):
        ch = src[i]
        if ch == "\\":
            nxt = src[i + 1]
            out.append(nxt if nxt in "'" else "\\" + nxt)  # \' is not a JSON escape
            i += 2
            continue
        if ch == quote:
            out.append('"')
            return json.loads("".join(out))
        out.append('\\"' if ch == '"' else ch)
        i += 1
    raise ValueError("unterminated string literal")


def parse_household_html(html: str) -> dict[str, str]:
    i = html.find(HOUSEHOLD_MARKER)
    if i < 0:
        raise ReportError(rr.EXIT_SHAPE, f"data shape changed: {HOUSEHOLD_MARKER} not in household page HTML")
    j = html.find("=", i) + 1
    while html[j].isspace():
        j += 1
    if html.startswith("JSON.parse(", j):
        j += len("JSON.parse(")
    try:
        if html[j] in "'\"":  # JSON embedded as a JS string literal (real page: single-quoted)
            value = json.loads(_js_string_literal(html, j))
        else:
            value, _ = json.JSONDecoder().raw_decode(html, j)
    except ValueError as e:
        raise ReportError(rr.EXIT_SHAPE, f"data shape changed: cannot decode {HOUSEHOLD_MARKER}: {e}")
    members = rr._require(value, "members", list, "__PREFETCHED_HOUSEHOLD")
    kids = {}
    for n, m in enumerate(members):
        if rr._require(m, "role", str, f"household.members[{n}]") == "CHILD":
            kids[rr._require(m, "firstName", str, f"household.members[{n}]")] = rr._require(m, "directedId", str, f"household.members[{n}]")
    if not kids:
        raise ReportError(rr.EXIT_SHAPE, "data shape changed: no CHILD members in household data")
    return kids


def get_household(http: Http) -> dict[str, str]:
    path = "/activities/household-summary"
    status, info, raw = http.request("GET", path)
    err = classify_failure(status, info, raw, path)
    if err:
        raise err
    return parse_household_html(raw.decode("utf-8", "replace"))


def get_activity(http: Http, body: dict, cookies: dict | None = None):
    status, info, raw = http.request("POST", rr.ACTIVITY_PATH, body, cookies=cookies)
    err = classify_failure(status, info, raw, rr.ACTIVITY_PATH)
    if err:
        raise err
    try:
        return json.loads(raw)
    except ValueError:
        raise ReportError(rr.EXIT_SHAPE, f"data shape changed: {rr.ACTIVITY_PATH} did not return JSON ({info['content_type']})")


def parse_child_ids(spec: str | None) -> dict[str, str] | None:
    if not spec:
        return None
    out = {}
    for part in spec.split(","):
        name, _, cid = part.partition("=")
        if not cid:
            raise ReportError(rr.EXIT_USAGE, "--child-ids must look like Sam=amzn1.account...,Alex=amzn1.account...")
        out[name.strip()] = cid.strip()
    return out


# ------------------------------------------------------------------------ probe


def probe(http: Http, week: rr.Week, kid_names: list[str], household: dict[str, str], minimize: bool) -> dict:
    """Evaluation for a constrained client. Never prints cookie values."""
    result: dict = {"cookie_count": len(http.cookies), "cookie_names": sorted(http.cookies)}
    cid = household[kid_names[0]]
    body = week.request_body(cid)

    # 1) baseline payload size for current and a full week
    sizes = {}
    for label, w in (("this_week", rr.resolve_week("this")), ("last_week", rr.resolve_week("last"))):
        get_activity(http, w.request_body(cid))
        sizes[label] = http.calls[-1]["bytes"]
    result["activity_response_bytes"] = sizes
    result["household_page_bytes"] = next((c["bytes"] for c in http.calls if c["path"].startswith("/activities")), None)

    # 2) does it work without Origin/Referer (what a bare client sends)?
    bare = Http(http.base, http.cookies, browser_headers=False, timeout=http.timeout)
    try:
        get_activity(bare, body)
        result["works_without_origin_referer"] = True
    except ReportError as e:
        result["works_without_origin_referer"] = f"no: exit {e.code}: {e}"

    # 3) smallest cookie set that still works (greedy; one request per cookie)
    if minimize:
        baseline = rr.parse_activity(get_activity(http, body), week)["seconds"]
        needed = dict(http.cookies)
        for name in sorted(http.cookies):
            trial = {k: v for k, v in needed.items() if k != name}
            try:
                payload = get_activity(http, body, cookies=trial)
                if rr.parse_activity(payload, week)["seconds"] == baseline:  # same data, not an empty 200
                    needed = trial
            except ReportError as e:
                if e.code not in (rr.EXIT_SIGNED_OUT, rr.EXIT_UNEXPECTED, rr.EXIT_SHAPE):
                    raise  # bot check: stop immediately
            time.sleep(0.5)
        result["required_cookie_names"] = sorted(needed)
        result["required_cookie_header_bytes"] = len(cookie_header(needed))
    return result


# ------------------------------------------------------------------------- main


def parse_args(argv):
    p = argparse.ArgumentParser(description="Browser-free weekly Books reading time from Amazon Parent Dashboard.")
    p.add_argument("--week", default="this", help="this | last | YYYY-MM-DD (default: this)")
    p.add_argument("--out", help="write JSON report here (default: stdout)")
    p.add_argument("--kids", default=",".join(rr.DEFAULT_KIDS), help="comma-separated first names (default: every child)")
    p.add_argument("--cookie-file", default=os.environ.get(COOKIE_ENV, DEFAULT_COOKIE_FILE))
    p.add_argument("--child-ids", help="Sam=<directedId>,Alex=<directedId> to skip the household page (what an ESP32 would do)")
    p.add_argument("--base-url", default=rr.BASE, help=argparse.SUPPRESS)  # for the offline mock
    p.add_argument("--list-children", action="store_true", help="print each child's name and directedId, then exit")
    p.add_argument("--probe", action="store_true", help="evaluation mode: sizes, timings, header needs")
    p.add_argument("--probe-cookies", action="store_true", help="with --probe: find the minimal cookie set (1 request per cookie)")
    return p.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    try:
        week = rr.resolve_week(args.week)
        kid_names = [k.strip() for k in args.kids.split(",") if k.strip()]
        http = Http(args.base_url, load_cookies(args.cookie_file))
        log(f"week {week.start.date()} ({rr.TZ_NAME}); {len(http.cookies)} cookies loaded (values not shown)")
        household = parse_child_ids(args.child_ids) or get_household(http)
        if args.list_children:
            for name, cid in sorted(household.items()):
                print(f"{name}\t{cid}")
            return rr.EXIT_OK
        kid_names = kid_names or sorted(household)
        missing = [n for n in kid_names if n not in household]
        if missing:
            raise ReportError(rr.EXIT_KID_NOT_FOUND, f"not found: {', '.join(missing)} (have: {', '.join(sorted(household))})")
        kids = {}
        for name in kid_names:
            payload = get_activity(http, week.request_body(household[name]))
            kids[name] = rr.kid_report(name, rr.parse_activity(payload, week), week, "direct")
        log(f"activity data endpoint: POST {args.base_url}{rr.ACTIVITY_PATH}")
        report = rr.build_report(week, kids)
        if args.probe:
            report["probe"] = probe(http, week, kid_names, household, args.probe_cookies)
        report["http_calls"] = [{k: c[k] for k in ("method", "path", "status", "bytes", "ms")} for c in http.calls]
    except ReportError as e:
        print(f"ERROR ({e.code}): {e}", file=sys.stderr)
        return e.code
    except Exception as e:
        print(f"ERROR ({rr.EXIT_UNEXPECTED}): unexpected {e.__class__.__name__}: {e}", file=sys.stderr)
        return rr.EXIT_UNEXPECTED

    text = json.dumps(report, indent=2, ensure_ascii=False)
    if args.out:
        Path(args.out).write_text(text + "\n", encoding="utf-8")
        log(f"wrote {args.out}")
    else:
        print(text)
    stream = sys.stdout if args.out else sys.stderr
    print(f"Week of {report['week']['start']}" + ("" if week.complete else " (so far)"), file=stream)
    for k in kids.values():
        print(rr.summary_line(k), file=stream)
    return rr.EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
