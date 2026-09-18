#!/usr/bin/env python3
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
"""Generates app/ConverterData.generated.h.

Everything here is derived from the shipping app's own sources, so the compact
build's converter offers the same categories, the same units in the same order
and the same conversion factors:

  * units, ordering and flags  <- Calculator.ViewModels/DataLoaders/UnitConverterDataLoader.cs
  * category ids and glyphs    <- Calculator.ViewModels/Common/NavCategory.cs
  * display names              <- Calculator/Resources/en-US/Resources.resw

Currency is skipped: its ratios come from a service whose endpoints are dead in
the open-source build (see CurrencyHttpClient.cs), so the shipping app falls
back to placeholder data there.
"""

import os
import re
import xml.etree.ElementTree as ET

# The shipping loader derives these from the user's region at run time; the
# generated tables use its default, matching an en-US install.
REGION_FLAGS = {
    "_regionCode": "US",
    "useUSCustomaryAndFahrenheit": True,
    "useUSCustomary": True,
    "useSI": False,
    "useFahrenheit": True,
    "useWattInsteadOfKilowatt": False,  # _regionCode == "GB"
    "usePyeong": False,
}

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, "..", ".."))
LOADER = os.path.join(SRC, "Calculator.ViewModels", "DataLoaders", "UnitConverterDataLoader.cs")
NAVCAT = os.path.join(SRC, "Calculator.ViewModels", "Common", "NavCategory.cs")
RESW = os.path.join(SRC, "Calculator", "Resources", "en-US", "Resources.resw")
OUT = os.path.normpath(os.path.join(HERE, "..", "app", "ConverterData.generated.h"))


def load_resources():
    tree = ET.parse(RESW)
    return {e.get("name"): (e.find("value").text or "") for e in tree.getroot().findall("data")}


def load_category_ids():
    text = open(NAVCAT, encoding="utf-8-sig").read()
    ids = {m.group(1): int(m.group(2)) for m in re.finditer(r"private const int (\w+)Id\s*=\s*(\d+);", text)}
    glyphs, negatives = {}, {}
    for m in re.finditer(r"ViewMode = ViewMode\.(\w+).*?Glyph = \"(\\u[0-9A-Fa-f]{4})\".*?GroupType = CategoryGroupType\.(\w+).*?SupportsNegative = (\w+)", text):
        glyphs[m.group(1)] = (m.group(2), m.group(3), m.group(4))
    return ids, glyphs


def load_unit_ids(text):
    return {m.group(1): int(m.group(2)) for m in re.finditer(r"private const int (\w+)\s*=\s*(\d+);", text)}


def load_units(text):
    """AddUnit(ViewMode.X, Unit_Id, "UnitName_Y", "UnitAbbreviation_Y", order, [src], [tgt], [whimsical])"""
    units = []
    pattern = re.compile(
        r"AddUnit\(ViewMode\.(\w+),\s*(\w+),\s*\"([^\"]+)\"\s*,\s*\"([^\"]+)\"\s*,\s*(\d+)"
        r"(?:\s*,\s*([^,\)]+))?(?:\s*,\s*([^,\)]+))?(?:\s*,\s*([^,\)]+))?\s*\)")
    for m in pattern.finditer(text):
        def flag(raw):
            """Evaluates the C# boolean expression for the default (US) region."""
            if raw is None:
                return False
            expr = raw.strip()
            if expr in ("true", "false"):
                return expr == "true"
            # Translate C# boolean syntax to Python and evaluate against the
            # region constants the loader derives at the top of LoadData().
            expr = expr.replace("&&", " and ").replace("||", " or ").replace("!", " not ")
            expr = re.sub(r"\bnot\s*=", "!=", expr)  # undo damage to "!="
            return bool(eval(expr, {"__builtins__": {}}, REGION_FLAGS))

        units.append({
            "mode": m.group(1),
            "const": m.group(2),
            "name_key": m.group(3).strip(),
            "abbr_key": m.group(4).strip(),
            "order": int(m.group(5)),
            "source": flag(m.group(6)),
            "target": flag(m.group(7)),
            "whimsical": flag(m.group(8)),
        })
    return units


def load_factors(text):
    """conversionFactors: [ViewMode.X] = { { Unit_Id, <expr> }, ... }"""
    start = text.index("var conversionFactors")
    end = text.index("// Build explicit conversion data")
    body = text[start:end]
    factors = {}
    for block in re.finditer(r"\[ViewMode\.(\w+)\] = new Dictionary<int, double>\s*\{(.*?)\n                \},", body, re.S):
        mode, inner = block.group(1), block.group(2)
        entries = {}
        for m in re.finditer(r"\{\s*(\w+)\s*,\s*([^}]+?)\s*\}", inner):
            entries[m.group(1)] = eval(m.group(2).replace("/", "/"))  # plain arithmetic literals
        factors[mode] = entries
    return factors


def load_explicit(text):
    """Temperature's ratio/offset/offsetFirst triples."""
    start = text.index("var explicitConversions")
    end = text.index("// Build unit lookup by ID")
    body = text[start:end]
    out = {}
    for block in re.finditer(r"\[(\w+)\] = new Dictionary<int, \(double, double, bool\)>\s*\{(.*?)\n                \},", body, re.S):
        src, inner = block.group(1), block.group(2)
        row = {}
        for m in re.finditer(r"\{\s*(\w+)\s*,\s*\(([^)]+)\)\s*\}", inner):
            ratio, offset, first = [p.strip() for p in m.group(2).split(",")]
            row[m.group(1)] = (float(eval(ratio)), float(eval(offset)), first == "true")
        out[src] = row
    return out


def esc(text):
    out = ""
    for ch in text:
        o = ord(ch)
        if ch == '"':
            out += '\\"'
        elif ch == "\\":
            out += "\\\\"
        elif 32 <= o < 127:
            out += ch
        else:
            out += '\\x%04x""' % o
    return out


def main():
    text = open(LOADER, encoding="utf-8-sig").read()
    res = load_resources()
    cat_ids, cat_meta = load_category_ids()
    unit_ids = load_unit_ids(text)
    units = load_units(text)
    factors = load_factors(text)
    explicit = load_explicit(text)

    # Converter categories, in the manifest's order, minus currency.
    order = ["Volume", "Length", "Weight", "Temperature", "Energy", "Area",
             "Speed", "Time", "Power", "Data", "Pressure", "Angle"]

    by_mode = {}
    for u in units:
        by_mode.setdefault(u["mode"], []).append(u)
    for mode in by_mode:
        by_mode[mode].sort(key=lambda u: u["order"])

    lines = []
    lines.append("// Copyright (c) Microsoft Corporation. All rights reserved.")
    lines.append("// Licensed under the MIT License.")
    lines.append("//")
    lines.append("// GENERATED by tools/gen_converter_data.py -- do not edit by hand.")
    lines.append("// Sources: Calculator.ViewModels/DataLoaders/UnitConverterDataLoader.cs,")
    lines.append("//          Calculator.ViewModels/Common/NavCategory.cs,")
    lines.append("//          Calculator/Resources/en-US/Resources.resw")
    lines.append("")
    lines.append("#pragma once")
    lines.append("")
    lines.append("#include <array>")
    lines.append("")
    lines.append("namespace CalcCompact")
    lines.append("{")
    lines.append("    // Fields are ordered widest-first and use plain pointers rather than")
    lines.append("    // string views: at 158 units the padding and the extra relocations were")
    lines.append("    // worth several KB of the binary.")
    lines.append("    struct ConverterUnit")
    lines.append("    {")
    lines.append("        double factor;        // relative to the category's base unit")
    lines.append("        const wchar_t* name;")
    lines.append("        const wchar_t* abbreviation;")
    lines.append("        int id;")
    lines.append("        bool isConversionSource;")
    lines.append("        bool isConversionTarget;")
    lines.append("        bool isWhimsical;     // shown only in the suggested-values list")
    lines.append("    };")
    lines.append("")
    lines.append("    struct ConverterExplicitConversion")
    lines.append("    {")
    lines.append("        int fromId;")
    lines.append("        int toId;")
    lines.append("        double ratio;")
    lines.append("        double offset;")
    lines.append("        bool offsetFirst;")
    lines.append("    };")
    lines.append("")
    lines.append("    struct ConverterCategory")
    lines.append("    {")
    lines.append("        const ConverterUnit* units;")
    lines.append("        const ConverterExplicitConversion* explicitConversions;")
    lines.append("        const wchar_t* name;")
    lines.append("        const wchar_t* glyph;")
    lines.append("        unsigned short unitCount;")
    lines.append("        unsigned short explicitCount;")
    lines.append("        unsigned short id;")
    lines.append("        bool supportsNegative;")
    lines.append("    };")
    lines.append("")

    total_units = 0
    for mode in order:
        mode_units = by_mode.get(mode, [])
        total_units += len(mode_units)
        lines.append("    inline constexpr std::array<ConverterUnit, %d> k%sUnits = {{" % (len(mode_units), mode))
        for u in mode_units:
            uid = unit_ids[u["const"]]
            name = res.get(u["name_key"], u["const"])
            abbr = res.get(u["abbr_key"], "")
            factor = factors.get(mode, {}).get(u["const"])
            factor_text = "0.0" if factor is None else repr(float(factor))
            lines.append('        { %s, L"%s", L"%s", %d, %s, %s, %s },'
                         % (factor_text, esc(name), esc(abbr), uid,
                            "true" if u["source"] else "false",
                            "true" if u["target"] else "false",
                            "true" if u["whimsical"] else "false"))
        lines.append("    }};")
        lines.append("")

    # Temperature's explicit table.
    exp_rows = []
    for src, row in explicit.items():
        for dst, (ratio, offset, first) in row.items():
            exp_rows.append((unit_ids[src], unit_ids[dst], ratio, offset, first))
    lines.append("    inline constexpr std::array<ConverterExplicitConversion, %d> kTemperatureConversions = {{" % len(exp_rows))
    for a, b, r, o, f in exp_rows:
        lines.append("        { %d, %d, %s, %s, %s }," % (a, b, repr(r), repr(o), "true" if f else "false"))
    lines.append("    }};")
    lines.append("")

    lines.append("    inline constexpr std::array<ConverterCategory, %d> kConverterCategories = {{" % len(order))
    for mode in order:
        cid = cat_ids[mode]
        glyph, _group, negative = cat_meta.get(mode, ("\\u0000", "Converter", "PositiveOnly"))
        name = res.get("CategoryName_%sText" % mode, mode)
        supports_negative = "true" if negative in ("SupportsNegative", "SupportsAll") else "false"
        table = "kTemperatureConversions" if mode == "Temperature" else "nullptr"
        count = "kTemperatureConversions.size()" if mode == "Temperature" else "0"
        data = "kTemperatureConversions.data()" if mode == "Temperature" else "nullptr"
        lines.append('        { k%sUnits.data(), %s, L"%s", L"%s", %s, %s, %d, %s },'
                     % (mode, data, esc(name), glyph,
                        "static_cast<unsigned short>(k%sUnits.size())" % mode,
                        "static_cast<unsigned short>(%s)" % count, cid, supports_negative))
    lines.append("    }};")
    lines.append("}")

    with open(OUT, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")
    print("wrote %s: %d categories, %d units, %d explicit conversions"
          % (OUT, len(order), total_units, len(exp_rows)))


if __name__ == "__main__":
    main()
