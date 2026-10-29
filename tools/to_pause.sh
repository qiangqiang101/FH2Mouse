#!/bin/sh
# Restart the game with the freshly built FH2Mouse.dll and open the pause menu (needs pad.py serve running).
root=/c/Users/ADMIN/Documents/GitHub/FH2Mouse
game=/d/Games/FH2/azimuth-windows
taskkill //IM azimuth.exe //F >/dev/null 2>&1; sleep 3
cp "$root/build/FH2Mouse.dll" "$game/fh2-mods/" || exit 1
cd "$game" && (./azimuth.exe --skip-launcher >/dev/null 2>&1 &)
for i in $(seq 1 40); do grep -q "FH2Mouse\] watching" "$game/fh2-mods/platform.log" 2>/dev/null && break; sleep 2; done
sleep 15
cd "$root/tools"
python drive.py "focus; move 900 500; move 1000 600; click" >/dev/null
n=0; until python -W ignore -c "
from PIL import ImageGrab
import sys
im=ImageGrab.grab().convert('L'); px=im.crop((1500,760,1860,1000))
sys.exit(0 if sum(px.getdata())/(360*240)>110 else 1)" || [ $n -ge 20 ]; do n=$((n+1)); sleep 5; done
python pad.py press START 150 >/dev/null; sleep 3
