# Gad — Documentation

Version 0.4.0 · English (US)

## Choose a guide

Each language has a Quick Start, User Guide, and Developer Guide. User guides include twenty-two screenshots of real widgets in the light theme, in their own language.

| Document | Source | HTML |
| --- | --- | --- |
| Quick Start | [Markdown](QUICK_START.en-US.md) | [HTML](html/quick-start.en-US.html) |
| User Guide | [Markdown](USER_GUIDE.en-US.md) | [HTML](html/user-guide.en-US.html) |
| Developer Guide | [Markdown](DEVELOPER_GUIDE.en-US.md) | [HTML](html/developer-guide.en-US.html) |

All 18 guides are available as Markdown and standalone HTML.

[Tests and scenarios](../TESTING.en-US.md)

## Other languages

[Français](README.md) · [Español](README.es-ES.md) · [Português](README.pt-PT.md) · [Deutsch](README.de-DE.md) · [日本語](README.ja-JP.md)

## Regeneration {#regeneration}

Captures use a demo engine, temporary files, and isolated sessions. They never control real desktop transfers or the clipboard.

Export requires Python with Markdown. Captures also require Gad’s native build dependencies.

```sh
make docs-captures
make docs
make docs-check
```

138 captures · 18 HTML
