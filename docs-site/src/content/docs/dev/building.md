---
title: Building from source
description: "Full build instructions for macOS, Linux, Windows. Prerequisites, troubleshooting, presets."
---

See the [installation guide](/start/install/) for end-to-end build instructions. Quick reference:

| Platform | Preset | Notes |
|---|---|---|
| macOS | `macos-debug` / `macos-release` | Uses Homebrew Qt; vcpkg for everything else |
| Linux | `linux-debug` / `linux-release` | Full vcpkg-based; Qt built from source on first run |
| Windows | `windows-debug` / `windows-release` | Visual Studio 2022 + Qt online installer |

CI uses Linux + Windows presets with full vcpkg builds. Local macOS uses Homebrew Qt for speed.

Full content for this page is part of Phase 2 documentation.
