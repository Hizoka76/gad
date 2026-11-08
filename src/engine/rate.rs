//! Budget d’octets partagé : la limite globale couvre la somme des transferts.
//! Les crédits sont bornés à 100 ms ; une pause ne permet pas une rafale à la reprise.
use super::*;
use std::{
    collections::HashMap,
    time::{Duration, Instant},
};

/// Plafond du contrat JSON (1 Tio/s), également représentable exactement par Qt.
pub(super) const MAX: u64 = 1 << 40;
#[derive(Default)]
pub(super) struct Limiter {
    global: Bucket,
    jobs: HashMap<u64, Bucket>,
}
#[derive(Default)]
struct Bucket {
    rate: u64,
    credit: f64,
    updated: Option<Instant>,
    /// Dernier refus de budget local/global ; absent du journal et des mesures de débit.
    local_wait: Option<Instant>,
    global_wait: Option<Instant>,
}
impl Bucket {
    /// Une nouvelle valeur repart sans crédit ; aucun rattrapage après une baisse de limite.
    fn refill(&mut self, rate: u64, now: Instant) {
        if self.rate != rate {
            self.rate = rate;
            self.credit = 0.;
            self.updated = Some(now);
        }
        let elapsed = self
            .updated
            .replace(now)
            .map_or(0., |t| now.duration_since(t).as_secs_f64());
        self.credit = (self.credit + elapsed * rate as f64).min((rate as f64 / 10.).max(1.));
    }
    fn available(&self, requested: usize) -> usize {
        if self.rate == 0 {
            requested
        } else {
            requested.min(self.credit as usize)
        }
    }
    fn wait(&self, target: usize) -> Duration {
        if self.rate == 0 {
            return Duration::ZERO;
        }
        let quantum = target.min((self.rate / 50).max(1) as usize);
        Duration::from_secs_f64(
            ((quantum as f64 - self.credit).max(0.) / self.rate as f64).min(0.05),
        )
    }
}
impl Limiter {
    /// Une observation expire après une seconde ; une limite configurée ne prouve pas une attente.
    pub(super) fn waiting(&self, id: u64, now: Instant) -> (bool, bool) {
        let recent = |time: Option<Instant>| {
            time.is_some_and(|time| now.saturating_duration_since(time) < Duration::from_secs(1))
        };
        self.jobs.get(&id).map_or((false, false), |job| {
            (recent(job.global_wait), recent(job.local_wait))
        })
    }
    /// Les compteurs sont liés au travailleur actif, pas à l’historique conservé.
    pub(super) fn forget(&mut self, id: u64) {
        self.jobs.remove(&id);
    }
    /// Consomme les deux budgets atomiquement ; aucun octet n’est réservé à un travailleur endormi.
    fn grant(
        &mut self,
        id: u64,
        global: u64,
        local: u64,
        bytes: usize,
        now: Instant,
    ) -> Result<usize, Duration> {
        self.global.refill(global, now);
        let job = self.jobs.entry(id).or_default();
        job.refill(local, now);
        let global_wait = self.global.wait(bytes);
        let local_wait = job.wait(bytes);
        let wait = global_wait.max(local_wait);
        if !wait.is_zero() {
            if !global_wait.is_zero() {
                job.global_wait = Some(now);
            }
            if !local_wait.is_zero() {
                job.local_wait = Some(now);
            }
            return Err(wait.max(Duration::from_millis(1)));
        }
        let count = self.global.available(bytes).min(job.available(bytes));
        if count == 0 {
            return Err(Duration::from_millis(1));
        }
        if global != 0 {
            self.global.credit -= count as f64;
        }
        if local != 0 {
            job.credit -= count as f64;
        }
        Ok(count)
    }
}
/// Refuse les nombres négatifs, fractionnaires et les valeurs démesurées avant toute mutation.
pub(super) fn value(v: &Value) -> Result<u64, String> {
    v.as_u64()
        .filter(|n| *n <= MAX)
        .ok_or("Invalid speed: integer from 0 to 1 TiB/s, in bytes/s".into())
}
impl Engine {
    /// Attend sans bloquer les commandes ; pause, annulation et réglages réveillent cette attente.
    pub(super) fn transfer_budget(
        &self,
        id: u64,
        index: usize,
        bytes: usize,
    ) -> Result<usize, String> {
        loop {
            self.checkpoint_entry(id, index)?;
            if reconnect::probe_due() {
                self.check_connection(id)?;
            }
            let state = self.shared.state.lock().unwrap();
            let job = state
                .jobs
                .iter()
                .find(|j| j.id == id)
                .ok_or("Operation absent")?;
            if job.cancel
                || job.entries[index].cancel
                || self.shared.stop.load(std::sync::atomic::Ordering::SeqCst)
            {
                return Err("Canceled".into());
            }
            if state.paused || job.paused || job.entries[index].paused {
                continue;
            }
            let grant = self.shared.limiter.lock().unwrap().grant(
                id,
                state.rate_limit,
                job.rate_limit,
                bytes,
                Instant::now(),
            );
            match grant {
                Ok(count) => return Ok(count),
                Err(delay) => {
                    drop(self.shared.wake.wait_timeout(state, delay).unwrap());
                }
            }
        }
    }
    /// Applique le même plafond à toutes les demandes sélectionnées, après validation complète.
    pub(super) fn set_rate_limit(&self, v: &Value) -> Result<Value, String> {
        let limit = value(&v["bytes_per_second"])?;
        let ids = v["ids"]
            .as_array()
            .ok_or("Demandes manquantes")?
            .iter()
            .map(|id| id.as_u64().ok_or("Identifiant invalide"))
            .collect::<Result<std::collections::HashSet<_>, _>>()?;
        if ids.is_empty() {
            return Err("No request selected".into());
        }
        {
            let mut state = self.shared.state.lock().unwrap();
            if ids.iter().any(|id| !state.jobs.iter().any(|j| j.id == *id)) {
                return Err("Demande absente".into());
            }
            for job in &mut state.jobs {
                if ids.contains(&job.id) {
                    job.rate_limit = limit;
                }
            }
        }
        self.shared.wake.notify_all();
        self.save()?;
        Ok(json!({"ok":true}))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    /// Deux travailleurs partagent un seul crédit global ; leurs limites individuelles s’ajoutent comme contraintes.
    fn aggregate_budget_and_individual_caps() {
        let now = Instant::now();
        let mut limiter = Limiter::default();
        assert!(limiter.grant(1, 1000, 500, 100, now).is_err());
        assert_eq!(
            limiter.grant(1, 1000, 500, 100, now + Duration::from_millis(100)),
            Ok(50)
        );
        assert_eq!(
            limiter.grant(2, 1000, 0, 100, now + Duration::from_millis(100)),
            Ok(50)
        );
        assert!(limiter
            .grant(2, 1000, 0, 100, now + Duration::from_millis(100))
            .is_err());
    }
    #[test]
    /// Une longue inactivité reste bornée et retirer les limites débloque immédiatement.
    fn bounded_credit_live_changes_and_unlimited() {
        let now = Instant::now();
        let mut limiter = Limiter::default();
        assert_eq!(limiter.grant(1, 0, 0, 65536, now), Ok(65536));
        assert!(limiter.grant(1, 100, 0, 65536, now).is_err());
        assert_eq!(
            limiter.grant(1, 100, 0, 65536, now + Duration::from_secs(60)),
            Ok(10)
        );
        assert!(limiter
            .grant(1, 1, 0, 1, now + Duration::from_secs(60))
            .is_err());
        assert_eq!(
            limiter.grant(1, 0, 0, 65536, now + Duration::from_secs(60)),
            Ok(65536)
        );
    }
}

#[cfg(test)]
mod diagnostic_tests {
    use super::*;
    #[test]
    /// Seul un budget réellement refusé est signalé ; les observations expirent et sont libérées.
    fn rate_diagnostics_distinguish_global_and_local_waits() {
        let now = Instant::now();
        let mut limiter = Limiter::default();
        assert_eq!(limiter.grant(1, 0, 0, 100, now), Ok(100));
        assert_eq!(limiter.waiting(1, now), (false, false));
        assert!(limiter.grant(1, 100, 0, 100, now).is_err());
        assert_eq!(limiter.waiting(1, now), (true, false));
        assert_eq!(
            limiter.waiting(1, now + Duration::from_secs(1)),
            (false, false)
        );
        assert!(limiter.grant(2, 0, 100, 100, now).is_err());
        assert_eq!(limiter.waiting(2, now), (false, true));
        assert!(limiter.grant(3, 100, 100, 100, now).is_err());
        assert_eq!(limiter.waiting(3, now), (true, true));
        limiter.forget(3);
        assert_eq!(limiter.waiting(3, now), (false, false));
    }
}
