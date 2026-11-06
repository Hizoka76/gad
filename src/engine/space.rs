//! Estimation conservatrice de la place nécessaire, par système de fichiers.
//! Les destinations remplacées et sauvegardes occupent déjà le volume : ne jamais
//! déduire leur taille de la demande ni les compter deux fois comme allocation future.
use super::*;
use std::collections::{BTreeMap, BTreeSet};
use std::ffi::CString;
use std::os::unix::ffi::OsStrExt;

unsafe extern "C" {
    /// Pointeurs valides pendant l’appel ; renvoie zéro ou un code errno POSIX.
    fn sc_available_space(
        path: *const std::ffi::c_char,
        available: *mut u64,
        block: *mut u64,
    ) -> i32;
}

/// Demande indépendante de l’état partagé, pour effectuer les lectures hors du verrou moteur.
struct Demand {
    job: u64,
    path: PathBuf,
    size: u64,
    resident: u64,
    resident_device: Option<u64>,
    remote: bool,
}
#[derive(Clone)]
pub(super) struct Volume {
    device: u64,
    pub(super) available: u64,
    block: u64,
}
/// Utilise le parent de la cible : un lien final sera remplacé, pas suivi.
pub(super) fn volume(path: &Path) -> Result<Volume, String> {
    let mut parent = path.parent().ok_or("Destination sans parent")?;
    loop {
        match fs::metadata(parent) {
            Ok(meta) if meta.is_dir() => {
                let directory = fs::File::open(parent).map_err(|e| e.to_string())?;
                if fast_move::descriptor_is_kio_fuse(&directory) {
                    return Err("Remote space cannot be checked through KIO Fuse".into());
                }
                let name =
                    CString::new(parent.as_os_str().as_bytes()).map_err(|e| e.to_string())?;
                let (mut available, mut block) = (0, 0);
                // SAFETY : chaîne terminée par zéro et deux adresses exclusives de u64.
                let error =
                    unsafe { sc_available_space(name.as_ptr(), &mut available, &mut block) };
                if error != 0 {
                    return Err(std::io::Error::from_raw_os_error(error).to_string());
                }
                return Ok(Volume {
                    device: meta.dev(),
                    available,
                    block,
                });
            }
            Ok(_) => return Err("Un parent de destination n’est pas un dossier".into()),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
                parent = parent
                    .parent()
                    .ok_or("Aucun parent de destination accessible")?;
            }
            Err(error) => return Err(error.to_string()),
        }
    }
}
/// Arrondit chaque fichier aux blocs du volume ; u128 évite tout débordement de la somme.
fn needed(demand: &Demand, block: u64, device: u64) -> u128 {
    let block = u128::from(block.max(1));
    let total = u128::from(demand.size).div_ceil(block) * block;
    total.saturating_sub(if demand.resident_device == Some(device) {
        u128::from(demand.resident)
    } else {
        0
    })
}
/// Ne compare que les volumes du lot lancé, en réservant aussi leurs autres lots analysés.
fn assess(
    demands: &[Demand],
    id: u64,
    probe: impl Fn(&Path) -> Result<Volume, String>,
) -> Result<String, String> {
    let mut volumes: BTreeMap<u64, (Volume, PathBuf, u128)> = BTreeMap::new();
    let mut paths: BTreeMap<PathBuf, Result<Volume, String>> = BTreeMap::new();
    let mut warnings = BTreeSet::new();
    // Une seule interrogation par dossier, y compris en cas de capacité indisponible.
    let mut cached = |path: &Path| {
        let key = path.parent().unwrap_or(path).to_path_buf();
        paths.entry(key).or_insert_with(|| probe(path)).clone()
    };
    for demand in demands.iter().filter(|d| d.job == id) {
        if demand.remote {
            warnings.insert("Available space cannot be checked on the remote KIO Fuse destination; transfer continues without an estimate.".to_string());
            continue;
        }
        match cached(&demand.path) {
            Ok(v) => {
                volumes
                    .entry(v.device)
                    .or_insert((v, demand.path.clone(), 0));
            }
            Err(error) => {
                warnings.insert(format!("Espace non vérifiable pour {} : {error}. Transfert poursuivi sans estimation complète.",demand.path.display()));
            }
        }
    }
    let devices: BTreeSet<_> = volumes.keys().copied().collect();
    if !devices.is_empty() {
        for demand in demands {
            if demand.remote {
                continue;
            }
            let v = match cached(&demand.path) {
                Ok(v) => v,
                Err(error) => {
                    warnings.insert(format!(
                        "Estimation de la file incomplète pour {} : {error}",
                        demand.path.display()
                    ));
                    continue;
                }
            };
            if !devices.contains(&v.device) {
                continue;
            }
            let (budget, _, required) = volumes.get_mut(&v.device).unwrap();
            budget.available = budget.available.min(v.available);
            *required = required.saturating_add(needed(demand, v.block, v.device));
        }
    }
    for (_, (v, path, required)) in volumes {
        if required > u128::from(v.available) {
            return Err(format!("Espace insuffisant sur {} : {} octets nécessaires pour la file, {} octets disponibles. Les fichiers remplacés sont conservés pour la remise en état. Libérez de l’espace puis réactivez le lot.",path.display(),required,v.available));
        }
    }
    let mut shown = warnings.iter().take(3).cloned().collect::<Vec<_>>();
    if warnings.len() > 3 {
        shown.push(format!(
            "{} autres destinations non vérifiables",
            warnings.len() - 3
        ));
    }
    Ok(shown.join(" · "))
}
/// Seuls les blocs d’un temporaire appartenant encore au journal sont déjà payés.
fn demand(id: u64, remote: bool, entry: &Entry) -> Demand {
    let (resident, resident_device) = entry
        .undo
        .as_ref()
        .and_then(|r| {
            let path = r.staging.as_ref()?;
            let stamp = r.staging_stamp.as_ref()?;
            let meta = fs::symlink_metadata(path).ok()?;
            (meta.is_file() && stamp.owns_metadata(&meta))
                .then(|| (meta.blocks().saturating_mul(512), Some(meta.dev())))
        })
        .unwrap_or((0, None));
    Demand {
        job: id,
        path: entry.destination.clone(),
        size: entry.size,
        resident,
        resident_device,
        remote,
    }
}
impl Engine {
    /// Capture la file sous verrou, puis consulte les volumes et temporaires sans verrou.
    pub(super) fn check_queue_space(&self, id: u64) -> Result<(), String> {
        let jobs = {
            let state = self.shared.state.lock().unwrap();
            state
                .jobs
                .iter()
                .filter(|j| !terminal(&j.state) && !j.cancel && !j.undo_requested && !j.fast_move)
                .map(|j| j.snapshot())
                .collect::<Vec<_>>()
        };
        let demands = jobs
            .iter()
            .flat_map(|j| {
                j.entries
                    .iter()
                    .filter(|e| {
                        e.kind == "file"
                            && !terminal(&e.state)
                            && e.state != TransferState::Skipped
                            && !e.cancel
                            && !e.hidden
                    })
                    .map(|e| demand(j.id, j.destination_url.is_some(), e))
            })
            .collect::<Vec<_>>();
        let warning = assess(&demands, id, volume)?;
        self.update(id, |job| job.space_warning = warning);
        Ok(())
    }
    /// Dernier contrôle du fichier effectivement alloué, après résolution d’un éventuel conflit.
    pub(super) fn check_file_space(
        &self,
        id: u64,
        index: usize,
        effective: &Entry,
    ) -> Result<(), String> {
        if effective.kind != "file" {
            return Ok(());
        }
        let (record, remote) = {
            let state = self.shared.state.lock().unwrap();
            let job = state.jobs.iter().find(|j| j.id == id).ok_or("Lot absent")?;
            if !job.verify_space {
                return Ok(());
            }
            (
                job.entries[index].undo.clone(),
                job.destination_url.is_some(),
            )
        };
        let mut current = effective.clone();
        current.undo = record;
        let warning = assess(&[demand(id, remote, &current)], id, volume)?;
        if !warning.is_empty() {
            self.update(id, |job| job.space_warning = warning);
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn item(job: u64, path: &str, size: u64) -> Demand {
        Demand {
            job,
            path: path.into(),
            size,
            resident: 0,
            resident_device: None,
            remote: false,
        }
    }
    fn probe(path: &Path) -> Result<Volume, String> {
        Ok(Volume {
            device: if path.starts_with("/other") { 2 } else { 1 },
            available: 100,
            block: 1,
        })
    }
    #[test]
    fn waiting_and_paused_batches_share_capacity_without_subtracting_old_targets() {
        assert!(
            assess(&[item(1, "/disk/a", 60), item(2, "/disk/b", 50)], 1, probe)
                .unwrap_err()
                .contains("110 octets")
        );
        assert!(assess(&[item(1, "/disk/a", 60), item(2, "/disk/b", 40)], 1, probe).is_ok());
    }
    #[test]
    fn independent_volumes_do_not_consume_each_others_budget() {
        assert!(assess(&[item(1, "/disk/a", 60), item(2, "/other/b", 90)], 1, probe).is_ok());
        assert!(assess(
            &[item(1, "/disk/a", 60), item(1, "/other/b", 101)],
            1,
            probe
        )
        .is_err());
    }
    #[test]
    fn allocated_partial_blocks_are_not_reserved_twice() {
        let mut d = item(1, "/disk/a", 140);
        d.resident = 40;
        d.resident_device = Some(1);
        assert!(assess(&[d], 1, probe).is_ok());
    }
    #[test]
    fn partial_allocated_on_another_volume_is_not_a_discount() {
        let mut d = item(1, "/disk/a", 140);
        d.resident = 40;
        d.resident_device = Some(2);
        assert!(assess(&[d], 1, probe).is_err());
    }
    #[test]
    fn each_file_is_rounded_and_sums_cannot_wrap() {
        assert_eq!(needed(&item(1, "/disk/a", 1), 4096, 1), 4096);
        let huge = [item(1, "/disk/a", u64::MAX), item(2, "/disk/b", u64::MAX)];
        assert!(assess(&huge, 1, probe).is_err());
    }
    #[test]
    fn known_shortage_still_blocks_when_another_volume_is_unknown() {
        let items = [item(1, "/disk/a", 101), item(1, "/unknown/b", 1)];
        assert!(assess(&items, 1, |p| if p.starts_with("/unknown") {
            Err("indisponible".into())
        } else {
            probe(p)
        })
        .unwrap_err()
        .contains("Espace insuffisant"));
    }
    #[test]
    fn unknown_and_remote_capacity_warn_without_blocking() {
        assert!(
            !assess(&[item(1, "/disk/a", 1)], 1, |_| Err("indisponible".into()))
                .unwrap()
                .is_empty()
        );
        let mut remote = item(1, "/disk/a", 1);
        remote.remote = true;
        assert!(assess(&[remote], 1, probe).unwrap().contains("remote"));
    }
}
