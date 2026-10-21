#!/usr/bin/env bash
# One PGR4 race run of a Windows build tree under Proton (vkd3d-proton) on a Linux desktop (written on Hyprland),
# the Linux counterpart of ../lab/co_run.ps1 -Race. Reads the tree straight from the NTFS C:
# mount, keeps its own prefix and user data under ~/pgr4-proton (a fresh save every run, the
# vkd3d and pipeline caches kept), drives the St Petersburg route with route_player.py, holds
# HOLD seconds after the race mark, stops the game and prints the frame rate over the 40 s
# after the mark (perf_fps.py).
#
#   run.sh <tree> <tag> [--mode primary|none|oracle] [--extra "<flags>"] [--uncapped] [--hold 45]
#          [--height 720] [--cpus 16-23] [--tap]
# --tap turns on the push-buffer decoder (--pgr4_d3d_tap); off by default, as on Windows, because
# it costs the native path ~0.8 ms per frame. The 2026-10-21 Proton series ran with it on.
# --cpus pins the game (and the wineserver it starts) to those CPUs with taskset, e.g. eight
# E-cores of the i9-13900K as a rough stand-in for a slower 8-thread CPU.
# --mode oracle runs Xenos and native side by side (--pgr4_native, debug window) and captures
# both front buffers 5, 10 and 15 s after the race mark into ~/pgr4-proton/captures-<tag>/
# (PNGs and captures.csv with PSNR), the image check of the Windows race oracle.
# Run it from an SSH session as the desktop user; the game starts inside the Hyprland session
# through `hyprctl dispatch exec`. Logs: ~/pgr4-proton/logs/pgr4-proton-<tag>-<stamp>.log and
# pgr4-co-input-<tag>.log (same line formats as the Windows runs).
set -euo pipefail
tree=${1:?tree}; tag=${2:?tag}; shift 2
mode=primary; extra=""; uncapped=0; hold=45; height=720; cpus=""; tap=0
while [[ $# -gt 0 ]]; do
  case $1 in
    --mode) mode=$2; shift 2 ;;
    --extra) extra=$2; shift 2 ;;
    --uncapped) uncapped=1; shift ;;
    --hold) hold=$2; shift 2 ;;
    --height) height=$2; shift 2 ;;
    --cpus) cpus=$2; shift 2 ;;
    --tap) tap=1; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
here=$(cd "$(dirname "$0")" && pwd)
c=${PGR4_C:-/mnt/windows-c}
runtime=$c/recomp/builds/pgr4/native-$tree/run/runtime
game=$c/recomp/dumps/pgr4/game
keys=$c/recomp/builds/pgr4/lab-scripts/run/route-stpetersburg.keys
tools=$c/recomp/builds/pgr4/lab-scripts/perf
top=$HOME/pgr4-proton
proton=${PROTON:-$(ls -d "$top"/proton/GE-Proton*/ 2>/dev/null | sort -V | tail -1)}
proton=${proton%/}
[[ -x $proton/proton ]] || { echo "no Proton at '$proton' (set PROTON=<dir with ./proton>)" >&2; exit 1; }
[[ -f $runtime/pgr4_recompiled.exe ]] || { echo "no exe in $runtime" >&2; exit 1; }
pgrep -f 'pgr4_recompiled\.exe' >/dev/null && { echo "a pgr4_recompiled.exe is already running" >&2; exit 1; }
[[ -w /dev/uinput ]] || { echo "/dev/uinput is not writable: sudo setfacl -m u:$USER:rw /dev/uinput" >&2; exit 1; }

mkdir -p "$top/logs" "$top/prefix" "$top/userdata"
find "$top/userdata" -mindepth 1 -maxdepth 1 ! -name cache -exec rm -rf {} +   # fresh save, keep caches
stamp=$(date +%Y%m%d-%H%M%S)
base=$top/logs/pgr4-proton-$tag-$stamp
ilog=$top/logs/pgr4-co-input-$tag.log
rm -f "$ilog"
win() { echo "Z:${1//\//\\}"; }   # Proton maps Z: to /

args=(--game_data_root "$(win "$game")" --user_data_root "$(win "$top/userdata")"
  --execute_unclipped_draw_vs_on_cpu --gpu_plugin=xenos --d3d12_present_vsync --xmp_music_gain 0.35
  --d3d12_texture_shared_heaps --gpu_vblank_deadline_timer --gpu_wait_reg_mem_high_res
  --no-clear_memory_page_state --gpu_ring_wake_fast --texture_resource_reuse
  --texture_reuse_pool_limit_mib=256 --texture_cache_memory_limit_soft_lifetime=30
  --fullscreen=false --window_width=1280 "--window_height=$height" --mnk_mode
  --guest_frame_stats --pgr4_d3d_stats_frames=150 --log_file "$(win "$base.log")")
[[ $tap == 1 ]] && args+=(--pgr4_d3d_tap)
[[ $mode == primary ]] && args+=(--pgr4_native --pgr4_native_primary)
capdir=$top/captures-$tag
if [[ $mode == oracle ]]; then
  rm -rf "$capdir"; mkdir -p "$capdir"
  args+=(--pgr4_native --readback_resolve=fast "--pgr4_native_capture_dir=$(win "$capdir")")
fi
[[ $uncapped == 1 ]] && args+=(--vsync=false --d3d12_present_vsync=false)
# shellcheck disable=SC2206
[[ -n $extra ]] && args+=($extra)

# The game runs inside the desktop session; its command line goes through a file so that
# hyprctl's own argument parsing never sees it.
launcher=$top/logs/launch-$tag.sh
{
  echo '#!/usr/bin/env bash'
  echo "export STEAM_COMPAT_DATA_PATH='$top/prefix' STEAM_COMPAT_CLIENT_INSTALL_PATH='$top/steam'"
  echo "export PROTON_LOG=1 PROTON_LOG_DIR='$top/logs' DXVK_LOG_LEVEL=warn VKD3D_DEBUG=warn"
  echo "cd '$runtime'"
  printf 'exec %s%q run %q' "${cpus:+taskset -c $cpus }" "$proton/proton" "$runtime/pgr4_recompiled.exe"
  printf ' %q' "${args[@]}"
  echo " > '$base-stdout.log' 2>&1"
} > "$launcher"
chmod +x "$launcher"
mkdir -p "$top/steam"

export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
sig=$(ls -t "$XDG_RUNTIME_DIR"/hypr/ | head -1)
export HYPRLAND_INSTANCE_SIGNATURE=$sig
cleanup() {
  pkill -f 'pgr4_recompiled\.exe' 2>/dev/null || true
  sleep 3
  pkill -9 -f 'pgr4_recompiled\.exe' 2>/dev/null || true
  [[ -n ${player:-} ]] && kill "$player" 2>/dev/null || true
  echo "stopped $(date +%T)"
  echo "log: $base.log"
  echo "input: $ilog"
}
trap cleanup EXIT
# Hyprland 0.56 with a Lua config: dispatchers are Lua expressions.
out=$(hyprctl dispatch "hl.dsp.exec_cmd(\"$launcher\")" 2>&1) || true
[[ $out == ok ]] || { echo "hyprctl exec failed: $out" >&2; exit 1; }
echo "game launched $(date +%T) ($mode, $tree)"
python3 "$here/route_player.py" --script "$keys" --log "$ilog" --timeout 180 --height "$height" &
player=$!

deadline=$((SECONDS + 300)); ok=0
while (( SECONDS < deadline )); do
  sleep 2
  if grep -q '^\S\+ mark race' "$ilog" 2>/dev/null; then ok=1; break; fi
  if grep -q 'error' "$ilog" 2>/dev/null; then break; fi
  pgrep -f 'pgr4_recompiled\.exe' >/dev/null || { echo "GAME EXITED $(date +%T)"; break; }
done
echo "route reached mark race: $ok $(date +%T)"
[[ $ok == 1 ]] || { tail -5 "$ilog" 2>/dev/null; exit 1; }
if [[ $mode == oracle ]]; then
  # The game takes one capture per request file at its next Swap and deletes the file.
  for _ in 1 2 3; do sleep 5; : > "$capdir/capture.request"; done
  sleep $((hold > 15 ? hold - 15 : 0))
  echo "captures:"; cat "$capdir/captures.csv" 2>/dev/null || echo "  none"
else
  sleep "$hold"
fi
alive=0; pgrep -f 'pgr4_recompiled\.exe' >/dev/null && alive=1
echo "alive at end: $alive $(date +%T)"
mark=$(grep -m1 ' mark race' "$ilog" | cut -c12-19)
python3 "$tools/perf_fps.py" "$base.log" "$mark" 40 | head -1 || true
# For ab_compare.py: side,log,mark rows are built from these lines.
echo "RESULT $tag $base.log $(grep -m1 ' mark race' "$ilog" | cut -d' ' -f1) $alive"
