# Gad

[Français](README.md) · [English (US)](README.en-US.md) · [Español](README.es-ES.md) · [Português](README.pt-PT.md) · [Deutsch](README.de-DE.md) · [日本語](README.ja-JP.md)

**Gad, il copie.** — **Gestionnaire Avancé de Duplication** (Advanced Duplication Manager)

[Français](README.md) · [Quick Start](docs/QUICK_START.en-US.md)

Gad brings file copies and moves into one native KDE Plasma window. Its transfer engine is written in Rust, with a C++/Qt 6 Widgets interface.

- French, US English, Spanish, Portuguese, German, and Japanese interfaces with a saved language preference.
- A detailed queue with folder trees, priorities, and draggable rows.
- Up to two compatible transfers in parallel, fast renames, and automatic duplicate suppression.
- Pause controls, bandwidth limits, per-request estimates, and Plasma progress tracking.
- Readable conflicts, MD5/property/free-space checks, and history with undo.
- Partial-file recovery, device reconnection waiting, and retries limited to failed files.
- Local and remote sources and destinations through KIO Fuse: KDE Connect, FTP, SFTP, and SMB.

## Documentation

This README is a starting point. The guides describe complete behavior and limitations so detailed instructions do not have to be maintained twice.

| Audience | English (US) | Français |
|---|---|---|
| Users | [Illustrated User Guide](docs/USER_GUIDE.en-US.md) | [Guide illustré](docs/GUIDE_UTILISATEUR.md) |
| Developers | [Architecture and Protocol](docs/DEVELOPER_GUIDE.en-US.md) | [Architecture et protocole](docs/GUIDE_DEVELOPPEUR.md) |
| Testing | [Validation](TESTING.en-US.md) | [Tests](TESTING.md) |

[All HTML and PDF editions](docs/README.en-US.md)

## Installation

On Linux, install Rust/Cargo, a C++17 compiler, `pkg-config`, Qt 6 Widgets/Network/DBus and Qt build tools, KF6 WindowSystem, and Python 3. Wayland clipboard support uses `wl-clipboard`; remote URLs require KIO Fuse. See the [developer guide](docs/DEVELOPER_GUIDE.en-US.md#building).

```bash
python3 scripts/install.py
gad
```

Installation is per user and does not require administrator privileges. Use `gad --background` to start in the system tray.

## Copying from Dolphin

1. Copy with **Ctrl+C**, or cut with **Ctrl+X** to move files.
2. Right-click the destination folder and select **Paste with Gad** (Paste with Gad).
3. Repeat to add another request to the queue.

Optional **Ctrl+V integration in Dolphin** is enabled with `gad --configure --dolphin-ctrl-v` and disabled with `gad --configure --no-dolphin-ctrl-v`. Close and reopen Dolphin after changing it. In Gad, Ctrl+V pastes into the displayed destination; drag-and-drop copies files.

To protect the regular Dolphin launcher against lost shortcuts, install the watchdog with the command below, then close and reopen existing windows once. The module checks shortcuts every second, repairs the Gad action, and preserves native pasting in text fields.

```sh
python3 scripts/dolphin_integration.py --install --protect-launcher "$HOME/.local/bin/gad"
```


For other file managers: [pasting in Gad, drag-and-drop, and custom actions](docs/USER_GUIDE.en-US.md#using-gad-with-other-file-managers).

Undo depends on Gad's retained journal and backups. **Clear completed operations** (Clear finished operations) also removes their undo history. Read the [user guide](docs/USER_GUIDE.en-US.md) before clearing history you may need.

## Development and validation

```bash
make doctor
make test
make docs
```

Tests use temporary files and isolated sessions. They do not control your desktop instance of Gad. [Testing commands and requirements](TESTING.en-US.md).
