"""Builds the demo screen's data from the recorded days.

    python3 demo/build_data.py            (run from quant/, after `make`)

Reads every build/ravi*.log, runs the postmortem and a few forks on each, and
writes demo/data.json (for any UI) and demo/data.js (so index.html opens
straight from disk, with no server). Every number comes from a replay that
matched the recording, or from the verified part of a cut-off one.
"""
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(ROOT, "build")


def day_number(name):
    """ravi.log is day 1, ravi2.log day 2, and so on."""
    match = re.fullmatch(r"ravi(\d*)\.log", name)
    return int(match.group(1) or 1) if match else None


def run(*args):
    result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        sys.exit(f"{' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout


def feed_summary(day):
    """The live feed's own last line: how many events, how many missed, the busiest second."""
    path = os.path.join(BUILD, "live_feed.err" if day == 1 else f"live_feed{day}.err")
    if not os.path.exists(path):
        return {"status": "unknown"}
    line = [l for l in open(path).read().splitlines() if l.startswith("feed")][-1]
    ok = re.match(r"feed: (\d+) events, (\d+) missed, busiest second (\d+)", line)
    if ok:
        return {"status": "ok", "missed": int(ok.group(2)), "busiest_second": int(ok.group(3))}
    return {"status": "died", "note": line.removeprefix("feed ")}


def fork(log, fork_at, *changes):
    """Runs one fork and returns its report and both days, parsed from fr_tool's output."""
    out = run("build/fr_tool", "fork", "ravi", log, str(fork_at), "0", *changes)
    head = dict(re.findall(r"(\w+)=(\S+)", out.splitlines()[0]))
    days = {}
    for line in out.splitlines()[1:]:
        name, rest = line.split(" ", 1)
        days[name] = {k: float(v) for k, v in re.findall(r"(\w+)=(\S+)", rest)}
    return {
        "status": head["status"],
        "fork_at": int(head["fork_at"]),
        "first_difference": int(head["first_difference"]),
        "different_decisions": int(head["different"]),
        "change": " ".join(changes) or "no change",
        "original": days.get("original"),
        "forked": days.get("forked"),
    }


def main():
    logs = sorted((n for n in os.listdir(BUILD) if day_number(n)), key=day_number)
    if not logs:
        sys.exit("no build/ravi*.log files: record a day first")
    paths = [os.path.join("build", n) for n in logs]

    tmp = os.path.join(BUILD, "postmortem.json")
    run("build/postmortem", "--json", tmp, *paths)
    data = json.load(open(tmp))

    for day, path in zip(data["days"], paths):
        n = day["day"]
        ended = os.path.getmtime(os.path.join(ROOT, path))
        day["started"] = time.strftime("%Y-%m-%d %H:%M", time.localtime(ended - day["minutes"] * 60))
        day["feed"] = feed_summary(n)

    # What-ifs: from the middle of each day, what if Ravi had leaned $3 instead of $1?
    data["forks"] = []
    for day, path in zip(data["days"], paths):
        result = fork(path, day["events"] // 2, "lean_cents=300")
        result["day"] = day["day"]
        data["forks"].append(result)
    # And the safety check: a fork with no change must never decide differently.
    check = fork(paths[0], data["days"][0]["events"] // 2)
    check["day"] = data["days"][0]["day"]
    data["no_change_check"] = check

    data["built"] = time.strftime("%Y-%m-%d %H:%M")
    text = json.dumps(data, separators=(",", ":"))
    open(os.path.join(HERE, "data.json"), "w").write(text)
    open(os.path.join(HERE, "data.js"), "w").write("window.FR_DATA = " + text + ";\n")
    print(f"{len(data['days'])} days, {len(data['fills'])} fills, {len(data['forks'])} forks -> demo/data.json, demo/data.js")


if __name__ == "__main__":
    main()
