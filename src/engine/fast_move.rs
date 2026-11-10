//! Déplacements locaux par renommage : aucun contenu n’est relu lorsque MD5 est désactivé.
//! Une impossibilité de renommage rend la main à l’ordonnanceur avant toute copie classique.
use super::*;

/// Une entrée terminée ou à replanifier avec les réservations d’une copie ordinaire.
#[derive(Debug, PartialEq)]
pub(super) enum EntryOutcome {
    Complete,
    /// Toutes les entrées jusqu’à cet indice exclusif ont été déplacées atomiquement.
    TreeComplete(usize),
    Requeue,
}

/// Identifie le montage d’un chemin via un descripteur O_PATH sans suivre le dernier lien.
/// Les parents de destination inexistants sont remontés sans créer de fichier.
pub(super) fn mount_id(path: &Path) -> Option<u64> {
    let mut path = path;
    loop {
        match OpenOptions::new()
            .read(true)
            .custom_flags(0x200000 | 0x20000)
            .open(path)
        {
            Ok(file) => {
                return descriptor_mount_id(&file);
            }
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => path = path.parent()?,
            Err(_) => return None,
        }
    }
}

/// Lit l’identifiant du montage du descripteur déjà ouvert, sans rouvrir un chemin potentiellement modifié.
fn descriptor_mount_id(file: &fs::File) -> Option<u64> {
    use std::os::fd::AsRawFd;
    let info = fs::read_to_string(format!("/proc/self/fdinfo/{}", file.as_raw_fd())).ok()?;
    info.lines()
        .find_map(|line| line.strip_prefix("mnt_id:")?.trim().parse().ok())
}

/// KIO Fuse ne fournit pas de fsyncdir : son protocole confirme les opérations, sans durabilité POSIX du dossier.
pub(super) fn descriptor_is_kio_fuse(file: &fs::File) -> bool {
    let Some(id) = descriptor_mount_id(file) else {
        return false;
    };
    fs::read_to_string("/proc/self/mountinfo").is_ok_and(|text| is_kio_fuse_mount(&text, id))
}

/// Lecture ciblée du type du montage ; la table est toujours fournie par la lecture courante.
fn is_kio_fuse_mount(text: &str, id: u64) -> bool {
    if !text.contains("fuse.kio-fuse") {
        return false;
    }
    text.lines()
        .filter(|line| {
            line.split_whitespace()
                .next()
                .and_then(|field| field.parse::<u64>().ok())
                == Some(id)
        })
        .any(|line| {
            let mut fields = line.split_whitespace();
            // Un montage local n'a besoin d'aucune allocation, même si un autre montage est KIO.
            if fields
                .find(|field| *field == "-")
                .and_then(|_| fields.next())
                != Some("fuse.kio-fuse")
            {
                return false;
            }
            // Conserver exactement la validation des enregistrements du parseur complet.
            parse_mounts(line)
                .iter()
                .any(|mount| mount.id == id && mount.kind == "fuse.kio-fuse")
        })
}

/// Description d’un montage dans l’espace de noms du processus.
#[derive(Clone)]
pub(super) struct Mount {
    pub(super) id: u64,
    pub(super) device: String,
    pub(super) root: PathBuf,
    pub(super) point: PathBuf,
    pub(super) kind: String,
    /// Le détour par un alias ne doit jamais contourner un montage original en lecture seule.
    writable: bool,
}

/// Décode uniquement les échappements octaux définis pour les chemins de mountinfo.
fn mount_path(value: &str) -> PathBuf {
    value
        .replace("\\040", " ")
        .replace("\\011", "\t")
        .replace("\\012", "\n")
        .replace("\\134", "\\")
        .into()
}

/// Une ligne mal formée est ignorée ; son identifiant ne pourra donc autoriser aucun renommage.
pub(super) fn parse_mounts(text: &str) -> Vec<Mount> {
    text.lines()
        .filter_map(|line| {
            let fields: Vec<_> = line.split_whitespace().collect();
            let separator = fields.iter().position(|field| *field == "-")?;
            Some(Mount {
                id: fields.first()?.parse().ok()?,
                device: fields.get(2)?.to_string(),
                root: mount_path(fields.get(3)?),
                point: mount_path(fields.get(4)?),
                kind: fields.get(separator + 1)?.to_string(),
                writable: fields.get(5)?.split(',').any(|option| option == "rw")
                    && !fields
                        .get(separator + 3)?
                        .split(',')
                        .any(|option| option == "ro"),
            })
        })
        .collect()
}

/// Exclut les réseaux, FUSE et les systèmes pouvant recopier le contenu lors d’un renommage (overlay).
fn local_mount(id: u64, mounts: &[Mount]) -> bool {
    mounts
        .iter()
        .find(|mount| mount.id == id)
        .is_some_and(|mount| {
            matches!(
                mount.kind.as_str(),
                "ext2"
                    | "ext3"
                    | "ext4"
                    | "xfs"
                    | "btrfs"
                    | "f2fs"
                    | "jfs"
                    | "reiserfs"
                    | "tmpfs"
                    | "ramfs"
                    | "zfs"
            )
        })
}

/// Deux chemins bind-montés vers un même sous-arbre reçoivent la même clé.
/// La clé utilise le périphérique et la racine interne du montage, pas son point d’accès visible.
fn resource(mount: &Mount, path: &Path) -> Option<PathBuf> {
    let relative = path.strip_prefix(&mount.point).ok()?;
    Some(
        PathBuf::from(format!("/device-{}", mount.device))
            .join(mount.root.strip_prefix("/").ok()?)
            .join(relative),
    )
}

/// Position interne dans le système de fichiers, indépendante du chemin bind utilisé.
fn underlying_path(mount: &Mount, path: &Path) -> Option<PathBuf> {
    Some(mount.root.join(path.strip_prefix(&mount.point).ok()?))
}

/// Compare aussi les parents d’une destination future : une vue masquée par un sous-montage
/// ou un alias remonté vers un autre objet ne peut pas devenir un chemin de renommage.
pub(super) fn same_location(a: &Path, b: &Path) -> bool {
    let (mut a, mut b) = (a, b);
    let mut leaf = true;
    loop {
        // Le dernier lien est l’objet déplacé ; un parent est en revanche le dossier réellement traversé.
        let read = |path: &Path| {
            if leaf {
                fs::symlink_metadata(path)
            } else {
                fs::metadata(path)
            }
        };
        match (read(a), read(b)) {
            (Ok(a), Ok(b)) => {
                return (a.dev(), a.ino(), a.file_type()) == (b.dev(), b.ino(), b.file_type())
            }
            (Err(a_error), Err(b_error))
                if a_error.kind() == std::io::ErrorKind::NotFound
                    && b_error.kind() == std::io::ErrorKind::NotFound =>
            {
                if a.file_name() != b.file_name() {
                    return false;
                }
                let (Some(a_parent), Some(b_parent)) = (a.parent(), b.parent()) else {
                    return false;
                };
                a = a_parent;
                b = b_parent;
                leaf = false;
            }
            _ => return false,
        }
    }
}

/// Construit les chemins via un montage qui expose les deux emplacements internes.
/// La table seule ne suffit pas : les identifiants de montage et les inodes sont revérifiés.
fn rename_route_using(
    source: &Path,
    destination: &Path,
    mounts: &[Mount],
) -> Option<(PathBuf, PathBuf)> {
    let source_id = mount_id(source)?;
    let destination_id = mount_id(destination)?;
    rename_route_ids(source, destination, mounts, source_id, destination_id)
}
/// Réutilise les identifiants déjà sondés pendant une seule préparation.
fn rename_route_ids(
    source: &Path,
    destination: &Path,
    mounts: &[Mount],
    source_id: u64,
    destination_id: u64,
) -> Option<(PathBuf, PathBuf)> {
    if !local_mount(source_id, mounts) || !local_mount(destination_id, mounts) {
        return None;
    }
    if !mounts
        .iter()
        .filter(|mount| mount.id == source_id || mount.id == destination_id)
        .all(|mount| mount.writable)
    {
        return None;
    }
    if source_id == destination_id {
        return Some((source.to_owned(), destination.to_owned()));
    }
    let source_mount = mounts.iter().find(|mount| mount.id == source_id)?;
    let destination_mount = mounts.iter().find(|mount| mount.id == destination_id)?;
    for (candidate_id, from, to) in route_candidates(
        source_mount,
        destination_mount,
        &normalized_root(source)?,
        &normalized_root(destination)?,
        mounts,
    ) {
        if mount_id(&from) == Some(candidate_id)
            && mount_id(&to) == Some(candidate_id)
            && same_location(source, &from)
            && same_location(destination, &to)
        {
            return Some((from, to));
        }
    }
    None
}

/// Partie pure de la résolution : traduit deux vues bind vers une vue commune locale et accessible en écriture.
fn route_candidates(
    source_mount: &Mount,
    destination_mount: &Mount,
    source: &Path,
    destination: &Path,
    mounts: &[Mount],
) -> Vec<(u64, PathBuf, PathBuf)> {
    if source_mount.device != destination_mount.device
        || !source_mount.writable
        || !destination_mount.writable
    {
        return Vec::new();
    }
    let Some(source_internal) = underlying_path(source_mount, source) else {
        return Vec::new();
    };
    let Some(destination_internal) = underlying_path(destination_mount, destination) else {
        return Vec::new();
    };
    let mut common: Vec<_> = mounts
        .iter()
        .filter(|mount| {
            mount.device == source_mount.device
                && mount.writable
                && local_mount(mount.id, mounts)
                && source_internal.starts_with(&mount.root)
                && destination_internal.starts_with(&mount.root)
        })
        .collect();
    // Préférer la vue commune la plus précise ; un second alias reste utilisable si la première est masquée.
    common.sort_by_key(|mount| std::cmp::Reverse(mount.root.components().count()));
    common
        .into_iter()
        .filter_map(|mount| {
            Some((
                mount.id,
                mount
                    .point
                    .join(source_internal.strip_prefix(&mount.root).ok()?),
                mount
                    .point
                    .join(destination_internal.strip_prefix(&mount.root).ok()?),
            ))
        })
        .collect()
}

/// Un dossier contenant un point de montage doit rester traité entrée par entrée.
/// Contrôler aussi la vue commune évite de déplacer un sous-montage masqué dans la vue bind.
fn tree_without_mounts(source: &Path, routed: &Path, mounts: &[Mount]) -> bool {
    let Some(source) = normalized_root(source) else {
        return false;
    };
    !mounts
        .iter()
        .any(|mount| mount.point.starts_with(&source) || mount.point.starts_with(routed))
}

/// Autorise uniquement les dossiers ordinaires sans sous-montage dans les deux vues.
pub(super) fn tree_possible(source: &Path, destination: &Path) -> bool {
    let Ok(text) = fs::read_to_string("/proc/self/mountinfo") else {
        return false;
    };
    let mounts = parse_mounts(&text);
    rename_route_using(source, destination, &mounts)
        .is_some_and(|(from, _)| tree_without_mounts(source, &from, &mounts))
}

/// Réévalue les montages au dernier moment et conserve la publication sans écrasement.
/// EXDEV signale à l’ordonnanceur qu’il faut libérer la réservation rapide avant une recopie.
pub(super) fn rename_local(source: &Path, destination: &Path) -> std::io::Result<()> {
    let text = fs::read_to_string("/proc/self/mountinfo")?;
    let mounts = parse_mounts(&text);
    let (from, to) = rename_route_using(source, destination, &mounts)
        .ok_or_else(|| std::io::Error::from_raw_os_error(18))?;
    if fs::symlink_metadata(source)?.is_dir() && !tree_without_mounts(source, &from, &mounts) {
        return Err(std::io::Error::from_raw_os_error(18));
    }
    safe_parents(&from)
        .and_then(|()| safe_parents(&to))
        .map_err(std::io::Error::other)?;
    publish_new(&from, &to)
}

/// Détecte une copie sur soi-même ou dans soi-même même à travers deux bind mounts.
/// Une information absente laisse les validations historiques faire leur travail habituel.
pub(super) fn invalid_alias(source: &Path, destination: &Path, directory: bool) -> bool {
    let Some((source, destination)) = (|| {
        let text = fs::read_to_string("/proc/self/mountinfo").ok()?;
        let mounts = parse_mounts(&text);
        let key = |path: &Path| {
            let id = mount_id(path)?;
            resource(
                mounts.iter().find(|mount| mount.id == id)?,
                &normalized_root(path)?,
            )
        };
        Some((key(source)?, key(destination)?))
    })() else {
        return false;
    };
    source == destination || (directory && destination.starts_with(&source))
}

/// Cache éphémère des parents source ; les contrôles de publication restent indépendants.
fn preparation_parents(
    path: &Path,
    checked: &mut std::collections::HashSet<PathBuf>,
) -> Result<(), String> {
    let mut fresh = Vec::new();
    for parent in path.ancestors().skip(1) {
        if checked.contains(parent) {
            break;
        }
        let meta = fs::symlink_metadata(parent).map_err(reconnect::io_error)?;
        if !meta.is_dir() || meta.file_type().is_symlink() {
            return Err("Unsafe source parent".into());
        }
        fresh.push(parent.to_owned());
    }
    checked.extend(fresh);
    Ok(())
}
/// Une destination absente reprend le montage de son premier parent existant.
/// Les liens et chemins existants restent sondés directement, notamment les montages de fichiers.
fn preparation_mount(
    path: &Path,
    cache: &mut std::collections::HashMap<PathBuf, u64>,
) -> Option<u64> {
    if let Some(id) = cache.get(path) {
        return Some(*id);
    }
    let id = match fs::symlink_metadata(path) {
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            preparation_mount(path.parent()?, cache)?
        }
        Err(_) => return None,
        Ok(_) => mount_id(path)?,
    };
    cache.insert(path.to_owned(), id);
    Some(id)
}

/// Tous les parents, y compris les montages imbriqués, doivent permettre un déplacement local.
/// Une preuve absente laisse le lot dans le parcours classique, sans modifier les garanties MD5.
pub(super) fn eligible(entries: &[Entry], moving: bool, verify_md5: bool) -> bool {
    if !moving || verify_md5 || entries.is_empty() {
        return false;
    }
    let Ok(mounts) = fs::read_to_string("/proc/self/mountinfo") else {
        return false;
    };
    let mounts = parse_mounts(&mounts);
    let mut checked = std::collections::BTreeSet::new();
    let mut parents = std::collections::HashSet::new();
    let mut mount_cache = std::collections::HashMap::new();
    let mut targets = std::collections::BTreeSet::new();
    for entry in entries {
        if entry.hidden
            || entry.cancel
            || terminal(&entry.state)
            || entry.state == TransferState::Skipped
        {
            continue;
        }
        if entry
            .undo
            .as_ref()
            .is_some_and(|record| record.resume.is_some())
        {
            return false;
        }
        // Le parcours rapide exige des parents stables sans alias symbolique.
        if preparation_parents(&entry.source, &mut parents).is_err() {
            return false;
        }
        // Le renommage d’un autre lien physique changerait aussi le ctime du résultat suivi.
        // Garder la copie classique pour ces inodes évite de rendre leur restauration ambiguë.
        if fs::symlink_metadata(&entry.source)
            .map_or(true, |meta| meta.is_file() && meta.nlink() > 1)
        {
            return false;
        }
        // Fusion, remplacement et noms homonymes gardent le parcours classique et ses empreintes de contenu.
        if fs::symlink_metadata(&entry.destination).is_ok() || !targets.insert(&entry.destination) {
            return false;
        }
        let Some(source) = preparation_mount(&entry.source, &mut mount_cache) else {
            return false;
        };
        let Some(destination) = preparation_mount(&entry.destination, &mut mount_cache) else {
            return false;
        };
        if rename_route_ids(
            &entry.source,
            &entry.destination,
            &mounts,
            source,
            destination,
        )
        .is_none()
        {
            return false;
        }
        if checked.insert(source) && !local_mount(source, &mounts) {
            return false;
        }
    }
    !checked.is_empty()
}

/// Réserve les éléments réellement lus/écrits, et non leur dossier de destination commun.
/// Les dossiers du lot couvrent leurs descendants ; deux fichiers frères restent indépendants.
pub(super) fn reservations(entries: &[Entry]) -> Option<Vec<PathBuf>> {
    paths_reservations(
        entries
            .iter()
            .flat_map(|entry| [entry.source.clone(), entry.destination.clone()]),
    )
}

/// Clés de destination par entrée : réutilise les parents pour reconnaître les alias bind sans suivre le nom final.
/// Ce calcul peut accéder aux montages et doit rester dans les travailleurs de préparation.
pub(super) fn destination_keys(paths: impl Iterator<Item = PathBuf>) -> Option<Vec<PathBuf>> {
    let mounts = parse_mounts(&fs::read_to_string("/proc/self/mountinfo").ok()?);
    let mut parents = std::collections::HashMap::<PathBuf, PathBuf>::new();
    let mut result = Vec::new();
    for path in paths {
        let parent = path.parent()?;
        let base = if let Some(key) = parents.get(parent) {
            key.clone()
        } else {
            let normalized = normalized_root(&parent.join(".gad-layout-probe"))?;
            let normalized = normalized.parent()?;
            let id = mount_id(normalized)?;
            let mount = mounts.iter().find(|m| m.id == id)?;
            let key = resource(mount, normalized)?;
            parents.insert(parent.to_path_buf(), key.clone());
            key
        };
        result.push(base.join(path.file_name()?));
    }
    Some(result)
}

/// Normalise un chemin futur sans suivre son dernier lien ; ses parents peuvent ne pas encore exister.
fn normalized_root(path: &Path) -> Option<PathBuf> {
    let mut parent = path.parent()?;
    loop {
        match fs::canonicalize(parent) {
            Ok(real) => return Some(real.join(path.strip_prefix(parent).ok()?)),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
                parent = parent.parent()?
            }
            Err(_) => return None,
        }
    }
}

/// Élimine les descendants déjà protégés par une racine de dossier.
fn compact_paths(paths: std::collections::BTreeSet<PathBuf>) -> Vec<PathBuf> {
    let mut compact: Vec<PathBuf> = Vec::new();
    for path in paths {
        if compact
            .last()
            .is_none_or(|parent| !path.starts_with(parent))
        {
            compact.push(path);
        }
    }
    compact
}

/// Traduit les racines et sous-montages en clés internes communes aux alias bind.
fn paths_reservations(paths: impl Iterator<Item = PathBuf>) -> Option<Vec<PathBuf>> {
    let mounts = parse_mounts(&fs::read_to_string("/proc/self/mountinfo").ok()?);
    let roots = compact_paths(paths.collect());
    let mut resources = std::collections::BTreeSet::new();
    for root in roots {
        let root = normalized_root(&root)?;
        let id = mount_id(&root)?;
        let mount = mounts.iter().find(|mount| mount.id == id)?;
        resources.insert(resource(mount, &root)?);
        // Un sous-montage se réserve dans son propre système de fichiers, pas seulement dans celui du parent.
        for nested in mounts
            .iter()
            .filter(|nested| nested.point != root && nested.point.starts_with(&root))
        {
            resources.insert(resource(nested, &nested.point)?);
        }
    }
    Some(compact_paths(resources))
}

/// Synchronise les deux noms, même si les fichiers n’ont pas changé de support.
pub(super) fn sync_names(source: &Path, destination: &Path) -> Result<(), String> {
    for path in [source, destination] {
        fs::File::open(path.parent().ok_or("Parent absent")?)
            .and_then(|parent| parent.sync_all())
            .map_err(|error| error.to_string())?;
    }
    Ok(())
}

impl Engine {
    /// Un choix « Garder les deux » peut écrire un nom absent de l’analyse initiale.
    /// Réserver ce nom atomiquement avant de créer le temporaire empêche qu’un déplacement
    /// rapide déjà actif ou ajouté ensuite travaille sur cette nouvelle destination.
    pub(super) fn reserve_destination(
        &self,
        id: u64,
        index: usize,
        path: &Path,
    ) -> Result<(), String> {
        let added = paths_reservations(std::iter::once(path.to_owned()));
        let mut state = self.shared.state.lock().unwrap();
        loop {
            let job = state
                .jobs
                .iter()
                .find(|job| job.id == id)
                .ok_or("Operation absent")?;
            if job.cancel || job.entries.get(index).is_none_or(|entry| entry.cancel) {
                return Err("Canceled".into());
            }
            // Une réservation globale inconnue interdisait déjà tout voisin rapide.
            if job.paths.is_none() {
                return Ok(());
            }
            let blocked = state
                .active
                .keys()
                .filter(|active| **active != id)
                .any(|active| {
                    let other = state.jobs.iter().find(|job| job.id == *active);
                    match (added.as_ref(), other.and_then(|job| job.paths.as_ref())) {
                        (Some(added), Some(paths)) => added
                            .iter()
                            .any(|a| paths.iter().any(|b| a.starts_with(b) || b.starts_with(a))),
                        _ => true,
                    }
                });
            if !blocked {
                let job = state.jobs.iter_mut().find(|job| job.id == id).unwrap();
                job.paths = added.as_ref().map(|added| {
                    compact_paths(
                        job.paths
                            .as_ref()
                            .unwrap()
                            .iter()
                            .chain(added)
                            .cloned()
                            .collect(),
                    )
                });
                return Ok(());
            }
            state = self.shared.wake.wait(state).unwrap();
        }
    }
    /// Revient dans la file avant toute lecture de contenu lorsque le renommage n’est plus applicable.
    fn requeue_move(&self, id: u64, index: usize) -> Result<EntryOutcome, String> {
        self.update(id, |job| {
            job.fast_move = false;
            job.entries[index].state = TransferState::Waiting;
        });
        self.save()?;
        Ok(EntryOutcome::Requeue)
    }

    /// Journalise le retrait prévu avant le renommage atomique sans écrasement.
    pub(super) fn rename_entry(
        &self,
        id: u64,
        index: usize,
        entry: &Entry,
    ) -> Result<EntryOutcome, String> {
        self.rename_entry_using(id, index, entry, rename_local)
    }

    /// La primitive injectable sert à reproduire EXDEV sans changer les montages de la machine de test.
    pub(super) fn rename_entry_using(
        &self,
        id: u64,
        index: usize,
        entry: &Entry,
        rename: impl FnOnce(&Path, &Path) -> std::io::Result<()>,
    ) -> Result<EntryOutcome, String> {
        self.checkpoint_entry(id, index)?;
        safe_parents(&entry.source)?;
        safe_parents(&entry.destination)?;
        if !eligible(std::slice::from_ref(entry), true, false) {
            return self.requeue_move(id, index);
        }
        let meta = fs::symlink_metadata(&entry.source).map_err(|e| e.to_string())?;
        if (meta.dev(), meta.ino()) != entry.identity
            || source_kind(&meta) != Some(entry.kind.as_str())
        {
            return Err("Source was replaced; kept".into());
        }
        let stamp = undo::Stamp::read(&entry.source)?;
        let mut record = undo::Record {
            rename_move: true,
            source_stamp: Some(stamp.clone()),
            result: Some(stamp.clone()),
            source_removed: true,
            ..Default::default()
        };
        self.record(id, index, record.clone())?;
        self.checkpoint_entry(id, index)?;
        if !stamp.matches(&entry.source) {
            return Err("Source changed before the move; kept".into());
        }
        match rename(&entry.source, &entry.destination) {
            Ok(()) => {
                sync_names(&entry.source, &entry.destination)?;
                let renamed =
                    fs::symlink_metadata(&entry.destination).map_err(|e| e.to_string())?;
                record.rename_ctime = Some((renamed.ctime(), renamed.ctime_nsec()));
                self.record(id, index, record)?;
                self.verify_fast_result(
                    id,
                    index,
                    &[fast_verify::Expected {
                        entry,
                        stamp: &stamp,
                        ctime: None,
                    }],
                )?;
                self.update(id, |job| {
                    job.entries[index].size = if entry.kind == "file" { meta.len() } else { 0 };
                    job.entries[index].copied = job.entries[index].size;
                    job.entries[index].state = TransferState::Done;
                });
                self.save()?;
                Ok(EntryOutcome::Complete)
            }
            Err(error)
                if error.raw_os_error() == Some(18)
                    || error.kind() == std::io::ErrorKind::Unsupported =>
            {
                // Ne jamais commencer la copie de secours tant que la réservation rapide est active.
                self.update(id, |job| job.entries[index].undo = None);
                self.requeue_move(id, index)
            }
            Err(error) => Err(format!(
                "Déplacement par renommage impossible ; source conservée : {error}"
            )),
        }
    }

    /// Avant qu’une copie classique déplace un ancien résultat rapide vers sa sauvegarde,
    /// matérialise son empreinte de contenu. Ce travail ne se produit jamais sous réservation rapide.
    pub(super) fn protect_fast_result(
        &self,
        id: u64,
        index: usize,
        path: &Path,
    ) -> Result<(), String> {
        let meta = fs::symlink_metadata(path).map_err(|error| error.to_string())?;
        if !meta.is_file() {
            return Ok(());
        }
        let mut records: Vec<_> = {
            let state = self.shared.state.lock().unwrap();
            state
                .jobs
                .iter()
                .flat_map(|job| {
                    job.entries.iter().enumerate().filter_map(|(index, entry)| {
                        let record = entry.undo.as_ref()?;
                        (record.rename_move
                            && !record.reverted
                            && record.digest.is_none()
                            && record.rename_ctime == Some((meta.ctime(), meta.ctime_nsec())))
                        .then(|| (job.id, index, record.clone()))
                    })
                })
                .collect()
        };
        // Le contrôle de fichier se fait hors du mutex partagé de l’interface.
        records.retain(|(_, _, record)| {
            record
                .result
                .as_ref()
                .is_some_and(|stamp| stamp.matches(path))
        });
        if records.is_empty() {
            return Ok(());
        }
        let digest = md5_file(path, Some(&meta), |_| self.checkpoint_entry(id, index))?;
        let now = fs::symlink_metadata(path).map_err(|error| error.to_string())?;
        if (now.ctime(), now.ctime_nsec()) != (meta.ctime(), meta.ctime_nsec()) {
            return Err("Destination changed during preparation; kept".into());
        }
        for (job, entry, mut record) in records {
            record.digest = Some(digest.clone());
            self.record(job, entry, record)?;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn targeted_kio_detection_matches_full_parser_including_malformed_lines() {
        let valid = "42 0 8:1 / /phone rw,relatime - fuse.kio-fuse source rw";
        let mut samples = vec![
            String::new(),
            valid.to_owned(),
            valid.replace(' ', "\t"),
            valid.replace("fuse.kio-fuse", "fuse.kio-fuse-extra"),
            valid.replace("fuse.kio-fuse", "fuse.sshfs"),
            valid.replace("42 ", "4200 "),
            valid.replace("42 ", "invalid "),
        ];
        let local = "42 0 8:1 / /fuse.kio-fuse rw - ext4 source rw";
        samples.push(local.to_owned());
        samples.push(format!("{local}\n{valid}\n"));
        samples.push(format!("{valid}\n{local}\n"));
        let fields = valid.split_whitespace().collect::<Vec<_>>();
        for removed in 0..fields.len() {
            samples.push(
                fields
                    .iter()
                    .enumerate()
                    .filter(|(index, _)| *index != removed)
                    .map(|(_, value)| *value)
                    .collect::<Vec<_>>()
                    .join(" "),
            );
        }
        for text in samples {
            for id in [0, 1, 42, 128, 4200, u64::MAX] {
                let expected = parse_mounts(&text)
                    .iter()
                    .any(|mount| mount.id == id && mount.kind == "fuse.kio-fuse");
                assert_eq!(is_kio_fuse_mount(&text, id), expected, "id={id} {text:?}");
            }
        }
    }
    #[test]
    #[ignore = "benchmark explicite de l'identification KIO Fuse"]
    fn benchmark_kio_mount_detection() {
        for scenario in ["local", "local-with-fuse", "kio"] {
            let text = (1..=128)
                .map(|id| {
                    let kind = if id == 128 && scenario != "local" {
                        "fuse.kio-fuse"
                    } else {
                        "ext4"
                    };
                    format!("{id} 0 8:1 / /mount/{id} rw,relatime - {kind} /dev/example rw\n")
                })
                .collect::<String>();
            let id = if scenario == "kio" { 128 } else { 3 };
            assert_eq!(is_kio_fuse_mount(&text, id), scenario == "kio");
            for trial in 0..3 {
                let start = std::time::Instant::now();
                for _ in 0..1000 {
                    std::hint::black_box(is_kio_fuse_mount(std::hint::black_box(&text), id));
                }
                println!(
                    "kio_mount_detection scenario={scenario} trial={trial} elapsed_ms={:.3}",
                    start.elapsed().as_secs_f64() * 1000.0
                );
            }
        }
    }
    #[test]
    /// Les montages locaux restent admissibles ; FUSE, réseau, overlay et identifiants inconnus sont refusés.
    fn fast_mount_rejects_network_fuse_overlay_and_unknown_ids() {
        let local = "10 1 8:2 / /data rw - ext4 /dev/sda2 rw\n";
        assert!(local_mount(10, &parse_mounts(local)));
        assert!(!local_mount(99, &parse_mounts(local)));
        let alias = format!("{local}11 1 8:2 /documents /alias rw - ext4 /dev/sda2 rw\n");
        assert!(local_mount(10, &parse_mounts(&alias)));
        for filesystem in ["fuse.sshfs", "nfs", "cifs", "overlay", "unknown"] {
            assert!(!local_mount(
                10,
                &parse_mounts(&local.replace("ext4", filesystem))
            ));
        }
        assert!(!local_mount(10, &parse_mounts("10 malformed")));
    }

    #[test]
    /// Les alias, espaces, barres obliques et sous-montages partagent des clés cohérentes par composantes.
    fn fast_mount_resource_keys_collapse_bind_aliases_and_keep_devices_separate() {
        let mounts = parse_mounts("10 1 8:2 / /volume rw - ext4 /dev/sda2 rw\n11 1 8:2 /documents /alias\\040documents rw - ext4 /dev/sda2 rw\n12 1 8:3 / /volume/documents/usb rw - ext4 /dev/sdb1 rw\n");
        assert_eq!(
            resource(&mounts[0], Path::new("/volume/documents/file.txt")),
            resource(&mounts[1], Path::new("/alias documents/file.txt"))
        );
        assert_ne!(
            resource(&mounts[0], Path::new("/volume/file.txt")),
            resource(&mounts[2], Path::new("/volume/documents/usb/file.txt"))
        );
        assert!(resource(&mounts[1], Path::new("/alias documents-other/file.txt")).is_none());
        assert_eq!(mount_path(r"/a\040b\134040"), PathBuf::from(r"/a b\040"));
    }
    /// Les lignes représentent les trois vues du fstab, sans créer de montage dans les tests.
    fn bind_mounts() -> Vec<Mount> {
        parse_mounts("92 1 8:18 / /media/SeizeTera rw - ext4 /dev/sdb2 rw\n94 1 8:18 /Autres /home/demo/Autres rw - ext4 /dev/sdb2 rw\n98 1 8:18 /Musiques /home/demo/Musiques rw - ext4 /dev/sdb2 rw\n")
    }

    #[test]
    /// Deux bind mounts distincts utilisent leurs vrais sous-arbres dans la vue commune du même ext4.
    fn fast_bind_mounts_resolve_through_common_mount_in_both_directions() {
        let mounts = bind_mounts();
        let source = Path::new("/home/demo/Musiques/Album avec espaces/piste #1.flac");
        let destination = Path::new("/home/demo/Autres/Archives/piste #1.flac");
        let expected = (
            92,
            PathBuf::from("/media/SeizeTera/Musiques/Album avec espaces/piste #1.flac"),
            PathBuf::from("/media/SeizeTera/Autres/Archives/piste #1.flac"),
        );
        assert_eq!(
            route_candidates(&mounts[2], &mounts[1], source, destination, &mounts),
            vec![expected.clone()]
        );
        assert_eq!(
            route_candidates(&mounts[1], &mounts[2], destination, source, &mounts),
            vec![(expected.0, expected.2, expected.1)]
        );
    }

    #[test]
    /// Un même numéro de disque ne suffit pas sans vue commune de la bonne partition.
    fn fast_bind_mounts_require_common_local_mount_and_same_device() {
        let mounts = bind_mounts();
        let source = Path::new("/home/demo/Musiques/a");
        let destination = Path::new("/home/demo/Autres/a");
        assert!(
            route_candidates(&mounts[2], &mounts[1], source, destination, &mounts[1..]).is_empty()
        );
        let mut other = mounts[1].clone();
        other.device = "8:19".into();
        assert!(route_candidates(&mounts[2], &other, source, destination, &mounts).is_empty());
        let mut network = mounts.clone();
        network[0].kind = "fuse.sshfs".into();
        assert!(
            route_candidates(&network[2], &network[1], source, destination, &network).is_empty()
        );
        assert!(route_candidates(
            &mounts[2],
            &mounts[1],
            Path::new("/home/demo/Musiques-other/a"),
            destination,
            &mounts
        )
        .is_empty());
    }

    #[test]
    /// Aucun détour ne doit contourner un montage d’origine ou une vue commune en lecture seule.
    fn fast_bind_mounts_preserve_read_only_mount_restrictions() {
        let mounts = bind_mounts();
        let source = Path::new("/home/demo/Musiques/a");
        let destination = Path::new("/home/demo/Autres/a");
        for index in 0..mounts.len() {
            let mut readonly = mounts.clone();
            readonly[index].writable = false;
            assert!(
                route_candidates(&readonly[2], &readonly[1], source, destination, &readonly)
                    .is_empty()
            );
        }
        let parsed=parse_mounts("1 0 8:1 / /readonly ro - ext4 /dev/sda1 rw\n2 0 8:2 / /super-ro rw - ext4 /dev/sda2 ro\n");
        assert!(parsed.iter().all(|mount| !mount.writable));
    }
    #[test]
    /// Ni un point de montage source ni un sous-montage caché dans la vue commune ne doit être déplacé en bloc.
    fn fast_tree_rejects_mount_roots_and_nested_mounts_in_either_view() {
        let parent = std::env::temp_dir().canonicalize().unwrap();
        let source = parent.join("gad-tree-mount-plan");
        let routed = PathBuf::from("/common/gad-tree-mount-plan");
        let mut mounts = Vec::new();
        assert!(tree_without_mounts(&source, &routed, &mounts));
        for point in [source.clone(), source.join("usb"), routed.join("hidden")] {
            mounts.push(Mount {
                id: 1,
                device: "8:2".into(),
                root: "/".into(),
                point,
                kind: "ext4".into(),
                writable: true,
            });
            assert!(!tree_without_mounts(&source, &routed, &mounts));
            mounts.clear();
        }
    }
}

#[cfg(test)]
mod preparation_tests {
    use super::*;
    /// Les parents absents partagent la résolution ; une nouvelle préparation relit les changements.
    #[test]
    fn mount_cache_is_scoped_and_handles_missing_paths_and_links() {
        struct Cleanup(PathBuf);
        impl Drop for Cleanup {
            fn drop(&mut self) {
                let _ = fs::remove_dir_all(&self.0);
            }
        }
        let f = Cleanup(std::env::temp_dir().join(format!(
            "gad-probe-{}-{}",
            std::process::id(),
            snapshot::touch()
        )));
        fs::create_dir_all(f.0.join("source/sub")).unwrap();
        fs::create_dir(f.0.join("dest")).unwrap();
        let mut cache = std::collections::HashMap::new();
        let missing = f.0.join("dest/new/sub/file");
        assert_eq!(preparation_mount(&missing, &mut cache), mount_id(&missing));
        assert!(cache.contains_key(&f.0.join("dest")));
        symlink(f.0.join("source"), f.0.join("dest/link")).unwrap();
        let link = f.0.join("dest/link");
        assert_eq!(preparation_mount(&link, &mut cache), mount_id(&link));
        let mut parents = std::collections::HashSet::new();
        assert!(preparation_parents(&f.0.join("source/sub/child"), &mut parents).is_ok());
        assert!(preparation_parents(&link.join("child"), &mut parents).is_err());
        fs::rename(f.0.join("source/sub"), f.0.join("source/old")).unwrap();
        symlink("old", f.0.join("source/sub")).unwrap();
        assert!(preparation_parents(
            &f.0.join("source/sub/child"),
            &mut std::collections::HashSet::new()
        )
        .is_err());
    }
}
