//! Publication sans écrasement sur les montages qui refusent RENAME_NOREPLACE.
//! Le nouveau nom est créé exclusivement ; son identité est journalisée avant le retrait du temporaire.
use super::*;
use std::os::unix::fs::DirBuilderExt;

pub(super) fn publish(
    source: &Path,
    destination: &Path,
    mut acknowledge: impl FnMut(&fs::Metadata) -> std::io::Result<()>,
    rename: impl FnOnce(&Path, &Path) -> std::io::Result<()>,
) -> std::io::Result<()> {
    match rename(source, destination) {
        Ok(()) => return Ok(()),
        Err(error) if matches!(error.raw_os_error(), Some(22 | 38 | 95)) => (),
        Err(error) => return Err(error),
    }
    let metadata = fs::symlink_metadata(source)?;
    if metadata.is_file() {
        // link ne remplace jamais une destination existante, même apparue après les contrôles.
        fs::hard_link(source, destination)?;
    } else if metadata.file_type().is_symlink() {
        symlink(fs::read_link(source)?, destination)?;
    } else if metadata.is_dir() {
        // Seuls les dossiers privés vides de préparation/restauration utilisent ce repli.
        if fs::read_dir(source)?.next().is_some() {
            return Err(std::io::Error::new(
                std::io::ErrorKind::Unsupported,
                "Cannot publish a nonempty folder without atomic no-replace rename",
            ));
        }
        // Le masque utilisateur ne doit pas réduire les permissions déjà décidées par la copie.
        // Garder le nouveau dossier privé jusqu’à sa préparation, puis agir sur son descripteur.
        fs::DirBuilder::new().mode(0o700).create(destination)?;
        let directory = OpenOptions::new()
            .read(true)
            .custom_flags(0x20000 | 0x10000) // O_NOFOLLOW | O_DIRECTORY
            .open(destination)?;
        // Comme la copie ordinaire, respecter les droits imposés par les services KIO sans chmod.
        if !fast_move::descriptor_is_kio_fuse(&directory) {
            directory.set_permissions(metadata.permissions())?;
        }
    } else {
        return Err(std::io::Error::new(
            std::io::ErrorKind::Unsupported,
            "Unsupported publication source",
        ));
    }
    // Le lien physique peut changer ctime : capturer les versions après sa création.
    let source_version = fs::symlink_metadata(source)?;
    let source_stamp =
        undo::Stamp::from_metadata(source, &metadata).map_err(std::io::Error::other)?;
    if !source_stamp.matches_metadata(source, &source_version) {
        return Err(std::io::Error::other(
            "Publication source changed; files kept",
        ));
    }
    let destination_version = fs::symlink_metadata(destination)?;
    // SSHFS peut attribuer un autre inode FUSE au nouveau lien : conserver son identité réelle.
    // Un refus du journal laisse les deux noms intacts, sans effacer une destination inconnue.
    acknowledge(&destination_version)?;
    // Le fsync du journal peut durer : ne retirer aucun nom substitué ou modifié entre-temps.
    for (path, expected) in [
        (source, &source_version),
        (destination, &destination_version),
    ] {
        let current = fs::symlink_metadata(path)?;
        if !same_version(expected, &current) {
            return Err(std::io::Error::other(
                "Publication changed while saving its journal; files kept",
            ));
        }
    }
    if metadata.is_dir() {
        fs::remove_dir(source)
    } else {
        fs::remove_file(source)
    }
}
