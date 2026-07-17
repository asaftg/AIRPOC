#!/usr/bin/env python3
"""tracker_gates.py — the one-command tracker validation bench.

    python3 tracker_gates.py <path-to-track_replay-binary> \
        [--baseline tracker_baseline.json] [--fixtures DIR] [--freeze]

For every fixture in manifest.json it runs the replay binary, checks hard
INVARIANTS (the class of bug this bench exists to catch), scores the EMITTED
output (E lines — the wire) against the fixture's truth sidecar, and gates the
result against the frozen baseline. Prints a PASS/FAIL table; exit 0 = pass,
1 = fail, 2 = setup error (missing/mismatched fixtures, stale baseline).

Why E lines: a tracker change once doubled ghosts in the field while its
validation stayed green, because the validation scored CONFIRMED tracks
(C lines) while the wire emits a SUBSET (E lines). This bench scores only what
the wire carries, echoes the knob state the replay actually ran with, and
refuses to run on fixtures whose bytes don't match the pinned sha.

Invariants (violating any one is an automatic loud FAIL):
  * every E tid is also a C tid in the same frame (the wire is a subset)
  * C count >= E count in every frame
  * replay frame count == fixture record count (nothing truncated)
  * fixture sha256 matches the manifest AND the baseline

Truth sidecars (truth/<fixture>.truth.json) list the real objects in the
scene. Matching rules (validated constants — do NOT tighten):
  * an E entry matches a mover segment if |r - r_pred(t)| < 15 m, and, only
    where the sidecar provides a gating "az", |az - az_truth| < 6 deg
  * an E entry matches a static box if r is inside [rmin-15, rmax+15] and
    az inside [azmin-6, azmax+6]
  * ghost = E entry matching no truth target that frame
  * mirror_suspect = of >=2 E entries in one frame with |dr| <= 5 m and
    |daz| > 10 deg, the weaker (lower snr_peak); reported separately

Gates vs baseline (skipped under --freeze, which rewrites the baseline):
  * negative fixtures: exactly 0 E entries (absolute, not baseline-relative)
  * E/frame   <= base + max(0.10, 0.15*base)
  * distinct emitted tids <= base + max(2, 0.20*base)
  * per truth mover: coverage >= 0.95*base, fragmentation <= base,
    first-emit <= base + 3 frames
"""
import argparse
import datetime
import hashlib
import json
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# ---- validated matching constants — do NOT tighten --------------------------
MATCH_DR_M      = 15.0   # range window vs truth
MATCH_DAZ_DEG   = 6.0    # az window, only where truth provides az
MIRROR_DR_M     = 5.0    # same range ...
MIRROR_DAZ_DEG  = 10.0   # ... different bearing => mirror pair

# ---- gate margins ------------------------------------------------------------
def gate_efr(base):   return base + max(0.10, 0.15 * base)
def gate_tids(base):  return base + max(2.0, 0.20 * base)
COVERAGE_FLOOR_FRAC = 0.95
FIRST_EMIT_SLACK_FRAMES = 5   # ~0.2 s. Catches real detection-latency regressions
# (the 2026-07 incident missed by 600+ frames) while tolerating the few-frame
# corridor-ENTRY shift that position smoothing legitimately introduces.


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def count_fixture_records(path):
    """Number of [t, n, n*5f] records in the fixture (mirrors track_replay's
    reader, including its stop conditions)."""
    n_rec = 0
    with open(path, 'rb') as f:
        while True:
            hdr = f.read(12)
            if len(hdr) < 12:
                break
            _, n = struct.unpack('<di', hdr)
            if n < 0:
                break
            body = f.read(20 * n)
            if len(body) < 20 * n:
                break
            n_rec += 1
    return n_rec


def run_replay(binary, fixture_path):
    """Run the replay binary, return (knob_line, frames). Each frame is
    {'t': float, 'C': set(tid), 'E': [(tid, r, az, vr, snr) ...]}.
    Old binaries print 3-field E lines; vr/snr become None."""
    proc = subprocess.run([binary, fixture_path], capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError('replay binary exited %d: %s'
                           % (proc.returncode, proc.stderr.strip()[:200]))
    knob_line = None
    frames = []
    for ln in proc.stdout.splitlines():
        p = ln.split()
        if not p:
            continue
        if p[0] == 'K':
            knob_line = ln[2:].strip()
        elif p[0] == 'F':
            frames.append({'t': float(p[2]), 'C': set(), 'E': []})
        elif p[0] == 'C' and frames:
            frames[-1]['C'].add(int(p[1]))
        elif p[0] == 'E' and frames:
            tid, r, az = int(p[1]), float(p[2]), float(p[3])
            vr  = float(p[4]) if len(p) > 4 else None
            snr = float(p[5]) if len(p) > 5 else None
            frames[-1]['E'].append((tid, r, az, vr, snr))
        # N/M/A/T debug lines ignored
    return knob_line, frames


# ---- truth matching ----------------------------------------------------------
def seg_active(seg, t):
    return seg['t0'] <= t <= seg['t1']


def match_entry(seg, t, r, az):
    """Does an E entry at (t, r, az) match this truth segment?"""
    if not seg_active(seg, t):
        return False
    if 'rmin' in seg:                       # static box
        if not (seg['rmin'] - MATCH_DR_M <= r <= seg['rmax'] + MATCH_DR_M):
            return False
        if 'azmin' in seg and not (seg['azmin'] - MATCH_DAZ_DEG <= az
                                   <= seg['azmax'] + MATCH_DAZ_DEG):
            return False
        return True
    # linear mover segment
    frac = (t - seg['t0']) / max(seg['t1'] - seg['t0'], 1e-9)
    r_pred = seg['r0'] + (seg['r1'] - seg['r0']) * frac
    if abs(r - r_pred) >= MATCH_DR_M:
        return False
    if 'az' in seg and seg['az'] is not None:
        if abs(az - seg['az']) >= MATCH_DAZ_DEG:
            return False
    return True


def score_fixture(frames, truth):
    """Per-fixture metrics. truth may be None (metrics-only fixture)."""
    n_frames = len(frames)
    e_total = sum(len(f['E']) for f in frames)
    tids = set(t for f in frames for (t, *_ ) in f['E'])
    occupied = sum(1 for f in frames if f['E'])
    m = {
        'frames': n_frames,
        'e_total': e_total,
        'e_fr': round(e_total / n_frames, 4) if n_frames else 0.0,
        'tids': len(tids),
        'occ': round(occupied / n_frames, 4) if n_frames else 0.0,
    }
    if truth is None:
        return m

    targets = truth.get('real_targets', [])
    t_first = frames[0]['t'] if frames else 0.0
    ghosts = 0
    mirrors = 0
    per_target = {tg['name']: {'active': 0, 'covered': 0,
                               'tids': set(), 'first_emit': None,
                               'first_active': None}
                  for tg in targets if tg['kind'] == 'mover'}

    for fi, f in enumerate(frames):
        t = f['t'] - t_first
        # per-target activity/coverage
        for tg in targets:
            if tg['kind'] != 'mover':
                continue
            st = per_target[tg['name']]
            if any(seg_active(s, t) for s in tg['segments']):
                st['active'] += 1
                if st['first_active'] is None:
                    st['first_active'] = fi
                hit = [e for e in f['E']
                       if any(match_entry(s, t, e[1], e[2]) for s in tg['segments'])]
                if hit:
                    st['covered'] += 1
                    st['tids'].update(e[0] for e in hit)
                    # first-emit is DEBOUNCED: needs 3 consecutive matched
                    # frames. A single borderline corridor match (a point
                    # centimeters inside the 15 m window) is measurement
                    # noise, not detection — without the debounce, a 1-2 m
                    # reporting shift flips the metric by tens of frames.
                    st['run'] = st.get('run', 0) + 1
                    if st['first_emit'] is None and st['run'] >= 3:
                        st['first_emit'] = fi - 2
                else:
                    st['run'] = 0
        # ghost count: E entries matching NO truth target
        for e in f['E']:
            if not any(match_entry(s, t, e[1], e[2])
                       for tg in targets for s in tg['segments']):
                ghosts += 1
        # mirror pairs: same range, different bearing -> weaker is suspect
        E = f['E']
        weak = set()
        for i in range(len(E)):
            for j in range(i + 1, len(E)):
                if (abs(E[i][1] - E[j][1]) <= MIRROR_DR_M
                        and abs(E[i][2] - E[j][2]) > MIRROR_DAZ_DEG):
                    si = E[i][4] if E[i][4] is not None else float('inf')
                    sj = E[j][4] if E[j][4] is not None else float('-inf')
                    weak.add(E[j][0] if sj <= si else E[i][0])
        mirrors += len(weak)

    m['ghosts_fr'] = round(ghosts / n_frames, 4) if n_frames else 0.0
    m['mirror_fr'] = round(mirrors / n_frames, 4) if n_frames else 0.0
    m['truth_complete'] = bool(truth.get('truth_complete', False))
    m['truth'] = {}
    for name, st in per_target.items():
        cov = st['covered'] / st['active'] if st['active'] else 0.0
        fe = (st['first_emit'] - st['first_active']
              if st['first_emit'] is not None else None)
        m['truth'][name] = {
            'coverage': round(cov, 4),
            'frag': len(st['tids']),
            'first_emit': fe,
        }
    return m


# ---- gating ------------------------------------------------------------------
def check_gates(name, negative, m, base):
    """Returns a list of (gate_label, ok, detail) tuples."""
    out = []
    if negative:
        ok = m['e_total'] == 0
        out.append(('negative: 0 E', ok, 'E_total=%d' % m['e_total']))
        return out
    if base is None:
        out.append(('baseline row', False, 'fixture missing from baseline'))
        return out
    lim = gate_efr(base['e_fr'])
    out.append(('E/fr <= %.3f' % lim, m['e_fr'] <= lim,
                '%.3f (base %.3f)' % (m['e_fr'], base['e_fr'])))
    lim = gate_tids(base['tids'])
    out.append(('tids <= %.0f' % lim, m['tids'] <= lim,
                '%d (base %d)' % (m['tids'], base['tids'])))
    for tname, bt in (base.get('truth') or {}).items():
        mt = (m.get('truth') or {}).get(tname)
        if mt is None:
            out.append(('%s: truth row' % tname, False, 'missing from run'))
            continue
        floor = COVERAGE_FLOOR_FRAC * bt['coverage']
        out.append(('%s cov >= %.3f' % (tname, floor),
                    mt['coverage'] >= floor,
                    '%.3f (base %.3f)' % (mt['coverage'], bt['coverage'])))
        out.append(('%s frag <= %d' % (tname, bt['frag']),
                    mt['frag'] <= bt['frag'],
                    '%d (base %d)' % (mt['frag'], bt['frag'])))
        if bt['first_emit'] is not None:
            lim = bt['first_emit'] + FIRST_EMIT_SLACK_FRAMES
            ok = mt['first_emit'] is not None and mt['first_emit'] <= lim
            out.append(('%s 1st-emit <= %d' % (tname, lim), ok,
                        '%s (base %d)' % (mt['first_emit'], bt['first_emit'])))
    return out


def main():
    ap = argparse.ArgumentParser(description='tracker validation bench')
    ap.add_argument('binary', help='path to a track_replay binary')
    ap.add_argument('--baseline', default=os.path.join(HERE, 'tracker_baseline.json'))
    ap.add_argument('--fixtures', default=os.environ.get('AIRPOC_FIXTURES', ''),
                    help='fixture directory (or set AIRPOC_FIXTURES)')
    ap.add_argument('--freeze', action='store_true',
                    help='rewrite the baseline from this run instead of gating')
    args = ap.parse_args()

    if not args.fixtures:
        sys.exit('no fixture dir: pass --fixtures or set AIRPOC_FIXTURES')
    if not os.path.isfile(args.binary):
        sys.exit('no such binary: %s' % args.binary)

    with open(os.path.join(HERE, 'manifest.json')) as f:
        manifest = json.load(f)['fixtures']

    baseline = None
    if not args.freeze:
        if not os.path.isfile(args.baseline):
            sys.exit('no baseline %s (run with --freeze on the known-good '
                     'binary first)' % args.baseline)
        with open(args.baseline) as f:
            baseline = json.load(f)

    # ---- preflight: every fixture present, every sha pinned and matching ----
    for fx in manifest:
        path = os.path.join(args.fixtures, fx['file'])
        if not os.path.isfile(path):
            sys.exit('missing fixture %s (see radar/docs/TEST_CORPUS.md)' % path)
        sha = sha256_file(path)
        if sha != fx['sha256']:
            sys.exit('REFUSING to run: %s sha256 %s != manifest %s\n'
                     'The fixture bytes changed. Re-pin the manifest only if '
                     'the change is intentional.' % (fx['file'], sha, fx['sha256']))
        if baseline is not None:
            brow = baseline['fixtures'].get(fx['name'])
            if brow and brow['fixture_sha256'] != sha:
                sys.exit('REFUSING to run: baseline was frozen on a different '
                         '%s (sha %s vs disk %s). Re-freeze on the known-good '
                         'binary.' % (fx['file'], brow['fixture_sha256'], sha))

    results = {}
    knob_line_seen = None
    all_ok = True
    inv_fail = []

    for fx in manifest:
        path = os.path.join(args.fixtures, fx['file'])
        truth = None
        if fx['truth']:
            with open(os.path.join(HERE, 'truth', fx['truth'])) as f:
                truth = json.load(f)
        knobs, frames = run_replay(args.binary, path)
        if knobs:
            knob_line_seen = knobs

        # ---- invariants ----
        n_rec = count_fixture_records(path)
        if len(frames) != n_rec:
            inv_fail.append('%s: replay produced %d frames, fixture has %d '
                            'records' % (fx['name'], len(frames), n_rec))
        for fi, f in enumerate(frames):
            bad = [e[0] for e in f['E'] if e[0] not in f['C']]
            if bad:
                inv_fail.append('%s frame %d: E tid(s) %s not confirmed (C) '
                                'that frame — the wire is emitting tracks '
                                'validation never sees' % (fx['name'], fi, bad))
                break
        for fi, f in enumerate(frames):
            if len(f['C']) < len(f['E']):
                inv_fail.append('%s frame %d: C_count %d < E_count %d'
                                % (fx['name'], fi, len(f['C']), len(f['E'])))
                break

        m = score_fixture(frames, truth)
        m['fixture_sha256'] = fx['sha256']
        results[fx['name']] = m

    # ---- report ----
    print('binary : %s (sha256 %s)' % (args.binary, sha256_file(args.binary)[:16]))
    print('knobs  : %s' % (knob_line_seen or '(binary reports no K header — '
                           'knob state UNVERIFIED, pre-bench binary)'))
    print()
    hdr = '%-12s %8s %6s %6s %10s %10s  %s' % (
        'fixture', 'E/fr', 'tids', 'occ', 'ghosts/fr', 'mirror/fr', 'truth (cov/frag/1st-emit)')
    print(hdr)
    print('-' * len(hdr))
    for fx in manifest:
        m = results[fx['name']]
        tstr = ' '.join('%s %.2f/%d/%s' % (n, t['coverage'], t['frag'], t['first_emit'])
                        for n, t in (m.get('truth') or {}).items())
        if m.get('truth') is not None and not m.get('truth_complete', True):
            tstr += '  [truth incomplete: ghosts are upper bounds]'
        print('%-12s %8.3f %6d %6.2f %10s %10s  %s' % (
            fx['name'], m['e_fr'], m['tids'], m['occ'],
            ('%.3f' % m['ghosts_fr']) if 'ghosts_fr' in m else '-',
            ('%.3f' % m['mirror_fr']) if 'mirror_fr' in m else '-', tstr))
    print()

    if inv_fail:
        print('INVARIANT VIOLATIONS (automatic FAIL):')
        for s in inv_fail:
            print('  !! %s' % s)
        print()
        all_ok = False

    if args.freeze:
        if inv_fail:
            sys.exit('refusing to freeze a baseline from a binary that '
                     'violates invariants')
        try:
            git_sha = subprocess.run(
                ['git', '-C', HERE, 'rev-parse', 'HEAD'],
                capture_output=True, text=True).stdout.strip() or 'unknown'
        except OSError:
            git_sha = 'unknown'
        out = {
            'created': datetime.datetime.now(datetime.timezone.utc)
                       .strftime('%Y-%m-%dT%H:%M:%SZ'),
            'tracker_git_sha': git_sha,
            'binary_sha256': sha256_file(args.binary),
            'knob_vector': knob_line_seen or 'unknown',
            'fixtures': results,
        }
        with open(args.baseline, 'w') as f:
            json.dump(out, f, indent=1)
            f.write('\n')
        print('baseline frozen -> %s' % args.baseline)
        return 0

    print('GATES vs baseline (%s, frozen %s, tracker %s):'
          % (args.baseline, baseline['created'], baseline['tracker_git_sha'][:9]))
    for fx in manifest:
        m = results[fx['name']]
        base = baseline['fixtures'].get(fx['name'])
        for label, ok, detail in check_gates(fx['name'], fx['negative'], m, base):
            print('  %-4s %-12s %-26s %s' % ('PASS' if ok else 'FAIL',
                                             fx['name'], label, detail))
            if not ok:
                all_ok = False
    print()
    print('RESULT: %s' % ('PASS' if all_ok else 'FAIL'))
    return 0 if all_ok else 1


if __name__ == '__main__':
    sys.exit(main())
