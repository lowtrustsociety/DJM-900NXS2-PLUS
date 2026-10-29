# Ioskeley Mono

The font of the patcher page (`tools/web_patcher_template.html`): Ioskeley
Mono by Renzhi Li (Belleve Invis), version 34.4.0, under the SIL Open Font
License 1.1 (`OFL.txt`). `tools/make_patcher.py` puts these files into the
page as they are.

- `IoskeleyMono-Regular.woff2` (weight 400) and `IoskeleyMono-Bold.woff2`
  (700): unchanged.
- `IoskeleyMono-Medium.woff2` (500): cut down to the characters of the
  Regular one (it held about six times as many), with its name, copyright
  and licence kept:

      pyftsubset IoskeleyMono-Medium.woff2 --unicodes=<the Regular file's characters> \
          --layout-features='*' --name-IDs='*' --name-legacy --name-languages='*' \
          --notdef-outline --glyph-names --flavor=woff2
