#pragma once
// Profils personnels : snapshots des nouvelles demandes, sans sources ni authentification.
#include "presentation.h"
#include "completion_plan.h"
#include <cmath>

namespace TransferProfiles {
inline constexpr qint64 MaximumRate=1LL<<40;
inline constexpr auto SettingsKey="transferProfiles";
struct Profile {
    QString id,name;
    QJsonObject gui;
    QJsonObject options;
    qint64 rate=0;
    /// Un identifiant reste stable lors d’un renommage ; seuls les réglages sont comparés dans le menu.
    QJsonObject json() const {return {{"id",id},{"name",name},{"options",options},{"gui",gui},{"request_rate_limit",rate}};}
};
inline QString nameKey(const QString &value){return value.normalized(QString::NormalizationForm_C).toCaseFolded();}
inline bool validRate(const QJsonValue &value) {
    const auto rate=value.toDouble(-1);
    return value.isDouble() && std::isfinite(rate) && rate>=0 && rate<=MaximumRate && std::floor(rate)==rate;
}
/// Garder les mêmes limites de motifs que le moteur ; aucune règle arbitraire ne devient un chemin.
inline bool validRules(const QJsonObject &rules) {
    if(rules.size()!=3 || !rules["include_hidden"].isBool())return false;
    for(const auto &key:QStringList{"directories","files"}) {
        if(!rules[key].isArray() || rules[key].toArray().size()>128)return false;
        for(const auto &value:rules[key].toArray()) {
            const auto pattern=value.toString();
            if(!value.isString() || pattern.isEmpty() || pattern.toUtf8().size()>256 || pattern.contains('/') || pattern.contains(QChar(0)) || pattern.contains('\n') || pattern.contains('\r'))return false;
        }
    }
    return true;
}
/// Valider avant toute écriture ou modification des préférences en cours.
inline bool validate(Profile &profile,QString &error) {
    error.clear();profile.name=profile.name.trimmed();
    if(profile.name.isEmpty() || profile.name.size()>100 || profile.name.contains(QChar(0)) || profile.name.contains('\n') || profile.name.contains('\r')){error=GAD_TR("Enter a profile name (up to 100 characters).");return false;}
    if(QUuid(profile.id).isNull()){error=GAD_TR("Invalid transfer profile settings.");return false;}
    for(const auto &key:QStringList{"verify_fast_move","verify_md5","verify_properties","verify_space","allow_duplication","keep_history","resume_interrupted"})
        if(!profile.options[key].isBool()){error=GAD_TR("Invalid transfer profile settings.");return false;}
    for(const auto &key:QStringList{"autostart","dolphin_ctrl_v","konqueror_ctrl_v"})
        if(!profile.gui[key].isBool()){error=GAD_TR("Invalid transfer profile settings.");return false;}
    if(profile.options.size()!=10 || !profile.options["conflict_policy"].isString() || !QStringList{"ask","skip","newer","rename"}.contains(profile.options["conflict_policy"].toString()) || !validRate(profile.options["rate_limit"]) || !profile.options["exclusions"].isObject() || !validRules(profile.options["exclusions"].toObject()) || profile.rate<0 || profile.rate>MaximumRate || (profile.gui.size()!=6+(profile.gui.contains("krusader_ctrl_v")?1:0)) || (profile.gui.contains("krusader_ctrl_v") && !profile.gui["krusader_ctrl_v"].isBool()) || !profile.gui["completion_action"].isString() || !Completion::validAction(profile.gui["completion_action"].toString()) || !Completion::validDelay(profile.gui["completion_delay"]) || !profile.gui["language"].isString() || profile.gui["language"].toString().isEmpty() || profile.gui["language"].toString().size()>100){error=GAD_TR("Invalid transfer profile settings.");return false;}
    return true;
}
inline bool store(const QList<Profile> &profiles,QString &error);
/// Compléter une seule fois les anciennes définitions avec les préférences présentes lors de la migration.
inline Profile migrationDefaults() {
    Profile profile;QSettings settings;
    profile.options={{"keep_history",settings.value("keepHistory",true).toBool()},{"resume_interrupted",settings.value("resumeInterrupted",true).toBool()},{"rate_limit",qBound<qint64>(0,settings.value("rateLimit",0).toLongLong(),MaximumRate)}};
    QFile receipt(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)+"/gad/dolphin-paste.json");
    const bool dolphin=receipt.open(QIODevice::ReadOnly) && !QJsonDocument::fromJson(receipt.readAll()).object()["files"].toObject().isEmpty();
    const auto savedLanguage=settings.value("language","system").toString();
    profile.gui={{"language",savedLanguage=="system"?savedLanguage:I18n::resolve(savedLanguage)},{"autostart",autostartEnabled()},{"dolphin_ctrl_v",dolphin},{"konqueror_ctrl_v",settings.value("konquerorCtrlV",true).toBool()}};
    return profile;
}
/// Un document inconnu ou abîmé reste intact. V1 est réécrit sans aucune destination.
inline QList<Profile> load(QString &error) {
    error.clear();const auto raw=QSettings().value(SettingsKey).toByteArray();if(raw.isEmpty() && !QSettings().contains(SettingsKey))return {};
    const auto invalid=[&](){error=GAD_TR("Saved transfer profiles are invalid; kept unchanged.");return QList<Profile>{};};
    if(raw.size()>1024*1024)return invalid();
    const auto document=QJsonDocument::fromJson(raw);const auto root=document.object();const auto version=root["version"].toInt();
    if(!document.isObject() || (version!=1 && version!=2 && version!=3 && version!=4 && version!=5) || !root["profiles"].isArray() || root["profiles"].toArray().size()>64)return invalid();
    const auto defaults=version==1?migrationDefaults():Profile{};QList<Profile> result;QSet<QString> names,ids;
    for(const auto &value:root["profiles"].toArray()) {
        if(!value.isObject())return invalid();
        auto object=value.toObject();Profile profile;profile.id=object.take("id").toString();profile.name=object.take("name").toString();
        if(version==1) {
            object.remove("destination");const auto rate=object.take("rate_limit");if(!validRate(rate) || object.size()!=5)return invalid();
            profile.rate=rate.toInteger();profile.options=object;for(auto it=defaults.options.begin();it!=defaults.options.end();++it)profile.options[it.key()]=it.value();profile.gui=defaults.gui;
        } else {
            const auto rate=object.take("request_rate_limit");if(!validRate(rate) || object.size()!=2 || !object["options"].isObject() || !object["gui"].isObject())return invalid();
            profile.rate=rate.toInteger();profile.options=object["options"].toObject();profile.gui=object["gui"].toObject();
        }
        if(version<3)profile.options["conflict_policy"]="ask";
        if(version<4)profile.options["verify_fast_move"]=false;
        if(version<5){profile.gui["completion_action"]="none";profile.gui["completion_delay"]=30;}
        QString validation;if(!validate(profile,validation) || names.contains(nameKey(profile.name)) || ids.contains(profile.id))return invalid();
        names.insert(nameKey(profile.name));ids.insert(profile.id);result.append(profile);
    }
    if(version<5 && !store(result,error))return {};
    return result;
}
/// QSettings synchronise atomiquement son fichier ; vérifier l’erreur et conserver la valeur précédente si possible.
inline bool store(const QList<Profile> &profiles,QString &error) {
    error.clear();if(profiles.size()>64){error=GAD_TR("Up to 64 transfer profiles can be saved.");return false;}
    QJsonArray values;QSet<QString> names,ids;
    for(auto profile:profiles) {
        if(!validate(profile,error))return false;
        if(names.contains(nameKey(profile.name))){error=GAD_TR("A profile with this name already exists.");return false;}
        if(ids.contains(profile.id)){error=GAD_TR("Invalid transfer profile settings.");return false;}
        names.insert(nameKey(profile.name));ids.insert(profile.id);values.append(profile.json());
    }
    const auto raw=QJsonDocument(QJsonObject{{"version",5},{"profiles",values}}).toJson(QJsonDocument::Compact);
    if(raw.size()>1024*1024){error=GAD_TR("Invalid transfer profile settings.");return false;}
    QSettings settings;const auto previous=settings.value(SettingsKey);settings.setValue(SettingsKey,raw);settings.sync();
    if(settings.status()!=QSettings::NoError){if(previous.isValid())settings.setValue(SettingsKey,previous);else settings.remove(SettingsKey);settings.sync();error=GAD_TR("Unable to save transfer profiles.");return false;}
    return true;
}
}
