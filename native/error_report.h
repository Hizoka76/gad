#pragma once
#include "i18n.h"
// Export lisible et machine du même instantané ; aucune commande de copie n’est exécutée ici.
#include <QtCore>
#include "presentation.h"
namespace ErrorReport {
inline QJsonObject job(const QJsonObject &report,qint64 id) {
    for(const auto &value:report["jobs"].toArray())if(value.toObject()["id"].toInteger()==id)return value.toObject();
    return {};
}
inline QString source(const QJsonObject &report,const QJsonObject &record) {
    const auto metadata=job(report,record["job"].toInteger());
    if(record["source"].isArray()) {QStringList names;for(const auto &path:record["source"].toArray())names.append(sourceLabel(metadata,path.toString()));return names.join("\n");}
    return sourceLabel(metadata,record["source"].toString());
}
inline QString destination(const QJsonObject &report,const QJsonObject &record) {
    return destinationLabel(job(report,record["job"].toInteger()),record["destination"].toString());
}
/// Texte brut UTF-8 : les noms contenant sauts de ligne ou caractères HTML ne sont jamais interprétés.
inline QString detail(const QJsonObject &report,const QJsonObject &record) {
    QString text=QString(GAD_TR("Request #%1 · %2 · %3\nSource: %4\nDestination: %5\nError: %6\n"))
        .arg(record["job"].toInteger()).arg(record["mode"]=="move"?GAD_TR("Move"):GAD_TR("Copy"),label(record["state"].toString()),source(report,record),destination(report,record),I18n::message(record["error"].toString()));
    if(record["scope"]=="entry")text+=QString(GAD_TR("Type: %1\nCopied: %2 / %3 bytes\n")).arg(record["kind"]=="directory"?GAD_TR("Folder"):record["kind"]=="link"?GAD_TR("Symbolic link"):GAD_TR("File")).arg(record["copied"].toInteger()).arg(record["size"].toInteger());
    if(!record["partial_path"].toString().isEmpty())text+=GAD_TR("Partial file: ")+record["partial_path"].toString()+"\n";
    if(!record["durable_bytes"].isNull() && record.contains("durable_bytes"))text+=QString(GAD_TR("Resumable data: %1 bytes\n")).arg(record["durable_bytes"].toInteger());
    if(record["published"].toBool())text+=GAD_TR("A file has already been created; it will be verified before any retry.\n");
    for(const auto &name:QStringList{"md5_source","md5_destination"})if(!record[name].toString().isEmpty())text+=name+" : "+record[name].toString()+"\n";
    if(record["properties"].isObject())text+=GAD_TR("Properties:\n")+QString::fromUtf8(QJsonDocument(record["properties"].toObject()).toJson(QJsonDocument::Indented));
    text+=record["retryable"].toBool()?GAD_TR("Retry available (recognized remnants will be restored before copying again).\n"):GAD_TR("Retry unavailable: ")+I18n::message(record["retry_reason"].toString())+"\n";
    return text;
}
inline QByteArray encode(const QJsonObject &report,bool json) {
    if(json)return QJsonDocument(report).toJson(QJsonDocument::Indented);
    QString text=QString(GAD_TR("Gad — Error report\nDate: %1\n%2 error(s)\n\n"))
        .arg(QDateTime::fromSecsSinceEpoch(report["generated_unix"].toInteger(),QTimeZone::UTC).toString(Qt::ISODate)).arg(report["records"].toArray().size());
    for(const auto &record:report["records"].toArray())text+=detail(report,record.toObject())+"\n";
    return text.toUtf8();
}
/// QSaveFile remplace le rapport seulement après une écriture complète, y compris si le disque est plein.
inline bool save(const QString &path,const QJsonObject &report,bool json,QString &error) {
    QSaveFile file(path);const auto data=encode(report,json);
    if(!file.open(QIODevice::WriteOnly) || file.write(data)!=data.size() || !file.commit()) {error=file.errorString();return false;}
    return true;
}
/// Les références restent stables après tri ; les lignes du bilan sont ciblées sans élargir un dossier à ses voisins.
inline QJsonObject retryRequest(const QList<QJsonObject> &records) {
    QJsonArray jobs,entries;QSet<QString> seen;
    for(const auto &record:records) {
        if(!record["retryable"].toBool())continue;
        const auto id=record["job"].toInteger();const auto key=QString::number(id)+"/"+(record["index"].isNull()?"job":QString::number(record["index"].toInt()));
        if(seen.contains(key)) {continue;}
        seen.insert(key);
        if(record["scope"]=="job")jobs.append(id);else entries.append(QJsonObject{{"job",id},{"index",record["index"]}});
    }
    return {{"command",Protocol::Command::RetryErrors},{"jobs",jobs},{"entries",entries},{"exact",true}};
}
}
