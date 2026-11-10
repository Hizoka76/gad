#include "window.h"
#include "bridge.h"
#include "protocol.h"
#include <algorithm>

/// Capture toutes les options affichées, sans source, destination ni état de la file.
TransferProfiles::Profile Window::currentTransferProfile() const {
    TransferProfiles::Profile profile;profile.id=QUuid::createUuid().toString(QUuid::WithoutBraces);
    profile.options={{"verify_fast_move",verifyFastMove->isChecked()},{"conflict_policy",conflictPolicy->currentData().toString()},{"verify_md5",verifyMd5->isChecked()},{"verify_properties",verifyProperties->isChecked()},{"verify_space",verifySpace->isChecked()},{"allow_duplication",allowDuplication->isChecked()},{"exclusions",exclusionRules},{"keep_history",keepHistory->isChecked()},{"resume_interrupted",resumeInterrupted->isChecked()},{"rate_limit",qRound64(globalRate->value()*1048576)}};
    profile.gui={{"progressMessage",progressMessage->isChecked()},{"krusader_ctrl_v",findChild<QCheckBox *>("krusaderShortcut")->isChecked()},{"completion_action",completionAction->currentData().toString()},{"completion_delay",completionDelay->value()},{"autostart",findChild<QCheckBox *>("launchAtStartup")->isChecked()},{"dolphin_ctrl_v",findChild<QCheckBox *>("dolphinShortcut")->isChecked()},{"konqueror_ctrl_v",findChild<QCheckBox *>("konquerorShortcut")->isChecked()},{"language",languageChoice}};
    profile.rate=qRound64(requestRate->value()*1048576);return profile;
}

/// Une configuration validée groupe les options internes ; les deux actions externes ont leur propre retour arrière.
static QJsonObject profileConfiguration(const TransferProfiles::Profile &profile) {
    auto request=profile.options;auto gui=profile.gui;gui.remove("autostart");gui.remove("dolphin_ctrl_v");gui["request_rate_limit"]=profile.rate;
    request["command"]=Protocol::Command::Configure;request["gui_settings"]=gui;return request;
}

/// Empêcher les changements concurrents de réglages pendant le traitement asynchrone du raccourci.
void Window::setProfileApplying(bool applying) {
    profileApplying=applying;
    for(const auto &name:QStringList{"profilesButton","languageMenuButton","settingsPanel","shortcutsPanel","ratesPanel","completionPanel"})if(auto *widget=findChild<QWidget *>(name))widget->setEnabled(!applying);
    if(auto *dialog=findChild<QDialog *>("profilesDialog"))dialog->setEnabled(!applying);
}

/// Préparer les effets externes, appliquer toutes les options, et restaurer les états précédents en cas d’échec.
bool Window::applyTransferProfile(const QString &id,std::function<void(QJsonObject)> reply) {
    const auto fail=[this,reply](const QString &message){if(reply)reply({{"error",message}});else error(message);return false;};
    if(completionExecuting){return fail(GAD_TR("A completion action is being dispatched; no operation was added"));}
    if(profileApplying){return fail(GAD_TR("A transfer profile is already being applied."));}
    QString failure;const auto profiles=TransferProfiles::load(failure);if(!failure.isEmpty()){return fail(failure);}
    auto found=std::find_if(profiles.begin(),profiles.end(),[&](const auto &profile){return profile.id==id;});
    if(found==profiles.end()){return fail(GAD_TR("This transfer profile no longer exists."));}
    const auto profile=*found;
    if(!languageAvailable(profile.gui["language"].toString())){return fail(GAD_TR("The language saved in this profile is unavailable."));}
    auto *dolphin=findChild<QCheckBox *>("dolphinShortcut");
    if(dolphin->property("configuring").toBool() || findChild<QCheckBox *>("krusaderShortcut")->property("configuring").toBool()){return fail(GAD_TR("A transfer profile is already being applied."));}
    const auto previous=currentTransferProfile();const bool changeDolphin=profile.gui["dolphin_ctrl_v"].toBool()!=previous.gui["dolphin_ctrl_v"].toBool();
    const bool changeKrusader=profile.gui.contains("krusader_ctrl_v") && profile.gui["krusader_ctrl_v"]!=previous.gui["krusader_ctrl_v"];
    const bool changeStartup=profile.gui["autostart"].toBool()!=autostartEnabled();
    const bool startupExisted=QFileInfo::exists(autostartPath());QByteArray startupBytes;QFileDevice::Permissions startupPermissions{};
    if(changeStartup && startupExisted){QFile file(autostartPath());if(!file.open(QIODevice::ReadOnly) || file.size()>1024*1024){return fail(GAD_TR("Unable to change Gad's startup setting."));}startupBytes=file.readAll();startupPermissions=file.permissions();if(file.error()!=QFile::NoError){return fail(GAD_TR("Unable to change Gad's startup setting."));}}
    setProfileApplying(true);
    const auto finish=[this,reply](const QString &message){setProfileApplying(false);refreshProfilesList();if(reply)reply(message.isEmpty()?QJsonObject{{"ok",true}}:QJsonObject{{"error",message}});else if(!message.isEmpty())error(message);};
    auto startupApplied=std::make_shared<bool>(false);
    const auto restoreStartup=[startupApplied,startupExisted,startupBytes,startupPermissions]{
        if(!*startupApplied)return true;
        if(!startupExisted)return !QFileInfo::exists(autostartPath()) || QFile::remove(autostartPath());
        QSaveFile file(autostartPath());return file.open(QIODevice::WriteOnly) && file.write(startupBytes)==startupBytes.size() && file.setPermissions(startupPermissions) && file.commit();
    };
    const auto rollback=[this,previous,changeDolphin,changeKrusader,restoreStartup,finish](QString message,bool engineChanged){
        bool restored=restoreStartup();
        if(engineChanged){QScopedValueRollback<bool> guard(profileCommitting,true);if(handle(profileConfiguration(previous)).contains("error"))restored=false;}
        {auto *startup=findChild<QCheckBox *>("launchAtStartup");QSignalBlocker blocker(startup);startup->setChecked(autostartEnabled());}
        const auto done=[finish,message,restored](QJsonObject result){auto text=message;if(!restored || result.contains("error"))text+="\n"+GAD_TR("Some settings could not be restored after the profile failed. Check the options.");finish(text);};
        const auto restoreDolphin=[this,previous,changeDolphin,done](QJsonObject krusader){
            const auto final=[done,krusader](QJsonObject result){done(krusader.contains("error")?krusader:result);};
            if(changeDolphin)configureDolphin(previous.gui["dolphin_ctrl_v"].toBool(),final);else final({{"ok",true}});
        };
        if(changeKrusader)configureKrusader(previous.gui["krusader_ctrl_v"].toBool(),restoreDolphin);else restoreDolphin({{"ok",true}});
    };
    auto completed=std::make_shared<bool>(true);
    const auto commit=[this,profile,changeStartup,startupApplied,rollback,finish,completed](QJsonObject external){
        if(external.contains("error")){*completed=false;rollback(external["error"].toString(),false);return;}
        if(changeStartup){if(!setAutostart(profile.gui["autostart"].toBool())){*completed=false;rollback(GAD_TR("Unable to change Gad's startup setting."),false);return;}*startupApplied=true;}
        const auto result=[&]{QScopedValueRollback<bool> guard(profileCommitting,true);return handle(profileConfiguration(profile));}();
        if(result.contains("error")){*completed=false;rollback(result["error"].toString(),true);return;}
        {auto *startup=findChild<QCheckBox *>("launchAtStartup");QSignalBlocker blocker(startup);startup->setChecked(autostartEnabled());}
        finish({});
    };
    const auto applyDolphin=[this,profile,changeDolphin,commit](QJsonObject krusader){if(krusader.contains("error")){commit(krusader);return;}if(changeDolphin)configureDolphin(profile.gui["dolphin_ctrl_v"].toBool(),commit);else commit({{"ok",true}});};
    if(changeKrusader)configureKrusader(profile.gui["krusader_ctrl_v"].toBool(),applyDolphin);else applyDolphin({{"ok",true}});
    return *completed;
}

/// Résumé complet des paramètres sauvegardés ; aucun chemin ne fait partie du profil.
QString Window::transferProfileDescription(const TransferProfiles::Profile &profile) const {
    const auto speed=[](qint64 rate){return rate?QLocale().toString(rate/1048576.0,'f',2)+GAD_TR(" MiB/s"):GAD_TR("Unlimited");};
    QStringList completionLines{GAD_TR("Batch completion")+": "+Completion::label(profile.gui["completion_action"].toString()),QString(GAD_TR("Countdown: %1 s…")).arg(profile.gui["completion_delay"].toInt())};
    QStringList lines{GAD_TR("File conflicts:")+" "+conflictPolicy->itemText(conflictPolicy->findData(profile.options["conflict_policy"].toString())),GAD_TR("Global speed limit:")+" "+speed(profile.options["rate_limit"].toInteger()),GAD_TR("Default limit for new requests:")+" "+speed(profile.rate)};
    const auto flag=[&](bool enabled,const QString &text){lines.append((enabled?QString::fromUtf8("✓ "):QString::fromUtf8("— "))+text);};
    flag(profile.options["verify_fast_move"].toBool(),GAD_TR("Verify fast moves"));
    flag(profile.options["verify_md5"].toBool(),GAD_TR("Verify files using MD5"));flag(profile.options["verify_properties"].toBool(),GAD_TR("Verify properties"));flag(profile.options["verify_space"].toBool(),GAD_TR("Check available space"));flag(profile.options["keep_history"].toBool(),GAD_TR("Keep history"));flag(profile.options["resume_interrupted"].toBool(),GAD_TR("Resume after interruption"));flag(profile.options["allow_duplication"].toBool(),GAD_TR("Allow duplication in the same folder"));
    const auto rules=profile.options["exclusions"].toObject();flag(rules["include_hidden"].toBool(),GAD_TR("Include hidden items"));
    const auto patterns=[](const QJsonArray &values){QStringList names;for(const auto &value:values)names.append(value.toString());return names.join(", ");};
    lines.append(GAD_TR("Excluded folders")+": "+patterns(rules["directories"].toArray()));lines.append(GAD_TR("Excluded files and symbolic links")+": "+patterns(rules["files"].toArray()));
    flag(profile.gui["progressMessage"].toBool(),GAD_TR("Show transfer progress in KDE"));
    flag(profile.gui["autostart"].toBool(),GAD_TR("Launch at startup"));flag(profile.gui["dolphin_ctrl_v"].toBool(),GAD_TR("Ctrl+V with Gad in Dolphin"));flag(profile.gui["konqueror_ctrl_v"].toBool(),GAD_TR("Ctrl+V with Gad in Konqueror"));
    if(profile.gui.contains("krusader_ctrl_v"))flag(profile.gui["krusader_ctrl_v"].toBool(),GAD_TR("Ctrl+V with Gad in Krusader"));
    auto language=profile.gui["language"].toString();if(language=="system")language=GAD_TR("System");else for(const auto &available:I18n::languages())if(available.locale==language){language=available.name;break;}
    lines.append(GAD_TR("Language")+": "+language);return (lines+completionLines).join('\n');
}

/// Le menu applique directement le snapshot choisi ; aucune opération de fichier n’est lancée.
void Window::setupProfiles(QHBoxLayout *row) {
    auto *button=I18n::localized(new QToolButton,"text",[]{return GAD_TR("Profiles");});button->setObjectName("profilesButton");
    button->setIcon(QIcon::fromTheme("bookmarks",style()->standardIcon(QStyle::SP_DirHomeIcon)));button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);button->setPopupMode(QToolButton::InstantPopup);
    I18n::bindProperty(button,"toolTip",[]{return GAD_TR("Apply all saved options without changing the destination.");});
    profilesMenu=new QMenu(button);profilesMenu->setObjectName("profilesMenu");button->setMenu(profilesMenu);row->addWidget(button);
    connect(profilesMenu,&QMenu::aboutToShow,this,[this]{refreshProfilesMenu();});
    I18n::onChange(profilesMenu,"profiles",[this]{refreshProfilesMenu();refreshProfilesList();});
}

/// Relire au moment de l’ouverture évite d’appliquer une ancienne définition après un renommage.
void Window::refreshProfilesMenu() {
    if(!profilesMenu)return;
    profilesMenu->clear();QString failure;auto profiles=TransferProfiles::load(failure);
    std::sort(profiles.begin(),profiles.end(),[](const auto &a,const auto &b){return QString::localeAwareCompare(a.name,b.name)<0;});
    const auto current=currentTransferProfile();
    for(const auto &profile:profiles) {
        auto *action=profilesMenu->addAction(QIcon::fromTheme("bookmarks"),QString(profile.name).replace('&',"&&"));action->setData(profile.id);action->setObjectName("applyProfile_"+profile.id);
        auto comparedGui=current.gui;if(!profile.gui.contains("krusader_ctrl_v"))comparedGui.remove("krusader_ctrl_v");
        action->setCheckable(true);action->setChecked(profile.options==current.options && profile.gui==comparedGui && profile.rate==current.rate);
        action->setToolTip(transferProfileDescription(profile));connect(action,&QAction::triggered,this,[this,id=profile.id]{QTimer::singleShot(0,this,[this,id]{applyTransferProfile(id);});});
    }
    if(profiles.isEmpty()){auto *empty=profilesMenu->addAction(failure.isEmpty()?GAD_TR("No saved profiles"):failure);empty->setEnabled(false);}
    profilesMenu->addSeparator();auto *save=profilesMenu->addAction(QIcon::fromTheme("document-save"),GAD_TR("Save current settings…"));save->setObjectName("saveProfileAction");save->setEnabled(failure.isEmpty());connect(save,&QAction::triggered,this,[this]{saveCurrentProfile();});
    auto *manage=profilesMenu->addAction(QIcon::fromTheme("configure"),GAD_TR("Manage profiles…"));manage->setObjectName("manageProfilesAction");connect(manage,&QAction::triggered,this,[this]{showProfiles();});
}

/// Enregistrer un snapshot n’applique pas les modifications éventuellement choisies dans cette boîte.
void Window::saveCurrentProfile() {
    if(auto *existing=findChild<QDialog *>("saveProfileDialog")){existing->show();existing->raise();return;}
    auto snapshot=currentTransferProfile();auto *dialog=I18n::localized(new QDialog(this),"windowTitle",[]{return GAD_TR("Save transfer profile");});dialog->setObjectName("saveProfileDialog");dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setMinimumSize(600,300);
    auto *layout=new QVBoxLayout(dialog);auto *help=I18n::localized(new QLabel,"text",[]{return GAD_TR("Save all current options.\nThe destination is never included.");});help->setWordWrap(true);layout->addWidget(help);
    auto *preview=new QPlainTextEdit;preview->setObjectName("profilePreview");preview->setReadOnly(true);I18n::onChange(preview,"summary",[this,preview,snapshot]{preview->setPlainText(transferProfileDescription(snapshot));});layout->addWidget(preview);dialog->resize(750,600);
    auto *form=new QFormLayout;auto *name=new QLineEdit;name->setObjectName("profileName");name->setMaxLength(100);form->addRow(I18n::localized(new QLabel,"text",[]{return GAD_TR("Name:");}),name);
    layout->addLayout(form);
    auto *failure=new QLabel;failure->setObjectName("profileSaveError");failure->setTextFormat(Qt::PlainText);failure->setWordWrap(true);layout->addWidget(failure);
    auto *buttons=new QDialogButtonBox;auto *save=I18n::localized(new QPushButton(QIcon::fromTheme("document-save"),QString()),"text",[]{return GAD_TR("Save profile");});save->setObjectName("confirmSaveProfile");buttons->addButton(save,QDialogButtonBox::AcceptRole);
    auto *cancel=I18n::localized(new QPushButton(QIcon::fromTheme("dialog-cancel"),QString()),"text",[]{return GAD_TR("Cancel");});buttons->addButton(cancel,QDialogButtonBox::RejectRole);layout->addWidget(buttons);connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,dialog,[this,dialog,snapshot,name,failure]()mutable {
        QString message;auto profiles=TransferProfiles::load(message);if(!message.isEmpty()){failure->setText(message);return;}
        snapshot.name=name->text();
        if(!TransferProfiles::validate(snapshot,message)){failure->setText(message);return;}
        profiles.append(snapshot);if(!TransferProfiles::store(profiles,message)){failure->setText(message);return;}
        refreshProfilesMenu();refreshProfilesList();dialog->accept();
    });dialog->open();name->setFocus();
}

/// Actualiser la liste conserve l’identifiant sélectionné, y compris lors d’un changement de langue.
void Window::refreshProfilesList() {
    auto *dialog=findChild<QDialog *>("profilesDialog");if(!dialog)return;
    auto *list=dialog->findChild<QListWidget *>("profilesList");
    const auto selected=list->currentItem()?list->currentItem()->data(Qt::UserRole).toString():QString();
    QString failure;const auto profiles=TransferProfiles::load(failure);
    {QSignalBlocker blocker(list);list->clear();for(const auto &profile:profiles){auto *item=new QListWidgetItem(profile.name,list);item->setData(Qt::UserRole,profile.id);item->setToolTip(transferProfileDescription(profile));if(profile.id==selected)list->setCurrentItem(item);}}
    if(!list->currentItem() && list->count()){QSignalBlocker blocker(list);list->setCurrentRow(0);}
    updateProfileSelection();
}

/// Une sélection ne reconstruit pas les items : préserver double-clic, survol et références Qt.
void Window::updateProfileSelection() {
    auto *dialog=findChild<QDialog *>("profilesDialog");if(!dialog)return;
    auto *list=dialog->findChild<QListWidget *>("profilesList");
    const auto id=list->currentItem()?list->currentItem()->data(Qt::UserRole).toString():QString();
    QString failure;const auto profiles=TransferProfiles::load(failure);QString detail=failure;
    for(const auto &profile:profiles)if(profile.id==id)detail=transferProfileDescription(profile);
    dialog->findChild<QPlainTextEdit *>("profileDetail")->setPlainText(detail);
    for(const auto &name:QStringList{"applyProfile","updateProfile","renameProfile","deleteProfile"})dialog->findChild<QPushButton *>(name)->setEnabled(!id.isEmpty() && failure.isEmpty() && !profileApplying);
    dialog->findChild<QPushButton *>("saveNewProfile")->setEnabled(failure.isEmpty() && !profileApplying);
}

/// Gestion des définitions uniquement : supprimer un profil ne modifie aucun transfert ni fichier.
void Window::showProfiles() {
    if(auto *existing=findChild<QDialog *>("profilesDialog")){refreshProfilesList();existing->show();existing->raise();return;}
    auto *dialog=I18n::localized(new QDialog(this),"windowTitle",[]{return GAD_TR("Transfer profiles");});dialog->setObjectName("profilesDialog");dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setMinimumSize(800,450);dialog->resize(900,650);
    auto *layout=new QVBoxLayout(dialog);auto *help=I18n::localized(new QLabel,"text",[]{return GAD_TR("Profiles save all options, including global settings.\nApplying a profile does not change the destination or start a transfer.");});help->setWordWrap(true);layout->addWidget(help);
    auto *list=new QListWidget;list->setObjectName("profilesList");layout->addWidget(list);auto *detail=new QPlainTextEdit;detail->setObjectName("profileDetail");detail->setReadOnly(true);layout->addWidget(detail,2);
    auto *actions=new QHBoxLayout;const auto button=[&](const QString &name,const QString &icon,std::function<QString()> text){auto *b=I18n::localized(new QPushButton(QIcon::fromTheme(icon),QString()),"text",text);b->setObjectName(name);actions->addWidget(b);return b;};
    auto *apply=button("applyProfile","dialog-ok-apply",[]{return GAD_TR("Apply profile");});auto *save=button("saveNewProfile","document-save-as",[]{return GAD_TR("Save current settings…");});auto *update=button("updateProfile","document-save",[]{return GAD_TR("Update from current settings");});layout->addLayout(actions);
    auto *bottom=new QHBoxLayout;actions=bottom;auto *rename=button("renameProfile","edit-rename",[]{return GAD_TR("Rename profile…");});auto *remove=button("deleteProfile","edit-delete",[]{return GAD_TR("Delete profile");});bottom->addStretch();auto *close=button("closeProfiles","window-close",[]{return GAD_TR("Close");});layout->addLayout(bottom);
    const auto selected=[list]{return list->currentItem()?list->currentItem()->data(Qt::UserRole).toString():QString();};
    connect(close,&QPushButton::clicked,dialog,&QDialog::accept);connect(save,&QPushButton::clicked,this,[this]{saveCurrentProfile();});
    connect(apply,&QPushButton::clicked,this,[this,selected]{applyTransferProfile(selected());});
    connect(list,&QListWidget::itemDoubleClicked,this,[this](QListWidgetItem *item){applyTransferProfile(item->data(Qt::UserRole).toString());});
    connect(list,&QListWidget::currentRowChanged,this,[this]{updateProfileSelection();});
    connect(update,&QPushButton::clicked,this,[this,selected]{QString failure;auto profiles=TransferProfiles::load(failure);if(!failure.isEmpty()){error(failure);return;}for(auto &profile:profiles)if(profile.id==selected()){const auto snapshot=currentTransferProfile();profile.options=snapshot.options;profile.gui=snapshot.gui;profile.rate=snapshot.rate;if(!TransferProfiles::store(profiles,failure)){error(failure);return;}refreshProfilesMenu();refreshProfilesList();return;}});
    connect(remove,&QPushButton::clicked,this,[this,selected]{QString failure;auto profiles=TransferProfiles::load(failure);if(!failure.isEmpty()){error(failure);return;}const auto id=selected();profiles.erase(std::remove_if(profiles.begin(),profiles.end(),[id](const auto &profile){return profile.id==id;}),profiles.end());if(!TransferProfiles::store(profiles,failure)){error(failure);return;}refreshProfilesMenu();refreshProfilesList();});
    connect(rename,&QPushButton::clicked,this,[this,dialog,selected]{
        const auto id=selected();QString failure;const auto profiles=TransferProfiles::load(failure);if(!failure.isEmpty()){error(failure);return;}
        for(const auto &profile:profiles)if(profile.id==id){auto *input=new QInputDialog(dialog);input->setObjectName("renameProfileDialog");input->setAttribute(Qt::WA_DeleteOnClose);I18n::bindProperty(input,"windowTitle",[]{return GAD_TR("Rename profile…");});I18n::bindProperty(input,"labelText",[]{return GAD_TR("Name:");});input->setTextValue(profile.name);I18n::onChange(input,"buttons",[input]{input->setOkButtonText(GAD_TR("Rename profile…"));input->setCancelButtonText(GAD_TR("Cancel"));});
            connect(input,&QInputDialog::textValueSelected,this,[this,id](const QString &name){QString failure;auto profiles=TransferProfiles::load(failure);if(!failure.isEmpty()){error(failure);return;}for(auto &profile:profiles)if(profile.id==id){profile.name=name;if(!TransferProfiles::store(profiles,failure)){error(failure);return;}refreshProfilesMenu();refreshProfilesList();return;}});input->open();return;}
    });
    // Le peuplement initial a lieu après la construction de tous les boutons.
    refreshProfilesList();dialog->open();
}
