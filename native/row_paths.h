#pragma once
// Choix du chemin représenté par une ligne, sans consulter le système de fichiers.
#include <QtWidgets>
#include "protocol.h"

namespace RowPaths {
struct Location {
    QString path;
    bool directory=false;
};
/// Une copie validée désigne son résultat ; une ligne ignorée, annulée ou restaurée désigne sa source.
inline Location entry(const QJsonObject &value) {
    if(value["hidden"].toBool())return {};
    const auto path=value[value["state"]==Protocol::State::Done?"destination":"source"].toString();
    if(!QDir::isAbsolutePath(path))return {};
    return {path,value["kind"]=="directory"};
}
/// Un lot ouvre son dossier de destination, sans lancer arbitrairement l’un de ses fichiers.
inline Location group(const QJsonObject &job) {
    const auto path=job["destination"].toString();
    return QDir::isAbsolutePath(path)?Location{path,true}:Location{};
}
/// Mémoriser uniquement les données d’ouverture évite des recherches dans tout l’historique au survol d’un menu.
inline void store(QTreeWidgetItem *item,const Location &location) {
    item->setData(0,Qt::UserRole+4,location.path);
    item->setData(0,Qt::UserRole+5,location.directory);
}
}
