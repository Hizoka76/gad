# Commandes de validation ; les tests n’installent ni ne relancent l’application du bureau.
PYTHON ?= python3
.PHONY: test test-unit test-python test-qt test-integration test-bench doctor check

test:
	$(PYTHON) scripts/run_tests.py all

test-unit:
	$(PYTHON) scripts/run_tests.py unit

test-python:
	$(PYTHON) scripts/run_tests.py python

test-qt:
	$(PYTHON) scripts/run_tests.py qt

test-integration:
	$(PYTHON) scripts/run_tests.py integration

test-bench:
	$(PYTHON) scripts/run_tests.py bench

doctor:
	$(PYTHON) scripts/run_tests.py doctor

check:
	$(PYTHON) scripts/run_tests.py lint

# Exporter les guides existants ; les captures se régénèrent séparément sans toucher au bureau.
.PHONY: docs docs-captures docs-check docs-diagrams
DOC_DOTS := $(wildcard docs/tools/*.dot)
DOC_DIAGRAMS := $(patsubst docs/tools/%.dot,docs/images/%.png,$(DOC_DOTS)) $(patsubst docs/tools/%.dot,docs/images/%.svg,$(DOC_DOTS))

docs-diagrams: $(DOC_DIAGRAMS)

docs/images/%.png: docs/tools/%.dot
	dot -Tpng "$<" -o "$@"

docs/images/%.svg: docs/tools/%.dot
	dot -Tsvg "$<" -o "$@"

docs: docs-diagrams
	$(PYTHON) scripts/build_docs.py
	$(PYTHON) scripts/check_docs.py

docs-captures:
	$(PYTHON) scripts/capture_docs.py

docs-check:
	$(PYTHON) scripts/check_docs.py

# Essai réseau réel optionnel : module pyftpdlib, kio-fuse et /dev/fuse requis.
.PHONY: test-remote
test-remote:
	cargo build --locked
	$(PYTHON) scripts/test_remote_destinations.py

# Validation optionnelle de l’intégration Ctrl+V avec les bibliothèques KDE réelles.
.PHONY: test-dolphin-paste
test-dolphin-paste:
	$(PYTHON) scripts/test_dolphin_paste.py

# Les .qm sont versionnés et embarqués : Linguist n’est requis que pour les modifier.
.PHONY: translations translations-check
translations:
	$(PYTHON) scripts/translations.py

translations-check:
	$(PYTHON) scripts/translations.py --check

# Compilation et paquets système isolés ; aucun envoi automatique au PPA.
.PHONY: build package-source package-binary
build:
	./build.sh

package-source:
	$(PYTHON) scripts/package.py source

package-binary:
	$(PYTHON) scripts/package.py binary

# Pages de manuel des commandes publiques, dans les six langues.
.PHONY: manpages manpages-check
manpages:
	$(PYTHON) scripts/generate_manpages.py

manpages-check:
	$(PYTHON) scripts/generate_manpages.py --check

# Aucun transfert réel : toutes les préférences et le serveur utilisent un compte temporaire.
.PHONY: test-cli-settings
test-cli-settings: build
	dbus-run-session -- $(PYTHON) scripts/test_cli_settings.py

# Simulation de la perte des raccourcis et contrôle de la réparation périodique.
.PHONY: test-dolphin-watchdog
test-dolphin-watchdog:
	$(PYTHON) scripts/test_dolphin_watchdog.py

.PHONY: test-dolphin-watchdog-real
test-dolphin-watchdog-real:
	$(PYTHON) scripts/test_dolphin_watchdog_real.py

# Nettoyage local : les releases publiées et les données personnelles sont conservées.
.PHONY: clean clean-preview
clean:
	$(PYTHON) scripts/clean.py --apply

clean-preview:
	$(PYTHON) scripts/clean.py
