#!/bin/bash
# offload_pull — run FROM the workstation: pull recordings off the Jetson.
#   ./offload_pull.sh [user@host] [--meta|--display|--full] [--prune-native] [dest]
# Two passes: meta+display for all sessions first, raw (--full) afterwards.
# Resumable; zstd wire compression when both rsync ends support it.
set -euo pipefail

HOST=${1:-asaftg@orin-nano}
TIER=${2:---display}
PRUNE=""
[ "${3:-}" = "--prune-native" ] && { PRUNE=1; DEST=${4:-./recordings}; } || DEST=${3:-./recordings}
SRC="$HOST:/data/recordings/"
mkdir -p "$DEST"

RS="rsync -a --partial --info=progress2"
if rsync --version | head -1 | grep -qE ' 3\.([2-9]|[1-9][0-9])' &&
   ssh "$HOST" "rsync --version | head -1" | grep -qE ' 3\.([2-9]|[1-9][0-9])'; then
    RS="$RS --compress --compress-choice=zstd"
else
    RS="$RS -z"
fi

EXC_RAW=(--exclude '*/eo_y10/**')
EXC_DISP=(--exclude '*/eo_y10/**' --exclude '*/eo_jpeg/**')

case "$TIER" in
--meta)    $RS "${EXC_DISP[@]}" "$SRC" "$DEST/" ;;
--display) $RS "${EXC_RAW[@]}"  "$SRC" "$DEST/" ;;
--full)
    $RS "${EXC_RAW[@]}" "$SRC" "$DEST/"          # pass 1: browsable fast
    $RS "$SRC" "$DEST/"                          # pass 2: raw channels
    ;;
*) echo "tier must be --meta|--display|--full"; exit 1 ;;
esac

if [ -n "$PRUNE" ] && [ "$TIER" = "--full" ]; then
    for d in "$DEST"/*/; do
        sid=$(basename "$d")
        [ -d "$d/eo_y10" ] || continue
        if python3 "$(dirname "$0")/airec_dump.py" "$d" --verify --chan eo_y10 >/dev/null; then
            echo "verified $sid — purging raw on the Jetson"
            ssh "$HOST" "curl -s 'localhost:8093/ctl?purge_native=$sid'" && echo
        else
            echo "VERIFY FAILED for $sid — raw kept on the Jetson" >&2
        fi
    done
fi
echo "done -> $DEST"
