#pragma once
// Options persistantes : uniquement avec --configure, pour séparer préférences et transferts.
#include "protocol.h"
#include <QtCore>
#include <cmath>

namespace CliOptions {
inline QJsonObject configure(const QStringList &args,QString &error) {
    error.clear();
    QJsonObject result{{"command",Protocol::Command::Configure}},patch,gui;
    const QHash<QString,QString> flags{{"md5","verify_md5"},{"properties","verify_properties"},{"verify-space","verify_space"},{"keep-history","keep_history"},{"resume-interrupted","resume_interrupted"},{"allow-duplication","allow_duplication"},{"include-hidden","include_hidden"},{"autostart","autostart"},{"konqueror-ctrl-v","konqueror_ctrl_v"},{"dolphin-ctrl-v","dolphin_ctrl_v"}};
    QSet<QString> supplied;
    for(int i=2;i<args.size();++i) {
        auto name=args[i];bool enabled=true;
        if(!name.startsWith("--")){error="Unexpected configuration argument: "+name;return {};}
        name.remove(0,2);if(name.startsWith("no-")){enabled=false;name.remove(0,3);}
        if(flags.contains(name)) {
            if(supplied.contains(name)){error="Repeated or conflicting option: --"+name;return {};}
            supplied.insert(name);const auto key=flags.value(name);
            if(key=="include_hidden")patch[key]=enabled;
            else if(key=="autostart" || key=="konqueror_ctrl_v" || key=="dolphin_ctrl_v")gui[key]=enabled;
            else result[key]=enabled;
            continue;
        }
        if(!enabled){error="Unknown configuration option: "+args[i];return {};}
        if(name=="clear-exclusions") {patch["directories"]=QJsonArray();patch["files"]=QJsonArray();continue;}
        if(name!="exclude-folder" && name!="exclude-file" && name!="rate-limit" && name!="language") {error="Unknown configuration option: "+args[i];return {};}
        if(i+1>=args.size() || args[i+1].startsWith("--")){error="Missing value for --"+name;return {};}
        const auto value=args[++i];
        if(name=="exclude-folder" || name=="exclude-file") {const auto key=name=="exclude-folder"?"directories":"files";auto list=patch[key].toArray();list.append(value);patch[key]=list;}
        else {
            if(supplied.contains(name)){error="Repeated option: --"+name;return {};}
            supplied.insert(name);
            if(name=="language")gui["language"]=value;
            else {bool ok=false;const auto rate=value.toDouble(&ok);if(!ok || !std::isfinite(rate) || rate<0 || rate>1048576){error="Invalid --rate-limit (MiB/s): "+value;return {};}result["rate_limit"]=qRound64(rate*1048576);}
        }
    }
    if(result.size()==1 && patch.isEmpty() && gui.isEmpty()){error="--configure requires at least one setting";return {};}
    if(!patch.isEmpty())result["exclusion_patch"]=patch;
    if(!gui.isEmpty())result["gui_settings"]=gui;
    return result;
}
}
