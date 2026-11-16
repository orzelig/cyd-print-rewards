"""Build spec.json for host_test, using the Python implementation as the reference.

  python make_spec.py OUT_DIR [--live]

--live also fetches this and last week's real responses for each kid with the
saved cookie (4 small requests), so the C++ parser is checked against real data.
Responses are written to OUT_DIR, which should be a private scratch folder.
"""
import json
import sys
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent / "tools"  # the Python reference (reading-report)
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))
import reading_report as rr  # noqa: E402

out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
live = "--live" in sys.argv
TZ = rr.TZ


def ts(*a):
    return int(datetime(*a, tzinfo=TZ).timestamp())


spec = {"weeks": [], "parses": [], "classify": []}
for label, t, now in [
    ("mon-midday", ts(2026, 11, 16, 11, 15), ts(2026, 11, 16, 11, 15)),
    ("sunday-just-after-midnight", ts(2026, 11, 15, 0, 5), ts(2026, 11, 15, 0, 5)),
    ("saturday-late", ts(2026, 11, 21, 23, 59), ts(2026, 11, 21, 23, 59)),
    ("dst-end-week", ts(2026, 11, 4, 12), ts(2026, 11, 16, 12)),
    ("dst-start-week", ts(2026, 3, 10, 12), ts(2026, 11, 16, 12)),
    ("last-week", ts(2026, 11, 9, 12), ts(2026, 11, 16, 12)),
]:
    w = rr.resolve_week(datetime.fromtimestamp(t, TZ).date().isoformat(), now=datetime.fromtimestamp(now, TZ))
    spec["weeks"].append({"label": label, "t": t, "now": now, "start": w.start_epoch, "query_end": w.query_end_epoch,
                          "days_counted": w.days_counted, "complete": w.complete,
                          "start_date": w.start.date().isoformat(),
                          "span": int(w.end_excl.timestamp()) - w.start_epoch})


def add_parse(label, name, payload_text, week_t, now_t, status=0):
    f = out / f"{label}.json"
    f.write_text(payload_text)
    case = {"label": label, "name": name, "file": str(f), "t": week_t, "now": now_t, "status": status}
    if status == 0:
        w = rr.resolve_week(datetime.fromtimestamp(week_t, TZ).date().isoformat(), now=datetime.fromtimestamp(now_t, TZ))
        k = rr.kid_report(name, rr.parse_activity(json.loads(payload_text), w), w, "x")
        case.update(total_minutes=k["total_minutes"], total_seconds=k["total_seconds"],
                    avg_minutes=k["daily_average_minutes"], summary=rr.summary_line(k),
                    titles=[{"minutes": t["minutes"]} for t in k["titles"]],
                    daily=[d["minutes"] for d in k["daily"]])
    spec["parses"].append(case)


# Synthetic payloads (same generator as the Python offline tests)
import test_offline as fx  # noqa: E402

now = ts(2026, 11, 16, 11, 15)
for wk_label, t in (("this", now), ("last", ts(2026, 11, 9, 12))):
    w = rr.resolve_week(datetime.fromtimestamp(t, TZ).date().isoformat(), now=datetime.fromtimestamp(now, TZ))
    for name in ("Sam", "Alex"):
        add_parse(f"synthetic-{name}-{wk_label}", name, json.dumps(fx.activity_payload(fx.KIDS[name], w.request_body(fx.KIDS[name]))), t, now)
add_parse("shape-renamed", "Sam", json.dumps({"activityData": []}), now, now, status=5)
add_parse("shape-not-json", "Sam", "<html>", now, now, status=5)
add_parse("shape-no-intervals", "Sam", json.dumps({"activityV2Data": [{"category": "BOOK"}]}), now, now, status=5)
add_parse("empty-week", "Sam", json.dumps({"activityV2Data": []}), now, now)

if live:
    import direct_report as dr  # noqa: E402
    http = dr.Http(rr.BASE, dr.load_cookies(dr.DEFAULT_COOKIE_FILE))
    kids = dr.get_household(http)
    real_now = int(datetime.now(TZ).timestamp())
    for wk_label, back in (("this", 0), ("last", 1)):
        w = rr.resolve_week(wk_label)
        for name in ("Sam", "Alex"):
            st, info, raw = http.request("POST", rr.ACTIVITY_PATH, w.request_body(kids[name]))
            assert dr.classify_failure(st, info, raw, rr.ACTIVITY_PATH) is None, st
            add_parse(f"live-{name}-{wk_label}", name, raw.decode(), w.start_epoch + 3600 * 12, real_now)

for label, code, loc, ct, body, status in [
    ("ok", 200, "", "application/json;charset=UTF-8", "{}", 0),
    ("401", 401, "", "application/json", "{}", 3),
    ("redirect-signin", 302, "https://www.amazon.com/ap/signin?x", "text/html", "", 3),
    ("signin-page", 200, "", "text/html", "<input id='ap_password'>", 3),
    ("captcha-redirect", 302, "https://www.amazon.com/errors/validateCaptcha", "text/html", "", 4),
    ("captcha-page", 200, "", "text/html", "Enter the characters you see below (captcha)", 4),
    ("html-200", 200, "", "text/html", "<html>hi</html>", 5),
    ("500", 500, "", "text/html", "oops", 1),
]:
    spec["classify"].append({"label": label, "code": code, "location": loc, "content_type": ct, "body": body, "status": status})

(out / "spec.json").write_text(json.dumps(spec, indent=1))
print(f"spec: {len(spec['weeks'])} weeks, {len(spec['parses'])} parses, {len(spec['classify'])} classify -> {out / 'spec.json'}")
