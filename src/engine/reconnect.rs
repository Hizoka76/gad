//! Attente de reconnexion : les chemins seuls ne prouvent jamais qu’un volume est revenu.
//! La reprise conserve les indices et journaux, puis réutilise les contrôles de préfixe existants.
use super::*;
use std::{
    cell::Cell,
    time::{Duration, Instant},
};

thread_local! {
    static TRANSIENT: Cell<bool> = const { Cell::new(false) };
    static LAST_PROBE: Cell<Option<Instant>> = const { Cell::new(None) };
}
pub(super) fn clear_io() {
    TRANSIENT.set(false);
}
/// Seules les erreurs de connexion sont candidates ; espace insuffisant et permissions restent des erreurs.
pub(super) fn io_error(error: std::io::Error) -> String {
    if matches!(
        error.raw_os_error(),
        Some(5 | 6 | 19 | 32 | 104 | 107 | 110 | 112 | 113 | 116 | 121)
    ) {
        TRANSIENT.set(true);
    }
    error.to_string()
}
pub(super) fn probe_due() -> bool {
    LAST_PROBE.with(|last| {
        let now = Instant::now();
        if last
            .get()
            .is_none_or(|before| now.duration_since(before) >= Duration::from_millis(500))
        {
            last.set(Some(now));
            true
        } else {
            false
        }
    })
}
#[derive(Clone, Serialize, Deserialize)]
/// L’inode du dossier d’ancrage et le périphérique distinguent un montage revenu d’un dossier vide ou remplacé.
pub(super) struct Anchor {
    path: PathBuf,
    device: u64,
    inode: u64,
    block: Option<PathBuf>,
    remote: bool,
    #[serde(default)]
    uuid: Option<String>,
}
impl Anchor {
    pub(super) fn at(path: PathBuf, remote: bool) -> Option<Self> {
        let meta = fs::metadata(&path).ok()?;
        if !meta.is_dir() {
            return None;
        }
        let dev = meta.dev();
        let major = ((dev >> 8) & 0xfff) | ((dev >> 32) & 0xfffff000);
        let minor = (dev & 0xff) | ((dev >> 12) & 0xffffff00);
        let block = PathBuf::from(format!("/sys/dev/block/{major}:{minor}"));
        Some(Self {
            path,
            device: dev,
            inode: meta.ino(),
            block: block.exists().then_some(block),
            remote,
            uuid: uuid_for(dev),
        })
    }
    fn matched_device(&self) -> Option<u64> {
        let meta = fs::symlink_metadata(&self.path).ok()?;
        if !meta.is_dir() || meta.ino() != self.inode {
            return None;
        }
        let dev = meta.dev();
        if !same_volume(
            self.device,
            dev,
            self.uuid.as_deref(),
            uuid_for(dev).as_deref(),
        ) {
            return None;
        }
        if self.block.is_some() && !block_path(dev).exists() {
            return None;
        }
        Some(dev)
    }
    fn online(&self) -> bool {
        self.matched_device().is_some()
    }
    fn rebound(&self) -> Option<Self> {
        let device = self.matched_device()?;
        let mut anchor = self.clone();
        anchor.device = device;
        if anchor.block.is_some() {
            anchor.block = Some(block_path(device));
        }
        Some(anchor)
    }
}
/// Un numéro de périphérique réutilisé n’identifie pas à lui seul le disque attendu.
fn same_volume(before: u64, after: u64, expected: Option<&str>, actual: Option<&str>) -> bool {
    match expected {
        Some(uuid) => actual == Some(uuid),
        None => before == after,
    }
}
fn block_path(dev: u64) -> PathBuf {
    let major = ((dev >> 8) & 0xfff) | ((dev >> 32) & 0xfffff000);
    let minor = (dev & 0xff) | ((dev >> 12) & 0xffffff00);
    PathBuf::from(format!("/sys/dev/block/{major}:{minor}"))
}
/// L’UUID fourni par udev permet de reconnaître le même système de fichiers après réattribution de dev_t.
fn uuid_for(dev: u64) -> Option<String> {
    if ((dev >> 8) & 0xfff) | ((dev >> 32) & 0xfffff000) == 0 {
        return None;
    }
    fs::read_dir("/dev/disk/by-uuid")
        .ok()?
        .filter_map(Result::ok)
        .find_map(|entry| {
            fs::metadata(entry.path())
                .ok()
                .filter(|m| m.rdev() == dev)
                .map(|_| entry.file_name().to_string_lossy().into_owned())
        })
}
/// Tous les accès aux montages et métadonnées ont lieu hors du verrou du moteur.
pub(super) fn capture(entries: &[Entry]) -> Vec<Anchor> {
    capture_paths(
        entries
            .iter()
            .flat_map(|e| [e.source.clone(), e.destination.clone()]),
    )
}
pub(super) fn capture_paths(paths: impl Iterator<Item = PathBuf>) -> Vec<Anchor> {
    let mounts = fs::read_to_string("/proc/self/mountinfo")
        .map(|s| fast_move::parse_mounts(&s))
        .unwrap_or_default();
    let mut result = Vec::new();
    let mut seen = std::collections::HashSet::new();
    for path in paths {
        let Some(id) = fast_move::mount_id(&path) else {
            continue;
        };
        let Some(mount) = mounts.iter().find(|m| m.id == id) else {
            continue;
        };
        let remote = mount.kind.starts_with("fuse")
            || matches!(mount.kind.as_str(), "cifs" | "smb3" | "nfs" | "nfs4" | "9p");
        if seen.insert(mount.point.clone()) {
            if let Some(anchor) = Anchor::at(mount.point.clone(), remote) {
                result.push(anchor);
            }
        }
        // Le montage FUSE peut survivre alors que le service distant ou son sous-arbre disparaît.
        if remote {
            let mut existing = path.parent().unwrap_or(path.as_path());
            while !fs::symlink_metadata(existing).is_ok_and(|m| m.is_dir()) {
                let Some(parent) = existing.parent() else {
                    break;
                };
                existing = parent;
            }
            if seen.insert(existing.to_owned()) {
                if let Some(anchor) = Anchor::at(existing.to_owned(), true) {
                    result.push(anchor);
                }
            }
        }
    }
    result
}

/// Le garde habituel supprime les échecs ordinaires ; une déconnexion conserve le temporaire journalisé.
pub(super) struct CopyTemp {
    engine: Engine,
    id: u64,
    temp: Temp,
}
impl CopyTemp {
    pub(super) fn new(engine: Engine, id: u64, path: PathBuf) -> Self {
        Self {
            engine,
            id,
            temp: Temp(path),
        }
    }
}
impl Drop for CopyTemp {
    fn drop(&mut self) {
        if self.engine.connection_lost(self.id) {
            self.temp.0 = PathBuf::new();
        }
    }
}
impl Engine {
    fn connection_lost(&self, id: u64) -> bool {
        self.connection_unavailable(id, true)
    }
    fn connection_unavailable(&self, id: u64, active: bool) -> bool {
        let anchors = self
            .shared
            .state
            .lock()
            .unwrap()
            .jobs
            .iter()
            .find(|j| j.id == id)
            .map(|j| j.connections.clone())
            .unwrap_or_default();
        anchors.iter().any(|a| {
            a.matched_device()
                .is_none_or(|device| active && device != a.device)
        }) || (TRANSIENT.get() && anchors.iter().any(|a| a.remote))
    }
    /// Refuser les opérations avant de créer un fichier dans un point de montage devenu vide.
    pub(super) fn check_connection(&self, id: u64) -> Result<(), String> {
        if self.connection_lost(id) {
            Err("Device unavailable: waiting for reconnection".into())
        } else {
            Ok(())
        }
    }
    /// Rend le travailleur et conserve le journal ; l’attente ne monopolise aucun disque de l’ordonnanceur.
    pub(super) fn park_disconnected(&self, id: u64, index: usize, error: &str) -> bool {
        if !self.connection_lost(id) {
            return false;
        }
        {
            let mut state = self.shared.state.lock().unwrap();
            let Some(job) = state.jobs.iter_mut().find(|j| j.id == id) else {
                return false;
            };
            if job.cancel || job.undo_requested || job.entries.get(index).is_some_and(|e| e.cancel)
            {
                return false;
            }
            job.state = TransferState::DeviceWaiting;
            job.error="Device unavailable · reconnect the same mount at the same location. Verification before resuming.".into();
            if let Some(entry) = job.entries.get_mut(index) {
                entry.state = TransferState::DeviceWaiting;
                entry.interrupted = true;
                entry.resume_checking = false;
                entry.error = format!("En attente de reconnexion : {error}");
            }
        }
        let _ = self.save();
        self.shared.wake.notify_all();
        let engine = self.clone();
        thread::spawn(move || engine.wait_connection(id, index));
        true
    }
    /// Une attente peut être annulée ou mise en pause ; la reconnexion ne lève jamais ces choix.
    fn wait_connection(&self, id: u64, index: usize) {
        let mut next_probe = Instant::now() + Duration::from_secs(1);
        loop {
            let snapshot = {
                let state = self.shared.state.lock().unwrap();
                let Some(job) = state.jobs.iter().find(|j| j.id == id) else {
                    return;
                };
                if job.cancel
                    || job.undo_requested
                    || job.state != TransferState::DeviceWaiting
                    || self.shared.stop.load(std::sync::atomic::Ordering::SeqCst)
                {
                    return;
                }
                if !job.entries.is_empty()
                    && job.entries.get(index).is_none_or(|e| e.cancel || e.hidden)
                {
                    drop(state);
                    self.update(id, |j| j.state = TransferState::Waiting);
                    return;
                }
                if state.active.contains_key(&id)
                    || state.paused
                    || job.paused
                    || job.entries.get(index).is_some_and(|e| e.paused)
                {
                    drop(
                        self.shared
                            .wake
                            .wait_timeout(state, Duration::from_millis(250))
                            .unwrap(),
                    );
                    continue;
                }
                let delay = next_probe.saturating_duration_since(Instant::now());
                if !delay.is_zero() {
                    drop(self.shared.wake.wait_timeout(state, delay).unwrap());
                    continue;
                }
                next_probe = Instant::now() + Duration::from_secs(1);
                job.snapshot()
            };
            clear_io();
            if snapshot.connections.iter().all(Anchor::online) {
                // Une seule vérification est autorisée à la fois ; les commandes restent disponibles.
                self.update(id, |j| {
                    if !j.cancel && j.state == TransferState::DeviceWaiting {
                        j.error = "Device reconnected · verification before resuming…".into();
                    }
                });
                let result = self.prepare_reconnect(snapshot);
                match result {
                    Ok(prepared) => {
                        {
                            let mut state = self.shared.state.lock().unwrap();
                            let Some(current) = state.jobs.iter_mut().find(|j| {
                                j.id == id && !j.cancel && j.state == TransferState::DeviceWaiting
                            }) else {
                                return;
                            };
                            current.state = TransferState::Scanning;
                        }
                        let engine = self.clone();
                        self.spawn_preparation(id, false, 0, move || {
                            engine.check_connection(id)?;
                            Ok(prepared)
                        });
                        return;
                    }
                    Err(error) => {
                        if !self.connection_unavailable(id, false) {
                            self.update(id, |j| {
                                if !j.cancel {
                                    j.state = TransferState::Error;
                                    j.error = format!("Reconnexion : {error}");
                                    if let Some(entry) = j.entries.get_mut(index) {
                                        entry.state = TransferState::Error;
                                        entry.error = j.error.clone();
                                    }
                                }
                            });
                            let _ = self.save();
                            return;
                        }
                        self.update(id, |j| {
                            if !j.cancel {
                                j.state = TransferState::DeviceWaiting;
                            }
                        });
                    }
                }
            }
            let state = self.shared.state.lock().unwrap();
            drop(
                self.shared
                    .wake
                    .wait_timeout(state, Duration::from_secs(1))
                    .unwrap(),
            );
        }
    }
    /// Les destinations KIO sans point durable redémarrent seulement le fichier privé non publié.
    pub(super) fn prepare_reconnect(
        &self,
        mut job: Job,
    ) -> Result<(PathBuf, Vec<Entry>, scheduling::Disks), String> {
        let anchors = job
            .connections
            .iter()
            .map(Anchor::rebound)
            .collect::<Option<Vec<_>>>()
            .ok_or("Le volume attendu n’est pas encore disponible")?;
        let mut devices = std::collections::HashMap::new();
        for (old, new) in job.connections.iter().zip(&anchors) {
            if old.device != new.device
                && devices
                    .insert(old.device, new.device)
                    .is_some_and(|previous| previous != new.device)
            {
                return Err("Resume refused: ambiguous volume identity".into());
            }
        }
        for entry in &mut job.entries {
            if let Some(device) = devices.get(&entry.identity.0) {
                entry.identity.0 = *device;
            }
            if let Some(record) = &mut entry.undo {
                record.rebind(&devices);
            }
        }
        let mut probed = false;
        for entry in &mut job.entries {
            self.checkpoint_preparation(job.id)?;
            if entry.hidden
                || entry.cancel
                || terminal(&entry.state)
                || entry.state == TransferState::Skipped
            {
                continue;
            }
            let meta = fs::symlink_metadata(&entry.source).map_err(io_error)?;
            if (meta.dev(), meta.ino()) != entry.identity
                || source_kind(&meta) != Some(entry.kind.as_str())
            {
                return Err("Resume refused: different source or mount; files kept".into());
            }
            if !probed && entry.kind == "file" {
                let mut source = OpenOptions::new()
                    .read(true)
                    .custom_flags(0x20000 | 0x800)
                    .open(&entry.source)
                    .map_err(io_error)?;
                let mut byte = [0u8; 1];
                source.read(&mut byte).map_err(io_error)?;
                probed = true;
            }
            if let Some(record) = &entry.undo {
                if record.resume.is_none()
                    && entry.kind != "directory"
                    && record.result.is_none()
                    && record.backup.is_none()
                    && !record.source_removed
                    && !record.rename_move
                {
                    if !record
                        .source_stamp
                        .as_ref()
                        .is_some_and(|stamp| stamp.matches(&entry.source))
                    {
                        return Err("Resume refused: source version cannot be verified".into());
                    }
                    self.checkpoint_preparation(job.id)?;
                    undo::cleanup_staging(record)?;
                    entry.undo = None;
                    entry.copied = 0;
                    entry.verified = 0;
                }
            }
        }
        job.resume_enabled = true; // Autorise la reprise dans cette session, indépendamment de l’option de reprise après crash.
        let id = job.id;
        let mut prepared = self.prepare_resume(job)?;
        // Publier ensemble les identités revalidées et les ancrages, avant de rendre le lot exécutable.
        if !devices.is_empty() {
            self.checkpoint_preparation(id)?;
            let mut state = self.shared.state.lock().unwrap();
            let current = state
                .jobs
                .iter_mut()
                .find(|j| {
                    j.id == id
                        && !j.cancel
                        && matches!(
                            j.state,
                            TransferState::Scanning | TransferState::DeviceWaiting
                        )
                })
                .ok_or("Canceled")?;
            validation::preserve_controls(&mut prepared.1, &current.entries);
            current.entries = prepared.1.clone();
            current.connections = anchors;
            state.row_layout = None;
        }
        Ok(prepared)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn rebinding_preserves_file_versions_and_content_digests() {
        let stamp = json!({"dev":1,"ino":7,"len":42,"seconds":123,"nanos":9,"mode":33152,"kind":"file","link":null});
        let version = json!({"stamp":stamp,"ctime":[456,8]});
        let point = json!({"source":version,"target":version,"bytes":20,"digest":"prefix-hash","replace":true});
        let mut record:undo::Record=serde_json::from_value(json!({"staging_stamp":stamp,"result":stamp,"original":stamp,"source_stamp":stamp,"restored_source":stamp,"resume":point,"digest":"full-hash"})).unwrap();
        record.rebind(&[(1, 2)].into_iter().collect());
        let value = serde_json::to_value(record).unwrap();
        for key in [
            "staging_stamp",
            "result",
            "original",
            "source_stamp",
            "restored_source",
        ] {
            assert_eq!(value[key]["dev"], 2);
            assert_eq!(value[key]["ino"], 7);
            assert_eq!(value[key]["len"], 42);
        }
        assert_eq!(value["resume"]["source"]["stamp"]["dev"], 2);
        assert_eq!(value["resume"]["target"]["stamp"]["dev"], 2);
        assert_eq!(value["resume"]["source"]["ctime"], json!([456, 8]));
        assert_eq!(value["resume"]["digest"], "prefix-hash");
        assert_eq!(value["digest"], "full-hash");
    }
    #[test]
    fn volume_uuid_authorizes_new_device_but_rejects_reused_number() {
        assert!(same_volume(1, 2, Some("volume-a"), Some("volume-a")));
        assert!(!same_volume(1, 1, Some("volume-a"), Some("volume-b")));
        assert!(!same_volume(1, 2, None, None));
        assert!(same_volume(1, 1, None, None));
        assert!(!same_volume(1, 1, Some("volume-a"), None));
    }
}
