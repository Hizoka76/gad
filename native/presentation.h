#pragma once
#include "i18n.h"
// Présentation des états, icônes et chemins ; préférences de démarrage XDG.
#include <QtWidgets>
#include "clipboard.h"

/// Emplacement XDG de l’entrée de démarrage propre au compte utilisateur.
inline QString autostartPath() {
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)+"/autostart/org.hizoka.gad.desktop";
}
/// L’entrée de session est la source de vérité, même si elle a été modifiée dans KDE.
inline bool autostartEnabled() {
    if(!QFileInfo::exists(autostartPath()))return false;
    QSettings entry(autostartPath(),QSettings::IniFormat);
    return !entry.value("Desktop Entry/Hidden",false).toBool();
}
/// Écrit atomiquement le lanceur ou retire uniquement l’entrée appartenant à Gad.
inline bool setAutostart(bool enabled) {
    const auto path=autostartPath();
    if(!enabled)return !QFileInfo::exists(path) || QFile::remove(path);
    if(!QDir().mkpath(QFileInfo(path).absolutePath()))return false;
    // Les règles Exec des fichiers desktop exigent leur propre échappement, sans shell.
    auto executable=QCoreApplication::applicationFilePath();
    executable.replace("\\","\\\\").replace("\"","\\\"").replace("`","\\`").replace("$","\\$").replace("%","%%");
    executable.replace("\\","\\\\"); // Échappement supplémentaire de la valeur Desktop Entry.
    const auto contents=QString("[Desktop Entry]\nType=Application\nName=Gad\nExec=\"%1\" --background\nIcon=gad-boucles-document\nTerminal=false\nStartupNotify=false\n").arg(executable).toUtf8();
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents)==contents.size() && file.commit();
}
/// Le journal garde la trace d’un résultat, d’un temporaire ou d’un original à restaurer.
/// Les octets copiés couvrent aussi les anciens historiques dont le journal est incomplet.
inline bool hasTransferChanges(const QJsonObject &entry) {
    if(entry["copied"].toDouble()>0)return true;
    const auto record=entry["undo"].toObject();
    for(const auto &key:{"result","backup","staging","restore_staging"})if(record.contains(key) && !record[key].isNull())return true;
    return record["source_removed"].toBool() || record["source_dir_removed"].toBool() || record["directory_move"].toBool() || record["rename_move"].toBool();
}
/// Traduit l’état métier et ajoute la pause d’affichage sans modifier l’état du moteur.
inline QString label(const QString &state, bool paused = false, bool unchanged = false) {
    if(state==Protocol::State::Undone && unchanged)return GAD_TR("Canceled");
    if(state==Protocol::State::DeviceWaiting)return paused?GAD_TR("Device disconnected · paused"):GAD_TR("Waiting for device");
    if (paused) {
        if (state == Protocol::State::Running || state == Protocol::State::Waiting || state == Protocol::State::Verifying) return GAD_TR("Paused");
        if (state == Protocol::State::Conflict) return GAD_TR("Paused · conflict");
        if (state == Protocol::State::Scanning) return GAD_TR("Scanning… · queue paused");
    }
    // Le cache évite de reconstruire les traductions pour chaque fichier à chaque actualisation.
    static QString language;
    static QMap<QString, QString> names;
    if(language!=QLocale().name()) {
        language=QLocale().name();
        names = {{"scanning", GAD_TR("Scanning…")}, {"waiting", GAD_TR("Queued")}, {"running", GAD_TR("Running")}, {"verifying", GAD_TR("Verifying MD5…")}, {"done", "OK"}, {"error", GAD_TR("Error")}, {"cancelled", GAD_TR("Canceled")}, {"undo_waiting", GAD_TR("Restoration queued")}, {"undoing", GAD_TR("Restoring…")}, {"undone", GAD_TR("Restored")}, {"undo_error", GAD_TR("Restoration incomplete")}, {"skipped", GAD_TR("Skipped")}, {"conflict", GAD_TR("Decision required")}};
    }
    return names.value(state, state);
}
/// Reconnaît les états terminés pour le nettoyage, les menus et le compteur de lots.
inline bool finished(const QString &s) { return Protocol::terminal(s); }
/// Formate les octets selon la langue et les unités de l’environnement Qt.
inline QString bytes(double n) { return QLocale().formattedDataSize(qint64(n)); }

/// Charge une seule fois l’icône embarquée ; l’initialisation de la ressource est nécessaire dans une archive statique.
inline const QIcon &applicationIcon() {
    static const QIcon icon = [] {
        Q_INIT_RESOURCE(gad_assets);
        return QIcon(":/icons/gad.png");
    }();
    return icon;
}

// Generate the desktop sizes using Qt, preserving the selected image and alpha.
/// Exporte les tailles de bureau de façon atomique et ignore les images déjà identiques.
inline int exportIcons(const QString &root) {
    int changed = 0;
    for (int size : {16, 22, 24, 32, 48, 64, 128, 256, 512}) {
        const auto directory = QDir(root).filePath(QString("%1x%1/apps").arg(size));
        if (!QDir().mkpath(directory)) return 1;
        QByteArray bytes;
        QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly);
        if (!applicationIcon().pixmap(size, size).save(&buffer, "PNG")) return 1;
        const auto path = QDir(directory).filePath("gad-boucles-document.png");
        QFile current(path);
        if (current.open(QIODevice::ReadOnly) && current.readAll() == bytes) continue;
        current.close();
        QSaveFile output(path);
        if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) return 1;
        ++changed;
    }
    std::printf("{\"changed\":%d}\n", changed);
    return 0;
}

// Paint at each supported size so the badge stays legible on small Plasma panels.
/// Dessine l’icône de notification et son compteur ; le dessin dépend de la pause et des conflits.
inline QIcon transferIcon(int count, bool paused, bool conflict) {
    QIcon icon;
    for (int size : {22, 24, 32, 48, 64, 128}) {
        QPixmap image(size, size); image.fill(Qt::transparent);
        QPainter painter(&image); painter.setRenderHint(QPainter::Antialiasing);
        painter.drawPixmap(QRect(0, 0, size, size), applicationIcon().pixmap(size, size));
        painter.scale(size / 64.0, size / 64.0);
        painter.setPen(QPen(QColor("#ffffff"), 2));
        if (count > 0) {
            painter.setBrush(conflict ? QColor("#c53636") : paused ? QColor("#b66a00") : QColor("#146b3a"));
            painter.drawRoundedRect(QRectF(24, 1, 39, 30), 10, 10);
            const QString number = count > 99 ? "99+" : QString::number(count);
            QFont font = QApplication::font(); font.setBold(true); font.setPixelSize(number.size() > 2 ? 17 : 24);
            painter.setFont(font); painter.setPen(Qt::white);
            painter.drawText(QRectF(24, 1, 39, 30), Qt::AlignCenter, number);
        }
        painter.end(); icon.addPixmap(image);
    }
    return icon;
}

/// Affiche l’URL d’origine d’une source distante, y compris pour les descendants d’un dossier.
inline QString sourceLabel(const QJsonObject &job, const QString &path) {
    const auto roots=job["sources"].toArray(), urls=job["source_urls"].toArray();
    for(int i=0;i<roots.size() && i<urls.size();++i) {
        const auto url=QUrl::fromEncoded(urls[i].toString().toUtf8(),QUrl::StrictMode);
        if(!SourceUrls::remoteScheme(url.scheme()))continue;
        const auto relative=QDir(roots[i].toString()).relativeFilePath(path);
        if(relative==".." || relative.startsWith("../") || QDir::isAbsolutePath(relative))continue;
        auto display=url;
        if(relative!=".")display.setPath(url.path().endsWith('/')?url.path()+relative:url.path()+"/"+relative);
        return display.toDisplayString();
    }
    return path;
}

/// Présente une destination distante et ses descendants sans exposer le montage interne.
inline QString destinationLabel(const QJsonObject &job, const QString &path) {
    const QJsonObject mapping{{"sources",QJsonArray{job["destination"]}}, {"source_urls",QJsonArray{job["destination_url"]}}};
    return sourceLabel(mapping,path);
}

/// Widgets d’un côté de la comparaison ; leur groupe Qt possède les labels.
struct ConflictFilePanel {
    QGroupBox *box = nullptr;
    QLabel *name = nullptr, *path = nullptr, *metadata = nullptr;
};

/// Fenêtre propriétaire des widgets et adaptateur des actions utilisateur vers le moteur.
