//! Temporaires de copie visibles, publiés sous leur nom définitif seulement à la fin.
//! Un nom occupé, même par un lien symbolique, n’est jamais réutilisé ni tronqué.
use super::*;
use std::collections::HashSet;

fn readable(destination: &Path) -> Result<String, String> {
    let name = destination
        .file_name()
        .and_then(|n| n.to_str())
        .ok_or("Invalid destination filename")?;
    Ok(if name.starts_with('.') {
        format!("gad-{name}")
    } else {
        name.into()
    })
}
/// Les noms trop longs utilisent un identifiant court, toujours visible.
pub(super) fn candidate(
    destination: &Path,
    id: u64,
    index: usize,
    pid: u32,
    attempt: u16,
) -> Result<PathBuf, String> {
    let name = readable(destination)?;
    let visible = if attempt == 0 {
        format!("{name}.part")
    } else {
        format!("{name}.gad-{pid}-{id}-{index}-{attempt}.part")
    };
    Ok(destination.with_file_name(if visible.len() <= 255 {
        visible
    } else {
        format!("gad-{pid}-{id}-{index}-{attempt}.part")
    }))
}
/// Reconnaît le nom attendu ou un suffixe propre à cette entrée. Les anciens
/// journaux interrompus gardent leur temporaire caché et leur identité d’origine.
pub(super) fn matches(path: &Path, destination: &Path, id: u64, index: usize) -> bool {
    if path.parent() != destination.parent() {
        return false;
    }
    let Some(name) = path.file_name().and_then(|n| n.to_str()) else {
        return false;
    };
    let Ok(readable) = readable(destination) else {
        return false;
    };
    if name == format!("{readable}.part") {
        return true;
    }
    let prefix = format!("{readable}.gad-");
    let Some(fields) = name
        .strip_prefix(&prefix)
        .or_else(|| name.strip_prefix("gad-"))
        .or_else(|| name.strip_prefix(".gad-"))
        .and_then(|s| s.strip_suffix(".part"))
    else {
        return false;
    };
    let fields = fields.split('-').collect::<Vec<_>>();
    fields.len() == 4
        && fields[0].parse::<u32>().is_ok()
        && fields[1].parse::<u64>().ok() == Some(id)
        && fields[2].parse::<usize>().ok() == Some(index)
        && fields[3].parse::<u16>().is_ok()
}
/// Vérifie seulement le candidat courant ; le cas usuel ne copie aucun chemin de la file.
fn reserved_name(state: &State, candidate: &Path) -> bool {
    let name = candidate.file_name();
    let parent = candidate.parent();
    // Le nom élimine la plupart des candidats sans reparcourir le préfixe commun des chemins.
    let same = |path: &Path| match name {
        Some(name) => path.file_name() == Some(name) && path.parent() == parent,
        None => path == candidate,
    };
    state
        .jobs
        .iter()
        .filter(|job| !terminal(&job.state) && !job.cancel)
        .any(|job| {
            job.entries.iter().any(|entry| same(&entry.destination))
                || (job.state == TransferState::Scanning
                    && job
                        .sources
                        .iter()
                        .filter_map(|source| {
                            source.file_name().map(|name| job.destination.join(name))
                        })
                        .any(|path| path == candidate))
        })
}
/// En cas de collisions répétées, un seul index amortit les recherches de suffixes suivants.
fn reserved_names(state: &State) -> HashSet<PathBuf> {
    let mut reserved = HashSet::new();
    for job in state
        .jobs
        .iter()
        .filter(|job| !terminal(&job.state) && !job.cancel)
    {
        reserved.extend(job.entries.iter().map(|entry| entry.destination.clone()));
        if job.state == TransferState::Scanning {
            reserved.extend(
                job.sources
                    .iter()
                    .filter_map(|source| source.file_name().map(|name| job.destination.join(name))),
            );
        }
    }
    reserved
}
impl Engine {
    /// La réservation logique évite aussi les noms destinés à un autre transfert de la file.
    pub(super) fn create_staging(
        &self,
        id: u64,
        index: usize,
        destination: &Path,
    ) -> Result<(PathBuf, fs::File), String> {
        let mut reserved: Option<HashSet<PathBuf>> = None;
        for attempt in 0..1000 {
            let path = candidate(destination, id, index, std::process::id(), attempt)?;
            let planned = match &reserved {
                Some(names) => names.contains(&path),
                None => reserved_name(&self.shared.state.lock().unwrap(), &path),
            };
            if !planned {
                match OpenOptions::new()
                    .write(true)
                    .create_new(true)
                    .mode(0o600)
                    .open(&path)
                {
                    Ok(file) => return Ok((path, file)),
                    Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => {}
                    Err(error) => return Err(reconnect::io_error(error)),
                }
            }
            if reserved.is_none() {
                reserved = Some(reserved_names(&self.shared.state.lock().unwrap()));
            }
        }
        Err("Unable to create temporary file".into())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn direct_reservation_matches_index_for_all_job_states() {
        for status in [
            "scanning",
            "waiting",
            "running",
            "done",
            "error",
            "cancelled",
            "undone",
        ] {
            for cancelled in [false, true] {
                let state:State = serde_json::from_value(json!({"jobs":[{"id":1,"sources":["/source/future.part"],"destination":"/dest","mode":"copy","verify_md5":false,"state":status,"cancel":cancelled,"error":"","policy":"ask","entries":[{"source":"/source/file","destination":"/dest/planned.part","kind":"file","size":0,"copied":0,"verified":0,"state":"waiting","error":""}]}]})).unwrap();
                let names = reserved_names(&state);
                for path in [
                    "/dest/planned.part",
                    "/dest/future.part",
                    "/other/future.part",
                    "/dest/free.part",
                    "//dest//planned.part",
                    "/dest/./planned.part",
                    "/",
                    "",
                ] {
                    assert_eq!(
                        reserved_name(&state, Path::new(path)),
                        names.contains(Path::new(path)),
                        "{status}/{cancelled}/{path}"
                    );
                }
            }
        }
    }
}
