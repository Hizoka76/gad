//! Validation et analyse hors du thread Qt ; aucun effet de copie avant validation complète.
use super::*;

type Prepared = (PathBuf, Vec<Entry>, scheduling::Disks);
/// Une préparation travaille sur un instantané ; les commandes reçues entre-temps gardent leurs effets.
pub(super) fn preserve_controls(entries: &mut [Entry], previous: &[Entry]) {
    // Une reconnexion ou un filtrage peut décaler les indices de l’analyse précédente.
    let mut controls = std::collections::HashMap::new();
    let mut next = vec![usize::MAX; previous.len()];
    // Chaîner les occurrences dans leur ordre initial, sans allocation par chemin unique.
    for (index, entry) in previous.iter().enumerate().rev() {
        next[index] = controls
            .insert(
                (
                    entry.source.as_path(),
                    entry.destination.as_path(),
                    entry.kind.as_str(),
                ),
                index,
            )
            .unwrap_or(usize::MAX);
    }
    for entry in entries {
        let Some(index) = controls.get_mut(&(
            entry.source.as_path(),
            entry.destination.as_path(),
            entry.kind.as_str(),
        )) else {
            continue;
        };
        if *index == usize::MAX {
            continue;
        }
        let old = &previous[*index];
        *index = next[*index];
        entry.paused = old.paused;
        entry.hidden |= old.hidden;
        entry.cancel |= old.cancel;
        if entry.cancel && !terminal(&entry.state) {
            entry.state = TransferState::Cancelled;
        }
    }
}
impl Engine {
    /// Le dossier cible et toutes les racines sont contrôlés dans le travailleur de préparation.
    pub(super) fn validate_roots(
        &self,
        id: u64,
        sources: &[PathBuf],
        destination: &Path,
    ) -> Result<PathBuf, String> {
        self.checkpoint_preparation(id)?;
        let destination =
            fs::canonicalize(destination).map_err(|e| format!("Destination : {e}"))?;
        if !destination.is_dir() {
            return Err("The destination must be an existing folder".into());
        }
        let allow_duplication = self
            .shared
            .state
            .lock()
            .unwrap()
            .jobs
            .iter()
            .find(|j| j.id == id)
            .is_some_and(|j| j.allow_duplication && j.mode == "copy");
        // Contrôler toutes les racines avant toute copie, sans suivre les liens source.
        for source in sources {
            self.checkpoint_preparation(id)?;
            let meta =
                fs::symlink_metadata(source).map_err(|e| format!("{} : {e}", source.display()))?;
            let name = source
                .file_name()
                .ok_or("Cannot copy the filesystem root")?;
            if allow_duplication && duplication::same_target(source, &destination.join(name)) {
                continue;
            }
            if fast_move::invalid_alias(source, &destination.join(name), meta.is_dir()) {
                return Err(
                    "Source and destination are identical or nested through their mounts".into(),
                );
            }
            if !meta.file_type().is_symlink() {
                let canonical = fs::canonicalize(source).map_err(|e| e.to_string())?;
                if meta.is_dir() && destination.starts_with(&canonical) {
                    return Err("A folder cannot be copied into itself".into());
                }
                if destination.join(name) == canonical {
                    return Err("La source et la destination sont identiques".into());
                }
            } else {
                // Normaliser seulement le parent : le lien lui-même, même pendant, ne doit jamais être suivi.
                let parent = fs::canonicalize(source.parent().ok_or("Dossier source absent")?)
                    .map_err(|e| e.to_string())?;
                if parent.join(name) == destination.join(name) {
                    return Err("La source et la destination sont identiques".into());
                }
            }
        }

        let connections = reconnect::capture_paths(
            sources
                .iter()
                .cloned()
                .chain(std::iter::once(destination.clone())),
        );
        self.update(id, |job| {
            if job.connections.is_empty() {
                job.connections = connections;
            }
        });
        Ok(destination)
    }
    /// Une préparation annulée ou remplacée ne doit plus enrichir le même identifiant de lot.
    pub(super) fn checkpoint_preparation(&self, id: u64) -> Result<(), String> {
        let state = self.shared.state.lock().unwrap();
        if self.shared.stop.load(std::sync::atomic::Ordering::SeqCst)
            || state
                .jobs
                .iter()
                .find(|job| job.id == id)
                .is_none_or(|job| {
                    job.cancel
                        || !matches!(
                            job.state,
                            TransferState::Scanning | TransferState::DeviceWaiting
                        )
                })
        {
            return Err("Canceled".into());
        }
        Ok(())
    }
    /// Le résultat tardif d’une préparation annulée est ignoré ; les suivants conservent leur ordre.
    /// Le paramètre de travail permet de tester une attente de métadonnées sans montage réel.
    pub(super) fn spawn_preparation(
        &self,
        id: u64,
        background: bool,
        pause_epoch: u64,
        prepare: impl FnOnce() -> Result<Prepared, String> + Send + 'static,
    ) -> thread::JoinHandle<()> {
        let engine = self.clone();
        let activity = completion::Activity::new(self.clone(), true);
        thread::spawn(move || {
            let _preparation = activity;
            let result = prepare().and_then(|(destination, entries, disks)| {
                let filtered = engine.filter_duplicates(id, entries)?;
                let entries = filtered.entries;
                let disks = if filtered.removed > 0 {
                    scheduling::disks_for_entries(&entries)
                } else {
                    disks
                };
                let options = engine
                    .shared
                    .state
                    .lock()
                    .unwrap()
                    .jobs
                    .iter()
                    .find(|job| job.id == id)
                    .map(|job| (job.mode == "move", job.verify_md5));
                let (fast, paths) = options
                    .map(|(moving, md5)| {
                        let paths = fast_move::reservations(&entries);
                        (
                            paths.is_some() && fast_move::eligible_for_job(&entries, moving, md5),
                            paths,
                        )
                    })
                    .unwrap_or((false, None));
                // Le montage est inspecté hors verrou ; une attente réseau ne bloque pas les commandes Qt.
                let durable_resume = !fs::File::open(&destination)
                    .is_ok_and(|file| fast_move::descriptor_is_kio_fuse(&file));
                let known = engine
                    .shared
                    .state
                    .lock()
                    .unwrap()
                    .jobs
                    .iter()
                    .find(|j| j.id == id)
                    .is_some_and(|j| !j.connections.is_empty());
                let connections = if known {
                    Vec::new()
                } else {
                    reconnect::capture(&entries)
                };
                {
                    let keys = filtered.target_keys.or_else(|| {
                        fast_move::destination_keys(entries.iter().map(|e| e.destination.clone()))
                    });
                    Ok((
                        destination,
                        entries,
                        disks,
                        fast,
                        paths,
                        durable_resume,
                        keys,
                        filtered.removed,
                        connections,
                    ))
                }
            });
            if let Err(error) = &result {
                let index = engine
                    .shared
                    .state
                    .lock()
                    .unwrap()
                    .jobs
                    .iter()
                    .find(|j| j.id == id && !j.connections.is_empty())
                    .and_then(|j| {
                        j.entries
                            .iter()
                            .position(|e| {
                                !e.cancel && !e.hidden && (e.interrupted || !terminal(&e.state))
                            })
                            .or_else(|| j.entries.is_empty().then_some(0))
                    });
                if let Some(index) = index {
                    if engine.park_disconnected(id, index, error) {
                        return;
                    }
                }
            }
            {
                let mut state = engine.shared.state.lock().unwrap();
                if state
                    .jobs
                    .iter()
                    .find(|job| job.id == id)
                    .is_none_or(|job| job.cancel || job.state != TransferState::Scanning)
                    || engine.shared.stop.load(std::sync::atomic::Ordering::SeqCst)
                {
                    return;
                }
                let resume = background
                    && matches!(&result,Ok((_,entries,..)) if !entries.is_empty())
                    && state.pause_epoch == pause_epoch;
                if let Ok((_, entries, _, _, _, _, _, removed, _)) = &result {
                    state.duplicates_ignored = state.duplicates_ignored.saturating_add(*removed);
                    if *removed > 0
                        && (entries.is_empty()
                            || entries
                                .iter()
                                .all(|e| e.hidden && e.state == TransferState::Skipped))
                    {
                        // Un collage entièrement dupliqué peut démarrer les fichiers
                        // déjà préparés, sans lever une pause explicite de la file.
                        let automatic = state
                            .jobs
                            .iter()
                            .find(|j| j.id == id)
                            .is_some_and(|j| !j.start_required);
                        if automatic {
                            for job in &mut state.jobs {
                                if job.start_required {
                                    job.start_required = false;
                                }
                            }
                        }
                        state.jobs.retain(|j| j.id != id);
                        state.row_layout = None;
                        drop(state);
                        let _ = engine.save();
                        engine.shared.wake.notify_all();
                        return;
                    }
                }
                let Some(job) = state.jobs.iter_mut().find(|job| job.id == id) else {
                    return;
                };
                if job.cancel
                    || job.state != TransferState::Scanning
                    || engine.shared.stop.load(std::sync::atomic::Ordering::SeqCst)
                {
                    return;
                }
                match result {
                    Ok((
                        destination,
                        mut entries,
                        disks,
                        fast,
                        paths,
                        durable_resume,
                        keys,
                        _,
                        connections,
                    )) => {
                        // Même un chemin de montage saisi directement ne doit pas promettre de points durables sur KIO Fuse.
                        if !durable_resume {
                            job.resume_enabled = false;
                        }
                        if job.connections.is_empty() {
                            job.connections = connections;
                        }
                        job.error.clear();
                        job.destination = destination;
                        preserve_controls(&mut entries, &job.entries);
                        job.entries = entries;
                        job.disks = disks;
                        job.fast_move = fast;
                        job.paths = paths;
                        job.destination_keys = keys;
                        job.state = if job.entries.is_empty() {
                            TransferState::Done
                        } else {
                            TransferState::Waiting
                        };
                    }
                    Err(error) => {
                        job.error = error;
                        job.state = TransferState::Error;
                    }
                }
                let automatic = !job.start_required && job.state == TransferState::Waiting;
                if automatic {
                    for job in &mut state.jobs {
                        if job.start_required {
                            job.start_required = false;
                        }
                    }
                }
                state.row_layout = None;
                // Une pause demandée après le collage est prioritaire sur son lancement automatique.
                if resume {
                    state.paused = false;
                }
            }
            if let Err(error) = engine.save() {
                engine.update(id, |job| {
                    job.state = TransferState::Error;
                    job.error = error;
                });
            }
            engine.shared.wake.notify_all();
        })
    }
}
