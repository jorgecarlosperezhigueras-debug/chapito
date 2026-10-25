# kotoGBA — desarrollo recuperable

kotoGBA permite aprender japonés jugando a Pokémon Rojo Fuego japonés Rev. 1
(`BPRJ1`), con traducción contextual al castellano, vocabulario, gramática y kanjis.

## Estado vigente

| Parte | Versión comprobada | Estado |
| --- | --- | --- |
| Entrega para la SD | **B19/B10** | Última entrega instalable validada |
| Código del ejecutable | **B19** | Recompilado desde cero, NDS idéntico al entregado |
| Memoria de desarrollo | **B12 — 50/36** | Tanda 000–007 adoptada; no instalable |
| Hito B / bloque 2 | Abiertos | La migración no los da por terminados |

El estado detallado está en [project/STATE.json](project/STATE.json), las huellas
en [project/CHECKPOINTS.json](project/CHECKPOINTS.json) y las comprobaciones
en [evidence/migration-20261025/](evidence/migration-20261025/).

## Empezar desde otra sesión o herramienta

Lee [AGENTS.md](AGENTS.md) y [docs/RECUPERACION.md](docs/RECUPERACION.md).
`CLAUDE.md` remite al mismo contrato de continuidad.

```sh
git clone https://github.com/jorgecarlosperezhigueras-debug/chapito.git
cd chapito
git switch kotogba-persistencia-20261025
python3 -m unittest discover -s tests/recovery -v
python3 -m unittest discover -s tests/build -v
python3 -m unittest discover -s tests/linguistic -v
python3 tools/recovery/checkpoint.py contract
python3 tools/build/b19.py check-sources
python3 linguistic/verify.py
```

Python 3.12 o posterior permite ejecutar estas comprobaciones sin instalar
dependencias Python adicionales. La CI pública ejecuta las mismas suites y
conserva el commit y los resultados, incluso cuando una prueba falla.

## Compilar B19

`current/` conserva 43 archivos propios y el manifiesto de las 526 fuentes
congeladas. El constructor verifica fuentes, SDK y dependencias antes de ejecutar
y exige que el NDS y `arm9.bin` reproduzcan B19 exactamente.

```sh
python3 tools/build/b19.py build \
  --inputs /ruta/privada/kotoGBA_B19_PRIVATE_BUILD_INPUTS_20261025.zip \
  --output /ruta/privada/nueva/resultado_B19 --jobs 3
```

El paquete mínimo puede regenerarse desde el checkpoint B19/B10 conservado;
[current/README.md](current/README.md) documenta el comando `make-inputs`.
La compilación completa se ha ejecutado localmente y está acreditada en el
informe `build-b19.json`. La CI pública no dispone de los inputs privados y no
debe confundirse con esa compilación completa.

## Memoria y trabajo por lotes

[linguistic/](linguistic/) conserva 28 fuentes y esquemas verificados de B12,
incluidos el importador y el código de exportación. Los archivos privados están
identificados por huellas. La revisión vigente es **50/36**; volver a B11 como
base perdería el avance posterior comprobado.

Cada lote necesita el payload real, sus comprobaciones y un checkpoint
recuperable. El diario rechaza duplicados, solapamientos y recibos corruptos;
un recibo sin sus correcciones no es una copia del trabajo. No abrir el siguiente
lote mientras el anterior sólo exista en una sesión temporal.

El guardado duradero del ZIP final B12 está pendiente de resolver la aprobación
del destino privado. El código conserva esta limitación expresamente.

## Estructura

| Ruta | Contenido |
| --- | --- |
| `current/` | Fuentes propias B19 y dependencias fijadas |
| `linguistic/` | Código de memoria B12 y manifiestos |
| `tools/build/` | Recuperación y compilación reproducible |
| `tools/recovery/` | Verificación de archivos y diario de lotes |
| `tests/` | Pruebas con datos sintéticos |
| `project/` | Autoridad, estado y checkpoints |
| `evidence/` | Resultados de verificaciones realizadas |

Las carpetas históricas `patch_files/`, `packages/` y los scripts V0.x se mantienen
como antecedentes. Su descripción anterior está en
[docs/HISTORICO_V04.md](docs/HISTORICO_V04.md). No son la fuente vigente B19.

Este repositorio público conserva código y metadatos. La ROM, BIOS, partidas,
libros, tipografías, bases y checkpoints privados permanecen separados.
