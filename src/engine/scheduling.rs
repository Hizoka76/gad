//! Identification des disques physiques et choix du prochain lot.
//! Deux copies au maximum peuvent travailler sur des ensembles de disques disjoints ;
//! un volume inconnu sérialise les copies classiques. Un renommage local prouvé peut partager
//! leur disque si les chemins sont indépendants. Les restaurations sont exclusives.

use super::*;
use std::collections::BTreeSet;

pub(super) type Disks = Option<BTreeSet<String>>;

// Unknown/virtual filesystems run alone. A partition and its parent disk must
// reserve the same resource, including disks behind device-mapper and RAID.
/// Remonte une partition vers son disque et développe les dépendances RAID/LVM.
/// Une topologie inconnue, inaccessible ou trop profonde retourne None par prudence.
pub(super) fn physical(node: &Path, depth: usize) -> Option<BTreeSet<String>> {
    if depth > 32 {
        return None;
    }
    let node = fs::canonicalize(node).ok()?;
    if node.join("partition").exists() {
        return physical(node.parent()?, depth + 1);
    }
    let slaves = fs::read_dir(node.join("slaves")).ok()?;
    let mut result = BTreeSet::new();
    for slave in slaves {
        result.extend(physical(&slave.ok()?.path(), depth + 1)?);
    }
    if !result.is_empty() {
        return Some(result);
    }
    // The device link also groups namespaces belonging to one NVMe device.
    let device = fs::canonicalize(node.join("device")).ok()?;
    result.insert(device.to_string_lossy().into_owned());
    Some(result)
}
/// Cherche le périphérique d’un chemin ou de son premier parent existant,
/// notamment lorsque la destination n’a pas encore été créée.
fn path_device(path: &Path) -> Option<u64> {
    let mut path = path;
    loop {
        match fs::symlink_metadata(path) {
            Ok(meta) => return Some(meta.dev()),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => path = path.parent()?,
            Err(_) => return None,
        }
    }
}
/// Réunit les disques des sources et destinations, y compris les montages imbriqués.
/// Une seule résolution incertaine rend l’ensemble inconnu pour le parallélisme.
pub(super) fn disks_for_entries(entries: &[Entry]) -> Disks {
    let mut devices = BTreeSet::new();
    for entry in entries {
        devices.insert(path_device(&entry.source)?);
        devices.insert(path_device(&entry.destination)?);
    }
    let mut result = BTreeSet::new();
    for dev in devices {
        // Décodage Linux de dev_t pour résoudre le lien /sys/dev/block/majeur:mineur.
        let major = ((dev >> 8) & 0xfff) | ((dev >> 32) & 0xfffff000);
        let minor = (dev & 0xff) | ((dev >> 12) & 0xffffff00);
        result.extend(physical(
            &PathBuf::from(format!("/sys/dev/block/{major}:{minor}")),
            0,
        )?);
    }
    if result.is_empty() {
        None
    } else {
        Some(result)
    }
}
/// Autorise le parallèle uniquement entre deux ensembles connus et disjoints.
fn compatible(a: &Disks, b: &Disks) -> bool {
    match (a, b) {
        (Some(a), Some(b)) => a.is_disjoint(b),
        _ => false,
    }
}
/// Les renommages peuvent partager le disque d’une copie, mais jamais ses chemins.
/// Une réservation inconnue ou des racines qui se recouvrent conservent la sérialisation.
fn compatible_jobs(a: &Job, b: &Job) -> bool {
    if a.fast_move || b.fast_move {
        let (Some(a_paths), Some(b_paths)) = (&a.paths, &b.paths) else {
            return false;
        };
        return !a_paths
            .iter()
            .any(|a| b_paths.iter().any(|b| a.starts_with(b) || b.starts_with(a)));
    }
    compatible(&a.disks, &b.disks)
}
/// Choisit un lot en respectant les réservations, les pauses et l’ordre de file
/// sur chaque disque. Une restauration attend l’absence de toute copie active ;
/// l’appelant doit réserver le résultat sans relâcher le verrou d’état.
pub(super) fn next(state: &mut State) -> Option<(u64, bool)> {
    // Rollbacks are exclusive and newest-first, even while the queue is paused.
    if state.jobs.iter().any(|j| j.undo_requested) {
        if !state.active.is_empty() {
            return None;
        }
        return state
            .jobs
            .iter()
            .filter(|j| j.undo_requested && j.state != TransferState::Scanning)
            .max_by_key(|j| {
                (
                    if j.execution_order == 0 {
                        j.id
                    } else {
                        j.execution_order
                    },
                    j.id,
                )
            })
            .map(|j| (j.id, true));
    }
    for job in &mut state.jobs {
        if !state.active.contains_key(&job.id)
            && job.cancel
            && matches!(
                job.state,
                TransferState::Waiting | TransferState::DeviceWaiting
            )
        {
            job.state = TransferState::Cancelled;
            for entry in &mut job.entries {
                if !terminal(&entry.state) && entry.state != TransferState::Skipped {
                    entry.state = TransferState::Cancelled;
                }
            }
        }
    }
    if state.paused || state.active.len() >= 2 {
        return None;
    }
    // Un lot bloqué réserve aussi son ordre sur ses disques face aux candidats suivants.
    let mut earlier: Vec<&Job> = Vec::new();
    for id in row_queue::ordered_jobs(state) {
        let job = &**state.jobs.iter().find(|job| job.id == id).unwrap();
        if terminal(&job.state) || state.active.contains_key(&job.id) {
            continue;
        }
        // A scan may still discover a mount point: keep its ordering barrier.
        if job.state == TransferState::Scanning {
            break;
        }
        if job.state != TransferState::Waiting
            || job.paused
            || row_queue::blocked_by_partial_request(state, job.id)
        {
            continue;
        }
        if state.active.keys().all(|id| {
            state
                .jobs
                .iter()
                .find(|active| active.id == *id)
                .is_some_and(|active| compatible_jobs(job, active))
        }) && earlier.iter().all(|earlier| compatible_jobs(job, earlier))
        {
            return Some((job.id, false));
        }
        // Respecter la priorité de file entre les opérations partageant un disque.
        earlier.push(job);
    }
    None
}
impl Engine {
    /// Libère la réservation du lot et réveille les travailleurs qui peuvent démarrer.
    pub(super) fn release_worker(&self, id: u64) {
        self.shared.state.lock().unwrap().active.remove(&id);
        self.shared.wake.notify_all();
    }
}
