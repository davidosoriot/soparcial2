#!/usr/bin/env bash
#
# Script de pruebas del editor "edi" (equipo de 3 - SO2026B).
# Ejercita el binario standalone (editor/editor) enviandole una secuencia de
# comandos por stdin (como haria un usuario real) y valida:
#   1. lo que el editor imprime en consola (grep sobre la salida capturada), y
#   2. el contenido REAL que quedo escrito en el archivo en disco (diff),
#      que es la prueba de fondo de que open/read/write/lseek/ftruncate se
#      usaron correctamente.
#
# Uso: ./tests/test_editor.sh   (ejecutar desde la raiz del proyecto o desde tests/)

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
EDITOR_BIN="$ROOT_DIR/editor/editor"
WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

TOTAL=0
FAILED=0

pass() { echo "  [OK]   $1"; }
fail() { echo "  [FAIL] $1"; FAILED=$((FAILED + 1)); }
check() {
    # check <descripcion> <condicion-como-comando-shell>
    TOTAL=$((TOTAL + 1))
    if eval "$2"; then pass "$1"; else fail "$1"; fi
}

if [ ! -x "$EDITOR_BIN" ]; then
    echo "No se encontro '$EDITOR_BIN'. Compile primero con: make -C '$ROOT_DIR' all"
    exit 1
fi

echo "=== Suite de pruebas: edi (comandos base + Nivel 2 + Nivel 3) ==="
echo

# --------------------------------------------------------------------------
echo "-- 1. Comandos base: o, a, p, d, q --"
F1="$WORKDIR/base.txt"
OUT1=$(printf 'a linea uno\na linea dos\na linea tres\np\np 2\nd 2\np\nq\n' | "$EDITOR_BIN" "$F1" 2>&1)

check "o crea el archivo en disco"                 "[ -f '$F1' ]"
check "a agrega lineas (p sin args las muestra)"    "echo \"\$OUT1\" | grep -q 'linea uno'"
check "p <n> imprime solo la linea pedida"          "echo \"\$OUT1\" | grep -qE '2: linea dos'"
check "d elimina la linea correcta"                  "printf 'linea uno\nlinea tres\n' | diff -q - '$F1' >/dev/null"
check "q termina sin dejar el archivo corrupto"      "[ \"\$(wc -l < '$F1')\" = 2 ]"

# --------------------------------------------------------------------------
echo
echo "-- 2. Nivel 2: insercion arbitraria (i) y busqueda (s) --"
F2="$WORKDIR/nivel2.txt"
OUT2=$(printf 'a alfa\na gamma\ni 2 beta\ns a\nq\n' | "$EDITOR_BIN" "$F2" 2>&1)

check "i inserta desplazando el resto sin perder datos" \
    "printf 'alfa\nbeta\ngamma\n' | diff -q - '$F2' >/dev/null"
check "s encuentra todas las coincidencias (alfa, beta, gamma contienen 'a')" \
    "echo \"\$OUT2\" | grep -cE '[0-9]+: (alfa|beta|gamma)' | grep -q '^3$'"
check "s reporta 0 coincidencias cuando no hay match" \
    "printf 'a xyz\ns noexiste\nq\n' | '$EDITOR_BIN' '$WORKDIR/nomatch.txt' 2>&1 | grep -q 'Sin coincidencias'"

# --------------------------------------------------------------------------
echo
echo "-- 3. Nivel 3: metadatos (m) y portapapeles (y/x) --"
F3="$WORKDIR/nivel3.txt"
OUT3=$(printf 'a uno\na dos\ny 1\nx 3\nm\nq\n' | "$EDITOR_BIN" "$F3" 2>&1)

check "y/x copian y pegan la linea correcta" \
    "printf 'uno\ndos\nuno\n' | diff -q - '$F3' >/dev/null"
check "m reporta el inodo real via fstat" \
    "echo \"\$OUT3\" | grep -q 'Inodo:'"
check "m reporta el tamano real del archivo" \
    "echo \"\$OUT3\" | grep -q \"Tamano: *\$(stat -c%s '$F3') bytes\""

# --------------------------------------------------------------------------
echo
echo "-- 4. Casos borde y manejo de errores (no debe crashear ni corromper) --"

check "comando sobre archivo aun no abierto se rechaza con mensaje" \
    "printf 'p\nq\n' | '$EDITOR_BIN' 2>&1 | grep -q 'no hay archivo abierto'"

F4="$WORKDIR/edge.txt"
printf 'a linea1\n' | "$EDITOR_BIN" "$F4" >/dev/null 2>&1
OUT4=$(printf 'p 99\nd 99\ni 99 x\np abc\nq\n' | "$EDITOR_BIN" "$F4" 2>&1)
check "p con linea fuera de rango no crashea y avisa"   "echo \"\$OUT4\" | grep -qi 'no existe'"
check "d con linea fuera de rango no crashea y avisa"   "echo \"\$OUT4\" | grep -qi 'no existe'"
check "i con posicion invalida no crashea y avisa"      "echo \"\$OUT4\" | grep -qi 'maximo permitido'"
check "p con argumento no numerico se rechaza"          "echo \"\$OUT4\" | grep -qi 'invalido'"
check "el archivo no se corrompio tras los errores"     "printf 'linea1\n' | diff -q - '$F4' >/dev/null"

check "p sobre archivo vacio no crashea" \
    "printf 'p\nq\n' | '$EDITOR_BIN' '$WORKDIR/empty.txt' 2>&1 | grep -q 'archivo vacio'"

check "'o' sobre ruta invalida reporta error sin crashear (exit 0)" \
    "printf 'q\n' | '$EDITOR_BIN' '/ruta/inexistente/x.txt' >/dev/null 2>&1; [ \$? -eq 0 ]"

F5="$WORKDIR/nonewline.txt"
printf 'sin salto final' > "$F5"
printf 'a nueva\nq\n' | "$EDITOR_BIN" "$F5" >/dev/null 2>&1
check "append respeta archivos sin '\\n' final (no concatena lineas)" \
    "printf 'sin salto final\nnueva\n' | diff -q - '$F5' >/dev/null"

check "Ctrl+D (EOF) en el prompt cierra sin colgarse" \
    "printf '' | timeout 3 '$EDITOR_BIN' '$WORKDIR/eof.txt' >/dev/null 2>&1; [ \$? -eq 0 ]"

# --------------------------------------------------------------------------
echo
echo "-- 5. Integracion con el shell (fork/execve, categoria 'editor') --"
SHELL_BIN="$ROOT_DIR/shell/eafitOS"
if [ -x "$SHELL_BIN" ]; then
    F6="$WORKDIR/via_shell.txt"
    OUT6=$(printf 'editor %s\nexit\n' "$F6" | "$SHELL_BIN" 2>&1)
    check "el comando 'editor' del shell aparece en 'help'" \
        "printf 'help editor\nexit\n' | '$SHELL_BIN' 2>&1 | grep -q 'Lanza'"
    check "'editor <archivo>' crea el archivo via fork/execve" "[ -f '$F6' ]"
    check "el shell reporta el codigo de salida del editor" \
        "echo \"\$OUT6\" | grep -q 'Codigo de salida'"
else
    echo "  (shell/eafitOS no compilado; omitiendo pruebas de integracion. Ejecute 'make all' en la raiz.)"
fi

# --------------------------------------------------------------------------
echo
echo "=== Resultado: $((TOTAL - FAILED))/$TOTAL pruebas superadas ==="
if [ "$FAILED" -gt 0 ]; then
    echo "$FAILED prueba(s) fallaron."
    exit 1
fi
exit 0
