# Gad — Documentation

Version 0.2.0 · English (US)

## Choose a guide

Each language has a Quick Start, User Guide, and Developer Guide. User guides include twenty-two screenshots of real widgets in the light theme, in their own language.

| Document | Source | HTML | PDF |
| --- | --- | --- | --- |
| Quick Start | [Markdown](QUICK_START.en-US.md) | [HTML](html/quick-start.en-US.html) | [PDF](pdf/quick-start.en-US.pdf) |
| User Guide | [Markdown](USER_GUIDE.en-US.md) | [HTML](html/user-guide.en-US.html) | [PDF](pdf/user-guide.en-US.pdf) |
| Developer Guide | [Markdown](DEVELOPER_GUIDE.en-US.md) | [HTML](html/developer-guide.en-US.html) | [PDF](pdf/developer-guide.en-US.pdf) |

All 18 guides are available as Markdown, standalone HTML, and PDF. English PDFs use US Letter; other editions use A4.

[Tests and scenarios](../TESTING.en-US.md)

## Other languages

[Français](README.md) · [Español](README.es-ES.md) · [Português](README.pt-PT.md) · [Deutsch](README.de-DE.md) · [日本語](README.ja-JP.md)

## Regeneration {#regeneration}

Captures use a demo engine, temporary files, and isolated sessions. They never control real desktop transfers or the clipboard.

Export requires Python with Markdown, Graphviz, and WeasyPrint. Captures also require Gad’s native build dependencies.

```sh
make docs-captures
make docs
make docs-check
```

132 captures · 18 HTML · 18 PDF
