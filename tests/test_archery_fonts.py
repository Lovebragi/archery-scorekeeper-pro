#!/usr/bin/env python3
"""Font-coverage and integrity gate for the archery scorekeeper UI fonts.

This test needs no Pillow, ESP-IDF, or network: it reads the generated font C
files exactly as LVGL would, compares them with the committed manifests, and
checks that every non-ASCII character in the application's UI sources has a
glyph. Adding Chinese text without regenerating the fonts therefore fails the
static gate instead of showing placeholder boxes on the device.
"""

from __future__ import annotations

import hashlib
import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import lvgl_font_tools as tools  # noqa: E402

FONTS = (
    ("archery_font_16", 16),
    ("archery_font_20", 20),
)
UI_SOURCES = (
    ROOT / "main" / "archery_ui.c",
    ROOT / "main" / "archery_score.c",
    # 语音实验：状态与错误提示直接来自这两个文件，也必须被字库覆盖。
    ROOT / "main" / "archery_app.c",
    ROOT / "main" / "archery_wifi.c",
    ROOT / "main" / "archery_voice.c",
)
INVENTORY = ROOT / "assets" / "fonts" / "archery_chars.txt"
# 一个确定不存在的码点，用来证明覆盖检查不是恒真。
KNOWN_ABSENT = 0x9F98


def font_path(name: str) -> Path:
    return ROOT / "assets" / "fonts" / f"{name}.c"


def manifest_path(name: str) -> Path:
    return ROOT / "assets" / "fonts" / f"{name}.json"


class ArcheryFontTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.ui_codepoints = tools.scan_source_codepoints(UI_SOURCES)
        cls.inventory = tools.load_inventory(INVENTORY)
        cls.fonts = {name: tools.parse_font_c(font_path(name)) for name, _ in FONTS}

    def test_ui_text_is_utf8_and_printable(self) -> None:
        for path in UI_SOURCES:
            text = path.read_text(encoding="utf-8")
            self.assertNotIn("\ufffd", text)
        self.assertTrue(self.ui_codepoints, "UI sources yielded no text")

    def test_every_ui_codepoint_is_in_the_inventory(self) -> None:
        missing = [
            cp for cp in sorted(self.ui_codepoints)
            if cp > tools.ASCII_LAST and cp not in self.inventory
        ]
        self.assertEqual(
            missing, [],
            "UI text is missing from assets/fonts/archery_chars.txt: "
            + " ".join(f"U+{cp:04X}" for cp in missing)
            + " (rerun tools/gen_lvgl_text_font.py --update-inventory)")

    def test_every_font_covers_every_ui_codepoint(self) -> None:
        for name, _size in FONTS:
            covered = tools.cmap_codepoints(self.fonts[name])
            missing = [
                cp for cp in sorted(self.ui_codepoints) if cp not in covered
            ]
            self.assertEqual(
                missing, [],
                f"{name} has no glyph for "
                + " ".join(f"U+{cp:04X}" for cp in missing))

    def test_coverage_check_has_a_negative_control(self) -> None:
        for name, _size in FONTS:
            covered = tools.cmap_codepoints(self.fonts[name])
            self.assertNotIn(KNOWN_ABSENT, covered)
            self.assertIsNone(tools.glyph_index_for(self.fonts[name], KNOWN_ABSENT))

    def test_font_metadata_matches_the_manifest(self) -> None:
        for name, size in FONTS:
            font = self.fonts[name]
            manifest = json.loads(manifest_path(name).read_text(encoding="utf-8"))
            self.assertEqual(manifest["name"], name)
            self.assertEqual(manifest["size"], size)
            self.assertEqual(manifest["bpp"], 4)
            self.assertEqual(manifest["line_height"], font["line_height"])
            self.assertEqual(manifest["base_line"], font["base_line"])
            self.assertEqual(manifest["codepoints"], sorted(manifest["codepoints"]))
            self.assertEqual(len(manifest["glyphs"]), len(manifest["codepoints"]))
            self.assertEqual(len(font["glyphs"]), len(manifest["glyphs"]) + 1)

    def test_glyph_bitmaps_are_intact(self) -> None:
        for name, _size in FONTS:
            font = self.fonts[name]
            manifest = json.loads(manifest_path(name).read_text(encoding="utf-8"))
            entries = manifest["glyphs"]
            for index, entry in enumerate(entries):
                glyph_index = entry["glyph"]
                self.assertEqual(glyph_index, index + 1)
                self.assertEqual(entry["codepoint"], manifest["codepoints"][index])
                glyph = font["glyphs"][glyph_index]
                self.assertEqual(entry["box_w"], glyph["box_w"])
                self.assertEqual(entry["box_h"], glyph["box_h"])
                self.assertEqual(entry["ofs_x"], glyph["ofs_x"])
                self.assertEqual(entry["ofs_y"], glyph["ofs_y"])
                self.assertEqual(entry["adv_w"], glyph["adv_w"])
                data = tools.glyph_bitmap_bytes(font, glyph_index)
                self.assertEqual(
                    hashlib.sha256(data).hexdigest(), entry["sha256"],
                    f"{name}: bitmap of U+{entry['codepoint']:04X} changed")

    def test_bitmap_offsets_are_contiguous(self) -> None:
        # Each glyph starts on a byte boundary: LVGL reads PLAIN bitmaps from
        # glyph_dsc[i].bitmap_index, so a gap would silently shift every glyph.
        for name, _size in FONTS:
            font = self.fonts[name]
            expected = 0
            for index, glyph in enumerate(font["glyphs"]):
                self.assertEqual(glyph["bitmap_index"], expected,
                                 f"{name}: glyph {index} offset")
                size = (glyph["box_w"] * glyph["box_h"] * 4 + 7) // 8
                expected += size
            self.assertEqual(expected, len(font["bitmap"]),
                             f"{name}: trailing bitmap bytes")

    def test_visible_glyphs_contain_ink(self) -> None:
        for name, _size in FONTS:
            font = self.fonts[name]
            codepoints = sorted(tools.cmap_codepoints(font))
            for codepoint in codepoints:
                if codepoint == 0x20:
                    continue
                index = tools.glyph_index_for(font, codepoint)
                self.assertIsNotNone(index)
                pixels = tools.glyph_pixels(font, index)
                self.assertTrue(
                    any(value > 0 for value in pixels),
                    f"{name}: U+{codepoint:04X} renders blank")

    def test_fonts_share_one_character_set(self) -> None:
        sets = [tools.cmap_codepoints(font) for font in self.fonts.values()]
        self.assertEqual(sets[0], sets[1])
        self.assertTrue(set(range(tools.ASCII_FIRST, tools.ASCII_LAST + 1))
                        <= sets[0])
        self.assertLessEqual(self.inventory, sets[0])

    def test_generated_file_is_self_describing(self) -> None:
        for name, size in FONTS:
            text = font_path(name).read_text(encoding="utf-8")
            self.assertIn("Generated by tools/gen_lvgl_text_font.py", text)
            self.assertIn(f"size: {size}px", text)
            self.assertIn("bpp: 4", text)
            self.assertIn(f"const lv_font_t {name} = {{", text)


if __name__ == "__main__":
    unittest.main()
