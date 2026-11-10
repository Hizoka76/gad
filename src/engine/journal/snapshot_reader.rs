//! Lire l'état et la séquence en un seul parcours, sans mettre les lots dans un arbre JSON.
use super::*;
use serde::de::{self, value::MapAccessDeserializer, DeserializeSeed, MapAccess, Visitor};

pub(super) struct Loaded {
    pub state: State,
    pub sequence: u64,
}
struct Fields<A> {
    inner: A,
    sequence: Option<u64>,
    jobs_seen: bool,
}
impl<'de, A: MapAccess<'de>> MapAccess<'de> for Fields<A> {
    type Error = A::Error;
    fn next_key_seed<K: DeserializeSeed<'de>>(
        &mut self,
        seed: K,
    ) -> Result<Option<K::Value>, A::Error> {
        loop {
            let Some(key) = self.inner.next_key::<String>()? else {
                return Ok(None);
            };
            if key == "_gad_journal_seq" {
                if self.sequence.is_some() {
                    return Err(de::Error::duplicate_field("_gad_journal_seq"));
                }
                self.sequence = Some(self.inner.next_value()?);
                continue;
            }
            self.jobs_seen |= key == "jobs";
            return seed
                .deserialize(de::value::StringDeserializer::new(key))
                .map(Some);
        }
    }
    fn next_value_seed<V: DeserializeSeed<'de>>(&mut self, seed: V) -> Result<V::Value, A::Error> {
        self.inner.next_value_seed(seed)
    }
}
impl<'de> Deserialize<'de> for Loaded {
    fn deserialize<D: serde::Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        struct SnapshotVisitor;
        impl<'de> Visitor<'de> for SnapshotVisitor {
            type Value = Loaded;
            fn expecting(&self, formatter: &mut std::fmt::Formatter) -> std::fmt::Result {
                formatter.write_str("a Gad snapshot object")
            }
            fn visit_map<A: MapAccess<'de>>(self, inner: A) -> Result<Loaded, A::Error> {
                let mut fields = Fields {
                    inner,
                    sequence: None,
                    jobs_seen: false,
                };
                let state = State::deserialize(MapAccessDeserializer::new(&mut fields))?;
                // Les en-têtes WAL peuvent omettre jobs, les instantanés ne le pouvaient pas.
                if !fields.jobs_seen {
                    return Err(de::Error::missing_field("jobs"));
                }
                Ok(Loaded {
                    state,
                    sequence: fields.sequence.unwrap_or(0),
                })
            }
        }
        deserializer.deserialize_map(SnapshotVisitor)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[derive(Deserialize)]
    struct PreviousSequence {
        #[serde(default, rename = "_gad_journal_seq")]
        sequence: u64,
        #[serde(rename = "jobs")]
        _jobs: de::IgnoredAny,
    }
    #[test]
    fn single_pass_preserves_previous_validation_and_state() {
        let samples = [
            r#"{"jobs":[]}"#,
            r#"{"jobs":[],"_gad_journal_seq":42,"paused":true}"#,
            r#"{"_gad_journal_seq":42,"jobs":[],"unknown":{"jobs":[],"_gad_journal_seq":999}}"#,
            r#"{"jo\u0062s":[],"_gad_journal_s\u0065q":42}"#,
            r#"{"jobs":[],"_gad_journal_seq":18446744073709551615}"#,
            r#"{}"#,
            r#"null"#,
            r#"[]"#,
            r#"[0,[]]"#,
            r#"{"jobs":null}"#,
            r#"{"jobs":[],"jobs":[]}"#,
            r#"{"jobs":[],"_gad_journal_seq":1,"_gad_journal_seq":2}"#,
            r#"{"jobs":[],"_gad_journal_seq":null}"#,
            r#"{"jobs":[],"_gad_journal_seq":-1}"#,
            r#"{"jobs":[],"_gad_journal_seq":1.5}"#,
            r#"{"jobs":[],"_gad_journal_seq":"42"}"#,
            r#"{"jobs":[],"_gad_journal_seq":18446744073709551616}"#,
            r#"{"jobs":[],"_gad_journal_seq":1} trailing"#,
        ];
        for text in samples {
            let expected = serde_json::from_str::<PreviousSequence>(text).and_then(|meta| {
                serde_json::from_str::<State>(text)
                    .map(|state| (serde_json::to_value(state).unwrap(), meta.sequence))
            });
            let actual = serde_json::from_str::<Loaded>(text)
                .map(|loaded| (serde_json::to_value(loaded.state).unwrap(), loaded.sequence));
            assert_eq!(actual.is_ok(), expected.is_ok(), "{text}");
            if let (Ok(actual), Ok(expected)) = (actual, expected) {
                assert_eq!(actual, expected, "{text}");
            }
        }
    }
}
