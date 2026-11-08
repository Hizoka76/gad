//! Politiques capturées à l’ajout : les préférences futures ne changent pas une demande en attente.
use super::*;

/// Les valeurs publiques ne permettent pas un remplacement inconditionnel par défaut.
pub(super) fn parse(value: &Value) -> Result<String, String> {
    match value.as_str() {
        Some(policy @ ("ask" | "skip" | "newer" | "rename")) => Ok(policy.to_owned()),
        _ => Err("Invalid conflict policy".into()),
    }
}

/// Les anciens journaux restent interactifs.
pub(super) fn ask() -> String {
    "ask".into()
}

/// Comparer les dates des fichiers eux-mêmes, sans suivre un lien ni remplacer un dossier.
/// Une date illisible est une erreur explicite ; aucune substitution risquée n’est autorisée.
pub(super) fn newer(source: &Path, destination: &Path) -> Result<&'static str, String> {
    let source = fs::symlink_metadata(source).map_err(|e| e.to_string())?;
    let destination = fs::symlink_metadata(destination).map_err(|e| e.to_string())?;
    if !source.is_file() || !destination.is_file() {
        return Err(
            "Cannot compare modification dates safely; choose another conflict policy".into(),
        );
    }
    let source = source.modified().map_err(|e| e.to_string())?;
    let destination = destination.modified().map_err(|e| e.to_string())?;
    Ok(if source > destination {
        "replace"
    } else {
        "skip"
    })
}
