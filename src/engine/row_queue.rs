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
    let shape = state
        .jobs
        .iter()
        .map(|j| (j.id, j.entries.len()))
        .collect::<Vec<_>>();
    if let Some(cache) = &state.row_layout {
        if cache.shape == shape && cache.batches == state.queue_order {
            return cache.clone();
        }
    }
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
/// Indexe aussi les destinations contenues dans un dossier d’une autre demande.
fn dependent_pairs(state: &State, root_of: &HashMap<Key, Key>) -> (Dependencies, Predecessors) {
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
