# Prueba Gad {#testing-gad}

[francés](TESTING.md) · [Guía para desarrolladores](docs/DEVELOPER_GUIDE.es-ES.md) · [Documentación](docs/README.es-ES.md)

## Validación en ejecución {#running-validation}

Desde la raíz del repositorio:

```bash
make doctor           # Check tools, native libraries, and Rust components
make test             # Formatting, Clippy, Rust, Python, Qt, and integration
```

Sin marca:

```bash
python3 scripts/run_tests.py all
python3 scripts/run_tests.py all --keep-going
python3 scripts/run_tests.py all --list
```

El corredor devuelve cero solo cuando pasan todas las etapas requeridas. Normalmente se detiene ante el primer fallo. `--keep-going` también ejecuta etapas posteriores independientes; La integración sigue dependiendo de una construcción exitosa.

## Grupos de prueba {#test-groups}

| Dominio | Cobertura |
|---|---|
| `make test-unit` | Pruebas unitarias Rust y Python. |
| `make test-python` | Rutas XDG, integración Dolphin, ejecutor y lógica de generación de documentación. |
| `make test-qt` | Portapapeles, widgets, conflictos, menús, informes de errores y notificaciones. |
| `make test-integration` | Cree y pruebe el protocolo local y ejecutable real. |
| `make check` | Formateo y Clippy con advertencias tratadas como errores. |
| `make test-bench` | Punto de referencia de actualización explícito de gran historial. |
| `make docs-check` | Enlaces de documentación existente, capturas y exportaciones bilingües. |

Los nombres de los grupos de ejecutores son `unit`, `python`, `qt`, `integration`, `lint`, `bench` y `all`.

```bash
python3 scripts/run_tests.py unit --filter rollback_
python3 scripts/test_qt.py --suite row_actions
python3 scripts/test_qt.py --suite clipboard --suite notifications
cargo test --locked engine::tests::copies_tree_and_symlink_without_following_it
```

El filtro de corredor se aplica a las pruebas Rust; Las pruebas de Python aún se ejecutan. El punto de referencia se ignora durante la validación normal. Los scripts de contenedor individuales siguen estando disponibles.

## Informes {#reports}

Cada invocación de corredor escribe lo siguiente en `target/test-results/`:

- Un archivo `.log` para cada etapa ejecutada.
- `results.json`, incluidos argumentos de comando, códigos de salida, estado y tiempo transcurrido.
- `junit.xml`, con **una entrada por etapa de comando**, no uno por prueba individual Rust/Qt. Los casos y aserciones a nivel de marco permanecen en los registros de etapa.

```bash
python3 scripts/run_tests.py all --report-dir /tmp/gad-results
```

JSON y JUnit describen la última ejecución e identifican sus registros. Es posible que aún existan registros de etapas anteriores omitidas. `target/` está excluido de los archivos fuente por `.gitignore`.

## Requisitos {#requirements}

- Linux y Rust/Cargo reciente, con Rust 1.85 o posterior, Rustfmt y Clippy.
- Un compilador de C++17, `pkg-config`, Qt 6 widgets/red/DBus/prueba.
- `qmake6`, `moc`, `rcc`, KF6 WindowSystem y `dbus-run-session`.
- Python 3. Las pruebas de script de rutina utilizan solo su biblioteca estándar.

El entorno CI Debian Trixie suministrado instala:

```bash
sudo apt install build-essential pkg-config python3 cargo rustc rustfmt rust-clippy \
  qt6-base-dev qt6-base-dev-tools qt6-qpa-plugins qmake6 libkf6windowsystem-dev dbus fonts-dejavu-core
```

Utilice los paquetes correspondientes en otras distribuciones. `make doctor` identifica herramientas faltantes. Exportar documentación por separado requiere Markdown, Graphviz; estos no son necesarios para las siete pruebas unitarias de documentación estándar. Consulte el [índice de documentación](docs/README.es-ES.md#regeneration).

## Aislamiento {#isolation}

Las pruebas del sistema de archivos utilizan árboles temporales. Qt se ejecuta fuera de la pantalla con HOME/XDG aislado y una sesión privada de D-Bus. Las pruebas no instalan Gad, no modifican el portapapeles real del usuario, no reinician la instancia de escritorio ni operan con archivos de transferencia personales.

Las aperturas de escritorio se interceptan mediante `QDesktopServices`; No se inicia ningún editor o administrador de archivos real. Las notificaciones KIO y KDE utilizan servicios falsos. La integración controla sólo su propio ejecutable aislado. Los servidores FTP/SFTP/SMB reales no son necesarios para la suite normal.

La eliminación del dispositivo se simula cambiando el nombre de los directorios de anclaje temporales, nunca desconectando o desmontando un disco real. Las tablas de montaje simuladas prueban la lógica de montaje vinculado sin privilegios de montaje.

## Agregar una prueba de regresión {#adding-a-regression-test}

| Área | Ubicación y enfoque |
|---|---|
| Motor Rust | Agregue casos `#[test]` en `src/engine/tests.rs`; use `Fixture`, espere los estados comerciales e inspeccione el contenido del archivo, así como JSON. Termine el trabajo y apague los motores nuevos cuando sea posible. |
| Fila/menú/acciones abiertas | Agregue ranuras o filas de datos en `tests/row_actions.cpp`; Se abre simular el motor e interceptar el escritorio. |
| Comportamiento del portapapeles y de la ventana | Ampliar `tests/clipboard.cpp`. |
| Progreso del bus D | Amplíe `tests/notifications.cpp` con el servidor JobView privado falso. |
| secuencias de comandos de Python | Agregue `tests/python/test_*.py` usando `unittest`, `TemporaryDirectory` y simulaciones de entorno/subproceso. El descubrimiento es automático. |
| Nueva suite Qt | Agregue su nombre a `SUITES` en `scripts/qt_tests.py`, su fuente C++ y el moc generado correspondiente. |

Pruebe el comportamiento observable: estados resultantes, bytes correctos, originales retenidos, destinos restaurados, argumentos de solicitud de escritorio y ausencia de cambios no deseados. Prefiera un predicado de señal/estado a retrasos arbitrarios. No realice la prueba controlando la aplicación de escritorio real.

## Integración continua {#continuous-integration}

`.github/workflows/tests.yml` proporciona una validación completa en un contenedor Debian Trixie y conserva informes sobre fallas. Se activa cuando el proyecto está alojado en un repositorio de GitHub adecuado. Se suministra la configuración; Aquí no se afirma ninguna ejecución remota de CI.

## Movimientos locales rápidos y monturas vinculantes. {#fast-local-moves-and-bind-mounts}

```bash
python3 scripts/run_tests.py unit --rust-filter fast_
```

Los casos `fast_move` inspeccionan los inodos después de la transferencia y deshacer, la ejecución junto con la copia, rutas independientes que comparten una carpeta de destino, reservas de nombres después de conflictos, rutas superpuestas, respaldo de MD5, objetivos simultáneos, reprogramación EXDEV, cancelación, cadenas de reemplazo y recuperación de diario. La modificación de la misma longitud con mtime restaurado aún debe hacer que deshacer rechace un resultado alterado.

Los dispositivos rápidos utilizan tmpfs de Linux grabables en `/dev/shm`, por lo que un `/tmp` con respaldo superpuesto en CI no deshabilita accidentalmente la ruta de cambio de nombre local. El comportamiento de cambio de nombre inyectado simula EXDEV sin mover archivos a otro volumen.

`fast_bind_mounts` utiliza tablas de montaje sintéticas para distintas vistas de enlace y su vista subyacente común. Las pruebas cubren la traducción en ambos sentidos, faltando exposición común, diferentes sistemas de archivos, restricciones de solo lectura y sufijos de destino que aún no existen.

`fast_tree` agrega preservación de permisos/inodos de raíz completa, carpetas vacías, raíces múltiples, pausas específicas, deshacer parcial, archivos externos agregados más tarde, contenidos modificados y recuperación antes/después de la publicación raíz. Los montajes anidados, incluidos los ocultos en una vista común, impiden el acceso directo.

## Recuperación parcial tras la interrupción {#partial-recovery-after-interruption}

```bash
cargo test --locked resume_
python3 scripts/test_resume_integration.py
```

Compile el ejecutable antes de ejecutar un script de integración independiente. Las pruebas cubren ordenación de puntos duraderos, validación de versión/propiedad temporal y de origen, comparación de prefijos, reconstrucción MD5, truncamiento de bytes finales solo después de la validación e identidad temporal preservada. El contenido o los metadatos alterados deben fallar sin truncarse un archivo cuestionable.

La integración binaria real utiliza un proceso aislado, SIGKILL, reinicio y comparación de bytes/digest para verificar que se reanude el mismo archivo parcial. Nunca mata la instancia Gad del usuario. Los trabajos cancelados expresamente no deben convertirse en trabajos automáticos recuperables.

## Destinos remotos {#remote-destinations}

```bash
make test-remote
```

Esta prueba opcional de red real requiere `pyftpdlib`, fusible KIO y `/dev/fuse` utilizable. En su lugar, la validación normal utiliza servicios KIO falsos. El servidor y los archivos opcionales permanecen en un entorno temporal; no es una prueba contra una cuenta remota personal.

Verifique la validación de URL, las asignaciones de puntos finales, las opciones de operación/origen, los metadatos de servicios no compatibles, la semántica de sincronización y la recuperación de destino KIO duradera deshabilitada. Las pruebas no deben tratar las propiedades o capacidades no disponibles como garantías exitosas inventadas.

## Dolphin Ctrl+V y progreso del escritorio {#dolphin-ctrlv-and-desktop-progress}

```bash
make test-dolphin-paste
```

La prueba de integración opcional requiere archivos de desarrollo CMake, Qt 6 Widgets/Test y KF6 XmlGui. Utiliza KXMLGUI/KActionCollection real en entornos aislados de HOME/XDG, D-Bus y portapapeles fuera de la pantalla. Las acciones son contadores: no se produce ninguna copia real ni reinicio del usuario Dolphin.

Verifica Gad Ctrl+V, preservación de la alternativa nativa Shift+Insert KIO, pegado de campo de texto nativo, configuración Ctrl+V restaurada después de deshabilitar, validación completa antes de modificar la configuración, rechazo de recibos no válidos y reversión después de fallas de escritura.

Las pruebas del portapapeles inyectan respuestas de sesión Dolphin para ventanas activas únicas, alias de servicio, espacios y `#`, títulos cortos, carpetas cerradas, cambios de navegación/enfoque, servicios no disponibles y URL remotas. Las pruebas de Python cubren la migración/restauración de la integración sin alterar acciones no relacionadas.

Las pruebas de notificación ejercitan comandos que regresan a través del controlador orientado al motor, etiquetas de pausa sincronizadas/estado de bandeja, una única vista de progreso combinada, velocidad en detalles, contabilidad de finalización de MD5, reconexión del servicio y la transición de `suspended` de nuevo a falso. Las notificaciones ordinarias no deben duplicar la vista nativa del trabajo.

## Comprobaciones de espacio libre, prioridades y árboles {#free-space-checks-priorities-and-trees}

Las pruebas de espacio cubren presupuestos compartidos por volumen, volúmenes separados, redondeo de bloques, desbordamiento, archivos parciales asignados, persistencia de opciones y cambios de nombre rápidos. Un dispositivo tmpfs escaso crea una escasez sin llenar el disco. La capacidad remota simulada desconocida advierte y completa. Qt comprueba las opciones capturadas y una advertencia permanece visible después de un resultado correcto.

Las pruebas de prioridad utilizan el programador real para un orden estable de múltiples solicitudes, trabajo en pausa, rechazo atómico de selección obsoleta y deshacer correctamente después de reemplazos reordenados. La integración de IPC reordena y restaura las solicitudes en cola antes de continuar con las copias.

Las pruebas de widgets verifican el bosque sin encabezado, los acentos de solicitudes compartidas en temas claros/oscuros, la selección de solicitudes completas, las referencias de arrastre internas estables, el rechazo de arrastre externo, la paternidad/destinos preservados y la estabilidad de selección/desplazamiento/expansión. Las pruebas del motor cubren raíces independientes alternas, orden de hermanos expuesto por conflictos, reparentalización no válida, prioridades de marcadores de posición y dependencias de reemplazo durante la pausa/finalización.

Los objetos nativos Qt se almacenan en caché de contenido en `target/qt-test-cache` con fuente/encabezado, versión Qt/KDE y hashes de identidad del compilador. Un bloqueo de compilación impide la construcción simultánea de caché. Los ejecutables de prueba, los archivos, la configuración y las sesiones D-Bus permanecen actualizados y aislados.

Las dependencias de destino normalizadas incluyen alias principales, nombres repetidos, pausas selectivas y futuras reservas de nombres de copia. Las advertencias de propiedad de carpeta resumen solo sus propios descendientes reales.

## Supresión de transferencias duplicadas {#duplicate-transfer-suppression}

Los casos `duplicates_` cubren pegados repetidos en pausa, preparación simultánea, duplicados dentro de un envío, carpetas superpuestas, alias principales e índices estables restaurados. Conservan destinos distintos, intenciones de copiar/mover, entradas de directorio de enlaces físicos con nombres diferentes y nuevas solicitudes intencionales después de la finalización/cancelación.

Qt verifica un único mensaje informativo de las actualizaciones de solo metadatos sin solicitudes repetidas. Los escenarios de conflicto utilizan fuentes genuinamente distintas que comparten un nombre de archivo, por lo que prueban los conflictos en lugar de duplicar la supresión.

## Límites y estimaciones de ancho de banda {#bandwidth-limits-and-estimates}

`engine::rate` utiliza un reloj inyectado para verificar presupuestos compartidos/globales y por solicitud, crédito inactivo limitado, consumo agregado y cambios en vivo. Las pruebas del motor realizan una copia real limitada, eliminan el límite y verifican la salida. Se cubren la pausa, la cancelación de baja velocidad, la validación atómica, la persistencia y la preservación de un límite más nuevo durante la preparación del reintento.

Qt comprueba la conversión de MiB a bytes y un destino por solicitud de propiedad incluso cuando se seleccionan varias de sus filas. Cambiar la selección mientras el cuadro de diálogo de tasa está abierto no debe cambiar sus ID capturados.

Las pruebas de métricas deterministas cubren tasas/tiempos de solicitudes simultáneas independientes, fases de copia versus MD5, pausas/conflictos, cambios de nombre, prefijos reanudados, restablecimientos de contadores, progreso estancado y formato de duración. El widget de resumen real se actualiza en `poll.unchanged` sin reemplazar filas o su selección/información sobre herramientas.

## Reconexión del dispositivo {#device-reconnection}

Los casos `reconnect_` simulan la desaparición de origen/destino cambiando el nombre de los anclajes temporales. Cubren recuperación validada de prefijo duradero, pausas retenidas, no escrituras en un directorio de reemplazo vacío, rechazo de fuente modificada, cancelación y liberación de trabajadores para trabajo independiente.

Los casos adicionales incluyen reiniciar un archivo propio no publicado sin un punto duradero, rechazar la reproducción de la publicación, cambios de número de dispositivo autorizados por UUID, resúmenes de metadatos/contenido conservados durante la nueva vinculación y comandos simultáneos de pausa/cancelación durante la preparación. No se monta ni desmonta ningún sistema de archivos real.

Qt verifica las etiquetas de fila y progreso, la disponibilidad de cancelación, una vista de trabajo nativa suspendida y la resolución asincrónica de KIO que no agrega trabajo ni abre la ventana.

## Informes de errores y reintentos {#error-reports-and-retries}

Las pruebas `errors_` cubren informes de solo lectura, fallas de preparación/finalización, validación atómica, índices obsoletos, referencias exactas de informes, preparación de reintentos reparada y protección de salida publicada modificada. Una solicitud real parcialmente fallida verifica que el contenido, el inodo, las marcas de tiempo y el diario del vecino exitoso permanezcan sin cambios después del reintento.

Se conservan la pausa global, las entradas canceladas/omitidas y el trabajo pendiente no seleccionado. La agregación de directorios evita registros de errores duplicados y al mismo tiempo conserva los errores de carpetas reales.

Qt verifica el filtrado y los objetivos de solicitud estables después de la clasificación, las acciones de reintento contextual, los informes vacíos, el texto UTF-8 completo, el JSON analizable y la escritura de informes atómicos. Las exportaciones utilizan únicamente rutas temporales. Cada botón de diálogo de error se marca con un tema de íconos deliberadamente no disponible para ejercitar los íconos de respaldo estándar Qt.

## Generación de la documentación {#documentation-generation}

```sh
make docs-captures
make docs
make docs-check
```

Las pruebas de documentación comprueban el catálogo de seis idiomas y los tres tipos de guía, la navegación entre idiomas, los formatos de papel, los pies de figura y las imágenes integradas. También verifican el formato compacto del inicio rápido, las capturas propias de cada idioma y la conservación del código de los ejemplos.

La generación produce diagramas traducidos y 18 guías. Después comprueba los enlaces y anclas, las 84 capturas con tema claro y sus dimensiones y huellas, el idioma del HTML, las imágenes integradas y las 36 exportaciones HTML/Markdown. Requiere las dependencias opcionales indicadas en el índice de documentación.

[Documentation](docs/README.es-ES.md#regeneration)

## Traducciones de aplicaciones {#application-translations}

Cinco pruebas de Python verifican la cobertura de cadenas Qt marcadas, huellas digitales TS/QM, exclusión de comentarios e identificadores no marcados, traducciones faltantes y preservación de parámetros y enlaces. Las pruebas Qt cargan el catálogo francés real incorporado y verifican widgets, estados, preferencias de idioma, informes de texto y rutas sin cambios/JSON. También se tratan las transiciones de exportación/filtro y actualización de informes vacíos. Ejecute `make translations-check` para una verificación independiente.

Las pruebas Qt en vivo alternan francés e inglés estadounidense durante una transferencia activa con una respuesta a la encuesta sin cambios. Verifican la identidad de las filas, la selección, la expansión, las entradas de destino/tasa, los cuadros de diálogo Acerca de/error/informe/tasa, las opciones de conflicto, los recuentos de enlaces estables y la ausencia de comandos de escritura del motor.

## Exclusiones y elementos ocultos {#exclusions-and-hidden-items}

Las pruebas Rust cubren patrones Unicode, validación atómica, valores predeterminados heredados, eliminación de carpetas, archivos temporales, enlaces simbólicos, persistencia de reglas por trabajo, reintentos, solicitudes totalmente excluidas y movimientos seguidos de deshacer. Las pruebas Qt cubren preferencias guardadas, ediciones canceladas y rechazadas, traducción en vivo y captura de reglas para nuevas solicitudes.

```sh
cargo test --locked exclusions
python3 scripts/test_qt.py --suite row_actions
```

## Integración Konqueror {#konqueror-integration}

`python3 scripts/test_qt.py --suite konqueror` verifica la carpeta actual, las URL codificadas y remotas, la preservación en modo de corte, el pegado de texto nativo y Shift+Insert en un entorno aislado. El complemento también se cargó en Konqueror 26.08.2 y se extrajo en un directorio temporal con HOME/XDG y D-Bus privados; Se verificó la URL exacta de la carpeta activa. Los registros locales están en `target/konqueror-real-test.log`.

## Diálogo de configuración {#settings-menu}

Las pruebas Qt verifican los tamaños mínimos, las cinco pestañas, Cerrar y Esc, la conservación de controles, la traducción inmediata y el botón de velocidad en la barra de acciones. La CLI se prueba con valores inválidos, opciones contradictorias, exclusiones parciales y sincronización de preferencias. Python comprueba los seis manuales, los seis idiomas, la instalación y el rechazo de traducciones ausentes.

```sh
make manpages-check
make test-cli-settings
```

## Vigilancia automática de Ctrl+V {#dolphin-watchdog}

Las pruebas Qt cubren la recreación de acciones, el temporizador real, la desactivación, la restauración del pegado nativo, los cambios del usuario, los diálogos modales, los registros inválidos y las acciones ausentes. El pegado de texto sigue siendo nativo. Python verifica la protección reversible del lanzador, sus argumentos y los cambios posteriores. La prueba con Dolphin real actualiza los menús y pulsa Ctrl+V: una orden ficticia registra el lanzamiento sin copiar archivos ni acceder al escritorio.

```sh
make test-dolphin-watchdog
make test-dolphin-watchdog-real
```


## Verificaciones recientes de la interfaz

`exclusionsTabCommitsOnFocusLossAndTranslates` y `exclusionsSaveWhenFocusLeavesTheField` verifican el guardado al perder el foco, el rechazo sin perder texto, campos independientes y cierre de opciones. `clipboardAdditionKeepsPreparedQueueWaiting` comprueba que añadir archivos del portapapeles no inicia trabajo preparado, pausado ni histórico. También se prueban Copiar/Cortar, pegado externo automático, Ctrl+O/Ctrl+Mayús+O/Ctrl+Q/Ctrl+Alt+W y las cinco pestañas.
