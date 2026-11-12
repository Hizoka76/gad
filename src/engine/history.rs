//! Persistance du journal et orchestration des remises en état.
//! Le journal décrit les étapes avant les changements de fichiers pour permettre
//! une récupération après interruption ; le chargement ne relance jamais une copie.

use super::*;

impl Engine {
    /// Résout le dossier XDG du journal Gad.
    pub fn desktop() -> Result<Self, String> {
        let data = std::env::var_os("XDG_DATA_HOME")
            .map(PathBuf::from)
            // Comme Qt et les scripts d’installation, ignorer une valeur XDG vide ou relative.
            .filter(|path| path.is_absolute())
            .or_else(|| {
                std::env::var_os("HOME")
                    .map(PathBuf::from)
                    .filter(|home| home.is_absolute())
                    .map(|home| home.join(".local/share"))
            })
            .ok_or("User data folder missing")?;
        let path = data.join("gad/state.json");
        Self::persistent(path)
    }
    /// Charge les opérations et transforme celles interrompues en erreurs récupérables.
    /// Nettoie l’historique non conservé uniquement si ses sauvegardes peuvent être libérées.
    pub(super) fn persistent(path: PathBuf) -> Result<Self, String> {
        let (mut state, sequence) = journal::load(&path)?;
        let mut kept = vec![];
        let mut resume_ids = Vec::new();
        for mut job in state.jobs {
            // Une demande jamais validée n’est pas un transfert interrompu à reprendre.
            if job.start_required && !terminal(&job.state) && !job.cancel && !job.undo_requested {
                job.resume_pending = false;
                job.recovery = false;
                if job.state == TransferState::Scanning {
                    job.state = TransferState::Error;
                    job.error = "File preparation was interrupted. Add the sources again.".into();
                }
                kept.push(job);
                continue;
            }
            if !terminal(&job.state) {
                job.resume_pending = job.resume_enabled
                    && !job.cancel
                    && !job.undo_requested
                    && job.retry_entries.is_none()
                    && !job.entries.iter().any(|entry| {
                        !terminal(&entry.state)
                            && entry.undo.as_ref().is_some_and(|record| {
                                record.directory_move
                                    || record.source_dir_removed
                                    || record.source_removed
                                    || record.rename_move
                                    || record.backup.is_some()
                                    || (entry.kind != "directory" && record.result.is_some())
                            })
                    })
                    && !matches!(
                        job.state,
                        TransferState::Undoing | TransferState::UndoWaiting
                    );
                job.recovery = true;
                job.state = TransferState::Error;
                job.cancel = true;
                job.undo_requested = false;
                job.error = if job.resume_pending {
                    "Transfer interrupted on last shutdown. Resume is available from the context menu after verifying the partial file".into()
                } else {
                    "Transfer interrupted on last shutdown. Use Cancellation → Cancel, delete, and move back to restore".into()
                };
                for e in &mut job.entries {
                    if !terminal(&e.state) && e.state != TransferState::Skipped {
                        e.interrupted = true;
                        e.resume_checking = false;
                        e.resume_checked = 0;
                        e.state = TransferState::Error;
                    }
                }
            } else if !state.keep_history && !job.recovery {
                let result = job.entries.iter().try_for_each(undo::discard);
                if let Err(error) = result {
                    job.recovery = true;
                    job.state = TransferState::Error;
                    job.error = error;
                    kept.push(job);
                }
                continue;
            }
            if state.resume_interrupted
                && job.recovery
                && job.resume_pending
                && job.state == TransferState::Error
            {
                resume_ids.push(job.id);
            }
            kept.push(job);
        }
        state.jobs = kept;
        state.next_id = state
            .next_id
            .max(state.jobs.iter().map(|j| j.id).max().unwrap_or(0));
        let engine = Self::unstarted(state, Some(path));
        engine.initialize_history(sequence)?;
        for id in resume_ids {
            if let Err(error) = engine.resume_interrupted(&[id]) {
                engine.update(id, |job| {
                    job.error = format!("Reprise non démarrée : {error}")
                });
            }
        }
        Ok(engine)
    }
    /// L’étape initiale possède encore la seule poignée externe et peut échouer sans fuite.
    pub(super) fn initialize_history(&self, sequence: u64) -> Result<(), String> {
        self.shared.writer.lock().unwrap().sequence = sequence;
        self.save()?;
        self.start_workers();
        Ok(())
    }
    /// Persiste seulement les lots modifiés, sans réduire les frontières de durabilité.
    pub(super) fn save(&self) -> Result<(), String> {
        self.persist(false)
    }
    /// La fermeture produit aussi un instantané autonome, après l’arrêt des travailleurs.
    pub(super) fn checkpoint_history(&self) -> Result<(), String> {
        self.persist(true)
    }
    fn persist(&self, checkpoint: bool) -> Result<(), String> {
        let Some(path) = &self.shared.history_path else {
            return Ok(());
        };
        let mut writer = self
            .shared
            .writer
            .lock()
            .map_err(|_| "Verrou du journal indisponible")?;
        let state = self
            .shared
            .state
            .lock()
            .map_err(|_| "Engine state unavailable")?;
        writer.save(path, &state, checkpoint)
    }
    /// Remplace une entrée à son indice stable et persiste immédiatement sa nouvelle étape.
    pub(super) fn store_entry(&self, id: u64, index: usize, entry: Entry) -> Result<(), String> {
        self.update(id, |j| j.entries[index] = entry);
        self.save()
    }
    /// Persiste le journal d’une entrée avant l’effet de fichier qui en dépend.
    pub(super) fn record(&self, id: u64, index: usize, record: undo::Record) -> Result<(), String> {
        self.update(id, |j| j.entries[index].undo = Some(record));
        self.save()
    }
    /// Libère les journaux des lots terminés sans supprimer leurs résultats finaux.
    pub(super) fn discard_history(&self) -> Result<(), String> {
        self.discard_jobs(None)
    }
    /// Nettoie uniquement les lots terminés sélectionnés, hors travailleurs actifs
    /// et restaurations demandées. Le verrou de maintenance protège leurs sauvegardes.
    pub(super) fn discard_jobs(
        &self,
        selected: Option<&std::collections::HashSet<u64>>,
    ) -> Result<(), String> {
        // Une annulation sans historique à retirer ne doit jamais attendre sa propre restauration.
        if selected.is_some_and(std::collections::HashSet::is_empty) {
            return Ok(());
        }
        // Le nettoyage et la restauration ne doivent pas libérer les mêmes sauvegardes en parallèle.
        let _maintenance = self.shared.maintenance.lock().unwrap();
        let completed = {
            let state = self.shared.state.lock().unwrap();
            state
                .jobs
                .iter()
                .filter(|j| {
                    terminal(&j.state)
                        && !state.active.contains_key(&j.id)
                        && !j.undo_requested
                        && selected.is_none_or(|ids| ids.contains(&j.id))
                })
                .cloned()
                .collect::<Vec<_>>()
        };
        for job in &completed {
            for entry in &job.entries {
                undo::discard(entry)?;
            }
        }
        let ids = completed
            .iter()
            .map(|j| j.id)
            .collect::<std::collections::HashSet<_>>();
        self.shared
            .state
            .lock()
            .unwrap()
            .jobs
            .retain(|j| !ids.contains(&j.id));
        self.save()
    }
    /// Valide les résultats, recrée les dossiers source, restaure les fichiers
    /// puis nettoie les dossiers cible vides. Une sélection partielle garde ses voisins ;
    /// chaque étape est sauvegardée pour permettre une nouvelle tentative après erreur.
    pub(super) fn rollback(&self, mut job: Job) {
        let _maintenance = self.shared.maintenance.lock().unwrap();
        self.update(job.id, |j| {
            j.state = TransferState::Undoing;
            j.error.clear();
        });
        let selected = |index: usize| {
            job.undo_entries
                .as_ref()
                .is_none_or(|indices| indices.contains(&index))
        };
        let indices = (0..job.entries.len())
            .filter(|i| selected(*i))
            .collect::<std::collections::HashSet<_>>();
        let retry = job.retry_entries.is_some();
        // Une restauration isolée de fichier peut nécessiter de recréer ses parents source supprimés.
        let ancestors = job
            .entries
            .iter()
            .enumerate()
            .filter(|(_, e)| {
                e.kind == "directory"
                    && (indices.iter().any(|i| {
                        job.entries[*i].source.starts_with(&e.source)
                            && job.entries[*i].destination.starts_with(&e.destination)
                    }) || job.retry_entries.as_ref().is_some_and(|indices| {
                        indices.iter().any(|i| {
                            job.entries[*i].source.starts_with(&e.source)
                                && job.entries[*i].destination.starts_with(&e.destination)
                        })
                    }))
            })
            .map(|(i, _)| i)
            .collect::<std::collections::HashSet<_>>();
        let partial = job.undo_entries.is_some();
        let result = (|| {
            self.save()?;
            // Indexer les sauvegardes par destination évite de parcourir tout le lot pour chaque fichier.
            let mut replacements: std::collections::HashMap<&Path, Vec<(usize, &PathBuf)>> =
                std::collections::HashMap::new();
            for (index, entry) in job.entries.iter().enumerate() {
                if indices.contains(&index) {
                    if let Some(record) = entry.undo.as_ref().filter(|record| !record.reverted) {
                        if let Some(backup) = &record.backup {
                            replacements
                                .entry(&entry.destination)
                                .or_default()
                                .push((index, backup));
                        }
                    }
                }
            }
            // Validate every output before altering any file. Later jobs are rolled back first.
            for (index, entry) in job.entries.iter().enumerate() {
                if indices.contains(&index) || ancestors.contains(&index) {
                    // Un fichier du même lot peut avoir été remplacé par une entrée suivante.
                    // Son résultat se trouve alors dans la sauvegarde de cette entrée, qui doit
                    // faire partie de la restauration. Le contrôle de contenu reste obligatoire.
                    let protected_output = replacements
                        .get(entry.destination.as_path())
                        .into_iter()
                        .flatten()
                        .filter(|(later, _)| *later > index)
                        .map(|(_, backup)| *backup)
                        .find(|backup| {
                            entry
                                .undo
                                .as_ref()
                                .and_then(|record| record.result.as_ref())
                                .is_some_and(|stamp| stamp.matches(backup))
                        });
                    if let Some(output) = protected_output {
                        let mut protected_entry = entry.clone();
                        protected_entry.destination = output.clone();
                        undo::preflight(&protected_entry)?;
                    } else {
                        undo::preflight(entry)?;
                    }
                }
            }
            self.restore_trees(&mut job, &indices)?;
            for (index, entry) in job.entries.iter_mut().enumerate() {
                if entry.kind == "directory"
                    && (indices.contains(&index) || ancestors.contains(&index))
                {
                    undo::restore_directory(entry, &mut |e| {
                        self.store_entry(job.id, index, e.clone())
                    })?;
                }
            }
            // Les fichiers sont restaurés en sens inverse pour défaire les effets dans l’ordre opposé.
            for (index, entry) in job.entries.iter_mut().enumerate().rev() {
                if entry.kind != "directory"
                    && indices.contains(&index)
                    && entry.state != TransferState::Undone
                {
                    entry.state = TransferState::Undoing;
                    self.store_entry(job.id, index, entry.clone())?;
                    undo::restore_file(entry, &mut |e| self.store_entry(job.id, index, e.clone()))?;
                }
            }
            for (index, entry) in job.entries.iter_mut().enumerate().rev() {
                if entry.kind == "directory" && indices.contains(&index) {
                    undo::finish_directory(entry, &mut |e| {
                        self.store_entry(job.id, index, e.clone())
                    })?;
                }
            }
            for (index, entry) in job.entries.iter().enumerate().rev() {
                if ancestors.contains(&index) && !indices.contains(&index) {
                    undo::restore_parent_metadata(entry)?;
                }
            }
            Ok::<(), String>(())
        })();
        let result = result.and_then(|()| {
            if retry {
                self.prepare_retry(&mut job)?;
            }
            Ok(())
        });
        if retry && result.is_ok() {
            return;
        }
        self.update(job.id, |j| {
            j.undo_requested = false;
            if let Err(error) = result {
                j.state = TransferState::UndoError;
                j.error = format!("Remise en état interrompue : {error}");
                for entry in &mut j.entries {
                    if entry.state == TransferState::Undoing {
                        entry.state = TransferState::Error;
                        entry.error = error.clone();
                    }
                }
                for index in &indices {
                    j.entries[*index].error = error.clone();
                    if j.entries[*index].state != TransferState::Undone {
                        j.entries[*index].state = TransferState::Error;
                    }
                }
            } else {
                j.undo_entries = None;
                j.state = if partial
                    && j.entries.iter().any(|e| {
                        e.state != TransferState::Undone && e.state != TransferState::Skipped
                    }) {
                    if j.entries.iter().any(|e| {
                        e.state == TransferState::Cancelled || e.state == TransferState::Error
                    }) {
                        TransferState::Cancelled
                    } else {
                        TransferState::Done
                    }
                } else {
                    TransferState::Undone
                };
                j.error.clear();
                j.recovery = false;
            }
        });
        if let Err(error) = self.save() {
            self.update(job.id, |j| j.error = error);
        }
    }
}
