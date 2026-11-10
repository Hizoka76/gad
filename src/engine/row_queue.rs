//! Forêt de fichiers indépendante des demandes : indices métier immuables et ordre visuel persistant.
use super::*;
use std::collections::{HashMap, HashSet};
type Dependencies = HashSet<(u64, u64)>;
type Predecessors = HashMap<Key, HashSet<Key>>;
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub(super) struct Key {
    pub job: u64,
    pub index: Option<usize>,
}
#[derive(Clone)]
pub(super) struct Layout {
    shape: Vec<(u64, usize)>,
    batches: Vec<u64>,
    pub roots: Vec<Key>,
    pub children: HashMap<Option<Key>, Vec<Key>>,
    pub parents: HashMap<Key, Option<Key>>,
    pub ordered: Vec<Key>,
    pub root_of: HashMap<Key, Key>,
    dependencies: Dependencies,
    predecessors: Predecessors,
}
/// Recrée l’index seulement après analyse, retrait de demande ou changement explicite d’ordre.
pub(super) fn layout(state: &mut State) -> Arc<Layout> {
    if let Some(cache) = &state.row_layout {
        if cache.batches == state.queue_order
            && cache.shape.len() == state.jobs.len()
            && cache
                .shape
                .iter()
                .zip(&state.jobs)
                .all(|((id, count), job)| *id == job.id && *count == job.entries.len())
        {
            return cache.clone();
        }
    }
    // Capturer la forme seulement quand une reconstruction est réellement nécessaire.
    let shape = state
        .jobs
        .iter()
        .map(|job| (job.id, job.entries.len()))
        .collect::<Vec<_>>();
    let mut children: HashMap<Option<Key>, Vec<Key>> = HashMap::new();
    let mut parents = HashMap::new();
    for job in queue::ordered(state) {
        if job.entries.is_empty() {
            let key = Key {
                job: job.id,
                index: None,
            };
            children.entry(None).or_default().push(key);
            parents.insert(key, None);
            continue;
        }
        let mut directories: HashMap<(PathBuf, PathBuf), Vec<usize>> = HashMap::new();
        for (i, e) in job.entries.iter().enumerate() {
            if e.kind == "directory" {
                directories
                    .entry((e.source.clone(), e.destination.clone()))
                    .or_default()
                    .push(i);
            }
        }
        for (i, e) in job.entries.iter().enumerate() {
            let key = Key {
                job: job.id,
                index: Some(i),
            };
            let mut parent = None;
            let (mut source, mut target) = (e.source.as_path(), e.destination.as_path());
            while let (Some(s), Some(t)) = (source.parent(), target.parent()) {
                source = s;
                target = t;
                if let Some(indices) = directories.get(&(s.to_path_buf(), t.to_path_buf())) {
                    let candidate = indices
                        .iter()
                        .copied()
                        .take_while(|n| *n < i)
                        .last()
                        .unwrap_or(indices[0]);
                    parent = Some(Key {
                        job: job.id,
                        index: Some(candidate),
                    });
                    break;
                }
            }
            parents.insert(key, parent);
            children.entry(parent).or_default().push(key);
        }
    }
    let ranks = state
        .row_order
        .iter()
        .enumerate()
        .map(|(i, key)| (*key, i))
        .collect::<HashMap<_, _>>();
    for siblings in children.values_mut() {
        siblings.sort_by_key(|key| {
            ranks
                .get(key)
                .or_else(|| {
                    if parents.get(key) == Some(&None) {
                        ranks.get(&Key {
                            job: key.job,
                            index: None,
                        })
                    } else {
                        None
                    }
                })
                .copied()
                .unwrap_or(usize::MAX)
        });
    }
    let roots = children.get(&None).cloned().unwrap_or_default();
    let (ordered, root_of) = flatten(&children);
    let (dependencies, predecessors) = dependent_pairs(state, &root_of);
    let result = Arc::new(Layout {
        shape,
        batches: state.queue_order.clone(),
        roots,
        children,
        parents,
        ordered,
        root_of,
        dependencies,
        predecessors,
    });
    state.row_layout = Some(result.clone());
    state.row_revision = snapshot::touch();
    result
}
/// Préordre itératif : un dossier reste toujours avant ses descendants, même après une reprise.
fn flatten(children: &HashMap<Option<Key>, Vec<Key>>) -> (Vec<Key>, HashMap<Key, Key>) {
    let mut ordered = Vec::new();
    let mut root_of = HashMap::new();
    let mut pending = children
        .get(&None)
        .into_iter()
        .flatten()
        .rev()
        .map(|key| (*key, *key))
        .collect::<Vec<_>>();
    while let Some((key, root)) = pending.pop() {
        ordered.push(key);
        root_of.insert(key, root);
        if let Some(items) = children.get(&Some(key)) {
            pending.extend(items.iter().rev().map(|item| (*item, root)));
        }
    }
    (ordered, root_of)
}
/// Une ligne en attente est déplaçable, mais aucune branche contenant une écriture active ne l’est.
pub(super) fn movable(state: &State, key: Key) -> bool {
    let Some(job) = state.jobs.iter().find(|j| j.id == key.job) else {
        return false;
    };
    if job.cancel || job.undo_requested || terminal(&job.state) {
        return false;
    }
    match key.index {
        None => job.state == TransferState::Scanning && job.entries.is_empty(),
        Some(i) => job
            .entries
            .get(i)
            .is_some_and(|e| e.state == TransferState::Waiting && !e.hidden && !e.cancel),
    }
}
/// Choisit les demandes selon leur première branche encore exécutable ; les réservations restent par demande.
pub(super) fn ordered_jobs(state: &mut State) -> Vec<u64> {
    let view = layout(state);
    let mut seen = HashSet::new();
    let mut result = Vec::new();
    let jobs = state
        .jobs
        .iter()
        .map(|j| (j.id, &**j))
        .collect::<HashMap<_, _>>();
    let blocked = blocked_roots(state, &view);
    let ready = view
        .ordered
        .iter()
        .filter_map(|key| {
            let job = jobs.get(&key.job)?;
            let runnable = job.state == TransferState::Scanning
                || key.index.is_some_and(|i| {
                    job.entries[i].state == TransferState::Waiting
                        && (!job.entries[i].paused || job.entries[i].cancel)
                });
            runnable.then(|| view.root_of[key])
        })
        .collect::<HashSet<_>>();
    for root in &view.roots {
        if ready.contains(root) && !blocked.contains(root) && seen.insert(root.job) {
            result.push(root.job);
        }
    }
    // Les lots sans travail restant doivent encore finaliser leurs annulations ou dossiers.
    for job in queue::ordered(state) {
        if !job
            .entries
            .iter()
            .any(|e| e.state == TransferState::Waiting && e.paused && !e.cancel)
            && seen.insert(job.id)
        {
            result.push(job.id);
        }
    }
    result
}
/// Un travailleur traite une branche, puis rend la main pour permettre l’alternance entre demandes.
pub(super) fn branch(state: &mut State, id: u64) -> (Vec<usize>, u64) {
    let view = layout(state);
    let Some(job) = state.jobs.iter().find(|j| j.id == id) else {
        return (Vec::new(), state.row_revision);
    };
    let blocked = blocked_roots(state, &view);
    let root = view
        .ordered
        .iter()
        .find(|key| {
            key.job == id
                && !blocked.contains(&view.root_of[key])
                && key.index.is_some_and(|i| {
                    job.entries[i].state == TransferState::Waiting
                        && (!job.entries[i].paused || job.entries[i].cancel)
                })
        })
        .map(|key| view.root_of[key]);
    let indices = if let Some(root) = root {
        view.ordered
            .iter()
            .filter(|key| view.root_of.get(key) == Some(&root))
            .filter_map(|k| k.index)
            .collect()
    } else {
        Vec::new()
    };
    (indices, state.row_revision)
}
/// Conserve les dépendances des remplacements : les demandes partageant une destination restent contiguës entre elles.
fn validate_dependencies(state: &State, view: &Layout, order: &[Key]) -> Result<(), String> {
    let ranks = order
        .iter()
        .enumerate()
        .map(|(i, k)| (*k, i))
        .collect::<HashMap<_, _>>();
    // Dans une demande, deux noms visant la même cible gardent leur chronologie de journal.
    for job in &state.jobs {
        let mut targets: HashMap<&Path, usize> = HashMap::new();
        for (i, e) in job.entries.iter().enumerate() {
            let target = job
                .destination_keys
                .as_ref()
                .and_then(|keys| keys.get(i))
                .unwrap_or(&e.destination);
            if let Some(previous) = targets.insert(target, i) {
                if ranks.get(&Key {
                    job: job.id,
                    index: Some(previous),
                }) > ranks.get(&Key {
                    job: job.id,
                    index: Some(i),
                }) {
                    return Err("These files target the same destination; their order is preserved to allow restoration.".into());
                }
            }
        }
    }
    let roots = order
        .iter()
        .filter(|k| view.parents.get(k) == Some(&None))
        .copied()
        .collect::<Vec<_>>();
    let mut ranges = HashMap::new();
    for (position, key) in roots.iter().enumerate() {
        ranges
            .entry(key.job)
            .and_modify(|range: &mut (usize, usize)| range.1 = position)
            .or_insert((position, position));
    }
    for (a, b) in &view.dependencies {
        if state
            .jobs
            .iter()
            .any(|j| (j.id == *a || j.id == *b) && terminal(&j.state))
        {
            continue;
        }
        if let (Some((a0, a1)), Some((b0, b1))) = (ranges.get(a), ranges.get(b)) {
            if a0 <= b1 && b0 <= a1 {
                return Err("These requests replace shared destinations. Select their branches together: interleaving them would prevent reliable restoration.".into());
            }
        }
    }
    Ok(())
}
/// La plupart des chemins n'appartiennent qu'à une branche : aucune table allouée dans ce cas.
/// Les collisions réelles conservent un ensemble haché, sans recherche linéaire croissante.
enum RootSet {
    Single(Key),
    Multiple(HashSet<Key>),
}
impl RootSet {
    fn insert(&mut self, root: Key) {
        match self {
            Self::Single(previous) if *previous != root => {
                *self = Self::Multiple(HashSet::from([*previous, root]));
            }
            Self::Multiple(roots) => {
                roots.insert(root);
            }
            _ => {}
        }
    }
    fn visit(&self, mut visit: impl FnMut(&Key)) {
        match self {
            Self::Single(root) => visit(root),
            Self::Multiple(roots) => {
                for root in roots {
                    visit(root);
                }
            }
        }
    }
}
/// Indexe aussi les destinations contenues dans un dossier d’une autre demande.
fn dependent_pairs(state: &State, root_of: &HashMap<Key, Key>) -> (Dependencies, Predecessors) {
    let mut targets: HashMap<&Path, RootSet> = HashMap::new();
    let mut directories: HashMap<&Path, RootSet> = HashMap::new();
    fn paths(job: &Job, index: usize) -> [Option<&Path>; 2] {
        [
            Some(job.entries[index].destination.as_path()),
            job.destination_keys
                .as_ref()
                .and_then(|keys| keys.get(index))
                .map(PathBuf::as_path)
                .filter(|path| *path != job.entries[index].destination),
        ]
    }
    for job in &state.jobs {
        for (i, entry) in job.entries.iter().enumerate() {
            for path in paths(job, i).into_iter().flatten() {
                let key = Key {
                    job: job.id,
                    index: Some(i),
                };
                // Les décisions portent sur les branches : ne pas conserver leurs feuilles.
                let root = root_of[&key];
                targets
                    .entry(path)
                    .and_modify(|roots| roots.insert(root))
                    .or_insert(RootSet::Single(root));
                if entry.kind == "directory" {
                    directories
                        .entry(path)
                        .and_modify(|roots| roots.insert(root))
                        .or_insert(RootSet::Single(root));
                }
            }
        }
    }
    let mut pairs = HashSet::new();
    let mut predecessors: HashMap<Key, HashSet<Key>> = HashMap::new();
    for job in &state.jobs {
        for (i, _) in job.entries.iter().enumerate() {
            let current = Key {
                job: job.id,
                index: Some(i),
            };
            let root = root_of[&current];
            // Les ensembles de sortie dédoublonnent déjà les relations entre branches.
            // Aucun ensemble temporaire de feuilles n'est nécessaire pour chaque fichier.
            let mut relate = |other: &Key| {
                if *other == root {
                    return;
                }
                if other.job != job.id {
                    pairs.insert((job.id.min(other.job), job.id.max(other.job)));
                } else {
                    let (early, late) = if root.index < other.index {
                        (root, *other)
                    } else {
                        (*other, root)
                    };
                    predecessors.entry(late).or_default().insert(early);
                }
            };
            for path in paths(job, i).into_iter().flatten() {
                if let Some(roots) = targets.get(path) {
                    roots.visit(&mut relate);
                }
                for parent in path.ancestors().skip(1) {
                    if let Some(roots) = directories.get(parent) {
                        roots.visit(&mut relate);
                    }
                }
            }
        }
    }
    (pairs, predecessors)
}
/// Une racine ayant une destination répétée attend la fin de ses prédécesseurs de journal.
fn blocked_roots(state: &State, view: &Layout) -> HashSet<Key> {
    let pending = state
        .jobs
        .iter()
        .flat_map(|j| {
            j.entries
                .iter()
                .enumerate()
                .filter(|(_, e)| e.state == TransferState::Waiting)
                .map(|(i, _)| {
                    view.root_of[&Key {
                        job: j.id,
                        index: Some(i),
                    }]
                })
        })
        .collect::<HashSet<_>>();
    view.predecessors
        .iter()
        .filter(|(_, previous)| previous.iter().any(|root| pending.contains(root)))
        .map(|(root, _)| *root)
        .collect()
}
/// Une demande partiellement publiée garde sa place devant ses dépendantes, même lorsqu’un de ses fichiers est en pause.
pub(super) fn blocked_by_partial_request(state: &State, id: u64) -> bool {
    let Some(view) = &state.row_layout else {
        return false;
    };
    let effects = |job: &Job| {
        job.entries.iter().any(|e| {
            e.undo
                .as_ref()
                .is_some_and(|r| r.result.is_some() && !r.reverted)
        })
    };
    let Some(candidate) = state.jobs.iter().find(|j| j.id == id) else {
        return false;
    };
    if effects(candidate) {
        return false;
    }
    state.jobs.iter().any(|other| {
        other.id != id
            && !terminal(&other.state)
            && !other.cancel
            && !other.undo_requested
            && view
                .dependencies
                .contains(&(id.min(other.id), id.max(other.id)))
            && other
                .entries
                .iter()
                .any(|e| e.state == TransferState::Waiting && !e.cancel && !e.hidden)
            && effects(other)
    })
}

/// Regroupe explicitement les branches lors d’une ancienne commande de priorité de demande entière.
pub(super) fn batch_order(state: &mut State) {
    let view = layout(state);
    let rank = queue::ids(state)
        .iter()
        .enumerate()
        .map(|(i, id)| (*id, i))
        .collect::<HashMap<_, _>>();
    let mut children = view.children.clone();
    if let Some(roots) = children.get_mut(&None) {
        roots.sort_by_key(|k| rank.get(&k.job).copied().unwrap_or(usize::MAX));
    }
    state.row_order = flatten(&children).0;
    state.row_layout = None;
}
impl Engine {
    /// Valide les références stables et les parents avant d’enregistrer un changement de priorité visuel.
    pub(super) fn reorder_rows(&self, value: &Value) -> Result<Value, String> {
        let action = value["action"].as_str().ok_or("Missing priority action")?;
        if !["next", "up", "down", "last", "before", "after"].contains(&action) {
            return Err("Unknown priority".into());
        }
        let selected: Vec<Key> =
            serde_json::from_value(value["rows"].clone()).map_err(|_| "Invalid selected rows")?;
        let chosen = selected.iter().copied().collect::<HashSet<_>>();
        if chosen.is_empty() || chosen.len() != selected.len() {
            return Err("Empty or duplicate selection".into());
        }
        let order = {
            let mut state = self.shared.state.lock().unwrap();
            let view = layout(&mut state);
            let mut active_ancestors = HashSet::new();
            for key in &view.ordered {
                if key.index.is_some_and(|i| {
                    state
                        .jobs
                        .iter()
                        .find(|j| j.id == key.job)
                        .is_some_and(|j| {
                            matches!(
                                j.entries[i].state,
                                TransferState::Running
                                    | TransferState::Verifying
                                    | TransferState::Conflict
                            )
                        })
                }) {
                    let mut parent = view.parents[key];
                    while let Some(p) = parent {
                        active_ancestors.insert(p);
                        parent = view.parents[&p];
                    }
                }
            }
            for key in &selected {
                if !view.parents.contains_key(key) || !movable(&state, *key) {
                    return Err("Only queued rows can be moved. The list may have changed.".into());
                }
                if active_ancestors.contains(key) {
                    return Err(
                        "This branch contains an active transfer; move only its queued files."
                            .into(),
                    );
                }
            }
            let mut selected = view
                .ordered
                .iter()
                .filter(|k| chosen.contains(k) && !ancestor_selected(&view, **k, &chosen))
                .copied()
                .collect::<Vec<_>>();
            let mut children = view.children.clone();
            if action == "before" || action == "after" {
                let target: Key = serde_json::from_value(value["target"].clone())
                    .map_err(|_| "Invalid move target")?;
                let parent = *view
                    .parents
                    .get(&target)
                    .ok_or("La ligne cible n’existe plus")?;
                if chosen.contains(&target) || selected.iter().any(|k| view.parents[k] != parent) {
                    return Err("Keep files in their folder. Move an entire branch to change its position among other folders.".into());
                }
                let siblings = children.get_mut(&parent).ok_or("Parent absent")?;
                let moving = selected.iter().copied().collect::<HashSet<_>>();
                siblings.retain(|k| !moving.contains(k));
                let position = siblings
                    .iter()
                    .position(|k| *k == target)
                    .ok_or("Target moved")?
                    + usize::from(action == "after");
                siblings.splice(position..position, selected);
            } else {
                // Traiter ensuite promeut aussi les ancêtres encore en attente, sans toucher à une branche active.
                if action == "next" {
                    let mut ancestors = Vec::new();
                    for key in &selected {
                        let mut parent = view.parents[key];
                        while let Some(p) = parent {
                            if movable(&state, p) {
                                ancestors.push(p);
                            }
                            parent = view.parents[&p];
                        }
                    }
                    selected.extend(ancestors);
                }
                let moving = selected.iter().copied().collect::<HashSet<_>>();
                for siblings in children.values_mut() {
                    match action {
                        "next" => siblings.sort_by_key(|k| !moving.contains(k)),
                        "last" => siblings.sort_by_key(|k| moving.contains(k)),
                        "up" => {
                            for i in 1..siblings.len() {
                                if moving.contains(&siblings[i])
                                    && !moving.contains(&siblings[i - 1])
                                {
                                    siblings.swap(i, i - 1);
                                }
                            }
                        }
                        "down" => {
                            for i in (1..siblings.len()).rev() {
                                if moving.contains(&siblings[i - 1])
                                    && !moving.contains(&siblings[i])
                                {
                                    siblings.swap(i - 1, i);
                                }
                            }
                        }
                        _ => unreachable!(),
                    }
                }
            }
            let order = flatten(&children).0;
            validate_dependencies(&state, &view, &order)?;
            state.row_order = order.clone();
            state.row_layout = None;
            layout(&mut state);
            order
        };
        self.save()?;
        self.shared.wake.notify_all();
        Ok(json!({"ok":true,"row_order":order}))
    }
}
/// Vérifie les ascendants sans interpréter la profondeur comme un indice moteur.
fn ancestor_selected(view: &Layout, key: Key, selected: &HashSet<Key>) -> bool {
    let mut parent = view.parents[&key];
    while let Some(p) = parent {
        if selected.contains(&p) {
            return true;
        }
        parent = view.parents[&p];
    }
    false
}

#[cfg(test)]
mod dependency_tests {
    use super::*;
    fn fixture(files: usize, shared: bool) -> (State, HashMap<Key, Key>) {
        let mut jobs = Vec::new();
        let mut root_of = HashMap::new();
        for id in 1..=2 {
            let target = if shared {
                "/out/common".to_owned()
            } else {
                format!("/out/{id}")
            };
            let entries = (0..=files).map(|index| {
                let suffix = if index == 0 { String::new() } else { format!("/f{index}") };
                json!({"source":format!("/source/{id}{suffix}"),"destination":format!("{target}{suffix}"),"kind":if index==0 {"directory"}else{"file"},"size":0,"copied":0,"verified":0,"state":"waiting","error":""})
            }).collect::<Vec<_>>();
            jobs.push(serde_json::from_value(json!({"id":id,"sources":[],"destination":"/out","mode":"copy","verify_md5":false,"state":"waiting","error":"","cancel":false,"policy":"ask","entries":entries})).unwrap());
            let root = Key {
                job: id,
                index: Some(0),
            };
            for index in 0..=files {
                root_of.insert(
                    Key {
                        job: id,
                        index: Some(index),
                    },
                    root,
                );
            }
        }
        (
            State {
                jobs,
                ..State::default()
            },
            root_of,
        )
    }
    fn reference_pairs(state: &State, root_of: &HashMap<Key, Key>) -> (Dependencies, Predecessors) {
        let mut targets: HashMap<&Path, HashSet<Key>> = HashMap::new();
        let mut directories: HashMap<&Path, HashSet<Key>> = HashMap::new();
        fn paths(job: &Job, index: usize) -> [Option<&Path>; 2] {
            [
                Some(job.entries[index].destination.as_path()),
                job.destination_keys
                    .as_ref()
                    .and_then(|keys| keys.get(index))
                    .map(PathBuf::as_path),
            ]
        }
        for job in &state.jobs {
            for (i, entry) in job.entries.iter().enumerate() {
                for path in paths(job, i).into_iter().flatten() {
                    let key = Key {
                        job: job.id,
                        index: Some(i),
                    };
                    targets.entry(path).or_default().insert(key);
                    if entry.kind == "directory" {
                        directories.entry(path).or_default().insert(key);
                    }
                }
            }
        }
        let mut pairs = HashSet::new();
        let mut predecessors: HashMap<Key, HashSet<Key>> = HashMap::new();
        for job in &state.jobs {
            for (i, _) in job.entries.iter().enumerate() {
                let current = Key {
                    job: job.id,
                    index: Some(i),
                };
                let root = root_of[&current];
                let mut related = HashSet::new();
                for path in paths(job, i).into_iter().flatten() {
                    related.extend(targets.get(path).into_iter().flatten().copied());
                    for parent in path.ancestors().skip(1) {
                        related.extend(directories.get(parent).into_iter().flatten().copied());
                    }
                }
                for other in related {
                    if other.job != job.id {
                        pairs.insert((job.id.min(other.job), job.id.max(other.job)));
                    } else {
                        let other = root_of[&other];
                        if other != root {
                            let (early, late) = if root.index < other.index {
                                (root, other)
                            } else {
                                (other, root)
                            };
                            predecessors.entry(late).or_default().insert(early);
                        }
                    }
                }
            }
        }
        (pairs, predecessors)
    }
    #[test]
    fn branch_dependencies_match_reference_with_aliases_and_duplicate_targets() {
        fn next(seed: &mut u64) -> usize {
            *seed = seed.wrapping_mul(6364136223846793005).wrapping_add(1);
            (*seed >> 32) as usize
        }
        let paths = [
            "/out",
            "/out/a",
            "/out/a/f",
            "/out/a/deep/f",
            "/out/b",
            "/out/b/f",
            "/alias",
            "/alias/a",
            "/alias/a/f",
        ];
        let mut seed = 0x6ad12345;
        for case in 0..500 {
            let (mut state, _) = fixture(36, case % 2 == 0);
            if case % 5 == 0 {
                state.jobs.pop();
            }
            let mut roots = HashMap::new();
            for job in &mut state.jobs {
                let id = job.id;
                job.entries.truncate(4 + next(&mut seed) % 33);
                for (index, entry) in job.entries.iter_mut().enumerate() {
                    entry.destination = paths[next(&mut seed) % paths.len()].into();
                    entry.kind = if index < 4 || next(&mut seed) % 4 == 0 {
                        "directory"
                    } else {
                        "file"
                    }
                    .into();
                    let root = if index < 4 {
                        index
                    } else {
                        next(&mut seed) % 4
                    };
                    roots.insert(
                        Key {
                            job: id,
                            index: Some(index),
                        },
                        Key {
                            job: id,
                            index: Some(root),
                        },
                    );
                }
                job.destination_keys = match case % 4 {
                    0 => None,
                    1 => Some(
                        job.entries
                            .iter()
                            .map(|entry| entry.destination.clone())
                            .collect(),
                    ),
                    _ => Some(
                        (0..job.entries.len().saturating_sub(case % 3))
                            .map(|_| PathBuf::from(paths[next(&mut seed) % paths.len()]))
                            .collect(),
                    ),
                };
            }
            assert_eq!(
                dependent_pairs(&state, &roots),
                reference_pairs(&state, &roots),
                "case {case}"
            );
        }
    }
    #[test]
    fn repeated_targets_keep_all_predecessors_between_branches() {
        let (mut state, _) = fixture(64, false);
        state.jobs.truncate(1);
        let mut roots = HashMap::new();
        for (index, entry) in state.jobs[0].entries.iter_mut().enumerate() {
            entry.destination = "/out/shared".into();
            roots.insert(
                Key {
                    job: 1,
                    index: Some(index),
                },
                Key {
                    job: 1,
                    index: Some(index % 4),
                },
            );
        }
        let result = dependent_pairs(&state, &roots);
        assert_eq!(result, reference_pairs(&state, &roots));
        assert!(result.0.is_empty());
        for late in 1..4 {
            let expected = (0..late)
                .map(|index| Key {
                    job: 1,
                    index: Some(index),
                })
                .collect::<HashSet<_>>();
            assert_eq!(
                result.1[&Key {
                    job: 1,
                    index: Some(late)
                }],
                expected
            );
        }
    }
    #[test]
    #[ignore = "benchmark explicite du graphe de dépendances"]
    fn benchmark_dependency_graph() {
        for shared in [false, true] {
            let (state, roots) = fixture(5000, shared);
            for trial in 0..3 {
                let start = std::time::Instant::now();
                for _ in 0..5 {
                    std::hint::black_box(dependent_pairs(std::hint::black_box(&state), &roots));
                }
                println!(
                    "dependency_graph shared={shared} trial={trial} elapsed_ms={:.3}",
                    start.elapsed().as_secs_f64() * 1000.0
                );
            }
        }
    }
    #[test]
    #[ignore = "benchmark explicite de la construction et du cache de l'arbre"]
    fn benchmark_row_layout() {
        let (mut state, _) = fixture(5000, false);
        for trial in 0..3 {
            let start = std::time::Instant::now();
            for _ in 0..5 {
                state.row_layout = None;
                std::hint::black_box(layout(std::hint::black_box(&mut state)));
            }
            println!(
                "row_layout cold trial={trial} elapsed_ms={:.3}",
                start.elapsed().as_secs_f64() * 1000.0
            );
        }
        let mut template = state.jobs[0].clone();
        template.entries.clear();
        let mut history = State::default();
        for id in 1..=2000 {
            let mut job = template.clone();
            job.id = id;
            history.jobs.push(job);
        }
        layout(&mut history);
        for trial in 0..3 {
            let start = std::time::Instant::now();
            for _ in 0..1000 {
                std::hint::black_box(layout(std::hint::black_box(&mut history)));
            }
            println!(
                "row_layout cached trial={trial} elapsed_ms={:.3}",
                start.elapsed().as_secs_f64() * 1000.0
            );
        }
    }
}
