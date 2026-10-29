#!/bin/sh
# Rebuild, reinstall FH2Mouse.dll and restart the game straight into it; waits for the title screen.
set -e
root=/c/Users/ADMIN/Documents/GitHub/FH2Mouse
game=/d/Games/FH2/azimuth-windows
cmake --build "$root/build" 2>&1 | grep -E "error|FAILED" && exit 1
taskkill //IM azimuth.exe //F >/dev/null 2>&1 || true
sleep 3
cp "$root/build/FH2Mouse.dll" "$game/fh2-mods/FH2Mouse.dll"
cd "$game" && (./azimuth.exe --skip-launcher >/dev/null 2>&1 &)
for i in $(seq 1 40); do grep -q "FH2Mouse\] \(watching\|stopped\|couldn\)" "$game/fh2-mods/platform.log" 2>/dev/null && break; sleep 2; done
sleep ${1:-15}
grep -a "FH2Mouse" "$game/fh2-mods/platform.log" | tail -5
