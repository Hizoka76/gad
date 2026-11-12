"""Encoder un chemin littéral pour Exec, sans interprétation de ses caractères spéciaux."""


def executable(path):
    """Séparer l’échappement d’argument de celui de la valeur Desktop Entry."""
    value = str(path)
    if '\0' in value:
        raise ValueError('A Desktop executable cannot contain NUL.')
    for character in ('\\', '"', '`', '$'):
        value = value.replace(character, '\\' + character)
    # Les codes de champ sont développés même dans les arguments entre guillemets.
    value = value.replace('%', '%%')
    value = value.replace('\\', '\\\\').replace('\n', '\\n').replace('\r', '\\r').replace('\t', '\\t')
    return '"' + value + '"'
