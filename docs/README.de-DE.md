# Gad — Dokumentation

Version 0.2.0 · Deutsch

## Ein Handbuch auswählen

Für jede Sprache gibt es einen Schnellstart, ein Benutzerhandbuch und ein Entwicklerhandbuch. Die Benutzerhandbücher enthalten zweiundzwanzig Aufnahmen der echten Oberfläche im hellen Design und in ihrer jeweiligen Sprache.

| Dokument | Quelle | HTML | PDF |
| --- | --- | --- | --- |
| Schnellstart | [Markdown](QUICK_START.de-DE.md) | [HTML](html/quick-start.de-DE.html) | [PDF](pdf/quick-start.de-DE.pdf) |
| Benutzerhandbuch | [Markdown](USER_GUIDE.de-DE.md) | [HTML](html/user-guide.de-DE.html) | [PDF](pdf/user-guide.de-DE.pdf) |
| Entwicklerhandbuch | [Markdown](DEVELOPER_GUIDE.de-DE.md) | [HTML](html/developer-guide.de-DE.html) | [PDF](pdf/developer-guide.de-DE.pdf) |

Alle 18 Handbücher sind als Markdown, eigenständiges HTML und PDF verfügbar. Englische PDFs verwenden US Letter, alle anderen A4.

[Tests und Szenarien](../TESTING.de-DE.md)

## Weitere Sprachen

[Français](README.md) · [English (US)](README.en-US.md) · [Español](README.es-ES.md) · [Português](README.pt-PT.md) · [日本語](README.ja-JP.md)

## Neu erzeugen {#regeneration}

Die Aufnahmen verwenden eine Demo-Engine, temporäre Dateien und isolierte Sitzungen. Echte Übertragungen und die Zwischenablage des Desktops werden nicht gesteuert.

Für den Export werden Python mit Markdown, Graphviz und WeasyPrint benötigt. Die Aufnahmen benötigen zusätzlich die nativen Build-Abhängigkeiten von Gad.

```sh
make docs-captures
make docs
make docs-check
```

132 captures · 18 HTML · 18 PDF
