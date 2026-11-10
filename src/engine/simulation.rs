//! Prévisualisation en lecture seule : aucun lot, journal, temporaire ni nom réservé.
use super::*;
use std::collections::{BTreeMap, HashSet};
use std::sync::atomic::{AtomicBool, Ordering};

/// Les jetons sont créés avant le travailleur pour rendre fiable une fermeture immédiate.
#[derive(Default)]
pub(super) struct Registry {
    next: u64,
    pub(super) tasks: BTreeMap<u64, Arc<AtomicBool>>,
}

struct Plan {
    mode: String,
    policy: String,
    rules: exclusions::Rules,
    md5: bool,
    duplication: bool,
    rows: Vec<Value>,
    reserved: HashSet<PathBuf>,
    queued: HashSet<(PathBuf, PathBuf, String)>,
    cancelled: Arc<AtomicBool>,
}
/// Paramètres nommés pour éviter de confondre les chemins et les motifs d’une prévisualisation.
struct PreviewRow<'a> {
    source: &'a Path,
    destination: &'a Path,
    parent: Option<usize>,
    kind: &'a str,
    size: u64,
    action: &'a str,
    reason: &'a str,
    detail: String,
}
impl Plan {
    fn checkpoint(&self) -> Result<(), String> {
        if self.cancelled.load(Ordering::Relaxed) {
            Err("Simulation canceled".into())
        } else {
            Ok(())
        }
    }
    /// Une ligne exclue représente aussi son sous-arbre, qui n’est jamais parcouru.
    fn row(&mut self, row: PreviewRow<'_>) -> usize {
        let PreviewRow {
            source,
            destination,
            parent,
            kind,
            size,
            action,
            reason,
            detail,
        } = row;
        let index = self.rows.len();
        self.rows.push(json!({"source":source,"destination":destination,"parent":parent,"kind":kind,"size":size,"action":action,"reason":reason,"detail":detail,"fast_move":false}));
        index
    }
    fn scan(
        &mut self,
        source: &Path,
        target: &Path,
        parent: Option<usize>,
        missing_parent: bool,
        inherited_conflict: bool,
    ) -> Result<(), String> {
        self.checkpoint()?;
        // Une limite explicite évite de saturer l’interface ; aucun plan tronqué ne peut être lancé.
        if self.rows.len() >= 200_000 {
            return Err("Simulation contains too many items (maximum 200000)".into());
        }
        let meta = match fs::symlink_metadata(source) {
            Ok(m) => m,
            Err(e) => {
                self.row(PreviewRow {
                    source,
                    destination: target,
                    parent,
                    kind: "file",
                    size: 0,
                    action: "error",
                    reason: "io",
                    detail: e.to_string(),
                });
                return Ok(());
            }
        };
        let kind = source_kind(&meta).unwrap_or("special");
        let name = source
            .file_name()
            .and_then(|n| n.to_str())
            .ok_or("Invalid copy source.")?;
        let size = if kind == "file" { meta.len() } else { 0 };
        if self.rules.excludes(name, kind == "directory") {
            self.row(PreviewRow {
                source,
                destination: target,
                parent,
                kind,
                size,
                action: "excluded",
                reason: if !self.rules.include_hidden && name.starts_with('.') {
                    "hidden"
                } else {
                    "pattern"
                },
                detail: (if kind == "directory" {
                    &self.rules.directories
                } else {
                    &self.rules.files
                })
                .iter()
                .find(|pattern| exclusions::matches(pattern, name))
                .cloned()
                .unwrap_or_default(),
            });
            return Ok(());
        }
        if kind == "special" {
            self.row(PreviewRow {
                source,
                destination: target,
                parent,
                kind,
                size: 0,
                action: "error",
                reason: "special",
                detail: String::new(),
            });
            return Ok(());
        }
        let queued = self
            .queued
            .contains(&(source.into(), target.into(), self.mode.clone()));
        if queued && kind != "directory" {
            self.row(PreviewRow {
                source,
                destination: target,
                parent,
                kind,
                size,
                action: "excluded",
                reason: "queued",
                detail: String::new(),
            });
            return Ok(());
        }
        let mut destination = target.to_path_buf();
        let existing = if missing_parent {
            None
        } else {
            match fs::symlink_metadata(target) {
                Ok(m) => Some(m),
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => None,
                Err(e) => {
                    self.row(PreviewRow {
                        source,
                        destination: target,
                        parent,
                        kind,
                        size,
                        action: "error",
                        reason: "io",
                        detail: e.to_string(),
                    });
                    return Ok(());
                }
            }
        };
        let merge = kind == "directory" && existing.as_ref().is_some_and(|m| m.is_dir());
        let collision =
            existing.is_some() && !merge || self.reserved.contains(target) && !merge && !queued;
        let mut action = self.mode.clone();
        let mut reason = if inherited_conflict {
            "ask"
        } else if merge {
            "merge"
        } else {
            "transfer"
        };
        let mut conflict = inherited_conflict;
        if collision {
            match self.policy.as_str() {
                "skip" => {
                    self.row(PreviewRow {
                        source,
                        destination: target,
                        parent,
                        kind,
                        size,
                        action: "excluded",
                        reason: "policy_skip",
                        detail: String::new(),
                    });
                    return Ok(());
                }
                "rename" => {
                    destination = available_name(target, |p| self.reserved.contains(p))?;
                    reason = "rename";
                }
                "newer" if existing.is_some() => match conflicts::newer(source, target) {
                    Ok("skip") => {
                        self.row(PreviewRow {
                            source,
                            destination: target,
                            parent,
                            kind,
                            size,
                            action: "excluded",
                            reason: "not_newer",
                            detail: String::new(),
                        });
                        return Ok(());
                    }
                    Ok(_) => reason = "replace",
                    Err(e) => {
                        self.row(PreviewRow {
                            source,
                            destination: target,
                            parent,
                            kind,
                            size,
                            action: "error",
                            reason: "io",
                            detail: e,
                        });
                        return Ok(());
                    }
                },
                _ => {
                    conflict = true;
                    reason = "ask";
                }
            }
        }
        if conflict {
            action = "conflict".into();
        }
        let index = self.row(PreviewRow {
            source,
            destination: &destination,
            parent,
            kind,
            size,
            action: &action,
            reason,
            detail: String::new(),
        });
        self.reserved.insert(destination.clone());
        if kind == "directory" {
            let mut children =
                match fs::read_dir(source).and_then(|it| it.collect::<Result<Vec<_>, _>>()) {
                    Ok(v) => v,
                    Err(e) => {
                        self.rows[index]["action"] = json!("error");
                        self.rows[index]["reason"] = json!("io");
                        self.rows[index]["detail"] = json!(e.to_string());
                        return Ok(());
                    }
                };
            sort_children(&mut children);
            for child in children {
                self.scan(
                    &child.path(),
                    &destination.join(child.file_name()),
                    Some(index),
                    missing_parent || existing.is_none() || collision && !merge,
                    conflict,
                )?;
            }
        }
        if queued
            && kind == "directory"
            && self.rows[index + 1..]
                .iter()
                .all(|row| row["action"] == "excluded")
        {
            self.rows[index]["action"] = json!("excluded");
            self.rows[index]["reason"] = json!("queued");
        }
        Ok(())
    }
}

impl Engine {
    /// Point d’entrée volontairement indépendant du verrou et des révisions de la file.
    pub(super) fn simulation_request(&self, v: &Value) -> Option<Result<Value, String>> {
        match v["command"].as_str()? {
            "simulation_begin" => Some((|| {
                if self.shared.stop.load(Ordering::Relaxed) {
                    return Err("Engine stopped".into());
                }
                let mut registry = self.shared.simulations.lock().unwrap();
                if self.shared.stop.load(Ordering::Relaxed) {
                    return Err("Engine stopped".into());
                }
                if registry.tasks.len() >= 4 {
                    return Err("Too many simulations are running".into());
                }
                registry.next = registry
                    .next
                    .checked_add(1)
                    .ok_or("Simulation identifier exhausted")?;
                let id = registry.next;
                registry.tasks.insert(id, Arc::new(AtomicBool::new(false)));
                Ok(json!({"id":id}))
            })()),
            "simulation_cancel" => Some((|| {
                let id = v["id"].as_u64().ok_or("Invalid simulation identifier")?;
                if let Some(flag) = self.shared.simulations.lock().unwrap().tasks.remove(&id) {
                    flag.store(true, Ordering::Relaxed);
                }
                Ok(json!({"ok":true}))
            })()),
            "simulate" => Some((|| {
                let id = v["id"].as_u64().ok_or("Invalid simulation identifier")?;
                let flag = self
                    .shared
                    .simulations
                    .lock()
                    .unwrap()
                    .tasks
                    .get(&id)
                    .cloned()
                    .ok_or("Simulation canceled")?;
                let result = self.simulate(v, flag);
                self.shared.simulations.lock().unwrap().tasks.remove(&id);
                result
            })()),
            _ => None,
        }
    }
    fn simulate(&self, v: &Value, cancelled: Arc<AtomicBool>) -> Result<Value, String> {
        let mode = v["mode"].as_str().unwrap_or("copy");
        if !["copy", "move"].contains(&mode) {
            return Err("Invalid transfer mode".into());
        }
        let destination = v["destination"]
            .as_str()
            .filter(|s| !s.is_empty())
            .ok_or("Missing destination")?;
        let destination = fs::canonicalize(destination).map_err(|e| e.to_string())?;
        if !destination.is_dir() {
            return Err("The destination must be an existing folder".into());
        }
        let inputs = v["sources"]
            .as_array()
            .filter(|a| !a.is_empty())
            .ok_or("No files to copy")?;
        let mut sources = Vec::new();
        for input in inputs {
            sources.push(
                std::path::absolute(
                    input
                        .as_str()
                        .filter(|s| !s.is_empty())
                        .ok_or("Invalid copy source.")?,
                )
                .map_err(|e| e.to_string())?,
            );
        }
        let (rules, policy, md5, duplication, jobs) = {
            let state = self.shared.state.lock().unwrap();
            (
                v.get("exclusions")
                    .map(exclusions::Rules::parse)
                    .transpose()?
                    .unwrap_or_else(|| state.exclusions.clone()),
                v.get("conflict_policy")
                    .map(conflicts::parse)
                    .transpose()?
                    .unwrap_or_else(|| state.conflict_policy.clone()),
                v["verify_md5"].as_bool().unwrap_or(state.verify_md5),
                v["allow_duplication"]
                    .as_bool()
                    .unwrap_or(state.allow_duplication),
                state.jobs.iter().map(|j| j.snapshot()).collect::<Vec<_>>(),
            )
        };
        let mut queued = HashSet::new();
        let mut reserved = HashSet::new();
        for job in &jobs {
            if terminal(&job.state) || job.cancel {
                continue;
            }
            for entry in &job.entries {
                if terminal(&entry.state)
                    || entry.state == TransferState::Skipped
                    || entry.cancel
                    || entry.hidden
                {
                    continue;
                }
                queued.insert((
                    entry.source.clone(),
                    entry.destination.clone(),
                    job.mode.clone(),
                ));
                reserved.insert(entry.destination.clone());
            }
        }
        let mut plan = Plan {
            mode: mode.into(),
            policy,
            rules,
            md5,
            duplication,
            rows: Vec::new(),
            reserved,
            queued,
            cancelled,
        };
        let mut roots = HashSet::new();
        for source in sources {
            plan.checkpoint()?;
            if !roots.insert(source.clone()) {
                continue;
            }
            let name = source
                .file_name()
                .ok_or("Cannot copy the filesystem root")?;
            let mut target = destination.join(name);
            let meta = fs::symlink_metadata(&source);
            let same = duplication::same_target(&source, &target);
            if same && plan.duplication && mode == "copy" {
                let directory = meta.as_ref().is_ok_and(|m| m.is_dir());
                let stem = if directory {
                    name.to_string_lossy().into_owned()
                } else {
                    source
                        .file_stem()
                        .unwrap_or(name)
                        .to_string_lossy()
                        .into_owned()
                };
                let extension = if directory {
                    String::new()
                } else {
                    source
                        .extension()
                        .map(|s| format!(".{}", s.to_string_lossy()))
                        .unwrap_or_default()
                };
                let mut found = None;
                for number in 1..=100_000 {
                    plan.checkpoint()?;
                    let path = target.with_file_name(format!("{stem}_copied{number}{extension}"));
                    match fs::symlink_metadata(&path) {
                        Ok(_) => continue,
                        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
                            if !plan.reserved.contains(&path) {
                                found = Some(path);
                                break;
                            }
                        }
                        Err(e) => return Err(e.to_string()),
                    }
                }
                target = found.ok_or("No available duplication name")?;
            } else if fast_move::invalid_alias(
                &source,
                &target,
                meta.as_ref().is_ok_and(|m| m.is_dir()),
            ) {
                plan.row(PreviewRow {
                    source: &source,
                    destination: &target,
                    parent: None,
                    kind: "file",
                    size: 0,
                    action: "error",
                    reason: "same_target",
                    detail: String::new(),
                });
                continue;
            }
            plan.scan(&source, &target, None, false, false)?;
        }
        plan.checkpoint()?;
        // Même preuve que la préparation réelle : liens physiques, collisions et
        // montages ne doivent jamais être annoncés comme un renommage sans preuve.
        let mut fast = false;
        if mode == "move"
            && !plan.md5
            && !plan.rows.iter().any(|r| {
                matches!(
                    r["reason"].as_str(),
                    Some("merge" | "rename" | "replace" | "ask")
                )
            })
        {
            let entries = plan
                .rows
                .iter()
                .filter(|r| !matches!(r["action"].as_str(), Some("excluded" | "error")))
                .map(|row| {
                    let mut entry = row.clone();
                    entry["copied"] = json!(0);
                    entry["verified"] = json!(0);
                    entry["state"] = json!("waiting");
                    entry["error"] = json!("");
                    serde_json::from_value::<Entry>(entry).map_err(|e| e.to_string())
                })
                .collect::<Result<Vec<_>, _>>()?;
            fast = fast_move::reservations(&entries).is_some()
                && fast_move::eligible(&entries, true, false);
        }
        for row in &mut plan.rows {
            row["fast_move"] = json!(
                fast && row["action"] == "move"
                    && (row["kind"] != "directory"
                        || !plan.rules.active()
                            && fast_move::tree_possible(
                                Path::new(row["source"].as_str().unwrap()),
                                Path::new(row["destination"].as_str().unwrap())
                            ))
            );
        }
        plan.checkpoint()?;
        let space = self.simulation_space(&plan.rows, &jobs, &plan.cancelled)?;
        Ok(
            json!({"ok":true,"rows":plan.rows,"volumes":space.0,"space_complete":space.1,"queue_unscanned":jobs.iter().any(|j|j.state==TransferState::Scanning)}),
        )
    }
}
