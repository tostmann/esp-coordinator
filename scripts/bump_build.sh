#!/usr/bin/env bash
# Pre-build snapshot + bump for esp-coordinator (ESP-IDF).
#
# Reihenfolge nach globaler CLAUDE.md-Regel:
#   1. Wenn working tree dirty: `git add -A; git commit -m "build snapshot v<PREV>"`.
#      Der Commit repräsentiert den Stand der gerade gebaut wird (= Rollback-Ziel).
#   2. build_number.txt += 1.
#   3. version.txt neu schreiben als "<MAJOR>.<MINOR>.<BUILD>".
#      ESP-IDF liest version.txt automatisch und setzt esp_app_desc_t::version
#      (OTA-Image-Header). main/version.h.in wird in main/CMakeLists.txt via
#      configure_file aus version.txt + build_number.txt regeneriert.
#
# MAJOR/MINOR werden aus dem aktuellen version.txt geparst — bei Phasenwechsel
# einfach version.txt manuell auf "2.0.<n>" o.ä. setzen. BUILD ist immer der
# Counter aus build_number.txt.
#
# Wird vom idf.py-Wrapper in idf_env.sh vor build/flash/app/app-flash/all
# aufgerufen.

set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION_FILE="$DIR/version.txt"
BUILD_NUM_FILE="$DIR/build_number.txt"

# --- MAJOR / MINOR aus version.txt parsen (fallback 1.1) -------------------
if [ -f "$VERSION_FILE" ]; then
    cur="$(tr -d '[:space:]' < "$VERSION_FILE")"
    MAJOR="${cur%%.*}"
    rest="${cur#*.}"
    MINOR="${rest%%.*}"
fi
MAJOR="${MAJOR:-1}"
MINOR="${MINOR:-1}"

# --- Build-Counter laden ---------------------------------------------------
if [ -f "$BUILD_NUM_FILE" ]; then
    prev_n="$(tr -d '[:space:]' < "$BUILD_NUM_FILE")"
    [ -z "$prev_n" ] && prev_n=0
else
    prev_n=0
fi

# --- Schritt 1: Snapshot vor Bump (label = PREVIOUS version) ---------------
if git -C "$DIR" rev-parse --git-dir >/dev/null 2>&1; then
    REPO_ROOT="$(git -C "$DIR" rev-parse --show-toplevel)"
    if ! git -C "$REPO_ROOT" diff --quiet || ! git -C "$REPO_ROOT" diff --cached --quiet; then
        git -C "$REPO_ROOT" add -A >/dev/null
        if git -C "$REPO_ROOT" commit -m "build snapshot v${MAJOR}.${MINOR}.${prev_n}" >/dev/null 2>&1; then
            echo "[bump] snapshot committed: build snapshot v${MAJOR}.${MINOR}.${prev_n}"
        fi
    fi
fi

# --- Schritt 2 + 3: Counter und version.txt schreiben ----------------------
n=$((prev_n + 1))
echo "$n" > "$BUILD_NUM_FILE"
echo "${MAJOR}.${MINOR}.${n}" > "$VERSION_FILE"

echo "[bump] esp-coordinator v${MAJOR}.${MINOR}.${n}  ($(date -Iseconds))"
