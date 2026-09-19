# Guía paso a paso — demo de la sustentación (Parcial 2, Alternativa 1)

Esta guía muestra cómo compilar el proyecto y demostrar en vivo todas las
funcionalidades de la compresión Huffman concurrente integrada al editor
`edi`: paralelismo real, sincronización sin espera activa, ejecución en
segundo plano, prevención de condiciones de carrera y corrección algorítmica
verificable con checksums.

## 0. Clonar y compilar

```bash
git clone https://github.com/davidosoriot/soparcial2.git
cd soparcial2
make
```

Compila `editor/editor` (con `-pthread`) y `shell/eafitOS`.

## 1. Regresión: nada del Parcial 1 se rompió

```bash
./tests/test_editor.sh
```
→ `24/24 pruebas superadas`. Buen punto de partida para la sustentación:
demuestra que el editor base sigue intacto.

## 2. Suite automática de la compresión concurrente

```bash
./tests/test_compress.sh
```
→ `10/10 pruebas superadas`: roundtrip con `md5sum` (archivo vacío, símbolo
único, texto, binario aleatorio, multi-bloque), no-bloqueo del prompt, y
rechazo de ediciones concurrentes. Es el resumen "todo funciona"; los pasos
de abajo son para **mostrarlo en vivo**.

## 3. Compresión en vivo con un archivo pequeño (lo básico)

```bash
./editor/editor
```
Dentro del editor:
```
edi> o README.md
edi> c README.md.huf
edi> j
edi> q
```
Verás el mensaje "iniciada en segundo plano" y, al hacer `q`, el editor
espera (join) a que termine e imprime el tamaño original vs. comprimido.

## 4. Lo importante: demostrar que es realmente concurrente y en 2do plano

Genera un archivo grande para que la compresión tarde varios segundos (así
se ve el progreso en tiempo real y hay tiempo de escribir el siguiente
comando antes de que termine):

```bash
python3 -c "
import random
random.seed(1)
words=['the','quick','brown','fox','concurrencia','hilos','mutex','semaforo']
with open('grande.txt','w') as f:
    for _ in range(6000000): f.write(random.choice(words)+' ')
"
```

```bash
./editor/editor
```
```
edi> o grande.txt
edi> c grande.txt.huf
edi> a esto deberia ser rechazado
edi> j
edi> j
edi> q
```

Qué mostrar en pantalla:

- `c` regresa el prompt **de inmediato** (no se congela) — demuestra el
  *background worker*.
- El comando `a` es **rechazado** con
  `archivo bloqueado: hay una tarea en segundo plano...` — demuestra la
  protección contra condición de carrera.
- Sin que pidas nada, aparecen líneas `[job] comprimiendo ... NN%`
  intercaladas — el hilo "ticker" reportando progreso en tiempo real.
- `j` da el estado bajo demanda en cualquier momento.
- `q` espera (join) a que termine antes de salir — no deja hilos huérfanos.

## 5. Descompresión y verificación de integridad (prueba de corrección)

```bash
./editor/editor
```
```
edi> u grande.txt.huf grande.dec
edi> j
edi> q
```

Checksum que pide la rúbrica:
```bash
md5sum grande.txt grande.dec
```
Ambos hashes deben ser idénticos.

## 6. A través del shell integrador (integración con el Parcial 1)

```bash
./shell/eafitOS
```
```
eafitOS> editor grande.txt
edi> c grande.txt.huf
edi> q
eafitOS> exit
```
Muestra que el binario del editor —lanzado por `fork`+`execve` desde el
shell, igual que en el Parcial 1— trae la compresión incluida sin ningún
cambio en el shell.

## 7. (Opcional) Evidencia técnica de ausencia de condiciones de carrera

Para argumentar "sin condiciones de carrera" con algo más fuerte que "no se
cae":

```bash
gcc -std=gnu99 -D_GNU_SOURCE -pthread -Icompress -fsanitize=thread \
    -o editor_tsan editor/editor.c compress/huffman.c compress/pcompress.c -pthread -fsanitize=thread
./editor_tsan   # repetir compresion/descompresion de grande.txt
```

Si no reporta ningún `WARNING: ThreadSanitizer`, es evidencia directa (no
solo "corrió bien") de ausencia de data races. En WSL puede requerir
`setarch $(uname -m) -R ./editor_tsan` si ThreadSanitizer se queja del
mapeo de memoria al arrancar (particularidad del entorno, no del programa).

---

Con los pasos 3–5 ya se cubren en vivo los tres puntos más pesados de la
rúbrica (concurrencia, sincronización, integración); el paso 7 es el extra
que distingue una sustentación sólida si se quiere usar.
