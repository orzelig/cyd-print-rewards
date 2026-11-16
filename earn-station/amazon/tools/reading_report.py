#!/usr/bin/env python3
"""Weekly "Books" reading time per kid from Amazon Parent Dashboard (parents.amazon.com).

Reuses an already signed-in browser profile with headless Playwright. It never
types credentials and never tries to get past a sign-in or bot-check page: it
stops with a clear exit code instead.

Data source (verified 2026-11-16):
  * Kids:     window.__PREFETCHED_HOUSEHOLD on /activities/household-summary
              -> members[] {firstName, role: "CHILD", directedId}
  * Activity: POST https://parents.amazon.com/ajax/get-weekly-activities-v2
              body {childDirectedId, startTime, endTime (epoch s),
                    aggregationInterval: 86400, timeZone: <READING_TZ>}
              -> activityV2Data[] {category: "BOOK", intervals[] {startTime,
                 aggregatedDuration, aggregatedActivityResults[] {key,
                 activityDuration (seconds), attributes {TITLE, ...}}}}

Usage:
  python reading_report.py --week this|last|YYYY-MM-DD --out report.json
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import tempfile
from dataclasses import dataclass
from datetime import date, datetime, timedelta
from pathlib import Path
from urllib.parse import urlparse
from zoneinfo import ZoneInfo

TZ_NAME = os.environ.get("READING_TZ", "America/New_York")  # household time zone (IANA name)
TZ = ZoneInfo(TZ_NAME)
HOST = "parents.amazon.com"
BASE = f"https://{HOST}"
ACTIVITY_PAGE = f"{BASE}/activities/household-summary"
ACTIVITY_PATH = "/ajax/get-weekly-activities-v2"
ACTIVITY_URL = BASE + ACTIVITY_PATH
BOOK_CATEGORY = "BOOK"
DEFAULT_KIDS = [k for k in os.environ.get("READING_KIDS", "").split(",") if k.strip()]  # first names
PROFILE_ENV = "READING_REPORT_PROFILE"

# Exit codes (documented in README.md)
EXIT_OK = 0
EXIT_UNEXPECTED = 1
EXIT_USAGE = 2
EXIT_SIGNED_OUT = 3
EXIT_BOT_CHECK = 4
EXIT_SHAPE = 5
EXIT_BROWSER = 6
EXIT_KID_NOT_FOUND = 7

SIGNIN_SELECTORS = "#ap_password, #ap_email, form[name='signIn'], form[name='signin']"
BOT_SELECTORS = (
    "form[action*='validateCaptcha'], #captchacharacters, img[src*='captcha'], "
    "iframe[src*='arkoselabs'], iframe[src*='funcaptcha'], #cvf-page-content, "
    "#auth-captcha-image, [data-testid='captcha']"
)


class ReportError(Exception):
    def __init__(self, code: int, message: str):
        super().__init__(message)
        self.code = code


def log(msg: str) -> None:
    print(f"[reading-report] {msg}", file=sys.stderr, flush=True)


# --------------------------------------------------------------------------- week


@dataclass(frozen=True)
class Week:
    start: datetime  # Sunday 00:00 local
    end_excl: datetime  # following Sunday 00:00 local
    now: datetime

    @property
    def complete(self) -> bool:
        return self.now >= self.end_excl

    @property
    def start_epoch(self) -> int:
        return int(self.start.timestamp())

    @property
    def query_end_epoch(self) -> int:
        # Same convention as the dashboard: "now" for the running week,
        # Saturday 23:59:59 for a finished week.
        if self.complete:
            return int(self.end_excl.timestamp()) - 1
        return int(self.now.timestamp())

    @property
    def last_day(self) -> date:
        return (self.end_excl - timedelta(days=1)).date() if self.complete else self.now.date()

    @property
    def days_counted(self) -> int:
        return (self.last_day - self.start.date()).days + 1

    def request_body(self, child_id: str) -> dict:
        return {
            "childDirectedId": child_id,
            "startTime": self.start_epoch,
            "endTime": self.query_end_epoch,
            "aggregationInterval": 86400,
            "timeZone": TZ_NAME,
        }


def local_midnight(d: date) -> datetime:
    return datetime(d.year, d.month, d.day, tzinfo=TZ)


def sunday_of(d: date) -> date:
    return d - timedelta(days=(d.weekday() + 1) % 7)  # Mon=0 .. Sun=6


def resolve_week(spec: str, now: datetime | None = None) -> Week:
    now = (now or datetime.now(TZ)).astimezone(TZ)
    if spec == "this":
        sun = sunday_of(now.date())
    elif spec == "last":
        sun = sunday_of(now.date()) - timedelta(days=7)
    else:
        try:
            sun = sunday_of(date.fromisoformat(spec))
        except ValueError:
            raise ReportError(EXIT_USAGE, f"--week must be this, last or YYYY-MM-DD (got {spec!r})")
    start = local_midnight(sun)
    if start > now:
        raise ReportError(EXIT_USAGE, f"week starting {sun} is in the future")
    return Week(start=start, end_excl=local_midnight(sun + timedelta(days=7)), now=now)


# ------------------------------------------------------------------------ parsing


def _require(obj, key, typ, where):
    if not isinstance(obj, dict) or key not in obj:
        raise ReportError(EXIT_SHAPE, f"data shape changed: missing '{key}' in {where}")
    val = obj[key]
    if typ is not None and (not isinstance(val, typ) or isinstance(val, bool)):
        raise ReportError(EXIT_SHAPE, f"data shape changed: '{key}' in {where} is {type(val).__name__}")
    return val


def parse_activity(payload, week: Week) -> dict:
    """Return {'seconds', 'titles': {key: {title, seconds}}, 'daily': {date: seconds}, 'categories'}."""
    if not isinstance(payload, dict):
        raise ReportError(EXIT_SHAPE, f"data shape changed: activity response is {type(payload).__name__}, not an object")
    data = _require(payload, "activityV2Data", list, "activity response")
    titles: dict[str, dict] = {}
    daily: dict[date, float] = {}
    categories: set[str] = set()
    total = 0.0
    for i, cat in enumerate(data):
        name = _require(cat, "category", str, f"activityV2Data[{i}]")
        categories.add(name)
        intervals = _require(cat, "intervals", list, f"activityV2Data[{i}]")
        if name != BOOK_CATEGORY:
            continue
        for j, iv in enumerate(intervals):
            where = f"activityV2Data[{i}].intervals[{j}]"
            iv_start = _require(iv, "startTime", (int, float), where)
            results = _require(iv, "aggregatedActivityResults", list, where)
            iv_total = iv.get("aggregatedDuration")
            when = datetime.fromtimestamp(iv_start, TZ)
            if not (week.start <= when < week.end_excl):
                log(f"warning: skipping interval at {when.isoformat()} outside requested week")
                continue
            iv_sum = 0.0
            for k, res in enumerate(results):
                rwhere = f"{where}.aggregatedActivityResults[{k}]"
                secs = _require(res, "activityDuration", (int, float), rwhere)
                attrs = _require(res, "attributes", dict, rwhere)
                title = attrs.get("TITLE")
                if not isinstance(title, str) or not title.strip():
                    log(f"warning: untitled book entry in {rwhere}")
                    title = "(untitled)"
                key = res.get("key") if isinstance(res.get("key"), str) else title
                entry = titles.setdefault(key, {"title": title.strip(), "seconds": 0.0})
                entry["seconds"] += secs
                iv_sum += secs
            if isinstance(iv_total, (int, float)) and abs(iv_total - iv_sum) > 1:
                log(f"warning: {where} total {iv_total}s != sum of titles {iv_sum}s; using sum of titles")
            daily[when.date()] = daily.get(when.date(), 0.0) + iv_sum
            total += iv_sum
    return {"seconds": total, "titles": titles, "daily": daily, "categories": sorted(categories)}


def round_min(seconds: float) -> int:
    return int(seconds / 60 + 0.5)


def fmt_minutes(minutes: int) -> str:
    h, m = divmod(minutes, 60)
    return f"{h}h {m}m" if h else f"{m}m"


def kid_report(name: str, parsed: dict, week: Week, source: str) -> dict:
    total_min = round_min(parsed["seconds"])
    days = []
    d = week.start.date()
    while d <= week.last_day:
        days.append({"date": d.isoformat(), "weekday": d.strftime("%a"), "minutes": round_min(parsed["daily"].get(d, 0))})
        d += timedelta(days=1)
    titles = sorted(parsed["titles"].values(), key=lambda t: -t["seconds"])
    return {
        "name": name,
        "total_minutes": total_min,
        "total_seconds": int(parsed["seconds"]),
        "daily_average_minutes": round_min(parsed["seconds"] / week.days_counted),
        "days_counted": week.days_counted,
        "titles": [{"title": t["title"], "minutes": round_min(t["seconds"]), "seconds": int(t["seconds"])} for t in titles],
        "daily": days,
        "source": source,
    }


def summary_line(k: dict) -> str:
    return f"{k['name']} {fmt_minutes(k['total_minutes'])} (daily avg {fmt_minutes(k['daily_average_minutes'])})"


# ------------------------------------------------------------------------ browser


def check_blockers(page) -> None:
    """Stop (never interact) if Amazon shows a sign-in or bot-check page."""
    parsed = urlparse(page.url)
    path = parsed.path.lower()
    shown = f"{parsed.scheme}://{parsed.netloc}{parsed.path}"
    if "validatecaptcha" in path or "/ap/cvf" in path or page.locator(BOT_SELECTORS).count():
        raise ReportError(EXIT_BOT_CHECK, f"Amazon is showing a captcha / bot check ({shown}). "
                          "Not attempting it. A person must open the box browser and clear it.")
    if any(p in path for p in ("/ap/signin", "/ap/mfa", "/ap/challenge", "/ap/register")) or page.locator(SIGNIN_SELECTORS).count():
        raise ReportError(EXIT_SIGNED_OUT, f"Amazon session expired: redirected to sign-in ({shown}). "
                          "A person must sign in again in the box browser.")
    if parsed.hostname != HOST:
        raise ReportError(EXIT_SIGNED_OUT, f"ended up on {shown} instead of {HOST}; session is probably expired.")


def read_household(page) -> dict[str, str]:
    hh = page.evaluate("() => window.__PREFETCHED_HOUSEHOLD || null")
    if hh is None:
        raise ReportError(EXIT_SHAPE, "data shape changed: window.__PREFETCHED_HOUSEHOLD not found on the activity page")
    members = _require(hh, "members", list, "__PREFETCHED_HOUSEHOLD")
    kids = {}
    for i, m in enumerate(members):
        role = _require(m, "role", str, f"household.members[{i}]")
        if role != "CHILD":
            continue
        name = _require(m, "firstName", str, f"household.members[{i}]")
        kids[name] = _require(m, "directedId", str, f"household.members[{i}]")
    if not kids:
        raise ReportError(EXIT_SHAPE, "data shape changed: no CHILD members in household data")
    return kids


class ActivityCapture:
    """Collects get-weekly-activities-v2 responses the page loads in the background."""

    def __init__(self, page):
        self.pending = []
        self.items = []  # (body, payload)
        self.endpoints_seen = set()
        page.on("response", self._on_response)

    def _on_response(self, resp):
        if HOST in resp.url and "/ajax/" in resp.url:
            self.endpoints_seen.add(f"{resp.request.method} {resp.url.split('?')[0]} -> {resp.status}")
        if ACTIVITY_PATH in resp.url and resp.request.method == "POST":
            self.pending.append(resp)

    def drain(self):
        while self.pending:
            resp = self.pending.pop(0)
            if resp.status in (401, 403):
                raise ReportError(EXIT_SIGNED_OUT, f"{ACTIVITY_PATH} returned HTTP {resp.status}; session expired.")
            if resp.status != 200:
                log(f"warning: {ACTIVITY_PATH} returned HTTP {resp.status}; ignoring that response")
                continue
            try:
                body = resp.request.post_data_json
                payload = resp.json()
            except Exception as e:  # body unavailable / not JSON
                log(f"warning: could not read captured activity response: {e}")
                continue
            if isinstance(body, dict):
                self.items.append((body, payload))

    def find(self, child_id: str, week: Week):
        self.drain()
        for body, payload in reversed(self.items):
            if (body.get("childDirectedId") == child_id and body.get("startTime") == week.start_epoch
                    and body.get("timeZone") == TZ_NAME
                    and isinstance(body.get("endTime"), (int, float))
                    and (not week.complete or body["endTime"] >= week.query_end_epoch)):
                return payload
        return None


def wait_quiet(page, ms=10000):
    try:
        page.wait_for_load_state("networkidle", timeout=ms)
    except Exception:
        pass  # analytics beacons can keep the network busy; not fatal


def fetch_in_page(page, body: dict):
    """Ask the page itself to POST the same request it makes (same origin, same cookies)."""
    res = page.evaluate(
        """async ([url, body]) => {
            const r = await fetch(url, {method: 'POST', credentials: 'include',
                headers: {'content-type': 'application/json', 'accept': 'application/json'},
                body: JSON.stringify(body)});
            return {status: r.status, url: r.url, ct: r.headers.get('content-type') || '', text: await r.text()};
        }""",
        [ACTIVITY_PATH, body],
    )
    if res["status"] in (401, 403) or "/ap/signin" in res["url"]:
        raise ReportError(EXIT_SIGNED_OUT, f"{ACTIVITY_PATH} returned HTTP {res['status']}; session expired.")
    low = res["text"][:5000].lower()
    if "captcha" in low and "json" not in res["ct"]:
        raise ReportError(EXIT_BOT_CHECK, f"{ACTIVITY_PATH} returned a captcha page. Not attempting it.")
    if res["status"] != 200:
        raise ReportError(EXIT_SHAPE, f"{ACTIVITY_PATH} returned HTTP {res['status']}")
    try:
        return json.loads(res["text"])
    except ValueError:
        if "ap_password" in low or "signin" in low:
            raise ReportError(EXIT_SIGNED_OUT, f"{ACTIVITY_PATH} returned a sign-in page; session expired.")
        raise ReportError(EXIT_SHAPE, f"data shape changed: {ACTIVITY_PATH} did not return JSON ({res['ct']})")


def collect(context, week: Week, kid_names: list[str], discover: bool = False, debug_dir: str | None = None) -> dict:
    page = context.new_page()
    try:
        cap = ActivityCapture(page)
        log(f"opening {ACTIVITY_PAGE}")
        page.goto(ACTIVITY_PAGE, wait_until="domcontentloaded", timeout=60000)
        check_blockers(page)
        wait_quiet(page)
        check_blockers(page)
        household = read_household(page)
        log(f"children in household: {', '.join(sorted(household))}")

        missing = [n for n in kid_names if n not in household]
        if missing:
            raise ReportError(EXIT_KID_NOT_FOUND, f"not found in household: {', '.join(missing)} "
                              f"(children are: {', '.join(sorted(household))})")

        kids = {}
        endpoint_logged = False
        for name in kid_names:
            cid = household[name]
            payload, source = cap.find(cid, week), "captured"
            if payload is None:
                # Make the page load this kid's activity itself via the child switcher.
                switch = page.locator(f".carousel-item[aria-label='{name}']")
                if switch.count() == 1:
                    try:
                        with page.expect_response(lambda r: ACTIVITY_PATH in r.url and cid in (r.request.post_data or ""), timeout=20000):
                            switch.dispatch_event("click")  # items sit in a collapsed switcher
                        wait_quiet(page, 8000)
                    except Exception as e:
                        log(f"warning: child switcher for {name} did not load activity ({e.__class__.__name__})")
                    check_blockers(page)
                    payload = cap.find(cid, week)
                else:
                    log(f"warning: child switcher for {name} not found on page")
            if payload is None:
                # Week the page did not load on its own (e.g. older weeks): same request, from the page.
                source = "replayed"
                payload = fetch_in_page(page, week.request_body(cid))
            if not endpoint_logged:
                log(f"activity data endpoint: POST {ACTIVITY_URL} (body: childDirectedId, startTime, endTime, "
                    f"aggregationInterval, timeZone) -> activityV2Data[]")
                endpoint_logged = True
            parsed = parse_activity(payload, week)
            log(f"{name}: categories in response {parsed['categories'] or '[]'}; source={source}")
            kids[name] = kid_report(name, parsed, week, source)

        if discover:
            log("JSON endpoints seen on parents.amazon.com:")
            for e in sorted(cap.endpoints_seen):
                log(f"  {e}")
        return kids
    except ReportError:
        if debug_dir:
            _save_debug(page, debug_dir)
        raise
    finally:
        page.close()


def _save_debug(page, debug_dir):
    try:
        d = Path(debug_dir)
        d.mkdir(parents=True, exist_ok=True)
        os.chmod(d, 0o700)
        page.screenshot(path=str(d / "failure.png"), full_page=True)
        (d / "failure-url.txt").write_text(page.url.split("?")[0] + "\n")
        log(f"debug screenshot saved to {d / 'failure.png'}")
    except Exception as e:
        log(f"could not save debug screenshot: {e}")


def open_context(pw, args):
    """Returns (context, cleanup_fn)."""
    if args.cdp:
        try:
            browser = pw.chromium.connect_over_cdp(args.cdp)
        except Exception as e:
            raise ReportError(EXIT_BROWSER, f"could not connect to browser at {args.cdp}: {e}")
        if not browser.contexts:
            raise ReportError(EXIT_BROWSER, "connected over CDP but the browser has no context/profile open")
        return browser.contexts[0], lambda: None

    profile = args.profile or os.environ.get(PROFILE_ENV)
    if not profile:
        raise ReportError(EXIT_USAGE, f"no browser profile: pass --profile DIR or set {PROFILE_ENV} (or use --cdp URL)")
    profile = os.path.expanduser(profile)
    if not os.path.isdir(profile):
        raise ReportError(EXIT_BROWSER, f"browser profile directory not found: {profile}")

    tmp = None
    user_data_dir = profile
    if args.copy_profile:
        tmp = tempfile.mkdtemp(prefix="reading-report-")
        user_data_dir = os.path.join(tmp, "profile")
        shutil.copytree(profile, user_data_dir, symlinks=True, ignore_dangling_symlinks=True,
                        ignore=shutil.ignore_patterns("Singleton*", "lockfile", "*.lock", "Cache", "Code Cache",
                                                      "GPUCache", "Service Worker", "Crashpad", "ShaderCache"))
    kwargs = dict(headless=not args.headed, timezone_id=TZ_NAME, locale="en-US", viewport={"width": 1280, "height": 900})
    if args.channel:
        kwargs["channel"] = args.channel
    if args.system_keyring:
        # Let Chrome decrypt cookies with the OS keyring the profile was created with.
        kwargs["ignore_default_args"] = ["--password-store=basic", "--use-mock-keychain"]
    try:
        ctx = pw.chromium.launch_persistent_context(user_data_dir, **kwargs)
    except Exception as e:
        if tmp:
            shutil.rmtree(tmp, ignore_errors=True)
        msg = str(e)
        if any(s in msg for s in ("ProcessSingleton", "already in use", "SingletonLock", "profile appears to be in use")):
            raise ReportError(EXIT_BROWSER, "the browser profile is in use by a running browser. Close it, "
                              "or re-run with --copy-profile, or use --cdp to attach to the running browser.")
        raise ReportError(EXIT_BROWSER, f"could not launch browser with profile {profile}: {msg.splitlines()[0]}")

    def cleanup():
        try:
            ctx.close()
        finally:
            if tmp:
                shutil.rmtree(tmp, ignore_errors=True)

    return ctx, cleanup


# --------------------------------------------------------------------------- main


def build_report(week: Week, kids: dict) -> dict:
    return {
        "generated_at": datetime.now(TZ).isoformat(timespec="seconds"),
        "timezone": TZ_NAME,
        "category": "Books",
        "week": {
            "start": week.start.date().isoformat(),
            "end": (week.end_excl - timedelta(days=1)).date().isoformat(),
            "complete": week.complete,
            "through": week.now.isoformat(timespec="minutes") if not week.complete else None,
            "days_counted": week.days_counted,
        },
        "endpoint": {"method": "POST", "url": ACTIVITY_URL, "path": "activityV2Data[category=BOOK].intervals[].aggregatedActivityResults[]"},
        "kids": kids,
    }


def parse_args(argv):
    p = argparse.ArgumentParser(description="Weekly Books reading time per kid from Amazon Parent Dashboard.")
    p.add_argument("--week", default="this", help="this | last | YYYY-MM-DD (any day in the Sun–Sat week). Default: this")
    p.add_argument("--out", help="write JSON report to this file (default: print JSON to stdout)")
    p.add_argument("--kids", default=",".join(DEFAULT_KIDS), help="comma-separated first names (default: $READING_KIDS)")
    src = p.add_mutually_exclusive_group()
    src.add_argument("--profile", help=f"signed-in browser user-data dir (or env {PROFILE_ENV})")
    src.add_argument("--cdp", help="attach to a running browser instead, e.g. http://127.0.0.1:9222")
    p.add_argument("--copy-profile", action="store_true", help="copy the profile to a temp dir first (use while the browser is open)")
    p.add_argument("--channel", help="Playwright browser channel, e.g. chrome (default: bundled Chromium)")
    p.add_argument("--system-keyring", action="store_true", help="let the browser use the OS keyring to decrypt cookies")
    p.add_argument("--headed", action="store_true", help="show the browser window (debugging only)")
    p.add_argument("--discover", action="store_true", help="also log every parents.amazon.com JSON endpoint the page called")
    p.add_argument("--debug-dir", help="on failure, save a screenshot here")
    return p.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    try:
        week = resolve_week(args.week)
        kid_names = [k.strip() for k in args.kids.split(",") if k.strip()]
        if not kid_names:
            raise ReportError(EXIT_USAGE, "say which kids: --kids Sam,Alex or READING_KIDS=Sam,Alex")
        log(f"week {week.start.date()} – {(week.end_excl - timedelta(days=1)).date()} ({TZ_NAME})"
            + ("" if week.complete else f", so far through {week.now:%a %H:%M}"))
        try:
            from playwright.sync_api import sync_playwright
        except ImportError:
            raise ReportError(EXIT_BROWSER, "playwright is not installed: pip install -r requirements.txt && playwright install chromium")
        with sync_playwright() as pw:
            ctx, cleanup = open_context(pw, args)
            try:
                kids = collect(ctx, week, kid_names, discover=args.discover, debug_dir=args.debug_dir)
            finally:
                cleanup()
    except ReportError as e:
        print(f"ERROR ({e.code}): {e}", file=sys.stderr)
        return e.code
    except KeyboardInterrupt:
        return 130
    except Exception as e:
        print(f"ERROR ({EXIT_UNEXPECTED}): unexpected {e.__class__.__name__}: {e}", file=sys.stderr)
        return EXIT_UNEXPECTED

    report = build_report(week, kids)
    text = json.dumps(report, indent=2, ensure_ascii=False)
    if args.out:
        Path(args.out).write_text(text + "\n", encoding="utf-8")
        log(f"wrote {args.out}")
    else:
        print(text)
    label = f"Week of {report['week']['start']}" + ("" if week.complete else " (so far)")
    print(label, file=sys.stderr if not args.out else sys.stdout)
    for k in kids.values():
        print(summary_line(k), file=sys.stderr if not args.out else sys.stdout)
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
