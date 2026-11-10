#!/usr/bin/env python3
"""Générer les manuels dans les six langues de Gad des commandes réellement prises en charge."""
import argparse
import json
import re
from pathlib import Path
import tomllib

ROOT = Path(__file__).resolve().parent.parent

# Les options techniques internes et les alias historiques ne constituent pas l’interface publique.
GAD_OPTIONS = [
 ('--progress-message, --no-progress-message', 'With --configure, show or hide the KDE transfer progress message immediately. Enabled by default. Hiding it does not pause or cancel transfers. Saved in transfer profiles.', 'Avec --configure, afficher ou masquer immédiatement le message de progression KDE. Activé par défaut. Masquer le message ne met pas en pause et n’annule pas les transferts. Enregistré dans les profils.'),
 ('--completion-action ACTION', 'With --configure, save none, notify, quit, suspend or poweroff as the batch-completion action. This only saves a preference. In Options > Batch completion, click Activate for this batch to enable it once. Profiles never activate actions.', 'Avec --configure, enregistrer none, notify, quit, suspend ou poweroff comme action de fin de lot. Seule la préférence est enregistrée. Dans Options > Fin de lot, cliquer sur Activer pour ce lot pour une activation unique. Les profils n’activent jamais une action.'),
 ('--completion-delay SECONDS', 'With --configure, save a cancelable countdown from 5 to 3600 whole seconds (default 30). New work restarts the full countdown after completion; errors, cancellations and removed requests cancel the action.', 'Avec --configure, enregistrer un compte à rebours annulable de 5 à 3600 secondes entières (30 par défaut). Une nouvelle demande relance le délai entier après sa fin ; les erreurs, annulations et demandes supprimées annulent l’action.'),
 ('--verify-fast-move, --no-verify-fast-move', 'With --configure, enable or disable post-rename verification for new fast moves. Checks identity, type, metadata, symbolic links, the complete tree and absence of the original path without reading file contents. A failed check keeps the moved result and its restoration journal. Saved in profiles. MD5 remains separate and forces the copy path.', 'Avec --configure, activer ou désactiver la vérification après renommage pour les nouveaux déplacements rapides. Contrôle l’identité, le type, les métadonnées, les liens symboliques, l’arborescence complète et l’absence du chemin source, sans lire le contenu. Un échec conserve le résultat déplacé et son journal de restauration. Enregistré dans les profils. Le MD5 reste distinct et impose le parcours de copie.'),
 ('--conflict-policy ask|skip|newer|rename', 'With --configure, set the conflict policy for new requests: ask, skip existing files, newer (replace only strictly older destination files), or rename (keep both). Existing folders are merged. Equal or newer destination files are kept; unreadable dates and symbolic links fail safely. Saved in transfer profiles; existing requests keep their policy.', 'Avec --configure, définir la politique des nouvelles demandes : ask (demander), skip (ignorer les fichiers existants), newer (remplacer uniquement les destinations strictement plus anciennes) ou rename (conserver les deux). Les dossiers existants sont fusionnés. Les destinations de date égale ou plus récente sont conservées ; les dates illisibles et liens symboliques provoquent un échec sans remplacement. Enregistré dans les profils ; les demandes existantes gardent leur politique.'),
 ('--profile NAME', 'Apply all options saved in the named profile before a copy, move or paste. Used alone, apply settings without starting a transfer. The destination is never stored in a profile; use --to or --paste. Explicit command options override the profile. An unknown profile fails without queuing files.', 'Appliquer toutes les options du profil nommé avant une copie, un déplacement ou un collage. Seul, appliquer les réglages sans lancer de transfert. La destination n’est jamais enregistrée dans un profil ; utiliser --to ou --paste. Les options explicites de la commande priment sur le profil. Un profil inconnu échoue sans mettre de fichiers en file.'),
 ('--help, -h', 'Display command help and exit.', "Afficher l’aide de la commande et quitter."),
 ('--background', 'Start Gad in the system tray without showing its main window.', "Démarrer Gad dans la zone de notification sans ouvrir sa fenêtre principale."),
 ('--copy SOURCE... --to FOLDER', 'Queue copies of the sources into the destination folder.', "Mettre en file la copie des sources dans le dossier de destination."),
 ('--move SOURCE... --to FOLDER', 'Queue moves of the sources into the destination folder.', "Mettre en file le déplacement des sources dans le dossier de destination."),
 ('--paste FOLDER', 'Queue clipboard files into the destination. Cut clipboard data requests a move. Starts the transfer without opening the main window.', "Mettre en file les fichiers du presse-papiers vers la destination. Une opération Couper demande un déplacement. Le transfert démarre sans ouvrir la fenêtre principale."),
 ('--paste-current', 'Paste into the active supported Dolphin or Konqueror folder. Requires the corresponding file manager integration.', "Coller dans le dossier actif de Dolphin ou Konqueror. Nécessite l’intégration correspondante du gestionnaire de fichiers."),
 ('--paste-context URL...', 'Paste using the folder or selected file context supplied by a KIO service menu.', "Coller dans le dossier déduit du contexte fourni par le menu de service KIO."),
 ('--md5', 'Verify source and destination contents using MD5 for the submitted transfer. Does not change the saved preference.', "Vérifier le contenu source et destination par MD5 pour le transfert demandé. Ne modifie pas la préférence enregistrée."),
 ('--properties', 'Verify visible owners, groups, permissions, ACLs and extended attributes for the submitted transfer. This checks properties; it does not replicate them. Does not change the saved preference.', "Vérifier les propriétaires, groupes, permissions, ACL et attributs étendus visibles pour le transfert demandé. Cette vérification ne reproduit pas les propriétés. Ne modifie pas la préférence enregistrée."),
 ('--status', 'Print the current engine state as JSON.', "Afficher l’état actuel du moteur au format JSON."),
 ('--smoke-test', 'Run an isolated graphical startup check with temporary storage.', "Effectuer un contrôle graphique de démarrage isolé dans un espace temporaire."),
 ('gad://show', 'Show the main window of the running instance.', "Afficher la fenêtre principale de l’instance en cours."),
]
# Ces options enregistrent les préférences ; les options de transfert seules gardent leur portée.
GAD_OPTIONS += [
 ('--configure OPTIONS...', 'Save settings in the running instance, or start Gad in the tray to save them. Requires at least one setting. Boolean switches listed below must be used with --configure; use their --no- form to disable them.', "Enregistrer les préférences dans l’instance ouverte, ou démarrer Gad dans la zone de notification pour les enregistrer. Exige au moins un réglage. Les options booléennes ci-dessous s’utilisent avec --configure ; leur forme --no- les désactive."),
 ('--no-md5, --no-properties', 'With --configure, disable the corresponding verification preference. --configure --md5 and --configure --properties enable it.', "Avec --configure, désactiver la vérification correspondante. --configure --md5 et --configure --properties l’activent."),
 ('--verify-space, --no-verify-space', 'With --configure, enable or disable available space checks for new transfers.', "Avec --configure, activer ou désactiver la vérification de l’espace disponible pour les nouveaux transferts."),
 ('--keep-history, --no-keep-history', 'With --configure, enable or disable persistent transfer history.', "Avec --configure, activer ou désactiver la conservation de l’historique des transferts."),
 ('--resume-interrupted, --no-resume-interrupted', 'With --configure, enable or disable durable checkpoints and automatic recovery for new transfers.', "Avec --configure, activer ou désactiver les points de reprise et la reprise automatique pour les nouveaux transferts."),
 ('--include-hidden, --no-include-hidden', 'With --configure, include or exclude hidden items. Keep existing exclusion patterns.', "Avec --configure, inclure ou exclure les éléments cachés en conservant les motifs d’exclusion existants."),
 ('--allow-duplication, --no-allow-duplication', 'With --configure, enable or disable copies named name_copied1 in the source folder.', "Avec --configure, autoriser ou non les copies nommées nom_copied1 dans le dossier source."),
 ('--autostart, --no-autostart', 'With --configure, enable or disable launching Gad when the current user signs in.', "Avec --configure, activer ou désactiver le lancement de Gad à l’ouverture de la session du compte courant."),
 ('--konqueror-ctrl-v, --no-konqueror-ctrl-v', 'With --configure, enable or disable paste interception in Konqueror launched with Gad. The integration plugin must be installed.', "Avec --configure, activer ou désactiver l’interception du collage dans Konqueror lancé avec Gad. Le module d’intégration doit être installé."),
 ('--language LOCALE', 'With --configure, select system or an installed locale such as fr_FR or en_US.', "Avec --configure, choisir system ou une langue installée telle que fr_FR ou en_US."),
 ('--rate-limit MIB_PER_SECOND', 'With --configure, set the global speed limit in MiB/s; 0 means unlimited. Applies immediately.', "Avec --configure, fixer le débit global en Mio/s ; 0 signifie illimité. Effet immédiat."),
 ('--request-rate-limit MIB_PER_SECOND', 'With --configure, set the default speed limit for new requests in MiB/s; 0 means unlimited. Existing requests and the global speed limit stay unchanged.', 'Avec --configure, fixer le débit maximal par défaut des nouvelles demandes en Mio/s ; 0 signifie illimité. Les demandes existantes et le débit global restent inchangés.'),
 ('--exclude-folder PATTERN, --exclude-file PATTERN', 'With --configure, replace the corresponding pattern list. Repeat an option for multiple patterns; quote shell wildcards. Unspecified lists remain unchanged.', "Avec --configure, remplacer la liste de motifs correspondante. Répéter une option pour plusieurs motifs ; protéger les jokers par des guillemets. Les listes non fournies restent inchangées."),
 ('--clear-exclusions', 'With --configure, clear both pattern lists. Later pattern options add rules. Does not change hidden item handling.', "Avec --configure, vider les deux listes de motifs. Les options de motifs suivantes ajoutent des règles. Ne modifie pas le traitement des éléments cachés."),
]
def escaped(value):
    """Protéger les caractères de contrôle roff dans les textes et noms d’options."""
    return value.replace('\\', r'\e').replace('-', r'\-')


GAD_OPTIONS.append(('--krusader-ctrl-v, --no-krusader-ctrl-v', 'With --configure, enable or disable Ctrl+V with Gad in Krusader using the active panel. Restart Krusader after changing it. F5 and F6 remain unchanged.', 'Avec --configure, activer ou désactiver Ctrl+V avec Gad dans le panneau actif de Krusader. Redémarrer Krusader après une modification. F5 et F6 restent inchangés.'))

GAD_OPTIONS.append(('--dolphin-ctrl-v, --no-dolphin-ctrl-v', 'With --configure, enable or disable Ctrl+V integration in Dolphin. Restart Dolphin after changing it.', 'Avec --configure, activer ou désactiver Ctrl+V avec Gad dans Dolphin. Redémarrer Dolphin après une modification.'))

GAD_OPTIONS.append(('--dolphin [URL...]', 'Start Dolphin with the Gad watchdog, which checks and repairs lost Ctrl+V shortcuts every second. Existing Dolphin processes must be reopened to load the module.', 'Lancer Dolphin avec la surveillance Gad, qui vérifie et répare les raccourcis Ctrl+V perdus chaque seconde. Les processus Dolphin déjà ouverts doivent être relancés pour charger le module.'))

MAN_TEXT = {'en': {'title': 'Queued file transfer manager for KDE',
        'description': 'Gad manages copies and moves with queuing, pause, resume, verification and '
                       'restoration. Without arguments, it shows the main window. Commands are '
                       'forwarded to the existing instance when available.',
        'paths': 'Sources and destinations accept local paths and file, ftp, sftp, smb and '
                 'kdeconnect URLs supported by KIO Fuse. The relevant services and mounts must be '
                 'available. Quote paths containing spaces.',
        'settings': 'Options supplied with --configure are persistent and reflected in graphical '
                    'settings. New transfers use saved preferences unless explicitly overridden. '
                    'Not all settings apply retroactively to existing transfers. Configure Ctrl+V '
                    'in Dolphin with --configure --dolphin-ctrl-v or --no-dolphin-ctrl-v.',
        'files': 'HTML and Markdown guides in the supported languages.'},
 'fr': {'title': 'Gestionnaire de transferts de fichiers pour KDE',
        'description': 'Gad gère les copies et déplacements avec file d’attente, pause, reprise, '
                       'vérification et restauration. Sans argument, il affiche la fenêtre '
                       'principale. Les commandes sont transmises à l’instance existante '
                       'lorsqu’elle est disponible.',
        'paths': 'Les sources et destinations acceptent les chemins locaux et les URL file, ftp, '
                 'sftp, smb et kdeconnect prises en charge par KIO Fuse. Les services et montages '
                 'correspondants doivent être disponibles. Mettre entre guillemets les chemins '
                 'contenant des espaces.',
        'settings': 'Les options de --configure sont persistantes et se reflètent dans les '
                    'paramètres graphiques. Les nouveaux transferts utilisent les préférences '
                    'enregistrées, sauf surcharge explicite. Les réglages ne sont pas tous '
                    'rétroactifs aux transferts existants. Configurer Ctrl+V dans Dolphin avec '
                    '--configure --dolphin-ctrl-v ou --no-dolphin-ctrl-v.',
        'files': 'Guides HTML et Markdown dans les langues disponibles.'}}


def base_render(name, french=False):
    """La commande publique unique possède un manuel de référence anglais et français."""
    if name != 'gad':
        raise ValueError(name)
    language = 'fr' if french else 'en'
    text = MAN_TEXT[language]
    version = tomllib.loads((ROOT / 'Cargo.toml').read_text())['package']['version']
    lines = ['.\\" Generated by scripts/generate_manpages.py; edit the generator.', f'.TH GAD 1 "" "Gad {version}" "User Commands"', '.SH '+('NOM' if french else 'NAME'), 'gad '+r'\- '+text['title'], '.SH SYNOPSIS', '.B gad', '.RI [ options ]', '.SH DESCRIPTION', escaped(text['description']), '.SH OPTIONS']
    for option, english, translated in GAD_OPTIONS:
        lines += ['.TP', '.B '+escaped(option), escaped(translated if french else english)]
    lines += ['.SH '+('CHEMINS' if french else 'PATHS'), escaped(text['paths']), '.SH '+('EXEMPLES' if french else 'EXAMPLES'), '.nf']
    for command in ['gad --copy "/home/user/My file.txt" --to /media/backup --md5', 'gad --move /home/user/file.txt --to /media/archive --properties', 'gad --paste /media/backup', 'gad --status', 'gad --configure --dolphin-ctrl-v --konqueror-ctrl-v', 'gad --configure --rate-limit 25 --exclude-folder "__*" --exclude-file "*.tmp"']:
        lines.append(escaped(command))
    lines += ['.fi', '.SH '+('RÉGLAGES' if french else 'SETTINGS'), escaped(text['settings']), '.SH '+('FICHIERS' if french else 'FILES'), '.TP', '.I /usr/share/gad/docs/', escaped(text['files']), '.SH '+('VOIR AUSSI' if french else 'SEE ALSO'), '.BR dolphin (1), konqueror (1)', '.SH '+('AUTEUR' if french else 'AUTHOR'), 'Terence Belleguic <hizo@free.fr>.', '']
    return '\n'.join(lines)


LANGUAGES = ('en', 'fr', 'es', 'pt', 'de', 'ja')


def filename(name, language):
    return name + ('.' + language if language != 'en' else '') + '.1'


def render(name, language='en'):
    """Exiger un catalogue complet : aucun repli anglais silencieux dans un manuel traduit."""
    if language not in LANGUAGES:
        raise ValueError(language)
    if language in ('en', 'fr'):
        return base_render(name, language == 'fr').replace('"User Commands"', '"Commandes utilisateur"' if language == 'fr' else '"User Commands"')
    catalog = json.loads((ROOT / 'packaging/manpages' / (language + '.json')).read_text())
    translated = []
    for line in base_render(name).splitlines():
        if line.startswith('.TH '):
            line = line.replace('User Commands', {'es':'Órdenes de usuario','pt':'Comandos do utilizador','de':'Benutzerbefehle','ja':'ユーザーコマンド'}[language])
        elif line.startswith('.SH '):
            line = '.SH ' + catalog['headings'][line[4:]]
        elif line.startswith(escaped(name) + r' \- '):
            line = escaped(name) + r' \- ' + catalog['titles'][name]
        elif line and not line.startswith(('.', 'gad', 'Terence')):
            line = escaped(catalog['texts'][line])
        # roff ne coupe pas spontanément les suites de caractères japonais.
        if language == "ja" and not line.startswith("."):
            line = re.sub(r"([\u3040-\u30ff\u3400-\u9fff])(?=[\u3040-\u30ff\u3400-\u9fff])", r"\1\\:", line)
        if language == "ja" and not line.startswith("."):
            translated.append(".ad l")
        translated.append(line)
    return '\n'.join(translated) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    for name in ('gad',):
        for language in LANGUAGES:
            target = ROOT / 'packaging' / filename(name, language)
            contents = render(name, language)
            if args.check:
                if not target.exists() or target.read_text() != contents:
                    raise SystemExit(f'Manpage à régénérer : {target}')
            else:
                target.write_text(contents)


if __name__ == '__main__':
    main()
