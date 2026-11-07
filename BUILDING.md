# Compiler Gad et préparer un PPA

Gad requiert Linux, Rust/Cargo ≥ 1.85, un compilateur C++17, Python ≥ 3.11,
pkg-config, Qt 6 (Widgets, Network, DBus, qmake, moc et rcc), et KDE
Frameworks 6 WindowSystem et XmlGui, ainsi que CMake pour le module Dolphin. Les catalogues `.qm` sont fournis ; Linguist
n’est nécessaire que pour modifier les traductions.

Sur Ubuntu disposant de KF6 :

```sh
sudo apt install build-essential cmake cargo rustc pkg-config python3 qt6-base-dev qt6-base-dev-tools qmake6 libkf6windowsystem-dev libkf6xmlgui-dev
./build.sh
./target/release/gad
```

Sur KDE neon, le paquet de développement KDE est `kf6-kwindowsystem-dev` ;
`qt6-base-dev` fournit aussi qmake6 et les outils de base. La version de Rust
fournie par Ubuntu Noble standard est insuffisante : utiliser un compilateur
récent pour la compilation locale. Le PPA doit disposer de toutes les
dépendances de construction pour sa série cible, y compris KF6 et Rust récent.
Le fichier `debian/changelog` cible actuellement `noble` pour neon ; ce n’est
pas une promesse de compilation sur un environnement Ubuntu Noble standard.

## Paquets Debian

Installer les outils de paquetage :

```sh
sudo apt install debhelper devscripts dpkg-dev fakeroot lintian
python3 scripts/package.py binary --output target/packages-local
python3 scripts/package.py source --output target/packages-ppa
```

Chaque commande exige un nouveau répertoire de sortie. Elle crée une copie
isolée, inclut les crates de `Cargo.lock` avec `cargo vendor`, produit l’archive
`orig.tar.xz`, puis lance `dpkg-buildpackage`. La préparation nécessite les
crates en cache ou un accès réseau. La compilation du paquet utilise
`--frozen` et n’accède jamais au réseau. `target/`, les SDK locaux, les caches
et les données de l’utilisateur sont exclus des sources distribuées.

`prepare` réalise uniquement l’archive et l’arbre source. L’option
`--no-check-builddeps` permet un essai local avec un compilateur rustup ; elle
ne valide pas la disponibilité des dépendances sur Launchpad.

Le paquet installe le programme dans `/usr/bin`, les menus KIO, l’icône,
le gestionnaire de raccourcis Dolphin et les 36 documents HTML/PDF dans
`/usr/share/gad/docs`. Il ne modifie aucun raccourci ou réglage de compte.
Les traductions sont embarquées dans le binaire. Le module Ctrl+V spécifique
à Konqueror n’est pas inclus dans ce paquet : il dépend de son SDK particulier ;
le menu contextuel commun reste disponible.

Une ancienne installation dans `~/.local/bin` et `~/.local/share` peut être
prioritaire sur les fichiers système. Vérifier `command -v gad` avant un essai.
La suppression du paquet conserve l’historique et les réglages personnels.

## Publication sur Launchpad

Confirmer d’abord la licence de Gad dans `debian/copyright`. Les licences des
crates sont conservées dans `vendor/`. Adapter la distribution et la version
dans `debian/changelog` pour chaque série Ubuntu et vérifier ses dépendances
dans un environnement propre. Un binaire compilé sur neon ne constitue pas
une validation de compilation Ubuntu.

Après construction des sources, contrôler et signer le fichier `_source.changes` :

```sh
lintian target/packages-ppa/*_source.changes
debsign -kIDENTIFIANT_CLE target/packages-ppa/*_source.changes
dput ppa:PROPRIETAIRE/NOM target/packages-ppa/*_source.changes
```

Les scripts ne signent et n’envoient rien automatiquement. La clé, le nom du
PPA et la série cible restent des choix du mainteneur. Les paquets non signés
produits localement servent à la vérification avant cet envoi.

## Surveillance du raccourci Dolphin

Le module utilise les interfaces publiques Qt et KXMLGUI. Il vérifie les actions
réelles chaque seconde, répare leurs raccourcis et transmet le collage directement
à Gad, sans dépendre du contexte de menu recréé par Dolphin. Les champs de texte
restent natifs. La désactivation de Ctrl+V avec Gad arrête la réparation et restaure
le collage natif. Les modifications de raccourcis faites par l’utilisateur sont respectées.

```sh
make test-dolphin-watchdog
make test-dolphin-watchdog-real
gad --dolphin --new-window
```

Pour protéger le lanceur habituel lors d’une installation locale :

```sh
python3 scripts/dolphin_integration.py --install --protect-launcher "$HOME/.local/bin/gad"
```

Seul le lanceur du compte courant est surchargé. Sa configuration précédente
est conservée dans `gad/dolphin-launcher.json` et restaurée à la désinstallation.
Le chemin des plugins reste limité au processus Dolphin. Les processus déjà
ouverts doivent être relancés une fois pour charger le module.
