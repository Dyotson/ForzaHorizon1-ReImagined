#!/usr/bin/env bash
# Decompile guest functions with a headless Ghidra kept running (ghidra_projects/ForzaHorizon,
# read-only; Ghidra 12.1.4 in ~/ghidra_12.1.4_PUBLIC, Java 21 from Homebrew).
#   tools/ghidra_decomp.sh start          # start the server (about a minute to load)
#   tools/ghidra_decomp.sh 8287BBE0 ...   # decompile; prints the .c paths (waits up to 5 min)
#   tools/ghidra_decomp.sh stop
# The function containing an address is decompiled (one is created at the address if Ghidra
# has none). Files go to $GHIDRA_DECOMP_DIR (default /tmp/forza-decomp-$USER).
# GHIDRA_PROJECT=ForzaHorizonTU4 uses the Title Update 4 project (TU4 addresses).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR=${GHIDRA_DECOMP_DIR:-/tmp/forza-decomp-$USER}
GHIDRA=${GHIDRA_HOME:-$HOME/ghidra_12.1.4_PUBLIC}
mkdir -p "$DIR/out"
case "${1:-}" in
  start)
    : > "$DIR/list"
    export PATH=/home/linuxbrew/.linuxbrew/opt/openjdk@21/bin:$PATH
    export JAVA_HOME=/home/linuxbrew/.linuxbrew/opt/openjdk@21/libexec
    DECOMP_LIST=$DIR/list DECOMP_OUT=$DIR/out MAXMEM=4G nohup timeout 7200 \
      "$GHIDRA/support/analyzeHeadless" "$ROOT/ghidra_projects" "${GHIDRA_PROJECT:-ForzaHorizon}" -process default.xex \
      -noanalysis -readOnly -scriptPath "$ROOT/tools/ghidra" -postScript Decomp.java \
      > "$DIR/headless.log" 2>&1 &
    echo $! > "$DIR/pid"; echo "started $(cat "$DIR/pid"), log $DIR/headless.log" ;;
  stop)
    echo quit >> "$DIR/list" ;;
  "") sed -n 2,7p "$0" ;;
  *)
    for a in "$@"; do a=${a#0x}; a=${a^^}; echo "$a" >> "$DIR/list"; done
    for a in "$@"; do
      a=${a#0x}; a=${a^^}; t=0
      while [ ! -f "$DIR/out/$a.c" ] && [ $t -lt 600 ]; do sleep 0.5; t=$((t + 1)); done
      echo "$DIR/out/$a.c"
    done ;;
esac
