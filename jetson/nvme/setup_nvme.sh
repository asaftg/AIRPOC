#!/bin/bash
# Provision the NVMe as the AIRPOC data volume. DESTRUCTIVE to the given disk.
#   sudo ./setup_nvme.sh [/dev/nvme0n1] [-f]
# Refuses a non-blank disk unless -f. Installs and enables the /data mount unit
# (WantedBy, not Required: a missing/dead NVMe never blocks boot — the recorder
# reports disk absent and refuses to record).
set -euo pipefail
cd "$(dirname "$0")"

DEV=${1:-/dev/nvme0n1}
FORCE=${2:-}
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
[ -b "$DEV" ] || { echo "no block device $DEV"; exit 1; }

if lsblk -no FSTYPE "$DEV" | grep -q . && [ "$FORCE" != "-f" ]; then
    echo "$DEV is not blank; rerun with -f to wipe it"
    exit 1
fi

parted -s "$DEV" mklabel gpt mkpart data ext4 1MiB 100%
udevadm settle
PART="${DEV}p1"
[ -b "$PART" ] || PART="${DEV}1"
mkfs.ext4 -q -F -L AIRPOC-DATA "$PART"

install -m 644 data.mount /etc/systemd/system/data.mount
systemctl daemon-reload
systemctl enable --now data.mount

mkdir -p /data/recordings
chown "${SUDO_USER:-asaftg}:${SUDO_USER:-asaftg}" /data /data/recordings

df -h /data
echo "OK: AIRPOC-DATA mounted at /data"
