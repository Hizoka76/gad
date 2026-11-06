# Gad {#gad}

[Français](README.md) · [English (US)](README.en-US.md) · [Español](README.es-ES.md) · [Português](README.pt-PT.md) · [Deutsch](README.de-DE.md) · [日本語](README.ja-JP.md)

**Gad, il copie.** — **Gestionnaire Avancé de Duplication** (Gestor avanzado de duplicación)

[francés](README.md) · [Inicio rápido](docs/QUICK_START.es-ES.md)

Gad trae copias de archivos y los mueve a una ventana nativa KDE Plasma. Su motor de transferencia está escrito en Rust, con una interfaz de 6 widgets en C++/Qt.

- Interfaces en francés, inglés estadounidense, español, portugués, alemán y japonés con una preferencia de idioma guardada.
- Una cola detallada con árboles de carpetas, prioridades y filas que se pueden arrastrar.
- Hasta dos transferencias compatibles en paralelo, cambios de nombre rápidos y supresión automática de duplicados.
- Controles de pausa, límites de ancho de banda, estimaciones por solicitud y seguimiento del progreso Plasma.
- Conflictos legibles, comprobaciones de MD5/propiedad/espacio libre e historial con deshacer.
- Recuperación parcial de archivos, espera de reconexión del dispositivo y reintentos limitados a archivos fallidos.
- Fuentes y destinos locales y remotos a través de KIO Fuse: KDE Connect, FTP, SFTP y SMB.

## Documentación {#documentation}

Este README es un punto de partida. Las guías describen el comportamiento completo y las limitaciones, por lo que no es necesario mantener instrucciones detalladas dos veces.

| Audiencia | Inglés (Estados Unidos) | francés |
|---|---|---|
| Usuarios | [Guía del usuario ilustrada](docs/USER_GUIDE.es-ES.md) | [Guía ilustrada](docs/GUIDE_UTILISATEUR.md) |
| Desarrolladores | [Arquitectura y Protocolo](docs/DEVELOPER_GUIDE.es-ES.md) | [Arquitectura y protocolo](docs/GUIDE_DEVELOPPEUR.md) |
| Pruebas | [Validación](TESTING.es-ES.md) | [Pruebas](TESTING.md) |

[Todas las ediciones HTML y PDF](docs/README.es-ES.md)

## Instalación {#installation}

En Linux, instale Rust/Cargo, un compilador de C++17, `pkg-config`, Qt 6 Widgets/Network/DBus y herramientas de compilación Qt, KF6 WindowSystem y Python 3. La compatibilidad con el portapapeles Wayland utiliza `wl-clipboard`; Las URL remotas requieren el fusible KIO. Consulte el [guía para desarrolladores](docs/DEVELOPER_GUIDE.es-ES.md#building).

```bash
python3 scripts/install.py
gad
```

La instalación es por usuario y no requiere privilegios de administrador. Utilice `gad --background` para iniciar en la bandeja del sistema.

## Copiando desde Dolphin {#copying-from-dolphin}

1. Copie con **Ctrl+C** o corte con **Ctrl+X** para mover archivos.
2. Haga clic derecho en la carpeta de destino y seleccione **Pegado con Gad** (Pegar con Gad).
3. Repita para agregar otra solicitud a la cola.

El **Integración Ctrl+V en Dolphin** opcional se habilita con `gad-dolphin-paste --enable` y se deshabilita con `gad-dolphin-paste --disable`. Cierre y vuelva a abrir Dolphin después de cambiarlo. En Gad, Ctrl+V pega en el destino mostrado; Arrastrar y soltar copias de archivos.

Para otros administradores de archivos: [pasta en Gad, arrastrar y soltar, y acciones personalizadas](docs/USER_GUIDE.es-ES.md#using-gad-with-other-file-managers).

Deshacer depende del diario retenido y de las copias de seguridad de Gad. **Borrar operaciones completadas** (Borrar operaciones finalizadas) también elimina su historial de deshacer. Lea el [guía de usuario](docs/USER_GUIDE.es-ES.md) antes de borrar el historial que pueda necesitar.

## Desarrollo y validación {#development-and-validation}

```bash
make doctor
make test
make docs
```

Las pruebas utilizan archivos temporales y sesiones aisladas. No controlan su instancia de escritorio de Gad. [Comandos y requisitos de prueba](TESTING.es-ES.md).
