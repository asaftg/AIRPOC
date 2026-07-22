# Deploying the console to the Jetson

The cloud repo is the only write target. You commit and push from wherever you author, the
Jetson **pulls**, and nothing is ever hand-edited on the device. This page is the procedure
plus the three traps that have actually bitten.

## The procedure

```bash
# on the Jetson, in the checkout
git rev-parse --short HEAD                    # BEFORE
git fetch origin && git reset --hard origin/main
git rev-parse --short HEAD                    # AFTER  — must have moved

cd app
rm -f web_assets.h && make                    # see trap 1

pkill -x app                                  # or: pkill -f "[.]/app -p 8080"
setsid nohup ./app -p 8080 -e 127.0.0.1:8091 -r 127.0.0.1:8092 \
      -c 127.0.0.1:8093 -d 127.0.0.1:8094 -t 127.0.0.1:8095 -u 127.0.0.1:8096 \
      >/tmp/app.log 2>&1 </dev/null &
```

In normal field use the [launcher](../launcher/README.md) (`:8088`) starts and stops the whole
stack for you; the line above is what it runs for the console.

## Trap 1 — `web_assets.h` is not rebuilt by a source change

`make` rebuilds `web_assets.h` only when a `web/` file is **newer** than it. A `git reset
--hard` writes checkout timestamps that can land the wrong side of that comparison, so the
binary silently keeps the OLD page and you debug a UI that was never deployed. **Always
`rm -f web_assets.h` before `make`.**

## Trap 2 — a failed fetch deploys the old commit silently

With no route to GitHub (field AP, DNS down), `git fetch` fails, `git reset --hard
origin/main` succeeds against the **stale** `origin/main` you already had, and the build
"works". Nothing errors. **Echo the commit before and after** and confirm it moved — that is
the only check that catches this.

## Trap 3 — verify the EFFECT, not your edit

A restarted binary is not a deployed change. Check the thing itself:

```bash
curl -s localhost:8080/app.js | grep -c "<a string only the new code has>"
curl -s localhost:8080/fstats                 # is the new feed actually connected?
curl -s "localhost:8080/ctl?fus_gate=1.2"     # 200 proves ROUTING, not that the knob applied
curl -s localhost:8096/stats                  # read the daemon's own knobs back
```

A `/ctl` reply of `ok` only means the console accepted and forwarded the request. Whether the
daemon honoured it is only visible in that daemon's `/stats`.

## Shared bench

Other agents work on the same box and the same checkout. `git reset --hard` there will
discard someone else's uncommitted work, and a `reset` run against your in-flight push has
already lost a commit once. Ask before touching the Jetson, and stage + commit + push in one
step rather than leaving work sitting in the device's working tree.
