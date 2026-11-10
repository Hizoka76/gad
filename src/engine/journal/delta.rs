//! Remplacement borné dans le JSON exact d’un lot ; les empreintes détectent une base manquante.
use super::*;
use serde_json::value::RawValue;

pub(super) struct Encoded {
    pub raw: Box<RawValue>,
    digest: std::sync::OnceLock<String>,
}
impl Encoded {
    pub fn new(raw: Box<RawValue>) -> Self {
        Self {
            raw,
            digest: std::sync::OnceLock::new(),
        }
    }
    /// Un lot complet remplacé avant le prochain delta n’a pas besoin d’une seconde empreinte.
    fn digest(&self) -> &str {
        self.digest
            .get_or_init(|| format!("{:x}", md5::compute(self.raw.get().as_bytes())))
    }
}
#[derive(Serialize, Deserialize, Clone)]
pub(super) struct Patch {
    pub id: u64,
    base: String,
    result: String,
    offset: usize,
    remove: usize,
    insert: String,
}
/// Lit uniquement l’identité ; les données métier sont désérialisées avant de retourner l’état.
pub(super) fn id(raw: &RawValue) -> Result<u64, String> {
    #[derive(Deserialize)]
    struct Identity {
        id: u64,
    }
    serde_json::from_str::<Identity>(raw.get())
        .map(|value| value.id)
        .map_err(failure)
}
/// Les blocs égaux passent par la comparaison de tranches optimisée par le compilateur.
/// Seul le premier bloc différent nécessite une recherche octet par octet.
fn common_prefix(a: &[u8], b: &[u8]) -> usize {
    let blocks = a
        .chunks_exact(64)
        .zip(b.chunks_exact(64))
        .take_while(|(a, b)| a == b)
        .count()
        * 64;
    blocks
        + a[blocks..]
            .iter()
            .zip(&b[blocks..])
            .take_while(|(a, b)| a == b)
            .count()
}
/// Parcourir depuis la fin évite de construire une copie inversée du JSON.
fn common_suffix(a: &[u8], b: &[u8]) -> usize {
    let blocks = a
        .rchunks_exact(64)
        .zip(b.rchunks_exact(64))
        .take_while(|(a, b)| a == b)
        .count()
        * 64;
    blocks
        + a[..a.len() - blocks]
            .iter()
            .rev()
            .zip(b[..b.len() - blocks].iter().rev())
            .take_while(|(a, b)| a == b)
            .count()
}
impl Patch {
    pub fn between(id: u64, old: &Encoded, new: &Encoded) -> Result<Option<Self>, String> {
        let (a, b) = (old.raw.get(), new.raw.get());
        let mut prefix = common_prefix(a.as_bytes(), b.as_bytes());
        while !a.is_char_boundary(prefix) || !b.is_char_boundary(prefix) {
            prefix -= 1;
        }
        let mut suffix = common_suffix(&a.as_bytes()[prefix..], &b.as_bytes()[prefix..]);
        while !a.is_char_boundary(a.len() - suffix) || !b.is_char_boundary(b.len() - suffix) {
            suffix -= 1;
        }
        let insert = &b[prefix..b.len() - suffix];
        if insert.len() >= b.len() / 2 {
            return Ok(None);
        }
        let patch = Self {
            id,
            base: old.digest().to_owned(),
            result: new.digest().to_owned(),
            offset: prefix,
            remove: a.len() - prefix - suffix,
            insert: insert.into(),
        };
        // Les petits lots ou les changements dispersés gardent le format complet.
        if serde_json::to_vec(&patch).map_err(failure)?.len() >= b.len() / 2 {
            return Ok(None);
        }
        Ok(Some(patch))
    }
    pub fn apply(&self, old: &Encoded) -> Result<Encoded, String> {
        if old.digest() != self.base {
            return Err(failure("base du delta incorrecte"));
        }
        let text = old.raw.get();
        let end = self
            .offset
            .checked_add(self.remove)
            .ok_or_else(|| failure("bornes du delta invalides"))?;
        if end > text.len() || !text.is_char_boundary(self.offset) || !text.is_char_boundary(end) {
            return Err(failure("bornes du delta invalides"));
        }
        let mut result = String::new();
        result.push_str(&text[..self.offset]);
        result.push_str(&self.insert);
        result.push_str(&text[end..]);
        let encoded = Encoded::new(RawValue::from_string(result).map_err(failure)?);
        if encoded.digest() != self.result || id(&encoded.raw)? != self.id {
            return Err(failure("résultat du delta incorrect"));
        }
        Ok(encoded)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn value(id: u64, name: &str) -> Encoded {
        Encoded::new(
            serde_json::value::to_raw_value(
                &json!({"id":id,"name":name,"padding":"z".repeat(4096)}),
            )
            .unwrap(),
        )
    }
    #[test]
    #[ignore = "benchmark explicite du rejeu des deltas"]
    fn benchmark_delta_replay() {
        let values = (0..=30)
            .map(|index| value(7, &format!("{}-{index:06}", "x".repeat(2 * 1024 * 1024))))
            .collect::<Vec<_>>();
        let patches = values
            .windows(2)
            .map(|pair| Patch::between(7, &pair[0], &pair[1]).unwrap().unwrap())
            .collect::<Vec<_>>();
        for trial in 0..3 {
            let mut current = Encoded::new(values[0].raw.clone());
            let start = std::time::Instant::now();
            for patch in &patches {
                current = patch.apply(&current).unwrap();
            }
            println!(
                "delta_replay trial={trial} elapsed_ms={:.3}",
                start.elapsed().as_secs_f64() * 1000.0
            );
            assert_eq!(current.raw.get(), values.last().unwrap().raw.get());
        }
    }
    #[test]
    #[ignore = "benchmark explicite du calcul de delta"]
    fn benchmark_delta_comparison() {
        let old = value(7, &"x".repeat(4 * 1024 * 1024));
        let new = value(7, &format!("{}y", "x".repeat(4 * 1024 * 1024 - 1)));
        // Les empreintes sont déjà mémorisées dans le chemin des deltas successifs.
        old.digest();
        new.digest();
        for trial in 0..3 {
            let start = std::time::Instant::now();
            for _ in 0..100 {
                std::hint::black_box(Patch::between(7, std::hint::black_box(&old), &new).unwrap());
            }
            println!(
                "delta_comparison trial={trial} elapsed_ms={:.3}",
                start.elapsed().as_secs_f64() * 1000.0
            );
        }
    }
    #[test]
    fn block_comparison_matches_scalar_at_all_edges_and_lengths() {
        // Inclut les frontières de blocs, les tranches vides et les longueurs différentes.
        for len in [0, 1, 63, 64, 65, 127, 128, 129, 255, 256, 257] {
            let a = vec![b'x'; len];
            for other_len in [0, 1, 63, 64, 65, 127, 128, 129, 255, 256, 257] {
                let original = vec![b'x'; other_len];
                for change in 0..=other_len {
                    let mut b = original.clone();
                    if change < other_len {
                        b[change] = b'y';
                    }
                    let prefix = a.iter().zip(&b).take_while(|(a, b)| a == b).count();
                    let suffix = a
                        .iter()
                        .rev()
                        .zip(b.iter().rev())
                        .take_while(|(a, b)| a == b)
                        .count();
                    assert_eq!(
                        common_prefix(&a, &b),
                        prefix,
                        "prefix {len}/{other_len}/{change}"
                    );
                    assert_eq!(
                        common_suffix(&a, &b),
                        suffix,
                        "suffix {len}/{other_len}/{change}"
                    );
                }
            }
        }
    }
    #[test]
    fn patches_roundtrip_changes_around_block_boundaries() {
        for index in 0..257 {
            let old = value(7, &"a".repeat(257));
            for replacement in ["", "b", "é", "😀", "quote\"\\\n"] {
                let name = format!(
                    "{}{}{}",
                    "a".repeat(index),
                    replacement,
                    "a".repeat(256 - index)
                );
                let new = value(7, &name);
                let patch = Patch::between(7, &old, &new).unwrap().unwrap();
                assert_eq!(patch.apply(&old).unwrap().raw.get(), new.raw.get());
            }
        }
    }
    #[test]
    fn unicode_and_escaped_fragments_roundtrip() {
        let names = ["é", "ê", "日本", "日々", "😀", "😁", "quote\"\\\n", ""];
        for from in names {
            for to in names {
                let old = value(7, from);
                let new = value(7, to);
                let patch = Patch::between(7, &old, &new).unwrap().unwrap();
                assert_eq!(patch.apply(&old).unwrap().raw.get(), new.raw.get());
            }
        }
    }
    #[test]
    fn wrong_base_result_identity_and_bounds_are_rejected() {
        let old = value(7, "é");
        let new = value(7, "ê");
        let patch = Patch::between(7, &old, &new).unwrap().unwrap();
        let mut bad = patch.clone();
        bad.base = "bad".into();
        assert!(bad.apply(&old).is_err());
        let mut bad = patch.clone();
        bad.result = "bad".into();
        assert!(bad.apply(&old).is_err());
        let mut bad = patch.clone();
        bad.offset = usize::MAX;
        assert!(bad.apply(&old).is_err());
        let mut bad = patch.clone();
        bad.remove = usize::MAX;
        assert!(bad.apply(&old).is_err());
        let mut bad = patch.clone();
        bad.offset += 1;
        assert!(bad.apply(&old).is_err());
        let other = value(8, "é");
        assert!(Patch::between(7, &old, &other)
            .unwrap()
            .unwrap()
            .apply(&old)
            .is_err());
    }
    #[test]
    fn malformed_json_is_rejected_even_with_matching_digest() {
        let old = value(7, "original");
        let mut patch = Patch::between(7, &old, &value(7, "modified"))
            .unwrap()
            .unwrap();
        patch.offset = 0;
        patch.remove = old.raw.get().len();
        patch.insert = "{invalid json".into();
        patch.result = format!("{:x}", md5::compute(&patch.insert));
        assert!(patch.apply(&old).is_err());
    }
    #[test]
    fn broad_changes_fall_back_to_full_jobs() {
        let old = value(1, "short");
        let new = value(1, &"a".repeat(12000));
        assert!(Patch::between(1, &old, &new).unwrap().is_none());
    }
}
