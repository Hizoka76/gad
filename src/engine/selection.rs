//! Sémantique des commandes d’annulation et des touches Suppr/Retour arrière.
//! Masquer une entrée ne supprime pas son journal : ses indices doivent rester
//! stables tant qu’un travailleur ou une restauration peut encore les utiliser.

use super::*;
use std::collections::HashSet;

impl Engine {
    /// Arrête les copies en cours et à venir sans supprimer les résultats terminés.
    /// Une restauration déjà commencée finit son étape transactionnelle.
    pub(super) fn cancel_all(&self) -> Result<Value, String> {
        {
            let mut state = self.shared.state.lock().unwrap();
            for job in &mut state.jobs {
                // Une reprise en erreur reste une opération en attente d’action, même si son état est terminal.
                if job.resume_pending && job.state == TransferState::Error {
                    job.resume_pending = false;
                    job.cancel = true;
                    job.state = TransferState::Cancelled;
                    for entry in &mut job.entries {
                        if entry.interrupted {
                            entry.state = TransferState::Cancelled;
                        }
                    }
                }
                // A started rollback is transactional and must finish its current restoration.
                if !terminal(&job.state) && job.state != TransferState::Undoing {
                    job.cancel = true;
                    job.resume_pending = false;
                    job.undo_requested = false;
                    job.retry_entries = None;
                    job.retry_all = false;
                    if job.state == TransferState::UndoWaiting
                        || job.state == TransferState::Scanning
                    {
                        job.state = TransferState::Cancelled;
                    }
                }
            }
        }
        self.save()?;
        self.shared.wake.notify_all();
        Ok(json!({"ok":true}))
    }
    /// Interprète annulation simple, suppression d’historique, restauration et clavier.
    /// Valide toute la sélection avant mutation ; un dossier inclut ses descendants.
    /// Les lignes masquées conservent leur journal jusqu’au nettoyage du lot.
    pub(super) fn delete_selection(&self, v: &Value) -> Result<Value, String> {
        let cancel_only = v["command"] == Command::CancelSelection.as_str();
        let force_restore = v["command"] == Command::UndoSelection.as_str();
        let restore = force_restore || v["command"] == Command::BackspaceSelection.as_str();
        let jobs: HashSet<u64> = v["jobs"]
            .as_array()
            .ok_or("Missing operation selection")?
            .iter()
            .map(|id| id.as_u64().ok_or("Identifiant invalide"))
            .collect::<Result<_, _>>()?;
        let entries: Vec<(u64, usize)> = v["entries"]
            .as_array()
            .ok_or("Missing file selection")?
            .iter()
            .map(|e| {
                Ok((
                    e["job"].as_u64().ok_or("Missing operation")?,
                    usize::try_from(e["index"].as_u64().ok_or("Ligne manquante")?)
                        .map_err(|_| "Ligne invalide")?,
                ))
            })
            .collect::<Result<_, &str>>()?;
        let mut remove = HashSet::new();
        {
            let mut state = self.shared.state.lock().unwrap();
            let selected = jobs
                .iter()
                .copied()
                .chain(entries.iter().map(|(id, _)| *id))
                .collect::<HashSet<_>>();
            // La validation complète précède toute mutation pour ne pas annuler seulement une partie d’une sélection invalide.
            for id in &selected {
                let job = state
                    .jobs
                    .iter()
                    .find(|j| j.id == *id)
                    .ok_or("Operation not found")?;
                if restore
                    && !job.journaled
                    && (terminal(&job.state)
                        || entries.iter().any(|(id, index)| {
                            *id == job.id
                                && job.entries.get(*index).is_some_and(|e| terminal(&e.state))
                        }))
                {
                    return Err("This older operation has no restoration journal".into());
                }
                if job.undo_requested || job.state == TransferState::Undoing {
                    return Err(
                        "Restoration is running; wait for it to finish before removing its rows"
                            .into(),
                    );
                }
            }
            for (id, index) in &entries {
                if state
                    .jobs
                    .iter()
                    .find(|j| j.id == *id)
                    .unwrap()
                    .entries
                    .get(*index)
                    .is_none()
                {
                    return Err("The selected row no longer exists".into());
                }
            }
            for job in &mut state.jobs {
                if jobs.contains(&job.id) {
                    if cancel_only {
                        // Une interruption récupérable reste annulable bien que son état soit terminal.
                        if job.resume_pending && job.state == TransferState::Error {
                            job.resume_pending = false;
                            job.cancel = true;
                            job.state = TransferState::Cancelled;
                            for entry in &mut job.entries {
                                if entry.interrupted {
                                    entry.state = TransferState::Cancelled;
                                }
                            }
                        }
                        if !terminal(&job.state) {
                            job.cancel = true;
                            job.resume_pending = false;
                            if job.state == TransferState::Scanning {
                                job.state = TransferState::Cancelled;
                            }
                        }
                        continue;
                    }
                    if restore
                        && (force_restore || terminal(&job.state))
                        && job.state != TransferState::Undone
                    {
                        job.cancel = true;
                        job.resume_pending = false;
                        job.retry_entries = None;
                        job.retry_all = false;
                        job.undo_requested = true;
                        job.undo_entries = None;
                        if terminal(&job.state) || job.state == TransferState::Scanning {
                            job.state = TransferState::UndoWaiting;
                        }
                        continue;
                    }
                    if terminal(&job.state) || ["waiting", "scanning"].contains(&job.state.as_str())
                    {
                        remove.insert(job.id);
                    } else {
                        job.cancel = true;
                        job.resume_pending = false;
                    }
                    continue;
                }
                let mut selected_entries = entries
                    .iter()
                    .filter(|(id, _)| *id == job.id)
                    .map(|(_, i)| *i)
                    .collect::<HashSet<_>>();
                if selected_entries.is_empty() {
                    continue;
                }
                let directories = selected_entries
                    .iter()
                    .copied()
                    .filter(|index| job.entries[*index].kind == "directory")
                    .collect::<HashSet<_>>();
                controls::expand_directories(job, &directories, &mut selected_entries);
                let stopped = terminal(&job.state);
                let interrupted = job.resume_pending && job.state == TransferState::Error;
                let mut undo_indices = Vec::new();
                for (index, entry) in job.entries.iter_mut().enumerate() {
                    if !selected_entries.contains(&index) {
                        continue;
                    }
                    if cancel_only {
                        if interrupted && entry.interrupted && entry.state == TransferState::Error {
                            entry.cancel = true;
                            entry.state = TransferState::Cancelled;
                        }
                        if !terminal(&entry.state) && entry.state != TransferState::Skipped {
                            entry.cancel = true;
                            if entry.state == TransferState::Waiting {
                                entry.state = TransferState::Cancelled;
                            }
                        }
                    } else if force_restore && entry.state != TransferState::Undone {
                        entry.cancel = true;
                        undo_indices.push(index);
                    } else if stopped
                        || terminal(&entry.state)
                        || entry.state == TransferState::Skipped
                    {
                        if restore && entry.state != TransferState::Undone {
                            undo_indices.push(index);
                        } else {
                            // Masquer la ligne ne supprime pas le fichier source ni un résultat déjà publié.
                            entry.hidden = true;
                        }
                    } else if entry.state == TransferState::Waiting {
                        entry.cancel = true;
                        entry.hidden = true;
                        entry.state = TransferState::Cancelled;
                    } else {
                        entry.cancel = true;
                    }
                }
                // Une sélection partielle laisse les autres fichiers récupérables ; le dernier annule le lot.
                if cancel_only
                    && interrupted
                    && !job.entries.iter().any(|entry| {
                        entry.interrupted
                            && entry.state == TransferState::Error
                            && !entry.cancel
                            && !entry.hidden
                    })
                {
                    job.resume_pending = false;
                    job.cancel = true;
                    job.state = TransferState::Cancelled;
                }
                if !undo_indices.is_empty() {
                    job.undo_entries = Some(undo_indices);
                    job.undo_requested = true;
                    if terminal(&job.state) {
                        job.state = TransferState::UndoWaiting;
                    }
                }
                if !cancel_only
                    && !job.undo_requested
                    && !job.entries.is_empty()
                    && job.entries.iter().all(|e| e.hidden)
                {
                    if terminal(&job.state) || job.state == TransferState::Waiting {
                        remove.insert(job.id);
                    } else {
                        job.cancel = true;
                        job.resume_pending = false;
                    }
                }
            }
            // Pending jobs have no filesystem effects. Reserve their cancellation before
            // removing them so a concurrent planner also observes the missing job.
            for job in &mut state.jobs {
                if remove.contains(&job.id) && !terminal(&job.state) {
                    job.cancel = true;
                    job.resume_pending = false;
                    job.state = TransferState::Cancelled;
                }
            }
        }
        self.discard_jobs(Some(&remove))?;
        self.save()?;
        self.shared.wake.notify_all();
        Ok(json!({"ok":true}))
    }
    /// Retire les lots terminés devenus entièrement invisibles et libère leur journal.
    /// Un échec de nettoyage redevient une erreur visible au lieu d’être perdu.
    pub(super) fn prune_hidden_jobs(&self) -> Result<(), String> {
        let ids = self
            .shared
            .state
            .lock()
            .unwrap()
            .jobs
            .iter()
            .filter(|j| {
                terminal(&j.state)
                    && !j.undo_requested
                    && !j.entries.is_empty()
                    && j.entries.iter().all(|e| e.hidden)
            })
            .map(|j| j.id)
            .collect::<HashSet<_>>();
        if ids.is_empty() {
            return Ok(());
        }
        if let Err(error) = self.discard_jobs(Some(&ids)) {
            for id in ids {
                self.update(id, |j| {
                    j.state = TransferState::Error;
                    j.error = format!("Nettoyage incomplet : {error}");
                });
            }
            return Err(error);
        }
        Ok(())
    }
}
