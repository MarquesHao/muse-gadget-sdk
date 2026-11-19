# Fonts

`muse_font_cjk_noto_16.c` is the CJK fallback for the caption font, built in
only with `CONFIG_MUSE_CJK_FONT`: Noto Sans SC Medium at 16 px, 4 bpp, for
CJK punctuation, kana, the 6763 hanzi of GB2312 and the fullwidth forms. It
has no Hangul, so Korean shows as boxes with it.
`tools/muse/gen_cjk_noto_font.sh` regenerates it.

Noto Sans SC is by Adobe and Google (https://github.com/notofonts/noto-cjk),
licensed under the SIL Open Font License, version 1.1. This file is a
conversion of an unaltered subset of the font.

(The GNU Unifont bitmap CJK font, the accented-Latin font and the
Vietnamese Montserrat letters that used to live here were removed with
the other unused upstream assets; Noto Sans SC is the only bundled
caption fallback now.)
