#pragma once
#include "i18n.h"
// Mesures volatiles par demande : aucun calcul de débit n’est enregistré dans le journal.
#include <QtCore>
#include <cmath>
#include "protocol.h"

namespace TransferMetrics {
/// Fenêtre glissante de cinq secondes d’activité, avec une horloge injectée pour les tests.
class Meter {
    QList<QPair<qint64,double>> samples{{0,0}};
    qint64 elapsed=0;
    double total=0;
public:
    void reset(){samples={{0,0}};elapsed=0;total=0;}
    void add(qint64 ms,double delta) {
        if(ms<=0)return;
        elapsed+=ms;total+=qMax(0.,delta);samples.append({elapsed,total});
        while(samples.size()>2 && samples[1].first<=elapsed-5000)samples.removeFirst();
    }
    /// Un demi-seconde de recul évite d’afficher un temps très instable au démarrage.
    bool ready() const {return elapsed>=500;}
    double speed() const {
        const auto duration=elapsed-samples.first().first;
        return duration>=500?(total-samples.first().second)*1000/duration:0.;
    }
};

/// Les sommes de travail et les états sont recalculés uniquement pour les demandes modifiées.
struct Probe {
    QString name,status,phase="waiting";
    double copied=0,verified=0,copyLeft=0,verifyLeft=0;
    bool paused=false,md5=false,blocked=false;
    static Probe from(const QJsonObject &job) {
        Probe p;p.status=job["state"].toString();p.md5=job["verify_md5"].toBool();p.paused=job["paused"].toBool();
        QStringList names;
        for(const auto &root:job["sources"].toArray())names.append(QFileInfo(root.toString()).fileName());
        p.name=names.join(", ");p.name.replace('\n'," ⏎ ");p.name.replace('\r'," ");p.name.replace('\t'," ");if(p.name.size()>60)p.name=p.name.left(57)+"…";
        bool copying=false,verifying=false,conflict=false,checking=false,hasPending=false,allPaused=true;
        for(const auto &value:job["entries"].toArray()) {
            const auto e=value.toObject();const auto s=e["state"].toString();
            if(e["hidden"].toBool() || e["cancel"].toBool() || e["kind"]=="directory")continue;
            if(s==Protocol::State::Skipped || s==Protocol::State::Cancelled || s==Protocol::State::Undone || s==Protocol::State::Error)continue;
            const bool pending=s==Protocol::State::Waiting || s==Protocol::State::Running || s==Protocol::State::Verifying || s==Protocol::State::Conflict;
            if(pending) {hasPending=true;allPaused &= e["paused"].toBool();p.blocked |= e["paused"].toBool();}
            if(s==Protocol::State::Running || s==Protocol::State::Verifying || s==Protocol::State::Conflict)p.paused |= e["paused"].toBool();
            const auto size=qMax(0.,e["size"].toDouble());
            const auto copied=qBound(0.,e["copied"].toDouble(),size);
            const auto verified=qBound(0.,e["verified"].toDouble(),size*2);
            // Un renommage ou le préfixe reconnu à la reprise ne constitue pas du trafic nouvellement copié.
            if(!e["undo"].toObject()["rename_move"].toBool())p.copied+=qMax(0.,copied-e["resumed_from"].toDouble());
            p.verified+=verified;
            if(pending) {
                p.copyLeft+=qMax(0.,size-copied);
                if(p.md5 && e["kind"]=="file")p.verifyLeft+=qMax(0.,2*size-verified);
            }
            copying |= s==Protocol::State::Running;
            verifying |= s==Protocol::State::Verifying;
            conflict |= s==Protocol::State::Conflict;
            checking |= e["resume_checking"].toBool();
        }
        p.paused |= hasPending && allPaused;
        if(p.status==Protocol::State::DeviceWaiting)p.phase="device";
        else if(p.status==Protocol::State::Undoing || p.status==Protocol::State::UndoWaiting)p.phase="undo";
        else if(checking)p.phase="resume";
        else if(conflict)p.phase="conflict";
        else if(job["fast_move"].toBool())p.phase="rename";
        else if(verifying)p.phase="md5";
        else if(copying)p.phase="copy";
        return p;
    }
};

/// Mesures séparées : le débit de copie et celui des deux relectures MD5 ne se mélangent pas.
struct Sample {
    Probe probe;
    Meter copy,verify;
    qint64 time=0;
    double previousCopy=0,previousVerify=0;
    QString previousPhase;
    bool initialized=false;
    QString phase(bool globalPause) const {
        if(probe.phase=="undo" || probe.phase=="device")return probe.phase;
        return globalPause || probe.paused?QString("pause"):probe.phase;
    }
    void tick(qint64 now,bool paused) {
        const auto current=phase(paused);
        const bool reset=!initialized || now<time || probe.copied<previousCopy || probe.verified<previousVerify;
        if(reset || current=="pause" || current=="conflict" || current=="waiting" || current=="resume" || current=="device") {copy.reset();verify.reset();}
        else if(current==previousPhase) {
            if(current=="copy")copy.add(now-time,probe.copied-previousCopy);
            if(current=="md5")verify.add(now-time,probe.verified-previousVerify);
        }
        // Exclure l’intervalle qui chevauche deux phases, sans perdre les mesures de l’autre phase.
        previousPhase=current;time=now;previousCopy=probe.copied;previousVerify=probe.verified;initialized=true;
    }
};

/// Arrondit à la seconde ; les durées excessives ou inconnues restent non estimées.
inline QString duration(double seconds) {
    if(!std::isfinite(seconds) || seconds<0 || seconds>365.*86400)return GAD_TR("unknown");
    const auto n=qint64(std::ceil(seconds));
    if(n<60)return QString(GAD_TR("%1 s")).arg(n);
    if(n<3600)return QString(GAD_TR("%1 min %2 s")).arg(n/60).arg(n%60);
    if(n<86400)return QString(GAD_TR("%1 h %2 min")).arg(n/3600).arg((n%3600)/60);
    return QString(GAD_TR("%1 d %2 h")).arg(n/86400).arg((n%86400)/3600);
}
inline QString speed(double value) {return QLocale().formattedDataSize(qint64(value))+"/s";}
inline QString detail(const Sample &s,bool paused) {
    const auto phase=s.phase(paused);
    if(phase=="device")return GAD_TR("Waiting for device · verification before resuming");
    if(phase=="pause")return GAD_TR("Paused · remaining time suspended");
    if(phase=="conflict")return GAD_TR("Decision required · remaining time unknown");
    if(phase=="resume")return GAD_TR("Checking resume data… · estimating remaining time");
    if(phase=="rename")return GAD_TR("Local rename · no data copied");
    if(phase=="undo")return GAD_TR("Restoring · remaining time unknown");
    if(phase=="waiting")return GAD_TR("Queued · estimate available when started");
    const double rate=phase=="md5"?s.verify.speed():s.copy.speed();
    QString result=phase=="md5"?GAD_TR("MD5: "):GAD_TR("Copy: ");
    if(rate<=0)return result+((phase=="md5"?s.verify.ready():s.copy.ready())?GAD_TR("0 bytes/s · remaining time unknown"):GAD_TR("calculating speed… · estimating remaining time"));
    result+=speed(rate);
    if(s.probe.blocked)return result+GAD_TR(" · total time unknown · some files paused");
    const double copyRate=s.copy.speed(),verifyRate=s.verify.speed();
    const auto &p=s.probe;
    if((p.copyLeft==0 || copyRate>0) && (p.verifyLeft==0 || verifyRate>0))
        return result+GAD_TR(" · remaining ≈ ")+duration((p.copyLeft>0?p.copyLeft/copyRate:0)+(p.verifyLeft>0?p.verifyLeft/verifyRate:0));
    // Sans mesure MD5, annoncer uniquement l’étape estimable, jamais une fausse durée totale.
    if(phase=="copy" && p.copyLeft>0 && copyRate>0)
        return result+GAD_TR(" · copy time remaining ≈ ")+duration(p.copyLeft/copyRate)+GAD_TR(" · MD5 time not yet estimated");
    return result+GAD_TR(" · total remaining time not yet estimated");
}

/// Cache incrémental : les ticks sans modification font décroître le débit sans relire les entrées.
class Tracker {
    QHash<qint64,Sample> samples;
public:
    QStringList sync(const QJsonObject &state,const QSet<qint64> &changed,qint64 now,bool reset=false) {
        if(reset)samples.clear();
        QSet<qint64> live;QStringList lines;
        const bool paused=state["paused"].toBool();
        for(const auto &value:state["jobs"].toArray()) {
            const auto job=value.toObject();const auto status=job["state"].toString();const auto id=job["id"].toInteger();
            if(Protocol::terminal(status))continue;
            live.insert(id);
            const bool fresh=!samples.contains(id);auto &s=samples[id];
            if(fresh || changed.contains(id))s.probe=Probe::from(job);
            s.tick(now,paused);
            if(status==Protocol::State::DeviceWaiting || status==Protocol::State::Running || status==Protocol::State::Undoing || status==Protocol::State::UndoWaiting)
                lines.append(QString("#%1 · %2 — %3").arg(id).arg(s.probe.name,detail(s,paused)));
        }
        for(auto it=samples.begin();it!=samples.end();)if(!live.contains(it.key()))it=samples.erase(it);else ++it;
        return lines;
    }
};
}
