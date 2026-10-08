## V0.5-C — corrección posterior a run 75

Rama: `kotogba-v0.5-auto-sync`. Base inmediata: `56e81f7144585132281e2f8006e950ad5e393c4a` (run 75).

- El hook lineal de lectura de bytes conserva LR con dos saltos, sin BL. El hook anterior guardaba LR después de que BL ya lo hubiese sobrescrito.
- La pantalla inferior se dibuja al iniciar: espera de diálogo automático. Ya no carga el bitmap antiguo de TRADUCIR.
- El cargador comprueba apertura, tamaño, lectura y vector de reset de `/_gba/bios.bin` antes de aplicar las relocaciones fijas del motor.
- No se distribuye emibios como BIOS de QA: su estructura no es compatible con esos parches.
- ROM y BIOS del usuario no se incluyen. Conserva la BIOS y las partidas que funcionaban previamente.

Validación de lanzamiento y de I001 → I005: pendiente hasta ejecutar con una BIOS compatible. Compilación correcta por sí sola no valida el juego.

# kotoGBA — GBARunner3 para aprender japonés jugando

Repositorio de construcción de **kotoGBA**, una adaptación experimental de GBARunner3 para cargar una ROM de GBA propiedad del usuario y asociarle un paquete didáctico independiente.

## Estado actual

### V0.3 — aprobada en hardware real

- Selector de archivos `.gba` desde la tarjeta SD.
- Navegación con cruceta, `A` y `B`.
- Identificación del juego mediante cabecera GBA.
- Asociación automática con `/_gba/kotogba/packages/<GAMECODE>_<REV_HEX>.koto`.
- Corrección de ejecución en EWRAM validada en una Nintendo 3DS real.

### V0.4 — paquete FireRed integrado

La V0.4 conserva íntegramente el runtime V0.3 aprobado y añade al artefacto de instalación el paquete real:

```text
/_gba/kotogba/packages/BPRJ_01.koto
```

Ese paquete corresponde a **Pocket Monsters FireRed (Japón), revisión 1** (`BPRJ`, revisión `01`) y contiene **78 fichas didácticas** del prototipo de Pueblo Paleta.

El archivo `.koto` es un contenedor ZIP con:

- `manifest.json`
- `cards.json`
- `resources.json`
- `recognizers.json`

El checkpoint V0.4 es autónomo y reproducible: el paquete validado está versionado en el repositorio, no depende de descargas temporales y su SHA-256 es:

```text
a7c725161f3f033d52b2e2e90becc3f295fc90e8bd8b48acb202323054219328
```

La ROM no se incluye en el repositorio ni en los artefactos. El usuario debe colocar su copia legal en una de las rutas examinadas por el selector, preferiblemente:

```text
/_gba/kotogba/games/Pocket Monsters - FireRed (Japan) (Rev 1).gba
```

## Base técnica fijada

- GBARunner3: `ecaa817815d9761745606592f04affe5ee9c3731`
- libtwl: `e069645bed14a93e149e873e9273f04851e3a04e`
- contenedor de compilación: `devkitpro/devkitarm:20241104`

## Siguiente fase

La V0.5 conectará el contexto del juego con los identificadores de ficha —por ejemplo `I001`, `U011` o `B003`— para abrir la tarjeta didáctica adecuada durante la partida. Después se activarán las vistas **Traducción**, **Palabras**, **Gramática** y **Volver**.

## Propiedad y distribución

Este repositorio no contiene ROMs de GBA, BIOS ni partidas guardadas. El paquete `.koto` contiene únicamente datos didácticos creados para el proyecto.
