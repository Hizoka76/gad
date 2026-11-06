#pragma once
#include "i18n.h"
#include "protocol.h"
// Lecture des formats de fichiers du presse-papiers KDE/GNOME.
// Le marqueur Couper détermine le déplacement ; les URI sont décodées strictement
// et le recours à wl-paste permet de lire le presse-papiers sans activer Gad.

#include <QtWidgets>
#include <functional>
#include "kio_sources.h"

namespace Clipboard {
/// Résultat indivisible de lecture : chemins, intention Copier/Couper et éventuelle erreur.
struct Files {
    // Chemins locaux décodés ou URL KDE Connect/FTP/SFTP conservées pour leur résolution.
    QStringList sources;
    // Déplacement uniquement si le presse-papiers annonce explicitement Couper.
    QString mode = "copy";
    // Une erreur invalide la sélection complète.
    QString error;
    // Distingue un lecteur indisponible d’un presse-papiers disponible mais invalide.
    bool available = false;
};
// Le décodeur accepte un lecteur injectable pour tester les formats sans toucher au bureau.
using Read = std::function<QByteArray(const QString &)>;

/// Décode les formats KDE/GNOME, le marqueur Couper et les sources locales/KDE Connect/FTP/SFTP.
/// Une seule URI non prise en charge invalide toute la sélection.
inline Files decode(const QStringList &types, const Read &read) {
    Files result;
    result.available = true;
    QByteArray payload;
    // Le marqueur KDE détermine Couper ; GNOME peut aussi encoder ce choix dans sa propre liste.
    const auto cut = types.contains("application/x-kde-cutselection")
        ? read("application/x-kde-cutselection").trimmed() : QByteArray();
    if (cut == "1") result.mode = "move";
    if (types.contains("text/uri-list")) payload = read("text/uri-list");
    else if (types.contains("application/x-kde4-urilist")) payload = read("application/x-kde4-urilist");
    else if (types.contains("x-special/gnome-copied-files")) {
        payload = read("x-special/gnome-copied-files");
        const auto newline = payload.indexOf('\n');
        if (newline < 0) { result.error = GAD_TR("Invalid clipboard file list."); return result; }
        result.mode = payload.left(newline).trimmed() == "cut" ? "move" : "copy";
        payload = payload.mid(newline + 1);
    } else {
        result.error = GAD_TR("The clipboard does not contain a file list. Copy or cut files in your file manager.");
        return result;
    }
    // Les lecteurs externes peuvent rencontrer un nouveau presse-papiers entre le marqueur et les URI.
    // Refuser cette lecture incohérente plutôt que déplacer des fichiers copiés entre-temps.
    if(types.contains("application/x-kde-cutselection") && (types.contains("text/uri-list") || types.contains("application/x-kde4-urilist"))
        && read("application/x-kde-cutselection").trimmed()!=cut) {
        result.error=GAD_TR("The clipboard changed while being read. Paste again.");return result;
    }
    // Les URI sont validées avant ajout et décodées après séparation des lignes encodées.
    for (auto line : payload.split('\n')) {
        if (line.endsWith('\r')) line.chop(1);
        if (line.isEmpty() || line.startsWith('#')) continue;
        const auto url = QUrl::fromEncoded(line, QUrl::StrictMode);
        if (!SourceUrls::supported(url)) {
            result.error = SourceUrls::unsupportedMessage();
            result.sources.clear(); return result;
        }
        result.sources << (url.isLocalFile()?SourceUrls::localPath(url):url.toString(QUrl::FullyEncoded));
    }
    result.sources.removeDuplicates();
    if (result.sources.isEmpty()) result.error = GAD_TR("The clipboard file list is empty or inaccessible.");
    return result;
}

/// Interroge wl-paste avec des délais bornés et sans passer par un interpréteur de commandes.
/// Une lecture partielle en erreur invalide le résultat complet.
inline Files wayland(const QString &helper) {
    // Les arguments sont passés directement à QProcess ; aucun nom de fichier ne devient du code shell.
    auto run = [&](const QStringList &args, QByteArray &output) {
        QProcess process;
        process.start(helper, args);
        if (!process.waitForStarted(500)) return false;
        if (!process.waitForFinished(1500)) { process.kill(); process.waitForFinished(500); return false; }
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) return false;
        output = process.readAllStandardOutput(); return true;
    };
    QByteArray offered;
    if (!run({"--list-types"}, offered)) return {};
    QStringList types;
    for (const auto &line : offered.split('\n')) if (!line.trimmed().isEmpty()) types << QString::fromUtf8(line.trimmed());
    bool failed = false;
    auto result = decode(types, [&](const QString &type) {
        QByteArray data;
        if (!run({"--no-newline", "--type", type}, data)) failed = true;
        return data;
    });
    if (failed) { result.sources.clear(); result.error = GAD_TR("Unable to read files from the Wayland clipboard. Paste again."); }
    return result;
}

/// Privilégie wl-paste sous Wayland, puis utilise QClipboard si ce lecteur est indisponible.
inline Files current() {
    // La lecture Wayland externe évite de devoir afficher la fenêtre simplement pour accéder aux formats.
    if (QGuiApplication::platformName().startsWith("wayland")) {
        const auto helper = QStandardPaths::findExecutable("wl-paste");
        if (!helper.isEmpty()) {
            const auto result = wayland(helper);
            if (result.available) return result;
        }
    }
    const auto *mime = QApplication::clipboard()->mimeData();
    if (!mime) return {{}, "copy", GAD_TR("The clipboard is inaccessible. Try again with the Gad window active."), true};
    auto result = decode(mime->formats(), [mime](const QString &format) { return mime->data(format); });
    if (!result.error.isEmpty() && QGuiApplication::platformName().startsWith("wayland"))
        result.error += GAD_TR("\nIf files are visible in the KDE clipboard, check that wl-paste (the wl-clipboard package) is installed.");
    return result;
}
/// Construit la requête de lot en préservant le mode copie/déplacement issu du presse-papiers.
inline QJsonObject request(const Files &files, const QString &destination) {
    QJsonArray sources;
    for (const auto &source : files.sources) sources.append(source);
    return {{"command", Protocol::Command::Enqueue}, {"sources", sources}, {"destination", destination}, {"mode", files.mode}};
}
}
