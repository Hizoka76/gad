//! Ignore les opérations déjà prévues, sans comparer les contenus ni toucher aux fichiers.
//! Les chemins normalisés distinguent les liens physiques mais reconnaissent les alias de montage.
use super::*;
use std::collections::{HashMap, HashSet};

#[derive(Clone, Hash, PartialEq, Eq)]
struct Key {
    source: PathBuf,
    target: PathBuf,
    identity: (u64, u64),
    kind: String,
    moving: bool,
}
struct Keys {
    source: Option<Vec<PathBuf>>,
    target: Option<Vec<PathBuf>>,
}
/// Toutes les consultations de parents/montages sont effectuées hors du verrou moteur.
fn keys(job: &Job) -> Keys {
    Keys {
        source: fast_move::destination_keys(job.entries.iter().map(|e| e.source.clone())),
        target: job.destination_keys.clone().or_else(|| {
            fast_move::destination_keys(job.entries.iter().map(|e| e.destination.clone()))
        }),
    }
}
fn entry_keys(job: &Job, keys: &Keys, index: usize) -> [Key; 2] {
    let entry = &job.entries[index];
    let plain = Key {
        source: entry.source.clone(),
        target: entry.destination.clone(),
        identity: entry.identity,
        kind: entry.kind.clone(),
        moving: job.mode == "move",
    };
    let mut normalized = plain.clone();
    if let Some(source) = keys.source.as_ref().and_then(|all| all.get(index)) {
        normalized.source = source.clone();
    }
    if let Some(target) = keys.target.as_ref().and_then(|all| all.get(index)) {
        normalized.target = target.clone();
    }
    [plain, normalized]
}
/// Un élément ignoré, annulé ou masqué ne réserve aucune copie future.
fn relevant(entry: &Entry) -> bool {
    !entry.hidden
        && !entry.cancel
        && !matches!(
            entry.state,
            TransferState::Skipped
                | TransferState::Cancelled
                | TransferState::Undone
                | TransferState::Error
        )
}
/// Conserve les dossiers nécessaires aux nouveaux enfants, sans garder des branches entièrement dupliquées.
fn keep_ancestors(entries: &[Entry], keep: &mut [bool]) {
    let mut directories: HashMap<(PathBuf, PathBuf), Vec<usize>> = HashMap::new();
    for (i, e) in entries.iter().enumerate() {
        if e.kind == "directory" {
            directories
                .entry((e.source.clone(), e.destination.clone()))
                .or_default()
                .push(i);
        }
    }
    for i in 0..entries.len() {
        if !keep[i] {
            continue;
        }
        let (mut source, mut target) = (
            entries[i].source.as_path(),
            entries[i].destination.as_path(),
        );
        while let (Some(s), Some(t)) = (source.parent(), target.parent()) {
            source = s;
            target = t;
            if let Some(indices) = directories.get(&(s.to_path_buf(), t.to_path_buf())) {
                let ancestor = indices
                    .iter()
                    .copied()
                    .take_while(|n| *n < i)
                    .last()
                    .unwrap_or(indices[0]);
                keep[ancestor] = true;
            }
        }
    }
}

pub(super) struct Filtered {
    pub entries: Vec<Entry>,
    pub removed: u64,
    pub target_keys: Option<Vec<PathBuf>>,
}
impl Engine {
    /// L’ordre d’acceptation tranche les ajouts simultanés : attendre les analyses antérieures avant la décision atomique.
    pub(super) fn filter_duplicates(
        &self,
        id: u64,
        mut entries: Vec<Entry>,
    ) -> Result<Filtered, String> {
        let (job, older, preserve) = {
            let mut state = self.shared.state.lock().unwrap();
            loop {
                let current = state
                    .jobs
                    .iter()
                    .find(|j| j.id == id)
                    .ok_or("Operation absent")?;
                if current.cancel || self.shared.stop.load(std::sync::atomic::Ordering::SeqCst) {
                    return Err("Canceled".into());
                }
                let unstarted = current.execution_order == 0
                    && current.entries.iter().all(|e| e.undo.is_none());
                if !unstarted {
                    return Ok(Filtered {
                        target_keys: None,
                        entries,
                        removed: 0,
                    });
                }
                if state.jobs.iter().any(|j| {
                    j.id < id
                        && j.state == TransferState::Scanning
                        && !j.cancel
                        && !j.undo_requested
                }) {
                    state = self.shared.wake.wait(state).unwrap();
                    continue;
                }
                let preserve = !current.entries.is_empty();
                let current = current.snapshot();
                let older = state
                    .jobs
                    .iter()
                    .filter(|j| {
                        j.id < id
                            && !j.cancel
                            && !j.undo_requested
                            && (!terminal(&j.state)
                                || (j.state == TransferState::Done
                                    && current.dedup_candidates.contains(&j.id)))
                    })
                    .map(|j| j.snapshot())
                    .collect::<Vec<_>>();
                break (current, older, preserve);
            }
        };
        let mut prepared = job;
        prepared.entries = entries;
        prepared.destination_keys = None;
        let incoming = keys(&prepared);
        let older = older
            .into_iter()
            .map(|job| {
                let keys = keys(&job);
                (job, keys)
            })
            .collect::<Vec<_>>();
        let mut seen = HashSet::new();
        {
            let state = self.shared.state.lock().unwrap();
            let current = state
                .jobs
                .iter()
                .find(|j| j.id == id)
                .ok_or("Operation absent")?;
            if current.cancel
                || current.state != TransferState::Scanning
                || self.shared.stop.load(std::sync::atomic::Ordering::SeqCst)
            {
                return Err("Canceled".into());
            }
            for (snapshot, keys) in &older {
                let Some(actual) = state.jobs.iter().find(|j| j.id == snapshot.id) else {
                    continue;
                };
                if actual.cancel
                    || actual.undo_requested
                    || (terminal(&actual.state) && actual.state != TransferState::Done)
                {
                    continue;
                }
                for (i, entry) in snapshot.entries.iter().enumerate() {
                    let Some(now) = actual.entries.get(i) else {
                        continue;
                    };
                    if relevant(now)
                        && now.source == entry.source
                        && now.destination == entry.destination
                        && now.identity == entry.identity
                        && now.kind == entry.kind
                    {
                        seen.extend(entry_keys(snapshot, keys, i));
                    }
                }
            }
        }
        let mut keep = vec![true; prepared.entries.len()];
        for (i, e) in prepared.entries.iter().enumerate() {
            if e.state != TransferState::Waiting || e.hidden || e.cancel {
                continue;
            }
            let variants = entry_keys(&prepared, &incoming, i);
            keep[i] = !variants.iter().any(|key| seen.contains(key));
            seen.extend(variants);
        }
        keep_ancestors(&prepared.entries, &mut keep);
        let removed = keep.iter().filter(|keep| !**keep).count() as u64;
        entries = prepared.entries;
        let target_keys = if preserve {
            // Les indices déjà publiés dans l’historique restent valides ; aucun journal ne possède ces lignes.
            for (entry, keep) in entries.iter_mut().zip(&keep) {
                if !keep {
                    entry.state = TransferState::Skipped;
                    entry.hidden = true;
                    entry.cancel = false;
                }
            }
            incoming.target
        } else {
            entries = entries
                .into_iter()
                .zip(&keep)
                .filter(|(_, keep)| **keep)
                .map(|(e, _)| e)
                .collect();
            incoming.target.map(|keys| {
                keys.into_iter()
                    .zip(&keep)
                    .filter(|(_, keep)| **keep)
                    .map(|(key, _)| key)
                    .collect()
            })
        };
        Ok(Filtered {
            entries,
            removed,
            target_keys,
        })
    }
}
