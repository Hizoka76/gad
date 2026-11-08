//! Révisions volatiles et diffusion des seuls lots modifiés, sans modifier le journal.
use super::*;
use std::sync::atomic::{AtomicU64, Ordering};

// Une horloge de processus évite toute collision entre une mutation de lot et une mutation de file.
static CLOCK: AtomicU64 = AtomicU64::new(1);
pub(super) fn touch() -> u64 {
    CLOCK.fetch_add(1, Ordering::SeqCst) + 1
}

/// Encapsule le lot pour marquer chaque accès mutable, y compris ceux des commandes ciblées.
#[derive(Clone, Serialize, Deserialize)]
#[serde(transparent)]
pub(super) struct TrackedJob {
    job: Job,
    #[serde(skip, default = "touch")]
    revision: u64,
}
impl From<Job> for TrackedJob {
    fn from(job: Job) -> Self {
        Self {
            job,
            revision: touch(),
        }
    }
}
impl std::ops::Deref for TrackedJob {
    type Target = Job;
    fn deref(&self) -> &Job {
        &self.job
    }
}
impl std::ops::DerefMut for TrackedJob {
    fn deref_mut(&mut self) -> &mut Job {
        self.revision = touch();
        &mut self.job
    }
}
impl TrackedJob {
    /// Copie métier destinée à un travailleur, sans exporter la révision de diffusion.
    pub(super) fn snapshot(&self) -> Job {
        self.job.clone()
    }
}

/// Marque aussi les changements globaux et suppressions, y compris lors d’un retour d’erreur.
pub(super) struct Mutation(pub bool);
impl Drop for Mutation {
    fn drop(&mut self) {
        if self.0 {
            touch();
        }
    }
}
impl Engine {
    /// Réponse incrémentale autonome : métadonnées de file, ordre des lots et seuls lots modifiés.
    pub(super) fn poll(&self, since: Option<u64>) -> Result<Value, String> {
        let mut state = self.shared.state.lock().unwrap();
        let view = row_queue::layout(&mut state);
        let revision = CLOCK.load(Ordering::SeqCst);
        if since == Some(revision) {
            return Ok(
                json!({"revision":revision,"unchanged":true,"queue_guard":completion::snapshot(&state),"diagnostics":diagnostics::snapshot(&mut state,&self.shared.limiter.lock().unwrap())}),
            );
        }
        let reset = since.is_none_or(|value| value > revision);
        let jobs = state
            .jobs
            .iter()
            .filter(|job| reset || job.revision > since.unwrap_or(0))
            .map(serde_json::to_value)
            .collect::<Result<Vec<_>, _>>()
            .map_err(|e| e.to_string())?;
        let mut result = json!({"revision":revision,"reset":reset,"jobs":jobs,
            "queue_order":queue::ids(&state),"duplicates_ignored":state.duplicates_ignored,"rate_limit":state.rate_limit,
            "job_ids":state.jobs.iter().map(|job|job.id).collect::<Vec<_>>(),
            "conflict_policy":state.conflict_policy,"allow_duplication":state.allow_duplication,"exclusions":state.exclusions,"verify_space":state.verify_space,"verify_properties":state.verify_properties,"verify_fast_move":state.verify_fast_move,"paused":state.paused,"verify_md5":state.verify_md5,"keep_history":state.keep_history,"resume_interrupted":state.resume_interrupted,
            "protocol_version":protocol::VERSION,"parallel_limit":2});
        if reset || since.is_none_or(|since| since < state.row_revision) {
            result["row_order"] = json!(view.ordered);
        }
        result["queue_guard"] = completion::snapshot(&state);
        result["diagnostics"] =
            diagnostics::snapshot(&mut state, &self.shared.limiter.lock().unwrap());
        Ok(result)
    }
}
