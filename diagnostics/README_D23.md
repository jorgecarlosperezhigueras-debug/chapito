# kotoGBA D23: auditoría de SD (no es una ROM)

## Estado y alcance

Esta rama añade herramientas de diagnóstico independientes. No modifica los fuentes de
la interfaz, la ROM D23/D23R1, las partidas ni las listas personales.
La base histórica del repositorio no se presenta como si fuese D23.

La herramienta compara **todos los bytes** de `KANJI.K20` y `BPRJ_01.ks5` dentro de
una imagen FAT16/FAT32 con los archivos de un respaldo ZIP o carpeta.
Comprueba tamaño y SHA-256 completo. Informa por separado de archivos ausentes,
lecturas incompletas, contenido diferente, ubicaciones duplicadas, referencias
ambiguas y rutas que no coinciden. Nunca extrae el ZIP ni modifica la imagen.

**No comprueba** el formato interno de las tarjetas, el arranque de FireRed, el acceso
DLDI mientras el juego se ejecuta, la memoria del emulador, ni la navegación
vocabulario → kanji → componente → volver. Una cabecera coincidente no demuestra
que el resto del archivo esté intacto. Un contenido idéntico tampoco demuestra que
la ROM pueda acceder a él después de arrancar.

## Ejecución

Cerrar melonDS antes de examinar su SD. Trabajar con una copia estable de la imagen.
No se aceptan dispositivos físicos. Python 3.11 y dependencias para las pruebas:

```sh
python -m pip install pyfatfs==1.1.0 fs==2.4.16 setuptools==75.8.0 six==1.17.0 appdirs==1.4.4
python diagnostics/d23_sd_audit.py --image sd.img --reference kotoGBA_D23_Punto_Aprobado_20261012.zip --report auditoria_nueva.json
python -m unittest -v diagnostics.test_d23_sd_audit
```

El informe se crea con apertura exclusiva: no sobrescribe un archivo existente.
`--offset` permite indicar el comienzo del volumen FAT en bytes; el valor por defecto
es 0 (imagen del volumen, no autodetección de particiones MBR/GPT).

No se ha inventado ninguna ruta de instalación de D23. Para comprobar rutas, leer
primero las rutas exactas de los fuentes D23 y añadir, una vez por archivo:
`--expected-path 'KANJI.K20=/ruta/verificada/KANJI.K20'` y
`--expected-path 'BPRJ_01.ks5=/ruta/verificada/BPRJ_01.ks5'`.
Los ejemplos anteriores de rutas son marcadores, no instrucciones de instalación.

`PASS_CONTENT_ONLY` significa únicamente bytes idénticos en una ubicación no ambigua;
`PASS_CONTENT_AND_PATHS` añade las dos rutas proporcionadas. Ninguno significa que
kotoGBA haya arrancado o que un toque abra una tarjeta. El informe deja ambos
indicadores de ejecución y navegación explícitamente en falso.

## Pruebas

Las pruebas crean imágenes FAT de prueba con datos sintéticos (no tarjetas reales,
ni BIOS, ni juegos). Incluyen una corrupción al final del archivo cuya cabecera
sigue siendo idéntica, archivos repartidos en varios clústeres, FAT16/FAT32,
diferencias de tamaño, rutas y mayúsculas, duplicados, un volumen con desplazamiento,
ZIP como referencia y comprobación del hash de la imagen antes y después de leerla.
El resultado de GitHub Actions certifica el auditor, no la ROM.

## Siguiente comprobación sobre los archivos reales

Aplicar el auditor a la SD y al respaldo aprobado; registrar las rutas exactas que
utiliza D23. Si contenido y rutas coinciden, instrumentar los resultados de apertura
y lectura antes y después de iniciar el juego. No emitir de forma indiscriminada
«Copia KANJI.K20» cuando el error real pudiera ser de lectura o de inicialización.
Después de resolver el arranque, probar con toques reales vocabulario → kanji →
componente → volver, incluyendo conservación del desplazamiento y guardados.

No se ha identificado aún una causa única para la pantalla blanca ni se ha aplicado
un parche de arranque a D23 en esta rama.
