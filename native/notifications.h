#pragma once
#include "i18n.h"
#include "protocol.h"
// Publication d’un suivi agrégé dans les transferts natifs de Plasma.
// Une session contient les lots en cours, exclut l’ancien historique et conserve
// une seule vue KDE. Cette couche mesure la progression mais ne copie aucun fichier.

#include <QtCore>
#include <QtDBus>
#include <functional>

// One native KDE job for the current queue. No file operation is performed here.
/// Contrôleur du suivi natif ; son parent Qt possède sa durée de vie.
class TransferNotifications : public QObject {
    Q_OBJECT
    // Identifiants de la session actuelle, conservés jusqu’à sa clôture native.
    QSet<qint64> tracked;
    // Chronomètre monotone entre deux mesures, indépendant de l’horloge civile.
    QElapsedTimer speedClock;
    // Compteurs précédents par couple lot/indice pour les deltas de débit.
    QHash<QString, qulonglong> previousCopied;
    // Dernière mesure positive, conservée pour les détails du transfert terminé.
    qulonglong lastSpeed = 0;
    // Chemin D-Bus obtenu de KDE ; vide tant que la demande asynchrone est en cours.
    QString viewPath;
    // Une indisponibilité de Plasma ne doit ni perdre le suivi ni provoquer des appels à chaque rafraîchissement.
    QElapsedTimer viewRetryClock;
    quint64 viewGeneration = 0;
    // Dernier état complet à renvoyer aussi si KDE demande une actualisation.
    QVariantMap latest;
    // État de la session et de la demande distante ; enabled désactive les publications dans certains tests.
    bool enabled, requesting = false, complete = false, announced = false;
    // Code natif : zéro en réussite, un pour l’annulation, autre valeur pour l’erreur.
    uint finishCode = 0;
    // Bilan de terminaison fourni à KDE, avec un lien dans tous les états.
    QString finishText;
    // Relais des commandes vers la fenêtre, qui vérifie l’état frais avant d’appeler Rust.
    std::function<void(const QString &)> control;
    /// Délimite les lots sans copie restante pour fermer le suivi une fois la session achevée.
    static bool terminal(const QString &s) {
        return Protocol::terminal(s);
    }
    /// Point unique pour le lien, présent dans chaque message publié pendant et après le transfert.
    static QString withGadLink(const QString &text) {
        return text+GAD_TR(" · <a href=\"gad://show\">Show Gad</a>");
    }
    /// Construit les appels D-Bus avec des arguments typés, sans exécution bloquante de commande externe.
    static QDBusMessage method(const QString &service,const QString &path,const QString &interface,const QString &name,const QVariantList &args) {
        auto m=QDBusMessage::createMethodCall(service,path,interface,name);m.setArguments(args);return m;
    }
    /// Envoie une mise à jour asynchrone uniquement après obtention du chemin de la vue KDE.
    void sendView(const QString &name,const QVariantList &args) {
        if(viewPath.isEmpty())return;
        QDBusConnection::sessionBus().asyncCall(method("org.kde.JobViewServer",viewPath,"org.kde.JobViewV3",name,args));
    }
    /// Déconnecte les commandes de cette vue, publie ses derniers compteurs et annonce sa fin.
    void terminateView() {
        auto bus=QDBusConnection::sessionBus();
        bus.disconnect("org.kde.JobViewServer",viewPath,"org.kde.JobViewV3",QString(),this,nullptr);
        sendView("update",{latest});
        sendView("terminate",{finishCode,finishText,QVariantMap{}});
        viewPath.clear();
    }
    /// Demande une unique vue KDE et branche ses commandes après réception asynchrone de son chemin.
    /// Une copie peut finir avant cette réponse : elle est alors terminée aussitôt la vue obtenue.
    void requestView() {
        requesting=true;viewRetryClock.start();
        const auto generation=viewGeneration;
        // La vue native affiche immédiatement les compteurs déjà connus, même si l’analyse continue.
        auto hints=latest;hints["immediate"]=true;
        auto *watcher=new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(method("org.kde.JobViewServer","/JobViewServer","org.kde.JobViewServerV2","requestView",{"org.hizoka.gad",3,hints})),this);
        connect(watcher,&QDBusPendingCallWatcher::finished,this,[this,watcher,generation] {
            QDBusPendingReply<QDBusObjectPath> reply=*watcher;watcher->deleteLater();
            if(generation!=viewGeneration)return;
            requesting=false;
            if(reply.isError())return;
            viewPath=reply.value().path();
            // Les commandes ne sont branchées que sur l’objet de cette vue, fourni par le service KDE.
            auto bus=QDBusConnection::sessionBus();
            bus.connect("org.kde.JobViewServer",viewPath,"org.kde.JobViewV3","suspendRequested",this,SLOT(suspendRequested()));
            bus.connect("org.kde.JobViewServer",viewPath,"org.kde.JobViewV3","resumeRequested",this,SLOT(resumeRequested()));
            bus.connect("org.kde.JobViewServer",viewPath,"org.kde.JobViewV3","cancelRequested",this,SLOT(cancelRequested()));
            bus.connect("org.kde.JobViewServer",viewPath,"org.kde.JobViewV3","updateRequested",this,SLOT(updateRequested()));
            sendView("update",{latest});
            if(complete){terminateView();tracked.clear();announced=false;}
        });
    }
public:
    /// Reçoit le relais des commandes vers le moteur ; active=false permet les tests d’interface sans bureau réel.
    TransferNotifications(std::function<void(const QString &)> action,QObject *parent=nullptr,bool active=true)
        :QObject(parent),enabled(active),control(std::move(action)) {
        // Un redémarrage du service invalide ses anciens objets, sans abandonner les lots suivis.
        if(enabled) {
            auto *watcher=new QDBusServiceWatcher("org.kde.JobViewServer",QDBusConnection::sessionBus(),QDBusServiceWatcher::WatchForOwnerChange,this);
            connect(watcher,&QDBusServiceWatcher::serviceOwnerChanged,this,[this](const QString &,const QString &oldOwner,const QString &) {
                if(oldOwner.isEmpty())return; // L’activation initiale ne doit pas invalider la demande en cours.
                ++viewGeneration;
                if(!viewPath.isEmpty())QDBusConnection::sessionBus().disconnect("org.kde.JobViewServer",viewPath,"org.kde.JobViewV3",QString(),this,nullptr);
                viewPath.clear();requesting=false;viewRetryClock.invalidate();
            });
        }
    }
    /// Ferme une vue encore publiée pour ne pas laisser de transfert natif abandonné.
    ~TransferNotifications() override {
        if(!viewPath.isEmpty())sendView("terminate",{uint(1),withGadLink(GAD_TR("Transfer tracking closed.")),QVariantMap{}});
    }
    /// Retient aussi un lot très court qui pourrait déjà être terminé au premier rafraîchissement.
    void begin(qint64 id) { if(id>0)tracked.insert(id); }
    /// Indique si la session contient encore des lots suivis.
    bool isTracking() const {return !tracked.isEmpty();}
    /// Fournit l’instantané des propriétés publiées, utile pour vérifier les compteurs dans les tests.
    QVariantMap properties() const {return latest;}
    /// Retrouve seulement les lots encore actifs du suivi dans un état récent du moteur.
    QJsonArray activeIds(const QJsonObject &state) const {
        QJsonArray ids;
        for(const auto &v:state["jobs"].toArray()){const auto j=v.toObject();if(tracked.contains(j["id"].toInteger())&&!terminal(j["state"].toString()))ids.append(j["id"]);}
        return ids;
    }
    /// Agrège les lots, compte les fichiers et publie progression, pause et débit.
    /// Les lots ajoutés rejoignent la session existante ; aucun second message classique n’est créé.
    void sync(const QJsonObject &state,bool hidden) {
        const bool starting=!announced;
        // Include jobs added to an existing session, even if Gad has since opened.
        if(hidden || !tracked.isEmpty())for(const auto &v:state["jobs"].toArray()) {
            const auto j=v.toObject();if(!terminal(j["state"].toString()))tracked.insert(j["id"].toInteger());
        }
        if(tracked.isEmpty())return;
        // Les octets de copie et le travail MD5 sont comptés séparément : leurs unités n’ont pas le même rôle.
        qulonglong files=0,doneFiles=0,total=0,copied=0,dirs=0,doneDirs=0;
        long double work=0,processed=0;
        qulonglong byteDelta=0;
        QHash<QString,qulonglong> currentCopied;
        int batches=0,doneBatches=0,activeBatches=0,waiting=0,errors=0,cancelled=0;
        bool deviceWaiting=false;
        bool suspended=true,scanning=false,verifying=false,conflict=false,restoring=false;
        for(const auto &v:state["jobs"].toArray()) {
            const auto job=v.toObject();if(!tracked.contains(job["id"].toInteger()))continue;
            ++batches;const auto status=job["state"].toString();const bool stopped=terminal(status);
            if(stopped)++doneBatches;else {++activeBatches;if(status==Protocol::State::Waiting||status==Protocol::State::Scanning)++waiting;}
            if(status==Protocol::State::Error||status==Protocol::State::UndoError)++errors;
            if(status==Protocol::State::Cancelled)++cancelled;
            scanning |= status==Protocol::State::Scanning;
            deviceWaiting |= status==Protocol::State::DeviceWaiting;
            restoring |= status==Protocol::State::Undoing||status==Protocol::State::UndoWaiting;
            bool filePause=false,hasPending=false,allPendingPaused=true;
            int entryIndex=0;
            for(const auto &entryValue:job["entries"].toArray()) {
                const auto key=QString::number(job["id"].toInteger())+"/"+QString::number(entryIndex++);
                const auto e=entryValue.toObject();const auto s=e["state"].toString();
                const bool finished=s==Protocol::State::Done||s==Protocol::State::Skipped||s==Protocol::State::Undone;
                const bool activeEntry=s==Protocol::State::Running||s==Protocol::State::Verifying||s==Protocol::State::Conflict;
                if(!e["cancel"].toBool() && !e["hidden"].toBool()) {
                    // Une pause avant démarrage laisse désormais la ligne en attente et libère le travailleur.
                    if(s==Protocol::State::Waiting || (e["kind"]!="directory" && activeEntry)){hasPending=true;allPendingPaused &= e["paused"].toBool();}
                    filePause |= activeEntry && e["paused"].toBool();
                }
                if(e["kind"]=="directory"){++dirs;if(finished)++doneDirs;continue;}
                ++files;if(finished)++doneFiles;
                const auto size=qulonglong(e["size"].toDouble());
                // La différence par entrée évite une pointe artificielle quand un compteur est remis à zéro.
                const auto transferred=qMin(size,qulonglong(e["copied"].toDouble()));
                // Un renommage avance les octets logiques, sans représenter un débit de lecture/écriture.
                const auto measured=e["undo"].toObject()["rename_move"].toBool()?qulonglong(0):transferred;
                currentCopied.insert(key,measured);
                const auto previous=previousCopied.value(key,0);
                if(measured>previous)byteDelta+=measured-previous;
                total+=size;copied+=transferred;
                // Une vérification relit source et cible : son travail total vaut deux tailles en plus de la copie.
                const bool md5=job["verify_md5"].toBool()&&e["kind"]=="file";
                work+=static_cast<long double>(size)*(md5?3:1);
                processed+=finished?static_cast<long double>(size)*(md5?3:1):static_cast<long double>(qMin(size,qulonglong(e["copied"].toDouble())))+(md5?e["verified"].toDouble():0);
                verifying |= s==Protocol::State::Verifying;
                conflict |= s==Protocol::State::Conflict;
            }
            if(!stopped)suspended &= status==Protocol::State::DeviceWaiting||state["paused"].toBool()||job["paused"].toBool()||filePause||(hasPending && allPendingPaused);
        }
        // Le premier échantillon fixe une base ; les octets déjà copiés ne deviennent pas un faux débit.
        const qint64 elapsed=speedClock.isValid()?speedClock.elapsed():0;
        const qulonglong speed=!starting && elapsed>0 && activeBatches>0 && !suspended
            ? qulonglong(static_cast<long double>(byteDelta)*1000/elapsed):0;
        // Conserver la dernière mesure positive permet d’afficher le débit dans les détails après la fin.
        if(starting)lastSpeed=0;
        if(speed>0)lastSpeed=speed;
        previousCopied=std::move(currentCopied);
        speedClock.start();
        const int percent=work>0?qBound(0,int(processed*100/work),activeBatches>0?99:100):(activeBatches==0?100:0);
        QString info=QString(GAD_TR("%1/%2 batch(es) completed · %3/%4 file(s)")).arg(doneBatches).arg(batches).arg(doneFiles).arg(files);
        if(deviceWaiting)info+=GAD_TR(" · Waiting for device");
        if(scanning)info+=GAD_TR(" · Scanning…");
        if(waiting)info+=QString(GAD_TR(" · %1 batch(es) queued")).arg(waiting);
        if(verifying)info+=GAD_TR(" · MD5 verification");
        if(conflict)info+=GAD_TR(" · Conflict: open Gad");
        if(restoring)info+=GAD_TR(" · Restoring");
        // Plasma ouvre ce lien avec le lanceur Gad, qui relève l’instance existante.
        info=withGadLink(info);
        // Les noms de propriétés correspondent au contrat org.kde.JobViewV3 attendu par Plasma.
        latest={{"title",GAD_TR("Gad transfers")},{"infoMessage",info},{"descriptionLabel1",GAD_TR("Batches")},{"descriptionValue1",QString(GAD_TR("%1 / %2 completed")).arg(doneBatches).arg(batches)},{"descriptionLabel2",GAD_TR("Files")},{"descriptionValue2",QString(GAD_TR("%1 / %2 processed")).arg(doneFiles).arg(files)},{"totalBytes",QVariant::fromValue(total)},{"processedBytes",QVariant::fromValue(copied)},{"totalFiles",QVariant::fromValue(files)},{"processedFiles",QVariant::fromValue(doneFiles)},{"totalDirectories",QVariant::fromValue(dirs)},{"processedDirectories",QVariant::fromValue(doneDirs)},{"speed",QVariant::fromValue(activeBatches>0?speed:lastSpeed)},{"percent",uint(percent)},{"suspended",activeBatches>0&&suspended}};
        complete=activeBatches==0;
        finishCode=errors?uint(100):(cancelled?uint(1):uint(0));
        // Le texte de fin remplace infoMessage dans Plasma : il doit donc porter aussi le lien.
        const auto result=errors?QString(GAD_TR("%1 batch(es) failed.")).arg(errors)
            :cancelled?QString(GAD_TR("%1 batch(es) canceled.")).arg(cancelled)
            :QString(GAD_TR("%1 batch(es) completed.")).arg(doneBatches);
        finishText=withGadLink(result);
        if(starting)announced=true;
        if(enabled) {
            // Réessayer au plus toutes les deux secondes après un échec, ou immédiatement après un redémarrage.
            if(viewPath.isEmpty() && !requesting && (starting || (!complete && (!viewRetryClock.isValid() || viewRetryClock.elapsed()>=2000))))requestView();
            else if(!viewPath.isEmpty())sendView("update",{latest});
            if(complete && !viewPath.isEmpty())terminateView();
        }
        // Keep the session until an outstanding request has received its path.
        if(complete&&!requesting){tracked.clear();announced=false;}
    }
private Q_SLOTS:
    /// Relaie une pause uniquement pour une session native encore active.
    void suspendRequested(){if(!viewPath.isEmpty()&&!complete)control("pause");}
    /// Relaie la reprise des lots concernés par la session.
    void resumeRequested(){if(!viewPath.isEmpty()&&!complete)control("resume");}
    /// Relaie l’annulation des lots du suivi sans effectuer elle-même de suppression de fichier.
    void cancelRequested(){if(!viewPath.isEmpty()&&!complete)control("cancel");}
    /// Renvoie les dernières propriétés si KDE en demande une synchronisation.
    void updateRequested(){sendView("update",{latest});}
};
