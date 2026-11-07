#pragma once
// Le chemin des plugins ne change que dans le processus Dolphin enfant.
#include <QtCore>
#include <cstdio>
namespace DolphinLauncher {
inline QString pluginRoot() {
    QStringList roots;
    for(const auto &data:QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation))roots.append(data+"/gad/dolphin/plugins");
    roots.append(QCoreApplication::applicationDirPath()+"/../lib/gad/plugins");
    if(QStringList{"debug","release"}.contains(QFileInfo(QCoreApplication::applicationDirPath()).fileName()))roots.append(QCoreApplication::applicationDirPath()+"/../dolphin-plugin/plugins");
    for(const auto &root:roots)if(QFileInfo::exists(root+"/generic/libgad_dolphin_watchdog.so"))return QDir(root).absolutePath();
    return {};
}
inline int launch(const QStringList &arguments) {
    const auto program=QStandardPaths::findExecutable("dolphin"),root=pluginRoot();
    if(program.isEmpty() || root.isEmpty()){std::fprintf(stderr,"Dolphin or its Gad watchdog plugin is unavailable. Reinstall Gad with Dolphin integration.\n");return 1;}
    QProcess child;child.setProgram(program);child.setArguments(QStringList{"-plugin","gad-dolphin-watchdog"}+arguments);
    auto environment=QProcessEnvironment::systemEnvironment();environment.insert("GAD_DOLPHIN_EXECUTABLE",QCoreApplication::applicationFilePath());const auto existing=environment.value("QT_PLUGIN_PATH");environment.insert("QT_PLUGIN_PATH",root+(existing.isEmpty()?QString():":"+existing));child.setProcessEnvironment(environment);
    child.setStandardInputFile(QProcess::nullDevice());child.setStandardOutputFile(QProcess::nullDevice());child.setStandardErrorFile(QProcess::nullDevice());
    return child.startDetached()?0:1;
}
}
