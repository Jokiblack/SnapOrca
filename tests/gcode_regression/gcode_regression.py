#!/usr/bin/env python3
"""Prueba de regresión de G-code de la Snapmaker U1 (SnapOrca).

Lamina con la CLI `snapmaker-orca` un conjunto fijo de modelos pequeños (generados aquí mismo) con los
perfiles de sistema de la U1 de `resources/profiles/Snapmaker/`, normaliza los G-code (quita las líneas
con fecha, hora o versión) y los compara con las referencias guardadas en `reference/`.

    gcode_regression.py --slicer <AppImage | squashfs-root | AppRun | snapmaker-orca> [--out DIR]
    gcode_regression.py --slicer ... --update          # regenera las referencias (cambio intencionado)
    gcode_regression.py --list                         # enumera los casos

Solo biblioteca estándar de Python 3. Código de salida: 0 todo idéntico, 1 diferencias o referencias que
faltan, 2 fallo de laminado o de uso. Con `--allow-missing` una referencia ausente no es un fallo (modo
"bootstrap" de la integración continua, cuando todavía no hay referencias).

Opciones de la CLI (src/Snapmaker_Orca.cpp y src/libslic3r/Config.cpp, read_cli):
  * `--load-settings` (impresora y proceso), `--load-filaments` (uno por ranura), `--slice 1`,
    `--outputdir`, `--datadir`. Los JSON de perfil se pasan por su ruta real dentro del árbol
    `resources/profiles` (`PresetBundle::resolve_preset_config` sube hasta `Snapmaker.json`).
  * Cualquier clave de PrintConfig.cpp es una opción con la máxima prioridad. Los booleanos NO
    aceptan el valor como token aparte (`--enable-prime-tower 0` tomaría el `0` como fichero de
    entrada): se escriben `--clave=valor`. Aquí se usa siempre la forma `--clave=valor`.
"""

import argparse
import difflib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
REFERENCE_DIR = os.path.join(HERE, "reference")

PRINTER = "Snapmaker U1 (0.4 nozzle)"
PROCESS = "0.20mm Standard @Snapmaker U1 (0.4 nozzle)"
FILAMENT = "Snapmaker PLA Basic @U1"
BED_TYPE = "Textured PEI Plate"  # default_bed_type de la U1; la CLI no lo aplica sola
# Cuatro colores distintos: con un 3mf propio y 4 filamentos iguales la CLI mapearía todos al mismo
# y apagaría la torre (Snapmaker_Orca.cpp, disable_wipe_tower_after_mapping).
COLOURS = "#FF0000;#00FF00;#0000FF;#FFFF00"
# Boquillas mezcladas: cabezales 1 y 3 de 0,4 mm, 2 y 4 de 0,6 mm. SnapOrca declara el diámetro de
# cada cabezal en el vector `nozzle_diameter` del perfil de impresora (una entrada por cabezal, las
# pestañas Boquilla 1..4 de la aplicación). Los casos *_mixed laminan con una copia del árbol de
# perfiles en la que ese vector del preset "Snapmaker U1 (0.4 nozzle)" lleva estos valores; lo que
# haría un usuario al editar el perfil. No se usa `--nozzle-diameter` por línea de comandos: la
# validación temprana de la CLI (m_config.validate antes de cargar los presets) rechaza boquillas
# mezcladas ("Bridge line width must not exceed nozzle diameter"). La CLI no compone el proceso por
# cabezal (PerHeadProcess, solo en la aplicación): todos los cabezales laminan con el proceso cargado.
MIXED_NOZZLES = ["0.4", "0.6", "0.4", "0.6"]

# Versión que declaran los 3mf generados: >= 2.2.0 para no activar las migraciones de proyectos
# antiguos de la CLI y no más nueva que el programa (se pasa --allow-newer-file por si acaso).
APP_VERSION = "2.2.0"
CUBE = 20.0


# ----------------------------------------------------------------------------- mallas

def cube_mesh(size):
    """Cubo centrado en XY, apoyado en Z=0. Devuelve (vértices, triángulos), normales hacia fuera."""
    h = size / 2.0
    v = [(-h, -h, 0.0), (h, -h, 0.0), (h, h, 0.0), (-h, h, 0.0),
         (-h, -h, size), (h, -h, size), (h, h, size), (-h, h, size)]
    t = [(0, 2, 1), (0, 3, 2),   # inferior
         (4, 5, 6), (4, 6, 7),   # superior
         (0, 1, 5), (0, 5, 4),   # frontal  (y = -h)
         (1, 2, 6), (1, 6, 5),   # derecha  (x = +h)
         (2, 3, 7), (2, 7, 6),   # trasera  (y = +h)
         (3, 0, 4), (3, 4, 7)]   # izquierda (x = -h)
    return v, t


def extrude_profile(points, tris, depth):
    """Extruye un polígono del plano XZ a lo largo de Y.

    `points`: [(x, z)] en sentido antihorario (X a la derecha, Z arriba), `tris`: triángulos del
    polígono con el mismo sentido. Devuelve una malla cerrada centrada en Y (de -depth/2 a
    +depth/2), con las normales hacia fuera.
    """
    n = len(points)
    y0, y1 = -depth / 2.0, depth / 2.0
    v = [(x, y0, z) for x, z in points] + [(x, y1, z) for x, z in points]
    t = []
    for a, b, c in tris:
        t.append((a, b, c))                   # cara frontal (y = y0): normal hacia -Y
        t.append((a + n, c + n, b + n))       # cara trasera (y = y1): normal hacia +Y
    for i in range(n):
        j = (i + 1) % n
        # Lateral de la arista i->j (polígono antihorario en XZ): antihorario visto desde fuera.
        t.append((i, j + n, j))
        t.append((i, i + n, j + n))
    return v, t


def overhang_mesh():
    """Pieza en L de 20x20x20 mm: columna de 8 mm de ancho y voladizo de 12 mm a 15 mm de altura.

    Perfil en XZ (antihorario): (0,0) (8,0) (8,15) (20,15) (20,20) (0,20). Un voladizo así necesita
    soporte.
    """
    pts = [(0.0, 0.0), (8.0, 0.0), (8.0, 15.0), (20.0, 15.0), (20.0, 20.0), (0.0, 20.0)]
    tris = [(0, 1, 2), (0, 2, 5), (5, 2, 3), (5, 3, 4)]
    v, t = extrude_profile(pts, tris, CUBE)
    # Centrar en X (el perfil va de 0 a 20).
    v = [(x - 10.0, y, z) for x, y, z in v]
    return v, t


def normal(a, b, c):
    ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
    vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
    nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
    l = (nx * nx + ny * ny + nz * nz) ** 0.5 or 1.0
    return nx / l, ny / l, nz / l


def check_mesh(v, t):
    """Comprobación barata de que la malla es cerrada y orientada: cada arista aparece dos veces, en
    sentidos opuestos."""
    edges = {}
    for a, b, c in t:
        for e in ((a, b), (b, c), (c, a)):
            edges[e] = edges.get(e, 0) + 1
    for (a, b), cnt in edges.items():
        if cnt != 1 or edges.get((b, a), 0) != 1:
            raise RuntimeError("malla no cerrada u orientada: arista %r" % ((a, b),))
    # Volumen con signo positivo = normales hacia fuera.
    vol = 0.0
    for a, b, c in t:
        p, q, r = v[a], v[b], v[c]
        vol += (p[0] * (q[1] * r[2] - q[2] * r[1]) - p[1] * (q[0] * r[2] - q[2] * r[0])
                + p[2] * (q[0] * r[1] - q[1] * r[0])) / 6.0
    if vol <= 0:
        raise RuntimeError("malla con las normales hacia dentro (volumen %g)" % vol)


def write_stl_ascii(path, name, v, t):
    lines = ["solid %s" % name]
    for a, b, c in t:
        lines.append("  facet normal %.6e %.6e %.6e" % normal(v[a], v[b], v[c]))
        lines.append("    outer loop")
        for i in (a, b, c):
            lines.append("      vertex %.6e %.6e %.6e" % v[i])
        lines.append("    endloop")
        lines.append("  endfacet")
    lines.append("endsolid %s" % name)
    with open(path, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


# ----------------------------------------------------------------------------- 3mf

def fmt(x):
    s = ("%.6f" % x).rstrip("0").rstrip(".")
    return s if s not in ("", "-0") else "0"


# Pintado multimaterial por triángulo (atributo `paint_color` de <triangle>, bbs_3mf.cpp
# MMU_SEGMENTATION_ATTR). Codificación de TriangleSelector (Model.cpp, CONST_FILAMENTS): triángulo sin
# dividir con estado N (N = extrusor): "4" = extrusor 1, "8" = 2, "0C" = 3, "1C" = 4.
PAINT = {1: "4", 2: "8", 3: "0C", 4: "1C"}


def model_xml(objects):
    """`objects`: lista de dict(name, vertices, triangles, position=(x, y), paint={tri_index: extruder})."""
    out = ['<?xml version="1.0" encoding="UTF-8"?>',
           '<model unit="millimeter" xml:lang="en-US" '
           'xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">',
           ' <metadata name="Application">Snapmaker_Orca-%s</metadata>' % APP_VERSION,
           ' <metadata name="Snapmaker_Orca:3mfVersion">1</metadata>',
           ' <metadata name="Title">SnapOrca gcode_regression</metadata>',
           ' <resources>']
    for i, o in enumerate(objects):
        oid = i + 1
        out.append('  <object id="%d" name="%s" type="model">' % (oid, o["name"]))
        out.append('   <mesh>')
        out.append('    <vertices>')
        for x, y, z in o["vertices"]:
            out.append('     <vertex x="%s" y="%s" z="%s"/>' % (fmt(x), fmt(y), fmt(z)))
        out.append('    </vertices>')
        out.append('    <triangles>')
        paint = o.get("paint") or {}
        for k, (a, b, c) in enumerate(o["triangles"]):
            attr = ' paint_color="%s"' % PAINT[paint[k]] if k in paint else ""
            out.append('     <triangle v1="%d" v2="%d" v3="%d"%s/>' % (a, b, c, attr))
        out.append('    </triangles>')
        out.append('   </mesh>')
        out.append('  </object>')
    out.append(' </resources>')
    out.append(' <build>')
    for i, o in enumerate(objects):
        x, y = o["position"]
        # Matriz 3x4 por columnas: m00 m10 m20 m01 m11 m21 m02 m12 m22 tx ty tz
        out.append('  <item objectid="%d" transform="1 0 0 0 1 0 0 0 1 %s %s 0" printable="1"/>'
                   % (i + 1, fmt(x), fmt(y)))
    out.append(' </build>')
    out.append('</model>')
    return "\n".join(out) + "\n"


def model_settings_xml(objects):
    out = ['<?xml version="1.0" encoding="UTF-8"?>', '<config>']
    for i, o in enumerate(objects):
        oid = i + 1
        out.append('  <object id="%d" auto_drop="0">' % oid)
        out.append('    <metadata key="name" value="%s"/>' % o["name"])
        out.append('    <metadata key="extruder" value="%d"/>' % o.get("extruder", 1))
        out.append('    <part id="%d" subtype="normal_part">' % oid)
        out.append('      <metadata key="name" value="%s"/>' % o["name"])
        out.append('      <metadata key="matrix" value="1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1"/>')
        out.append('    </part>')
        out.append('  </object>')
    # Una bandeja con las instancias ya colocadas: con un 3mf propio (tag Application) la CLI no
    # reordena los objetos (need_arrange = false), así que las posiciones son las del fichero.
    out.append('  <plate>')
    out.append('    <metadata key="plater_id" value="1"/>')
    out.append('    <metadata key="plater_name" value=""/>')
    out.append('    <metadata key="locked" value="false"/>')
    for i in range(len(objects)):
        out.append('    <model_instance>')
        out.append('      <metadata key="object_id" value="%d"/>' % (i + 1))
        out.append('      <metadata key="instance_id" value="0"/>')
        out.append('      <metadata key="identify_id" value="%d"/>' % (i + 1))
        out.append('    </model_instance>')
    out.append('  </plate>')
    out.append('</config>')
    return "\n".join(out) + "\n"


CONTENT_TYPES = """<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
 <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
 <Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/>
 <Default Extension="png" ContentType="image/png"/>
</Types>"""

RELS = """<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
 <Relationship Target="/3D/3dmodel.model" Id="rel-1" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/>
</Relationships>"""


def write_3mf(path, objects):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        def add(name, data):
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))  # reproducible byte a byte
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, data)
        add("[Content_Types].xml", CONTENT_TYPES)
        add("_rels/.rels", RELS)
        add("3D/3dmodel.model", model_xml(objects))
        add("Metadata/model_settings.config", model_settings_xml(objects))


CENTER = (135.5, 136.0)  # centro de la cama de la U1 (printable_area 0.5..270.5 x 1..271)
FOUR = [(95.0, 95.0), (175.0, 95.0), (95.0, 175.0), (175.0, 175.0)]


def generate_models(models_dir):
    """Escribe los modelos en `models_dir` y devuelve {nombre: ruta}."""
    os.makedirs(models_dir, exist_ok=True)
    cv, ct = cube_mesh(CUBE)
    ov, ot = overhang_mesh()
    check_mesh(cv, ct)
    check_mesh(ov, ot)
    paths = {}

    def obj(name, v, t, pos, extruder=1, paint=None):
        return dict(name=name, vertices=v, triangles=t, position=pos, extruder=extruder, paint=paint)

    p = os.path.join(models_dir, "cube20.stl")
    write_stl_ascii(p, "cube20", cv, ct)
    paths["cube20.stl"] = p

    p = os.path.join(models_dir, "cube20_e2.3mf")  # un cubo en el extrusor 2 (cabezal de 0,6 mm al mezclar)
    write_3mf(p, [obj("cube_T2", cv, ct, CENTER, extruder=2)])
    paths["cube20_e2.3mf"] = p

    p = os.path.join(models_dir, "cubes4.3mf")  # cuatro cubos, uno por extrusor
    write_3mf(p, [obj("cube_T%d" % (i + 1), cv, ct, FOUR[i], extruder=i + 1) for i in range(4)])
    paths["cubes4.3mf"] = p

    # Cubo pintado: objeto en el extrusor 1; tapa superior pintada con el 2, cara derecha (x=+h) con
    # el 3 y cara trasera (y=+h) con el 4 (índices de triángulo de cube_mesh).
    paint = {2: 2, 3: 2, 6: 3, 7: 3, 8: 4, 9: 4}
    p = os.path.join(models_dir, "cube20_painted.3mf")
    write_3mf(p, [obj("cube_painted", cv, ct, CENTER, extruder=1, paint=paint)])
    paths["cube20_painted.3mf"] = p

    for ext in (1, 2):
        p = os.path.join(models_dir, "overhang_e%d.3mf" % ext)
        write_3mf(p, [obj("overhang_T%d" % ext, ov, ot, CENTER, extruder=ext)])
        paths["overhang_e%d.3mf" % ext] = p
    return paths


# ----------------------------------------------------------------------------- casos

def support(kind):
    base = {"enable_support": "1"}
    if kind == "normal":
        base.update(support_type="normal(auto)", support_style="default")
    elif kind == "tree":
        base.update(support_type="tree(auto)", support_style="tree_hybrid")
    elif kind == "organic":
        base.update(support_type="tree(auto)", support_style="organic")
    else:
        raise ValueError(kind)
    return base


def build_cases():
    """Cada caso: nombre -> dict(model, filaments, settings{clave: valor}, mixed)."""
    cases = {}

    def add(name, model, filaments, settings=None, mixed=False, colours=False):
        s = dict(settings or {})
        if colours:
            s["filament_colour"] = COLOURS
        cases[name] = dict(model=model, filaments=filaments, settings=s, mixed=mixed)

    for mixed, suffix in ((False, ""), (True, "_mixed")):
        # Un filamento. Con boquillas mezcladas el cubo va al extrusor 2 (0,6 mm) con 2 filamentos
        # cargados, para que la boquilla distinta se use de verdad.
        if not mixed:
            add("cube_1f", "cube20.stl", 1)
        else:
            add("cube_1f_mixed", "cube20_e2.3mf", 2, mixed=True)
        add("cubes_4f_tower" + suffix, "cubes4.3mf", 4, {"enable_prime_tower": "1"}, mixed, colours=True)
        add("cubes_4f_notower" + suffix, "cubes4.3mf", 4, {"enable_prime_tower": "0"}, mixed, colours=True)
        ov = "overhang_e1.3mf" if not mixed else "overhang_e2.3mf"
        nf = 1 if not mixed else 2
        add("support_normal" + suffix, ov, nf, support("normal"), mixed)
        add("support_tree" + suffix, ov, nf, support("tree"), mixed)
        add("support_organic" + suffix, ov, nf, support("organic"), mixed)
        add("painted_4f" + suffix, "cube20_painted.3mf", 4, {"enable_prime_tower": "1"}, mixed, colours=True)
    return cases


CASES = build_cases()


# ----------------------------------------------------------------------------- laminado

def find_slicer(arg, work):
    """Devuelve el ejecutable a lanzar (AppRun si existe: fija LD_LIBRARY_PATH a las bibliotecas
    empaquetadas) a partir de un .AppImage, un directorio extraído o un binario."""
    arg = os.path.abspath(arg)
    if os.path.isdir(arg):
        root = arg
    elif arg.lower().endswith(".appimage"):
        root = os.path.join(work, "squashfs-root")
        if not os.path.isdir(root):
            os.chmod(arg, 0o755)
            subprocess.run([arg, "--appimage-extract"], cwd=work, check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    else:
        root = os.path.dirname(os.path.dirname(arg))
        if os.path.basename(os.path.dirname(arg)) != "bin":
            return arg
    for cand in ("AppRun", os.path.join("bin", "snapmaker-orca"), "snapmaker-orca"):
        p = os.path.join(root, cand)
        if os.path.isfile(p) and os.access(p, os.X_OK):
            return p
    raise SystemExit("no encuentro AppRun ni bin/snapmaker-orca en %s" % root)


def mixed_profiles(profiles, out):
    """Copia del árbol de perfiles con las boquillas mezcladas en el preset de impresora de la U1.

    Se copia el árbol entero (Snapmaker.json, Snapmaker/ y OrcaFilamentLibrary*) porque la CLI resuelve
    `inherits` buscando el manifiesto junto al fichero cargado. Solo cambia el vector `nozzle_diameter`
    del preset, por sustitución textual, para no reescribir el JSON (lleva BOM y un formato propio).
    """
    dst = os.path.join(out, "profiles_mixed")
    shutil.rmtree(dst, ignore_errors=True)
    os.makedirs(dst)
    for n in ("Snapmaker", "Snapmaker.json", "OrcaFilamentLibrary", "OrcaFilamentLibrary.json"):
        p = os.path.join(profiles, n)
        if os.path.isdir(p):
            shutil.copytree(p, os.path.join(dst, n))
        elif os.path.isfile(p):
            shutil.copy(p, os.path.join(dst, n))
    machine = os.path.join(dst, "Snapmaker", "machine", PRINTER + ".json")
    with open(machine, "rb") as f:
        raw = f.read()
    bom = raw.startswith(b"\xef\xbb\xbf")
    text = raw.decode("utf-8-sig")
    new = '"nozzle_diameter": [\n%s\n    ]' % ",\n".join('        "%s"' % d for d in MIXED_NOZZLES)
    text, n = re.subn(r'"nozzle_diameter"\s*:\s*\[[^\]]*\]', new, text)
    if n != 1:
        raise SystemExit("no encuentro nozzle_diameter en %s" % machine)
    with open(machine, "wb") as f:
        f.write((b"\xef\xbb\xbf" if bom else b"") + text.encode("utf-8"))
    return dst


def profile_paths(profiles):
    paths = {
        "machine": os.path.join(profiles, "Snapmaker", "machine", PRINTER + ".json"),
        "process": os.path.join(profiles, "Snapmaker", "process", PROCESS + ".json"),
        "filament": os.path.join(profiles, "Snapmaker", "filament", FILAMENT + ".json"),
        "manifest": os.path.join(profiles, "Snapmaker.json"),
    }
    for k, p in paths.items():
        if not os.path.isfile(p):
            raise SystemExit("falta el perfil %s: %s" % (k, p))
    return paths


def slice_case(name, case, slicer, profiles, models, out, datadir, debug, timeout):
    rundir = os.path.join(out, "run", name)
    shutil.rmtree(rundir, ignore_errors=True)
    os.makedirs(rundir)
    cmd = [slicer, "--debug", str(debug), "--datadir", datadir, "--outputdir", rundir, "--allow-newer-file",
           "--load-settings", profiles["machine"], "--load-settings", profiles["process"]]
    for _ in range(case["filaments"]):
        cmd += ["--load-filaments", profiles["filament"]]
    cmd.append("--curr-bed-type=" + BED_TYPE)
    for key, value in sorted(case["settings"].items()):
        cmd.append("--%s=%s" % (key.replace("_", "-"), value))
    cmd += ["--slice", "1", models[case["model"]]]
    with open(os.path.join(out, "run", name + ".cmd"), "w") as f:
        f.write(" ".join(json.dumps(c) if " " in c else c for c in cmd) + "\n")
    t0 = time.time()
    with open(os.path.join(out, "run", name + ".log"), "w") as log:
        try:
            rc = subprocess.run(cmd, cwd=rundir, stdout=log, stderr=subprocess.STDOUT, timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            rc = "timeout"
    gcode = os.path.join(rundir, "plate_1.gcode")
    dt = time.time() - t0
    if not os.path.isfile(gcode):
        print("  [%s] FALLO (rc=%s, %.0fs): no hay plate_1.gcode; ver run/%s.log" % (name, rc, dt, name))
        result = os.path.join(rundir, "result.json")
        if os.path.isfile(result):
            with open(result, encoding="utf-8", errors="replace") as f:
                print("    result.json: " + f.read().replace("\n", " ")[:400])
        return None
    print("  [%s] laminado (rc=%s, %.0fs)" % (name, rc, dt))
    return gcode


# ----------------------------------------------------------------------------- normalización

VOLATILE = (
    re.compile(r"^; generated by "),                          # "; generated by Snapmaker Orca <ver> on <fecha>"
    re.compile(r"^;.*\d{4}-\d{2}-\d{2}[ T]\d{2}:\d{2}"),       # cualquier comentario con fecha-hora ISO
    re.compile(r"^;.*Snapmaker_Orca-\d"),                     # cualquier comentario con la versión del programa
)


def normalize(path, slicer_root=None):
    out = []
    with open(path, encoding="utf-8", errors="replace", newline="") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if any(p.match(line) for p in VOLATILE):
                continue
            out.append(line)
    return "\n".join(out) + "\n"


def compare(name, generated, reference_dir, update, allow_missing, diff_dir):
    ref = os.path.join(reference_dir, name + ".gcode")
    text = normalize(generated)
    if update:
        os.makedirs(reference_dir, exist_ok=True)
        with open(ref, "w", newline="\n") as f:
            f.write(text)
        print("  [%s] referencia escrita (%d líneas)" % (name, text.count("\n")))
        return True
    if not os.path.isfile(ref):
        print("  [%s] SIN REFERENCIA (%s)" % (name, "se ignora" if allow_missing else "fallo"))
        return allow_missing
    with open(ref, encoding="utf-8", newline="") as f:
        ref_text = f.read()
    if ref_text == text:
        print("  [%s] IDÉNTICO (%d líneas)" % (name, text.count("\n")))
        return True
    diff = list(difflib.unified_diff(ref_text.splitlines(), text.splitlines(),
                                     "reference/%s.gcode" % name, "generated/%s.gcode" % name, lineterm=""))
    added = sum(1 for l in diff if l.startswith("+") and not l.startswith("+++"))
    removed = sum(1 for l in diff if l.startswith("-") and not l.startswith("---"))
    print("  [%s] DIFERENTE: -%d/+%d líneas (referencia %d, generado %d)"
          % (name, removed, added, ref_text.count("\n"), text.count("\n")))
    for l in diff:
        if re.match(r"^[-+]; [a-z_0-9]+ = ", l) or re.match(r"^[-+]; (total|estimated|filament used|filament:)", l):
            print("    " + l[:160])
    os.makedirs(diff_dir, exist_ok=True)
    with open(os.path.join(diff_dir, name + ".diff"), "w", newline="\n") as f:
        f.write("\n".join(diff) + "\n")
    return False


# ----------------------------------------------------------------------------- main

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--slicer", help="AppImage, directorio squashfs-root, AppRun o binario snapmaker-orca")
    ap.add_argument("--profiles", default=os.path.join(REPO, "resources", "profiles"),
                    help="árbol de perfiles con Snapmaker.json y Snapmaker/ (por defecto el del repositorio)")
    ap.add_argument("--out", default=os.path.join(HERE, "out"), help="directorio de trabajo (G-code, logs, diffs)")
    ap.add_argument("--reference", default=REFERENCE_DIR, help="directorio de referencias")
    ap.add_argument("--cases", nargs="*", help="subconjunto de casos (por defecto todos)")
    ap.add_argument("--update", action="store_true", help="regenera las referencias con los G-code de esta ejecución")
    ap.add_argument("--allow-missing", action="store_true", help="una referencia que falta no es un fallo")
    ap.add_argument("--list", action="store_true", help="enumera los casos y termina")
    ap.add_argument("--debug", type=int, default=2, help="nivel de --debug de la CLI (0 fatal .. 5 trace)")
    ap.add_argument("--timeout", type=int, default=1800, help="segundos por caso")
    ap.add_argument("--models-only", action="store_true", help="solo genera los modelos en <out>/models")
    args = ap.parse_args(argv)

    names = args.cases or list(CASES)
    unknown = [n for n in names if n not in CASES]
    if unknown:
        ap.error("casos desconocidos: %s (usa --list)" % ", ".join(unknown))
    if args.list:
        for n in names:
            c = CASES[n]
            print("%-24s %-20s %d filamento(s) %s" % (n, c["model"], c["filaments"],
                  " ".join("%s=%s" % kv for kv in sorted(c["settings"].items()))))
        return 0

    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    models = generate_models(os.path.join(out, "models"))
    if args.models_only:
        print("modelos en %s" % os.path.join(out, "models"))
        return 0
    if not args.slicer:
        ap.error("falta --slicer")

    slicer = find_slicer(args.slicer, out)
    profiles = profile_paths(os.path.abspath(args.profiles))
    profiles_mixed = None
    if any(CASES[n]["mixed"] for n in names):
        profiles_mixed = profile_paths(mixed_profiles(os.path.abspath(args.profiles), out))
    datadir = os.path.join(out, "datadir")  # --datadir limpio: no depende de ~/.config
    shutil.rmtree(datadir, ignore_errors=True)
    os.makedirs(datadir)
    gcode_dir = os.path.join(out, "gcode")
    diff_dir = os.path.join(out, "diff")
    shutil.rmtree(gcode_dir, ignore_errors=True)
    shutil.rmtree(diff_dir, ignore_errors=True)
    os.makedirs(gcode_dir)
    print("slicer:     %s" % slicer)
    print("perfiles:   %s" % os.path.abspath(args.profiles))
    print("referencia: %s" % args.reference)

    failed_slice, failed_cmp = [], []
    for n in names:
        gcode = slice_case(n, CASES[n], slicer, profiles_mixed if CASES[n]["mixed"] else profiles, models, out,
                           datadir, args.debug, args.timeout)
        if gcode is None:
            failed_slice.append(n)
            continue
        # Copia normalizada para el artefacto de la integración continua.
        with open(os.path.join(gcode_dir, n + ".gcode"), "w", newline="\n") as f:
            f.write(normalize(gcode))
        if not compare(n, gcode, args.reference, args.update, args.allow_missing, diff_dir):
            failed_cmp.append(n)

    print()
    if failed_slice:
        print("FALLO de laminado: %s" % ", ".join(failed_slice))
        return 2
    if failed_cmp:
        print("DIFERENCIAS en: %s (diffs en %s)" % (", ".join(failed_cmp), diff_dir))
        print("Si el cambio es intencionado, regenera las referencias con --update y súbelas en el mismo PR.")
        return 1
    print("OK: %d caso(s) %s" % (len(names), "actualizados" if args.update else "idénticos a la referencia"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
