//! Types issus du vocabulaire unique Rust/Qt ; le JSON historique conserve ses chaînes.
include!(concat!(env!("OUT_DIR"), "/protocol.rs"));

/// Les anciennes fixtures de tests peuvent encore nommer les états par chaîne ; la production utilise les variantes.
#[cfg(test)]
impl From<&str> for TransferState {
    fn from(value: &str) -> Self {
        Self::from_wire(value).expect("Internal state absent from manifest")
    }
}
impl std::ops::Deref for TransferState {
    type Target = str;
    fn deref(&self) -> &str {
        self.as_str()
    }
}
impl PartialEq<&str> for TransferState {
    fn eq(&self, other: &&str) -> bool {
        self.as_str() == *other
    }
}
