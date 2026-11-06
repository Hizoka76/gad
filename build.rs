//! Construction de la partie Qt liée au programme Rust.
//! Le compilateur C++ traite l’interface, rcc embarque l’icône et moc produit
//! les métadonnées Qt des slots D-Bus ; Cargo lie ensuite l’archive et ses bibliothèques.

use std::{env, path::PathBuf, process::Command};

/// Déclare les dépendances de reconstruction, génère les sources Qt et les compile
/// en objets PIC avant d’annoncer à Cargo les bibliothèques natives à lier.
fn main() {
    println!("cargo:rerun-if-changed=native");
    println!("cargo:rerun-if-env-changed=SOURCE_DATE_EPOCH");
    println!("cargo:rerun-if-changed=translations");
    println!("cargo:rerun-if-changed=assets/icons/gad.qrc");
    println!("cargo:rerun-if-changed=assets/icons/gad.png");
    println!("cargo:rerun-if-changed=assets/flags");
    println!("cargo:rerun-if-changed=Cargo.toml");
    let out = PathBuf::from(env::var_os("OUT_DIR").unwrap());
    generate_protocol(&out);
    println!("cargo:rerun-if-changed=scripts/resources.py");
    let manifest = out.join("gad.qrc");
    assert!(Command::new("python3")
        .arg("scripts/resources.py")
        .arg(&manifest)
        .status()
        .unwrap()
        .success());
    let flags = Command::new("pkg-config")
        .args([
            "--cflags",
            "Qt6Widgets",
            "Qt6Network",
            "Qt6DBus",
            "KF6WindowSystem",
        ])
        .output()
        .expect("Installer pkg-config, Qt6Widgets, Qt6Network, Qt6DBus et KF6WindowSystem");
    assert!(
        flags.status.success(),
        "Qt6 ou KF6WindowSystem introuvables"
    );
    // Version et date sont incorporées à la boîte À propos sans lecture de fichier au lancement.
    let version = env::var("CARGO_PKG_VERSION").unwrap();
    let mut date_command = Command::new("date");
    if let Ok(epoch) = env::var("SOURCE_DATE_EPOCH") {
        date_command.arg(format!("--date=@{epoch}"));
    }
    let date = date_command
        .args(["-u", "+%Y-%m-%d"])
        .output()
        .expect("Date de compilation indisponible");
    assert!(date.status.success(), "Date de compilation indisponible");
    let date = String::from_utf8(date.stdout).unwrap();
    // Compiler l’interface en PIC permet de la lier à l’exécutable Rust avec les bibliothèques Qt.
    let cflags = String::from_utf8(flags.stdout).unwrap();
    let native_sources = [
        "window",
        "window_conflicts",
        "window_commands",
        "window_rows",
        "window_open",
        "window_errors",
        "application",
        "storage",
    ];
    for source in native_sources {
        let mut compiler = Command::new("c++");
        compiler.args(["-std=c++17", "-O2", "-fPIC", "-Wall", "-Wextra"]);
        compiler.arg(format!("-DGAD_VERSION=\"{version}\""));
        compiler.arg(format!("-DGAD_BUILD_DATE=\"{}\"", date.trim()));
        compiler.args(cflags.split_whitespace());
        assert!(compiler
            .arg("-c")
            .arg(format!("native/{source}.cpp"))
            .arg("-o")
            .arg(out.join(format!("{source}.o")))
            .status()
            .unwrap()
            .success());
    }
    let qt_paths = Command::new("qmake6")
        .args(["-query", "QT_INSTALL_LIBEXECS"])
        .output()
        .expect("qmake6 introuvable");
    assert!(
        qt_paths.status.success(),
        "Dossier des outils Qt introuvable"
    );
    // rcc produit le code de ressource à partir du manifeste ; les pixels ne dépendent pas d’un thème installé.
    let rcc = PathBuf::from(String::from_utf8(qt_paths.stdout.clone()).unwrap().trim()).join("rcc");
    let resource = out.join("gad_assets.cpp");
    assert!(Command::new(rcc)
        .args(["--name", "gad_assets"])
        .arg(&manifest)
        .arg("-o")
        .arg(&resource)
        .status()
        .unwrap()
        .success());
    let mut resource_compiler = Command::new("c++");
    resource_compiler.args(["-std=c++17", "-O2", "-fPIC"]);
    for flag in cflags.split_whitespace() {
        resource_compiler.arg(flag);
    }
    assert!(resource_compiler
        .arg("-c")
        .arg(&resource)
        .arg("-o")
        .arg(out.join("gad_assets.o"))
        .status()
        .unwrap()
        .success());
    // moc fournit les métadonnées nécessaires à la livraison des signaux D-Bus aux slots Qt.
    let moc = PathBuf::from(String::from_utf8(qt_paths.stdout.clone()).unwrap().trim()).join("moc");
    let moc_source = out.join("moc_notifications.cpp");
    assert!(Command::new(moc)
        .arg(std::fs::canonicalize("native/notifications.h").unwrap())
        .arg("-o")
        .arg(&moc_source)
        .status()
        .unwrap()
        .success());
    let mut moc_compiler = Command::new("c++");
    moc_compiler.args(["-std=c++17", "-O2", "-fPIC"]);
    for flag in cflags.split_whitespace() {
        moc_compiler.arg(flag);
    }
    assert!(moc_compiler
        .arg("-c")
        .arg(&moc_source)
        .arg("-o")
        .arg(out.join("notifications_moc.o"))
        .status()
        .unwrap()
        .success());
    // L’archive rassemble les unités de la fenêtre et les objets générés par Qt.
    assert!(Command::new("ar")
        .arg("crs")
        .arg(out.join("libwindow.a"))
        .args(
            native_sources
                .iter()
                .map(|source| out.join(format!("{source}.o")))
        )
        .arg(out.join("gad_assets.o"))
        .arg(out.join("notifications_moc.o"))
        .status()
        .unwrap()
        .success());
    println!("cargo:rustc-link-search=native={}", out.display());
    println!("cargo:rustc-link-lib=static=window");
    for lib in [
        "Qt6Widgets",
        "Qt6Gui",
        "Qt6Network",
        "Qt6DBus",
        "KF6WindowSystem",
        "Qt6Core",
        "stdc++",
    ] {
        println!("cargo:rustc-link-lib={lib}");
    }
}

/// Génère les enums Rust depuis le manifeste également inclus par le préprocesseur C++.
fn generate_protocol(out: &std::path::Path) {
    let definition = std::fs::read_to_string("native/protocol.def").unwrap();
    let mut output = String::from("// Généré : modifier native/protocol.def.\n");
    for (prefix, name) in [("GAD_STATE(", "TransferState"), ("GAD_COMMAND(", "Command")] {
        let values: Vec<Vec<&str>> = definition
            .lines()
            .filter_map(|line| line.strip_prefix(prefix))
            .map(|line| {
                line.trim_end_matches(')')
                    .split(',')
                    .map(str::trim)
                    .collect()
            })
            .collect();
        output += &format!("#[derive(Clone, Copy, Debug, PartialEq, Eq, serde::Serialize, serde::Deserialize)]\npub enum {name} {{\n");
        for value in &values {
            output += &format!("#[serde(rename = \"{}\")] {},\n", value[1], value[0]);
        }
        output +=
            &format!("}}\nimpl {name} {{\npub fn as_str(self) -> &'static str {{ match self {{\n");
        for value in &values {
            output += &format!("Self::{} => \"{}\",\n", value[0], value[1]);
        }
        output += "}}\npub fn from_wire(value: &str) -> Option<Self> { match value {\n";
        for value in &values {
            output += &format!("\"{}\" => Some(Self::{}),\n", value[1], value[0]);
        }
        output += "_ => None, } }\n";
        if name == "TransferState" {
            output += "pub fn is_terminal(self) -> bool { match self {\n";
            for value in &values {
                output += &format!("Self::{} => {},\n", value[0], value[2]);
            }
            output += "}}\n";
        }
        output += "}\n";
    }
    let version = definition
        .lines()
        .find_map(|line| line.strip_prefix("GAD_PROTOCOL_VERSION("))
        .unwrap()
        .trim_end_matches(')');
    output += &format!("pub const VERSION: u64 = {version};\n");
    std::fs::write(out.join("protocol.rs"), output).unwrap();
}
