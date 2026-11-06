#include "i18n.h"
// Implémentation de la fenêtre Gad, séparée par responsabilité.
#include "window.h"
#include "bridge.h"

/// Lit les métadonnées lors du changement de conflit, pas à chaque tick de progression.
void Window::populateConflictFile(ConflictFilePanel &panel, const QString &path, const QString &displayPath){
    const QFileInfo file(path);
    panel.name->setText(file.fileName());
    panel.path->setText(displayPath.isEmpty()?path:displayPath);
    panel.path->setToolTip(panel.path->text());
    QString details;
    if (file.isSymLink()) details = GAD_TR("Symbolic link → ") + file.symLinkTarget();
    else if (!file.exists()) details = GAD_TR("File unavailable");
    else if (file.isDir()) details = GAD_TR("Folder");
    else details = GAD_TR("Size: ") + bytes(file.size());
    if (file.lastModified().isValid()) details += GAD_TR("\nModified: ") + QLocale().toString(file.lastModified(), QLocale::ShortFormat);
    panel.metadata->setText(details);
}

/// Affiche le conflit choisi et remet le choix « tous les suivants » à zéro pour une nouvelle ligne.
void Window::displayConflict(){
    const int position = conflictSelector->currentIndex();
    if (position < 0 || position >= pendingConflicts.size()) return;
    const auto entry = pendingConflicts[position].toObject();
    const auto key = conflictSelector->currentData().toString();
    conflictJob = entry["job"].toInteger();
    conflictIndex = entry["index"].toInt();
    if (displayedConflictKey == key && displayedConflictLanguage==QLocale().name()) return;
    const bool newConflict=displayedConflictKey!=key;
    displayedConflictLanguage=QLocale().name();
    displayedConflictKey = key;
    if(newConflict)all->setChecked(false);
    all->setText(QString(GAD_TR("Apply this choice to future conflicts in batch #%1")).arg(conflictJob));
    conflictSource.box->setTitle(entry["mode"] == "move" ? GAD_TR("File to move") : GAD_TR("File to copy"));
    populateConflictFile(conflictSource, entry["source"].toString(),entry["source_label"].toString());
    populateConflictFile(conflictDestination, entry["destination"].toString(),entry["destination_label"].toString());
}

/// Conserve la même cible d’action quand un deuxième transfert rencontre aussi un conflit.
void Window::updateConflictPanel(const QJsonArray &entries){
    const auto selectedKey = displayedConflictKey;
    pendingConflicts = entries;
    const QSignalBlocker blocker(conflictSelector);
    QStringList keys, labels;
    for (const auto &value : entries) {
        const auto entry = value.toObject();
        keys.append(QString("%1/%2").arg(entry["job"].toInteger()).arg(entry["index"].toInt()));
        labels.append(QString(GAD_TR("Batch #%1 — %2")).arg(entry["job"].toInteger()).arg(QFileInfo(entry["source"].toString()).fileName()));
    }
    bool changed=conflictSelector->count()!=keys.size();
    for(int i=0;!changed && i<keys.size();++i)changed=conflictSelector->itemData(i).toString()!=keys[i] || conflictSelector->itemText(i)!=labels[i];
    // Conserver le modèle évite de fermer le menu déroulant à chaque tick de progression.
    if(changed) {
        conflictSelector->clear();
        for(int i=0;i<keys.size();++i)conflictSelector->addItem(labels[i],keys[i]);
    }
    const int previous = conflictSelector->findData(selectedKey);
    if (!entries.isEmpty()) conflictSelector->setCurrentIndex(previous >= 0 ? previous : 0);
    conflictSelector->setVisible(entries.size() > 1);
    conflictTitle->setText(entries.size()==1 ? GAD_TR("A file already exists") : QString(GAD_TR("%1 filename conflicts to resolve")).arg(entries.size()));
    if (entries.isEmpty()) {
        conflictJob = 0; conflictIndex = -1; displayedConflictKey.clear();
    } else displayConflict();
    conflictBox->setVisible(!entries.isEmpty());
}

/// Construit une carte de comparaison dont les textes restent copiables et sans interprétation HTML.
ConflictFilePanel Window::makeConflictFilePanel(std::function<QString()> title, const QString &prefix){
    ConflictFilePanel panel;
    panel.box = I18n::localized(new QGroupBox(QString(),conflictBox),"title",std::move(title));
    auto *layout = new QVBoxLayout(panel.box);
    panel.name = new QLabel;
    panel.path = new QLabel;
    panel.metadata = new QLabel;
    panel.name->setObjectName(prefix + "Name");
    panel.path->setObjectName(prefix + "Path");
    panel.metadata->setObjectName(prefix + "Metadata");
    auto font = panel.name->font(); font.setBold(true); panel.name->setFont(font);
    for (auto *label : {panel.name, panel.path, panel.metadata}) {
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        layout->addWidget(label);
    }
    return panel;
}

