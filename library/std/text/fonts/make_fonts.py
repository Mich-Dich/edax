# Makes the fonts std::text::font embeds (library/std/text/fonts/*.ttf) from the system's copies:
#
#   python3 library/std/text/fonts/make_fonts.py <dmsans variable .ttf> <DejaVuSansMono.ttf>
#
# Each font is a static instance (DM Sans: weights 400 and 600 at optical size 14), cut to Latin-1,
# Latin Extended-A and a few symbols, with hinting removed. Kerning pairs from GPOS are written
# into a version-0 `kern` table, the only kerning std::text::font reads; GPOS, GSUB, GDEF and the
# other tables a renderer without shaping doesn't use are dropped.
#
# Needs fontTools (pip install fonttools). Licences: OFL.txt (DM Sans), LICENSE-DejaVu.txt.

import sys
from fontTools.ttLib import TTFont, newTable
from fontTools.ttLib.tables._k_e_r_n import KernTable_format_0
from fontTools.varLib import instancer
from fontTools import subset

UNICODES = (list(range(0x20, 0x7F)) + list(range(0xA0, 0x180)) +
            [0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x20AC, 0x2122,
             0x2190, 0x2191, 0x2192, 0x2193, 0x2212])
DROP = ["GPOS", "GSUB", "GDEF", "STAT", "gasp", "FFTM", "DSIG", "prep", "fpgm", "cvt "]


def kerning_pairs(font):
    """(left glyph, right glyph) -> x advance adjustment, from GPOS pair positioning."""
    pairs = {}
    if "GPOS" not in font:
        return pairs
    gpos = font["GPOS"].table
    for lookup in gpos.LookupList.Lookup:
        subtables = lookup.SubTable
        if lookup.LookupType == 9:   # extension
            subtables = [s.ExtSubTable for s in subtables]
        for st in subtables:
            if getattr(st, "LookupType", 2) != 2 and lookup.LookupType != 9:
                continue
            if not hasattr(st, "Format") or not hasattr(st, "Coverage"):
                continue
            firsts = st.Coverage.glyphs
            if st.Format == 1:
                for left, ps in zip(firsts, st.PairSet):
                    for pv in ps.PairValueRecord:
                        v = getattr(pv.Value1, "XAdvance", 0) if pv.Value1 else 0
                        if v:
                            pairs.setdefault((left, pv.SecondGlyph), v)
            elif st.Format == 2:
                c1 = st.ClassDef1.classDefs
                c2 = st.ClassDef2.classDefs
                seconds = {}
                for g in font.getGlyphOrder():
                    seconds.setdefault(c2.get(g, 0), []).append(g)
                for left in firsts:
                    rec = st.Class1Record[c1.get(left, 0)]
                    for cls, r2 in enumerate(rec.Class2Record):
                        v = getattr(r2.Value1, "XAdvance", 0) if r2.Value1 else 0
                        if not v:
                            continue
                        for right in seconds.get(cls, []):
                            pairs.setdefault((left, right), v)
    return pairs


def finish(font, out):
    pairs = kerning_pairs(font)
    kern = newTable("kern")
    kern.version = 0
    table = KernTable_format_0()
    table.version, table.coverage, table.format = 0, 1, 0
    table.kernTable = pairs
    kern.kernTables = [table]
    font["kern"] = kern
    for tag in DROP:
        if tag in font:
            del font[tag]
    font.save(out)
    print(out, len(pairs), "kerning pairs")


def cut(font, keep_layout):
    opts = subset.Options()
    opts.hinting = False
    opts.layout_features = ["kern"] if keep_layout else []
    opts.name_IDs = ["*"]
    opts.notdef_outline = True
    s = subset.Subsetter(opts)
    s.populate(unicodes=UNICODES)
    s.subset(font)
    return font


def main():
    dm_sans, dejavu_mono = sys.argv[1], sys.argv[2]
    for weight, name in [(400, "DMSans-Regular.ttf"), (600, "DMSans-SemiBold.ttf")]:
        f = instancer.instantiateVariableFont(TTFont(dm_sans), {"wght": weight, "opsz": 14})
        finish(cut(f, True), name)
    finish(cut(TTFont(dejavu_mono), True), "DejaVuSansMono.ttf")


if __name__ == "__main__":
    main()
