<h1>SnapOrca</h1>

[![Build all](https://github.com/Jokiblack/SnapOrca/actions/workflows/build_all.yml/badge.svg?branch=main)](https://github.com/Jokiblack/SnapOrca/actions/workflows/build_all.yml)

**[English](#english) · [Español](#español)**

> SnapOrca is an **unofficial**, community build. It is not affiliated with or endorsed by Snapmaker, the OrcaSlicer team, Bambu Lab or Prusa Research.
>
> SnapOrca es una versión **no oficial** hecha por la comunidad. No está afiliada ni respaldada por Snapmaker, el equipo de OrcaSlicer, Bambu Lab ni Prusa Research.

---

## English

SnapOrca is **Snapmaker Orca 2.4.1** (the official slicer for the Snapmaker U1) merged with **OrcaSlicer 2.5.0-alpha**. It adds improvements ported from **Bambu Studio** and the **multi-nozzle printing** work of the **LixNix** fork. It is built and tested mainly for the **Snapmaker U1** (4-toolhead toolchanger).

### Download

📥 **[Latest release](https://github.com/Jokiblack/SnapOrca/releases/latest)**: Windows installer and portable.

- The installer is called **SnapOrca** and installs to `Program Files\SnapOrca`, **next to** the official Snapmaker Orca, without replacing it.
- Settings are stored in `%APPDATA%\SnapOrca`. The first time it starts, SnapOrca copies your user presets from the official Snapmaker Orca.
- With the U1 default profiles (4 × 0.4 mm nozzles), the G-code is the same as without the multi-nozzle changes. This was checked by slicing 22 test jobs. The only difference is the final `SET_PRESSURE_ADVANCE ADVANCE=0`, which is no longer emitted (see Snapmaker#714).

### Credits: where the features come from

| Source | What SnapOrca takes from it |
|---|---|
| [Snapmaker Orca](https://github.com/Snapmaker/OrcaSlicer) by **Snapmaker** | The base: Snapmaker U1 support and profiles, flow variants (Standard / High Flow), Full Spectrum mixed filament, the block-based wipe tower for the U1, device page, Moonraker / MQTT connection. |
| [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer) by **SoftFever** and the OrcaSlicer contributors | The 2.5.0-alpha engine and interface: printer agents and plugins, new slicing options, fixes and performance work. |
| [Bambu Studio](https://github.com/bambulab/BambuStudio) by **Bambu Lab** | Separate pattern for the layer under the top surface; "Global monotonic line" pattern; separate first-layer infill line width; periodic modifier and "modifier ignores infill"; Archimedean-spiral ironing; 13 organic / hybrid tree-support fixes and a warning for uncovered overhangs; 3MF and cut-tool hardening. |
| [LixNix fork of Snapmaker Orca](https://github.com/LixNix-Swap-Org/Snapmaker-Orca) by **LixNix** | **Different nozzle sizes on the U1**: per-nozzle diameter and preferred layer height, per-extruder layer height and its combining engine, fractional support layers and wipe-tower slabs, per-toolhead line widths, support material per nozzle, "prime tower counts every filament that prints"; crash fixes (right-click on the plate, object list, printer switch); setup wizard with Snapmaker first; U1 line widths as % of the nozzle; macOS hidden-web-view fix. |
| [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer) / [Slic3r](https://github.com/Slic3r/Slic3r) | The common origin of all the slicers above. |

The merge, the fixes and the tests were done with the help of **Claude (Anthropic)**. Commits contain a `Co-Authored-By` line.

### What SnapOrca adds over the official Snapmaker Orca 2.4.1

**Engine and interface**
- OrcaSlicer 2.5.0-alpha engine and interface, on top of Snapmaker's U1 code.
- The Bambu Studio improvements and the LixNix multi-nozzle features listed above.

**Multi-nozzle (different nozzle sizes on the U1)**
- Nozzle 1–4 tabs, each with its own diameter, flow (Standard / High Flow) and preferred layer height.
- A per-toolhead line-width selector in the Process tab.
- Warnings when a filament or a layer height does not fit a toolhead's nozzle.
- Wipe tower and supports that adapt to each nozzle.

**Fixes made in SnapOrca**
- **Crash on Prepare:** the bed was drawn with the wrong shader.
- **Duplicated nozzle panel** in the sidebar.
- **Tree / organic supports with % line widths:** "105%" was read as 105 mm.
- **Float precision** of the support nozzle diameter.
- **Toolchange temperatures:** M104 is sent to every tool.
- **Wipe tower:** sparse-layer combination; interface temperature at toolchange; tower options that do nothing with the U1 tower are hidden.
- **Profile updater:** error at start-up.
- **Default process per U1 nozzle:** 0.2 → 0.10 mm, 0.4 → 0.20 mm, 0.6 → 0.30 mm, 0.8 → 0.40 mm.
- **Profile defaults:** `max_bridge_length` and raft expansion kept at the Snapmaker values.
- **Two calibrations** that always failed.
- **Unit tests:** the slicing tests that the merge broke are fixed.

**Performance**
- Objects outside the camera are no longer drawn (restored from Snapmaker).
- Batched plate icons.
- Lighter memory guard.

**Diagnostics**
- A crash catcher writes `crash_*.log` and `crash_*.dmp` to `%APPDATA%\SnapOrca\log`.
- Extra logging when switching tabs.

**Translations**
- The 23 languages the merge had dropped are back.
- Spanish is complete, including the multi-nozzle texts.

**Installs separately**
- Its own folder, uninstaller, Start-menu entry and data folder.
- It does not take over the official `Snapmaker_Orca://` links.

### In progress: pull requests after 2.5.0-snaporca.1

These changes are in open pull requests and go into the next release once "Build all" is green and the U1 default G-code (4 × 0.4 mm, 0.20mm Standard, Snapmaker PLA Basic; 1 and 4 filaments; with and without prime tower) is confirmed identical to `main`. Every new feature is behind an option that is off by default.

**Fixes to the engine**
- **Per-toolhead speeds now reach the U1 G-code** ([#1](https://github.com/Jokiblack/SnapOrca/pull/1)). Three causes: `GCode::process_slot_of` returned the extruder index instead of the toolhead column (the U1 has Standard / High Flow columns per toolhead); the prime tower read every toolhead's speeds from column 0; a filament without its own column read filament 1's values. With equal columns the G-code does not change.
- **High Flow per filament** ([#11](https://github.com/Jokiblack/SnapOrca/pull/11)). Porting the LixNix column model was studied and rejected (≈2,000 lines, breaks Snapmaker profile loading and 3MF files shared with 2.4). Instead: a **"Create High Flow values"** button in the Filament tab (shown only when a toolhead is on High Flow and the filament has no High Flow values), a fix so a user preset with two flow modes no longer loses its High Flow values on restart, and a fix in `Print::apply` that applied the flow-column offset twice with segmented filaments.
- **Prime tower outside the bed on 0.2 mm single-extruder profiles** ([#2](https://github.com/Jokiblack/SnapOrca/pull/2)): Geeetech M1, Mellow M1 and Rook MK1 LDO. The tower estimate used purge volume while the planner lays whole rows plus ramming and wall; the estimate now reserves what the planner prints, only for single-extruder-multi-material towers. The U1 keeps its tower where it was.

**Ported from Bambu Studio**
- **First-layer infill line width per toolhead** ([#3](https://github.com/Jokiblack/SnapOrca/pull/3), Bambu c9089d03c): `main` had the option as a single value; it is now one column per toolhead like the other widths. 0 = use the first-layer line width, as before.
- **Mixed colour: no diagonal Z descent when travelling between sublayers** ([#4](https://github.com/Jokiblack/SnapOrca/pull/4), Bambu bf8d52c72). The other three B7 fixes were already in `main` or do not apply to Orca 2.5's preview.
- **Cut keeps the filament of every part** ([#5](https://github.com/Jokiblack/SnapOrca/pull/5), Bambu 29faefd6a): contour and dovetail cuts of a multi-part object merged all parts into one volume without config. Painted areas were already kept when "Keep painted areas after mesh changes" is on.
- **Reversed-face detection on import** ([#6](https://github.com/Jokiblack/SnapOrca/pull/6), Bambu b2c5009cf and follow-ups): new preference "Detect reversed faces when importing models" (off); a warning in the object list points to Fix Model. Whole-mesh inversion was already repaired.
- **Regression tests only**, because the fix was already in `main` or Orca 2.5 solves it another way: modifier infill density on cut models with connectors and supports after changing filament shrinkage ([#8](https://github.com/Jokiblack/SnapOrca/pull/8)); travel before the first tool selection and layer Z after a raft toolchange in spiral vase ([#9](https://github.com/Jokiblack/SnapOrca/pull/9)); periodic modifier ([#10](https://github.com/Jokiblack/SnapOrca/pull/10)). The uncovered-overhang warning was already complete.

**Ported from the LixNix fork** ([#7](https://github.com/Jokiblack/SnapOrca/pull/7))
- **Mesh level of detail** in the 3D view: preference "Simplify distant objects" (off). Slicing never uses the simplified mesh.
- **Vendor profile updates only through Snapmaker OTA**: option (off) that skips Orca's profile CDN. The GitHub new-version notice is untouched.
- Not ported: stealth mode (already complete in SnapOrca), Sentry crash reporting, and the change of `independent_support_layer_height`'s default.

### Known limitations
- The per-toolhead **speeds** from LixNix have no effect on the U1 in 2.5.0-snaporca.1. Fixed in [#1](https://github.com/Jokiblack/SnapOrca/pull/1), pending verification. Layer heights, line widths, the tower and supports per nozzle do work.
- Multi-nozzle printing is new. Check the preview, and start with small parts.
- Main target: **Windows x64**. Linux and macOS builds are made but less tested.

### Feedback
Report problems in [Issues](https://github.com/Jokiblack/SnapOrca/issues). Do not report SnapOrca problems to Snapmaker or OrcaSlicer.

---

## Español

SnapOrca es **Snapmaker Orca 2.4.1** (el laminador oficial de la Snapmaker U1) fusionado con **OrcaSlicer 2.5.0-alpha**. Añade mejoras traídas de **Bambu Studio** y la **impresión con boquillas de distinto tamaño** del fork de **LixNix**. Está hecho y probado sobre todo para la **Snapmaker U1** (cambiador de 4 cabezales).

### Descarga

📥 **[Última versión](https://github.com/Jokiblack/SnapOrca/releases/latest)**: instalador y portable para Windows.

- El instalador se llama **SnapOrca** y se instala en `Program Files\SnapOrca`, **junto al** Snapmaker Orca oficial, sin sustituirlo.
- Los ajustes se guardan en `%APPDATA%\SnapOrca`. La primera vez que se abre, SnapOrca copia tus perfiles de usuario del Snapmaker Orca oficial.
- Con los perfiles por defecto de la U1 (4 boquillas de 0.4 mm), el G-code es el mismo que sin los cambios de boquillas distintas. Se ha comprobado laminando 22 trabajos de prueba. La única diferencia es que ya no se emite el `SET_PRESSURE_ADVANCE ADVANCE=0` final (ver Snapmaker#714).

### Créditos: de dónde salen las funciones

| Origen | Qué toma SnapOrca de él |
|---|---|
| [Snapmaker Orca](https://github.com/Snapmaker/OrcaSlicer), de **Snapmaker** | La base: soporte y perfiles de la Snapmaker U1, variantes de flujo (Standard / High Flow), filamento mixto Full Spectrum, la torre de purga por bloques de la U1, la página de dispositivo y la conexión Moonraker / MQTT. |
| [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer), de **SoftFever** y los colaboradores de OrcaSlicer | El motor y la interfaz de la 2.5.0-alpha: agentes de impresora y plugins, opciones de laminado nuevas, arreglos y mejoras de rendimiento. |
| [Bambu Studio](https://github.com/bambulab/BambuStudio), de **Bambu Lab** | Patrón propio para la capa bajo la superficie superior; patrón «Global monotonic line»; ancho de línea propio para el relleno de la primera capa; modificador periódico y «el modificador ignora el relleno»; planchado en espiral de Arquímedes; 13 arreglos del soporte en árbol orgánico e híbrido y aviso de voladizos sin cubrir; refuerzos en 3MF y en el corte. |
| [Fork de Snapmaker Orca de LixNix](https://github.com/LixNix-Swap-Org/Snapmaker-Orca), de **LixNix** | **Boquillas de distinto tamaño en la U1**: diámetro y altura de capa preferida por boquilla, altura de capa por cabezal y su motor de combinación, capas de soporte fraccionarias y bloques de la torre de purga, anchos de línea por cabezal, material de soporte según la boquilla, «la torre cuenta todos los filamentos que imprimen»; arreglos de cierres (clic derecho en la bandeja, lista de objetos, cambio de impresora); asistente con Snapmaker primero; anchos de línea de la U1 en % de la boquilla; arreglo de las vistas web ocultas en macOS. |
| [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer) / [Slic3r](https://github.com/Slic3r/Slic3r) | El origen común de todos los laminadores anteriores. |

La fusión, los arreglos y las pruebas se han hecho con ayuda de **Claude (Anthropic)**. Los commits llevan una línea `Co-Authored-By`.

### Qué añade SnapOrca respecto al Snapmaker Orca 2.4.1 oficial

**Motor e interfaz**
- El motor y la interfaz de OrcaSlicer 2.5.0-alpha, sobre el código de la U1 de Snapmaker.
- Las mejoras de Bambu Studio y las funciones de boquillas distintas de LixNix de la tabla anterior.

**Boquillas de distinto tamaño en la U1**
- Pestañas Boquilla 1–4, cada una con su diámetro, su flujo (Standard / High Flow) y su altura de capa preferida.
- Selector de ancho de línea por cabezal en la pestaña Proceso.
- Avisos cuando un filamento o una altura de capa no encajan con la boquilla de un cabezal.
- Torre de purga y soportes que se adaptan a cada boquilla.

**Arreglos hechos en SnapOrca**
- **Cierre al abrir Preparar:** la cama se dibujaba con el shader equivocado.
- **Panel de boquillas repetido** en la barra lateral.
- **Soportes en árbol / orgánicos con anchos en %:** «105%» se leía como 105 mm.
- **Precisión del diámetro de boquilla** de los soportes.
- **Temperaturas en los cambios de cabezal:** se manda M104 a todas las herramientas.
- **Torre de purga:** combinación de capas dispersas; temperatura de interfaz en el cambio; se ocultan las opciones que no hacen nada con la torre de la U1.
- **Actualizador de perfiles:** error al arrancar.
- **Proceso por defecto de cada boquilla de la U1:** 0.2 → 0.10 mm, 0.4 → 0.20 mm, 0.6 → 0.30 mm, 0.8 → 0.40 mm.
- **Valores de perfil:** `max_bridge_length` y la expansión de la balsa se mantienen con los valores de Snapmaker.
- **Dos calibraciones** que fallaban siempre.
- **Pruebas automáticas:** arregladas las de laminado que había roto la fusión.

**Rendimiento**
- Ya no se dibujan los objetos que están fuera de la cámara (recuperado de Snapmaker).
- Iconos de bandeja dibujados en lote.
- Vigilancia de memoria más ligera.

**Diagnóstico**
- Un capturador de cierres guarda `crash_*.log` y `crash_*.dmp` en `%APPDATA%\SnapOrca\log`.
- Más registro al cambiar de pestaña.

**Traducciones**
- Vuelven los 23 idiomas que la fusión había borrado.
- Español completo, incluidos los textos de boquillas distintas.

**Instalación aparte**
- Carpeta, desinstalador, entrada del menú Inicio y carpeta de datos propios.
- No se apropia de los enlaces `Snapmaker_Orca://` del oficial.

### En marcha: pull requests posteriores a la 2.5.0-snaporca.1

Estos cambios están en pull requests abiertas y entrarán en la siguiente versión cuando "Build all" esté en verde y se confirme que el G-code por defecto de la U1 (4 × 0,4 mm, 0.20mm Standard, Snapmaker PLA Basic; 1 y 4 filamentos; con y sin torre de purga) sigue idéntico al de `main`. Toda función nueva va detrás de una opción desactivada por defecto.

**Arreglos del motor**
- **Las velocidades por cabezal ya llegan al G-code de la U1** ([#1](https://github.com/Jokiblack/SnapOrca/pull/1)). Tres causas: `GCode::process_slot_of` devolvía el índice del extrusor en vez de la columna del cabezal (la U1 tiene columnas Standard / High Flow por cabezal); la torre de purga leía las velocidades de todos los cabezales en la columna 0; un filamento sin columna propia leía los valores del filamento 1. Con columnas iguales el G-code no cambia.
- **High Flow por filamento** ([#11](https://github.com/Jokiblack/SnapOrca/pull/11)). Se estudió portar el modelo de columnas de LixNix y se descartó (≈2.000 líneas, rompe la carga de los perfiles Snapmaker y los 3MF compartidos con la 2.4). En su lugar: botón **«Crear valores High Flow»** en la pestaña Filamento (solo aparece con un cabezal en High Flow y un filamento sin esos valores), un arreglo para que un preset de usuario con dos modos de flujo no pierda sus valores High Flow al reiniciar, y un arreglo en `Print::apply`, que aplicaba dos veces el desplazamiento de columna con filamentos segmentados.
- **Torre de purga fuera de la cama en perfiles de 0,2 mm de un solo extrusor** ([#2](https://github.com/Jokiblack/SnapOrca/pull/2)): Geeetech M1, Mellow M1 y Rook MK1 LDO. La estimación de la torre usaba el volumen de purga mientras el planificador pone filas enteras más ramming y pared; ahora la estimación reserva lo que el planificador imprime, solo en torres de un extrusor multimaterial. La U1 conserva su torre donde estaba.

**Traído de Bambu Studio**
- **Ancho de línea del relleno de la primera capa por cabezal** ([#3](https://github.com/Jokiblack/SnapOrca/pull/3), Bambu c9089d03c): `main` tenía la opción como un solo valor; ahora es una columna por cabezal como los demás anchos. 0 = usar el ancho de la primera capa, como antes.
- **Color mezclado: sin bajadas de Z en diagonal al desplazarse entre subcapas** ([#4](https://github.com/Jokiblack/SnapOrca/pull/4), Bambu bf8d52c72). Los otros tres arreglos B7 ya estaban en `main` o no aplican a la vista previa de Orca 2.5.
- **Cortar conserva el filamento de cada pieza** ([#5](https://github.com/Jokiblack/SnapOrca/pull/5), Bambu 29faefd6a): los cortes por contorno y en cola de milano de un objeto multipieza fundían todas las piezas en un volumen sin configuración. Las zonas pintadas ya se conservaban con «Mantener las zonas pintadas tras cambiar la malla».
- **Detección de caras invertidas al importar** ([#6](https://github.com/Jokiblack/SnapOrca/pull/6), Bambu b2c5009cf y siguientes): preferencia nueva «Detectar caras invertidas al importar modelos» (desactivada); un aviso en la lista de objetos remite a Reparar modelo. La inversión de la malla entera ya se reparaba.
- **Solo pruebas de regresión**, porque el arreglo ya estaba en `main` o Orca 2.5 lo resuelve de otra forma: densidad de relleno de modificadores en piezas cortadas con conectores y soportes tras cambiar la contracción del filamento ([#8](https://github.com/Jokiblack/SnapOrca/pull/8)); desplazamiento antes de la primera selección de herramienta y Z de capa tras el cambio de herramienta de la balsa en vaso espiral ([#9](https://github.com/Jokiblack/SnapOrca/pull/9)); modificador periódico ([#10](https://github.com/Jokiblack/SnapOrca/pull/10)). El aviso de voladizos sin cubrir ya estaba completo.

**Traído del fork de LixNix** ([#7](https://github.com/Jokiblack/SnapOrca/pull/7))
- **Nivel de detalle de la malla** en la vista 3D: preferencia «Simplificar los objetos lejanos» (desactivada). El laminado nunca usa la malla simplificada.
- **Actualizaciones de perfiles de fabricante solo por OTA de Snapmaker**: opción (desactivada) que salta el CDN de perfiles de Orca. El aviso de versión nueva de GitHub no cambia.
- No se portan: el modo sigilo (ya está completo en SnapOrca), el envío de errores a Sentry y el cambio de valor por defecto de `independent_support_layer_height`.

### Limitaciones conocidas
- Las **velocidades** por cabezal de LixNix no tienen efecto en la U1 en la 2.5.0-snaporca.1. Arreglado en [#1](https://github.com/Jokiblack/SnapOrca/pull/1), pendiente de verificación. Las alturas de capa, los anchos de línea, la torre y los soportes por boquilla sí funcionan.
- La impresión con boquillas distintas es nueva. Revisa la vista previa y empieza con piezas pequeñas.
- Plataforma principal: **Windows x64**. Las versiones de Linux y macOS se compilan, pero se han probado menos.

### Comentarios
Informa de los problemas en [Issues](https://github.com/Jokiblack/SnapOrca/issues). No informes de problemas de SnapOrca a Snapmaker ni a OrcaSlicer.

---

# How to compile
- Windows 64-bit  
  - Tools needed: Visual Studio 2022 (or newer) with the C++ workload, CMake, Git, Strawberry Perl, NSIS (for the installer).
      - Strawberry Perl is [available on their GitHub repository](https://github.com/StrawberryPerl/Perl-Dist-Strawberry/releases/).
  - Run `build_win.bat -d` once to build the dependencies, then `build_win.bat -s -i` to build and install into `build\Snapmaker_Orca`, or `build_win.bat -s --installer` to also create the NSIS installer.
  - Run `build_win.bat -h` for all options.

- Mac 64-bit  
  - Tools needed: Xcode, Cmake, git, gettext, libtool, automake, autoconf, texinfo
      - You can install most of them by running `brew install cmake gettext libtool automake autoconf texinfo`
  - run `./build_release_macos.sh`
  - To build and debug in Xcode:
      - run `Xcode.app`
      - open ``build_`arch`/Snapmaker_Orca.Xcodeproj``
      - menu bar: Product => Scheme => Snapmaker_Orca
      - menu bar: Product => Scheme => Edit Scheme...
          - Run => Info tab => Build Configuration: `RelWithDebInfo`
          - Run => Options tab => Document Versions: uncheck `Allow debugging when browsing versions`
      - menu bar: Product => Run

- Ubuntu 
  - Dependencies **Will be auto-installed with the shell script**: `libmspack-dev libgstreamerd-3-dev libsecret-1-dev libwebkit2gtk-4.0-dev libosmesa6-dev libssl-dev libcurl4-openssl-dev eglexternalplatform-dev libudev-dev libdbus-1-dev extra-cmake-modules libgtk2.0-dev libglew-dev libudev-dev libdbus-1-dev cmake git texinfo`
  - run `sudo ./build_linux.sh -u`
  - run `./build_linux.sh -dsir`


# Note: 
If you're running Klipper, it's recommended to add the following configuration to your `printer.cfg` file.
```
# Enable object exclusion
[exclude_object]

# Enable arcs support
[gcode_arcs]
resolution: 0.1
```


## Some background
SnapOrca is a fork of Snapmaker Orca merged with OrcaSlicer 2.5. Snapmaker Orca is originally forked from OrcaSlicer.

Snapmaker_Orca is originally forked from Bambu Studio, it was previously known as BambuStudio-SoftFever.
Bambu Studio is forked from [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer) by Prusa Research, which is from [Slic3r](https://github.com/Slic3r/Slic3r) by Alessandro Ranellucci and the RepRap community. 
Orca Slicer incorporates a lot of features from SuperSlicer by @supermerill
Orca Slicer's logo is designed by community member Justin Levine(@freejstnalxndr)  


# License
SnapOrca is licensed under the GNU Affero General Public License, version 3, like all the projects it is based on; its full source code is in this repository.

Snapmaker Orca is licensed under the GNU Affero General Public License, version 3. Snapmaker_Orca is based on Orca Slicer by SoftFever

Orca Slicer is licensed under the GNU Affero General Public License, version 3. Orca Slicer is based on Bambu Studio by BambuLab.

Bambu Studio is licensed under the GNU Affero General Public License, version 3. Bambu Studio is based on PrusaSlicer by PrusaResearch.

PrusaSlicer is licensed under the GNU Affero General Public License, version 3. PrusaSlicer is owned by Prusa Research. PrusaSlicer is originally based on Slic3r by Alessandro Ranellucci.

Slic3r is licensed under the GNU Affero General Public License, version 3. Slic3r was created by Alessandro Ranellucci with the help of many other contributors.

The GNU Affero General Public License, version 3 ensures that if you use any part of this software in any way (even behind a web server), your software must be released under the same license.

Orca Slicer includes a pressure advance calibration pattern test adapted from Andrew Ellis' generator, which is licensed under GNU General Public License, version 3. Ellis' generator is itself adapted from a generator developed by Sineos for Marlin, which is licensed under GNU General Public License, version 3.

The Bambu networking plugin is based on non-free libraries from BambuLab. It is optional to the Orca Slicer and provides extended functionalities for Bambulab printer users.

# Feedback & Contribution
For SnapOrca problems, open an issue at https://github.com/Jokiblack/SnapOrca/issues.
