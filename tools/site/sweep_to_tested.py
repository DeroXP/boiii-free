"""Turns a sweep (tools/dev/session/sweep.ps1 -> sweep-<tag>.txt, and with -PollPools pools-<tag>.json) into the
website's site/tested.json, which build_site.py makes the Tested page from.

  python -I tools/site/sweep_to_tested.py <sweep-<tag>.txt> "<commit> (<version>)" \
      --stack-name "DS4C Patched v0.85.1" --stack-name ... [--pools pools-<tag>.json] \
      [--results map-checks/results.jsonl] [--settings "..."]

--stack-name gives the mods' names in load order (the first is the main mod). With --pools, each map gets the
most of the game's model and image tables its level used (results.jsonl ties a run's process to its map; the
last run of a map counts).
"""
import argparse
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
NAMES = {
    'zm_zod': 'Shadows of Evil', 'zm_tomb': 'Origins', 'zm_theater': 'Kino der Toten', 'zm_moon': 'Moon',
    'zm_asylum': 'Verr\u00fcckt', 'zm_cosmodrome': 'Ascension', 'zm_prototype': 'Nacht der Untoten',
    'zm_sumpf': 'Shi No Numa', 'zm_temple': 'Shangri-La', 'zm_factory': 'The Giant', 'zm_castle': 'Der Eisendrache',
    'zm_island': 'Zetsubou No Shima', 'zm_stalingrad': 'Gorod Krovi', 'zm_genesis': 'Revelations',
    'zm_ccube': 'Cheese Cube Unlimited', 'zm_town': 'Town Reimagined', 'zm_giant': 'The Giant (Remastered)',
    'zm_der_riese': 'Der Riese: Declassified', 'zm_shoothouse': 'Shoot Haus', 'zm_mw2rust': 'Rust V5',
    'zm_supermario64': 'Super Mario 64', 'zm_prison': 'Mob of the Dead', 'zm_chickenfila': 'Chick-fil-A',
    'zm_die': 'Die Rise', 'zm_deepboom': 'Project: Kaboom - Bazar', 'zm_blundeezy': 'Project Blundeezy',
    'zm_log_overunder': 'Sanctum Obitus', 'zm_wizardcastle': 'Return to Hogwartz', 'zm_backrooms': 'Backroomz',
    'zm_digital_circus': 'The Amazing Digital Circus', 'zm_mw4_shipment': 'MW4 Shipment Zombie',
    'zm_big_bang_tower': 'Big Bang Tower', 'zm_buriedchallenge': 'The Buried Challenge',
}
# The game's own table sizes (docs/research/engine-re.md, "Asset pools the game finds by slot").
LIMITS = {'xmodel': 11264, 'image': 49152}
HOW = ('tools/dev/session/map-check.ps1 -Live, one game per map: Xerces.exe launches the stack, the map is '
       'started from the console, the check waits for the player in the level, plays 30 s with the intro '
       'skipped, captures the screen, goes back to the menu and quits; a crash, a freeze, an error box or '
       'the time running out ends the run. Workshop maps as installed on the test PC.')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('sweep')
    p.add_argument('build')
    p.add_argument('--stack-name', action='append', default=[])
    p.add_argument('--pools')
    p.add_argument('--results', default=r'C:\Users\Ginld\XercesDev\map-checks\results.jsonl')
    p.add_argument('--settings', default="Larger zone memory at 8 GB and the precise FPS limit, as the stack's "
                                         "owner plays it")
    p.add_argument('--out', default=str(ROOT / 'site' / 'tested.json'))
    a = p.parse_args()
    if not a.stack_name:
        p.error('--stack-name for each mod, in load order')

    peaks = {}
    if a.pools:
        pools = json.load(open(a.pools, encoding='utf-8'))
        for line in open(a.results, encoding='utf-8-sig'):
            try:
                r = json.loads(line.lstrip('\ufeff'))
            except ValueError:
                continue
            pid = str(r.get('pid') or '')
            if pid in pools and r.get('map'):
                peaks[r['map']] = {t: pools[pid].get(t, [0, 0])[0] for t in LIMITS}

    maps = []
    for line in open(a.sweep, encoding='utf-8-sig'):
        m = re.match(r'^(zm_\S+)\s+(\S+)\s+menu\s+([\d.]*) s, load\s+([\d.]*) s\s+(.*)$', line.strip())
        if not m:
            continue
        name, result, _menu, load, detail = m.groups()
        entry = {'map': name, 'name': NAMES.get(name, name), 'result': result}
        if load:
            entry['load_s'] = float(load)
        if result != 'in_game':
            entry['note'] = detail.strip()
        if name in peaks:
            entry['pools'] = peaks[name]
        maps.append(entry)
    if not maps:
        print(f'{a.sweep}: no map lines')
        return 1
    out = {
        'schema': 'xerces.site.tested/1',
        'build': a.build,
        'settings': a.settings,
        'stack': a.stack_name,
        'maps': maps,
        'how': HOW,
    }
    if peaks:
        out['limits'] = LIMITS
    pathlib.Path(a.out).write_text(json.dumps(out, indent=2, ensure_ascii=False) + '\n', encoding='utf-8',
                                   newline='\n')
    print(f"{a.out}: {len(maps)} maps, {sum(1 for m in maps if m['result'] == 'in_game')} played, "
          f"{len(peaks)} with pool peaks")
    return 0


if __name__ == '__main__':
    sys.exit(main())
