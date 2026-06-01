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

# --- MAJOR / MINOR aus version.txt parsen ----------------------------------
# BUILD-3: validieren BEVOR geparst wird. version.txt liegt auf NFS; ein
# transienter Read kann eine leere / NUL- / abgeschnittene Datei liefern. Die
# alte Logik fiel dann still auf 1.1 zurück und überschrieb version.txt mit
# "1.1.<n>" — die echte MAJOR.MINOR-Phase (z.B. 1.2) ging verloren, der Counter
# lief mit falscher Version weiter. Stattdessen laut abbrechen, statt eine
# degradierte Version festzuschreiben. Akzeptiert MAJOR.MINOR oder
# MAJOR.MINOR.BUILD (Hand-Edit bei Phasenwechsel), lehnt alles andere ab.
if [ -f "$VERSION_FILE" ]; then
    cur="$(tr -d '[:space:]' < "$VERSION_FILE")"
    if ! printf '%s' "$cur" | grep -Eq '^[0-9]+\.[0-9]+(\.[0-9]+)?$'; then
        echo "[bump] ERROR: version.txt unlesbar/korrupt: '$cur'" >&2
        echo "[bump] verweigere Überschreiben mit degradierter Version. version.txt reparieren und erneut bauen." >&2
        exit 1
    fi
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
# BUILD-1: die Version-Files liegen auf einem NFS-Mount; ein blankes `echo >`
# flusht nur in den Client-Cache, sodass ein Leser (CMake configure_file, ein
# zweiter Build-Step) die Datei NUL-präfixiert / halb geschrieben sieht ->
# Versions-Counter Schrott. Atomar schreiben mit fsync + atomarem Rename
# (_atomic_write_bytes-Pattern aus der globalen Regel). python3 ist im
# gesourcten ESP-IDF-Env auf PATH; bei Fehlen bricht der Hook laut ab (set -e),
# statt eine kaputte Datei zu hinterlassen.
_atomic_write() {
    python3 - "$1" "$2" <<'PY'
import os, sys
path, content = sys.argv[1], sys.argv[2]
data = (content + "\n").encode()
tmp = path + ".tmp"
with open(tmp, "wb") as f:
    f.write(data); f.flush(); os.fsync(f.fileno())
os.replace(tmp, path)                       # atomar; frische Inode umgeht stale page-cache
dfd = os.open(os.path.dirname(path) or ".", os.O_RDONLY)
try:
    os.fsync(dfd)                           # committet den Rename
finally:
    os.close(dfd)
PY
}

n=$((prev_n + 1))
_atomic_write "$BUILD_NUM_FILE" "$n"
_atomic_write "$VERSION_FILE" "${MAJOR}.${MINOR}.${n}"

echo "[bump] esp-coordinator v${MAJOR}.${MINOR}.${n}  ($(date -Iseconds))"
