//! Renommage atomique d’un dossier complet : un seul changement de nom et un journal groupé.
//! Les sélections partielles et les arbres modifiés conservent le parcours par entrée.
use super::*;
use std::collections::{HashMap, HashSet};

/// Les descendants sont contigus dans l’ordre de l’analyse, parents avant enfants.
fn span(entries: &[Entry], start: usize) -> std::ops::Range<usize> {
    let root = &entries[start].source;
    let count = entries[start..]
        .iter()
        .take_while(|entry| entry.source.starts_with(root))
        .count();
    start..start + count
}

/// Vérifie le contenu réel sans lire les fichiers ni suivre les liens symboliques.
/// Un élément ajouté, retiré ou remplacé interdit de traiter l’arbre comme un bloc.
fn exact_tree(entries: &[Entry], destination: bool) -> bool {
    inspect_tree(entries, destination, |_, _| true)
}

/// Le même parcours vérifie les noms, identités et métadonnées, sans relire chaque entrée séparément.
fn inspect_tree(
    entries: &[Entry],
    destination: bool,
    mut check: impl FnMut(usize, &fs::Metadata) -> bool,
) -> bool {
    let expected: HashMap<_, _> = entries
        .iter()
        .enumerate()
        .map(|(index, entry)| {
            (
                if destination {
                    entry.destination.as_path()
                } else {
                    entry.source.as_path()
                },
                (index, entry),
            )
        })
        .collect();
    if expected.len() != entries.len() {
        return false;
    }
    let Some(root) = entries.first() else {
        return false;
    };
    let mut pending = vec![if destination {
        root.destination.clone()
    } else {
        root.source.clone()
    }];
    let mut visited = 0;
    while let Some(path) = pending.pop() {
        let Some((index, entry)) = expected.get(path.as_path()) else {
            return false;
        };
        let Ok(meta) = fs::symlink_metadata(&path) else {
            return false;
        };
        if (meta.dev(), meta.ino()) != entry.identity
            || source_kind(&meta) != Some(entry.kind.as_str())
            || !check(*index, &meta)
        {
            return false;
        }
        visited += 1;
        if meta.is_dir() {
            let Ok(children) = fs::read_dir(&path) else {
                return false;
            };
            for child in children {
                let Ok(child) = child else { return false };
                pending.push(child.path());
            }
        }
    }
    visited == entries.len()
}

/// Une pause ou annulation d’enfant doit rester respectée : pas de renommage global dans ce cas.
fn ready(job: &Job, range: std::ops::Range<usize>, virgin: bool) -> bool {
    // exact_tree compare l’arbre réel aux éléments sélectionnés : un filtre sans effet ne bloque pas le renommage.
    job.fast_move
        && !job.cancel
        && !job.paused
        && job.entries[range.clone()]
            .iter()
            .enumerate()
            .all(|(offset, entry)| {
                !entry.paused
                    && !entry.cancel
                    && !entry.hidden
                    && (entry.state == TransferState::Waiting
                        || (offset == 0 && entry.state == TransferState::Running))
                    && (!virgin || entry.undo.is_none())
            })
}

impl Engine {
    /// Abandonner une préparation de bloc n’a aucun effet sur les fichiers ; ses journaux étaient vierges.
    fn clear_tree_plan(&self, id: u64, range: std::ops::Range<usize>) -> Result<(), String> {
        self.update(id, |job| {
            for entry in &mut job.entries[range] {
                entry.undo = None;
            }
        });
        self.save()
    }

    /// Prépare toutes les identités dans un seul état durable avant l’unique renommage du dossier.
    /// Les fichiers conservent leur ctime : même un arrêt juste après le renommage reste interprétable.
    pub(super) fn rename_tree(
        &self,
        id: u64,
        start: usize,
    ) -> Result<Option<EntryOutcome>, String> {
        self.rename_tree_using(id, start, fast_move::rename_local)
    }
    /// Primitive injectable pour contrôler les modifications survenant juste après le renommage.
    pub(super) fn rename_tree_using(
        &self,
        id: u64,
        start: usize,
        rename: impl FnOnce(&Path, &Path) -> std::io::Result<()>,
    ) -> Result<Option<EntryOutcome>, String> {
        let (range, tree) = {
            let state = self.shared.state.lock().unwrap();
            let job = state
                .jobs
                .iter()
                .find(|job| job.id == id)
                .ok_or("Operation absent")?;
            if job.entries[start].kind != "directory" {
                return Ok(None);
            }
            let range = span(&job.entries, start);
            if !ready(job, range.clone(), true) {
                return Ok(None);
            }
            (range.clone(), job.entries[range].to_vec())
        };
        let root = &tree[0];
        let mut metadata = vec![None; tree.len()];
        if fs::symlink_metadata(&root.destination).is_ok()
            || !fast_move::tree_possible(&root.source, &root.destination)
            || !inspect_tree(&tree, false, |index, meta| {
                metadata[index] = Some(meta.clone());
                true
            })
        {
            return Ok(None);
        }
        let properties_enabled = self
            .shared
            .state
            .lock()
            .unwrap()
            .jobs
            .iter()
            .any(|job| job.id == id && job.verify_properties);
        let mut property_snapshots = Vec::new();
        if properties_enabled {
            let mut memory = 0usize;
            for (offset, entry) in tree.iter().enumerate() {
                self.checkpoint_entry(id, start)?;
                let snapshot = properties::Snapshot::read(&entry.source);
                if !snapshot.complete() {
                    self.report_properties(id, start + offset, &snapshot, &entry.source);
                    return Err(
                        "Move stopped: source properties cannot be verified; tree kept".into(),
                    );
                }
                memory = memory.saturating_add(snapshot.memory_usage());
                // Au-delà de ce budget, contrôler les entrées individuellement sans conserver toutes leurs valeurs en mémoire.
                if memory > 32 * 1024 * 1024 {
                    return Ok(None);
                }
                property_snapshots.push(snapshot);
            }
        }
        let mut plan = Vec::with_capacity(tree.len());
        for (offset, entry) in tree.iter().enumerate() {
            self.checkpoint_entry(id, start)?;
            let changed = {
                let state = self.shared.state.lock().unwrap();
                state
                    .jobs
                    .iter()
                    .find(|job| job.id == id)
                    .is_none_or(|job| {
                        let current = &job.entries[start + offset];
                        current.paused || current.cancel || current.hidden
                    })
            };
            if changed {
                return Ok(None);
            }
            // L’identité a déjà été vérifiée dans le parcours exact, avant la sauvegarde du plan.
            let meta = metadata[offset].take().ok_or("Incomplete tree metadata")?;
            let stamp = undo::Stamp::from_metadata(&entry.source, &meta)?;
            let directory = entry.kind == "directory";
            plan.push((
                undo::Record {
                    directory_move: offset == 0,
                    rename_move: !directory,
                    rename_ctime: (!directory).then_some((meta.ctime(), meta.ctime_nsec())),
                    result: Some(stamp.clone()),
                    source_stamp: Some(stamp),
                    source_removed: !directory,
                    source_dir_removed: directory,
                    ..Default::default()
                },
                meta,
            ));
        }
        drop(metadata);
        // Contrôler à nouveau le lot après les lectures de métadonnées, sans verrou pendant les accès disque.
        {
            let mut state = self.shared.state.lock().unwrap();
            let job = state
                .jobs
                .iter_mut()
                .find(|job| job.id == id)
                .ok_or("Operation absent")?;
            if !ready(job, range.clone(), true) {
                return Ok(None);
            }
            for (entry, (record, meta)) in job.entries[range.clone()].iter_mut().zip(&plan) {
                entry.undo = Some(record.clone());
                if entry.kind == "file" {
                    entry.size = meta.len();
                }
            }
        }
        self.save()?;
        self.checkpoint_entry(id, start)?;
        let still_ready = {
            let state = self.shared.state.lock().unwrap();
            state
                .jobs
                .iter()
                .find(|job| job.id == id)
                .is_some_and(|job| ready(job, range.clone(), false))
        };
        if !still_ready
            || !inspect_tree(&tree, false, |index, now| {
                let before = &plan[index].1;
                same_version(before, now)
                    && (before.ctime(), before.ctime_nsec()) == (now.ctime(), now.ctime_nsec())
            })
        {
            self.clear_tree_plan(id, range)?;
            return Ok(None);
        }
        match rename(&root.source, &root.destination) {
            Ok(()) => {
                fast_move::sync_names(&root.source, &root.destination)?;
                let expected = tree
                    .iter()
                    .zip(&plan)
                    .enumerate()
                    .map(|(offset, (entry, (record, meta)))| fast_verify::Expected {
                        entry,
                        stamp: record.source_stamp.as_ref().unwrap(),
                        ctime: (offset != 0).then_some((meta.ctime(), meta.ctime_nsec())),
                    })
                    .collect::<Vec<_>>();
                self.verify_fast_result(id, start, &expected)?;
                self.update(id, |job| {
                    for entry in &mut job.entries[range] {
                        entry.copied = entry.size;
                        entry.state = TransferState::Done;
                    }
                });
                for (offset, snapshot) in property_snapshots.iter().enumerate() {
                    self.report_properties(id, start + offset, snapshot, &tree[offset].destination);
                }
                self.save()?;
                Ok(Some(EntryOutcome::TreeComplete(start + tree.len())))
            }
            Err(error) => {
                self.clear_tree_plan(id, range.clone())?;
                if error.raw_os_error() == Some(18)
                    || error.kind() == std::io::ErrorKind::Unsupported
                {
                    self.update(id, |job| {
                        job.fast_move = false;
                        job.entries[start].state = TransferState::Waiting;
                    });
                    self.save()?;
                    Ok(Some(EntryOutcome::Requeue))
                } else if error.kind() == std::io::ErrorKind::AlreadyExists {
                    Ok(None) // La fusion et les conflits ordinaires gardent leur comportement habituel.
                } else {
                    Err(format!(
                        "Renommage du dossier impossible ; source conservée : {error}"
                    ))
                }
            }
        }
    }

    /// Une remise en état complète peut rendre le dossier entier à sa source, inode compris.
    /// Les arbres partiellement restaurés ou enrichis de fichiers externes restent traités par entrée.
    pub(super) fn restore_trees(
        &self,
        job: &mut Job,
        selected: &HashSet<usize>,
    ) -> Result<(), String> {
        for start in (0..job.entries.len()).rev() {
            if !job.entries[start]
                .undo
                .as_ref()
                .is_some_and(|record| record.directory_move && !record.reverted)
            {
                continue;
            }
            let range = span(&job.entries, start);
            if !range.clone().all(|index| selected.contains(&index)) {
                continue;
            }
            let tree = &job.entries[range.clone()];
            let root = &tree[0];
            if tree
                .iter()
                .any(|entry| entry.undo.as_ref().is_none_or(|record| record.reverted))
                || !fs::symlink_metadata(&root.source)
                    .is_err_and(|error| error.kind() == std::io::ErrorKind::NotFound)
                || !root.source.parent().is_some_and(|parent| parent.is_dir())
                || !fast_move::tree_possible(&root.destination, &root.source)
                || !exact_tree(tree, true)
            {
                continue;
            }
            self.save()?;
            fast_move::rename_local(&root.destination, &root.source).map_err(|error| {
                format!("Retour du dossier par renommage impossible ; résultat conservé : {error}")
            })?;
            fast_move::sync_names(&root.source, &root.destination)?;
            for entry in &mut job.entries[range.clone()] {
                let record = entry.undo.as_mut().unwrap();
                record.reverted = true;
                record.restored_source = record.result.clone();
                entry.state = TransferState::Undone;
                entry.cancel = false;
                entry.paused = false;
                entry.error.clear();
            }
            self.update(job.id, |current| {
                current.entries[range.clone()].clone_from_slice(&job.entries[range])
            });
            self.save()?;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn combined_tree_inspection_keeps_name_identity_and_version_checks() {
        struct Cleanup(PathBuf);
        impl Drop for Cleanup {
            fn drop(&mut self) {
                let _ = fs::remove_dir_all(&self.0);
            }
        }
        let fixture = Cleanup(std::env::temp_dir().join(format!(
            "gad-tree-inspect-{}-{}",
            std::process::id(),
            snapshot::touch()
        )));
        let source = fixture.0.join("source");
        let target = fixture.0.join("dest");
        fs::create_dir_all(&source).unwrap();
        fs::create_dir(&target).unwrap();
        let file = source.join("file");
        fs::write(&file, b"before").unwrap();
        let engine = Engine::new();
        engine.request(&json!({"command":"enqueue","prepare_only":true,"mode":"move","sources":[source],"destination":target}).to_string()).unwrap();
        let entries = (0..500)
            .find_map(|_| {
                let entries = {
                    let state = engine.shared.state.lock().unwrap();
                    state
                        .jobs
                        .first()
                        .filter(|job| job.state == TransferState::Waiting)
                        .map(|job| job.entries.clone())
                };
                if entries.is_none() {
                    thread::sleep(std::time::Duration::from_millis(10));
                }
                entries
            })
            .expect("prepared tree");
        let mut metadata = vec![None; entries.len()];
        assert!(inspect_tree(&entries, false, |index, meta| {
            metadata[index] = Some(meta.clone());
            true
        }));
        assert!(metadata.iter().all(Option::is_some));
        fs::write(&file, b"a changed file").unwrap();
        assert!(exact_tree(&entries, false));
        assert!(!inspect_tree(&entries, false, |index, meta| same_version(
            metadata[index].as_ref().unwrap(),
            meta
        )));
        fs::write(source.join("extra"), b"unselected").unwrap();
        assert!(!exact_tree(&entries, false));
        let mut duplicates = entries.clone();
        duplicates.push(entries[0].clone());
        assert!(!exact_tree(&duplicates, false));
        engine.request(r#"{"command":"shutdown"}"#).unwrap();
    }
}
