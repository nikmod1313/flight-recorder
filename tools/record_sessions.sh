#!/bin/sh
# Records back-to-back live sessions of Real Ravi, each to its own log, and checks
# that each one replays. A session whose feed dies is kept: its log is torn, and
# everything before the tear still replays. The next session starts anyway.
#
#   tools/record_sessions.sh <hours> <minutes per session> <out dir>
#
# Run it under caffeinate so the Mac doesn't sleep (closing the lid still does):
#   nohup caffeinate -i tools/record_sessions.sh 40 60 build/lb > build/lb.out 2>&1 &
# Stop it with:  pkill -f record_sessions.sh; pkill -f live_feed.py
set -u
hours=$1
minutes=$2
out=$3
mkdir -p "$out"
end=$(( $(date +%s) + hours * 3600 ))

while [ "$(date +%s)" -lt "$end" ]; do
    name="$out/$(date +%Y%m%d-%H%M%S)"
    python3 tools/live_feed.py --symbol btcusdt --seconds $(( minutes * 60 )) 2>"$name.err" |
        build/fr_tool record-stdin ravi "$name.log" >"$name.out" 2>&1
    check=$(build/fr_tool replay ravi "$name.log" 0 2>&1 | head -1)
    echo "$(date '+%F %T') $name.log $(tail -1 "$name.err") | $check" >>"$out/index.txt"
    sleep 5  # a moment before reconnecting, so a dead network isn't hammered
done
