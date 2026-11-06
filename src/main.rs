//! Point d’entrée et frontière C entre le moteur Rust et l’interface Qt.
//! Les requêtes/réponses sont des objets JSON ; Rust conserve la propriété des chaînes
//! allouées et fournit une fonction dédiée pour les libérer depuis C++.

mod engine;

use std::ffi::{c_char, c_int, CStr, CString};
use std::sync::OnceLock;

static ENGINE: OnceLock<Result<engine::Engine, String>> = OnceLock::new();

/// # Safety
/// `input` must point to a valid NUL-terminated UTF-8 string for the duration of the call.
/// Free the returned allocation once with `sc_free`.
#[no_mangle]
pub unsafe extern "C" fn sc_request(input: *const c_char) -> *mut c_char {
    let result = std::panic::catch_unwind(|| {
        let request = CStr::from_ptr(input).to_str().map_err(|e| e.to_string())?;
        ENGINE
            .get_or_init(engine::Engine::desktop)
            .as_ref()
            .map_err(Clone::clone)?
            .request(request)
    });
    let value = match result {
        Ok(Ok(value)) => value,
        Ok(Err(error)) => serde_json::json!({"error": error}),
        Err(_) => serde_json::json!({"error": "Internal engine error"}),
    };
    CString::new(value.to_string()).unwrap().into_raw()
}

/// # Safety
/// `value` must be null or a live allocation returned by `sc_request`, freed only once.
#[no_mangle]
pub unsafe extern "C" fn sc_free(value: *mut c_char) {
    if !value.is_null() {
        drop(CString::from_raw(value));
    }
}

unsafe extern "C" {
    /// Démarre la boucle Qt ; argc/argv restent valides pendant cet appel bloquant.
    fn sc_gui(argc: c_int, argv: *mut *mut c_char) -> c_int;
}

/// Conserve les arguments Unix octet pour octet et garde leurs CString vivantes
/// pendant toute l’exécution de Qt, puis transmet son code de sortie.
fn main() {
    let args: Vec<CString> = std::env::args_os()
        .map(|s| {
            use std::os::unix::ffi::OsStrExt;
            CString::new(s.as_os_str().as_bytes()).unwrap()
        })
        .collect();
    let mut pointers: Vec<*mut c_char> = args.iter().map(|s| s.as_ptr() as *mut c_char).collect();
    pointers.push(std::ptr::null_mut());
    std::process::exit(unsafe { sc_gui(args.len() as c_int, pointers.as_mut_ptr()) });
}
