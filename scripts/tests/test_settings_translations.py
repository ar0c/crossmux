#!/usr/bin/env python3
"""Check explicit Chinese settings translations and six-size built-in glyph coverage."""

import importlib.util
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("gen_i18n", ROOT / "scripts/gen_i18n.py")
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)


class SettingsTranslationsTest(unittest.TestCase):
    def test_restored_large_ui_glyphs_have_compiled_bitmap_data(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if compiler is None:
            self.skipTest("C++ compiler unavailable")
        required = "且云仍代决冻另含听响影忙旧核满给诊轮队限隔零"
        codepoints = ",".join(str(ord(char)) for char in required)
        source = r'''
#include <cassert>
#include <cstddef>
#include <builtinFonts/notosans_cjk_14.h>
#include <builtinFonts/notosans_cjk_16.h>
#include <builtinFonts/notosans_cjk_18.h>
int main() {
  const uint32_t required[] = { CODEPOINTS };
  const EpdFontData* fonts[] = { &notosans_cjk_14, &notosans_cjk_16, &notosans_cjk_18 };
  const size_t sizes[] = { sizeof(notosans_cjk_14Bitmaps), sizeof(notosans_cjk_16Bitmaps), sizeof(notosans_cjk_18Bitmaps) };
  for (size_t f=0; f<3; ++f) {
    const auto& font = *fonts[f];
    assert(font.is2Bit && font.groups == nullptr);
    for (auto cp : required) {
      const EpdGlyph* glyph = nullptr;
      for (uint32_t i=0; i<font.intervalCount; ++i) {
        const auto& interval = font.intervals[i];
        if (cp >= interval.first && cp <= interval.last) {
          glyph = &font.glyph[interval.offset + cp - interval.first]; break;
        }
      }
      assert(glyph && glyph->width > 0 && glyph->height > 0 && glyph->advanceX > 0);
      assert(glyph->dataLength == (glyph->width * glyph->height + 3) / 4);
      assert(glyph->dataOffset + glyph->dataLength <= sizes[f]);
      bool hasInk = false;
      for (uint32_t i=0; i<glyph->dataLength; ++i) hasInk |= font.bitmap[glyph->dataOffset+i] != 0;
      assert(hasInk);
    }
  }
}
'''.replace("CODEPOINTS", codepoints)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            cpp = path / "glyphs.cpp"
            exe = path / "glyphs.exe"
            cpp.write_text(source, encoding="utf-8")
            subprocess.run([compiler, "-std=c++17", "-UNDEBUG", "-I" + str(ROOT / "lib/EpdFont"),
                            str(cpp), "-o", str(exe)], check=True, capture_output=True, text=True)
            subprocess.run([str(exe)], check=True, capture_output=True, text=True)

    def test_settings_and_glyph_coverage(self):
        english = generator.parse_yaml_file(str(ROOT / "lib/I18n/translations/english.yaml"))
        chinese = generator.parse_yaml_file(str(ROOT / "lib/I18n/translations/chinese.yaml"))
        sources = [ROOT / "src/SettingsList.h", ROOT / "src/HomeButtonSettings.h"]
        sources += list((ROOT / "src/activities/settings").glob("*.cpp"))
        sources += list((ROOT / "src/activities/settings").glob("*.h"))
        keys = set().union(*(set(re.findall(r"\bSTR_[A-Z0-9_]+\b", path.read_text())) for path in sources))
        self.assertFalse(keys - chinese.keys(), sorted(keys - chinese.keys()))
        for key in english:
            if key.startswith(("STR_THEME_", "STR_SPACING_")):
                self.assertEqual(chinese[key], english[key], key)
        self.assertEqual(chinese["STR_INX_TAB_POSITION"], "Inx 标签栏位置")

        cjk = lambda text: set(re.findall(r"[一-鿿]", text))
        required = cjk((ROOT / "lib/I18n/translations/chinese.yaml").read_text())
        required |= cjk((ROOT / "lib/EpdFont/scripts/cn_almanac_chars.txt").read_text())
        common = cjk((ROOT / "lib/EpdFont/scripts/chars_3500_common.txt").read_text()) | required
        for name, expected in (("cn_common_chars.txt", common), ("cn_i18n_chars.txt", required)):
            self.assertEqual(cjk((ROOT / "lib/EpdFont/scripts" / name).read_text()), expected, name)
        for size in (8, 10, 12, 14, 16, 18):
            name = "notosans_cjk_common_intervals.h" if size <= 12 else f"notosans_cjk_{size}.h"
            header = (ROOT / "lib/EpdFont/builtinFonts" / name).read_text()
            intervals = re.search(r"Intervals\[\] = \{(.*?)\n\};", header, re.S).group(1)
            codepoints = set()
            for first, last in re.findall(r"\{\s*(0x[0-9A-F]+),\s*(0x[0-9A-F]+),", intervals):
                codepoints.update(range(int(first, 16), int(last, 16) + 1))
            missing = {ord(char) for char in (common if size <= 12 else required)} - codepoints
            self.assertFalse(missing, (size, "".join(map(chr, sorted(missing)))))


if __name__ == "__main__":
    unittest.main()
