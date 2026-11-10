#pragma once
// Le catalogue d’export relie les langues de l’interface aux noms réels des documents installés.
#include "i18n.h"

namespace Documentation {
struct Link {QString kind,title;QUrl html,markdown;};
/// Autoriser uniquement un fichier de documentation situé dans sa racine, même à travers un lien symbolique.
inline QUrl localFile(const QString &root,const QString &relative,const QString &extension) {
    if(relative.isEmpty() || QDir::isAbsolutePath(relative) || QFileInfo(relative).suffix()!=extension)return {};
    const auto base=QFileInfo(root).canonicalFilePath();
    const QFileInfo file(QDir(root).filePath(relative));const auto path=file.canonicalFilePath();
    if(base.isEmpty() || path.isEmpty() || !file.isFile() || !path.startsWith(base+"/"))return {};
    return QUrl::fromLocalFile(path);
}
/// Chercher les documents installés, puis ceux placés à côté d’un exécutable portable ou de développement.
inline QStringList roots() {
    QStringList result;
    for(const auto &directory:QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation))result.append(directory+"/gad/docs");
    const auto application=QCoreApplication::applicationDirPath();result.append(application+"/docs");
    if(QStringList{"debug","release"}.contains(QFileInfo(application).fileName()))result.append(QDir(application).filePath("../../docs"));
    return result;
}
/// Chaque type garde la meilleure édition disponible ; un fichier manquant ne crée jamais de lien mort.
inline QList<Link> links(const QLocale &locale=QLocale()) {
    QList<QPair<QString,QJsonObject>> records;
    for(const auto &root:roots()) {
        QFile file(root+"/catalog.json");if(!file.open(QIODevice::ReadOnly))continue;
        const auto catalog=QJsonDocument::fromJson(file.readAll()).object();
        if(catalog["format"]!="gad-documentation" || catalog["version"].toInt()!=1)continue;
        for(const auto &value:catalog["guides"].toArray())records.append({root,value.toObject()});
    }
    QList<Link> result;
    for(const auto &kind:QStringList{"quick","user","developer"}) {
        Link best;int bestRank=4;
        for(const auto &record:records) {
            const auto entry=record.second;if(entry["kind"].toString()!=kind || entry["title"].toString().isEmpty())continue;
            const auto language=entry["locale"].toString();const QLocale candidate(language);
            const int rank=language==locale.name()?0:candidate.language()==locale.language()?1:language=="en_US"?2:3;
            if(rank>=bestRank || rank==3)continue;
            const auto html=localFile(record.first,entry["html"].toString(),"html"),markdown=localFile(record.first,entry["md"].toString(),"md");
            if(html.isEmpty() && markdown.isEmpty())continue;
            best={kind,entry["title"].toString(),html,markdown};bestRank=rank;
        }
        if(bestRank<3)result.append(best);
    }
    return result;
}
/// Les adresses locales et les titres sont échappés indépendamment pour le texte enrichi Qt.
inline QString html() {
    const auto available=links();
    if(available.isEmpty())return "<p>"+GAD_TR("Documentation is not installed.").toHtmlEscaped()+"</p>";
    QStringList lines;
    for(const auto &entry:available) {
        const auto primary=entry.html.isEmpty()?entry.markdown:entry.html;
        auto line=QString("<a href=\"%1\">%2</a>").arg(primary.toString(QUrl::FullyEncoded).toHtmlEscaped(),entry.title.toHtmlEscaped());
        if(!entry.html.isEmpty() && !entry.markdown.isEmpty())line+=QString(" · <a href=\"%1\">Markdown</a>").arg(entry.markdown.toString(QUrl::FullyEncoded).toHtmlEscaped());
        lines.append(line);
    }
    return "<p><b>"+GAD_TR("Documentation").toHtmlEscaped()+"</b><br>"+lines.join("<br>")+"</p>";
}
}
