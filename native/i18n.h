#pragma once
// Les textes sont traduits à l’affichage : chemins, identifiants et journal restent inchangés.
#include <QtCore>
#include <memory>
#include <functional>
#include <algorithm>

#define GAD_TR(source) QCoreApplication::translate("Gad", source)

namespace I18n {
/// Une liaison décrit exclusivement une propriété d’interface, jamais le contenu d’un chemin.
struct Binding {
    QPointer<QObject> owner;
    QByteArray key;
    std::function<void()> apply;
};
inline QList<Binding> &bindings() {static QList<Binding> entries;return entries;}
inline void pruneBindings() {
    auto &entries=bindings();
    entries.erase(std::remove_if(entries.begin(),entries.end(),[](const Binding &entry){return entry.owner.isNull();}),entries.end());
}
/// Le propriétaire Qt borne la durée de vie de chaque rappel ; une même propriété remplace sa liaison.
inline void onChange(QObject *owner,const QByteArray &key,std::function<void()> apply,bool initial=true) {
    pruneBindings();auto &entries=bindings();
    entries.erase(std::remove_if(entries.begin(),entries.end(),[owner,&key](const Binding &entry){return entry.owner==owner && entry.key==key;}),entries.end());
    entries.append({owner,key,apply});
    if(!owner->property("_gadTranslationOwner").toBool()) {
        owner->setProperty("_gadTranslationOwner",true);
        QObject::connect(owner,&QObject::destroyed,[]{pruneBindings();});
    }
    if(initial)apply();
}
inline void bindProperty(QObject *owner,const char *name,std::function<QString()> text) {
    const QByteArray property(name);
    onChange(owner,property,[owner,property,text]{owner->setProperty(property.constData(),text());});
}
/// Conserve le widget et ses signaux : seul son texte est réévalué à chaque changement de langue.
template<class T> T *localized(T *object,const char *property,std::function<QString()> text) {
    bindProperty(object,property,std::move(text));return object;
}
inline void retranslate() {
    pruneBindings();const auto snapshot=bindings();
    for(const auto &entry:snapshot)if(entry.owner)entry.apply();
}

/// Traduit uniquement un diagnostic complet connu. Aucun remplacement dans un nom de fichier.
/// Les détails externes/non catalogués restent lisibles dans leur langue d’origine.
inline QString message(const QString &source) {
    return QCoreApplication::translate("Engine", source.toUtf8().constData());
}

/// Chaque catalogue valide fournit sa locale, son nom natif et son image associée.
struct Language {QString locale;QString name;QString catalog;QString flag;};
inline QList<Language> languages() {
    QMap<QString,Language> found;
    // L’anglais américain reste disponible comme langue source, même sans catalogue.
    found.insert("en_US",{"en_US","English (US)",{},":/flags/en_US.png"});
    QStringList directories{":/i18n",QCoreApplication::applicationDirPath()+"/translations"};
    const auto locations=QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    for(auto it=locations.crbegin();it!=locations.crend();++it)directories.append(*it+"/gad/translations");
    // Les fichiers utilisateur passent après les ressources et les catalogues système.
    for(const auto &folder:directories) {
        const QDir directory(folder);
        for(const auto &file:directory.entryList({"gad_*.qm"},QDir::Files,QDir::Name)) {
            const auto code=file.mid(4,file.size()-7);
            if(!QRegularExpression("^[a-z]{2,3}(?:_[A-Z][a-z]{3})?(?:_(?:[A-Z]{2}|[0-9]{3}))?$").match(code).hasMatch())continue;
            const QLocale locale(code);if(locale.language()==QLocale::C)continue;
            QTranslator probe;const auto path=directory.filePath(file);if(!probe.load(path))continue;
            auto name=locale.nativeLanguageName();if(name.isEmpty())name=QLocale::languageToString(locale.language());
            if(!name.isEmpty())name[0]=name[0].toUpper();
            QString flag;
            for(const auto &candidate:QStringList{directory.filePath(code+".png"),directory.filePath("flags/"+code+".png"),directory.filePath("../flags/"+code+".png"),":/flags/"+code+".png"})
                if(QFile::exists(candidate)){flag=candidate;break;}
            found.insert(code,{code,name,path,flag});
        }
    }
    auto result=found.values();
    for(auto &entry:result) {
        int same=0;for(const auto &other:found)if(other.name==entry.name)++same;
        if(same>1)entry.name+=" ("+entry.locale+")";
    }
    return result;
}
/// Cherche la variante exacte puis une variante disponible de la même langue.
inline QString resolve(const QString &choice, const QLocale &system = QLocale::system()) {
    const auto requested=choice.isEmpty() || choice=="system"?system:QLocale(choice);
    const auto available=languages();
    auto normalized=choice;normalized.replace('-','_');
    for(const auto &entry:available)if(entry.locale.compare(normalized,Qt::CaseInsensitive)==0)return entry.locale;
    for(const auto &entry:available)if(entry.locale==requested.name())return entry.locale;
    for(const auto &entry:available)if(QLocale(entry.locale).language()==requested.language())return entry.locale;
    for(const auto &entry:available)if(entry.locale=="fr_FR")return entry.locale;
    return "en_US";
}
/// Change les traducteurs puis actualise les propriétés déjà affichées, sans recréer de widget.
inline void initialize(const QString &choice = QString()) {
    static std::unique_ptr<QTranslator> translator;
    static std::unique_ptr<QTranslator> qtTranslator;
    if (translator) QCoreApplication::removeTranslator(translator.get());
    if (qtTranslator) QCoreApplication::removeTranslator(qtTranslator.get());
    auto selected = choice;
    if (selected.isEmpty()) selected = qEnvironmentVariable("GAD_LANGUAGE");
    if (selected.isEmpty()) selected = QSettings().value("language", "system").toString();
    const auto language = resolve(selected);
    QLocale::setDefault(QLocale(language));
    translator = std::make_unique<QTranslator>();
    for(const auto &entry:languages())if(entry.locale==language && !entry.catalog.isEmpty() && translator->load(entry.catalog)) {
        QCoreApplication::installTranslator(translator.get());break;
    }
    qtTranslator = std::make_unique<QTranslator>();
    if (qtTranslator->load("qtbase_" + language, QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        QCoreApplication::installTranslator(qtTranslator.get());
    retranslate();
}
}
