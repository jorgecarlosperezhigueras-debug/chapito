# Checkpoint: diagnóstico de SD para D23

## Resultado comprobado

Se han ejecutado 20 pruebas del auditor en GitHub Actions y las 20 han pasado.
La segunda ejecución terminó correctamente; la primera detectó un fallo al crear
una imagen sintética mediante sobrescritura multiclúster en pyfatfs. Se corrigió
la preparación de esa prueba generando una imagen independiente con el último
byte alterado, sin eliminar ni suavizar la comprobación de corrupción.

- Rama: `kotogba-d23-auditoria-sd`.
- Commit de código probado: `77bb830f98b4cb9eae360b91600e156286750c68`.
- Ejecución: https://github.com/jorgecarlosperezhigueras-debug/chapito/actions/runs/38646713916
- Job: `116135247828`.
- Registro: `Ran 20 tests in 0.576s` seguido de `OK`.
- Artefacto: `kotoGBA_D23_DIAGNOSTICO_SD_NO_ROM`.
- ID del artefacto: `11932926801`.
- SHA-256 del ZIP: `2fcb0186dc13eab53802675e77fbaaa156fb87fa28104ef6fc7713d18c88837e`.

## Qué se ha mejorado

Se compara el contenido completo y el tamaño de KANJI.K20 y BPRJ_01.ks5, no solo
sus cabeceras. Se distinguen archivos ausentes, rutas incorrectas, referencias
ambiguas, archivos truncados y contenido diferente. Las rutas reales de D23 no
se presuponen: deben obtenerse de sus fuentes y facilitarse al auditor.

Las imágenes se abren en modo de solo lectura. Se ha verificado que su hash no
cambia después de analizarlas. Las pruebas cubren FAT16, FAT32 y volúmenes con
un desplazamiento explícito. El caso de cabecera intacta y último byte corrupto
se detecta correctamente.

## Límites, sin confundir diagnóstico con reparación

Los archivos usados en estas pruebas son SINTÉTICOS. No son las tarjetas reales,
la SD del usuario, la BIOS ni Pokémon. No se han podido analizar en esta ejecución
los bytes de la SD original ni del respaldo D23 debido al ClientError del entorno
local. GitHub sí ha permitido guardar y ejecutar estas herramientas independientes.

No se ha vuelto a ejecutar melonDS ni generado capturas nuevas. No se ha reparado
ni recompilado una ROM D23 en este checkpoint. La pantalla blanca y la navegación
vocabulario → kanji → componente → volver siguen sin validarse.

Los cambios son únicamente cuatro archivos nuevos de diagnóstico y este informe.
No se han modificado fuentes existentes de la interfaz ni ramas anteriores, y no
se ha sustituido D23 por la versión histórica V0.5 del repositorio.

La comprobación pendiente es aplicar el auditor a la SD y al respaldo reales,
confirmar las rutas de D23 y, si coinciden, registrar apertura y lectura de datos
antes y después del arranque. Una coincidencia de bytes fuera del emulador no
prueba acceso DLDI ni comportamiento de memoria durante el juego.
