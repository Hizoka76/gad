# Gad

[Français](README.md) · [English (US)](README.en-US.md) · [Español](README.es-ES.md) · [Português](README.pt-PT.md) · [Deutsch](README.de-DE.md) · [日本語](README.ja-JP.md)

**Gad, il copie.** — **Gestionnaire Avancé de Duplication**

[English (US)](README.en-US.md) · [Démarrage rapide](docs/DEMARRAGE_RAPIDE.md)

Gad regroupe les copies et déplacements de fichiers dans une fenêtre native pour KDE Plasma. Son moteur est écrit en Rust et son interface en C++/Qt 6 Widgets.

- Interface en français, anglais américain, espagnol, portugais, allemand ou japonais, avec choix de langue mémorisé.
- File détaillée avec arborescence, priorités et glisser-déposer des lignes.
- Jusqu’à deux transferts compatibles en parallèle, déplacements rapides par renommage et doublons ignorés.
- Pause, limites de débit, estimation par demande et suivi Plasma.
- Conflits lisibles, vérifications MD5/propriétés/espace et historique avec remise en état.
- Reprise partielle, attente de reconnexion et réessai ciblé des fichiers en erreur.
- Sources et destinations locales ou distantes via KIO Fuse : KDE Connect, FTP, SFTP et SMB.

## Documentation

Le README sert de point d’entrée. Les explications complètes et les limites de chaque fonction sont dans les guides, pour éviter de maintenir deux descriptions concurrentes.

| Public | Français | English (US) |
|---|---|---|
| Utilisateurs | [Guide illustré](docs/GUIDE_UTILISATEUR.md) | [Illustrated User Guide](docs/USER_GUIDE.en-US.md) |
| Développeurs | [Architecture et protocole](docs/GUIDE_DEVELOPPEUR.md) | [Architecture and Protocol](docs/DEVELOPER_GUIDE.en-US.md) |
| Tests | [Validation](TESTING.md) | [Testing](TESTING.en-US.md) |

[Tous les guides en HTML et PDF](docs/README.md)

## Installer et lancer

Sous Linux, prévoir Rust/Cargo, un compilateur C++17, `pkg-config`, Qt 6 Widgets/Network/DBus, les outils Qt, KF6 WindowSystem et Python 3. Sous Wayland, installer `wl-clipboard` ; les adresses distantes demandent KIO Fuse. Les détails sont dans le [guide développeur](docs/GUIDE_DEVELOPPEUR.md#compilation).

```bash
python3 scripts/install.py
gad
```

L’installation est propre au compte utilisateur et ne demande pas de droits administrateur. Pour démarrer discrètement : `gad --background`.

## Copier depuis Dolphin

1. Copier avec **Ctrl+C**, ou couper avec **Ctrl+X** pour déplacer.
2. Au clic droit sur le dossier de destination, choisir **Coller avec Gad**.
3. Répéter pour ajouter une autre demande à la file.

L’association facultative de **Ctrl+V à Gad dans Dolphin** s’active avec `gad --configure --dolphin-ctrl-v` et se retire avec `gad --configure --no-dolphin-ctrl-v`. Fermer puis rouvrir Dolphin pour appliquer le changement. Dans Gad, Ctrl+V colle vers la destination affichée ; un glisser-déposer effectue une copie.

Pour protéger le lanceur Dolphin habituel contre la perte du raccourci, installez le module de surveillance avec la commande ci-dessous, puis fermez et rouvrez les anciennes fenêtres une fois. Le module vérifie les raccourcis chaque seconde, répare l’action Gad et conserve le collage natif dans les champs de texte.

```sh
python3 scripts/dolphin_integration.py --install --protect-launcher "$HOME/.local/bin/gad"
```


Pour les autres gestionnaires : [collage dans Gad, glisser-déposer et actions personnalisées](docs/GUIDE_UTILISATEUR.md#utiliser-gad-avec-un-autre-gestionnaire-de-fichiers).

Une remise en état repose sur le journal et les sauvegardes conservées par Gad. **Effacer les opérations terminées** retire aussi cette possibilité de restauration. Consultez le [guide utilisateur](docs/GUIDE_UTILISATEUR.md) avant de nettoyer un historique à conserver.

## Développer et vérifier

```bash
make doctor
make test
make docs
```

Les tests utilisent des fichiers temporaires et des sessions isolées. Ils ne pilotent pas l’instance Gad du bureau. [Commandes et prérequis des tests](TESTING.md).
