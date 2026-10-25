# Recuperación y pruebas de kotoGBA

## Qué queda operativo con este cambio

El código de infraestructura, las instrucciones para agentes y las huellas de los checkpoints se guardan en Git. El workflow `kotoGBA - recuperación (no ROM)` prueba el diario de lotes y el control de publicación, conserva informes y comprueba por separado el compilador ARM. Ninguno de esos controles compila ni valida B19. El resultado de CI es infraestructura, no una ROM nueva.

La migración de las fuentes actuales NO está terminada. `project/STATE.json` lo declara expresamente. El código histórico retenido en este repositorio no reemplaza B19/B10 ni B11. No se han trasladado ni alterado los ZIP privados, la ROM original, partidas ni colecciones. No se aprovisiona ningún servidor ni Codespace.

## Recuperar el estado sin leer la conversación

```sh
git clone https://github.com/jorgecarlosperezhigueras-debug/chapito.git
cd chapito
git switch kotogba-persistencia-20261025
python3 -m unittest discover -s tests/recovery -v
python3 tools/recovery/checkpoint.py contract
```

Lee las incidencias abiertas y verifica el último commit de la rama antes de continuar. Si se trabaja desde otra IA, `AGENTS.md` es el contrato común y `CLAUDE.md` remite a él.

## Recuperar los archivos actuales (pendiente de ejecución)

Los archivos de B19/B10 y B11 se mantienen en la Biblioteca privada del proyecto. Sus nombres y huellas están en `project/CHECKPOINTS.json`. Los identificadores privados no se publican aquí. El checkpoint B19 consta de once segmentos consecutivos de un único ZIP, no once ZIP independientes. Deben reconstruirse en orden y comprobar la huella del ZIP entero.

La herramienta siguiente sólo lee y verifica el archivo local; NO lo extrae, NO lo sube y NO ejecuta programas del ZIP:

```sh
python3 tools/recovery/checkpoint.py verify-archive --key b11 --archive /ruta/privada/kotoGBA_B11_BLOQUE1_MEMORIA_ADOPTADA_NO_INSTALABLE_20261025.zip
python3 tools/recovery/checkpoint.py verify-archive --key b19_full --archive /ruta/privada/kotoGBA_HITO_B_CHECKPOINT_B19_B10_AVANCE_VALIDADO_20261025.zip
```

Tras comprobar los bytes, el responsable de la migración debe inventariar las fuentes B19 y sus dependencias reales, revisar licencias/secretos/recursos privados, importar sólo lo publicable y fijar el comando de compilación de B19. No adivinar un Makefile a partir de V0.5. El SDK y los datos voluminosos necesitan almacenamiento privado durable, no la carpeta temporal de un runner ni únicamente artefactos con caducidad.

Para B11, dentro de `Recuperacion_R38/r38_trabajo`, el procedimiento documentado es:

```sh
python3 desarrollo/pruebas_HITO_B/memory/memory.py restore-copy --backup desarrollo/pruebas_HITO_B/memory/data/kotogba_memory_b11.sqlite.gz --output /ruta/privada/nueva/kotogba_b11.sqlite
python3 desarrollo/pruebas_HITO_B/memory/continuation_b11/export_runtime_v3.py --db /ruta/privada/nueva/kotogba_b11.sqlite --output /ruta/privada/export/BPRJ_01.kl29 --base-kc15 desarrollo/pruebas_HITO_B/memory/data/runtime_base/BPRJ_01.kc15 --output-kc15 /ruta/privada/export/BPRJ_01.kc15
```

Esto es una transcripción del procedimiento del checkpoint, no una nueva ejecución. No importar el parche v1 grueso de B11; conservar su parche v2 exacto y las decisiones adoptadas. El caso literal `しまっ` y el cierre del bloque 2 no están acreditados aquí.

## Diario de lotes

`tools/recovery/checkpoint.py record --packet /ruta/privada/recibo.json --journal /ruta/privada/diario` guarda un recibo inmutable por lote. El recibo acepta sólo metadatos, huellas e identificadores; no texto japonés, traducciones ni datos del jugador. Su estado siempre es `prepared`, nunca `imported`.

Repetir exactamente el mismo recibo no duplica trabajo. Cambiarlo bajo el mismo ID falla. Solapar apariciones dentro de la misma base falla. La escritura se serializa mediante bloqueo local y el archivo final se publica atómicamente. Un corte anterior a la publicación puede dejar un temporal oculto, pero no un recibo final truncado. El bloqueo sólo cubre escritores que usen este programa en el mismo sistema de archivos local: NO es un bloqueo distribuido.

El agente conserva tanto las correcciones reales como sus recibos en almacenamiento privado duradero antes de seguir. Para integración remota se usa un único escritor y actualización Git con SHA esperado; esta herramienta no hace push ni integra la memoria por su cuenta.

## Publicación

`python3 tools/recovery/checkpoint.py release-gate` falla mientras no consten la importación de fuentes actuales, la compilación y las pruebas de runtime. Cambiar booleanos a mano no constituye evidencia: el control es una barrera básica, no una certificación independiente. La comprobación física y el cierre del Hito B siguen separados.

Los informes de Actions tienen retención de 90 días y no sustituyen las fuentes ni los checkpoints privados. Los commits son la continuidad del código; la Biblioteca conserva por ahora los archivos de recuperación existentes. Un trabajo futuro no está ejecutándose simplemente porque exista una incidencia.

## Referencias de la infraestructura

- https://docs.github.com/en/actions/how-tos/write-workflows/choose-when-workflows-run/control-workflow-concurrency
- https://docs.github.com/en/actions/tutorials/store-and-share-data
- https://github.com/actions/checkout
- https://github.com/actions/upload-artifact
