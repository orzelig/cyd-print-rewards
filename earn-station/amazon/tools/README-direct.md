# Direct mode (no browser): direct_report.py + login_capture.py

`direct_report.py` calls the Amazon Parent Dashboard endpoint directly with plain HTTP (Python standard library only). It needs no browser, so it shows what a small device like an ESP32 would have to do.

## Getting a session cookie (one time, by a person)

```bash
python login_capture.py        # needs Playwright: pip install -r requirements.txt && playwright install chromium
```
This opens a **separate** Chromium window with its own profile in `~/.config/reading-report/browser-profile`. **You** sign in there; the script never touches the login form. When the dashboard loads, it:
* writes the amazon.com cookies to `~/.config/reading-report/cookie.txt` (folder 700, file 600)
* copies them into the profile, so `reading_report.py --profile ~/.config/reading-report/browser-profile` works too.

The copy is needed because a *visible* Playwright Chromium doesn't save cookies when it closes, while a headless one does. Without it, the profile ends up signed out.

`cookie.txt` is a live login to the Amazon account, so treat it like a password. The scripts never print cookie values.

## Usage

```bash
python direct_report.py --week this|last|YYYY-MM-DD --out report.json
python direct_report.py --child-ids "Sam=amzn1.account...,Alex=amzn1.account..."   # skips the 140 KB household page
python direct_report.py --probe --probe-cookies       # evaluation: sizes, headers, minimal cookie set
```
The output JSON and the exit codes are the same as `reading_report.py`. For direct mode, exit 3 means the cookie was rejected (HTTP 401/403 or a redirect to sign-in): run `login_capture.py` again.

## Live findings (2026-11-16, probe against the real endpoint)

| Question | Result |
|---|---|
| Works without a browser? | **Yes.** Plain urllib with a non-browser User-Agent got HTTP 200 and the same numbers as the dashboard for every child. |
| Cookies actually required | **2 of 19: `at-main`, `ubid-main`.** About 490 bytes of Cookie header. |
| CSRF token / Origin / Referer needed? | No. |
| Missing or invalid cookie | Clean HTTP 401, easy to detect. |
| Activity response size | 2.3 KB (this week so far) and 4.3 KB (full last week) per kid. |
| Household page (to find child IDs) | 140 KB of HTML. Skip it on small devices and hard-code the two `directedId`s. |
| Latency | 160–310 ms per call. |
| Cookie lifetime (as set) | `at-main` and `ubid-main` expire **2027-11-16** (1 year). Amazon can revoke them earlier from its side (password change, "sign out everywhere", security checks), so the real lifetime is **not yet known**. Run it daily for a while to find out. |

## What this means for an ESP32

Technically it's easy: one HTTPS POST per kid, a ~500-byte Cookie header, and a JSON reply of a few KB. That fits comfortably with `esp_http_client` plus the ESP-IDF certificate bundle and a streaming or filtered JSON parser. Hard-code the child IDs.

The catch is that `at-main` is the main login token for the whole Amazon account, not a read-only key:
* If it's stored on the ESP32, anyone with physical access can dump the flash and get into the account, unless flash encryption and NVS encryption are turned on.
* When Amazon revokes the token, the device can't sign in again by itself. A person runs `login_capture.py` and gets the new cookie onto the device.

The safer setup: run `direct_report.py` on the box on a schedule (no browser needed any more) and serve `report.json` on the LAN. The ESP32 only fetches and displays that file, so the Amazon token never leaves the box.
