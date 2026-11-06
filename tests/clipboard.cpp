// Tests hors écran de la véritable interface et du décodage du presse-papiers.
// Le moteur est remplacé par un double JSON pour vérifier les commandes émises
// sans effectuer de copie ni modifier les fichiers de l’utilisateur.

#include "../native/window.h"
#include "../native/application.h"
#include "../native/dolphin_destination.h"
#include <QtTest>
#include <cstdlib>
#include <cstring>

/// Double KIO Fuse sur le bus privé : son délai permet de contrôler l’absence de blocage et l’ordre des lots.
class TestKioFuse : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface","org.kde.KIOFuse.VFS")
public:
    QString path;
    QStringList calls;
    bool fail=false;
public Q_SLOTS:
    /// Renvoie uniquement un chemin de test ; aucun téléphone ni montage réel n’est consulté.
    QString mountUrl(const QString &url) {
        calls.append(url);
        if(fail){sendErrorReply("org.kde.KIOFuse.TestError","Téléphone déconnecté");return {};}
        setDelayedReply(true);
        const auto reply=message().createReply(QVariantList{path});
        QTimer::singleShot(30,this,[reply]{QDBusConnection::sessionBus().send(reply);});
        return {};
    }
};

static QList<QJsonObject> received;
static QJsonObject fakePoll;
// Le double applique la pause pour tester un vrai aller-retour commande → état → widgets.
static bool applyPauseCommands=false;
static QJsonObject fakeStatus{{"jobs", QJsonArray()}, {"paused", false}, {"protocol_version", 3}};
/// Simule la frontière Rust : renvoie l’état préparé ou enregistre la commande reçue.
extern "C" char *sc_request(const char *input) {
    auto request = QJsonDocument::fromJson(input).object();
    QJsonObject result;
    if(request["command"]=="poll" && !fakePoll.isEmpty())result=fakePoll;
    else if (request["command"] == "status" || request["command"] == "poll") result = fakeStatus;
    else {
        received << request;
        if(applyPauseCommands && request["command"]=="pause")fakeStatus["paused"]=request["paused"];
        result = {{"id", 1}};
    }
    return strdup(QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
}
/// Libère les réponses du double de moteur avec l’allocateur utilisé par strdup.
extern "C" void sc_free(char *value) { free(value); }
/// Interrompt le test immédiatement avec une explication si une assertion échoue.
static void check(bool valid, const char *message) { if (!valid) { std::fprintf(stderr, "%s\n", message); std::abort(); } }

/// Exécute les scénarios presse-papiers, clavier, sélection, survol, icône et cycle de vie de fenêtre.
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    applicationIcon();I18n::initialize("fr_FR");
    app.setApplicationName("GadTests"); app.setOrganizationName("GadTests");
    // Le raccourci ne doit jamais réutiliser les anciens fichiers sélectionnés comme destination.
    struct DolphinMock {
        QString title="/tmp/destination avec #";
        QStringList names{"org.kde.dolphin-1","org.kde.dolphin"};
        QSet<QString> active{"/dolphin/Dolphin_1"};
        bool opened=true,changeTitle=false,loseFocus=false,failed=false;
        int titleReads=0,activeReads=0;
        QDBusMessage call(const QString &service,const QString &path,const QString &,const QString &method,const QVariantList &) {
            if(failed)return QDBusMessage::createError("org.kde.Test.Error","Indisponible");
            QVariant value;
            if(method=="ListNames")value=names;
            else if(method=="GetNameOwner")value=service.endsWith("-2")?QString(":1.2"):QString(":1.1");
            else if(method=="Introspect")value=QString("<node><node name='Dolphin_1'/><node name='Dolphin_2'/></node>");
            else if(method=="isActiveWindow") {++activeReads;value=active.contains(path) && !(loseFocus && activeReads>2);}
            else if(method=="isUrlOpen")value=opened;
            else if(method=="Get"){++titleReads;value=QVariant::fromValue(QDBusVariant(changeTitle && titleReads>1?title+"/autre":title));}
            return QDBusMessage::createMethodCall("org.kde.Test","/test","org.kde.Test","test").createReply({value});
        }
        DolphinDestination::Result resolve() {
            return DolphinDestination::current([this](const QString &service,const QString &path,const QString &interface,const QString &method,const QVariantList &arguments){return call(service,path,interface,method,arguments);});
        }
    };
    // Reproduire le vrai format KDE : [General] est réservé par le lecteur INI Qt.
    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation));
    QFile dolphinConfig(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)+"/dolphinrc");
    check(dolphinConfig.open(QIODevice::WriteOnly),"Création de la configuration Dolphin isolée");
    dolphinConfig.write("[General]\nShowFullPathInTitlebar=true\n");dolphinConfig.close();
    DolphinMock dolphin;
    check(dolphin.resolve().destination=="/tmp/destination avec #","La fenêtre active fournit la destination exacte, sans utiliser la sélection précédente");
    dolphin.active={"/dolphin/Dolphin_2"};dolphin.titleReads=0;
    check(dolphin.resolve().destination=="/tmp/destination avec #","Le bon onglet de la fenêtre active reste ciblé");
    dolphin.active={"/dolphin/Dolphin_1","/dolphin/Dolphin_2"};check(!dolphin.resolve().error.isEmpty(),"Deux fenêtres actives imposent un refus sans copie");
    dolphin.active={};check(!dolphin.resolve().error.isEmpty(),"Aucune fenêtre active ne choisit un dossier par défaut");
    dolphin.active={"/dolphin/Dolphin_1"};dolphin.title="Saison 01";check(!dolphin.resolve().error.isEmpty(),"Un simple nom de dossier ne devient jamais un chemin relatif");
    dolphin.title="/tmp/destination";dolphin.opened=false;check(!dolphin.resolve().error.isEmpty(),"Le chemin doit être reconnu comme ouvert par Dolphin");
    dolphin.opened=true;dolphin.changeTitle=true;dolphin.titleReads=0;check(!dolphin.resolve().error.isEmpty(),"Une navigation entre deux lectures annule la préparation");
    dolphin.changeTitle=false;dolphin.loseFocus=true;dolphin.activeReads=0;check(!dolphin.resolve().error.isEmpty(),"Une perte de focus n’utilise pas une ancienne destination");
    dolphin.loseFocus=false;dolphin.title="sftp://alice@server/dossier%20avec%23";
    check(dolphin.resolve().destination=="sftp://alice@server/dossier%20avec%23","Les destinations distantes conservent leur encodage");
    dolphin.failed=true;check(!dolphin.resolve().error.isEmpty(),"Une panne D-Bus ne soumet aucun lot");
    // Le module Konqueror fournit une vue active stable, avec refus d’une navigation pendant la lecture.
    int konquerorReads=0;bool konquerorChanged=false;QStringList konquerorServices{"org.hizoka.Gad.Konqueror.p123"};
    const DolphinDestination::Call konquerorCall=[&](const QString &,const QString &,const QString &,const QString &method,const QVariantList &){
        const QVariant value=method=="ListNames"?QVariant(konquerorServices):QVariant(konquerorChanged && ++konquerorReads>1?QString():QString("sftp://alice@server/folder%20with%23"));
        return QDBusMessage::createMethodCall("org.test","/test","org.test","test").createReply({value});
    };
    check(DolphinDestination::konqueror(konquerorCall).destination=="sftp://alice@server/folder%20with%23","Konqueror conserve l’URL exacte du dossier actif");
    konquerorChanged=true;check(!DolphinDestination::konqueror(konquerorCall).error.isEmpty(),"Konqueror refuse un dossier changé entre deux lectures");
    konquerorChanged=false;konquerorServices.append("org.hizoka.Gad.Konqueror.p456");
    check(!DolphinDestination::konqueror(konquerorCall).error.isEmpty(),"Deux destinations Konqueror actives sont refusées");
    konquerorServices.clear();check(DolphinDestination::konqueror(konquerorCall).destination.isEmpty(),"L’absence de Konqueror laisse la détection Dolphin disponible");
    QString shortcutParseError;const auto currentPaste=parse({"gad","--paste-dolphin"},shortcutParseError);
    check(shortcutParseError.isEmpty() && currentPaste["dolphin_current"]==true && !currentPaste.contains("destination"),"Le raccourci ne reçoit aucun argument issu de l’ancienne sélection");
    check(parse({"gad","--paste-current"},shortcutParseError)==currentPaste,"La commande commune aux gestionnaires conserve la destination active");
    const auto stalePaste=parse({"gad","--paste-dolphin","file:///ancienne/source","sftp://serveur/selection"},shortcutParseError);
    check(shortcutParseError.isEmpty() && stalePaste==currentPaste,"Les URL de contexte périmées restent ignorées, même multiples ou distantes");
    // Une réponse différentielle garde les lots omis, retire les supprimés et respecte l’ordre du moteur.
    QueueSnapshot snapshot;
    snapshot.apply({{"revision",1},{"reset",true},{"job_ids",QJsonArray{1,2}},{"jobs",QJsonArray{QJsonObject{{"id",1},{"state","done"}},QJsonObject{{"id",2},{"state","waiting"}}}}});
    auto deltaIds=snapshot.apply({{"revision",2},{"job_ids",QJsonArray{1,2}},{"jobs",QJsonArray{QJsonObject{{"id",2},{"state","running"}}}}});
    check(deltaIds==QSet<qint64>{2} && snapshot.state()["jobs"].toArray()[0].toObject()["state"]=="done","Fusion des seuls lots modifiés");
    snapshot.apply({{"revision",3},{"job_ids",QJsonArray{2}},{"jobs",QJsonArray{}}});
    check(snapshot.state()["jobs"].toArray().size()==1 && snapshot.state()["jobs"].toArray()[0].toObject()["id"]==2,"Suppression d’un lot absent de l’ordre courant");
    check(snapshot.apply({{"revision",3},{"unchanged",true}}).isEmpty() && snapshot.revision()==3,"Une révision identique préserve le cache");
    // Les URI invalides restent identifiables comme telles : aucune conversion silencieuse en chemin local.
    for(const auto &invalid:QStringList{"file://server/tmp/file","file:///tmp/file?ignored=1","file:///tmp/file#ignored","ftp://files.example/%ZZ","file://alice@localhost/tmp/file","file://localhost:22/tmp/file","file:///tmp/%FF","file:///tmp/%00bad","file://","file://localhost","sftp://server/%00bad","smb://server/share/%FF"}) {
        check(SourceUrls::argument(invalid)==invalid,"La CLI conserve une URL invalide pour son rejet explicite");
        QString error;const auto request=parse({"gad","--copy",invalid,"--to","/tmp"},error);
        bool done=false;QJsonObject refused;
        auto *resolver=new SourceUrls::Resolver(request,&app,[&](QJsonObject result){done=true;refused=result;});resolver->start();
        check(done && refused.contains("error"),"Les URI invalides sont rejetées avant toute résolution ou copie");
    }
    check(!SourceUrls::isUrl("folder/proto://file") && QDir::isAbsolutePath(SourceUrls::argument("folder/proto://file")),"Un chemin relatif contenant deux-points et des slashs reste un chemin");
    check(SourceUrls::argument("file://localhost/tmp/with%20space")=="/tmp/with space","localhost désigne le chemin local, sans préfixe UNC");
    check(SourceUrls::argument("file:///tmp/%EF%BF%BD")==QString::fromUtf8("/tmp/\xef\xbf\xbd") && SourceUrls::argument("file:///tmp/%25FF")=="/tmp/%FF","Les caractères Unicode valides et les signes pourcent ne sont pas rejetés");
    QMap<QString,QByteArray> localhostFormats{{"text/uri-list","file://localhost/tmp/with%20space\n"}};
    auto localhostFiles=Clipboard::decode(localhostFormats.keys(),[&](const QString &format){return localhostFormats.value(format);});
    check(localhostFiles.error.isEmpty() && localhostFiles.sources==QStringList{"/tmp/with space"},"Le presse-papiers normalise localhost comme la CLI");
    // Changer d’intention entre les deux formats ne doit pas transformer une copie en déplacement.
    int markerReads=0;
    const auto changedClipboard=Clipboard::decode({"text/uri-list","application/x-kde-cutselection"},[&](const QString &format) {
        if(format=="application/x-kde-cutselection")return ++markerReads==1?QByteArray("1"):QByteArray("0");
        return QByteArray("file:///tmp/copied-after-cut\n");
    });
    check(!changedClipboard.error.isEmpty() && changedClipboard.sources.isEmpty(),"Un changement Copier/Couper pendant la lecture invalide la demande");
    // Les destinations distantes non montées ne doivent pas devenir des chemins locaux fictifs.
    for(const auto &arguments:QList<QStringList>{{"gad","--copy","/tmp/source","--to","https://server/share"},{"gad","--move","/tmp/source","--to","file://server/tmp"},{"gad","--paste","ftp://server/path?bad=1"},{"gad","--paste-context","sftp://server/path#bad"}}) {
        QString error;parse(arguments,error);check(!error.isEmpty(),"La CLI refuse explicitement une destination distante non montée");
    }
    QString destinationError;
    check(parse({"gad","--copy","/tmp/source","--to","file://localhost/tmp/destination"},destinationError)["destination"]=="/tmp/destination" && destinationError.isEmpty(),"Une destination file locale est décodée comme les sources");
    // Formats KDE/GNOME : espaces, accents, sauts de ligne et marqueur Couper doivent survivre au décodage.
    const QByteArray urls("# commentaire\r\nfile:///tmp/avec%20espaces/%C3%A9t%C3%A9%0A.txt\r\nfile:///tmp/deux\r\n");
    QMap<QString, QByteArray> formats{{"text/uri-list", urls}, {"application/x-kde-cutselection", "1"}};
    auto read = [&](const QString &type) { return formats.value(type); };
    auto files = Clipboard::decode(formats.keys(), read);
    check(files.error.isEmpty() && files.mode == "move" && files.sources == QStringList{"/tmp/avec espaces/été\n.txt", "/tmp/deux"}, "Décodage KDE cut/URI");
    formats = {{"x-special/gnome-copied-files", "cut\nfile:///tmp/deux\n"}};
    check(Clipboard::decode(formats.keys(), read).mode == "move", "Décodage GNOME cut");
    formats = {{"application/x-kde4-urilist", "file:///tmp/deux\n"}};
    check(Clipboard::decode(formats.keys(), read).sources == QStringList{"/tmp/deux"}, "Décodage KDE alternatif");
    formats = {{"text/uri-list", "file:///tmp/deux\nhttps://serveur/partage\n"}};
    files = Clipboard::decode(formats.keys(), read);
    check(!files.error.isEmpty() && files.sources.isEmpty(), "Rejet intégral des protocoles non pris en charge");
    // Les URI KDE Connect restent encodées pour ne pas confondre les caractères du nom avec la syntaxe URL.
    const QString phoneUrl="kdeconnect://phone/android/DCIM/avec%20espace%23.jpg";
    formats={{"text/uri-list",phoneUrl.toUtf8()+"\n"},{"application/x-kde-cutselection","1"}};
    files=Clipboard::decode(formats.keys(),read);
    check(files.error.isEmpty() && files.mode=="move" && files.sources==QStringList{phoneUrl},"Le presse-papiers accepte le téléphone et conserve Couper");
    formats={{"text/uri-list",(phoneUrl+"?ambigu=1\n").toUtf8()}};
    check(!Clipboard::decode(formats.keys(),read).error.isEmpty(),"Une URL de téléphone ambiguë reste refusée");
    QTemporaryDir temp;
    QFile helper(temp.filePath("wl-paste")); check(helper.open(QIODevice::WriteOnly), "Création helper");
    helper.write("#!/bin/sh\nif [ \"$1\" = --list-types ]; then printf '%s\\n' text/uri-list application/x-kde-cutselection; elif [ \"$3\" = text/uri-list ]; then printf '%s\\n' file:///tmp/deux; else printf 1; fi\n");
    helper.close(); helper.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    files = Clipboard::wayland(helper.fileName());
    check(files.error.isEmpty() && files.mode == "move" && files.sources == QStringList{"/tmp/deux"}, "Lecture du helper Wayland");
    // Le faux moteur permet de vérifier les actions réelles des widgets sans lancer de transfert.
    Window window; window.show(); app.processEvents();
    check(!applicationIcon().isNull() && !window.windowIcon().pixmap(64,64).isNull(), "Icône Gad choisie embarquée et chargée");
    auto *destination = window.findChild<QLineEdit *>("destinationInput");
    destination->setText(temp.path()); destination->setFocus(); app.processEvents();
    auto putFiles = [&](bool cut) {
        auto *mime = new QMimeData;
        mime->setUrls({QUrl::fromLocalFile("/tmp/avec espaces/été\n.txt")});
        if (cut) mime->setData("application/x-kde-cutselection", "1");
        QApplication::clipboard()->setMimeData(mime); app.processEvents();
    };
    putFiles(false); received.clear();
    QTest::keyClick(destination, Qt::Key_V, Qt::ControlModifier);
    check(received.size() == 1 && received.last()["command"] == "enqueue" && received.last()["mode"] == "copy", "Ctrl+V doit copier malgré le focus Destination");
    check(destination->text() == temp.path(), "Ctrl+V fichiers ne doit pas modifier la destination");
    putFiles(true); received.clear();
    QTest::keyClick(destination, Qt::Key_V, Qt::ControlModifier);
    check(received.size() == 1 && received.last()["mode"] == "move", "Ctrl+X puis Ctrl+V doit déplacer");
    received.clear(); QApplication::clipboard()->setText("/tmp/nouvelle-destination"); destination->selectAll();
    QTest::keyClick(destination, Qt::Key_V, Qt::ControlModifier);
    check(received.isEmpty() && destination->text() == "/tmp/nouvelle-destination", "Ctrl+V texte doit rester utilisable dans le champ Destination");
    putFiles(false); received.clear();
    for (auto *button : window.findChildren<QPushButton *>()) if (button->text() == "Coller ici") QTest::mouseClick(button, Qt::LeftButton);
    check(received.size() == 1 && received.last()["mode"] == "copy", "Bouton Coller ici");
    // L’option MD5 doit être mémorisée et transmise aux requêtes de la fenêtre comme de Dolphin.
    auto *spaceCheck=window.findChild<QCheckBox *>("verifySpace");
    check(spaceCheck && !spaceCheck->isChecked(),"La vérification d’espace est facultative et désactivée par défaut");
    received.clear();spaceCheck->setChecked(true);
    check(QSettings().value("verifySpace").toBool() && received.last()["command"]=="configure" && received.last()["verify_space"]==true,"Option d’espace mémorisée et transmise au moteur");
    spaceCheck->setChecked(false);
    auto *md5 = window.findChild<QCheckBox *>("verifyMd5");
    check(md5 && !md5->isChecked(), "Option MD5 désactivée par défaut");
    md5->setChecked(true);
    check(QSettings().value("verifyMd5").toBool(), "Option MD5 mémorisée");
    received.clear(); window.enqueue({"/tmp/source-md5"});
    check(received.size() == 1 && received.last()["verify_md5"].toBool(), "Case MD5 transmise aux nouveaux transferts");
    received.clear(); window.handle({{"command", "enqueue"}, {"sources", QJsonArray{"/tmp/source-dolphin"}}, {"destination", temp.path()}});
    check(received.size() == 1 && received.last()["verify_md5"].toBool(), "Les demandes Dolphin utilisent la case MD5");
    received.clear(); window.handle({{"command", "enqueue"}, {"sources", QJsonArray{"/tmp/source"}}, {"destination", temp.path()}, {"verify_md5", false}});
    check(received.size() == 1 && received.last()["verify_md5"] == false, "Un choix explicite reste propre à son opération");
    md5->setChecked(false);
    // Regression: progress polling must preserve the hovered row and its tooltip.
    // Une progression simulée doit réutiliser les mêmes lignes et leur état de survol.
    QJsonObject entry{{"source", "/tmp/fichier-en-cours"}, {"destination", "/tmp/destination/fichier-en-cours"},
                      {"kind", "file"}, {"state", "running"}, {"size", 100000}, {"copied", 0}, {"error", ""}};
    QJsonObject job{{"id", 42}, {"mode", "copy"}, {"sources", QJsonArray{"/tmp/fichier-en-cours"}},
                    {"destination", "/tmp/destination"}, {"state", "running"}, {"entries", QJsonArray{entry}}, {"error", ""}};
    const auto publish = [&] { fakeStatus["jobs"] = QJsonArray{job}; window.refresh(); app.processEvents(); };
    publish();
    auto *tree = window.findChild<QTreeWidget *>();
    check(tree->selectionMode()==QAbstractItemView::ExtendedSelection,"Sélection multiple activée");
    auto *row = window.entryRow(42,0);auto *parent=RowTree::owner(row);
    check(row==tree->topLevelItem(0) && !row->parent(),"Le fichier est affiché sans en-tête de lot");
    tree->setCurrentItem(row);
    const QPersistentModelIndex index(tree->indexFromItem(row));
    const auto position = tree->visualItemRect(row).center();
    const QPoint sourcePosition(tree->header()->sectionViewportPosition(0)+25, position.y());
    QTest::mouseMove(tree->viewport(), sourcePosition);
    QHelpEvent help(QEvent::ToolTip, sourcePosition, tree->viewport()->mapToGlobal(sourcePosition));
    QApplication::sendEvent(tree->viewport(), &help);
    check(QToolTip::isVisible(), "Infobulle initialement visible");
    const auto tooltip = QToolTip::text();
    QSignalSpy removed(tree->model(), &QAbstractItemModel::rowsRemoved);
    QSignalSpy inserted(tree->model(), &QAbstractItemModel::rowsInserted);
    QSignalSpy reset(tree->model(), &QAbstractItemModel::modelReset);
    for (int i = 1; i <= 3; ++i) {
        entry["copied"] = i * 10000; job["entries"] = QJsonArray{entry}; publish();
        QTest::qWait(450); // Includes a real polling timer tick.
        check(index.isValid() && row==tree->topLevelItem(0) && RowTree::owner(row)==parent, "Identité des lignes conservée pendant le transfert");
        check(tree->currentItem() == row && row->isSelected(), "Sélection conservée");
        check(QToolTip::isVisible() && QToolTip::text() == tooltip, "Infobulle conservée pendant le rafraîchissement");
    }
    check(removed.isEmpty() && inserted.isEmpty() && reset.isEmpty(), "Aucune reconstruction du modèle pendant la progression");
    fakeStatus["paused"] = true; window.refresh();
    check(row->text(2) == "En pause" && parent->text(2) == "En pause", "La pause est visible sur le fichier et son opération");
    check(index.isValid() && row==window.entryRow(42,0), "La pause conserve la ligne existante");
    entry["state"] = "waiting"; job["state"] = "waiting"; job["entries"] = QJsonArray{entry}; publish();
    check(row->text(2) == "En pause" && parent->text(2) == "En pause", "Les fichiers en attente indiquent aussi la pause");
    entry["state"] = "done"; job["entries"] = QJsonArray{entry}; publish();
    check(row->text(2) == "OK", "La pause ne change pas les fichiers terminés");
    entry["state"] = "running"; job["state"] = "running"; job["entries"] = QJsonArray{entry};
    fakeStatus["paused"] = false; publish();
    check(row->text(2) == "En cours" && parent->text(2) == "En cours", "La reprise rétablit les états réels");
    QToolTip::hideText();
    auto *filter=static_cast<TransferTree *>(window.findChild<QTreeWidget *>());
    filter->setStateFilter("done");
    check(row->isHidden() && index.isValid(), "Filtrer conserve les lignes existantes");
    filter->clearColumnFilter(2);
    check(!row->isHidden() && row==window.entryRow(42,0), "Retirer le filtre conserve les lignes existantes");
    QJsonObject second = entry; second["source"] = "/tmp/deuxieme";
    job["entries"] = QJsonArray{entry, second}; publish();
    check(tree->topLevelItemCount()==2 && tree->topLevelItem(0)==row && index.isValid(), "Ajout de fichiers sans reconstruire les lignes existantes");
    fakeStatus["jobs"] = QJsonArray(); window.refresh();
    check(tree->topLevelItemCount() == 0 && !index.isValid(), "Effacement des opérations absententes du moteur");
    auto *tray = window.findChild<QSystemTrayIcon *>("transferTray");
    check(tray && !tray->icon().isNull(), "Icône de notification disponible");
    check(tray->toolTip().contains("0 transfert(s) restant(s)"), "Compteur vide");
    job["entries"] = QJsonArray{entry};
    QJsonObject queued = job; queued["id"] = 43; queued["state"] = "waiting"; queued["mode"] = "move";
    QJsonObject scanning = job; scanning["id"] = 44; scanning["state"] = "scanning"; scanning["entries"] = QJsonArray();
    QJsonObject completed = job; completed["id"] = 45; completed["state"] = "done";
    fakeStatus["jobs"] = QJsonArray{job, queued, scanning, completed}; window.refresh();
    tree->clearSelection();
    window.entryRow(42,0)->setSelected(true);window.entryRow(43,0)->setSelected(true);
    received.clear();window.cancelSelection(false);
    check(received.size()==1 && received.last()["command"]=="cancel_selection" && received.last()["jobs"].toArray().isEmpty() && received.last()["entries"].toArray()==QJsonArray{QJsonObject{{"job",42},{"index",0}},QJsonObject{{"job",43},{"index",0}}},"Annulation multiple de fichiers de demandes différentes");
    tree->clearSelection(); window.refresh();
    // Les commandes globales doivent rester accessibles sans aucune sélection.
    auto *cancelAll = window.findChild<QAction *>("cancelAllAction");
    check(cancelAll && cancelAll->isEnabled(), "Tout annuler reste disponible sans sélection");
    received.clear(); cancelAll->trigger();
    check(received.size()==1 && received.last()["command"]=="cancel_all", "Tout annuler ne dépend pas de la sélection");
    auto *undoAll = window.findChild<QAction *>("undoAllAction");
    check(undoAll && undoAll->isEnabled(), "Tout remettre comme avant reste disponible sans sélection");
    received.clear(); undoAll->trigger();
    check(received.size()==1 && received.last()["command"]=="undo_all", "Remise en état globale sans sélection");
    // Les raccourcis ne doivent agir sur les fichiers que lorsque la liste a le focus.
    tree->setFocus(); tree->setCurrentItem(window.entryRow(42,0)); app.processEvents();
    received.clear(); QTest::keyClick(tree, Qt::Key_Delete);
    check(received.size()==1 && received.last()["command"]=="delete_selection" && received.last()["jobs"].toArray().isEmpty() && received.last()["entries"].toArray()==QJsonArray{QJsonObject{{"job",42},{"index",0}}}, "Suppr transmet seulement le fichier sélectionné");
    window.updateRowActions();
    auto *rowUndo=window.findChild<QAction *>("rowUndo");
    check(rowUndo && rowUndo->isEnabled(), "Menu contextuel disponible sur la sélection");
    received.clear(); rowUndo->trigger();
    check(received.size()==1 && received.last()["command"]=="undo_selection" && received.last()["jobs"].toArray().isEmpty() && received.last()["entries"].toArray()==QJsonArray{QJsonObject{{"job",42},{"index",0}}}, "Annulation contextuelle limitée au fichier sélectionné");
    received.clear(); window.findChild<QAction *>("rowPause")->trigger();
    check(received.size()==1 && received.last()["command"]=="pause_selection" && received.last()["paused"]==true, "Pause contextuelle de la sélection");
    received.clear(); window.findChild<QAction *>("rowResume")->trigger();
    check(received.size()==1 && received.last()["command"]=="pause_selection" && received.last()["paused"]==false, "Reprise contextuelle de la sélection");
    received.clear(); QTest::keyClick(tree, Qt::Key_Backspace);
    check(received.size()==1 && received.last()["command"]=="backspace_selection" && received.last()["entries"].toArray()==QJsonArray{QJsonObject{{"job",42},{"index",0}}}, "Retour arrière transmet seulement le fichier sélectionné");
    tree->topLevelItem(0)->setSelected(true);
    received.clear(); QTest::keyClick(tree, Qt::Key_Delete);
    check(received.size()==1 && received.last()["jobs"].toArray().isEmpty() && received.last()["entries"].toArray()==QJsonArray{QJsonObject{{"job",42},{"index",0}}}, "Suppr conserve la granularité du fichier racine");
    destination->setFocus(); destination->setText("abc"); destination->setCursorPosition(0); app.processEvents();
    received.clear(); QTest::keyClick(destination, Qt::Key_Delete);
    check(received.isEmpty() && destination->text()=="bc", "Suppr conserve son comportement dans le champ Destination");
    received.clear(); destination->setCursorPosition(1); QTest::keyClick(destination, Qt::Key_Backspace);
    check(received.isEmpty() && destination->text()=="c", "Retour arrière reste utilisable dans Destination");
    tree->clearSelection();
    tree->topLevelItem(0)->setSelected(true); tree->topLevelItem(1)->setSelected(true);
    received.clear();window.cancelSelection(true);
    check(received.size()==1 && received.last()["command"]=="undo_selection" && received.last()["jobs"].toArray().isEmpty() && received.last()["entries"].toArray()==QJsonArray{QJsonObject{{"job",42},{"index",0}},QJsonObject{{"job",43},{"index",0}}},"Remise en état des fichiers de demandes différentes");
    auto *keep=window.findChild<QCheckBox *>("keepHistory");check(keep && keep->isChecked(),"Conservation de l’historique activée par défaut");
    received.clear();keep->setChecked(false);
    check(received.size()==1 && received.last()["command"]=="configure" && received.last()["keep_history"]==false && QSettings().value("keepHistory").toBool()==false,"Option de conservation enregistrée");
    keep->setChecked(true);
    // A background paste must enqueue immediately without showing or restoring the window.
    // Un collage externe caché doit rester caché tout en soumettant immédiatement le travail.
    window.hide(); received.clear();
    auto pasted = window.handle({{"command", "paste"}, {"destination", temp.path()}});
    check(!pasted.contains("error") && received.size()==1 && received.last()["command"]=="enqueue" && received.last()["background"]==true, "Coller avec Gad ajoute immédiatement la copie en arrière-plan");
    check(!window.isVisible(), "Le collage externe laisse la fenêtre masquée");
    putFiles(true); received.clear();
    window.handle({{"command", "paste"}, {"destination", temp.path()}});
    check(!window.isVisible() && received.size()==1 && received.last()["mode"]=="move", "Le collage externe d’un fichier coupé reste en arrière-plan");
    window.handle({{"command", "show"}}); window.showMinimized(); app.processEvents();
    received.clear(); window.handle({{"command", "enqueue"}, {"background", true}, {"sources", QJsonArray{"/tmp/source"}}, {"destination", temp.path()}});
    check(window.isMinimized() && received.size()==1, "Un collage en arrière-plan ne restaure pas une fenêtre réduite");
    window.showNormal(); app.processEvents(); received.clear();
    window.handle({{"command", "paste"}, {"destination", temp.path()}});
    check(window.isVisible() && received.size()==1, "Une fenêtre déjà ouverte reste visible après un collage externe");
    {
    Window coldStart;
    received.clear(); coldStart.handle({{"command", "enqueue"}, {"background", true}, {"sources", QJsonArray{"/tmp/source"}}, {"destination", temp.path()}});
    check(!coldStart.isVisible() && received.size()==1, "Premier lancement par collage : la fenêtre reste masquée et la copie part");
    coldStart.showMainWindow(); app.processEvents();
    check(coldStart.isVisible() && !coldStart.isMinimized(),"Le clic sur la notification ouvre Gad");
    coldStart.hide();
    }
    window.activateWindow(); window.raise(); app.processEvents(); putFiles(false);

    for (auto *button:window.findChildren<QPushButton *>()) if(button->text()=="Effacer les opérations terminées")check(!button->icon().isNull(),"Icône du nettoyage");
    check(tray->toolTip().contains("3 transfert(s) restant(s)") && tray->toolTip().contains("1 en cours · 1 en attente · 1 en analyse"), "Compteur par opération incluant la file et excluant les transferts terminés");
    const auto busyIcon = tray->icon().cacheKey();
    window.refresh(); check(tray->icon().cacheKey() == busyIcon, "Icône stable quand le compteur est inchangé");
    fakeStatus["paused"] = true; window.refresh();
    check(tray->toolTip().contains("File en pause") && tray->icon().cacheKey() != busyIcon, "Icône et infobulle de pause");
    applyPauseCommands=true;fakeStatus["paused"]=false;window.refresh();
    QPushButton *globalPause=nullptr;
    for(auto *button:window.findChildren<QPushButton *>())if(button->text()=="Tout mettre en pause")globalPause=button;
    check(globalPause,"Le bouton global existe avant la pause native");
    received.clear();window.controlDesktopTransfers("pause");
    check(received.size()==1 && received.last()["command"]=="pause" && received.last()["paused"]==true,"La notification utilise la pause globale de la fenêtre");
    check(globalPause->text()=="Reprendre la file" && tray->toolTip().contains("File en pause"),"La pause native synchronise bouton et icône à partir de la réponse moteur");
    received.clear();window.controlDesktopTransfers("resume");
    check(received.size()==1 && received.last()["command"]=="pause" && received.last()["paused"]==false && globalPause->text()=="Tout mettre en pause","La reprise native synchronise le bouton de fenêtre");
    check(!tray->toolTip().contains("File en pause"),"La reprise native efface la pause du systray");
    window.controlDesktopTransfers("pause");received.clear();globalPause->click();
    check(received.size()==1 && received.last()["command"]=="pause" && received.last()["paused"]==false && globalPause->text()=="Tout mettre en pause","Le bouton reprend réellement une pause demandée depuis la notification");
    window.controlDesktopTransfers("pause");
    // L’annulation reste ciblée sur les lots actifs et ne devient pas une pause globale.
    received.clear();window.controlDesktopTransfers("cancel");
    check(received.size()==1 && received.last()["command"]=="cancel_selection" && !received.last().contains("paused") && !received.last()["jobs"].toArray().isEmpty(),"L’annulation native conserve sa commande ciblée");
    QAction *resume = nullptr; QAction *show = nullptr;
    for (auto *action : tray->contextMenu()->actions()) {
        if (action->text() == "Reprendre la file") resume = action;
        if (action->text() == "Afficher Gad") show = action;
    }
    check(resume && show, "Menu de notification");
    received.clear(); resume->trigger();
    check(received.size() == 1 && received.last()["command"] == "pause" && received.last()["paused"] == false, "Reprise depuis le menu de notification");
    check(globalPause->text()=="Tout mettre en pause" && !tray->toolTip().contains("File en pause"),"Le systray reprend la pause native et resynchronise la fenêtre");
    applyPauseCommands=false;fakeStatus["paused"]=true;window.refresh();
    window.hide(); show->trigger(); app.processEvents();
    check(window.isVisible(), "Réouverture depuis le menu de notification");
    window.hide(); tray->activated(QSystemTrayIcon::Trigger); app.processEvents();
    check(window.isVisible(), "Réouverture au clic sur l’icône");
    tray->activated(QSystemTrayIcon::Trigger); app.processEvents();
    check(!window.isVisible(), "Un clic masque la fenêtre affichée");
    tray->activated(QSystemTrayIcon::Trigger); app.processEvents();
    check(window.isVisible() && !window.isMinimized(), "Le clic suivant réaffiche la fenêtre");
    window.showMinimized(); app.processEvents();
    tray->activated(QSystemTrayIcon::Trigger); app.processEvents();
    check(window.isVisible() && !window.isMinimized(), "Un clic restaure une fenêtre réduite");
    tray->activated(QSystemTrayIcon::Context); app.processEvents();
    check(window.isVisible(), "Le clic droit ne masque pas la fenêtre");
    fakeStatus["paused"] = false; entry["state"] = "conflict"; job["entries"] = QJsonArray{entry};
    fakeStatus["jobs"] = QJsonArray{job}; window.refresh();
    check(tray->toolTip().contains("Un conflit attend votre choix") && tray->toolTip().contains("1 transfert(s) restant(s)"), "Conflit visible dans la notification");
    fakeStatus["jobs"] = QJsonArray(); window.refresh();
    check(tray->toolTip().contains("0 transfert(s) restant(s)"), "Compteur revient à zéro");
    entry["state"] = "verifying"; entry["verified"] = 50000;
    job["verify_md5"] = true; job["entries"] = QJsonArray{entry};
    fakeStatus["jobs"] = QJsonArray{job}; window.refresh();
    auto *verifiedRow = window.entryRow(42,0);
    check(verifiedRow->text(2) == "Vérification MD5…" && verifiedRow->text(3).startsWith("MD5 "), "Étape et progression MD5 visibles");
    filter->setStateFilter("verifying");
    check(!verifiedRow->isHidden(), "Le filtre d’état retrouve la vérification MD5");
    filter->clearColumnFilter(2);
    entry["state"] = "done"; entry["md5_source"] = "900150983cd24fb0d6963f7d28e17f72"; entry["md5_destination"] = entry.value("md5_source");
    job["state"] = "done"; job["entries"] = QJsonArray{entry}; fakeStatus["jobs"] = QJsonArray{job}; window.refresh();
    check(verifiedRow->text(4).startsWith("MD5 OK · 900150"), "Résultat MD5 affiché par fichier");
    // Le repli automatique se produit une seule fois ; rouvrir un lot terminé reste possible.
    auto *completedParent=tree->topLevelItem(0);
    check(completedParent==verifiedRow && completedParent->text(2)=="OK" && completedParent->foreground(2).color()==QColor("#24834e"),"Un fichier terminé reste vert sans en-tête supplémentaire");
    completedParent->setExpanded(true);window.refresh();
    check(completedParent->isExpanded(),"Une opération terminée rouverte reste ouverte");
    entry["state"]="cancelled"; job["state"]="cancelled"; job["entries"]=QJsonArray{entry}; publish();
    tree->clearSelection(); tree->topLevelItem(0)->setSelected(true); window.updateRowActions();
    auto *retry=window.findChild<QAction *>("rowRetry");
    check(retry && retry->isEnabled(), "Réactivation disponible pour une opération annulée");
    received.clear(); retry->trigger();
    check(received.size()==1 && received.last()["command"]=="retry_selection" && received.last()["entries"].toArray()==QJsonArray{QJsonObject{{"job",42},{"index",0}}}, "Réactivation du fichier sélectionné");
    fakeStatus["jobs"] = QJsonArray(); window.refresh();
    if (const auto preview = qEnvironmentVariable("GAD_TRAY_PREVIEW"); !preview.isEmpty()) {
        QPixmap strip(64 * 5, 64); strip.fill(QColor("#eef0f2")); QPainter painter(&strip);
        int x = 0;
        for (const auto &icon : {transferIcon(0, false, false), transferIcon(3, false, false), transferIcon(12, true, false), transferIcon(5, false, true), transferIcon(100, false, false)}) {
            painter.drawPixmap(x, 0, icon.pixmap(64, 64)); x += 64;
        }
        painter.end(); check(strip.save(preview), "Aperçu des icônes");
    }
    // Le contexte Dolphin peut être un dossier ou une sélection de fichiers du même parent.
    QFile contextFile(temp.filePath("document"));check(contextFile.open(QIODevice::WriteOnly),"Fichier de contexte");contextFile.close();
    QString parseError;
    auto parsed=parse({"gad","--paste-context",contextFile.fileName()},parseError);
    check(parseError.isEmpty() && parsed["destination"]==temp.path(),"Le raccourci fonctionne sur un fichier sélectionné");
    parsed=parse({"gad","--paste-context",contextFile.fileName(),temp.filePath("autre")},parseError);
    check(parsed["destination"]==temp.path(),"Le raccourci fonctionne avec plusieurs éléments sélectionnés");
    parsed=parse({"gad","--paste-context",temp.path()},parseError);
    check(parsed["destination"]==temp.path() && parsed["background"]==true,"Le raccourci conserve le dossier de destination et demande un collage en arrière-plan");
    parsed=parse({"gad","--paste",temp.path()},parseError);
    check(parsed["command"]=="paste" && parsed["background"]==true,"Le collage en ligne de commande démarre en arrière-plan");
    // Comparaison visuelle : des fichiers réels donnent des tailles vérifiables aux deux cartes.
    QDir(temp.path()).mkpath("incoming"); QDir(temp.path()).mkpath("existing");
    const auto incomingPath=temp.filePath("incoming/rapport.txt");
    const auto existingPath=temp.filePath("existing/rapport.txt");
    { QFile file(incomingPath);check(file.open(QIODevice::WriteOnly),"Source de comparaison");file.write("nouveau"); }
    { QFile file(existingPath);check(file.open(QIODevice::WriteOnly),"Destination de comparaison");file.write("ancien fichier plus long"); }
    QJsonObject comparisonEntry{{"kind","file"},{"source",incomingPath},{"destination",existingPath},{"state","conflict"},{"size",7},{"copied",0}};
    QJsonObject comparisonJob{{"id",101},{"state","running"},{"mode","copy"},{"sources",QJsonArray{incomingPath}},{"destination",temp.filePath("existing")},{"entries",QJsonArray{comparisonEntry}}};
    fakeStatus["jobs"]=QJsonArray{comparisonJob};window.refresh();window.showNormal();app.processEvents();
    auto *conflictPanel=window.findChild<QFrame *>("conflictPanel");
    auto *selector=window.findChild<QComboBox *>("conflictSelector");
    check(conflictPanel && conflictPanel->isVisible(),"Le panneau de comparaison est visible");
    auto *sourcePathLabel=window.findChild<QLabel *>("conflictSourcePath");
    auto *destinationPathLabel=window.findChild<QLabel *>("conflictDestinationPath");
    check(sourcePathLabel->text()==incomingPath && destinationPathLabel->text()==existingPath,"La source et la destination ont des chemins séparés");
    check(window.findChild<QLabel *>("conflictSourceMetadata")->text().contains(bytes(7)) && window.findChild<QLabel *>("conflictDestinationMetadata")->text().contains(bytes(24)),"Les tailles des deux fichiers sont comparables");
    check(window.findChild<QLabel *>("conflictSourceMetadata")->text().contains("Modifié le"),"La date est affichée dans la comparaison");
    // L’arrivée d’un second conflit ne doit jamais déplacer la cible des boutons à l’insu de l’utilisateur.
    auto secondEntry=comparisonEntry;secondEntry["source"]=temp.filePath("incoming/<autre>.txt");
    auto parentEntry=secondEntry;parentEntry["kind"]="directory";parentEntry["state"]="done";
    auto secondJob=comparisonJob;secondJob["id"]=102;secondJob["mode"]="move";secondJob["entries"]=QJsonArray{parentEntry,secondEntry};
    fakeStatus["jobs"]=QJsonArray{comparisonJob,secondJob};window.refresh();
    check(selector->count()==2 && selector->currentData().toString()=="101/0","Le conflit déjà affiché reste sélectionné");
    QSignalSpy conflictRowsRemoved(selector->model(),&QAbstractItemModel::rowsRemoved);
    window.refresh();window.refresh();
    check(conflictRowsRemoved.isEmpty(),"Le sélecteur des conflits ne se reconstruit pas à chaque mise à jour");
    auto *apply=window.findChild<QCheckBox *>("applyToBatchConflicts");apply->setChecked(true);
    selector->setCurrentIndex(1);window.refresh();
    check(!apply->isChecked() && apply->text().contains("#102"),"Le choix global ne fuit pas vers un autre conflit");
    check(sourcePathLabel->text()==secondEntry["source"].toString() && sourcePathLabel->textFormat()==Qt::PlainText,"Un nom contenant des chevrons reste du texte brut");
    received.clear();QTest::mouseClick(window.findChild<QPushButton *>("resolveConflict_rename"),Qt::LeftButton);
    check(received.size()==1 && received.last()["command"]=="resolve" && received.last()["id"]==102 && received.last()["index"]==1 && received.last()["action"]=="rename" && received.last()["all"]==false,"Garder les deux cible précisément le fichier affiché");
    apply->setChecked(true);received.clear();QTest::mouseClick(window.findChild<QPushButton *>("resolveConflict_replace"),Qt::LeftButton);
    check(received.size()==1 && received.last()["all"]==true && received.last()["id"]==102,"Appliquer aux suivants reste limité au lot affiché");
    filter->setStateFilter("done");
    QTest::mouseClick(window.findChild<QPushButton *>("locateConflict"),Qt::LeftButton);
    check(filter->presentation().state.isEmpty() && tree->currentItem()->data(0,Qt::UserRole).toLongLong()==102 && RowTree::index(tree->currentItem())==1,"Voir le fichier retrouve la ligne même lorsqu’un filtre la cachait");
    if(const auto preview=qEnvironmentVariable("GAD_CONFLICT_PREVIEW");!preview.isEmpty()) {
        selector->setCurrentIndex(0);window.refresh();QTest::mouseClick(window.findChild<QPushButton *>("locateConflict"),Qt::LeftButton);
        window.resize(1150,720);app.processEvents();check(window.grab().save(preview),"Aperçu de la comparaison de conflits");
    }
    // Une résolution extérieure pendant un changement de filtre ne doit pas laisser de pointeur d’item périmé.
    filter->setStateFilter("done");
    fakeStatus["jobs"]=QJsonArray();
    QTest::mouseClick(window.findChild<QPushButton *>("locateConflict"),Qt::LeftButton);
    check(tree->topLevelItemCount()==0 && !conflictPanel->isVisible(),"Le panneau et la navigation tolèrent un conflit résolu entre-temps");
    // La résolution distante ne bloque ni la boucle Qt ni l’ordre d’arrivée des copies suivantes.
    TestKioFuse fuse;fuse.path=temp.filePath("mounted/avec espace#.jpg");
    auto bus=QDBusConnection::sessionBus();
    check(bus.registerService("org.kde.KIOFuse"),"Service KIO Fuse de test");
    check(bus.registerObject("/org/kde/KIOFuse",&fuse,QDBusConnection::ExportAllSlots),"Objet KIO Fuse de test");
    window.hide();received.clear();
    auto preparing=window.handle({{"command","enqueue"},{"sources",QJsonArray{phoneUrl}},{"destination",temp.path()},{"background",true},{"verify_md5",true},{"mode","copy"}});
    auto queuedLocal=window.handle({{"command","enqueue"},{"sources",QJsonArray{"/tmp/local-after-phone"}},{"destination",temp.path()},{"background",true}});
    check(preparing["preparing"]==true && queuedLocal["preparing"]==true && received.isEmpty(),"Les lots attendent la résolution sans la dépasser");
    bool eventDelivered=false;QTimer::singleShot(0,&app,[&]{eventDelivered=true;});
    check(QTest::qWaitFor([&]{return received.size()==2;},std::chrono::milliseconds(2000)),"Les deux demandes sont soumises après la résolution");
    check(eventDelivered && !window.isVisible(),"La résolution reste asynchrone et n’ouvre pas Gad");
    check(received[0]["sources"].toArray()==QJsonArray{fuse.path} && received[0]["source_urls"].toArray()==QJsonArray{phoneUrl} && received[0]["verify_md5"]==true,"Le moteur reçoit le montage, l’origine et les options capturées");
    check(received[1]["sources"].toArray()==QJsonArray{"/tmp/local-after-phone"},"La demande locale conserve sa place après le téléphone");
    const QJsonObject labeledJob{{"sources",QJsonArray{temp.filePath("mounted")}},{"source_urls",QJsonArray{"kdeconnect://phone/android/DCIM"}}};
    check(sourceLabel(labeledJob,fuse.path)=="kdeconnect://phone/android/DCIM/avec espace%23.jpg","Les descendants affichent l’URL d’origine");
    check(sourceLabel(labeledJob,temp.filePath("mounted-other/file"))==temp.filePath("mounted-other/file"),"Le préfixe d’un autre dossier n’est pas pris pour une source du téléphone");
    parsed=parse({"gad","--copy",phoneUrl,"--to",temp.path()},parseError);
    check(parsed["sources"].toArray()==QJsonArray{phoneUrl},"La CLI conserve l’URL du téléphone");
    check(QTest::qWaitFor([&]{return window.statusBar()->isHidden();},std::chrono::milliseconds(2000)),"L’état de préparation est retiré après la dernière demande");

    // Une reconnexion réutilise KIO en arrière-plan et n’envoie aucune nouvelle demande de copie.
    const auto waitingJob=QJsonObject{{"id",77},{"sources",QJsonArray{fuse.path}},{"source_urls",QJsonArray{phoneUrl}},{"destination",temp.path()},{"state","device_waiting"},{"entries",QJsonArray{}}};
    fakeStatus["jobs"]=QJsonArray{waitingJob};received.clear();const int reconnectCalls=fuse.calls.size();
    window.refresh();
    check(QTest::qWaitFor([&]{return fuse.calls.size()>reconnectCalls;},std::chrono::milliseconds(2000)),"Le téléphone est résolu à nouveau sans bloquer Qt");
    QTest::qWait(80);check(received.isEmpty() && !window.isVisible(),"La reconnexion ne crée pas de lot et ne relève pas Gad");
    window.refresh();check(fuse.calls.size()==reconnectCalls+1,"Les sondes KIO sont espacées");
    fakeStatus["jobs"]=QJsonArray{};window.refresh();

    // FTP, SFTP et SMB utilisent le même pont que KDE Connect ; aucune connexion réelle n’est requise ici.
    for(const auto &scheme:QStringList{"ftp","sftp","smb"}) {
        const auto networkUrl=scheme+"://alice@files.example:2222/dossier/rapport%20final%23.txt";
        formats={{"text/uri-list",networkUrl.toUtf8()+"\n"}};
        auto networkFiles=Clipboard::decode(formats.keys(),read);
        check(networkFiles.error.isEmpty() && networkFiles.sources==QStringList{networkUrl},"FTP/SFTP/SMB sont acceptés dans le presse-papiers");
        parsed=parse({"gad","--copy",networkUrl,"--to",temp.path()},parseError);
        check(parsed["sources"].toArray()==QJsonArray{networkUrl} && SourceUrls::requiredProtocol(parsed)==(scheme=="smb"?9:8),"La CLI conserve hôte, utilisateur, port et nom encodé");
        const int callsBefore=fuse.calls.size();received.clear();
        window.handle({{"command","enqueue"},{"sources",QJsonArray{networkUrl}},{"destination",temp.path()},{"background",true},{"mode","copy"}});
        check(QTest::qWaitFor([&]{return received.size()==1;},std::chrono::milliseconds(2000)),"La source réseau est soumise après résolution");
        check(fuse.calls.size()==callsBefore+1 && fuse.calls.last()==networkUrl && received[0]["sources"].toArray()==QJsonArray{fuse.path},"KIO Fuse reçoit l’adresse réseau exacte et fournit le chemin au moteur");
        check(received[0]["source_urls"].toArray()==QJsonArray{networkUrl},"L’origine distante reste dans le lot");
        const QJsonObject networkJob{{"sources",QJsonArray{temp.filePath("mounted")}},{"source_urls",QJsonArray{scheme+"://alice@files.example:2222/dossier"}}};
        check(sourceLabel(networkJob,fuse.path)==scheme+"://alice@files.example:2222/dossier/avec espace%23.jpg","L’affichage réseau masque le montage interne");
        check(QTest::qWaitFor([&]{return window.statusBar()->isHidden();},std::chrono::milliseconds(2000)),"La préparation réseau est terminée");
    }
    // Les destinations utilisent le même adaptateur, après les sources et avant toute soumission.
    for(const auto &scheme:QStringList{"ftp","sftp","smb","kdeconnect"}) {
        const auto address=scheme+"://server/destination%20avec%23";
        parsed=parse({"gad","--copy","/tmp/local","--to",address},parseError);
        check(parseError.isEmpty() && parsed["destination"]==address && SourceUrls::requiredProtocol(parsed)==12,"Une destination distante exige une instance compatible");
        received.clear();const int before=fuse.calls.size();
        window.handle({{"command","enqueue"},{"sources",QJsonArray{"/tmp/local"}},{"destination",address},{"background",true},{"verify_md5",true}});
        check(QTest::qWaitFor([&]{return received.size()==1;},std::chrono::milliseconds(2000)),"La destination est résolue avant la soumission");
        check(fuse.calls.size()==before+1 && fuse.calls.last()==address && received[0]["destination"]==fuse.path && received[0]["destination_url"]==address,"La destination conserve son origine et utilise le montage pour écrire");
        check(received[0]["sources"].toArray()==QJsonArray{"/tmp/local"} && received[0]["verify_md5"]==true && !window.isVisible(),"Le lot garde ses options et reste discret");
        QJsonObject mapping{{"destination",temp.filePath("mounted")},{"destination_url",address}};
        check(destinationLabel(mapping,fuse.path)==scheme+"://server/destination avec%23/avec espace%23.jpg","Les descendants de destination montrent leur adresse distante");
        check(QTest::qWaitFor([&]{return window.statusBar()->isHidden();},std::chrono::milliseconds(2000)),"La préparation de destination est terminée");
    }
    // Résoudre les deux côtés reste atomique et ne laisse pas de mot de passe dans les métadonnées.
    received.clear();const int twoSides=fuse.calls.size();
    window.handle({{"command","enqueue"},{"sources",QJsonArray{"sftp://source/data"}},{"destination","ftp://alice:secret@server/out"},{"background",true}});
    check(QTest::qWaitFor([&]{return received.size()==1;},std::chrono::milliseconds(2000)),"Les deux côtés distants sont résolus");
    check(fuse.calls.mid(twoSides)==QStringList{"sftp://source/data","ftp://alice:secret@server/out"} && received[0]["destination_url"]=="ftp://alice@server/out","Les montages suivent l’ordre et les mots de passe restent chez KIO");
    check(!QJsonDocument(received[0]).toJson().contains("secret"),"Le moteur ne reçoit aucun mot de passe de destination");
    check(QTest::qWaitFor([&]{return window.statusBar()->isHidden();},std::chrono::milliseconds(2000)),"La préparation des deux côtés est terminée");
    // Les URL invalides sont toutes rejetées avant le premier montage, même si les sources sont valides.
    QJsonObject badDestination;bool badDestinationDone=false;const int noCalls=fuse.calls.size();
    auto *badTarget=new SourceUrls::Resolver({{"sources",QJsonArray{phoneUrl}},{"destination","https://server/out"}},&app,[&](QJsonObject result){badDestination=result;badDestinationDone=true;});
    badTarget->start();
    check(badDestinationDone && badDestination.contains("error") && fuse.calls.size()==noCalls,"Une destination invalide n’engage aucun montage");

    // Le contexte Dolphin distingue le dossier lui-même du parent d’un fichier distant.
    const auto savedMountPath=fuse.path;
    for(const bool folder:{false,true}) {
        const auto contextUrl=folder?QString("sftp://server/dir"):QString("sftp://server/dir/selected.txt");
        fuse.path=folder?temp.path():temp.filePath("selected.txt");
        if(!folder){QFile file(fuse.path);check(file.open(QIODevice::WriteOnly),"Fichier de contexte privé");file.write("test");}
        parsed=parse({"gad","--paste-context",contextUrl},parseError);
        check(parseError.isEmpty() && parsed["destination_context"].toArray()==QJsonArray{contextUrl},"Le contexte distant attend la résolution pour connaître le type");
        received.clear();
        window.handle({{"command","enqueue"},{"sources",QJsonArray{"/tmp/local"}},{"destination",contextUrl},{"destination_context",QJsonArray{contextUrl}},{"background",true}});
        check(QTest::qWaitFor([&]{return received.size()==1;},std::chrono::milliseconds(2000)),"Le contexte distant est résolu");
        check(received[0]["destination"]==temp.path() && received[0]["destination_url"]=="sftp://server/dir" && !received[0].contains("destination_context"),"Dolphin colle dans le bon dossier et le moteur ne reçoit que le chemin résolu");
        check(QTest::qWaitFor([&]{return window.statusBar()->isHidden();},std::chrono::milliseconds(2000)),"Le contexte distant est terminé");
    }
    fuse.path=savedMountPath;
    // Une panne de destination ne soumet aucun lot et ne bloque pas les suivants.
    fuse.fail=true;received.clear();
    window.handle({{"command","enqueue"},{"sources",QJsonArray{"/tmp/local"}},{"destination","sftp://server/unavailable"},{"background",true}});
    check(QTest::qWaitFor([&]{return window.statusBar()->isHidden();},std::chrono::milliseconds(2000)) && received.isEmpty(),"Une erreur de destination ne lance aucun transfert");
    fuse.fail=false;
    received.clear();
    window.handle({{"command","enqueue"},{"sources",QJsonArray{"/tmp/local"}},{"destination","smb://server/share"},{"background",true}});
    window.findChild<QAction *>("cancelAllAction")->trigger();
    QTest::qWait(100);
    check(received.size()==1 && received[0]["command"]=="cancel_all","Tout annuler ignore également les réponses tardives des destinations");

    // L’annulation globale couvre aussi les lots préparés hors du moteur et leurs suivants locaux.
    for(const auto &actionName:QStringList{"cancelAllAction","undoAllAction"}) {
        received.clear();
        window.handle({{"command","enqueue"},{"sources",QJsonArray{phoneUrl}},{"destination",temp.path()},{"background",true}});
        window.handle({{"command","enqueue"},{"sources",QJsonArray{"/tmp/queued-after-remote"}},{"destination",temp.path()},{"background",true}});
        window.findChild<QAction *>(actionName)->trigger();
        QTest::qWait(100);
        check(received.size()==1 && received[0]["command"]==(actionName=="cancelAllAction"?"cancel_all":"undo_all"),"Tout annuler retire aussi les préparations et ignore les réponses KIO tardives");
        check(window.statusBar()->isHidden(),"L’annulation retire l’indicateur de préparation");
        received.clear();
        window.handle({{"command","enqueue"},{"sources",QJsonArray{phoneUrl}},{"destination",temp.path()},{"background",true}});
        check(QTest::qWaitFor([&]{return received.size()==1;},std::chrono::milliseconds(2000)),"Un nouveau lot fonctionne après annulation de la préparation");
        check(QTest::qWaitFor([&]{return window.statusBar()->isHidden();},std::chrono::milliseconds(2000)),"La nouvelle préparation est terminée");
    }
    // Une URL fournie avec un mot de passe est transmise intacte à KIO, mais ne le laisse pas dans le journal.
    const QString credentialUrl="sftp://alice:secret%40pass@files.example:2222/data/file.txt";
    QJsonObject credentialResult;bool credentialDone=false;
    auto *credentialResolver=new SourceUrls::Resolver({{"command","enqueue"},{"sources",QJsonArray{credentialUrl}}},&app,[&](QJsonObject value){credentialResult=value;credentialDone=true;});
    credentialResolver->start();
    check(QTest::qWaitFor([&]{return credentialDone;},std::chrono::milliseconds(2000)),"Résolution déléguée à KIO");
    check(fuse.calls.last()==credentialUrl && credentialResult["source_urls"].toArray()==QJsonArray{"sftp://alice@files.example:2222/data/file.txt"},"Les identifiants restent gérés par KIO et le mot de passe ne devient pas un libellé persistant");

    // Une adresse non supportée ou une erreur de connexion invalide toute la demande avant copie.
    QJsonObject failedResolution;bool resolutionFinished=false;const int beforeInvalid=fuse.calls.size();
    auto *invalidResolver=new SourceUrls::Resolver({{"command","enqueue"},{"sources",QJsonArray{phoneUrl,"https://server/file"}}},&app,[&](QJsonObject value){failedResolution=value;resolutionFinished=true;});
    invalidResolver->start();
    check(resolutionFinished && failedResolution.contains("error") && fuse.calls.size()==beforeInvalid,"La validation complète précède les montages");
    fuse.fail=true;resolutionFinished=false;
    auto *failedResolver=new SourceUrls::Resolver({{"command","enqueue"},{"sources",QJsonArray{"/tmp/local",phoneUrl}}},&app,[&](QJsonObject value){failedResolution=value;resolutionFinished=true;});
    failedResolver->start();
    check(QTest::qWaitFor([&]{return resolutionFinished;},std::chrono::milliseconds(2000)) && failedResolution["error"].toString().contains("KIO Fuse"),"L’erreur distante est explicite et ne soumet pas une copie partielle");

    // Les ticks inchangés ne réécrivent pas les lignes ; un changement de filtre ou de pause reste visible.
    {
        auto incrementalJob=job;auto incrementalEntry=entry;incrementalEntry["state"]="running";
        incrementalJob["state"]="running";incrementalJob["entries"]=QJsonArray{incrementalEntry};
        fakePoll={{"revision",1},{"reset",true},{"job_ids",QJsonArray{42}},{"jobs",QJsonArray{incrementalJob}},{"paused",false}};
        Window cached;auto *cachedTree=cached.findChild<QTreeWidget *>();
        auto *cachedParent=cachedTree->topLevelItem(0);auto *cachedRow=cached.entryRow(42,0);
        QSignalSpy changes(cachedTree->model(),&QAbstractItemModel::dataChanged);
        fakePoll={{"revision",1},{"unchanged",true}};cached.refresh();
        check(changes.isEmpty() && cachedParent==cachedTree->topLevelItem(0),"Un tick inchangé conserve les lignes sans signaux de modification");
        auto *cachedFilter=static_cast<TransferTree *>(cached.findChild<QTreeWidget *>());cachedFilter->setStateFilter("done");
        check(cachedParent->isHidden(),"Le filtre est actualisé même sans mutation du moteur");
        cachedFilter->clearColumnFilter(2);check(!cachedParent->isHidden(),"Le filtre Tous réaffiche le lot inchangé");
        fakePoll={{"revision",2},{"job_ids",QJsonArray{42}},{"jobs",QJsonArray{}},{"paused",true}};cached.refresh();
        check(cachedRow->text(2)=="En pause","La pause globale actualise les lignes conservées dans le cache");
        // Le menu déjà ouvert doit suivre le changement d’état sans nouvelle sélection.
        cachedTree->setCurrentItem(cachedRow);cachedRow->setSelected(true);cached.updateRowActions();
        auto *cachedRetry=cached.findChild<QAction *>("rowRetry");check(!cachedRetry->isEnabled(),"Pas de réactivation sur une ligne active");
        incrementalEntry["state"]="cancelled";incrementalJob["state"]="cancelled";incrementalJob["entries"]=QJsonArray{incrementalEntry};
        fakePoll={{"revision",3},{"job_ids",QJsonArray{42}},{"jobs",QJsonArray{incrementalJob}},{"paused",true}};cached.refresh();
        check(cachedRetry->isEnabled(),"Le menu suit automatiquement le passage en annulé sans reselection");
        incrementalJob["state"]="running";
        fakePoll={{"revision",4},{"job_ids",QJsonArray{42}},{"jobs",QJsonArray{incrementalJob}},{"paused",true}};cached.refresh();
        check(!cachedRetry->isEnabled(),"La réactivation attend la fin du lot contenant la ligne annulée");
        fakePoll={{"revision",5},{"job_ids",QJsonArray{}},{"jobs",QJsonArray{}},{"paused",true}};cached.refresh();
        check(cachedTree->topLevelItemCount()==0,"Les lots retirés disparaissent aussi du cache visuel");
        fakePoll=QJsonObject{};
    }
    // Une erreur doit être rendue au client sans attendre l’acquittement du message.
    bool errorDismissed=false;QTimer dismissError;dismissError.setSingleShot(true);
    QObject::connect(&dismissError,&QTimer::timeout,&window,[&]{errorDismissed=true;for(auto *dialog:window.findChildren<QMessageBox *>())dialog->accept();});
    dismissError.start(100);const auto errorResult=window.error("Erreur de test <fichier>");
    check(!errorDismissed && errorResult["error"]=="Erreur de test <fichier>","Le message d’erreur ne bloque pas la réponse IPC");dismissError.stop();
    auto *errorDialog=window.findChild<QMessageBox *>("errorDialog");
    check(errorDialog && errorDialog->textFormat()==Qt::PlainText,"Les noms de fichiers restent du texte dans le message d’erreur");errorDialog->close();
    // Le manifeste du protocole ne doit pas masquer la version de compilation de l’application.
    window.showAbout();auto *aboutDialog=window.findChild<QMessageBox *>("aboutDialog");
    check(aboutDialog && aboutDialog->informativeText().contains("test-version"),"La version de compilation reste affichée dans À propos");
    aboutDialog->close();
    // L’option crée puis retire son entrée dans le répertoire XDG temporaire du test.
    auto *startup=window.findChild<QCheckBox *>("launchAtStartup");
    check(startup && !startup->isChecked(),"Le démarrage automatique est désactivé par défaut");
    startup->setChecked(true);
    check(autostartEnabled(),"Activation du démarrage de session");
    QFile startupFile(autostartPath());check(startupFile.open(QIODevice::ReadOnly),"Lecture de l’entrée de démarrage");
    check(startupFile.readAll().contains("--background"),"Le démarrage automatique masque la fenêtre");startupFile.close();
    startup->setChecked(false);check(!QFileInfo::exists(autostartPath()),"Désactivation du démarrage de session");
    QString startupError;
    check(parse({"gad","--background"},startupError)["command"]=="background","Commande de démarrage discret");
    check(parse({"gad","gad://show"},startupError)["command"]=="show","Le lien de progression relève Gad");
    window.hide();window.handle({{"command","background"}});check(!window.isVisible(),"Le démarrage discret ne relève pas l’instance");
    window.handle(parse({"gad","gad://show"},startupError));check(window.isVisible(),"Le lien affiche une fenêtre masquée");

    // La sortie explicite doit terminer la boucle Qt même si la fenêtre est masquée.
    QAction *quit = nullptr;
    for (auto *action : tray->contextMenu()->actions()) if (action->text() == "Quitter Gad") quit = action;
    check(quit != nullptr, "Action Quitter disponible");
    window.hide();
    QTimer::singleShot(0, quit, &QAction::trigger);
    QTimer::singleShot(1500, &app, [&] { app.exit(9); });
    check(app.exec() == 0, "Quitter fonctionne aussi lorsque la fenêtre est masquée");
    std::puts("OK : presse-papiers, survol stable, compteur de notification, pause, conflit et réouverture depuis l’icône.");
}

#include "clipboard_test.moc"
