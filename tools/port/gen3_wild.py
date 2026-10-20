#!/usr/bin/env python3
"""Port option "Nur Hoenn" (gen-3 only wild Pokémon): builds alternative
wild encounter tables from the decomp's Ruby/Sapphire data and pokeemerald's
Emerald data (tools/port/data/emerald_wild_encounters.json).

Per map and encounter kind (grass, surfing, Rock Smash, old/good/super rod):
  1. slots with a gen-3 species stay as they are;
  2. the other slots get gen-3 species found at the same place in Ruby,
     Sapphire or Emerald, weighted by how often they appear there;
  3. if fewer than MIN_SPECIES remain, species from the other water kinds
     of the same map, then from neighbouring maps (connections and warps,
     e.g. the route in front of a cave) are added;
  4. slot levels stay; a species too evolved for the slot's level is
     replaced by its pre-evolution.

usage: gen3_wild.py <out.h> <report.md>
"""
import json
import os
import re
import sys
from collections import defaultdict, deque

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
MIN_SPECIES = 4

RATES = {
    "land_mons": [20, 20, 10, 10, 10, 10, 5, 5, 4, 4, 1, 1],
    "water_mons": [60, 30, 5, 4, 1],
    "rock_smash_mons": [60, 30, 5, 4, 1],
    "fishing_mons": [70, 30, 60, 20, 20, 40, 40, 15, 4, 1],
}
RODS = {"old_rod": [0, 1], "good_rod": [2, 3, 4], "super_rod": [5, 6, 7, 8, 9]}
# The Safari Zone has almost only Kanto/Johto Pokémon: it becomes the place
# for the rarer Hoenn ones (version exclusives of both games included).
SAFARI = {
    "MAP_SAFARI_ZONE_SOUTHWEST": {"SPECIES_SKITTY": 3, "SPECIES_PLUSLE": 2, "SPECIES_MINUN": 2, "SPECIES_SPINDA": 2, "SPECIES_SEEDOT": 2, "SPECIES_LOTAD": 2},
    "MAP_SAFARI_ZONE_SOUTHEAST": {"SPECIES_ZANGOOSE": 2, "SPECIES_SEVIPER": 2, "SPECIES_KECLEON": 2, "SPECIES_ELECTRIKE": 2, "SPECIES_SWABLU": 2, "SPECIES_SHROOMISH": 2},
    "MAP_SAFARI_ZONE_NORTHWEST": {"SPECIES_TROPIUS": 2, "SPECIES_ABSOL": 1, "SPECIES_CHIMECHO": 1, "SPECIES_SPHEAL": 2, "SPECIES_SNORUNT": 2, "SPECIES_BAGON": 1},
    "MAP_SAFARI_ZONE_NORTHEAST": {"SPECIES_MAWILE": 2, "SPECIES_SABLEYE": 2, "SPECIES_LUNATONE": 2, "SPECIES_SOLROCK": 2, "SPECIES_TRAPINCH": 2, "SPECIES_BELDUM": 1},
}
WATER_KINDS = ["water_mons", "old_rod", "good_rod", "super_rod"]


def path(*p):
    return os.path.join(ROOT, *p)


def load_species():
    num = {}
    for l in open(path("include/constants/species.h")):
        m = re.match(r"#define (SPECIES_\w+) (\d+)", l)
        if m:
            num[m.group(1)] = int(m.group(2))
    return num


NUM = load_species()


def is_gen3(s):
    return NUM["SPECIES_TREECKO"] <= NUM.get(s, 0) <= NUM["SPECIES_CHIMECHO"]


def load_preevo():
    """species -> (pre-evolution, level it evolves at or None)"""
    pre = {}
    cur = None
    for l in open(path("src/data/pokemon/evolution.h")):
        m = re.match(r"\s*\[(SPECIES_\w+)\]\s*=\s*\{(.*)\},?\s*$", l)
        if not m:
            continue
        src = m.group(1)
        for kind, param, dst in re.findall(r"\{(EVO_\w+),\s*(\w+),\s*(SPECIES_\w+)\}", m.group(2)):
            lvl = int(param) if kind == "EVO_LEVEL" else None
            if kind.startswith("EVO_LEVEL"):
                lvl = int(param) if param.isdigit() else None
            if dst not in pre:
                pre[dst] = (src, lvl)
    return pre


PRE = load_preevo()
mtype = {}


def fit_level(species, level):
    """Walk back the evolution chain until the species is plausible."""
    while species in PRE:
        src, lvl = PRE[species]
        if lvl is None:
            lvl = 30  # friendship / item / trade evolutions: a mid-game form
        if level >= lvl:
            break
        species = src
    return species


def kinds_of(enc):
    """encounter -> {kind: [(slot_rate, min, max, species)]}"""
    out = {}
    for k in ("land_mons", "water_mons", "rock_smash_mons"):
        if enc.get(k):
            out[k] = [(RATES[k][i], m["min_level"], m["max_level"], m["species"]) for i, m in enumerate(enc[k]["mons"])]
    if enc.get("fishing_mons"):
        mons = enc["fishing_mons"]["mons"]
        for rod, idx in RODS.items():
            out[rod] = [(RATES["fishing_mons"][i], mons[i]["min_level"], mons[i]["max_level"], mons[i]["species"]) for i in idx]
    return out


def load_encounters(p):
    return json.load(open(p))["wild_encounter_groups"][0]["encounters"]


def load_neighbours():
    nb = defaultdict(list)
    global mtype
    for mj in os.listdir(path("data/maps")):
        f = path("data/maps", mj, "map.json")
        if not os.path.exists(f):
            continue
        d = json.load(open(f))
        me = d["id"]
        mtype[me] = d.get("map_type")
        for c in d.get("connections") or []:
            nb[me].append(c["map"])
            nb[c["map"]].append(me)
        for w in d.get("warp_events") or []:
            nb[me].append(w["dest_map"])
            nb[w["dest_map"]].append(me)
    return nb


def main():
    out_h, report = sys.argv[1], sys.argv[2]
    rs = load_encounters(path("src/data/wild_encounters.json"))
    em = load_encounters(path("tools/port/data/emerald_wild_encounters.json"))
    nb = load_neighbours()

    # gen-3 weight per (map, kind) over Ruby, Sapphire and Emerald
    pool = defaultdict(lambda: defaultdict(float))
    for enc in rs + em:
        for kind, slots in kinds_of(enc).items():
            tot = sum(r for r, _, _, _ in slots)
            for r, _, _, s in slots:
                if is_gen3(s):
                    pool[(enc["map"], kind)][s] += r / tot

    def related(kind):
        # rods borrow only from rods (no fished Wingull); surfing from all water
        if kind == "water_mons":
            return WATER_KINDS
        if kind in RODS:
            return list(RODS)
        return [kind]

    def add_best(w, src, factor):
        for s, x in sorted(src.items(), key=lambda t: -t[1]):
            if len(w) >= MIN_SPECIES:
                return
            w.setdefault(s, x * factor)

    def candidates(m, kind):
        if m.startswith("MAP_SAFARI_ZONE") and kind == "land_mons":
            return dict(SAFARI[m])
        w = dict(pool[(m, kind)])
        if len(w) >= MIN_SPECIES:
            return w
        for k2 in related(kind):  # same place, other water kinds
            if k2 != kind:
                add_best(w, pool[(m, k2)], 0.5)
        # neighbouring maps (connections, warps), nearest first
        seen = {m}
        q = deque([(m, 0)])
        while q and len(w) < MIN_SPECIES:
            cur, d = q.popleft()
            if d == 3:
                continue
            for n in nb[cur]:
                if n in seen:
                    continue
                seen.add(n)
                q.append((n, d + 1))
                for k2 in related(kind):
                    add_best(w, pool[(n, k2)], 0.5 / (d + 1))
        # last resort: the same kind of place anywhere in Hoenn
        if len(w) < 2:
            agg = defaultdict(float)
            for (m2, k2), sp in pool.items():
                if k2 in related(kind) and mtype.get(m2) == mtype.get(m):
                    for s, x in sp.items():
                        agg[s] += x
            add_best(w, agg, 0.3 / max(1, len(agg)))
        if len(w) < 2:  # e.g. Rock Smash outside caves: any place
            agg = defaultdict(float)
            for (m2, k2), sp in pool.items():
                if k2 in related(kind):
                    for s, x in sp.items():
                        agg[s] += x
            add_best(w, agg, 0.3 / max(1, len(agg)))
        return w

    lines = ["// Generated by tools/port/gen3_wild.py - port option \"Nur Hoenn\".", ""]
    rep = ["# Nur Hoenn – wilde Pokémon (generiert)", ""]
    headers = defaultdict(list)
    for enc in rs:
        ver = "RUBY" if enc["base_label"].endswith("_Ruby") else "SAPPHIRE"
        label = "PortGen3_" + enc["base_label"]
        kinds = kinds_of(enc)
        changed = []
        new = {}
        for kind, slots in kinds.items():
            w = candidates(enc["map"], kind)
            keep = [s if is_gen3(s) else None for _, _, _, s in slots]
            if not w:
                new[kind] = slots
                if any(k is None for k in keep):
                    changed.append("%s: keine Hoenn-Pokémon gefunden, unverändert" % kind)
                continue
            tot = sum(w.values())
            target = {s: x / tot for s, x in w.items()}
            have = defaultdict(float)
            slot_tot = sum(r for r, _, _, _ in slots)
            for (r, _, _, s), k in zip(slots, keep):
                if k:
                    have[k] += r / slot_tot
            res = []
            for (r, lo, hi, s), k in zip(slots, keep):
                if k:
                    res.append((r, lo, hi, k))
                    continue
                best = max(target, key=lambda x: (target[x] - have[x], x))
                have[best] += r / slot_tot
                res.append((r, lo, hi, fit_level(best, lo)))
            new[kind] = res
            if res != slots:
                changed.append("%s: %s → %s" % (kind, ", ".join(sorted({s[8:] for *_, s in slots})),
                                                 ", ".join(sorted({s[8:] for *_, s in res}))))
        # C output
        lines.append("#ifdef %s" % ver)
        info = {}
        for k in ("land_mons", "water_mons", "rock_smash_mons"):
            if k in new:
                name = "%s_%s" % (label, k)
                lines.append("static const struct WildPokemon %s[] = {" % name)
                lines += ["    { %d, %d, %s }," % (lo, hi, s) for _, lo, hi, s in new[k]]
                lines.append("};")
                lines.append("static const struct WildPokemonInfo %sInfo = { %d, %s };" % (name, enc[k]["encounter_rate"], name))
                info[k] = "&%sInfo" % name
        if "old_rod" in new:
            name = "%s_fishing_mons" % label
            allf = new["old_rod"] + new["good_rod"] + new["super_rod"]
            lines.append("static const struct WildPokemon %s[] = {" % name)
            lines += ["    { %d, %d, %s }," % (lo, hi, s) for _, lo, hi, s in allf]
            lines.append("};")
            lines.append("static const struct WildPokemonInfo %sInfo = { %d, %s };" % (name, enc["fishing_mons"]["encounter_rate"], name))
            info["fishing_mons"] = "&%sInfo" % name
        lines.append("#endif")
        headers[ver].append((enc["map"], info))
        if changed:
            rep.append("- **%s** (%s): %s" % (enc["map"][4:], ver.title(), "; ".join(changed)))

    lines.append("")
    lines.append("const struct WildPokemonHeader gPortGen3WildMonHeaders[] =")
    lines.append("{")
    for ver in ("RUBY", "SAPPHIRE"):
        lines.append("#ifdef %s" % ver)
        for m, info in headers[ver]:
            lines.append("    { MAP_GROUP(%s), MAP_NUM(%s), %s, %s, %s, %s }," % (
                m, m, info.get("land_mons", "NULL"), info.get("water_mons", "NULL"),
                info.get("rock_smash_mons", "NULL"), info.get("fishing_mons", "NULL")))
        lines.append("#endif")
    lines.append("    { MAP_GROUP(MAP_UNDEFINED), MAP_NUM(MAP_UNDEFINED), NULL, NULL, NULL, NULL },")
    lines.append("};")
    open(out_h, "w").write("\n".join(lines) + "\n")
    open(report, "w").write("\n".join(rep) + "\n")


main()
