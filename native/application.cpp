#include "i18n.h"
#include "protocol.h"
// Implémentation de la fenêtre Gad, séparée par responsabilité.
#include "window.h"
#include "bridge.h"
#ifndef GAD_VERSION
#define GAD_VERSION "développement"
#endif
#ifndef GAD_BUILD_DATE
#define GAD_BUILD_DATE ""
#endif

#include "application.h"
#include "cli_options.h"
#include "dolphin_launcher.h"
#include "paths.h"
#include "dolphin_destination.h"
#include <memory>
#include <algorithm>
#include <cstdio>
#include <unistd.h>


/// Convertit les arguments CLI en requête JSON, notamment la destination déduite du contexte Dolphin.
QJsonObject parse(const QStringList &args, QString &error) {
    error.clear();auto filtered=args;const auto profile=CliOptions::takeProfile(filtered,error);if(!error.isEmpty())return {};
    if(!profile.isNull()) {
        auto request=(filtered.size()==1 || (filtered.size()==2 && filtered[1]=="--configure"))?QJsonObject{{"command",Protocol::Command::Configure}}:parse(filtered,error);
        if(!error.isEmpty() || !CliOptions::attachProfile(request,profile,error))return {};
        return request;
    }
    if (args.size() == 1) return {{"command", Protocol::Command::Show}};
    const auto option = args[1];
    if(option=="--configure")return CliOptions::configure(args,error);
    // Le lien de notification n’accepte aucune commande ni chemin arbitraire.
    if ((option == "gad://show" || option == "gad://show/") && args.size()==2) return {{"command",Protocol::Command::Show}};
    if (option == "--background" && args.size()==2) return {{"command",Protocol::Command::Background}};
    if (option == "--status" && args.size() == 2) return {{"command", Protocol::Command::Status}};
    // KIO développe %U : ignorer toute ancienne sélection et lire uniquement la fenêtre active.
    if((option=="--paste-dolphin" || option=="--paste-current") && args.size()>=2)return {{"command",Protocol::Command::Paste},{"dolphin_current",true},{"background",true}};
    if (option == "--paste-context" && args.size() >= 3) {
        QStringList context;
        for(int i=2;i<args.size();++i){context.append(LocalPaths::destination(args[i],error));if(!error.isEmpty())return {};}
        // Le type des éléments distants ne peut être connu qu’après le montage asynchrone.
        if(std::any_of(context.cbegin(),context.cend(),[](const QString &value){return SourceUrls::isUrl(value);})) {
            QJsonArray items;for(const auto &item:context)items.append(item);
            return {{"command",Protocol::Command::Paste},{"destination",context[0]},{"destination_context",items},{"background",true}};
        }
        QString path;
        if (context.size()==1 && QFileInfo(context[0]).isDir()) path=context[0];
        else {
            for (const auto &item:context) {
                const auto parent=QFileInfo(item).absolutePath();
                if (path.isEmpty()) path=parent;
                else if (parent!=path) {error=GAD_TR("The selection does not share a single destination folder");return {};}
            }
        }
        return {{"command",Protocol::Command::Paste},{"destination",path},{"background",true}};
    }
    if (option == "--paste" && args.size() == 3) {
        const auto path=LocalPaths::destination(args[2],error);if(!error.isEmpty())return {};
        return {{"command",Protocol::Command::Paste},{"destination",path},{"background",true}};
    }
    if (option == "--copy" || option == "--move") {
        int separator = args.indexOf("--to", 2);
        if (separator >= 3 && separator == args.size() - 2) {
            const auto path=LocalPaths::destination(args.last(),error);if(!error.isEmpty())return {};
            QJsonArray sources; for (int i = 2; i < separator; ++i) sources.append(SourceUrls::argument(args[i]));
            return {{"command", Protocol::Command::Enqueue}, {"sources", sources}, {"destination", path}, {"mode", option == "--move" ? "move" : "copy"}};
        }
    }
    error = GAD_TR("Usage: gad [--background | --paste FOLDER | --copy SOURCE… --to FOLDER | --move SOURCE… --to FOLDER | --status | --smoke-test]");
    return {};
}

/// Démarre Qt, transmet la commande à l’instance existante ou crée le serveur local propriétaire.
/// Les modes de test isolent leur journal et leur socket pour ne pas atteindre l’instance du bureau.
extern "C" int sc_gui(int argc, char **argv) {
    // Vérifier les octets avant Qt : une conversion avec remplacement pourrait sélectionner un autre fichier existant.
    for(int i=1;i<argc;++i) {
        const QByteArray argument(argv[i]);
        if(QString::fromUtf8(argument).toUtf8()!=argument) {
            std::fprintf(stderr,"Argument non UTF-8 : nom de fichier non pris en charge.\n");return 2;
        }
    }
    QApplication app(argc, argv);
    app.setApplicationName("Gad"); app.setOrganizationName("Gad"); app.setDesktopFileName("org.hizoka.gad");
    app.setApplicationVersion(QString::fromUtf8(GAD_VERSION));
    app.setWindowIcon(applicationIcon());
    I18n::initialize();
    auto args = app.arguments();
    if(args.size()>1 && args[1]=="--dolphin")return DolphinLauncher::launch(args.mid(2));
    QString profileError;const auto profile=CliOptions::takeProfile(args,profileError);
    if(!profileError.isEmpty()){std::fprintf(stderr,"%s\n",qPrintable(profileError));return 2;}
    const bool configuring=args.size()>1 && args[1]=="--configure";
    const bool explicitMd5 = !configuring && args.removeAll("--md5") > 0;
    const bool explicitProperties=!configuring && args.removeAll("--properties")>0;
    if (args.size() == 3 && args[1] == "--export-icons") {if(!profile.isNull())return 2;return exportIcons(args[2]);}
    // L’aide est une commande : un opérande -h peut être un nom ou un motif valide.
    if (args.size()==2 && (args[1]=="--help" || args[1]=="-h")) {
        std::puts(qPrintable(GAD_TR("Gad copies. — File transfer manager for KDE\n  gad\n  gad --background\n  gad --paste FOLDER\n  gad --copy SOURCE… --to FOLDER\n  gad --move SOURCE… --to FOLDER\n  gad --status\n  gad --smoke-test\n  Add --md5 to verify file contents.")));
        std::puts("  gad --paste-current\n  gad --paste-context URL...\n  gad --dolphin [URL...]\n  gad --profile NAME [--copy SOURCE... --to FOLDER | --move SOURCE... --to FOLDER | --paste FOLDER]\n  gad --configure [--md5|--no-md5] [--properties|--no-properties]\n    [--verify-fast-move|--no-verify-fast-move] [--verify-space|--no-verify-space] [--keep-history|--no-keep-history]\n    [--progress-message|--no-progress-message]\n    [--resume-interrupted|--no-resume-interrupted] [--include-hidden|--no-include-hidden]\n    [--allow-duplication|--no-allow-duplication] [--autostart|--no-autostart]\n    [--dolphin-ctrl-v|--no-dolphin-ctrl-v]\n    [--krusader-ctrl-v|--no-krusader-ctrl-v]\n    [--konqueror-ctrl-v|--no-konqueror-ctrl-v] [--language LOCALE]\n    [--rate-limit MIB_PER_SECOND] [--request-rate-limit MIB_PER_SECOND] [--exclude-folder PATTERN] [--exclude-file PATTERN]\n    [--completion-action none|notify|clear|quit|suspend|poweroff] [--completion-delay SECONDS] [--clear-exclusions] [--conflict-policy ask|skip|newer|rename]");
        std::printf("  --properties: %s\n",qPrintable(GAD_TR("Verify properties")));return 0;
    }
    const bool smoke = args.size() == 2 && args[1] == "--smoke-test";
    std::unique_ptr<QTemporaryDir> smokeStorage;
    if (smoke) {
        smokeStorage = std::make_unique<QTemporaryDir>(QDir::tempPath()+"/gad-smoke-XXXXXX");
        if (!smokeStorage->isValid()) return 1;
        qputenv("XDG_DATA_HOME", QFile::encodeName(smokeStorage->path()+"/data"));
        qputenv("XDG_CONFIG_HOME", QFile::encodeName(smokeStorage->path()+"/config"));
    }
    QString error;
    QJsonObject initial = smoke ? QJsonObject{{"command", Protocol::Command::Show}} :
        (!profile.isNull() && (args.size()==1 || (args.size()==2 && args[1]=="--configure")))?QJsonObject{{"command",Protocol::Command::Configure}}:parse(args,error);
    if(!profile.isNull())CliOptions::attachProfile(initial,profile,error);
    if (!error.isEmpty()) { std::fprintf(stderr, "%s\n", qPrintable(error)); return 2; }
    if(initial.take("dolphin_current").toBool()) {
        const auto target=DolphinDestination::current();
        if(!target.error.isEmpty()){std::fprintf(stderr,"%s\n",qPrintable(target.error));QMessageBox::warning(nullptr,"Gad",target.error);return 1;}
        initial["destination"]=target.destination;
    }
    if (initial["command"] == Protocol::Command::Paste) {
        const auto files = Clipboard::current();
        if (!files.error.isEmpty()) {
            std::fprintf(stderr, "%s\n", qPrintable(files.error));
            QMessageBox::warning(nullptr, "Gad", files.error);
            return 1;
        }
        const auto context=initial.value("destination_context");const auto selectedProfile=initial.value("profile");
        initial = Clipboard::request(files, initial["destination"].toString());
        if(context.isArray())initial["destination_context"]=context;
        if(selectedProfile.isString())initial["profile"]=selectedProfile;
        initial["background"] = true;
    }
    // Transmet le jeton reçu par le lanceur KDE à l’instance déjà ouverte sous Wayland.
    if(initial["command"]== Protocol::Command::Show)initial["activation_token"]=qEnvironmentVariable("XDG_ACTIVATION_TOKEN");
    if (explicitMd5) initial["verify_md5"] = true;
    if(explicitProperties)initial["verify_properties"]=true;
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtime.isEmpty()) { std::fprintf(stderr, "%s", qPrintable(GAD_TR("XDG_RUNTIME_DIR not found\n"))); return 1; }
    const QString name = runtime + "/gad" + (smoke ? "-smoke-" + QString::number(getpid()) : "") + ".sock";
    // Un client transmet sa requête à l’instance existante ; -1 signifie qu’aucun serveur n’a répondu.
    auto forward = [&]() -> int {
        QLocalSocket client;
        QString endpoint=name;
        client.connectToServer(endpoint);
        if (!client.waitForConnected(200))return -1;
        if (initial.contains("profile") || initial["command"]==Protocol::Command::Configure || initial["verify_space"].toBool() || initial["mode"] == "move" || initial["verify_md5"].toBool() || initial["verify_properties"].toBool() || SourceUrls::needsResolution(initial)) {
            QLocalSocket probe; probe.connectToServer(endpoint);
            if (!probe.waitForConnected(500)) return 1;
            probe.write("{\"command\":\"status\",\"summary\":true}\n"); probe.waitForBytesWritten(1000);
            QByteArray status;
            QElapsedTimer probeTimeout; probeTimeout.start();
            while (!status.contains('\n') && probeTimeout.elapsed() < 3000) {
                if (probe.bytesAvailable() || probe.waitForReadyRead(200)) status += probe.readAll();
                else if(probe.state()==QLocalSocket::UnconnectedState)break;
            }
            // Une sonde tronquée ou invalide n’identifie aucune version : échouer sans dialogue de compatibilité.
            QJsonParseError probeError;const auto end=status.indexOf('\n');
            const auto response=QJsonDocument::fromJson(status.left(end),&probeError);
            const auto version=response.object().value("protocol_version");
            const int availableVersion=version.toInt(-1);
            if(end<0 || probeError.error!=QJsonParseError::NoError || !response.isObject() || !version.isDouble() || availableVersion<1) {
                std::fprintf(stderr,"%s",qPrintable(GAD_TR("Gad is not responding\n")));return 1;
            }
            const int requiredVersion = initial["command"]==Protocol::Command::Configure?Protocol::Command::Version:qMax(SourceUrls::requiredProtocol(initial),initial["verify_md5"].toBool() ? 3 : 2);
            if (availableVersion < requiredVersion) {
                const QString message = GAD_TR("An older version of Gad is still running. Let its transfers finish, then quit and restart Gad to enable this feature. No transfer has been added.");
                std::fprintf(stderr, "%s\n", qPrintable(message));
                QMessageBox::warning(nullptr, "Gad", message);
                return 1;
            }
        }
        client.write(QJsonDocument(initial).toJson(QJsonDocument::Compact) + '\n');
        if (!client.waitForBytesWritten(3000)) return 1;
        QByteArray reply;
        QElapsedTimer timeout; timeout.start();
        // Les intégrations et restaurations disposent de 60 s ; la résolution KIO a son propre délai de 30 s.
        const int responseTimeout=initial.contains("profile") || initial["command"]==Protocol::Command::Configure?60000:
            SourceUrls::needsResolution(initial)?35000:10000;
        while (!reply.contains('\n') && timeout.elapsed() < responseTimeout) {
            if (client.bytesAvailable() || client.waitForReadyRead(500)) reply += client.readAll();
            else if (client.state() == QLocalSocket::UnconnectedState) break;
        }
        // Une déconnexion ou un délai peut laisser des octets sans réponse complète et valide.
        QJsonParseError replyError;
        const auto newline=reply.indexOf('\n');
        const auto response=QJsonDocument::fromJson(reply.left(newline),&replyError);
        if (newline<0 || replyError.error!=QJsonParseError::NoError || !response.isObject()) {
            std::fprintf(stderr, "%s", qPrintable(GAD_TR("Gad is not responding\n"))); return 1;
        }
        std::fputs(reply.left(newline+1).constData(), stdout);
        return response.object().contains("error") ? 1 : 0;
    };
    // Le client --status ne doit jamais créer un second moteur si aucun serveur n’est disponible.
    const int forwarded = forward(); if (forwarded >= 0) return forwarded;
    if (initial["command"] == Protocol::Command::Status) { std::fprintf(stderr, "%s", qPrintable(GAD_TR("Gad is not running\n"))); return 1; }
    // Une configuration lancée depuis un terminal ne garde pas ce terminal occupé.
    // Le processus détaché prend le verrou normal, puis reçoit la même requête validée.
    if(initial["command"]==Protocol::Command::Configure || initial.contains("profile")) {
        QProcess launcher;launcher.setProgram(QCoreApplication::applicationFilePath());launcher.setArguments({"--background"});
        launcher.setStandardInputFile(QProcess::nullDevice());launcher.setStandardOutputFile(QProcess::nullDevice());launcher.setStandardErrorFile(QProcess::nullDevice());
        if(!launcher.startDetached())return 1;
        for(int i=0;i<50;++i){QThread::msleep(100);const auto result=forward();if(result>=0)return result;}
        std::fprintf(stderr,"%s",qPrintable(GAD_TR("Gad is not responding\n")));return 1;
    }
    // Le verrou tranche une course entre deux lancements qui n’auraient pas encore trouvé de socket.
    QLockFile lock(runtime + "/gad" + (smoke ? "-smoke-" + QString::number(getpid()) : "") + ".lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock()) {
        for (int i = 0; i < 15; ++i) { QThread::msleep(200); const int result = forward(); if (result >= 0) return result; }
        std::fprintf(stderr, "%s", qPrintable(GAD_TR("An instance is starting or not responding\n"))); return 1;
    }
    QLocalServer::removeServer(name);
    QLocalServer server; server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!server.listen(name)) { std::fprintf(stderr, "%s\n", qPrintable(server.errorString())); return 1; }
    Window window(initial["command"]!=Protocol::Command::Show);
    // Chaque connexion transporte une ligne JSON, avec une limite de taille avant décodage.
    QObject::connect(&server, &QLocalServer::newConnection, &window, [&] {
        while (auto *socket = server.nextPendingConnection()) {
            auto *buffer = new QByteArray;
            QObject::connect(socket, &QObject::destroyed, [buffer] { delete buffer; });
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QLocalSocket::readyRead, &window, [socket, buffer, &window] {
                if(socket->property("handling").toBool())return;
                buffer->append(socket->readAll());
                if (buffer->size() > 16 * 1024 * 1024) { socket->abort(); return; }
                if (!buffer->contains('\n')) return;
                socket->setProperty("handling",true);
                const auto doc = QJsonDocument::fromJson(buffer->left(buffer->indexOf('\n')));
                const QPointer<QLocalSocket> guarded(socket);
                const auto respond=[guarded](QJsonObject result){if(!guarded)return;guarded->write(QJsonDocument(result).toJson(QJsonDocument::Compact)+'\n');guarded->flush();guarded->disconnectFromServer();};
                if(doc.isObject())window.handleAsync(doc.object(),respond);else respond({{"error",GAD_TR("Invalid request")}});
            });
        }
    });
    // Le traitement après démarrage de la boucle Qt évite de montrer une fenêtre pour un collage caché.
    QTimer::singleShot(0, &window, [&] { window.handleAsync(initial,[](QJsonObject){}); });
    if (smoke) QTimer::singleShot(700, &app, [&] {
        const auto screenshot = qEnvironmentVariable("GAD_SCREENSHOT");
        if (!screenshot.isEmpty()) window.grab().save(screenshot);
        app.quit();
    });
    return app.exec();
}
