//! En-tête durable typé : comparer les réglages sans recréer leur arbre JSON.
use super::*;

pub(super) struct Cached {
    fields: Fields,
    pub raw: Box<RawValue>,
}
// Une seule liste lie capture, comparaison et sérialisation. Un test la confronte à State.
macro_rules! settings {
    ($($field:ident: $kind:ty),* $(,)?) => {
        #[derive(Serialize)]
        struct Fields { $($field: $kind),* }
        impl Fields {
            fn capture(state: &State) -> Self {
                Self { $($field: state.$field.to_owned()),* }
            }
            fn matches(&self, state: &State) -> bool {
                true $(&& self.$field == state.$field)*
            }
        }
    };
}
settings! {
    conflict_policy: String,
    exclusions: exclusions::Rules,
    allow_duplication: bool,
    paused: bool,
    verify_md5: bool,
    verify_properties: bool,
    verify_fast_move: bool,
    verify_space: bool,
    resume_interrupted: bool,
    next_id: u64,
    duplicates_ignored: u64,
    rate_limit: u64,
    queue_order: Vec<u64>,
    row_order: Vec<row_queue::Key>,
    next_execution_order: u64,
    keep_history: bool,
}
impl Cached {
    pub fn new(state: &State) -> Result<Self, String> {
        let fields = Fields::capture(state);
        let raw = to_raw_value(&fields).map_err(failure)?;
        Ok(Self { fields, raw })
    }
    /// Vérifier les valeurs elles-mêmes couvre les mutations directes, sans révision supplémentaire.
    pub fn matches(&self, state: &State) -> bool {
        self.fields.matches(state)
    }
}
