#!/usr/bin/env bash
# Runs vmstat, iostat -x and top in the background, one timestamped line per output line,
# while a command (or an idle wait) runs in the foreground.
#
#   ./monitor.sh <outdir> <interval_s> idle <seconds>        idle baseline
#   ./monitor.sh <outdir> <interval_s> -- <command...>       monitor a workload
#
# Best: ONE invocation, so idle baseline and workloads share a single timeline. The workload
# records its own idle interval (idle_secs=) and per-workload markers; pass the same out=:
#   ./monitor.sh mon 1 -- ./workload phase=char reps=0 idle_secs=120 char_secs=60 gap_secs=20 \
#                         file=/data/workload.dat out=mon
#   python3 analyze_system.py mon
#
# A separate idle-only baseline needs its own directory (each run starts fresh logs):
#   ./monitor.sh mon_idle 1 idle 120 && python3 analyze_system.py mon_idle
set -euo pipefail

out=${1:?usage: monitor.sh <outdir> <interval_s> (idle <seconds> | -- <command...>)}
interval=${2:?missing interval}
mode=${3:?missing 'idle <seconds>' or '-- <command...>'}
shift 3

command -v perl   >/dev/null || { echo "perl is needed for timestamps" >&2; exit 1; }
command -v iostat >/dev/null || { echo "iostat not found (install the sysstat package)" >&2; exit 1; }
mkdir -p "$out"
if [[ -e "$out/vmstat.log" ]]; then
  echo "$out already holds logs; use a fresh directory (a second run would overwrite them)" >&2
  exit 1
fi

{
  echo "date:      $(date -Is)"
  echo "kernel:    $(uname -r)"
  echo "cpus:      $(nproc)"
  echo "cpu:       $(lscpu | sed -n 's/^Model name:[[:space:]]*//p')"
  echo "governor:  $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo n/a)"
  lsblk -d -o NAME,ROTA,SIZE,MODEL 2>/dev/null || true
} > "$out/meta.txt"

# One long-lived process stamps every line with sub-second epoch time (no fork per line).
stamp='BEGIN{$|=1} printf "%.3f %s", time, $_'
# top prints every process; keep the header plus the 8 busiest processes per refresh.
top_keep='BEGIN{$|=1} $n=0 if /^top - /; $n++; printf "%.3f %s", time, $_ if $n<=17'

cleanup() { pkill -P $$ 2>/dev/null || true; }
trap cleanup EXIT INT TERM

stdbuf -oL vmstat "$interval"       | perl -MTime::HiRes=time -ne "$stamp"    > "$out/vmstat.log" &
stdbuf -oL iostat -x "$interval"    | perl -MTime::HiRes=time -ne "$stamp"    > "$out/iostat.log" &
stdbuf -oL top -b -d "$interval"    | perl -MTime::HiRes=time -ne "$top_keep" > "$out/top.log" &

sleep "$interval"   # let the samplers print their first (since-boot) report; analysis drops it

if [[ "$mode" == idle ]]; then
  secs=${1:?idle needs <seconds>}
  t0=$(date +%s.%N); sleep "$secs"; t1=$(date +%s.%N)
  [[ -s "$out/markers.csv" ]] || echo "phase,label,rho,rep,t_start,t_end" > "$out/markers.csv"
  echo "char,idle,0,0,$t0,$t1" >> "$out/markers.csv"
elif [[ "$mode" == -- ]]; then
  "$@"
else
  echo "mode must be 'idle <seconds>' or '-- <command...>'" >&2; exit 1
fi

sleep "$interval"   # capture the final interval
