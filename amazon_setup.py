#!/usr/bin/env python3
"""Load the Amazon Kids sign-in onto the earning station, so Kindle reading earns like timed reading.

  python3 amazon_setup.py                   # set up (or refresh the sign-in), count Kindle reading from now on
  python3 amazon_setup.py --count-this-week # first setup only: also credit this week's Kindle reading so far
  python3 amazon_setup.py --status          # what the station last saw
  python3 amazon_setup.py --forget          # remove the sign-in from the station

Before the first run (and whenever the station shows "Amazon sign-in needed"), sign in with
  python3 earn-station/amazon/tools/login_capture.py
which saves ~/.config/reading-report/cookie.txt. This script sends only the two cookies the station
needs (at-main, ubid-main) straight to the station, plus each child's name and Amazon id. It never
prints them. The station asks Amazon once an hour from 2 pm to 9 pm.
"""
import argparse
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOLS = HERE / "earn-station" / "amazon" / "tools"
sys.path.insert(0, str(TOOLS))
QUOTA = os.environ.get("QUOTA_HOST", "printquota.local")


def http(method, url, body=None, headers=None, timeout=15):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method, headers=headers or {})
    if data is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


def station():
    """The earning station's address and key, from the print quota device."""
    code, text = http("GET", f"http://{QUOTA}/api/earn/device/access",
                      headers={"X-Client": "ota", **({"Authorization": basic()} if os.environ.get("ADMIN_PASS") else {})})
    if code != 200:
        sys.exit(f"Can't get the station from {QUOTA} ({code}): {text}")
    devices = [d for d in json.loads(text) if d.get("ip")]
    if not devices:
        sys.exit("No earning station is connected to the print quota device.")
    return devices[0]["ip"], devices[0]["key"]


def basic():
    import base64
    return "Basic " + base64.b64encode(f"admin:{os.environ['ADMIN_PASS']}".encode()).decode()


def show_status(ip):
    code, text = http("GET", f"http://{ip}/")
    k = json.loads(text).get("kindle", {})
    print(f"Kindle: {k.get('state')}" + (f" - {k['error']}" if k.get("error") else ""))
    if k.get("last"):
        print("  last checked:", time.strftime("%a %H:%M", time.localtime(k["last"])))
    if k.get("next"):
        print("  next check:  ", time.strftime("%a %H:%M", time.localtime(k["next"])))
    for kid in k.get("kids", []):
        mins = lambda s: f"{s // 3600}h {s // 60 % 60:02d}m" if s >= 3600 else f"{max(s, 0) // 60}m"
        who = kid.get("station_name") or "(no one with this name on the station)"
        amazon = mins(kid["amazon_week_s"]) if kid.get("amazon_week_s", -1) >= 0 else "?"
        print(f"  {kid['name']} -> {who}: Amazon this week {amazon}, counted {mins(kid.get('credited_s', 0))}")
    if k.get("counting_from_now"):
        print("  (counting starts from the first check; earlier reading this week isn't credited)")


def children(cookie_file):
    saved = Path(cookie_file).with_name("children.tsv")  # written by login_capture.py
    if saved.exists():
        kids = [{"name": n.strip(), "id": i.strip()} for n, i in
                (line.split("\t", 1) for line in saved.read_text().splitlines() if "\t" in line)]
        if kids:
            return kids
    out =subprocess.run([sys.executable, str(TOOLS / "direct_report.py"), "--list-children", "--cookie-file", cookie_file],
                         capture_output=True, text=True, timeout=60)
    if out.returncode != 0:
        sys.exit("Couldn't list the children from Amazon (exit %d):\n%s" % (out.returncode, out.stderr.strip()[-500:]))
    kids = []
    for line in out.stdout.splitlines():
        if "\t" in line:
            name, cid = line.split("\t", 1)
            kids.append({"name": name.strip(), "id": cid.strip()})
    if not kids:
        sys.exit("Amazon listed no children.")
    return kids


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--cookie-file", default=os.path.expanduser("~/.config/reading-report/cookie.txt"))
    p.add_argument("--count-this-week", action="store_true", help="first setup: credit this week's Kindle reading so far")
    p.add_argument("--status", action="store_true")
    p.add_argument("--forget", action="store_true")
    args = p.parse_args()

    ip, key = station()
    hdr = {"X-Earn-Key": key}
    if args.status:
        return show_status(ip)
    if args.forget:
        code, text = http("POST", f"http://{ip}/amazon/forget", {}, hdr)
        return print("Removed the Amazon sign-in from the station." if code == 200 else f"Failed ({code}): {text}")

    import direct_report  # the cookie file reader from reading-report
    cookies = direct_report.load_cookies(args.cookie_file)
    if not cookies.get("at-main") or not cookies.get("ubid-main"):
        sys.exit(f"{args.cookie_file} has no at-main / ubid-main cookie. Sign in again with login_capture.py.")
    kids = children(args.cookie_file)
    body = {"at": cookies["at-main"], "ubid": cookies["ubid-main"], "kids": kids, "count_this_week": args.count_this_week}
    code, text = http("POST", f"http://{ip}/amazon/setup", body, hdr)
    if code != 200:
        sys.exit(f"The station refused it ({code}): {text}")
    print("Sent to the station: the sign-in and " + ", ".join(k["name"] for k in kids) + ". Checking Amazon now...")
    for _ in range(30):
        time.sleep(2)
        k = json.loads(http("GET", f"http://{ip}/")[1]).get("kindle", {})
        if k.get("last") and not k.get("fetching"):
            break
    show_status(ip)


if __name__ == "__main__":
    main()
