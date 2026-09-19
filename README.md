# Editor-Parcial2 — Compresor Huffman Concurrente integrado al editor `edi` (SO2026B)

Evaluación práctica (segundo parcial) de Sistemas Operativos: **Alternativa 1
— Compresor de Archivos Huffman Concurrente Integrado al Editor**. Este
proyecto parte del editor de texto CLI construido en el
[Parcial 1](https://github.com/davidosoriot/soparcial1) y le agrega un
módulo de compresión/descompresión Huffman que corre **en segundo plano**
usando hilos POSIX (`pthread`), con paralelismo real en las fases que lo
permiten y sincronización explícita (mutex + variables de condición) donde
el enunciado lo exige.

**Restricciones de la implementación:** solo `pthread` (nada de OpenMP), los
hilos y sus estructuras de trabajo viven en el **heap**, y lo único que se
paraleliza es la **iteración** sobre los bloques del archivo (conteo de
frecuencias y codificación/decodificación); la construcción del árbol de
Huffman en sí es secuencial.

## Estructura del repositorio

```
Editor-Parcial2/
├── Makefile
├── editor/                    # el editor "edi" (heredado del Parcial 1 + integración)
│   ├── editor.c / editor.h    # comandos o p a d i s m y x q + NUEVOS: c u j
│   └── Makefile               # ahora compila compress/ y linkea -pthread
├── compress/                  # NUEVO: modulo de compresion Huffman concurrente
│   ├── huffman.h / huffman.c  # arbol de Huffman, tabla de codigos, bit I/O (secuencial)
│   └── pcompress.h / pcompress.c  # orquestacion concurrente + jobs en 2do plano
├── shell/                     # shell educativo (sin cambios funcionales; invoca
│                               # el binario del editor via fork+execve, que ya
│                               # incluye 'c'/'u'/'j' automaticamente)
└── tests/
    ├── test_editor.sh         # suite del Parcial 1 (24 casos) - sigue en 24/24
    └── test_compress.sh       # NUEVO: 10 casos para la compresion concurrente
```

## Compilar y ejecutar

```bash
make                     # compila editor/editor (con -pthread) y shell/eafitOS
./tests/test_editor.sh   # regresion del parcial 1 (24/24)
./tests/test_compress.sh # pruebas de la compresion concurrente (10/10)

./editor/editor archivo.txt     # editor standalone
./shell/eafitOS                 # shell; dentro: "editor archivo.txt"

make clean
```

Dentro del editor:

```
edi> o archivo.txt
edi> c archivo.txt.huf        # comprime EN SEGUNDO PLANO; el prompt vuelve de inmediato
edi> j                        # consulta el progreso bajo demanda
edi> u archivo.txt.huf salida.txt   # descomprime (tambien en segundo plano)
edi> q                        # si hay una tarea corriendo, espera (join) antes de salir
```

Mientras una tarea está activa sobre un archivo, el propio editor imprime su
avance sin que el usuario tenga que pedirlo (hilo "ticker", ver más abajo), y
cualquier intento de editar (`a`/`d`/`i`/`x`) **ese mismo archivo** se
rechaza con un mensaje explícito hasta que la tarea termine.

## 1. Arquitectura de la concurrencia

### 1.1 Por qué el job vive en el heap (no es un capricho del enunciado)

`cmd_compress`/`cmd_decompress` deben **retornar de inmediato** al bucle
`edi>` — esa es la definición misma de "tarea en segundo plano" que pide el
enunciado. `job_start_compress()` lanza el **hilo coordinador** y regresa
sin esperarlo. Si el `struct CompressJob` (con sus mutex, su progreso, sus
rutas de archivo) viviera en el *stack* de `cmd_compress`, esa memoria
quedaría inválida en el instante en que `cmd_compress` retorna — pero el
hilo coordinador sigue vivo y sigue escribiendo sobre esa misma memoria
minutos después. Por eso:

- El `CompressJob` se reserva con `calloc` (`pcompress.c: job_alloc_common`).
- El pool de hilos trabajadores de cada fase (`pthread_t *tids = malloc(...)`)
  también es dinámico, porque su tamaño depende en tiempo de ejecución del
  número de núcleos disponibles y de cuántos bloques tiene el archivo.
- Cada argumento de hilo trabajador (`FreqWorkerArg`, `EncodeShared`,
  `DecodeShared`) es una estructura propia en el heap, liberada explícitamente
  tras el `pthread_join` correspondiente.

### 1.2 Fase 1 — Conteo de frecuencias en paralelo (con reducción local)

Cada hilo trabajador cuenta bytes **en su propia copia local** de
`freq[256]` (sin ningún mutex durante el conteo, porque ningún otro hilo
toca esa memoria). Los bloques se reparten dinámicamente con un contador
compartido protegido por un mutex corto (`TaskCounter` /
`task_counter_next`), de forma que ningún hilo se queda ocioso si otro
termina antes. Al terminar todos, el hilo coordinador **reduce** (suma) las
N tablas locales en una sola tabla global — exactamente la técnica de
"reducción local" que sugiere el enunciado, sin necesidad de un mutex por
cada incremento de frecuencia.

> Esta es la iteración que se paraleliza en esta fase: el recorrido byte a
> byte de cada bloque (`for (size_t i = 0; i < len; i++) arg->freq[buf[i]]++;`
> en `pcompress.c`).

### 1.3 Construcción del árbol — deliberadamente secuencial

Construir un árbol de Huffman a partir de 256 frecuencias es, en el peor
caso, O(256²) con la selección ingenua de dos mínimos que usa
`huffman_build_tree()`. Frente al tiempo de E/S de un archivo real esto es
imperceptible; paralelizarlo no aportaría nada medible y complicaría la
lógica sin necesidad. El árbol se **serializa explícitamente** en el
encabezado del `.huf` (formato descrito abajo) en vez de reconstruirse a
partir de las frecuencias en la descompresión — así se elimina por completo
cualquier riesgo de desempate no determinista entre compresor y
descompresor.

### 1.4 Fase 2 — Codificación paralela + escritura ordenada (productor-consumidor)

Esta es la parte central exigida por la rúbrica ("un hilo coordinador o cola
de salida sincronizada... en el orden secuencial correcto, sin bloqueos
activos"):

- Los **hilos trabajadores son productores**: toman el siguiente bloque
  pendiente (mismo reparto dinámico que en la fase 1, con su propio mutex
  `task_mutex`), lo codifican con la tabla de códigos —compartida y de
  **solo lectura**, por lo que leerla desde varios hilos a la vez es
  seguro sin ningún lock— y dejan el resultado en `slots[idx]`.
- El **hilo coordinador es el único consumidor**: recorre las casillas en
  orden `0,1,2,...` y, para cada una, hace `pthread_cond_wait()` — **bloquea
  el hilo sin consumir CPU** — hasta que esa casilla puntual esté lista,
  sin importar el orden real en que los trabajadores terminaron. Así el
  archivo de salida queda en el orden correcto aunque el trabajo interno se
  haya hecho en paralelo y fuera de orden.

Esto es una variante clásica de productor-consumidor con **restricción de
orden**, implementada con `pthread_mutex_t` + `pthread_cond_t` — sin
`while(1) { }` de espera activa en ningún punto.

### 1.5 Descompresión — paralela sin necesidad de orden

El encabezado guarda, para cada bloque, su tamaño original y su cantidad de
bits comprimidos. Antes de lanzar los trabajadores, el coordinador puede
calcular por adelantado el offset exacto de cada bloque en el archivo de
**salida** (suma acumulada de tamaños originales). Con ese offset conocido,
cada trabajador decodifica su bloque de forma totalmente independiente y
escribe con `pwrite()` posicional — sin mutex ni variable de condición,
porque cada uno escribe en una región disjunta del archivo. El árbol de
Huffman se comparte entre todos los hilos, pero solo se **lee**, nunca se
modifica, durante la decodificación.

### 1.6 Prevención de condiciones de carrera editor ↔ compresión

`check_job_lock()` (en `editor.c`) es el único punto de verificación: antes
de cualquier operación que modifique el archivo en disco (`a`, `d`, `i`,
`x`→`insert_at`), se comprueba si hay una tarea activa cuyo `input_path` o
`output_path` coincida con el archivo abierto (`job_blocks_path()` en
`pcompress.c`). Si coincide, la edición se rechaza con un mensaje explícito
en vez de proceder — así se evita que el editor trunque/desplace bytes del
archivo mientras los hilos trabajadores lo están leyendo con `pread()`.

### 1.7 Progreso en tiempo real sin bloquear al usuario

Cada job lanza, además del hilo coordinador, un **hilo "ticker"**
(`progress_ticker` en `pcompress.c`) que cada ~400 ms imprime el progreso
actual sin que el usuario tenga que pedirlo, y `j` lo consulta bajo demanda
en cualquier momento. Un mutex de impresión (`ui_print_lock`/`ui_print_unlock`)
evita que el ticker y el prompt `edi>` del hilo principal intercalen texto a
mitad de línea. *Nota:* el muestreo del ticker (`nanosleep` + lectura) es
solo una comodidad de interfaz; no es el mecanismo de sincronización que se
evalúa — ese vive en los mutex/condvar de las secciones 1.2, 1.4 y 1.5 sobre
los datos realmente compartidos.

## 2. Formato del archivo `.huf`

```
[u32 magic="HUFP"] [u32 version] [u32 num_blocks] [u64 tamaño_original]
[u32 block_size_nominal] [u32 tree_bit_count] [tree_bit_count bits, arbol en preorden]
por cada bloque, en orden:
  [u64 original_len] [u64 bit_count] [ceil(bit_count/8) bytes de payload]
```

El árbol se serializa en preorden (1 bit de tipo + 8 bits de símbolo si es
hoja), lo que permite reconstruirlo exactamente sin ambigüedad. Cada bloque
se alinea a byte de forma independiente, lo que permite procesarlo (codificar
o decodificar) sin depender de ningún otro bloque.

## 3. Comandos nuevos del editor

| Comando | Descripción | Syscalls/primitivas clave |
|---|---|---|
| `c [archivo_salida]` | Comprime el archivo abierto en segundo plano (por defecto `<archivo>.huf`) | `pthread_create`, `pread`, `pthread_mutex_t`, `pthread_cond_t` |
| `u <archivo.huf> [salida]` | Descomprime en segundo plano | `pthread_create`, `pread`/`pwrite` posicional |
| `j` | Consulta el estado/progreso de la tarea activa | `pthread_mutex_t` (getters protegidos) |

## 4. Mapeo con la rúbrica de evaluación (Alternativa 1)

| Criterio (rúbrica) | Dónde se cumple |
|---|---|
| Concurrencia en Huffman (25 pts) | §1.2 (conteo paralelo) y §1.4 (codificación paralela + salida ordenada) en `pcompress.c` |
| Mecanismos de sincronización (25 pts) | `task_mutex` (reparto dinámico), `out_mutex`+`out_cond` (orden de escritura sin espera activa), `state_mutex` (estado del job), `stat_mutex` (progreso de decodificación) |
| Integración con el editor (20 pts) | `editor.c`: `cmd_compress`/`cmd_decompress` no bloquean el REPL; `check_job_lock` evita condiciones de carrera; ticker de progreso en tiempo real |
| Corrección algorítmica (15 pts) | `tests/test_compress.sh`: roundtrip con `md5sum` sobre 5 casos (vacío, símbolo único, texto, binario aleatorio, multi-bloque) — ver §5 |
| Manejo de errores y recursos (15 pts) | `job_join_and_free` (join limpio, sin hilos huérfanos, incluso al salir con `q`), liberación de todo `malloc`/`calloc` en cada rama de error, verificado con AddressSanitizer (§5) |

## 5. Metodología de pruebas

```bash
./tests/test_editor.sh     # 24/24 — regresion del Parcial 1 (no se toco nada de su logica)
./tests/test_compress.sh   # 10/10 — roundtrip md5 + no-bloqueo + rechazo de ediciones concurrentes
```

Verificación adicional durante el desarrollo (no forma parte del build por
defecto, requiere `clang`/`gcc` con soporte de sanitizers):

```bash
# Sin condiciones de carrera detectadas por ThreadSanitizer en un ciclo
# completo de compresion+descompresion de un archivo de 40 MB con un
# intento de edicion concurrente de por medio:
gcc -std=gnu99 -D_GNU_SOURCE -pthread -Icompress -fsanitize=thread \
    -o editor_tsan editor/editor.c compress/huffman.c compress/pcompress.c -pthread -fsanitize=thread

# Sin fugas de memoria ni comportamiento indefinido (AddressSanitizer + UBSan)
# en el mismo escenario:
gcc -std=gnu99 -D_GNU_SOURCE -pthread -Icompress -fsanitize=address,undefined \
    -o editor_asan editor/editor.c compress/huffman.c compress/pcompress.c -pthread -fsanitize=address,undefined
```

(En WSL, ThreadSanitizer requiere ASLR deshabilitado para arrancar:
`setarch $(uname -m) -R ./editor_tsan`; es una particularidad del entorno de
desarrollo, no del programa.)

## 6. Pendiente para el equipo (fuera del alcance de este código)

- Documento de sustentación (PDF) y video explicativo: usar las secciones de
  arriba como base técnica, pero la argumentación debe quedar en palabras
  propias del equipo.
- Ajustar el número de integrantes/autoría según la conformación real del
  equipo para este parcial (el enunciado permite hasta 5).

## Referencia: diseño del editor base (Parcial 1)

El editor `edi` (comandos `o p a d i s m y x q`) y su integración con el
shell educativo se documentan en detalle en el
[README del Parcial 1](https://github.com/davidosoriot/soparcial1/blob/main/README.md):
diseño del índice de offsets por línea, estrategia de inserción/borrado con
`lseek`+buffers dinámicos, y la decisión de exponer el editor como categoría
propia del shell vía `fork`+`execve`. Ese diseño no se modificó en este
parcial; solo se le agregó el módulo `compress/` y los tres comandos nuevos.
