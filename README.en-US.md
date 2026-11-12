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


**0.4.0** — Choose the destination and options, then add files in Gad. When idle, review the list and click **Start**. External paste from the file manager starts automatically; additions during a transfer join the queue. **Ctrl+C** copies and **Ctrl+X** moves. Drag and drop into Gad always prepares a copy.

## Documentation

This README is a starting point. The guides describe complete behavior and limitations so detailed instructions do not have to be maintained twice.

| Audience | English (US) | Français |
|---|---|---|
| Users | [Illustrated User Guide](docs/USER_GUIDE.en-US.md) | [Guide illustré](docs/GUIDE_UTILISATEUR.md) |
| Developers | [Architecture and Protocol](docs/DEVELOPER_GUIDE.en-US.md) | [Architecture et protocole](docs/GUIDE_DEVELOPPEUR.md) |
| Testing | [Validation](TESTING.en-US.md) | [Tests](TESTING.md) |

[All HTML and Markdown editions](docs/README.en-US.md)

## Installation

### Ubuntu PPA

The [Gad PPA](https://launchpad.net/~hizo/+archive/ubuntu/gad) installs Gad and provides updates through APT:

```sh
sudo add-apt-repository ppa:hizo/gad
sudo apt update
sudo apt install gad
```

Check the PPA page for a published package matching your Ubuntu release. If `add-apt-repository` is missing, install `software-properties-common`.

### Precompiled downloads

[GitHub releases](https://github.com/Hizoka76/gad/releases/latest) offer several formats. Replace the example filenames below with the names of the files you downloaded.

#### Debian package `.deb`

Download the package for your architecture, then install it with APT to resolve dependencies available in your repositories:

```sh
sudo apt install ./gad_VERSION_amd64.deb
```

#### `.tar.gz` archive: sources and compiled binary

The `gad-VERSION-linux-amd64.tar.gz` archive contains source code, vendored Rust dependencies, documentation and the compiled program under `prebuilt/`. No compilation is needed to run it:

```sh
tar -xzf gad-VERSION-linux-amd64.tar.gz
cd gad-VERSION
./prebuilt/usr/bin/gad
```

#### Direct executable

The `gad-linux-amd64` file is also a ready-compiled binary:

```sh
chmod +x gad-linux-amd64
./gad-linux-amd64
```

Linux amd64 downloads target x86-64 processors. The direct binary does not install a launcher or Dolphin plugin; the `.deb` and archive contain the integration files shipped with that version. These binaries are not self-contained: Qt/KF6 are required at the versions listed in the release notes. A release matches its tag and may be older than the current repository code.

### From source

On Linux, install Rust/Cargo, a C++17 compiler, `pkg-config`, Qt 6 Widgets/Network/DBus and Qt build tools, KF6 WindowSystem/XmlGui, CMake, and Python 3. Wayland clipboard support uses `wl-clipboard`; remote URLs require KIO Fuse. See the [developer guide](docs/DEVELOPER_GUIDE.en-US.md#building).

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
