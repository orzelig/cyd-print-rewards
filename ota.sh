#!/bin/bash
# Update the devices over Wi-Fi with the firmware last built by PlatformIO.
#   ./ota.sh quota     print quota device (printquota.local)
#   ./ota.sh station   every paired earning station (address and key come from the quota device)
# If the quota device has an admin password, set ADMIN_PASS first.
set -euo pipefail
cd "$(dirname "$0")"
QUOTA=${QUOTA:-printquota.local}
AUTH=(-H "X-Client: ota")  # never empty: macOS bash 3.2 treats an empty array as unset
if [ -n "${ADMIN_PASS:-}" ]; then AUTH+=(-u "admin:$ADMIN_PASS"); fi

case "${1:-}" in
  quota)
    fw=print-quota/.pio/build/cyd/firmware.bin
    echo "Uploading $fw to $QUOTA ..."
    curl -sS --fail-with-body "${AUTH[@]}" -F "firmware=@$fw" "http://$QUOTA/update"; echo
    ;;
  station)
    fw=earn-station/.pio/build/cyd/firmware.bin
    curl -sS --fail-with-body "${AUTH[@]}" "http://$QUOTA/api/earn/device/access" |
      python3 -c 'import sys,json; [print(d["ip"], d["key"], d["name"]) for d in json.load(sys.stdin) if d.get("ip")]' |
      while read -r ip key name; do
        echo "Uploading $fw to $name at $ip ..."
        curl -sS --fail-with-body -H "X-Earn-Key: $key" -F "firmware=@$fw" "http://$ip/update"; echo
      done
    ;;
  *)
    echo "usage: $0 quota|station" >&2
    exit 1
    ;;
esac
