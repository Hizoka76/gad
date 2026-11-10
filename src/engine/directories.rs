//! Agrégation des états des dossiers : relations réutilisées tant que les chemins sont identiques.
use super::*;
use std::collections::HashMap;
const MAX_CACHED_ENTRIES: usize = 16_384;
const MAX_CACHE_BYTES: usize = 8 * 1024 * 1024;

#[derive(Default)]
pub(super) struct Cache {
    index: Option<Box<Index>>,
}
impl Clone for Cache {
    /// Une copie métier ou historique ne doit pas prolonger la vie du cache actif.
    fn clone(&self) -> Self {
        Self::default()
    }
}
impl Cache {
    pub(super) fn clear(&mut self) {
        self.index = None;
    }
    #[cfg(test)]
    pub(super) fn populated(&self) -> bool {
        self.index.is_some()
    }
}
struct Shape {
    source: PathBuf,
    target: PathBuf,
    directory: bool,
}
struct Index {
    shape: Option<Vec<Shape>>,
    directories: Vec<usize>,
    ancestors: Vec<Vec<usize>>,
}
impl Index {
    fn matches(&self, entries: &[Entry]) -> bool {
        self.shape.as_ref().is_some_and(|shape| {
            shape.len() == entries.len()
                && shape.iter().zip(entries).all(|(old, entry)| {
                    old.directory == (entry.kind == "directory")
                        && old.source == entry.source
                        && old.target == entry.destination
                })
        })
    }
    fn build(entries: &[Entry], budget: usize) -> Option<Self> {
        let mut directories = HashMap::<&Path, Vec<usize>>::new();
        let mut indices = Vec::new();
        for (i, entry) in entries.iter().enumerate() {
            if entry.kind == "directory" {
                directories.entry(&entry.source).or_default().push(i);
                indices.push(i);
            }
        }
        // Les lots de fichiers seuls gardent le parcours simple, sans copier leurs chemins.
        if directories.is_empty() {
            return None;
        }
        let mut ancestors = vec![Vec::new(); entries.len()];
        for (i, entry) in entries.iter().enumerate() {
            for source in entry.source.ancestors() {
                if let Some(parents) = directories.get(source) {
                    for &parent in parents {
                        if parent != i
                            && entry.destination.starts_with(&entries[parent].destination)
                        {
                            ancestors[i].push(parent);
                        }
                    }
                }
            }
        }
        // Budget du texte et des relations conservées ; les grands graphes restent temporaires.
        let overhead = std::mem::size_of::<Shape>() + std::mem::size_of::<Vec<usize>>();
        let bytes = entries
            .iter()
            .zip(&ancestors)
            .fold(0usize, |sum, (entry, parents)| {
                sum.saturating_add(overhead)
                    .saturating_add(entry.source.as_os_str().len())
                    .saturating_add(entry.destination.as_os_str().len())
                    .saturating_add(
                        parents
                            .capacity()
                            .saturating_mul(std::mem::size_of::<usize>()),
                    )
            })
            .saturating_add(
                indices
                    .capacity()
                    .saturating_mul(std::mem::size_of::<usize>()),
            );
        let shape = (entries.len() <= MAX_CACHED_ENTRIES && bytes <= budget).then(|| {
            entries
                .iter()
                .map(|entry| Shape {
                    source: entry.source.clone(),
                    target: entry.destination.clone(),
                    directory: entry.kind == "directory",
                })
                .collect()
        });
        Some(Self {
            shape,
            directories: indices,
            ancestors,
        })
    }
}
/// Les états sont recalculés à chaque appel ; seuls les liens structurels sont mémorisés.
pub(super) fn finish(job: &mut Job) {
    finish_with_budget(job, MAX_CACHE_BYTES);
}
fn finish_with_budget(job: &mut Job, budget: usize) {
    #[derive(Clone, Copy, Default)]
    struct Counts {
        errors: i64,
        pending: i64,
        cancelled: i64,
    }
    impl Counts {
        fn add(&mut self, state: TransferState, delta: i64) {
            self.errors += delta * i64::from(state == TransferState::Error);
            self.pending += delta
                * i64::from(!matches!(
                    state,
                    TransferState::Done | TransferState::Skipped | TransferState::Cancelled
                ));
            self.cancelled += delta * i64::from(state == TransferState::Cancelled);
        }
        fn state(self) -> TransferState {
            if self.errors > 0 {
                TransferState::Error
            } else if self.pending > 0 {
                TransferState::Running
            } else if self.cancelled > 0 {
                TransferState::Cancelled
            } else {
                TransferState::Done
            }
        }
    }

    let (entries, cache) = (&mut job.entries, &mut job.directory_index);
    if cache
        .index
        .as_ref()
        .is_none_or(|index| !index.matches(entries))
    {
        cache.index = Index::build(entries, budget).map(Box::new);
    }
    let Some(index) = cache.index.as_ref() else {
        return;
    };
    let mut counts = vec![Counts::default(); entries.len()];
    for (i, entry) in entries.iter().enumerate() {
        for &parent in &index.ancestors[i] {
            counts[parent].add(entry.state, 1);
        }
    }
    // Le même ordre inverse conserve les règles des reprises et des dossiers dupliqués.
    for &i in index.directories.iter().rev() {
        let entry = &entries[i];
        let before = entry.state;
        if matches!(
            before,
            TransferState::Waiting | TransferState::Undone | TransferState::Error
        ) {
            continue;
        }
        let after = if entry.cancel {
            TransferState::Cancelled
        } else {
            counts[i].state()
        };
        if before != after {
            entries[i].state = after;
            for &parent in &index.ancestors[i] {
                counts[parent].add(before, -1);
                counts[parent].add(after, 1);
            }
        }
    }
    if index.shape.is_none() {
        cache.clear();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn small_cache_budget_still_updates_states_without_retaining_the_index() {
        let mut job:Job=serde_json::from_value(json!({"id":1,"sources":[],"destination":"/target","mode":"copy","verify_md5":false,"state":"running","error":"","cancel":false,"policy":"ask","entries":[
            {"source":"/source","destination":"/target","kind":"directory","size":0,"copied":0,"verified":0,"state":"running","error":""},
            {"source":"/source/f","destination":"/target/f","kind":"file","size":1,"copied":1,"verified":0,"state":"done","error":""}
        ]})).unwrap();
        finish_with_budget(&mut job, 1);
        assert_eq!(job.entries[0].state, TransferState::Done);
        assert!(!job.directory_index.populated());
        job.entries[0].state = TransferState::Running;
        job.entries[1].state = TransferState::Waiting;
        finish(&mut job);
        assert_eq!(job.entries[0].state, TransferState::Running);
        assert!(job.directory_index.populated());
    }
}
