//! Commandes ciblées : sélection, pause/reprise, restauration globale et réactivation.
//! La réactivation prépare les lignes annulées après leur remise en état, sans
//! réécrire le journal ni recopier les fichiers voisins déjà terminés.

use super::*;
use std::collections::{HashMap, HashSet};

// Expand directories while retaining stable journal/GUI entry indices.
/// Valide les identifiants et développe les dossiers en indices d’entrées stables.
/// Le booléen distingue la sélection d’un lot entier de celle de ses enfants.
pub(super) fn selection(
    state: &State,
    value: &Value,
) -> Result<HashMap<u64, (bool, HashSet<usize>)>, String> {
    let mut result = HashMap::new();
    for id in value["jobs"].as_array().ok_or("Missing selection")? {
        let id = id.as_u64().ok_or("Identifiant invalide")?;
        let job = state
            .jobs
            .iter()
            .find(|j| j.id == id)
            .ok_or("Operation not found")?;
        result.insert(id, (true, (0..job.entries.len()).collect()));
    }
    for row in value["entries"].as_array().ok_or("Missing selection")? {
        let id = row["job"].as_u64().ok_or("Missing operation")?;
        let index = row["index"]
            .as_u64()
            .and_then(|i| usize::try_from(i).ok())
            .ok_or("Ligne invalide")?;
        let job = state
            .jobs
            .iter()
            .find(|j| j.id == id)
            .ok_or("Operation not found")?;
        let entry = job.entries.get(index).ok_or("Ligne introuvable")?;
        let selected = &mut result
            .entry(id)
            .or_insert_with(|| (false, HashSet::new()))
            .1;
        selected.insert(index);
        if entry.kind == "directory" {
            selected.extend(
                job.entries
                    .iter()
                    .enumerate()
                    .filter(|(_, e)| {
                        e.source.starts_with(&entry.source)
                            && e.destination.starts_with(&entry.destination)
                    })
                    .map(|(i, _)| i),
            );
        }
    }
    Ok(result)
}
impl Engine {
    /// Demande la restauration des lots journalisés, terminés ou non.
    /// Les anciens lots non restaurables sont comptés pour être signalés à l’interface.
    pub(super) fn undo_all(&self) -> Result<Value, String> {
        let skipped;
        {
            let mut state = self.shared.state.lock().unwrap();
            if state.jobs.iter().any(|j| j.state == TransferState::Undoing) {
                return Err("Restoration is already running; wait for it to finish".into());
            }
            skipped = state
                .jobs
                .iter()
                .filter(|j| !j.journaled && j.state != TransferState::Undone)
                .count();
            for job in &mut state.jobs {
                if !job.journaled || job.state == TransferState::Undone {
                    continue;
                }
                job.cancel = true;
                job.resume_pending = false;
                job.undo_requested = true;
                job.undo_entries = None;
                job.retry_entries = None;
                job.retry_all = false;
                if terminal(&job.state) || job.state == TransferState::Scanning {
                    job.state = TransferState::UndoWaiting;
                }
            }
        }
        self.save()?;
        self.shared.wake.notify_all();
        Ok(json!({"ok":true,"skipped_legacy":skipped}))
    }
    /// Applique pause, reprise ou réactivation à la sélection validée.
    /// La réactivation exige l’arrêt du lot et passe d’abord par sa remise en état.
    pub(super) fn control_selection(&self, value: &Value) -> Result<Value, String> {
        let retry = value["command"] == Command::RetrySelection.as_str();
        {
            let mut state = self.shared.state.lock().unwrap();
            let selected = selection(&state, value)?;
            // Une commande sans travail applicable ne doit pas reprendre les autres lots de la file.
            if selected.is_empty() {
                return Err("No row selected".into());
            }
            let applicable = selected.iter().any(|(id, (whole, indices))| {
                let job = state.jobs.iter().find(|job| job.id == *id).unwrap();
                if retry {
                    (*whole && job.state == TransferState::Cancelled)
                        || indices
                            .iter()
                            .any(|index| job.entries[*index].state == TransferState::Cancelled)
                } else {
                    !terminal(&job.state)
                        && (*whole
                            || indices.iter().any(|index| {
                                !terminal(&job.entries[*index].state)
                                    && job.entries[*index].state != TransferState::Skipped
                            }))
                }
            });
            if !applicable {
                return Err(if retry {
                    "No canceled rows to reactivate"
                } else {
                    "No active rows to pause or resume"
                }
                .into());
            }
            for (id, (whole, indices)) in &selected {
                let job = state.jobs.iter().find(|j| j.id == *id).unwrap();
                if job.undo_requested || job.state == TransferState::Undoing {
                    return Err("Restoration is running for this operation".into());
                }
                if retry
                    && ((*whole && job.state == TransferState::Cancelled)
                        || indices
                            .iter()
                            .any(|i| job.entries[*i].state == TransferState::Cancelled))
                {
                    if !job.journaled {
                        return Err("This older operation has no resume journal".into());
                    }
                    if state.active.contains_key(id) || !terminal(&job.state) {
                        return Err(
                            "Wait for the operation to finish before reactivating canceled rows"
                                .into(),
                        );
                    }
                }
            }
            // La même requête traite la sélection de lots et de fichiers, après validation de tous les indices.
            let paused = value["paused"].as_bool().unwrap_or(true);
            for job in &mut state.jobs {
                let Some((whole, indices)) = selected.get(&job.id) else {
                    continue;
                };
                if retry {
                    let indices = indices
                        .iter()
                        .copied()
                        .filter(|i| job.entries[*i].state == TransferState::Cancelled)
                        .collect::<Vec<_>>();
                    if indices.is_empty() && !(*whole && job.state == TransferState::Cancelled) {
                        continue;
                    }
                    job.undo_entries = Some(
                        indices
                            .iter()
                            .copied()
                            .filter(|i| job.entries[*i].kind != "directory")
                            .collect(),
                    );
                    job.retry_entries = Some(indices);
                    job.retry_all = *whole;
                    job.undo_requested = true;
                    job.cancel = true;
                    job.resume_pending = false;
                    job.paused = false;
                    job.state = TransferState::UndoWaiting;
                } else {
                    if *whole {
                        job.paused = paused && !terminal(&job.state);
                    } else if !paused {
                        job.paused = false;
                    }
                    for index in indices {
                        if !terminal(&job.entries[*index].state) {
                            job.entries[*index].paused = paused;
                        }
                    }
                }
            }
            if retry || !paused {
                state.paused = false;
            }
        }
        self.save()?;
        self.shared.wake.notify_all();
        Ok(json!({"ok":true}))
    }
    /// Après restauration, remet les lignes annulées en attente et complète une analyse
    /// interrompue si le lot entier est réactivé. Préserve les journaux des voisins
    /// terminés et réactive les parents nécessaires dont la destination manque.
    pub(super) fn prepare_retry(&self, job: &mut Job) -> Result<(), String> {
        let mut selected: HashSet<usize> = job
            .retry_entries
            .clone()
            .unwrap_or_default()
            .into_iter()
            .collect();
        // A full cancellation may have interrupted the scan. Discover remaining
        // sources again without replacing the journal of already completed files.
        self.update(job.id, |j| j.cancel = false);
        if job.retry_all {
            let mut scanned = Vec::new();
            for source in &job.sources {
                if fs::symlink_metadata(source).is_err()
                    && !job
                        .entries
                        .iter()
                        .any(|entry| entry.source == *source && entry.state == TransferState::Done)
                {
                    return Err(format!(
                        "Source introuvable pour la réactivation : {}",
                        source.display()
                    ));
                }
                if fs::symlink_metadata(source).is_ok() {
                    self.scan(
                        job.id,
                        source,
                        &job.destination
                            .join(source.file_name().ok_or("Source invalide")?),
                        &mut scanned,
                    )?;
                }
            }
            for entry in scanned {
                if !job.entries.iter().any(|e| e.source == entry.source) {
                    job.entries.push(entry);
                }
            }
        }
        // Un fichier réactivé doit retrouver ses dossiers cible avant d’être recopié.
        let paths = selected
            .iter()
            .map(|i| {
                (
                    job.entries[*i].source.clone(),
                    job.entries[*i].destination.clone(),
                )
            })
            .collect::<Vec<_>>();
        for (index, entry) in job.entries.iter().enumerate() {
            if entry.kind == "directory"
                && paths.iter().any(|(source, target)| {
                    source.starts_with(&entry.source) && target.starts_with(&entry.destination)
                })
                && ((entry.state == TransferState::Error && entry.error.is_empty())
                    || fs::symlink_metadata(&entry.destination)
                        .is_err_and(|e| e.kind() == std::io::ErrorKind::NotFound))
            {
                selected.insert(index);
            }
        }
        for (index, entry) in job.entries.iter_mut().enumerate() {
            if !selected.contains(&index) {
                continue;
            }
            let meta = fs::symlink_metadata(&entry.source).map_err(|e| {
                format!(
                    "Réactivation de {} impossible : {e}",
                    entry.source.display()
                )
            })?;
            if fast_move::invalid_alias(&entry.source, &entry.destination, meta.is_dir()) {
                return Err("Retry refused: source and destination are identical or nested through their mounts".into());
            }
            // Les indices et descendants décrivent l’ancien type ; refuser une reprise ambiguë plutôt que les réutiliser.
            if source_kind(&meta) != Some(entry.kind.as_str()) {
                return Err(format!("Type de source modifié pour {}. Réajoutez cette source comme une nouvelle opération",entry.source.display()));
            }
            entry.identity = (meta.dev(), meta.ino());
            entry.size = if entry.kind == "file" { meta.len() } else { 0 };
            entry.copied = 0;
            entry.verified = 0;
            entry.md5_source = None;
            entry.md5_destination = None;
            entry.cancel = false;
            entry.hidden = false;
            entry.paused = false;
            entry.state = TransferState::Waiting;
            entry.error.clear();
            // Les journaux de fichiers ont été restaurés ; garder celui des dossiers encore utilisés protège leur provenance.
            if entry.kind != "directory" {
                entry.undo = None;
            }
        }
        job.cancel = false;
        job.paused = false;
        job.undo_requested = false;
        job.undo_entries = None;
        job.retry_entries = None;
        job.retry_all = false;
        job.state = TransferState::Waiting;
        job.error.clear();
        job.recovery = false;
        job.disks = scheduling::disks_for_entries(&job.entries);
        job.paths = fast_move::reservations(&job.entries);
        job.destination_keys =
            fast_move::destination_keys(job.entries.iter().map(|e| e.destination.clone()));
        job.fast_move = job.paths.is_some()
            && fast_move::eligible(&job.entries, job.mode == "move", job.verify_md5);
        {
            let mut state = self.shared.state.lock().unwrap();
            if let Some(current) = state.jobs.iter_mut().find(|j| j.id == job.id) {
                // Un réglage de débit effectué pendant la préparation reste prioritaire.
                job.rate_limit = current.rate_limit;
                **current = job.clone();
            }
            state.row_layout = None;
        }
        self.shared.wake.notify_all();
        self.save()
    }
}
