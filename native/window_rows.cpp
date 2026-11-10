#include "i18n.h"
#include "protocol.h"
// Implémentation de la fenêtre Gad, séparée par responsabilité.
#include "window.h"
#include "bridge.h"
#include <algorithm>
#include <functional>
#include <QScopeGuard>

/// Le rang parmi les enfants visuels ne représente plus l’indice du moteur.
QTreeWidgetItem *Window::entryRow(qint64 job,int index) const {
    const auto found=entryRows.constFind(job);
    return found==entryRows.cend() || index<0 || index>=found->size()?nullptr:(*found)[index];
}

/// Compteurs et icône restent actifs même lorsque l’arbre est différé en arrière-plan.
QJsonArray Window::refreshSummary(const QSet<qint64> &changed) {
    int waiting=0,running=0,done=0,errors=0,activeJobs=0,runningJobs=0,queuedJobs=0,scanningJobs=0,deviceWaitingJobs=0;
    double totalBytes=0,copiedBytes=0,sessionBytes=0,sessionCopied=0;QJsonArray conflicts;QSet<qint64> live;
    const auto jobs=state["jobs"].toArray();
    for(const auto &value:jobs) {
        const auto job=value.toObject();const auto id=job["id"].toInteger();const auto status=job["state"].toString();live.insert(id);
        if(!finished(status)){++activeJobs;if(status==Protocol::State::Running || status==Protocol::State::Undoing)++runningJobs;else if(status==Protocol::State::Scanning)++scanningJobs;else {++queuedJobs;if(status==Protocol::State::DeviceWaiting)++deviceWaitingJobs;}}
        if(changed.contains(id) || !jobTotals.contains(id)) {
            JobTotals totals;int index=0;
            for(const auto &entry:job["entries"].toArray()) {
                const auto e=entry.toObject();const auto status=e["state"].toString();
                if(e["kind"]!="directory" && !e["hidden"].toBool()) {
                    if(status==Protocol::State::Waiting || status==Protocol::State::DeviceWaiting)++totals.waiting;
                    else if(status==Protocol::State::Running || status==Protocol::State::Verifying || status==Protocol::State::Conflict || status==Protocol::State::Undoing)++totals.running;
                    else if(status==Protocol::State::Done)++totals.done;else if(status==Protocol::State::Error)++totals.errors;
                    totals.bytes+=e["size"].toDouble();totals.copied+=e["copied"].toDouble();
                }
                if(status==Protocol::State::Conflict){auto conflict=e;conflict["job"]=id;conflict["index"]=index;conflict["mode"]=job["mode"];conflict["source_label"]=sourceLabel(job,e["source"].toString());conflict["destination_label"]=destinationLabel(job,e["destination"].toString());totals.conflicts.append(conflict);}++index;
            }
            jobTotals[id]=totals;
        }
        const auto &totals=jobTotals[id];waiting+=totals.waiting;running+=totals.running;done+=totals.done;errors+=totals.errors;totalBytes+=totals.bytes;copiedBytes+=totals.copied;
        if(!progressInitialized){progressOffsets[id]=totals.copied;if(finished(status))progressHistory.insert(id);}
        if(!finished(status))progressHistory.remove(id);
        if(!progressHistory.contains(id)) {
            const auto offset=qMin(progressOffsets.value(id,0.),totals.copied);progressOffsets[id]=offset;
            sessionBytes+=qMax(0.,totals.bytes-offset);sessionCopied+=qMax(0.,totals.copied-offset);
        }
        for(const auto &conflict:totals.conflicts)conflicts.append(conflict);
    }
    for(auto it=jobTotals.begin();it!=jobTotals.end();)if(!live.contains(it.key()))it=jobTotals.erase(it);else ++it;
    progressInitialized=true;
    for(auto it=progressOffsets.begin();it!=progressOffsets.end();)if(!live.contains(it.key())){progressHistory.remove(it.key());it=progressOffsets.erase(it);}else ++it;
    const bool badgePaused=paused || (deviceWaitingJobs>0 && runningJobs==0);const bool decision=!conflicts.isEmpty();
    const QString visualKey=QString("%1/%2/%3").arg(activeJobs).arg(badgePaused).arg(decision);
    if(trayVisualKey!=visualKey){tray->setIcon(transferIcon(activeJobs,badgePaused,decision));trayVisualKey=visualKey;}
    QString tooltip=QString(GAD_TR("Gad — %1 transfer(s) remaining\n%2 active · %3 queued · %4 scanning\nFiles: %5 active · %6 queued")).arg(activeJobs).arg(runningJobs).arg(queuedJobs).arg(scanningJobs).arg(running).arg(waiting);
    if(paused)tooltip+=GAD_TR("\nQueue paused");
    if(deviceWaitingJobs)tooltip+=QString(GAD_TR("\n%1 request(s) waiting for reconnection")).arg(deviceWaitingJobs);
    if(decision)tooltip+=GAD_TR("\nA conflict needs your decision");
    if(tray->toolTip()!=tooltip)tray->setToolTip(tooltip);
    trayPause->setText(paused?GAD_TR("Resume queue"):GAD_TR("Pause all"));progress->setValue(sessionBytes>0?qBound(0,int(1000*sessionCopied/sessionBytes),1000):0);
    summary->setText(QString(GAD_TR("%1 operation(s) · %2 queued · %3 active · %4 OK · %5 error(s)  —  %6 / %7%8")).arg(jobs.size()).arg(waiting).arg(running).arg(done).arg(errors).arg(bytes(copiedBytes)).arg(bytes(totalBytes)).arg(paused?GAD_TR(" · Paused"):""));
    return conflicts;
}

/// Met à jour les lignes existantes à partir d’un instantané JSON, sans reconstruire le modèle.
/// La conservation des items préserve le survol, les infobulles, les indices et la sélection.
void Window::refresh(){
    if (closing) return;
    const auto delta=engine({{"command",Protocol::Command::Poll},{"since",queueSnapshot.revision()}});
    if(delta.contains("error")){summary->setText(I18n::message(delta["error"].toString()));return;}
    const auto changed=queueSnapshot.apply(delta);state=queueSnapshot.state();
    // Un seul contrôle pilote la file, qu’elle soit préparée, active ou en pause.
    bool ready=false,preparing=false,active=false;
    for(const auto &value:state["jobs"].toArray()){const auto job=value.toObject();if(finished(job["state"].toString()) || job["cancel"].toBool())continue;if(job["start_required"].toBool()){preparing|=job["state"]==Protocol::State::Scanning;ready|=job["state"]==Protocol::State::Waiting;}else active=true;}
    for(const auto &request:sourceQueue){if(request["prepare_only"].toBool())preparing=true;else active=true;}
    paused=state["paused"].toBool();
    // Une ancienne pause ne transforme pas une préparation en transfert déjà lancé.
    pause->setText(active?(paused?GAD_TR("Resume queue"):GAD_TR("Pause all")):GAD_TR("Start"));
    pause->setIcon(QIcon::fromTheme(active && !paused?"media-playback-pause":"media-playback-start"));
    pause->setEnabled(!completionExecuting && (active || (ready && !preparing)));
    // Les actions suivent les opérations réelles, même après un nettoyage ou un
    // instantané inchangé. Un filtre visuel ne doit pas bloquer la file masquée.
    bool cancellable=!sourceQueue.isEmpty(),restorable=!sourceQueue.isEmpty(),clearable=false,restoring=false;
    for(const auto &value:state["jobs"].toArray()) {
        const auto job=value.toObject();const auto status=job["state"].toString();
        const bool undo=job["undo_requested"].toBool() || status==Protocol::State::Undoing || status==Protocol::State::UndoWaiting;
        restoring|=undo;
        cancellable|=(!finished(status) && !job["cancel"].toBool() && !undo) || (status==Protocol::State::Error && job["resume_pending"].toBool());
        restorable|=job.value("journaled").toBool(true) && status!=Protocol::State::Undone && !undo;
        clearable|=finished(status) && !undo;
    }
    findChild<QAction *>("cancelAllAction")->setEnabled(cancellable && !completionExecuting);
    findChild<QAction *>("undoAllAction")->setEnabled(restorable && !restoring && !completionExecuting);
    findChild<QPushButton *>("cancellationMenu")->setEnabled(!completionExecuting && (cancellable || (restorable && !restoring)));
    findChild<QPushButton *>("clearHistoryButton")->setEnabled(clearable && !restoring && !completionExecuting);

    if(state.contains("rate_limit")) {
        const auto bytes=state["rate_limit"].toInteger();QSignalBlocker blocker(globalRate);
        globalRate->setValue(bytes/1048576.0);QSettings().setValue("rateLimit",bytes);
    }
    const auto ignored=state["duplicates_ignored"].toInteger();
    if(reportedDuplicates>=0 && ignored>reportedDuplicates) {
        statusBar()->showMessage(QString(GAD_TR("%1 item(s) already queued: not added")).arg(ignored-reportedDuplicates),5000);statusBar()->show();
    }
    reportedDuplicates=ignored;
    // Le suivi natif conserve ses mesures de débit et ses tentatives de reconnexion même sans mutation.
    desktopTransfers->sync(state,!isVisible() || isMinimized());
    // Même sans nouvelle révision, actualiser les mesures pour éviter un débit figé pendant une attente d’E/S.
    refreshReconnect();
    auto metricsLines=metricsTracker.sync(state,changed,metricsClock.elapsed(),delta["reset"].toBool());
    diagnosticTracker.sync(state,metricsTracker,metricsClock.elapsed(),delta["reset"].toBool());
    refreshDiagnostics();
    refreshCompletion(state);
    if(closing)return;
    for(auto it=reconnectWarnings.cbegin();it!=reconnectWarnings.cend();++it)if(!it.value().isEmpty())metricsLines.append(QString("#%1 · %2").arg(it.key()).arg(it.value()));
    const auto metricsText=metricsLines.join("\n");
    if(requestMetrics->text()!=metricsText)requestMetrics->setText(metricsText);
    requestMetrics->setVisible(!metricsLines.isEmpty());
    const auto conflicts=refreshSummary(changed);
    if(deferHiddenTree && (!isVisible() || isMinimized())){treePending=true;return;}
    auto *view=static_cast<TransferTree *>(tree);
    const bool headerChanged=renderedViewRevision!=view->presentationRevision();
    const bool filterChanged=renderedHeaderFilterRevision!=view->headerFilterRevision();
    const auto rawQueueOrder=state["row_order"].toArray();
    const bool orderChanged=rawQueueOrder!=renderedQueueOrder;
    const bool force=treePending || headerChanged || renderedLanguage!=QLocale().name() || renderedBase!=palette().color(QPalette::Base) || filterChanged || renderedPause!=state["paused"].toBool() || delta["reset"].toBool() || !delta.contains("revision");
    if(delta["unchanged"].toBool() && !force && !orderChanged)return;
    treePending=false;
    // La largeur automatique relirait tous les items pendant chaque modification.
    // Suspendre aussi la peinture, puis rétablir exactement les réglages du demandeur.
    const bool updates=tree->updatesEnabled();
    const auto orderResize=tree->header()->sectionResizeMode(TableView::OrderColumn);
    tree->setUpdatesEnabled(false);
    tree->header()->setSectionResizeMode(TableView::OrderColumn,QHeaderView::Fixed);
    const auto rendering=qScopeGuard([this,updates,orderResize]{tree->header()->setSectionResizeMode(TableView::OrderColumn,orderResize);tree->setUpdatesEnabled(updates);});
    renderedQueueOrder=rawQueueOrder;
    renderedViewRevision=view->presentationRevision();
    renderedHeaderFilterRevision=view->headerFilterRevision();
    renderedLanguage=QLocale().name();
    renderedBase=palette().color(QPalette::Base);
    renderedPause=state["paused"].toBool();
    const auto jobs = state["jobs"].toArray();
    // Le numéro vient exclusivement de l’ordre du moteur, jamais du rang après tri ou filtrage.
    auto queueOrder=rawQueueOrder;
    const auto positionKey=[](qint64 id,int index){return QString::number(id)+"/"+QString::number(index);};
    if(queueOrder.isEmpty()) {
        QHash<qint64,QJsonObject> byId;for(const auto &value:jobs){const auto job=value.toObject();byId.insert(job["id"].toInteger(),job);}
        auto ids=state["queue_order"].toArray();if(ids.isEmpty())for(const auto &value:jobs)ids.append(value.toObject()["id"]);
        for(const auto &value:ids) {
            const auto id=value.toInteger();const auto entries=byId.value(id)["entries"].toArray();
            if(entries.isEmpty())queueOrder.append(QJsonObject{{"job",id},{"index",QJsonValue::Null}});
            else for(int i=0;i<entries.size();++i)queueOrder.append(QJsonObject{{"job",id},{"index",i}});
        }
    }
    QHash<QString,qint64> positions;qint64 position=0;
    for(const auto &value:queueOrder){const auto key=value.toObject();positions.insert(positionKey(key["job"].toInteger(),key["index"].isNull()?-1:key["index"].toInt()),++position);}

    QSet<qint64> liveJobs;
    for (const auto &value : jobs) liveJobs.insert(value.toObject()["id"].toInteger());
    for (auto it = jobRows.begin(); it != jobRows.end();) {
        if (!liveJobs.contains(it.key())) {
            QSignalBlocker blocker(tree);jobTotals.remove(it.key());
            const auto rows=entryRows.take(it.key());for(auto *row:rows)RowTree::attach(row,nullptr,tree);
            qDeleteAll(rows);delete it.value();it=jobRows.erase(it);
        }
        else ++it;
    }
    // Keep model indexes, hover, selection and tooltips alive between progress updates.
    // Éviter les écritures de texte identiques limite les signaux Qt et conserve les infobulles actives.
    const auto text = [](QTreeWidgetItem *item, int column, const QString &value, const QString &tooltip=QString()) {
        if (item->text(column) != value) item->setText(column, value);
        const auto tip=tooltip.isNull()?value:tooltip;
        if (item->toolTip(column) != tip) item->setToolTip(column, tip);
    };
    const auto orderText=[&](QTreeWidgetItem *row,qint64 id,int index){
        const auto position=positions.value(positionKey(id,index),0);
        const auto date=row->data(0,HistorySearch::CreatedRole).toLongLong();
        const auto added=date>0?QLocale().toString(QDateTime::fromSecsSinceEpoch(date),QLocale::ShortFormat):GAD_TR("Unknown date (older history)");
        text(row,TableView::OrderColumn,position>0?QString::number(position):QString(),QString(GAD_TR("Batch #%1")).arg(id)+"\n"+GAD_TR("Added:")+" "+added);
        row->setTextAlignment(TableView::OrderColumn,Qt::AlignRight|Qt::AlignVCenter);
    };
    const auto operationIcon=[this](QTreeWidgetItem *row,const QString &mode){
        if(row->data(0,RowTree::OperationRole).toString()==mode)return;
        const bool move=mode=="move";
        row->setIcon(0,QIcon::fromTheme(move?"edit-cut":"edit-copy",style()->standardIcon(move?QStyle::SP_ArrowRight:QStyle::SP_FileIcon)));
        row->setData(0,RowTree::OperationRole,mode);
    };
    // Les dates appartiennent à la demande, y compris pour ses sous-fichiers et ses URL originales.
    const auto searchData=[](QTreeWidgetItem *row,const QJsonObject &job,const QString &paths){
        row->setData(0,HistorySearch::CreatedRole,job["created_at"].toInteger());
        row->setData(0,HistorySearch::JobStateRole,job["state"].toString());
        row->setData(0,HistorySearch::PathsRole,paths);
    };
    for (const auto &value : jobs) {
        const auto job = value.toObject(); const auto id = job["id"].toInteger(); const auto status = job["state"].toString();
        if(force || changed.contains(id) || !jobRows.contains(id)) {
            const auto entries = job["entries"].toArray();
            const auto limit=job["rate_limit"].toInteger();
            const QString rateDetail=limit>0?QString(GAD_TR("Limit: %1")).arg(TransferMetrics::speed(limit)):QString();
            const bool jobChanged=std::any_of(entries.cbegin(),entries.cend(),[](const QJsonValue &entry){return hasTransferChanges(entry.toObject());});
            auto *parent = jobRows.value(id, nullptr);
            if (!parent) {
                parent = new QTreeWidgetItem;
                jobRows.insert(id, parent);
                parent->setData(0, Qt::UserRole, id);
                parent->setData(0,RowTree::EntryIndexRole,-1);
                parent->setData(0, Qt::UserRole + 1, QString::number(id));
                parent->setIcon(0, QIcon::fromTheme("folder-copy"));
                QFont font = parent->font(0); font.setBold(true); parent->setFont(0, font);
                parent->setExpanded(true);
            }
            const bool placeholderSelected=parent->isSelected();
            auto &rows=entryRows[id];
            // Une réanalyse raccourcit parfois la liste : détacher d’abord les survivants évite de supprimer leurs descendants.
            if(rows.size()>entries.size()) {
                for(auto *row:rows)RowTree::attach(row,nullptr,tree);
                while(rows.size()>entries.size())delete rows.takeLast();
            }
            // Construire les nouvelles branches hors du modèle évite une suppression
            // et une réinsertion Qt par fichier lors du rattachement à son dossier.
            const int firstNew=rows.size();
            while(rows.size()<entries.size()) {
                auto *row=new QTreeWidgetItem;
                row->setData(0,RowTree::OwnerRole,QVariant::fromValue(reinterpret_cast<quintptr>(parent)));
                row->setData(0,Qt::UserRole,id);
                row->setData(0,Qt::UserRole+1,QString::number(id)+"/"+QString::number(rows.size()));
                row->setData(0,RowTree::EntryIndexRole,rows.size());
                rows.append(row);
            }
            QVector<QString> sourcePaths,destinationPaths;
            QVector<bool> shown(entries.size(),false),completed(entries.size(),false);
            QHash<QString,QVector<int>> directories;
            const auto directoryKey=[](const QString &source,const QString &target){return source+QChar(0)+target;};
            // Les reprises peuvent ajouter des entrées en fin de liste : indexer tous les dossiers avant de rattacher leurs fichiers.
            for(int i=0;i<entries.size();++i) {
                const auto entry=entries[i].toObject();
                sourcePaths.append(QDir::cleanPath(entry["source"].toString()));destinationPaths.append(QDir::cleanPath(entry["destination"].toString()));
                if(entry["kind"]=="directory")directories[directoryKey(sourcePaths[i],destinationPaths[i])].append(i);
            }
            QVector<QTreeWidgetItem *> folders(rows.size(),nullptr);
            for(int i=0;i<entries.size();++i) {
                auto source=sourcePaths[i],target=destinationPaths[i];QTreeWidgetItem *folder=nullptr;
                while(true) {
                    const auto sourceParent=QFileInfo(source).path(),targetParent=QFileInfo(target).path();
                    if(sourceParent==source || targetParent==target)break;
                    source=sourceParent;target=targetParent;
                    const auto found=directories.constFind(directoryKey(source,target));
                    if(found==directories.cend())continue;
                    const auto candidate=std::lower_bound(found->cbegin(),found->cend(),i);
                    folder=rows[candidate==found->cbegin()?found->first():*std::prev(candidate)];break;
                }
                folders[i]=folder;
            }
            QList<QTreeWidgetItem *> roots;
            QHash<QTreeWidgetItem *,QList<QTreeWidgetItem *>> children;
            for(int i=firstNew;i<rows.size();++i) {
                auto *folder=folders[i];
                if(!folder)roots.append(rows[i]);
                else if(!folder->treeWidget())folder->addChild(rows[i]);
                else children[folder].append(rows[i]);
            }
            tree->addTopLevelItems(roots);
            for(auto it=children.cbegin();it!=children.cend();++it)it.key()->addChildren(it.value());
            for(int i=0;i<firstNew;++i) {
                auto *folder=folders[i];
                // Une réanalyse peut inverser l’ancienne parenté : éviter un cycle temporaire.
                for(auto *ancestor=folder;ancestor;ancestor=ancestor->parent())if(ancestor==rows[i]){RowTree::attach(folder,nullptr,tree);break;}
                RowTree::attach(rows[i],folder,tree);
            }
            // Qt ne mémorise le développement qu’après insertion dans son modèle.
            if(entries.isEmpty())RowTree::attach(parent,nullptr,tree);
            else if(parent->treeWidget())tree->takeTopLevelItem(tree->indexOfTopLevelItem(parent));
            const bool dark=palette().color(QPalette::Base).lightness()<128;
            // Palette fixe de 24 teintes. Le pas de 7 espace les couleurs des lots
            // successifs et parcourt toute la palette avant de réutiliser une couleur.
            const int colorIndex=int((quint64(id)%24)*7%24);
            const QColor accent=QColor::fromHsl(colorIndex*15,170,dark?160:105);
            const QColor base=palette().color(QPalette::Base);const double alpha=dark?0.24:0.14;
            const QColor tint=QColor::fromRgbF(base.redF()*(1-alpha)+accent.redF()*alpha,base.greenF()*(1-alpha)+accent.greenF()*alpha,base.blueF()*(1-alpha)+accent.blueF()*alpha);
            // Le mode est visible par l’icône Copier/Couper ; l’aide garde le chemin et les options utiles.
            QStringList sourceOptions;
            if(job["verify_md5"].toBool())sourceOptions.append(QStringLiteral("MD5"));
            operationIcon(parent,job["mode"].toString());
            parent->setData(0,RowTree::AccentRole,accent);for(int c=0;c<TableView::ColumnCount;++c)parent->setBackground(c,tint);
            if(placeholderSelected && !rows.isEmpty())for(auto *row:rows)if(!row->parent())row->setSelected(true);
            const auto sources = job["sources"].toArray();
            QStringList names; for (const auto &s : sources) names << QFileInfo(s.toString()).fileName();
            parent->setData(0,HistorySearch::CreatedRole,job["created_at"].toInteger());
            orderText(parent,id,-1);
            text(parent, 0, names.join(", "));
            text(parent, 1, destinationLabel(job,job["destination"].toString())); text(parent, 2, job["start_required"].toBool() && status==Protocol::State::Waiting?GAD_TR("Ready to start"):label(status, paused || job["paused"].toBool(),!jobChanged));
            parent->setData(0,Qt::UserRole+3,status);
            parent->setData(0,RowTree::ErrorRetryRole,finished(status) && status!=Protocol::State::Undone && job["journaled"].toBool() && !job["undo_requested"].toBool() && (status!=Protocol::State::UndoError || job["retry_entries"].isArray()));
            QStringList searchPaths;for(const auto &source:job["sources"].toArray())searchPaths.append(source.toString());for(const auto &source:job["source_urls"].toArray())searchPaths.append(source.toString());searchPaths.append(job["destination"].toString());searchPaths.append(job["destination_url"].toString());
            searchData(parent,job,searchPaths.join("\n"));
            RowPaths::store(parent,RowPaths::group(job));
            parent->setData(0,RowTree::CanQueueRole,(status==Protocol::State::Waiting || status==Protocol::State::Scanning) && !job["cancel"].toBool() && !job["undo_requested"].toBool());
            parent->setData(0,Qt::UserRole+6,job["recovery"].toBool() && job["resume_pending"].toBool() && finished(status) && !job["undo_requested"].toBool());
            QString jobDetail=I18n::message(job["error"].toString());
            const auto exclusions=job["exclusions"].toObject();
            const bool selectionRestricted=!exclusions.value("include_hidden").toBool(true) || !exclusions["directories"].toArray().isEmpty() || !exclusions["files"].toArray().isEmpty();
            if(jobDetail.isEmpty() && entries.isEmpty() && status==Protocol::State::Done && selectionRestricted)jobDetail=GAD_TR("No items to transfer after exclusions.");
            if(jobDetail.isEmpty() && job["undo_requested"].toBool() && status!=Protocol::State::Undoing) jobDetail=GAD_TR("Restoration requested · waiting for active transfers to finish");
            if(jobDetail.isEmpty() && job.contains("journaled") && !job["journaled"].toBool())jobDetail=GAD_TR("Older operation · restoration unavailable");
            if(jobDetail.isEmpty() && job["fast_move"].toBool() && (status==Protocol::State::Waiting || status==Protocol::State::Running || status==Protocol::State::Done))
                jobDetail=GAD_TR("Fast move · local rename");
            if(job["fast_move"].toBool() && status==Protocol::State::Done) {
                for(const auto &value:entries)if(value.toObject()["undo"].toObject()["directory_move"].toBool()) {
                    jobDetail=GAD_TR("Fast move · folder renamed as a whole");break;
                }
            }
            if(jobDetail.isEmpty() && !job["destination_url"].toString().isEmpty())
                jobDetail=GAD_TR("Remote destination · permissions and dates set by the service · automatic resume unavailable");
            const auto spaceWarning=I18n::message(job["space_warning"].toString());
            if(!spaceWarning.isEmpty()) {
                if(!jobDetail.isEmpty())jobDetail+=" · ";
                jobDetail+=spaceWarning;
            }
            // Un lot se replie seulement à la transition vers OK ; une réouverture manuelle reste respectée.
            parent->setData(0, Qt::UserRole+2, status);
            const QBrush parentColor=status==Protocol::State::Done ? QBrush(QColor("#24834e")) : (status==Protocol::State::Error || status==Protocol::State::UndoError) ? QBrush(QColor("#d64545")) : QBrush();
            if (parent->foreground(2)!=parentColor) parent->setForeground(2,parentColor);
            double jobSize = 0, jobCopied = 0; int propertyWarnings=0, propertyChecked=0;
            QVector<int> branchWarnings(entries.size(),0);
            QVector<bool> branchChanges(entries.size(),false);
            // Un dossier représente aussi les restaurations réellement effectuées dans ses enfants.
            for(int i=0;i<entries.size();++i)if(hasTransferChanges(entries[i].toObject())) {
                branchChanges[i]=true;for(auto *folder=rows[i]->parent();folder;folder=folder->parent())branchChanges[RowTree::index(folder)]=true;
            }
            // Après suppression des en-têtes, chaque dossier ne résume que les avertissements de sa propre branche.
            for(int i=0;i<entries.size();++i) {
                const auto report=entries[i].toObject()["properties"].toObject();
                if(report.isEmpty() || report["status"]=="ok")continue;
                ++branchWarnings[i];for(auto *folder=rows[i]->parent();folder;folder=folder->parent())++branchWarnings[RowTree::index(folder)];
            }
            int index = 0;
            for (const auto &entryValue : entries) {
                const auto e = entryValue.toObject(); const auto s = e["state"].toString();
                const auto kind = e["kind"].toString();
                const auto size = e["size"].toDouble(), copied = e["copied"].toDouble();
                if (kind != "directory") {
                    jobSize += size; jobCopied += copied;
                }
                const bool match = !e["hidden"].toBool();
                auto *item=rows[index];
                if(item->data(0,RowTree::KindRole).toString()!=kind) {
                    item->setData(0,RowTree::KindRole,kind);
                }
                operationIcon(item,job["mode"].toString());
                const auto sourceText=sourceLabel(job,e["source"].toString());
                const auto name=QFileInfo(sourcePaths[index]).fileName();
                auto nameFont=item->font(0);if(nameFont.bold()!=(kind=="directory")){nameFont.setBold(kind=="directory");item->setFont(0,nameFont);}
                text(item,0,name,Qt::convertFromPlainText((QStringList{sourceText}+sourceOptions).join("\n")));
                item->setData(0,RowTree::AccentRole,accent);
                item->setData(0,RowTree::CanQueueRole,s==Protocol::State::Waiting && !finished(status) && status!=Protocol::State::Undoing && status!=Protocol::State::UndoWaiting && !e["cancel"].toBool() && !e["hidden"].toBool() && !job["cancel"].toBool() && !job["undo_requested"].toBool());
                auto flags=item->flags()|Qt::ItemIsDropEnabled;
                if(item->data(0,RowTree::CanQueueRole).toBool())flags|=Qt::ItemIsDragEnabled;else flags&=~Qt::ItemIsDragEnabled;
                if(item->flags()!=flags)item->setFlags(flags);
                for(int c=0;c<TableView::ColumnCount;++c)if(item->background(c)!=QBrush(tint))item->setBackground(c,tint);
                text(item,1,destinationLabel(job,e["destination"].toString()));
                const auto displayedState=status==Protocol::State::DeviceWaiting && !finished(s) && s!=Protocol::State::Skipped?Protocol::State::DeviceWaiting:s;
                text(item,2,job["start_required"].toBool() && displayedState==Protocol::State::Waiting?GAD_TR("Ready to start"):label(displayedState,paused || job["paused"].toBool() || e["paused"].toBool(),!branchChanges[index]));
                completed[index]=kind=="directory" && s==Protocol::State::Done && item->data(0,Qt::UserRole+2).toString()!=Protocol::State::Done;
                item->setData(0,Qt::UserRole+2,s);
                if(e["resume_checking"].toBool() && !(paused || job["paused"].toBool() || e["paused"].toBool()))text(item,2,GAD_TR("Checking resume data…"));
                item->setData(0,Qt::UserRole+3,s);
                const bool retryFailure=status==Protocol::State::UndoError && job["retry_entries"].toArray().contains(index) && !I18n::message(e["error"].toString()).isEmpty();
                item->setData(0,RowTree::ErrorRetryEntryRole,!e["cancel"].toBool() && !e["hidden"].toBool() && ((s==Protocol::State::Error && !(kind=="directory" && I18n::message(e["error"].toString()).isEmpty())) || retryFailure));
                RowPaths::store(item,RowPaths::entry(e));
                text(item, 3, s == Protocol::State::Verifying ? GAD_TR("MD5 ") + bytes(e["verified"].toDouble()) + " / " + bytes(size * 2)
                     : kind == "directory" ? GAD_TR("Folder") : bytes(copied) + " / " + bytes(size));
                if(e["resume_checking"].toBool())text(item,3,GAD_TR("Resume ")+bytes(e["resume_checked"].toDouble())+" / "+bytes(e["undo"].toObject()["resume"].toObject()["bytes"].toDouble()));
                QString detail = I18n::message(e["error"].toString());
                if (detail.isEmpty() && !e["md5_source"].toString().isEmpty() && e["md5_source"] == e["md5_destination"])
                    detail = GAD_TR("MD5 OK · ") + e["md5_source"].toString();
                const auto fastCheck=e["fast_move_check"].toObject();
                if(detail.isEmpty() && !finished(s) && fastCheck["status"]=="checking")detail=GAD_TR("Verifying fast move · metadata and paths");
                if(detail.isEmpty() && s==Protocol::State::Done && fastCheck["status"]=="ok")detail=GAD_TR("Fast move verified · identity and metadata");
                if(detail.isEmpty() && fastCheck["status"]=="error")detail=I18n::message("Fast-move verification failed; moved result kept.");
                if(!fastCheck["path"].toString().isEmpty())detail+="\n"+GAD_TR("Checked path:")+" "+fastCheck["path"].toString();
                if(detail.isEmpty() && s==Protocol::State::Done && e["undo"].toObject()["directory_move"].toBool())
                    detail=GAD_TR("Folder moved by renaming");
                if(detail.isEmpty() && s==Protocol::State::Done && e["undo"].toObject()["rename_move"].toBool())
                    detail=GAD_TR("Fast move · no data copied");
                if (detail.isEmpty() && s == Protocol::State::Verifying) detail = GAD_TR("Reading and comparing source and destination contents");
                if(detail.isEmpty() && e["resume_checking"].toBool())detail=GAD_TR("Checking the source and previously written bytes before resuming");
                if(e["resumed_from"].toDouble()>0 && I18n::message(e["error"].toString()).isEmpty() && !e["resume_checking"].toBool()) {
                    if(!detail.isEmpty())detail+=" · ";
                    detail+=GAD_TR("Resumed from ")+bytes(e["resumed_from"].toDouble());
                }
                QString propertyTooltip;
                const auto report=e["properties"].toObject();
                const bool propertyWarning=!report.isEmpty() && report["status"]!="ok";
                if(!report.isEmpty()) {
                    ++propertyChecked;if(propertyWarning)++propertyWarnings;
                    const auto propertyText=report["status"]=="ok"?QString(GAD_TR("Visible properties match")):report["status"]=="different"?QString(GAD_TR("Properties differ")):QString(GAD_TR("Property check incomplete"));
                    if(!detail.isEmpty())detail+=" · ";
                    detail+=propertyText;
                    QStringList lines{detail};
                    for(const auto &field:{"differences","unavailable"})for(const auto &value:report[field].toArray())lines.append(I18n::message(value.toString()));
                    propertyTooltip=Qt::convertFromPlainText(lines.join('\n'));
                }
                if(!spaceWarning.isEmpty() || (!I18n::message(job["error"].toString()).isEmpty() && detail!=I18n::message(job["error"].toString()))) {
                    const auto extra=spaceWarning.isEmpty()?I18n::message(job["error"].toString()):spaceWarning;
                    if(!detail.isEmpty())detail+=" · ";
                    detail+=extra;
                    propertyTooltip=propertyTooltip.isEmpty()?Qt::convertFromPlainText(detail):propertyTooltip+"<br/>"+Qt::convertFromPlainText(extra);
                }
                const bool branchWarning=kind=="directory" && branchWarnings[index]>0;
                if(branchWarning){
                    const auto extra=QString(GAD_TR("Properties: %1 warning(s)")).arg(branchWarnings[index]);
                    if(!detail.isEmpty())detail+=" · ";
                    detail+=extra;propertyTooltip=propertyTooltip.isEmpty()?Qt::convertFromPlainText(detail):propertyTooltip+"<br/>"+Qt::convertFromPlainText(extra);
                }
                if(propertyTooltip.isNull() && !fastCheck["path"].toString().isEmpty())propertyTooltip=Qt::convertFromPlainText(detail);
                // La limite de la demande est visible sur chaque ligne concernée,
                // sans remplacer les erreurs, vérifications ou avertissements existants.
                if(!rateDetail.isEmpty()) {
                    if(!detail.isEmpty()){detail+=" · ";}detail+=rateDetail;
                    if(!propertyTooltip.isNull())propertyTooltip+="<br/>"+rateDetail.toHtmlEscaped();
                }
                text(item, 4, detail,propertyTooltip);
                if(item->data(4,Qt::UserRole).toBool()!=(propertyWarning || branchWarning || !spaceWarning.isEmpty())) {
                    item->setIcon(4,(propertyWarning || branchWarning || !spaceWarning.isEmpty())?QIcon::fromTheme("dialog-warning",style()->standardIcon(QStyle::SP_MessageBoxWarning)):QIcon());item->setData(4,Qt::UserRole,propertyWarning || branchWarning || !spaceWarning.isEmpty());
                }
                // Une icône et un état en gras rendent le blocage repérable sans modifier la sélection.
                if(item->data(2,Qt::UserRole).toString()!=s) {
                    item->setIcon(2,s==Protocol::State::Conflict?QIcon::fromTheme("dialog-warning",style()->standardIcon(QStyle::SP_MessageBoxWarning)):QIcon());
                    auto stateFont=item->font(2);stateFont.setBold(s==Protocol::State::Conflict);item->setFont(2,stateFont);item->setData(2,Qt::UserRole,s);
                }
                const QBrush color = s == Protocol::State::Error ? QBrush(QColor("#d64545")) : s == Protocol::State::Done ? QBrush(QColor("#24834e")) : QBrush();
                if (item->foreground(2) != color) item->setForeground(2, color);
                if (e["hidden"].toBool() && item->isSelected()) item->setSelected(false);
                const double fraction=size>0?(s==Protocol::State::Verifying?e["verified"].toDouble()/(size*2):copied/size):(s==Protocol::State::Done?1:0);
                item->setData(3,TableView::ProgressRole,kind=="directory"?QVariant():QVariant(fraction));
                item->setData(0,HistorySearch::CreatedRole,job["created_at"].toInteger());
                orderText(item,id,index);
                searchData(item,job,sourceLabel(job,e["source"].toString())+"\n"+destinationLabel(job,e["destination"].toString())+"\n"+e["source"].toString()+"\n"+e["destination"].toString());
                shown[index]=match && view->matches(item);
                ++index;
            }
            // Les fichiers n’ont rien à développer. Ouvrir les nouveaux dossiers une fois
            // leurs cellules remplies évite des milliers de calculs de disposition visibles.
            for(int i=firstNew;i<rows.size();++i)if(entries[i].toObject()["kind"]=="directory")rows[i]->setExpanded(true);
            // Remonter la visibilité jusqu’au lot, même si une reprise a ajouté un enfant après d’autres racines.
            for(int i=0;i<rows.size();++i)if(shown[i]) {
                for(auto *folder=rows[i]->parent();folder;folder=folder->parent())shown[folder->data(0,RowTree::EntryIndexRole).toInt()]=true;
            }
            for(int i=0;i<rows.size();++i) {
                if(rows[i]->isHidden()==shown[i])rows[i]->setHidden(!shown[i]);
                if(!shown[i] && rows[i]->isSelected())rows[i]->setSelected(false);
                if(completed[i])RowTree::collapseCompleted(rows[i]);
            }

            // Un filtre ouvre ses branches pertinentes sans rouvrir les dossiers à chaque progression.
            if(filterChanged && view->presentation().filtered()) {
                for(int i=0;i<rows.size();++i)if(shown[i])for(auto *folder=rows[i]->parent();folder;folder=folder->parent())folder->setExpanded(true);
            }
            if(propertyChecked) {
                const auto propertyText=propertyWarnings?QString(GAD_TR("Properties: %1 warning(s)")).arg(propertyWarnings):QString(GAD_TR("Properties verified: %1")).arg(propertyChecked);
                if(!jobDetail.isEmpty())jobDetail+=" · ";
                jobDetail+=propertyText;
            }
            if(!rateDetail.isEmpty()){if(!jobDetail.isEmpty())jobDetail+=" · ";jobDetail+=rateDetail;}
            text(parent,4,jobDetail);
            const bool groupWarning=propertyWarnings>0 || !spaceWarning.isEmpty();
            if(parent->data(4,Qt::UserRole).toBool()!=groupWarning) {
                parent->setIcon(4,groupWarning?QIcon::fromTheme("dialog-warning",style()->standardIcon(QStyle::SP_MessageBoxWarning)):QIcon());parent->setData(4,Qt::UserRole,groupWarning);
            }
            text(parent, 3, bytes(jobCopied) + " / " + bytes(jobSize));
            parent->setData(3,TableView::ProgressRole,jobSize>0?jobCopied/jobSize:(status==Protocol::State::Done?1:0));
            const bool allRemoved=!entries.isEmpty() && std::all_of(entries.begin(),entries.end(),[](const QJsonValue &e){return e.toObject()["hidden"].toBool();});
            const bool hidden = (entries.isEmpty() && !view->matches(parent)) || (allRemoved && I18n::message(job["error"].toString()).isEmpty());
            if (parent->isHidden() != hidden) parent->setHidden(hidden);
            if(hidden && parent->isSelected())parent->setSelected(false);
        } else if(orderChanged) {
            // Seul le rang change : conserver le rendu, les infobulles et les mesures du lot.
            orderText(jobRows.value(id),id,-1);
            const auto &rows=entryRows[id];
            for(int i=0;i<rows.size();++i)orderText(rows[i],id,i);
        }
    }
    // Garder le rang métier pour les égalités, sans défaire puis refaire le tri à chaque progression.
    qint64 rank=0;
    for(const auto &value:queueOrder) {
        const auto key=value.toObject();auto *row=key["index"].isNull()?jobRows.value(key["job"].toInteger()):entryRow(key["job"].toInteger(),key["index"].toInt());
        if(row && row->data(0,TableView::QueueRankRole).toLongLong()!=rank)row->setData(0,TableView::QueueRankRole,rank);
        ++rank;
    }
    if(view->presentation().sorts.isEmpty())syncQueueOrder(queueOrder);
    view->applyPresentation();
    // Les actions d’un menu déjà ouvert suivent aussi les transitions sans nouvelle sélection.
    updateRowActions();
    updateConflictPanel(conflicts);
    conflictBox->setVisible(conflictJob != 0);
}

/// Le préordre métier ne change que la place de frères : une ligne garde toujours son dossier réel.
void Window::syncQueueOrder(const QJsonArray &order) {
    const int scroll=tree->verticalScrollBar()->value();QSignalBlocker blocker(tree);
    QHash<QTreeWidgetItem *,int> positions;
    for(const auto &value:order) {
        const auto key=value.toObject();const auto id=key["job"].toInteger();
        auto *row=key["index"].isNull()?jobRows.value(id):entryRow(id,key["index"].toInt(-1));
        if(!row || !row->treeWidget())continue;
        auto *parent=row->parent();const int wanted=positions[parent]++;
        if((parent?parent->child(wanted):tree->topLevelItem(wanted))!=row)RowTree::attach(row,parent,tree,wanted);
    }
    tree->verticalScrollBar()->setValue(scroll);
}
