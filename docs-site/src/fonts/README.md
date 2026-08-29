# Bundled webfonts

Latin-subset WOFF2 builds of **Geist** and **Geist Mono**, the same families the
desktop app uses (`DesignTokens::fontSans` / `fontMono`), so the docs and the
product render in one typeface.

Upstream: <https://github.com/vercel/geist-font> — SIL Open Font License 1.1.
The full licence text ships with the upstream repo; these are unmodified
glyph subsets of those files, redistributed under the same terms.

## Why subsets

The source TTFs in `desktop/fonts/` are ~128 KB each; all six faces would be
~780 KB. Subset to latin plus the punctuation the docs actually use, they come
to ~110 KB total.

## Regenerating

Only needed when the upstream fonts are updated or a new glyph range is
required (a new language, a new box-drawing character in a diagram).

```bash
python3 -m venv /tmp/fontenv
/tmp/fontenv/bin/pip install "fonttools[woff]" brotli

# From the repo root:
for f in Geist-Regular Geist-Medium Geist-SemiBold Geist-Bold \
         GeistMono-Regular GeistMono-SemiBold; do
  /tmp/fontenv/bin/pyftsubset "desktop/fonts/$f.ttf" \
    --output-file="docs-site/src/fonts/$f.woff2" \
    --flavor=woff2 --desubroutinize \
    --layout-features='kern,liga,calt,tnum,ss01' \
    --unicodes='U+0000-00FF,U+0131,U+0152-0153,U+02BB-02BC,U+02C6,U+02DA,U+02DC,U+0304,U+0308,U+0329,U+2000-206F,U+2074,U+20AC,U+2122,U+2191,U+2193,U+2192,U+2190,U+2212,U+2215,U+FEFF,U+FFFD,U+2713,U+2717,U+2500-257F'
done
```

## Why `src/fonts/` and not `public/fonts/`

`src/` assets go through Vite, so the emitted URLs are hashed and automatically
prefixed with the deploy `BASE`. Files in `public/` are served verbatim, which
would mean hardcoding `/Reqloom/fonts/...` in CSS and breaking the org-page
deploy where `BASE` is `/`.

Note that `calt` is included in the subset but switched **off** for code via
`font-variant-ligatures: none` in `custom.css` — Geist Mono's contextual
alternates fuse `--` and swallow the preceding space, which would render
`cmake --preset` as `cmake--preset`.
