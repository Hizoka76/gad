//! Règles capturées par lot : motifs de noms, sans chemin ni suivi des liens.
use super::*;

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub(super) struct Rules {
    pub include_hidden: bool,
    pub directories: Vec<String>,
    pub files: Vec<String>,
}
impl Default for Rules {
    fn default() -> Self {
        Self {
            include_hidden: true,
            directories: Vec::new(),
            files: Vec::new(),
        }
    }
}
impl Rules {
    /// Valide avant réservation d’un lot ou mutation des préférences globales.
    pub fn parse(value: &Value) -> Result<Self, String> {
        let rules: Self = serde_json::from_value(value.clone())
            .map_err(|error| format!("Invalid exclusion rules: {error}"))?;
        for patterns in [&rules.directories, &rules.files] {
            if patterns.len() > 128 {
                return Err("At most 128 exclusion patterns are allowed per type.".into());
            }
            for pattern in patterns {
                if pattern.is_empty()
                    || pattern.len() > 256
                    || pattern.contains(['/', '\0', '\n', '\r'])
                {
                    return Err("Exclusion patterns must be nonempty names of at most 256 bytes, without slashes or line breaks.".into());
                }
            }
        }
        Ok(rules)
    }
    pub fn active(&self) -> bool {
        !self.include_hidden || !self.directories.is_empty() || !self.files.is_empty()
    }
    pub fn excludes(&self, name: &str, directory: bool) -> bool {
        (!self.include_hidden && name.starts_with('.'))
            || (if directory {
                &self.directories
            } else {
                &self.files
            })
            .iter()
            .any(|pattern| matches(pattern, name))
    }
}
/// Glob Unicode borné par la validation : * = texte quelconque, ? = un caractère.
/// La programmation dynamique évite tout retour arrière exponentiel sur les motifs malveillants.
pub(super) fn matches(pattern: &str, name: &str) -> bool {
    // Les motifs usuels n'ont besoin ni de matrice ni de convertir le nom en caractères.
    if !pattern.contains(['*', '?']) {
        return pattern == name;
    }
    if !pattern.contains('*') {
        let mut characters = name.chars();
        return pattern.chars().all(|token| {
            characters
                .next()
                .is_some_and(|c| token == '?' || token == c)
        }) && characters.next().is_none();
    }
    if !pattern.contains('?') {
        if let Some((prefix, suffix)) = pattern.split_once('*') {
            if !suffix.contains('*') {
                // Ne pas autoriser le chevauchement du préfixe et du suffixe.
                return name.len() >= prefix.len() + suffix.len()
                    && name.starts_with(prefix)
                    && name.ends_with(suffix);
            }
        }
    }
    // Cas général borné : une seule ligne de programmation dynamique réutilisée en place.
    let name: Vec<char> = name.chars().collect();
    let mut previous = vec![false; name.len() + 1];
    previous[0] = true;
    for token in pattern.chars() {
        let mut diagonal = previous[0];
        previous[0] &= token == '*';
        for i in 1..=name.len() {
            let old = previous[i];
            previous[i] = if token == '*' {
                old || previous[i - 1]
            } else {
                (token == '?' || token == name[i - 1]) && diagonal
            };
            diagonal = old;
        }
    }
    previous[name.len()]
}

#[cfg(test)]
mod tests {
    use super::*;
    fn reference_matches(pattern: &str, name: &str) -> bool {
        let name: Vec<char> = name.chars().collect();
        let mut previous = vec![false; name.len() + 1];
        previous[0] = true;
        for token in pattern.chars() {
            let mut next = vec![false; name.len() + 1];
            next[0] = token == '*' && previous[0];
            for i in 1..=name.len() {
                next[i] = if token == '*' {
                    previous[i] || next[i - 1]
                } else {
                    (token == '?' || token == name[i - 1]) && previous[i - 1]
                };
            }
            previous = next;
        }
        previous[name.len()]
    }
    #[test]
    fn optimized_globs_match_reference_exhaustively() {
        fn words(alphabet: &[&str], depth: usize) -> Vec<String> {
            let mut all = vec![String::new()];
            let mut level = all.clone();
            for _ in 0..depth {
                level = level
                    .iter()
                    .flat_map(|prefix| {
                        alphabet
                            .iter()
                            .map(move |letter| format!("{prefix}{letter}"))
                    })
                    .collect();
                all.extend(level.iter().cloned());
            }
            all
        }
        for pattern in words(&["a", "é", "*", "?"], 4) {
            for name in words(&["a", "é", "b"], 4) {
                assert_eq!(
                    matches(&pattern, &name),
                    reference_matches(&pattern, &name),
                    "{pattern:?}/{name:?}"
                );
            }
        }
        for pattern in ["ab*ab", "é*é", "?", "*?*", "😀?*.txt", "[a]*", "a**?z"] {
            for name in ["", "ab", "é", "😀", "😀é.txt", "[a]", "abz", "e\u{301}"] {
                assert_eq!(
                    matches(pattern, name),
                    reference_matches(pattern, name),
                    "{pattern:?}/{name:?}"
                );
            }
        }
    }
    #[test]
    #[ignore = "benchmark explicite des motifs d'exclusion"]
    fn benchmark_exclusion_matching() {
        for (pattern, name) in [
            ("__*", "__pycache__"),
            ("*.tmp", "rapport-de-transfert.tmp"),
            ("file-???.txt", "file-éab.txt"),
            ("a*?b*c", "aébbbccc"),
        ] {
            assert!(matches(pattern, name));
            for trial in 0..3 {
                let start = std::time::Instant::now();
                let mut found = 0;
                for _ in 0..100_000 {
                    found += usize::from(matches(
                        std::hint::black_box(pattern),
                        std::hint::black_box(name),
                    ));
                }
                assert_eq!(found, 100_000);
                println!(
                    "exclusion_matching pattern={pattern} trial={trial} elapsed_ms={:.3}",
                    start.elapsed().as_secs_f64() * 1000.0
                );
            }
        }
    }
    #[test]
    fn wildcard_names_and_unicode() {
        for (pattern, name) in [
            ("__*", "__pycache__"),
            ("*.tmp", "test.tmp"),
            ("*~", "backup~"),
            ("?.tmp", "é.tmp"),
            ("a**?z", "abz"),
            ("*", ""),
        ] {
            assert!(matches(pattern, name), "{pattern} / {name}");
        }
        for (pattern, name) in [
            ("*.tmp", "test.tmp.old"),
            ("?.tmp", "été.tmp"),
            ("cache", "Cache"),
            ("[ab]", "a"),
            ("__*", "cache"),
        ] {
            assert!(!matches(pattern, name), "{pattern} / {name}");
        }
    }
    #[test]
    fn defaults_hidden_and_types() {
        let defaults = Rules::default();
        assert!(!defaults.active());
        assert!(!defaults.excludes(".secret", false));
        let rules =
            Rules::parse(&json!({"include_hidden":false,"directories":["__*"],"files":["*.tmp"]}))
                .unwrap();
        assert!(rules.excludes(".hidden", true));
        assert!(rules.excludes(".hidden", false));
        assert!(rules.excludes("__pycache__", true));
        assert!(!rules.excludes("__file", false));
        assert!(rules.excludes("a.tmp", false));
        assert!(!rules.excludes("a.tmp", true));
    }
    #[test]
    fn invalid_rules_are_rejected() {
        for value in [
            json!(null),
            json!({"include_hidden":"false"}),
            json!({"unknown":true}),
            json!({"files":[""]}),
            json!({"files":["a/b"]}),
            json!({"directories":["a\nb"]}),
            json!({"files":["a".repeat(257)]}),
            json!({"files":vec!["*";129]}),
        ] {
            assert!(Rules::parse(&value).is_err(), "{value}");
        }
    }
}
