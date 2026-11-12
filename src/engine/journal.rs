//! Instantané atomique et journal incrémental durable des seuls lots modifiés.
//! Chaque intention est fsync avant l’opération de fichier correspondante.
//! Un numéro de séquence rend la compaction récupérable entre publication et troncature.
use super::*;
use serde_json::value::{to_raw_value, RawValue};
use std::collections::{HashMap, HashSet};
use std::io::{BufRead, BufReader};
use std::os::unix::fs::DirBuilderExt;
mod delta;
mod settings;
mod snapshot_reader;

// Ctime détecte aussi une modification dont mtime a été rétabli après réécriture.
type Fingerprint = (u64, u64, u64, i64, i64, i64, i64);
const MAX_RECORDS: usize = 64;
// La mémoire supplémentaire reste bornée même avec un historique volumineux.
const MAX_CACHE_BYTES: usize = 16 * 1024 * 1024;
const MAX_JOB_CACHE_BYTES: usize = 8 * 1024 * 1024;
const MAX_HEADER_CACHE_BYTES: usize = 1024 * 1024;
const MAX_BYTES: u64 = 64 * 1024 * 1024;

#[derive(Default)]
pub(super) struct Writer {
    pub sequence: u64,
    revisions: HashMap<u64, u64>,
    header: Option<settings::Cached>,
    ids: Vec<u64>,
    snapshot: Option<Fingerprint>,
    log_identity: Option<(u64, u64)>,
    length: u64,
    records: usize,
    poisoned: bool,
    encoded: HashMap<u64, delta::Encoded>,
    // Éviter de sérialiser deux fois les gros lots à chaque sauvegarde.
    oversized: HashSet<u64>,
}
#[cfg(test)]
#[derive(Serialize, Deserialize)]
struct Record {
    sequence: u64,
    header: Value,
    jobs: Vec<Value>,
    ids: Vec<u64>,
}
/// Rejouer directement les types métier évite les objets JSON intermédiaires de chaque entrée.
#[derive(Deserialize)]
struct ReadRecord<'a> {
    sequence: u64,
    #[serde(borrow)]
    header: &'a RawValue,
    ids: Vec<u64>,
    #[serde(borrow)]
    jobs: Option<Vec<&'a RawValue>>,
    #[serde(default)]
    format: Option<u32>,
    #[serde(default, borrow)]
    full_jobs: Vec<&'a RawValue>,
    #[serde(default)]
    patches: Vec<delta::Patch>,
}
enum LoadedJob {
    Snapshot(Box<snapshot::TrackedJob>),
    Encoded(delta::Encoded),
}
/// Sérialiser directement les lots évite un arbre JSON intermédiaire par entrée.
#[derive(Serialize)]
#[serde(untagged)]
enum FullJob<'a> {
    Raw(&'a RawValue),
    Job(&'a snapshot::TrackedJob),
}
#[derive(Serialize)]
struct WriteRecord<'a> {
    sequence: u64,
    header: &'a RawValue,
    ids: &'a [u64],
    // Les lecteurs anciens attendent un tableau : null leur impose un refus sûr.
    jobs: Option<()>,
    format: u32,
    full_jobs: Vec<FullJob<'a>>,
    patches: Vec<delta::Patch>,
}
#[derive(Serialize)]
struct Checkpoint<'a> {
    #[serde(flatten)]
    state: &'a State,
    #[serde(rename = "_gad_journal_seq")]
    sequence: u64,
}
/// Liste explicite des réglages persistants ; un test la compare au sérialiseur State.
#[cfg(test)]
fn header(state: &State) -> Value {
    macro_rules! fields { ($($field:ident),* $(,)?) => { json!({$(stringify!($field):state.$field),*}) }; }
    fields!(
        conflict_policy,
        exclusions,
        allow_duplication,
        paused,
        verify_md5,
        verify_properties,
        verify_fast_move,
        verify_space,
        resume_interrupted,
        next_id,
        duplicates_ignored,
        rate_limit,
        queue_order,
        row_order,
        next_execution_order,
        keep_history
    )
}
pub(super) fn log_path(path: &Path) -> PathBuf {
    path.with_extension("json.wal")
}
fn fingerprint(meta: &fs::Metadata) -> Fingerprint {
    (
        meta.dev(),
        meta.ino(),
        meta.len(),
        meta.mtime(),
        meta.mtime_nsec(),
        meta.ctime(),
        meta.ctime_nsec(),
    )
}
fn failure(error: impl std::fmt::Display) -> String {
    format!("Journal Gad : {error}")
}

/// Les fichiers de journal suivent la même règle de lecture et d’écriture :
/// aucun lien final ni fichier spécial. NONBLOCK évite d’attendre sur un FIFO altéré.
fn regular_file(path: &Path) -> Result<Option<fs::File>, String> {
    let file = match OpenOptions::new()
        .read(true)
        .custom_flags(0x20000 | 0x800)
        .open(path)
    {
        Ok(file) => file,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(error) => return Err(failure(error)),
    };
    if !file.metadata().map_err(failure)?.is_file() {
        return Err(failure("le journal n’est pas un fichier ordinaire"));
    }
    Ok(Some(file))
}

/// Rejoue seulement des enregistrements complets, contigus et vérifiés.
/// Une dernière écriture incomplète n’a autorisé aucun effet de fichier et peut être ignorée.
pub(super) fn load(path: &Path) -> Result<(State, u64), String> {
    let snapshot = match regular_file(path)? {
        Some(mut file) => {
            let mut bytes = Vec::new();
            file.read_to_end(&mut bytes).map_err(failure)?;
            let loaded: snapshot_reader::Loaded =
                serde_json::from_slice(&bytes).map_err(failure)?;
            Some((loaded.state, loaded.sequence))
        }
        None => None,
    };
    let log = regular_file(&log_path(path))?;
    if snapshot.is_none() {
        if let Some(file) = &log {
            if file.metadata().map_err(failure)?.len() > 0 {
                return Err(failure("instantané absent, journal incrémental conservé"));
            }
        }
    }
    let (mut state, mut sequence) = snapshot.unwrap_or_default();
    let base_sequence = sequence;
    // Uniquement le dernier en-tête validé, borné et libéré à la fin du chargement.
    let mut last_header: Option<Box<RawValue>> = None;
    let saved_jobs = std::mem::take(&mut state.jobs);
    let mut jobs = HashMap::with_capacity(saved_jobs.len());
    let mut ids = Vec::with_capacity(saved_jobs.len());
    for job in saved_jobs {
        let id = job.id;
        if jobs
            .insert(id, LoadedJob::Snapshot(Box::new(job)))
            .is_some()
        {
            return Err(failure("identifiant de lot répété"));
        }
        ids.push(id);
    }
    if let Some(file) = log {
        let mut reader = BufReader::with_capacity(64 * 1024, file);
        let mut line = Vec::new();
        loop {
            line.clear();
            if reader.read_until(b'\n', &mut line).map_err(failure)? == 0 {
                break;
            }
            if !line.ends_with(b"\n") {
                break;
            }
            if line.len() < 34 || line[32] != b' ' {
                return Err(failure("enregistrement incrémental endommagé"));
            }
            let payload = &line[33..line.len() - 1];
            if format!("{:x}", md5::compute(payload)).as_bytes() != &line[..32] {
                return Err(failure("somme du journal incorrecte"));
            }
            let record: ReadRecord<'_> = serde_json::from_slice(payload).map_err(failure)?;
            if record.sequence <= base_sequence {
                continue;
            }
            if record.sequence
                != sequence
                    .checked_add(1)
                    .ok_or_else(|| failure("séquence épuisée"))?
            {
                return Err(failure("séquence du journal discontinue"));
            }
            if !record.header.get().starts_with('{') {
                return Err(failure("réglages du journal invalides"));
            }
            let live = record.ids.iter().copied().collect::<HashSet<_>>();
            if live.len() != record.ids.len() {
                return Err(failure("ordre de lots invalide"));
            }
            let full = match (record.format, record.jobs) {
                (None, Some(full)) if record.full_jobs.is_empty() && record.patches.is_empty() => {
                    full
                }
                (Some(2), None) => record.full_jobs,
                _ => return Err(failure("format de journal inconnu ou incohérent")),
            };
            jobs.retain(|id, _| live.contains(id));
            let mut updated = HashSet::new();
            for raw in full {
                let id = delta::id(raw)?;
                if !live.contains(&id) || !updated.insert(id) {
                    return Err(failure("lot hors de l’ordre ou répété"));
                }
                jobs.insert(id, LoadedJob::Encoded(delta::Encoded::new(raw.to_owned())));
            }
            for patch in record.patches {
                if !live.contains(&patch.id) || !updated.insert(patch.id) {
                    return Err(failure("delta hors de l’ordre ou répété"));
                }
                let Some(LoadedJob::Encoded(base)) = jobs.get(&patch.id) else {
                    return Err(failure("base du delta absente"));
                };
                let encoded = patch.apply(base)?;
                jobs.insert(patch.id, LoadedJob::Encoded(encoded));
            }
            if live.iter().any(|id| !jobs.contains_key(id)) {
                return Err(failure("lot manquant dans le journal"));
            }
            if last_header
                .as_ref()
                .is_none_or(|previous| previous.get() != record.header.get())
            {
                state = serde_json::from_str(record.header.get()).map_err(failure)?;
                // Un grand en-tête invalide aussi l'ancienne base, même si on ne le conserve pas.
                last_header = (record.header.get().len() <= MAX_HEADER_CACHE_BYTES)
                    .then(|| record.header.to_owned());
            }
            ids = record.ids;
            sequence = record.sequence;
        }
    }
    state.jobs = ids
        .into_iter()
        .map(|id| match jobs.remove(&id).unwrap() {
            LoadedJob::Snapshot(job) => Ok(*job),
            LoadedJob::Encoded(encoded) => {
                serde_json::from_str::<snapshot::TrackedJob>(encoded.raw.get())
                    .map_err(failure)
                    .and_then(|job| {
                        if job.id == id {
                            Ok(job)
                        } else {
                            Err(failure("identité de lot incohérente"))
                        }
                    })
            }
        })
        .collect::<Result<Vec<_>, String>>()?;
    Ok((state, sequence))
}
/// Encoder un enregistrement complet avec son empreinte avant toute E/S durable.
fn encode_record(value: &impl Serialize) -> Result<Vec<u8>, String> {
    // Réserver le préfixe puis sérialiser directement dans le tampon qui sera écrit.
    let mut bytes = Vec::with_capacity(1024);
    bytes.resize(33, b' ');
    serde_json::to_writer(&mut bytes, value).map_err(failure)?;
    let digest = md5::compute(&bytes[33..]);
    let mut checksum = &mut bytes[..32];
    write!(&mut checksum, "{digest:x}").map_err(failure)?;
    bytes.push(b'\n');
    Ok(bytes)
}

/// Un temporaire appartient à une seule compaction ; aucun ancien nom n’est tronqué ou réutilisé.
fn snapshot_temporary(path: &Path) -> Result<(fs::File, Temp), String> {
    static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
    for _ in 0..1000 {
        let serial = NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let temporary = path.with_extension(format!("json.tmp-{}-{serial}", std::process::id()));
        let file = match OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .custom_flags(0x20000 | 0x800)
            .open(&temporary)
        {
            Ok(file) => file,
            Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
            Err(error) => return Err(failure(error)),
        };
        let stamp = undo::Stamp::from_metadata(&temporary, &file.metadata().map_err(failure)?)
            .map_err(failure)?;
        return Ok((file, Temp::new(temporary, stamp)));
    }
    Err(failure(std::io::Error::from_raw_os_error(17)))
}
impl Writer {
    /// Valide le descripteur et le nom avant toute écriture, sans suivre un lien ni attendre sur un FIFO.
    fn validate_log(&self, path: &Path, file: &fs::File, append: bool) -> Result<(), String> {
        let meta = file.metadata().map_err(failure)?;
        let named = fs::symlink_metadata(log_path(path)).map_err(failure)?;
        let identity = (meta.dev(), meta.ino());
        if !meta.is_file()
            || meta.nlink() != 1
            || !named.is_file()
            || (named.dev(), named.ino()) != identity
            || self
                .log_identity
                .is_some_and(|expected| expected != identity)
            || (append && self.log_identity.is_none())
            || (self.log_identity.is_some() && meta.len() != self.length)
        {
            return Err(failure("journal incrémental modifié hors de Gad"));
        }
        Ok(())
    }
    fn open_log(&self, path: &Path, append: bool) -> Result<fs::File, String> {
        let file = OpenOptions::new()
            .write(true)
            .truncate(false)
            .append(append)
            .create(!append && self.log_identity.is_none())
            .mode(0o600)
            .custom_flags(0x20000 | 0x800)
            .open(log_path(path))
            .map_err(failure)?;
        self.validate_log(path, &file, append)?;
        Ok(file)
    }

    /// Une seule instance sous le verrou writer ; state doit rester stable pendant la capture.
    pub(super) fn save(
        &mut self,
        path: &Path,
        state: &State,
        checkpoint: bool,
    ) -> Result<(), String> {
        if self.poisoned {
            return Err(failure(
                "écriture précédente incertaine, redémarrage nécessaire",
            ));
        }
        let meta = match fs::symlink_metadata(path) {
            Ok(meta) if meta.is_file() => Some(meta),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => None,
            Ok(_) => return Err(failure("l’instantané n’est pas un fichier ordinaire")),
            Err(error) => return Err(failure(error)),
        };
        if let (Some(expected), Some(meta)) = (self.snapshot, meta.as_ref()) {
            if fingerprint(meta) != expected {
                return Err(failure("instantané modifié hors de Gad"));
            }
        }
        let revisions = state
            .jobs
            .iter()
            .map(|job| (job.id, job.revision()))
            .collect::<HashMap<_, _>>();
        let ids = state.jobs.iter().map(|job| job.id).collect::<Vec<_>>();
        let header_changed = self
            .header
            .as_ref()
            .is_none_or(|header| !header.matches(state));
        // Ne pas compacter un historique inchangé au seul motif que le seuil est atteint.
        if !checkpoint
            && self.snapshot.is_some()
            && meta.is_some()
            && !header_changed
            && self.ids == ids
            && self.revisions == revisions
        {
            return Ok(());
        }
        if checkpoint
            || self.snapshot.is_none()
            || meta.is_none()
            || self.records >= MAX_RECORDS
            || self.length >= MAX_BYTES
        {
            return self.checkpoint(path, state, ids, revisions);
        }
        let sequence = self
            .sequence
            .checked_add(1)
            .ok_or_else(|| failure("séquence épuisée"))?;
        // Ne publier le nouveau cache qu'après l'écriture durable de son enregistrement.
        let header = header_changed
            .then(|| settings::Cached::new(state))
            .transpose()?;
        let mut pending = Vec::new();
        let mut cache_size = 0;
        let mut fallback = Vec::new();
        let mut oversized = Vec::new();
        for job in state
            .jobs
            .iter()
            .filter(|job| self.revisions.get(&job.id) != Some(&job.revision()))
        {
            if cache_size >= MAX_CACHE_BYTES || self.oversized.contains(&job.id) {
                fallback.push(job);
                continue;
            }
            let encoded = delta::Encoded::new(to_raw_value(job).map_err(failure)?);
            let size = encoded.raw.get().len();
            if size > MAX_JOB_CACHE_BYTES {
                oversized.push(job.id);
                fallback.push(job);
                continue;
            }
            if size > MAX_CACHE_BYTES - cache_size {
                fallback.push(job);
                continue;
            }
            cache_size += size;
            pending.push((job.id, encoded));
        }
        let mut full_jobs = Vec::new();
        let mut patches = Vec::new();
        for (id, encoded) in &pending {
            let patch = self
                .encoded
                .get(id)
                .map(|old| delta::Patch::between(*id, old, encoded))
                .transpose()?
                .flatten();
            if let Some(patch) = patch {
                patches.push(patch);
            } else {
                full_jobs.push(FullJob::Raw(&encoded.raw));
            }
        }
        let uncached = fallback.iter().map(|job| job.id).collect::<HashSet<_>>();
        full_jobs.extend(fallback.into_iter().map(FullJob::Job));
        let bytes = encode_record(&WriteRecord {
            sequence,
            header: &header
                .as_ref()
                .or(self.header.as_ref())
                .ok_or_else(|| failure("en-tête du journal absent"))?
                .raw,
            ids: &ids,
            jobs: None,
            format: 2,
            full_jobs,
            patches,
        })?;
        let mut log = self.open_log(path, true)?;
        if let Err(error) = log.write_all(&bytes).and_then(|_| log.sync_all()) {
            // Le refus n’autorise aucun effet de fichier. Revenir à la dernière frontière durable.
            if log
                .set_len(self.length)
                .and_then(|_| log.sync_all())
                .is_err()
            {
                self.poisoned = true;
            }
            return Err(failure(error));
        }
        // La base ne change qu’après fsync réussi ; un refus conserve la dernière base durable.
        let live = ids.iter().copied().collect::<HashSet<_>>();
        let replaced = pending.iter().map(|(id, _)| *id).collect::<HashSet<_>>();
        self.encoded
            .retain(|id, _| live.contains(id) && !uncached.contains(id) && !replaced.contains(id));
        if self
            .encoded
            .values()
            .map(|value| value.raw.get().len())
            .sum::<usize>()
            + cache_size
            > MAX_CACHE_BYTES
        {
            self.encoded.clear();
        }
        self.encoded.extend(pending);
        self.oversized.retain(|id| live.contains(id));
        self.oversized.extend(oversized);
        self.length += bytes.len() as u64;
        self.sequence = sequence;
        self.records += 1;
        if let Some(header) = header {
            self.header = Some(header);
        }
        self.ids = ids;
        self.revisions = revisions;
        Ok(())
    }
    /// Publier d’abord l’instantané durable : un ancien WAL encore présent sera ignoré par séquence.
    fn checkpoint(
        &mut self,
        path: &Path,
        state: &State,
        ids: Vec<u64>,
        revisions: HashMap<u64, u64>,
    ) -> Result<(), String> {
        let header = settings::Cached::new(state)?;
        let parent = path.parent().ok_or_else(|| failure("dossier absent"))?;
        fs::DirBuilder::new()
            .recursive(true)
            .mode(0o700)
            .create(parent)
            .map_err(failure)?;
        let bytes = serde_json::to_vec(&Checkpoint {
            state,
            sequence: self.sequence,
        })
        .map_err(failure)?;
        // Valider le WAL avant la publication évite de remplacer l’instantané si son journal a été substitué.
        let log = self.open_log(path, false)?;
        let pinned_log = fingerprint(&log.metadata().map_err(failure)?);
        let (mut output, temporary) = snapshot_temporary(path)?;
        output
            .write_all(&bytes)
            .and_then(|_| output.sync_all())
            .map_err(failure)?;
        temporary
            .check_version(&output.metadata().map_err(failure)?)
            .map_err(failure)?;
        fs::rename(&temporary.path, path).map_err(failure)?;
        fs::File::open(parent)
            .and_then(|file| file.sync_all())
            .map_err(failure)?;
        // Après publication, un refus exige un rechargement avant toute nouvelle écriture.
        self.poisoned = true;
        self.validate_log(path, &log, false)?;
        if fingerprint(&log.metadata().map_err(failure)?) != pinned_log {
            return Err(failure("journal incrémental modifié hors de Gad"));
        }
        // Le descripteur est celui qui a été validé : aucun O_TRUNC ne précède les contrôles.
        log.set_len(0)
            .and_then(|_| log.sync_all())
            .map_err(failure)?;
        fs::File::open(parent)
            .and_then(|file| file.sync_all())
            .map_err(failure)?;
        let meta = log.metadata().map_err(failure)?;
        self.log_identity = Some((meta.dev(), meta.ino()));
        self.snapshot = Some(fingerprint(&fs::symlink_metadata(path).map_err(failure)?));
        self.encoded.clear();
        self.oversized.clear();
        self.length = 0;
        self.records = 0;
        self.header = Some(header);
        self.ids = ids;
        self.revisions = revisions;
        self.poisoned = false;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::PermissionsExt;
    struct Fixture(PathBuf);
    impl Fixture {
        fn new() -> Self {
            let root = std::env::temp_dir().join(format!(
                "gad-journal-{}-{}",
                std::process::id(),
                snapshot::touch()
            ));
            fs::create_dir(&root).unwrap();
            Self(root)
        }
        fn path(&self) -> PathBuf {
            self.0.join("private/state.json")
        }
    }
    impl Drop for Fixture {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }
    fn job(id: u64) -> snapshot::TrackedJob {
        serde_json::from_value(json!({"id":id,"sources":[],"destination":"/unused","state":"done","entries":[],"error":"","cancel":false,"policy":"ask","verify_md5":false,"mode":"copy"})).unwrap()
    }
    fn state_value(state: &State) -> Value {
        serde_json::to_value(state).unwrap()
    }
    fn append(path: &Path, sequence: u64, state: &State) {
        let record = Record {
            sequence,
            header: header(state),
            jobs: state
                .jobs
                .iter()
                .map(|job| serde_json::to_value(job).unwrap())
                .collect(),
            ids: state.jobs.iter().map(|job| job.id).collect(),
        };
        let payload = serde_json::to_vec(&record).unwrap();
        let line = format!(
            "{:x} {}\n",
            md5::compute(&payload),
            String::from_utf8(payload).unwrap()
        );
        OpenOptions::new()
            .append(true)
            .open(log_path(path))
            .unwrap()
            .write_all(line.as_bytes())
            .unwrap();
    }
    #[test]
    fn encoded_record_matches_previous_bytes_and_checksum() {
        for size in [0, 1, 255, 256, 1024, 65536, 1024 * 1024] {
            let value = json!({"sequence":42,"header":{"name":"é😀\n\""},"jobs":[],"padding":"x".repeat(size)});
            let payload = serde_json::to_vec(&value).unwrap();
            let mut expected = format!("{:x} ", md5::compute(&payload)).into_bytes();
            expected.extend_from_slice(&payload);
            expected.push(b'\n');
            assert_eq!(encode_record(&value).unwrap(), expected, "size={size}");
        }
        // Une erreur de sérialisation ne produit aucun enregistrement partiel exploitable.
        struct Refused;
        impl Serialize for Refused {
            fn serialize<S: serde::Serializer>(&self, _serializer: S) -> Result<S::Ok, S::Error> {
                Err(serde::ser::Error::custom("refused"))
            }
        }
        assert!(encode_record(&Refused).is_err());
    }
    #[test]
    fn raw_wal_header_keeps_object_validation_and_covered_record_rules() {
        let f = Fixture::new();
        let path = f.path();
        let state = State::default();
        Writer::default().save(&path, &state, false).unwrap();
        for header in [
            "null",
            "[]",
            r#"{"paused":false,"paused":true}"#,
            r#"{"paused":false,"p\u0061used":true}"#,
        ] {
            let payload = format!(r#"{{"sequence":1,"header":{header},"ids":[],"jobs":[]}}"#);
            fs::write(
                log_path(&path),
                format!("{:x} {payload}\n", md5::compute(payload.as_bytes())),
            )
            .unwrap();
            assert!(load(&path).is_err(), "{header}");
        }
        // Un préfixe déjà couvert par l'instantané conserve sa règle d'ignorance.
        let payload = r#"{"sequence":0,"header":null,"ids":[],"jobs":[]}"#;
        fs::write(
            log_path(&path),
            format!("{:x} {payload}\n", md5::compute(payload.as_bytes())),
        )
        .unwrap();
        assert_eq!(state_value(&load(&path).unwrap().0), state_value(&state));
        let payload = r#"{"sequence":1,"header":  {"rate_limit":7}  ,"ids":[],"jobs":[]}"#;
        fs::write(
            log_path(&path),
            format!("{:x} {payload}\n", md5::compute(payload.as_bytes())),
        )
        .unwrap();
        assert_eq!(load(&path).unwrap().0.rate_limit, 7);
    }
    #[test]
    fn replay_header_cache_discards_old_base_after_oversized_header() {
        let f = Fixture::new();
        let path = f.path();
        Writer::default()
            .save(&path, &State::default(), false)
            .unwrap();
        let small = json!({"rate_limit":5});
        let large = json!({"rate_limit":77,"unknown_padding":"x".repeat(MAX_HEADER_CACHE_BYTES+1)});
        let mut bytes = Vec::new();
        for (index, header) in [&small, &small, &large, &small].into_iter().enumerate() {
            let payload =
                serde_json::to_vec(&json!({"sequence":index+1,"header":header,"ids":[],"jobs":[]}))
                    .unwrap();
            writeln!(
                &mut bytes,
                "{:x} {}",
                md5::compute(&payload),
                std::str::from_utf8(&payload).unwrap()
            )
            .unwrap();
        }
        fs::write(log_path(&path), bytes).unwrap();
        assert_eq!(load(&path).unwrap().0.rate_limit, 5);
    }
    #[test]
    #[ignore = "benchmark explicite des en-têtes WAL avec ordre personnalisé"]
    fn benchmark_wal_headers() {
        for mode in ["changing", "stable"] {
            let f = Fixture::new();
            let path = f.path();
            let mut state = State {
                row_order: (0..10000)
                    .map(|index| row_queue::Key {
                        job: 1,
                        index: Some(index),
                    })
                    .collect(),
                ..State::default()
            };
            if mode == "stable" {
                state.jobs.push(job(1));
            }
            let mut writer = Writer::default();
            writer.save(&path, &state, false).unwrap();
            for index in 1..=30 {
                if mode == "changing" {
                    state.rate_limit = index;
                } else {
                    state.jobs[0].error = format!("round-{index:06}");
                }
                writer.save(&path, &state, false).unwrap();
            }
            for trial in 0..3 {
                let start = std::time::Instant::now();
                for _ in 0..3 {
                    let loaded = load(std::hint::black_box(&path)).unwrap();
                    assert_eq!(loaded.0.rate_limit, if mode == "changing" { 30 } else { 0 });
                    assert_eq!(loaded.0.row_order.len(), 10000);
                    if mode == "stable" {
                        assert_eq!(loaded.0.jobs[0].error, "round-000030");
                    }
                    std::hint::black_box(loaded);
                }
                println!(
                    "wal_headers mode={mode} trial={trial} elapsed_ms={:.3}",
                    start.elapsed().as_secs_f64() * 1000.0
                );
            }
        }
    }
    #[test]
    #[ignore = "benchmark explicite du chargement d'instantané"]
    fn benchmark_snapshot_load() {
        let f = Fixture::new();
        let path = f.path();
        let entries=(0..10000).map(|index|json!({"source":format!("/source/f{index}"),"destination":format!("/out/f{index}"),"kind":"file","size":9,"copied":9,"verified":0,"state":"done","error":""})).collect::<Vec<_>>();
        let state:State=serde_json::from_value(json!({"jobs":[{"id":1,"sources":[],"destination":"/out","mode":"copy","verify_md5":false,"state":"done","error":"","cancel":false,"policy":"ask","entries":entries}]})).unwrap();
        Writer::default().save(&path, &state, false).unwrap();
        for trial in 0..3 {
            let start = std::time::Instant::now();
            for _ in 0..10 {
                std::hint::black_box(load(std::hint::black_box(&path)).unwrap());
            }
            println!(
                "snapshot_load trial={trial} elapsed_ms={:.3}",
                start.elapsed().as_secs_f64() * 1000.0
            );
        }
    }
    #[test]
    #[ignore = "benchmark explicite de l'encodage WAL"]
    fn benchmark_record_encoding() {
        for size in [256, 4 * 1024 * 1024] {
            let record =
                json!({"sequence":1,"header":{},"jobs":[],"ids":[],"padding":"x".repeat(size)});
            let count = if size == 256 { 10000 } else { 10 };
            for trial in 0..3 {
                let start = std::time::Instant::now();
                for _ in 0..count {
                    std::hint::black_box(encode_record(std::hint::black_box(&record)).unwrap());
                }
                println!(
                    "record_encoding size={size} count={count} trial={trial} elapsed_ms={:.3}",
                    start.elapsed().as_secs_f64() * 1000.0
                );
            }
        }
    }
    #[test]
    #[ignore = "benchmark explicite des sauvegardes sans mutation"]
    fn benchmark_unchanged_header() {
        let f = Fixture::new();
        let path = f.path();
        let mut writer = Writer::default();
        let state = State {
            row_order: (0..10_000)
                .map(|index| row_queue::Key {
                    job: 1,
                    index: Some(index),
                })
                .collect(),
            ..State::default()
        };
        writer.save(&path, &state, false).unwrap();
        for trial in 0..3 {
            let start = std::time::Instant::now();
            for _ in 0..100 {
                writer
                    .save(&path, std::hint::black_box(&state), false)
                    .unwrap();
            }
            println!(
                "unchanged_header trial={trial} elapsed_ms={:.3}",
                start.elapsed().as_secs_f64() * 1000.0
            );
        }
    }
    #[test]
    /// Atteindre le seuil n’impose aucune réécriture si rien n’a changé.
    fn unchanged_save_defers_compaction_but_explicit_checkpoint_still_runs() {
        let f = Fixture::new();
        let path = f.path();
        let mut writer = Writer::default();
        let mut state = State::default();
        writer.save(&path, &state, false).unwrap();
        for value in 1..=MAX_RECORDS as u64 {
            state.rate_limit = value;
            writer.save(&path, &state, false).unwrap();
        }
        let inode = fs::metadata(&path).unwrap().ino();
        let length = fs::metadata(log_path(&path)).unwrap().len();
        writer.save(&path, &state, false).unwrap();
        assert_eq!(writer.records, MAX_RECORDS);
        assert_eq!(fs::metadata(&path).unwrap().ino(), inode);
        assert_eq!(fs::metadata(log_path(&path)).unwrap().len(), length);
        state.rate_limit += 1;
        writer.save(&path, &state, false).unwrap();
        assert_eq!(writer.records, 0);
        assert_eq!(state_value(&load(&path).unwrap().0), state_value(&state));
        let inode = fs::metadata(&path).unwrap().ino();
        writer.save(&path, &state, true).unwrap();
        assert_ne!(fs::metadata(&path).unwrap().ino(), inode);
    }
    #[test]
    /// Un instantané mal formé doit être refusé sans panic ni réécriture.
    fn malformed_snapshots_are_preserved_and_never_panic() {
        let f = Fixture::new();
        let path = f.path();
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        for bytes in [
            b"null".as_slice(),
            b"123",
            b"[]",
            br#"{"jobs":{}}"#,
            br#"{"jobs":[{"id":1},{"id":1}]}"#,
        ] {
            fs::write(&path, bytes).unwrap();
            assert!(load(&path).is_err());
            assert_eq!(fs::read(&path).unwrap(), bytes);
        }
        let bytes = serde_json::to_vec(&json!({"jobs":[job(1),job(1)]})).unwrap();
        fs::write(&path, &bytes).unwrap();
        assert!(load(&path).is_err());
        assert_eq!(fs::read(&path).unwrap(), bytes);
    }
    #[test]
    /// Le lecteur ne suit pas de lien final et ne tente pas de lire un fichier spécial.
    fn loading_rejects_journal_links_and_directories_without_touching_targets() {
        let f = Fixture::new();
        let path = f.path();
        let mut writer = Writer::default();
        writer.save(&path, &State::default(), false).unwrap();
        let outside = f.0.join("outside");
        fs::write(&outside, b"untouched").unwrap();
        fs::remove_file(log_path(&path)).unwrap();
        symlink(&outside, log_path(&path)).unwrap();
        assert!(load(&path).is_err());
        fs::remove_file(log_path(&path)).unwrap();
        fs::create_dir(log_path(&path)).unwrap();
        assert!(load(&path).is_err());
        fs::remove_dir(log_path(&path)).unwrap();
        fs::remove_file(&path).unwrap();
        symlink(&outside, &path).unwrap();
        assert!(load(&path).is_err());
        assert_eq!(fs::read(&outside).unwrap(), b"untouched");
    }
    #[test]
    /// Une seule entrée modifiée ne doit plus réécrire les autres entrées du lot.
    fn patches_replay_job_changes_and_reset_safely_after_compaction() {
        let f = Fixture::new();
        let path = f.path();
        let mut writer = Writer::default();
        let mut state = State::default();
        let mut tracked = job(1);
        tracked.error = "padding".repeat(2000);
        state.jobs.push(tracked);
        writer.save(&path, &state, false).unwrap();
        state.jobs[0].paused = true;
        writer.save(&path, &state, false).unwrap();
        let before = writer.length;
        state.jobs[0].rate_limit = 42;
        writer.save(&path, &state, false).unwrap();
        assert!(writer.length - before < 1000);
        assert_eq!(state_value(&load(&path).unwrap().0), state_value(&state));
        let lines = fs::read(log_path(&path)).unwrap();
        let last = lines
            .split(|byte| *byte == b'\n')
            .filter(|line| !line.is_empty())
            .next_back()
            .unwrap();
        let record: ReadRecord<'_> = serde_json::from_slice(&last[33..]).unwrap();
        assert!(record.jobs.is_none());
        assert_eq!(record.format, Some(2));
        assert_eq!(record.patches.len(), 1);
        // Un ancien lecteur doit échouer explicitement, plutôt qu’ignorer les deltas.
        assert!(serde_json::from_slice::<Record>(&last[33..]).is_err());
        writer.save(&path, &state, true).unwrap();
        assert!(writer.encoded.is_empty());
        state.jobs[0].rate_limit = 43;
        writer.save(&path, &state, false).unwrap();
        let bytes = fs::read(log_path(&path)).unwrap();
        let record: ReadRecord<'_> = serde_json::from_slice(&bytes[33..]).unwrap();
        assert!(record.patches.is_empty());
        assert_eq!(record.full_jobs.len(), 1);
        assert_eq!(state_value(&load(&path).unwrap().0), state_value(&state));
    }
    #[test]
    /// Un delta manquant ou déplacé ne doit jamais produire une récupération approximative.
    fn missing_patch_base_and_unknown_format_are_preserved() {
        let f = Fixture::new();
        let path = f.path();
        let mut writer = Writer::default();
        let mut state = State::default();
        let mut tracked = job(1);
        tracked.error = "x".repeat(6000);
        state.jobs.push(tracked);
        writer.save(&path, &state, false).unwrap();
        state.jobs[0].paused = true;
        writer.save(&path, &state, false).unwrap();
        state.jobs[0].rate_limit = 17;
        writer.save(&path, &state, false).unwrap();
        let bytes = fs::read(log_path(&path)).unwrap();
        let last = bytes
            .split(|byte| *byte == b'\n')
            .filter(|line| !line.is_empty())
            .next_back()
            .unwrap();
        let mut value: Value = serde_json::from_slice(&last[33..]).unwrap();
        value["sequence"] = json!(1);
        let payload = serde_json::to_vec(&value).unwrap();
        let line = format!(
            "{:x} {}\n",
            md5::compute(&payload),
            String::from_utf8(payload).unwrap()
        );
        fs::write(log_path(&path), line.as_bytes()).unwrap();
        assert!(load(&path).is_err());
        assert_eq!(fs::read(log_path(&path)).unwrap(), line.as_bytes());
        value["format"] = json!(999);
        let payload = serde_json::to_vec(&value).unwrap();
        let line = format!(
            "{:x} {}\n",
            md5::compute(&payload),
            String::from_utf8(payload).unwrap()
        );
        fs::write(log_path(&path), line.as_bytes()).unwrap();
        assert!(load(&path).is_err());
    }
    #[test]
    fn oversized_jobs_are_not_retained_in_the_delta_cache() {
        let f = Fixture::new();
        let path = f.path();
        let mut writer = Writer::default();
        let mut state = State::default();
        let mut tracked = job(1);
        tracked.error = "x".repeat(MAX_JOB_CACHE_BYTES + 1);
        state.jobs.push(tracked);
        writer.save(&path, &state, false).unwrap();
        state.jobs[0].paused = true;
        writer.save(&path, &state, false).unwrap();
        assert!(writer.encoded.is_empty());
        assert!(writer.oversized.contains(&1));
        assert_eq!(state_value(&load(&path).unwrap().0), state_value(&state));
        writer.save(&path, &state, true).unwrap();
        assert!(writer.oversized.is_empty());
    }
    #[test]
    /// Une écriture refusée ne doit pas devenir la base d’un delta ultérieur.
    fn rejected_write_keeps_the_last_durable_delta_base() {
        let f = Fixture::new();
        let path = f.path();
        let mut writer = Writer::default();
        let mut state = State::default();
        let mut tracked = job(1);
        tracked.error = "x".repeat(6000);
        state.jobs.push(tracked);
        writer.save(&path, &state, false).unwrap();
        state.jobs[0].paused = true;
        writer.save(&path, &state, false).unwrap();
        let sequence = writer.sequence;
        let saved = writer.encoded[&1].raw.get().to_owned();
        let parked = f.0.join("wal.saved");
        fs::rename(log_path(&path), &parked).unwrap();
        fs::write(log_path(&path), b"").unwrap();
        state.jobs[0].rate_limit = 123;
        assert!(writer.save(&path, &state, false).is_err());
        assert_eq!(writer.sequence, sequence);
        assert_eq!(writer.encoded[&1].raw.get(), saved);
        fs::remove_file(log_path(&path)).unwrap();
        fs::rename(parked, log_path(&path)).unwrap();
        writer.save(&path, &state, false).unwrap();
        assert_eq!(load(&path).unwrap().0.jobs[0].rate_limit, 123);
    }
    #[test]
    /// Remplacer une base ne doit pas compter deux fois sa taille et évincer l’autre lot actif.
    fn replacing_cached_job_preserves_other_bases_within_budget() {
        let f = Fixture::new();
        let path = f.path();
        let mut writer = Writer::default();
        let mut state = State::default();
        for id in [1, 2] {
            let mut tracked = job(id);
            tracked.error = "x".repeat(6 * 1024 * 1024);
            state.jobs.push(tracked);
        }
        writer.save(&path, &state, false).unwrap();
        for tracked in &mut state.jobs {
            tracked.paused = true;
        }
        writer.save(&path, &state, false).unwrap();
        assert_eq!(writer.encoded.len(), 2);
        state.jobs[0].rate_limit = 17;
        writer.save(&path, &state, false).unwrap();
        assert_eq!(writer.encoded.len(), 2);
        assert!(
            writer
                .encoded
                .values()
                .map(|value| value.raw.get().len())
                .sum::<usize>()
                <= MAX_CACHE_BYTES
        );
        let saved = load(&path).unwrap().0;
        assert_eq!(saved.jobs[0].rate_limit, 17);
        assert_eq!(saved.jobs[1].error.len(), 6 * 1024 * 1024);
    }
    #[test]
    fn header_covers_all_persistent_settings() {
        let state = State::default();
        let mut expected = state_value(&state);
        expected.as_object_mut().unwrap().remove("jobs");
        assert_eq!(header(&state), expected);
    }
    #[test]
    fn cached_header_covers_each_setting_and_detects_its_mutations() {
        let state = State::default();
        let cached = settings::Cached::new(&state).unwrap();
        let mut expected = state_value(&state);
        expected.as_object_mut().unwrap().remove("jobs");
        assert_eq!(
            serde_json::from_str::<Value>(cached.raw.get()).unwrap(),
            expected
        );
        assert!(cached.matches(&state));
        for (name, original) in expected.as_object().unwrap() {
            let replacement = match original {
                Value::Bool(value) => json!(!value),
                Value::Number(value) => json!(value.as_u64().unwrap() + 1),
                Value::String(value) => json!(format!("{value}-changed")),
                Value::Array(_) if name == "row_order" => json!([{"job":1,"index":0}]),
                Value::Array(_) => json!([1]),
                Value::Object(_) if name == "exclusions" => {
                    json!({"include_hidden":false,"directories":["__*"],"files":["*.tmp"]})
                }
                _ => panic!("nouveau réglage à couvrir : {name}"),
            };
            let mut changed = state_value(&state);
            changed[name] = replacement;
            let changed: State = serde_json::from_value(changed).unwrap();
            assert!(!cached.matches(&changed), "{name}");
            let next = settings::Cached::new(&changed).unwrap();
            assert!(next.matches(&changed));
            assert!(!next.matches(&state), "{name}");
        }
    }
    #[test]
    fn job_progress_reuses_header_but_reordering_refreshes_it() {
        let f = Fixture::new();
        let path = f.path();
        let mut writer = Writer::default();
        let mut state = State {
            jobs: vec![job(1)],
            ..State::default()
        };
        writer.save(&path, &state, false).unwrap();
        let pointer = writer.header.as_ref().unwrap().raw.get().as_ptr();
        state.jobs[0].error = "progress".into();
        writer.save(&path, &state, false).unwrap();
        assert_eq!(writer.header.as_ref().unwrap().raw.get().as_ptr(), pointer);
        state.row_order.push(row_queue::Key {
            job: 1,
            index: None,
        });
        writer.save(&path, &state, false).unwrap();
        assert_ne!(writer.header.as_ref().unwrap().raw.get().as_ptr(), pointer);
        assert_eq!(state_value(&load(&path).unwrap().0), state_value(&state));
    }
    #[test]
    fn rejected_record_does_not_publish_pending_header() {
        let f = Fixture::new();
        let path = f.path();
        let mut writer = Writer::default();
        let mut state = State::default();
        writer.save(&path, &state, false).unwrap();
        let pointer = writer.header.as_ref().unwrap().raw.get().as_ptr();
        // Une modification externe force le refus avant toute écriture.
        let file = OpenOptions::new()
            .write(true)
            .open(log_path(&path))
            .unwrap();
        file.set_len(1).unwrap();
        state.paused = !state.paused;
        assert!(writer.save(&path, &state, false).is_err());
        assert_eq!(writer.header.as_ref().unwrap().raw.get().as_ptr(), pointer);
        assert!(!writer.header.as_ref().unwrap().matches(&state));
        assert_eq!(writer.sequence, 0);
        file.set_len(0).unwrap();
        writer.save(&path, &state, false).unwrap();
        assert!(writer.header.as_ref().unwrap().matches(&state));
        assert_eq!(state_value(&load(&path).unwrap().0), state_value(&state));
    }
    #[test]
    fn changed_jobs_only_and_removals_and_order_survive_replay() {
        let f = Fixture::new();
        let path = f.path();
        let mut writer = Writer::default();
        let mut state = State {
            jobs: vec![job(1), job(2), job(3)],
            ..State::default()
        };
        writer.save(&path, &state, false).unwrap();
        let inode = fs::metadata(&path).unwrap().ino();
        state.jobs[1].error = "changed".into();
        writer.save(&path, &state, false).unwrap();
        let log = fs::read_to_string(log_path(&path)).unwrap();
        let first: ReadRecord<'_> = serde_json::from_str(&log[33..]).unwrap();
        assert_eq!(first.full_jobs.len(), 1);
        assert_eq!(delta::id(first.full_jobs[0]).unwrap(), 2);
        state.jobs.remove(0);
        state.jobs.swap(0, 1);
        state.paused = true;
        writer.save(&path, &state, false).unwrap();
        assert_eq!(state_value(&load(&path).unwrap().0), state_value(&state));
        assert_eq!(fs::metadata(&path).unwrap().ino(), inode);
        let length = fs::metadata(log_path(&path)).unwrap().len();
        writer.save(&path, &state, false).unwrap();
        assert_eq!(fs::metadata(log_path(&path)).unwrap().len(), length);
    }
    #[test]
    fn partial_tail_is_ignored_but_complete_corruption_is_rejected() {
        let f = Fixture::new();
        let path = f.path();
        let mut writer = Writer::default();
        let mut state = State::default();
        writer.save(&path, &state, false).unwrap();
        state.paused = true;
        writer.save(&path, &state, false).unwrap();
        let committed = fs::read(log_path(&path)).unwrap();
        state.rate_limit = 123;
        writer.save(&path, &state, false).unwrap();
        let all = fs::read(log_path(&path)).unwrap();
        for offset in (committed.len()..all.len()).step_by(7) {
            fs::write(log_path(&path), &all[..offset]).unwrap();
            let (replayed, sequence) = load(&path).unwrap();
            assert!(replayed.paused);
            assert_eq!(replayed.rate_limit, 0);
            assert_eq!(sequence, 1);
        }
        fs::write(log_path(&path), &all).unwrap();
        assert_eq!(load(&path).unwrap().0.rate_limit, 123);
        let mut corrupt = all.clone();
        corrupt[40] ^= 1;
        fs::write(log_path(&path), &corrupt).unwrap();
        assert!(load(&path).is_err());
        assert_eq!(fs::read(log_path(&path)).unwrap(), corrupt);
    }
    #[test]
    fn compaction_survives_old_log_after_snapshot_publication() {
        let f = Fixture::new();
        let path = f.path();
        let mut writer = Writer::default();
        let mut state = State::default();
        writer.save(&path, &state, false).unwrap();
        state.paused = true;
        writer.save(&path, &state, false).unwrap();
        let old = fs::read(log_path(&path)).unwrap();
        writer.save(&path, &state, true).unwrap();
        assert!(fs::read(log_path(&path)).unwrap().is_empty());
        // Arrêt juste après le renommage de l’instantané, avant la remise à zéro du WAL.
        fs::write(log_path(&path), &old).unwrap();
        assert_eq!(load(&path).unwrap().1, 1);
        state.rate_limit = 456;
        append(&path, 2, &state);
        assert_eq!(state_value(&load(&path).unwrap().0), state_value(&state));
    }
    #[test]
    fn record_count_compacts_and_new_deltas_continue() {
        let f = Fixture::new();
        let path = f.path();
        let mut writer = Writer::default();
        let mut state = State::default();
        writer.save(&path, &state, false).unwrap();
        for limit in 1..=MAX_RECORDS as u64 + 3 {
            state.rate_limit = limit;
            writer.save(&path, &state, false).unwrap();
        }
        assert!(writer.records < MAX_RECORDS);
        assert_eq!(state_value(&load(&path).unwrap().0), state_value(&state));
    }
    #[test]
    fn missing_snapshot_gaps_duplicates_and_symlinks_are_rejected() {
        let f = Fixture::new();
        let path = f.path();
        let mut writer = Writer::default();
        let mut state = State::default();
        writer.save(&path, &state, false).unwrap();
        state.paused = true;
        writer.save(&path, &state, false).unwrap();
        let log = fs::read(log_path(&path)).unwrap();
        append(&path, 1, &state);
        assert!(load(&path).is_err());
        fs::write(log_path(&path), &log).unwrap();
        append(&path, 3, &state);
        assert!(load(&path).is_err());
        fs::write(log_path(&path), &log).unwrap();
        assert_eq!(
            writer.snapshot,
            Some(fingerprint(&fs::metadata(&path).unwrap()))
        );
        let parked = f.0.join("snapshot.json");
        thread::sleep(std::time::Duration::from_millis(10));
        fs::rename(&path, &parked).unwrap();
        assert!(load(&path).is_err());
        fs::rename(parked, &path).unwrap();
        // Le contenu est revenu, mais le renommage externe change ctime : l’ancien écrivain doit refuser.
        assert!(writer
            .save(&path, &state, true)
            .unwrap_err()
            .contains("instantané modifié hors de Gad"));
        let (reloaded, sequence) = load(&path).unwrap();
        assert_eq!(state_value(&reloaded), state_value(&state));
        writer = Writer {
            sequence,
            ..Writer::default()
        };
        writer.save(&path, &reloaded, true).unwrap();
        assert_eq!(
            fs::metadata(&path).unwrap().permissions().mode() & 0o777,
            0o600
        );
        assert_eq!(
            fs::metadata(log_path(&path)).unwrap().permissions().mode() & 0o777,
            0o600
        );
        assert_eq!(
            fs::metadata(path.parent().unwrap())
                .unwrap()
                .permissions()
                .mode()
                & 0o777,
            0o700
        );
        let outside = f.0.join("untouched");
        fs::write(&outside, b"safe").unwrap();
        fs::remove_file(log_path(&path)).unwrap();
        symlink(&outside, log_path(&path)).unwrap();
        state.rate_limit = 42;
        assert!(writer.save(&path, &state, false).is_err());
        assert_eq!(fs::read(outside).unwrap(), b"safe");
    }
    #[test]
    fn legacy_snapshot_loads_without_log_and_invalid_log_is_preserved() {
        let f = Fixture::new();
        let path = f.path();
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        let state = State::default();
        fs::write(&path, serde_json::to_vec(&state).unwrap()).unwrap();
        assert_eq!(load(&path).unwrap().1, 0);
        fs::write(log_path(&path), b"broken\n").unwrap();
        assert!(Engine::persistent(path.clone()).is_err());
        assert_eq!(fs::read(log_path(&path)).unwrap(), b"broken\n");
    }
    #[test]
    /// Un ancien nom temporaire ne peut pas servir à tronquer un autre fichier.
    fn review_journal_checkpoint_uses_an_exclusive_temporary() {
        let f = Fixture::new();
        let path = f.path();
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        let foreign = f.0.join("foreign");
        fs::write(&foreign, b"foreign contents").unwrap();
        fs::hard_link(&foreign, path.with_extension("json.tmp")).unwrap();
        let result = Writer::default().save(&path, &State::default(), true);
        assert_eq!(fs::read(&foreign).unwrap(), b"foreign contents");
        assert!(result.is_ok(), "{result:?}");
        assert!(load(&path).is_ok());
    }

    #[test]
    /// La compaction valide le WAL avant de publier un instantané ou de tronquer un fichier.
    fn review_journal_compaction_refuses_a_replaced_log() {
        for linked in [false, true] {
            let f = Fixture::new();
            let path = f.path();
            let mut writer = Writer::default();
            let mut state = State::default();
            writer.save(&path, &state, false).unwrap();
            let snapshot = fs::read(&path).unwrap();
            fs::rename(log_path(&path), f.0.join("saved-wal")).unwrap();
            let foreign = f.0.join("foreign");
            fs::write(&foreign, b"foreign contents").unwrap();
            if linked {
                fs::hard_link(&foreign, log_path(&path)).unwrap();
            } else {
                fs::rename(&foreign, log_path(&path)).unwrap();
            }
            state.paused = true;
            let result = writer.save(&path, &state, true);
            assert!(result.is_err(), "Replaced WAL accepted: linked={linked}");
            assert_eq!(fs::read(log_path(&path)).unwrap(), b"foreign contents");
            assert_eq!(fs::read(&path).unwrap(), snapshot);
        }
    }

    #[test]
    /// Une réécriture externe reste détectée même si sa taille et sa date de modification sont conservées.
    fn review_journal_refuses_snapshot_rewrite_with_preserved_mtime() {
        let f = Fixture::new();
        let path = f.path();
        let mut writer = Writer::default();
        let mut state = State::default();
        writer.save(&path, &state, false).unwrap();
        let before = fs::metadata(&path).unwrap();
        let original = fs::read_to_string(&path).unwrap();
        let changed = original.replacen(":false", ":true ", 1);
        assert_ne!(original, changed);
        assert_eq!(original.len(), changed.len());
        thread::sleep(std::time::Duration::from_millis(10));
        fs::write(&path, &changed).unwrap();
        fs::File::open(&path)
            .unwrap()
            .set_modified(before.modified().unwrap())
            .unwrap();
        state.paused = true;
        assert!(
            writer.save(&path, &state, false).is_err(),
            "Modified snapshot accepted despite changed ctime"
        );
        assert_eq!(fs::read_to_string(&path).unwrap(), changed);
        assert!(fs::read(log_path(&path)).unwrap().is_empty());
    }

    #[test]
    /// Le sous-processus borne le scénario afin qu’une ouverture de FIFO ne bloque pas toute la suite.
    fn review_journal_fifo_never_blocks_a_writer() {
        const ROOT: &str = "GAD_JOURNAL_FIFO_TEST_ROOT";
        let Some(root) = std::env::var_os(ROOT) else {
            let f = Fixture::new();
            let mut child = std::process::Command::new(std::env::current_exe().unwrap())
                .args([
                    "--exact",
                    "engine::journal::tests::review_journal_fifo_never_blocks_a_writer",
                    "--nocapture",
                ])
                .env(ROOT, &f.0)
                .stdout(std::process::Stdio::piped())
                .stderr(std::process::Stdio::piped())
                .spawn()
                .unwrap();
            let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
            let mut timed_out = false;
            while child.try_wait().unwrap().is_none() {
                if std::time::Instant::now() >= deadline {
                    timed_out = true;
                    child.kill().unwrap();
                    break;
                }
                thread::sleep(std::time::Duration::from_millis(10));
            }
            let output = child.wait_with_output().unwrap();
            assert!(
                !timed_out && output.status.success(),
                "FIFO write did not fail promptly: {}",
                String::from_utf8_lossy(&output.stderr)
            );
            return;
        };
        use std::os::unix::ffi::OsStrExt;
        unsafe extern "C" {
            fn mkfifo(path: *const std::ffi::c_char, mode: u32) -> std::ffi::c_int;
        }
        for checkpoint in [false, true] {
            let directory = PathBuf::from(&root).join(format!("case-{checkpoint}"));
            fs::create_dir(&directory).unwrap();
            // Le garde ne possède que le dossier créé ici, jamais un emplacement préexistant.
            let f = Fixture(directory);
            let path = f.path();
            let mut writer = Writer::default();
            let mut state = State::default();
            writer.save(&path, &state, false).unwrap();
            fs::remove_file(log_path(&path)).unwrap();
            let fifo = std::ffi::CString::new(log_path(&path).as_os_str().as_bytes()).unwrap();
            // SAFETY : chaîne terminée par NUL, mode Linux valide, aucun pointeur conservé par libc.
            assert_eq!(unsafe { mkfifo(fifo.as_ptr(), 0o600) }, 0);
            state.paused = true;
            assert!(writer.save(&path, &state, checkpoint).is_err());
        }
    }
}
