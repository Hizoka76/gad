//! Primitives de restauration et contrôle d’appartenance des fichiers.
//! Un chemin seul ne suffit pas : les empreintes d’identité et, pour les résultats
//! ordinaires, le contenu permettent de refuser la suppression d’un fichier modifié.

use super::*;
use serde::{Deserialize, Serialize};
use std::os::unix::fs::{DirBuilderExt, PermissionsExt};
use std::time::{Duration, SystemTime};

#[derive(Clone, Debug, Serialize, Deserialize)]
/// Empreinte d’un chemin : identité de fichier et métadonnées utiles aux contrôles de restauration.
pub(super) struct Stamp {
    /// Identifiant du périphérique hébergeant l’élément.
    dev: u64,
    /// Inode, à interpréter avec dev pour identifier le fichier.
    ino: u64,
    /// Taille observée ; elle peut évoluer légitimement pour un dossier.
    len: u64,
    /// Partie entière de la date de modification Unix.
    seconds: i64,
    /// Précision nanoseconde de la date de modification.
    nanos: i64,
    /// Permissions Unix et bits de type observés.
    mode: u32,
    /// Propriétaire et groupe ; absents des anciens journaux, qui restent lisibles.
    #[serde(default)]
    owner: Option<(u32, u32)>,
    /// Type attendu : fichier, dossier ou lien symbolique.
    kind: String,
    /// Texte de la cible si l’élément est un lien symbolique.
    link: Option<PathBuf>,
}
impl Stamp {
    /// Ne change que le numéro de périphérique après preuve d’un UUID de volume identique.
    pub(super) fn rebind(&mut self, devices: &std::collections::HashMap<u64, u64>) {
        if let Some(device) = devices.get(&self.dev) {
            self.dev = *device;
        }
    }
    /// Capture l’identité et les métadonnées du chemin sans suivre un lien symbolique.
    pub(super) fn read(path: &Path) -> Result<Self, String> {
        let m = fs::symlink_metadata(path)
            .map_err(|e| format!("{} : {}", path.display(), reconnect::io_error(e)))?;
        Ok(Self {
            dev: m.dev(),
            ino: m.ino(),
            len: m.len(),
            seconds: m.mtime(),
            nanos: m.mtime_nsec(),
            mode: m.mode(),
            owner: Some((m.uid(), m.gid())),
            kind: if m.is_dir() {
                "directory"
            } else if m.file_type().is_symlink() {
                "link"
            } else {
                "file"
            }
            .into(),
            link: if m.file_type().is_symlink() {
                Some(fs::read_link(path).map_err(reconnect::io_error)?)
            } else {
                None
            },
        })
    }
    /// Identité seule des temporaires, dont les taille et dates changent pendant la copie.
    pub(super) fn owns_metadata(&self, meta: &fs::Metadata) -> bool {
        meta.dev() == self.dev
            && meta.ino() == self.ino
            && source_kind(meta) == Some(self.kind.as_str())
    }
    pub(super) fn same_inode(&self, path: &Path) -> bool {
        fs::symlink_metadata(path).is_ok_and(|meta| self.owns_metadata(&meta))
    }
    /// Vérifie qu’un chemin correspond à l’empreinte. Pour un dossier, seuls
    /// l’identité et le type sont stables lorsque son contenu change.
    pub(super) fn matches(&self, path: &Path) -> bool {
        Self::read(path).is_ok_and(|m| {
            self.dev == m.dev
                && self.ino == m.ino
                && self.kind == m.kind
                && (self.kind == "directory"
                    || (self.len == m.len
                        && self.mode == m.mode
                        && self.owner.is_none_or(|owner| m.owner == Some(owner))
                        && self.seconds == m.seconds
                        && self.nanos == m.nanos
                        && self.link == m.link))
        })
    }
    /// Vérifie le périphérique d’un parent existant pour ne pas restaurer dans
    /// le point de montage vide d’un volume qui aurait disparu.
    fn volume(&self, path: &Path) -> Result<(), String> {
        let mut parent = path.parent();
        while let Some(p) = parent {
            match fs::symlink_metadata(p) {
                Ok(meta) => {
                    if !meta.is_dir() || meta.file_type().is_symlink() {
                        return Err(format!(
                            "Dossier déplacé ou remplacé, conservé : {}",
                            p.display()
                        ));
                    }
                    return if meta.dev() == self.dev {
                        Ok(())
                    } else {
                        Err(format!("Le volume d’origine n’est pas disponible au chemin {}. Remontez le volume avant de restaurer",p.display()))
                    };
                }
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => parent = p.parent(),
                Err(error) => return Err(error.to_string()),
            }
        }
        Err("Volume du transfert introuvable".into())
    }
    /// Rétablit les permissions et la date prises en charge, hors liens symboliques.
    fn metadata(&self, path: &Path) -> Result<(), String> {
        if self.kind == "link" {
            return Ok(());
        }
        // O_PATH permet d’identifier le montage même si les permissions à restaurer interdisent la lecture.
        let file = OpenOptions::new()
            .read(true)
            .custom_flags(0x200000 | 0x20000)
            .open(path)
            .map_err(|error| error.to_string())?;
        if fast_move::descriptor_is_kio_fuse(&file) {
            return Ok(());
        }
        fs::set_permissions(path, fs::Permissions::from_mode(self.mode & 0o7777))
            .map_err(|e| e.to_string())?;
        let time = if self.seconds >= 0 {
            SystemTime::UNIX_EPOCH
                .checked_add(Duration::new(self.seconds as u64, self.nanos as u32))
        } else {
            SystemTime::UNIX_EPOCH
                .checked_sub(Duration::from_secs(self.seconds.unsigned_abs()))
                .and_then(|t| t.checked_add(Duration::from_nanos(self.nanos as u64)))
        };
        if let Some(t) = time {
            fs::File::open(path)
                .and_then(|f| f.set_times(fs::FileTimes::new().set_modified(t)))
                .map_err(|e| e.to_string())?;
        }
        Ok(())
    }
}
#[derive(Clone, Default, Serialize, Deserialize)]
#[serde(default)]
/// Journal d’une entrée. Les champs optionnels décrivent les étapes déjà prévues ou réalisées ;
/// ils permettent de reprendre une restauration sans supposer qu’une étape a été terminée.
pub(super) struct Record {
    /// Préfixe durable d’une copie non encore publiée ; absent après la phase de publication.
    pub resume: Option<resume::Point>,
    /// Racine déplacée d’un seul renommage ; ses descendants possèdent aussi leur propre journal.
    pub directory_move: bool,
    /// Le résultat est l’original renommé : son retour conserve l’inode et ne détruit aucun contenu.
    pub rename_move: bool,
    /// Ctime après renommage : détecte une modification même si la taille et mtime ont été rétablies.
    pub rename_ctime: Option<(i64, i64)>,
    /// Chemin privé utilisé avant la publication de la copie.
    pub staging: Option<PathBuf>,
    /// Identité du temporaire pour ne pas supprimer un remplacement externe.
    pub staging_stamp: Option<Stamp>,
    /// Empreinte du résultat prévu lors de la publication.
    pub result: Option<Stamp>,
    /// MD5 du contenu copié, utilisé pour protéger une suppression de restauration.
    pub digest: Option<String>,
    /// Ancienne destination déplacée à l’abri avant son remplacement.
    pub backup: Option<PathBuf>,
    /// Empreinte de cette ancienne destination sauvegardée.
    pub original: Option<Stamp>,
    /// Ctime de la sauvegarde après son renommage : détecte aussi une mtime rétablie.
    pub backup_ctime: Option<(i64, i64)>,
    /// Version de la source avant sa suppression ou du dossier avant son retrait.
    pub source_stamp: Option<Stamp>,
    /// La suppression de la source d’un fichier a été journalisée.
    pub source_removed: bool,
    /// Le retrait du dossier source a été journalisé.
    pub source_dir_removed: bool,
    /// Empreinte de la source préparée ou publiée par une restauration.
    pub restored_source: Option<Stamp>,
    /// Emplacement privé de la source en cours de restauration.
    pub restore_staging: Option<PathBuf>,
    /// Cette entrée a achevé sa restauration ; les étapes ne doivent pas être rejouées.
    pub reverted: bool,
}
impl Record {
    /// Les empreintes de contenu, dates et inodes restent contrôlées après un changement de dev_t.
    pub(super) fn rebind(&mut self, devices: &std::collections::HashMap<u64, u64>) {
        for stamp in [
            &mut self.staging_stamp,
            &mut self.result,
            &mut self.original,
            &mut self.source_stamp,
            &mut self.restored_source,
        ]
        .into_iter()
        .flatten()
        {
            stamp.rebind(devices);
        }
        if let Some(point) = &mut self.resume {
            point.rebind(devices);
        }
    }
}
/// Teste l’existence du chemin lui-même, y compris celle d’un lien pendant.
fn exists(path: &Path) -> bool {
    fs::symlink_metadata(path).is_ok()
}
/// Refuse une suppression ou un déplacement si l’empreinte ne correspond plus.
fn own(path: &Path, stamp: &Stamp) -> Result<(), String> {
    if stamp.matches(path) {
        Ok(())
    } else {
        Err(format!(
            "Élément modifié ou remplacé, conservé : {}",
            path.display()
        ))
    }
}
/// Rend durable le changement de nom dans le répertoire parent.
fn sync_parent(path: &Path) -> Result<(), String> {
    sync_directory(path.parent().ok_or("Parent absent")?).map_err(|e| e.to_string())
}
/// Met à jour la copie locale du journal puis appelle la sauvegarde fournie.
fn persist(
    entry: &mut Entry,
    r: &Record,
    save: &mut impl FnMut(&Entry) -> Result<(), String>,
) -> Result<(), String> {
    entry.undo = Some(r.clone());
    save(entry)
}
/// Réserve un dossier privé avec un nom unique et des permissions 0700.
pub(super) fn private_directory(parent: &Path, prefix: &str) -> Result<PathBuf, String> {
    for n in 0..1000 {
        let path = parent.join(format!(".gad-{prefix}-{}-{n}", std::process::id()));
        match fs::DirBuilder::new().mode(0o700).create(&path) {
            Ok(()) => return Ok(path),
            Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => (),
            Err(e) => return Err(e.to_string()),
        }
    }
    Err("Unable to reserve temporary folder".into())
}
/// Détermine si la destination est encore le résultat publié par Gad.
/// Distingue aussi une destination jamais publiée d’un résultat modifié ou remplacé.
pub(super) fn check_output(entry: &Entry, r: &Record) -> Result<bool, String> {
    if !exists(&entry.destination) {
        if let Some(stamp) = r.result.as_ref().or(r.staging_stamp.as_ref()) {
            stamp.volume(&entry.destination)?;
        }
        return Ok(false);
    }
    if let Some(stamp) = &r.result {
        if stamp.matches(&entry.destination) {
            if r.rename_move && r.digest.is_none() && entry.kind == "file" {
                let meta = fs::symlink_metadata(&entry.destination).map_err(|e| e.to_string())?;
                if r.rename_ctime != Some((meta.ctime(), meta.ctime_nsec())) {
                    return Err(format!("Résultat rapide modifié ou renommage interrompu avant validation du journal, conservé : {}", entry.destination.display()));
                }
            }
            if let Some(digest) = &r.digest {
                if md5_file(&entry.destination, None, |_| Ok(()))? != *digest {
                    return Err(format!(
                        "Contenu modifié, conservé : {}",
                        entry.destination.display()
                    ));
                }
            }
            return Ok(true);
        }
    }
    if r.original
        .as_ref()
        .is_some_and(|s| s.matches(&entry.destination))
        && r.backup.as_ref().is_none_or(|p| !exists(p))
    {
        return Ok(false);
    }
    // No publication was planned: a pre-existing directory belongs to the user.
    if r.result.is_none() && r.backup.is_none() {
        return Ok(false);
    }
    Err(format!(
        "Destination modifiée depuis le transfert, conservée : {}",
        entry.destination.display()
    ))
}
/// Contrôle la sauvegarde avant toute suppression du résultat et avant son retour.
fn check_backup(entry: &Entry, r: &Record) -> Result<(), String> {
    // L’ancienne destination doit encore correspondre au journal avant toute manipulation de sa sauvegarde.
    if let Some(backup) = &r.backup {
        if let Some(stamp) = &r.original {
            stamp.volume(backup)?;
        }
        if exists(backup) {
            own(
                backup,
                r.original
                    .as_ref()
                    .ok_or("Journal de remplacement incomplet")?,
            )?;
            if let Some(expected) = r.backup_ctime {
                let meta = fs::symlink_metadata(backup).map_err(|error| error.to_string())?;
                if expected != (meta.ctime(), meta.ctime_nsec()) {
                    return Err("Backup changed since the transfer; files kept".into());
                }
            }
        } else if !r
            .original
            .as_ref()
            .is_some_and(|stamp| stamp.matches(&entry.destination))
        {
            return Err(format!(
                "Sauvegarde du fichier remplacé absente ; destination conservée : {}",
                backup.display()
            ));
        }
    }
    Ok(())
}
/// Une source déjà remise en place peut avoir été éditée depuis une restauration interrompue.
fn check_restored_source(entry: &Entry, r: &Record) -> Result<(), String> {
    if r.source_removed && entry.kind == "file" && exists(&entry.source) {
        if let Some(digest) = &r.digest {
            if md5_file(&entry.source, None, |_| Ok(()))? != *digest {
                return Err("Restored source changed; destination kept".into());
            }
        }
    }
    Ok(())
}
/// Les parents source peuvent manquer avant restauration ; les parents existants
/// doivent rester des dossiers réels, sans redirection par lien symbolique.
fn check_parents(path: &Path) -> Result<(), String> {
    let mut parent = path.parent();
    while let Some(directory) = parent {
        match fs::symlink_metadata(directory) {
            Ok(meta) if !meta.is_dir() || meta.file_type().is_symlink() => {
                return Err(format!(
                    "Dossier déplacé ou remplacé, conservé : {}",
                    directory.display()
                ));
            }
            Ok(_) => (),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => (),
            Err(error) => return Err(reconnect::io_error(error)),
        }
        parent = directory.parent();
    }
    Ok(())
}
/// Réutilisé après une copie de retour : ses parents peuvent avoir été redirigés entre-temps.
fn check_paths(entry: &Entry, r: &Record) -> Result<(), String> {
    check_parents(&entry.destination)?;
    if r.source_removed || r.source_dir_removed {
        check_parents(&entry.source)?;
    }
    if let Some(backup) = &r.backup {
        check_parents(backup)?;
    }
    Ok(())
}
/// Vérifie les résultats, sauvegardes et sources avant les modifications de restauration.
/// Une erreur conserve les fichiers litigieux au lieu de tenter de les écraser.
pub(super) fn preflight(entry: &Entry) -> Result<(), String> {
    let Some(r) = &entry.undo else { return Ok(()) };
    if r.reverted {
        return Ok(());
    }
    check_paths(entry, r)?;
    let output = check_output(entry, r)?;
    check_backup(entry, r)?;
    if r.source_removed && !exists(&entry.source) {
        if let Some(stamp) = &r.source_stamp {
            stamp.volume(&entry.source)?;
        }
        if !output {
            return Err("Restoration refused: a moved file is missing".into());
        }
    }
    if r.source_removed
        && exists(&entry.source)
        && !r
            .source_stamp
            .as_ref()
            .is_some_and(|s| s.matches(&entry.source))
        && !r
            .restored_source
            .as_ref()
            .is_some_and(|s| s.matches(&entry.source))
    {
        return Err(format!(
            "La source a été recréée ou modifiée, conservée : {}",
            entry.source.display()
        ));
    }
    check_restored_source(entry, r)?;
    Ok(())
}
/// Recrée un dossier source supprimé par un déplacement, en journalisant
/// un emplacement privé de restauration avant sa publication.
pub(super) fn restore_directory(
    entry: &mut Entry,
    save: &mut impl FnMut(&Entry) -> Result<(), String>,
) -> Result<(), String> {
    let Some(mut r) = entry.undo.clone() else {
        return Ok(());
    };
    if !r.source_dir_removed || r.reverted {
        return Ok(());
    }
    if exists(&entry.source) {
        if r.source_stamp
            .as_ref()
            .is_some_and(|s| s.matches(&entry.source))
            || r.restored_source
                .as_ref()
                .is_some_and(|s| s.matches(&entry.source))
        {
            return Ok(());
        }
        return Err(format!(
            "Dossier source recréé par une autre action : {}",
            entry.source.display()
        ));
    }
    if let Some(stamp) = &r.source_stamp {
        stamp.volume(&entry.source)?;
    }
    cleanup_staging(&r)?;
    safe_parents(&entry.source)?;
    let temp = private_directory(entry.source.parent().ok_or("Parent absent")?, "restore-dir")?;
    r.restore_staging = Some(temp.clone());
    r.restored_source = Some(Stamp::read(&temp)?);
    persist(entry, &r, save)?;
    publish_new(&temp, &entry.source).map_err(|e| e.to_string())?;
    sync_parent(&entry.source)?;
    // Keep directories writable until every child has been restored.
    Ok(())
}
/// Remet un fichier déplacé à sa source, retire la copie reconnue et restaure
/// l’ancienne destination sauvegardée. La restauration inter-volumes utilise une copie
/// contrôlée plutôt qu’un renommage impossible entre systèmes de fichiers.
pub(super) fn restore_file(
    entry: &mut Entry,
    save: &mut impl FnMut(&Entry) -> Result<(), String>,
) -> Result<(), String> {
    let Some(mut r) = entry.undo.clone() else {
        entry.state = TransferState::Undone;
        entry.cancel = false;
        entry.paused = false;
        return save(entry);
    };
    if r.reverted {
        return Ok(());
    }
    // Un journal de dossier en bloc existe avant la création de tous les parents cible.
    // Si le renommage n’a pas eu lieu (ou a déjà été défait), cette entrée n’a rien à retirer.
    if r.rename_move
        && r.backup.is_none()
        && r.staging.is_none()
        && fs::symlink_metadata(&entry.destination)
            .is_err_and(|error| error.kind() == std::io::ErrorKind::NotFound)
        && r.source_stamp
            .as_ref()
            .is_some_and(|stamp| stamp.matches(&entry.source))
    {
        r.reverted = true;
        entry.state = TransferState::Undone;
        entry.cancel = false;
        entry.paused = false;
        entry.error.clear();
        return persist(entry, &r, save);
    }
    safe_parents(&entry.destination)?;
    let mut output = check_output(entry, &r)?;
    if r.source_removed && !exists(&entry.source) {
        if !output {
            return Err(format!(
                "La copie à remettre à sa source est absente : {}",
                entry.destination.display()
            ));
        }
        if let Some(stamp) = &r.source_stamp {
            stamp.volume(&entry.source)?;
        }
        if r.rename_move {
            safe_parents(&entry.source)?;
            r.restored_source = r.result.clone();
            persist(entry, &r, save)?;
            check_output(entry, &r)?;
            fast_move::rename_local(&entry.destination, &entry.source).map_err(|error| {
                format!("Retour par renommage impossible ; résultat conservé : {error}")
            })?;
            sync_parent(&entry.source)?;
            sync_parent(&entry.destination)?;
            output = false;
        } else {
            cleanup_staging(&r)?;
            safe_parents(&entry.source)?;
            let dir = private_directory(
                entry.source.parent().ok_or("Parent source absent")?,
                "restore",
            )?;
            let temp = dir.join("file");
            let prepared = (|| {
                if entry.kind == "link" {
                    symlink(
                        fs::read_link(&entry.destination).map_err(|e| e.to_string())?,
                        &temp,
                    )
                    .map_err(|e| e.to_string())?;
                } else {
                    let mut input = OpenOptions::new()
                        .read(true)
                        .custom_flags(0x20000 | 0x800)
                        .open(&entry.destination)
                        .map_err(|e| e.to_string())?;
                    let mut output = OpenOptions::new()
                        .write(true)
                        .create_new(true)
                        .mode(0o600)
                        .open(&temp)
                        .map_err(|e| e.to_string())?;
                    std::io::copy(&mut input, &mut output).map_err(|e| e.to_string())?;
                    output.sync_all().map_err(|e| e.to_string())?;
                    if let Some(digest) = &r.digest {
                        if md5_file(&temp, None, |_| Ok(()))? != *digest {
                            return Err("Contents changed while moving back to the source".into());
                        }
                    }
                }
                if let Some(stamp) = &r.source_stamp {
                    stamp.metadata(&temp)?;
                }
                r.restore_staging = Some(temp.clone());
                r.restored_source = Some(Stamp::read(&temp)?);
                persist(entry, &r, save)?;
                publish_new(&temp, &entry.source)
                    .map_err(|e| format!("Source existante conservée : {e}"))?;
                sync_parent(&entry.source)
            })();
            if prepared.is_err() {
                let _ = fs::remove_file(&temp);
            }
            let _ = fs::remove_dir(&dir);
            prepared?;
        }
    }
    if r.source_removed
        && !r
            .source_stamp
            .as_ref()
            .is_some_and(|s| s.matches(&entry.source))
        && !r
            .restored_source
            .as_ref()
            .is_some_and(|s| s.matches(&entry.source))
    {
        return Err(format!(
            "Original non restauré : {}",
            entry.source.display()
        ));
    }
    // La copie de retour et sa sauvegarde du journal ont pu prendre du temps : revalider
    // les deux fichiers conservés avant de supprimer le seul résultat encore valide.
    check_paths(entry, &r)?;
    check_backup(entry, &r)?;
    check_restored_source(entry, &r)?;
    if output {
        check_output(entry, &r)?;
        fs::remove_file(&entry.destination).map_err(|e| e.to_string())?;
        sync_parent(&entry.destination)?;
    }
    if let Some(backup) = &r.backup {
        if let Some(stamp) = &r.original {
            stamp.volume(backup)?;
        }
        if exists(backup) {
            own(
                backup,
                r.original.as_ref().ok_or("Sauvegarde sans empreinte")?,
            )?;
            publish_new(backup, &entry.destination)
                .map_err(|e| format!("Sauvegarde conservée ; restauration impossible : {e}"))?;
            sync_parent(&entry.destination)?;
        }
        if let Some(parent) = backup.parent() {
            let _ = fs::remove_dir(parent);
        }
    }
    // L’étape n’est marquée terminée qu’après les restaurations et le nettoyage nécessaires.
    cleanup_staging(&r)?;
    r.reverted = true;
    entry.state = TransferState::Undone;
    entry.cancel = false;
    entry.paused = false;
    entry.error.clear();
    persist(entry, &r, save)
}
/// Retire uniquement un dossier cible créé par Gad et encore vide, puis restaure
/// les métadonnées source et marque cette étape comme terminée.
pub(super) fn finish_directory(
    entry: &mut Entry,
    save: &mut impl FnMut(&Entry) -> Result<(), String>,
) -> Result<(), String> {
    let Some(mut r) = entry.undo.clone() else {
        entry.state = TransferState::Undone;
        entry.cancel = false;
        entry.paused = false;
        return save(entry);
    };
    if r.reverted {
        return Ok(());
    }
    if let Some(result) = &r.result {
        if exists(&entry.destination) {
            safe_parents(&entry.destination)?;
            own(&entry.destination, result)?;
            fs::remove_dir(&entry.destination).map_err(|e| {
                format!("Dossier conservé (il contient peut-être de nouveaux fichiers) : {e}")
            })?;
        }
    }
    if r.source_dir_removed {
        if let Some(original) = &r.source_stamp {
            original.metadata(&entry.source)?;
        }
    }
    cleanup_staging(&r)?;
    r.reverted = true;
    entry.state = TransferState::Undone;
    entry.cancel = false;
    entry.paused = false;
    entry.error.clear();
    persist(entry, &r, save)
}
/// Supprime les temporaires reconnus par leur périphérique et inode.
/// La taille d’un fichier partiellement écrit peut légitimement avoir changé.
pub(super) fn cleanup_staging(r: &Record) -> Result<(), String> {
    for (path, stamp) in [
        (&r.staging, &r.staging_stamp),
        (&r.restore_staging, &r.restored_source),
    ] {
        if let (Some(path), Some(stamp)) = (path, stamp) {
            if exists(path) {
                // Partial data may have changed its size, but the inode must still be ours.
                let now = Stamp::read(path)?;
                if now.dev != stamp.dev || now.ino != stamp.ino {
                    return Err("Temporary file replaced; kept".into());
                }
                if now.kind == "directory" {
                    fs::remove_dir(path)
                } else {
                    fs::remove_file(path)
                }
                .map_err(|e| e.to_string())?;
            }
            if let Some(parent) = path.parent() {
                if parent
                    .file_name()
                    .is_some_and(|n| n.to_string_lossy().starts_with(".gad-restore-"))
                {
                    let _ = fs::remove_dir(parent);
                }
            }
        }
    }
    Ok(())
}
/// Abandonne la possibilité de restauration en supprimant les sauvegardes et
/// temporaires reconnus, tout en conservant les fichiers finaux.
pub(super) fn discard(entry: &Entry) -> Result<(), String> {
    let Some(r) = &entry.undo else { return Ok(()) };
    if let Some(backup) = &r.backup {
        if let Some(stamp) = &r.original {
            stamp.volume(backup)?;
        }
        if exists(backup) {
            own(
                backup,
                r.original
                    .as_ref()
                    .ok_or("Journal de sauvegarde incomplet")?,
            )?;
            fs::remove_file(backup).map_err(|e| e.to_string())?;
        }
        if let Some(parent) = backup.parent() {
            let _ = fs::remove_dir(parent);
        }
    }
    cleanup_staging(r)
}

/// Rétablit les métadonnées des parents recréés pour une restauration partielle
/// sans supprimer les dossiers cible qui contiennent encore des voisins.
pub(super) fn restore_parent_metadata(entry: &Entry) -> Result<(), String> {
    if let Some(record) = &entry.undo {
        if record.source_dir_removed {
            if let Some(stamp) = &record.source_stamp {
                stamp.metadata(&entry.source)?;
            }
        }
    }
    Ok(())
}
