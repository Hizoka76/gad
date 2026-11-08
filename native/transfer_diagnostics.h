#pragma once
// Causes observées et limites configurées ; aucune déduction sur la santé ou la vitesse maximale d’un disque.
#include "transfer_metrics.h"
#include "presentation.h"

namespace TransferDiagnostics {
struct Item {qint64 id=0;QString name,status,reason,limits,metrics;};
struct Observation {double progress=-1;qint64 since=0;QString phase;};
class Tracker {
    QHash<qint64,Observation> observations;
    QList<Item> current;
public:
    const QList<Item> &items() const {return current;}
    void sync(const QJsonObject &state,const TransferMetrics::Tracker &meters,qint64 now,bool reset=false) {
        if(reset){observations.clear();}
        current.clear();QSet<qint64> live;
        for(const auto &value:state["jobs"].toArray()) {
            const auto job=value.toObject();const auto status=job["state"].toString();const auto id=job["id"].toInteger();
            if(Protocol::terminal(status))continue;
            const auto *sample=meters.sample(id);if(!sample)continue;
            live.insert(id);auto phase=sample->phase(state["paused"].toBool());
            if(status==Protocol::State::Scanning)phase="scan";
            if(status==Protocol::State::Waiting && phase!="pause")phase="waiting";
            const auto diagnostic=state["diagnostics"].toObject().value(QString::number(id)).toObject();
            const auto queue=diagnostic["queue_reason"].toString();
            const bool global=diagnostic["global_throttled"].toBool(),local=diagnostic["request_throttled"].toBool();
            const double progress=sample->probe.copied+sample->probe.verified;
            auto &observation=observations[id];
            if(observation.progress<0 || now<observation.since || observation.progress!=progress || observation.phase!=phase || global || local)observation.since=now;
            observation.progress=progress;observation.phase=phase;
            Item item;item.id=id;item.name=sample->probe.name;item.status=label(status,state["paused"].toBool() || sample->probe.paused);
            item.metrics=TransferMetrics::detail(*sample,state["paused"].toBool());
            const auto limit=[](qint64 bytes){return bytes>0?TransferMetrics::speed(bytes):GAD_TR("Unlimited");};
            // Le second plafond est celui réellement capturé par la demande, pas la préférence future.
            item.limits=GAD_TR("Global")+": "+limit(state["rate_limit"].toInteger())+"\n"+GAD_TR("Request")+": "+limit(job["rate_limit"].toInteger());
            if(job["start_required"].toBool() && status!=Protocol::State::Scanning){item.status=GAD_TR("Ready to start");item.reason=GAD_TR("Waiting for Start in the main window.");}
            else if(phase=="device")item.reason=GAD_TR("Device or remote mount unavailable. Waiting for reconnection and verification before resuming.");
            else if(phase=="pause")item.reason=GAD_TR("Paused by the queue, batch, or current file.");
            else if(phase=="conflict")item.reason=GAD_TR("A file conflict needs your decision in the main window.");
            else if(phase=="md5")item.reason=GAD_TR("MD5 verification is reading source and destination. Copying bytes is complete for the current file.");
            else if(phase=="resume")item.reason=GAD_TR("Checking the saved prefix against the source before resuming. No new bytes are copied during this check.");
            else if(phase=="fast_verify")item.reason=GAD_TR("Checking the fast-move result: identity, metadata, tree and source removal. File contents are not reread.");
            else if(phase=="rename")item.reason=GAD_TR("Moving by local rename. File data is not being copied.");
            else if(phase=="undo")item.reason=status==Protocol::State::UndoWaiting?GAD_TR("Restoration needs exclusive access and is waiting for active transfers to finish."):GAD_TR("Restoring files and backups. Ordinary transfers wait until restoration finishes.");
            else if(queue=="scan")item.reason=GAD_TR("Scanning sources and preparing the file list.");
            else if(queue=="paused")item.reason=GAD_TR("Paused by the queue, batch, or current file.");
            else if(queue=="restoration_priority" || queue=="restoration_exclusive")item.reason=GAD_TR("Waiting for exclusive restoration work.");
            else if(queue=="parallel_limit")item.reason=GAD_TR("Both transfer workers are occupied. Waiting for an available slot.");
            else if(queue=="earlier_scan")item.reason=GAD_TR("An earlier request is still being scanned. Its disk reservations are not known yet.");
            else if(queue=="row_order")item.reason=GAD_TR("Waiting for earlier rows in the processing order.");
            else if(queue=="paths_unknown")item.reason=GAD_TR("Path independence cannot be verified. Gad serializes these transfers.");
            else if(queue=="resources_unknown")item.reason=GAD_TR("Disk topology is unknown. Gad serializes these transfers to avoid competing access.");
            else if(queue=="shared_resources")item.reason=GAD_TR("An active transfer uses the same disk or overlapping paths.");
            else if(queue=="queue_priority")item.reason=GAD_TR("An earlier request has priority on shared resources.");
            else if(phase=="copy" && global && local)item.reason=GAD_TR("Waiting for both global and request speed budgets (observed within the last second).");
            else if(phase=="copy" && global)item.reason=GAD_TR("Waiting for the global speed budget (observed within the last second).");
            else if(phase=="copy" && local)item.reason=GAD_TR("Waiting for the request speed budget (observed within the last second).");
            else if(phase=="copy" && diagnostic.isEmpty())item.reason=GAD_TR("Copying. Detailed wait telemetry is unavailable.");
            else if(phase=="copy")item.reason=GAD_TR("Copying. No speed-limit wait was observed recently.");
            else if(queue=="active")item.reason=GAD_TR("Preparing file operations or waiting for file I/O.");
            else item.reason=GAD_TR("Queued. Waiting for the scheduler to start this request.");
            QStringList blockers;for(const auto &value:diagnostic["blockers"].toArray())blockers.append("#"+QString::number(value.toInteger()));
            if(!blockers.isEmpty())item.reason+="\n"+GAD_TR("Blocking requests:")+" "+blockers.join(", ");
            if((phase=="copy" || phase=="md5") && !global && !local && now-observation.since>=5000)
                item.reason+="\n"+QString(GAD_TR("No byte progress for %1. File I/O or finalization may be waiting; progress alone cannot identify a disk or network bottleneck.")).arg(TransferMetrics::duration((now-observation.since)/1000.));
            if(job["verify_properties"].toBool())item.reason+="\n"+GAD_TR("Property verification is enabled; this setting alone does not prove it is causing a delay.");
            current.append(item);
        }
        for(auto it=observations.begin();it!=observations.end();)if(!live.contains(it.key()))it=observations.erase(it);else ++it;
    }
};
}
