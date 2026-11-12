//! Points de reprise durables : le temporaire est synchronisé avant le journal.
//! Une reprise contrôle la version source et relit le préfixe des deux fichiers avant toute écriture.
use super::*;
use std::io::{Seek, SeekFrom};
use std::time::{Duration, Instant};

pub(super) const CHECKPOINT_BYTES: u64 = 64 * 1024 * 1024;
pub(super) const CHECKPOINT_INTERVAL: Duration = Duration::from_secs(2);

#[derive(Clone, Serialize, Deserialize)]
/// Le ctime complète les identités et mtime, notamment si une date a été rétablie après modification.
pub(super) struct Version {
    stamp: undo::Stamp,
    ctime: (i64, i64),
}
impl Version {
    fn read(path: &Path) -> Result<Self, String> {
        let meta = fs::symlink_metadata(path).map_err(reconnect::io_error)?;
        Ok(Self {
            stamp: undo::Stamp::from_metadata(path, &meta)?,
            ctime: (meta.ctime(), meta.ctime_nsec()),
        })
    }
    fn matches(&self, path: &Path) -> bool {
        fs::symlink_metadata(path).is_ok_and(|meta| {
            (meta.ctime(), meta.ctime_nsec()) == self.ctime
                && self.stamp.matches_metadata(path, &meta)
        })
    }
}

#[derive(Clone, Serialize, Deserialize)]
/// Référence du contenu réellement synchronisé, distincte du compteur de progression en mémoire.
pub(super) struct Point {
    source: Version,
    target: Option<Version>,
    pub bytes: u64,
    digest: String,
    pub replace: bool,
}
/// Flux réouverts après contrôle : les deux curseurs sont placés au point validé.
pub(super) struct Partial {
    pub path: PathBuf,
    pub output: fs::File,
    pub source: fs::File,
    pub metadata: fs::Metadata,
    pub record: undo::Record,
    pub hash: md5::Context,
    pub bytes: u64,
}

impl Point {
    pub(super) fn rebind(&mut self, devices: &std::collections::HashMap<u64, u64>) {
        self.source.stamp.rebind(devices);
        if let Some(target) = &mut self.target {
            target.stamp.rebind(devices);
        }
    }
    /// Une décision de remplacement n’est réutilisée que pour cette version précise de la cible.
    pub(super) fn initial(entry: &Entry, replace: bool) -> Result<Self, String> {
        let target = if replace {
            Some(Version::read(&entry.destination)?)
        } else {
            None
        };
        Ok(Self {
            source: Version::read(&entry.source)?,
            target,
            bytes: 0,
            digest: format!("{:x}", md5::compute([])),
            replace,
        })
    }
    pub(super) fn environment(&self, entry: &Entry) -> Result<(), String> {
        if !self.source.matches(&entry.source) {
            return Err("Resume refused: source changed or its volume is unavailable".into());
        }
        let target_ok = match &self.target {
            Some(target) => self.replace && target.matches(&entry.destination),
            None => {
                !self.replace
                    && fs::symlink_metadata(&entry.destination)
                        .is_err_and(|error| error.kind() == std::io::ErrorKind::NotFound)
            }
        };
        if !target_ok {
            return Err("Resume refused: destination changed since the transfer".into());
        }
        Ok(())
    }
}

/// Validation structurelle avant ouverture : aucun chemin de journal arbitraire ne devient une cible d’écriture.
fn partial_path(
    id: u64,
    index: usize,
    entry: &Entry,
) -> Result<(&Path, &undo::Record, &Point), String> {
    let record = entry.undo.as_ref().ok_or("Journal de reprise absent")?;
    let point = record.resume.as_ref().ok_or("Point de reprise absent")?;
    if record.result.is_some()
        || record.backup.is_some()
        || record.source_removed
        || record.rename_move
        || record.directory_move
    {
        return Err("Resume refused: publication or restoration must be verified".into());
    }
    let path = record
        .staging
        .as_deref()
        .ok_or("Temporaire de reprise absent")?;
    if !staging::matches(path, &entry.destination, id, index) {
        return Err("Resume refused: temporary file does not match this entry".into());
    }
    if point.digest.len() != 32 || !point.digest.bytes().all(|byte| byte.is_ascii_hexdigit()) {
        return Err("Empreinte de reprise invalide".into());
    }
    safe_parents(path)?;
    point.environment(entry)?;
    Ok((path, record, point))
}

/// Vérifie l’identité du temporaire en ignorant la taille et mtime, qui évoluent pendant l’écriture.
fn own_partial(record: &undo::Record, path: &Path, meta: &fs::Metadata) -> Result<(), String> {
    let stamp = record
        .staging_stamp
        .as_ref()
        .ok_or("Missing temporary file identity")?;
    if !meta.is_file() || meta.nlink() != 1 || !stamp.owns_metadata(meta) || !stamp.same_inode(path)
    {
        return Err("Resume refused: temporary file was replaced or has another hard link".into());
    }
    Ok(())
}

/// Rouvre seulement la version du préfixe qui a réellement été vérifiée, sans encore la tronquer.
fn reopen_verified(
    record: &undo::Record,
    path: &Path,
    input: &fs::File,
    verified: &fs::Metadata,
) -> Result<fs::File, String> {
    use std::os::unix::fs::PermissionsExt;
    let current = input.metadata().map_err(reconnect::io_error)?;
    own_partial(record, path, &current)?;
    if !same_version(verified, &current) {
        return Err("Resume refused: temporary file changed during verification".into());
    }
    let mut expected = current.clone();
    if current.mode() & 0o200 == 0 {
        input
            .set_permissions(fs::Permissions::from_mode(
                (current.mode() & 0o7777) | 0o200,
            ))
            .map_err(reconnect::io_error)?;
        expected = input.metadata().map_err(reconnect::io_error)?;
        // chmod change légitimement ctime, mais ne peut changer ni identité, ni taille, ni contenu daté.
        if !current.is_file()
            || (
                current.dev(),
                current.ino(),
                current.len(),
                current.uid(),
                current.gid(),
            ) != (
                expected.dev(),
                expected.ino(),
                expected.len(),
                expected.uid(),
                expected.gid(),
            )
            || current.modified().ok() != expected.modified().ok()
        {
            return Err("Resume refused: temporary file changed during verification".into());
        }
    }
    let output = OpenOptions::new()
        .read(true)
        .write(true)
        .custom_flags(0x20000 | 0x800)
        .open(path)
        .map_err(reconnect::io_error)?;
    let opened = output.metadata().map_err(reconnect::io_error)?;
    own_partial(record, path, &opened)?;
    if !same_version(&expected, &opened)
        || (expected.mode(), expected.uid(), expected.gid())
            != (opened.mode(), opened.uid(), opened.gid())
    {
        return Err("Resume refused: temporary file changed during verification".into());
    }
    Ok(output)
}

impl Engine {
    /// Synchronise d’abord les octets et le nom du temporaire ; le JSON ne promet jamais des octets non durables.
    pub(super) fn save_copy_point(
        &self,
        position: (u64, usize),
        entry: &Entry,
        output: &fs::File,
        record: &mut undo::Record,
        bytes: u64,
        hash: &md5::Context,
    ) -> Result<(), String> {
        let (id, index) = position;
        if record.resume.is_none() {
            return Ok(());
        }
        let path = record
            .staging
            .as_deref()
            .ok_or("Temporaire de reprise absent")?;
        own_partial(
            record,
            path,
            &output.metadata().map_err(reconnect::io_error)?,
        )?;
        let point = record.resume.as_mut().ok_or("Point de reprise absent")?;
        point.environment(entry)?;
        output
            .sync_all()
            .map_err(|error| format!("Sauvegarde du point de reprise : {error}"))?;
        fs::File::open(entry.destination.parent().ok_or("Parent absent")?)
            .and_then(|parent| parent.sync_all())
            .map_err(reconnect::io_error)?;
        point.bytes = bytes;
        point.digest = format!("{:x}", hash.clone().finalize());
        self.record(id, index, record.clone())
    }

    /// Relit le préfixe source/temporaire, reconstruit le hash du flux puis tronque seulement la fin non validée.
    pub(super) fn open_partial(
        &self,
        id: u64,
        index: usize,
        entry: &Entry,
    ) -> Result<Partial, String> {
        let (path, record, point) = partial_path(id, index, entry)?;
        let mut source = OpenOptions::new()
            .read(true)
            .custom_flags(0x20000 | 0x800)
            .open(&entry.source)
            .map_err(reconnect::io_error)?;
        let source_meta = source.metadata().map_err(reconnect::io_error)?;
        if !source_meta.is_file()
            || (source_meta.dev(), source_meta.ino()) != entry.identity
            || point.bytes > source_meta.len()
        {
            return Err("Resume refused: inconsistent source version or size".into());
        }
        let mut input = OpenOptions::new()
            .read(true)
            .custom_flags(0x20000 | 0x800)
            .open(path)
            .map_err(|error| {
                format!(
                    "Temporaire de reprise indisponible : {}",
                    reconnect::io_error(error)
                )
            })?;
        let initial = input.metadata().map_err(reconnect::io_error)?;
        own_partial(record, path, &initial)?;
        if initial.len() < point.bytes {
            return Err(
                "Resume refused: temporary file is shorter than the validated checkpoint".into(),
            );
        }
        self.update(id, |job| {
            job.entries[index].resume_checking = true;
            job.entries[index].resume_checked = 0;
        });
        let mut left = point.bytes;
        let mut hash = md5::Context::new();
        let mut a = io_buffer(left, 1024 * 1024);
        let mut b = vec![0; a.len()];
        while left > 0 {
            self.checkpoint_entry(id, index)?;
            let count = left.min(a.len() as u64) as usize;
            source
                .read_exact(&mut a[..count])
                .map_err(reconnect::io_error)?;
            input
                .read_exact(&mut b[..count])
                .map_err(reconnect::io_error)?;
            if a[..count] != b[..count] {
                return Err(
                    "Resume refused: previously written contents no longer match the source".into(),
                );
            }
            hash.consume(&b[..count]);
            left -= count as u64;
            self.progress(
                id,
                index,
                snapshot::Counter::ResumeChecked,
                point.bytes - left,
            );
        }
        if format!("{:x}", hash.clone().finalize()) != point.digest {
            return Err("Resume refused: digest of the previously written portion differs".into());
        }
        point.environment(entry)?;
        let now = input.metadata().map_err(reconnect::io_error)?;
        own_partial(record, path, &now)?;
        if !same_version(&initial, &now)
            || (initial.ctime(), initial.ctime_nsec()) != (now.ctime(), now.ctime_nsec())
        {
            return Err("Resume refused: temporary file changed during verification".into());
        }
        self.checkpoint_entry(id, index)?;
        // Une pause peut être intervenue depuis le dernier contrôle du préfixe.
        let mut output = reopen_verified(record, path, &input, &now)?;
        output.set_len(point.bytes).map_err(reconnect::io_error)?;
        output
            .seek(SeekFrom::Start(point.bytes))
            .map_err(reconnect::io_error)?;
        source
            .seek(SeekFrom::Start(point.bytes))
            .map_err(reconnect::io_error)?;
        self.update(id, |job| {
            let entry = &mut job.entries[index];
            entry.resume_checking = false;
            entry.resumed_from = point.bytes;
            entry.copied = point.bytes;
        });
        Ok(Partial {
            path: path.to_owned(),
            output,
            source,
            metadata: source_meta,
            record: record.clone(),
            hash,
            bytes: point.bytes,
        })
    }

    /// Réserve les lots récupérables avant de lancer leurs contrôles hors du thread Qt.
    pub(super) fn resume_interrupted(&self, ids: &[u64]) -> Result<Value, String> {
        if ids.iter().collect::<std::collections::HashSet<_>>().len() != ids.len() {
            return Err("Duplicate resume identifiers".into());
        }
        if ids.is_empty() {
            return Err("No interrupted transfer selected".into());
        }
        let snapshots = {
            let mut state = self.shared.state.lock().unwrap();
            for id in ids {
                if state.active.contains_key(id)
                    || !state.jobs.iter().any(|job| {
                        job.id == *id
                            && job.recovery
                            && job.resume_pending
                            && !job.undo_requested
                            && terminal(&job.state)
                    })
                {
                    return Err("This batch is not an idle recoverable interruption".into());
                }
            }
            ids.iter()
                .map(|id| {
                    let job = state.jobs.iter_mut().find(|job| job.id == *id).unwrap();
                    job.cancel = false;
                    job.state = TransferState::Scanning;
                    job.error.clear();
                    job.resolution = None;
                    job.snapshot()
                })
                .collect::<Vec<_>>()
        };
        if let Err(error) = self.save() {
            for snapshot in &snapshots {
                self.update(snapshot.id, |job| {
                    job.state = TransferState::Error;
                    job.error = error.clone();
                });
            }
            return Err(error);
        }
        for snapshot in snapshots {
            let engine = self.clone();
            let id = snapshot.id;
            self.spawn_preparation(id, false, 0, move || engine.prepare_resume(snapshot));
        }
        Ok(json!({"ok":true}))
    }

    /// Ne rejoue que les entrées interrompues, jamais les fichiers terminés, annulés ou masqués.
    pub(super) fn prepare_resume(
        &self,
        job: Job,
    ) -> Result<(PathBuf, Vec<Entry>, scheduling::Disks), String> {
        if !job.resume_enabled || !job.journaled {
            return Err("Ce lot ne contient pas de journal de reprise compatible".into());
        }
        let mut entries = job.entries;
        if entries.is_empty() {
            let destination = self.validate_roots(job.id, &job.sources, &job.destination)?;
            for source in &job.sources {
                self.scan(
                    job.id,
                    source,
                    &destination.join(source.file_name().ok_or("Source invalide")?),
                    &mut entries,
                )?;
            }
            let disks = scheduling::disks_for_entries(&entries);
            return Ok((destination, entries, disks));
        }
        let mut empty_staging = Vec::new();
        for (index, entry) in entries.iter_mut().enumerate() {
            self.checkpoint_preparation(job.id)?;
            if entry.hidden
                || entry.cancel
                || matches!(
                    entry.state,
                    TransferState::Done
                        | TransferState::Undone
                        | TransferState::Skipped
                        | TransferState::Cancelled
                )
                || (entry.state == TransferState::Error && !entry.interrupted)
            {
                continue;
            }
            safe_parents(&entry.source)?;
            let source = fs::symlink_metadata(&entry.source).map_err(|error| {
                format!(
                    "Source de reprise indisponible : {}",
                    reconnect::io_error(error)
                )
            })?;
            if (source.dev(), source.ino()) != entry.identity
                || source_kind(&source) != Some(entry.kind.as_str())
            {
                return Err("Resume refused: a source was replaced".into());
            }
            if let Some(record) = &entry.undo {
                if record.resume.is_some() {
                    let (path, record, point) = partial_path(job.id, index, entry)?;
                    let meta = fs::symlink_metadata(path).map_err(|error| {
                        format!(
                            "Temporaire de reprise indisponible : {}",
                            reconnect::io_error(error)
                        )
                    })?;
                    own_partial(record, path, &meta)?;
                    if meta.len() < point.bytes || point.bytes > source.len() {
                        return Err("Inconsistent checkpoint".into());
                    }
                    entry.copied = point.bytes;
                } else if entry.kind == "directory"
                    && !record.source_dir_removed
                    && !record.directory_move
                {
                    if !record
                        .result
                        .as_ref()
                        .is_some_and(|stamp| stamp.matches(&entry.destination))
                    {
                        if fs::symlink_metadata(&entry.destination)
                            .is_err_and(|error| error.kind() == std::io::ErrorKind::NotFound)
                        {
                            empty_staging.push((index, record.clone()));
                        } else {
                            return Err("Resume refused: target folder changed".into());
                        }
                    }
                } else if record.result.is_none()
                    && record.backup.is_none()
                    && !record.source_removed
                {
                    let path = record
                        .staging
                        .as_ref()
                        .ok_or("Temporary file has no resume identity")?;
                    match fs::symlink_metadata(path) {
                        Ok(meta)
                            if (meta.len() == 0
                                || (entry.kind == "link" && meta.file_type().is_symlink()))
                                && record
                                    .staging_stamp
                                    .as_ref()
                                    .is_some_and(|stamp| stamp.owns_metadata(&meta)) => {}
                        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
                        _ => {
                            return Err(
                                "Resume refused: no durable checkpoint for this temporary file"
                                    .into(),
                            )
                        }
                    }
                    empty_staging.push((index, record.clone()));
                } else {
                    return Err("Resume refused: publication interrupted; use restoration".into());
                }
            }
            entry.state = TransferState::Waiting;
            entry.error.clear();
            entry.verified = 0;
            entry.resume_checking = false;
            entry.resume_checked = 0;
        }
        // Ces temporaires vides n’ont jamais reçu de contenu ; leur retrait précède un redémarrage de cette seule entrée.
        for (index, record) in empty_staging {
            self.checkpoint_preparation(job.id)?;
            undo::cleanup_staging(&record)?;
            entries[index].undo = None;
        }
        let disks = scheduling::disks_for_entries(&entries);
        Ok((job.destination, entries, disks))
    }
}

/// Décide des sauvegardes sans dépendre de la vitesse du disque.
pub(super) fn due(total: u64, saved: u64, since: Instant) -> bool {
    total > saved && (total - saved >= CHECKPOINT_BYTES || since.elapsed() >= CHECKPOINT_INTERVAL)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    /// La réouverture ne tronque jamais un préfixe modifié et accepte un temporaire valide en lecture seule.
    fn reopen_checks_verified_contents_and_readonly_permissions() {
        use std::os::unix::fs::PermissionsExt;
        let dir = undo::private_directory(&std::env::temp_dir(), "resume-reopen-test").unwrap();
        let path = dir.join("partial");
        fs::write(&path, b"verified").unwrap();
        let input = fs::File::open(&path).unwrap();
        let record = undo::Record {
            staging: Some(path.clone()),
            staging_stamp: Some(undo::Stamp::read(&path).unwrap()),
            ..Default::default()
        };
        let verified = input.metadata().unwrap();
        fs::write(&path, b"changed contents").unwrap();
        assert!(reopen_verified(&record, &path, &input, &verified).is_err());
        assert_eq!(fs::read(&path).unwrap(), b"changed contents");
        fs::set_permissions(&path, fs::Permissions::from_mode(0o400)).unwrap();
        let verified = input.metadata().unwrap();
        let output = reopen_verified(&record, &path, &input, &verified).unwrap();
        assert_eq!(output.metadata().unwrap().mode() & 0o200, 0o200);
        assert_eq!(fs::read(&path).unwrap(), b"changed contents");
        drop(output);
        drop(input);
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    /// Un point exige de nouveaux octets ; le volume ou le temps suffisent ensuite à le déclencher.
    fn resume_points_follow_bytes_or_elapsed_time() {
        let now = Instant::now();
        assert!(!due(0, 0, now - Duration::from_secs(3)));
        assert!(!due(CHECKPOINT_BYTES - 1, 0, now));
        assert!(due(CHECKPOINT_BYTES, 0, now));
        assert!(due(1, 0, now - Duration::from_secs(3)));
        assert!(!due(1, 1, now - Duration::from_secs(3)));
    }
}
