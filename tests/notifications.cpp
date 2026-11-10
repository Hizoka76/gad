// Tests du protocole KDE sur un bus D-Bus privé. Les faux services enregistrent
// les propriétés et commandes sans publier de notification sur le bureau réel.

#include "../native/notifications.h"
#include <QtTest>
#include <cstdio>

class NotificationServer : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface","org.freedesktop.Notifications")
public:
    int calls=0;QStringList actions;QString body;uint lastId=0;
public Q_SLOTS:
    /// Compte toute notification classique : le test exige qu’aucune ne double la vue de progression.
    uint Notify(const QString &,uint,const QString &,const QString &,const QString &text,const QStringList &offered,const QVariantMap &,int) {
        ++calls;actions=offered;body=text;return lastId=uint(100+calls);
    }
Q_SIGNALS:
    void ActionInvoked(uint id,const QString &action);
    void ActivationToken(uint id,const QString &token);
};
class JobView : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface","org.kde.JobViewV3")
public:
    QVariantMap properties;bool terminated=false;uint errorCode=0;QString errorText;int updates=0;
    /// Attache la vue simulée au service qui en possède la durée de vie.
    explicit JobView(QObject *parent):QObject(parent){}
public Q_SLOTS:
    /// Mémorise les propriétés reçues pour vérifier les quantités, le débit et le pourcentage.
    void update(const QVariantMap &values){properties=values;++updates;}
    /// Mémorise la terminaison et son code pour distinguer réussite, erreur et annulation.
    void terminate(uint code,const QString &text,const QVariantMap &){terminated=true;errorCode=code;errorText=text;}
Q_SIGNALS:
    void suspendRequested();void resumeRequested();void cancelRequested();void updateRequested();
};
class JobServer : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface","org.kde.JobViewServerV2")
public:
    QList<JobView *> views;QString desktop;int capabilities=0, requests=0, failures=0;
public Q_SLOTS:
    /// Crée un objet de suivi indépendant, exporté sur le bus privé avec le même protocole que Plasma.
    QDBusObjectPath requestView(const QString &entry,int flags,const QVariantMap &hints){
        ++requests;
        if(failures>0){--failures;sendErrorReply("org.kde.TestUnavailable","Service temporairement indisponible");return QDBusObjectPath("/");}
        desktop=entry;capabilities=flags;
        auto *view=new JobView(this);view->properties=hints;views.append(view);
        const auto path=QString("/test/job%1").arg(views.size());
        QDBusConnection::sessionBus().registerObject(path,view,QDBusConnection::ExportAllSlots|QDBusConnection::ExportAllSignals);
        return QDBusObjectPath(path);
    }
};
/// Échoue avec un message explicite ; les attentes asynchrones utilisent la même assertion.
static void check(bool valid,const char *message){if(!valid){std::fprintf(stderr,"%s\n",message);std::abort();}}
#undef QTRY_COMPARE_WITH_TIMEOUT
#undef QTRY_VERIFY_WITH_TIMEOUT
#define QTRY_COMPARE_WITH_TIMEOUT(actual, expected, timeout) check(QTest::qWaitFor([&]{return (actual)==(expected);},std::chrono::milliseconds(timeout)), #actual " == " #expected)
#define QTRY_VERIFY_WITH_TIMEOUT(condition, timeout) check(QTest::qWaitFor([&]{return bool(condition);},std::chrono::milliseconds(timeout)), #condition)
/// Construit une entrée synthétique dont le volume et la progression sont contrôlés par le test.
static QJsonObject entry(QString state="running",int copied=25){return {{"kind","file"},{"size",100},{"copied",copied},{"state",state},{"verified",0}};}
/// Construit un lot synthétique pour les scénarios de progression et de fin.
static QJsonObject job(int id,QString state="running"){return {{"id",id},{"state",state},{"entries",QJsonArray{entry()}},{"verify_md5",false}};}
/// Vérifie l’unicité de la notification, les commandes natives, le débit et les fins de session.
int main(int argc,char **argv){
    QCoreApplication app(argc,argv);
    Q_INIT_RESOURCE(gad_assets);I18n::initialize("fr_FR");
    auto bus=QDBusConnection::sessionBus();
    check(bus.registerService("org.freedesktop.Notifications"),"Service notifications isolé");
    check(bus.registerService("org.kde.JobViewServer"),"Service KDE isolé");
    // Les services du bus privé reçoivent les mêmes appels qu’un Plasma réel, sans notification visible.
    NotificationServer notifications;JobServer jobs;
    check(bus.registerObject("/org/freedesktop/Notifications",&notifications,QDBusConnection::ExportAllSlots|QDBusConnection::ExportAllSignals),"Objet notifications");
    check(bus.registerObject("/JobViewServer",&jobs,QDBusConnection::ExportAllSlots),"Objet suivi KDE");
    QString command;
    TransferNotifications progress([&](const QString &value){command=value;});
    auto first=job(1);auto second=job(2,"waiting");second["entries"]=QJsonArray{entry("waiting",0)};
    QJsonObject state{{"jobs",QJsonArray{job(99,"done"),first,second}},{"paused",false}};
    // Une fenêtre visible ne démarre pas de suivi ; le masquage ouvre ensuite une seule session.
    progress.sync(state,false);
    QTest::qWait(50);check(jobs.views.isEmpty()&&notifications.calls==0,"Pas de popup si la fenêtre est visible");
    progress.sync(state,true);
    QTRY_COMPARE_WITH_TIMEOUT(jobs.views.size(),1,2000);
    QTest::qWait(20);check(notifications.calls==0,"Aucune notification supplémentaire en plus du suivi natif");
    auto *view=jobs.views[0];
    QTRY_VERIFY_WITH_TIMEOUT(view->properties.contains("percent"),2000);
    check(jobs.desktop=="org.hizoka.gad"&&jobs.capabilities==3,"Association Gad et commandes natives");
    check(view->properties["totalFiles"].toULongLong()==2&&view->properties["totalBytes"].toULongLong()==200,"Seule la file courante entre dans le total");
    check(view->properties["percent"].toUInt()==12&&view->properties["infoMessage"].toString().contains("0/2 lot"),"Progression et nombre de lots");
    check(view->properties["speed"].toULongLong()==0,"Le premier échantillon ne compte pas les octets déjà copiés");
    check(view->properties["infoMessage"].toString().contains("<a href=\"gad://show\">Afficher Gad</a>"),"Lien d’ouverture présent dans la progression native");
    // Le débit nécessite un intervalle réel et une hausse de compteur, puis retombe à zéro sans mouvement.
    QTest::qWait(30);
    first["entries"]=QJsonArray{entry("running",50)};state["jobs"]=QJsonArray{first,second};progress.sync(state,false);
    QTRY_VERIFY_WITH_TIMEOUT(view->properties["speed"].toULongLong()>0,2000);
    check(jobs.views.size()==1,"La progression réutilise le même suivi KDE");
    QTest::qWait(30);progress.sync(state,false);
    QTRY_COMPARE_WITH_TIMEOUT(view->properties["speed"].toULongLong(),qulonglong(0),2000);
    // Une déconnexion suspend le suivi sans terminer le lot et annonce l’attente du périphérique.
    auto offline=first;offline["state"]="device_waiting";offline["entries"]=QJsonArray{entry("device_waiting",50)};
    state["jobs"]=QJsonArray{offline};progress.sync(state,false);
    QTRY_VERIFY_WITH_TIMEOUT(view->properties["suspended"].toBool(),2000);
    check(view->properties["infoMessage"].toString().contains("périphérique"),"Le suivi natif explique l’attente de reconnexion");
    state["jobs"]=QJsonArray{first,second};progress.sync(state,false);
    QTRY_VERIFY_WITH_TIMEOUT(!view->properties["suspended"].toBool(),2000);
    // Les signaux du serveur natif doivent remonter aux commandes du contrôleur.
    emit view->suspendRequested();QTRY_COMPARE_WITH_TIMEOUT(command,QString("pause"),2000);
    emit view->resumeRequested();QTRY_COMPARE_WITH_TIMEOUT(command,QString("resume"),2000);
    // Aller-retour de l’état moteur : le suivi KDE doit quitter son état suspendu à la reprise.
    state["paused"]=true;progress.sync(state,false);
    QTRY_VERIFY_WITH_TIMEOUT(view->properties["suspended"].toBool(),2000);
    check(view->properties["speed"].toULongLong()==0,"La pause native affiche immédiatement un débit nul");
    state["paused"]=false;progress.sync(state,false);
    QTRY_VERIFY_WITH_TIMEOUT(!view->properties["suspended"].toBool(),2000);
    check(jobs.views.size()==1,"Pause et reprise gardent la même notification KDE");
    // Une file composée seulement de lignes en attente pausées est suspendue ; une autre ligne prête peut avancer.
    auto pausedEntry=entry("waiting",0);pausedEntry["paused"]=true;
    auto pausedJob=first;pausedJob["state"]="waiting";pausedJob["entries"]=QJsonArray{pausedEntry};state["jobs"]=QJsonArray{pausedJob};progress.sync(state,false);
    QTRY_VERIFY_WITH_TIMEOUT(view->properties["suspended"].toBool(),2000);
    pausedJob["entries"]=QJsonArray{pausedEntry,entry("waiting",0)};state["jobs"]=QJsonArray{pausedJob};progress.sync(state,false);
    QTRY_VERIFY_WITH_TIMEOUT(!view->properties["suspended"].toBool(),2000);
    state["jobs"]=QJsonArray{first,second};progress.sync(state,false);
    emit view->cancelRequested();QTRY_COMPARE_WITH_TIMEOUT(command,QString("cancel"),2000);
    // Le progrès inclut la relecture MD5 ; une copie déjà écrite ne vaut pas encore 100 %.
    first["verify_md5"]=true;auto verifying=entry("verifying",100);verifying["verified"]=100;first["entries"]=QJsonArray{verifying};
    state["jobs"]=QJsonArray{first,second};state["paused"]=true;progress.sync(state,false);
    QTRY_COMPARE_WITH_TIMEOUT(view->properties["percent"].toUInt(),uint(50),2000);
    check(view->properties["suspended"].toBool()&&view->properties["infoMessage"].toString().contains("MD5"),"La pause et MD5 restent visibles sans annoncer 100 % trop tôt");
    check(notifications.calls==0,"Aucun message classique lors des mises à jour non plus");
    check(view->properties["speed"].toULongLong()==0,"La pause ne conserve pas un débit périmé");
    first["state"]="done";first["entries"]=QJsonArray{entry("done",100)};
    second["state"]="done";second["entries"]=QJsonArray{entry("done",100)};
    state["jobs"]=QJsonArray{first,second};state["paused"]=false;progress.sync(state,true);
    QTRY_VERIFY_WITH_TIMEOUT(view->terminated,2000);check(view->errorCode==0,"Fin native réussie");
    check(view->errorText.contains("2 lot(s) terminé(s).") && view->errorText.contains("gad://show"),"La réussite conserve le lien sans code d’erreur");
    check(view->properties["speed"].toULongLong()>0,"Le dernier débit mesuré reste disponible dans les détails après la fin");
    check(view->properties["percent"].toUInt()==100&&view->properties["processedFiles"].toULongLong()==2,"Le suivi termine à 100 %");
    // Une fin avant réponse asynchrone ne doit pas laisser une notification bloquée.
    auto fast=job(3,"done");fast["entries"]=QJsonArray{entry("done",100)};state["jobs"]=QJsonArray{fast};
    progress.begin(3);progress.sync(state,true);
    QTRY_COMPARE_WITH_TIMEOUT(jobs.views.size(),2,2000);
    QTRY_VERIFY_WITH_TIMEOUT(jobs.views[1]->terminated,2000);
    check(jobs.views[1]->errorText.contains("gad://show"),"Une réussite immédiate conserve le lien");
    check(!progress.isTracking(),"Une copie très courte n’abandonne pas un suivi bloqué");
    // Une erreur de transfert et une annulation ont une terminaison distincte de la réussite.
    auto failed=job(4);state["jobs"]=QJsonArray{failed};progress.sync(state,true);
    QTRY_COMPARE_WITH_TIMEOUT(jobs.views.size(),3,2000);
    failed["state"]="error";state["jobs"]=QJsonArray{failed};progress.sync(state,true);
    QTRY_VERIFY_WITH_TIMEOUT(jobs.views[2]->terminated,2000);
    check(jobs.views[2]->errorCode!=0,"Une erreur n’est pas annoncée comme une réussite");
    check(jobs.views[2]->errorText.contains("1 lot(s) en erreur.") && jobs.views[2]->errorText.contains("<a href=\"gad://show\">Afficher Gad</a>"),"Le message final d’erreur porte son propre lien vers Gad");
    auto cancelled=job(5);state["jobs"]=QJsonArray{cancelled};progress.sync(state,true);
    QTRY_COMPARE_WITH_TIMEOUT(jobs.views.size(),4,2000);
    cancelled["state"]="cancelled";state["jobs"]=QJsonArray{cancelled};progress.sync(state,true);
    QTRY_VERIFY_WITH_TIMEOUT(jobs.views[3]->terminated,2000);
    check(jobs.views[3]->errorCode==1,"L’annulation ferme le suivi natif");
    check(jobs.views[3]->errorText.contains("1 lot(s) annulé(s).") && jobs.views[3]->errorText.contains("gad://show"),"L’annulation conserve aussi le lien");
    check(notifications.calls==0,"Une seule notification native par session, y compris à la fin");
    // Une panne initiale est retentée avec un délai ; perdre Plasma recrée ensuite le suivi courant.
    QString recoveredCommand;
    TransferNotifications recovering([&](const QString &value){recoveredCommand=value;});
    jobs.failures=1;const int beforeRetry=jobs.requests;
    auto recoverable=job(6);state["jobs"]=QJsonArray{recoverable};recovering.sync(state,true);
    QTRY_COMPARE_WITH_TIMEOUT(jobs.requests,beforeRetry+1,2000);
    QTest::qWait(30);recovering.sync(state,true);QTest::qWait(30);
    check(jobs.requests==beforeRetry+1 && jobs.views.size()==4,"Pas de boucle d’appels en cas d’indisponibilité de Plasma");
    QTest::qWait(2050);recovering.sync(state,true);
    QTRY_COMPARE_WITH_TIMEOUT(jobs.views.size(),5,2000);
    auto *oldView=jobs.views.last();
    check(bus.unregisterService("org.kde.JobViewServer"),"Arrêt du service simulé");QTest::qWait(30);
    check(bus.registerService("org.kde.JobViewServer"),"Redémarrage du service simulé");QTest::qWait(30);
    recovering.sync(state,true);QTRY_COMPARE_WITH_TIMEOUT(jobs.views.size(),6,2000);QTest::qWait(30);
    emit oldView->cancelRequested();QTest::qWait(30);check(recoveredCommand.isEmpty(),"L’ancien suivi ne contrôle plus les transferts");
    emit jobs.views.last()->suspendRequested();QTRY_COMPARE_WITH_TIMEOUT(recoveredCommand,QString("pause"),2000);
    recoverable["state"]="cancelled";state["jobs"]=QJsonArray{recoverable};recovering.sync(state,true);
    QTRY_VERIFY_WITH_TIMEOUT(jobs.views.last()->terminated,2000);
    check(notifications.calls==0,"La récupération ne crée pas de notification classique en doublon");
    // Les renommages comptent dans l’avancement, mais ne fabriquent aucun débit de copie.
    TransferNotifications metadataOnly([](const QString &){},nullptr,false);
    auto renamedJob=job(10);auto renamed=entry("running",0);
    renamed["undo"]=QJsonObject{{"rename_move",true}};renamedJob["entries"]=QJsonArray{renamed};
    auto copyingJob=job(11);copyingJob["entries"]=QJsonArray{entry("running",0)};
    QJsonObject mixed{{"jobs",QJsonArray{renamedJob,copyingJob}},{"paused",false}};
    metadataOnly.sync(mixed,true);QTest::qWait(30);
    renamed["state"]="done";renamed["copied"]=100;renamedJob["state"]="done";renamedJob["entries"]=QJsonArray{renamed};
    mixed["jobs"]=QJsonArray{renamedJob,copyingJob};metadataOnly.sync(mixed,true);
    check(metadataOnly.properties()["processedBytes"].toULongLong()==100,"Le renommage compte dans les octets logiques traités");
    check(metadataOnly.properties()["speed"].toULongLong()==0,"Le renommage ne gonfle pas le débit de copie");
    QTest::qWait(30);copyingJob["entries"]=QJsonArray{entry("running",50)};
    mixed["jobs"]=QJsonArray{renamedJob,copyingJob};metadataOnly.sync(mixed,true);
    check(metadataOnly.properties()["speed"].toULongLong()>0,"Une copie à côté du renommage conserve son débit réel");
    // Une erreur immédiate avant la réponse D-Bus conserve aussi le lien dans les sources US.
    I18n::initialize("en_US");
    TransferNotifications englishFailure([](const QString &){});
    auto immediate=job(50,"error");QJsonObject englishState{{"jobs",QJsonArray{immediate}},{"paused",false}};
    const auto previousViews=jobs.views.size();englishFailure.begin(50);englishFailure.sync(englishState,true);
    QTRY_COMPARE_WITH_TIMEOUT(jobs.views.size(),previousViews+1,2000);
    QTRY_VERIFY_WITH_TIMEOUT(jobs.views.last()->terminated,2000);
    check(jobs.views.last()->errorText.contains("<a href=\"gad://show\">Show Gad</a>"),"Le message US reste cliquable même après une erreur immédiate");
    check(notifications.calls==0,"Le lien d’erreur ne crée pas une seconde notification");
    I18n::initialize("fr_FR");
    // La destruction d’un contrôleur publié ne doit pas envoyer une dernière chaîne vide.
    auto *closing=new TransferNotifications([](const QString &){});
    const auto beforeClosing=jobs.views.size();closing->begin(51);
    closing->sync(QJsonObject{{"jobs",QJsonArray{job(51)}},{"paused",false}},true);
    QTRY_COMPARE_WITH_TIMEOUT(jobs.views.size(),beforeClosing+1,2000);
    auto *closedView=jobs.views.last();
    QTRY_VERIFY_WITH_TIMEOUT(closedView->updates>0,2000);delete closing;
    QTRY_VERIFY_WITH_TIMEOUT(closedView->terminated,2000);
    check(closedView->errorText.contains("gad://show"),"La fermeture du suivi garde le lien");
    // Le choix utilisateur masque le suivi, sans commande de pause ni annulation métier.
    QString hiddenCommand;TransferNotifications hidden([&](const QString &action){hiddenCommand=action;});
    const int beforeHidden=jobs.requests;const auto hiddenState=QJsonObject{{"jobs",QJsonArray{job(501)}},{"paused",false}};
    check(hidden.setProperty("progressMessage",false),"Propriété progressMessage modifiable");hidden.sync(hiddenState,true);QTest::qWait(50);check(jobs.requests==beforeHidden,"Aucune vue KDE lorsque le suivi est désactivé");
    hidden.setProperty("progressMessage",true);hidden.sync(hiddenState,true);QTRY_COMPARE_WITH_TIMEOUT(jobs.requests,beforeHidden+1,2000);auto *hiddenView=jobs.views.last();QTRY_VERIFY_WITH_TIMEOUT(hiddenView->updates>0,2000);
    hidden.setProperty("progressMessage",false);QTRY_VERIFY_WITH_TIMEOUT(hiddenView->terminated,2000);check(hiddenCommand.isEmpty(),"Masquer ne commande pas les transferts");
    Q_EMIT hiddenView->cancelRequested();QTest::qWait(20);check(hiddenCommand.isEmpty(),"Une vue masquée ne peut plus annuler le lot");
    hidden.setProperty("progressMessage",true);hidden.sync(hiddenState,false);QTRY_COMPARE_WITH_TIMEOUT(jobs.requests,beforeHidden+2,2000);auto *reopened=jobs.views.last();QTRY_VERIFY_WITH_TIMEOUT(reopened->updates>0,2000);hidden.setProperty("progressMessage",false);QTRY_VERIFY_WITH_TIMEOUT(reopened->terminated,2000);
    // La désactivation avant la réponse D-Bus ferme également la vue obtenue tardivement.
    TransferNotifications pending([](const QString &){});const int beforePending=jobs.requests;pending.sync(QJsonObject{{"jobs",QJsonArray{job(502)}},{"paused",false}},true);pending.setProperty("progressMessage",false);
    QTRY_COMPARE_WITH_TIMEOUT(jobs.requests,beforePending+1,2000);QTRY_VERIFY_WITH_TIMEOUT(jobs.views.last()->terminated,2000);
    // Un basculement rapide ne doit fermer que l’ancienne réponse, jamais la nouvelle vue.
    TransferNotifications rapid([](const QString &){});const int beforeRapid=jobs.requests;const auto rapidState=QJsonObject{{"jobs",QJsonArray{job(503)}},{"paused",false}};
    rapid.sync(rapidState,true);rapid.setProgressMessage(false);rapid.setProgressMessage(true);rapid.sync(rapidState,true);
    QTRY_COMPARE_WITH_TIMEOUT(jobs.requests,beforeRapid+2,2000);auto *obsolete=jobs.views[jobs.views.size()-2];auto *current=jobs.views.last();
    QTRY_VERIFY_WITH_TIMEOUT(obsolete->terminated,2000);QTRY_VERIFY_WITH_TIMEOUT(current->updates>0,2000);check(!current->terminated,"La nouvelle vue reste active après une ancienne réponse");rapid.setProgressMessage(false);QTRY_VERIFY_WITH_TIMEOUT(current->terminated,2000);
    for(const auto *published:jobs.views) {
        check(published->properties["infoMessage"].toString().count("gad://show")==1,"Chaque état du suivi possède exactement un lien");
        if(published->terminated)check(published->errorText.count("gad://show")==1,"Chaque bilan final possède exactement un lien");
    }
    std::puts("OK : suivi KDE unique, débit, lots/fichiers/octets, MD5, commandes et reconnexion après panne de Plasma.");
}
#include "notifications_test.moc"
