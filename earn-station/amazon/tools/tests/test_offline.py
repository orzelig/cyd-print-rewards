"""Offline tests: a mocked parents.amazon.com served through Playwright routes.

No network, no Amazon account. Mirrors the real data shape verified on 2026-11-16.
Run:  python tests/test_offline.py      (or: pytest tests/)
"""
import io
import json
import sys
import tempfile
from contextlib import redirect_stderr, redirect_stdout
from datetime import datetime, timedelta
from pathlib import Path

import os  # noqa: E402
os.environ.setdefault("READING_KIDS", "Sam,Alex")  # the kids these tests ask for by default
os.environ["READING_TZ"] = "America/New_York"  # the week math below expects US Eastern
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import reading_report as rr  # noqa: E402


KIDS = {"Alex": "amzn1.account.ALEXTEST", "Sam": "amzn1.account.SAMTEST", "Jamie": "amzn1.account.JAMIETEST"}
# (day offset from Sunday, title, minutes) per kid; same every week
BOOKS = {
    "Sam": [(0, "The Wild Robot", 45), (0, "Dog Man: Mothering Heights", 20), (1, "The Wild Robot", 30)],
    "Alex": [(0, "The Getaway (Diary of a Wimpy Kid #12)", 25), (1, "The Meltdown (Diary of a Wimpy Kid #13)", 62)],
    "Jamie": [(0, "Pete the Cat", 10)],
}


def activity_payload(cid, body):
    name = next(n for n, i in KIDS.items() if i == cid)
    start, end = body["startTime"], body["endTime"]
    intervals = {}
    for off, title, mins in BOOKS[name]:
        day = datetime.fromtimestamp(start, rr.TZ) + timedelta(days=off)
        s = int(rr.local_midnight(day.date()).timestamp())
        if s > end:
            continue
        iv = intervals.setdefault(s, {"startTime": s, "endTime": s + 86399, "aggregatedDuration": 0,
                                      "aggregatedCount": 0, "aggregatedActivityResults": []})
        iv["aggregatedActivityResults"].append({
            "key": "//amazon/book/" + title, "activityCount": 1, "activityDuration": mins * 60,
            "attributes": {"TITLE": title, "CATEGORY": "BOOK", "REPORTING_CATEGORY": "BOOK", "AUTHORITY": "AMAZON_BOOK"},
            "lastActivityTimeStamp": s + 3600, "counter": 1, "queryLog": None})
        iv["aggregatedDuration"] += mins * 60
    ivs = list(intervals.values())
    video = {"category": "VIDEO", "aggregatedDuration": 9999, "aggregatedCount": 1, "intervals": [
        {"startTime": start, "endTime": start + 86399, "aggregatedDuration": 9999, "aggregatedCount": 1,
         "aggregatedActivityResults": [{"key": "v", "activityDuration": 9999, "attributes": {"TITLE": "Some Show"}}]}]}
    return {"activityV2Data": [
        {"category": "BOOK", "aggregatedDuration": sum(i["aggregatedDuration"] for i in ivs), "aggregatedCount": 1, "intervals": ivs},
        video]}


def page_html():
    this, last = rr.resolve_week("this"), rr.resolve_week("last")
    bodies = {cid: [this.request_body(cid), last.request_body(cid)] for cid in KIDS.values()}
    members = [{"role": "ADULT", "firstName": "Parent", "directedId": "amzn1.account.P", "avatarUri": None}] + [
        {"role": "CHILD", "firstName": n, "directedId": i, "avatarUri": None} for n, i in KIDS.items()]
    items = "".join(f'<div class="carousel-item" aria-label="{n}" data-id="{i}">{n}</div>' for n, i in KIDS.items())
    return f"""<!doctype html><html><head><title>Amazon Parent Dashboard</title></head><body>
<div aria-label="Child Switcher">{items}</div>
<script>
window.__PREFETCHED_HOUSEHOLD = {json.dumps({"members": members, "householdId": "h"})};
const BODIES = {json.dumps(bodies)};
function load(cid) {{ for (const b of BODIES[cid]) fetch('/ajax/get-weekly-activities-v2', {{method: 'POST',
  headers: {{'content-type': 'application/json'}}, body: JSON.stringify(b)}}); }}
document.querySelectorAll('.carousel-item').forEach(el => el.addEventListener('click', () => load(el.dataset.id)));
load({json.dumps(KIDS["Alex"])});
</script></body></html>"""


SIGNIN_HTML = "<form name='signIn'><input id='ap_email'><input id='ap_password' type='password'></form>"
CAPTCHA_HTML = "<form action='/errors/validateCaptcha'><input id='captchacharacters'></form>"


def make_context(pw, scenario="ok"):
    browser = pw.chromium.launch(headless=True)
    ctx = browser.new_context(timezone_id=rr.TZ_NAME)

    def page_route(route):
        # Playwright does not route requests that follow a redirect, so serve the
        # blocker pages in place (path-based detection is tested separately).
        if scenario == "signin":
            return route.fulfill(status=200, content_type="text/html", body=SIGNIN_HTML)
        if scenario == "captcha":
            return route.fulfill(status=200, content_type="text/html", body=CAPTCHA_HTML)
        route.fulfill(status=200, content_type="text/html", body=page_html())

    def activity_route(route):
        if scenario == "expired_api":
            return route.fulfill(status=401, content_type="application/json", body="{}")
        body = json.loads(route.request.post_data)
        payload = activity_payload(body["childDirectedId"], body)
        if scenario == "shape":
            payload = {"activityData": payload["activityV2Data"]}
        route.fulfill(status=200, content_type="application/json", body=json.dumps(payload))

    def amazon_route(route):
        html = CAPTCHA_HTML if "validateCaptcha" in route.request.url else SIGNIN_HTML
        route.fulfill(status=200, content_type="text/html", body=html)

    ctx.route("**/*", lambda r: r.abort())  # lowest priority: nothing reaches the real network
    ctx.route(rr.ACTIVITY_PAGE, page_route)
    ctx.route("**" + rr.ACTIVITY_PATH, activity_route)
    ctx.route("https://www.amazon.com/**", amazon_route)
    ctx.route("https://parents.amazon.com/ajax/get-*", lambda r: r.fulfill(status=200, content_type="application/json", body="{}")
              if rr.ACTIVITY_PATH not in r.request.url else r.fallback())
    return browser, ctx


def run_cli(argv, scenario="ok"):
    """Run main() with open_context swapped for the mocked context."""
    def fake_open_context(pw, _args):
        browser, ctx = make_context(pw, scenario)
        return ctx, browser.close

    orig = rr.open_context
    rr.open_context = fake_open_context
    out, err = io.StringIO(), io.StringIO()
    try:
        with redirect_stdout(out), redirect_stderr(err):
            code = rr.main(argv)
    finally:
        rr.open_context = orig
    return code, out.getvalue(), err.getvalue()


def expected_minutes(name, week):
    return sum(m for off, _, m in BOOKS[name] if off < week.days_counted)


# ----------------------------------------------------------------------------- tests

def test_week_math_dst():
    w = rr.resolve_week("2026-11-04", now=datetime(2026, 11, 16, 12, tzinfo=rr.TZ))
    assert w.start.isoformat() == "2026-11-01T00:00:00-04:00"      # EDT
    assert w.end_excl.isoformat() == "2026-11-08T00:00:00-05:00"   # EST (DST ended Nov 1)
    assert w.query_end_epoch - w.start_epoch == 7 * 86400 + 3600 - 1
    assert w.complete and w.days_counted == 7
    t = rr.resolve_week("this", now=datetime(2026, 11, 16, 12, tzinfo=rr.TZ))  # a Monday
    assert t.start.date().isoformat() == "2026-11-15" and t.days_counted == 2 and not t.complete
    s = rr.resolve_week("this", now=datetime(2026, 11, 15, 0, 5, tzinfo=rr.TZ))  # Sunday just after midnight
    assert s.start.date().isoformat() == "2026-11-15" and s.days_counted == 1


def test_this_week_captured_and_switcher():
    with tempfile.TemporaryDirectory() as d:
        out = Path(d) / "report.json"
        code, stdout, err = run_cli(["--week", "this", "--out", str(out)])
        assert code == 0, err
        rep = json.loads(out.read_text())
        week = rr.resolve_week("this")
        for name in ("Sam", "Alex"):
            k = rep["kids"][name]
            assert k["total_minutes"] == expected_minutes(name, week), (name, k)
            assert k["daily_average_minutes"] == rr.round_min(k["total_minutes"] * 60 / week.days_counted)
            assert all("Some Show" != t["title"] for t in k["titles"])  # VIDEO filtered out
        assert rep["kids"]["Alex"]["source"] == "captured"
        assert rep["kids"]["Sam"]["source"] == "captured"  # loaded by the page via child switcher
        assert "Jamie" not in rep["kids"]
        assert "activity data endpoint: POST https://parents.amazon.com/ajax/get-weekly-activities-v2" in err
        assert "Sam " in stdout and "daily avg" in stdout
        print(stdout)


def test_last_week_captured():
    code, stdout, err = run_cli(["--week", "last"])
    assert code == 0, err
    rep = json.loads(stdout)
    assert rep["week"]["complete"] and rep["week"]["days_counted"] == 7
    assert rep["kids"]["Sam"]["total_minutes"] == 95
    assert rep["kids"]["Sam"]["daily_average_minutes"] == 14  # 95/7 = 13.6
    titles = {t["title"]: t["minutes"] for t in rep["kids"]["Sam"]["titles"]}
    assert titles == {"The Wild Robot": 75, "Dog Man: Mothering Heights": 20}


def test_older_week_replayed():
    code, stdout, err = run_cli(["--week", "2026-10-01"])
    assert code == 0, err
    rep = json.loads(stdout)
    assert rep["week"]["start"] == "2026-09-27"
    assert rep["kids"]["Sam"]["source"] == "replayed" and rep["kids"]["Sam"]["total_minutes"] == 95
    assert rep["kids"]["Alex"]["total_minutes"] == 87


def test_signin_page_exit_3():
    code, _, err = run_cli(["--week", "this"], scenario="signin")
    assert code == rr.EXIT_SIGNED_OUT, err
    assert "sign-in" in err


def test_blocker_detection_by_url():
    from playwright.sync_api import sync_playwright
    with sync_playwright() as pw:
        browser, ctx = make_context(pw)
        page = ctx.new_page()
        for url, code in (("https://www.amazon.com/ap/signin?x=1", rr.EXIT_SIGNED_OUT),
                          ("https://www.amazon.com/errors/validateCaptcha", rr.EXIT_BOT_CHECK),
                          ("https://www.amazon.com/ap/cvf/request", rr.EXIT_BOT_CHECK)):
            page.goto(url)
            try:
                rr.check_blockers(page)
                raise AssertionError(f"no blocker detected for {url}")
            except rr.ReportError as e:
                assert e.code == code, (url, e.code, str(e))
        browser.close()


def test_captcha_exit_4():
    code, _, err = run_cli(["--week", "this"], scenario="captcha")
    assert code == rr.EXIT_BOT_CHECK, err


def test_api_401_exit_3():
    code, _, err = run_cli(["--week", "this"], scenario="expired_api")
    assert code == rr.EXIT_SIGNED_OUT, err


def test_shape_change_exit_5():
    code, _, err = run_cli(["--week", "this"], scenario="shape")
    assert code == rr.EXIT_SHAPE, err
    assert "activityV2Data" in err


def test_unknown_kid_exit_7():
    code, _, err = run_cli(["--week", "this", "--kids", "Sam,Zed"])
    assert code == rr.EXIT_KID_NOT_FOUND, err


def test_no_profile_exit_2(monkeypatch=None):
    import os
    os.environ.pop(rr.PROFILE_ENV, None)
    err = io.StringIO()
    with redirect_stderr(err):
        code = rr.main(["--week", "this"])
    assert code == rr.EXIT_USAGE, err.getvalue()


def test_future_week_exit_2():
    with redirect_stderr(io.StringIO()):
        assert rr.main(["--week", "2099-01-01"]) == rr.EXIT_USAGE


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
