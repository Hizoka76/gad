//! Sources instables et restauration prudente : tous les chemins appartiennent aux fixtures.
use super::*;
use std::os::unix::fs::PermissionsExt;

/// Rétablir mtime après une écriture reproduit un changement invisible au seul couple taille/date.
pub(super) fn rewrite_preserving_mtime(path: &Path, contents: &[u8]) {
    let modified = fs::metadata(path).unwrap().modified().unwrap();
    // tmpfs peut réutiliser l’horloge du même tick pour plusieurs écritures immédiates.
    thread::sleep(Duration::from_millis(10));
    fs::write(path, contents).unwrap();
    fs::File::open(path)
        .unwrap()
        .set_times(fs::FileTimes::new().set_modified(modified))
        .unwrap();
}

#[test]
/// Une modification de même taille doit invalider la copie, le MD5 et la suppression source.
fn same_size_source_change_is_detected_even_when_mtime_is_restored() {
    let f = Fixture::new();
    let source = f.0.join("source/sub/été\n.txt");
    let before = fs::metadata(&source).unwrap();
    rewrite_preserving_mtime(&source, b"BONJOUR\0MONDE");
    let after = fs::metadata(&source).unwrap();
    assert_eq!(before.len(), after.len());
    assert_eq!(before.modified().unwrap(), after.modified().unwrap());
    assert!(!same_version(&before, &after));
    assert!(verify_source(&source, &before).is_err());
    assert!(md5_file(&source, Some(&before), |_| Ok(())).is_err());
    assert!(remove_original(&source, &before).is_err());
    assert_eq!(fs::read(source).unwrap(), b"BONJOUR\0MONDE");
}

#[test]
/// Le nom peut être remplacé pendant qu’un descripteur conserve l’ancien fichier ouvert.
fn source_path_replacement_is_not_confused_with_the_open_file() {
    let f = Fixture::new();
    let source = f.0.join("source/sub/été\n.txt");
    let open = fs::File::open(&source).unwrap();
    let before = open.metadata().unwrap();
    fs::rename(&source, source.with_extension("old")).unwrap();
    fs::write(&source, b"bonjour\0monde").unwrap();
    assert!(verify_source(&source, &before).is_err());
    assert_ne!(
        open.metadata().unwrap().ino(),
        fs::metadata(source).unwrap().ino()
    );
}

#[test]
/// Une édition pendant la relecture cible ne doit pas rendre le MD5 source périmé valide.
fn source_change_during_target_verification_is_detected_with_restored_mtime() {
    let f = Fixture::new();
    let source = f.0.join("source/sub/été\n.txt");
    let destination = f.0.join("dest/check");
    fs::copy(&source, &destination).unwrap();
    let expected = fs::metadata(&source).unwrap();
    let result = compare_md5(&source, &destination, &expected, |read| {
        if read > expected.len() {
            rewrite_preserving_mtime(&source, b"BONJOUR\0MONDE");
        }
        Ok(())
    });
    assert!(result.unwrap_err().contains("Source file changed"));
    assert_eq!(fs::read(destination).unwrap(), b"bonjour\0monde");
}

#[test]
/// Une modification avant le début du transfert reste autorisée : copier la version courante.
fn queued_source_can_change_before_copying_starts() {
    let f = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    let source = f.0.join("source/sub/été\n.txt");
    engine
        .request(
            &json!({"command":"enqueue","sources":[source],"destination":f.0.join("dest")})
                .to_string(),
        )
        .unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    fs::write(&source, b"new contents written while queued").unwrap();
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    let state = wait(&engine, |s| {
        ["done", "error"].contains(&s["jobs"][0]["state"].as_str().unwrap())
    });
    assert_eq!(state["jobs"][0]["state"], "done", "{state}");
    assert_eq!(
        fs::read(f.0.join("dest/été\n.txt")).unwrap(),
        b"new contents written while queued"
    );
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Sans MD5, arrêter une source modifiée conserve l’ancienne destination et l’original édité.
fn changing_source_during_copy_or_move_keeps_both_user_files() {
    for mode in ["copy", "move"] {
        let f = Fixture::new();
        let engine = Engine::new();
        engine
            .request(r#"{"command":"pause","paused":true}"#)
            .unwrap();
        engine
            .request(r#"{"command":"configure","rate_limit":262144}"#)
            .unwrap();
        let source = f.0.join("source/large");
        let destination = f.0.join("dest/large");
        fs::write(&source, vec![1u8; 1024 * 1024]).unwrap();
        fs::write(&destination, b"old destination").unwrap();
        engine.request(&json!({"command":"enqueue","mode":mode,"verify_md5":false,"sources":[source],"destination":f.0.join("dest")}).to_string()).unwrap();
        wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
        // Forcer le chemin copie/suppression, indépendamment des montages de la machine de test.
        engine.update(1, |job| job.fast_move = false);
        engine
            .request(r#"{"command":"pause","paused":false}"#)
            .unwrap();
        wait(&engine, |s| {
            s["jobs"][0]["entries"][0]["state"] == "conflict"
        });
        engine
            .request(r#"{"command":"resolve","id":1,"action":"replace"}"#)
            .unwrap();
        wait(&engine, |s| {
            s["jobs"][0]["entries"][0]["copied"].as_u64().unwrap() > 0
        });
        rewrite_preserving_mtime(&source, &vec![2u8; 1024 * 1024]);
        let state = wait(&engine, |s| {
            ["error", "done"].contains(&s["jobs"][0]["state"].as_str().unwrap())
        });
        assert_eq!(state["jobs"][0]["state"], "error", "{mode}: {state}");
        assert_eq!(state["jobs"][0]["entries"][0]["state"], "error");
        assert_eq!(fs::read(&destination).unwrap(), b"old destination");
        assert_eq!(fs::read(&source).unwrap(), vec![2u8; 1024 * 1024]);
        engine.request(r#"{"command":"shutdown"}"#).unwrap();
    }
}

/// Prépare deux fichiers pour vérifier qu’un problème bloque toute la sélection avant modification.
fn two_file_transfer(f: &Fixture, moving: bool, replace: bool) -> Engine {
    let engine = Engine::new();
    fs::write(f.0.join("source/a"), b"first source").unwrap();
    fs::write(f.0.join("source/b"), b"second source").unwrap();
    if replace {
        fs::write(f.0.join("dest/a"), b"old backup").unwrap();
    }
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    engine.request(&json!({"command":"enqueue","mode":if moving {"move"} else {"copy"},"sources":[f.0.join("source/a"),f.0.join("source/b")],"destination":f.0.join("dest")}).to_string()).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    engine.update(1, |job| job.fast_move = false);
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    if replace {
        wait(&engine, |s| {
            s["jobs"][0]["entries"][0]["state"] == "conflict"
        });
        engine
            .request(r#"{"command":"resolve","id":1,"action":"replace"}"#)
            .unwrap();
    }
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    engine
}

#[test]
/// Une sauvegarde altérée à taille/date identiques bloque le lot avant le retrait d’un voisin.
fn edited_backup_blocks_restoration_before_any_selected_output_is_removed() {
    let f = Fixture::new();
    let engine = two_file_transfer(&f, false, true);
    let state = engine.poll(None).unwrap();
    let record = &state["jobs"][0]["entries"][0]["undo"];
    assert!(record["backup_ctime"].is_array());
    let backup = PathBuf::from(record["backup"].as_str().unwrap());
    rewrite_preserving_mtime(&backup, b"bad backup");
    let state = request_undo(&engine, vec![1]);
    assert_eq!(state["jobs"][0]["state"], "undo_error");
    assert!(state["jobs"][0]["error"]
        .as_str()
        .unwrap()
        .contains("Backup changed"));
    assert_eq!(fs::read(f.0.join("dest/a")).unwrap(), b"first source");
    assert_eq!(fs::read(f.0.join("dest/b")).unwrap(), b"second source");
    assert_eq!(fs::read(backup).unwrap(), b"bad backup");
    assert!(engine
        .request(r#"{"command":"shutdown"}"#)
        .unwrap_err()
        .contains("Backup changed"));
    engine
        .request(r#"{"command":"configure","keep_history":true}"#)
        .unwrap();
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Des permissions modifiées sur un résultat ne doivent pas être annulées par suppression du fichier.
fn edited_output_permissions_block_restoration() {
    let f = Fixture::new();
    let engine = two_file_transfer(&f, false, false);
    let target = f.0.join("dest/a");
    let mode = fs::metadata(&target).unwrap().mode() & 0o7777;
    fs::set_permissions(&target, fs::Permissions::from_mode(mode ^ 0o040)).unwrap();
    let state = request_undo(&engine, vec![1]);
    assert_eq!(state["jobs"][0]["state"], "undo_error");
    assert_eq!(fs::read(&target).unwrap(), b"first source");
    assert_eq!(fs::read(f.0.join("dest/b")).unwrap(), b"second source");
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Un résultat déplacé disparu doit être détecté avant le retour d’un autre fichier du lot.
fn missing_moved_output_blocks_restoration_before_restoring_neighbors() {
    let f = Fixture::new();
    let engine = two_file_transfer(&f, true, false);
    fs::remove_file(f.0.join("dest/a")).unwrap();
    let state = request_undo(&engine, vec![1]);
    assert_eq!(state["jobs"][0]["state"], "undo_error");
    assert!(!f.0.join("source/a").exists());
    assert!(!f.0.join("source/b").exists());
    assert_eq!(fs::read(f.0.join("dest/b")).unwrap(), b"second source");
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Une source publiée lors d’un retour interrompu est relue avant de retirer le dernier résultat valide.
fn edited_restored_source_keeps_the_valid_destination() {
    let f = Fixture::new();
    let engine = two_file_transfer(&f, true, false);
    let source = f.0.join("source/a");
    fs::copy(f.0.join("dest/a"), &source).unwrap();
    let stamp = undo::Stamp::read(&source).unwrap();
    engine.update(1, |job| {
        job.entries[0].undo.as_mut().unwrap().restored_source = Some(stamp)
    });
    rewrite_preserving_mtime(&source, b"FIRST SOURCE");
    let state = request_undo(&engine, vec![1]);
    assert_eq!(state["jobs"][0]["state"], "undo_error");
    assert!(state["jobs"][0]["error"]
        .as_str()
        .unwrap()
        .contains("Restored source changed"));
    assert_eq!(fs::read(source).unwrap(), b"FIRST SOURCE");
    assert_eq!(fs::read(f.0.join("dest/a")).unwrap(), b"first source");
    assert_eq!(fs::read(f.0.join("dest/b")).unwrap(), b"second source");
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Une sauvegarde peut changer après le précontrôle : la revérifier après la copie de retour.
fn backup_changed_during_restore_does_not_delete_the_valid_result() {
    let f = Fixture::new();
    let engine = two_file_transfer(&f, true, true);
    let mut entry = engine.shared.state.lock().unwrap().jobs[0].entries[0].clone();
    undo::preflight(&entry).unwrap();
    let backup = entry.undo.as_ref().unwrap().backup.clone().unwrap();
    let mut altered = false;
    let result = undo::restore_file(&mut entry, &mut |saved| {
        if !altered && saved.undo.as_ref().unwrap().restore_staging.is_some() {
            rewrite_preserving_mtime(&backup, b"bad backup");
            altered = true;
        }
        Ok(())
    });
    assert!(altered);
    assert!(result.unwrap_err().contains("Backup changed"));
    assert_eq!(fs::read(f.0.join("dest/a")).unwrap(), b"first source");
    assert_eq!(fs::read(f.0.join("source/a")).unwrap(), b"first source");
    assert_eq!(fs::read(backup).unwrap(), b"bad backup");
    assert!(engine
        .request(r#"{"command":"shutdown"}"#)
        .unwrap_err()
        .contains("Backup changed"));
    engine
        .request(r#"{"command":"configure","keep_history":true}"#)
        .unwrap();
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Les anciens journaux restent lisibles et les permissions disponibles restent contrôlées.
fn old_history_without_owner_or_backup_ctime_still_restores_unchanged_files() {
    let f = Fixture::new();
    let engine = two_file_transfer(&f, false, true);
    let mut entry = engine.shared.state.lock().unwrap().jobs[0].entries[0].clone();
    let mut value = serde_json::to_value(&entry).unwrap();
    let record = value["undo"].as_object_mut().unwrap();
    record.remove("backup_ctime");
    for field in ["source_stamp", "staging_stamp", "result", "original"] {
        if let Some(stamp) = record.get_mut(field).and_then(Value::as_object_mut) {
            stamp.remove("owner");
        }
    }
    entry = serde_json::from_value(value).unwrap();
    undo::preflight(&entry).unwrap();
    undo::restore_file(&mut entry, &mut |_| Ok(())).unwrap();
    assert_eq!(fs::read(f.0.join("dest/a")).unwrap(), b"old backup");
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Un parent redirigé doit être refusé dès le précontrôle, même s’il expose le même inode.
fn restoration_preflight_rejects_symlinked_parent_before_destructive_steps() {
    let f = Fixture::new();
    let engine = two_file_transfer(&f, false, false);
    let entry = engine.shared.state.lock().unwrap().jobs[0].entries[0].clone();
    fs::rename(f.0.join("dest"), f.0.join("redirected")).unwrap();
    symlink(f.0.join("redirected"), f.0.join("dest")).unwrap();
    assert!(undo::preflight(&entry).is_err());
    assert_eq!(fs::read(f.0.join("redirected/a")).unwrap(), b"first source");
    assert_eq!(
        fs::read(f.0.join("redirected/b")).unwrap(),
        b"second source"
    );
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Un déplacement externe du parent pendant la copie de retour ne doit pas autoriser une suppression via un lien.
fn parent_redirected_during_restore_keeps_the_result() {
    let f = Fixture::new();
    let engine = two_file_transfer(&f, true, false);
    let mut entry = engine.shared.state.lock().unwrap().jobs[0].entries[0].clone();
    undo::preflight(&entry).unwrap();
    let mut redirected = false;
    let result = undo::restore_file(&mut entry, &mut |saved| {
        if !redirected && saved.undo.as_ref().unwrap().restore_staging.is_some() {
            fs::rename(f.0.join("dest"), f.0.join("redirected")).unwrap();
            symlink(f.0.join("redirected"), f.0.join("dest")).unwrap();
            redirected = true;
        }
        Ok(())
    });
    assert!(redirected);
    assert!(result.is_err());
    assert_eq!(fs::read(f.0.join("redirected/a")).unwrap(), b"first source");
    assert_eq!(fs::read(f.0.join("source/a")).unwrap(), b"first source");
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}
