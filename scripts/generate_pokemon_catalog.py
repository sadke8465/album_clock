"""Generate compact firmware species/game metadata from PokeAPI CSV data.

Pass a directory containing pokemon_species.csv, pokemon_species_names.csv,
and pokemon_types.csv from PokeAPI (data/v2/csv). No sprites are embedded.
"""
import csv
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]


def load(path):
    with path.open(encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle))


def generate(source):
    species = {int(r["id"]): r for r in load(source / "pokemon_species.csv") if 1 <= int(r["id"]) <= 809}
    names = {int(r["pokemon_species_id"]): r["name"] for r in load(source / "pokemon_species_names.csv") if r["local_language_id"] == "9"}
    types = {}
    for r in load(source / "pokemon_types.csv"):
        k = int(r["pokemon_id"])
        if k in species:
            types.setdefault(k, {})[int(r["slot"])] = int(r["type_id"])
    parents = {k: int(v["evolves_from_species_id"] or 0) for k, v in species.items()}
    children = set(parents.values())
    games = json.loads((ROOT / "data/pokemon-games.json").read_text())
    lines = ["#pragma once", "#include <stdint.h>", "// Generated from PokeAPI, original species metadata only.",
             "struct PokemonSpecies { const char *name; uint16_t family,parent; uint8_t generation,type1,type2,stage,flags; };",
             "static const PokemonSpecies POKEMON_SPECIES[] = {", '  {"",0,0,0,0,0,0,0},']
    for k in range(1, 810):
        r = species[k]
        parent = parents[k]
        stage = 3 if parent and k not in children else 2 if parent else 1
        flags = int(r["is_legendary"] == "1" or r["is_mythical"] == "1") | (int(r["has_gender_differences"] == "1") << 1)
        name = names.get(k, r["identifier"])
        lines.append(f'  {{{json.dumps(name, ensure_ascii=False)},{r["evolution_chain_id"]},{parent},{r["generation_id"]},{types.get(k,{}).get(1,0)},{types.get(k,{}).get(2,0)},{stage},{flags}}},')
    lines += ["};", "static constexpr uint16_t POKEMON_COUNT=809;", "struct PokemonGame { const char *name; uint16_t maxDex; bool animated,shiny,backAnimated,female; };",
              'static const PokemonGame POKEMON_GAMES[] = {{"Mixed games",809,true,true,true,true},']
    for g in games:
        lines.append(f'  {{{json.dumps(g["name"])},{g["max"]},{str(bool(g["animated"])).lower()},{str(g["shiny"]).lower()},{str(g["backAnimated"]).lower()},{str(g["female"]).lower()}}},')
    lines += ["};", f"static constexpr uint8_t POKEMON_GAME_COUNT={len(games)};", ""]
    (ROOT / "firmware/include/PokemonCatalog.h").write_text("\n".join(lines), encoding="utf-8")
    (ROOT / "worker/src/pokemon-games.ts").write_text(
        "// Generated from data/pokemon-games.json.\n" +
        "export interface PokemonGameAsset {id:number;name:string;path:string;max:number;extra?:number[];animated:string;static:string;animatedAtRoot?:boolean;transparent:boolean;shiny:boolean;backAnimated:boolean;female:boolean;}\n" +
        "export const POKEMON_GAMES:PokemonGameAsset[]=" + json.dumps(games, indent=2) + ";\n", encoding="utf-8")


if __name__ == "__main__":
    generate(pathlib.Path(sys.argv[1]))
