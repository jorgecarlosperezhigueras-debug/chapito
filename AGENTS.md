# kotoGBA: contrato de continuidad

Lee primero `project/STATE.json`, `project/CHECKPOINTS.json` y `docs/RECUPERACION.md`. Este contrato sirve para cualquier agente, incluido Claude. Trabaja exclusivamente en kotoGBA.

## Autoridad y límites

Esta rama conserva fuentes propias B19 en `current/` y código de memoria B12 en `linguistic/`, con dependencias privadas fijadas por hash. B19 se ha recompilado en una carpeta nueva y reproduce exactamente el NDS entregado. Los checkpoints B19/B10, B11 y B12 se han verificado de nuevo por bytes e integridad. La entrega SD vigente sigue siendo B19/B10; la memoria de desarrollo más reciente es B12, revisión 50/runtime 36, no instalable. El bloque 2 sigue abierto. No sustituirla por B11 (39/27), que se conserva como antecedente. El ZIP final B12 todavía no tiene confirmación nueva de copia privada duradera; consulta el bloqueo en `project/STATE.json`. Las carpetas V0.x son históricas.

El bloque 2 está autorizado, pero su cierre no está acreditado. No conviertas los números comunicados en conversaciones en revisiones importadas. Recupera resultados únicamente de archivos verificables y conserva el origen de cada decisión. No repitas las decisiones ya adoptadas en B11 ni en la tanda 000–007 de B12; verifica ledger, hashes y SQLite.

## Trabajar sin perder el avance

1. Una tarea acotada por incidencia y una rama por cambio. Ninguna operación debe depender de que sobreviva esta conversación.
2. Conserva el checkpoint de entrada. Modifica una copia y verifica la preimagen antes de importar. Un único escritor de memoria; lectores paralelos sólo sobre snapshots inmutables.
3. Guarda cada lote preparado antes de abrir el siguiente. El diario de `tools/recovery/checkpoint.py` es un registro de procedencia; NO importa decisiones en la base lingüística.
4. Los lotes privados contienen las correcciones reales; sus recibos contienen las huellas de entrada/salida. Un recibo sin su archivo privado no permite reconstruir las correcciones. Conserva ambos en almacenamiento privado duradero. No dependas sólo de artefactos temporales de Actions.
5. Antes de mover una rama remota, comprueba su SHA esperado; nunca uses force-push. Comprueba lectura de vuelta y CI. Un commit local sin push no es un checkpoint remoto.
6. Al finalizar o encontrar un bloqueo, deja el SHA, archivos modificados, pruebas ejecutadas, evidencias y siguiente acción exacta. No afirmes que seguirás trabajando en segundo plano.

## Producto

Respeta la UX aprobada: japonés literal y traducción contextual al castellano; vocabulario y gramática enlazados; gramática negra; furigana; ventanas cerrables al toque exterior; desplazamiento visible; prioridad al elemento seleccionado; palabras completas, no kanjis artificialmente separados; nombres del jugador sin inventarlos; frases largas sin spoilers. No inventes caracteres ausentes en `しまっ`.

## Publicación y pruebas

Este repositorio es público. No publiques ROM, BIOS, partidas, colecciones personales, libros, fuentes tipográficas, bases de diccionario ni checkpoints completos. Revisa derechos, contenido y secretos de cada archivo antes de incorporarlo. Los filtros de extensiones son sólo una defensa adicional, no una auditoría legal o de secretos.

Una prueba de infraestructura o de compilador NO acredita compilación de B19, funcionamiento en emulador ni consola física. La CI pública comprueba código, contratos y datos sintéticos; no dispone de los inputs privados para compilar B19. El constructor completo está probado localmente. El workflow público no genera un instalador. Para una entrega hacen falta fuentes actuales verificadas, compilación de esas fuentes, datos emparejados, pruebas y recuperación del archivo final. La prueba de Nintendo DS física requiere a Jorge. No cierres el Hito B sin las evidencias correspondientes.

No crees servidores, Codespaces de pago, servicios nuevos ni gastos. No cambies el alcance de los cinco bloques para declarar terminado lo pendiente.
