# Recuperar y continuar kotoGBA

## Autoridad actual

La SD vigente es **B19/B10**. El desarrollo más reciente es **B12, memoria 50 /
runtime 36**, con la tanda 000–007 adoptada. B12 todavía no es instalable y el
bloque 2 sigue abierto. B11 39/27 es un punto anterior de recuperación.

El código propio B19 está en `current/`; el código lingüístico B12 está en
`linguistic/`. `project/STATE.json` recoge los bloqueos actuales. Las fuentes
V0.x retenidas en otras carpetas son antecedentes, no el ejecutable vigente.

## Recuperación del código

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

La CI pública ejecuta estas comprobaciones con el commit exacto de cada cambio.
La compilación completa se ha reproducido localmente; necesita inputs privados
y todavía no dispone de su suministro privado en GitHub Actions.

## Recuperación del ejecutable B19

El checkpoint B19/B10 existente consta de **once segmentos consecutivos de un
único ZIP**. Sus hashes están verificados en `evidence/migration-20261025/archives.json`.
Reconstruir en orden y comprobar el hash del archivo entero antes de extraerlo.
Las rutas y huellas canónicas están en `project/CHECKPOINTS.json`.

Una vez recuperada la raíz `Recuperacion_R38/r38_trabajo`:

```sh
python3 tools/build/b19.py make-inputs \
  --checkpoint-root /ruta/restaurada/Recuperacion_R38/r38_trabajo \
  --output /ruta/privada/nueva/kotoGBA_B19_PRIVATE_BUILD_INPUTS_20261025.zip
python3 tools/build/b19.py build \
  --inputs /ruta/privada/nueva/kotoGBA_B19_PRIVATE_BUILD_INPUTS_20261025.zip \
  --output /ruta/privada/nueva/resultado_B19 --jobs 3
```

`make-inputs` regenera exactamente el paquete mínimo de 54.993.645 bytes.
El constructor recompone las 526 fuentes en una carpeta nueva, verifica los
inputs, compila libtwl/ARM7/ARM9 y comprueba el NDS y `arm9.bin`. Rechaza salidas
existentes. No modifica las memorias ni necesita una ROM o BIOS comercial.

La reproducción actual del NDS tiene 572.416 bytes y SHA-256
`1cd6128200e6a08824202681868f500a085971ccbb16afae884d7e30774e22ad`.
La evidencia del build y de la regeneración del paquete está versionada. Los
ELF de depuración pueden variar por las rutas; no se usan como identidad del NDS.

## Recuperación de B12

El checkpoint final es
`kotoGBA_B12_TANDA_000_007_ADOPTADA_NO_INSTALABLE_20261025.zip`,
513.632.421 bytes. Su hash, SQLite 50/36 y par de runtime se han comprobado
directamente. **Su copia duradera privada sigue pendiente de aprobación**;
no confundir el hash registrado en Git con la conservación del archivo.

Cuando ese ZIP esté disponible, recuperar una copia privada y ejecutar:

```sh
python3 linguistic/verify.py --private-project /ruta/restaurada/Recuperacion_R38/r38_trabajo
```

[../linguistic/README.md](../linguistic/README.md) documenta los comandos y las
58 dependencias privadas. Conservar las guardas del exportador, el sello v2 y
los adaptadores privados exactos. No sustituirlos por datos genéricos ni
modificar una guarda para exportar con entradas ausentes.

## Lotes y recuperación tras un corte

El mecanismo de adopción B12 conserva el paquete completo en SQLite dentro de
la transacción, respaldo previo, intención de adopción, libro e informe. Puede
reconocer una operación ya aplicada y recuperar un commit interrumpido. Se
requiere un único escritor coordinado; los lectores usan snapshots.

El diario público añade comprobaciones de procedencia:

```sh
python3 tools/recovery/checkpoint.py record \
  --packet /ruta/privada/recibo.json --journal /ruta/privada/diario
```

El recibo contiene metadatos y hashes; siempre significa `prepared`, nunca
`imported`. Repetir un recibo idéntico no duplica el trabajo. Una colisión de
identidad, solapamiento o recibo corrupto detiene la operación. También se valida
todo el diario al reintentar un lote ya guardado. La escritura usa bloqueo local,
sincronización y publicación atómica sin sobrescritura.

El diario **no contiene las correcciones**. Antes de abrir otro lote deben quedar
conservados el payload, el libro, el snapshot y sus verificaciones en un destino
privado duradero. Esa confirmación externa no está automatizada todavía; no
tratar el temporal local ni los informes de Actions como único respaldo.

## Verificación de archivos y entregas

`verify-archive` sólo lee y verifica el archivo local: no extrae ni ejecuta
programas del ZIP. Hash y CRC se calculan sobre el mismo descriptor; se rechazan
cambios durante la lectura, colisiones de nombres, rutas peligrosas y enlaces.

```sh
python3 tools/recovery/checkpoint.py verify-archive \
  --key b11 --archive /ruta/privada/kotoGBA_B11_BLOQUE1_MEMORIA_ADOPTADA_NO_INSTALABLE_20261025.zip
python3 tools/recovery/checkpoint.py release-gate
```

El último comando debe seguir bloqueando una nueva entrega instalable. Compilar
B19 y verificar B12 no significa que B12 esté integrado en la SD. No se han
ejecutado nuevas pruebas de emulador o consola en esta migración.

Los informes de Actions tienen una retención de 90 días. Los commits conservan
el código; los checkpoints privados conservan los datos y resultados reales.

## Referencias de automatización

- https://docs.github.com/en/actions/how-tos/write-workflows/choose-when-workflows-run/control-workflow-concurrency
- https://docs.github.com/en/actions/tutorials/store-and-share-data
