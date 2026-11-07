//! Tests de régression du moteur sur des arborescences temporaires isolées.

use super::*;
use std::{
    sync::atomic::{AtomicU64, Ordering},
    time::Duration,
};
static NEXT: AtomicU64 = AtomicU64::new(0);
/// Arborescence isolée d’un test, automatiquement nettoyée à sa destruction.
struct Fixture(PathBuf);
impl Fixture {
    /// Crée une arborescence temporaire propre à ce test, avec un nom indépendant des autres tests.
    fn new() -> Self {
        Self::new_in(&std::env::temp_dir())
    }
    /// tmpfs local connu, même si /tmp appartient à un overlay dans le conteneur CI.
    fn local() -> Self {
        Self::new_in(Path::new("/dev/shm"))
    }
    /// Choisit uniquement la racine de test ; le contenu reste identique et privé.
    fn new_in(root: &Path) -> Self {
        let p = root.join(format!(
            "gad-test-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir_all(p.join("source/sub")).unwrap();
        fs::create_dir(p.join("dest")).unwrap();
        fs::write(p.join("source/sub/été\n.txt"), b"bonjour\0monde").unwrap();
        Self(p)
    }
    /// Une destination distincte permet de tester les priorités sans créer des doublons.
    fn enqueue_distinct(&self, engine: &Engine, number: usize) -> u64 {
        let destination = self.0.join(format!("dest-{number}"));
        fs::create_dir_all(&destination).unwrap();
        engine.request(&json!({"command":"enqueue", "sources":[self.0.join("source")], "destination":destination}).to_string()).unwrap()["id"].as_u64().unwrap()
    }
    /// Soumet l’arborescence de la fixture et retourne l’identifiant du lot créé.
    fn enqueue(&self, engine: &Engine) -> u64 {
        engine.request(&json!({"command":"enqueue", "sources":[self.0.join("source")], "destination":self.0.join("dest")}).to_string()).unwrap()["id"].as_u64().unwrap()
    }
}
impl Drop for Fixture {
    /// Nettoie uniquement le dossier temporaire appartenant à cette fixture.
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}
/// Attend un état observable avec une limite de temps et affiche l’état final en cas d’échec.
fn wait(engine: &Engine, predicate: impl Fn(&Value) -> bool) -> Value {
    for _ in 0..500 {
        let state = engine.request(r#"{"command":"status"}"#).unwrap();
        if predicate(&state) {
            return state;
        }
        thread::sleep(Duration::from_millis(10));
    }
    panic!(
        "Délai dépassé : {}",
        engine.request(r#"{"command":"status"}"#).unwrap()
    );
}
#[test]
/// Vérifie que la copie conserve les fichiers et le texte des liens sans suivre leur cible.
fn copies_tree_and_symlink_without_following_it() {
    let fixture = Fixture::new();
    let engine = Engine::new();
    symlink("sub/été\n.txt", fixture.0.join("source/lien")).unwrap();
    fixture.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert_eq!(
        fs::read(fixture.0.join("dest/source/sub/été\n.txt")).unwrap(),
        b"bonjour\0monde"
    );
    assert_eq!(
        fs::read_link(fixture.0.join("dest/source/lien")).unwrap(),
        PathBuf::from("sub/été\n.txt")
    );
}
#[test]
/// Vérifie que les copies sur elles-mêmes et dans leurs propres descendants sont refusées.
fn rejects_recursive_and_identical_copy() {
    let fixture = Fixture::new();
    let engine = Engine::new();
    for (index, dest) in [fixture.0.join("source/sub"), fixture.0.clone()]
        .into_iter()
        .enumerate()
    {
        engine.request(&json!({"command":"enqueue","sources":[fixture.0.join("source")],"destination":dest}).to_string()).unwrap();
        wait(&engine, |state| state["jobs"][index]["state"] == "error");
    }
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}
#[test]
/// Vérifie que la pause bloque la file, l’ordre est conservé et un conflit peut être renommé.
fn pause_fifo_cancel_and_conflict_rename() {
    let fixture = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    fs::create_dir_all(fixture.0.join("other/source/sub")).unwrap();
    fs::write(fixture.0.join("other/source/sub/été\n.txt"), b"second").unwrap();
    let first = fixture.enqueue(&engine);
    engine.request(&json!({"command":"enqueue","sources":[fixture.0.join("other/source")],"destination":fixture.0.join("dest")}).to_string()).unwrap();
    wait(&engine, |s| s["jobs"][1]["state"] == "waiting");
    assert!(!fixture.0.join("dest/source").exists());
    engine
        .request(&json!({"command":"cancel","id":first}).to_string())
        .unwrap();
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    wait(&engine, |s| s["jobs"][1]["state"] == "done");
    assert_eq!(
        engine.request(r#"{"command":"status"}"#).unwrap()["jobs"][0]["state"],
        "cancelled"
    );
    let third = fixture.enqueue(&engine);
    wait(&engine, |s| {
        s["jobs"][2]["entries"]
            .as_array()
            .is_some_and(|a| a.iter().any(|e| e["state"] == "conflict"))
    });
    engine
        .request(&json!({"command":"resolve","id":third,"action":"rename","all":true}).to_string())
        .unwrap();
    wait(&engine, |s| s["jobs"][2]["state"] == "done");
    assert!(fixture
        .0
        .join("dest/source/sub/été\n (copie 1).txt")
        .exists());
}
#[test]
/// Vérifie que les destinations symboliques et sources spéciales ne contournent pas les contrôles.
fn refuses_destination_symlink_and_special_files() {
    let fixture = Fixture::new();
    let engine = Engine::new();
    fs::create_dir(fixture.0.join("outside")).unwrap();
    symlink(fixture.0.join("outside"), fixture.0.join("dest/source")).unwrap();
    fixture.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "error");
    assert!(fs::read_dir(fixture.0.join("outside"))
        .unwrap()
        .next()
        .is_none());
}
#[test]
/// Vérifie que une collision au dernier instant laisse la destination existante intacte.
fn atomic_publication_never_overwrites_an_existing_file() {
    let fixture = Fixture::new();
    let temp = fixture.0.join("temp");
    let existing = fixture.0.join("existing");
    fs::write(&temp, b"nouveau").unwrap();
    fs::write(&existing, b"original").unwrap();
    assert!(publish_new(&temp, &existing).is_err());
    assert_eq!(fs::read(&existing).unwrap(), b"original");
    let fresh = fixture.0.join("fresh");
    publish_new(&temp, &fresh).unwrap();
    assert_eq!(fs::read(&fresh).unwrap(), b"nouveau");
    assert!(!temp.exists());
}
#[test]
/// Vérifie que les choix ignorer, remplacer et annuler conservent les contenus attendus.
fn skip_replace_and_cancel_preserve_expected_contents() {
    for action in ["skip", "replace", "cancel"] {
        let fixture = Fixture::new();
        fs::create_dir_all(fixture.0.join("dest/source/sub")).unwrap();
        let target = fixture.0.join("dest/source/sub/été\n.txt");
        fs::write(&target, b"ancien").unwrap();
        let engine = Engine::new();
        let id = fixture.enqueue(&engine);
        wait(&engine, |s| {
            s["jobs"][0]["entries"]
                .as_array()
                .is_some_and(|a| a.iter().any(|e| e["state"] == "conflict"))
        });
        let request = if action == "cancel" {
            json!({"command":"cancel","id":id})
        } else {
            json!({"command":"resolve","id":id,"action":action})
        };
        engine.request(&request.to_string()).unwrap();
        wait(&engine, |s| {
            terminal(s["jobs"][0]["state"].as_str().unwrap())
        });
        assert_eq!(
            fs::read(&target).unwrap(),
            if action == "replace" {
                b"bonjour\0monde".as_slice()
            } else {
                b"ancien".as_slice()
            }
        );
        assert!(!fs::read_dir(target.parent().unwrap()).unwrap().any(|e| e
            .unwrap()
            .file_name()
            .to_string_lossy()
            .ends_with(".part")));
    }
}
#[test]
/// Vérifie que un lien pendant se copie, tandis qu’un nom non UTF-8 produit une erreur lisible.
fn handles_dangling_symlinks_and_rejects_non_utf8_names_cleanly() {
    use std::os::unix::ffi::OsStringExt;
    let fixture = Fixture::new();
    let engine = Engine::new();
    symlink("absent", fixture.0.join("source/pendant")).unwrap();
    fixture.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert_eq!(
        fs::read_link(fixture.0.join("dest/source/pendant")).unwrap(),
        PathBuf::from("absent")
    );
    fs::write(
        fixture
            .0
            .join("source")
            .join(std::ffi::OsString::from_vec(vec![0xff])),
        b"x",
    )
    .unwrap();
    fixture.enqueue(&engine);
    let result = wait(&engine, |s| s["jobs"][1]["state"] == "error");
    assert!(result["jobs"][1]["error"]
        .as_str()
        .unwrap()
        .contains("UTF-8"));
}
#[test]
/// Vérifie que le déplacement ne retire les sources qu’après réussite des copies.
fn move_tree_removes_originals_only_after_success() {
    let fixture = Fixture::new();
    let engine = Engine::new();
    symlink("absent", fixture.0.join("source/lien")).unwrap();
    fs::create_dir(fixture.0.join("source/vide")).unwrap();
    engine.request(&json!({"command":"enqueue","sources":[fixture.0.join("source")],"destination":fixture.0.join("dest"),"mode":"move"}).to_string()).unwrap();
    let result = wait(&engine, |s| {
        terminal(s["jobs"][0]["state"].as_str().unwrap())
    });
    assert_eq!(result["jobs"][0]["state"], "done", "{result}");
    assert_eq!(result["jobs"][0]["mode"], "move");
    assert!(!fixture.0.join("source").exists());
    assert_eq!(
        fs::read(fixture.0.join("dest/source/sub/été\n.txt")).unwrap(),
        b"bonjour\0monde"
    );
    assert_eq!(
        fs::read_link(fixture.0.join("dest/source/lien")).unwrap(),
        PathBuf::from("absent")
    );
    assert!(fixture.0.join("dest/source/vide").is_dir());
}
#[test]
/// Vérifie que un saut, une annulation ou un échec cible ne font pas perdre l’original.
fn move_skip_cancel_and_failed_destination_keep_original() {
    for action in ["skip", "cancel", "blocked"] {
        let fixture = Fixture::new();
        let engine = Engine::new();
        let source = fixture.0.join("source/sub/été\n.txt");
        let target = fixture.0.join("dest/été\n.txt");
        if action == "blocked" {
            fs::create_dir(&target).unwrap();
        } else {
            fs::write(&target, b"existant").unwrap();
        }
        let id = engine.request(&json!({"command":"enqueue","sources":[source],"destination":fixture.0.join("dest"),"mode":"move"}).to_string()).unwrap()["id"].as_u64().unwrap();
        wait(&engine, |s| {
            s["jobs"][0]["entries"][0]["state"] == "conflict"
        });
        let request = if action == "cancel" {
            json!({"command":"cancel","id":id})
        } else {
            json!({"command":"resolve","id":id,"action":if action=="blocked" {"replace"} else {"skip"}})
        };
        engine.request(&request.to_string()).unwrap();
        wait(&engine, |s| {
            terminal(s["jobs"][0]["state"].as_str().unwrap())
        });
        assert_eq!(fs::read(&source).unwrap(), b"bonjour\0monde");
        if action != "blocked" {
            assert_eq!(fs::read(&target).unwrap(), b"existant");
        }
    }
}
#[test]
/// Vérifie que un dossier contenant un fichier ignoré reste à sa source.
fn move_with_skipped_file_keeps_its_source_directory() {
    let fixture = Fixture::new();
    let engine = Engine::new();
    fs::create_dir_all(fixture.0.join("dest/source/sub")).unwrap();
    fs::write(fixture.0.join("dest/source/sub/été\n.txt"), b"existant").unwrap();
    engine.request(&json!({"command":"enqueue","sources":[fixture.0.join("source")],"destination":fixture.0.join("dest"),"mode":"move"}).to_string()).unwrap();
    wait(&engine, |s| {
        s["jobs"][0]["entries"]
            .as_array()
            .unwrap()
            .iter()
            .any(|e| e["state"] == "conflict")
    });
    engine
        .request(r#"{"command":"resolve","id":1,"action":"skip","all":true}"#)
        .unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert!(fixture.0.join("source/sub/été\n.txt").is_file());
}
#[test]
/// Vérifie que le contrôle d’identité protège une source modifiée avant sa suppression.
fn deletion_guard_keeps_replaced_or_modified_original() {
    let fixture = Fixture::new();
    let source = fixture.0.join("source/sub/été\n.txt");
    let expected = fs::symlink_metadata(&source).unwrap();
    fs::rename(&source, fixture.0.join("old")).unwrap();
    fs::write(&source, b"remplacement").unwrap();
    assert!(remove_original(&source, &expected).is_err());
    assert_eq!(fs::read(&source).unwrap(), b"remplacement");
    let expected = fs::symlink_metadata(&source).unwrap();
    fs::write(&source, b"modification plus longue").unwrap();
    assert!(remove_original(&source, &expected).is_err());
    assert!(source.exists());
}
#[test]
/// Vérifie que les empreintes de référence et une corruption de même taille sont distinguées.
fn md5_matches_known_vectors_and_detects_same_size_corruption() {
    let fixture = Fixture::new();
    let source = fixture.0.join("abc");
    let target = fixture.0.join("target");
    fs::write(&source, b"abc").unwrap();
    fs::write(&target, b"abc").unwrap();
    let expected = fs::metadata(&source).unwrap();
    let hashes = compare_md5(&source, &target, &expected, |_| Ok(())).unwrap();
    assert_eq!(hashes.0, "900150983cd24fb0d6963f7d28e17f72");
    assert_eq!(hashes.0, hashes.1);
    fs::write(&target, b"abd").unwrap();
    let hashes = compare_md5(&source, &target, &expected, |_| Ok(())).unwrap();
    assert_ne!(hashes.0, hashes.1);
    fs::write(&target, b"").unwrap();
    assert_eq!(
        md5_file(&target, None, |_| Ok(())).unwrap(),
        "d41d8cd98f00b204e9800998ecf8427e"
    );
    fs::write(&source, "source modifiée".as_bytes()).unwrap();
    assert!(compare_md5(&source, &target, &expected, |_| Ok(()))
        .unwrap_err()
        .contains("changed"));
}
#[test]
/// Vérifie que une copie ou un déplacement avec MD5 attend les deux lectures avant validation.
fn md5_copy_and_move_validate_before_completing() {
    for mode in ["copy", "move"] {
        let fixture = Fixture::new();
        let engine = Engine::new();
        fs::write(fixture.0.join("source/vide"), b"").unwrap();
        symlink("sub/été\n.txt", fixture.0.join("source/lien")).unwrap();
        engine.request(&json!({"command":"enqueue","sources":[fixture.0.join("source")],"destination":fixture.0.join("dest"),"mode":mode,"verify_md5":true}).to_string()).unwrap();
        let result = wait(&engine, |s| {
            terminal(s["jobs"][0]["state"].as_str().unwrap())
        });
        assert_eq!(result["jobs"][0]["state"], "done", "{result}");
        assert_eq!(result["jobs"][0]["verify_md5"], true);
        for entry in result["jobs"][0]["entries"].as_array().unwrap() {
            if entry["kind"] == "file" {
                assert!(entry["md5_source"].as_str().is_some());
                assert_eq!(entry["md5_source"], entry["md5_destination"]);
                assert_eq!(
                    entry["verified"].as_u64().unwrap(),
                    2 * entry["size"].as_u64().unwrap()
                );
            } else {
                assert!(entry["md5_source"].is_null());
            }
        }
        assert_eq!(fixture.0.join("source").exists(), mode == "copy");
    }
}
#[test]
/// Vérifie que le réglage MD5 est figé par lot et peut être imposé par la requête.
fn md5_setting_is_captured_per_job_and_can_be_overridden() {
    let fixture = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    engine
        .request(r#"{"command":"configure","verify_md5":true}"#)
        .unwrap();
    fixture.enqueue_distinct(&engine, 1);
    engine
        .request(r#"{"command":"configure","verify_md5":false}"#)
        .unwrap();
    fixture.enqueue_distinct(&engine, 2);
    engine.request(&json!({"command":"enqueue","sources":[fixture.0.join("source")],"destination":fixture.0.join("dest"),"verify_md5":true}).to_string()).unwrap();
    wait(&engine, |s| {
        s["jobs"]
            .as_array()
            .unwrap()
            .iter()
            .all(|j| j["state"] == "waiting")
    });
    let result = engine.request(r#"{"command":"status"}"#).unwrap();
    assert_eq!(result["jobs"][0]["verify_md5"], true);
    assert_eq!(result["jobs"][1]["verify_md5"], false);
    assert_eq!(result["jobs"][2]["verify_md5"], true);
    for id in 1..=3 {
        engine
            .request(&json!({"command":"cancel","id":id}).to_string())
            .unwrap();
    }
    wait(&engine, |s| {
        s["jobs"]
            .as_array()
            .unwrap()
            .iter()
            .all(|j| j["state"] == "cancelled")
    });
}
#[test]
/// Vérifie que annuler la vérification conserve les fichiers et un changement tardif de source est détecté.
fn md5_cancel_keeps_both_files_and_checks_source_after_destination_read() {
    let fixture = Fixture::new();
    let source = fixture.0.join("source/sub/été\n.txt");
    let target = fixture.0.join("check");
    fs::copy(&source, &target).unwrap();
    let expected = fs::metadata(&source).unwrap();
    let result = compare_md5(&source, &target, &expected, |_| Err("Canceled".into()));
    assert_eq!(result.unwrap_err(), "Canceled");
    assert!(source.exists() && target.exists());
    let result = compare_md5(&source, &target, &expected, |read| {
        if read > expected.len() {
            fs::write(&source, b"nouveau contenu plus long").unwrap();
        }
        Ok(())
    });
    assert!(result.unwrap_err().contains("Source file changed"));
    assert!(source.exists() && target.exists());
}
/// Demande la restauration des lots et attend leur réussite ou une erreur explicite.
fn request_undo(engine: &Engine, ids: Vec<u64>) -> Value {
    engine
        .request(&json!({"command":"undo_many","ids":ids}).to_string())
        .unwrap();
    wait(engine, |s| {
        ids.iter().all(|id| {
            s["jobs"].as_array().unwrap().iter().any(|j| {
                j["id"] == *id && ["undone", "undo_error"].contains(&j["state"].as_str().unwrap())
            })
        })
    })
}
#[test]
/// Vérifie que la restauration récupère copies, déplacements, remplacements, liens et dossiers.
fn rollback_copy_and_move_restores_overwritten_files_links_and_directories() {
    for mode in ["copy", "move"] {
        let fixture = Fixture::new();
        let engine = Engine::new();
        fs::create_dir_all(fixture.0.join("dest/source/sub")).unwrap();
        let target = fixture.0.join("dest/source/sub/été\n.txt");
        fs::write(&target, b"ancien").unwrap();
        fs::create_dir(fixture.0.join("source/vide")).unwrap();
        symlink("absent", fixture.0.join("source/lien")).unwrap();
        let id=engine.request(&json!({"command":"enqueue","sources":[fixture.0.join("source")],"destination":fixture.0.join("dest"),"mode":mode}).to_string()).unwrap()["id"].as_u64().unwrap();
        wait(&engine, |s| {
            s["jobs"][0]["entries"]
                .as_array()
                .unwrap()
                .iter()
                .any(|e| e["state"] == "conflict")
        });
        engine
            .request(
                &json!({"command":"resolve","id":id,"action":"replace","all":true}).to_string(),
            )
            .unwrap();
        wait(&engine, |s| s["jobs"][0]["state"] == "done");
        let result = request_undo(&engine, vec![id]);
        assert_eq!(result["jobs"][0]["state"], "undone", "{result}");
        assert_eq!(fs::read(&target).unwrap(), b"ancien");
        assert_eq!(
            fs::read(fixture.0.join("source/sub/été\n.txt")).unwrap(),
            b"bonjour\0monde"
        );
        assert_eq!(
            fs::read_link(fixture.0.join("source/lien")).unwrap(),
            PathBuf::from("absent")
        );
        assert!(fixture.0.join("source/vide").is_dir());
        assert!(!fixture.0.join("dest/source/vide").exists());
        assert!(!fixture.0.join("dest/source/lien").exists());
        assert_eq!(fs::read_dir(target.parent().unwrap()).unwrap().count(), 1);
    }
}
#[test]
/// Vérifie que la restauration protège les données ajoutées ou modifiées depuis la copie.
fn rollback_removes_only_created_directories_and_refuses_modified_outputs() {
    let fixture = Fixture::new();
    let engine = Engine::new();
    fixture.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    let target = fixture.0.join("dest/source/sub/été\n.txt");
    fs::write(&target, b"modification externe").unwrap();
    let result = request_undo(&engine, vec![1]);
    assert_eq!(result["jobs"][0]["state"], "undo_error");
    assert_eq!(fs::read(&target).unwrap(), b"modification externe");
    assert!(fixture.0.join("source/sub/été\n.txt").exists());
    let fixture = Fixture::new();
    let engine = Engine::new();
    fixture.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    let result = request_undo(&engine, vec![1]);
    assert_eq!(result["jobs"][0]["state"], "undone", "{result}");
    assert!(!fixture.0.join("dest/source").exists());
    assert!(fixture.0.join("source/sub/été\n.txt").exists());
}
#[test]
/// Vérifie que la remise en état reste possible pendant la pause et avant toute publication.
fn rollback_while_paused_and_before_start_does_not_touch_preexisting_data() {
    let fixture = Fixture::new();
    let engine = Engine::new();
    fs::write(fixture.0.join("dest/preexisting"), b"intact").unwrap();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    fixture.enqueue(&engine);
    fixture.enqueue_distinct(&engine, 2);
    wait(&engine, |s| {
        s["jobs"]
            .as_array()
            .unwrap()
            .iter()
            .all(|j| j["state"] == "waiting")
    });
    let result = request_undo(&engine, vec![1, 2]);
    assert!(result["jobs"]
        .as_array()
        .unwrap()
        .iter()
        .all(|j| j["state"] == "undone"));
    assert_eq!(
        fs::read(fixture.0.join("dest/preexisting")).unwrap(),
        b"intact"
    );
    assert!(!fixture.0.join("dest/source").exists());
}
#[test]
/// Vérifie que une source recréée bloque le retour du déplacement, qui reste retentable.
fn rollback_refuses_a_new_source_then_can_be_retried() {
    let fixture = Fixture::new();
    let engine = Engine::new();
    let source = fixture.0.join("source/sub/été\n.txt");
    let target = fixture.0.join("dest/été\n.txt");
    engine.request(&json!({"command":"enqueue","sources":[source],"destination":fixture.0.join("dest"),"mode":"move"}).to_string()).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    fs::write(&source, b"nouveau fichier").unwrap();
    assert_eq!(
        request_undo(&engine, vec![1])["jobs"][0]["state"],
        "undo_error"
    );
    assert_eq!(fs::read(&source).unwrap(), b"nouveau fichier");
    assert_eq!(fs::read(&target).unwrap(), b"bonjour\0monde");
    fs::remove_file(&source).unwrap();
    assert_eq!(request_undo(&engine, vec![1])["jobs"][0]["state"], "undone");
    assert_eq!(fs::read(&source).unwrap(), b"bonjour\0monde");
    assert!(!target.join("source").exists());
}
#[test]
/// Vérifie que le journal relu après redémarrage conserve la possibilité de remise en état.
fn history_survives_restart_and_still_allows_rollback() {
    let fixture = Fixture::new();
    let path = fixture.0.join("data/state.json");
    let engine = Engine::persistent(path.clone()).unwrap();
    engine
        .request(r#"{"command":"configure","keep_history":true}"#)
        .unwrap();
    fixture.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
    let loaded = Engine::persistent(path).unwrap();
    let state = loaded.request(r#"{"command":"status"}"#).unwrap();
    assert_eq!(state["jobs"].as_array().unwrap().len(), 1);
    let result = request_undo(&loaded, vec![1]);
    assert_eq!(result["jobs"][0]["state"], "undone", "{result}");
    assert!(!fixture.0.join("dest/source").exists());
    loaded
        .request(r#"{"command":"configure","keep_history":false}"#)
        .unwrap();
    loaded.request(r#"{"command":"shutdown"}"#).unwrap();
    let loaded = Engine::persistent(fixture.0.join("data/state.json")).unwrap();
    assert!(loaded.request(r#"{"command":"status"}"#).unwrap()["jobs"]
        .as_array()
        .unwrap()
        .is_empty());
}
#[test]
/// Vérifie que un arrêt entre sauvegarde et publication ne rend pas l’ancienne destination irrécupérable.
fn interrupted_journal_can_recover_a_backup_before_publication() {
    let fixture = Fixture::new();
    let engine = Engine::new();
    fs::create_dir_all(fixture.0.join("dest/source/sub")).unwrap();
    let target = fixture.0.join("dest/source/sub/été\n.txt");
    fs::write(&target, b"ancien").unwrap();
    fixture.enqueue(&engine);
    wait(&engine, |s| {
        s["jobs"][0]["entries"]
            .as_array()
            .unwrap()
            .iter()
            .any(|e| e["state"] == "conflict")
    });
    engine.request(r#"{"command":"cancel","id":1}"#).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "cancelled");
    let index = {
        engine.shared.state.lock().unwrap().jobs[0]
            .entries
            .iter()
            .position(|e| e.kind == "file")
            .unwrap()
    };
    let dir = undo::private_directory(target.parent().unwrap(), "undo-test").unwrap();
    let backup = dir.join("original");
    let original = undo::Stamp::read(&target).unwrap();
    let record = undo::Record {
        backup: Some(backup.clone()),
        original: Some(original),
        ..Default::default()
    };
    engine.record(1, index, record).unwrap();
    publish_new(&target, &backup).unwrap();
    let result = request_undo(&engine, vec![1]);
    assert_eq!(result["jobs"][0]["state"], "undone", "{result}");
    assert_eq!(fs::read(&target).unwrap(), b"ancien");
    assert!(!dir.exists());
}
#[test]
/// Vérifie que les lots qui se remplacent mutuellement sont restaurés du plus récent au plus ancien.
fn multiple_undo_reverses_overlapping_jobs_in_reverse_order() {
    let fixture = Fixture::new();
    let engine = Engine::new();
    fixture.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    fixture.enqueue(&engine);
    wait(&engine, |s| {
        s["jobs"][1]["entries"]
            .as_array()
            .unwrap()
            .iter()
            .any(|e| e["state"] == "conflict")
    });
    engine
        .request(r#"{"command":"resolve","id":2,"action":"replace","all":true}"#)
        .unwrap();
    wait(&engine, |s| s["jobs"][1]["state"] == "done");
    let result = request_undo(&engine, vec![1, 2]);
    assert!(
        result["jobs"]
            .as_array()
            .unwrap()
            .iter()
            .all(|j| j["state"] == "undone"),
        "{result}"
    );
    assert!(!fixture.0.join("dest/source").exists());
}
#[test]
/// Vérifie que la restauration arrête un déplacement actif et nettoie son résultat partiel.
fn rollback_cancels_an_active_move_and_removes_partial_output() {
    let fixture = Fixture::new();
    let engine = Engine::new();
    let source = fixture.0.join("source/large");
    fs::File::create(&source)
        .unwrap()
        .set_len(64 * 1024 * 1024)
        .unwrap();
    engine.request(&json!({"command":"enqueue","sources":[fixture.0.join("source")],"destination":fixture.0.join("dest"),"mode":"move","verify_md5":true}).to_string()).unwrap();
    wait(&engine, |s| {
        s["jobs"][0]["entries"]
            .as_array()
            .unwrap()
            .iter()
            .any(|e| e["copied"].as_u64().unwrap() > 0 && e["state"] != "done")
    });
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    let result = request_undo(&engine, vec![1]);
    assert_eq!(result["jobs"][0]["state"], "undone", "{result}");
    assert_eq!(fs::metadata(&source).unwrap().len(), 64 * 1024 * 1024);
    assert!(!fixture.0.join("dest/source").exists());
    assert!(fixture.0.join("source/sub/été\n.txt").exists());
}
#[test]
/// Vérifie que un déplacement interrompu est récupérable mais ne redémarre pas automatiquement.
fn history_recovers_interrupted_moves_without_restarting_them() {
    let fixture = Fixture::new();
    let engine = Engine::new();
    engine.request(&json!({"command":"enqueue","sources":[fixture.0.join("source")],"destination":fixture.0.join("dest"),"mode":"move"}).to_string()).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    let mut bytes = engine.request(r#"{"command":"status"}"#).unwrap();
    bytes["jobs"][0]["state"] = json!("running");
    bytes["keep_history"] = json!(true);
    let journal = fixture.0.join("recovery.json");
    fs::write(&journal, serde_json::to_vec(&bytes).unwrap()).unwrap();
    let loaded = Engine::persistent(journal).unwrap();
    let state = loaded.request(r#"{"command":"status"}"#).unwrap();
    assert_eq!(state["jobs"][0]["state"], "error");
    assert_eq!(state["jobs"][0]["recovery"], true);
    assert!(!fixture.0.join("source").exists());
    let result = request_undo(&loaded, vec![1]);
    assert_eq!(result["jobs"][0]["state"], "undone", "{result}");
    assert!(fixture.0.join("source/sub/été\n.txt").exists());
    assert!(!fixture.0.join("dest/source").exists());
}
#[test]
/// Vérifie que le nettoyage retire les sauvegardes sans altérer les résultats publiés.
fn clearing_history_releases_backups_without_changing_final_files() {
    let fixture = Fixture::new();
    let engine = Engine::new();
    fs::create_dir_all(fixture.0.join("dest/source/sub")).unwrap();
    let target = fixture.0.join("dest/source/sub/été\n.txt");
    fs::write(&target, b"ancien").unwrap();
    fixture.enqueue(&engine);
    wait(&engine, |s| {
        s["jobs"][0]["entries"]
            .as_array()
            .unwrap()
            .iter()
            .any(|e| e["state"] == "conflict")
    });
    engine
        .request(r#"{"command":"resolve","id":1,"action":"replace","all":true}"#)
        .unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert!(fs::read_dir(target.parent().unwrap()).unwrap().any(|e| e
        .unwrap()
        .file_name()
        .to_string_lossy()
        .starts_with(".gad-undo-")));
    engine.request(r#"{"command":"clear"}"#).unwrap();
    assert_eq!(fs::read(&target).unwrap(), b"bonjour\0monde");
    assert_eq!(fs::read_dir(target.parent().unwrap()).unwrap().count(), 1);
}
#[test]
/// Vérifie que une sauvegarde absente ne provoque pas la suppression du seul résultat restant.
fn rollback_does_not_remove_a_copy_when_the_replacement_backup_is_missing() {
    let fixture = Fixture::new();
    let engine = Engine::new();
    fs::create_dir_all(fixture.0.join("dest/source/sub")).unwrap();
    let target = fixture.0.join("dest/source/sub/été\n.txt");
    fs::write(&target, b"ancien").unwrap();
    fixture.enqueue(&engine);
    wait(&engine, |s| {
        s["jobs"][0]["entries"]
            .as_array()
            .unwrap()
            .iter()
            .any(|e| e["state"] == "conflict")
    });
    engine
        .request(r#"{"command":"resolve","id":1,"action":"replace","all":true}"#)
        .unwrap();
    let state = wait(&engine, |s| s["jobs"][0]["state"] == "done");
    let backup = state["jobs"][0]["entries"]
        .as_array()
        .unwrap()
        .iter()
        .find_map(|e| e["undo"]["backup"].as_str())
        .unwrap();
    fs::remove_file(backup).unwrap();
    assert_eq!(
        request_undo(&engine, vec![1])["jobs"][0]["state"],
        "undo_error"
    );
    assert_eq!(fs::read(&target).unwrap(), b"bonjour\0monde");
}
#[test]
/// Vérifie que le retour inter-volumes récupère la source et l’ancienne cible remplacée.
fn rollback_move_across_filesystems_restores_original_and_replaced_target() {
    if !Path::new("/dev/shm").is_dir() {
        return;
    }
    let fixture = Fixture::new();
    if fs::metadata(&fixture.0).unwrap().dev() == fs::metadata("/dev/shm").unwrap().dev() {
        return;
    }
    let external = Fixture(PathBuf::from("/dev/shm").join(format!(
        "gad-crossfs-{}-{}",
        std::process::id(),
        NEXT.fetch_add(1, Ordering::Relaxed)
    )));
    fs::create_dir(&external.0).unwrap();
    fs::create_dir_all(external.0.join("source/sub")).unwrap();
    fs::write(
        external.0.join("source/sub/été\n.txt"),
        b"ancien sur autre volume",
    )
    .unwrap();
    let engine = Engine::new();
    engine.request(&json!({"command":"enqueue","sources":[fixture.0.join("source")],"destination":external.0,"mode":"move"}).to_string()).unwrap();
    wait(&engine, |s| {
        s["jobs"][0]["entries"]
            .as_array()
            .unwrap()
            .iter()
            .any(|e| e["state"] == "conflict")
    });
    engine
        .request(r#"{"command":"resolve","id":1,"action":"replace","all":true}"#)
        .unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert!(!fixture.0.join("source").exists());
    let result = request_undo(&engine, vec![1]);
    assert_eq!(result["jobs"][0]["state"], "undone", "{result}");
    assert_eq!(
        fs::read(fixture.0.join("source/sub/été\n.txt")).unwrap(),
        b"bonjour\0monde"
    );
    assert_eq!(
        fs::read(external.0.join("source/sub/été\n.txt")).unwrap(),
        b"ancien sur autre volume"
    );
}
#[test]
/// Vérifie que un ancien historique reste consultable sans promettre une restauration impossible.
fn legacy_history_is_readable_but_never_claims_to_undo_unjournaled_transfers() {
    let fixture = Fixture::new();
    let engine = Engine::new();
    fixture.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    let mut state = engine.request(r#"{"command":"status"}"#).unwrap();
    state["keep_history"] = json!(true);
    state["jobs"][0]
        .as_object_mut()
        .unwrap()
        .remove("journaled");
    for e in state["jobs"][0]["entries"].as_array_mut().unwrap() {
        e.as_object_mut().unwrap().remove("undo");
    }
    let path = fixture.0.join("legacy.json");
    fs::write(&path, serde_json::to_vec(&state).unwrap()).unwrap();
    let loaded = Engine::persistent(path).unwrap();
    assert_eq!(
        loaded.request(r#"{"command":"status"}"#).unwrap()["jobs"][0]["state"],
        "done"
    );
    assert!(loaded
        .request(r#"{"command":"undo_many","ids":[1]}"#)
        .unwrap_err()
        .contains("older version"));
    assert_eq!(
        fs::read(fixture.0.join("dest/source/sub/été\n.txt")).unwrap(),
        b"bonjour\0monde"
    );
}
#[test]
/// Vérifie le nettoyage des temporaires de restauration Gad.
fn cleanup_gad_restore_staging() {
    let fixture = Fixture::new();
    let engine = Engine::new();
    fixture.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    let dir = fixture.0.join(".gad-restore-test");
    fs::create_dir(&dir).unwrap();
    let temp = dir.join("file");
    fs::write(&temp, b"ancien temporaire").unwrap();
    let index = engine.shared.state.lock().unwrap().jobs[0]
        .entries
        .iter()
        .position(|e| e.kind == "file")
        .unwrap();
    let mut record = engine.shared.state.lock().unwrap().jobs[0].entries[index]
        .undo
        .clone()
        .unwrap();
    record.restore_staging = Some(temp.clone());
    record.restored_source = Some(undo::Stamp::read(&temp).unwrap());
    engine.record(1, index, record).unwrap();
    assert_eq!(request_undo(&engine, vec![1])["jobs"][0]["state"], "undone");
    assert!(!dir.exists());
}
#[test]
/// Vérifie que tout annuler fonctionne sans sélection et préserve les fichiers déjà terminés.
fn cancel_all_is_independent_of_selection_and_keeps_completed_files() {
    let fixture = Fixture::new();
    let engine = Engine::new();
    fixture.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    fixture.enqueue(&engine);
    fixture.enqueue_distinct(&engine, 2);
    wait(&engine, |s| s["jobs"][2]["state"] == "waiting");
    engine.request(r#"{"command":"cancel_all"}"#).unwrap();
    let state = wait(&engine, |s| {
        s["jobs"][1]["state"] == "cancelled" && s["jobs"][2]["state"] == "cancelled"
    });
    assert_eq!(state["jobs"][0]["state"], "done");
    assert_eq!(
        fs::read(fixture.0.join("dest/source/sub/été\n.txt")).unwrap(),
        b"bonjour\0monde"
    );
}
#[test]
/// Vérifie que suppr peut retirer un lot en attente pendant la pause sans affecter son voisin.
fn delete_waiting_group_removes_only_that_group_even_while_paused() {
    let fixture = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    fixture.enqueue(&engine);
    fixture.enqueue_distinct(&engine, 2);
    wait(&engine, |s| s["jobs"][1]["state"] == "waiting");
    engine
        .request(r#"{"command":"delete_selection","jobs":[1],"entries":[]}"#)
        .unwrap();
    let state = engine.request(r#"{"command":"status"}"#).unwrap();
    assert_eq!(state["jobs"].as_array().unwrap().len(), 1);
    assert_eq!(state["jobs"][0]["id"], 2);
    assert_eq!(state["jobs"][0]["state"], "waiting");
    assert!(!fixture.0.join("dest/source").exists());
    assert!(fixture.0.join("source/sub/été\n.txt").exists());
    engine.request(r#"{"command":"cancel_all"}"#).unwrap();
}
#[test]
/// Vérifie que les lignes retirées de la file ne sont pas supprimées de la source d’un déplacement.
fn delete_pending_files_and_directory_preserves_them_during_move() {
    let fixture = Fixture::new();
    let engine = Engine::new();
    fs::write(fixture.0.join("source/z.txt"), "a déplacer".as_bytes()).unwrap();
    fs::create_dir(fixture.0.join("source/vide")).unwrap();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    engine.request(&json!({"command":"enqueue","sources":[fixture.0.join("source")],"destination":fixture.0.join("dest"),"mode":"move"}).to_string()).unwrap();
    let state = wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    let selected = state["jobs"][0]["entries"]
        .as_array()
        .unwrap()
        .iter()
        .enumerate()
        .filter(|(_, e)| {
            e["source"].as_str().unwrap().ends_with("été\n.txt")
                || e["source"].as_str().unwrap().ends_with("/vide")
        })
        .map(|(index, _)| json!({"job":1,"index":index}))
        .collect::<Vec<_>>();
    engine
        .request(&json!({"command":"delete_selection","jobs":[],"entries":selected}).to_string())
        .unwrap();
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    let state = wait(&engine, |s| {
        terminal(s["jobs"][0]["state"].as_str().unwrap())
    });
    assert_eq!(state["jobs"][0]["state"], "cancelled", "{state}");
    assert!(fixture.0.join("source/vide").is_dir());
    assert!(fixture.0.join("source/sub/été\n.txt").is_file());
    assert!(!fixture.0.join("dest/source/sub/été\n.txt").exists());
    assert_eq!(
        fs::read(fixture.0.join("dest/source/z.txt")).unwrap(),
        "a déplacer".as_bytes()
    );
    assert!(!fixture.0.join("source/z.txt").exists());
    assert_eq!(request_undo(&engine, vec![1])["jobs"][0]["state"], "undone");
    assert_eq!(
        fs::read(fixture.0.join("source/z.txt")).unwrap(),
        "a déplacer".as_bytes()
    );
    assert!(!fixture.0.join("dest/source").exists());
}
#[test]
/// Vérifie que suppr et Retour arrière arrêtent le fichier actif tout en laissant finir ses voisins.
fn delete_active_file_cancels_that_file_but_copies_its_siblings() {
    for command in ["delete_selection", "backspace_selection"] {
        let fixture = Fixture::new();
        let engine = Engine::new();
        let large = fixture.0.join("source/large");
        fs::File::create(&large)
            .unwrap()
            .set_len(64 * 1024 * 1024)
            .unwrap();
        engine.request(&json!({"command":"enqueue","sources":[fixture.0.join("source")],"destination":fixture.0.join("dest"),"verify_md5":true}).to_string()).unwrap();
        let state = wait(&engine, |s| {
            s["jobs"][0]["entries"].as_array().unwrap().iter().any(|e| {
                e["source"] == large.to_str().unwrap()
                    && e["copied"].as_u64().unwrap() > 0
                    && e["state"] != "done"
            })
        });
        engine
            .request(r#"{"command":"pause","paused":true}"#)
            .unwrap();
        let index = state["jobs"][0]["entries"]
            .as_array()
            .unwrap()
            .iter()
            .position(|e| e["source"] == large.to_str().unwrap())
            .unwrap();
        engine
            .request(
                &json!({"command":command,"jobs":[],"entries":[{"job":1,"index":index}]})
                    .to_string(),
            )
            .unwrap();
        let state = wait(&engine, |s| {
            s["jobs"][0]["entries"][index]["state"] == "cancelled"
        });
        assert_eq!(state["jobs"][0]["entries"][index]["hidden"], false);
        assert_eq!(state["jobs"][0]["cancel"], false);
        engine
            .request(r#"{"command":"pause","paused":false}"#)
            .unwrap();
        let state = wait(&engine, |s| {
            terminal(s["jobs"][0]["state"].as_str().unwrap())
        });
        assert_eq!(state["jobs"][0]["state"], "cancelled");
        assert!(large.exists());
        assert!(!fixture.0.join("dest/source/large").exists());
        assert_eq!(
            fs::read(fixture.0.join("dest/source/sub/été\n.txt")).unwrap(),
            b"bonjour\0monde"
        );
        engine.request(r#"{"command":"shutdown"}"#).unwrap();
    }
}
#[test]
/// Vérifie que le retrait de lignes terminées masque leur historique sans effacer leurs résultats.
fn delete_completed_rows_hides_history_without_deleting_files_and_persists() {
    let fixture = Fixture::new();
    fs::write(fixture.0.join("source/autre"), b"autre").unwrap();
    let path = fixture.0.join("history.json");
    let engine = Engine::persistent(path.clone()).unwrap();
    engine
        .request(r#"{"command":"configure","keep_history":true}"#)
        .unwrap();
    fixture.enqueue(&engine);
    let state = wait(&engine, |s| s["jobs"][0]["state"] == "done");
    let index = state["jobs"][0]["entries"]
        .as_array()
        .unwrap()
        .iter()
        .position(|e| e["kind"] == "file")
        .unwrap();
    let destination = PathBuf::from(
        state["jobs"][0]["entries"][index]["destination"]
            .as_str()
            .unwrap(),
    );
    engine
        .request(
            &json!({"command":"delete_selection","jobs":[],"entries":[{"job":1,"index":index}]})
                .to_string(),
        )
        .unwrap();
    assert!(destination.exists());
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
    let loaded = Engine::persistent(path).unwrap();
    assert_eq!(
        loaded.request(r#"{"command":"status"}"#).unwrap()["jobs"][0]["entries"][index]["hidden"],
        true
    );
    loaded
        .request(r#"{"command":"delete_selection","jobs":[1],"entries":[]}"#)
        .unwrap();
    assert!(loaded.request(r#"{"command":"status"}"#).unwrap()["jobs"]
        .as_array()
        .unwrap()
        .is_empty());
    assert!(destination.exists());
}
#[test]
/// Vérifie que une sélection contenant un indice invalide est refusée sans mutation partielle.
fn invalid_delete_selection_has_no_side_effects() {
    let fixture = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    fixture.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    assert!(engine
        .request(r#"{"command":"delete_selection","jobs":[1],"entries":[{"job":1,"index":999}]}"#)
        .is_err());
    let state = engine.request(r#"{"command":"status"}"#).unwrap();
    assert_eq!(state["jobs"][0]["state"], "waiting");
    assert_eq!(state["jobs"][0]["cancel"], false);
    engine.request(r#"{"command":"cancel_all"}"#).unwrap();
}
#[test]
/// Vérifie que deux réservations indépendantes avancent ensemble et un disque commun reste en attente.
fn parallel_workers_run_two_independent_jobs_and_keep_shared_disks_queued() {
    let fixtures = [Fixture::new(), Fixture::new(), Fixture::new()];
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    for f in &fixtures {
        fs::create_dir_all(f.0.join("dest/source/sub")).unwrap();
        fs::write(f.0.join("dest/source/sub/été\n.txt"), b"existing").unwrap();
        f.enqueue(&engine);
    }
    wait(&engine, |s| {
        s["jobs"]
            .as_array()
            .unwrap()
            .iter()
            .all(|j| j["state"] == "waiting")
    });
    {
        let mut state = engine.shared.state.lock().unwrap();
        state.jobs[0].disks = Some(["A".into(), "B".into()].into());
        state.jobs[1].disks = Some(["C".into(), "D".into()].into());
        state.jobs[2].disks = Some(["A".into(), "B".into()].into());
    }
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    wait(&engine, |s| {
        s["jobs"][0]["entries"]
            .as_array()
            .unwrap()
            .iter()
            .any(|e| e["state"] == "conflict")
            && s["jobs"][1]["entries"]
                .as_array()
                .unwrap()
                .iter()
                .any(|e| e["state"] == "conflict")
    });
    assert_eq!(engine.shared.state.lock().unwrap().active.len(), 2);
    assert_eq!(
        engine.request(r#"{"command":"status"}"#).unwrap()["jobs"][2]["state"],
        "waiting"
    );
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    engine.request(r#"{"command":"cancel_all"}"#).unwrap();
    wait(&engine, |s| {
        s["jobs"]
            .as_array()
            .unwrap()
            .iter()
            .all(|j| terminal(j["state"].as_str().unwrap()))
    });
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}
#[test]
/// Vérifie que l’ordonnanceur respecte les dépendances de disques, les inconnues et les restaurations exclusives.
fn scheduler_respects_shared_disks_unknown_devices_fifo_and_exclusive_rollback() {
    let fixture = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    for number in 0..3 {
        fixture.enqueue_distinct(&engine, number);
    }
    wait(&engine, |s| s["jobs"][2]["state"] == "waiting");
    {
        let mut state = engine.shared.state.lock().unwrap();
        state.paused = false;
        state.jobs[0].disks = Some(["A".into(), "B".into()].into());
        state.jobs[1].disks = Some(["B".into(), "C".into()].into());
        state.jobs[2].disks = Some(["C".into(), "D".into()].into());
        let first = state.jobs[0].disks.clone();
        state.active.insert(1, first);
        assert_eq!(scheduling::next(&mut state), None); // C is reserved by earlier pending B→C.
        state.jobs[2].disks = Some(["D".into(), "E".into()].into());
        assert_eq!(scheduling::next(&mut state), Some((3, false)));
        state.jobs[2].disks = None;
        assert_eq!(scheduling::next(&mut state), None);
        state.jobs[1].undo_requested = true;
        assert_eq!(scheduling::next(&mut state), None);
        state.active.clear();
        state.paused = true;
        assert_eq!(scheduling::next(&mut state), Some((2, true)));
        state.jobs[1].undo_requested = false;
        state.paused = true;
    }
    engine.request(r#"{"command":"cancel_all"}"#).unwrap();
}
#[test]
/// Vérifie que une topologie sysfs simulée relie partitions et volumes logiques au même disque.
fn disk_detection_collapses_partitions_and_device_mapper_and_rejects_unknown() {
    let f = Fixture::new();
    let root = f.0.join("sys");
    for name in ["disk", "other", "dm"] {
        fs::create_dir_all(root.join(name).join("slaves")).unwrap();
    }
    fs::create_dir_all(root.join("hardware-a")).unwrap();
    fs::create_dir_all(root.join("hardware-b")).unwrap();
    symlink(root.join("hardware-a"), root.join("disk/device")).unwrap();
    symlink(root.join("hardware-b"), root.join("other/device")).unwrap();
    fs::create_dir(root.join("disk/part1")).unwrap();
    fs::write(root.join("disk/part1/partition"), b"1").unwrap();
    symlink(root.join("disk/part1"), root.join("dm/slaves/part1")).unwrap();
    let disk = scheduling::physical(&root.join("disk"), 0);
    assert!(disk.is_some());
    assert_eq!(disk, scheduling::physical(&root.join("disk/part1"), 0));
    assert_eq!(disk, scheduling::physical(&root.join("dm"), 0));
    assert_ne!(disk, scheduling::physical(&root.join("other"), 0));
    assert_eq!(scheduling::physical(&root.join("missing"), 0), None);
    fs::create_dir_all(root.join("loop/slaves")).unwrap();
    assert_eq!(scheduling::physical(&root.join("loop"), 0), None);
}
#[test]
/// Vérifie que retour arrière restaure un fichier isolé avant de permettre la restauration du reste du lot.
fn backspace_restores_only_selected_copied_or_moved_file_then_whole_group() {
    for mode in ["copy", "move"] {
        let fixture = Fixture::new();
        fs::write(fixture.0.join("source/keep.txt"), b"keep").unwrap();
        let engine = Engine::new();
        engine.request(&json!({"command":"enqueue","mode":mode,"sources":[fixture.0.join("source")],"destination":fixture.0.join("dest")}).to_string()).unwrap();
        let state = wait(&engine, |s| s["jobs"][0]["state"] == "done");
        let index = state["jobs"][0]["entries"]
            .as_array()
            .unwrap()
            .iter()
            .position(|e| e["source"].as_str().unwrap().ends_with("été\n.txt"))
            .unwrap();
        engine.request(&json!({"command":"backspace_selection","jobs":[],"entries":[{"job":1,"index":index}]}).to_string()).unwrap();
        let state = wait(&engine, |s| {
            s["jobs"][0]["entries"][index]["state"] == "undone"
                && s["jobs"][0]["undo_requested"] == false
        });
        assert_eq!(state["jobs"][0]["state"], "done");
        assert_eq!(
            fs::read(fixture.0.join("source/sub/été\n.txt")).unwrap(),
            b"bonjour\0monde"
        );
        assert!(!fixture.0.join("dest/source/sub/été\n.txt").exists());
        assert_eq!(
            fs::read(fixture.0.join("dest/source/keep.txt")).unwrap(),
            b"keep"
        );
        engine
            .request(r#"{"command":"backspace_selection","jobs":[1],"entries":[]}"#)
            .unwrap();
        wait(&engine, |s| s["jobs"][0]["state"] == "undone");
        assert!(!fixture.0.join("dest/source").exists());
        assert_eq!(
            fs::read(fixture.0.join("source/keep.txt")).unwrap(),
            b"keep"
        );
        engine.request(r#"{"command":"shutdown"}"#).unwrap();
    }
}
#[test]
/// Vérifie que retour arrière retire l’attente et refuse d’effacer un résultat modifié.
fn backspace_waiting_rows_remove_them_and_modified_completed_files_are_preserved() {
    let fixture = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    fixture.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    engine
        .request(r#"{"command":"backspace_selection","jobs":[1],"entries":[]}"#)
        .unwrap();
    assert!(engine.request(r#"{"command":"status"}"#).unwrap()["jobs"]
        .as_array()
        .unwrap()
        .is_empty());
    fixture.enqueue(&engine);
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    let state = wait(&engine, |s| s["jobs"][0]["state"] == "done");
    let index = state["jobs"][0]["entries"]
        .as_array()
        .unwrap()
        .iter()
        .position(|e| e["kind"] == "file")
        .unwrap();
    let dest = state["jobs"][0]["entries"][index]["destination"]
        .as_str()
        .unwrap();
    fs::write(dest, b"modified after copy").unwrap();
    engine
        .request(
            &json!({"command":"backspace_selection","jobs":[],"entries":[{"job":2,"index":index}]})
                .to_string(),
        )
        .unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "undo_error");
    assert_eq!(fs::read(dest).unwrap(), b"modified after copy");
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}
#[test]
/// Vérifie que un collage valide reprend la file, mais un collage refusé ne change pas la pause.
fn background_paste_resumes_paused_queue_but_invalid_requests_do_not() {
    let fixture = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    let invalid = engine.request(&json!({"command":"enqueue","background":true,"sources":[fixture.0.join("missing")],"destination":fixture.0.join("dest")}).to_string());
    invalid.unwrap();
    wait(&engine, |state| state["jobs"][0]["state"] == "error");
    engine.request(r#"{"command":"clear"}"#).unwrap();
    assert_eq!(
        engine.request(r#"{"command":"status"}"#).unwrap()["paused"],
        true
    );
    engine.request(&json!({"command":"enqueue","background":true,"sources":[fixture.0.join("source")],"destination":fixture.0.join("dest")}).to_string()).unwrap();
    let state = wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert_eq!(state["paused"], false);
    assert_eq!(
        fs::read(fixture.0.join("dest/source/sub/été\n.txt")).unwrap(),
        b"bonjour\0monde"
    );
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}
#[test]
/// Vérifie que la restauration globale couvre les lots terminés et en attente sans sélection.
fn undo_all_restores_completed_and_pending_operations_without_selection() {
    let f = Fixture::new();
    let other = Fixture::new();
    let engine = Engine::new();
    engine.request(r#"{"command":"undo_all"}"#).unwrap();
    engine.request(&json!({"command":"enqueue","mode":"move","sources":[f.0.join("source")],"destination":f.0.join("dest")}).to_string()).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    other.enqueue(&engine);
    wait(&engine, |s| s["jobs"][1]["state"] == "waiting");
    engine.request(r#"{"command":"undo_all"}"#).unwrap();
    wait(&engine, |s| {
        s["jobs"]
            .as_array()
            .unwrap()
            .iter()
            .all(|j| j["state"] == "undone")
    });
    assert!(!f.0.join("dest/source").exists());
    assert_eq!(
        fs::read(f.0.join("source/sub/été\n.txt")).unwrap(),
        b"bonjour\0monde"
    );
    assert!(!other.0.join("dest/source").exists());
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}
#[test]
/// Vérifie que après Tout annuler, une restauration ciblée ignore les modifications de fichiers voisins.
fn selected_restore_after_cancel_all_leaves_other_completed_outputs_alone() {
    let f = Fixture::new();
    fs::write(f.0.join("source/keep"), b"keep").unwrap();
    let engine = Engine::new();
    f.enqueue(&engine);
    let state = wait(&engine, |s| s["jobs"][0]["state"] == "done");
    engine.request(r#"{"command":"cancel_all"}"#).unwrap();
    let index = state["jobs"][0]["entries"]
        .as_array()
        .unwrap()
        .iter()
        .position(|e| e["source"].as_str().unwrap().ends_with("été\n.txt"))
        .unwrap();
    // An unrelated output is modified and must not block restoration of this row.
    fs::write(f.0.join("dest/source/keep"), b"modified").unwrap();
    engine
        .request(
            &json!({"command":"undo_selection","jobs":[],"entries":[{"job":1,"index":index}]})
                .to_string(),
        )
        .unwrap();
    wait(&engine, |s| {
        s["jobs"][0]["entries"][index]["state"] == "undone"
            && s["jobs"][0]["undo_requested"] == false
    });
    assert!(!f.0.join("dest/source/sub/été\n.txt").exists());
    assert_eq!(fs::read(f.0.join("dest/source/keep")).unwrap(), b"modified");
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}
#[test]
/// Vérifie que la pause d’un lot laisse un autre avancer et sa reprise retire la pause globale.
fn pausing_selected_job_allows_another_job_then_resuming_clears_global_pause() {
    let f = Fixture::new();
    let other = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    f.enqueue(&engine);
    other.enqueue(&engine);
    wait(&engine, |s| s["jobs"][1]["state"] == "waiting");
    engine
        .request(r#"{"command":"pause_selection","jobs":[1],"entries":[],"paused":true}"#)
        .unwrap();
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    let state = wait(&engine, |s| s["jobs"][1]["state"] == "done");
    assert_eq!(state["jobs"][0]["state"], "waiting");
    assert!(!f.0.join("dest/source").exists());
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    engine
        .request(r#"{"command":"pause_selection","jobs":[1],"entries":[],"paused":false}"#)
        .unwrap();
    wait(&engine, |s| {
        s["jobs"][0]["state"] == "done" && s["paused"] == false
    });
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}
#[test]
/// Vérifie que réactiver un déplacement ne recopie pas ses résultats terminés et conserve son journal.
fn retry_cancelled_move_keeps_completed_files_and_preserves_full_rollback() {
    let f = Fixture::new();
    fs::write(f.0.join("source/a"), b"first completed").unwrap();
    fs::write(f.0.join("source/b"), b"new b").unwrap();
    fs::create_dir_all(f.0.join("dest/source")).unwrap();
    fs::write(f.0.join("dest/source/b"), b"old b").unwrap();
    let engine = Engine::new();
    engine.request(&json!({"command":"enqueue","mode":"move","sources":[f.0.join("source")],"destination":f.0.join("dest")}).to_string()).unwrap();
    wait(&engine, |s| {
        s["jobs"][0]["entries"]
            .as_array()
            .unwrap()
            .iter()
            .any(|e| e["state"] == "conflict")
    });
    assert!(!f.0.join("source/a").exists());
    let first_identity = fs::metadata(f.0.join("dest/source/a")).unwrap().ino();
    engine.request(r#"{"command":"cancel_all"}"#).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "cancelled");
    engine
        .request(r#"{"command":"retry_selection","jobs":[1],"entries":[]}"#)
        .unwrap();
    wait(&engine, |s| {
        s["jobs"][0]["entries"]
            .as_array()
            .unwrap()
            .iter()
            .any(|e| e["state"] == "conflict")
    });
    engine
        .request(r#"{"command":"resolve","id":1,"action":"replace","all":true}"#)
        .unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert_eq!(
        fs::metadata(f.0.join("dest/source/a")).unwrap().ino(),
        first_identity
    );
    assert_eq!(fs::read(f.0.join("dest/source/b")).unwrap(), b"new b");
    assert!(!f.0.join("source").exists());
    engine.request(r#"{"command":"undo_all"}"#).unwrap();
    let state = wait(&engine, |s| {
        s["jobs"][0]["state"] == "undone" || s["jobs"][0]["state"] == "undo_error"
    });
    assert_eq!(state["jobs"][0]["state"], "undone", "{state}");
    assert_eq!(fs::read(f.0.join("source/a")).unwrap(), b"first completed");
    assert_eq!(fs::read(f.0.join("source/b")).unwrap(), b"new b");
    assert_eq!(fs::read(f.0.join("dest/source/b")).unwrap(), b"old b");
    assert!(!f.0.join("dest/source/a").exists());
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}
#[test]
/// Vérifie que une réactivation complète les sources encore inconnues lors d’une annulation précoce.
fn retry_after_cancellation_before_scan_finishes_discovers_remaining_sources() {
    let f = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    f.enqueue(&engine);
    engine
        .request(r#"{"command":"cancel_selection","jobs":[1],"entries":[]}"#)
        .unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "cancelled");
    engine
        .request(r#"{"command":"retry_selection","jobs":[1],"entries":[]}"#)
        .unwrap();
    let state = wait(&engine, |s| {
        ["done", "undo_error", "error"]
            .iter()
            .any(|v| s["jobs"][0]["state"] == *v)
    });
    assert_eq!(state["jobs"][0]["state"], "done", "{state}");
    assert_eq!(
        fs::read(f.0.join("dest/source/sub/été\n.txt")).unwrap(),
        b"bonjour\0monde"
    );
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}
#[test]
/// Vérifie que la pause ciblée bloque le fichier choisi jusqu’à sa reprise.
fn individual_file_pause_and_resume_preserves_other_rows() {
    let f = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    f.enqueue(&engine);
    let state = wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    let index = state["jobs"][0]["entries"]
        .as_array()
        .unwrap()
        .iter()
        .position(|e| e["kind"] == "file")
        .unwrap();
    engine.request(&json!({"command":"pause_selection","jobs":[],"entries":[{"job":1,"index":index}],"paused":true}).to_string()).unwrap();
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    let state = wait(&engine, |s| {
        s["jobs"][0]["entries"][index]["paused"] == true
            && ["waiting", "running"]
                .contains(&s["jobs"][0]["entries"][index]["state"].as_str().unwrap())
    });
    assert_eq!(state["jobs"][0]["entries"][index]["copied"], 0);
    assert_eq!(state["jobs"][0]["entries"][index]["paused"], true);
    assert!(!f.0.join("dest/source/sub/été\n.txt").exists());
    engine.request(&json!({"command":"pause_selection","jobs":[],"entries":[{"job":1,"index":index}],"paused":false}).to_string()).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}
#[test]
/// Vérifie que la réactivation isolée recrée ses parents sans relancer les autres fichiers annulés.
fn retry_one_cancelled_file_creates_its_parents_without_retrying_siblings() {
    let f = Fixture::new();
    let engine = Engine::new();
    fs::write(f.0.join("source/other"), b"do not retry").unwrap();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    f.enqueue(&engine);
    let state = wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    let index = state["jobs"][0]["entries"]
        .as_array()
        .unwrap()
        .iter()
        .position(|e| e["source"].as_str().unwrap().ends_with("été\n.txt"))
        .unwrap();
    engine.request(r#"{"command":"cancel_all"}"#).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "cancelled");
    engine
        .request(
            &json!({"command":"retry_selection","jobs":[],"entries":[{"job":1,"index":index}]})
                .to_string(),
        )
        .unwrap();
    let state = wait(&engine, |s| {
        s["jobs"][0]["entries"][index]["state"] == "done" || s["jobs"][0]["state"] == "undo_error"
    });
    assert_eq!(
        state["jobs"][0]["entries"][index]["state"], "done",
        "{state}"
    );
    wait(&engine, |s| {
        terminal(s["jobs"][0]["state"].as_str().unwrap())
    });
    assert_eq!(
        fs::read(f.0.join("dest/source/sub/été\n.txt")).unwrap(),
        b"bonjour\0monde"
    );
    assert!(!f.0.join("dest/source/other").exists());
    assert_eq!(request_undo(&engine, vec![1])["jobs"][0]["state"], "undone");
    assert!(!f.0.join("dest/source").exists());
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}
/// Vérifie qu’une réponse portant un indice périmé ne modifie ni le conflit actif ni sa politique.
#[test]
fn conflict_resolution_rejects_stale_entry_before_applying_choice() {
    let fixture = Fixture::new();
    let engine = Engine::new();
    fs::create_dir_all(fixture.0.join("dest/source/sub")).unwrap();
    fs::write(
        fixture.0.join("dest/source/sub/été\n.txt"),
        b"destination existante",
    )
    .unwrap();
    fixture.enqueue(&engine);
    let state = wait(&engine, |s| {
        s["jobs"][0]["entries"]
            .as_array()
            .unwrap()
            .iter()
            .any(|e| e["state"] == "conflict")
    });
    let index = state["jobs"][0]["entries"]
        .as_array()
        .unwrap()
        .iter()
        .position(|e| e["state"] == "conflict")
        .unwrap();
    assert!(engine
        .request(r#"{"command":"resolve","id":1,"index":0,"action":"replace","all":true}"#)
        .unwrap_err()
        .contains("no longer pending"));
    let state = engine.request(r#"{"command":"status"}"#).unwrap();
    assert_eq!(state["jobs"][0]["policy"], "ask");
    assert_eq!(state["jobs"][0]["entries"][index]["state"], "conflict");
    engine
        .request(
            &json!({"command":"resolve","id":1,"index":index,"action":"rename","all":false})
                .to_string(),
        )
        .unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert_eq!(
        fs::read(fixture.0.join("dest/source/sub/été\n.txt")).unwrap(),
        b"destination existante"
    );
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}
/// Vérifie que l’origine lisible d’une source montée reste associée au même lot après relecture du journal.
#[test]
fn source_urls_survive_history_without_changing_copy_paths() {
    let fixture = Fixture::new();
    let history = fixture.0.join("source-labels.json");
    let engine = Engine::persistent(history.clone()).unwrap();
    engine
        .request(r#"{"command":"configure","keep_history":true}"#)
        .unwrap();
    let label = "kdeconnect://phone/android/source";
    engine.request(&json!({"command":"enqueue","sources":[fixture.0.join("source")],"source_urls":[label],"destination":fixture.0.join("dest")}).to_string()).unwrap();
    let state = wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert_eq!(state["jobs"][0]["source_urls"], json!([label]));
    assert_eq!(
        fs::read(fixture.0.join("dest/source/sub/été\n.txt")).unwrap(),
        b"bonjour\0monde"
    );
    assert!(engine.request(&json!({"command":"enqueue","sources":[fixture.0.join("source")],"source_urls":[],"destination":fixture.0.join("dest")}).to_string()).unwrap_err().contains("Inconsistent"));
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
    let loaded = Engine::persistent(history).unwrap();
    assert_eq!(
        loaded.request(r#"{"command":"status"}"#).unwrap()["jobs"][0]["source_urls"],
        json!([label])
    );
    loaded.request(r#"{"command":"shutdown"}"#).unwrap();
}
#[test]
/// Une erreur propre à un dossier vide ne doit pas être écrasée par l’agrégation de ses enfants.
fn failed_directory_never_becomes_done() {
    let f = Fixture::new();
    fs::create_dir(f.0.join("blocked")).unwrap();
    fs::write(f.0.join("dest/blocked"), b"existing").unwrap();
    let engine = Engine::new();
    engine.request(&json!({"command":"enqueue","sources":[f.0.join("blocked")],"destination":f.0.join("dest")}).to_string()).unwrap();
    let state = wait(&engine, |s| {
        terminal(s["jobs"][0]["state"].as_str().unwrap())
    });
    assert_eq!(state["jobs"][0]["state"], "error");
    assert_eq!(state["jobs"][0]["entries"][0]["state"], "error");
    assert_eq!(fs::read(f.0.join("dest/blocked")).unwrap(), b"existing");
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Un journal impossible à publier ne doit pas laisser un lot en analyse sans analyseur.
fn failed_enqueue_save_does_not_leave_a_scanning_job() {
    let f = Fixture::new();
    let history = f.0.join("state.json");
    let engine = Engine::persistent(history.clone()).unwrap();
    fs::remove_file(&history).unwrap();
    fs::create_dir(&history).unwrap();
    assert!(engine.request(&json!({"command":"enqueue","sources":[f.0.join("source")],"destination":f.0.join("dest")}).to_string()).is_err());
    let state = engine.request(r#"{"command":"status"}"#).unwrap();
    assert!(state["jobs"]
        .as_array()
        .unwrap()
        .iter()
        .all(|j| terminal(j["state"].as_str().unwrap())));
    assert!(!f.0.join("dest/source").exists());
    fs::remove_dir(&history).unwrap();
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Un suffixe dépassant NAME_MAX doit produire une erreur au lieu d’une recherche infinie.
fn conflict_rename_with_long_name_terminates_cleanly() {
    let f = Fixture::new();
    let name = format!("{}.txt", "a".repeat(248));
    let source = f.0.join(&name);
    let destination = f.0.join("dest").join(&name);
    fs::write(&source, b"new").unwrap();
    fs::write(&destination, b"old").unwrap();
    let engine = Engine::new();
    engine
        .request(
            &json!({"command":"enqueue","sources":[source],"destination":f.0.join("dest")})
                .to_string(),
        )
        .unwrap();
    wait(&engine, |s| {
        s["jobs"][0]["entries"][0]["state"] == "conflict"
    });
    engine
        .request(r#"{"command":"resolve","id":1,"index":0,"action":"rename"}"#)
        .unwrap();
    let state = wait(&engine, |s| {
        terminal(s["jobs"][0]["state"].as_str().unwrap())
    });
    assert_eq!(state["jobs"][0]["state"], "error");
    assert_eq!(fs::read(&source).unwrap(), b"new");
    assert_eq!(fs::read(&destination).unwrap(), b"old");
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}
#[test]
/// Deux sources homonymes remplacées dans un même lot doivent se défaire en ordre inverse.
fn rollback_same_name_sources_in_one_batch_restores_every_original() {
    for (mode, corrupt) in [
        ("copy", false),
        ("move", false),
        ("copy", true),
        ("move", true),
    ] {
        let f = Fixture::new();
        fs::create_dir(f.0.join("first")).unwrap();
        fs::create_dir(f.0.join("second")).unwrap();
        let first = f.0.join("first/file.txt");
        let second = f.0.join("second/file.txt");
        let target = f.0.join("dest/file.txt");
        fs::write(&first, b"first").unwrap();
        fs::write(&second, b"second").unwrap();
        fs::write(&target, b"old").unwrap();
        let engine = Engine::new();
        engine.request(&json!({"command":"enqueue","mode":mode,"sources":[first,second],"destination":f.0.join("dest")}).to_string()).unwrap();
        wait(&engine, |s| {
            s["jobs"][0]["entries"][0]["state"] == "conflict"
        });
        engine
            .request(r#"{"command":"resolve","id":1,"index":0,"action":"replace","all":true}"#)
            .unwrap();
        let copied = wait(&engine, |s| s["jobs"][0]["state"] == "done");
        assert_eq!(fs::read(&target).unwrap(), b"second");
        if corrupt {
            // Une empreinte de métadonnées identique ne doit pas masquer un changement de contenu.
            let backup = Path::new(
                copied["jobs"][0]["entries"][1]["undo"]["backup"]
                    .as_str()
                    .unwrap(),
            );
            let modified = fs::metadata(backup).unwrap().modified().unwrap();
            fs::write(backup, b"FIRST").unwrap();
            fs::File::open(backup)
                .unwrap()
                .set_times(fs::FileTimes::new().set_modified(modified))
                .unwrap();
        } else {
            // Restaurer uniquement le premier fichier doit préserver le remplacement suivant non sélectionné.
            engine
                .request(
                    r#"{"command":"undo_selection","jobs":[],"entries":[{"job":1,"index":0}]}"#,
                )
                .unwrap();
            wait(&engine, |s| s["jobs"][0]["state"] == "undo_error");
            assert_eq!(fs::read(&target).unwrap(), b"second");
        }
        engine.request(r#"{"command":"undo_all"}"#).unwrap();
        let state = wait(&engine, |s| {
            ["undone", "undo_error"].contains(&s["jobs"][0]["state"].as_str().unwrap())
        });
        if corrupt {
            assert_eq!(state["jobs"][0]["state"], "undo_error", "{state}");
            assert_eq!(fs::read(&target).unwrap(), b"second");
            assert_eq!(first.exists(), mode == "copy");
            assert_eq!(second.exists(), mode == "copy");
            engine.request(r#"{"command":"shutdown"}"#).unwrap();
            continue;
        }
        assert_eq!(state["jobs"][0]["state"], "undone", "{state}");
        assert_eq!(fs::read(&first).unwrap(), b"first");
        assert_eq!(fs::read(&second).unwrap(), b"second");
        assert_eq!(fs::read(&target).unwrap(), b"old");
        engine.request(r#"{"command":"shutdown"}"#).unwrap();
    }
}
#[test]
/// Réactiver une ligne annulée ne doit pas effacer l’erreur d’un fichier voisin non repris.
fn retry_cancelled_row_preserves_other_entry_errors_in_job_status() {
    let f = Fixture::new();
    let first = f.0.join("first.txt");
    let second = f.0.join("second.txt");
    fs::write(&first, b"first").unwrap();
    fs::write(&second, b"second").unwrap();
    fs::create_dir(f.0.join("dest/first.txt")).unwrap();
    fs::write(f.0.join("dest/second.txt"), b"old").unwrap();
    let engine = Engine::new();
    engine
        .request(
            &json!({"command":"enqueue","sources":[first,second],"destination":f.0.join("dest")})
                .to_string(),
        )
        .unwrap();
    wait(&engine, |s| {
        s["jobs"][0]["entries"][0]["state"] == "conflict"
    });
    engine
        .request(r#"{"command":"resolve","id":1,"index":0,"action":"replace"}"#)
        .unwrap();
    wait(&engine, |s| {
        s["jobs"][0]["entries"][1]["state"] == "conflict"
    });
    engine.request(r#"{"command":"cancel_all"}"#).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "cancelled");
    engine
        .request(r#"{"command":"retry_selection","jobs":[],"entries":[{"job":1,"index":1}]}"#)
        .unwrap();
    wait(&engine, |s| {
        s["jobs"][0]["entries"][1]["state"] == "conflict"
    });
    engine
        .request(r#"{"command":"resolve","id":1,"index":1,"action":"rename"}"#)
        .unwrap();
    let state = wait(&engine, |s| {
        terminal(s["jobs"][0]["state"].as_str().unwrap())
    });
    assert_eq!(state["jobs"][0]["entries"][0]["state"], "error");
    assert_eq!(state["jobs"][0]["entries"][1]["state"], "done");
    assert_eq!(state["jobs"][0]["state"], "error");
    assert!(f.0.join("dest/first.txt").is_dir());
    assert_eq!(
        fs::read(f.0.join("dest/second (copie 1).txt")).unwrap(),
        b"second"
    );
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// La diffusion différentielle signale une mutation, une suppression et une resynchronisation sans toucher au journal.
fn incremental_poll_preserves_history_and_marks_only_changed_jobs() {
    let f = Fixture::new();
    let engine = Engine::new();
    f.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    fs::create_dir(f.0.join("other")).unwrap();
    engine.request(&json!({"command":"enqueue","sources":[f.0.join("source")],"destination":f.0.join("other")}).to_string()).unwrap();
    wait(&engine, |s| s["jobs"][1]["state"] == "done");
    let first = engine.poll(None).unwrap();
    let since = first["revision"].as_u64().unwrap();
    assert_eq!(
        first["jobs"],
        engine.request(r#"{"command":"status"}"#).unwrap()["jobs"]
    );
    assert!(first["jobs"][0].get("revision").is_none());
    let idle = engine.poll(Some(since)).unwrap();
    assert!(idle["unchanged"] == true || idle["jobs"].as_array().unwrap().is_empty());
    engine.update(2, |job| job.entries[1].hidden = true);
    let changed = engine.poll(Some(since)).unwrap();
    assert_eq!(changed["jobs"].as_array().unwrap().len(), 1);
    assert_eq!(changed["jobs"][0]["id"], 2);
    assert_eq!(changed["job_ids"], json!([1, 2]));
    let reset = engine.poll(Some(u64::MAX)).unwrap();
    assert_eq!(reset["reset"], true);
    assert_eq!(reset["jobs"].as_array().unwrap().len(), 2);
    let revision = changed["revision"].as_u64().unwrap();
    engine.request(r#"{"command":"clear"}"#).unwrap();
    let cleared = engine.poll(Some(revision)).unwrap();
    assert_eq!(cleared["job_ids"], json!([]));
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Une attente de métadonnées ne bloque ni les commandes ni l’annulation ; sa réponse tardive est sans effet.
fn slow_preparation_keeps_fifo_and_can_be_cancelled_before_io_returns() {
    let f = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    f.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    engine.update(1, |job| {
        job.entries.clear();
        job.state = "scanning".into();
    });
    let (send, receive) = std::sync::mpsc::channel();
    let destination = f.0.join("dest");
    let pending = engine.spawn_preparation(1, true, 1, move || {
        receive.recv().unwrap();
        Ok((destination, Vec::new(), None))
    });
    f.enqueue(&engine);
    wait(&engine, |s| s["jobs"][1]["state"] == "scanning");
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    assert!(!f.0.join("dest/source").exists());
    let state = engine.request(r#"{"command":"status"}"#).unwrap();
    assert_eq!(state["jobs"][1]["state"], "scanning");
    engine.request(r#"{"command":"cancel_all"}"#).unwrap();
    wait(&engine, |s| {
        s["jobs"]
            .as_array()
            .unwrap()
            .iter()
            .all(|job| terminal(job["state"].as_str().unwrap()))
    });
    f.enqueue(&engine);
    wait(&engine, |s| s["jobs"][2]["state"] == "done");
    send.send(()).unwrap();
    pending.join().unwrap();
    let state = engine.request(r#"{"command":"status"}"#).unwrap();
    assert_eq!(state["jobs"][0]["state"], "cancelled");
    assert_eq!(state["jobs"][0]["entries"], json!([]));
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Un collage déjà soumis ne peut pas lever une pause explicitement demandée pendant sa validation.
fn explicit_pause_during_validation_takes_precedence_over_background_start() {
    let f = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    f.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    let mut prepared_entries = Vec::new();
    engine.update(1, |job| {
        prepared_entries = std::mem::take(&mut job.entries);
        job.state = "scanning".into();
    });
    let (send, receive) = std::sync::mpsc::channel();
    let destination = f.0.join("dest");
    let pending = engine.spawn_preparation(1, true, 1, move || {
        receive.recv().unwrap();
        Ok((destination, prepared_entries, None))
    });
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    send.send(()).unwrap();
    pending.join().unwrap();
    let state = engine.request(r#"{"command":"status"}"#).unwrap();
    assert_eq!(state["paused"], true);
    assert_eq!(state["jobs"][0]["state"], "waiting");
    engine.request(r#"{"command":"cancel_all"}"#).unwrap();
    wait(&engine, |s| {
        terminal(s["jobs"][0]["state"].as_str().unwrap())
    });
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
#[ignore = "mesure volontaire de sérialisation sur un historique volumineux"]
/// Compare l’ancien instantané complet et les deltas de transfert sur 10 000 lignes d’historique.
fn benchmark_large_history_polling() {
    use std::time::Instant;
    let f = Fixture::new();
    let engine = Engine::new();
    f.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    {
        let mut state = engine.shared.state.lock().unwrap();
        let template = state.jobs[0].snapshot();
        let entry = template.entries.last().unwrap().clone();
        state.jobs.clear();
        for id in 1..=200 {
            let mut job = template.clone();
            job.id = id;
            job.entries = vec![entry.clone(); 50];
            state.jobs.push(job.into());
        }
    }
    let mut full_bytes = 0;
    let full_time = Instant::now();
    for tick in 0..10 {
        engine.update(1, |job| job.entries[0].copied = tick);
        let data = engine.request(r#"{"command":"status"}"#).unwrap();
        full_bytes += serde_json::to_vec(&data).unwrap().len();
    }
    let full_elapsed = full_time.elapsed();
    let mut since = engine.poll(None).unwrap()["revision"].as_u64();
    let mut delta_bytes = 0;
    let delta_time = Instant::now();
    for tick in 0..10 {
        engine.update(1, |job| job.entries[0].copied = tick);
        let data = engine.poll(since).unwrap();
        since = data["revision"].as_u64();
        delta_bytes += serde_json::to_vec(&data).unwrap().len();
    }
    let delta_elapsed = delta_time.elapsed();
    assert!(delta_bytes < full_bytes / 100);
    eprintln!("Historique 10 000 lignes, 10 actualisations : complet={} octets / {:?}, différentiel={} octets / {:?}",full_bytes,full_elapsed,delta_bytes,delta_elapsed);
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Un lien passé par un alias de son dossier ne doit pas être copié sur son propre chemin réel.
fn review_rejects_symlink_self_copy_through_parent_alias() {
    let f = Fixture::new();
    let source = f.0.join("source/link");
    symlink("missing-target", &source).unwrap();
    symlink(f.0.join("source"), f.0.join("alias")).unwrap();
    let before = fs::symlink_metadata(&source).unwrap().ino();
    let engine = Engine::new();
    engine.request(&json!({"command":"enqueue","sources":[f.0.join("alias/link")],"destination":f.0.join("source")}).to_string()).unwrap();
    let state = wait(&engine, |s| {
        terminal(s["jobs"][0]["state"].as_str().unwrap())
            || s["jobs"][0]["entries"][0]["state"] == "conflict"
    });
    assert_eq!(state["jobs"][0]["state"], "error", "{state}");
    assert_eq!(fs::symlink_metadata(&source).unwrap().ino(), before);
    assert_eq!(
        fs::read_link(&source).unwrap(),
        PathBuf::from("missing-target")
    );
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Réactiver un dossier devenu fichier doit signaler le changement sans produire un dossier vide trompeur.
fn review_retry_refuses_changed_source_kind() {
    for mode in ["copy", "move"] {
        let f = Fixture::new();
        let source = f.0.join("changing");
        fs::create_dir(&source).unwrap();
        let engine = Engine::new();
        engine
            .request(r#"{"command":"pause","paused":true}"#)
            .unwrap();
        engine.request(&json!({"command":"enqueue","mode":mode,"sources":[source],"destination":f.0.join("dest")}).to_string()).unwrap();
        wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
        engine.request(r#"{"command":"cancel_all"}"#).unwrap();
        wait(&engine, |s| s["jobs"][0]["state"] == "cancelled");
        fs::remove_dir(&source).unwrap();
        fs::write(&source, b"replacement file").unwrap();
        engine
            .request(r#"{"command":"retry_selection","jobs":[1],"entries":[]}"#)
            .unwrap();
        let state = wait(&engine, |s| {
            terminal(s["jobs"][0]["state"].as_str().unwrap())
        });
        assert_eq!(state["jobs"][0]["state"], "undo_error", "{state}");
        assert!(state["jobs"][0]["error"]
            .as_str()
            .unwrap()
            .contains("Type de source modifié"));
        assert!(!f.0.join("dest/changing").exists());
        assert_eq!(fs::read(&source).unwrap(), b"replacement file");
        engine.request(r#"{"command":"shutdown"}"#).unwrap();
    }
}

#[test]
/// La taille et les compteurs utilisent la version effectivement copiée après une attente dans la file.
fn review_refreshes_file_size_when_queued_source_changes() {
    for contents in [b"new".as_slice(), b"longer replacement".as_slice()] {
        let f = Fixture::new();
        let source = f.0.join("changing.txt");
        fs::write(&source, b"initial").unwrap();
        let engine = Engine::new();
        engine
            .request(r#"{"command":"pause","paused":true}"#)
            .unwrap();
        engine
            .request(
                &json!({"command":"enqueue","sources":[source],"destination":f.0.join("dest")})
                    .to_string(),
            )
            .unwrap();
        wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
        fs::write(&source, contents).unwrap();
        engine
            .request(r#"{"command":"pause","paused":false}"#)
            .unwrap();
        let state = wait(&engine, |s| {
            terminal(s["jobs"][0]["state"].as_str().unwrap())
        });
        assert_eq!(state["jobs"][0]["state"], "done", "{state}");
        assert_eq!(
            state["jobs"][0]["entries"][0]["size"],
            contents.len() as u64
        );
        assert_eq!(
            state["jobs"][0]["entries"][0]["copied"],
            contents.len() as u64
        );
        assert_eq!(fs::read(f.0.join("dest/changing.txt")).unwrap(), contents);
        engine.request(r#"{"command":"shutdown"}"#).unwrap();
    }
}

#[test]
/// Une reprise vide ou limitée à l’historique ne doit pas lever la pause d’autres transferts.
fn iterative_noop_selection_never_resumes_other_jobs() {
    let f = Fixture::new();
    let engine = Engine::new();
    f.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    fs::create_dir(f.0.join("other")).unwrap();
    engine.request(&json!({"command":"enqueue","sources":[f.0.join("source")],"destination":f.0.join("other")}).to_string()).unwrap();
    wait(&engine, |s| s["jobs"][1]["state"] == "waiting");
    for command in ["retry_selection", "pause_selection"] {
        for jobs in [json!([]), json!([1])] {
            let response = engine.request(
                &json!({"command":command,"jobs":jobs,"entries":[],"paused":false}).to_string(),
            );
            assert!(
                response.is_err(),
                "Commande sans ligne réactivable acceptée : {command}"
            );
            assert_eq!(
                engine.request(r#"{"command":"status"}"#).unwrap()["paused"],
                true
            );
            assert!(!f.0.join("other/source").exists());
        }
    }
    engine.request(r#"{"command":"cancel_all"}"#).unwrap();
    wait(&engine, |s| s["jobs"][1]["state"] == "cancelled");
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Un dossier remplacé ou supprimé pendant l’attente ne doit pas produire une copie vide annoncée OK.
fn iterative_revalidates_queued_directory_identity() {
    for replacement in ["missing", "file", "directory"] {
        let f = Fixture::new();
        let source = f.0.join("changing");
        fs::create_dir(&source).unwrap();
        let engine = Engine::new();
        engine
            .request(r#"{"command":"pause","paused":true}"#)
            .unwrap();
        engine
            .request(
                &json!({"command":"enqueue","sources":[source],"destination":f.0.join("dest")})
                    .to_string(),
            )
            .unwrap();
        wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
        // Conserver l’ancien inode vivant rend la substitution déterministe, même sur un système réutilisant les inodes.
        fs::rename(&source, f.0.join("original-directory")).unwrap();
        if replacement == "file" {
            fs::write(&source, b"new file").unwrap();
        }
        if replacement == "directory" {
            fs::create_dir(&source).unwrap();
        }
        engine
            .request(r#"{"command":"pause","paused":false}"#)
            .unwrap();
        let state = wait(&engine, |s| {
            terminal(s["jobs"][0]["state"].as_str().unwrap())
        });
        assert_eq!(state["jobs"][0]["state"], "error", "{state}");
        assert!(!f.0.join("dest/changing").exists());
        engine.request(r#"{"command":"shutdown"}"#).unwrap();
    }
}

#[test]
/// Un échec du journal après réservation d’un dossier temporaire doit libérer ce dossier.
fn iterative_directory_staging_is_cleaned_when_journaling_fails() {
    let f = Fixture::new();
    let history = f.0.join("state.json");
    let engine = Engine::persistent(history.clone()).unwrap();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    f.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    engine.update(1, |job| job.state = TransferState::Running);
    engine.shared.state.lock().unwrap().paused = false;
    let entry = engine.shared.state.lock().unwrap().jobs[0].entries[0].clone();
    fs::remove_file(&history).unwrap();
    fs::create_dir(&history).unwrap();
    assert!(engine.copy_entry(1, 0, &entry, false, false).is_err());
    assert_eq!(
        fs::read_dir(f.0.join("dest")).unwrap().count(),
        0,
        "Dossier temporaire abandonné après refus du journal"
    );
    fs::remove_dir(&history).unwrap();
    engine.update(1, |job| job.state = TransferState::Error);
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Vérifie les invariants de restauration sur des arbres fusionnés, avec conflits, liens et redémarrage.
fn iterative_roundtrip_matrix_for_merged_trees_and_persistent_history() {
    fn contents(root: &Path) -> std::collections::BTreeMap<PathBuf, Vec<u8>> {
        fn walk(
            root: &Path,
            path: &Path,
            found: &mut std::collections::BTreeMap<PathBuf, Vec<u8>>,
        ) {
            let meta = fs::symlink_metadata(path).unwrap();
            let relative = path.strip_prefix(root).unwrap().to_path_buf();
            if meta.file_type().is_symlink() {
                let mut data = b"L".to_vec();
                data.extend(fs::read_link(path).unwrap().as_os_str().as_encoded_bytes());
                found.insert(relative, data);
            } else if meta.is_dir() {
                found.insert(relative, b"D".to_vec());
                for child in fs::read_dir(path).unwrap() {
                    walk(root, &child.unwrap().path(), found);
                }
            } else {
                let mut data = b"F".to_vec();
                data.extend(fs::read(path).unwrap());
                found.insert(relative, data);
            }
        }
        let mut found = std::collections::BTreeMap::new();
        walk(root, root, &mut found);
        found
    }
    for policy in ["skip", "rename", "replace"] {
        for mode in ["copy", "move"] {
            for md5 in [false, true] {
                let f = Fixture::new();
                let first = f.0.join("a/tree");
                let second = f.0.join("b/tree");
                let target = f.0.join("dest/tree");
                for (root, data) in [
                    (&first, b"first".as_slice()),
                    (&second, b"second".as_slice()),
                    (&target, b"old".as_slice()),
                ] {
                    fs::create_dir_all(root.join("sub/empty")).unwrap();
                    fs::write(root.join("sub/été.txt"), data).unwrap();
                    symlink("absent", root.join("link")).unwrap();
                }
                fs::write(first.join("only-first"), b"one").unwrap();
                fs::write(second.join("only-second"), b"two").unwrap();
                let before = [
                    contents(&f.0.join("a")),
                    contents(&f.0.join("b")),
                    contents(&f.0.join("dest")),
                ];
                let history = f.0.join("state.json");
                let engine = Engine::persistent(history.clone()).unwrap();
                engine
                    .request(r#"{"command":"configure","keep_history":true}"#)
                    .unwrap();
                engine.request(&json!({"command":"enqueue","mode":mode,"verify_md5":md5,"sources":[first,second],"destination":f.0.join("dest")}).to_string()).unwrap();
                let conflicted = wait(&engine, |s| {
                    s["jobs"][0]["entries"].as_array().is_some_and(|entries| {
                        entries.iter().any(|entry| entry["state"] == "conflict")
                    })
                });
                let index = conflicted["jobs"][0]["entries"]
                    .as_array()
                    .unwrap()
                    .iter()
                    .position(|entry| entry["state"] == "conflict")
                    .unwrap();
                engine.request(&json!({"command":"resolve","id":1,"index":index,"action":policy,"all":true}).to_string()).unwrap();
                let copied = wait(&engine, |s| {
                    terminal(s["jobs"][0]["state"].as_str().unwrap())
                });
                assert_eq!(
                    copied["jobs"][0]["state"], "done",
                    "{policy}/{mode}/{md5}: {copied}"
                );
                engine.request(r#"{"command":"shutdown"}"#).unwrap();
                let loaded = Engine::persistent(history).unwrap();
                loaded.request(r#"{"command":"undo_all"}"#).unwrap();
                let undone = wait(&loaded, |s| {
                    ["undone", "undo_error"].contains(&s["jobs"][0]["state"].as_str().unwrap())
                });
                assert_eq!(
                    undone["jobs"][0]["state"], "undone",
                    "{policy}/{mode}/{md5}: {undone}"
                );
                assert_eq!(
                    [
                        contents(&f.0.join("a")),
                        contents(&f.0.join("b")),
                        contents(&f.0.join("dest"))
                    ],
                    before,
                    "{policy}/{mode}/{md5}"
                );
                loaded.request(r#"{"command":"shutdown"}"#).unwrap();
            }
        }
    }
}

#[test]
/// Un argument vide ne doit jamais désigner implicitement le dossier courant.
fn iterative_rejects_empty_source_and_destination_before_creating_jobs() {
    let f = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    for request in [
        json!({"command":"enqueue","sources":[""],"destination":f.0.join("dest")}),
        json!({"command":"enqueue","sources":[f.0.join("source")],"destination":""}),
    ] {
        let response = engine.request(&request.to_string());
        if response.is_ok() {
            engine.request(r#"{"command":"cancel_all"}"#).unwrap();
        }
        assert!(response.is_err(), "Chemin vide accepté : {request}");
    }
    assert_eq!(
        engine.request(r#"{"command":"status"}"#).unwrap()["jobs"],
        json!([])
    );
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Les fichiers et liens sont renommés, puis remis en place avec leur identité et sans lecture de contenu.
fn fast_move_keeps_inodes_and_undo_restores_tree_and_links() {
    let f = Fixture::local();
    let source = f.0.join("source/sub/été\n.txt");
    let original = fs::metadata(&source).unwrap();
    symlink("missing", f.0.join("source/broken")).unwrap();
    let link_inode = fs::symlink_metadata(f.0.join("source/broken"))
        .unwrap()
        .ino();
    let engine = Engine::new();
    engine.request(&json!({"command":"enqueue","mode":"move","sources":[f.0.join("source")],"destination":f.0.join("dest")}).to_string()).unwrap();
    let completed = wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert_eq!(completed["jobs"][0]["fast_move"], true);
    let result = fs::metadata(f.0.join("dest/source/sub/été\n.txt")).unwrap();
    assert_eq!(
        (original.dev(), original.ino(), original.mode()),
        (result.dev(), result.ino(), result.mode())
    );
    assert_eq!(
        fs::symlink_metadata(f.0.join("dest/source/broken"))
            .unwrap()
            .ino(),
        link_inode
    );
    assert!(!f.0.join("source").exists());
    for entry in completed["jobs"][0]["entries"].as_array().unwrap() {
        if entry["kind"] != "directory" {
            assert_eq!(entry["undo"]["rename_move"], true);
            assert!(entry["undo"]["digest"].is_null());
            assert!(entry["md5_source"].is_null());
        }
    }
    engine.request(r#"{"command":"undo_all"}"#).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "undone");
    assert_eq!(fs::metadata(&source).unwrap().ino(), original.ino());
    assert_eq!(fs::read(&source).unwrap(), b"bonjour\0monde");
    assert_eq!(
        fs::read_link(f.0.join("source/broken")).unwrap(),
        PathBuf::from("missing")
    );
    assert!(!f.0.join("dest/source").exists());
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Une copie bloquée sur un conflit garde son disque, mais un renommage indépendant peut terminer.
fn fast_move_runs_beside_copy_on_same_volume() {
    let f = Fixture::local();
    fs::create_dir(f.0.join("other")).unwrap();
    fs::create_dir(f.0.join("arrivals")).unwrap();
    let moving = f.0.join("other/quick.txt");
    fs::write(&moving, b"quick").unwrap();
    let inode = fs::metadata(&moving).unwrap().ino();
    fs::create_dir_all(f.0.join("dest/source/sub")).unwrap();
    fs::write(f.0.join("dest/source/sub/été\n.txt"), b"old").unwrap();
    let engine = Engine::new();
    f.enqueue(&engine);
    wait(&engine, |s| {
        s["jobs"][0]["entries"]
            .as_array()
            .unwrap()
            .iter()
            .any(|e| e["state"] == "conflict")
    });
    engine.request(&json!({"command":"enqueue","mode":"move","sources":[moving],"destination":f.0.join("arrivals")}).to_string()).unwrap();
    let state = wait(&engine, |s| s["jobs"][1]["state"] == "done");
    assert_eq!(state["jobs"][1]["fast_move"], true);
    assert_eq!(state["jobs"][0]["state"], "running");
    assert_eq!(
        fs::metadata(f.0.join("arrivals/quick.txt")).unwrap().ino(),
        inode
    );
    engine.request(r#"{"command":"cancel_all"}"#).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "cancelled");
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Déplacer une source qu’une copie utilise attend sa libération, même si le disque autorise les renommages.
fn fast_move_waits_for_overlapping_copy_paths() {
    let f = Fixture::local();
    fs::create_dir(f.0.join("arrivals")).unwrap();
    fs::create_dir_all(f.0.join("dest/source/sub")).unwrap();
    fs::write(f.0.join("dest/source/sub/été\n.txt"), b"old").unwrap();
    let engine = Engine::new();
    f.enqueue(&engine);
    wait(&engine, |s| {
        s["jobs"][0]["entries"]
            .as_array()
            .unwrap()
            .iter()
            .any(|e| e["state"] == "conflict")
    });
    engine.request(&json!({"command":"enqueue","mode":"move","sources":[f.0.join("source/sub/été\n.txt")],"destination":f.0.join("arrivals")}).to_string()).unwrap();
    wait(&engine, |s| s["jobs"][1]["state"] == "waiting");
    {
        let mut state = engine.shared.state.lock().unwrap();
        assert!(state.jobs[1].fast_move);
        assert_eq!(scheduling::next(&mut state), None);
    }
    assert!(f.0.join("source/sub/été\n.txt").exists());
    engine
        .request(r#"{"command":"resolve","id":1,"action":"skip"}"#)
        .unwrap();
    wait(&engine, |s| s["jobs"][1]["state"] == "done");
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// MD5 conserve ses deux relectures et le parcours de copie, même sur un montage local unique.
fn fast_move_does_not_bypass_md5() {
    let f = Fixture::local();
    let source = f.0.join("source/sub/été\n.txt");
    let inode = fs::metadata(&source).unwrap().ino();
    let engine = Engine::new();
    engine.request(&json!({"command":"enqueue","mode":"move","verify_md5":true,"sources":[source],"destination":f.0.join("dest")}).to_string()).unwrap();
    let state = wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert_eq!(state["jobs"][0]["fast_move"], false);
    let entry = &state["jobs"][0]["entries"][0];
    assert!(!entry["md5_source"].as_str().unwrap().is_empty());
    assert_eq!(entry["md5_source"], entry["md5_destination"]);
    assert_ne!(
        fs::metadata(f.0.join("dest/été\n.txt")).unwrap().ino(),
        inode
    );
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Un refus EXDEV ne lit ni ne supprime rien ; il replanifie le lot avant sa copie de secours.
fn fast_move_exdev_requeues_before_copying() {
    let f = Fixture::local();
    let source = f.0.join("source/sub/été\n.txt");
    let inode = fs::metadata(&source).unwrap().ino();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    engine.request(&json!({"command":"enqueue","mode":"move","sources":[source],"destination":f.0.join("dest")}).to_string()).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    let entry = {
        let mut state = engine.shared.state.lock().unwrap();
        assert!(state.jobs[0].fast_move);
        state.jobs[0].state = TransferState::Running;
        state.paused = false;
        state.jobs[0].entries[0].clone()
    };
    let outcome = engine
        .rename_entry_using(1, 0, &entry, |_, _| {
            Err(std::io::Error::from_raw_os_error(18))
        })
        .unwrap();
    assert_eq!(outcome, EntryOutcome::Requeue);
    assert!(source.exists());
    assert!(!entry.destination.exists());
    {
        let mut state = engine.shared.state.lock().unwrap();
        assert!(!state.jobs[0].fast_move);
        assert!(state.jobs[0].entries[0].undo.is_none());
        assert_eq!(state.jobs[0].entries[0].state, "waiting");
        state.jobs[0].state = TransferState::Waiting;
    }
    engine.shared.wake.notify_all();
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert!(!source.exists());
    assert_ne!(fs::metadata(&entry.destination).unwrap().ino(), inode);
    assert_eq!(fs::read(&entry.destination).unwrap(), b"bonjour\0monde");
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Un conflit apparu après analyse renvoie vers la file classique sans toucher les deux fichiers.
fn fast_move_rechecks_new_destination_and_preserves_conflict() {
    let f = Fixture::local();
    let source = f.0.join("source/sub/été\n.txt");
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    engine.request(&json!({"command":"enqueue","mode":"move","sources":[source],"destination":f.0.join("dest")}).to_string()).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    assert!(engine.shared.state.lock().unwrap().jobs[0].fast_move);
    let target = f.0.join("dest/été\n.txt");
    fs::write(&target, b"external").unwrap();
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    let state = wait(&engine, |s| {
        s["jobs"][0]["entries"][0]["state"] == "conflict"
    });
    assert_eq!(state["jobs"][0]["fast_move"], false);
    assert!(source.exists());
    assert_eq!(fs::read(&target).unwrap(), b"external");
    engine
        .request(r#"{"command":"resolve","id":1,"action":"replace"}"#)
        .unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    engine.request(r#"{"command":"undo_all"}"#).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "undone");
    assert_eq!(fs::read(&source).unwrap(), b"bonjour\0monde");
    assert_eq!(fs::read(&target).unwrap(), b"external");
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Ctime empêche de remettre en état un résultat altéré dont taille et mtime ont été maquillées.
fn fast_move_undo_refuses_same_size_changes_with_restored_mtime() {
    let f = Fixture::local();
    let engine = Engine::new();
    engine.request(&json!({"command":"enqueue","mode":"move","sources":[f.0.join("source/sub/été\n.txt")],"destination":f.0.join("dest")}).to_string()).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    let target = f.0.join("dest/été\n.txt");
    let modified = fs::metadata(&target).unwrap().modified().unwrap();
    fs::write(&target, b"BONJOUR\0MONDE").unwrap();
    fs::File::open(&target)
        .unwrap()
        .set_times(fs::FileTimes::new().set_modified(modified))
        .unwrap();
    engine.request(r#"{"command":"undo_all"}"#).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "undo_error");
    assert_eq!(fs::read(&target).unwrap(), b"BONJOUR\0MONDE");
    assert!(!f.0.join("source/sub/été\n.txt").exists());
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Une copie remplaçant un résultat rapide protège son journal avant de déplacer l’original en sauvegarde.
fn fast_move_then_replacement_copy_can_undo_both_jobs() {
    let f = Fixture::local();
    let source = f.0.join("source/sub/été\n.txt");
    let original_inode = fs::metadata(&source).unwrap().ino();
    let engine = Engine::new();
    engine.request(&json!({"command":"enqueue","mode":"move","sources":[source],"destination":f.0.join("dest")}).to_string()).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    fs::create_dir(f.0.join("other")).unwrap();
    fs::write(f.0.join("other/été\n.txt"), b"replacement").unwrap();
    engine.request(&json!({"command":"enqueue","sources":[f.0.join("other/été\n.txt")],"destination":f.0.join("dest")}).to_string()).unwrap();
    wait(&engine, |s| {
        s["jobs"][1]["entries"][0]["state"] == "conflict"
    });
    engine
        .request(r#"{"command":"resolve","id":2,"action":"replace"}"#)
        .unwrap();
    let state = wait(&engine, |s| s["jobs"][1]["state"] == "done");
    assert!(state["jobs"][0]["entries"][0]["undo"]["digest"]
        .as_str()
        .is_some());
    engine.request(r#"{"command":"undo_all"}"#).unwrap();
    wait(&engine, |s| {
        s["jobs"][0]["state"] == "undone" && s["jobs"][1]["state"] == "undone"
    });
    assert_eq!(fs::metadata(&source).unwrap().ino(), original_inode);
    assert_eq!(fs::read(&source).unwrap(), b"bonjour\0monde");
    assert!(!f.0.join("dest/été\n.txt").exists());
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Le journal persistant garde les informations de retour rapide entre deux lancements.
fn fast_move_persistent_history_restores_original_inode() {
    let f = Fixture::local();
    let source = f.0.join("source/sub/été\n.txt");
    let inode = fs::metadata(&source).unwrap().ino();
    let journal = f.0.join("state.json");
    let engine = Engine::persistent(journal.clone()).unwrap();
    engine
        .request(r#"{"command":"configure","keep_history":true}"#)
        .unwrap();
    engine.request(&json!({"command":"enqueue","mode":"move","sources":[source],"destination":f.0.join("dest")}).to_string()).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
    let engine = Engine::persistent(journal).unwrap();
    engine.request(r#"{"command":"undo_all"}"#).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "undone");
    assert_eq!(fs::metadata(&source).unwrap().ino(), inode);
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Une cible créée à l’instant du renommage est conservée par RENAME_NOREPLACE.
fn fast_move_publication_race_preserves_source_and_external_target() {
    let f = Fixture::local();
    let source = f.0.join("source/sub/été\n.txt");
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    engine.request(&json!({"command":"enqueue","mode":"move","sources":[source],"destination":f.0.join("dest")}).to_string()).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    let entry = {
        let mut state = engine.shared.state.lock().unwrap();
        state.paused = false;
        state.jobs[0].state = TransferState::Running;
        state.jobs[0].entries[0].clone()
    };
    assert!(engine
        .rename_entry_using(1, 0, &entry, |source, destination| {
            fs::write(destination, b"external").unwrap();
            publish_new(source, destination)
        })
        .is_err());
    assert_eq!(fs::read(&source).unwrap(), b"bonjour\0monde");
    assert_eq!(fs::read(&entry.destination).unwrap(), b"external");
    engine.update(1, |job| job.state = TransferState::Error);
    engine.request(r#"{"command":"undo_all"}"#).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "undo_error");
    assert_eq!(fs::read(&source).unwrap(), b"bonjour\0monde");
    assert_eq!(fs::read(&entry.destination).unwrap(), b"external");
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Une annulation reçue avant le départ d’un renommage ne déplace aucun fichier.
fn fast_move_cancel_before_start_preserves_original() {
    let f = Fixture::local();
    let source = f.0.join("source/sub/été\n.txt");
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    engine.request(&json!({"command":"enqueue","mode":"move","sources":[source],"destination":f.0.join("dest")}).to_string()).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    assert!(engine.shared.state.lock().unwrap().jobs[0].fast_move);
    engine.request(r#"{"command":"cancel_all"}"#).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "cancelled");
    assert_eq!(fs::read(&source).unwrap(), b"bonjour\0monde");
    assert!(!f.0.join("dest/été\n.txt").exists());
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Un arrêt entre le renommage et sa confirmation ne permet pas de prétendre valider un contenu inconnu.
fn fast_move_recovery_preserves_unconfirmed_result() {
    let f = Fixture::local();
    let source = f.0.join("source/sub/été\n.txt");
    let engine = Engine::new();
    engine.request(&json!({"command":"enqueue","mode":"move","sources":[source],"destination":f.0.join("dest")}).to_string()).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    let journal = f.0.join("crash.json");
    {
        let mut state = engine.shared.state.lock().unwrap();
        state.keep_history = true;
        state.jobs[0].state = TransferState::Running;
        state.jobs[0].entries[0].undo.as_mut().unwrap().rename_ctime = None;
        fs::write(&journal, serde_json::to_vec(&*state).unwrap()).unwrap();
        state.jobs[0].state = TransferState::Done;
    }
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
    let engine = Engine::persistent(journal).unwrap();
    let state = engine.request(r#"{"command":"status"}"#).unwrap();
    assert_eq!(state["jobs"][0]["state"], "error");
    engine.request(r#"{"command":"undo_all"}"#).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "undo_error");
    assert!(!source.exists());
    assert_eq!(
        fs::read(f.0.join("dest/été\n.txt")).unwrap(),
        b"bonjour\0monde"
    );
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Deux noms d’un même inode ne doivent pas invalider les contrôles de restauration l’un de l’autre.
fn fast_move_hardlinked_sources_keep_classic_undo_guarantees() {
    let f = Fixture::local();
    let source = f.0.join("source/sub/été\n.txt");
    let alias = f.0.join("source/sub/alias.txt");
    fs::hard_link(&source, &alias).unwrap();
    let engine = Engine::new();
    engine.request(&json!({"command":"enqueue","mode":"move","sources":[source,alias],"destination":f.0.join("dest")}).to_string()).unwrap();
    let state = wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert_eq!(state["jobs"][0]["fast_move"], false);
    engine.request(r#"{"command":"undo_all"}"#).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "undone");
    assert_eq!(fs::read(&source).unwrap(), b"bonjour\0monde");
    assert_eq!(fs::read(&alias).unwrap(), b"bonjour\0monde");
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Un alias symbolique de parent conserve le parcours classique au lieu d’échouer en plein renommage rapide.
fn fast_move_rejects_symbolic_parent_alias_during_preparation() {
    let f = Fixture::local();
    symlink(f.0.join("source/sub"), f.0.join("alias")).unwrap();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    engine.request(&json!({"command":"enqueue","mode":"move","sources":[f.0.join("alias/été\n.txt")],"destination":f.0.join("dest")}).to_string()).unwrap();
    let state = wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    assert_eq!(state["jobs"][0]["fast_move"], false);
    engine.request(r#"{"command":"cancel_all"}"#).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "cancelled");
    assert!(f.0.join("source/sub/été\n.txt").exists());
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Une copie et un déplacement de fichiers différents vers le même dossier ne se bloquent pas.
fn fast_move_shares_destination_folder_with_active_copy() {
    let f = Fixture::local();
    fs::create_dir(f.0.join("other")).unwrap();
    let copied_source = f.0.join("source/sub/été\n.txt");
    let moving_source = f.0.join("other/quick.txt");
    let copied_target = f.0.join("dest/été\n.txt");
    let moving_target = f.0.join("dest/quick.txt");
    fs::write(&copied_target, b"existing").unwrap();
    fs::write(&moving_source, b"quick").unwrap();
    let inode = fs::metadata(&moving_source).unwrap().ino();
    let engine = Engine::new();
    engine
        .request(
            &json!({"command":"enqueue","sources":[copied_source],"destination":f.0.join("dest")})
                .to_string(),
        )
        .unwrap();
    wait(&engine, |s| {
        s["jobs"][0]["entries"][0]["state"] == "conflict"
    });
    engine.request(&json!({"command":"enqueue","mode":"move","sources":[moving_source],"destination":f.0.join("dest")}).to_string()).unwrap();
    let state = wait(&engine, |s| s["jobs"][1]["state"] == "done");
    assert_eq!(state["jobs"][0]["state"], "running");
    assert_eq!(state["jobs"][1]["fast_move"], true);
    assert_eq!(fs::metadata(&moving_target).unwrap().ino(), inode);
    assert!(!moving_source.exists());
    assert_eq!(fs::read(&copied_target).unwrap(), b"existing");
    assert!(copied_source.exists());
    engine.request(r#"{"command":"cancel_all"}"#).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "cancelled");
    engine.request(r#"{"command":"undo_all"}"#).unwrap();
    wait(&engine, |s| {
        s["jobs"]
            .as_array()
            .unwrap()
            .iter()
            .all(|job| job["state"] == "undone")
    });
    assert_eq!(fs::metadata(&moving_source).unwrap().ino(), inode);
    assert_eq!(fs::read(&copied_target).unwrap(), b"existing");
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Un nom choisi après conflit attend le voisin actif, puis devient réservé avant toute écriture.
fn fast_move_conflict_rename_reserves_new_output_before_copying() {
    let f = Fixture::local();
    fs::create_dir(f.0.join("other")).unwrap();
    let copied_source = f.0.join("source/sub/été\n.txt");
    let moving_source = f.0.join("other/été\n (copie 1).txt");
    let renamed_target = f.0.join("dest/été\n (copie 1).txt");
    fs::write(&moving_source, b"quick").unwrap();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    for (source, mode) in [(&copied_source, "copy"), (&moving_source, "move")] {
        engine.request(&json!({"command":"enqueue","mode":mode,"sources":[source],"destination":f.0.join("dest")}).to_string()).unwrap();
    }
    wait(&engine, |s| {
        s["jobs"]
            .as_array()
            .unwrap()
            .iter()
            .all(|job| job["state"] == "waiting")
    });
    {
        let mut state = engine.shared.state.lock().unwrap();
        let disks = state.jobs[1].disks.clone();
        state.jobs[1].state = TransferState::Running;
        state.active.insert(2, disks);
    }
    let copy = engine.clone();
    let target = renamed_target.clone();
    let (send, receive) = std::sync::mpsc::channel();
    let waiter = thread::spawn(move || send.send(copy.reserve_destination(1, 0, &target)).unwrap());
    assert!(matches!(
        receive.recv_timeout(Duration::from_millis(80)),
        Err(std::sync::mpsc::RecvTimeoutError::Timeout)
    ));
    assert!(!renamed_target.exists());
    engine.release_worker(2);
    receive
        .recv_timeout(Duration::from_secs(2))
        .unwrap()
        .unwrap();
    waiter.join().unwrap();
    {
        let mut state = engine.shared.state.lock().unwrap();
        state.paused = false;
        state.jobs[0].state = TransferState::Running;
        state.jobs[1].state = TransferState::Waiting;
        let disks = state.jobs[0].disks.clone();
        state.active.insert(1, disks);
        assert_eq!(
            scheduling::next(&mut state),
            None,
            "Le nouveau nom doit maintenant bloquer le déplacement rapide"
        );
        state.paused = true;
        state.active.clear();
        state.jobs[0].state = TransferState::Waiting;
    }
    engine.request(r#"{"command":"cancel_all"}"#).unwrap();
    wait(&engine, |s| {
        s["jobs"]
            .as_array()
            .unwrap()
            .iter()
            .all(|job| job["state"] == "cancelled")
    });
    assert!(copied_source.exists() && moving_source.exists());
    assert!(!renamed_target.exists());
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// La vue commune doit représenter le même inode ou le même parent d’un chemin futur.
fn fast_move_bind_route_checks_actual_identity_and_missing_suffixes() {
    let f = Fixture::local();
    let source = f.0.join("source/sub/été\n.txt");
    let other = f.0.join("source/sub/other.txt");
    fs::write(&other, b"bonjour\0monde").unwrap();
    symlink(f.0.join("source/sub"), f.0.join("alias")).unwrap();
    assert!(fast_move::same_location(
        &source,
        &f.0.join("alias/été\n.txt")
    ));
    assert!(!fast_move::same_location(&source, &other));
    assert!(fast_move::same_location(
        &f.0.join("source/sub/future/deep/file"),
        &f.0.join("alias/future/deep/file")
    ));
    assert!(!fast_move::same_location(
        &f.0.join("source/sub/future/deep/file"),
        &f.0.join("alias/future/deep/other")
    ));
    assert!(!fast_move::same_location(
        &f.0.join("dest/future/file"),
        &f.0.join("alias/future/file")
    ));
    assert!(!fast_move::same_location(
        &source,
        &f.0.join("alias/absent")
    ));
}

#[test]
/// Le dossier lui-même garde son inode, ses permissions et ceux de ses descendants, dans les deux sens.
fn fast_tree_moves_directory_in_one_rename_and_undo_keeps_root_inode() {
    use std::os::unix::fs::PermissionsExt;
    for empty in [false, true] {
        let f = Fixture::local();
        if empty {
            fs::remove_dir_all(f.0.join("source/sub")).unwrap();
        }
        fs::set_permissions(f.0.join("source"), fs::Permissions::from_mode(0o750)).unwrap();
        let before = fs::metadata(f.0.join("source")).unwrap();
        let engine = Engine::new();
        engine.request(&json!({"command":"enqueue","mode":"move","sources":[f.0.join("source")],"destination":f.0.join("dest")}).to_string()).unwrap();
        let done = wait(&engine, |s| s["jobs"][0]["state"] == "done");
        assert_eq!(
            done["jobs"][0]["entries"][0]["undo"]["directory_move"],
            true
        );
        let after = fs::metadata(f.0.join("dest/source")).unwrap();
        assert_eq!(
            (before.dev(), before.ino(), before.mode()),
            (after.dev(), after.ino(), after.mode())
        );
        engine.request(r#"{"command":"undo_all"}"#).unwrap();
        let undone = wait(&engine, |s| s["jobs"][0]["state"] == "undone");
        assert_eq!(
            fs::metadata(f.0.join("source")).unwrap().ino(),
            before.ino()
        );
        assert!(undone["jobs"][0]["entries"]
            .as_array()
            .unwrap()
            .iter()
            .all(|entry| entry["state"] == "undone"));
        assert!(!f.0.join("dest/source").exists());
        engine.request(r#"{"command":"shutdown"}"#).unwrap();
    }
}

#[test]
/// Une entrée ajoutée après analyse reste à la source ; elle n’est pas embarquée par un renommage en bloc.
fn fast_tree_changed_source_falls_back_without_moving_unknown_files() {
    let f = Fixture::local();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    engine.request(&json!({"command":"enqueue","mode":"move","sources":[f.0.join("source")],"destination":f.0.join("dest")}).to_string()).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    fs::write(f.0.join("source/sub/late.txt"), b"external").unwrap();
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    let done = wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert_eq!(
        done["jobs"][0]["entries"][0]["undo"]["directory_move"],
        false
    );
    assert_eq!(
        fs::read(f.0.join("source/sub/late.txt")).unwrap(),
        b"external"
    );
    assert!(!f.0.join("dest/source/sub/late.txt").exists());
    assert_eq!(
        fs::read(f.0.join("dest/source/sub/été\n.txt")).unwrap(),
        b"bonjour\0monde"
    );
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Une pause ciblée empêche de déplacer discrètement son fichier avec le dossier parent.
fn fast_tree_respects_paused_child_and_cancellation() {
    let f = Fixture::local();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    engine.request(&json!({"command":"enqueue","mode":"move","sources":[f.0.join("source")],"destination":f.0.join("dest")}).to_string()).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    engine.request(r#"{"command":"pause_selection","jobs":[],"entries":[{"job":1,"index":2}],"paused":true}"#).unwrap();
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    wait(&engine, |s| {
        ["waiting", "running"].contains(&s["jobs"][0]["state"].as_str().unwrap())
            && f.0.join("dest/source/sub").exists()
    });
    assert!(f.0.join("source/sub/été\n.txt").exists());
    assert!(!f.0.join("dest/source/sub/été\n.txt").exists());
    engine.request(r#"{"command":"cancel_all"}"#).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "cancelled");
    engine.request(r#"{"command":"undo_all"}"#).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "undone");
    assert_eq!(
        fs::read(f.0.join("source/sub/été\n.txt")).unwrap(),
        b"bonjour\0monde"
    );
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Une restauration ciblée d’un seul fichier doit laisser ses voisins dans le dossier déplacé.
fn fast_tree_partial_restore_then_full_restore_preserves_neighbors() {
    let f = Fixture::local();
    fs::write(f.0.join("source/sub/neighbor.txt"), b"neighbor").unwrap();
    let engine = Engine::new();
    engine.request(&json!({"command":"enqueue","mode":"move","sources":[f.0.join("source")],"destination":f.0.join("dest")}).to_string()).unwrap();
    let done = wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert_eq!(
        done["jobs"][0]["entries"][0]["undo"]["directory_move"],
        true
    );
    let file = done["jobs"][0]["entries"]
        .as_array()
        .unwrap()
        .iter()
        .position(|entry| entry["source"].as_str().unwrap().ends_with("neighbor.txt"))
        .unwrap();
    engine
        .request(
            &json!({"command":"undo_selection","jobs":[],"entries":[{"job":1,"index":file}]})
                .to_string(),
        )
        .unwrap();
    wait(&engine, |s| {
        s["jobs"][0]["entries"][file]["state"] == "undone"
    });
    assert_eq!(
        fs::read(f.0.join("source/sub/neighbor.txt")).unwrap(),
        b"neighbor"
    );
    assert!(f.0.join("dest/source/sub/été\n.txt").exists());
    assert!(!f.0.join("source/sub/été\n.txt").exists());
    engine.request(r#"{"command":"undo_all"}"#).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "undone");
    assert_eq!(
        fs::read(f.0.join("source/sub/été\n.txt")).unwrap(),
        b"bonjour\0monde"
    );
    assert!(!f.0.join("dest/source").exists());
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Un fichier extérieur ajouté dans le résultat reste sur place ; l’erreur peut être reprise après son retrait.
fn fast_tree_restore_preserves_external_files_and_can_finish_after_cleanup() {
    let f = Fixture::local();
    let engine = Engine::new();
    engine.request(&json!({"command":"enqueue","mode":"move","sources":[f.0.join("source")],"destination":f.0.join("dest")}).to_string()).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    let external = f.0.join("dest/source/external.txt");
    fs::write(&external, b"external").unwrap();
    engine.request(r#"{"command":"undo_all"}"#).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "undo_error");
    assert_eq!(fs::read(&external).unwrap(), b"external");
    assert!(!f.0.join("source/external.txt").exists());
    assert_eq!(
        fs::read(f.0.join("source/sub/été\n.txt")).unwrap(),
        b"bonjour\0monde"
    );
    fs::remove_file(&external).unwrap();
    engine.request(r#"{"command":"undo_all"}"#).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "undone");
    assert!(!f.0.join("dest/source").exists());
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Les journaux préalables suffisent à récupérer un arrêt avant ou après l’unique renommage du dossier.
fn fast_tree_recovers_planned_move_before_or_after_atomic_rename() {
    for published in [false, true] {
        let f = Fixture::local();
        let before = fs::metadata(f.0.join("source")).unwrap().ino();
        let engine = Engine::new();
        engine.request(&json!({"command":"enqueue","mode":"move","sources":[f.0.join("source")],"destination":f.0.join("dest")}).to_string()).unwrap();
        let done = wait(&engine, |s| s["jobs"][0]["state"] == "done");
        assert_eq!(
            done["jobs"][0]["entries"][0]["undo"]["directory_move"],
            true
        );
        if !published {
            fast_move::rename_local(&f.0.join("dest/source"), &f.0.join("source")).unwrap();
        }
        let journal = f.0.join("crash.json");
        {
            let mut state = engine.shared.state.lock().unwrap();
            state.keep_history = true;
            state.jobs[0].state = TransferState::Running;
            for entry in &mut state.jobs[0].entries {
                entry.state = TransferState::Waiting;
                entry.copied = 0;
            }
            fs::write(&journal, serde_json::to_vec(&*state).unwrap()).unwrap();
            state.jobs[0].state = TransferState::Done;
        }
        engine.request(r#"{"command":"shutdown"}"#).unwrap();
        let engine = Engine::persistent(journal).unwrap();
        engine.request(r#"{"command":"undo_all"}"#).unwrap();
        wait(&engine, |s| s["jobs"][0]["state"] == "undone");
        assert_eq!(fs::metadata(f.0.join("source")).unwrap().ino(), before);
        assert_eq!(
            fs::read(f.0.join("source/sub/été\n.txt")).unwrap(),
            b"bonjour\0monde"
        );
        assert!(!f.0.join("dest/source").exists());
        engine.request(r#"{"command":"shutdown"}"#).unwrap();
    }
}

#[test]
/// Un contenu altéré ne doit pas contourner les précontrôles grâce au raccourci de restauration du dossier.
fn fast_tree_undo_refuses_modified_content_before_renaming_root() {
    let f = Fixture::local();
    let engine = Engine::new();
    engine.request(&json!({"command":"enqueue","mode":"move","sources":[f.0.join("source")],"destination":f.0.join("dest")}).to_string()).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    let target = f.0.join("dest/source/sub/été\n.txt");
    let modified = fs::metadata(&target).unwrap().modified().unwrap();
    fs::write(&target, b"BONJOUR\0MONDE").unwrap();
    fs::File::open(&target)
        .unwrap()
        .set_times(fs::FileTimes::new().set_modified(modified))
        .unwrap();
    engine.request(r#"{"command":"undo_all"}"#).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "undo_error");
    assert!(!f.0.join("source").exists());
    assert_eq!(fs::read(&target).unwrap(), b"BONJOUR\0MONDE");
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Le saut des descendants d’un bloc terminé ne doit pas oublier le dossier suivant du lot.
fn fast_tree_moves_and_restores_multiple_roots_in_same_job() {
    let f = Fixture::local();
    let second = f.0.join("second");
    fs::create_dir(&second).unwrap();
    fs::write(second.join("file.txt"), b"second").unwrap();
    let first_inode = fs::metadata(f.0.join("source")).unwrap().ino();
    let second_inode = fs::metadata(&second).unwrap().ino();
    let engine = Engine::new();
    engine.request(&json!({"command":"enqueue","mode":"move","sources":[f.0.join("source"),second],"destination":f.0.join("dest")}).to_string()).unwrap();
    let done = wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert_eq!(
        done["jobs"][0]["entries"]
            .as_array()
            .unwrap()
            .iter()
            .filter(|entry| entry["undo"]["directory_move"] == true)
            .count(),
        2
    );
    assert_eq!(
        fs::metadata(f.0.join("dest/source")).unwrap().ino(),
        first_inode
    );
    assert_eq!(
        fs::metadata(f.0.join("dest/second")).unwrap().ino(),
        second_inode
    );
    engine.request(r#"{"command":"undo_all"}"#).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "undone");
    assert_eq!(fs::metadata(f.0.join("source")).unwrap().ino(), first_inode);
    assert_eq!(fs::metadata(&second).unwrap().ino(), second_inode);
    assert_eq!(fs::read(second.join("file.txt")).unwrap(), b"second");
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

/// Copie partielle privée et journal durable représentant un arrêt juste après un point validé.
struct ResumeFixture {
    fixture: Fixture,
    journal: PathBuf,
    source: PathBuf,
    target: PathBuf,
    partial: PathBuf,
    data: Vec<u8>,
    bytes: u64,
    inode: u64,
}
impl ResumeFixture {
    fn new(mode: &str, md5: bool, replace: bool) -> Self {
        let fixture = Fixture::local();
        let source = fixture.0.join("source/sub/été\n.txt");
        let target = fixture.0.join("dest/été\n.txt");
        let data: Vec<_> = (0..1024 * 1024).map(|index| (index % 251) as u8).collect();
        fs::write(&source, &data).unwrap();
        if replace {
            fs::write(&target, b"ancienne destination").unwrap();
        }
        let journal = fixture.0.join("journal/state.json");
        let engine = Engine::persistent(journal.clone()).unwrap();
        engine
            .request(r#"{"command":"configure","resume_interrupted":true,"keep_history":true}"#)
            .unwrap();
        engine
            .request(r#"{"command":"pause","paused":true}"#)
            .unwrap();
        engine.request(&json!({"command":"enqueue","mode":mode,"verify_md5":md5,"sources":[source],"destination":fixture.0.join("dest")}).to_string()).unwrap();
        wait(&engine, |state| state["jobs"][0]["state"] == "waiting");
        let entry = {
            let mut state = engine.shared.state.lock().unwrap();
            state.paused = false;
            state.jobs[0].state = TransferState::Running;
            state.jobs[0].fast_move = false;
            state.jobs[0].entries[0].state = TransferState::Running;
            state.jobs[0].entries[0].clone()
        };
        let partial = fixture
            .0
            .join(format!("dest/.gad-{}-1-0-0.part", std::process::id()));
        let mut output = OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(&partial)
            .unwrap();
        let mut record = undo::Record {
            staging: Some(partial.clone()),
            staging_stamp: Some(undo::Stamp::read(&partial).unwrap()),
            resume: Some(resume::Point::initial(&entry, replace).unwrap()),
            ..Default::default()
        };
        let bytes = 256 * 1024;
        output.write_all(&data[..bytes]).unwrap();
        let mut hash = md5::Context::new();
        hash.consume(&data[..bytes]);
        engine.update(1, |job| job.entries[0].copied = bytes as u64);
        engine
            .save_copy_point((1, 0), &entry, &output, &mut record, bytes as u64, &hash)
            .unwrap();
        drop(output);
        let inode = fs::metadata(&partial).unwrap().ino();
        // Arrêter uniquement ces travailleurs de test sans écrire un état terminal : équivalent du journal après crash.
        {
            let _state = engine.shared.state.lock().unwrap();
            engine.shared.stop.store(true, Ordering::SeqCst);
            engine.shared.wake.notify_all();
        }
        for worker in engine.shared.worker.lock().unwrap().drain(..) {
            worker.join().unwrap();
        }
        Self {
            fixture,
            journal,
            source,
            target,
            partial,
            data,
            bytes: bytes as u64,
            inode,
        }
    }
    fn edit_journal(&self, edit: impl FnOnce(&mut Value)) {
        let mut value: Value = serde_json::from_slice(&fs::read(&self.journal).unwrap()).unwrap();
        edit(&mut value);
        fs::write(&self.journal, serde_json::to_vec(&value).unwrap()).unwrap();
    }
}

#[test]
/// La copie réutilise le même temporaire, vérifie le préfixe puis réécrit seulement la fin non validée.
fn resume_copy_reuses_partial_inode_and_discards_uncommitted_tail() {
    let f = ResumeFixture::new("copy", false, false);
    OpenOptions::new()
        .append(true)
        .open(&f.partial)
        .unwrap()
        .write_all(b"fin non validee")
        .unwrap();
    let engine = Engine::persistent(f.journal.clone()).unwrap();
    let state = wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert_eq!(state["jobs"][0]["entries"][0]["resumed_from"], f.bytes);
    assert_eq!(fs::metadata(&f.target).unwrap().ino(), f.inode);
    assert_eq!(fs::read(&f.target).unwrap(), f.data);
    assert_eq!(fs::read(&f.source).unwrap(), f.data);
    assert!(!f.partial.exists());
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// MD5, déplacement et remplacement conservent leurs garanties, y compris la remise en état complète.
fn resume_move_replaces_only_approved_target_verifies_md5_and_can_undo() {
    let f = ResumeFixture::new("move", true, true);
    let engine = Engine::persistent(f.journal.clone()).unwrap();
    let done = wait(&engine, |s| s["jobs"][0]["state"] == "done");
    let entry = &done["jobs"][0]["entries"][0];
    assert_eq!(entry["resumed_from"], f.bytes);
    assert_eq!(entry["md5_source"], format!("{:x}", md5::compute(&f.data)));
    assert_eq!(entry["md5_source"], entry["md5_destination"]);
    assert!(!f.source.exists());
    assert_eq!(fs::metadata(&f.target).unwrap().ino(), f.inode);
    engine.request(r#"{"command":"undo_all"}"#).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "undone");
    assert_eq!(fs::read(&f.source).unwrap(), f.data);
    assert_eq!(fs::read(&f.target).unwrap(), b"ancienne destination");
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Un préfixe corrompu reste intact pour diagnostic et ne devient jamais un fichier final.
fn resume_refuses_corrupted_prefix_without_overwriting_anything() {
    let f = ResumeFixture::new("copy", false, false);
    OpenOptions::new()
        .write(true)
        .open(&f.partial)
        .unwrap()
        .write_all(b"corrompu")
        .unwrap();
    let corrupt = fs::read(&f.partial).unwrap();
    let engine = Engine::persistent(f.journal.clone()).unwrap();
    let state = wait(&engine, |s| {
        s["jobs"][0]["state"] == "error"
            && s["jobs"][0]["entries"][0]["error"]
                .as_str()
                .is_some_and(|error| error.contains("contents"))
    });
    assert!(state["jobs"][0]["recovery"].as_bool().unwrap());
    assert_eq!(fs::read(&f.partial).unwrap(), corrupt);
    assert!(!f.target.exists());
    assert_eq!(fs::read(&f.source).unwrap(), f.data);
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Ctime empêche de reprendre une source modifiée dont taille et mtime ont été rétablies.
fn resume_refuses_changed_source_even_when_size_and_mtime_match() {
    let f = ResumeFixture::new("copy", false, false);
    let modified = fs::metadata(&f.source).unwrap().modified().unwrap();
    OpenOptions::new()
        .write(true)
        .open(&f.source)
        .unwrap()
        .write_all(b"nouveau")
        .unwrap();
    fs::File::open(&f.source)
        .unwrap()
        .set_times(fs::FileTimes::new().set_modified(modified))
        .unwrap();
    let before = fs::read(&f.partial).unwrap();
    let engine = Engine::persistent(f.journal.clone()).unwrap();
    wait(&engine, |s| {
        s["jobs"][0]["state"] == "error"
            && s["jobs"][0]["error"]
                .as_str()
                .is_some_and(|error| error.contains("source"))
    });
    assert_eq!(fs::read(&f.partial).unwrap(), before);
    assert!(!f.target.exists());
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Une cible remplacée par une autre application ne réutilise pas l’ancien accord d’écrasement.
fn resume_refuses_changed_replacement_target() {
    let f = ResumeFixture::new("copy", false, true);
    fs::write(&f.target, b"travail exterieur").unwrap();
    let engine = Engine::persistent(f.journal.clone()).unwrap();
    wait(&engine, |s| {
        s["jobs"][0]["state"] == "error"
            && s["jobs"][0]["error"]
                .as_str()
                .is_some_and(|error| error.contains("destination"))
    });
    assert_eq!(fs::read(&f.target).unwrap(), b"travail exterieur");
    assert_eq!(fs::metadata(&f.partial).unwrap().len(), f.bytes);
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Un temporaire remplacé, tronqué ou devenu lien ne doit jamais être tronqué ou suivi par la reprise.
fn resume_refuses_replaced_short_or_symlinked_temporaries() {
    for kind in ["replaced", "short", "link"] {
        let f = ResumeFixture::new("copy", false, false);
        let external = f.fixture.0.join("external");
        fs::write(&external, b"ne pas toucher").unwrap();
        if kind == "short" {
            OpenOptions::new()
                .write(true)
                .open(&f.partial)
                .unwrap()
                .set_len(3)
                .unwrap();
        } else {
            fs::remove_file(&f.partial).unwrap();
            if kind == "link" {
                symlink(&external, &f.partial).unwrap();
            } else {
                fs::write(&f.partial, b"fichier exterieur").unwrap();
            }
        }
        let inode = fs::symlink_metadata(&f.partial).unwrap().ino();
        let engine = Engine::persistent(f.journal.clone()).unwrap();
        wait(&engine, |s| s["jobs"][0]["state"] == "error");
        assert_eq!(fs::symlink_metadata(&f.partial).unwrap().ino(), inode);
        assert_eq!(fs::read(&external).unwrap(), b"ne pas toucher");
        assert!(!f.target.exists());
        assert!(f.source.exists());
        engine.request(r#"{"command":"shutdown"}"#).unwrap();
    }
}

#[test]
/// Une indisponibilité du parent peut être corrigée puis retentée manuellement, sans perdre le préfixe.
fn resume_manual_retry_after_source_volume_becomes_available() {
    let f = ResumeFixture::new("copy", false, false);
    let parent = f.source.parent().unwrap();
    let hidden = f.fixture.0.join("source/held");
    fs::rename(parent, &hidden).unwrap();
    let engine = Engine::persistent(f.journal.clone()).unwrap();
    let state = wait(&engine, |s| s["jobs"][0]["state"] == "error");
    assert_eq!(state["jobs"][0]["resume_pending"], true);
    fs::rename(&hidden, parent).unwrap();
    engine
        .request(r#"{"command":"resume_interrupted","ids":[1]}"#)
        .unwrap();
    let done = wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert_eq!(done["jobs"][0]["entries"][0]["resumed_from"], f.bytes);
    assert_eq!(fs::read(&f.target).unwrap(), f.data);
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Désactiver la reprise automatique conserve le lot récupérable, mais seule la commande explicite le redémarre.
fn resume_option_off_waits_for_explicit_action() {
    let f = ResumeFixture::new("copy", false, false);
    f.edit_journal(|state| state["resume_interrupted"] = json!(false));
    let engine = Engine::persistent(f.journal.clone()).unwrap();
    let state = engine.request(r#"{"command":"status"}"#).unwrap();
    assert_eq!(state["jobs"][0]["state"], "error");
    assert_eq!(state["jobs"][0]["resume_pending"], true);
    assert!(!f.target.exists());
    assert!(f.partial.exists());
    engine
        .request(r#"{"command":"resume_interrupted","ids":[1]}"#)
        .unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert_eq!(fs::read(&f.target).unwrap(), f.data);
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Une annulation volontaire, avant ou après chargement, ne doit pas se transformer en reprise automatique.
fn resume_never_restarts_explicitly_cancelled_jobs() {
    for loaded in [false, true] {
        let f = ResumeFixture::new("copy", false, false);
        f.edit_journal(|state| {
            state["resume_interrupted"] = json!(false);
            if !loaded {
                state["jobs"][0]["cancel"] = json!(true);
            }
        });
        let engine = Engine::persistent(f.journal.clone()).unwrap();
        if loaded {
            engine.request(r#"{"command":"cancel_all"}"#).unwrap();
        }
        let state = engine.request(r#"{"command":"status"}"#).unwrap();
        assert_eq!(state["jobs"][0]["resume_pending"], false);
        assert!(engine
            .request(r#"{"command":"resume_interrupted","ids":[1]}"#)
            .is_err());
        assert!(!f.target.exists());
        assert!(f.source.exists());
        engine.request(r#"{"command":"shutdown"}"#).unwrap();
    }
}

#[test]
/// Une publication déjà engagée reste dans la récupération transactionnelle, pas dans une recopie automatique.
fn resume_refuses_ambiguous_publication_phase() {
    let f = ResumeFixture::new("move", true, false);
    f.edit_journal(|state| {
        state["jobs"][0]["entries"][0]["undo"]["result"] =
            state["jobs"][0]["entries"][0]["undo"]["staging_stamp"].clone();
    });
    let engine = Engine::persistent(f.journal.clone()).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "error");
    assert!(f.source.exists());
    assert!(f.partial.exists());
    assert!(!f.target.exists());
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Les fichiers déjà validés ne sont ni réouverts en source ni recopiés pendant la reprise du voisin.
fn resume_does_not_recopy_already_completed_files() {
    let f = ResumeFixture::new("copy", false, false);
    let completed = f.fixture.0.join("dest/finished.txt");
    fs::write(&completed, b"termine").unwrap();
    let inode = fs::metadata(&completed).unwrap().ino();
    f.edit_journal(|state| {
        let mut entry = state["jobs"][0]["entries"][0].clone();
        entry["source"] = json!(f.fixture.0.join("source/sub/missing-finished.txt"));
        entry["destination"] = json!(completed);
        entry["state"] = json!("done");
        entry["undo"] = Value::Null;
        entry["size"] = json!(7);
        entry["copied"] = json!(7);
        state["jobs"][0]["entries"]
            .as_array_mut()
            .unwrap()
            .push(entry);
    });
    let engine = Engine::persistent(f.journal.clone()).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert_eq!(fs::read(&completed).unwrap(), b"termine");
    assert_eq!(fs::metadata(&completed).unwrap().ino(), inode);
    assert_eq!(fs::read(&f.target).unwrap(), f.data);
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Les permissions déjà appliquées au temporaire n’empêchent pas sa reprise ; elles sont remises à l’arrivée.
fn resume_handles_readonly_partial_and_restores_source_permissions() {
    use std::os::unix::fs::PermissionsExt;
    let f = ResumeFixture::new("copy", false, false);
    fs::set_permissions(&f.source, fs::Permissions::from_mode(0o444)).unwrap();
    fs::set_permissions(&f.partial, fs::Permissions::from_mode(0o444)).unwrap();
    f.edit_journal(|state| {
        let entry: Entry = serde_json::from_value(state["jobs"][0]["entries"][0].clone()).unwrap();
        let reference =
            serde_json::to_value(resume::Point::initial(&entry, false).unwrap()).unwrap();
        state["jobs"][0]["entries"][0]["undo"]["resume"]["source"] = reference["source"].clone();
    });
    let engine = Engine::persistent(f.journal.clone()).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert_eq!(fs::read(&f.target).unwrap(), f.data);
    assert_eq!(fs::metadata(&f.target).unwrap().mode() & 0o777, 0o444);
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Une reprise automatique conserve les pauses persistantes, globales ou ciblées.
fn resume_preserves_global_and_individual_pause_until_user_resumes() {
    for global in [false, true] {
        let f = ResumeFixture::new("copy", false, false);
        f.edit_journal(|state| {
            if global {
                state["paused"] = json!(true);
            } else {
                state["jobs"][0]["entries"][0]["paused"] = json!(true);
            }
        });
        let engine = Engine::persistent(f.journal.clone()).unwrap();
        wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
        assert!(!f.target.exists());
        assert_eq!(fs::metadata(&f.partial).unwrap().len(), f.bytes);
        if global {
            engine
                .request(r#"{"command":"pause","paused":false}"#)
                .unwrap();
        } else {
            engine.request(r#"{"command":"pause_selection","jobs":[],"entries":[{"job":1,"index":0}],"paused":false}"#).unwrap();
        }
        wait(&engine, |s| s["jobs"][0]["state"] == "done");
        assert_eq!(fs::read(&f.target).unwrap(), f.data);
        engine.request(r#"{"command":"shutdown"}"#).unwrap();
    }
}

#[test]
/// Un point à zéro ou à la fin du fichier reste valide et conserve le temporaire d’origine.
fn resume_accepts_zero_and_full_file_checkpoints() {
    for full in [false, true] {
        let f = ResumeFixture::new("copy", true, false);
        let bytes = if full { f.data.len() } else { 0 };
        if full {
            fs::write(&f.partial, &f.data).unwrap();
        }
        f.edit_journal(|state| {
            let point = &mut state["jobs"][0]["entries"][0]["undo"]["resume"];
            point["bytes"] = json!(bytes);
            point["digest"] = json!(format!("{:x}", md5::compute(&f.data[..bytes])));
        });
        let engine = Engine::persistent(f.journal.clone()).unwrap();
        let done = wait(&engine, |s| s["jobs"][0]["state"] == "done");
        let entry = &done["jobs"][0]["entries"][0];
        assert_eq!(entry["resumed_from"], bytes);
        assert_eq!(
            entry["md5_destination"],
            format!("{:x}", md5::compute(&f.data))
        );
        assert_eq!(fs::metadata(&f.target).unwrap().ino(), f.inode);
        assert_eq!(fs::read(&f.target).unwrap(), f.data);
        engine.request(r#"{"command":"shutdown"}"#).unwrap();
    }
}

#[test]
/// Une empreinte mal formée ou différente du préfixe interdit toute publication et toute troncature.
fn resume_rejects_invalid_or_mismatched_digest() {
    for digest in [
        "",
        "bad",
        "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz",
        "00000000000000000000000000000000",
    ] {
        let f = ResumeFixture::new("copy", false, false);
        f.edit_journal(|state| {
            state["jobs"][0]["entries"][0]["undo"]["resume"]["digest"] = json!(digest)
        });
        let engine = Engine::persistent(f.journal.clone()).unwrap();
        wait(&engine, |s| s["jobs"][0]["state"] == "error");
        assert!(!f.target.exists());
        assert_eq!(fs::read(&f.partial).unwrap(), f.data[..f.bytes as usize]);
        assert_eq!(fs::read(&f.source).unwrap(), f.data);
        engine.request(r#"{"command":"shutdown"}"#).unwrap();
    }
}

#[test]
/// Un second lien physique expose le même inode ailleurs : la reprise ne doit modifier aucun de ses noms.
fn resume_refuses_hardlinked_partial() {
    let f = ResumeFixture::new("copy", false, false);
    let alias = f.fixture.0.join("external-alias");
    fs::hard_link(&f.partial, &alias).unwrap();
    let engine = Engine::persistent(f.journal.clone()).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "error");
    assert!(!f.target.exists());
    assert_eq!(fs::metadata(&alias).unwrap().nlink(), 2);
    assert_eq!(fs::read(&alias).unwrap(), f.data[..f.bytes as usize]);
    assert_eq!(fs::metadata(&f.partial).unwrap().ino(), f.inode);
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Un nom de temporaire associé à un autre lot, indice ou dossier ne peut pas être réutilisé.
fn resume_refuses_unrelated_staging_paths() {
    for name in [
        ".gad-1-2-0-0.part",
        ".gad-1-1-9-0.part",
        ".gad-1-1-0-no.part",
        "ordinary.part",
        "../.gad-1-1-0-0.part",
    ] {
        let f = ResumeFixture::new("copy", false, false);
        let wrong = f.target.parent().unwrap().join(name);
        fs::rename(&f.partial, &wrong).unwrap();
        f.edit_journal(|state| state["jobs"][0]["entries"][0]["undo"]["staging"] = json!(wrong));
        let engine = Engine::persistent(f.journal.clone()).unwrap();
        wait(&engine, |s| s["jobs"][0]["state"] == "error");
        assert!(!f.target.exists());
        assert_eq!(fs::metadata(&wrong).unwrap().ino(), f.inode);
        assert_eq!(fs::read(&wrong).unwrap(), f.data[..f.bytes as usize]);
        engine.request(r#"{"command":"shutdown"}"#).unwrap();
    }
}

#[test]
/// Une destination nouvelle apparue pendant l’arrêt, même symbolique, ne doit pas être écrasée.
fn resume_preserves_destination_created_during_interruption() {
    for link in [false, true] {
        let f = ResumeFixture::new("copy", false, false);
        let other = f.fixture.0.join("other.txt");
        if link {
            fs::write(&other, b"externe").unwrap();
            symlink(&other, &f.target).unwrap();
        } else {
            fs::write(&f.target, b"externe").unwrap();
        }
        let engine = Engine::persistent(f.journal.clone()).unwrap();
        wait(&engine, |s| s["jobs"][0]["state"] == "error");
        assert_eq!(fs::read(&f.target).unwrap(), b"externe");
        assert_eq!(
            fs::symlink_metadata(&f.target)
                .unwrap()
                .file_type()
                .is_symlink(),
            link
        );
        assert_eq!(fs::read(&f.partial).unwrap(), f.data[..f.bytes as usize]);
        engine.request(r#"{"command":"shutdown"}"#).unwrap();
    }
}

#[test]
/// Avant le premier point, seul un temporaire vide ou absent peut être recréé sans ambiguïté.
fn resume_restarts_unwritten_entry_before_first_checkpoint() {
    for exists in [false, true] {
        let f = ResumeFixture::new("copy", false, false);
        if exists {
            fs::write(&f.partial, []).unwrap();
        } else {
            fs::remove_file(&f.partial).unwrap();
        }
        f.edit_journal(|state| state["jobs"][0]["entries"][0]["undo"]["resume"] = Value::Null);
        let engine = Engine::persistent(f.journal.clone()).unwrap();
        wait(&engine, |s| s["jobs"][0]["state"] == "done");
        assert_eq!(fs::read(&f.target).unwrap(), f.data);
        assert!(!f.partial.exists());
        engine.request(r#"{"command":"shutdown"}"#).unwrap();
    }
}

#[test]
/// Une partie écrite sans point durable reste conservée ; sa longueur ne constitue pas une preuve de validité.
fn resume_refuses_nonempty_partial_without_checkpoint() {
    let f = ResumeFixture::new("copy", false, false);
    f.edit_journal(|state| state["jobs"][0]["entries"][0]["undo"]["resume"] = Value::Null);
    let engine = Engine::persistent(f.journal.clone()).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "error");
    assert!(!f.target.exists());
    assert_eq!(fs::read(&f.partial).unwrap(), f.data[..f.bytes as usize]);
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Une commande invalide, y compris un lot valide mélangé à un inconnu, ne doit changer aucun lot.
fn resume_command_validation_is_atomic() {
    let f = ResumeFixture::new("copy", false, false);
    f.edit_journal(|state| state["resume_interrupted"] = json!(false));
    let engine = Engine::persistent(f.journal.clone()).unwrap();
    let before = engine.request(r#"{"command":"status"}"#).unwrap()["jobs"].clone();
    for ids in [
        Value::Null,
        json!([]),
        json!([1, 1]),
        json!([1, 999]),
        json!([1, -1]),
        json!([1, "1"]),
        json!([1, 1.5]),
        json!([true]),
    ] {
        let response =
            engine.request(&json!({"command":"resume_interrupted","ids":ids}).to_string());
        assert!(response.is_err(), "Identifiants acceptés : {ids}");
        assert_eq!(
            engine.request(r#"{"command":"status"}"#).unwrap()["jobs"],
            before
        );
        assert_eq!(fs::read(&f.partial).unwrap(), f.data[..f.bytes as usize]);
    }
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// La désactivation de l’historique normal ne doit pas jeter le journal d’une copie interrompue.
fn resume_survives_disabled_history() {
    let f = ResumeFixture::new("copy", false, false);
    f.edit_journal(|state| state["keep_history"] = json!(false));
    let engine = Engine::persistent(f.journal.clone()).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert_eq!(fs::read(&f.target).unwrap(), f.data);
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
    let engine = Engine::persistent(f.journal.clone()).unwrap();
    assert!(engine.request(r#"{"command":"status"}"#).unwrap()["jobs"]
        .as_array()
        .unwrap()
        .is_empty());
    assert_eq!(fs::read(&f.target).unwrap(), f.data);
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Un ancien journal sans consentement de reprise conserve ses fichiers et exige une récupération manuelle.
fn resume_legacy_jobs_are_never_auto_started() {
    let f = ResumeFixture::new("copy", false, false);
    f.edit_journal(|state| {
        let job = state["jobs"][0].as_object_mut().unwrap();
        job.remove("resume_enabled");
        job.remove("resume_pending");
        job.remove("destination_url");
        job.remove("verify_properties");
        let entry = job["entries"][0].as_object_mut().unwrap();
        entry.remove("properties");
        for key in [
            "interrupted",
            "resume_checking",
            "resume_checked",
            "resumed_from",
        ] {
            entry.remove(key);
        }
        entry["undo"].as_object_mut().unwrap().remove("resume");
    });
    let engine = Engine::persistent(f.journal.clone()).unwrap();
    let state = engine.request(r#"{"command":"status"}"#).unwrap();
    assert_eq!(state["jobs"][0]["state"], "error");
    assert_eq!(state["jobs"][0]["resume_pending"], false);
    assert!(engine
        .request(r#"{"command":"resume_interrupted","ids":[1]}"#)
        .is_err());
    assert!(!f.target.exists());
    assert_eq!(fs::read(&f.partial).unwrap(), f.data[..f.bytes as usize]);
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// L’origine de destination est persistée pour l’affichage, sans remplacer le chemin de travail POSIX.
fn remote_destination_origin_survives_history_and_undo() {
    let f = Fixture::local();
    let journal = f.0.join("journal/state.json");
    let address = "sftp://alice@server:2222/cop ies%23";
    let engine = Engine::persistent(journal.clone()).unwrap();
    engine
        .request(r#"{"command":"configure","keep_history":true}"#)
        .unwrap();
    engine.request(&json!({"command":"enqueue","sources":[f.0.join("source/sub/été\n.txt")],"destination":f.0.join("dest"),"destination_url":address,"verify_md5":true}).to_string()).unwrap();
    let done = wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert_eq!(done["jobs"][0]["destination_url"], address);
    assert_eq!(done["jobs"][0]["resume_enabled"], false);
    assert_eq!(done["jobs"][0]["destination"], json!(f.0.join("dest")));
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
    let engine = Engine::persistent(journal).unwrap();
    assert_eq!(
        engine.request(r#"{"command":"status"}"#).unwrap()["jobs"][0]["destination_url"],
        address
    );
    engine.request(r#"{"command":"undo_all"}"#).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "undone");
    assert!(!f.0.join("dest/été\n.txt").exists());
    assert_eq!(
        fs::read(f.0.join("source/sub/été\n.txt")).unwrap(),
        b"bonjour\0monde"
    );
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Quitter au moment où les travailleurs deviennent inactifs doit toujours les réveiller et les joindre.
fn shutdown_does_not_lose_wakeup_when_workers_become_idle() {
    let (sent, received) = std::sync::mpsc::channel();
    thread::spawn(move || {
        for _ in 0..200 {
            let engine = Engine::new();
            thread::yield_now();
            engine.request(r#"{"command":"shutdown"}"#).unwrap();
            assert!(engine.shared.worker.lock().unwrap().is_empty());
            assert!(engine
                .request(r#"{"command":"pause","paused":true}"#)
                .is_err());
        }
        sent.send(()).unwrap();
    });
    received
        .recv_timeout(Duration::from_secs(10))
        .expect("La fermeture a perdu un réveil ou n’a pas joint ses travailleurs");
}

/// Pose un attribut réel, binaire ou ACL, sur le chemin lui-même sans suivre un lien.
fn set_test_attribute(path: &Path, name: &str, value: &[u8]) {
    use std::{ffi::CString, os::unix::ffi::OsStrExt};
    unsafe extern "C" {
        fn lsetxattr(
            path: *const std::ffi::c_char,
            name: *const std::ffi::c_char,
            value: *const u8,
            size: usize,
            flags: i32,
        ) -> i32;
    }
    let path = CString::new(path.as_os_str().as_bytes()).unwrap();
    let name = CString::new(name).unwrap();
    // SAFETY : les trois tampons vivent pendant l’appel ; aucun pointeur n’est conservé par la libc.
    let code = unsafe { lsetxattr(path.as_ptr(), name.as_ptr(), value.as_ptr(), value.len(), 0) };
    assert_eq!(code, 0, "{}", std::io::Error::last_os_error());
}

#[test]
/// Une copie ordinaire vérifiée indique l’égalité des propriétés visibles, indépendamment du MD5.
fn properties_copy_checks_equal_metadata() {
    let f = Fixture::local();
    let engine = Engine::new();
    engine.request(&json!({"command":"enqueue","sources":[f.0.join("source/sub/été\n.txt")],"destination":f.0.join("dest"),"verify_properties":true}).to_string()).unwrap();
    let done = wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert_eq!(done["jobs"][0]["entries"][0]["properties"]["status"], "ok");
    assert_eq!(
        fs::read(f.0.join("dest/été\n.txt")).unwrap(),
        b"bonjour\0monde"
    );
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Une copie avec xattr perdu reste terminée, mais expose la différence sans journaliser la valeur privée.
fn properties_copy_warns_when_extended_attribute_is_lost() {
    let f = Fixture::local();
    let source = f.0.join("source/sub/été\n.txt");
    set_test_attribute(&source, "user.gad-secret", b"valeur-privee\0\xff");
    let engine = Engine::new();
    engine.request(&json!({"command":"enqueue","sources":[source],"destination":f.0.join("dest"),"verify_properties":true,"verify_md5":true}).to_string()).unwrap();
    let done = wait(&engine, |s| s["jobs"][0]["state"] == "done");
    let report = &done["jobs"][0]["entries"][0]["properties"];
    assert_eq!(report["status"], "different");
    assert!(report["differences"]
        .to_string()
        .contains("user.gad-secret"));
    assert!(!done.to_string().contains("valeur-privee"));
    assert_eq!(fs::read(source).unwrap(), b"bonjour\0monde");
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Une perte de propriété bloque la suppression de l’original et laisse intacte une destination à remplacer.
fn properties_classic_move_preserves_original_and_existing_destination() {
    let f = Fixture::local();
    let source = f.0.join("source/sub/été\n.txt");
    let target = f.0.join("dest/été\n.txt");
    set_test_attribute(&source, "user.gad", b"important");
    fs::write(&target, b"ancienne destination").unwrap();
    let engine = Engine::new();
    engine.request(&json!({"command":"enqueue","mode":"move","sources":[source],"destination":f.0.join("dest"),"verify_properties":true,"verify_md5":true}).to_string()).unwrap();
    wait(&engine, |s| {
        s["jobs"][0]["entries"][0]["state"] == "conflict"
    });
    engine
        .request(r#"{"command":"resolve","id":1,"index":0,"action":"replace"}"#)
        .unwrap();
    let state = wait(&engine, |s| s["jobs"][0]["state"] == "error");
    assert_eq!(
        state["jobs"][0]["entries"][0]["properties"]["status"],
        "different"
    );
    assert_eq!(fs::read(source).unwrap(), b"bonjour\0monde");
    assert_eq!(fs::read(target).unwrap(), b"ancienne destination");
    assert!(!fs::read_dir(f.0.join("dest")).unwrap().any(|item| item
        .unwrap()
        .file_name()
        .to_string_lossy()
        .ends_with(".part")));
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Le contrôle reste compatible avec le renommage rapide d’un fichier et conserve son xattr.
fn properties_fast_file_move_keeps_attributes_and_inode() {
    let f = Fixture::local();
    let source = f.0.join("source/sub/été\n.txt");
    let target = f.0.join("dest/été\n.txt");
    set_test_attribute(&source, "user.gad", b"original");
    let inode = fs::metadata(&source).unwrap().ino();
    let engine = Engine::new();
    engine.request(&json!({"command":"enqueue","mode":"move","sources":[source],"destination":f.0.join("dest"),"verify_properties":true}).to_string()).unwrap();
    let done = wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert_eq!(done["jobs"][0]["entries"][0]["properties"]["status"], "ok");
    assert_eq!(done["jobs"][0]["entries"][0]["undo"]["rename_move"], true);
    assert_eq!(fs::metadata(target).unwrap().ino(), inode);
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Le renommage de l’arbre entier vérifie aussi ses descendants sans sacrifier son optimisation.
fn properties_fast_tree_checks_every_entry_and_can_undo() {
    let f = Fixture::local();
    let source = f.0.join("source");
    set_test_attribute(&source, "user.gad", b"racine");
    set_test_attribute(&source.join("sub/été\n.txt"), "user.gad", b"fichier");
    let before = properties::Snapshot::read(&source);
    let engine = Engine::new();
    engine.request(&json!({"command":"enqueue","mode":"move","sources":[source],"destination":f.0.join("dest"),"verify_properties":true}).to_string()).unwrap();
    let done = wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert_eq!(
        done["jobs"][0]["entries"][0]["undo"]["directory_move"],
        true
    );
    assert!(done["jobs"][0]["entries"]
        .as_array()
        .unwrap()
        .iter()
        .all(|e| e["properties"]["status"] == "ok"));
    engine.request(r#"{"command":"undo_all"}"#).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "undone");
    assert!(properties::compare(&before, &properties::Snapshot::read(&source)).ok());
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Le contrôle d’un lien symbolique ne doit pas lire les attributs du fichier auquel il renvoie.
fn properties_symlink_is_checked_without_following_target() {
    let f = Fixture::local();
    let source = f.0.join("source/link");
    set_test_attribute(
        &f.0.join("source/sub/été\n.txt"),
        "user.gad",
        b"attribut de la cible",
    );
    symlink("sub/été\n.txt", &source).unwrap();
    let engine = Engine::new();
    engine.request(&json!({"command":"enqueue","sources":[source],"destination":f.0.join("dest"),"verify_properties":true}).to_string()).unwrap();
    let done = wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert_eq!(done["jobs"][0]["entries"][0]["properties"]["status"], "ok");
    assert_eq!(
        fs::read_link(f.0.join("dest/link")).unwrap(),
        Path::new("sub/été\n.txt")
    );
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Une ACL POSIX réelle est détectée comme perdue même si les bits de permissions ont été reproduits.
fn properties_detects_lost_posix_acl() {
    let f = Fixture::local();
    let source = f.0.join("source/sub/été\n.txt");
    let uid = fs::metadata(&source).unwrap().uid();
    let mut acl = 2u32.to_le_bytes().to_vec();
    for (tag, permission, id) in [
        (1u16, 6u16, u32::MAX),
        (2, 4, uid + 123),
        (4, 4, u32::MAX),
        (16, 4, u32::MAX),
        (32, 0, u32::MAX),
    ] {
        acl.extend(tag.to_le_bytes());
        acl.extend(permission.to_le_bytes());
        acl.extend(id.to_le_bytes());
    }
    set_test_attribute(&source, "system.posix_acl_access", &acl);
    let engine = Engine::new();
    engine.request(&json!({"command":"enqueue","sources":[source],"destination":f.0.join("dest"),"verify_properties":true}).to_string()).unwrap();
    let done = wait(&engine, |s| s["jobs"][0]["state"] == "done");
    let report = &done["jobs"][0]["entries"][0]["properties"];
    assert_eq!(report["status"], "different");
    assert!(report["differences"]
        .as_array()
        .unwrap()
        .iter()
        .any(|text| text.as_str().unwrap().starts_with("Access ACL")));
    assert_eq!(
        fs::metadata(source).unwrap().mode() & 0o7777,
        fs::metadata(f.0.join("dest/été\n.txt")).unwrap().mode() & 0o7777
    );
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// L’option est figée par lot et les rapports restent consultables après redémarrage.
fn properties_option_is_captured_and_report_is_persisted() {
    let f = Fixture::local();
    let journal = f.0.join("journal/state.json");
    let source = f.0.join("source/sub/été\n.txt");
    let engine = Engine::persistent(journal.clone()).unwrap();
    engine
        .request(r#"{"command":"configure","keep_history":true,"verify_properties":true}"#)
        .unwrap();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    engine
        .request(
            &json!({"command":"enqueue","sources":[source],"destination":f.0.join("dest")})
                .to_string(),
        )
        .unwrap();
    engine
        .request(r#"{"command":"configure","verify_properties":false}"#)
        .unwrap();
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    let done = wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert_eq!(done["verify_properties"], false);
    assert_eq!(done["jobs"][0]["verify_properties"], true);
    assert_eq!(done["jobs"][0]["entries"][0]["properties"]["status"], "ok");
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
    let engine = Engine::persistent(journal).unwrap();
    let state = engine.request(r#"{"command":"status"}"#).unwrap();
    assert_eq!(
        state["jobs"][0]["entries"][0]["properties"],
        done["jobs"][0]["entries"][0]["properties"]
    );
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Un lot repris garde son contrôle de propriétés même si le réglage global a changé depuis sa création.
fn properties_resumed_file_keeps_its_captured_option() {
    let f = ResumeFixture::new("copy", true, false);
    f.edit_journal(|state| {
        state["verify_properties"] = json!(false);
        state["jobs"][0]["verify_properties"] = json!(true);
    });
    let engine = Engine::persistent(f.journal.clone()).unwrap();
    let done = wait(&engine, |s| s["jobs"][0]["state"] == "done");
    let entry = &done["jobs"][0]["entries"][0];
    assert_eq!(entry["resumed_from"], f.bytes);
    assert_eq!(entry["properties"]["status"], "ok");
    assert_eq!(fs::read(&f.target).unwrap(), f.data);
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Un fichier ajouté séparément ne doit pas être inclus dans la sélection du dossier qui l’a aussi découvert.
fn tree_directory_selection_respects_separate_destination_roots() {
    for command in ["pause_selection", "cancel_selection", "delete_selection"] {
        let f = Fixture::local();
        let engine = Engine::new();
        engine
            .request(r#"{"command":"pause","paused":true}"#)
            .unwrap();
        engine.request(&json!({"command":"enqueue","sources":[f.0.join("source/sub"),f.0.join("source/sub/été\n.txt")],"destination":f.0.join("dest")}).to_string()).unwrap();
        wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
        engine
            .request(
                &json!({"command":command,"jobs":[],"entries":[{"job":1,"index":0}],"paused":true})
                    .to_string(),
            )
            .unwrap();
        let state = engine.request(r#"{"command":"status"}"#).unwrap();
        let entries = &state["jobs"][0]["entries"];
        assert_eq!(entries[1]["source"], entries[2]["source"]);
        assert_ne!(entries[1]["destination"], entries[2]["destination"]);
        let field = if command == "pause_selection" {
            "paused"
        } else if command == "delete_selection" {
            "hidden"
        } else {
            "cancel"
        };
        assert_eq!(entries[0][field], true);
        assert_eq!(entries[1][field], true);
        assert_eq!(entries[2][field], false);
        assert_eq!(
            fs::read(f.0.join("source/sub/été\n.txt")).unwrap(),
            b"bonjour\0monde"
        );
        assert_eq!(fs::read_dir(f.0.join("dest")).unwrap().count(), 0);
        engine.request(r#"{"command":"cancel_all"}"#).unwrap();
        wait(&engine, |s| s["jobs"][0]["state"] == "cancelled");
        engine.request(r#"{"command":"shutdown"}"#).unwrap();
    }
}

#[test]
/// Un dossier devient OK quand ses propres enfants ont fini, sans attendre une copie indépendante du même fichier.
fn tree_directory_completion_ignores_other_destination_branch() {
    let f = Fixture::local();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    engine.request(&json!({"command":"enqueue","sources":[f.0.join("source/sub"),f.0.join("source/sub/été\n.txt")],"destination":f.0.join("dest")}).to_string()).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    let mut job = engine.shared.state.lock().unwrap().jobs[0].snapshot();
    job.entries[0].state = TransferState::Running;
    job.entries[1].state = TransferState::Done;
    job.entries[2].state = TransferState::Waiting;
    finish_directories(&mut job);
    assert_eq!(job.entries[0].state, TransferState::Done);
    assert_eq!(job.entries[2].state, TransferState::Waiting);
    engine.request(r#"{"command":"cancel_all"}"#).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "cancelled");
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Réactiver une feuille indépendante ne doit pas recréer le dossier annulé d’une autre branche de destination.
fn tree_retry_independent_file_does_not_reactivate_other_branch() {
    let f = Fixture::local();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    engine
        .request(
            &json!({"command":"enqueue","sources":[f.0.join("source/sub"),f.0.join("source/sub/été
.txt")],"destination":f.0.join("dest")})
            .to_string(),
        )
        .unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    engine.request(r#"{"command":"cancel_all"}"#).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "cancelled");
    engine
        .request(r#"{"command":"retry_selection","jobs":[],"entries":[{"job":1,"index":2}]}"#)
        .unwrap();
    let done = wait(&engine, |s| {
        s["jobs"][0]["entries"][2]["state"] == "done"
            && terminal(s["jobs"][0]["state"].as_str().unwrap())
    });
    assert_eq!(done["jobs"][0]["entries"][0]["state"], "cancelled");
    assert_eq!(done["jobs"][0]["entries"][1]["state"], "cancelled");
    assert!(!f.0.join("dest/sub").exists());
    assert_eq!(
        fs::read(f.0.join(
            "dest/été
.txt"
        ))
        .unwrap(),
        b"bonjour monde"
    );
    engine.request(r#"{"command":"shutdown"}"#).unwrap();
}

#[test]
/// Les options sont persistées et figées à la soumission, comme les contrôles MD5.
fn space_option_is_captured_and_persisted() {
    let fixture = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    engine
        .request(r#"{"command":"configure","verify_space":true}"#)
        .unwrap();
    fixture.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    engine
        .request(r#"{"command":"configure","verify_space":false}"#)
        .unwrap();
    let state = engine.request(r#"{"command":"status"}"#).unwrap();
    assert_eq!(state["verify_space"], false);
    assert_eq!(state["jobs"][0]["verify_space"], true);
    let state = engine.shared.state.lock().unwrap();
    let restored: State = serde_json::from_slice(&serde_json::to_vec(&*state).unwrap()).unwrap();
    assert!(restored.jobs[0].verify_space);
    assert!(!restored.verify_space);
}
#[test]
/// Un fichier creux trop grand déclenche un refus avant toute allocation réelle ou création cible.
fn insufficient_space_stops_batch_before_writing_and_preserves_source() {
    let fixture = Fixture::local();
    let engine = Engine::new();
    let target = fixture.0.join("dest");
    let free = space::volume(&fixture.0.join("dest/file"))
        .unwrap()
        .available;
    let source = fixture.0.join("source/sub/été\n.txt");
    fs::OpenOptions::new()
        .write(true)
        .open(&source)
        .unwrap()
        .set_len(free.saturating_add(1024 * 1024))
        .unwrap();
    engine
        .request(r#"{"command":"configure","verify_space":true}"#)
        .unwrap();
    engine
        .request(
            &json!({"command":"enqueue","sources":[fixture.0.join("source")],"destination":target})
                .to_string(),
        )
        .unwrap();
    let state = wait(&engine, |s| s["jobs"][0]["state"] == "error");
    assert!(state["jobs"][0]["error"]
        .as_str()
        .unwrap()
        .contains("Espace insuffisant"));
    assert!(!target.join("source").exists());
    assert_eq!(fs::metadata(source).unwrap().len(), free + 1024 * 1024);
}
#[test]
/// Les montages imbriqués et liens finaux utilisent le parent réellement alloué.
fn space_native_query_handles_missing_descendants_and_target_symlink() {
    let fixture = Fixture::new();
    let v = space::volume(&fixture.0.join("absent/child/file")).unwrap();
    std::os::unix::fs::symlink("/missing/target", fixture.0.join("target-link")).unwrap();
    assert!(v.available > 0);
    assert!(space::volume(&fixture.0.join("target-link")).is_ok());
}

#[test]
/// Les renommages prouvés ne réservent pas une seconde fois tout le contenu du fichier.
fn space_check_does_not_block_fast_moves_on_same_filesystem() {
    let fixture = Fixture::local();
    let engine = Engine::new();
    let free = space::volume(&fixture.0.join("dest/file"))
        .unwrap()
        .available;
    let source = fixture.0.join("source/sub/été\n.txt");
    fs::OpenOptions::new()
        .write(true)
        .open(source)
        .unwrap()
        .set_len(free + 1024 * 1024)
        .unwrap();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    engine
        .request(r#"{"command":"configure","verify_space":true}"#)
        .unwrap();
    engine.request(&json!({"command":"enqueue","mode":"move","sources":[fixture.0.join("source")],"destination":fixture.0.join("dest")}).to_string()).unwrap();
    let state = wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    assert!(state["jobs"][0]["fast_move"].as_bool().unwrap());
    assert!(engine.check_queue_space(1).is_ok());
}

#[test]
/// Une origine distante simulée avertit sans empêcher une copie vers la fixture locale.
fn unknown_remote_space_warns_and_copy_still_finishes() {
    let fixture = Fixture::new();
    let engine = Engine::new();
    engine.request(&json!({"command":"enqueue","verify_space":true,"sources":[fixture.0.join("source")],"destination":fixture.0.join("dest"),"destination_url":"sftp://example.invalid/destination"}).to_string()).unwrap();
    let state = wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert!(state["jobs"][0]["space_warning"]
        .as_str()
        .unwrap()
        .contains("cannot be checked"));
    assert_eq!(
        fs::read(fixture.0.join("dest/source/sub/été\n.txt")).unwrap(),
        b"bonjour\0monde"
    );
}

#[test]
/// Les priorités changent le prochain démarrage, sans changer les identifiants ni l’historique.
fn priority_next_changes_dispatch_and_persists_separate_queue_order() {
    let fixture = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    for number in 0..3 {
        fixture.enqueue_distinct(&engine, number);
    }
    wait(&engine, |s| {
        s["jobs"]
            .as_array()
            .unwrap()
            .iter()
            .all(|j| j["state"] == "waiting")
    });
    engine
        .request(r#"{"command":"reorder_queue","ids":[3],"action":"next"}"#)
        .unwrap();
    let state = engine.request(r#"{"command":"status"}"#).unwrap();
    assert_eq!(state["queue_order"], json!([3, 1, 2]));
    assert_eq!(
        state["jobs"]
            .as_array()
            .unwrap()
            .iter()
            .map(|j| j["id"].as_u64().unwrap())
            .collect::<Vec<_>>(),
        vec![1, 2, 3]
    );
    let mut restored: State = serde_json::from_value(state).unwrap();
    assert_eq!(restored.queue_order, vec![3, 1, 2]);
    restored.paused = false;
    assert_eq!(scheduling::next(&mut restored), Some((3, false)));
    let poll = engine.request(r#"{"command":"poll"}"#).unwrap();
    assert_eq!(poll["queue_order"], json!([3, 1, 2]));
}
#[test]
/// Plusieurs lots se déplacent en conservant leur ordre relatif, sans lever les pauses.
fn priority_moves_multiple_batches_stably_and_preserves_pauses() {
    let fixture = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    for number in 0..4 {
        fixture.enqueue_distinct(&engine, number);
    }
    wait(&engine, |s| {
        s["jobs"]
            .as_array()
            .unwrap()
            .iter()
            .all(|j| j["state"] == "waiting")
    });
    engine.update(2, |j| j.paused = true);
    for (action, expected) in [
        ("next", vec![2, 3, 1, 4]),
        ("down", vec![1, 2, 3, 4]),
        ("up", vec![2, 3, 1, 4]),
        ("last", vec![1, 4, 2, 3]),
    ] {
        engine
            .request(&json!({"command":"reorder_queue","ids":[3,2],"action":action}).to_string())
            .unwrap();
        let state = engine.request(r#"{"command":"status"}"#).unwrap();
        assert_eq!(state["queue_order"], json!(expected));
        assert_eq!(state["paused"], true);
        assert_eq!(state["jobs"][1]["paused"], true);
    }
}
#[test]
/// Toute sélection périmée ou invalide est rejetée avant une mutation de la file.
fn priority_rejects_invalid_and_active_batches_atomically() {
    let fixture = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    for number in 0..2 {
        fixture.enqueue_distinct(&engine, number);
    }
    wait(&engine, |s| {
        s["jobs"]
            .as_array()
            .unwrap()
            .iter()
            .all(|j| j["state"] == "waiting")
    });
    for request in [
        json!({"ids":[1,999],"action":"next"}),
        json!({"ids":[1,1],"action":"next"}),
        json!({"ids":[],"action":"up"}),
        json!({"ids":[2],"action":"invalid"}),
    ] {
        let mut request = request;
        request["command"] = json!("reorder_queue");
        assert!(engine.request(&request.to_string()).is_err());
        assert_eq!(
            engine.request(r#"{"command":"status"}"#).unwrap()["queue_order"],
            json!([1, 2])
        );
    }
    engine.shared.state.lock().unwrap().active.insert(1, None);
    assert!(engine
        .request(r#"{"command":"reorder_queue","ids":[1,2],"action":"next"}"#)
        .is_err());
    assert_eq!(
        engine.request(r#"{"command":"status"}"#).unwrap()["queue_order"],
        json!([1, 2])
    );
    engine.shared.state.lock().unwrap().active.remove(&1);
}
#[test]
/// Les sauvegardes sont restaurées dans l’ordre réel des remplacements, après inversion des priorités.
fn priority_reordered_replacements_undo_in_reverse_execution_order() {
    let fixture = Fixture::new();
    let engine = Engine::new();
    for (folder, text) in [
        ("first", b"premier".as_slice()),
        ("second", b"second".as_slice()),
    ] {
        fs::create_dir(fixture.0.join(folder)).unwrap();
        fs::write(fixture.0.join(folder).join("same.txt"), text).unwrap();
    }
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    for folder in ["first", "second"] {
        engine.request(&json!({"command":"enqueue","sources":[fixture.0.join(folder).join("same.txt")],"destination":fixture.0.join("dest")}).to_string()).unwrap();
    }
    wait(&engine, |s| {
        s["jobs"]
            .as_array()
            .unwrap()
            .iter()
            .all(|j| j["state"] == "waiting")
    });
    engine
        .request(r#"{"command":"reorder_queue","ids":[2],"action":"next"}"#)
        .unwrap();
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    wait(&engine, |s| {
        s["jobs"][1]["state"] == "done" && s["jobs"][0]["entries"][0]["state"] == "conflict"
    });
    assert_eq!(
        fs::read(fixture.0.join("dest/same.txt")).unwrap(),
        b"second"
    );
    engine
        .request(r#"{"command":"resolve","id":1,"action":"replace","all":true}"#)
        .unwrap();
    let state = wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert!(
        state["jobs"][1]["execution_order"].as_u64().unwrap()
            < state["jobs"][0]["execution_order"].as_u64().unwrap()
    );
    assert_eq!(
        fs::read(fixture.0.join("dest/same.txt")).unwrap(),
        b"premier"
    );
    let result = request_undo(&engine, vec![1, 2]);
    assert!(
        result["jobs"]
            .as_array()
            .unwrap()
            .iter()
            .all(|j| j["state"] == "undone"),
        "{result}"
    );
    assert!(!fixture.0.join("dest/same.txt").exists());
}

#[test]
/// Le glisser-déposer de racines intercale les fichiers de deux demandes sans changer leurs destinations.
fn row_order_interleaves_independent_sources_and_keeps_undo_valid() {
    let f = Fixture::new();
    let engine = Engine::new();
    for name in ["a.txt", "b.txt", "c.txt"] {
        fs::write(f.0.join(name), name.as_bytes()).unwrap();
    }
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    engine.request(&json!({"command":"enqueue","sources":[f.0.join("a.txt"),f.0.join("c.txt")],"destination":f.0.join("dest")}).to_string()).unwrap();
    engine.request(&json!({"command":"enqueue","sources":[f.0.join("b.txt")],"destination":f.0.join("dest")}).to_string()).unwrap();
    wait(&engine, |s| {
        s["jobs"]
            .as_array()
            .unwrap()
            .iter()
            .all(|j| j["state"] == "waiting")
    });
    engine.request(r#"{"command":"reorder_rows","rows":[{"job":2,"index":0}],"target":{"job":1,"index":1},"action":"before"}"#).unwrap();
    let state = engine.request(r#"{"command":"status"}"#).unwrap();
    assert_eq!(
        state["row_order"],
        json!([{"job":1,"index":0},{"job":2,"index":0},{"job":1,"index":1}])
    );
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    let state = wait(&engine, |s| {
        s["jobs"]
            .as_array()
            .unwrap()
            .iter()
            .all(|j| j["state"] == "done")
    });
    assert!(
        state["jobs"][0]["execution_order"].as_u64().unwrap()
            > state["jobs"][1]["execution_order"].as_u64().unwrap()
    );
    for name in ["a.txt", "b.txt", "c.txt"] {
        assert_eq!(
            fs::read(f.0.join("dest").join(name)).unwrap(),
            name.as_bytes()
        );
    }
    let undone = request_undo(&engine, vec![1, 2]);
    assert!(
        undone["jobs"]
            .as_array()
            .unwrap()
            .iter()
            .all(|j| j["state"] == "undone"),
        "{undone}"
    );
}
#[test]
/// Les frères d’un dossier se réordonnent ; un conflit rend l’ordre d’exécution observable sans temporisation.
fn row_order_reorders_folder_children_without_changing_their_parent() {
    let f = Fixture::new();
    let engine = Engine::new();
    for name in ["a.txt", "b.txt"] {
        fs::write(f.0.join("source").join(name), name.as_bytes()).unwrap();
    }
    fs::create_dir(f.0.join("dest/source")).unwrap();
    for name in ["a.txt", "b.txt"] {
        fs::write(f.0.join("dest/source").join(name), b"ancien").unwrap();
    }
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    f.enqueue(&engine);
    let state = wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    let entries = state["jobs"][0]["entries"].as_array().unwrap();
    let a = entries
        .iter()
        .position(|e| e["source"].as_str().unwrap().ends_with("/a.txt"))
        .unwrap();
    let b = entries
        .iter()
        .position(|e| e["source"].as_str().unwrap().ends_with("/b.txt"))
        .unwrap();
    engine.request(&json!({"command":"reorder_rows","rows":[{"job":1,"index":b}],"target":{"job":1,"index":a},"action":"before"}).to_string()).unwrap();
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    let state = wait(&engine, |s| {
        s["jobs"][0]["entries"][b]["state"] == "conflict"
    });
    assert_eq!(state["jobs"][0]["entries"][a]["state"], "waiting");
    engine
        .request(r#"{"command":"resolve","id":1,"action":"skip","all":true}"#)
        .unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert_eq!(fs::read(f.0.join("dest/source/a.txt")).unwrap(), b"ancien");
}
#[test]
/// Un déplacement de priorité ne peut ni sortir un fichier de son dossier ni changer sa destination.
fn row_order_rejects_reparenting_and_stale_indices_atomically() {
    let f = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    f.enqueue(&engine);
    let before = wait(&engine, |s| s["jobs"][0]["state"] == "waiting")["row_order"].clone();
    for request in [
        r#"{"command":"reorder_rows","rows":[{"job":1,"index":2}],"target":{"job":1,"index":0},"action":"before"}"#,
        r#"{"command":"reorder_rows","rows":[{"job":1,"index":999}],"action":"next"}"#,
    ] {
        assert!(engine.request(request).is_err());
        assert_eq!(
            engine.request(r#"{"command":"status"}"#).unwrap()["row_order"],
            before
        );
    }
}
#[test]
/// L’ordre complet ne traverse la frontière Qt qu’en cas de changement, sans envoi à chaque progression.
fn row_order_is_persisted_and_diff_poll_omits_unchanged_layout() {
    let f = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    f.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    let first = engine.request(r#"{"command":"poll"}"#).unwrap();
    let revision = first["revision"].clone();
    engine
        .request(r#"{"command":"configure","verify_md5":true}"#)
        .unwrap();
    let delta = engine
        .request(&json!({"command":"poll","since":revision}).to_string())
        .unwrap();
    assert!(delta.get("row_order").is_none());
    engine
        .request(r#"{"command":"reorder_rows","rows":[{"job":1,"index":2}],"action":"next"}"#)
        .unwrap();
    let state = engine.shared.state.lock().unwrap();
    let restored: State = serde_json::from_slice(&serde_json::to_vec(&*state).unwrap()).unwrap();
    assert!(!restored.row_order.is_empty());
}

#[test]
/// Les chaînes de remplacements restent restaurables : on déplace leurs demandes ensemble.
fn row_order_refuses_interleaved_replacements_but_accepts_a_whole_request() {
    let f = Fixture::new();
    let engine = Engine::new();
    for folder in ["first", "second"] {
        fs::create_dir(f.0.join(folder)).unwrap();
        for name in ["x", "y"] {
            fs::write(f.0.join(folder).join(name), folder.as_bytes()).unwrap();
        }
    }
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    for folder in ["first", "second"] {
        engine.request(&json!({"command":"enqueue","sources":[f.0.join(folder).join("x"),f.0.join(folder).join("y")],"destination":f.0.join("dest")}).to_string()).unwrap();
    }
    let before = wait(&engine, |s| {
        s["jobs"]
            .as_array()
            .unwrap()
            .iter()
            .all(|j| j["state"] == "waiting")
    })["row_order"]
        .clone();
    assert!(engine.request(r#"{"command":"reorder_rows","rows":[{"job":2,"index":0}],"target":{"job":1,"index":1},"action":"before"}"#).unwrap_err().contains("shared destinations"));
    assert_eq!(
        engine.request(r#"{"command":"status"}"#).unwrap()["row_order"],
        before
    );
    engine.request(r#"{"command":"reorder_rows","rows":[{"job":2,"index":0},{"job":2,"index":1}],"target":{"job":1,"index":0},"action":"before"}"#).unwrap();
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    wait(&engine, |s| {
        s["jobs"][0]["entries"][0]["state"] == "conflict"
    });
    engine
        .request(r#"{"command":"resolve","id":1,"action":"replace","all":true}"#)
        .unwrap();
    wait(&engine, |s| {
        s["jobs"]
            .as_array()
            .unwrap()
            .iter()
            .all(|j| j["state"] == "done")
    });
    let undone = request_undo(&engine, vec![1, 2]);
    assert!(
        undone["jobs"]
            .as_array()
            .unwrap()
            .iter()
            .all(|j| j["state"] == "undone"),
        "{undone}"
    );
}
#[test]
/// Une destination répétée dans la même demande garde l’ordre attendu par son journal.
fn row_order_keeps_duplicate_destinations_in_original_entry_order() {
    let f = Fixture::new();
    let engine = Engine::new();
    for folder in ["one", "two"] {
        fs::create_dir(f.0.join(folder)).unwrap();
        fs::write(f.0.join(folder).join("same"), folder.as_bytes()).unwrap();
    }
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    engine.request(&json!({"command":"enqueue","sources":[f.0.join("one/same"),f.0.join("two/same")],"destination":f.0.join("dest")}).to_string()).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    assert!(engine.request(r#"{"command":"reorder_rows","rows":[{"job":1,"index":1}],"target":{"job":1,"index":0},"action":"before"}"#).unwrap_err().contains("same destination"));
}

#[test]
/// Une demande partiellement publiée bloque ses dépendantes pendant la pause d’un autre fichier.
fn row_order_partial_request_keeps_replacement_dependencies_until_completion() {
    let f = Fixture::new();
    let engine = Engine::new();
    fs::create_dir(f.0.join("other")).unwrap();
    for (path, content) in [
        ("a", b"original".as_slice()),
        ("c", b"fin".as_slice()),
        ("other/a", b"remplacement".as_slice()),
    ] {
        fs::write(f.0.join(path), content).unwrap();
    }
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    engine.request(&json!({"command":"enqueue","sources":[f.0.join("a"),f.0.join("c")],"destination":f.0.join("dest")}).to_string()).unwrap();
    engine.request(&json!({"command":"enqueue","sources":[f.0.join("other/a")],"destination":f.0.join("dest")}).to_string()).unwrap();
    wait(&engine, |s| {
        s["jobs"]
            .as_array()
            .unwrap()
            .iter()
            .all(|j| j["state"] == "waiting")
    });
    engine.request(r#"{"command":"pause_selection","jobs":[],"entries":[{"job":1,"index":1}],"paused":true}"#).unwrap();
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    wait(&engine, |s| {
        s["jobs"][0]["entries"][0]["state"] == "done" && s["jobs"][0]["state"] == "waiting"
    });
    thread::sleep(Duration::from_millis(30));
    assert_eq!(
        engine.request(r#"{"command":"status"}"#).unwrap()["jobs"][1]["state"],
        "waiting"
    );
    engine.request(r#"{"command":"pause_selection","jobs":[],"entries":[{"job":1,"index":1}],"paused":false}"#).unwrap();
    wait(&engine, |s| {
        s["jobs"][1]["entries"][0]["state"] == "conflict"
    });
    engine
        .request(r#"{"command":"resolve","id":2,"action":"replace","all":true}"#)
        .unwrap();
    wait(&engine, |s| s["jobs"][1]["state"] == "done");
    let undone = request_undo(&engine, vec![1, 2]);
    assert!(
        undone["jobs"]
            .as_array()
            .unwrap()
            .iter()
            .all(|j| j["state"] == "undone"),
        "{undone}"
    );
}
#[test]
/// Une priorité donnée pendant l’analyse se transmet aux racines qui apparaissent ensuite.
fn row_order_scanning_placeholder_priority_survives_analysis() {
    let f = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    for number in 0..3 {
        f.enqueue_distinct(&engine, number);
    }
    wait(&engine, |s| {
        s["jobs"]
            .as_array()
            .unwrap()
            .iter()
            .all(|j| j["state"] == "waiting")
    });
    let entries = {
        let mut state = engine.shared.state.lock().unwrap();
        let job = &mut state.jobs[2];
        job.state = TransferState::Scanning;
        std::mem::take(&mut job.entries)
    };
    engine
        .request(r#"{"command":"reorder_rows","rows":[{"job":3,"index":null}],"action":"next"}"#)
        .unwrap();
    engine.update(3, |j| {
        j.entries = entries;
        j.state = TransferState::Waiting;
    });
    let state = engine.request(r#"{"command":"status"}"#).unwrap();
    assert_eq!(state["row_order"][0], json!({"job":3,"index":0}));
}

#[test]
/// Finaliser une demande sans nouvelle écriture ne doit pas la faire passer après un remplacement réel.
fn row_order_cancelled_remaining_file_does_not_change_undo_chronology() {
    let f = Fixture::new();
    let engine = Engine::new();
    fs::create_dir(f.0.join("other")).unwrap();
    for name in ["a", "c", "other/a"] {
        fs::write(f.0.join(name), name.as_bytes()).unwrap();
    }
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    engine.request(&json!({"command":"enqueue","sources":[f.0.join("a"),f.0.join("c")],"destination":f.0.join("dest")}).to_string()).unwrap();
    engine.request(&json!({"command":"enqueue","sources":[f.0.join("other/a")],"destination":f.0.join("dest")}).to_string()).unwrap();
    wait(&engine, |s| {
        s["jobs"]
            .as_array()
            .unwrap()
            .iter()
            .all(|j| j["state"] == "waiting")
    });
    engine.request(r#"{"command":"pause_selection","jobs":[],"entries":[{"job":1,"index":1}],"paused":true}"#).unwrap();
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    wait(&engine, |s| {
        s["jobs"][0]["entries"][0]["state"] == "done" && s["jobs"][0]["state"] == "waiting"
    });
    engine
        .request(r#"{"command":"cancel_selection","jobs":[],"entries":[{"job":1,"index":1}]}"#)
        .unwrap();
    wait(&engine, |s| {
        s["jobs"][1]["entries"][0]["state"] == "conflict"
    });
    engine
        .request(r#"{"command":"resolve","id":2,"action":"replace","all":true}"#)
        .unwrap();
    wait(&engine, |s| {
        s["jobs"]
            .as_array()
            .unwrap()
            .iter()
            .all(|j| terminal(j["state"].as_str().unwrap()))
    });
    let undone = request_undo(&engine, vec![1, 2]);
    assert!(
        undone["jobs"]
            .as_array()
            .unwrap()
            .iter()
            .all(|j| j["state"] == "undone"),
        "{undone}"
    );
}

#[test]
/// Une entrée homonyme ne dépasse pas son prédécesseur en pause, même sans glisser-déposer.
fn row_order_duplicate_target_waits_for_paused_predecessor() {
    let f = Fixture::new();
    let engine = Engine::new();
    for folder in ["one", "two"] {
        fs::create_dir(f.0.join(folder)).unwrap();
        fs::write(f.0.join(folder).join("same"), folder.as_bytes()).unwrap();
    }
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    engine.request(&json!({"command":"enqueue","sources":[f.0.join("one/same"),f.0.join("two/same")],"destination":f.0.join("dest")}).to_string()).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    engine.request(r#"{"command":"pause_selection","jobs":[],"entries":[{"job":1,"index":0}],"paused":true}"#).unwrap();
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    thread::sleep(Duration::from_millis(30));
    assert!(!f.0.join("dest/same").exists());
    engine.request(r#"{"command":"pause_selection","jobs":[],"entries":[{"job":1,"index":0}],"paused":false}"#).unwrap();
    wait(&engine, |s| {
        s["jobs"][0]["entries"][1]["state"] == "conflict"
    });
    engine
        .request(r#"{"command":"resolve","id":1,"action":"replace","all":true}"#)
        .unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    let undone = request_undo(&engine, vec![1]);
    assert_eq!(undone["jobs"][0]["state"], "undone");
}
#[test]
/// La normalisation est commune aux alias de parents et ne suit pas le lien final à remplacer.
fn row_order_destination_keys_normalize_parents_without_following_final_symlink() {
    let f = Fixture::new();
    symlink(f.0.join("dest"), f.0.join("alias")).unwrap();
    symlink("/outside/file", f.0.join("dest/link")).unwrap();
    let keys = fast_move::destination_keys(
        [
            f.0.join("dest/file"),
            f.0.join("alias/file"),
            f.0.join("dest/link"),
        ]
        .into_iter(),
    )
    .unwrap();
    assert_eq!(keys[0], keys[1]);
    assert_ne!(keys[0], keys[2]);
    assert_eq!(
        fs::read_link(f.0.join("dest/link")).unwrap(),
        Path::new("/outside/file")
    );
}
#[test]
/// Des chemins d’affichage distincts correspondant au même nom sur un montage gardent leur dépendance.
fn row_order_normalized_aliases_keep_same_request_predecessors() {
    let f = Fixture::new();
    let engine = Engine::new();
    for name in ["a", "b"] {
        fs::write(f.0.join(name), name.as_bytes()).unwrap();
    }
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    engine.request(&json!({"command":"enqueue","sources":[f.0.join("a"),f.0.join("b")],"destination":f.0.join("dest")}).to_string()).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    {
        let mut state = engine.shared.state.lock().unwrap();
        state.jobs[0].destination_keys =
            Some(vec!["/device-test/same".into(), "/device-test/same".into()]);
        state.row_layout = None;
    }
    assert!(engine.request(r#"{"command":"reorder_rows","rows":[{"job":1,"index":1}],"target":{"job":1,"index":0},"action":"before"}"#).is_err());
    let mut state = engine.shared.state.lock().unwrap();
    state.jobs[0].entries[0].paused = true;
    state.paused = false;
    assert_eq!(scheduling::next(&mut state), None);
    state.paused = true;
}

#[test]
/// Garder les deux ne prend pas le nom d’une entrée antérieure déplacée plus loin dans la file.
fn row_order_rename_preserves_names_reserved_by_other_pending_entries() {
    let f = Fixture::new();
    let engine = Engine::new();
    fs::write(f.0.join("a.txt"), b"nouveau").unwrap();
    fs::write(f.0.join("a (copie 1).txt"), b"prevu").unwrap();
    fs::write(f.0.join("dest/a.txt"), b"ancien").unwrap();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    engine.request(&json!({"command":"enqueue","sources":[f.0.join("a (copie 1).txt"),f.0.join("a.txt")],"destination":f.0.join("dest")}).to_string()).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    engine.request(r#"{"command":"reorder_rows","rows":[{"job":1,"index":1}],"target":{"job":1,"index":0},"action":"before"}"#).unwrap();
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    wait(&engine, |s| {
        s["jobs"][0]["entries"][1]["state"] == "conflict"
    });
    engine
        .request(r#"{"command":"resolve","id":1,"action":"rename","all":false}"#)
        .unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert_eq!(
        fs::read(f.0.join("dest/a (copie 1).txt")).unwrap(),
        b"prevu"
    );
    assert_eq!(
        fs::read(f.0.join("dest/a (copie 2).txt")).unwrap(),
        b"nouveau"
    );
    let undone = request_undo(&engine, vec![1]);
    assert_eq!(undone["jobs"][0]["state"], "undone");
    assert_eq!(fs::read(f.0.join("dest/a.txt")).unwrap(), b"ancien");
}

#[test]
/// Un double collage sous pause conserve uniquement la première demande et sa pause.
fn duplicates_repeated_paused_background_paste_is_ignored() {
    let f = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    f.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    engine.request(&json!({"command":"enqueue","sources":[f.0.join("source")],"destination":f.0.join("dest"),"background":true}).to_string()).unwrap();
    let state = wait(&engine, |s| s["duplicates_ignored"] == 3);
    assert_eq!(state["jobs"].as_array().unwrap().len(), 1);
    assert_eq!(state["paused"], true);
    assert!(!f.0.join("dest/source").exists());
}

#[test]
/// Les analyses concurrentes réservent les opérations dans l’ordre des demandes.
fn duplicates_simultaneous_submissions_keep_oldest() {
    let f = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    for _ in 0..8 {
        f.enqueue(&engine);
    }
    let state = wait(&engine, |s| s["duplicates_ignored"] == 21);
    assert_eq!(state["jobs"].as_array().unwrap().len(), 1);
    assert_eq!(state["jobs"][0]["id"], 1);
    assert_eq!(state["jobs"][0]["entries"].as_array().unwrap().len(), 3);
}

#[test]
/// Une sélection répétée dans une même demande ne crée pas deux copies.
fn duplicates_inside_submission_keep_one_tree() {
    let f = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    engine.request(&json!({"command":"enqueue","sources":[f.0.join("source"),f.0.join("source")],"destination":f.0.join("dest")}).to_string()).unwrap();
    let state = wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    assert_eq!(state["jobs"][0]["entries"].as_array().unwrap().len(), 3);
    assert_eq!(state["duplicates_ignored"], 3);
}

#[test]
/// Un fichier déjà prévu est retiré d’un nouveau dossier, dont les nouveaux enfants restent copiables.
fn duplicates_overlapping_folder_keeps_unique_children_and_parents() {
    let f = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    fs::create_dir_all(f.0.join("dest/source/sub")).unwrap();
    let file = f.0.join("source/sub/été\n.txt");
    engine.request(&json!({"command":"enqueue","sources":[file],"destination":f.0.join("dest/source/sub")}).to_string()).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    fs::write(f.0.join("source/sub/new"), b"new").unwrap();
    f.enqueue(&engine);
    let state = wait(&engine, |s| s["jobs"][1]["state"] == "waiting");
    let entries = state["jobs"][1]["entries"].as_array().unwrap();
    assert_eq!(entries.len(), 3);
    assert!(entries.iter().all(|e| e["source"] != json!(file)));
    assert_eq!(state["duplicates_ignored"], 1);
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    wait(&engine, |s| {
        s["jobs"]
            .as_array()
            .unwrap()
            .iter()
            .all(|j| j["state"] == "done")
    });
    assert_eq!(fs::read(f.0.join("dest/source/sub/new")).unwrap(), b"new");
}

#[test]
/// Les alias des parents source et destination désignent bien la même opération.
fn duplicates_parent_aliases_are_recognized() {
    let f = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    symlink(f.0.join("source"), f.0.join("alias")).unwrap();
    symlink(f.0.join("dest"), f.0.join("target-alias")).unwrap();
    let file = f.0.join("source/sub/été\n.txt");
    for (source, target) in [
        (file, f.0.join("dest")),
        (f.0.join("alias/sub/été\n.txt"), f.0.join("target-alias")),
    ] {
        engine
            .request(
                &json!({"command":"enqueue","sources":[source],"destination":target}).to_string(),
            )
            .unwrap();
    }
    let state = wait(&engine, |s| s["duplicates_ignored"] == 1);
    assert_eq!(state["jobs"].as_array().unwrap().len(), 1);
}

#[test]
/// La destination et le type d’opération distinguent des intentions différentes.
fn duplicates_other_destination_and_move_are_preserved() {
    let f = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    f.enqueue(&engine);
    f.enqueue_distinct(&engine, 2);
    engine.request(&json!({"command":"enqueue","mode":"move","sources":[f.0.join("source")],"destination":f.0.join("dest")}).to_string()).unwrap();
    let state = wait(&engine, |s| {
        s["jobs"].as_array().unwrap().len() == 3
            && s["jobs"]
                .as_array()
                .unwrap()
                .iter()
                .all(|j| j["state"] == "waiting")
    });
    assert_eq!(state["duplicates_ignored"], 0);
}

#[test]
/// Deux noms source distincts restent distincts, même s’ils sont des liens physiques.
fn duplicates_hardlinks_and_real_conflicts_are_preserved() {
    let f = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    fs::create_dir(f.0.join("other")).unwrap();
    let a = f.0.join("source/sub/été\n.txt");
    let b = f.0.join("other/été\n.txt");
    fs::hard_link(&a, &b).unwrap();
    engine
        .request(
            &json!({"command":"enqueue","sources":[a,b],"destination":f.0.join("dest")})
                .to_string(),
        )
        .unwrap();
    let state = wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    assert_eq!(state["jobs"][0]["entries"].as_array().unwrap().len(), 2);
    assert_eq!(state["duplicates_ignored"], 0);
}

#[test]
/// Une demande annulée ne réserve plus les fichiers ; une copie terminée peut être répétée volontairement.
fn duplicates_cancelled_and_completed_history_allow_new_request() {
    let f = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    f.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    engine.request(r#"{"command":"cancel_all"}"#).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "cancelled");
    f.enqueue(&engine);
    wait(&engine, |s| s["jobs"][1]["state"] == "waiting");
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    wait(&engine, |s| s["jobs"][1]["state"] == "done");
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    f.enqueue(&engine);
    let state = wait(&engine, |s| s["jobs"][2]["state"] == "waiting");
    assert_eq!(state["duplicates_ignored"], 0);
}

#[test]
/// La reprise d’une file ancienne conserve les indices publiés en masquant seulement les doublons.
fn duplicates_restored_unstarted_entries_keep_stable_indices() {
    let f = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    f.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    fs::write(f.0.join("source/new"), b"new").unwrap();
    let entries = {
        let mut state = engine.shared.state.lock().unwrap();
        let mut cloned = state.jobs[0].snapshot();
        cloned.id = 2;
        cloned.state = TransferState::Scanning;
        let mut entries = cloned.entries.clone();
        let mut new = entries.last().unwrap().clone();
        new.source = f.0.join("source/new");
        new.destination = f.0.join("dest/source/new");
        let m = fs::metadata(&new.source).unwrap();
        new.identity = (m.dev(), m.ino());
        new.size = m.len();
        entries.push(new);
        cloned.entries = entries.clone();
        state.jobs.push(cloned.into());
        entries
    };
    let filtered = engine.filter_duplicates(2, entries).unwrap();
    assert_eq!(filtered.entries.len(), 4);
    assert_eq!(filtered.removed, 2); // Le dossier racine reste nécessaire au nouvel enfant.
    assert!(filtered.entries[1].hidden && filtered.entries[2].hidden);
    assert_eq!(filtered.entries[3].source, f.0.join("source/new"));
    engine.update(2, |j| {
        j.execution_order = 1;
    });
    let again = engine.filter_duplicates(2, filtered.entries).unwrap();
    assert_eq!(again.removed, 0); // Aucun filtrage d’une opération déjà exécutée.
}

#[test]
/// Les limites sont modifiables pendant l’attente et la suppression des plafonds termine la copie.
fn rate_limits_live_change_pause_and_unlimited_completion() {
    let f = Fixture::new();
    let engine = Engine::new();
    fs::write(f.0.join("source/large"), vec![5u8; 128 * 1024]).unwrap();
    engine
        .request(r#"{"command":"configure","rate_limit":16384}"#)
        .unwrap();
    f.enqueue(&engine);
    wait(&engine, |s| {
        s["jobs"][0]["entries"].as_array().is_some_and(|entries| {
            entries
                .iter()
                .any(|e| e["copied"].as_u64().unwrap() > 0 && e["state"] == "running")
        })
    });
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    thread::sleep(Duration::from_millis(70));
    let before = engine.request(r#"{"command":"status"}"#).unwrap();
    engine
        .request(r#"{"command":"configure","rate_limit":0}"#)
        .unwrap();
    thread::sleep(Duration::from_millis(70));
    let after = engine.request(r#"{"command":"status"}"#).unwrap();
    assert_eq!(before["jobs"][0]["entries"], after["jobs"][0]["entries"]);
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert_eq!(
        fs::read(f.0.join("dest/source/large")).unwrap(),
        vec![5u8; 128 * 1024]
    );
}

#[test]
/// Un plafond individuel très bas n’empêche pas une annulation immédiate.
fn rate_individual_wait_can_be_cancelled() {
    let f = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    f.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    engine
        .request(r#"{"command":"set_rate_limit","ids":[1],"bytes_per_second":1}"#)
        .unwrap();
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    wait(&engine, |s| {
        s["jobs"][0]["entries"]
            .as_array()
            .unwrap()
            .iter()
            .any(|e| e["kind"] == "file" && e["state"] == "running")
    });
    let start = std::time::Instant::now();
    engine.request(r#"{"command":"cancel_all"}"#).unwrap();
    let s = wait(&engine, |s| s["jobs"][0]["state"] == "cancelled");
    assert!(start.elapsed() < Duration::from_secs(1));
    assert_eq!(s["jobs"][0]["rate_limit"], 1);
    assert!(!f.0.join("dest/source/sub/été\n.txt").exists());
}

#[test]
/// Les changements sont atomiques, persistés et rétrocompatibles avec l’absence de plafond.
fn rate_validation_persistence_and_defaults() {
    let f = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    f.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    for value in [json!(-1), json!(1.5), json!("10"), json!(rate::MAX + 1)] {
        assert!(engine
            .request(
                &json!({"command":"configure","rate_limit":value,"verify_md5":true}).to_string()
            )
            .is_err());
        assert!(engine
            .request(
                &json!({"command":"set_rate_limit","ids":[1],"bytes_per_second":value}).to_string()
            )
            .is_err());
    }
    assert!(engine
        .request(r#"{"command":"set_rate_limit","ids":[1,999],"bytes_per_second":10}"#)
        .is_err());
    let s = engine.request(r#"{"command":"status"}"#).unwrap();
    assert_eq!(s["rate_limit"], 0);
    assert_eq!(s["jobs"][0]["rate_limit"], 0);
    assert_eq!(s["verify_md5"], false);
    engine
        .request(r#"{"command":"configure","rate_limit":50000}"#)
        .unwrap();
    engine
        .request(r#"{"command":"set_rate_limit","ids":[1],"bytes_per_second":25000}"#)
        .unwrap();
    let mut s = engine.request(r#"{"command":"status"}"#).unwrap();
    let restored: State = serde_json::from_value(s.clone()).unwrap();
    assert_eq!(restored.rate_limit, 50000);
    assert_eq!(restored.jobs[0].rate_limit, 25000);
    s.as_object_mut().unwrap().remove("rate_limit");
    s["jobs"][0].as_object_mut().unwrap().remove("rate_limit");
    let restored: State = serde_json::from_value(s).unwrap();
    assert_eq!(restored.rate_limit, 0);
    assert_eq!(restored.jobs[0].rate_limit, 0);
    assert_eq!(engine.poll(None).unwrap()["rate_limit"], 50000);
    // Une préparation de réactivation utilise un instantané ; elle ne doit pas écraser un nouveau plafond.
    let mut stale = engine.shared.state.lock().unwrap().jobs[0].snapshot();
    engine
        .request(r#"{"command":"set_rate_limit","ids":[1],"bytes_per_second":40000}"#)
        .unwrap();
    engine.prepare_retry(&mut stale).unwrap();
    assert_eq!(
        engine.request(r#"{"command":"status"}"#).unwrap()["jobs"][0]["rate_limit"],
        40000
    );
}

/// Installe un ancrage de test sans débrancher ni monter un périphérique réel.
fn reconnect_guard(engine: &Engine, id: u64, path: PathBuf) {
    let anchor = reconnect::Anchor::at(path, false).unwrap();
    engine.update(id, |j| j.connections = vec![anchor]);
}
#[test]
/// Une source retirée pendant la copie conserve son préfixe durable, puis reprend après relecture.
fn reconnect_source_resumes_verified_prefix_and_keeps_pause() {
    let f = Fixture::new();
    let engine = Engine::new();
    let contents = vec![37u8; 8 * 1024 * 1024];
    fs::write(f.0.join("source/large"), &contents).unwrap();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    f.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    reconnect_guard(&engine, 1, f.0.join("source"));
    engine
        .request(r#"{"command":"configure","rate_limit":1048576}"#)
        .unwrap();
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    wait(&engine, |s| {
        s["jobs"][0]["entries"]
            .as_array()
            .unwrap()
            .iter()
            .any(|e| e["undo"]["resume"]["bytes"].as_u64().unwrap_or(0) > 0)
    });
    fs::rename(f.0.join("source"), f.0.join("offline")).unwrap();
    let state = wait(&engine, |s| s["jobs"][0]["state"] == "device_waiting");
    let file = state["jobs"][0]["entries"]
        .as_array()
        .unwrap()
        .iter()
        .find(|e| e["state"] == "device_waiting")
        .unwrap();
    let partial = PathBuf::from(file["undo"]["staging"].as_str().unwrap());
    assert!(partial.exists());
    assert!(file["undo"]["resume"]["bytes"].as_u64().unwrap() > 0);
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    fs::rename(f.0.join("offline"), f.0.join("source")).unwrap();
    engine
        .request(r#"{"command":"configure","rate_limit":0}"#)
        .unwrap();
    thread::sleep(Duration::from_millis(300));
    assert_eq!(
        engine.request(r#"{"command":"status"}"#).unwrap()["jobs"][0]["state"],
        "device_waiting"
    );
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    let done = wait(&engine, |s| {
        terminal(s["jobs"][0]["state"].as_str().unwrap())
    });
    assert_eq!(done["jobs"][0]["state"], "done", "{done}");
    assert!(done["jobs"][0]["entries"]
        .as_array()
        .unwrap()
        .iter()
        .any(|e| e["resumed_from"].as_u64().unwrap_or(0) > 0));
    assert_eq!(fs::read(f.0.join("dest/source/large")).unwrap(), contents);
    assert!(!partial.exists());
}
#[test]
/// Un dossier vide au même emplacement n’est pas le volume attendu ; aucun fichier n’y est créé.
fn reconnect_destination_waits_for_original_anchor_not_empty_replacement() {
    let f = Fixture::new();
    let engine = Engine::new();
    fs::write(f.0.join("source/large"), vec![9u8; 2 * 1024 * 1024]).unwrap();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    f.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    reconnect_guard(&engine, 1, f.0.join("dest"));
    engine
        .request(r#"{"command":"configure","rate_limit":1048576}"#)
        .unwrap();
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    wait(&engine, |s| {
        s["jobs"][0]["entries"]
            .as_array()
            .unwrap()
            .iter()
            .any(|e| e["kind"] == "file" && e["copied"].as_u64().unwrap() > 0)
    });
    fs::rename(f.0.join("dest"), f.0.join("offline")).unwrap();
    fs::create_dir(f.0.join("dest")).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "device_waiting");
    thread::sleep(Duration::from_millis(200));
    assert_eq!(fs::read_dir(f.0.join("dest")).unwrap().count(), 0);
    fs::remove_dir(f.0.join("dest")).unwrap();
    fs::rename(f.0.join("offline"), f.0.join("dest")).unwrap();
    engine
        .request(r#"{"command":"configure","rate_limit":0}"#)
        .unwrap();
    let done = wait(&engine, |s| {
        terminal(s["jobs"][0]["state"].as_str().unwrap())
    });
    assert_eq!(done["jobs"][0]["state"], "done", "{done}");
    assert_eq!(
        fs::metadata(f.0.join("dest/source/large")).unwrap().len(),
        2 * 1024 * 1024
    );
}
#[test]
/// Annuler pendant l’attente interdit tout redémarrage ultérieur et fonctionne sous pause globale.
fn reconnect_cancellation_never_restarts_after_return() {
    let f = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    f.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    reconnect_guard(&engine, 1, f.0.join("source"));
    fs::rename(f.0.join("source"), f.0.join("offline")).unwrap();
    assert!(engine.park_disconnected(1, 0, "test"));
    engine.request(r#"{"command":"cancel_all"}"#).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "cancelled");
    fs::rename(f.0.join("offline"), f.0.join("source")).unwrap();
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    thread::sleep(Duration::from_millis(100));
    assert!(!f.0.join("dest/source").exists());
}
#[test]
/// Un fichier remplacé hors ligne fait refuser la reprise, sans supprimer le temporaire conservé.
fn reconnect_refuses_changed_source_and_preserves_partial() {
    let f = Fixture::new();
    let engine = Engine::new();
    fs::write(f.0.join("source/large"), vec![3u8; 2 * 1024 * 1024]).unwrap();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    f.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    reconnect_guard(&engine, 1, f.0.join("source"));
    engine
        .request(r#"{"command":"configure","rate_limit":1048576}"#)
        .unwrap();
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    wait(&engine, |s| {
        s["jobs"][0]["entries"]
            .as_array()
            .unwrap()
            .iter()
            .any(|e| e["kind"] == "file" && e["copied"].as_u64().unwrap() > 0)
    });
    fs::rename(f.0.join("source"), f.0.join("offline")).unwrap();
    let state = wait(&engine, |s| s["jobs"][0]["state"] == "device_waiting");
    let entry = state["jobs"][0]["entries"]
        .as_array()
        .unwrap()
        .iter()
        .find(|e| e["state"] == "device_waiting")
        .unwrap();
    let partial = PathBuf::from(entry["undo"]["staging"].as_str().unwrap());
    fs::rename(f.0.join("offline/large"), f.0.join("offline/old-large")).unwrap();
    fs::write(f.0.join("offline/large"), vec![4u8; 2 * 1024 * 1024]).unwrap();
    fs::rename(f.0.join("offline"), f.0.join("source")).unwrap();
    let refused = wait(&engine, |s| s["jobs"][0]["state"] == "error");
    assert!(refused["jobs"][0]["error"]
        .as_str()
        .unwrap()
        .contains("Resume refused"));
    assert!(partial.exists());
    assert!(!f.0.join("dest/source/large").exists());
}

#[test]
/// Sans point durable distant, seul le temporaire non publié et reconnu est retiré avant un redémarrage du fichier.
fn reconnect_without_durable_point_restarts_only_owned_staging() {
    let f = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    engine.request(&json!({"command":"enqueue","sources":[f.0.join("source/sub/été\n.txt")],"destination":f.0.join("dest")}).to_string()).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    let part =
        f.0.join(format!("dest/.gad-{}-1-0-0.part", std::process::id()));
    fs::write(&part, b"partial").unwrap();
    engine.update(1, |j| {
        j.state = TransferState::Scanning;
        let e = &mut j.entries[0];
        e.state = TransferState::DeviceWaiting;
        e.interrupted = true;
        e.copied = 7;
        e.undo = Some(undo::Record {
            staging: Some(part.clone()),
            staging_stamp: Some(undo::Stamp::read(&part).unwrap()),
            source_stamp: Some(undo::Stamp::read(&e.source).unwrap()),
            ..Default::default()
        });
    });
    let snapshot = engine.shared.state.lock().unwrap().jobs[0].snapshot();
    let prepared = engine.prepare_reconnect(snapshot).unwrap();
    assert!(!part.exists());
    assert!(prepared.1[0].undo.is_none());
    assert_eq!(prepared.1[0].copied, 0);
    assert_eq!(prepared.1[0].state, TransferState::Waiting);
    engine.request(r#"{"command":"cancel_all"}"#).unwrap();
}
#[test]
/// Une interruption après publication ne peut pas déclencher une seconde copie automatique.
fn reconnect_after_publication_requires_recovery_and_keeps_result() {
    let f = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    engine.request(&json!({"command":"enqueue","sources":[f.0.join("source/sub/été\n.txt")],"destination":f.0.join("dest")}).to_string()).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    let target = f.0.join("dest/été\n.txt");
    fs::write(&target, b"published").unwrap();
    engine.update(1, |j| {
        j.state = TransferState::Scanning;
        let e = &mut j.entries[0];
        e.state = TransferState::DeviceWaiting;
        e.interrupted = true;
        e.undo = Some(undo::Record {
            result: Some(undo::Stamp::read(&target).unwrap()),
            source_stamp: Some(undo::Stamp::read(&e.source).unwrap()),
            ..Default::default()
        });
    });
    let snapshot = engine.shared.state.lock().unwrap().jobs[0].snapshot();
    assert!(engine
        .prepare_reconnect(snapshot)
        .err()
        .unwrap()
        .contains("publication interrupted"));
    assert_eq!(fs::read(target).unwrap(), b"published");
    engine.request(r#"{"command":"cancel_all"}"#).unwrap();
}

#[test]
/// Une demande en attente de périphérique ne monopolise pas le travailleur des copies indépendantes.
fn reconnect_releases_worker_for_other_requests() {
    let f = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    f.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    reconnect_guard(&engine, 1, f.0.join("source"));
    fs::rename(f.0.join("source"), f.0.join("offline")).unwrap();
    assert!(engine.park_disconnected(1, 0, "test"));
    let other = f.0.join("independent.txt");
    fs::write(&other, b"independent").unwrap();
    engine
        .request(
            &json!({"command":"enqueue","sources":[other],"destination":f.0.join("dest")})
                .to_string(),
        )
        .unwrap();
    wait(&engine, |s| s["jobs"][1]["state"] == "waiting");
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    let state = wait(&engine, |s| s["jobs"][1]["state"] == "done");
    assert_eq!(state["jobs"][0]["state"], "device_waiting");
    assert_eq!(
        fs::read(f.0.join("dest/independent.txt")).unwrap(),
        b"independent"
    );
    engine.request(r#"{"command":"cancel_all"}"#).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "cancelled");
    fs::rename(f.0.join("offline"), f.0.join("source")).unwrap();
}

#[test]
/// Une annulation et une pause ciblées reçues pendant la vérification ne sont pas écrasées par son ancien instantané.
fn reconnect_preparation_preserves_concurrent_entry_controls() {
    let f = Fixture::new();
    let engine = Engine::new();
    fs::write(f.0.join("source/a"), b"a").unwrap();
    fs::write(f.0.join("source/b"), b"b").unwrap();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    f.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    let (entries, a, b) = {
        let mut state = engine.shared.state.lock().unwrap();
        let job = &mut state.jobs[0];
        job.state = TransferState::Scanning;
        let entries = job.entries.clone();
        let a = entries
            .iter()
            .position(|e| e.source.ends_with("a"))
            .unwrap();
        let b = entries
            .iter()
            .position(|e| e.source.ends_with("b"))
            .unwrap();
        (entries, a, b)
    };
    let (send, receive) = std::sync::mpsc::channel();
    let destination = f.0.join("dest");
    let pending = engine.spawn_preparation(1, false, 0, move || {
        receive.recv().unwrap();
        Ok((destination, entries, None))
    });
    engine
        .request(
            &json!({"command":"cancel_selection","jobs":[],"entries":[{"job":1,"index":a}]})
                .to_string(),
        )
        .unwrap();
    engine.request(&json!({"command":"pause_selection","paused":true,"jobs":[],"entries":[{"job":1,"index":b}]}).to_string()).unwrap();
    send.send(()).unwrap();
    pending.join().unwrap();
    let state = engine.request(r#"{"command":"status"}"#).unwrap();
    assert_eq!(state["jobs"][0]["entries"][a]["cancel"], true);
    assert_eq!(state["jobs"][0]["entries"][b]["paused"], true);
    assert!(!f.0.join("dest/source/a").exists());
    engine.request(r#"{"command":"cancel_all"}"#).unwrap();
}

#[test]
/// Réessayer une copie partiellement échouée conserve les octets, inode et journal de son voisin OK.
fn errors_retry_only_failed_files_keeps_success_and_global_pause() {
    let f = Fixture::new();
    let engine = Engine::new();
    let bad = f.0.join("source/bad");
    fs::write(&bad, b"bad").unwrap();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    f.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    fs::remove_file(&bad).unwrap();
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    let failed = wait(&engine, |s| s["jobs"][0]["state"] == "error");
    wait(&engine, |_| {
        !engine.shared.state.lock().unwrap().active.contains_key(&1)
    });
    let good = f.0.join("dest/source/sub/été\n.txt");
    let before = fs::metadata(&good).unwrap();
    let report = engine.request(r#"{"command":"error_report"}"#).unwrap();
    assert_eq!(report["records"].as_array().unwrap().len(), 1);
    assert_eq!(report["records"][0]["retryable"], true);
    fs::write(&bad, b"fixed").unwrap();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    engine
        .request(r#"{"command":"retry_errors","jobs":[1],"entries":[]}"#)
        .unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    assert_eq!(
        engine.request(r#"{"command":"status"}"#).unwrap()["paused"],
        true
    );
    assert!(!f.0.join("dest/source/bad").exists());
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    let done = wait(&engine, |s| {
        terminal(s["jobs"][0]["state"].as_str().unwrap())
    });
    assert_eq!(done["jobs"][0]["state"], "done", "{done}");
    let after = fs::metadata(&good).unwrap();
    assert_eq!(
        (before.dev(), before.ino(), before.modified().unwrap()),
        (after.dev(), after.ino(), after.modified().unwrap())
    );
    let good_index = failed["jobs"][0]["entries"]
        .as_array()
        .unwrap()
        .iter()
        .position(|e| e["destination"] == json!(good))
        .unwrap();
    assert_eq!(
        failed["jobs"][0]["entries"][good_index]["undo"],
        done["jobs"][0]["entries"][good_index]["undo"]
    );
    assert_eq!(fs::read(f.0.join("dest/source/bad")).unwrap(), b"fixed");
    assert_eq!(
        engine.request(r#"{"command":"error_report"}"#).unwrap()["count"],
        0
    );
}

#[test]
/// Les lignes annulées, ignorées et simplement en attente ne sont pas activées par un réessai d’erreurs.
fn errors_retry_scope_preserves_cancelled_skipped_and_waiting() {
    let f = Fixture::new();
    let engine = Engine::new();
    for name in ["bad", "cancelled", "skipped", "pending"] {
        fs::write(f.0.join("source").join(name), name.as_bytes()).unwrap();
    }
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    f.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    engine.update(1, |j| {
        j.state = TransferState::Error;
        for e in &mut j.entries {
            match e.source.file_name().unwrap().to_string_lossy().as_ref() {
                "bad" => {
                    e.state = TransferState::Error;
                    e.error = "test".into();
                }
                "cancelled" => {
                    e.state = TransferState::Cancelled;
                    e.cancel = true;
                }
                "skipped" => e.state = TransferState::Skipped,
                "pending" => {}
                _ => e.state = TransferState::Done,
            }
        }
    });
    engine
        .request(r#"{"command":"retry_errors","jobs":[1],"entries":[]}"#)
        .unwrap();
    let state = wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    let rows = state["jobs"][0]["entries"].as_array().unwrap();
    assert!(rows
        .iter()
        .any(|e| e["source"].as_str().unwrap().ends_with("pending") && e["paused"] == true));
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    wait(&engine, |s| {
        s["jobs"][0]["entries"]
            .as_array()
            .unwrap()
            .iter()
            .any(|e| e["source"].as_str().unwrap().ends_with("bad") && e["state"] == "done")
    });
    for name in ["cancelled", "skipped", "pending"] {
        assert!(!f.0.join("dest/source").join(name).exists());
    }
    engine.request(r#"{"command":"cancel_all"}"#).unwrap();
}

#[test]
/// Une analyse sans lignes peut être relancée après réparation de la source ; les racines restent entièrement validées.
fn errors_retry_preparation_revalidates_roots_and_recovers_missing_source() {
    let f = Fixture::new();
    let engine = Engine::new();
    let source = f.0.join("missing");
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    engine
        .request(
            &json!({"command":"enqueue","sources":[source],"destination":f.0.join("dest")})
                .to_string(),
        )
        .unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "error");
    let report = engine.request(r#"{"command":"error_report"}"#).unwrap();
    assert_eq!(report["records"][0]["scope"], "job");
    assert_eq!(report["records"][0]["retryable"], true);
    fs::write(&source, b"available").unwrap();
    engine
        .request(r#"{"command":"retry_errors","jobs":[1],"entries":[]}"#)
        .unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert_eq!(fs::read(f.0.join("dest/missing")).unwrap(), b"available");
    engine.request(&json!({"command":"enqueue","sources":[f.0.join("source")],"destination":f.0.join("source")}).to_string()).unwrap();
    wait(&engine, |s| s["jobs"][1]["state"] == "error");
    engine
        .request(r#"{"command":"retry_errors","jobs":[2],"entries":[]}"#)
        .unwrap();
    let refused = wait(&engine, |s| s["jobs"][1]["state"] == "error");
    assert!(!refused["jobs"][1]["error"].as_str().unwrap().is_empty());
    assert!(!f.0.join("source/source").exists());
}

#[test]
/// Lire le bilan ne modifie ni la révision ni l’état et garde les détails complets, y compris les erreurs de finalisation.
fn errors_report_readonly_full_details_and_nonretryable_finalization() {
    let f = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    f.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    engine.update(1, |j| {
        j.state = TransferState::Error;
        j.error = "échec\n<texte>".into();
        j.entries[0].state = TransferState::Done;
        j.entries[0].error = "dossier source non nettoyé".into();
        j.entries[1].state = TransferState::DeviceWaiting;
        j.entries[1].error = "attente".into();
        j.entries[2].state = TransferState::Error;
        j.entries[2].error = "échec\n<texte>".into();
    });
    let before = engine.poll(None).unwrap();
    let report = engine.request(r#"{"command":"error_report"}"#).unwrap();
    assert_eq!(report["records"][0]["retryable"], false);
    assert!(report["records"]
        .as_array()
        .unwrap()
        .iter()
        .any(|r| r["error"] == "échec\n<texte>"));
    assert_eq!(report["format"], "gad-error-report");
    assert_eq!(report["version"], 1);
    let delta = engine.poll(before["revision"].as_u64()).unwrap();
    assert!(
        delta["unchanged"] == true || delta["jobs"].as_array().is_some_and(|jobs| jobs.is_empty())
    );
    assert!(!report["records"]
        .as_array()
        .unwrap()
        .iter()
        .any(|r| r["state"] == "device_waiting"));
    engine.request(r#"{"command":"cancel_all"}"#).unwrap();
}

#[test]
/// Une sélection mêlant une erreur inactive et une demande encore active est rejetée avant mutation.
fn errors_retry_validation_is_atomic_and_stale_indices_are_rejected() {
    let f = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    f.enqueue(&engine);
    f.enqueue_distinct(&engine, 2);
    wait(&engine, |s| s["jobs"][1]["state"] == "waiting");
    engine.update(1, |j| {
        j.state = TransferState::Error;
        j.entries[2].state = TransferState::Error;
    });
    engine.update(2, |j| j.entries[2].state = TransferState::Error);
    let before = engine.request(r#"{"command":"status"}"#).unwrap();
    assert!(engine
        .request(r#"{"command":"retry_errors","jobs":[1,2],"entries":[]}"#)
        .is_err());
    assert!(engine
        .request(r#"{"command":"retry_errors","jobs":[],"entries":[{"job":1,"index":9999}]}"#)
        .is_err());
    assert_eq!(
        before["jobs"],
        engine.request(r#"{"command":"status"}"#).unwrap()["jobs"]
    );
    engine.request(r#"{"command":"cancel_all"}"#).unwrap();
}

#[test]
/// Un échec de préparation du réessai peut être retenté après réparation, sans rester bloqué en restauration incomplète.
fn errors_retry_can_recover_after_failed_retry_preparation() {
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
    fs::remove_file(&source).unwrap();
    engine.update(1, |j| {
        j.state = TransferState::Error;
        j.entries[0].state = TransferState::Error;
        j.entries[0].error = "source absente".into();
    });
    engine
        .request(r#"{"command":"retry_errors","jobs":[1],"entries":[]}"#)
        .unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "undo_error");
    let report = engine.request(r#"{"command":"error_report"}"#).unwrap();
    assert!(report["records"]
        .as_array()
        .unwrap()
        .iter()
        .any(|r| r["scope"] == "entry" && r["retryable"] == true));
    fs::write(&source, b"fixed").unwrap();
    engine
        .request(r#"{"command":"retry_errors","jobs":[1],"entries":[]}"#)
        .unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    let done = wait(&engine, |s| {
        terminal(s["jobs"][0]["state"].as_str().unwrap())
    });
    assert_eq!(done["jobs"][0]["state"], "done", "{done}");
}

#[test]
/// Le bilan utilise des références exactes : choisir une erreur de dossier ne réessaie pas implicitement ses enfants masqués par un filtre.
fn errors_exact_selection_does_not_expand_failed_directory() {
    let f = Fixture::new();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    f.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    engine.update(1, |j| {
        j.state = TransferState::Error;
        j.entries[0].state = TransferState::Error;
        j.entries[0].error = "erreur dossier".into();
        j.entries[1].state = TransferState::Done;
        j.entries[2].state = TransferState::Error;
        j.entries[2].error = "erreur fichier".into();
    });
    let result = engine
        .request(
            r#"{"command":"retry_errors","jobs":[],"entries":[{"job":1,"index":0}],"exact":true}"#,
        )
        .unwrap();
    assert_eq!(result["retried"], 1);
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    let state = wait(&engine, |s| s["jobs"][0]["state"] == "error");
    assert_eq!(state["jobs"][0]["entries"][2]["state"], "error");
    assert!(!f.0.join("dest/source/sub/été\n.txt").exists());
}

#[test]
/// Un résultat modifié par l’utilisateur après une erreur n’est ni supprimé ni remplacé par le réessai.
fn errors_retry_preserves_modified_published_output() {
    let f = Fixture::new();
    let engine = Engine::new();
    f.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    wait(&engine, |_| {
        !engine.shared.state.lock().unwrap().active.contains_key(&1)
    });
    let target = f.0.join("dest/source/sub/été\n.txt");
    fs::write(&target, b"contenu utilisateur").unwrap();
    engine.update(1, |j| {
        j.state = TransferState::Error;
        j.entries[2].state = TransferState::Error;
        j.entries[2].error = "publication à vérifier".into();
    });
    engine
        .request(r#"{"command":"retry_errors","jobs":[1],"entries":[]}"#)
        .unwrap();
    let state = wait(&engine, |s| s["jobs"][0]["state"] == "undo_error");
    assert!(!state["jobs"][0]["error"].as_str().unwrap().is_empty());
    assert_eq!(fs::read(target).unwrap(), b"contenu utilisateur");
    assert_eq!(
        fs::read(f.0.join("source/sub/été\n.txt")).unwrap(),
        b"bonjour\0monde"
    );
}

#[test]
/// Les motifs sautent les dossiers avant d’en parcourir les fichiers et ne suivent jamais les liens.
fn exclusions_copy_skips_cache_temp_and_hidden_items() {
    let f = Fixture::new();
    fs::create_dir_all(f.0.join("source/__pycache__/nested")).unwrap();
    fs::write(f.0.join("source/__pycache__/nested/cache.pyc"), b"cache").unwrap();
    fs::write(f.0.join("source/work.tmp"), b"temp").unwrap();
    fs::write(f.0.join("source/backup~"), b"backup").unwrap();
    fs::write(f.0.join("source/.secret"), b"secret").unwrap();
    fs::create_dir(f.0.join("source/.hidden")).unwrap();
    fs::write(f.0.join("source/.hidden/visible"), b"hidden").unwrap();
    fs::write(f.0.join("source/__ordinary.txt"), b"keep").unwrap();
    symlink("__pycache__", f.0.join("source/cache-link")).unwrap();
    let engine = Engine::new();
    let id=engine.request(&json!({"command":"enqueue","sources":[f.0.join("source")],"destination":f.0.join("dest"),"exclusions":{"include_hidden":false,"directories":["__*"],"files":["*.tmp","*~"]}}).to_string()).unwrap()["id"].as_u64().unwrap();
    let done = wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert_eq!(done["jobs"][0]["id"], id);
    for path in ["__pycache__", "work.tmp", "backup~", ".secret", ".hidden"] {
        assert!(!f.0.join("dest/source").join(path).exists(), "{path}");
    }
    assert_eq!(
        fs::read(f.0.join("dest/source/__ordinary.txt")).unwrap(),
        b"keep"
    );
    assert_eq!(
        fs::read(f.0.join("dest/source/sub/été\n.txt")).unwrap(),
        b"bonjour\0monde"
    );
    assert_eq!(
        fs::read_link(f.0.join("dest/source/cache-link")).unwrap(),
        PathBuf::from("__pycache__")
    );
    assert!(done["jobs"][0]["entries"]
        .as_array()
        .unwrap()
        .iter()
        .all(|e| !e["source"].as_str().unwrap().contains("__pycache__")));
}

#[test]
/// Les réglages nouveaux ne changent ni les lots en pause ni les règles conservées au redémarrage.
fn exclusions_are_frozen_and_persisted_with_defaults() {
    let f = Fixture::new();
    fs::write(f.0.join("source/a.tmp"), b"temp").unwrap();
    fs::write(f.0.join("source/.hidden"), b"hidden").unwrap();
    let journal = f.0.join("state.json");
    let engine = Engine::persistent(journal.clone()).unwrap();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    engine.request(r#"{"command":"configure","keep_history":true,"exclusions":{"include_hidden":false,"files":["*.tmp"]}}"#).unwrap();
    f.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "waiting");
    engine
        .request(r#"{"command":"configure","exclusions":{}}"#)
        .unwrap();
    let state = engine.request(r#"{"command":"status"}"#).unwrap();
    assert_eq!(state["exclusions"]["include_hidden"], true);
    assert_eq!(state["jobs"][0]["exclusions"]["include_hidden"], false);
    assert_eq!(state["jobs"][0]["exclusions"]["files"], json!(["*.tmp"]));
    drop(engine);
    let loaded: State = serde_json::from_slice(&fs::read(journal).unwrap()).unwrap();
    assert!(!loaded.jobs[0].exclusions.include_hidden);
    assert!(loaded.exclusions.include_hidden);
}

#[test]
/// Les anciens transferts continuent d’inclure les cachés tant que l’option n’est pas désactivée.
fn exclusions_default_includes_hidden_and_validation_is_atomic() {
    let f = Fixture::new();
    fs::write(f.0.join("source/.keep"), b"hidden").unwrap();
    let engine = Engine::new();
    assert!(engine
        .request(r#"{"command":"configure","rate_limit":1024,"exclusions":{"files":["a/b"]}}"#)
        .is_err());
    assert_eq!(
        engine.request(r#"{"command":"status"}"#).unwrap()["rate_limit"],
        0
    );
    assert!(engine.request(&json!({"command":"enqueue","sources":[f.0.join("source")],"destination":f.0.join("dest"),"exclusions":{"include_hidden":"no"}}).to_string()).is_err());
    assert!(engine.request(r#"{"command":"status"}"#).unwrap()["jobs"]
        .as_array()
        .unwrap()
        .is_empty());
    f.enqueue(&engine);
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert_eq!(fs::read(f.0.join("dest/source/.keep")).unwrap(), b"hidden");
    let mut old = engine.request(r#"{"command":"status"}"#).unwrap()["jobs"][0].clone();
    old.as_object_mut().unwrap().remove("exclusions");
    let old: Job = serde_json::from_value(old).unwrap();
    assert!(old.exclusions.include_hidden);
}

#[test]
/// Les exclusions s’appliquent aussi aux racines explicitement sélectionnées, sans lever une pause inutilement.
fn exclusions_can_empty_a_background_request_without_resuming_queue() {
    let f = Fixture::new();
    let hidden = f.0.join(".hidden");
    fs::write(&hidden, b"keep").unwrap();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    engine.request(&json!({"command":"enqueue","sources":[hidden],"destination":f.0.join("dest"),"background":true,"exclusions":{"include_hidden":false}}).to_string()).unwrap();
    let done = wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert_eq!(done["paused"], true);
    assert!(done["jobs"][0]["entries"].as_array().unwrap().is_empty());
    assert!(!f.0.join("dest/.hidden").exists());
}

#[test]
/// Un déplacement filtré ne renomme jamais tout le dossier : les exclus restent sur place, même après remise en état.
fn exclusions_move_keeps_omitted_files_and_undo_restores_selected_files() {
    for md5 in [false, true] {
        let f = Fixture::local();
        fs::create_dir(f.0.join("source/__pycache__")).unwrap();
        fs::write(f.0.join("source/__pycache__/cache"), b"keep").unwrap();
        fs::write(f.0.join("source/file.tmp"), b"temp").unwrap();
        let engine = Engine::new();
        engine.request(&json!({"command":"enqueue","mode":"move","verify_md5":md5,"sources":[f.0.join("source")],"destination":f.0.join("dest"),"exclusions":{"directories":["__*"],"files":["*.tmp"]}}).to_string()).unwrap();
        let done = wait(&engine, |s| s["jobs"][0]["state"] == "done");
        assert_eq!(
            fs::read(f.0.join("source/__pycache__/cache")).unwrap(),
            b"keep"
        );
        assert_eq!(fs::read(f.0.join("source/file.tmp")).unwrap(), b"temp");
        assert!(!f.0.join("source/sub/été\n.txt").exists());
        assert!(f.0.join("dest/source/sub/été\n.txt").exists());
        assert_ne!(
            done["jobs"][0]["entries"][0]["undo"]["directory_move"],
            true
        );
        engine
            .request(r#"{"command":"undo_many","ids":[1]}"#)
            .unwrap();
        let undone = wait(&engine, |s| s["jobs"][0]["state"] == "undone");
        assert_eq!(undone["jobs"][0]["state"], "undone");
        assert_eq!(
            fs::read(f.0.join("source/sub/été\n.txt")).unwrap(),
            b"bonjour\0monde"
        );
        assert_eq!(
            fs::read(f.0.join("source/__pycache__/cache")).unwrap(),
            b"keep"
        );
        assert!(!f.0.join("dest/source").exists());
    }
}

#[test]
/// Une analyse échouée et relancée conserve ses exclusions après changement des réglages globaux.
fn exclusions_retry_preparation_uses_original_rules() {
    let f = Fixture::new();
    fs::write(f.0.join("source/ignored.tmp"), b"keep").unwrap();
    let destination = f.0.join("missing");
    let engine = Engine::new();
    engine.request(&json!({"command":"enqueue","sources":[f.0.join("source")],"destination":destination,"exclusions":{"files":["*.tmp"]}}).to_string()).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "error");
    engine
        .request(r#"{"command":"configure","exclusions":{}}"#)
        .unwrap();
    fs::create_dir(&destination).unwrap();
    engine
        .request(r#"{"command":"retry_errors","jobs":[1],"entries":[],"exact":true}"#)
        .unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert!(destination.join("source/sub/été\n.txt").exists());
    assert!(!destination.join("source/ignored.tmp").exists());
}

#[test]
/// Les copies répétées réservent des noms distincts dès la pause, gardent l’extension et se restaurent séparément.
fn duplication_reserves_names_and_undo_preserves_original() {
    let f = Fixture::new();
    let source = f.0.join("été.txt");
    fs::write(&source, b"original").unwrap();
    fs::write(f.0.join("été_copied1.txt"), b"existing").unwrap();
    let engine = Engine::new();
    engine
        .request(r#"{"command":"pause","paused":true}"#)
        .unwrap();
    engine
        .request(r#"{"command":"configure","allow_duplication":true}"#)
        .unwrap();
    for _ in 0..2 {
        engine
            .request(&json!({"command":"enqueue","sources":[source],"destination":f.0}).to_string())
            .unwrap();
    }
    let state = wait(&engine, |s| {
        s["jobs"].as_array().unwrap().len() == 2
            && s["jobs"]
                .as_array()
                .unwrap()
                .iter()
                .all(|j| j["state"] == "waiting")
    });
    let mut targets: Vec<_> = state["jobs"]
        .as_array()
        .unwrap()
        .iter()
        .map(|j| j["entries"][0]["destination"].as_str().unwrap().to_owned())
        .collect();
    targets.sort();
    assert_eq!(
        targets,
        vec![
            f.0.join("été_copied2.txt").to_str().unwrap(),
            f.0.join("été_copied3.txt").to_str().unwrap()
        ]
    );
    engine
        .request(r#"{"command":"configure","allow_duplication":false}"#)
        .unwrap();
    let stored: State = serde_json::from_value(
        serde_json::to_value(&*engine.shared.state.lock().unwrap()).unwrap(),
    )
    .unwrap();
    assert!(!stored.allow_duplication);
    assert!(stored
        .jobs
        .iter()
        .all(|j| j.allow_duplication && !j.duplicate_targets.is_empty()));
    engine
        .request(r#"{"command":"pause","paused":false}"#)
        .unwrap();
    wait(&engine, |s| {
        s["jobs"]
            .as_array()
            .unwrap()
            .iter()
            .all(|j| j["state"] == "done")
    });
    for path in &targets {
        assert_eq!(fs::read(path).unwrap(), b"original");
    }
    engine.request(r#"{"command":"undo_all"}"#).unwrap();
    wait(&engine, |s| {
        s["jobs"]
            .as_array()
            .unwrap()
            .iter()
            .all(|j| j["state"] == "undone")
    });
    assert_eq!(fs::read(source).unwrap(), b"original");
    assert_eq!(fs::read(f.0.join("été_copied1.txt")).unwrap(), b"existing");
    for path in &targets {
        assert!(!Path::new(path).exists());
    }
}

#[test]
/// Un dossier est dupliqué comme arbre frère ; les exclusions et les liens pendants restent respectés.
fn duplication_folder_preserves_exclusions_and_symlinks() {
    let f = Fixture::new();
    let engine = Engine::new();
    fs::write(f.0.join("source/temporary.tmp"), b"excluded").unwrap();
    symlink("missing", f.0.join("source/link")).unwrap();
    engine.request(&json!({"command":"enqueue","sources":[f.0.join("source")],"destination":f.0,"allow_duplication":true,"exclusions":{"files":["*.tmp"]}}).to_string()).unwrap();
    wait(&engine, |s| s["jobs"][0]["state"] == "done");
    assert_eq!(
        fs::read(f.0.join("source_copied1/sub/été\n.txt")).unwrap(),
        b"bonjour\0monde"
    );
    assert!(!f.0.join("source_copied1/temporary.tmp").exists());
    assert_eq!(
        fs::read_link(f.0.join("source_copied1/link")).unwrap(),
        PathBuf::from("missing")
    );
    engine.request(&json!({"command":"enqueue","sources":[f.0.join("source/link")],"destination":f.0.join("source"),"allow_duplication":true}).to_string()).unwrap();
    wait(&engine, |s| s["jobs"][1]["state"] == "done");
    assert_eq!(
        fs::read_link(f.0.join("source/link_copied1")).unwrap(),
        PathBuf::from("missing")
    );
}

#[test]
/// L’option ne transforme jamais un déplacement sur place ni une copie récursive en duplication.
fn duplication_retains_recursive_and_move_guards() {
    let f = Fixture::new();
    let engine = Engine::new();
    for (index, (mode, destination)) in [("move", f.0.clone()), ("copy", f.0.join("source/sub"))]
        .into_iter()
        .enumerate()
    {
        engine.request(&json!({"command":"enqueue","mode":mode,"sources":[f.0.join("source")],"destination":destination,"allow_duplication":true}).to_string()).unwrap();
        wait(&engine, |s| s["jobs"][index]["state"] == "error");
    }
    assert!(!f.0.join("source_copied1").exists());
    assert_eq!(
        fs::read(f.0.join("source/sub/été\n.txt")).unwrap(),
        b"bonjour\0monde"
    );
}

// Régressions des contrôles de stabilité source et des préconditions de restauration.
mod integrity;
