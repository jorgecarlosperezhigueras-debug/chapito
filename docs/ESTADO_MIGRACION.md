# Migración verificable de kotoGBA — 25 de octubre de 2026

## Estados que deben conservarse

| Parte | Autoridad comprobada | Uso |
| --- | --- | --- |
| Juego instalable | B19/B10 | Mantener la instalación SD validada |
| Memoria de desarrollo | B12, revisión 50/runtime 36 | Continuar el bloque 2; todavía no instalable |
| Memoria anterior | B11, revisión 39/runtime 27 | Recuperación y comparación |

El bloque 2 y el Hito B siguen abiertos. La migración no adopta nuevas decisiones
lingüísticas ni convierte B12 en una actualización SD.

## Verificación realizada en esta recuperación

Se verificaron las once partes y la concatenación del checkpoint B19/B10, los ZIP
B11 y B12, y el instalador B19/B10. Se leyeron sus miembros para comprobar CRC,
nombres y rutas. Las memorias B11 y B12 superaron `PRAGMA integrity_check` y sus
revisiones se consultaron directamente en SQLite. Los pares de runtime y el
exportador sellado B12 coinciden con sus manifiestos.

Se recompiló B19 desde una carpeta nueva, con libtwl, ARM7 y ARM9. El ejecutable
de 572.416 bytes coincide exactamente con la entrega B19:

```text
1cd6128200e6a08824202681868f500a085971ccbb16afae884d7e30774e22ad
```

Los informes reproducibles están en `evidence/migration-20261025/`. Las pruebas
de recuperación ejercitan interrupciones, concurrencia, corrupción y reintentos.
Las pruebas del constructor comprueban sus entradas y la separación de datos.

## Código y datos

`current/` conserva los cambios propios B19 y las huellas de sus 526 fuentes.
`linguistic/` conserva el código de la memoria y exportación B12; sus datos y
dependencias privadas siguen separados. `tools/recovery/` registra lotes y
comprueba archivos sin modificar las memorias.

El paquete mínimo de compilación pesa 54.993.645 bytes. Su nombre y huella están
en `current/PRIVATE_BUILD_INPUTS.json`. Permite reconstruir B19 sin reutilizar
objetos compilados ni depender de las rutas temporales de una conversación.

## Automatización y límites

La CI pública ejecuta pruebas de las herramientas, contratos y comprobaciones
de fuentes cada vez que cambian sus archivos. Sus informes incluyen el commit
exacto y se conservan durante 90 días. La compilación completa utiliza el mismo
comando verificable, pero necesita las dependencias privadas en el ejecutor.
No se ha configurado un suministro privado de esas dependencias a GitHub
Actions; una ejecución de CI pública no debe describirse como un build B19.

No se han ejecutado nuevas pruebas de emulador ni de consola física en esta
migración. La reproducción binaria del NDS no sustituye esas pruebas.

## Pendiente de conservación privada

El ZIP final B12 y el nuevo paquete mínimo de compilación están verificados
localmente. La copia duradera de estos dos archivos todavía requiere resolver
la aprobación de su guardado en el destino privado. Los checkpoints B19/B10 y
B11 anteriores ya tienen referencias duraderas en la Biblioteca del proyecto.
Un hash publicado en Git no conserva por sí solo los bytes privados.

No avanzar con otra edición masiva mientras un lote terminado o un nuevo
checkpoint sólo exista en el entorno temporal. Cada lote debe conservar su
payload completo, procedencia, comprobaciones y copia recuperable antes del
siguiente; el diario de metadatos no basta.
