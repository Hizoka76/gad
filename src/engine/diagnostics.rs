//! Diagnostic volatil : expliquer les réservations réelles sans modifier les lots ni leur journal.
use super::*;

/// Les observations temporelles restent fraîches, même quand la structure est triviale.
fn observation(
    job: &Job,
    global_rate: u64,
    limiter: &rate::Limiter,
    reason: &str,
    blockers: &[u64],
) -> Value {
    let (global_wait, request_wait) = limiter.waiting(job.id, std::time::Instant::now());
    json!({"queue_reason":reason,"blockers":blockers,"global_throttled":global_rate>0 && global_wait,"request_throttled":job.rate_limit>0 && request_wait,"request_rate_limit":job.rate_limit})
}

/// Même ordre et mêmes règles que l’ordonnanceur ; aucune interrogation des disques sous ce verrou.
pub(super) fn snapshot(state: &mut State, limiter: &rate::Limiter) -> Value {
    let mut result = serde_json::Map::new();
    if state.jobs.iter().all(|job| terminal(&job.state)) {
        return Value::Object(result);
    }
    // Les lots simplement préparés ne demandent aucun calcul des dépendances de leurs fichiers.
    if state.jobs.iter().all(|job| {
        terminal(&job.state) || (job.start_required && job.state != TransferState::Scanning)
    }) {
        for job in state.jobs.iter().filter(|job| !terminal(&job.state)) {
            result.insert(
                job.id.to_string(),
                observation(job, state.rate_limit, limiter, "prepared", &[]),
            );
        }
        return Value::Object(result);
    }
    let ordered = row_queue::ordered_jobs(state);
    let jobs = state
        .jobs
        .iter()
        .map(|job| (job.id, &**job))
        .collect::<std::collections::HashMap<_, _>>();
    let mut earlier: Vec<&Job> = Vec::new();
    let mut scanning = None;
    let mut active = state.active.keys().copied().collect::<Vec<_>>();
    active.sort_unstable();
    let restoring = state
        .jobs
        .iter()
        .filter(|job| job.undo_requested)
        .map(|job| job.id)
        .collect::<Vec<_>>();
    for id in ordered {
        let Some(job) = jobs.get(&id).copied() else {
            continue;
        };
        if terminal(&job.state) {
            continue;
        }
        let mut blockers = Vec::new();
        let reason = if job.start_required && job.state != TransferState::Scanning {
            "prepared"
        } else if job.state == TransferState::DeviceWaiting {
            "device"
        } else if state.active.contains_key(&id) {
            "active"
        } else if job.state == TransferState::Scanning {
            "scan"
        } else if job.undo_requested {
            blockers = active.clone();
            "restoration_exclusive"
        } else if state.paused || job.paused {
            "paused"
        } else if !restoring.is_empty() {
            blockers = restoring.clone();
            "restoration_priority"
        } else if active.len() >= 2 {
            blockers = active.clone();
            "parallel_limit"
        } else if let Some(id) = scanning {
            blockers.push(id);
            "earlier_scan"
        } else if row_queue::blocked_by_partial_request(state, id) {
            "row_order"
        } else {
            let incompatible = active
                .iter()
                .filter_map(|id| jobs.get(id).copied())
                .find(|other| !scheduling::compatible_jobs(job, other));
            if let Some(other) = incompatible {
                blockers.push(other.id);
                if (job.fast_move || other.fast_move)
                    && (job.paths.is_none() || other.paths.is_none())
                {
                    "paths_unknown"
                } else if !job.fast_move
                    && !other.fast_move
                    && (job.disks.is_none() || other.disks.is_none())
                {
                    "resources_unknown"
                } else {
                    "shared_resources"
                }
            } else if let Some(other) = earlier
                .iter()
                .find(|other| !scheduling::compatible_jobs(job, other))
            {
                blockers.push(other.id);
                "queue_priority"
            } else {
                "scheduler"
            }
        };
        result.insert(
            id.to_string(),
            observation(job, state.rate_limit, limiter, reason, &blockers),
        );
        if !job.start_required && !state.active.contains_key(&id) {
            if job.state == TransferState::Scanning && scanning.is_none() {
                scanning = Some(id);
            }
            if job.state == TransferState::Waiting
                && !job.paused
                && !row_queue::blocked_by_partial_request(state, id)
            {
                earlier.push(job);
            }
        }
    }
    Value::Object(result)
}
