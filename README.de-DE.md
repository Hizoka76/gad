# Gad {#gad}

[Français](README.md) · [English (US)](README.en-US.md) · [Español](README.es-ES.md) · [Português](README.pt-PT.md) · [Deutsch](README.de-DE.md) · [日本語](README.ja-JP.md)

**Gad, il copie.** — **Gestionnaire Avancé de Duplication** (Erweiterter Duplizierungsmanager)

[Französisch](README.md) · [Schnellstart](docs/QUICK_START.de-DE.md)

Gad bringt Dateikopien und -verschiebungen in ein natives KDE Plasma-Fenster. Seine Übertragungs-Engine ist in Rust geschrieben und verfügt über eine C++/Qt 6 Widgets-Schnittstelle.

- Benutzeroberflächen für Französisch, US-Englisch, Spanisch, Portugiesisch, Deutsch und Japanisch mit einer gespeicherten Spracheinstellung.
- Eine detaillierte Warteschlange mit Ordnerbäumen, Prioritäten und verschiebbaren Zeilen.
- Bis zu zwei kompatible Übertragungen parallel, schnelle Umbenennungen und automatische Unterdrückung von Duplikaten.
- Pausenkontrollen, Bandbreitenbeschränkungen, Schätzungen pro Anfrage und Plasma-Fortschrittsverfolgung.
- Lesbare Konflikte, MD5/Property/Free-Space-Prüfungen und Verlauf mit Rückgängigmachen.
- Wiederherstellung teilweiser Dateien, Warten auf erneute Geräteverbindung und auf fehlgeschlagene Dateien beschränkte Wiederholungsversuche.
- Lokale und entfernte Quellen und Ziele über KIO Fuse: KDE Connect, FTP, SFTP und SMB.

## Dokumentation {#documentation}

Diese README-Datei ist ein Ausgangspunkt. Die Anleitungen beschreiben das vollständige Verhalten und die Einschränkungen, sodass detaillierte Anweisungen nicht zweimal gepflegt werden müssen.

| Publikum | Englisch (USA) | Französisch |
|---|---|---|
| Benutzer | [Illustriertes Benutzerhandbuch](docs/USER_GUIDE.de-DE.md) | [Leitfaden illustriert](docs/GUIDE_UTILISATEUR.md) |
| Entwickler | [Architektur und Protokoll](docs/DEVELOPER_GUIDE.de-DE.md) | [Architektur und Protokoll](docs/GUIDE_DEVELOPPEUR.md) |
| Testen | [Validierung](TESTING.de-DE.md) | [Tests](TESTING.md) |

[Alle HTML- und PDF-Editionen](docs/README.de-DE.md)

## Installation {#installation}

Installieren Sie unter Linux Rust/Cargo, einen C++17-Compiler, `pkg-config`, Qt 6 Widgets/Network/DBus und Qt Build-Tools, KF6 WindowSystem und Python 3. Die Unterstützung der Wayland-Zwischenablage verwendet `wl-clipboard`; Remote-URLs erfordern KIO Fuse. Siehe [Entwicklerhandbuch](docs/DEVELOPER_GUIDE.de-DE.md#building).

```bash
python3 scripts/install.py
gad
```

Die Installation erfolgt pro Benutzer und erfordert keine Administratorrechte. Verwenden Sie `gad --background`, um in der Taskleiste zu starten.

## Kopieren von Dolphin {#copying-from-dolphin}

1. Kopieren Sie mit **Strg+C** oder schneiden Sie mit **Strg+X** aus, um Dateien zu verschieben.
2. Klicken Sie mit der rechten Maustaste auf den Zielordner und wählen Sie **Paste mit Gad** (Einfügen mit Gad).
3. Wiederholen Sie diesen Vorgang, um der Warteschlange eine weitere Anfrage hinzuzufügen.

Das optionale **Integration von Strg+V in Dolphin** wird mit `gad-dolphin-paste --enable` aktiviert und mit `gad-dolphin-paste --disable` deaktiviert. Schließen Sie Dolphin und öffnen Sie es erneut, nachdem Sie es geändert haben. In Gad wird Strg+V in das angezeigte Ziel eingefügt; Drag-and-Drop kopiert Dateien.

Für andere Dateimanager: [Pasting in Gad, Drag-and-Drop und benutzerdefinierte Aktionen](docs/USER_GUIDE.de-DE.md#using-gad-with-other-file-managers).

Das Rückgängigmachen hängt vom aufbewahrten Journal und den Backups von Gad ab. **Löschen Sie abgeschlossene Vorgänge** (Abgeschlossene Vorgänge löschen) entfernt auch deren Rückgängig-Historie. Lesen Sie [Bedienungsanleitung](docs/USER_GUIDE.de-DE.md), bevor Sie den Verlauf löschen, den Sie möglicherweise benötigen.

## Entwicklung und Validierung {#development-and-validation}

```bash
make doctor
make test
make docs
```

Tests verwenden temporäre Dateien und isolierte Sitzungen. Sie steuern nicht Ihre Desktop-Instanz von Gad. [Testen von Befehlen und Anforderungen](TESTING.de-DE.md).
