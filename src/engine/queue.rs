//! Ordre de lancement indépendant de l’historique et des indices de fichiers.
use super::*;
use std::collections::{HashMap, HashSet};

/// Ignore les identifiants retirés et ajoute les nouveaux lots en fin de file.
pub(super) fn ids(state: &State) -> Vec<u64> {
    let live = state.jobs.iter().map(|j| j.id).collect::<HashSet<_>>();
    let mut seen = HashSet::new();
    state
        .queue_order
        .iter()
        .copied()
        .chain(state.jobs.iter().map(|j| j.id))
        .filter(|id| live.contains(id) && seen.insert(*id))
        .collect()
}
/// Vue ordonnée pour l’ordonnanceur ; aucun déplacement du journal chronologique.
pub(super) fn ordered(state: &State) -> Vec<&Job> {
    let jobs = state
        .jobs
        .iter()
        .map(|j| (j.id, &**j))
        .collect::<HashMap<_, _>>();
    ids(state)
        .iter()
        .filter_map(|id| jobs.get(id).copied())
        .collect()
}
/// Un transfert déjà réservé et une remise en état ne peuvent pas changer de priorité.
pub(super) fn movable(state: &State, job: &Job) -> bool {
    matches!(job.state, TransferState::Waiting | TransferState::Scanning)
        && !job.cancel
        && !job.undo_requested
        && !state.active.contains_key(&job.id)
}
impl Engine {
    /// Valide toute la sélection avant de déplacer les lots, en gardant leur ordre relatif.
    pub(super) fn reorder_queue(&self, value: &Value) -> Result<Value, String> {
        let action = value["action"].as_str().ok_or("Missing priority action")?;
        if !["next", "up", "down", "last"].contains(&action) {
            return Err("Unknown priority".into());
        }
        let selected = value["ids"]
            .as_array()
            .ok_or("Missing selected batches")?
            .iter()
            .map(|id| id.as_u64().ok_or("Identifiant invalide"))
            .collect::<Result<Vec<_>, _>>()?;
        let selected_set = selected.iter().copied().collect::<HashSet<_>>();
        if selected.is_empty() || selected_set.len() != selected.len() {
            return Err("Empty or duplicate batch selection".into());
        }
        let order = {
            let mut state = self.shared.state.lock().unwrap();
            for id in &selected {
                let job = state
                    .jobs
                    .iter()
                    .find(|j| j.id == *id)
                    .ok_or("Lot introuvable ; actualisez la liste")?;
                if !movable(&state, job) {
                    return Err("Only queued or scanning batches can be reordered; active transfers are never interrupted".into());
                }
            }
            let mut order = ids(&state);
            let moving = state
                .jobs
                .iter()
                .filter(|j| movable(&state, j))
                .map(|j| j.id)
                .collect::<HashSet<_>>();
            let slots = order
                .iter()
                .enumerate()
                .filter(|(_, id)| moving.contains(id))
                .map(|(i, _)| i)
                .collect::<Vec<_>>();
            let mut pending = slots.iter().map(|i| order[*i]).collect::<Vec<_>>();
            match action {
                "next" => pending.sort_by_key(|id| !selected_set.contains(id)),
                "last" => pending.sort_by_key(|id| selected_set.contains(id)),
                "up" => {
                    for i in 1..pending.len() {
                        if selected_set.contains(&pending[i])
                            && !selected_set.contains(&pending[i - 1])
                        {
                            pending.swap(i, i - 1);
                        }
                    }
                }
                "down" => {
                    for i in (1..pending.len()).rev() {
                        if selected_set.contains(&pending[i - 1])
                            && !selected_set.contains(&pending[i])
                        {
                            pending.swap(i - 1, i);
                        }
                    }
                }
                _ => unreachable!(),
            }
            for (slot, id) in slots.iter().zip(pending) {
                order[*slot] = id;
            }
            state.queue_order = order.clone();
            row_queue::batch_order(&mut state);
            order
        };
        self.save()?;
        self.shared.wake.notify_all();
        Ok(json!({"ok":true,"queue_order":order}))
    }
}
