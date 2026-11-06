//! Contrôle comparatif des métadonnées visibles par le compte courant, sans les modifier.
//! Les valeurs d’ACL/xattr restent en mémoire ; le journal ne contient que les différences et limites.
use super::*;
use std::collections::{BTreeMap, BTreeSet};
use std::ffi::CString;
use std::os::unix::ffi::OsStrExt;

unsafe extern "C" {
    fn llistxattr(path: *const std::ffi::c_char, list: *mut u8, size: usize) -> isize;
    fn lgetxattr(
        path: *const std::ffi::c_char,
        name: *const std::ffi::c_char,
        value: *mut u8,
        size: usize,
    ) -> isize;
}

#[derive(Clone, Serialize, Deserialize)]
/// Rapport sans valeurs d’attributs, conservé dans l’historique avec l’entrée.
pub(super) struct Report {
    pub status: String,
    pub differences: Vec<String>,
    pub unavailable: Vec<String>,
}
impl Report {
    /// Un déplacement classique n’est validé que si aucun écart ni contrôle incomplet ne subsiste.
    pub fn ok(&self) -> bool {
        self.status == "ok"
    }
}

/// Version cohérente du propriétaire, groupe, mode et des attributs du chemin lui-même (jamais sa cible symbolique).
pub(super) struct Snapshot {
    basic: Result<(u32, u32, u32), String>,
    attrs: Result<BTreeMap<Vec<u8>, Vec<u8>>, String>,
}

/// Une croissance concurrente exige une nouvelle mesure, avec limites d’essais et de mémoire.
fn buffer(mut read: impl FnMut(*mut u8, usize) -> isize) -> Result<Vec<u8>, String> {
    for _ in 0..3 {
        let count = read(std::ptr::null_mut(), 0);
        if count < 0 {
            return Err(std::io::Error::last_os_error().to_string());
        }
        if count > 65536 {
            return Err("Metadata too large to verify".into());
        }
        if count == 0 {
            return Ok(Vec::new());
        }
        let mut value = vec![0; count as usize];
        let actual = read(value.as_mut_ptr(), value.len());
        if actual >= 0 {
            value.truncate(actual as usize);
            return Ok(value);
        }
        let error = std::io::Error::last_os_error();
        if error.raw_os_error() != Some(34) {
            return Err(error.to_string());
        }
    }
    Err("Attributes changed while being read".into())
}

/// Lit les attributs binaires sans suivre un lien et sans enregistrer leurs valeurs dans le journal.
fn attributes(path: &Path) -> Result<BTreeMap<Vec<u8>, Vec<u8>>, String> {
    let path = CString::new(path.as_os_str().as_bytes()).map_err(|e| e.to_string())?;
    // SAFETY : chemin terminé par NUL et tampon valide fourni par buffer ; la libc n’en conserve aucun pointeur.
    let names = buffer(|out, size| unsafe { llistxattr(path.as_ptr(), out, size) })?;
    let mut attributes = BTreeMap::new();
    let mut total = 0;
    for name in names
        .split(|byte| *byte == 0)
        .filter(|name| !name.is_empty())
    {
        let key = CString::new(name).map_err(|e| e.to_string())?;
        // SAFETY : mêmes garanties de durée de vie, plus nom d’attribut terminé par NUL.
        let value =
            buffer(|out, size| unsafe { lgetxattr(path.as_ptr(), key.as_ptr(), out, size) })?;
        total += value.capacity();
        if total > 8 * 1024 * 1024 {
            return Err("Attribute set too large to verify".into());
        }
        attributes.insert(name.to_vec(), value);
    }
    Ok(attributes)
}

impl Snapshot {
    /// Une modification d’identité ou de ctime pendant la lecture interdit un rapport rassurant.
    pub fn read(path: &Path) -> Self {
        let before = fs::symlink_metadata(path);
        let attrs = attributes(path);
        let after = fs::symlink_metadata(path);
        match (before, after) {
            (Ok(a), Ok(b))
                if (a.dev(), a.ino(), a.ctime(), a.ctime_nsec())
                    == (b.dev(), b.ino(), b.ctime(), b.ctime_nsec()) =>
            {
                Self {
                    basic: Ok((b.uid(), b.gid(), b.mode() & 0o7777)),
                    attrs,
                }
            }
            (Err(error), _) | (_, Err(error)) => Self {
                basic: Err(error.to_string()),
                attrs,
            },
            _ => Self {
                basic: Err("Properties changed while being read".into()),
                attrs: Err("Inconsistent ACL/attribute read".into()),
            },
        }
    }
    /// Borne les captures d’un renommage d’arbre ; un grand ensemble sera contrôlé entrée par entrée.
    pub fn memory_usage(&self) -> usize {
        std::mem::size_of::<Self>()
            + self.attrs.as_ref().map_or(0, |attrs| {
                attrs
                    .iter()
                    .map(|(name, value)| name.capacity() + value.capacity() + 64)
                    .sum::<usize>()
            })
    }
    /// Évite de commencer un déplacement dont les métadonnées ne pourront pas être contrôlées.
    pub fn complete(&self) -> bool {
        self.basic.is_ok() && self.attrs.is_ok()
    }
}

/// Compare les octets d’ACL et attributs, sans confondre une impossibilité de lecture avec une absence.
pub(super) fn compare(source: &Snapshot, target: &Snapshot) -> Report {
    let mut report = Report {
        status: String::new(),
        differences: vec![],
        unavailable: vec![],
    };
    match (&source.basic, &target.basic) {
        (Ok((uid, gid, mode)), Ok((other_uid, other_gid, other_mode))) => {
            if uid != other_uid {
                report
                    .differences
                    .push(format!("Propriétaire différent : UID {uid} → {other_uid}"));
            }
            if gid != other_gid {
                report
                    .differences
                    .push(format!("Groupe différent : GID {gid} → {other_gid}"));
            }
            if mode != other_mode {
                report.differences.push(format!(
                    "Permissions différentes : {mode:04o} → {other_mode:04o}"
                ));
            }
        }
        (Err(error), _) => report
            .unavailable
            .push(format!("Propriétés source : {error}")),
        (_, Err(error)) => report
            .unavailable
            .push(format!("Propriétés destination : {error}")),
    }
    match (&source.attrs, &target.attrs) {
        (Ok(a), Ok(b)) => {
            let names = a.keys().chain(b.keys()).collect::<BTreeSet<_>>();
            for name in names {
                if a.get(name) == b.get(name) {
                    continue;
                }
                let label = match name.as_slice() {
                    b"system.posix_acl_access" => "Access ACL".to_owned(),
                    b"system.posix_acl_default" => "Default ACL".to_owned(),
                    _ => format!("Attribut étendu « {} »", String::from_utf8_lossy(name)),
                };
                let change = if !b.contains_key(name) {
                    "absent de la destination"
                } else if !a.contains_key(name) {
                    "additional at destination"
                } else {
                    "different"
                };
                report.differences.push(format!("{label} : {change}"));
            }
        }
        _ => {
            if let Err(error) = &source.attrs {
                report
                    .unavailable
                    .push(format!("ACL/attributs source non vérifiables : {error}"));
            }
            if let Err(error) = &target.attrs {
                report.unavailable.push(format!(
                    "ACL/attributs destination non vérifiables : {error}"
                ));
            }
        }
    }
    report.status = if !report.differences.is_empty() {
        "different"
    } else if !report.unavailable.is_empty() {
        "unavailable"
    } else {
        "ok"
    }
    .into();
    report
}

impl Engine {
    /// Le verrou ne couvre que l’option du lot ; toutes les lectures de métadonnées ont lieu hors verrou.
    pub(super) fn capture_properties(&self, id: u64, path: &Path) -> Option<Snapshot> {
        let enabled = self
            .shared
            .state
            .lock()
            .unwrap()
            .jobs
            .iter()
            .any(|job| job.id == id && job.verify_properties);
        enabled.then(|| Snapshot::read(path))
    }
    /// Enregistre le rapport, séparé de l’erreur de transfert pour qu’une copie avec avertissement reste utilisable.
    pub(super) fn report_properties(
        &self,
        id: u64,
        index: usize,
        source: &Snapshot,
        target: &Path,
    ) -> bool {
        let report = compare(source, &Snapshot::read(target));
        let ok = report.ok();
        self.update(id, |job| job.entries[index].properties = Some(report));
        ok
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    /// Fabrique une vue cohérente sans chown ni privilèges particuliers.
    fn sample(uid: u32, gid: u32, mode: u32, attrs: &[(&[u8], &[u8])]) -> Snapshot {
        Snapshot {
            basic: Ok((uid, gid, mode)),
            attrs: Ok(attrs
                .iter()
                .map(|(key, value)| (key.to_vec(), value.to_vec()))
                .collect()),
        }
    }
    #[test]
    /// Chaque changement de propriétaire, groupe ou mode doit rester identifiable séparément.
    fn properties_compare_owner_group_and_permissions_independently() {
        let report = compare(&sample(1, 2, 0o640, &[]), &sample(3, 4, 0o600, &[]));
        assert_eq!(report.status, "different");
        assert_eq!(report.differences.len(), 3);
        assert!(report.differences[2].contains("0640 → 0600"));
    }
    #[test]
    /// Les attributs manquants, supplémentaires et modifiés sont distincts, sans fuite de leurs valeurs.
    fn properties_compare_binary_attributes_without_persisting_values() {
        let a = sample(
            1,
            2,
            0o600,
            &[(b"user.changed", b"secret\0\xff"), (b"user.lost", b"value")],
        );
        let b = sample(
            1,
            2,
            0o600,
            &[(b"user.changed", b"other"), (b"user.extra", b"value")],
        );
        let report = compare(&a, &b);
        assert_eq!(report.differences.len(), 3);
        let json = serde_json::to_string(&report).unwrap();
        assert!(!json.contains("secret"));
        assert!(json.contains("absent de la destination"));
        assert!(json.contains("additional"));
    }
    #[test]
    /// Les ACL d’accès et celles héritées par défaut ne doivent pas être confondues.
    fn properties_distinguish_access_and_default_acl() {
        let a = sample(
            1,
            2,
            0o600,
            &[
                (b"system.posix_acl_access", b"binary"),
                (b"system.posix_acl_default", b"binary"),
            ],
        );
        let report = compare(&a, &sample(1, 2, 0o600, &[]));
        assert_eq!(report.differences.len(), 2);
        assert!(report
            .differences
            .iter()
            .any(|text| text.starts_with("Access ACL")));
        assert!(report
            .differences
            .iter()
            .any(|text| text.starts_with("Default ACL")));
    }
    #[test]
    /// Un refus de lecture n’autorise pas à conclure que les attributs sont identiques.
    fn properties_unreadable_attributes_are_never_treated_as_absent() {
        let mut a = sample(1, 2, 0o600, &[]);
        a.attrs = Err("Access denied".into());
        let report = compare(&a, &sample(1, 2, 0o600, &[]));
        assert_eq!(report.status, "unavailable");
        assert!(!report.ok());
        assert!(report.differences.is_empty());
        assert_eq!(report.unavailable.len(), 1);
        assert!(!a.complete());
    }
    #[test]
    /// Une différence connue ne doit pas masquer une autre propriété impossible à vérifier.
    fn properties_report_keeps_differences_and_incomplete_checks_together() {
        let mut a = sample(1, 2, 0o600, &[]);
        a.attrs = Err("Non pris en charge".into());
        let report = compare(&a, &sample(1, 3, 0o600, &[]));
        assert_eq!(report.status, "different");
        assert_eq!(report.differences.len(), 1);
        assert_eq!(report.unavailable.len(), 1);
    }
    #[test]
    /// Une vue incohérente des propriétés de base ne doit jamais produire un succès.
    fn properties_missing_metadata_cannot_produce_ok() {
        let mut a = sample(1, 2, 0o600, &[]);
        a.basic = Err("Source changed".into());
        assert_eq!(compare(&a, &sample(1, 2, 0o600, &[])).status, "unavailable");
    }
    unsafe extern "C" {
        fn __errno_location() -> *mut i32;
    }
    #[test]
    /// Une croissance concurrente relance la lecture et préserve les octets non textuels.
    fn properties_buffer_retries_growth_and_keeps_binary_bytes() {
        let mut calls = 0;
        let value = buffer(|out, size| {
            calls += 1;
            if size == 0 {
                return if calls == 1 { 1 } else { 3 };
            }
            if calls == 2 {
                // SAFETY : errno est propre au thread courant ; le code ERANGE simule la croissance entre deux appels.
                unsafe {
                    *__errno_location() = 34;
                }
                return -1;
            }
            // SAFETY : buffer vient d’allouer les trois octets demandés par l’appel de mesure.
            unsafe {
                std::ptr::copy_nonoverlapping([0, 255, 1].as_ptr(), out, 3);
            }
            3
        })
        .unwrap();
        assert_eq!(value, [0, 255, 1]);
        assert_eq!(calls, 4);
    }
    #[test]
    /// Des métadonnées énormes ou constamment modifiées ne doivent ni saturer la mémoire ni boucler.
    fn properties_buffer_limits_memory_and_unstable_retries() {
        assert!(buffer(|_, _| 65537).is_err());
        let mut reads = 0;
        assert!(buffer(|_, size| {
            if size == 0 {
                return 1;
            }
            reads += 1;
            // SAFETY : errno est propre au thread courant.
            unsafe {
                *__errno_location() = 34;
            }
            -1
        })
        .is_err());
        assert_eq!(reads, 3);
    }
}
