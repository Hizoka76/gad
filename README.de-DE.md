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


**0.3.0** — Wählen Sie Ziel und Optionen und fügen Sie Dateien in Gad hinzu. Wenn nichts läuft, prüfen Sie die Liste und klicken Sie auf **Starten**. Externes Einfügen aus dem Dateimanager startet automatisch; während einer Übertragung werden neue Aufträge eingereiht. **Strg+C** kopiert, **Strg+X** verschiebt. Ziehen und Ablegen bereitet immer eine Kopie vor.

## Dokumentation {#documentation}

Diese README-Datei ist ein Ausgangspunkt. Die Anleitungen beschreiben das vollständige Verhalten und die Einschränkungen, sodass detaillierte Anweisungen nicht zweimal gepflegt werden müssen.

| Publikum | Englisch (USA) | Französisch |
|---|---|---|
| Benutzer | [Illustriertes Benutzerhandbuch](docs/USER_GUIDE.de-DE.md) | [Leitfaden illustriert](docs/GUIDE_UTILISATEUR.md) |
| Entwickler | [Architektur und Protokoll](docs/DEVELOPER_GUIDE.de-DE.md) | [Architektur und Protokoll](docs/GUIDE_DEVELOPPEUR.md) |
| Testen | [Validierung](TESTING.de-DE.md) | [Tests](TESTING.md) |

[Alle HTML- und Markdown-Editionen](docs/README.de-DE.md)

## Installation {#installation}

### Ubuntu-PPA

Das [Gad-PPA](https://launchpad.net/~hizo/+archive/ubuntu/gad) ermöglicht die Installation und Aktualisierung von Gad über APT:

```sh
sudo add-apt-repository ppa:hizo/gad
sudo apt update
sudo apt install gad
```

Prüfen Sie auf der PPA-Seite, ob für Ihre Ubuntu-Version ein Paket veröffentlicht wurde. Falls `add-apt-repository` fehlt, installieren Sie `software-properties-common`.

### Vorkompilierte Downloads

Die [GitHub-Releases](https://github.com/Hizoka76/gad/releases/latest) bieten mehrere Formate. Ersetzen Sie die Beispieldateinamen unten durch die Namen Ihrer heruntergeladenen Dateien.

#### Debian-Paket `.deb`

Laden Sie das Paket für Ihre Architektur herunter und installieren Sie es mit APT, um verfügbare Abhängigkeiten aus Ihren Paketquellen aufzulösen:

```sh
sudo apt install ./gad_VERSION_amd64.deb
```

#### `.tar.gz`-Archiv: Quellen und kompiliertes Programm

Das Archiv `gad-VERSION-linux-amd64.tar.gz` enthält den Quellcode, eingebundene Rust-Abhängigkeiten, Dokumentation und das kompilierte Programm unter `prebuilt/`. Zum Starten ist keine Kompilierung erforderlich:

```sh
tar -xzf gad-VERSION-linux-amd64.tar.gz
cd gad-VERSION
./prebuilt/usr/bin/gad
```

#### Direkte Programmdatei

Auch `gad-linux-amd64` ist eine bereits kompilierte Programmdatei:

```sh
chmod +x gad-linux-amd64
./gad-linux-amd64
```

Linux-amd64-Downloads sind für x86-64-Prozessoren bestimmt. Die direkte Programmdatei installiert weder einen Starter noch das Dolphin-Modul; das `.deb` und das Archiv enthalten die Integrationsdateien der jeweiligen Version. Die Programme benötigen Qt/KF6 in den in den Release-Hinweisen angegebenen Versionen. Ein Release entspricht seinem Tag und kann älter als der aktuelle Repository-Code sein.

### Aus dem Quellcode

Installieren Sie unter Linux Rust/Cargo, einen C++17-Compiler, `pkg-config`, Qt 6 Widgets/Network/DBus und Qt Build-Tools, KF6 WindowSystem/XmlGui, CMake und Python 3. Die Unterstützung der Wayland-Zwischenablage verwendet `wl-clipboard`; Remote-URLs erfordern KIO Fuse. Siehe [Entwicklerhandbuch](docs/DEVELOPER_GUIDE.de-DE.md#building).

```bash
python3 scripts/install.py
gad
```

Die Installation erfolgt pro Benutzer und erfordert keine Administratorrechte. Verwenden Sie `gad --background`, um in der Taskleiste zu starten.

## Kopieren von Dolphin {#copying-from-dolphin}

1. Kopieren Sie mit **Strg+C** oder schneiden Sie mit **Strg+X** aus, um Dateien zu verschieben.
2. Klicken Sie mit der rechten Maustaste auf den Zielordner und wählen Sie **Paste mit Gad** (Einfügen mit Gad).
3. Wiederholen Sie diesen Vorgang, um der Warteschlange eine weitere Anfrage hinzuzufügen.

Die optionale **Ctrl+V-Integration mit Gad in Dolphin** wird mit `gad --configure --dolphin-ctrl-v` aktiviert und mit `gad --configure --no-dolphin-ctrl-v` deaktiviert. Dolphin anschließend schließen und erneut öffnen. In Gad fügt Ctrl+V Dateien am angezeigten Ziel ein; Ziehen und Ablegen kopiert Dateien.

Zum Schutz des normalen Dolphin-Starters vor verlorenen Tastenkürzeln das Modul mit dem folgenden Befehl installieren und bereits geöffnete Fenster einmal schließen und erneut öffnen. Das Modul prüft Tastenkürzel jede Sekunde, repariert die Gad-Aktion und erhält natives Einfügen in Textfeldern.

```sh
python3 scripts/dolphin_integration.py --install --protect-launcher "$HOME/.local/bin/gad"
```


Für andere Dateimanager: [Pasting in Gad, Drag-and-Drop und benutzerdefinierte Aktionen](docs/USER_GUIDE.de-DE.md#using-gad-with-other-file-managers).

Das Rückgängigmachen hängt vom aufbewahrten Journal und den Backups von Gad ab. **Löschen Sie abgeschlossene Vorgänge** (Abgeschlossene Vorgänge löschen) entfernt auch deren Rückgängig-Historie. Lesen Sie [Bedienungsanleitung](docs/USER_GUIDE.de-DE.md), bevor Sie den Verlauf löschen, den Sie möglicherweise benötigen.

## Entwicklung und Validierung {#development-and-validation}

```bash
make doctor
make test
make docs
```

Tests verwenden temporäre Dateien und isolierte Sitzungen. Sie steuern nicht Ihre Desktop-Instanz von Gad. [Testen von Befehlen und Anforderungen](TESTING.de-DE.md).
