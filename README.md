# CYD Print Rewards

Two small touchscreen devices that let kids earn 3D-printer time (and pocket money) by reading and doing chores.
Both run on the ESP32-2432S028R "Cheap Yellow Display" (CYD), a ~$15 board with a 2.8" 320×240 touchscreen.

| Device | Folder | What it does |
|---|---|---|
| **Print quota device** ("the bank") | [print-quota/](print-quota/) | Sits next to a Bambu Lab printer. Each person taps their name before printing, and the time the printer actually spends printing is taken from their balance. It keeps every balance and rule, and serves the admin page at `http://printquota.local`. |
| **Earning station** | [earn-station/](earn-station/) | Kids tap their name and start a reading timer or log a finished chore. What they earn goes straight to the bank. It can also count Kindle reading from Amazon Kids (optional, see below). |

You can run the print quota device alone. The earning station needs the print quota device.

## How earning works

All of this is set on the admin page, in the Earning station section. The defaults are:

- **Print time:** 2 minutes of printing for every minute read, added right away.
- **Filament money:** $0.50 per hour read, added right away.
- **Cash:** $1.00 per hour read. It's paid only when the kid reaches the weekly goal (7 hours, Sunday 00:00 to Saturday midnight). Until then it shows as "pending". Last week stays open until Sunday 8 am so late Kindle data still counts. If the goal wasn't reached by then, that week's pending cash is dropped. Filament money and cash build up on the device. When you pay some out, take it off in the admin page's Money section.
- **"Still reading?" check:** every 30 minutes the timer asks for a tap. If nobody taps within 3 minutes, the timer stops and the unanswered minutes don't count. Several people can read at once.
- **Chores:** each chore adds a fixed amount of print time, filament money and/or cash, up to a set number of times a day.

Prints are never paused or stopped by the device. A print that starts with time left always finishes, even if the balance goes negative.

## Hardware

- 1 or 2 × ESP32-2432S028R ("Cheap Yellow Display", ILI9341 + XPT2046 resistive touch). The pin setup is in each `platformio.ini`. Boards sold under the same name with a different display driver (for example ST7789) need those flags changed.
- A Bambu Lab printer on the same network, with LAN mode on (tested with a P2S). Without a printer you can try everything in **Demo** mode.

## Build and flash

You need [PlatformIO](https://platformio.org/) (the VS Code extension or `pip install platformio`).

```bash
cd print-quota && pio run -t upload
```

```bash
cd earn-station && pio run -t upload
```

Before building the earning station, set your **time zone** in [earn-station/platformio.ini](earn-station/platformio.ini) (`HOUSEHOLD_TZ_POSIX` and `HOUSEHOLD_TZ_NAME`; there are examples in the comments). The reading week runs Sunday to Saturday in that time zone. The print quota device's time zone is set on its admin page.

After the first USB flash, both devices update over Wi-Fi:

```bash
./ota.sh quota
```

```bash
./ota.sh station
```

`ota.sh` uploads the firmware PlatformIO built last. If you set an admin password, run `export ADMIN_PASS=...` first. If `printquota.local` doesn't resolve, use the IP: `QUOTA=192.168.x.y ./ota.sh quota`.

## First setup

1. **Wi-Fi.** On first boot each device opens a setup Wi-Fi network: `PrintQuota-Setup` or `EarnStation-Setup`. Join it with your phone and pick your home Wi-Fi. To forget the Wi-Fi later, hold the BOOT button for 5 seconds.
2. **Admin page.** Open `http://printquota.local`. Add people, top up balances, and in **Settings** enter the printer's IP address, serial number and LAN access code (on the printer: Settings → Network / LAN Only). Pick your time zone and set an **admin password**.
3. **Earning station.** Once it's on Wi-Fi it finds `printquota.local` and asks to pair. On the admin page tap **Allow**, after checking that the code matches the one on the station's screen. From then on the station sends its own key with every request.

The USB serial console (115200 baud) has recovery commands. For example, `clear-admin-password` on the print quota device removes the admin password.

## Kindle reading from Amazon Kids (optional)

The earning station can also count reading done on a Kindle with an Amazon Kids profile. It checks once an hour from 2 pm to 9 pm, on every boot, and early on Sunday morning. It credits only the "Books" time it hasn't counted yet, exactly like reading timed on the station. Each Amazon child is matched to a station person by first name.

**Before you turn it on, know this:**

- It uses the same web requests as the Amazon Parent Dashboard page. Amazon doesn't offer a public API for this, so it may stop working whenever Amazon changes the page. Check that using it fits Amazon's terms for your account.
- It needs a parent's Amazon sign-in, as two cookies (`at-main`, `ubid-main`). They are stored only in the station's flash and sent only to Amazon. They are never shown on the screen, the web page or the serial console. Anyone with physical access to the board could read the flash, so keep the station somewhere you trust.
- If Amazon asks for a new sign-in or shows a robot check, the station stops checking completely and shows "Amazon sign-in needed". It never tries to get past either.

Setup, from a computer on the same network:

```bash
python3 -m venv .venv && .venv/bin/pip install -r earn-station/amazon/tools/requirements.txt && .venv/bin/playwright install chromium
```

```bash
.venv/bin/python earn-station/amazon/tools/login_capture.py
```

That opens a separate browser window, where you sign in to Amazon yourself. The script never fills anything in. It saves the cookies to `~/.config/reading-report/cookie.txt` (chmod 600), and the kids' names and Amazon ids to `children.tsv` next to it.

```bash
.venv/bin/python amazon_setup.py
```

That sends the two cookies and the kids' names and ids to the station. Add `--count-this-week` the first time to also credit this week's reading so far. Use `--status` to see what the station last saw, and `--forget` to remove the sign-in. For the Python tools on their own (weekly reports from the command line), see [earn-station/amazon/tools/README.md](earn-station/amazon/tools/README.md).

If you don't want this feature, don't run the setup. The station never contacts Amazon until it has a sign-in.

## Security notes

- Everything runs on your local network over plain HTTP. Don't expose the devices to the internet.
- Without an admin password, anyone on your Wi-Fi can change balances. Set one.
- The earning station can only add reading and chores. It can't change balances or settings.

## Emulator

The earning station also runs in [Wokwi](https://wokwi.com/) (`pio run -e wokwi`, then start the simulator in VS Code). Set `BANK_HOST` in the `wokwi` section of `earn-station/platformio.ini` to your print quota device's IP, because mDNS doesn't reach the emulator.

## Repository layout

```
common/          shared UI kit + Noto Sans smooth fonts (make_fonts.py regenerates them)
print-quota/     the bank: printer (Bambu MQTT), users, quotas, earning rules, admin web page
earn-station/    reading timer, chores, Kindle reading (src/amazon.cpp + src/amazon/)
  amazon/tools/  Python reference tools: Amazon sign-in capture and weekly reading reports
  amazon/test_host/  desktop tests for the portable Amazon parsing code
amazon_setup.py  loads the Amazon sign-in onto the earning station
ota.sh           over-the-air updates
```

## License

The code is under the [MIT License](LICENSE). The Noto Sans font (`common/NotoSans-Bold.ttf` and the font headers made from it) is under the [SIL Open Font License 1.1](common/OFL.txt).

This project isn't affiliated with Bambu Lab or Amazon. Kindle and Amazon Kids are trademarks of Amazon.com, Inc. or its affiliates.
