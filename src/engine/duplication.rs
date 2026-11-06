//! Une duplication garde sa destination attribuée aux reprises et à la restauration.
use super::*;

/// Comparer les parents évite de suivre un lien symbolique sélectionné.
pub(super) fn same_target(source: &Path, target: &Path) -> bool {
    source.file_name() == target.file_name()
        && (source
            .parent()
            .and_then(|p| fs::canonicalize(p).ok())
            .zip(target.parent().and_then(|p| fs::canonicalize(p).ok()))
            .is_some_and(|(a, b)| a == b)
            || fast_move::invalid_alias(source, target, false))
}

impl Engine {
    /// Choisit un nom absent du disque et des plans déjà réservés, y compris ceux en pause.
    pub(super) fn duplicate_target(
        &self,
        id: u64,
        source: &Path,
        target: &Path,
    ) -> Result<PathBuf, String> {
        {
            let state = self.shared.state.lock().unwrap();
            let job = state
                .jobs
                .iter()
                .find(|j| j.id == id)
                .ok_or("Operation absent")?;
            if let Some(path) = job.duplicate_targets.get(source) {
                return Ok(path.clone());
            }
            if !job.allow_duplication
                || job.mode != "copy"
                || !job.sources.iter().any(|s| s == source)
            {
                return Ok(target.into());
            }
        }
        if !same_target(source, target) {
            return Ok(target.into());
        }
        let directory = fs::symlink_metadata(source)
            .map_err(|e| e.to_string())?
            .is_dir();
        let name = source
            .file_name()
            .and_then(|s| s.to_str())
            .ok_or("Invalid copy source.")?;
        let (stem, extension) = if directory {
            (name, String::new())
        } else {
            (
                source.file_stem().and_then(|s| s.to_str()).unwrap_or(name),
                source
                    .extension()
                    .map(|s| format!(".{}", s.to_string_lossy()))
                    .unwrap_or_default(),
            )
        };
        for number in 1..=100_000 {
            self.checkpoint_preparation_or_scan(id)?;
            let candidate = target.with_file_name(format!("{stem}_copied{number}{extension}"));
            match fs::symlink_metadata(&candidate) {
                Ok(_) => continue,
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
                Err(error) => return Err(error.to_string()),
            }
            let mut state = self.shared.state.lock().unwrap();
            if state.jobs.iter().any(|job| {
                (!terminal(&job.state) && job.duplicate_targets.values().any(|p| p == &candidate))
                    || job
                        .entries
                        .iter()
                        .any(|entry| entry.destination == candidate && !terminal(&entry.state))
            }) {
                continue;
            }
            let job = state
                .jobs
                .iter_mut()
                .find(|j| j.id == id)
                .ok_or("Operation absent")?;
            job.duplicate_targets
                .insert(source.into(), candidate.clone());
            return Ok(candidate);
        }
        Err("No available duplication name".into())
    }
    /// Le scan de réactivation peut aussi travailler sur un lot déjà en attente.
    fn checkpoint_preparation_or_scan(&self, id: u64) -> Result<(), String> {
        let state = self.shared.state.lock().unwrap();
        if state
            .jobs
            .iter()
            .find(|j| j.id == id)
            .is_none_or(|j| j.cancel)
        {
            Err("Canceled".into())
        } else {
            Ok(())
        }
    }
}
