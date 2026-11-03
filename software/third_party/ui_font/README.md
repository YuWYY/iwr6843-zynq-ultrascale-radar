# Embedded UI font

`NotoSansCJKsc-Regular.otf` is the unmodified Noto Sans CJK SC Regular 2.004 font,
copyright 2014–2021 Adobe, under SIL Open Font License 1.1. The complete license
is preserved in `LICENSE.txt`. Pinned upstream paths and file hashes are in
`provenance.json`.

`Radar UI Bitmap Subset` is the local derivative name. Its generated coverage
table remains OFL-1.1; the independent C renderer is MIT. Include this directory's
license and copyright notice with source or firmware distribution. The font is
bundled with the application, not offered as a separately sold font.

The application embeds only a glyph subset at 13, 18, 20, 26 and 32 pixels.
There is no runtime font loader, network request, global font installation or
Microsoft font redistribution. The original OTF is a build input, not loaded on
the target. Existing ASCII measurement digits keep the original bitmap alphabet.

Run `python tools/generate_ui_font.py` after changing display text; use `--check`
to prove the checked-in header matches its source and generation environment.
`subset_manifest.json` records used characters by font size, generation versions,
binary size and hashes. If introducing a new indirect string at another size,
add its function role to the generator and regenerate.
