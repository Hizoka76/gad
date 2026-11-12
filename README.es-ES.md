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


**0.4.0** — Elija destino y opciones y añada los archivos en Gad. Si no hay trabajo activo, revise la lista y pulse **Iniciar**. El pegado externo desde el gestor de archivos arranca automáticamente; los nuevos elementos durante una transferencia se añaden a la cola. **Ctrl+C** copia y **Ctrl+X** mueve. Arrastrar y soltar siempre prepara una copia.

## Documentación {#documentation}

Este README es un punto de partida. Las guías describen el comportamiento completo y las limitaciones, por lo que no es necesario mantener instrucciones detalladas dos veces.

| Audiencia | Inglés (Estados Unidos) | francés |
|---|---|---|
| Usuarios | [Guía del usuario ilustrada](docs/USER_GUIDE.es-ES.md) | [Guía ilustrada](docs/GUIDE_UTILISATEUR.md) |
| Desarrolladores | [Arquitectura y Protocolo](docs/DEVELOPER_GUIDE.es-ES.md) | [Arquitectura y protocolo](docs/GUIDE_DEVELOPPEUR.md) |
| Pruebas | [Validación](TESTING.es-ES.md) | [Pruebas](TESTING.md) |

[Todas las ediciones HTML y Markdown](docs/README.es-ES.md)

## Instalación {#installation}

### PPA de Ubuntu

El [PPA de Gad](https://launchpad.net/~hizo/+archive/ubuntu/gad) permite instalar Gad y recibir actualizaciones mediante APT:

```sh
sudo add-apt-repository ppa:hizo/gad
sudo apt update
sudo apt install gad
```

Compruebe en la página del PPA que existe un paquete publicado para su versión de Ubuntu. Si falta `add-apt-repository`, instale `software-properties-common`.

### Descargas precompiladas

Las [versiones de GitHub](https://github.com/Hizoka76/gad/releases/latest) ofrecen varios formatos. Sustituya los nombres de ejemplo por los de los archivos descargados.

#### Paquete Debian `.deb`

Descargue el paquete de su arquitectura e instálelo con APT para resolver las dependencias disponibles en sus repositorios:

```sh
sudo apt install ./gad_VERSION_amd64.deb
```

#### Archivo `.tar.gz`: fuentes y binario compilado

El archivo `gad-VERSION-linux-amd64.tar.gz` contiene el código fuente, las dependencias Rust incluidas, la documentación y el programa compilado en `prebuilt/`. No necesita compilarlo para ejecutarlo:

```sh
tar -xzf gad-VERSION-linux-amd64.tar.gz
cd gad-VERSION
./prebuilt/usr/bin/gad
```

#### Ejecutable directo

El archivo `gad-linux-amd64` también es un binario ya compilado:

```sh
chmod +x gad-linux-amd64
./gad-linux-amd64
```

Las descargas Linux amd64 están destinadas a procesadores x86-64. El binario directo no instala un lanzador ni el complemento de Dolphin; el `.deb` y el archivo incluyen los archivos de integración de esa versión. Estos binarios requieren Qt/KF6 con las versiones indicadas en las notas de publicación. Una versión publicada corresponde a su etiqueta y puede ser anterior al código actual del repositorio.

### Desde el código fuente

En Linux, instale Rust/Cargo, un compilador de C++17, `pkg-config`, Qt 6 Widgets/Network/DBus y herramientas de compilación Qt, KF6 WindowSystem/XmlGui, CMake y Python 3. La compatibilidad con el portapapeles Wayland utiliza `wl-clipboard`; Las URL remotas requieren el fusible KIO. Consulte el [guía para desarrolladores](docs/DEVELOPER_GUIDE.es-ES.md#building).

```bash
python3 scripts/install.py
gad
```

La instalación es por usuario y no requiere privilegios de administrador. Utilice `gad --background` para iniciar en la bandeja del sistema.

## Copiando desde Dolphin {#copying-from-dolphin}

1. Copie con **Ctrl+C** o corte con **Ctrl+X** para mover archivos.
2. Haga clic derecho en la carpeta de destino y seleccione **Pegado con Gad** (Pegar con Gad).
3. Repita para agregar otra solicitud a la cola.

La integración opcional de **Ctrl+V con Gad en Dolphin** se activa con `gad --configure --dolphin-ctrl-v` y se desactiva con `gad --configure --no-dolphin-ctrl-v`. Cierre y vuelva a abrir Dolphin para aplicar el cambio. En Gad, Ctrl+V pega en el destino mostrado; arrastrar y soltar copia los archivos.

Para proteger el lanzador habitual de Dolphin frente a la pérdida del atajo, instale el módulo con el comando siguiente y cierre y vuelva a abrir las ventanas existentes una vez. Comprueba los atajos cada segundo, repara la acción de Gad y conserva el pegado nativo en los campos de texto.

```sh
python3 scripts/dolphin_integration.py --install --protect-launcher "$HOME/.local/bin/gad"
```


Para otros administradores de archivos: [pasta en Gad, arrastrar y soltar, y acciones personalizadas](docs/USER_GUIDE.es-ES.md#using-gad-with-other-file-managers).

Deshacer depende del diario retenido y de las copias de seguridad de Gad. **Borrar operaciones completadas** (Borrar operaciones finalizadas) también elimina su historial de deshacer. Lea el [guía de usuario](docs/USER_GUIDE.es-ES.md) antes de borrar el historial que pueda necesitar.

## Desarrollo y validación {#development-and-validation}

```bash
make doctor
make test
make docs
```

Las pruebas utilizan archivos temporales y sesiones aisladas. No controlan su instancia de escritorio de Gad. [Comandos y requisitos de prueba](TESTING.es-ES.md).
