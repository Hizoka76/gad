#pragma once
// Options persistantes : uniquement avec --configure, pour séparer préférences et transferts.
#include "protocol.h"
#include <QtCore>
#include <cmath>

namespace CliOptions {
/// Extraire avant les autres drapeaux protège aussi un nom comme --md5 avec --profile=--md5.
inline QString takeProfile(QStringList &args,QString &error) {
    error.clear();QString profile;
    for(int i=1;i<args.size();) {
        const auto argument=args[i];if(argument!="--profile" && !argument.startsWith("--profile=")){++i;continue;}
        if(!profile.isNull()){error="Repeated option: --profile";return {};}
        QString name;
        if(argument=="--profile") {
            if(i+1>=args.size() || args[i+1].startsWith("--")){error="Missing value for --profile";return {};}
            name=args.takeAt(i+1);args.removeAt(i);
        } else {name=argument.mid(QString("--profile=").size());args.removeAt(i);}
        name=name.trimmed();if(name.isEmpty() || name.size()>100 || name.contains(QChar(0)) || name.contains('\n') || name.contains('\r')){error="Invalid profile name (1 to 100 characters)";return {};}
        profile=name;
    }
    return profile;
}
inline bool attachProfile(QJsonObject &request,const QString &profile,QString &error) {
    if(profile.isNull())return true;
    const auto command=request["command"].toString();
    if(!QStringList{Protocol::Command::Configure,Protocol::Command::Enqueue,Protocol::Command::Paste,Protocol::Command::Background}.contains(command)){error="--profile requires a configuration, copy, move or paste command";return false;}
    request["profile"]=profile;return true;
}
inline QJsonObject configure(const QStringList &arguments,QString &error) {
    auto args=arguments;const auto profile=takeProfile(args,error);if(!error.isEmpty())return {};
    QJsonObject result{{"command",Protocol::Command::Configure}},patch,gui;
    const QHash<QString,QString> flags{{"progress-message","progressMessage"},{"verify-fast-move","verify_fast_move"},{"md5","verify_md5"},{"properties","verify_properties"},{"verify-space","verify_space"},{"keep-history","keep_history"},{"resume-interrupted","resume_interrupted"},{"allow-duplication","allow_duplication"},{"include-hidden","include_hidden"},{"autostart","autostart"},{"krusader-ctrl-v","krusader_ctrl_v"},{"konqueror-ctrl-v","konqueror_ctrl_v"},{"dolphin-ctrl-v","dolphin_ctrl_v"}};
    QSet<QString> supplied;
    for(int i=2;i<args.size();++i) {
        auto name=args[i];bool enabled=true;
        if(!name.startsWith("--")){error="Unexpected configuration argument: "+name;return {};}
        name.remove(0,2);if(name.startsWith("no-")){enabled=false;name.remove(0,3);}
        if(flags.contains(name)) {
            if(supplied.contains(name)){error="Repeated or conflicting option: --"+name;return {};}
            supplied.insert(name);const auto key=flags.value(name);
            if(key=="include_hidden")patch[key]=enabled;
            else if(key=="progressMessage" || key=="krusader_ctrl_v" || key=="autostart" || key=="konqueror_ctrl_v" || key=="dolphin_ctrl_v")gui[key]=enabled;
            else result[key]=enabled;
            continue;
        }
        if(!enabled){error="Unknown configuration option: "+args[i];return {};}
        if(name=="clear-exclusions") {patch["directories"]=QJsonArray();patch["files"]=QJsonArray();continue;}
        if(name!="exclude-folder" && name!="exclude-file" && name!="rate-limit" && name!="request-rate-limit" && name!="language" && name!="conflict-policy" && name!="completion-action" && name!="completion-delay") {error="Unknown configuration option: "+args[i];return {};}
        if(i+1>=args.size() || args[i+1].startsWith("--")){error="Missing value for --"+name;return {};}
        const auto value=args[++i];
        if(name=="exclude-folder" || name=="exclude-file") {const auto key=name=="exclude-folder"?"directories":"files";auto list=patch[key].toArray();list.append(value);patch[key]=list;}
        else {
            if(supplied.contains(name)){error="Repeated option: --"+name;return {};}
            supplied.insert(name);
            if(name=="completion-action"){if(!QStringList{"none","notify","quit","suspend","poweroff"}.contains(value)){error="Invalid completion action: "+value;return {};}gui["completion_action"]=value;}
            else if(name=="completion-delay"){bool ok=false;const auto seconds=value.toInt(&ok);if(!ok || seconds<5 || seconds>3600){error="Invalid countdown: use 5 to 3600 whole seconds.";return {};}gui["completion_delay"]=seconds;}
            else if(name=="conflict-policy"){if(!QStringList{"ask","skip","newer","rename"}.contains(value)){error="Invalid conflict policy: "+value;return {};}result["conflict_policy"]=value;}
            else if(name=="language")gui["language"]=value;
            else {bool ok=false;const auto rate=value.toDouble(&ok);if(!ok || !std::isfinite(rate) || rate<0 || rate>1048576){error="Invalid --"+name+" (MiB/s): "+value;return {};}if(name=="request-rate-limit")gui["request_rate_limit"]=qRound64(rate*1048576);else result["rate_limit"]=qRound64(rate*1048576);}
        }
    }
    if(result.size()==1 && patch.isEmpty() && gui.isEmpty() && profile.isNull()){error="--configure requires at least one setting";return {};}
    if(!patch.isEmpty())result["exclusion_patch"]=patch;
    if(!gui.isEmpty())result["gui_settings"]=gui;
    if(!profile.isNull())result["profile"]=profile;
    return result;
}
}
