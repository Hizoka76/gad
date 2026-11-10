# Testing Gad

[Français](TESTING.md) · [Developer Guide](docs/DEVELOPER_GUIDE.en-US.md) · [Documentation](docs/README.en-US.md)

## Running validation

From the repository root:

```bash
make doctor           # Check tools, native libraries, and Rust components
make test             # Formatting, Clippy, Rust, Python, Qt, and integration
```

Without Make:

```bash
python3 scripts/run_tests.py all
python3 scripts/run_tests.py all --keep-going
python3 scripts/run_tests.py all --list
```

The runner returns zero only when all required stages pass. It normally stops at the first failure. `--keep-going` also runs independent later stages; integration remains dependent on a successful build.

## Test groups

| Command | Coverage |
|---|---|
| `make test-unit` | Rust and Python unit tests. |
| `make test-python` | XDG paths, Dolphin integration, runner, and documentation generation logic. |
| `make test-qt` | Clipboard, widgets, conflicts, menus, error reporting, and notifications. |
| `make test-integration` | Build and test the actual executable and local protocol. |
| `make check` | Formatting and Clippy with warnings treated as errors. |
| `make test-bench` | Explicit large-history refresh benchmark. |
| `make docs-check` | Existing documentation links, captures, and multilingual exports. |

Runner group names are `unit`, `python`, `qt`, `integration`, `lint`, `bench`, and `all`.

```bash
python3 scripts/run_tests.py unit --filter rollback_
python3 scripts/test_qt.py --suite row_actions
python3 scripts/test_qt.py --suite clipboard --suite notifications
cargo test --locked engine::tests::copies_tree_and_symlink_without_following_it
```

The runner filter applies to Rust tests; Python tests still run. The benchmark is ignored during normal validation. Individual wrapper scripts remain available.

## Benchmark fast moves

```bash
cargo build --release --locked
dbus-run-session -- python3 scripts/benchmark_rename.py --binary target/release/gad --label trial-
```

The script uses private files, settings, socket, and journals under `target/rename-performance/`. Three runs per size compare POSIX rename, a fresh KIO process, and Gad. JSON reports distinguish the physical rename from completion observed through IPC. History is kept and optional checks are disabled. `--visible` displays the Qt window on the offscreen backend; `--trace` traces a 1,000-file move and requires `strace`. Traced timings are not performance baselines. Starting `kioclient` does not exactly reproduce an action in an existing Dolphin window.

Use `dbus-run-session -- python3 scripts/benchmark_startup.py --binary target/release/gad --records 35` to benchmark startup with a large journal. Synthetic jobs are completed, with no files to transfer or restore. The script records startup time and Linux peak RSS under `target/perf-pass2/`.

Use `dbus-run-session -- python3 scripts/benchmark_copy.py --binary target/release/gad --files 100` to benchmark small-file copies. Add `--compare /path/to/older/gad` to alternate both binaries across three private trials. Contents and source preservation are verified. `wal_segment_bytes` measures only the final remaining segment, not cumulative disk writes. Reports are saved under `target/copy-performance/`.

## Reports

Each runner invocation writes the following under `target/test-results/`:

- A `.log` file for each executed stage.
- `results.json`, including command arguments, exit codes, status, and elapsed time.
- `junit.xml`, with **one entry per command stage**, not one per individual Rust/Qt test. Framework-level cases and assertions remain in the stage logs.

```bash
python3 scripts/run_tests.py all --report-dir /tmp/gad-results
```

JSON and JUnit describe the latest run and identify its logs. Logs from older skipped stages may still exist. `target/` is excluded from source files by `.gitignore`.

## Requirements

- Linux and recent Rust/Cargo, with Rust 1.85 or newer, rustfmt, and Clippy.
- A C++17 compiler, `pkg-config`, Qt 6 Widgets/Network/DBus/Test.
- `qmake6`, `moc`, `rcc`, KF6 WindowSystem, and `dbus-run-session`.
- Python 3. Routine script tests use only its standard library.

The supplied Debian Trixie CI environment installs:

```bash
sudo apt install build-essential pkg-config python3 cargo rustc rustfmt rust-clippy \
  qt6-base-dev qt6-base-dev-tools qt6-qpa-plugins qmake6 libkf6windowsystem-dev dbus fonts-dejavu-core
```

Use corresponding packages on other distributions. `make doctor` identifies missing tools. Exporting documentation separately requires Markdown, Graphviz; those are not required for the seven standard documentation unit tests. See the [documentation index](docs/README.en-US.md#regeneration).

## Isolation

Filesystem tests use temporary trees. Qt runs offscreen with isolated HOME/XDG and a private D-Bus session. Tests do not install Gad, modify the user's actual clipboard, restart the desktop instance, or operate on personal transfer files.

Desktop opens are intercepted through `QDesktopServices`; no real editor or file manager is launched. KIO and KDE notifications use fake services. Integration controls only its own isolated executable. Real FTP/SFTP/SMB servers are unnecessary for the normal suite.

Device removal is simulated by renaming temporary anchor directories, never by unplugging or unmounting a real disk. Mock mount tables test bind-mount logic without mount privileges.

## Adding a regression test

| Area | Location and approach |
|---|---|
| Rust engine | Add `#[test]` cases in `src/engine/tests.rs`; use `Fixture`, wait for business states, and inspect file contents as well as JSON. Finish work and shut down new engines where possible. |
| Row/menu/open actions | Add slots or data rows in `tests/row_actions.cpp`; mock the engine and intercept desktop opens. |
| Clipboard and window behavior | Extend `tests/clipboard.cpp`. |
| D-Bus progress | Extend `tests/notifications.cpp` with the private fake JobView server. |
| Python scripts | Add `tests/python/test_*.py` using `unittest`, `TemporaryDirectory`, and environment/subprocess mocks. Discovery is automatic. |
| New Qt suite | Add its name to `SUITES` in `scripts/qt_tests.py`, its C++ source, and the matching generated moc include. |

Test observable behavior: resulting states, correct bytes, retained originals, restored destinations, desktop request arguments, and absence of unintended changes. Prefer a signal/state predicate over arbitrary delays. Do not test by controlling the real desktop application.

## Continuous integration

`.github/workflows/tests.yml` supplies complete validation in a Debian Trixie container and retains reports on failure. It becomes active when the project is hosted in a suitable GitHub repository. The configuration is supplied; no remote CI execution is claimed here.

## Fast local moves and bind mounts

```bash
python3 scripts/run_tests.py unit --rust-filter fast_
```

`fast_move` cases inspect inodes after transfer and undo, execution alongside copying, independent paths sharing a destination folder, name reservations after conflicts, overlapping paths, MD5 fallback, concurrent targets, EXDEV rescheduling, cancellation, replacement chains, and journal recovery. Same-length modification with restored mtime must still make undo refuse an altered result.

Fast fixtures use writable Linux tmpfs at `/dev/shm`, so an overlay-backed `/tmp` in CI does not accidentally disable the local rename path. Injected rename behavior simulates EXDEV without moving files to another volume.

`fast_bind_mounts` uses synthetic mount tables for distinct bind views and their common underlying view. Tests cover translation both ways, missing common exposure, different filesystems, read-only restrictions, and destination suffixes that do not exist yet.

`fast_tree` adds whole-root inode/permission preservation, empty folders, multiple roots, targeted pauses, partial undo, outside files added later, modified contents, and recovery before/after root publication. Nested mounts, including ones hidden in a common view, prevent the shortcut.

## Partial recovery after interruption

```bash
cargo test --locked resume_
python3 scripts/test_resume_integration.py
```

Build the executable before running a standalone integration script. Tests cover durable point ordering, source and temporary ownership/version validation, prefix comparison, MD5 reconstruction, trailing-byte truncation only after validation, and preserved temporary identity. Altered content or metadata must fail without truncating a questionable file.

The actual-binary integration uses an isolated process, SIGKILL, relaunch, and byte/digest comparison to verify that the same partial file resumes. It never kills the user's Gad instance. Explicitly canceled work must not become recoverable automatic work.

## Remote destinations

```bash
make test-remote
```

This optional real-network test requires `pyftpdlib`, KIO Fuse, and usable `/dev/fuse`. Normal validation uses fake KIO services instead. The optional server and files remain in a temporary environment; it is not a test against a personal remote account.

Check URL validation, both endpoint mappings, operation/origin options, unsupported service metadata, synchronization semantics, and disabled durable KIO destination recovery. Tests must not treat unavailable properties or capacity as fabricated successful guarantees.

## Dolphin Ctrl+V and desktop progress

```bash
make test-dolphin-paste
```

The optional integration test requires CMake, Qt 6 Widgets/Test, and KF6 XmlGui development files. It uses real KXMLGUI/KActionCollection in isolated HOME/XDG, D-Bus, and offscreen clipboard environments. Actions are counters: no real copy or user Dolphin restart occurs.

It verifies Gad Ctrl+V, preservation of the native Shift+Insert KIO alternative, native text-field paste, restored Ctrl+V settings after disabling, complete validation before modifying configuration, rejection of invalid receipts, and rollback after write failures.

Clipboard tests inject Dolphin session responses for unique active windows, service aliases, spaces and `#`, short titles, closed folders, navigation/focus changes, unavailable services, and remote URLs. Python tests cover integration migration/restoration without altering unrelated actions.

Notification tests exercise commands returning through the engine-facing controller, synchronized pause labels/tray state, a single combined progress view, speed in details, MD5 completion accounting, service reconnection, and the transition of `suspended` back to false. Ordinary notifications must not duplicate the native job view.

## Free-space checks, priorities, and trees

Space tests cover shared per-volume budgets, separate volumes, block rounding, overflow, allocated partial files, option persistence, and fast renames. A sparse tmpfs fixture creates a shortage without filling the disk. Unknown simulated remote capacity warns and completes. Qt checks captured options and a warning remaining visible after an OK result.

Priority tests use the actual scheduler for stable multiple-request order, paused work, atomic stale-selection rejection, and correct undo after reordered replacements. IPC integration reorders and restores queued requests before proceeding with copies.

Widget tests check the header-free forest, shared request accents in light/dark themes, whole-request selection, stable internal drag references, foreign-drag rejection, preserved parenthood/destinations, and selection/hover/expansion stability. Engine tests cover alternating independent roots, sibling order exposed by conflicts, invalid reparenting, placeholder priorities, and replacement dependencies during pause/finalization.

Native Qt objects are content-cached under `target/qt-test-cache` with source/header, Qt/KDE version, and compiler identity hashes. A build lock prevents concurrent cache construction. Test executables, files, configuration, and D-Bus sessions remain fresh and isolated.

Normalized destination dependencies include parent aliases, repeated names, selective pauses, and future copy-name reservations. Folder property warnings summarize only their own actual descendants.

## Duplicate transfer suppression

`duplicates_` cases cover repeated paused pastes, concurrent preparation, duplicates inside one submission, overlapping folders, parent aliases, and restored stable indices. They preserve distinct destinations, copy/move intentions, hard-link directory entries with different names, and intentional new requests after completion/cancellation.

Qt checks a single informational message from metadata-only updates without repeated prompts. Conflict scenarios use genuinely distinct sources sharing a filename, so they test conflicts rather than duplicate suppression.

## Bandwidth limits and estimates

`engine::rate` uses an injected clock to verify shared/global and per-request budgets, bounded idle credit, aggregate consumption, and live changes. Engine tests perform a real limited copy, remove the limit, and verify output. Pause, low-rate cancellation, atomic validation, persistence, and preservation of a newer limit during retry preparation are covered.

Qt checks MiB-to-bytes conversion and one target per owning request even when several of its rows are selected. Changing selection while the rate dialog is open must not change its captured IDs.

Deterministic metrics tests cover independent simultaneous request rates/times, copy versus MD5 phases, pauses/conflicts, renames, resumed prefixes, counter resets, stalled progress, and duration formatting. The actual summary widget updates on `poll.unchanged` without replacing rows or their selection/tooltips.

## Device reconnection

`reconnect_` cases simulate source/destination disappearance by renaming temporary anchors. They cover validated durable-prefix recovery, retained pauses, no writes into an empty replacement directory, changed-source refusal, cancellation, and released workers for independent work.

Additional cases cover restarting an owned unpublished file without a durable point, refusing publication replay, UUID-authorized device-number changes, preserved metadata/content digests during rebinding, and concurrent pause/cancel commands during preparation. No real filesystem is mounted or unmounted.

Qt checks row and progress labels, cancellation availability, a suspended native job view, and asynchronous KIO resolution that neither adds work nor raises the window.

## Error reports and retries

`errors_` tests cover read-only reports, preparation/finalization failures, atomic validation, stale indices, exact report references, repaired retry preparation, and modified published-output protection. A real partly failed request checks that the successful neighbor's contents, inode, timestamps, and journal remain unchanged after retry.

Global pause, canceled/skipped entries, and unselected pending work are preserved. Directory aggregation avoids duplicate error records while retaining actual folder failures.

Qt verifies filtering and stable request targets after sorting, contextual retry actions, empty reports, full UTF-8 text, parseable JSON, and atomic report writes. Exports use temporary paths only. Every error-dialog button is checked with a deliberately unavailable icon theme to exercise standard Qt fallback icons.

## Documentation generation {#documentation-generation}

```sh
make docs-captures
make docs
make docs-check
```

Documentation tests cover the six-language catalog and all three guide types, same-audience language navigation, paper sizes, localized captions, and offline image embedding. They also check that quick starts use a compact print layout, that each user guide references its own screenshots, and that translated examples preserve the original code blocks.

The documentation build renders localized diagrams and 18 guides, then checks local links and anchors, 84 light-theme screenshots and their dimensions and hashes, HTML language tags, embedded images, and all 36 HTML/Markdown exports. It requires the optional dependencies listed in the documentation index.

[Documentation](docs/README.en-US.md#regeneration)

## Application translations

Five Python tests check marked Qt string coverage, TS/QM fingerprints, exclusion of comments and unmarked identifiers, missing translations, and preservation of parameters and links. Qt tests load the real embedded French catalog and check widgets, states, language preferences, text reports, and unchanged paths/JSON. Empty-report export and refresh/filter transitions are covered too. Run `make translations-check` for an independent check.

Live-language Qt tests alternate French and US English during an active transfer with an unchanged poll response. They verify row identity, selection, expansion, destination/rate inputs, About/error/report/rate dialogs, conflict choices, stable binding counts, and the absence of engine write commands.

## Exclusions and hidden items

Rust tests cover Unicode patterns, atomic validation, legacy defaults, folder pruning, temporary files, symbolic links, per-job rule persistence, retries, fully excluded requests, and moves followed by undo. Qt tests cover saved preferences, canceled and rejected edits, live translation, and rule capture for new requests.

```sh
cargo test --locked exclusions
python3 scripts/test_qt.py --suite row_actions
```

## Konqueror integration

`python3 scripts/test_qt.py --suite konqueror` checks the current folder, encoded and remote URLs, cut-mode preservation, native text pasting, and Shift+Insert in an isolated environment. The plugin was also loaded in Konqueror 26.08.2 extracted into a temporary directory with private HOME/XDG and D-Bus; the exact active-folder URL was verified. Local logs are in `target/konqueror-real-test.log`.

## Settings dialog {#settings-menu}

Qt tests check window minimum sizes, all five tabs, Close and Escape, retained controls, live translation, and the speed button in the action bar. CLI tests cover invalid values, conflicting switches, partial exclusion updates, and preference synchronization. Python tests check all six manual pages, six-language coverage, installation, and rejection of missing translations.

```sh
make manpages-check
make test-cli-settings
```

## Automatic Ctrl+V watchdog {#dolphin-watchdog}

Qt tests cover action recreation, the real timer, disabling, native paste restoration, user changes, modal dialogs, invalid receipts, and missing actions. Text pasting stays native. Python tests cover reversible launcher protection, arguments, and later customizations. The real Dolphin test refreshes its menus and presses Ctrl+V: a dummy command records the launch without copying files or accessing the desktop.

```sh
make test-dolphin-watchdog
make test-dolphin-watchdog-real
```

## Automated interface scenarios

`python3 scripts/test_qt.py --suite ui_interactions` drives real widgets using
mouse and keyboard events: Start/Pause/Resume, tabs, Escape, live language
switching, modal blocking, multiple selection and Ctrl+F. The `progressMessage`
preference is checked through its checkbox, Qt property, CLI, profiles and
reopening. Notification tests also cover active KDE views and late D-Bus replies.

This suite runs automatically in `make test`, `make test-qt` and CI. It uses a
simulated engine, private bus, temporary XDG directories and offscreen rendering;
real desktop transfers and shortcuts remain untouched.


Run the isolated directory completion benchmark below. The copy benchmark also accepts `--folders 20` to distribute files across directories, including non-divisible file counts.

```bash
cargo test --release --locked benchmark_directory_completion -- --ignored --nocapture
```


Journal delta computation also has an isolated benchmark: it compares two 4 MiB JSON strings with cached digests. This measures comparison overhead without disk I/O.

```bash
cargo test --release --locked benchmark_delta_comparison -- --ignored --nocapture
```


The following benchmark measures 100 unchanged saves with a custom order of 10,000 rows. The copy benchmark also accepts `--reorder` to customize row order before starting the transfer and exercise the active path.

```bash
cargo test --release --locked benchmark_unchanged_header -- --ignored --nocapture
```


The dependency graph benchmark rebuilds relationships five times for two jobs of 5,001 entries, with distinct and shared destinations. Fixture creation is excluded from timing. Tests also compare the optimized graph with the previous algorithm on 500 reproducible configurations with aliases, repeated destinations and multiple branches.

```bash
cargo test --release --locked benchmark_dependency_graph -- --ignored --nocapture
```


Additional hot-path benchmarks are ignored by default: staging reservations (100 creations, 5,000 planned files), sorting 5,000 names in three input orders, exclusions (100,000 comparisons), row layout caching, checkpoints with 5,000 history jobs, buffer allocation, small-file MD5 and KIO Fuse detection. Synthetic mount tests never alter real mounts. The copy benchmark now accepts `--verify-md5` and validates the resulting digests.

```bash
cargo test --release --locked benchmark_staging_reservations -- --ignored --nocapture
cargo test --release --locked benchmark_directory_sort -- --ignored --nocapture
cargo test --release --locked benchmark_exclusion_matching -- --ignored --nocapture
cargo test --release --locked benchmark_row_layout -- --ignored --nocapture
cargo test --release --locked benchmark_checkpoint_history -- --ignored --nocapture
cargo test --release --locked benchmark_copy_buffer -- --ignored --nocapture
cargo test --release --locked benchmark_small_file_md5 -- --ignored --nocapture
cargo test --release --locked benchmark_kio_mount_detection -- --ignored --nocapture
```


Journal benchmarks isolate loading 10,000 entries, delta replay, encoding 256-byte or 4 MiB records, and reading 30 headers each containing a 10,000-row order. The startup benchmark now accepts `--records 0`, `--wal-format delta`, `--custom-order` and `--compare`, with counterbalanced version ordering.

```bash
cargo test --release --locked benchmark_snapshot_load -- --ignored --nocapture
cargo test --release --locked benchmark_delta_replay -- --ignored --nocapture
cargo test --release --locked benchmark_record_encoding -- --ignored --nocapture
cargo test --release --locked benchmark_wal_headers -- --ignored --nocapture
dbus-run-session -- python3 scripts/benchmark_startup.py --binary target/release/gad --records 32 --wal-format delta --custom-order
```


## Recent interface checks

`exclusionsTabCommitsOnFocusLossAndTranslates` and `exclusionsSaveWhenFocusLeavesTheField` check focus-loss saving, rejection without losing text, independent fields and closing settings. `clipboardAdditionKeepsPreparedQueueWaiting` checks that clipboard additions do not start prepared, paused or historical work. Existing tests cover Copy/Cut, automatic external paste, Ctrl+O/Ctrl+Shift+O/Ctrl+Q/Ctrl+Alt+W and all five tabs.
