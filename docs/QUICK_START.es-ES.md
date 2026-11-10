# Gad — Inicio rápido {#gad-quick-start}

**Gad, il copie. · Gestor avanzado de duplicación · Versión 0.3.0**

[Guía del usuario](USER_GUIDE.es-ES.md) · [Guía para desarrolladores](DEVELOPER_GUIDE.es-ES.md) · [Todos los idiomas](README.es-ES.md)

## Instalar Gad

Utilice el [PPA de Gad](https://launchpad.net/~hizo/+archive/ubuntu/gad) para una versión de Ubuntu compatible. Las [versiones de GitHub](https://github.com/Hizoka76/gad/releases/latest) también ofrecen un `.deb`, un archivo `.tar.gz` con las fuentes y el programa compilado, y el binario directo `gad-linux-amd64`.

```sh
sudo add-apt-repository ppa:hizo/gad
sudo apt update
sudo apt install gad
```

Compruebe en la página del PPA que existe un paquete publicado para su versión de Ubuntu. Si falta `add-apt-repository`, instale `software-properties-common`.

Las órdenes, dependencias y diferencias entre formatos se explican en la [guía de usuario](USER_GUIDE.es-ES.md#installation-and-startup).

## Iniciar Gad {#start-gad}

Abra **Gad** desde el menú de su aplicación o ejecute:

```sh
gad
```

Si aún no está instalado, siga las instrucciones [instrucciones de instalación](USER_GUIDE.es-ES.md#installation-and-startup). Cerrar la ventana normalmente deja Gad en la bandeja del sistema cuando hay una bandeja disponible. Haga clic en su icono para mostrar la ventana nuevamente.

## Hacer la primera copia {#make-your-first-copy}

![Preparar transferencias en la lista principal](images/es-ES/22-preparation.png)

Elija destino y opciones y añada los archivos en Gad. Si no hay trabajo activo, revise la lista y pulse **Iniciar**. El pegado externo desde el gestor de archivos arranca automáticamente; los nuevos elementos durante una transferencia se añaden a la cola. **Ctrl+C** copia y **Ctrl+X** mueve. Arrastrar y soltar siempre prepara una copia.

## Seguir y controlar las transferencias {#read-and-control-transfers}

- **En espera**: esperando su turno. **En curso**: actualmente transfiriéndose. **DE ACUERDO**: terminado.
- Expanda una carpeta para ver sus archivos. Los colores de solicitud ayudan a identificar envíos separados; **Orden** muestra el orden de procesamiento.
- **pausar todo** pausa la cola. Reanúdalo con el mismo botón. Haga clic con el botón derecho en las filas para realizar acciones individuales de pausa, reanudación, reintento y cancelación.
- Seleccione las filas de espera y use **Proceso siguiente**, o arrástrelas a su posición mientras la tabla muestra el orden de la cola.
- La cancelación detiene la operación; **Deshacer y rehacer** también intenta deshacer el trabajo completado utilizando el historial retenido. Lea la confirmación y el resultado detallado antes de borrar el historial.

## Configurar las preferencias {#set-your-preferences}

![El diálogo de opciones se abre desde el bloque Configuración.](images/es-ES/02-sources-options.png)

*El diálogo de opciones se abre desde el bloque Configuración.*

En **Configuración → Opciones… → Exclusiones**, introduzca un patrón por línea para carpetas o archivos y enlaces simbólicos. Cada campo se guarda al perder el foco, sin botón Aplicar ni diálogo adicional. Si una regla es inválida, se indica el error, las reglas anteriores siguen activas y el texto permanece para corregirlo. Se aplica a nuevas transferencias. La opción de incluir elementos ocultos permanece en la pestaña de configuración.

**Permitir duplicación en la misma carpeta** crea nombres como `photo_copied1.jpg` al volver a copiar a la carpeta original. Está desactivado de forma predeterminada y se aplica sólo a las copias. Las opciones de selección, verificación y duplicación se capturan para nuevas solicitudes; Las solicitudes existentes mantienen su configuración. El límite de velocidad global puede cambiar durante los traslados.

## Pegar desde el gestor de archivos {#paste-directly-from-your-file-manager}

- **Dolphin:** habilite **Ctrl+V con Gad en Dolphin** en Configuración y reinicie Dolphin. También puede utilizar **Pegado con Gad** desde el menú contextual.
- **Konqueror:** inicie **Konqueror con Gad** y habilite su casilla de verificación en Configuración. Los archivos pegados van a Gad; Los pegados de texto siguen siendo nativos.
- **Otros directivos:** pegue dentro de Gad, arrastre archivos a él o configure una acción personalizada usando un destino explícito, por ejemplo `gad --paste "/home/demo/Documents/Copies"`. Su Ctrl+V nativo no se intercepta automáticamente.

Un pegado iniciado desde un administrador de archivos se puede ejecutar en segundo plano sin abrir Gad. Las notificaciones de progreso Plasma proporcionan un enlace **mostrar gad**.

## Resolver un conflicto o un error {#resolve-a-conflict-or-an-error}

![Comparación de conflictos de archivos](images/es-ES/08-conflict.png)

*Compare el origen y el destino existente antes de elegir una acción.*

Para un archivo existente, elija **Mantener el archivo existente**, **Mantenga ambos** o **Reemplazar archivo existente**. Aplique la elección a conflictos posteriores sólo cuando eso sea lo que pretende.

Abra **Resumen de errores...** para transferencias fallidas, copie o exporte los detalles y vuelva a intentar solo los archivos fallidos. Si un dispositivo desaparece, vuelva a conectar el mismo dispositivo en la misma ubicación y deje que Gad lo verifique antes de continuar.

**mantener la historia** conserva información de deshacer. **Borrar operaciones terminadas** elimina la información de restauración correspondiente. Para recuperación, direcciones remotas, comprobaciones de MD5 y todas las opciones de cancelación, continúe con [Guía del usuario](USER_GUIDE.es-ES.md).

## Configuración y páginas de manual {#command-settings}

`gad --configure` guarda las preferencias y actualiza los controles de la instancia abierta. Si no hay ninguna, inicia Gad en segundo plano y devuelve el control al terminal. Las formas `--no-…` desactivan las casillas. Las listas de exclusión no indicadas se conservan; repetir `--exclude-folder` o `--exclude-file` sustituye la lista correspondiente. `--clear-exclusions` vacía ambas listas. El límite global se expresa en MiB/s; 0 significa sin límite. `--md5` y `--properties` con una copia o movimiento afectan solo a ese traslado. Los atajos de Dolphin usan `gad --configure --dolphin-ctrl-v` o `--no-dolphin-ctrl-v`.

```bash
gad --configure --md5 --verify-space --no-include-hidden
gad --configure --exclude-folder "__*" --exclude-file "*.tmp" --language fr_FR --rate-limit 25
```

`man gad` · `make manpages`

## Vigilancia automática de Ctrl+V {#dolphin-watchdog}

Consulte la guía de usuario para proteger el lanzador habitual de Dolphin y cargar la vigilancia automática de Ctrl+V.

[Vigilancia automática de Ctrl+V](USER_GUIDE.es-ES.md#dolphin-watchdog)


## Perfiles de transferencia

En **Ajustes → Perfiles**, guarde todas las opciones con un nombre y selecciónelo para aplicarlas con un clic. **El destino nunca se guarda ni se modifica.** El idioma y el límite global tienen su efecto habitual. **Gestionar perfiles…** permite actualizar, renombrar y eliminar. Consulte los [perfiles de transferencia](USER_GUIDE.es-ES.md#transfer-profiles).

En la línea de órdenes, `gad --profile "Mi perfil"` aplica los ajustes sin iniciar una transferencia. Para copiar: `gad --profile "Mi perfil" --copy /ruta/origen --to /ruta/destino`; también funciona con `--move` / `--paste`. Gad espera a que se aplique el perfil. El destino procede de la orden y las opciones explícitas, como `--md5`, prevalecen. Un nombre desconocido devuelve un error sin encolar archivos. `--configure --profile "Mi perfil"` admite ajustes adicionales, aplicados después del perfil.


## Preparar transferencias en la lista principal

Elija destino y opciones y añada los archivos en Gad. Si no hay trabajo activo, revise la lista y pulse **Iniciar**. El pegado externo desde el gestor de archivos arranca automáticamente; los nuevos elementos durante una transferencia se añaden a la cola. **Ctrl+C** copia y **Ctrl+X** mueve. Arrastrar y soltar siempre prepara una copia.

## Opciones

**Ajustes → Opciones…** abre una ventana modal que bloquea la ventana principal mientras las transferencias continúan. Los cambios se aplican inmediatamente; **Cerrar** o Esc no los deshace. Los parámetros de línea de órdenes aparecen en gris. Tamaño mínimo: **800 × 740**; ventana principal, informe de errores y diagnóstico: **1100 × 600**.

Los cuatro apartados son **Ajustes**, **Atajos**, **Límites de velocidad** y **Fin del lote**. Los límites incluyen el global y el predeterminado para nuevas solicitudes. Para modificar una existente, seleccione sus filas y utilice **clic derecho → Limitar la velocidad de la solicitud…**. El límite aparece en **Detalle**, también en carpetas; sin límite no se añade texto.

## Añadir elementos, atajos y exclusiones

El menú **Añadir elementos** incluye **Añadir archivos…** (Ctrl+Mayús+O), **Añadir carpeta…** (Ctrl+O) y **Archivos del portapapeles** (Ctrl+V). Conserva el modo Copiar o Cortar. Si Gad está inactivo, los elementos añadidos dentro de Gad esperan a **Iniciar**; si hay transferencias iniciadas, se incorporan a la cola. Solo el pegado externo desde el gestor de archivos inicia automáticamente.

**Ctrl+F** abre la búsqueda. **Ctrl+Q** solicita salir y lo impide si hay transferencias activas. **Ctrl+Alt+W** oculta la ventana sin detenerlas. El botón de bandera junto a Perfiles cambia el idioma inmediatamente. Los botones principales muestran información emergente.

Hay cinco pestañas: configuración, atajos, límites de velocidad, fin del lote y exclusiones. Los parámetros de línea de comandos aparecen en gris. El límite de un lote existente se cambia en el menú contextual de sus filas y aparece en la columna de detalle.
