# Prompt: Opción 2 — que el juego no espere al hilo de render en GX2DrawDone

Copia todo lo que hay debajo de la línea en un chat nuevo de Claude Code abierto en el repo.

---

## Contexto del proyecto

Repo: `/home/sunking/Documentos/antigravity/ZeldaWWHDRecompAndroid`, rama `optimizaciones`
(fork de GreenNaugahyde/ZeldaWWHDRecompAndroid, MPL-2.0, ya fusionado con su 0.3/0.4). Es una
recompilación estática de The Legend of Zelda: Wind Waker HD (Wii U, PowerPC) a ARM64 para Android,
con GX2 (la API gráfica de la Wii U) implementada sobre Vulkan. Dispositivo de pruebas: OnePlus 11
(Snapdragon 8 Gen 2, Adreno 740, driver Qualcomm de fábrica), conectado por USB (`adb -s 3f0602c2`).

Reglas que debes respetar:
- Commits SOLO con `git -c user.email=gerardlupionroca@gmail.com commit ...` (nunca otro correo),
  terminando el mensaje con `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Un commit
  después de cada fase que funcione, para poder volver atrás.
- Nunca subas ni hagas commit de assets de Nintendo.
- El usuario prefiere respuestas cortas y directas, en español. Antes de cada fase resume el plan en
  2 líneas. No hagas resúmenes finales largos.
- Compilar: `cd android && ./gradlew assembleRelease -PwwhdDeviceRecomp -PwwhdVersionCode=<N> -PwwhdVersionName=<nombre> > /tmp/claude-1000/gradle-build.log 2>&1`
  (sube N en cada build; el último usado fue 34). Instalar:
  `adb -s 3f0602c2 install -r android/app/build/outputs/apk/release/app-release.apk`.
  Mientras no cambie `tools/recomp/hooks.txt` no hace falta recompilar el código del juego (solo
  el runtime, ~25 s de build).
- Para medir, el usuario se pone en una escena pesada y te dice "graba". Entonces:
  `P=$(adb -s 3f0602c2 shell pidof org.wwhdrecomp.app); adb -s 3f0602c2 logcat -c; sleep 30; adb -s 3f0602c2 logcat -d --pid=$P | grep wwhd | grep -E "\[pace\]|\[gx2\] frame|draws/frame|KiB/frame|interp\]"`.
  Perfiles: `adb shell simpleperf record --app org.wwhdrecomp.app -p $P -g -f 1500 --duration 20 -o /data/local/tmp/x.data`
  (añade `-e cpu-clock --trace-offcpu` para ver bloqueos) y `simpleperf report -i ... --comms 'GX2 render' --sort symbol`.
  Símbolos de libwwhd.so: `llvm-symbolizer -e android/app/build/intermediates/cxx/Release/*/obj/arm64-v8a/libwwhd.so -f -C 0x<offset>`
  (NDK en `~/Android/Sdk/ndk/27.2.12479018`).
- Ajustes temporales sin recompilar: un archivo `wwhd.env` (líneas `CLAVE=valor`) en
  `/sdcard/Android/data/org.wwhdrecomp.app/files/` se lee al arrancar (`MainActivity.applyEnvironment`).
  Bórralo cuando acabes.

## El problema, con datos medidos

En las vistas pesadas (unos 7.000 draws por frame: ~5.300 por el camino rápido y ~1.600 resueltos
del todo) el juego cae de 60 a 30 fps de golpe. Mediciones con interpolación a 60 activada:

```
[pace] last 300 flips: 25 on time; vsyncs lost: game late 275, render late 0, GPU late 22
[gx2] frame 1201, 30.2 swaps/s; GX2DrawDone 2.0/frame, 10.6 ms/frame; CPU per frame: game 10.4 ms, render 13.1 ms
```

- Hilo del juego ("guest"): ~10 ms de CPU por frame, más ~11–12 ms bloqueado en `GX2DrawDone`
  (lo llama 2 veces por frame) esperando a que el hilo de render ("GX2 render") ejecute todo lo
  encolado. Total ~21,7 ms > 16,7 ms, así que pierde el vsync y cada pasada dura 33 ms. Con la
  interpolación la lógica solo avanza en pasadas alternas, así que además el juego va a cámara
  lenta (~15 pasos de lógica por segundo en lugar de 30).
- El perfil off-CPU confirma que el hilo de render pasa mucho tiempo ocioso esperando trabajo en su
  cola. No están en paralelo: los `GX2DrawDone` los obligan a ir en serie.
- La CPU del render en esa vista es de 13–24 ms (varía con la temperatura). Lo que queda es sobre
  todo el driver de Adreno procesando draws: ya se han quitado las copias y comparaciones grandes
  (commit 2aa3f84: seguimiento de escrituras `memw` y uniform blocks acotados).
- Objetivo: que juego y render se solapen de verdad, para que el frame cueste más o menos
  max(juego, render), ~13–14 ms, en lugar de juego + espera. Así cabe en 16,7 ms → 60 fps estables.

## Cómo funciona hoy (lee estos archivos antes de tocar nada)

- `runtime/src/gx2/gx2_core.cpp`:
  - Las funciones HLE de GX2 (las llamadas del juego) llaman a `emit(op, payload)`, que hace
    `enqueue()`: guarda el op y su payload en `g_q_pending` (un vector protegido por `g_q_mutex`).
    `render_thread_main()` intercambia ese vector con `g_q_work` y llama a `execute()` /
    `execute_one()`, que a su vez llama a `gfx::draw`, `gfx::clear_color`, etc. (en `runtime/src/vk/`).
  - `GX2DrawDone` (unas líneas por debajo de `GX2Flush`) emite `OP_DRAW_DONE` y, según
    `g_drawdone_mode` (0 = esperar, el modo correcto; 1 = un DrawDone de retraso; 2 = no esperar
    nunca), llama a `render_sync()` (= `issue_fence()` + `wait_fence()`, sobre `g_fence_done`).
    La app ya tiene la opción "drawdone_mode" en Opciones → Gráficos (`OptionsMenu.java`, `strings.xml`
    `drawdone_modes`). Los modos 1 y 2 NO son seguros: el render lee la memoria del juego cuando le
    llega el turno a cada draw, y para entonces el juego puede haberla reescrito.
  - El juego espera a la GPU para "flipear": cerca de `if (gfx::frames_completed() < pf.swap)`
    (~línea 460) y en `GX2WaitForVsync`. Hay estadísticas `[pace]` (game late / render late / GPU late).
  - `apply_regs` incrementa `g_shader_state_gen` (solo con registros que cambian el shader, filtro
    `g_shader_reg_filter` de vk_draw.cpp) y `g_draw_state_gen` (cualquier registro que no sea de datos).
- Qué lee el render de la memoria del juego al ejecutar cada op (esto es lo que hay que hacer seguro):
  1. `OP_SET_REGS`: los valores ya van copiados en la cola. Seguro.
  2. `OP_CALL` (display lists: `GX2CallDisplayList` / `GX2DirectCallDisplayList`):
     `execute((const uint32*)mem::ptr(p[0]), p[1] / 4)` lee la display list de la memoria del juego
     al ejecutarla, y dentro puede haber más `OP_CALL` anidados. NO es seguro.
  3. `OP_DRAW` / `OP_DRAW_INDEXED` → `gfx::draw` (`runtime/src/vk/vk_draw.cpp`):
     - Vértices: `vertex_buffer(addr, size)` → `copy_tracked` / `copy_deduped` (`mem::ptr(addr)`).
     - Índices: `build_indices(prim, count, indexType, indexAddr, ...)` lee los índices de la memoria
       del juego.
     - Uniform blocks: `bind_stage` → `copy_tracked(blocks, addr, size, ...)` (direcciones en los
       registros `mmSQ_VTX/PS_UNIFORM_BLOCK_START + i*7`, tamaño en la palabra 1 = tamaño-1).
     - Programas de shader: `program_hash` / `get_shader_uncached` (decompila desde `mem::ptr(addr)`
       con `regs[mmSQ_PGM_START_VS/PS]`) y los fetch shaders (`LatteFetchShader`).
     - Texturas: `vk_surfaces.cpp` (`sampled_texture`, `check_texture`, `upload_surface`,
       `sparse_hash`) hashea y sube los datos desde la memoria del juego cuando cambian.
  4. Clears / copias (`OP_CLEAR_*`, `OP_COPY_SURFACE`, `OP_COPY_TO_SCAN`): revisa cada `case` de
     `execute_one` en busca de `mem::ptr` / `ld32` sobre punteros del juego (p. ej. `unpack_struct`).
- En este renderer no hay lecturas de la GPU hacia la memoria del juego: el único
  `vkCmdCopyImageToBuffer` es el volcado de depuración `dump_texture`. Así que el juego nunca
  necesita esperar a que la GPU escriba en su memoria, lo cual simplifica mucho el diseño.
  Verifícalo igualmente.
- Seguimiento de escrituras (`runtime/src/mem_writes.h/.cpp`, namespace `memw`): una "generación"
  por página de 4 KiB. Se actualiza con `DCFlushRange/DCStoreRange/DCZeroRange`
  (`hle/coreinit_mem.cpp`), `GX2Invalidate` con el bit CPU 0x40 (`gx2_core.cpp`), `FSReadFile`
  (`hle/fs.cpp`) y al cargar un estado (`mark_all` en `savestate.cpp`). API: `memw::now()` y
  `memw::unchanged_since(addr, size, gen)`. Medido: en vértices nunca falla; en uniform blocks solo
  aparecen cambios sin aviso en zonas que el shader no lee (cola de rangos de 64 KiB).
- La interpolación (`runtime/src/interp.cpp`, `interp_fx.cpp`) cambia matrices en la memoria del
  juego desde hooks (p. ej. `hook_027F5018` en el hilo update_ubo, que escribe matrices intermedias
  en mitad de la pasada y luego vuelve a poner las exactas). Cuidado: cualquier copia de datos debe
  ver los valores que el juego tenía en el momento del draw, igual que ahora.

## Diseño propuesto

Idea central: **todo lo que el render vaya a leer de la memoria del juego se fija en el momento en
que el juego emite el comando** (en el hilo del juego). Así `GX2DrawDone` puede devolver el control
sin esperar al render, porque nada de lo que el juego escriba después afecta a comandos ya encolados.

### Fase 0 — medir el techo ANTES de programar nada (obligatorio)
1. Con la build actual pon "drawdone_mode" en 2 (Nunca) en la escena pesada, graba y compara con el
   modo 0. Puede haber fallos gráficos en el modo 2, da igual: es para ver cuánto rendimiento da
   quitar la espera.
2. Importante: en una sesión anterior "No wait" rindió PEOR. Si vuelve a pasar, averigua por qué
   antes de seguir: si el render se queda atrás y el pacing de flips (`frames_completed` / `pf.swap`,
   `GX2WaitForVsync`) penaliza; si los dos hilos compiten por núcleos (mira `platform::set_core_mode`
   y `apply_thread_cores`) o por temperatura. El rediseño solo sirve si quitar la espera ayuda. Si no
   ayuda, hay que arreglar primero el pacing (p. ej. limitar cuánto se adelanta el juego: 1 frame
   como máximo, esperando la valla del frame N-1 en lugar del DrawDone actual).
3. Informa al usuario con números antes de pasar a la fase 1.

### Fase 1 — display lists
- En `GX2CallDisplayList` / `GX2DirectCallDisplayList`, cuando no se está grabando una display list
  (`t_rec.start == 0`) y hay hilo de render, copia el contenido de la lista a la cola: un op nuevo,
  `OP_CALL_INLINE`, cuyo payload son las palabras de la lista. Si la lista contiene `OP_CALL`,
  incrústalo también de forma recursiva al copiar, con un límite de profundidad. Si se está grabando
  una display list, deja el `OP_CALL` normal: se resolverá al ejecutar la lista exterior, que ya se
  habrá copiado.
- `execute_one` para `OP_CALL_INLINE`: `execute(p, n)`.

### Fase 2 — datos de los draws
- Al emitir `OP_DRAW_INDEXED`: copia los índices (count × tamaño según indexType, teniendo en
  cuenta los tipos de primitiva que `build_indices` expande) a un "arena de instantáneas" y pasa en
  el payload un identificador o puntero en lugar de `indexAddr`. `build_indices` leerá de ahí.
- Vértices y uniform blocks: necesitas saber qué buffers usará el draw sin hacer en el hilo del
  juego el trabajo de `gfx::draw`. Opciones, de menor a mayor complejidad:
  a) Copiar todos los attribute buffers enlazados (`mmSQ_VTX_ATTRIBUTE_BLOCK_START + i*7`, palabras
     0 = dirección y 1 = tamaño-1, 16 slots) y todos los uniform blocks enlazados de VS/PS, pero
     **solo si sus páginas cambiaron** desde la última instantánea de ese (dirección, tamaño)
     (`memw::unchanged_since`). Si no cambiaron, reutiliza la instantánea anterior. Casi todos los
     draws no cambian nada (camino rápido), así que el coste en el hilo del juego debería ser
     pequeño. Mídelo.
  b) Cachear por shader qué slots usa (el fetch shader y `rm.uniformBuffersBindingPoint`) para no
     copiar slots que no se usan.
- Arena de instantáneas: memoria del host (no Vulkan) en bloques de, por ejemplo, 8 MiB. El juego
  escribe en ella y el render lee. Cada bloque se libera cuando el render pasa la valla
  (`g_fence_done`) emitida después de su último uso. Las instantáneas reutilizadas entre frames
  necesitan un último uso actualizado. Más sencillo: un mapa (dirección, tamaño) → {puntero, gen,
  último serial de valla}; si cambia la gen se crea una nueva y la vieja se libera al pasar su valla.
- En `vk_draw.cpp`, `vertex_buffer` / `bind_stage` / `build_indices` leen de la instantánea (un
  puntero que llega en el payload o en una tabla paralela a la cola) en lugar de `mem::ptr(addr)`.
  El dedupe por envío (`SubmissionCopies`) puede quedarse igual, usando como clave la instantánea.

### Fase 3 — shaders y texturas
- Programas de shader y fetch shaders: suelen ser estables, pero la memoria se libera y se reutiliza
  al cambiar de escena. Al emitir un draw, comprueba con `memw` si cambiaron las páginas de
  `PGM_START_VS/PS` (y del fetch shader); si cambiaron, copia el programa (tamaño en la palabra +1)
  a la arena y pásalo. Si no, el render puede seguir usando su caché por hash de contenido. Nunca
  dejes que el render decompile memoria que el juego puede estar reescribiendo.
- Texturas: hoy `upload_surface` hashea y sube desde la memoria del juego. Con el juego adelantado,
  como mucho una textura recién escrita se vería un frame antes. Acéptalo en la primera versión,
  pero documéntalo. Si aparecen fallos (minimapa, cámara pictográfica, texturas que se cargan en
  streaming), copia al emitir solo las texturas cuyas páginas cambiaron desde su última subida.

### Fase 4 — DrawDone sin espera
- Nuevo valor de "drawdone_mode", "Instantáneas" (3), seleccionable en la app. Si funciona sin
  fallos, pasará a ser el valor por defecto; el modo 0 queda como alternativa segura. En ese modo
  `GX2DrawDone` emite `OP_DRAW_DONE` y no espera, salvo para no adelantarse más de 1 frame: espera
  la valla del DrawDone del frame anterior, como el modo 1 actual. Así acotas la latencia y la
  memoria de la arena.
- Revisa todos los demás puntos donde el juego espera al render o a la GPU (`render_sync()` llamado
  desde otros HLE, `GX2WaitForVsync`, el pacing de flips, `savestate.cpp` que serializa en la
  frontera de frame, `gfx::save_caches` al pausar) y asegúrate de que siguen siendo correctos con el
  juego adelantado. Los estados guardados deben seguir haciendo `render_sync` completo.

## Pruebas (en cada fase)
1. Escenas: el menú inicial (la cámara recorre la isla), Isla Taura con muchos NPCs, la vista
   pesada que use el usuario, el mar navegando, entrar y salir de casas y mazmorras (cambios de
   escena = memoria liberada y reutilizada), menús de pausa/inventario y mapa, cargar un estado
   guardado.
2. Comprueba que no hay fallos gráficos (modelos estirados o fuera de sitio, texturas cambiadas,
   parpadeos) ni cierres. Pide al usuario que confirme visualmente.
3. Métricas: líneas `[pace]` (on time, game late), swaps/s, `GX2DrawDone ms/frame`, CPU del juego y
   del render. Éxito = 60 swaps/s con "on time" ≥ 290/300 en la vista pesada, a 720p y con el
   driver de fábrica.
4. Añade un log periódico con el coste del hilo del juego en instantáneas (KiB copiados/frame,
   reutilizadas/creadas) en el mismo formato que los logs `[gfx] last 300 frames`.

## Cosas que NO debes hacer
- No quites el modo 0 ni cambies su comportamiento.
- No toques la lógica de interpolación ni de true60 salvo lo necesario.
- No subas el nivel de optimización del recompilador ni cambies `tools/recomp/hooks.txt` sin avisar
  (cambiar la lista de hooks obliga a recompilar el juego entero, ~5 min en el dispositivo).
- No hagas commit de nada que no compile ni de builds que el usuario no haya probado en la escena
  pesada.
