//! Révisions volatiles et diffusion des seuls lots modifiés, sans modifier le journal.
use super::*;
use std::sync::atomic::{AtomicU64, Ordering};

// Une horloge de processus évite toute collision entre une mutation de lot et une mutation de file.
static CLOCK: AtomicU64 = AtomicU64::new(1);
pub(super) fn touch() -> u64 {
    CLOCK.fetch_add(1, Ordering::SeqCst) + 1
}

/// Compteurs volatils : une progression ne rend pas un autre travailleur exécutable.
#[derive(Clone, Copy)]
pub(super) enum Counter {
    Copied,
    Verified,
    ResumeChecked,
}
impl Counter {
    fn value(self, entry: &mut Entry) -> &mut u64 {
        match self {
            Self::Copied => &mut entry.copied,
            Self::Verified => &mut entry.verified,
            Self::ResumeChecked => &mut entry.resume_checked,
        }
    }
}

/// Encapsule le lot pour marquer chaque accès mutable, y compris ceux des commandes ciblées.
#[derive(Clone, Serialize, Deserialize)]
#[serde(transparent)]
pub(super) struct TrackedJob {
    job: Job,
    #[serde(skip, default = "touch")]
    revision: u64,
    #[serde(skip, default = "touch")]
    full_revision: u64,
    #[serde(skip)]
    rate_revision: u64,
    #[serde(skip)]
    entry_revisions: std::collections::HashMap<usize, u64>,
}
impl From<Job> for TrackedJob {
    fn from(job: Job) -> Self {
        let revision = touch();
        Self {
            job,
            revision,
            full_revision: revision,
            rate_revision: 0,
            entry_revisions: Default::default(),
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
        self.full_revision = self.revision;
        self.entry_revisions.clear();
        &mut self.job
    }
}
impl TrackedJob {
    /// Une valeur identique ne modifie ni révision de présentation ni journal.
    pub(super) fn set_counter(&mut self, index: usize, counter: Counter, value: u64) {
        let Some(entry) = self.job.entries.get_mut(index) else {
            return;
        };
        let target = counter.value(entry);
        if *target != value {
            *target = value;
            self.revision = touch();
            self.entry_revisions.insert(index, self.revision);
        }
    }
    /// Un réglage de débit n’invalide ni les chemins ni les entrées.
    pub(super) fn set_rate(&mut self, value: u64) {
        if self.job.rate_limit != value {
            self.job.rate_limit = value;
            self.revision = touch();
            self.rate_revision = self.revision;
        }
    }
    /// Un client déjà synchronisé ne reçoit que les compteurs et options réellement modifiés.
    fn patch_since(&self, since: u64) -> Option<Value> {
        if since < self.full_revision {
            return None;
        }
        let mut patch = json!({"id": self.job.id});
        if since < self.rate_revision {
            patch["rate_limit"] = json!(self.job.rate_limit);
        }
        let mut indices = self
            .entry_revisions
            .iter()
            .filter_map(|(index, revision)| (*revision > since).then_some(*index))
            .collect::<Vec<_>>();
        indices.sort_unstable();
        if !indices.is_empty() {
            patch["entries"] = json!(indices.iter().map(|index| {
                let e = &self.job.entries[*index];
                json!({"index":index,"copied":e.copied,"verified":e.verified,"resume_checked":e.resume_checked})
            }).collect::<Vec<_>>());
        }
        Some(patch)
    }
    /// Libérer un cache interne ne change ni le journal ni la révision visible par Qt.
    pub(super) fn clear_directory_index(&mut self) {
        self.job.directory_index.clear();
    }
    /// Révision du lot pour ne persister que les lots modifiés.
    pub(super) fn revision(&self) -> u64 {
        self.revision
    }
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
        self.poll_changes(since, false)
    }
    /// Les clients qui demandent les deltas d’entrée gardent un repli complet après toute mutation générale.
    pub(super) fn poll_changes(
        &self,
        since: Option<u64>,
        entry_deltas: bool,
    ) -> Result<Value, String> {
        let mut state = self.shared.state.lock().unwrap();
        let view = row_queue::layout(&mut state);
        let revision = CLOCK.load(Ordering::SeqCst);
        if since == Some(revision) {
            return Ok(
                json!({"revision":revision,"unchanged":true,"queue_guard":completion::snapshot(&state),"diagnostics":diagnostics::snapshot(&mut state,&self.shared.limiter.lock().unwrap())}),
            );
        }
        let reset = since.is_none_or(|value| value > revision);
        let mut jobs = Vec::new();
        let mut patches = Vec::new();
        for job in state
            .jobs
            .iter()
            .filter(|job| reset || job.revision > since.unwrap_or(0))
        {
            if entry_deltas && !reset {
                if let Some(patch) = job.patch_since(since.unwrap_or(0)) {
                    patches.push(patch);
                    continue;
                }
            }
            jobs.push(serde_json::to_value(job).map_err(|e| e.to_string())?);
        }
        let mut result = json!({"revision":revision,"reset":reset,"jobs":jobs,
            "queue_order":queue::ids(&state),"duplicates_ignored":state.duplicates_ignored,"rate_limit":state.rate_limit,
            "job_ids":state.jobs.iter().map(|job|job.id).collect::<Vec<_>>(),
            "conflict_policy":state.conflict_policy,"allow_duplication":state.allow_duplication,"exclusions":state.exclusions,"verify_space":state.verify_space,"verify_properties":state.verify_properties,"verify_fast_move":state.verify_fast_move,"paused":state.paused,"verify_md5":state.verify_md5,"keep_history":state.keep_history,"resume_interrupted":state.resume_interrupted,
            "protocol_version":protocol::VERSION,"parallel_limit":2});
        if entry_deltas {
            result["job_patches"] = json!(patches);
        }
        if reset || since.is_none_or(|since| since < state.row_revision) {
            result["row_order"] = json!(view.ordered);
        }
        result["queue_guard"] = completion::snapshot(&state);
        result["diagnostics"] =
            diagnostics::snapshot(&mut state, &self.shared.limiter.lock().unwrap());
        Ok(result)
    }
}
