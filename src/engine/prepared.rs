//! Validation explicite des ajouts manuels, indépendante de toute pause ou reprise.
use super::*;
impl Engine {
    /// Ne libère que les lots analysés et choisis ; jamais un ajout manuel arrivé ensuite.
    pub(super) fn start_prepared(&self, value: &Value) -> Result<Value, String> {
        let ids = value["ids"]
            .as_array()
            .filter(|ids| !ids.is_empty())
            .ok_or("No prepared transfers selected")?;
        let ids = ids
            .iter()
            .map(|v| v.as_u64().ok_or("Invalid prepared transfer identifier"))
            .collect::<Result<std::collections::BTreeSet<_>, _>>()?;
        {
            let mut state = self.shared.state.lock().unwrap();
            for id in &ids {
                let job = state
                    .jobs
                    .iter()
                    .find(|j| j.id == *id)
                    .ok_or("Prepared transfer no longer exists")?;
                if !job.start_required
                    || job.cancel
                    || job.undo_requested
                    || job.state != TransferState::Waiting
                {
                    return Err("Wait for file preparation to finish before starting".into());
                }
            }
            for job in &mut state.jobs {
                if ids.contains(&job.id) {
                    job.start_required = false;
                }
            }
        }
        if let Err(error) = self.save() {
            let mut state = self.shared.state.lock().unwrap();
            for job in &mut state.jobs {
                if ids.contains(&job.id) && job.state == TransferState::Waiting {
                    job.start_required = true;
                }
            }
            return Err(error);
        }
        self.shared.wake.notify_all();
        Ok(json!({"ok":true,"started":ids.len()}))
    }
}
