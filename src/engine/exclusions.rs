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

#[cfg(test)]
mod tests {
    use super::*;
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
