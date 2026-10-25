# Memoria lingüística B12: código y recuperación

Este directorio conserva 28 archivos de código y esquema de B12, memoria **50/runtime 36**. Son copias byte a byte del checkpoint B12 verificado; `SOURCES.json` registra sus rutas originales y SHA-256. B12 es desarrollo no instalable. La entrega SD sigue siendo B19/B10 y el bloque 2 permanece abierto.

## Qué se puede ejecutar desde un clon

Se mantiene la distribución original bajo `source/r38_trabajo`, con el adaptador propio del diccionario a su lado en `source/dictionary_data`. Los módulos conservan sus rutas relativas, identidades y guardas históricas.

```sh
python3 linguistic/verify.py
python3 -m unittest discover -s tests/linguistic -v
python3 linguistic/source/r38_trabajo/desarrollo/pruebas_HITO_B/memory/memory.py --help
python3 linguistic/source/r38_trabajo/desarrollo/pruebas_HITO_B/memory/continuation_b12/adopt_b12.py --help
python3 linguistic/source/r38_trabajo/desarrollo/pruebas_HITO_B/memory/continuation_b12/export_runtime_v4.py --help
python3 linguistic/source/r38_trabajo/desarrollo/pruebas_HITO_B/memory/continuation_b12/publish_snapshot_v4.py --help
```

Las pruebas públicas usan SQLite en RAM y entradas inventadas. Comprueban que las tablas del diccionario participan en la huella, que el orden de inserción no cambia el estado lógico, que se detectan cambios y pérdidas de filas, y que las salidas no reemplazan las entradas. También comprueban las dependencias de importación y el rechazo de una exportación sin contrato privado.

Python 3.11 o posterior proporciona `hashlib.file_digest`; la verificación de esta migración se ejecutó con Python 3.12. El adaptador de importación XML usa **lxml 6.1.1**. El ZIP privado incluye su wheel para CPython 3.12/Linux x86_64. La comprobación pública y las cinco ayudas anteriores no necesitan lxml ni datos del diccionario.

## Recuperar datos y contratos privados

Recuperar primero `kotoGBA_B12_TANDA_000_007_ADOPTADA_NO_INSTALABLE_20261025.zip`: 513.632.421 bytes, SHA-256 `9bac60c7a80ec2a0e9b6a800ae64e7dd67741ed9416a7d4fd75e8639a1d0c1bc`. Conservar juntos el directorio del proyecto (`kotoGBA_B12` en la entrega) y `dictionary_data`.

`PRIVATE_INPUTS.json` contiene únicamente nombres, tamaños y hashes de 58 dependencias privadas. Incluye los contratos, paquetes revisados, historial y snapshots necesarios. Tres adaptadores históricos combinan código con datos lingüísticos y traducciones incrustadas; permanecen en el checkpoint privado. No se han sustituido por versiones modificadas para evitar las guardas.

Verificar una recuperación completa en lectura:

```sh
python3 linguistic/verify.py --private-project /ruta/recuperada/kotoGBA_B12
```

El comando comprueba que el código recuperado coincide con el código conservado en Git y verifica las dependencias privadas. No importa, exporta ni modifica la SQLite. Las herramientas reales se ejecutan desde esa recuperación verificada, manteniendo sus contratos y dependencias juntos.

Para validar o exportar, usar una copia de trabajo de la memoria y destinos nuevos. El `--help` anterior documenta los argumentos exactos. El snapshot canónico B12 se restaura con `memory.py restore-copy`. Para continuar el libro de adopciones existente, la identidad binaria requerida es `recovery/kotogba_B12_adoption_50_36.sqlite`, SHA-256 `c4c4ee17eb2789e9ad5e52c897ae7f840b4f801586b048d18977949055f380c3`. La SQLite canónica comprimida tiene contenido lógico equivalente y una identidad binaria distinta; no intercambiar ambas preimágenes.

## Qué protege el trabajo por lotes

El controlador `adopt_b12.py` exige que el paquete real exista dentro del árbol de memoria. Antes de importar, comprueba su hash, conserva un respaldo comprimido verificable y publica una intención con sincronización de archivo y directorio. Los importadores guardan el **JSON completo del paquete** en `koto_packets` dentro de la misma transacción que registra la adopción. El libro y el informe final se sincronizan antes de devolver el control. Un reintento después de un commit interrumpido reconstruye el predecesor y compara todas las tablas; una segunda adopción idéntica no repite el trabajo.

Estas garantías cubren los archivos locales y las transacciones. El controlador presupone un único escritor coordinado; el bloqueo del importador no es un bloqueo distribuido de toda la operación. El diario genérico de recuperación conserva recibos y no sustituye el paquete real. Antes de abrir el siguiente lote deben conservarse fuera de la sesión el paquete, el libro, el respaldo y la evidencia de importación. La confirmación de ese respaldo privado duradero todavía depende del servicio de almacenamiento y no la ejecuta este código público.

Los sellos B10/B11/B12 son finitos: no admiten otra revisión simplemente cambiando un contador. Un nuevo estado exige un nuevo contrato y evidencia que mantengan el linaje. Las guardas originales quedan intactas en esta migración.
