#!/usr/bin/env bash
# ====================================================================================
# Suite de pruebas: compresion Huffman concurrente en segundo plano (Parcial 2,
# Alternativa 1). Verifica:
#   1. Correccion algoritmica: roundtrip comprimir->descomprimir identico al
#      original, comprobado con md5sum (segun pide la rubrica), sobre varios
#      casos borde (archivo vacio, un unico simbolo repetido, texto normal,
#      binario aleatorio y un archivo grande de varios bloques).
#   2. Concurrencia real: el editor sigue respondiendo (no se bloquea) mientras
#      la compresion corre en segundo plano.
#   3. Prevencion de condiciones de carrera: una edicion sobre el archivo que
#      esta siendo comprimido debe ser rechazada mientras el job este activo.
#   4. Manejo de errores: descomprimir un archivo que no es .huf no debe
#      crashear, debe reportar error limpio.
#
# Uso: ./tests/test_compress.sh   (ejecutar desde la raiz del repo, tras `make`)
# ====================================================================================
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BIN="$ROOT_DIR/editor/editor"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAIL=0

ok()   { PASS=$((PASS+1)); echo "  [OK]   $1"; }
bad()  { FAIL=$((FAIL+1)); echo "  [FAIL] $1"; }

if [ ! -x "$BIN" ]; then
    echo "No se encontro el binario '$BIN'. Ejecute 'make' primero." >&2
    exit 1
fi

run_editor() {
    # run_editor <script_de_comandos> <archivo_log>
    timeout 60 "$BIN" < "$1" > "$2" 2>&1
    echo $?
}

roundtrip_case() {
    local name="$1" src="$2"
    local huf="$WORK/${name}.huf"
    local dec="$WORK/${name}.dec"
    local clog="$WORK/${name}_c.log"
    local dlog="$WORK/${name}_d.log"

    printf 'o %s\nc %s\nq\n' "$src" "$huf" > "$WORK/${name}_c.cmd"
    printf 'u %s %s\nq\n' "$huf" "$dec" > "$WORK/${name}_d.cmd"

    run_editor "$WORK/${name}_c.cmd" "$clog" > /dev/null
    if grep -q "ERROR" "$clog"; then
        bad "$name: la compresion reporto error (ver $clog)"
        return
    fi
    run_editor "$WORK/${name}_d.cmd" "$dlog" > /dev/null
    if grep -q "ERROR" "$dlog"; then
        bad "$name: la descompresion reporto error (ver $dlog)"
        return
    fi

    if [ ! -f "$dec" ]; then
        bad "$name: no se genero el archivo descomprimido"
        return
    fi

    local md5_src md5_dec
    md5_src=$(md5sum "$src" | awk '{print $1}')
    md5_dec=$(md5sum "$dec" | awk '{print $1}')
    if [ "$md5_src" = "$md5_dec" ]; then
        local sz_src sz_huf
        sz_src=$(stat -c%s "$src")
        sz_huf=$(stat -c%s "$huf")
        ok "$name: roundtrip identico (md5 coincide, ${sz_src} -> ${sz_huf} bytes)"
    else
        bad "$name: el archivo descomprimido NO coincide con el original (md5 distinto)"
    fi
}

echo "=== Suite de pruebas: compresion Huffman concurrente (Parcial 2) ==="
echo ""
echo "-- 1. Correccion algoritmica: roundtrip comprimir -> descomprimir (md5) --"

# Caso borde: archivo vacio
: > "$WORK/empty.txt"
roundtrip_case "empty" "$WORK/empty.txt"

# Caso borde: un unico simbolo repetido (fuerza el arbol de 2 hojas sintetico)
printf 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa' > "$WORK/single.txt"
roundtrip_case "single_symbol" "$WORK/single.txt"

# Texto normal, un solo bloque
printf 'hello world\nsegunda linea con texto de prueba\n' > "$WORK/small.txt"
roundtrip_case "small_text" "$WORK/small.txt"

# Binario aleatorio de varios MB (fuerza varios bloques de 1 MiB, alta entropia)
head -c 3500000 /dev/urandom > "$WORK/random.bin"
roundtrip_case "random_binary" "$WORK/random.bin"

# Texto grande y repetitivo de varios bloques (alfabeto reducido, buena compresion)
python3 - "$WORK/big.txt" <<'PYEOF'
import random, sys
random.seed(7)
words = ["the","quick","brown","fox","jumps","over","lazy","dog",
         "sistemas","operativos","concurrencia","hilos","mutex","semaforo"]
with open(sys.argv[1], "w") as f:
    for _ in range(400000):
        f.write(random.choice(words) + " ")
PYEOF
roundtrip_case "big_multiblock" "$WORK/big.txt"

echo ""
echo "-- 2. El editor no se bloquea mientras comprime, y rechaza ediciones concurrentes --"

BIGFILE="$WORK/race.txt"
python3 - "$BIGFILE" <<'PYEOF'
import random, sys
random.seed(3)
words = ["the","quick","brown","fox","jumps","over","lazy","dog","concurrencia","hilos"]
with open(sys.argv[1], "w") as f:
    for _ in range(6000000):
        f.write(random.choice(words) + " ")
PYEOF

RACE_CMD="$WORK/race.cmd"
RACE_LOG="$WORK/race.log"
printf 'o %s\nc %s.huf\na intento de edicion durante compresion\nj\nq\n' "$BIGFILE" "$BIGFILE" > "$RACE_CMD"
run_editor "$RACE_CMD" "$RACE_LOG" > /dev/null

if grep -q "iniciada en segundo plano" "$RACE_LOG"; then
    ok "el comando 'c' retorna de inmediato (no bloquea el prompt)"
else
    bad "el comando 'c' no parece haber arrancado en segundo plano"
fi

if grep -q "archivo bloqueado" "$RACE_LOG"; then
    ok "la edicion concurrente sobre el archivo en compresion fue rechazada"
else
    bad "se esperaba que la edicion concurrente fuera rechazada (condicion de carrera no evitada)"
fi

if grep -q "Compresion en curso" "$RACE_LOG" || grep -q "Compresion completada" "$RACE_LOG"; then
    ok "'j' reporta el estado de la tarea bajo demanda"
else
    bad "'j' no reporto el estado esperado"
fi

if md5sum "$BIGFILE" "${BIGFILE}.dec" > /dev/null 2>&1; then :; fi
printf 'u %s.huf %s.dec\nq\n' "$BIGFILE" "$BIGFILE" > "$WORK/race_dec.cmd"
run_editor "$WORK/race_dec.cmd" "$WORK/race_dec.log" > /dev/null
if [ -f "${BIGFILE}.dec" ] && [ "$(md5sum "$BIGFILE" | awk '{print $1}')" = "$(md5sum "${BIGFILE}.dec" | awk '{print $1}')" ]; then
    ok "el archivo grande (multi-bloque) sobrevive intacto pese al intento de edicion concurrente"
else
    bad "el archivo grande no coincide tras el ciclo completo"
fi

echo ""
echo "-- 3. Manejo de errores: entrada invalida no debe crashear --"

printf 'u %s %s.dec\nq\n' "$WORK/small.txt" "$WORK/should_fail" > "$WORK/badinput.cmd"
rc=$(run_editor "$WORK/badinput.cmd" "$WORK/badinput.log")
if [ "$rc" = "0" ] && grep -q "ERROR" "$WORK/badinput.log"; then
    ok "descomprimir un archivo que no es .huf termina limpio y reporta error"
else
    bad "descomprimir un archivo invalido no reporto error limpio (exit=$rc)"
fi

echo ""
echo "=== Resultado: ${PASS}/$((PASS+FAIL)) pruebas superadas ==="
[ "$FAIL" -eq 0 ]
