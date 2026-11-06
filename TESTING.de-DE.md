# Testen von Gad {#testing-gad}

[Französisch](TESTING.md) · [Entwicklerhandbuch](docs/DEVELOPER_GUIDE.de-DE.md) · [Dokumentation](docs/README.de-DE.md)

## Validierung ausführen {#running-validation}

Aus dem Repository-Stammverzeichnis:

```bash
make doctor           # Check tools, native libraries, and Rust components
make test             # Formatting, Clippy, Rust, Python, Qt, and integration
```

Ohne Marke:

```bash
python3 scripts/run_tests.py all
python3 scripts/run_tests.py all --keep-going
python3 scripts/run_tests.py all --list
```

Der Läufer gibt nur dann Null zurück, wenn alle erforderlichen Etappen bestanden wurden. Normalerweise stoppt es beim ersten Fehler. `--keep-going` führt auch unabhängige spätere Phasen aus; Die Integration bleibt von einem erfolgreichen Build abhängig.

## Testgruppen {#test-groups}

| Befehl | Abdeckung |
|---|---|
| `make test-unit` | Rust- und Python-Komponententests. |
| `make test-python` | XDG-Pfade, Dolphin-Integration, Runner und Dokumentationsgenerierungslogik. |
| `make test-qt` | Zwischenablage, Widgets, Konflikte, Menüs, Fehlerberichte und Benachrichtigungen. |
| `make test-integration` | Erstellen und testen Sie die tatsächliche ausführbare Datei und das lokale Protokoll. |
| `make check` | Formatierung und Clippy mit Warnungen, die als Fehler behandelt werden. |
| `make test-bench` | Expliziter Benchmark zur Aktualisierung großer Historien. |
| `make docs-check` | Vorhandene Dokumentationslinks, Erfassungen und zweisprachige Exporte. |

Die Namen der Läufergruppen sind `unit`, `python`, `qt`, `integration`, `lint`, `bench` und `all`.

```bash
python3 scripts/run_tests.py unit --filter rollback_
python3 scripts/test_qt.py --suite row_actions
python3 scripts/test_qt.py --suite clipboard --suite notifications
cargo test --locked engine::tests::copies_tree_and_symlink_without_following_it
```

Der Runner-Filter gilt für Rust-Tests; Python-Tests werden weiterhin ausgeführt. Der Benchmark wird während der normalen Validierung ignoriert. Einzelne Wrapper-Skripte bleiben weiterhin verfügbar.

## Berichte {#reports}

Jeder Runner-Aufruf schreibt Folgendes unter `target/test-results/`:

- Eine `.log`-Datei für jede ausgeführte Stufe.
- `results.json`, einschließlich Befehlsargumenten, Exit-Codes, Status und verstrichener Zeit.
- `junit.xml`, mit **ein Eintrag pro Kommandostufe**, nicht einer pro einzelnem Rust/Qt-Test. Fälle und Behauptungen auf Framework-Ebene verbleiben in den Phasenprotokollen.

```bash
python3 scripts/run_tests.py all --report-dir /tmp/gad-results
```

JSON und JUnit beschreiben den letzten Lauf und identifizieren seine Protokolle. Möglicherweise sind noch Protokolle von älteren übersprungenen Phasen vorhanden. `target/` wird durch `.gitignore` aus den Quelldateien ausgeschlossen.

## Anforderungen {#requirements}

- Linux und aktuelles Rust/Cargo, mit Rust 1.85 oder neuer, rustfmt und Clippy.
- Ein C++17-Compiler, `pkg-config`, Qt 6 Widgets/Netzwerk/DBus/Test.
- `qmake6`, `moc`, `rcc`, KF6 WindowSystem und `dbus-run-session`.
- Python 3. Routine-Skripttests verwenden nur seine Standardbibliothek.

Die mitgelieferte Debian Trixie CI-Umgebung installiert:

```bash
sudo apt install build-essential pkg-config python3 cargo rustc rustfmt rust-clippy \
  qt6-base-dev qt6-base-dev-tools qt6-qpa-plugins qmake6 libkf6windowsystem-dev dbus fonts-dejavu-core
```

Verwenden Sie entsprechende Pakete auf anderen Distributionen. `make doctor` identifiziert fehlende Werkzeuge. Für den separaten Export der Dokumentation sind Markdown, Graphviz und WeasyPrint erforderlich. Diese sind für die sieben Standard-Dokumentations-Unit-Tests nicht erforderlich. Siehe [Dokumentationsindex](docs/README.de-DE.md#regeneration).

## Isolierung {#isolation}

Dateisystemtests verwenden temporäre Bäume. Qt läuft außerhalb des Bildschirms mit isoliertem HOME/XDG und einer privaten D-Bus-Sitzung. Bei Tests wird Gad nicht installiert, die tatsächliche Zwischenablage des Benutzers wird nicht geändert, die Desktop-Instanz wird nicht neu gestartet und es werden keine persönlichen Übertragungsdateien bearbeitet.

Desktop-Öffnungen werden über `QDesktopServices` abgefangen; Es wird kein echter Editor oder Dateimanager gestartet. Die Benachrichtigungen KIO und KDE nutzen gefälschte Dienste. Die Integration steuert nur ihre eigene isolierte ausführbare Datei. Echte FTP/SFTP/SMB-Server sind für die normale Suite nicht erforderlich.

Das Entfernen des Geräts wird durch Umbenennen temporärer Ankerverzeichnisse simuliert, niemals durch Herausziehen oder Aushängen einer echten Festplatte. Schein-Mount-Tabellen testen die Bind-Mount-Logik ohne Mount-Berechtigungen.

## Hinzufügen eines Regressionstests {#adding-a-regression-test}

| Bereich | Standort und Anfahrt |
|---|---|
| Rust-Motor | Fügen Sie `#[test]`-Fälle in `src/engine/tests.rs` hinzu. Verwenden Sie `Fixture`, warten Sie auf Geschäftsstatus und überprüfen Sie den Dateiinhalt sowie JSON. Beenden Sie die Arbeiten und schalten Sie neue Motoren nach Möglichkeit ab. |
| Zeilen-/Menü-/Öffnen-Aktionen | Fügen Sie Slots oder Datenzeilen in `tests/row_actions.cpp` hinzu; Verspotten Sie die Engine und der Intercept-Desktop wird geöffnet. |
| Verhalten der Zwischenablage und des Fensters | Erweitern Sie `tests/clipboard.cpp`. |
| D-Bus-Fortschritt | Erweitern Sie `tests/notifications.cpp` mit dem privaten gefälschten JobView-Server. |
| Python-Skripte | Fügen Sie `tests/python/test_*.py` mithilfe von `unittest`, `TemporaryDirectory` und Umgebungs-/Unterprozess-Mocks hinzu. Die Erkennung erfolgt automatisch. |
| Neue Qt-Suite | Fügen Sie seinen Namen zu `SUITES` in `scripts/qt_tests.py`, seiner C++-Quelle und dem passenden generierten Moc-Include hinzu. |

Testen Sie beobachtbares Verhalten: resultierende Zustände, korrekte Bytes, beibehaltene Originale, wiederhergestellte Ziele, Desktop-Anforderungsargumente und das Fehlen unbeabsichtigter Änderungen. Bevorzugen Sie ein Signal-/Zustandsprädikat gegenüber willkürlichen Verzögerungen. Führen Sie keine Tests durch, indem Sie die echte Desktop-Anwendung steuern.

## Kontinuierliche Integration {#continuous-integration}

`.github/workflows/tests.yml` bietet eine vollständige Validierung in einem Debian-Trixie-Container und speichert Fehlerberichte. Es wird aktiv, wenn das Projekt in einem geeigneten GitHub-Repository gehostet wird. Die Konfiguration wird mitgeliefert; Hier wird keine Remote-CI-Ausführung beansprucht.

## Schnelle lokale Bewegungen und Bindung von Reittieren {#fast-local-moves-and-bind-mounts}

```bash
python3 scripts/run_tests.py unit --rust-filter fast_
```

`fast_move`-Fälle prüfen Inodes nach Übertragung und Rückgängigmachen, Ausführung neben Kopieren, unabhängige Pfade, die einen Zielordner teilen, Namensreservierungen nach Konflikten, überlappende Pfade, MD5-Fallback, gleichzeitige Ziele, EXDEV-Neuplanung, Abbruch, Ersetzungsketten und Journalwiederherstellung. Eine Änderung gleicher Länge mit wiederhergestellter mtime muss immer noch dazu führen, dass das Rückgängigmachen ein geändertes Ergebnis ablehnt.

Schnelle Geräte verwenden beschreibbare Linux-tmpfs bei `/dev/shm`, sodass ein überlagertes `/tmp` in CI nicht versehentlich den lokalen Umbenennungspfad deaktiviert. Das injizierte Umbenennungsverhalten simuliert EXDEV, ohne Dateien auf ein anderes Volume zu verschieben.

`fast_bind_mounts` verwendet synthetische Mount-Tabellen für unterschiedliche Bindungsansichten und ihre gemeinsame zugrunde liegende Ansicht. Die Tests umfassen die Übersetzung in beide Richtungen, fehlende gemeinsame Offenlegung, unterschiedliche Dateisysteme, Lesebeschränkungen und Zielsuffixe, die noch nicht existieren.

`fast_tree` bietet die Beibehaltung des gesamten Root-Inodes/Berechtigungen, leere Ordner, mehrere Roots, gezielte Pausen, teilweises Rückgängigmachen, später hinzugefügte externe Dateien, geänderte Inhalte und Wiederherstellung vor/nach der Root-Veröffentlichung. Verschachtelte Mounts, einschließlich solcher, die in einer gemeinsamen Ansicht ausgeblendet sind, verhindern die Verknüpfung.

## Teilweise Erholung nach Unterbrechung {#partial-recovery-after-interruption}

```bash
cargo test --locked resume_
python3 scripts/test_resume_integration.py
```

Erstellen Sie die ausführbare Datei, bevor Sie ein eigenständiges Integrationsskript ausführen. Die Tests umfassen dauerhafte Punktreihenfolge, Quell- und temporäre Besitz-/Versionsvalidierung, Präfixvergleich, MD5-Rekonstruktion, Abschneiden nachfolgender Bytes erst nach Validierung und Beibehaltung der temporären Identität. Geänderte Inhalte oder Metadaten müssen fehlschlagen, ohne dass eine fragwürdige Datei abgeschnitten wird.

Die Ist-Binär-Integration verwendet einen isolierten Prozess, SIGKILL, Relaunch und Byte/Digest-Vergleich, um zu überprüfen, ob dieselbe Teildatei fortgesetzt wird. Die Gad-Instanz des Benutzers wird niemals beendet. Ausdrücklich abgesagte Arbeiten dürfen nicht zu wiederherstellbaren automatischen Arbeiten werden.

## Entlegene Ziele {#remote-destinations}

```bash
make test-remote
```

Für diesen optionalen Test im realen Netzwerk sind die Sicherungen `pyftpdlib`, KIO und verwendbare `/dev/fuse` erforderlich. Bei der normalen Validierung werden stattdessen gefälschte KIO-Dienste verwendet. Der optionale Server und die Dateien bleiben in einer temporären Umgebung; Es handelt sich nicht um einen Test mit einem persönlichen Remote-Konto.

Überprüfen Sie die URL-Validierung, beide Endpunktzuordnungen, Vorgangs-/Ursprungsoptionen, nicht unterstützte Dienstmetadaten, Synchronisierungssemantik und deaktivierte dauerhafte KIO-Zielwiederherstellung. Tests dürfen nicht verfügbare Eigenschaften oder Kapazitäten nicht als fabrizierte Erfolgsgarantien behandeln.

## Dolphin Strg+V und Desktop-Fortschritt {#dolphin-ctrlv-and-desktop-progress}

```bash
make test-dolphin-paste
```

Für den optionalen Integrationstest sind CMake-, Qt 6 Widgets/Test- und KF6 XmlGui-Entwicklungsdateien erforderlich. Es verwendet echtes KXMLGUI/KActionCollection in isolierten HOME/XDG-, D-Bus- und Offscreen-Zwischenablageumgebungen. Aktionen sind Zähler: Es findet keine echte Kopie oder kein Neustart des Benutzers Dolphin statt.

Es überprüft Gad Strg+V, die Beibehaltung der nativen Umschalt+Einfg-Alternative KIO, das Einfügen nativer Textfelder, die Wiederherstellung von Strg+V-Einstellungen nach der Deaktivierung, die vollständige Validierung vor dem Ändern der Konfiguration, die Ablehnung ungültiger Belege und das Rollback nach Schreibfehlern.

Zwischenablagetests injizieren Dolphin-Sitzungsantworten für eindeutige aktive Fenster, Dienstaliase, Leerzeichen und `#`, Kurztitel, geschlossene Ordner, Navigations-/Fokusänderungen, nicht verfügbare Dienste und Remote-URLs. Python-Tests decken die Migration/Wiederherstellung der Integration ab, ohne nicht damit zusammenhängende Aktionen zu ändern.

Benachrichtigungstests üben Befehle aus, die über den mit der Engine verbundenen Controller zurückgegeben werden, synchronisierte Pausenbeschriftungen/Fachstatus, eine einzige kombinierte Fortschrittsansicht, Geschwindigkeit in Details, MD5-Abschlussabrechnung, Dienstwiederverbindung und der Übergang von `suspended` zurück auf „Falsch“. Gewöhnliche Benachrichtigungen dürfen die native Jobansicht nicht duplizieren.

## Freiraumprüfungen, Prioritäten und Bäume {#free-space-checks-priorities-and-trees}

Space-Tests umfassen gemeinsame Budgets pro Volume, separate Volumes, Blockrundung, Überlauf, zugewiesene Teildateien, Optionspersistenz und schnelle Umbenennungen. Eine spärliche tmpfs-Einrichtung führt zu einem Mangel, ohne die Festplatte zu füllen. Unbekannte simulierte Remote-Kapazität warnt und schließt ab. Qt prüft erfasste Optionen und eine Warnung bleibt nach einem OK-Ergebnis sichtbar.

Bei Prioritätstests wird der tatsächliche Planer für eine stabile Mehrfachanforderungsreihenfolge, pausierte Arbeit, atomare Ablehnung veralteter Auswahl und korrektes Rückgängigmachen nach neu angeordneten Ersetzungen verwendet. Die IPC-Integration ordnet Anfragen in der Warteschlange neu und stellt sie wieder her, bevor mit den Kopien fortgefahren wird.

Widget-Tests überprüfen die Header-freie Gesamtstruktur, gemeinsame Anforderungsakzente in hellen/dunklen Themen, die Auswahl ganzer Anforderungen, stabile interne Drag-Referenzen, die Ablehnung von Fremd-Drags, die Beibehaltung der Elternschaft/Ziele und die Stabilität von Auswahl/Hover/Erweiterung. Engine-Tests umfassen abwechselnde unabhängige Wurzeln, durch Konflikte offengelegte Geschwisterreihenfolge, ungültige Neuzuordnung, Platzhalterprioritäten und Ersetzungsabhängigkeiten während der Pause/Finalisierung.

Native Qt-Objekte werden im Inhalt unter `target/qt-test-cache` mit Quelle/Header, Qt/KDE-Version und Compiler-Identitäts-Hashes zwischengespeichert. Eine Build-Sperre verhindert den gleichzeitigen Cache-Aufbau. Ausführbare Testdateien, Dateien, Konfigurationen und D-Bus-Sitzungen bleiben aktuell und isoliert.

Zu den normalisierten Zielabhängigkeiten gehören übergeordnete Aliase, wiederholte Namen, selektive Pausen und zukünftige Kopienamenreservierungen. Ordnereigenschaftswarnungen fassen nur ihre eigenen tatsächlichen Nachkommen zusammen.

## Unterdrückung doppelter Übertragungen {#duplicate-transfer-suppression}

`duplicates_`-Fälle umfassen wiederholt angehaltene Einfügungen, gleichzeitige Vorbereitung, Duplikate innerhalb einer Übermittlung, überlappende Ordner, übergeordnete Aliase und wiederhergestellte stabile Indizes. Sie bewahren unterschiedliche Ziele, Kopier-/Verschiebeabsichten, fest verknüpfte Verzeichniseinträge mit unterschiedlichen Namen und absichtliche neue Anforderungen nach Abschluss/Abbruch.

Qt prüft eine einzelne Informationsmeldung von Nur-Metadaten-Updates ohne wiederholte Aufforderungen. Konfliktszenarien verwenden wirklich unterschiedliche Quellen, die einen Dateinamen teilen, sodass sie Konflikte testen und nicht die Unterdrückung von Duplikaten.

## Bandbreitengrenzen und -schätzungen {#bandwidth-limits-and-estimates}

`engine::rate` verwendet eine injizierte Uhr, um gemeinsam/globale und per-Request-Budgets zu überprüfen, gebundene Leergutschrift, Gesamtverbrauch und Live-Änderungen. Motortests führen eine echte limitierte Kopie durch, entfernen Sie die Grenze und überprüfen Sie die Ausgabe. Pause, niedrig-rate Stornierung, atomare Validierung, Beharrlichkeit und Erhaltung einer neueren Grenze während der Retry-Präparation abgedeckt sind.

Qt prüft die MiB-zu-bytes-Konvertierung und ein Ziel pro Eigenwunsch auch dann, wenn mehrere seiner Zeilen ausgewählt werden. Die Änderung der Auswahl während des geöffneten Rate-Dialogs darf seine erfassten IDs nicht ändern.

Deterministische Metriktests decken unabhängige gleichzeitige Anforderungsraten/Zeiten ab, Kopie gegen MD5 Phasen, Pausen/Konflikte, Umbenennungen, wieder aufgenommene Präfixe, Zählerrückstellungen, gestoppte Fortschritte und Dauerformatierung. Das eigentliche Zusammenfassungs-Widget aktualisiert auf `poll.unchanged`, ohne Zeilen oder deren Auswahl/Werkzeuge zu ersetzen.

## Wiederverschaltung von Geräten {#device-reconnection}

`reconnect_`-Fälle simulieren das Verschwinden von Quellen/Destinationen durch Umbenennen von temporären Ankern. Sie decken validierte dauerhafte Präfix-Recovery, gehaltene Pausen, keine Schreibweise in ein leeres Ersatzverzeichnis, geänderte Quellenverweigerung, Löschung und freigesetzte Arbeiter für unabhängige Arbeit.

Weitere Fälle umfassen den Neustart einer eigenen unveröffentlichten Datei ohne dauerhaften Punkt, die Verweigerung der Veröffentlichungswiedergabe, UUID-autorisierte Gerätenummernänderungen, erhaltene Metadaten/Content-Verdauungen während der Wiederverbindung und gleichzeitige Pausen-/cancel-Befehle während der Vorbereitung. Kein reales Dateisystem ist montiert oder nicht montiert.

Qt überprüft Zeilen- und Fortschrittsetiketten, Stornoverfügbarkeit, eine hängende native Jobansicht und asynchrone KIO Auflösung, die weder Arbeit noch das Fenster erhöht.

## Fehlerberichte und Retries {#error-reports-and-retries}

`errors_`-Tests umfassen nur Leseberichte, Vorbereitungs-/Endbearbeitungsausfälle, atomare Validierung, Stale-Indizes, genaue Berichtsreferenzen, reparierte Retry-Präparation und geänderte Offenlegungsschrifts-Schutz. Eine echte teilweise fehlgeschlagene Anfrage überprüft, dass der erfolgreiche Nachbarinhalt, Inode, Zeitstempel und Journal nach dem Retry unverändert bleiben.

Globale Pause, stornierte/skippte Einträge und unausgewählte anstehende Arbeit werden erhalten. Directory-Aggregation vermeidet doppelte Fehleraufzeichnungen unter Beibehaltung tatsächlicher Ordnerausfälle.

Qt verifiziert Filter- und stabile Anforderungsziele nach Sortieren, kontextuellen Retry-Aktionen, leeren Berichten, vollem UTF-8 Text, parseable JSON und atomaren Bericht schreibt. Exporte verwenden nur temporäre Pfade. Jede fehlerdialog-Taste wird mit einem bewusst nicht verfügbaren Symbol-Thema überprüft, um Standard-Qt Fallback-Symbole zu trainieren.

## Dokumentation erzeugen {#documentation-generation}

```sh
make docs-captures
make docs
make docs-check
```

Die Dokumentationstests prüfen den Katalog mit sechs Sprachen und drei Handbuchtypen, Sprachwechsel, Papierformate, Bildunterschriften und eingebettete Bilder. Außerdem prüfen sie das kompakte Drucklayout der Schnellstarts, die eigenen Bildschirmaufnahmen jeder Sprache und die unveränderten Codebeispiele.

Der Build erzeugt übersetzte Diagramme und 18 Handbücher. Anschließend werden lokale Links und Sprungziele, die 78 Aufnahmen im hellen Design mit Abmessungen und Prüfsummen, HTML-Sprachangaben, eingebettete Bilder und alle 36 HTML-/PDF-Ausgaben geprüft. Dafür sind die im Dokumentationsindex genannten optionalen Abhängigkeiten nötig.

[Documentation](docs/README.de-DE.md#regeneration)

## Übersetzungen der Bewerbung {#application-translations}

Fünf Python-Tests mit Qt String-Abdeckung, TS/QM Fingerabdrücke, Ausschluss von Kommentaren und nicht markierten Kennungen, fehlende Übersetzungen und Erhaltung von Parametern und Links. Qt tests laden Sie den real eingebetteten französischen Katalog und überprüfen Sie Widgets, Zustände, Spracheinstellungen, Textberichte und unveränderte Pfade/JSON. Leerberichte Export und Auffrischungs-/Filterübergänge sind ebenfalls abgedeckt. Laufen `make translations-check` für eine unabhängige Prüfung.

Die Live-Sprache Qt-Tests wechseln während eines aktiven Transfers mit unveränderter Umfrageantwort Französisch und US Englisch ab. Sie verifizieren Zeilenidentität, Auswahl, Erweiterung, Ziel-/Rate-Eingänge, Über/Fehler/Report/Rate-Dialoge, Konfliktwahlen, stabile Bindungszählungen und das Fehlen von Motor-Schreibbefehlen.

## Ausschlüsse und versteckte Gegenstände {#exclusions-and-hidden-items}

Rust-Tests umfassen Unicode-Muster, Atomvalidierung, Vermächtnisvoraussetzungen, Ordnerschnitt, temporäre Dateien, symbolische Links, Per-Job-Regelbeharrlichkeit, Retries, vollständig ausgeschlossene Anfragen und bewegt sich gefolgt von undo. Qt-Tests decken gespeicherte Präferenzen, gelöschten und abgelehnten Edits, Live-Übersetzungen und Regelerfassung für neue Anfragen ab.

```sh
cargo test --locked exclusions
python3 scripts/test_qt.py --suite row_actions
```

## Konqueror Integration {#konqueror-integration}

`python3 scripts/test_qt.py --suite konqueror` überprüft den aktuellen Ordner, kodierte und Remote-URLs, Cut-Mode-Bewahrung, natives Text-Patent und Shift+Insert in einer isolierten Umgebung. Konqueror 26.08.2 in ein temporäres Verzeichnis mit privaten HOME/XDG und D-Bus extrahiert; die genaue Active-folder URL wurde überprüft. `target/konqueror-real-test.log`.

## Einstellungsmenü {#settings-menu}

Qt-Tests umfassen die Kontrollgruppierung, die Checkbox-Klicks halten das Menü offen, die Live-Übersetzung und die Auswahl-basierten Ratengrenzen. Ein gefälschter Shortcut-Helfer in einem privaten PATH-Checks erfolgreich und abgelehnt Dolphin-Änderungen ohne Änderung von Desktop-Einstellungen. Die Konqueror-Suite prüft sofortiges Abschalten und Wiedereinschalten des Ctrl+V-Filters.
