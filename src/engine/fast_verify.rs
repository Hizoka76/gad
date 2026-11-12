//! Vérification facultative après un renommage local, sans lecture du contenu des fichiers.
//! Le journal du déplacement reste disponible si le résultat a changé ; aucun retour automatique risqué.
use super::*;
use std::collections::{BTreeMap, HashSet};

#[derive(Clone, Serialize, Deserialize)]
pub(super) struct Report {
    pub status: String,
    pub path: Option<PathBuf>,
}

/// Ctime doit rester identique pour les descendants d’un dossier déplacé en bloc.
/// Le renommage de la racine change légitimement son propre ctime.
pub(super) struct Expected<'a> {
    pub entry: &'a Entry,
    pub stamp: &'a undo::Stamp,
    pub ctime: Option<(i64, i64)>,
}

impl Engine {
    /// Contrôler l’arbre exact et l’absence de l’ancienne racine, sans suivre de liens symboliques.
    pub(super) fn verify_fast_result(
        &self,
        id: u64,
        start: usize,
        expected: &[Expected<'_>],
    ) -> Result<(), String> {
        let enabled = self
            .shared
            .state
            .lock()
            .unwrap()
            .jobs
            .iter()
            .any(|job| job.id == id && job.verify_fast_move);
        if !enabled {
            return Ok(());
        }
        let root = expected
            .first()
            .ok_or("Fast-move verification failed; moved result kept.")?;
        let range = start..start + expected.len();
        self.update(id, |job| {
            for entry in &mut job.entries[range.clone()] {
                entry.fast_move_check = Some(Report {
                    status: "checking".into(),
                    path: None,
                });
            }
        });
        let mut checked_path = root.entry.destination.clone();
        let result = (|| {
            self.checkpoint_entry(id, start)?;
            // Les parents de l’ancienne racine existent toujours, contrairement à ceux de ses enfants.
            safe_parents(&root.entry.source)?;
            checked_path = root.entry.source.clone();
            if !fs::symlink_metadata(&root.entry.source)
                .is_err_and(|error| error.kind() == std::io::ErrorKind::NotFound)
            {
                return Err("Fast-move verification failed; moved result kept.".into());
            }
            let by_path = expected
                .iter()
                .enumerate()
                .map(|(offset, item)| (item.entry.destination.as_path(), (offset, item)))
                .collect::<BTreeMap<_, _>>();
            if by_path.len() != expected.len() {
                return Err("Fast-move verification failed; moved result kept.".into());
            }
            let mut pending = vec![root.entry.destination.clone()];
            let mut seen = HashSet::new();
            while let Some(path) = pending.pop() {
                checked_path = path.clone();
                let Some((offset, item)) = by_path.get(path.as_path()) else {
                    return Err("Fast-move verification failed; moved result kept.".into());
                };
                self.checkpoint_entry(id, start + offset)?;
                safe_parents(&path)?;
                let meta = fs::symlink_metadata(&path)
                    .map_err(|_| "Fast-move verification failed; moved result kept.")?;
                if !seen.insert(path.clone())
                    || !item.stamp.matches_fast_metadata(&path, &meta)
                    || (meta.dev(), meta.ino()) != item.entry.identity
                    || item
                        .ctime
                        .is_some_and(|ctime| ctime != (meta.ctime(), meta.ctime_nsec()))
                {
                    return Err("Fast-move verification failed; moved result kept.".into());
                }
                if item.entry.kind == "directory" {
                    for child in fs::read_dir(&path)
                        .map_err(|_| "Fast-move verification failed; moved result kept.")?
                    {
                        pending.push(
                            child
                                .map_err(|_| "Fast-move verification failed; moved result kept.")?
                                .path(),
                        );
                    }
                }
            }
            checked_path = root.entry.destination.clone();
            if seen.len() != expected.len() {
                return Err("Fast-move verification failed; moved result kept.".into());
            }
            checked_path = root.entry.source.clone();
            if !fs::symlink_metadata(&root.entry.source)
                .is_err_and(|error| error.kind() == std::io::ErrorKind::NotFound)
            {
                return Err("Fast-move verification failed; moved result kept.".into());
            }
            Ok(())
        })();
        let report = Report {
            status: if result.is_ok() { "ok" } else { "error" }.into(),
            path: result.as_ref().err().map(|_| checked_path),
        };
        self.update(id, |job| {
            for entry in &mut job.entries[range] {
                entry.fast_move_check = Some(report.clone());
            }
        });
        self.save()?;
        result
    }
}
