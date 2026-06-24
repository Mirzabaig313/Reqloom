# Bundled fonts

Drop the UI font files here and the app loads them automatically at startup
(`loadBundledFonts()` in `desktop/qml/main.cpp` registers every `.ttf`/`.otf`/
`.ttc` in this folder via `QFontDatabase::addApplicationFont`). No rebuild of
resources is needed during development — just place the files and relaunch.

The design system (theme.json / DesignTokens) expects:

| Family | Used for | Where to get it (OFL) |
|---|---|---|
| **Geist** | All UI text (`DesignTokens.fontSans`) | https://github.com/vercel/geist-font (Geist Sans `.ttf`/`.otf`) |
| **Noto Sans JP** | Japanese labels | https://fonts.google.com/noto/specimen/Noto+Sans+JP |
| **Geist Mono** | Code / paths (`DesignTokens.fontMono`) | same Geist repo |

Files currently bundled (the weights the app uses — 400/500/600/700):

```
Geist-Regular.ttf      Geist-Medium.ttf      Geist-SemiBold.ttf      Geist-Bold.ttf
GeistMono-Regular.ttf  GeistMono-Medium.ttf  GeistMono-SemiBold.ttf  GeistMono-Bold.ttf
NotoSansJP.ttf
```

The family names must match what `DesignTokens` references:
`Geist`, `Geist Mono`, `Noto Sans JP`.

Until the files are present the app falls back to the platform sans (you'll see
a `qt.qpa.fonts: ... missing font family "Geist"` warning, which is harmless).

For a shipped/installed build, copy these files into the app bundle at
`Contents/Resources/fonts` (the loader also checks `<app>/fonts` and
`<app>/../Resources/fonts`).
