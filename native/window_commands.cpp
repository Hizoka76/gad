#include "i18n.h"
#include "protocol.h"
// Implémentation de la fenêtre Gad, séparée par responsabilité.
#include "window.h"
#include "bridge.h"
#include "paths.h"
#include <algorithm>
#include <functional>

/// Complète et envoie une commande ciblée, puis affiche son éventuelle erreur et rafraîchit la vue.
void Window::performSelection(const QString &command, const QJsonObject &options){
    if(tree->selectedItems().isEmpty())return;
    auto request=selectionRequest(command);
    for(auto it=options.begin();it!=options.end();++it)request[it.key()]=it.value();
    const auto result=engine(request);
    if(result.contains("error"))error(result["error"].toString());
    refresh();
}

/// Les fichiers sélectionnés désignent leur lot ; l’ordre visuel détermine l’ordre stable des sélections.
void Window::reorderSelection(const QString &action) {
    QJsonArray rows;
    for(auto *item:tree->selectedItems()) {
        bool covered=false;for(auto *ancestor=item->parent();ancestor;ancestor=ancestor->parent())if(ancestor->isSelected()){covered=true;break;}
        if(!covered)rows.append(TransferTree::key(item));
    }
    if(rows.isEmpty())return;
    const auto result=engine({{"command",Protocol::Command::ReorderRows},{"rows",rows},{"action",action}});
    if(result.contains("error"))error(result["error"].toString());
    refresh();
}

/// Choisit l’annulation simple ou la remise en état, en conservant la granularité des lignes.
void Window::cancelSelection(bool restore){ performSelection(restore ? "undo_selection" : "cancel_selection"); }

/// Associe Suppr au retrait/arrêt et Retour arrière à la restauration des lignes terminées.
void Window::deleteSelection(bool restore){ performSelection(restore ? "backspace_selection" : "delete_selection"); }

/// Adapte les actions du clic droit aux états de la sélection et évite une seconde restauration concurrente.
void Window::updateRowActions(){
    bool active=false,stopped=false,undoable=false,restoring=false,retryBlocked=false,recoverable=false;
    for(auto *item:tree->selectedItems()) {
        const auto status=item->data(0,Qt::UserRole+3).toString();
        active |= !finished(status) && status!=Protocol::State::Skipped;
        bool itemStopped=status==Protocol::State::Cancelled;
        undoable |= status!=Protocol::State::Undone;
        restoring |= status==Protocol::State::Undoing || status==Protocol::State::UndoWaiting;
        itemStopped |= RowTree::hasCancelled(item);
        stopped |= itemStopped;
        // La réactivation touche le journal du lot ; son travailleur doit être arrêté, même pour un seul enfant.
        const auto owner=RowTree::owner(item);
        recoverable |= owner->data(0,Qt::UserRole+6).toBool();
        retryBlocked |= itemStopped && !finished(owner->data(0,Qt::UserRole+3).toString());
    }
    const auto selected=tree->selectedItems();bool allQueued=!selected.isEmpty(),canUp=false,canDown=false,canNext=false;
    for(auto *item:selected) {
        allQueued &= item->data(0,RowTree::CanQueueRole).toBool();
        auto *parent=item->parent();const int index=parent?parent->indexOfChild(item):tree->indexOfTopLevelItem(item);
        for(int i=0;i<index;++i)if(!(parent?parent->child(i):tree->topLevelItem(i))->isSelected())canUp=true;
        const int count=parent?parent->childCount():tree->topLevelItemCount();
        for(int i=index+1;i<count;++i)if(!(parent?parent->child(i):tree->topLevelItem(i))->isSelected())canDown=true;
        for(auto *ancestor=item;ancestor;ancestor=ancestor->parent()) {
            if(!ancestor->data(0,RowTree::CanQueueRole).toBool())continue;
            const auto *p=ancestor->parent();const int position=p?p->indexOfChild(ancestor):tree->indexOfTopLevelItem(ancestor);
            if(position>0)canNext=true;
        }
    }
    bool retryErrors=false;
    for(auto *item:selected) {
        if(!RowTree::owner(item)->data(0,RowTree::ErrorRetryRole).toBool())continue;
        QList<QTreeWidgetItem *> pending{item};
        while(!pending.isEmpty()){auto *row=pending.takeLast();retryErrors |= row->data(0,RowTree::ErrorRetryEntryRole).toBool() || (RowTree::index(row)<0 && row->data(0,Qt::UserRole+3).toString()==Protocol::State::Error);for(int i=0;i<row->childCount();++i)pending.append(row->child(i));}
    }
    rowRetryErrors->setEnabled(retryErrors);
    rowSelectRequest->setEnabled(!selected.isEmpty());
    rowRate->setEnabled(!selected.isEmpty() && active && !restoring);
    rowNext->setEnabled(allQueued && (canUp || canNext));rowUp->setEnabled(allQueued && canUp);
    rowDown->setEnabled(allQueued && canDown);rowLast->setEnabled(allQueued && canDown);
    rowCancel->setEnabled(active && !restoring);
    rowUndo->setEnabled(undoable && !restoring);
    rowPause->setEnabled(active && !restoring);
    rowResume->setEnabled(active && !restoring);
    rowRetry->setEnabled(stopped && !restoring && !retryBlocked);
    rowRecover->setEnabled(recoverable && !restoring);
    I18n::bindProperty(rowRetry,"toolTip",[=]{return retryBlocked?GAD_TR("Wait for the batch to finish before reactivating its canceled rows."):GAD_TR("Reactivates canceled rows after preparing their restoration.");});
    rowRemove->setEnabled(!tree->selectedItems().isEmpty() && !restoring);
    const auto locations=locationsForSelection(selectionRequest(QString()));
    rowOpenFolder->setEnabled(!locations.isEmpty());
    const bool oneFile=tree->selectedItems().size()==1 && locations.size()==1 && !locations[0].directory;
    rowLaunchFile->setEnabled(oneFile);
    I18n::bindProperty(rowOpenFolder,"toolTip",[=]{return GAD_TR("Opens a file's parent folder, the selected folder, or a batch destination.\nDuplicates are grouped together.");});
    I18n::bindProperty(rowLaunchFile,"toolTip",[=]{return oneFile?GAD_TR("Opens with the associated application:\n")+locations[0].path:GAD_TR("Select a single file to open it.");});
}

namespace {
/// Les ajouts de la fenêtre attendent Démarrer, sauf si un lot a déjà été lancé ou est en préparation distante.
bool manualAdditionWaits(const QJsonObject &state,const QQueue<QJsonObject> &pending) {
    for(const auto &value:state["jobs"].toArray()) {
        const auto job=value.toObject();
        if(!job["start_required"].toBool() && !finished(job["state"].toString()) && !job["cancel"].toBool())return false;
    }
    for(const auto &request:pending)if(!request["prepare_only"].toBool())return false;
    return true;
}
}

/// Dans une file inactive, les ajouts manuels attendent le contrôle de démarrage.
QJsonObject Window::enqueue(const QStringList &sources){
    QJsonArray array; for (const auto &source : sources) array.append(source);
    auto result=handle({{"command", Protocol::Command::Enqueue}, {"sources", array}, {"destination", destination->text()},{"prepare_only",manualAdditionWaits(state,sourceQueue)}});refresh();return result;
}

/// L’ajout dans Gad suit la préparation manuelle ; le collage externe conserve son démarrage immédiat.
QJsonObject Window::pasteInto(const QString &path, bool background){
    destination->setText(path);
    const auto files = Clipboard::current();
    if (!files.error.isEmpty()) return error(files.error);
    auto request = Clipboard::request(files, path);
    request["background"] = background;
    if(!background)request["prepare_only"]=manualAdditionWaits(state,sourceQueue);
    return handle(request);
}

/// Affiche une erreur sans bloquer la réponse JSON au client ni la préparation des lots suivants.
QJsonObject Window::error(const QString &text){
    if(!completionExecuting)cancelCompletion();
    auto *dialog=new QMessageBox(QMessageBox::Warning,"Gad",I18n::message(text),QMessageBox::Ok,this);
    I18n::bindProperty(dialog,"text",[text]{return I18n::message(text);});
    dialog->setObjectName("errorDialog");dialog->setAttribute(Qt::WA_DeleteOnClose);
    // Un chemin contenant des balises reste un nom de fichier, jamais du texte enrichi.
    dialog->setTextFormat(Qt::PlainText);dialog->open();
    return {{"error",text}};
}

/// Retire les lots encore hors du moteur et invalide leurs réponses asynchrones.
int Window::cancelSourcePreparation(){
    const int count=sourceQueue.size();
    ++sourceGeneration;
    if(sourceResolver)sourceResolver->deleteLater();
    sourceResolver.clear();sourceQueue.clear();resolvingSources=false;
    statusBar()->clearMessage();statusBar()->hide();
    return count;
}

/// Résout les sources dans l’ordre d’arrivée, puis utilise le même moteur que les copies locales.
void Window::resolveNextSources(){
    // Une ancienne relance différée ne doit pas ouvrir un second résolveur pour le même lot.
    if(sourceResolver)return;
    if(sourceQueue.isEmpty()) { resolvingSources=false;statusBar()->clearMessage();statusBar()->hide();return; }
    resolvingSources=true;
    const auto generation=sourceGeneration;
    statusBar()->showMessage(QString(GAD_TR("Accessing remote files… %1 batch(es) being prepared")).arg(sourceQueue.size()));statusBar()->show();
    sourceResolver=new SourceUrls::Resolver(sourceQueue.head(),this,[this,generation](QJsonObject result) {
        if(generation!=sourceGeneration)return;
        sourceResolver.clear();sourceQueue.dequeue();
        if(result.contains("error")) error(result["error"].toString());
        else { result["_sources_resolved"]=true;handle(result); }
        // Une annulation pendant une boîte d’erreur invalide aussi cette relance différée.
        QTimer::singleShot(0,this,[this,generation]{if(generation==sourceGeneration)resolveNextSources();});
    });
    sourceResolver->start();
}

/// Le script Python reste un composant privé, trouvé dans les données installées ou l’arbre de développement.
QString Window::dolphinIntegration() {
    auto path=QStandardPaths::locate(QStandardPaths::GenericDataLocation,"gad/integration/dolphin_shortcut.py");
    if(path.isEmpty() && QStringList{"debug","release"}.contains(QFileInfo(QCoreApplication::applicationDirPath()).fileName())) {
        const auto candidate=QCoreApplication::applicationDirPath()+"/../../scripts/dolphin_shortcut.py";
        if(QFileInfo(candidate).isFile())path=QFileInfo(candidate).canonicalFilePath();
    }
    return path;
}

/// Partage le même traitement asynchrone entre la case et la commande publique de Gad.
void Window::configureDolphin(bool enabled,std::function<void(QJsonObject)> reply) {
    auto *check=findChild<QCheckBox *>("dolphinShortcut");
    if(check->property("configuring").toBool()){reply({{"error","Dolphin shortcut configuration is already running."}});return;}
    const auto script=dolphinIntegration();const auto python=QStandardPaths::findExecutable("python3");
    if(script.isEmpty() || python.isEmpty()){reply({{"error",GAD_TR("Unable to change the Dolphin shortcut.")}});return;}
    const auto old=check->isChecked();
    check->setProperty("configuring",true);check->setEnabled(false);
    auto *process=new QProcess(this);auto *timeout=new QTimer(process);timeout->setSingleShot(true);
    connect(timeout,&QTimer::timeout,process,[process]{process->kill();});
    const auto finish=[check,process,enabled,old,reply](bool success){
        if(process->property("finishedSettings").toBool())return;
        process->setProperty("finishedSettings",true);check->setProperty("configuring",false);check->setEnabled(true);
        {QSignalBlocker blocker(check);check->setChecked(success?enabled:old);}
        const auto detail=QString::fromUtf8(process->readAllStandardError()).trimmed();
        reply(success?QJsonObject{{"ok",true}}:QJsonObject{{"error",detail.isEmpty()?GAD_TR("Unable to change the Dolphin shortcut."):detail}});
        process->deleteLater();
    };
    connect(process,&QProcess::finished,this,[finish](int code,QProcess::ExitStatus status){finish(code==0 && status==QProcess::NormalExit);});
    connect(process,&QProcess::errorOccurred,this,[finish](QProcess::ProcessError error){if(error==QProcess::FailedToStart)finish(false);});
    process->start(python,{script,enabled?"--enable-ctrl-v":"--disable-ctrl-v"});timeout->start(8000);
}

/// Configure l’action utilisateur Krusader sans bloquer les transferts.
void Window::configureKrusader(bool enabled,std::function<void(QJsonObject)> reply) {
    auto *check=findChild<QCheckBox *>("krusaderShortcut");
    if(check->property("configuring").toBool()){reply({{"error","Krusader shortcut configuration is already running."}});return;}
    auto script=QStandardPaths::locate(QStandardPaths::GenericDataLocation,"gad/integration/krusader_shortcut.py");
    if(script.isEmpty() && QStringList{"debug","release"}.contains(QFileInfo(QCoreApplication::applicationDirPath()).fileName()))script=QCoreApplication::applicationDirPath()+"/../../scripts/krusader_shortcut.py";
    const auto python=QStandardPaths::findExecutable("python3");
    if(script.isEmpty() || python.isEmpty()){reply({{"error",GAD_TR("Unable to change the Krusader shortcut.")}});return;}
    const auto old=check->isChecked();
    check->setProperty("configuring",true);check->setEnabled(false);
    auto *process=new QProcess(this);auto *timeout=new QTimer(process);timeout->setSingleShot(true);
    connect(timeout,&QTimer::timeout,process,[process]{process->kill();});
    const auto finish=[check,process,enabled,old,reply](bool success){
        if(process->property("finishedSettings").toBool())return;
        process->setProperty("finishedSettings",true);check->setProperty("configuring",false);check->setEnabled(true);
        {QSignalBlocker blocker(check);check->setChecked(success?enabled:old);}
        const auto detail=QString::fromUtf8(process->readAllStandardError()).trimmed();
        reply(success?QJsonObject{{"ok",true}}:QJsonObject{{"error",detail.isEmpty()?GAD_TR("Unable to change the Krusader shortcut."):detail}});
        process->deleteLater();
    };
    connect(process,&QProcess::finished,this,[finish](int code,QProcess::ExitStatus status){finish(code==0 && status==QProcess::NormalExit);});
    connect(process,&QProcess::errorOccurred,this,[finish](QProcess::ProcessError error){if(error==QProcess::FailedToStart)finish(false);});
    process->start(python,{script,enabled?"--enable-ctrl-v":"--disable-ctrl-v","--binary",QCoreApplication::applicationFilePath()});timeout->start(8000);
}

void Window::handleAsync(QJsonObject request,std::function<void(QJsonObject)> reply) {
    if(completionExecuting && (request["command"]==Protocol::Command::Configure || request["command"]==Protocol::Command::Enqueue || request["command"]==Protocol::Command::StartPrepared || request["command"]==Protocol::Command::Paste)){reply({{"error",GAD_TR("A completion action is being dispatched; no operation was added")}});return;}
    if(request.contains("profile")) {
        const auto name=request.take("profile");
        if(!name.isString() || name.toString().trimmed().isEmpty() || name.toString().size()>100){reply({{"error",GAD_TR("Enter a profile name (up to 100 characters).")}});return;}
        if(!QStringList{Protocol::Command::Configure,Protocol::Command::Enqueue,Protocol::Command::Paste,Protocol::Command::Background}.contains(request["command"].toString())){reply({{"error",GAD_TR("Invalid request")}});return;}
        if(request["command"]==Protocol::Command::Paste){const auto files=Clipboard::current();if(!files.error.isEmpty()){reply({{"error",files.error}});return;}const auto context=request.value("destination_context");auto paste=Clipboard::request(files,request["destination"].toString());if(context.isArray())paste["destination_context"]=context;paste["background"]=true;request=paste;}
        QString failure;const auto profiles=TransferProfiles::load(failure);if(!failure.isEmpty()){reply({{"error",failure}});return;}
        const auto key=TransferProfiles::nameKey(name.toString().trimmed());
        for(const auto &profile:profiles)if(TransferProfiles::nameKey(profile.name)==key){
            applyTransferProfile(profile.id,[this,request,reply](QJsonObject result){if(result.contains("error") || (request["command"]==Protocol::Command::Configure && request.size()==1)){reply(result);return;}handleAsync(request,reply);});return;
        }
        reply({{"error",GAD_TR("Transfer profile not found.")},{"profile",name}});return;
    }
    if(profileApplying && !profileCommitting && request["command"]==Protocol::Command::Configure){reply({{"error",GAD_TR("A transfer profile is already being applied.")}});return;}
    // Le même scan reçu par la socket reste asynchrone, comme celui de la fenêtre.
    if(request["command"]==Protocol::Command::Simulate) {
        auto result=std::make_shared<QJsonObject>();auto *worker=QThread::create([request,result]{*result=engine(request);});
        connect(worker,&QThread::finished,this,[reply,result]{reply(*result);});
        connect(worker,&QThread::finished,worker,&QObject::deleteLater);worker->start();return;
    }
    auto gui=request["gui_settings"].toObject();
    if(gui.contains("progressMessage") && !gui["progressMessage"].isBool()){reply({{"error",GAD_TR("Invalid transfer profile settings.")}});return;}
    if(gui.contains("completion_action") && (!gui["completion_action"].isString() || !Completion::validAction(gui["completion_action"].toString()))){reply({{"error",GAD_TR("Invalid completion action")}});return;}
    if(gui.contains("completion_delay") && !Completion::validDelay(gui["completion_delay"])){reply({{"error",GAD_TR("Invalid countdown: use 5 to 3600 whole seconds.")}});return;}

    if(request["command"]==Protocol::Command::Configure && gui.contains("krusader_ctrl_v")) {
        if(!gui["krusader_ctrl_v"].isBool()){reply({{"error",GAD_TR("Invalid transfer profile settings.")}});return;}
        const bool enabled=gui.take("krusader_ctrl_v").toBool();request["gui_settings"]=gui;
        configureKrusader(enabled,[this,request,reply](QJsonObject result){if(result.contains("error") || (request.size()==2 && request["gui_settings"].toObject().isEmpty())){reply(result);return;}handleAsync(request,reply);});return;
    }
    if(request["command"]!=Protocol::Command::Configure || !gui.contains("dolphin_ctrl_v")){reply(handle(request));return;}
    if(gui.contains("language") && !languageAvailable(gui["language"].toString())){reply({{"error","Unknown language: "+gui["language"].toString()}});return;}
    const bool enabled=gui.take("dolphin_ctrl_v").toBool();request["gui_settings"]=gui;
    if(gui.contains("request_rate_limit") && !TransferProfiles::validRate(gui["request_rate_limit"])) {reply({{"error",GAD_TR("Invalid transfer profile settings.")}});return;}
    configureDolphin(enabled,[this,request,reply](QJsonObject result){
        if(result.contains("error")){reply(result);return;}
        if(request.size()==2 && request["gui_settings"].toObject().isEmpty()){reply(result);return;}
        reply(handle(request));
    });
}

/// Traite les commandes d’interface avant délégation au moteur ; un collage en arrière-plan ne relève pas la fenêtre.
QJsonObject Window::handle(QJsonObject request){
    const auto command = request["command"].toString();
    if(completionExecuting && (command==Protocol::Command::Enqueue || command==Protocol::Command::StartPrepared || command==Protocol::Command::Paste || command==Protocol::Command::Configure))return {{"error",GAD_TR("A completion action is being dispatched; no operation was added")}};
    if(command==Protocol::Command::Enqueue && completionPlan.counting){completionPlan.counting=false;if(auto *dialog=findChild<QDialog *>("completionCountdown")){dialog->setObjectName("completionCountdownClosed");dialog->accept();}}

    if(request.contains("conflict_policy") && (!request["conflict_policy"].isString() || !QStringList{"ask","skip","newer","rename"}.contains(request["conflict_policy"].toString())))return {{"error","Invalid conflict policy"}};
    if(request.contains("verify_fast_move") && !request["verify_fast_move"].isBool())return {{"error","Invalid fast-move verification option"}};
    if(request.contains("profile"))return {{"error",GAD_TR("Invalid request")}};
    const auto gui=request.take("gui_settings").toObject();
    if(gui.contains("progressMessage") && !gui["progressMessage"].isBool())return {{"error",GAD_TR("Invalid transfer profile settings.")}};
    if(gui.contains("completion_action") && (!gui["completion_action"].isString() || !Completion::validAction(gui["completion_action"].toString())))return {{"error",GAD_TR("Invalid completion action")}};
    if(gui.contains("completion_delay") && !Completion::validDelay(gui["completion_delay"]))return {{"error",GAD_TR("Invalid countdown: use 5 to 3600 whole seconds.")}};

    if(profileApplying && !profileCommitting && (command==Protocol::Command::Configure || command==Protocol::Command::Shutdown))return {{"error",GAD_TR("A transfer profile is already being applied.")}};
    if(command==Protocol::Command::Configure) {
        if(gui.contains("request_rate_limit") && !TransferProfiles::validRate(gui["request_rate_limit"]))return {{"error",GAD_TR("Invalid transfer profile settings.")}};
        if(gui.contains("language")) {
            const auto locale=gui["language"].toString();
            if(!languageAvailable(locale))return {{"error","Unknown language: "+locale}};
        }
        // Les modifications partielles conservent les règles et la visibilité non fournies.
        if(request.contains("exclusion_patch")) {
            auto rules=exclusionRules;const auto patch=request.take("exclusion_patch").toObject();
            for(auto it=patch.begin();it!=patch.end();++it)rules[it.key()]=it.value();
            request["exclusions"]=rules;
        }
    }
    if (command == Protocol::Command::Show) {
        showMainWindow(request["activation_token"].toString());
        return {{"ok", true}};
    }
    if (command == Protocol::Command::Background) return {{"ok",true}};
    if (command == Protocol::Command::Paste) {
        if(!request.contains("destination_context"))return pasteInto(request["destination"].toString(),true);
        const auto files=Clipboard::current();
        if(!files.error.isEmpty())return error(files.error);
        auto paste=Clipboard::request(files,request["destination"].toString());
        paste["destination_context"]=request["destination_context"];
        paste["background"]=true;
        return handle(paste);
    }
    if(command==Protocol::Command::Enqueue) {
        QString invalid;const auto path=LocalPaths::destination(request["destination"].toString(),invalid);
        if(!invalid.isEmpty())return error(invalid);
        request["destination"]=path;
    }
    if(command==Protocol::Command::Enqueue && !request.contains("conflict_policy"))request["conflict_policy"]=conflictPolicy->currentData().toString();
    if(command==Protocol::Command::Enqueue && !request.contains("rate_limit"))request["rate_limit"]=qRound64(requestRate->value()*1048576);
    if(command==Protocol::Command::Enqueue && !request.contains("verify_fast_move"))request["verify_fast_move"]=verifyFastMove->isChecked();
    if (command == Protocol::Command::Enqueue && !request.contains("verify_md5")) request["verify_md5"] = verifyMd5->isChecked();
    if(command==Protocol::Command::Enqueue && !request.contains("allow_duplication"))request["allow_duplication"]=allowDuplication->isChecked();
    if(command==Protocol::Command::Enqueue && !request.contains("exclusions"))request["exclusions"]=exclusionRules;
    if(command==Protocol::Command::Enqueue && !request.contains("verify_space"))request["verify_space"]=verifySpace->isChecked();
    if(command==Protocol::Command::Enqueue && !request.contains("verify_properties"))request["verify_properties"]=verifyProperties->isChecked();
    if(command== Protocol::Command::Enqueue && !request.take("_sources_resolved").toBool() && (resolvingSources || SourceUrls::needsResolution(request))) {
        sourceQueue.enqueue(request);
        if(!resolvingSources)resolveNextSources();
        // Le montage n’a pas encore de lot moteur : activer immédiatement les
        // commandes de file, sans attendre le prochain rafraîchissement périodique.
        refresh();
        return {{"ok",true},{"preparing",true}};
    }

    auto result = command==Protocol::Command::Configure && request.size()==1 && !gui.isEmpty()?QJsonObject{{"ok",true}}:engine(request);
    if(command==Protocol::Command::Enqueue && result.contains("error"))cancelCompletion();
    if(!result.contains("error") && (command== Protocol::Command::CancelAll || command== Protocol::Command::UndoAll))
        result["cancelled_preparations"]=cancelSourcePreparation();
    if (command == Protocol::Command::Configure && !result.contains("error")) {
        if(gui.contains("progressMessage")){const bool visible=gui["progressMessage"].toBool();QSignalBlocker blocker(progressMessage);progressMessage->setChecked(visible);QSettings().setValue("progressMessage",visible);desktopTransfers->setProgressMessage(visible);}
        if(gui.contains("completion_action") || gui.contains("completion_delay")){
            cancelCompletion();QSignalBlocker action(completionAction),delay(completionDelay);
            if(gui.contains("completion_action")){completionAction->setCurrentIndex(completionAction->findData(gui["completion_action"].toString()));QSettings().setValue("completionAction",gui["completion_action"].toString());}
            if(gui.contains("completion_delay")){completionDelay->setValue(gui["completion_delay"].toInt());QSettings().setValue("completionDelay",gui["completion_delay"].toInt());}
            updateCompletionControls();
        }

        if(request.contains("verify_fast_move")){QSignalBlocker blocker(verifyFastMove);verifyFastMove->setChecked(request["verify_fast_move"].toBool());QSettings().setValue("verifyFastMove",verifyFastMove->isChecked());}
        if(request.contains("conflict_policy")){QSignalBlocker blocker(conflictPolicy);conflictPolicy->setCurrentIndex(conflictPolicy->findData(request["conflict_policy"].toString()));QSettings().setValue("conflictPolicy",request["conflict_policy"].toString());}
        if(gui.contains("request_rate_limit")){QSignalBlocker blocker(requestRate);const auto bytes=gui["request_rate_limit"].toInteger();requestRate->setValue(bytes/1048576.0);QSettings().setValue("requestRateLimit",bytes);}
        if(request.contains("allow_duplication")){QSignalBlocker blocker(allowDuplication);allowDuplication->setChecked(request["allow_duplication"].toBool());QSettings().setValue("allowDuplication",allowDuplication->isChecked());}
        if(request.contains("exclusions")) {
            const auto supplied=request["exclusions"].toObject();
            const bool directoriesChanged=exclusionRules["directories"].toArray()!=supplied["directories"].toArray();
            const bool filesChanged=exclusionRules["files"].toArray()!=supplied["files"].toArray();
            exclusionRules={{"include_hidden",supplied.value("include_hidden").toBool(true)},{"directories",supplied["directories"].toArray()},{"files",supplied["files"].toArray()}};
            QSignalBlocker blocker(includeHidden);includeHidden->setChecked(exclusionRules["include_hidden"].toBool());
            const auto names=[](const QJsonArray &array){QStringList values;for(const auto &value:array)values.append(value.toString());return values;};
            QSettings settings;settings.setValue("includeHidden",includeHidden->isChecked());settings.setValue("excludeDirectories",names(exclusionRules["directories"].toArray()));settings.setValue("excludeFiles",names(exclusionRules["files"].toArray()));
            // Les changements issus de la CLI ou d’un profil actualisent également les champs intégrés.
            const auto sync=[this](const char *name,const QJsonArray &values){if(auto *edit=findChild<QPlainTextEdit *>(name)){QStringList lines;for(const auto &value:values)lines.append(value.toString());const auto text=lines.join("\n");if(edit->toPlainText()!=text){QSignalBlocker blocker(edit);edit->setPlainText(text);}}};
            if(directoriesChanged)sync("excludedDirectories",exclusionRules["directories"].toArray());
            if(filesChanged)sync("excludedFiles",exclusionRules["files"].toArray());
            updateExclusionSummary();
        }
        if(request.contains("rate_limit")){QSignalBlocker blocker(globalRate);const auto bytes=request["rate_limit"].toInteger();globalRate->setValue(bytes/1048576.0);QSettings().setValue("rateLimit",bytes);}
        if(request.contains("verify_space")){QSignalBlocker blocker(verifySpace);verifySpace->setChecked(request["verify_space"].toBool());QSettings().setValue("verifySpace",verifySpace->isChecked());}
        if(request.contains("verify_properties")){QSignalBlocker blocker(verifyProperties);verifyProperties->setChecked(request["verify_properties"].toBool());QSettings().setValue("verifyProperties",verifyProperties->isChecked());}
        if (request.contains("verify_md5")) { QSignalBlocker blocker(verifyMd5); verifyMd5->setChecked(request["verify_md5"].toBool()); QSettings().setValue("verifyMd5", verifyMd5->isChecked()); }
        if(request.contains("resume_interrupted")){QSignalBlocker blocker(resumeInterrupted);resumeInterrupted->setChecked(request["resume_interrupted"].toBool());QSettings().setValue("resumeInterrupted",resumeInterrupted->isChecked());}
        if (request.contains("keep_history")) { QSignalBlocker blocker(keepHistory); keepHistory->setChecked(request["keep_history"].toBool()); QSettings().setValue("keepHistory", keepHistory->isChecked()); }
        if(gui.contains("autostart")) {
            const auto enabled=gui["autostart"].toBool();if(!setAutostart(enabled))return {{"error",GAD_TR("Unable to change Gad's startup setting.")}};
            auto *check=findChild<QCheckBox *>("launchAtStartup");QSignalBlocker blocker(check);check->setChecked(enabled);
        }
        if(gui.contains("konqueror_ctrl_v")) {
            const auto enabled=gui["konqueror_ctrl_v"].toBool();auto *check=findChild<QCheckBox *>("konquerorShortcut");QSignalBlocker blocker(check);check->setChecked(enabled);QSettings().setValue("konquerorCtrlV",enabled);
        }
        if(gui.contains("language"))setLanguage(gui["language"].toString());
    }
    if (command == Protocol::Command::Enqueue) {
        if (!request["background"].toBool()) { showMainWindow(); }
        if (result.contains("error")) error(result["error"].toString());
        else {
            destination->setText(request.contains("destination_url")?request["destination_url"].toString():request["destination"].toString());
            if (!request["prepare_only"].toBool() && (!isVisible() || isMinimized())) {
                desktopTransfers->begin(result["id"].toInteger());
            }
        }
    }
    refresh(); return result;
}

/// Encode une sélection stable sans dupliquer les enfants déjà couverts par un lot.
QJsonObject Window::selectionRequest(const QString &command) const {
        const auto selected=tree->selectedItems();
        QSet<qint64> wholeJobs;
        for(auto *item:selected)if(RowTree::index(item)<0)wholeJobs.insert(item->data(0,Qt::UserRole).toLongLong());
        QJsonArray jobs,entries;
        auto ids=wholeJobs.values(); std::sort(ids.begin(),ids.end(),std::greater<qint64>());
        for(auto id:ids)jobs.append(id);
        for(auto *item:selected)if(RowTree::index(item)>=0) {
            const auto id=item->data(0,Qt::UserRole).toLongLong();
            bool covered=wholeJobs.contains(id);
            for(auto *ancestor=item->parent();ancestor && !covered;ancestor=ancestor->parent())covered=ancestor->isSelected();
            if(!covered)entries.append(QJsonObject{{"job",id},{"index",item->data(0,RowTree::EntryIndexRole).toInt()}});
        }
        return {{"command",command},{"jobs",jobs},{"entries",entries}};
    }

/// Les identifiants sont capturés à l’ouverture : une nouvelle sélection ne change pas les cibles.
void Window::showRateLimit() {
    QSet<qint64> unique;QJsonArray ids;
    for(auto *item:tree->selectedItems())unique.insert(RowTree::owner(item)->data(0,Qt::UserRole).toLongLong());
    if(unique.isEmpty())return;
    auto ordered=unique.values();std::sort(ordered.begin(),ordered.end());for(auto id:ordered)ids.append(id);
    qint64 initial=0;
    for(const auto &value:state["jobs"].toArray())if(value.toObject()["id"].toInteger()==ordered.first())initial=value.toObject()["rate_limit"].toInteger();
    auto *settings=findChild<QDialog *>("settingsDialog");
    auto *dialog=new QDialog(settings && settings->isVisible()?static_cast<QWidget *>(settings):this);dialog->setObjectName("requestRateDialog");dialog->setAttribute(Qt::WA_DeleteOnClose);
    I18n::bindProperty(dialog,"windowTitle",[=]{return GAD_TR("Limit request speed");});auto *layout=new QVBoxLayout(dialog);
    auto *label=I18n::localized(new QLabel(QString()),"text",[=]{return QString(GAD_TR("Limit for each of the %1 selected request(s).\nApplies to all their rows. The global limit still applies.\n0 = unlimited.")).arg(ids.size());});label->setWordWrap(true);layout->addWidget(label);
    auto *spin=new QDoubleSpinBox;spin->setObjectName("requestRateLimit");spin->setRange(0,1048576);spin->setDecimals(2);I18n::bindProperty(spin,"suffix",[=]{return GAD_TR(" MiB/s");});I18n::bindProperty(spin,"specialValueText",[=]{return GAD_TR("Unlimited");});spin->setValue(initial/1048576.0);layout->addWidget(spin);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,dialog,[this,dialog,spin,ids]{
        const auto result=engine({{"command",Protocol::Command::SetRateLimit},{"ids",ids},{"bytes_per_second",qRound64(spin->value()*1048576)}});
        if(result.contains("error")){error(result["error"].toString());return;}
        dialog->accept();refresh();
    });
    dialog->open();
}

/// Les téléphones et services KIO sont sondés au plus toutes les cinq secondes, hors de la file des nouveaux collages.
void Window::refreshReconnect() {
    QSet<qint64> live;
    for(const auto &value:state["jobs"].toArray()) {
        const auto job=value.toObject();const auto id=job["id"].toInteger();
        if(job["state"]!=Protocol::State::DeviceWaiting)continue;
        live.insert(id);
        if(reconnectResolvers.value(id))continue;
        const auto now=metricsClock.elapsed();if(reconnectAttempts.contains(id) && now-reconnectAttempts[id]<5000)continue;
        QJsonArray sources;const auto paths=job["sources"].toArray(),urls=job["source_urls"].toArray();
        for(int i=0;i<paths.size();++i)sources.append(i<urls.size() && !urls[i].toString().isEmpty()?urls[i]:paths[i]);
        QJsonObject request{{"sources",sources},{"destination",job["destination_url"].toString().isEmpty()?job["destination"]:job["destination_url"]}};
        if(!SourceUrls::needsResolution(request))continue;
        reconnectAttempts[id]=now;
        reconnectResolvers[id]=new SourceUrls::Resolver(request,this,[this,id,paths,destination=job["destination"].toString()](QJsonObject result){
            reconnectResolvers.remove(id);
            bool waiting=false;for(const auto &v:state["jobs"].toArray())if(v.toObject()["id"].toInteger()==id && v.toObject()["state"]==Protocol::State::DeviceWaiting)waiting=true;
            if(!waiting || result.contains("error"))return;
            // Réécrire des chemins de journal sur un nouveau montage pourrait viser un autre fichier : conserver et informer.
            if(result["sources"].toArray()!=paths || result["destination"].toString()!=destination)
                reconnectWarnings[id]=GAD_TR("The remote mount path has changed; restore the original mount or add the files again.");
            else reconnectWarnings.remove(id);
        });
        reconnectResolvers[id]->start();
    }
    for(auto it=reconnectResolvers.begin();it!=reconnectResolvers.end();) {
        if(!live.contains(it.key())){if(it.value())it.value()->deleteLater();it=reconnectResolvers.erase(it);}else ++it;
    }
    for(auto it=reconnectWarnings.begin();it!=reconnectWarnings.end();)if(!live.contains(it.key()))it=reconnectWarnings.erase(it);else ++it;
    for(auto it=reconnectAttempts.begin();it!=reconnectAttempts.end();)if(!live.contains(it.key()))it=reconnectAttempts.erase(it);else ++it;
}

/// Les compteurs décrivent des motifs, pas des fichiers exclus déjà parcourus.
void Window::updateExclusionSummary() {
    const auto directories=exclusionRules["directories"].toArray().size(),files=exclusionRules["files"].toArray().size();
    exclusionSummary->setText(directories+files?QString(GAD_TR("%1 folder rule(s) · %2 file rule(s)")).arg(directories).arg(files):GAD_TR("No name exclusions"));
}

namespace {
/// QPlainTextEdit ne fournit pas editingFinished : valider après sa perte de focus native.
class ExclusionEdit : public QPlainTextEdit {
public:
    std::function<void()> commit;
protected:
    void focusOutEvent(QFocusEvent *event) override {
        QPlainTextEdit::focusOutEvent(event);
        if(commit)commit();
    }
};
}

/// Chaque champ valide ses motifs à la perte de focus, sans toucher au brouillon de l’autre champ.
void Window::setupExclusions(QVBoxLayout *layout) {
    auto *help=I18n::localized(new QLabel,"text",[]{return GAD_TR("One name pattern per line, at any depth.\n* matches any text; ? matches one character.\nFolder exclusions skip their entire contents.\nRules apply to new transfers only; excluded items are kept at the source during moves.");});help->setWordWrap(true);help->setTextFormat(Qt::PlainText);layout->addWidget(help);
    const auto lines=[](const QJsonArray &array){QStringList values;for(const auto &value:array)values.append(value.toString());return values.join("\n");};
    auto *directories=new ExclusionEdit;directories->setObjectName("excludedDirectories");directories->setPlainText(lines(exclusionRules["directories"].toArray()));directories->setPlaceholderText("__*\n__pycache__\nnode_modules");
    auto *files=new ExclusionEdit;files->setObjectName("excludedFiles");files->setPlainText(lines(exclusionRules["files"].toArray()));files->setPlaceholderText("*.tmp\n*.swp\n*~");
    auto *directoryLabel=I18n::localized(new QLabel,"text",[]{return GAD_TR("Excluded folders");});directoryLabel->setBuddy(directories);layout->addWidget(directoryLabel);layout->addWidget(directories);
    auto *fileLabel=I18n::localized(new QLabel,"text",[]{return GAD_TR("Excluded files and symbolic links");});fileLabel->setBuddy(files);layout->addWidget(fileLabel);layout->addWidget(files);
    const auto bind=[this](ExclusionEdit *edit,const QString &key){
        edit->commit=[this,edit,key]{
            const auto patterns=QJsonArray::fromStringList(edit->toPlainText().split('\n',Qt::SkipEmptyParts));
            if(patterns==exclusionRules[key].toArray())return;
            auto rules=exclusionRules;rules[key]=patterns;
            const auto result=handle({{"command",Protocol::Command::Configure},{"exclusions",rules}});
            if(result.contains("error"))error(result["error"].toString());
        };
    };
    bind(directories,"directories");bind(files,"files");
}
