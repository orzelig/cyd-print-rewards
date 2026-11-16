# reading-report

Pulls weekly **Books** reading time for Sam and Alex from the Amazon Parent Dashboard
(parents.amazon.com). It uses headless Playwright with the box browser's existing
signed-in session.

* No AI browsing, no passwords. The script never types into a page and never stores credentials.
* If Amazon shows a sign-in page, a captcha or a bot check, the script **stops** with an exit code. It never tries to get past one.
* It only produces numbers (a JSON file plus a short summary). It sends nothing to anyone.

## Where the data comes from

Checked against the live dashboard on 2026-11-16:

| What | Source |
|---|---|
| Kids → IDs | `window.__PREFETCHED_HOUSEHOLD.members[]` on `/activities/household-summary` (`role: "CHILD"`, `firstName`, `directedId`) |
| Activity | **`POST https://parents.amazon.com/ajax/get-weekly-activities-v2`** |
| Request body | `{childDirectedId, startTime, endTime, aggregationInterval: 86400, timeZone: "America/New_York"}`. Times are epoch **seconds**. A finished week ends at Saturday 23:59:59; the current week ends at "now". |
| Response | `activityV2Data[]` → `{category: "BOOK", intervals[] → {startTime, aggregatedDuration, aggregatedActivityResults[] → {key, activityDuration (seconds), attributes.TITLE}}}` |

How the script gets each kid's data:
1. It opens `/activities/household-summary`. That page loads the current and previous week for the selected child, and the script captures those responses with `page.on("response")`.
2. For each other kid, it clicks that kid in the page's child switcher, so the page loads that kid's weeks itself, and captures those responses too.
3. Weeks older than last week aren't loaded by the page. For those, the script makes the same POST from inside the page with `fetch` (same origin, same cookies). In the JSON these show as `"source": "replayed"`.

Only the `BOOK` category counts. Other categories, such as video and apps, are ignored.
Each run logs the endpoint it used to stderr. Add `--discover` to also list every `/ajax/` call the page made.

## Setup (on the box)

```bash
cd /home/box/scripts/reading-report
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/playwright install chromium        # add --with-deps the first time on a fresh box (needs sudo)
```

Point the script at the signed-in browser. Pick **one** of these:

**A. Profile directory (default, headless).** This is the browser's *user-data dir*: the parent of `Default/`. In that browser, open `chrome://version` and take the parent of "Profile Path". Examples: `~/.config/chromium`, `~/.config/google-chrome`, or the directory a Playwright persistent context was created with.
```bash
export READING_REPORT_PROFILE=/home/box/.config/chromium     # or pass --profile
```
* If that browser is open while the script runs, add `--copy-profile`. The script then works on a temporary copy, because Chrome locks a profile that's in use.
* If the profile was made by **Google Chrome** (not Playwright), Chrome may have encrypted its cookies with the OS keyring. Add `--channel chrome --system-keyring`. Otherwise the cookies can't be read, and the run looks like an expired session (exit 3).

**B. Attach to the running browser (`--cdp`).** Use this when cookie decryption is a problem. Start the box browser with `--remote-debugging-port=9222 --user-data-dir=<its dir>`. Chrome 136+ ignores the debugging port on the *default* user-data dir. Then run with `--cdp http://127.0.0.1:9222`. This uses the real browser window, so it isn't headless. The script opens one tab and closes it when done. Keep the port bound to localhost.

## Usage

```bash
.venv/bin/python reading_report.py --week this --out report.json         # this week so far
.venv/bin/python reading_report.py --week last --out last.json
.venv/bin/python reading_report.py --week 2026-11-04 --out wk.json       # any date in the wanted week
```

Options: `--kids Sam,Alex` (or set `READING_KIDS`), `--profile DIR` / `--cdp URL`, `--copy-profile`, `--channel chrome`,
`--system-keyring`, `--headed` (debug only), `--discover`, `--debug-dir DIR` (saves a screenshot on failure).
Without `--out`, the JSON goes to stdout and the summary goes to stderr.

Weeks run Sunday 00:00 to Saturday 23:59 in the household time zone (`READING_TZ`, default America/New_York). **Daily average = total ÷ days counted.** A finished week counts 7 days. "This week" counts the days so far, including today, which is how the dashboard does it. Minutes are rounded to the nearest minute.

### Output

stdout summary:
```
Week of 2026-11-15 (so far)
Sam 1h 35m (daily avg 48m)
Alex 1h 27m (daily avg 44m)
```

`report.json` (trimmed):
```json
{
  "generated_at": "2026-11-16T11:20:41-06:00",
  "timezone": "America/New_York",
  "category": "Books",
  "week": {"start": "2026-11-15", "end": "2026-11-21", "complete": false,
           "through": "2026-11-16T11:20-06:00", "days_counted": 2},
  "endpoint": {"method": "POST", "url": "https://parents.amazon.com/ajax/get-weekly-activities-v2", "path": "..."},
  "kids": {
    "Sam": {
      "name": "Sam", "total_minutes": 95, "total_seconds": 5700,
      "daily_average_minutes": 48, "days_counted": 2,
      "titles": [{"title": "The Wild Robot", "minutes": 75, "seconds": 4500},
                 {"title": "Dog Man: Mothering Heights", "minutes": 20, "seconds": 1200}],
      "daily": [{"date": "2026-11-15", "weekday": "Sun", "minutes": 65},
                {"date": "2026-11-16", "weekday": "Mon", "minutes": 30}],
      "source": "captured"
    },
    "Alex": {"...": "..."}
  }
}
```
(The sample numbers above come from the offline test fixture.)

## Exit codes

| Code | Meaning | What to do |
|---|---|---|
| 0 | OK | |
| 1 | Unexpected error (bug, network, timeout) | Re-run with `--debug-dir /tmp/rr-debug` and look at the screenshot |
| 2 | Bad usage: bad `--week`, a future week, no profile given | Fix the arguments |
| 3 | **Session expired**: sign-in page, or HTTP 401/403 from the API | A person signs in again in the box browser. The script never will. |
| 4 | **Captcha / bot check** (`validateCaptcha`, `/ap/cvf`, Arkose) | A person opens the box browser and clears it. The script never will. |
| 5 | **Data shape changed**: `__PREFETCHED_HOUSEHOLD` or `activityV2Data` missing, or a field has the wrong type | Run with `--discover --headed`, find the new endpoint/fields, and update the constants at the top of `reading_report.py` and `parse_activity()` |
| 6 | Browser problem: profile not found or locked, can't launch, CDP unreachable | See setup; `--copy-profile` for a locked profile |
| 7 | A requested kid isn't in the household | Check `--kids` spelling against the "children in household" log line |

Errors are printed to stderr as `ERROR (<code>): <message>`.

## Known failure modes

* **Headless gets a bot check.** In testing, a headless Chromium request to `amazon.com/ap/signin` was answered with a `validateCaptcha` page. A signed-in session usually goes straight to the dashboard. But if Amazon starts challenging the headless browser, you'll get exit 4 on every run. Options: run on a schedule less often, use `--cdp` against the real browser, or sign in again by hand. Don't automate the challenge.
* **Cookies can't be decrypted.** The profile came from Google Chrome but was launched with bundled Chromium, and you get exit 3 even though the box browser is signed in. Use `--channel chrome --system-keyring` or `--cdp`. A headless run with no unlocked desktop keyring can still fail; use `--cdp` then.
* **Profile locked.** The box browser is open on the same profile, giving exit 6. Use `--copy-profile`. The copy is deleted after the run, and any refreshed cookies are not written back.
* **The dashboard is redesigned.** You'll see exit 5, or the child switcher isn't found. A missing switcher only logs a warning: the script falls back to the in-page request, so the numbers stay correct.
* **"This week" on Sunday morning** covers only one day. The daily average is then the same as the total.
* **Time zone.** The script asks Amazon for days in `READING_TZ` whatever the box's own time zone is. Daily buckets come from Amazon's per-day intervals.
* **Titles** are Amazon's full store titles (e.g. "The Getaway (Diary of a Wimpy Kid #12): A holiday vacation gone…"). The same book on different days is merged by Amazon's item key.

## Tests

`tests/test_offline.py` serves a fake parents.amazon.com through Playwright routes. No network and no account are needed: every other request is blocked. It covers this week, last week, older weeks (the replay path), DST, sign-in, captcha, API 401, a shape change, an unknown kid and bad arguments.

```bash
.venv/bin/python tests/test_offline.py
```
