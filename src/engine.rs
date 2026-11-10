//! Moteur de copie et de déplacement : protocole JSON, analyse des sources,
//! travailleurs, points de pause/annulation et publication des fichiers.
//! Les modules voisins isolent le journal, la restauration et l’ordonnancement.
//! Les indices des entrées restent stables pour relier interface, transfert et journal.

use serde::{Deserialize, Serialize};
mod completion;
mod conflicts;
mod controls;
mod diagnostics;
mod directories;
mod duplicates;
mod duplication;
mod errors;
mod exclusions;
mod fast_move;
mod fast_verify;
mod prepared;
mod properties;
mod rate;
mod reconnect;
mod resume;
mod simulation;
mod staging;
mod tree_move;
use fast_move::EntryOutcome;
mod protocol;
use protocol::{Command, TransferState};
mod history;
mod journal;
mod queue;
mod row_queue;
mod scheduling;
mod selection;
mod snapshot;
mod space;
mod undo;
mod validation;
use serde_json::{json, Value};
use std::os::unix::fs::{symlink, MetadataExt, OpenOptionsExt};
use std::{
    fs::{self, OpenOptions},
    io::{Read, Write},
    path::{Path, PathBuf},
    sync::{Arc, Condvar, Mutex},
    thread,
};

#[derive(Clone, Serialize, Deserialize)]
/// Une ligne de fichier, lien ou dossier, identifiée par sa position stable dans un lot.
struct Entry {
    #[serde(default)]
    /// Résultat de la vérification facultative d’un renommage rapide.
    fast_move_check: Option<fast_verify::Report>,
    #[serde(default)]
    /// Pause propre à cette ligne, distincte de la pause globale et de celle du lot.
    paused: bool,
    #[serde(default)]
    /// Retrait visuel : conserve l’entrée et son journal tant que ses indices sont utilisés.
    hidden: bool,
    #[serde(default)]
    /// Demande d’arrêt coopérative propre à cette entrée.
    cancel: bool,
    #[serde(default)]
    /// Couple périphérique/inode de la source observé pendant l’analyse.
    identity: (u64, u64),
    /// Chemin de la source ; un lien est traité comme un élément à copier, pas à suivre.
    source: PathBuf,
    /// Chemin cible effectif, éventuellement modifié par un choix de renommage.
    destination: PathBuf,
    /// Type de ligne : file, link ou directory.
    kind: String,
    /// Nombre d’octets du fichier ordinaire ; nul pour les liens et dossiers.
    size: u64,
    /// Octets écrits dans la copie temporaire, avant publication éventuelle.
    copied: u64,
    /// Octets relus pour MD5, source et destination cumulées.
    verified: u64,
    /// Empreinte de la source lorsque la vérification MD5 a été effectuée.
    md5_source: Option<String>,
    /// Empreinte de la copie comparée à celle de la source.
    md5_destination: Option<String>,
    #[serde(default)]
    /// L’entrée était active lors d’un arrêt brutal et peut être contrôlée pour reprise.
    interrupted: bool,
    #[serde(default)]
    /// Contrôle du préfixe avant de réouvrir le temporaire en écriture.
    resume_checking: bool,
    #[serde(default)]
    resume_checked: u64,
    #[serde(default)]
    /// Position réellement réutilisée lors de la dernière reprise.
    resumed_from: u64,
    #[serde(default)]
    /// Comparaison des propriétés de la source et du résultat, sans valeurs privées d’attributs.
    properties: Option<properties::Report>,
    /// État métier sérialisé et interprété aussi par l’interface.
    state: TransferState,
    /// Dernière explication d’échec ou d’annulation de la ligne.
    error: String,
    #[serde(default)]
    /// Étapes persistées nécessaires à la remise en état de cette entrée.
    undo: Option<undo::Record>,
}
#[derive(Clone, Serialize, Deserialize)]
/// Un lot soumis par une action de copie ou de déplacement, avec ses options figées.
struct Job {
    /// Index strictement temporaire ; les clones et les journaux ne le conservent pas.
    #[serde(skip)]
    directory_index: directories::Cache,
    /// Les ajouts manuels sont analysés mais exigent une validation explicite.
    #[serde(default)]
    start_required: bool,
    #[serde(default)]
    /// Date d’ajout UTC en secondes Unix ; zéro désigne un ancien journal sans date connue.
    created_at: u64,
    #[serde(default)]
    /// Règles immuables du lot, réutilisées aux reprises et réactivations.
    exclusions: exclusions::Rules,
    #[serde(default)]
    allow_duplication: bool,
    #[serde(default)]
    duplicate_targets: std::collections::BTreeMap<PathBuf, PathBuf>,
    #[serde(default)]
    /// Suspend ce lot indépendamment des autres lots.
    paused: bool,
    #[serde(default)]
    /// Ordre réel du dernier lancement, pour restaurer les remplacements en sens inverse.
    execution_order: u64,
    #[serde(default)]
    /// Plafond propre à cette demande, en octets/s ; zéro signifie illimité.
    rate_limit: u64,
    #[serde(default)]
    /// Indices annulés à réactiver après la phase de restauration préparatoire.
    retry_entries: Option<Vec<usize>>,
    #[serde(default)]
    /// Une réactivation complète doit aussi compléter une analyse interrompue.
    retry_all: bool,
    #[serde(skip)]
    /// Disques calculés à l’analyse ; None sérialise les copies classiques, hors exception de renommage.
    disks: Option<std::collections::BTreeSet<String>>,
    #[serde(default)]
    /// Renommages locaux sans MD5 ; aucune copie de secours ne démarre sous cette réservation.
    fast_move: bool,
    #[serde(skip)]
    /// Racines et destination normalisées, calculées hors du verrou de l’ordonnanceur.
    paths: Option<Vec<PathBuf>>,
    #[serde(skip)]
    /// Identités de destination normalisées par montage, dans l’ordre stable des entrées.
    destination_keys: Option<Vec<PathBuf>>,
    #[serde(skip)]
    /// Demandes actives au moment de l’ajout, même si elles finissent pendant l’analyse.
    dedup_candidates: Vec<u64>,
    #[serde(default)]
    /// Montages attendus : empêche de copier dans un point de montage devenu vide.
    connections: Vec<reconnect::Anchor>,
    #[serde(default)]
    /// None restaure tout le lot ; Some limite la restauration aux indices donnés.
    undo_entries: Option<Vec<usize>>,
    #[serde(default)]
    /// Les anciens historiques non journalisés sont consultables mais non restaurables.
    journaled: bool,
    #[serde(default)]
    /// Demande de restauration traitée en priorité par l’ordonnanceur.
    undo_requested: bool,
    #[serde(default)]
    /// Conserve une opération interrompue même si l’historique normal est désactivé.
    recovery: bool,
    #[serde(default)]
    /// Autorisation de créer des points de reprise, capturée à l’ajout du lot.
    resume_enabled: bool,
    #[serde(default)]
    /// Distingue une interruption récupérable d’une annulation volontaire.
    resume_pending: bool,
    #[serde(default)]
    /// Vérification facultative des propriétaires, permissions, ACL et attributs.
    verify_properties: bool,
    #[serde(default)]
    verify_fast_move: bool,
    #[serde(default)]
    /// Contrôle facultatif de la place requise par la file avant écriture.
    verify_space: bool,
    #[serde(default)]
    /// Capacité inconnue : avertissement persistant, jamais un motif d’arrêt.
    space_warning: String,
    /// Option MD5 capturée à la création, indépendante des changements futurs du réglage.
    verify_md5: bool,
    /// copy conserve les sources ; move les retire après validation du résultat.
    mode: String,
    /// Identifiant monotone utilisé par le protocole, l’interface et les temporaires.
    id: u64,
    /// Racines initialement sélectionnées, utilisées aussi pour compléter une réactivation.
    sources: Vec<PathBuf>,
    #[serde(default)]
    /// Noms d’origine conservés pour afficher les URL distantes à la place du montage interne.
    source_urls: Vec<String>,
    #[serde(default)]
    /// Adresse distante sans mot de passe, utilisée pour présenter le chemin de montage.
    destination_url: Option<String>,
    /// Dossier de destination du lot, validé lors de sa soumission.
    destination: PathBuf,
    /// État global du lot ; les états des fichiers restent détaillés dans entries.
    state: TransferState,
    /// Liste stable issue de l’analyse, normalement parents avant descendants.
    entries: Vec<Entry>,
    /// Erreur globale, notamment lors de l’analyse, du journal ou de la restauration.
    error: String,
    /// Demande d’arrêt du lot, vérifiée avant toute attente de pause.
    cancel: bool,
    /// Choix de conflit appliqué aux suivants : ask, skip, replace ou rename.
    policy: String,
    #[serde(skip)]
    /// Choix ponctuel d’un conflit ; il n’est jamais réutilisé après redémarrage.
    resolution: Option<String>,
}
#[derive(Default, Serialize, Deserialize)]
#[serde(default)]
/// État sérialisable du moteur ; les réservations de travailleurs ne sont pas persistées.
struct State {
    #[serde(skip)]
    completion_claimed: bool,
    #[serde(skip)]
    work_epoch: u64,
    #[serde(skip)]
    last_work_command: String,
    #[serde(skip)]
    preparing: u64,
    #[serde(skip)]
    finalizing: u64,
    /// Politique par défaut, capturée dans chaque nouveau lot.
    #[serde(default = "conflicts::ask")]
    conflict_policy: String,
    /// Règles par défaut des futurs lots ; inclure les cachés préserve le comportement initial.
    exclusions: exclusions::Rules,
    /// Autorise uniquement les copies sur place, jamais les déplacements sur eux-mêmes.
    allow_duplication: bool,
    #[serde(skip)]
    /// Réservations en mémoire des lots exécutés ; absent du journal après arrêt.
    active: std::collections::HashMap<u64, Option<std::collections::BTreeSet<String>>>,
    /// Historique et file de travail dans leur ordre d’arrivée.
    jobs: Vec<snapshot::TrackedJob>,
    /// Pause globale de la file, en plus des pauses locales.
    paused: bool,
    #[serde(skip)]
    /// Distingue une pause récente d’une pause que le collage en arrière-plan peut lever.
    pause_epoch: u64,
    /// Valeur par défaut pour les futures opérations.
    verify_md5: bool,
    verify_properties: bool,
    #[serde(default)]
    verify_fast_move: bool,
    #[serde(default)]
    /// Contrôle facultatif de la place requise par la file avant écriture.
    verify_space: bool,
    /// Reprend automatiquement les nouveaux lots interrompus au prochain lancement.
    resume_interrupted: bool,
    /// Dernier identifiant attribué ; évite de recycler ceux de l’historique.
    next_id: u64,
    /// Nombre d’éléments ignorés car leur opération était déjà prévue.
    duplicates_ignored: u64,
    /// Plafond total des copies, modifiable à chaud ; zéro signifie illimité.
    rate_limit: u64,
    /// Priorités persistées, indépendantes de l’ordre chronologique du journal.
    queue_order: Vec<u64>,
    /// Préordre visuel personnalisé, sans réindexer les entrées du journal.
    row_order: Vec<row_queue::Key>,
    #[serde(skip)]
    row_layout: Option<Arc<row_queue::Layout>>,
    #[serde(skip)]
    row_revision: u64,
    /// Compteur monotone de lancement pour les restaurations après réordonnancement.
    next_execution_order: u64,
    /// Conserve les lots terminés et les informations permettant leur restauration.
    keep_history: bool,
}
/// Ressources partagées par l’interface, l’analyse et les deux travailleurs.
/// Ordre de sauvegarde : writer puis state ; ne jamais conserver state en appelant save.
struct Shared {
    /// Sérialise les commandes avec la prise de garde de fin de file, jamais les E/S des travailleurs.
    requests: Mutex<()>,
    /// Analyses sans écriture, annulables indépendamment de la file de transfert.
    simulations: Mutex<simulation::Registry>,
    /// Protège les mutations d’état ; les longues copies travaillent hors de ce verrou.
    state: Mutex<State>,
    /// Budgets volatils, toujours verrouillés après state et jamais persistés.
    limiter: Mutex<rate::Limiter>,
    /// Réveille les travailleurs après changement de pause, conflit, réservation ou annulation.
    wake: Condvar,
    /// Sérialise les remplacements du journal temporaire commun.
    writer: Mutex<journal::Writer>,
    /// Exclut les restaurations et nettoyages de sauvegardes concurrents.
    maintenance: Mutex<()>,
    /// Journal persistant ; None pour les moteurs de test sans sauvegarde.
    history_path: Option<PathBuf>,
    /// Signal d’arrêt définitif contrôlé par les travailleurs.
    stop: std::sync::atomic::AtomicBool,
    /// Poignées jointes à la fermeture pour attendre la fin des deux travailleurs.
    worker: Mutex<Vec<thread::JoinHandle<()>>>,
}
#[derive(Clone)]
/// Poignée clonable du même moteur ; cloner cette valeur ne démarre pas de travailleur.
pub struct Engine {
    /// Même état et mêmes verrous pour toutes les poignées et tâches du moteur.
    shared: Arc<Shared>,
}

impl Engine {
    #[cfg(test)]
    /// Crée un moteur de test sans journal sur disque.
    pub fn new() -> Self {
        Self::start(State::default(), None)
    }
    /// Partage l’état puis démarre deux travailleurs ; l’ordonnanceur réserve les lots
    /// sous le verrou d’état avant toute opération de fichiers.
    fn start(mut state: State, history_path: Option<PathBuf>) -> Self {
        if conflicts::parse(&json!(state.conflict_policy)).is_err() {
            state.conflict_policy = conflicts::ask();
        }
        state.next_execution_order = state.next_execution_order.max(state.next_id).max(
            state
                .jobs
                .iter()
                .map(|j| j.execution_order.max(j.id))
                .max()
                .unwrap_or(0),
        );
        let engine = Self {
            shared: Arc::new(Shared {
                requests: Mutex::new(()),
                simulations: Mutex::new(simulation::Registry::default()),
                state: Mutex::new(state),
                limiter: Mutex::new(rate::Limiter::default()),
                wake: Condvar::new(),
                writer: Mutex::new(journal::Writer::default()),
                maintenance: Mutex::new(()),
                history_path,
                stop: std::sync::atomic::AtomicBool::new(false),
                worker: Mutex::new(Vec::new()),
            }),
        };
        // Deux travailleurs suffisent ; leur simple existence n’autorise pas deux copies sur un même disque.
        for _ in 0..2 {
            let worker = engine.clone();
            let handle = thread::spawn(move || worker.work());
            engine.shared.worker.lock().unwrap().push(handle);
        }
        engine
    }
    /// Valide et distribue une commande JSON. Les mutations d’état sont protégées
    /// par le mutex ; les sauvegardes et attentes se font après libération de ce verrou.
    pub fn request(&self, request: &str) -> Result<Value, String> {
        let v: Value = serde_json::from_str(request).map_err(|e| e.to_string())?;
        // Les analyses parcourent le disque hors du verrou des commandes : pause,
        // annulation et rafraîchissement restent disponibles pendant la simulation.
        if let Some(result) = self.simulation_request(&v) {
            return result;
        }
        let _request_guard = self.shared.requests.lock().unwrap();
        if self.shared.stop.load(std::sync::atomic::Ordering::SeqCst)
            && v["command"] != Command::Status.as_str()
            && v["command"] != Command::Shutdown.as_str()
        {
            return Err("Engine stopped".into());
        }
        let command =
            Command::from_wire(v["command"].as_str().unwrap_or("")).ok_or("Commande inconnue")?;
        {
            let mut state = self.shared.state.lock().unwrap();
            if state.completion_claimed
                && !matches!(
                    command,
                    Command::Status
                        | Command::Poll
                        | Command::ErrorReport
                        | Command::CompletionRelease
                        | Command::Shutdown
                )
            {
                return Err(
                    "A completion action is being dispatched; no operation was added".into(),
                );
            }
            if !matches!(
                command,
                Command::Status
                    | Command::Poll
                    | Command::ErrorReport
                    | Command::Configure
                    | Command::SetRateLimit
                    | Command::CompletionClaim
                    | Command::CompletionRelease
                    | Command::Shutdown
            ) {
                state.work_epoch = state.work_epoch.saturating_add(1);
                state.last_work_command = command.as_str().into();
            }
        }
        let _mutation = snapshot::Mutation(!matches!(
            command,
            Command::Status | Command::Poll | Command::ErrorReport
        ));
        match command {
            Command::CompletionClaim => self.claim_completion(&v),
            Command::CompletionRelease => {
                self.shared.state.lock().unwrap().completion_claimed = false;
                Ok(json!({"ok":true}))
            }
            Command::Poll => self.poll(v["since"].as_u64()),
            Command::ErrorReport => self.error_report(),
            Command::RetryErrors => self.retry_errors(&v),
            // Expose un instantané complet et la version du protocole aux clients Qt/CLI.
            Command::Status => {
                let mut state = self.shared.state.lock().unwrap();
                let view = row_queue::layout(&mut state);
                let mut value = serde_json::to_value(&*state).map_err(|e| e.to_string())?;
                value["protocol_version"] = json!(protocol::VERSION);
                value["parallel_limit"] = json!(2);
                value["queue_guard"] = completion::snapshot(&state);
                value["diagnostics"] =
                    diagnostics::snapshot(&mut state, &self.shared.limiter.lock().unwrap());
                value["queue_order"] = json!(queue::ids(&state));
                value["row_order"] = json!(view.ordered);
                Ok(value)
            }
            // Applique les valeurs par défaut ; les options déjà capturées par les lots restent valides.
            Command::SetRateLimit => self.set_rate_limit(&v),
            Command::Configure => {
                if v.get("verify_fast_move")
                    .is_some_and(|value| !value.is_boolean())
                {
                    return Err("Invalid fast-move verification option".into());
                }
                let policy = v.get("conflict_policy").map(conflicts::parse).transpose()?;
                let limit = v.get("rate_limit").map(rate::value).transpose()?;
                let exclusions = v
                    .get("exclusions")
                    .map(exclusions::Rules::parse)
                    .transpose()?;
                if policy.is_none()
                    && limit.is_none()
                    && exclusions.is_none()
                    && v["verify_fast_move"].as_bool().is_none()
                    && v["verify_md5"].as_bool().is_none()
                    && v["keep_history"].as_bool().is_none()
                    && v["resume_interrupted"].as_bool().is_none()
                    && v["verify_properties"].as_bool().is_none()
                    && v["verify_space"].as_bool().is_none()
                    && v["allow_duplication"].as_bool().is_none()
                {
                    return Err("Option manquante".into());
                }
                {
                    let mut state = self.shared.state.lock().unwrap();
                    if let Some(policy) = policy {
                        state.conflict_policy = policy;
                    }
                    if let Some(rules) = exclusions {
                        state.exclusions = rules;
                    }
                    if let Some(limit) = limit {
                        state.rate_limit = limit;
                    }
                    if let Some(value) = v["allow_duplication"].as_bool() {
                        state.allow_duplication = value;
                    }
                    if let Some(value) = v["verify_space"].as_bool() {
                        state.verify_space = value;
                    }
                    if let Some(value) = v["verify_fast_move"].as_bool() {
                        state.verify_fast_move = value;
                    }
                    if let Some(value) = v["verify_properties"].as_bool() {
                        state.verify_properties = value;
                    }
                    if let Some(value) = v["verify_md5"].as_bool() {
                        state.verify_md5 = value;
                    }
                    if let Some(value) = v["resume_interrupted"].as_bool() {
                        state.resume_interrupted = value;
                    }
                    if let Some(value) = v["keep_history"].as_bool() {
                        state.keep_history = value;
                    }
                }
                self.shared.wake.notify_all();
                self.save()?;
                Ok(json!({"ok":true}))
            }
            // Valide le protocole puis réserve le lot avant ses contrôles de chemins en arrière-plan.
            Command::StartPrepared => self.start_prepared(&v),
            Command::Enqueue => {
                if v.get("prepare_only")
                    .is_some_and(|value| !value.is_boolean())
                {
                    return Err("Invalid preparation option".into());
                }
                if v.get("verify_fast_move")
                    .is_some_and(|value| !value.is_boolean())
                {
                    return Err("Invalid fast-move verification option".into());
                }
                let request_policy = v.get("conflict_policy").map(conflicts::parse).transpose()?;
                let request_rate = v
                    .get("rate_limit")
                    .map(rate::value)
                    .transpose()?
                    .unwrap_or(0);
                let exclusions = v
                    .get("exclusions")
                    .map(exclusions::Rules::parse)
                    .transpose()?;
                let mode = v["mode"].as_str().unwrap_or("copy").to_owned();
                if !["copy", "move"].contains(&mode.as_str()) {
                    return Err("Type de transfert invalide".into());
                }
                let destination = v["destination"]
                    .as_str()
                    .filter(|path| !path.is_empty())
                    .ok_or("Destination manquante ou vide")?;
                let sources: Vec<PathBuf> = v["sources"]
                    .as_array()
                    .ok_or("Sources manquantes")?
                    .iter()
                    .map(|s| {
                        s.as_str()
                            .filter(|path| !path.is_empty())
                            .map(PathBuf::from)
                            .ok_or("Source invalide ou vide")
                    })
                    .collect::<Result<_, _>>()?;
                if sources.is_empty() {
                    return Err("No files to copy".into());
                }
                let destination = std::path::absolute(destination).map_err(|e| e.to_string())?;
                let sources = sources
                    .into_iter()
                    .map(|p| {
                        if p.is_absolute() {
                            Ok(p)
                        } else {
                            std::env::current_dir().map(|d| d.join(p))
                        }
                    })
                    .collect::<Result<Vec<_>, _>>()
                    .map_err(|e| e.to_string())?;
                let source_urls = match v.get("source_urls") {
                    None => Vec::new(),
                    Some(value) => {
                        let values = value.as_array().ok_or("Origines des sources invalides")?;
                        if values.len() != sources.len() {
                            return Err("Inconsistent source origins".into());
                        }
                        values
                            .iter()
                            .map(|value| {
                                value
                                    .as_str()
                                    .map(str::to_owned)
                                    .ok_or_else(|| "Origine de source invalide".to_owned())
                            })
                            .collect::<Result<Vec<_>, _>>()?
                    }
                };
                let destination_url = match v.get("destination_url") {
                    None | Some(Value::Null) => None,
                    Some(value) => Some(
                        value
                            .as_str()
                            .filter(|value| !value.is_empty())
                            .ok_or("Origine de destination invalide")?
                            .to_owned(),
                    ),
                };
                let (id, pause_epoch) = {
                    let mut state = self.shared.state.lock().unwrap();
                    let dedup_candidates = state
                        .jobs
                        .iter()
                        .filter(|j| !terminal(&j.state) && !j.cancel && !j.undo_requested)
                        .map(|j| j.id)
                        .collect::<Vec<_>>();
                    state.next_id += 1;
                    let id = state.next_id;
                    let verify_md5 = v["verify_md5"].as_bool().unwrap_or(state.verify_md5);
                    let verify_fast_move = v["verify_fast_move"]
                        .as_bool()
                        .unwrap_or(state.verify_fast_move);
                    let verify_properties = v["verify_properties"]
                        .as_bool()
                        .unwrap_or(state.verify_properties);
                    let verify_space = v["verify_space"].as_bool().unwrap_or(state.verify_space);
                    let resume_enabled = state.resume_interrupted && destination_url.is_none();
                    let exclusions = exclusions.unwrap_or_else(|| state.exclusions.clone());
                    let policy = request_policy.unwrap_or_else(|| state.conflict_policy.clone());
                    let allow_duplication = v["allow_duplication"]
                        .as_bool()
                        .unwrap_or(state.allow_duplication);
                    // Une demande manuelle ne retient la file que lorsqu’aucun
                    // travail déjà autorisé n’attend, ne s’analyse ou ne s’exécute.
                    let start_required = v["prepare_only"].as_bool().unwrap_or(false)
                        && !state
                            .jobs
                            .iter()
                            .any(|j| !j.start_required && !terminal(&j.state) && !j.cancel);
                    state.jobs.push(
                        Job {
                            directory_index: Default::default(),
                            start_required,
                            created_at: std::time::SystemTime::now()
                                .duration_since(std::time::UNIX_EPOCH)
                                .unwrap_or_default()
                                .as_secs(),
                            exclusions,
                            allow_duplication,
                            duplicate_targets: Default::default(),
                            paused: false,
                            execution_order: 0,
                            rate_limit: request_rate,
                            retry_entries: None,
                            retry_all: false,
                            disks: None,
                            fast_move: false,
                            paths: None,
                            destination_keys: None,
                            dedup_candidates,
                            connections: Vec::new(),
                            undo_entries: None,
                            journaled: true,
                            undo_requested: false,
                            recovery: false,
                            resume_enabled,
                            resume_pending: false,
                            verify_md5,
                            verify_properties,
                            verify_fast_move,
                            verify_space,
                            space_warning: String::new(),
                            mode,
                            id,
                            sources: sources.clone(),
                            source_urls,
                            destination_url,
                            destination: destination.clone(),
                            state: TransferState::Scanning,
                            entries: vec![],
                            error: String::new(),
                            cancel: false,
                            policy,
                            resolution: None,
                        }
                        .into(),
                    );
                    (id, state.pause_epoch)
                };
                if let Err(error) = self.save() {
                    // Aucun analyseur n’a encore été lancé : terminer le lot pour éviter une attente sans fin.
                    self.update(id, |job| {
                        job.cancel = true;
                        job.resume_pending = false;
                        job.state = TransferState::Error;
                        job.error = error.clone();
                    });
                    return Err(error);
                }
                let engine = self.clone();
                self.spawn_preparation(
                    id,
                    v["background"].as_bool() == Some(true)
                        && !v["prepare_only"].as_bool().unwrap_or(false),
                    pause_epoch,
                    move || {
                        let destination = engine.validate_roots(id, &sources, &destination)?;
                        let mut entries = Vec::new();
                        for source in &sources {
                            engine.scan(
                                id,
                                source,
                                &destination.join(source.file_name().ok_or("Source invalide")?),
                                &mut entries,
                            )?;
                        }
                        let disks = scheduling::disks_for_entries(&entries);
                        Ok((destination, entries, disks))
                    },
                );
                Ok(json!({"id": id}))
            }
            // La pause ne termine aucun travailleur : le réveil leur fait relire les indicateurs.
            Command::Pause => {
                {
                    let mut state = self.shared.state.lock().unwrap();
                    state.paused = v["paused"].as_bool().unwrap_or(true);
                    state.pause_epoch = state.pause_epoch.wrapping_add(1);
                    if !state.paused {
                        for job in &mut state.jobs {
                            if job.start_required {
                                job.start_required = false;
                            }
                        }
                    }
                }
                self.shared.wake.notify_all();
                self.save()?;
                Ok(json!({"ok":true}))
            }
            // Le protocole historique reste disponible ; une restauration demande d’abord l’arrêt du lot.
            Command::Cancel | Command::CancelMany | Command::UndoMany => {
                let undo = v["command"] == Command::UndoMany.as_str();
                let ids: Vec<u64> = if let Some(id) = v["id"].as_u64() {
                    vec![id]
                } else {
                    v["ids"]
                        .as_array()
                        .ok_or("No operation selected")?
                        .iter()
                        .map(|id| id.as_u64().ok_or("Identifiant invalide"))
                        .collect::<Result<_, _>>()?
                };
                if ids.is_empty() {
                    return Err("No operation selected".into());
                }
                {
                    let mut state = self.shared.state.lock().unwrap();
                    if ids.iter().any(|id| !state.jobs.iter().any(|j| j.id == *id)) {
                        return Err("Operation not found".into());
                    }
                    if undo
                        && state
                            .jobs
                            .iter()
                            .any(|j| ids.contains(&j.id) && !j.journaled)
                    {
                        return Err("This operation comes from an older version without a journal. Its history is available, but automatic restoration is unavailable".into());
                    }
                    for job in &mut state.jobs {
                        if ids.contains(&job.id) {
                            job.cancel = true;
                            job.resume_pending = false;
                            if undo && job.state != TransferState::Undone {
                                job.retry_entries = None;
                                job.retry_all = false;
                                job.undo_entries = None;
                                job.undo_requested = true;
                                if terminal(&job.state) || job.state == TransferState::Scanning {
                                    job.state = TransferState::UndoWaiting;
                                }
                            } else if job.state == TransferState::Scanning {
                                job.state = TransferState::Cancelled;
                            }
                        }
                    }
                }
                self.save()?;
                self.shared.wake.notify_all();
                Ok(json!({"ok":true}))
            }
            // Le choix de conflit est consommé par le travailleur en attente ; all fixe aussi sa politique.
            Command::Resolve => {
                let id = v["id"].as_u64().ok_or("Identifiant manquant")?;
                let action = v["action"].as_str().ok_or("Choix manquant")?;
                if !["skip", "replace", "rename"].contains(&action) {
                    return Err("Choix invalide".into());
                }
                {
                    let mut state = self.shared.state.lock().unwrap();
                    let job = state
                        .jobs
                        .iter_mut()
                        .find(|job| job.id == id)
                        .ok_or("Operation not found")?;
                    // L’interface transmet l’indice affiché : un choix périmé ne doit pas
                    // répondre au conflit suivant du même lot. Les anciens clients peuvent l’omettre.
                    if let Some(value) = v.get("index") {
                        let index = value
                            .as_u64()
                            .and_then(|i| usize::try_from(i).ok())
                            .ok_or("Indice de conflit invalide")?;
                        if job.cancel
                            || job.entries.get(index).is_none_or(|entry| {
                                entry.state != TransferState::Conflict || entry.cancel
                            })
                        {
                            return Err(
                                "This conflict is no longer pending. The list will be refreshed"
                                    .into(),
                            );
                        }
                    }
                    job.resolution = Some(action.into());
                    if v["all"].as_bool() == Some(true) {
                        job.policy = action.into();
                    }
                }
                self.shared.wake.notify_all();
                Ok(json!({"ok":true}))
            }
            Command::ResumeInterrupted => {
                let ids = v["ids"]
                    .as_array()
                    .ok_or("Identifiants manquants")?
                    .iter()
                    .map(|id| id.as_u64().ok_or("Identifiant invalide"))
                    .collect::<Result<Vec<_>, _>>()?;
                self.resume_interrupted(&ids)
            }
            Command::ReorderRows => self.reorder_rows(&v),
            Command::ReorderQueue => self.reorder_queue(&v),
            Command::CancelAll => self.cancel_all(),
            Command::UndoAll => self.undo_all(),
            Command::PauseSelection | Command::RetrySelection => self.control_selection(&v),
            Command::DeleteSelection
            | Command::BackspaceSelection
            | Command::UndoSelection
            | Command::CancelSelection => self.delete_selection(&v),
            // Efface l’historique terminé et ses sauvegardes, pas les copies publiées.
            Command::Clear => {
                self.discard_history()?;
                Ok(json!({"ok":true}))
            }
            // Refuse toute fermeture avec opération active, puis attend les travailleurs après réveil.
            Command::Shutdown => {
                if self.shared.state.lock().unwrap().jobs.iter().any(|j| {
                    (!terminal(&j.state)
                        && (!j.start_required || j.state == TransferState::Scanning))
                        || j.undo_requested
                }) {
                    return Err("Operations are still active".into());
                }
                if !self.shared.state.lock().unwrap().keep_history {
                    self.discard_history()?;
                }
                self.save()?;
                {
                    // Le même verrou protège le prédicat d’attente : aucun travailleur ne peut s’endormir après le réveil.
                    let _state = self.shared.state.lock().unwrap();
                    self.shared
                        .stop
                        .store(true, std::sync::atomic::Ordering::SeqCst);
                    self.shared.wake.notify_all();
                }
                // Les simulations n’écrivent rien et ne retardent pas la fermeture.
                for (_, flag) in std::mem::take(&mut self.shared.simulations.lock().unwrap().tasks)
                {
                    flag.store(true, std::sync::atomic::Ordering::Relaxed);
                }
                for handle in self.shared.worker.lock().unwrap().drain(..) {
                    let _ = handle.join();
                }
                self.checkpoint_history()?;
                Ok(json!({"ok":true}))
            }
            _ => Err("Commande inconnue".into()),
        }
    }
    /// Modifie un lot s’il existe encore et réveille les travailleurs en attente.
    /// Cette méthode ne persiste pas à elle seule les changements.
    fn update(&self, id: u64, update: impl FnOnce(&mut Job)) {
        let mut state = self.shared.state.lock().unwrap();
        if let Some(job) = state.jobs.iter_mut().find(|j| j.id == id) {
            update(job);
        }
        self.shared.wake.notify_all();
    }
    /// Ne pas attribuer comme suffixe un nom déjà prévu par un autre fichier de la file.
    fn available_queued_name(&self, id: u64, index: usize, path: &Path) -> Result<PathBuf, String> {
        let (reserved, normalized_parent) = {
            let state = self.shared.state.lock().unwrap();
            let mut reserved = std::collections::HashSet::new();
            for job in &state.jobs {
                for (i, e) in job.entries.iter().enumerate() {
                    if job.id == id && i == index {
                        continue;
                    }
                    if terminal(&e.state)
                        || e.state == TransferState::Skipped
                        || e.hidden
                        || e.cancel
                    {
                        continue;
                    }
                    reserved.insert(e.destination.clone());
                    if let Some(key) = job.destination_keys.as_ref().and_then(|keys| keys.get(i)) {
                        reserved.insert(key.clone());
                    }
                }
                if job.state == TransferState::Scanning {
                    for source in &job.sources {
                        if let Some(name) = source.file_name() {
                            reserved.insert(job.destination.join(name));
                        }
                    }
                }
            }
            let parent = state
                .jobs
                .iter()
                .find(|j| j.id == id)
                .and_then(|j| j.destination_keys.as_ref())
                .and_then(|keys| keys.get(index))
                .and_then(|key| key.parent())
                .map(Path::to_path_buf);
            (reserved, parent)
        };
        available_name(path, |candidate| {
            reserved.contains(candidate)
                || normalized_parent.as_ref().is_some_and(|parent| {
                    candidate
                        .file_name()
                        .is_some_and(|name| reserved.contains(&parent.join(name)))
                })
        })
    }
    /// Un nom issu d’un conflit peut changer les dépendances et le parent visuel indexés.
    fn set_destination(&self, id: u64, index: usize, destination: &Path) {
        let changed = {
            let state = self.shared.state.lock().unwrap();
            state
                .jobs
                .iter()
                .find(|j| j.id == id)
                .is_some_and(|j| j.entries[index].destination != destination)
        };
        if !changed {
            return;
        }
        // Les montages sont consultés hors verrou ; seul le travailleur de copie change ce nom.
        let key = fast_move::destination_keys(std::iter::once(destination.to_path_buf()))
            .and_then(|mut keys| keys.pop());
        let mut state = self.shared.state.lock().unwrap();
        if let Some(job) = state.jobs.iter_mut().find(|j| j.id == id) {
            job.entries[index].destination = destination.to_path_buf();
            if let (Some(keys), Some(key)) = (&mut job.destination_keys, key) {
                if let Some(current) = keys.get_mut(index) {
                    *current = key;
                }
            } else {
                job.destination_keys = None;
            }
            state.row_layout = None;
        }
    }
    /// Énumère une source en profondeur, parents avant enfants, sans suivre les liens.
    /// Enregistre l’identité initiale des éléments et refuse les types spéciaux.
    fn scan(
        &self,
        id: u64,
        source: &Path,
        destination: &Path,
        entries: &mut Vec<Entry>,
    ) -> Result<(), String> {
        let rules = self
            .shared
            .state
            .lock()
            .unwrap()
            .jobs
            .iter()
            .find(|job| job.id == id)
            .ok_or("Operation absent")?
            .exclusions
            .clone();
        let destination = self.duplicate_target(id, source, destination)?;
        self.scan_selected(id, source, &destination, entries, &rules)
    }
    /// Les descendants exclus ne sont jamais énumérés ; les liens restent des entrées de fichier.
    fn scan_selected(
        &self,
        id: u64,
        source: &Path,
        destination: &Path,
        entries: &mut Vec<Entry>,
        rules: &exclusions::Rules,
    ) -> Result<(), String> {
        {
            let state = self.shared.state.lock().unwrap();
            if state
                .jobs
                .iter()
                .find(|j| j.id == id)
                .is_none_or(|j| j.cancel)
            {
                return Err("Canceled".into());
            }
        }
        if source.to_str().is_none() || destination.to_str().is_none() {
            return Err(
                "Nom de fichier non UTF-8 : cette version ne le prend pas en charge".into(),
            );
        }
        let name = source
            .file_name()
            .and_then(|name| name.to_str())
            .ok_or("Invalid copy source.")?;
        if !rules.include_hidden && name.starts_with('.') {
            return Ok(());
        }
        let meta =
            fs::symlink_metadata(source).map_err(|e| format!("{} : {e}", source.display()))?;
        if rules.excludes(name, meta.is_dir()) {
            return Ok(());
        }
        let kind = source_kind(&meta)
            .ok_or_else(|| format!("Fichier spécial non pris en charge : {}", source.display()))?;
        entries.push(Entry {
            paused: false,
            hidden: false,
            cancel: false,
            identity: (meta.dev(), meta.ino()),
            source: source.into(),
            destination: destination.into(),
            kind: kind.into(),
            size: if kind == "file" { meta.len() } else { 0 },
            copied: 0,
            verified: 0,
            properties: None,
            fast_move_check: None,
            md5_source: None,
            md5_destination: None,
            interrupted: false,
            resume_checking: false,
            resume_checked: 0,
            resumed_from: 0,
            state: TransferState::Waiting,
            error: String::new(),
            undo: None,
        });
        if kind == "directory" {
            let mut children = fs::read_dir(source)
                .map_err(|e| e.to_string())?
                .collect::<Result<Vec<_>, _>>()
                .map_err(|e| e.to_string())?;
            sort_children(&mut children);
            for child in children {
                self.scan_selected(
                    id,
                    &child.path(),
                    &destination.join(child.file_name()),
                    entries,
                    rules,
                )?;
            }
        }
        Ok(())
    }
    /// Vérifie l’arrêt et la pause à l’échelle du lot.
    fn checkpoint(&self, id: u64) -> Result<(), String> {
        self.checkpoint_for(id, None)
    }
    /// Ajoute au contrôle du lot les demandes de pause et d’annulation d’une ligne.
    fn checkpoint_entry(&self, id: u64, index: usize) -> Result<(), String> {
        self.checkpoint_for(id, Some(index))
    }
    /// Attend sur la condition de réveil tant qu’une pause s’applique. L’annulation
    /// est vérifiée avant la pause pour pouvoir arrêter un transfert déjà suspendu.
    fn checkpoint_for(&self, id: u64, index: Option<usize>) -> Result<(), String> {
        let mut state = self.shared.state.lock().unwrap();
        loop {
            if checkpoint_ready(&state, id, index)? {
                return Ok(());
            }
            state = self
                .shared
                .wake
                .wait_timeout(state, std::time::Duration::from_millis(500))
                .unwrap()
                .0;
            let cancelled = state.jobs.iter().find(|j| j.id == id).is_none_or(|j| {
                j.cancel || index.is_some_and(|i| j.entries.get(i).is_none_or(|e| e.cancel))
            });
            if !cancelled && index.is_some() && reconnect::probe_due() {
                drop(state);
                self.check_connection(id)?;
                state = self.shared.state.lock().unwrap();
            }
        }
    }
    /// Réserve un travail, effectue ses entrées puis libère les disques réservés.
    /// Les erreurs et annulations sont distinguées pour préserver les autres lignes ;
    /// les restaurations passent par la même réservation exclusive.
    fn work(&self) {
        loop {
            // Sélection et réservation restent atomiques sous state pour éviter le double départ du même lot.
            let (job, rollback, plan, row_revision) = {
                let mut state = self.shared.state.lock().unwrap();
                loop {
                    if self.shared.stop.load(std::sync::atomic::Ordering::SeqCst) {
                        return;
                    }
                    if let Some((id, rollback)) = scheduling::next(&mut state) {
                        let (plan, row_revision) = if rollback {
                            (Vec::new(), state.row_revision)
                        } else {
                            row_queue::branch(&mut state, id)
                        };
                        // Une simple finalisation de lignes annulées n’est pas une nouvelle écriture.
                        let starting = !rollback
                            && state.jobs.iter().find(|j| j.id == id).is_some_and(|j| {
                                plan.iter().any(|i| {
                                    let e = &j.entries[*i];
                                    e.state == TransferState::Waiting
                                        && !e.cancel
                                        && !e.hidden
                                        && !e.paused
                                })
                            });
                        if starting {
                            state.next_execution_order =
                                state.next_execution_order.saturating_add(1);
                        }
                        let sequence = state.next_execution_order;
                        let job = state.jobs.iter_mut().find(|j| j.id == id).unwrap();
                        if starting {
                            job.execution_order = sequence;
                        }
                        job.state = if rollback {
                            TransferState::Undoing
                        } else {
                            TransferState::Running
                        };
                        let job = job.snapshot();
                        state.active.insert(id, job.disks.clone());
                        break (job, rollback, plan, row_revision);
                    }
                    state = self.shared.wake.wait(state).unwrap();
                }
            };
            if rollback {
                let id = job.id;
                self.rollback(job);
                self.release_worker(id);
                continue;
            }
            // Un refus précède toute création de dossier ou fichier du lot.
            if job.verify_space {
                if let Err(error) = self.check_queue_space(job.id) {
                    self.update(job.id, |current| {
                        current.state = if current.cancel {
                            TransferState::Cancelled
                        } else {
                            TransferState::Error
                        };
                        current.error = if current.cancel {
                            String::new()
                        } else {
                            error.clone()
                        };
                        for entry in &mut current.entries {
                            if !terminal(&entry.state) && entry.state != TransferState::Skipped {
                                entry.state = if current.cancel || entry.cancel {
                                    TransferState::Cancelled
                                } else {
                                    TransferState::Error
                                };
                                entry.error = if current.cancel || entry.cancel {
                                    String::new()
                                } else {
                                    error.clone()
                                };
                            }
                        }
                    });
                    let _ = self.save();
                    self.release_worker(job.id);
                    continue;
                }
            }
            self.update(job.id, |j| j.state = TransferState::Running);
            let mut had_error = job.entries.iter().any(|e| e.state == TransferState::Error);
            let mut requeue = false;
            let mut completed_trees = Vec::<std::ops::Range<usize>>::new();
            for index in plan {
                let entry = &job.entries[index];
                if completed_trees.iter().any(|range| range.contains(&index)) {
                    continue;
                }
                if self.shared.state.lock().unwrap().row_revision != row_revision {
                    requeue = true;
                    break;
                }
                if self.checkpoint(job.id).is_err() {
                    break;
                }
                let skipped = {
                    let mut state = self.shared.state.lock().unwrap();
                    let current = state.jobs.iter_mut().find(|j| j.id == job.id).unwrap();
                    let e = &mut current.entries[index];
                    if e.paused && !e.cancel {
                        true
                    } else if e.cancel
                        || e.hidden
                        || terminal(&e.state)
                        || e.state == TransferState::Skipped
                    {
                        if !terminal(&e.state) && e.state != TransferState::Skipped {
                            e.state = TransferState::Cancelled;
                        }
                        true
                    } else {
                        e.state = TransferState::Running;
                        false
                    }
                };
                if skipped {
                    self.update(job.id, finish_directories);
                    continue;
                }

                let result =
                    self.copy_entry(job.id, index, entry, job.mode == "move", job.verify_md5);
                if let Ok(EntryOutcome::TreeComplete(end)) = &result {
                    completed_trees.push(index..*end);
                }
                if matches!(result, Ok(EntryOutcome::Requeue)) {
                    requeue = true;
                    break;
                }
                if let Err(error) = result {
                    if self.park_disconnected(job.id, index, &error) {
                        requeue = true;
                        break;
                    }
                    let cancelled = self
                        .shared
                        .state
                        .lock()
                        .unwrap()
                        .jobs
                        .iter()
                        .find(|j| j.id == job.id)
                        .is_none_or(|j| j.cancel || j.entries[index].cancel);
                    if !cancelled {
                        had_error = true;
                    }
                    self.update(job.id, |j| {
                        // Une erreur de synchronisation peut suivre le renommage physique du bloc.
                        // Ne jamais rejouer ses enfants ni écraser leurs journaux avec une nouvelle copie.
                        let end = if j.entries[index]
                            .undo
                            .as_ref()
                            .is_some_and(|record| record.directory_move)
                        {
                            index
                                + j.entries[index..]
                                    .iter()
                                    .take_while(|child| child.source.starts_with(&entry.source))
                                    .count()
                        } else {
                            index + 1
                        };
                        completed_trees.push(index..end);
                        for current in &mut j.entries[index..end] {
                            current.state = if cancelled {
                                TransferState::Cancelled
                            } else {
                                TransferState::Error
                            };
                            current.error = error.clone();
                        }
                    });
                }
                self.update(job.id, finish_directories);
            }
            // Une demande peut contenir plusieurs racines séparées par celles d’autres demandes.
            // Rendre la réservation à chaque branche permet de respecter cet ordre sans interrompre un fichier.
            if !requeue {
                let state = self.shared.state.lock().unwrap();
                if let Some(current) = state.jobs.iter().find(|j| j.id == job.id) {
                    requeue = !current.cancel
                        && current
                            .entries
                            .iter()
                            .any(|e| e.state == TransferState::Waiting && !e.hidden && !e.cancel);
                }
            }
            // Un EXDEV inattendu libère d’abord l’exception de parallélisme ; le lot reprendra en copie classique.
            if requeue {
                self.update(job.id, |current| {
                    if current.state != TransferState::DeviceWaiting {
                        current.state = TransferState::Waiting;
                    }
                });
                self.release_worker(job.id);
                continue;
            }
            // Les dossiers source sont retirés à la fin, en ordre inverse, et seulement s’ils sont vides.
            if job.mode == "move" && !had_error {
                for (index, entry) in job.entries.iter().enumerate().rev() {
                    if entry.kind != "directory" {
                        continue;
                    }
                    if self
                        .shared
                        .state
                        .lock()
                        .unwrap()
                        .jobs
                        .iter()
                        .find(|j| j.id == job.id)
                        .unwrap()
                        .entries[index]
                        .state
                        != TransferState::Done
                    {
                        continue;
                    }
                    if let Err(error) = self.check_connection(job.id) {
                        if self.park_disconnected(job.id, index, &error) {
                            requeue = true;
                            break;
                        }
                    }
                    let mut record = self
                        .shared
                        .state
                        .lock()
                        .unwrap()
                        .jobs
                        .iter()
                        .find(|j| j.id == job.id)
                        .unwrap()
                        .entries[index]
                        .undo
                        .clone()
                        .unwrap_or_default();
                    if record.source_dir_removed
                        && fs::symlink_metadata(&entry.source)
                            .is_err_and(|e| e.kind() == std::io::ErrorKind::NotFound)
                    {
                        continue;
                    }
                    if self.checkpoint(job.id).is_err() {
                        break;
                    }
                    match undo::Stamp::read(&entry.source) {
                        Ok(stamp) => record.source_stamp = Some(stamp),
                        Err(error) => {
                            if self.park_disconnected(job.id, index, &error) {
                                requeue = true;
                                break;
                            }
                            had_error = true;
                            self.update(job.id, |j| j.entries[index].error = error);
                            continue;
                        }
                    }
                    record.source_dir_removed = true;
                    if let Err(error) = self.record(job.id, index, record.clone()) {
                        had_error = true;
                        self.update(job.id, |j| j.entries[index].error = error);
                        continue;
                    }
                    let result = fs::symlink_metadata(&entry.source).and_then(|meta| {
                        if (meta.dev(), meta.ino()) != entry.identity || !meta.is_dir() {
                            return Err(std::io::Error::other("Source folder changed; kept"));
                        }
                        fs::remove_dir(&entry.source)
                    });
                    if let Err(error) = result {
                        // Ignored/conflicting files and newly created files must stay at the source.
                        record.source_dir_removed = false;
                        let _ = self.record(job.id, index, record);
                        if error.kind() != std::io::ErrorKind::DirectoryNotEmpty {
                            let error = reconnect::io_error(error);
                            if self.park_disconnected(job.id, index, &error) {
                                requeue = true;
                                break;
                            }
                            had_error = true;
                            self.update(job.id, |j| {
                                j.entries[index].state = TransferState::Error;
                                j.entries[index].error =
                                    format!("Dossier source conservé : {error}");
                            });
                        }
                    }
                }
            }
            if requeue {
                self.release_worker(job.id);
                continue;
            }
            self.update(job.id, |j| {
                finish_directories(j);
                j.resume_pending = false;
                for entry in &mut j.entries {
                    entry.resume_checking = false;
                }
                if j.cancel {
                    j.state = TransferState::Cancelled;
                    for entry in &mut j.entries {
                        if ["waiting", "running", "verifying"].contains(&entry.state.as_str()) {
                            entry.state = TransferState::Cancelled;
                        }
                    }
                } else {
                    // Une reprise partielle ne doit pas masquer les erreurs des lignes non réactivées.
                    j.state = if had_error
                        || j.entries
                            .iter()
                            .any(|entry| entry.state == TransferState::Error)
                    {
                        TransferState::Error
                    } else if j
                        .entries
                        .iter()
                        .any(|e| e.cancel || e.state == TransferState::Cancelled)
                    {
                        TransferState::Cancelled
                    } else {
                        TransferState::Done
                    };
                    if j.state == TransferState::Done {
                        j.recovery = false;
                    }
                }
            });
            let _finalization = completion::Activity::new(self.clone(), false);
            self.release_worker(job.id);
            if let Err(error) = self.prune_hidden_jobs().and_then(|_| self.save()) {
                self.update(job.id, |j| {
                    j.state = TransferState::Error;
                    j.error = error;
                });
            }
        }
    }
    /// Attend une décision de remplacement, renommage ou saut de fichier.
    /// Respecte aussi les pauses et annulations pendant l’attente du choix utilisateur.
    fn conflict(&self, id: u64, index: usize) -> Result<String, String> {
        let mut state = self.shared.state.lock().unwrap();
        loop {
            let globally_paused = state.paused;
            let job = state
                .jobs
                .iter_mut()
                .find(|j| j.id == id)
                .ok_or("Operation absent")?;
            if job.cancel || job.entries[index].cancel {
                return Err("Canceled".into());
            }
            if globally_paused || job.paused || job.entries[index].paused {
                state = self.shared.wake.wait(state).unwrap();
                continue;
            }
            if let Some(action) = job.resolution.take() {
                job.entries[index].state = TransferState::Running;
                return Ok(action);
            }
            if job.policy != "ask" {
                let policy = job.policy.clone();
                if policy == "newer" {
                    let entry = job.entries[index].clone();
                    drop(state);
                    return conflicts::newer(&entry.source, &entry.destination).map(str::to_owned);
                }
                return Ok(policy);
            }
            job.entries[index].state = TransferState::Conflict;
            state = self.shared.wake.wait(state).unwrap();
        }
    }
    /// Prépare et journalise une copie dans un fichier privé sur le disque cible.
    /// Le résultat est publié après les vérifications ; en mode déplacement,
    /// l’original n’est supprimé qu’après publication et contrôle de son identité.
    fn copy_entry(
        &self,
        id: u64,
        index: usize,
        entry: &Entry,
        moving: bool,
        verify_md5: bool,
    ) -> Result<EntryOutcome, String> {
        reconnect::clear_io();
        self.check_connection(id)?;
        let io = reconnect::io_error;
        self.checkpoint_entry(id, index)?;
        safe_parents(&entry.destination)?;
        let properties = self.capture_properties(id, &entry.source);
        if properties.is_some() {
            self.update(id, |job| job.entries[index].properties = None);
        }
        if moving
            && properties
                .as_ref()
                .is_some_and(|snapshot| !snapshot.complete())
        {
            self.report_properties(id, index, properties.as_ref().unwrap(), &entry.source);
            return Err("Move stopped: source properties cannot be verified; original kept".into());
        }
        if entry.kind == "directory" {
            if moving && !verify_md5 {
                if let Some(outcome) = self.rename_tree(id, index)? {
                    return Ok(outcome);
                }
            }
            // Comme pour les fichiers et les liens, l’identité du dossier doit encore correspondre à l’analyse.
            let source = fs::symlink_metadata(&entry.source)
                .map_err(|error| format!("Dossier source {} : {error}", entry.source.display()))?;
            if source_kind(&source) != Some("directory")
                || (source.dev(), source.ino()) != entry.identity
            {
                return Err("Source folder was replaced; kept".into());
            }
            match fs::symlink_metadata(&entry.destination) {
                Ok(meta) => {
                    if !meta.is_dir() || meta.file_type().is_symlink() {
                        return Err("The target folder is occupied by a file or link".into());
                    }
                }
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
                    let temp = undo::private_directory(
                        entry.destination.parent().ok_or("Dossier parent absent")?,
                        "dir",
                    )?;
                    // Le garde couvre aussi les erreurs de métadonnées et de journal avant publication.
                    let _cleanup = TempDirectory(temp.clone());
                    let stamp = undo::Stamp::read(&temp)?;
                    let record = undo::Record {
                        staging: Some(temp.clone()),
                        staging_stamp: Some(stamp.clone()),
                        result: Some(stamp),
                        ..Default::default()
                    };
                    self.record(id, index, record)?;
                    self.checkpoint_entry(id, index)?;
                    publish_new(&temp, &entry.destination).map_err(io)?;
                    use std::os::unix::fs::PermissionsExt;
                    let directory = fs::File::open(&entry.destination).map_err(io)?;
                    if !fast_move::descriptor_is_kio_fuse(&directory) {
                        directory
                            .set_permissions(fs::Permissions::from_mode(0o755))
                            .map_err(io)?;
                    }
                }
                Err(error) => return Err(error.to_string()),
            }
            if let Some(snapshot) = &properties {
                if !self.report_properties(id, index, snapshot, &entry.destination) && moving {
                    return Err(
                        "Source folder kept: properties differ or cannot be verified".into(),
                    );
                }
            }
            self.update(id, |j| j.entries[index].state = TransferState::Running);
            return Ok(EntryOutcome::Complete);
        }
        let fast = moving
            && !verify_md5
            && self
                .shared
                .state
                .lock()
                .unwrap()
                .jobs
                .iter()
                .find(|job| job.id == id)
                .is_some_and(|job| job.fast_move);
        if fast {
            let result = self.rename_entry(id, index, entry);
            if matches!(result, Ok(EntryOutcome::Complete)) {
                if let Some(snapshot) = &properties {
                    self.report_properties(id, index, snapshot, &entry.destination);
                }
            }
            return result;
        }
        let mut destination = entry.destination.clone();
        let prior = entry.undo.as_ref().filter(|record| record.resume.is_some());
        let mut replace = prior
            .and_then(|record| record.resume.as_ref())
            .is_some_and(|point| point.replace);
        let mut renamed = false;
        if prior.is_none() && fs::symlink_metadata(&destination).is_ok() {
            match self.conflict(id, index)?.as_str() {
                "skip" => {
                    self.update(id, |j| j.entries[index].state = TransferState::Skipped);
                    return Ok(EntryOutcome::Complete);
                }
                "replace" => {
                    replace = true;
                }
                "rename" => {
                    destination = self.available_queued_name(id, index, &destination)?;
                    renamed = true;
                }
                _ => unreachable!(),
            }
        }
        self.checkpoint_entry(id, index)?;
        loop {
            self.reserve_destination(id, index, &destination)?;
            // Le voisin attendu peut avoir publié ce suffixe pendant l’attente : en choisir un autre.
            if renamed && fs::symlink_metadata(&destination).is_ok() {
                destination = self.available_queued_name(id, index, &entry.destination)?;
                continue;
            }
            break;
        }
        let parent = destination
            .parent()
            .ok_or("Destination sans dossier parent")?;
        let mut effective = entry.clone();
        effective.destination = destination.clone();
        // Recontrôler le fichier juste avant son allocation : d’autres logiciels peuvent remplir le disque.
        self.check_file_space(id, index, &effective)?;
        let origin_stamp = undo::Stamp::read(&entry.source)?;
        let (temp, mut output, resumed, mut record, mut undo_hash, mut total) = if prior.is_some() {
            let resumed = self.open_partial(id, index, &effective)?;
            (
                resumed.path,
                resumed.output,
                Some((resumed.source, resumed.metadata)),
                resumed.record,
                resumed.hash,
                resumed.bytes,
            )
        } else {
            let (temp, output) = self.create_staging(id, index, &destination)?;
            let record = undo::Record {
                source_stamp: Some(origin_stamp),
                staging: Some(temp.clone()),
                staging_stamp: Some(undo::Stamp::read(&temp)?),
                ..Default::default()
            };
            (temp, output, None, record, md5::Context::new(), 0)
        };
        // Une validation de reprise refusée n’atteint pas ce garde : le préfixe est alors conservé pour récupération.
        let cleanup = reconnect::CopyTemp::new(self.clone(), id, temp.clone());
        // KIO Fuse charge autrement l’ancien contenu avant la première écriture ; ce fichier vient d’être créé exclusivement.
        // Fixer sa taille à zéro initialise explicitement son cache, sans relire un fichier FTP vide.
        if prior.is_none() && fast_move::descriptor_is_kio_fuse(&output) {
            output.set_len(0).map_err(io)?;
        }
        if record.source_stamp.is_none() {
            record.source_stamp = Some(undo::Stamp::read(&entry.source)?);
        }
        self.set_destination(id, index, &destination);
        self.record(id, index, record.clone())?;
        let source_metadata;
        if entry.kind == "link" {
            source_metadata = fs::symlink_metadata(&entry.source).map_err(io)?;
            if (source_metadata.dev(), source_metadata.ino()) != entry.identity {
                return Err("Source was replaced; kept".into());
            }
            let link = fs::read_link(&entry.source).map_err(io)?;
            drop(output);
            fs::remove_file(&temp).map_err(io)?;
            symlink(link, &temp).map_err(io)?;
            record.staging_stamp = Some(undo::Stamp::read(&temp)?);
            self.record(id, index, record.clone())?;
        } else {
            // Linux O_NOFOLLOW: a source swapped for a symlink is never followed.
            let (mut source, meta) = if let Some(resumed) = resumed {
                resumed
            } else {
                let source = OpenOptions::new()
                    .read(true)
                    .custom_flags(0x20000 | 0x800)
                    .open(&entry.source)
                    .map_err(io)?;
                let meta = source.metadata().map_err(io)?;
                (source, meta)
            };
            if (meta.dev(), meta.ino()) != entry.identity {
                return Err("Source was replaced; kept".into());
            }
            source_metadata = meta.clone();
            if !meta.is_file() {
                return Err("La source n’est plus un fichier ordinaire".into());
            }
            // La source peut avoir changé de taille pendant son attente, avant l’ouverture effective.
            self.update(id, |job| job.entries[index].size = meta.len());
            let resume_enabled = self
                .shared
                .state
                .lock()
                .unwrap()
                .jobs
                .iter()
                .find(|job| job.id == id)
                .is_some_and(|job| job.resume_enabled);
            if (resume_enabled || !fast_move::descriptor_is_kio_fuse(&output))
                && record.resume.is_none()
            {
                let current = fs::symlink_metadata(&entry.source).map_err(io)?;
                if !same_version(&meta, &current)
                    || (meta.ctime(), meta.ctime_nsec()) != (current.ctime(), current.ctime_nsec())
                {
                    return Err("Source changed before the first checkpoint".into());
                }
                record.resume = Some(resume::Point::initial(&effective, replace)?);
                self.save_copy_point(
                    (id, index),
                    &effective,
                    &output,
                    &mut record,
                    total,
                    &undo_hash,
                )?;
            }
            let mut saved = total;
            let mut point_clock = std::time::Instant::now();
            let mut connection_check = std::time::Instant::now();
            let mut buffer = copy_buffer(meta.len().saturating_sub(total));
            loop {
                self.checkpoint_entry(id, index)?;
                if connection_check.elapsed() >= std::time::Duration::from_millis(500) {
                    self.check_connection(id)?;
                    connection_check = std::time::Instant::now();
                }
                let count = source.read(&mut buffer).map_err(io)?;
                if count == 0 {
                    break;
                }
                // Une source alimentée en continu ne doit pas prolonger indéfiniment la copie.
                if total.saturating_add(count as u64) > meta.len() {
                    return Err("Source changed during copying".into());
                }
                // Fractionner le tampon permet les très faibles débits sans retarder les commandes.
                let mut written = 0;
                while written < count {
                    let budget = self.transfer_budget(id, index, count - written)?;
                    output
                        .write_all(&buffer[written..written + budget])
                        .map_err(io)?;
                    written += budget;
                    self.update(id, |j| j.entries[index].copied = total + written as u64);
                }
                undo_hash.consume(&buffer[..count]);
                total += count as u64;
                self.update(id, |j| j.entries[index].copied = total);
                if record.resume.is_some() && resume::due(total, saved, point_clock) {
                    self.save_copy_point(
                        (id, index),
                        &effective,
                        &output,
                        &mut record,
                        total,
                        &undo_hash,
                    )?;
                    saved = total;
                    point_clock = std::time::Instant::now();
                }
            }
            if total != meta.len() || !same_version(&meta, &source.metadata().map_err(io)?) {
                return Err("Source changed during copying".into());
            }
            if record.resume.is_some() {
                self.save_copy_point(
                    (id, index),
                    &effective,
                    &output,
                    &mut record,
                    total,
                    &undo_hash,
                )?;
            }
            // Les services KIO n’offrent pas tous chmod/utime ; le contenu est transmis avec leurs droits et dates par défaut.
            if !fast_move::descriptor_is_kio_fuse(&output) {
                output.set_permissions(meta.permissions()).map_err(io)?;
                if let Ok(modified) = meta.modified() {
                    output
                        .set_times(fs::FileTimes::new().set_modified(modified))
                        .map_err(io)?;
                }
            }
            output.sync_all().map_err(io)?;
            record.digest = Some(format!("{:x}", undo_hash.finalize()));
            // La source et le temporaire sont relus avant publication, et donc avant suppression de la source.
            if verify_md5 {
                self.update(id, |j| j.entries[index].state = TransferState::Verifying);
                let comparison = compare_md5(&entry.source, &temp, &source_metadata, |read| {
                    self.checkpoint_entry(id, index)?;
                    self.update(id, |j| j.entries[index].verified = read);
                    Ok(())
                })?;
                self.update(id, |j| {
                    j.entries[index].md5_source = Some(comparison.0.clone());
                    j.entries[index].md5_destination = Some(comparison.1.clone());
                });
                if comparison.0 != comparison.1 {
                    return Err(format!("MD5 différent : source {}, destination {}. Original et destination existante conservés", comparison.0, comparison.1));
                }
            }
        }
        self.checkpoint_entry(id, index)?;
        self.check_connection(id)?;
        safe_parents(&destination)?;
        if let Some(point) = &record.resume {
            point.environment(&effective)?;
        }
        if let Some(snapshot) = &properties {
            if !self.report_properties(id, index, snapshot, &temp) && moving {
                return Err(
                    "Move stopped: properties differ or cannot be verified; original kept".into(),
                );
            }
        }
        // La pause, le MD5 et les contrôles de propriétés ont pu laisser la source évoluer.
        verify_source(&entry.source, &source_metadata)?;
        record.resume = None;
        record.source_stamp = Some(undo::Stamp::read(&entry.source)?);
        record.result = Some(undo::Stamp::read(&temp)?);
        if replace {
            let newer = self
                .shared
                .state
                .lock()
                .unwrap()
                .jobs
                .iter()
                .find(|job| job.id == id)
                .is_some_and(|job| job.policy == "newer");
            if newer && conflicts::newer(&entry.source, &destination)? != "replace" {
                return Err("Destination is no longer older; kept".into());
            }
            self.protect_fast_result(id, index, &destination)?;
            let meta = fs::symlink_metadata(&destination).map_err(io)?;
            if meta.is_dir() {
                return Err("Impossible de remplacer un dossier par un fichier".into());
            }
            let backup_dir = undo::private_directory(parent, "undo")?;
            record.backup = Some(backup_dir.join("original"));
            record.original = Some(undo::Stamp::read(&destination)?);
        }
        // Le journal précède le renommage de l’ancienne destination afin qu’un arrêt reste récupérable.
        self.record(id, index, record.clone())?;
        if let Some(backup) = &record.backup {
            if !record.original.as_ref().unwrap().matches(&destination) {
                return Err("Destination changed before replacement; kept".into());
            }
            publish_new(&destination, backup).map_err(io)?;
            let backed_up = fs::symlink_metadata(backup).map_err(io)?;
            record.backup_ctime = Some((backed_up.ctime(), backed_up.ctime_nsec()));
            self.record(id, index, record.clone())?;
            sync_directory(backup.parent().unwrap()).map_err(io)?;
            sync_directory(parent).map_err(io)?;
        }
        // Recontrôler après la sauvegarde et l’écriture du journal, juste avant publication.
        let publication = verify_source(&entry.source, &source_metadata).and_then(|()| {
            publish_new(&temp, &destination).map_err(|error| {
                format!("Publication de la copie (destination conservée) : {error}")
            })
        });
        if let Err(error) = publication {
            if let Some(backup) = &record.backup {
                let _ = publish_new(backup, &destination);
            }
            return Err(error);
        }
        drop(cleanup);
        self.set_destination(id, index, &destination);
        if let Some(snapshot) = &properties {
            if !self.report_properties(id, index, snapshot, &destination) && moving {
                return Err("Copy created; original kept: properties differ or cannot be verified after publication".into());
            }
        }
        if moving {
            // The new name must be durable before we remove the only original.
            sync_directory(parent)
                .map_err(|e| format!("Copie créée ; original conservé, synchronisation du dossier cible impossible : {e}"))?;
            record.source_removed = true;
            self.record(id, index, record.clone())?;
            remove_original(&entry.source, &source_metadata)?;
        }
        self.update(id, |j| {
            j.entries[index].destination = destination;
            j.entries[index].state = TransferState::Done;
            j.entries[index].interrupted = false;
            j.entries[index].resume_checking = false;
        });
        self.save()?;
        Ok(EntryOutcome::Complete)
    }
}
/// Déduit les états des dossiers de ceux de leurs descendants, du bas vers le haut.
/// Ne réactive pas un dossier déjà remis en état.
fn finish_directories(job: &mut Job) {
    directories::finish(job);
}
/// État de pause/annulation sous le verrou : aucune E/S ni attente ici.
fn checkpoint_ready(state: &State, id: u64, index: Option<usize>) -> Result<bool, String> {
    let job = state
        .jobs
        .iter()
        .find(|job| job.id == id)
        .ok_or("Canceled")?;
    let entry = match index {
        Some(index) => Some(job.entries.get(index).ok_or("Canceled")?),
        None => None,
    };
    if job.cancel || entry.is_some_and(|entry| entry.cancel) {
        return Err("Canceled".into());
    }
    Ok(!state.paused && !job.paused && entry.is_none_or(|entry| !entry.paused))
}
/// Tampon de copie ; la taille maximale reste commune aux copies locales et distantes.
fn copy_buffer(remaining: u64) -> Vec<u8> {
    io_buffer(remaining, 64 * 1024)
}
/// Allocation commune aux copies, calculs MD5 et contrôles de reprise.
fn io_buffer(remaining: u64, maximum: usize) -> Vec<u8> {
    // Un tampon vide ferait passer read([]) pour une fin de fichier sans lecture réelle.
    vec![0; remaining.clamp(1, maximum as u64) as usize]
}

/// Tri commun à la préparation et à l’aperçu ; conserve l’ordre lexical des noms.
fn sort_children(children: &mut [fs::DirEntry]) {
    // Un seul nom créé par élément, sans cache si l'ordre est déjà monotone.
    if children.len() < 2 || children.is_sorted_by_key(|entry| entry.file_name()) {
        return;
    }
    // Strictement décroissant : des noms égaux doivent conserver leur ordre initial.
    if children
        .iter()
        .map(|entry| entry.file_name())
        .is_sorted_by(|left, right| left > right)
    {
        children.reverse();
    } else {
        children.sort_by_cached_key(|entry| entry.file_name());
    }
}

/// Classe les types réellement pris en charge, sans déduire le type depuis le nom ou la cible d’un lien.
fn source_kind(metadata: &fs::Metadata) -> Option<&'static str> {
    if metadata.file_type().is_symlink() {
        Some("link")
    } else if metadata.is_dir() {
        Some("directory")
    } else if metadata.is_file() {
        Some("file")
    } else {
        None
    }
}
/// Compare identité, taille, mtime, ctime et type, dans la précision du système de fichiers.
/// Ctime complète mtime si cette dernière a été rétablie ; ce n’est pas un instantané.
fn same_version(a: &fs::Metadata, b: &fs::Metadata) -> bool {
    (a.dev(), a.ino()) == (b.dev(), b.ino())
        && a.len() == b.len()
        && a.modified().ok() == b.modified().ok()
        && (a.ctime(), a.ctime_nsec()) == (b.ctime(), b.ctime_nsec())
        && a.file_type() == b.file_type()
}
/// Contrôle le chemin en plus du descripteur ouvert : une substitution externe reste détectée.
fn verify_source(path: &Path, expected: &fs::Metadata) -> Result<(), String> {
    let current = fs::symlink_metadata(path).map_err(reconnect::io_error)?;
    if !same_version(expected, &current) {
        return Err("Source changed during copying".into());
    }
    Ok(())
}
/// Lit un fichier ordinaire par blocs, signale la progression et vérifie
/// que sa version reste stable avant et après le calcul.
fn md5_file(
    path: &Path,
    expected: Option<&fs::Metadata>,
    mut progress: impl FnMut(u64) -> Result<(), String>,
) -> Result<String, String> {
    let mut file = OpenOptions::new()
        .read(true)
        .custom_flags(0x20000 | 0x800)
        .open(path)
        .map_err(|e| format!("Lecture MD5 : {}", reconnect::io_error(e)))?;
    let before = file.metadata().map_err(reconnect::io_error)?;
    if !before.is_file() || expected.is_some_and(|e| !same_version(e, &before)) {
        return Err("Source file changed before MD5 verification; original kept".into());
    }
    let mut hash = md5::Context::new();
    let mut buffer = io_buffer(before.len(), 1024 * 1024);
    let mut total = 0_u64;
    progress(0)?;
    loop {
        let n = file
            .read(&mut buffer)
            .map_err(|e| format!("Lecture MD5 : {}", reconnect::io_error(e)))?;
        if n == 0 {
            break;
        }
        // Refuser une source qui grandit avant de traiter une quantité de données non prévue.
        let next = total
            .checked_add(n as u64)
            .filter(|next| *next <= before.len())
            .ok_or("File changed during MD5 verification; original kept")?;
        hash.consume(&buffer[..n]);
        total = next;
        progress(total)?;
    }
    let after = file.metadata().map_err(reconnect::io_error)?;
    if total != before.len() || !same_version(&before, &after) {
        return Err("File changed during MD5 verification; original kept".into());
    }
    Ok(format!("{:x}", hash.finalize()))
}
/// Calcule les deux empreintes et cumule la progression des deux relectures.
/// Recontrôle ensuite la source pour détecter un changement pendant la lecture cible.
fn compare_md5(
    source: &Path,
    destination: &Path,
    expected: &fs::Metadata,
    mut progress: impl FnMut(u64) -> Result<(), String>,
) -> Result<(String, String), String> {
    let source_hash = md5_file(source, Some(expected), &mut progress)?;
    let destination_hash = md5_file(destination, None, |read| {
        progress(expected.len().saturating_add(read))
    })?;
    let current = fs::symlink_metadata(source).map_err(reconnect::io_error)?;
    if !same_version(expected, &current) {
        return Err("Source file changed during MD5 verification; original kept".into());
    }
    Ok((source_hash, destination_hash))
}
/// Supprime la source d’un déplacement uniquement si elle correspond encore
/// aux métadonnées observées pendant la copie.
fn remove_original(path: &Path, expected: &fs::Metadata) -> Result<(), String> {
    let current = fs::symlink_metadata(path)
        .map_err(|e| format!("Copie créée ; vérification de l’original impossible : {e}"))?;
    if !same_version(expected, &current) {
        return Err("Copy created; the source changed, so the original was kept".into());
    }
    fs::remove_file(path)
        .map_err(|e| format!("Copie créée ; suppression de l’original impossible : {e}"))
}
/// Synchronise strictement les dossiers locaux. KIO Fuse confirme les renommages par le protocole distant
/// et ne fournit pas fsyncdir ; seul ce type de montage vérifié sur descripteur utilise cette sémantique.
fn sync_directory(path: &Path) -> std::io::Result<()> {
    let directory = fs::File::open(path)?;
    if fast_move::descriptor_is_kio_fuse(&directory) {
        return Ok(());
    }
    directory.sync_all()
}

/// Publie un chemin avec renameat2(RENAME_NOREPLACE) : une destination apparue
/// entre-temps provoque une erreur au lieu d’être écrasée.
fn publish_new(source: &Path, destination: &Path) -> std::io::Result<()> {
    use std::{ffi::CString, os::unix::ffi::OsStrExt};
    unsafe extern "C" {
        /// Expose la primitive Linux/glibc utilisée pour la publication atomique sans écrasement.
        fn renameat2(
            oldfd: i32,
            oldpath: *const std::ffi::c_char,
            newfd: i32,
            newpath: *const std::ffi::c_char,
            flags: u32,
        ) -> i32;
    }
    let source = CString::new(source.as_os_str().as_bytes())?;
    let destination = CString::new(destination.as_os_str().as_bytes())?;
    // AT_FDCWD = -100, RENAME_NOREPLACE = 1 (Linux/glibc).
    if unsafe { renameat2(-100, source.as_ptr(), -100, destination.as_ptr(), 1) } == 0 {
        Ok(())
    } else {
        Err(std::io::Error::last_os_error())
    }
}
/// Reconnaît les états sans travail de copie restant ; une restauration demandée
/// peut néanmoins être planifiée séparément.
fn terminal(state: &str) -> bool {
    TransferState::from_wire(state).is_some_and(TransferState::is_terminal)
}
/// Garde RAII : le fichier privé est retiré à la sortie du bloc s’il existe encore.
struct Temp(PathBuf);
impl Drop for Temp {
    /// Retire le fichier temporaire lors de la sortie du bloc, y compris après erreur.
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.0);
    }
}
/// Retire un dossier privé abandonné, uniquement s’il est encore vide ; ne suit aucun lien.
struct TempDirectory(PathBuf);
impl Drop for TempDirectory {
    fn drop(&mut self) {
        let _ = fs::remove_dir(&self.0);
    }
}
/// Refuse les ancêtres qui sont des liens ou qui ne sont pas des dossiers.
/// Ce contrôle de chemins ne constitue pas une isolation contre les courses malveillantes.
fn safe_parents(path: &Path) -> Result<(), String> {
    let mut parent = path.parent();
    while let Some(p) = parent {
        let meta = fs::symlink_metadata(p)
            .map_err(|e| format!("{} : {}", p.display(), reconnect::io_error(e)))?;
        if meta.file_type().is_symlink() || !meta.is_dir() {
            return Err(format!("Dossier cible non sûr : {}", p.display()));
        }
        parent = p.parent();
    }
    Ok(())
}
/// Cherche un nom libre en conservant l’extension ; la publication atomique
/// reste nécessaire car cette recherche ne réserve pas le nom.
fn available_name(path: &Path, reserved: impl Fn(&Path) -> bool) -> Result<PathBuf, String> {
    let stem = path.file_stem().unwrap_or_default().to_string_lossy();
    let extension = path
        .extension()
        .map(|e| format!(".{}", e.to_string_lossy()))
        .unwrap_or_default();
    for n in 1u64.. {
        let candidate = path.with_file_name(format!("{stem} (copie {n}){extension}"));
        match fs::symlink_metadata(&candidate) {
            Ok(_) => continue,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
                if !reserved(&candidate) {
                    return Ok(candidate);
                }
            }
            // Un nom trop long ou un refus d’accès ne deviendra pas libre en changeant le compteur.
            Err(error) => return Err(format!("Recherche d’un nom de copie impossible : {error}")),
        }
    }
    unreachable!()
}

#[cfg(test)]
mod tests;
