# Fuentes actuales de kotoGBA — B19

Este directorio incorpora **43 archivos propios de kotoGBA**, comprobados contra
el conjunto congelado `candidate19` de la entrega B19/B10. Incluye el runtime,
sus módulos de lectura, detección, ventanas y composición, y los dos programas
de compilación y empaquetado. La ruta `overlays/` conserva la estructura histórica
para que la reconstrucción no cambie las entradas del compilador.

`B19_SOURCES.json` identifica los **526 archivos** de la compilación congelada.
Los restantes 483 archivos (base externa y cabeceras generadas) forman parte de
los inputs privados. No deben confundirse los overlays con una copia completa
del emulador ni con una actualización instalable de la memoria.

## Comprobaciones disponibles

Desde la raíz del repositorio:

```sh
python3 tools/build/b19.py check-sources
python3 -m unittest discover -s tests/build -v
python3 tools/build/b19.py verify-inputs --inputs /ruta/privada/kotoGBA_B19_PRIVATE_BUILD_INPUTS_20261025.zip
python3 tools/build/b19.py build --inputs /ruta/privada/kotoGBA_B19_PRIVATE_BUILD_INPUTS_20261025.zip --output /ruta/privada/nueva/resultado_B19 --jobs 3
```

El último comando verifica las huellas de fuentes y dependencias, crea un árbol
temporal nuevo, ejecuta el build real de libtwl, ARM7 y ARM9, y reconstruye el NDS.
Exige que el ejecutable final coincida byte a byte con B19. Los archivos de
depuración ELF pueden variar por sus rutas; la comparación binaria exigida se
aplica al NDS y al núcleo `arm9.bin`. Se conservan un informe JSON, el log y los
binarios en el directorio de salida. Los temporales privados se eliminan al acabar.

El programa se detiene si la salida ya existe o si cualquier input difiere de su
huella. No descarga herramientas ni modifica la memoria lingüística o los
archivos originales. Una modificación intencionada del runtime requiere un
nuevo candidato y su verificación; no se debe sobrescribir la identidad B19.

## Dependencias privadas fijadas

`PRIVATE_BUILD_INPUTS.json` fija nombres, tamaños y SHA-256 de un paquete mínimo
de 54.993.645 bytes. Contiene el SDK retenido, los fuentes externos exactos,
cabeceras de glifos y datos, el bootstrap D19 y los tres paquetes de vinculación
que comprueba el empaquetador. No contiene la ROM comercial, la BIOS GBA,
partidas, colecciones, el libro ni la base SQLite de aprendizaje.

El archivo debe conservarse en almacenamiento privado duradero. Git conserva
sus huellas, no sus bytes ni enlaces temporales de descarga.

### Regenerar el paquete mínimo desde el checkpoint conservado

El ZIP mínimo no es la única vía de recuperación. Tras restaurar el checkpoint
completo B19/B10, se puede reconstruir exactamente desde su directorio
`Recuperacion_R38/r38_trabajo`:

```sh
python3 tools/build/b19.py make-inputs --checkpoint-root /ruta/restaurada/Recuperacion_R38/r38_trabajo --output /ruta/privada/nueva/kotoGBA_B19_PRIVATE_BUILD_INPUTS_20261025.zip
```

El comando utiliza exclusivamente las 491 rutas fijadas en el manifiesto;
verifica cada huella, ordena las entradas y fija la fecha, los permisos y la
compresión ZIP. Exige el SHA-256 del paquete original
`73cf863d5beb9a1b4eed6c9b6f13341365db79a39d89fec4393a3e36c58b1e76`
antes de publicar el archivo. Rechaza destinos existentes y no modifica el
checkpoint. De este modo, los once segmentos del checkpoint B19/B10 ya
conservados bastan para recuperar las dependencias aunque se pierda la copia
reducida de 55 MB.

## Alcance de la automatización

Las comprobaciones de fuentes y las pruebas del verificador pueden ejecutarse
en CI pública sin datos privados. La compilación completa requiere el paquete
privado en el entorno ejecutor. Sin ese input, una CI de infraestructura no
acredita una compilación de B19. El comando de build no incluye pruebas de
emulador ni de consola física, ni empaqueta una nueva SD con B12.

B19/B10 sigue siendo la base instalable. La memoria de desarrollo tiene su
propia revisión y debe consultarse en `project/STATE.json`.
