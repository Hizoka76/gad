#include "i18n.h"
// Ouverture des éléments du menu contextuel par les associations du bureau, sans commande shell.
#include "window.h"
#include <QDesktopServices>

/// Retrouve la sélection capturée, indépendamment d’un repli automatique ou d’un changement de filtre.
QList<RowPaths::Location> Window::locationsForSelection(const QJsonObject &selection) const {
    QList<RowPaths::Location> locations;
    const auto append=[&](QTreeWidgetItem *item) {
        if(!item)return;
        const auto path=item->data(0,Qt::UserRole+4).toString();
        if(!path.isEmpty())locations.append({path,item->data(0,Qt::UserRole+5).toBool()});
    };
    for(const auto &id:selection["jobs"].toArray())append(jobRows.value(id.toInteger(),nullptr));
    for(const auto &value:selection["entries"].toArray()) {
        const auto entry=value.toObject();append(entryRow(entry["job"].toInteger(),entry["index"].toInt(-1)));
    }
    return locations;
}

/// Un clic utilise l’état frais : une remise en état peut avoir changé le fichier depuis l’affichage du menu.
void Window::openSelectedFolders() {
    const auto selection=selectionRequest(QString());refresh();
    QSet<QString> opened;
    for(const auto &location:locationsForSelection(selection)) {
        const auto folder=location.directory?location.path:QFileInfo(location.path).absolutePath();
        if(opened.contains(folder))continue;
        opened.insert(folder);
        if(!QDesktopServices::openUrl(QUrl::fromLocalFile(folder)))error(GAD_TR("Unable to open folder: ")+folder);
    }
}

/// Une ouverture reste une ouverture par association : le système choisit l’application et ses éventuelles confirmations.
void Window::launchSelectedFile() {
    const auto selection=selectionRequest(QString());
    if(!selection["jobs"].toArray().isEmpty() || selection["entries"].toArray().size()!=1)return;
    refresh();const auto locations=locationsForSelection(selection);
    if(locations.size()!=1 || locations[0].directory)return;
    const auto path=locations[0].path;
    if(!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))error(GAD_TR("Unable to open file: ")+path);
}
