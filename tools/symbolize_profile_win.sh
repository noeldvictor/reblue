#!/usr/bin/env bash
# Symbolise a desktop bd_sample_profiler dump against reblue_vk.exe's PDB and
# print the hottest functions. The dump's PCs are offsets from the module base
# (RVAs), which llvm-symbolizer takes with --relative-address.
#
#   bash tools/symbolize_profile_win.sh [profile.txt] [top]
#
# Defaults: the newest logs/guest_profile*.txt of the win-amd64-release build,
# top 30. The profile covers the guest's own threads (the Draw Thread carries
# the frame; the IO and loader threads show up as sub_8272BE80/sub_8217AE90
# and are not the frame), 1 kHz, the 16 s before the capture. "dropped" are
# PCs outside our module: the driver and the runtime.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)/out/build/win-amd64-release"
PROFILE="${1:-$(ls -t "$ROOT"/logs/guest_profile*.txt | head -1)}"
TOP="${2:-30}"
export PATH="/c/Program Files/LLVM/bin:$PATH"
TMP="$ROOT/logs/symbolize_tmp"
mkdir -p "$TMP"
grep "^# samples" "$PROFILE"
awk '/^# SAMPLES/{f=1;next} f&&NF==2{print $1, $2}' "$PROFILE" | head -800 > "$TMP/pcs.txt"
awk '{print "0x"$1}' "$TMP/pcs.txt" \
  | llvm-symbolizer --obj="$ROOT/reblue_vk.exe" --relative-address --no-inlines 2>/dev/null \
  | awk 'NR%3==1' > "$TMP/syms.txt"
paste -d'\t' <(awk '{print $2}' "$TMP/pcs.txt") "$TMP/syms.txt" \
  | awk -F'\t' '{c[$2]+=$1} END{for(k in c) print c[k]"\t"k}' \
  | sort -rn | head -"$TOP" | cut -c1-150
