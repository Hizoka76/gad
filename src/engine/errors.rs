//! Bilan structuré et réessai ciblé : ne sélectionne jamais implicitement un fichier OK ou annulé.
use super::*;
use std::collections::HashSet;

/// Une restauration explicitement demandée ne doit pas devenir une nouvelle copie.
fn eligible_job(job: &Job, active: bool) -> Result<(), String> {
    if active || !terminal(&job.state) || job.undo_requested {
        return Err("Wait for the request or its recovery to finish".into());
    }
    if !job.journaled {
        return Err("Retry journal unavailable".into());
    }
    if job.state == TransferState::UndoError && job.retry_entries.is_none() {
        return Err("Restoration incomplete: use Cancel and restore".into());
    }
    Ok(())
}
/// Un dossier sans message propre reflète seulement l’échec d’un enfant.
fn aggregate(entry: &Entry) -> bool {
    entry.kind == "directory" && entry.state == TransferState::Error && entry.error.is_empty()
}
fn failed(job: &Job, index: usize) -> bool {
    let entry = &job.entries[index];
    if entry.hidden || entry.cancel || aggregate(entry) {
        return false;
    }
    entry.state == TransferState::Error
        || (job.state == TransferState::UndoError
            && job
                .retry_entries
                .as_ref()
                .is_some_and(|ids| ids.contains(&index))
            && !entry.error.is_empty())
}

fn retry_reason(job: &Job, index: Option<usize>, active: bool) -> String {
    if let Err(reason) = eligible_job(job, active) {
        return reason;
    }
    match index {
        Some(i) if failed(job, i) => String::new(),
        None if job.entries.is_empty() && job.state == TransferState::Error => String::new(),
        _ => "This result is not a failed file that can be copied again".into(),
    }
}
impl Engine {
    /// Le rapport est un instantané en lecture seule ; les métadonnées d’origine permettent de présenter les URL distantes.
    pub(super) fn error_report(&self) -> Result<Value, String> {
        let state = self.shared.state.lock().unwrap();
        let mut records = Vec::new();
        let mut jobs = Vec::new();
        for job in &state.jobs {
            let before = records.len();
            let active = state.active.contains_key(&job.id);
            for (index, entry) in job.entries.iter().enumerate() {
                if entry.hidden || aggregate(entry) {
                    continue;
                }
                let described = entry.state == TransferState::Error
                    || (matches!(entry.state, TransferState::Done | TransferState::Undone)
                        && matches!(job.state, TransferState::Error | TransferState::UndoError)
                        && !entry.error.is_empty());
                if !described {
                    continue;
                }
                let reason = retry_reason(job, Some(index), active);
                records.push(json!({"job":job.id,"index":index,"scope":"entry","mode":job.mode,"state":entry.state,"kind":entry.kind,
                    "source":entry.source,"destination":entry.destination,"error":if entry.error.is_empty(){"Transfer error with no additional details"}else{&entry.error},
                    "copied":entry.copied,"size":entry.size,"resumed_from":entry.resumed_from,
                    "md5_source":entry.md5_source,"md5_destination":entry.md5_destination,"properties":entry.properties,
                    "partial_path":entry.undo.as_ref().and_then(|r|r.staging.as_ref()),
                    "durable_bytes":entry.undo.as_ref().and_then(|r|r.resume.as_ref()).map(|p|p.bytes),
                    "published":entry.undo.as_ref().is_some_and(|r|r.result.is_some()),"retryable":reason.is_empty(),"retry_reason":reason}));
            }
            if matches!(job.state, TransferState::Error | TransferState::UndoError)
                && (records.len() == before
                    || (!job.error.is_empty()
                        && !records[before..].iter().any(|r| r["error"] == job.error)))
            {
                let reason = retry_reason(job, None, active);
                records.push(json!({"job":job.id,"index":null,"scope":"job","mode":job.mode,"state":job.state,"kind":"operation",
                    "source":job.sources,"destination":job.destination,
                    "error":if job.error.is_empty(){"The request has finalization errors; see the affected rows"}else{&job.error},
                    "retryable":reason.is_empty(),"retry_reason":reason}));
            }
            if records.len() > before {
                jobs.push(json!({"id":job.id,"mode":job.mode,"sources":job.sources,"source_urls":job.source_urls,
                    "destination":job.destination,"destination_url":job.destination_url,"state":job.state}));
            }
        }
        Ok(
            json!({"format":"gad-error-report","version":1,"protocol_version":protocol::VERSION,
            "generated_unix":std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_err(|e|e.to_string())?.as_secs(),
            "count":records.len(),"records":records,"jobs":jobs}),
        )
    }
    /// Validation atomique de la sélection ; les erreurs d’analyse vides repassent par la validation complète des racines.
    pub(super) fn retry_errors(&self, value: &Value) -> Result<Value, String> {
        let mut preparations = Vec::new();
        let mut count = 0;
        {
            let mut state = self.shared.state.lock().unwrap();
            let selected = controls::selection(&state, value)?;
            let mut plans = Vec::new();
            for (id, (whole, mut indices)) in selected {
                if value["exact"].as_bool() == Some(true) && !whole {
                    let explicit = value["entries"]
                        .as_array()
                        .unwrap()
                        .iter()
                        .filter(|r| r["job"] == id)
                        .filter_map(|r| r["index"].as_u64().map(|i| i as usize))
                        .collect::<HashSet<_>>();
                    indices.retain(|i| explicit.contains(i));
                }
                let job = state.jobs.iter().find(|j| j.id == id).unwrap();
                let errors = indices
                    .into_iter()
                    .filter(|i| failed(job, *i))
                    .collect::<Vec<_>>();
                let preparation =
                    whole && job.entries.is_empty() && job.state == TransferState::Error;
                if errors.is_empty() && !preparation {
                    continue;
                }
                eligible_job(job, state.active.contains_key(&id))?;
                plans.push((id, errors, preparation));
            }
            if plans.is_empty() {
                return Err("No failed files to retry in this selection".into());
            }
            for (id, indices, preparation) in plans {
                let job = state.jobs.iter_mut().find(|j| j.id == id).unwrap();
                if preparation {
                    job.state = TransferState::Scanning;
                    job.cancel = false;
                    job.paused = false;
                    job.error.clear();
                    preparations.push(job.snapshot());
                    count += 1;
                } else {
                    let chosen = indices.iter().copied().collect::<HashSet<_>>();
                    // Les lignes encore en attente d’une demande arrêtée restent suspendues si elles n’étaient pas en erreur.
                    for (index, entry) in job.entries.iter_mut().enumerate() {
                        if entry.state == TransferState::Waiting && !chosen.contains(&index) {
                            entry.paused = true;
                        }
                    }
                    job.undo_entries = Some(
                        indices
                            .iter()
                            .copied()
                            .filter(|i| job.entries[*i].kind != "directory")
                            .collect(),
                    );
                    count += indices.len();
                    job.retry_entries = Some(indices);
                    job.retry_all = false;
                    job.undo_requested = true;
                    job.cancel = true;
                    job.resume_pending = false;
                    job.paused = false;
                    job.state = TransferState::UndoWaiting;
                }
            }
        }
        if let Err(error) = self.save() {
            for job in &preparations {
                self.update(job.id, |j| {
                    if j.state == TransferState::Scanning {
                        j.state = TransferState::Error;
                        j.error = error.clone();
                    }
                });
            }
            self.shared.wake.notify_all();
            return Err(error);
        }
        self.shared.wake.notify_all();
        for job in preparations {
            let engine = self.clone();
            let id = job.id;
            self.spawn_preparation(id, false, 0, move || {
                let destination = engine.validate_roots(id, &job.sources, &job.destination)?;
                let mut entries = Vec::new();
                for source in &job.sources {
                    engine.scan(
                        id,
                        source,
                        &destination.join(source.file_name().ok_or("Source invalide")?),
                        &mut entries,
                    )?;
                }
                let disks = scheduling::disks_for_entries(&entries);
                Ok((destination, entries, disks))
            });
        }
        Ok(json!({"ok":true,"retried":count}))
    }
}
