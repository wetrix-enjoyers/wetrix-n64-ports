#!/bin/bash
# Wetrix PortMaster launcher (R36S / aarch64; the hard GLES renderer by default).
#
# Layout under /roms/ports (or /roms2/ports):
#   Wetrix.sh               this file (ES entry point)
#   wetrix/                 game directory (GAMEDIR)
#     wetrix.aarch64        the recompiled game
#     wetrix-watch          Select+Start exit watcher
#     wetrix.z64            the ROM image, adopted on first launch (never shipped)
#     log.txt               last session log
#
# The ROM: drop your US Wetrix dump (.z64/.n64/.v64, any name) into wetrix/ or
# wetrix/data/. The game checks it against the hash it was recompiled from and
# adopts it as wetrix.z64 on first launch; the dump itself is not deleted.

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

source "$controlfolder/control.txt"
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

# Relative to this script, not PortMaster's $directory: ES may run the port
# from /roms2/ports while control.txt says "roms".
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GAMEDIR="$SCRIPT_DIR/wetrix"
GAME="$GAMEDIR/wetrix.aarch64"
WATCH="$GAMEDIR/wetrix-watch"

mkdir -p "$GAMEDIR/data"
cd "$GAMEDIR" || exit 1

> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

echo "=== Wetrix (recomp) ==="
date
echo "GAMEDIR=$GAMEDIR CFW=${CFW_NAME:-unknown}"

chmod +x "$GAME" "$WATCH" 2>/dev/null || true

# Device settings for the port (see wetrix-n64-ports/pc/include/render_select.h and
# wetrix-n64-ports/pc/include/debug_server.h in the source tree).
export SDL_VIDEO_DRIVER=kmsdrm
# Controller mapping. PortMaster's ($sdl_controllerconfig) comes first when it has
# one; ours follows it, and a later line for the same GUID wins. Ours is for the
# R36S's "GO-Super Gamepad", measured on the device: SDL's built-in
# entry for it has no start or back at all (Start is raw button 13, Select 12, so
# neither the game nor wetrix-watch could see them) and puts the labelled A on
# SDL's "b" and the labelled B on "a", the reverse of the N64 layout the game maps.
# Both the game and the watcher inherit this, so they agree.
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig
1900bb3e4b4800000011000000010000,GO-Super Gamepad,a:b1,b:b0,x:b2,y:b3,back:b12,start:b13,leftshoulder:b4,rightshoulder:b5,dpup:b8,dpdown:b9,dpleft:b10,dpright:b11,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:b6,righttrigger:b7,platform:Linux,"
# wetrix/renderer.txt, if present, names the renderer (hard, fast3d or soft).
RENDERER=hard
[ -f "$GAMEDIR/renderer.txt" ] && RENDERER="$(tr -d '[:space:]' < "$GAMEDIR/renderer.txt")"
echo "renderer: $RENDERER"
export WETRIX_RENDERER="$RENDERER"
export WETTER_RENDER_TIME=1                        # soft, hard: ms per display list into log.txt
export WETRIX_PACE_LOG="${WETRIX_PACE_LOG:-1}"     # task and present timing into log.txt
export WETRIX_FULLSCREEN=1
export WETRIX_F3D_SCALE="${WETRIX_F3D_SCALE:-2}"   # 640x480 panel = 2x native
export WETRIX_HARD_SCALE="${WETRIX_HARD_SCALE:-2}" # the same for hard
export WETRIX_DEBUG_PORT="${WETRIX_DEBUG_PORT:-0}" # debug console off on device
export WETRIX_SNAPSHOT=0                           # no per-frame RDRAM copies

echo "[Launch] starting wetrix.aarch64"
"$GAME" &
GAME_PID=$!
"$WATCH" "$GAME_PID" &
WATCH_PID=$!
wait "$GAME_PID"
GAME_RC=$?
kill "$WATCH_PID" 2>/dev/null || true
echo "[Exit] rc=$GAME_RC"

type pm_finish >/dev/null 2>&1 && pm_finish
exit "$GAME_RC"
