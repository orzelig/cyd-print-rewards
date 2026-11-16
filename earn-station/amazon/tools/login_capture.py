#!/usr/bin/env python3
"""Open a separate Playwright browser so a person can sign in to Amazon Parents,
then save the session cookies for direct_report.py.

* Uses its own profile directory, separate from your everyday browser.
* The script never fills in or clicks anything; it only waits until the dashboard
  has loaded with household data, i.e. you are signed in.
* Cookies are written to a chmod-600 file. Values are never printed.

Usage:
  python login_capture.py [--profile DIR] [--cookie-file FILE] [--timeout-min 10]
"""
from __future__ import annotations

import argparse
import os
import sys
import time
from datetime import datetime
from pathlib import Path

import reading_report as rr

CONFIG_DIR = Path("~/.config/reading-report").expanduser()


def signed_in(page) -> bool:
    try:
        if not page.url.startswith(rr.BASE):
            return False
        # The page may drop the global once it has drawn itself; its inline script is still in the page.
        return bool(page.evaluate("() => !!(window.__PREFETCHED_HOUSEHOLD && window.__PREFETCHED_HOUSEHOLD.members)"
                                  " || [...document.scripts].some(s => s.text.includes('__PREFETCHED_HOUSEHOLD'))"))
    except Exception:
        return False  # page mid-navigation


def find_kids(value, kids: dict) -> None:
    """Collect {firstName: directedId} for CHILD members from any JSON the dashboard loads."""
    if isinstance(value, dict):
        did, name = value.get("directedId"), value.get("firstName")
        if isinstance(did, str) and did.startswith("amzn1.") and isinstance(name, str) and value.get("role", "CHILD") == "CHILD":
            kids[name] = did
        for v in value.values():
            find_kids(v, kids)
    elif isinstance(value, list):
        for v in value:
            find_kids(v, kids)


class KidWatcher:
    """Reads the kids' names and ids from the page's own data requests (values never printed)."""

    def __init__(self, ctx):
        self.kids: dict[str, str] = {}
        self.paths: set[str] = set()
        ctx.on("response", self._on_response)

    def _on_response(self, resp):
        try:
            if not resp.url.startswith(rr.BASE) or "json" not in (resp.headers.get("content-type") or ""):
                return
            self.paths.add(resp.url.split("?")[0][len(rr.BASE):])
            find_kids(resp.json(), self.kids)
        except Exception:
            pass


def enter_pressed() -> bool:
    import select
    try:
        if select.select([sys.stdin], [], [], 0)[0]:
            sys.stdin.readline()
            return True
    except Exception:
        pass
    return False


def write_children(kids: dict, path: Path) -> None:
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        f.write("".join(f"{n}\t{i}\n" for n, i in sorted(kids.items())))


def write_cookie_file(cookies: list[dict], path: Path) -> list[dict]:
    amazon = [c for c in cookies if c["domain"].lstrip(".").endswith("amazon.com")]
    lines = ["# Netscape HTTP Cookie File (written by login_capture.py; treat like a password)"]
    for c in amazon:
        domain = c["domain"]
        prefix = "#HttpOnly_" if c.get("httpOnly") else ""
        expires = int(c["expires"]) if c.get("expires", -1) > 0 else 0
        lines.append("\t".join([prefix + domain, "TRUE" if domain.startswith(".") else "FALSE", c["path"],
                                "TRUE" if c.get("secure") else "FALSE", str(expires), c["name"], c["value"]]))
    path.parent.mkdir(parents=True, exist_ok=True)
    os.chmod(path.parent, 0o700)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    os.chmod(path, 0o600)
    return amazon


def persist_cookies(pw, profile: Path, cookies: list[dict]) -> None:
    ctx = pw.chromium.launch_persistent_context(str(profile), headless=True)
    try:
        ctx.add_cookies(cookies)
    finally:
        ctx.close()


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--profile", default=str(CONFIG_DIR / "browser-profile"))
    p.add_argument("--cookie-file", default=str(CONFIG_DIR / "cookie.txt"))
    p.add_argument("--timeout-min", type=float, default=10)
    args = p.parse_args(argv)

    from playwright.sync_api import sync_playwright

    profile = Path(args.profile).expanduser()
    profile.mkdir(parents=True, exist_ok=True)
    os.chmod(profile, 0o700)
    with sync_playwright() as pw:
        ctx = pw.chromium.launch_persistent_context(str(profile), headless=False, timezone_id=rr.TZ_NAME,
                                                   locale="en-US", viewport={"width": 1200, "height": 900})
        watcher = KidWatcher(ctx)
        page = ctx.pages[0] if ctx.pages else ctx.new_page()
        page.goto(rr.ACTIVITY_PAGE, wait_until="domcontentloaded")
        print("A separate browser window is open. Sign in to Amazon there yourself "
              f"(waiting up to {args.timeout_min:g} min)...\n"
              "If the dashboard is showing and nothing happens, press Enter here.", flush=True)
        deadline = time.monotonic() + args.timeout_min * 60
        last_nudge = 0.0
        while time.monotonic() < deadline:
            if any(signed_in(pg) for pg in ctx.pages):
                break
            on_dashboard = any(pg.url.startswith(rr.BASE) for pg in ctx.pages)
            if on_dashboard and watcher.kids:
                break  # the page loaded the household over the network instead of embedding it
            if enter_pressed():
                if on_dashboard:
                    break
                print("Not on the Amazon Parent Dashboard yet; finish signing in first.", flush=True)
            if not ctx.pages:
                print("ERROR: the browser window was closed before sign-in finished.", file=sys.stderr)
                return rr.EXIT_SIGNED_OUT
            # Signed in but landed on another dashboard page: open the activity page (never on sign-in pages).
            for pg in ctx.pages:
                try:
                    url = pg.url
                except Exception:
                    continue
                if url.startswith(rr.BASE) and "/activities/household-summary" not in url \
                        and time.monotonic() - last_nudge > 20:
                    last_nudge = time.monotonic()
                    try:
                        pg.goto(rr.ACTIVITY_PAGE, wait_until="domcontentloaded")
                    except Exception:
                        pass
                    break
            time.sleep(2)
        else:
            ctx.close()
            print("ERROR: timed out waiting for sign-in.", file=sys.stderr)
            return rr.EXIT_SIGNED_OUT

        time.sleep(3)  # let late Set-Cookie headers land
        all_cookies = ctx.cookies()
        saved = write_cookie_file(all_cookies, Path(args.cookie_file).expanduser())
        kids_file = Path(args.cookie_file).expanduser().with_name("children.tsv")
        if watcher.kids:
            write_children(watcher.kids, kids_file)
            print(f"Kids found: {', '.join(sorted(watcher.kids))} (ids saved to {kids_file})")
        else:
            print("Kids not found in the page's data. Data the page loaded: " + ", ".join(sorted(watcher.paths)[:15]))
        ctx.close()
        # A headed persistent Chromium does not write its cookie store on close (seen on macOS,
        # Playwright 1.5x); a headless one does. Write the session into the profile that way.
        persist_cookies(pw, profile, all_cookies)

    print(f"Signed in. Saved {len(saved)} amazon.com cookies to {args.cookie_file} (chmod 600).")
    print(f"Browser profile kept at {profile} (usable with reading_report.py --profile).")
    print("Cookie names and expiry (values not shown):")
    for c in sorted(saved, key=lambda c: c["name"]):
        exp = c.get("expires", -1)
        when = datetime.fromtimestamp(exp).strftime("%Y-%m-%d") if exp and exp > 0 else "session"
        print(f"  {c['name']:<28} {c['domain']:<22} expires {when}{'  httpOnly' if c.get('httpOnly') else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
