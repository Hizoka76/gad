//! Barrière volatile de fin de file : aucune nouvelle opération pendant l’envoi d’une action système.
use super::*;

pub(super) fn idle(state: &State) -> bool {
    state.active.is_empty()
        && state.preparing == 0
        && state.finalizing == 0
        && state
            .jobs
            .iter()
            .all(|job| terminal(&job.state) && !job.undo_requested)
}
pub(super) fn snapshot(state: &State) -> Value {
    json!({"idle":idle(state),"epoch":state.work_epoch,"next_id":state.next_id,"claimed":state.completion_claimed,"last_command":state.last_work_command})
}

/// Une analyse ou une sauvegarde finale ne doit pas être confondue avec une file réellement au repos.
pub(super) struct Activity {
    engine: Engine,
    preparing: bool,
}
impl Activity {
    pub(super) fn new(engine: Engine, preparing: bool) -> Self {
        {
            let mut state = engine.shared.state.lock().unwrap();
            if preparing {
                state.preparing += 1;
            } else {
                state.finalizing += 1;
            }
        }
        Self { engine, preparing }
    }
}
impl Drop for Activity {
    fn drop(&mut self) {
        let mut state = self.engine.shared.state.lock().unwrap();
        if self.preparing {
            state.preparing = state.preparing.saturating_sub(1);
        } else {
            state.finalizing = state.finalizing.saturating_sub(1);
        }
        snapshot::touch();
        self.engine.shared.wake.notify_all();
    }
}
impl Engine {
    /// Vérifier le même état que le compte à rebours, puis interdire atomiquement les nouvelles mutations.
    pub(super) fn claim_completion(&self, v: &Value) -> Result<Value, String> {
        let mut state = self.shared.state.lock().unwrap();
        let epoch = v["epoch"].as_u64().ok_or("Invalid completion guard")?;
        let next = v["next_id"].as_u64().ok_or("Invalid completion guard")?;
        let ids = v["ids"]
            .as_array()
            .filter(|ids| !ids.is_empty())
            .ok_or("No completed request selected")?;
        if state.completion_claimed
            || state.paused
            || !idle(&state)
            || state.work_epoch != epoch
            || state.next_id != next
        {
            return Err("The queue changed before the completion action".into());
        }
        if ids.iter().any(|id| {
            id.as_u64().is_none_or(|id| {
                state
                    .jobs
                    .iter()
                    .find(|job| job.id == id)
                    .is_none_or(|job| {
                        job.state != TransferState::Done
                            || job.cancel
                            || job.entries.iter().any(|entry| {
                                entry.cancel
                                    || !matches!(
                                        entry.state,
                                        TransferState::Done | TransferState::Skipped
                                    )
                            })
                    })
            })
        }) {
            return Err("The queue did not finish successfully".into());
        }
        state.completion_claimed = true;
        Ok(json!({"ok":true}))
    }
}
