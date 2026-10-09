# Plan: 60 fps estables en vistas pesadas (render en 2 hilos + DrawDone sin espera)

Documento de traspaso. Recoge el contexto, todas las mediciones hechas el 2026-10-05 y el plan
completo. Léelo entero antes de tocar código. Complementa a `docs/prompt-opcion2-drawdone.md`, que
describe con detalle la "opción 2" (instantáneas), aquí llamada **Fase B**.

---

## 1. Proyecto y reglas

- Repo: `/home/sunking/Documentos/antigravity/ZeldaWWHDRecompAndroid`, rama `optimizaciones`.
  Es una recompilación estática de Zelda: Wind Waker HD (Wii U, PowerPC) a ARM64 para Android, con
  GX2 implementado sobre Vulkan. Fork de GreenNaugahyde/ZeldaWWHDRecompAndroid (MPL-2.0).
- Dispositivo: OnePlus 11 (Snapdragon 8 Gen 2, Adreno 740, **driver de fábrica obligatorio**),
  `adb -s 3f0602c2`. Si `adb devices` sale vacío, pide al usuario que reconecte el USB.
- El usuario quiere **60 fps estables** con drivers de fábrica. No usa la app "Juegos" de OnePlus.
  No propongas soluciones que dependan de ella.
- Commits: SOLO `git -c user.email=gerardlupionroca@gmail.com commit ...`, con el mensaje terminado en
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Haz un commit después de cada fase que
  funcione y que el usuario haya probado en la escena pesada. Nunca hagas commit de assets de
  Nintendo ni de nada que no compile.
- Respuestas cortas y en español. Antes de cada fase, resume el plan en 2 líneas.
- Compilar (sube N en cada build; **la última fue 35**):
  `cd android && ./gradlew assembleRelease -PwwhdDeviceRecomp -PwwhdVersionCode=<N> -PwwhdVersionName=<nombre> > /tmp/claude-1000/gradle-build.log 2>&1`
  (~25 s si no cambia `tools/recomp/hooks.txt`; no lo cambies sin avisar, porque obliga a recompilar
  todo el juego, ~5 min).
  Instalar: `adb -s 3f0602c2 install -r android/app/build/outputs/apk/release/app-release.apk`.
- Ajustes sin recompilar: un archivo `wwhd.env` (`CLAVE=valor`) en
  `/sdcard/Android/data/org.wwhdrecomp.app/files/`. Bórralo al acabar.
- No toques la lógica de interpolación ni la de true60 salvo lo imprescindible. No quites el modo 0
  de `drawdone_mode` ni cambies su comportamiento.

## 2. Cómo medir

El usuario se pone en la escena pesada y dice "graba". Entonces:

```bash
P=$(timeout 5 adb -s 3f0602c2 shell pidof org.wwhdrecomp.app); adb -s 3f0602c2 logcat -c; sleep 30; adb -s 3f0602c2 logcat -d --pid=$P | grep wwhd | grep -E "\[pace\]|\[gx2\] frame|draws/frame|KiB/frame|interp\]|outdated"
```

Pon un `timeout` en el primer `adb`: si el dispositivo no está, `adb` se queda en
"waiting for device".

Perfil del hilo de render. En este móvil **hace falta `--app` y `-e cpu-clock`**; `cpu-cycles` no
está soportado y sin `--app` falla:

```bash
adb -s 3f0602c2 shell simpleperf record --app org.wwhdrecomp.app -e cpu-clock -g -f 1500 --duration 15 -o /data/local/tmp/x.data
adb -s 3f0602c2 shell simpleperf report -i /data/local/tmp/x.data --comms "'GX2 render'" --sort dso
adb -s 3f0602c2 shell simpleperf report -i /data/local/tmp/x.data --comms "'GX2 render'" --children --sort symbol --percent-limit 1.5
```

Las funciones de `libwwhd.so` salen como offsets. Para agruparlas por línea de código, saca el
tiempo propio por dirección y simbolízalo en el host:

```bash
D=$(adb -s 3f0602c2 shell simpleperf report -i /data/local/tmp/x.data --sort dso | grep -o '/data/app.*libwwhd.so' | head -1)
adb -s 3f0602c2 shell simpleperf report -i /data/local/tmp/x.data --comms "'GX2 render'" --dsos "$D" --sort vaddr_in_file --percent-limit 0.01 | awk '/^ *[0-9.]+%/{print $1, $2}' > va.txt
python3 agg.py ~/Android/Sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-symbolizer \
  android/app/build/intermediates/cxx/Release/*/obj/arm64-v8a/libwwhd.so va.txt
```

`agg.py`: suma los porcentajes por función externa y por la línea más interna de `runtime/`. Los
porcentajes son **relativos a libwwhd** dentro del hilo de render.

```python
import json, sys, subprocess, collections
sym, so, va = sys.argv[1:4]
rows = [l.split() for l in open(va)]
out = json.loads(subprocess.run([sym, '-e', so, '-C', '-i', '--output-style=JSON'] + [a for _, a in rows],
                                capture_output=True, text=True).stdout)
byline, byouter = collections.Counter(), collections.Counter()
for (p, a), j in zip(rows, out):
    pct = float(p[:-1]); fr = j['Symbol']
    ours = next((x for x in fr if '/runtime/' in x['FileName']), fr[0])
    byline[ours['FileName'].split('/')[-1] + ':' + str(ours['Line']) + ' ' + ours['FunctionName'][:50]] += pct
    byouter[fr[-1]['FunctionName'][:80]] += pct
for k, v in byouter.most_common(15): print('%5.2f' % v, k)
print('---')
for k, v in byline.most_common(30): print('%5.2f' % v, k)
```

Frecuencias de la CPU: los núcleos 0-2 son los pequeños, 3-6 los grandes (máx. 2,8 GHz) y 7 el
principal (máx. 3,2 GHz).

```bash
adb -s 3f0602c2 shell "for i in \$(seq 10); do echo \$(cat /sys/devices/system/cpu/cpu3/cpufreq/scaling_cur_freq) \$(cat /sys/devices/system/cpu/cpu7/cpufreq/scaling_cur_freq); sleep 0.2; done"
```

**Cuidado con el ruido:** con el mismo trabajo, la CPU del render por frame varía entre 14 y 25 ms
según la frecuencia del momento. Compara varias líneas `[gx2] frame` seguidas, nunca una sola, y
fíjate también en `draws/frame` para saber si la escena es la misma.

## 3. Resultados medidos (2026-10-05, escena pesada, interpolación a 60 activa)

### 3.1 Modo 0 (`drawdone_mode` = esperar; referencia)
```
[pace] last 300 flips: 32-52 on time; game late 237-275, render late 0, GPU late 9-27
[gx2] ~31 swaps/s, swap interval 2; GX2DrawDone 2.0/frame, 9.1-10.4 ms/frame; CPU game 9-14 ms, render 16-23 ms
[gfx] draws/frame: 5207 with state reused, ~1565 resolved
[gfx] KiB/frame: vertices 3361 (+4265 large; 21843 reused), uniform blocks 77 (138 reused), uniform vars 2135
[interp] 15.8-18.5 logic steps/s
```

### 3.2 Modo 2 (no esperar nunca; inseguro, solo para medir el techo)
```
[pace] 0-58 on time; game late 66-85, render late 158-233
[gx2] 29.7-33 swaps/s; GX2DrawDone 0.0 ms; CPU game 12-13 ms, render ~23 ms
[interp] 15.7 logic steps/s
```
**Conclusión:** quitar la espera no sube los fps. El cuello de botella pasa a ser el hilo de render
(~23 ms > 16,7 ms). Por eso la opción 2 sola (Fase B) **no basta**: primero hay que bajar el hilo
de render (Fase A).

### 3.3 Frecuencia de la CPU / ADPF (descartado como solución)
- Las CPU van a medio gas: los núcleos grandes a 0,7-2,2 GHz (de 2,8) y el principal muchas veces a
  0,86 GHz (de 3,2), con solo 57 °C. Ningún hilo está al 100 %, así que el gestor de frecuencias baja
  el reloj.
- Ya hay pistas ADPF: `runtime/src/android/perf_hint.cpp` crea la sesión bien
  (`[adpf] session for game thread …, render thread …, target 16.7 ms`, objetivo
  `effective_swap_interval` = 1 con la interpolación) e informa max(CPU juego, CPU render) en cada
  swap. También hay una implementación paralela en `runtime/src/platform.cpp` (`perf_hint_frame`).
- Probé `cmd power set-fixed-performance-mode-enabled true`: **sin efecto** (mismos fps y mismas
  frecuencias). El gestor del móvil es `uag`, el propio de OnePlus, que lo ignora. Ya está
  desactivado otra vez (`false`).
- Reiniciar el juego no cambió nada.
- Conclusión: no hay forma de subir la frecuencia desde la app con el sistema de fábrica. Hay que
  quitar trabajo del camino crítico o repartirlo entre más núcleos.

### 3.4 Perfil del hilo "GX2 render" (build 35, modo 2)
Por librería: `libwwhd.so` 48 %, `vulkan.adreno.so` (driver) 29 %, `libc` 21 % (memcpy 9 %, ioctl
3,7 %, memcmp 2 %, malloc/free ~2 %).

Tiempo propio en libwwhd por función (% de libwwhd; ~0,11 ms por cada 1 % a 23 ms/frame):
```
20.9 bind_stage          (uniforms/vars, UBOs, descriptor sets)
12.8 gx2::execute_one    (+ execute, apply_regs, touch_regs)
 9.5 get_shader_uncached (g_shaders.find + memcmp de las palabras de estado)
 6.3 hash tables (unordered_map::find; cache misses)
 5.7 draw_indices        (casi todo su unordered_map::find)
 5.1 check_texture       (sparse_hash: 256 lecturas dispersas por textura)
 5.1 record_draw
 4.1 resolve_textures
 3.6 gfx::draw
 3.2 get_fetch_shader
 3.0 copy_tracked
 2.8 find_or_create_surface
 2.5 get_pipeline
```
Líneas más calientes: `vk_draw.cpp:1504` (memcpy de las entradas remapeadas de uniform buffers
leídas de la memoria del juego, 7 %), `vk_draw.cpp:616` (`g_shaders.find`, 5,7 %),
`vk_surfaces.cpp:710` (`sparse_hash`, 4,7 %), `vk_draw.cpp:1375` (`g_set_cache.find`, 2,5 %).

**Diagnóstico:** el coste propio está muy repartido y casi todo es latencia de memoria (búsquedas
en tablas hash y lecturas de la memoria del juego). Ninguna micro-optimización quita más de
0,3-0,7 ms. Con micro-optimizaciones no se llega a 60.

### 3.5 Cambio ya hecho (commit `fc5b79c`, build 35)
`vk: cache converted indices per submission while their pages are unwritten`. `draw_indices()` en
`vk_draw.cpp` cachea por (dirección, count, tipo, primitiva) y por envío los índices ya convertidos
y subidos, validándolos con `memw::unchanged_since`. Se reutiliza el 92 % (~5650 reutilizados y
~455 construidos por frame; aparece en la línea `[gfx] … draws/frame`). La ganancia es pequeña: su
`unordered_map::find` ya cuesta casi lo mismo. El usuario no vio fallos nuevos.
Detalle: en el camino lento, si `R.cmdSerial` cambió entre `draw_indices` y `record_draw`, se
vuelven a subir los índices, porque los chunks de subida se reciclan por envío.

## 4. Objetivo y presupuesto

60 swaps/s con "on time" ≥ 290/300 en la vista pesada, a 720p y con el driver de fábrica. Hoy cada
frame cuesta juego + espera ≈ 13 + 10 ms, y el render solo ya cuesta ~23 ms. Con las dos fases, el
frame costaría ≈ max(juego ~13 ms, preparación ~15 ms, grabación/driver ~8 ms) y entraría en
16,7 ms, aunque justo.

## 5. Fase A: separar la grabación de Vulkan en un segundo hilo ("record thread")

**Idea:** el hilo "GX2 render" sigue interpretando la cola GX2 y haciendo todo el trabajo nuestro
(resolver shaders, pipelines, texturas, copias de vértices y uniforms a la memoria de subida). Pero
en vez de llamar a `vkCmd*`, `vkQueueSubmit`, etc., escribe comandos ligeros en una cola. Un hilo
nuevo ("GX2 record") los reproduce en el `VkCommandBuffer` real y hace los submits. Así salen del
hilo de render el driver (~29 %) y buena parte de libc (ioctl, el memcpy interno del driver):
unos 7-8 ms.

Llamadas afectadas (`grep -o "vkCmd[A-Za-z0-9]*" runtime/src/vk/*.cpp`): unas 80, en `vk_device.cpp`
(35), `vk_draw.cpp` (20), `vk_surfaces.cpp` (11), `lsfg.cpp` (10) y `vk_profile.cpp` (2). Las del
camino de draw son: BindPipeline, SetStencil*, SetBlendConstants, SetDepthBias, SetViewport,
SetScissor, BindVertexBuffers, BindDescriptorSets, BindIndexBuffer, Draw y DrawIndexed. Las demás:
render passes, barreras, copias, blits, clears, dispatch y timestamps.

Pasos sugeridos:
1. **Medir primero cuánto se gana.** En el perfil, separa el tiempo de las llamadas `vkCmd*` y
   `vkQueueSubmit`/present (inclusivo, `--children`) del resto del hilo de render. Si es menos de
   ~6 ms, replantéate la fase.
2. **Capa de grabación diferida.** Un tipo `CmdStream` con un buffer lineal de registros
   {opcode, argumentos inline}. Copia los arrays apuntados (barreras, regiones, clear values,
   offsets dinámicos, viewports) dentro del registro: **ningún puntero a la pila del hilo de render
   puede llegar al hilo de grabación**. Envoltorios `rec::CmdDrawIndexed(...)`, etc., con la misma
   firma que Vulkan, para que el cambio en los archivos sea mecánico. Un ajuste (p. ej. variable de
   entorno `WWHD_RECORD_THREAD=0`) para volver a la grabación directa y comparar o descartar fallos.
3. **Ciclo de vida del command buffer.** `command_buffer()` (vk_device.cpp ~240) hace
   `vkBeginCommandBuffer` y sube `R.cmdSerial`. Con el hilo nuevo, el hilo de render solo "abre un
   stream" con su serial; el begin, el end y el `vkQueueSubmit` los hace el hilo de grabación, en
   orden. `R.cmdSerial` debe seguir subiendo en el hilo de render en el mismo punto, porque
   `SubmissionCopies`, `draw_indices` y `copy_tracked` dependen de él.
4. **Reciclado de recursos.** Los chunks de subida (`upload_alloc`, `g_chunk_used` →
   `g_chunk_free`), los pools de descriptores (`alloc_descriptor_set`) y los callbacks de
   `on_complete` se liberan cuando termina el envío en la GPU. Con el hilo de grabación, el submit
   real llega más tarde, pero la liberación debe seguir atada a la valla de ese envío. Revisa
   `poll()` y `submit()` en vk_device.cpp. Nada debe reciclarse antes de que el submit real se haya
   hecho y su valla se haya señalado.
5. **Descriptor sets:** `vkUpdateDescriptorSets` y `cached_descriptor_set` pueden seguir en el hilo
   de render (no son comandos), pero el set no puede reutilizarse ni liberarse mientras un stream
   pendiente lo use. Hay un caché `g_set_cache` con `g_set_cache_epoch`: revisa cuándo se vacía.
6. **Sincronización con el hilo de render:**
   - `render_sync()` / `issue_fence()` / `wait_fence()` (`gx2_core.cpp`, `g_fence_done`) deben
     esperar también a que el hilo de grabación haya enviado todo (hoy significan "el render
     ejecutó todo"; para GX2DrawDone el juego solo necesita eso, pero `frames_completed` y el
     pacing miran la GPU).
   - Los puntos que leen de la GPU de forma síncrona (p. ej. `dump_texture`, capturas, guardar
     cachés) deben vaciar la cola de grabación antes.
   - El present / `gfx::swap()` (BLAST/ANativeWindow) va en el hilo de grabación, en orden.
7. **Hilos y núcleos:** ponle nombre al hilo, aplica la misma prioridad que al de render
   (`platform::set_thread_high_priority`) y añádelo a la sesión ADPF de `perf_hint.cpp` (hoy acepta
   2 tids: amplíalo a 3 y usa el máximo de los tres para `reportActual`). Revisa
   `platform::apply_thread_cores` y los modos de núcleo (`core_mode`).
8. **Estadística periódica:** CPU por frame del hilo de grabación en la línea `[gx2] frame`, y
   KiB/frame del stream.

Riesgos: punteros a la pila, orden de los submits respecto al present, recursos reciclados
demasiado pronto (se verían como basura aleatoria), y `lsfg.cpp` (frame generation), que graba sus
propios comandos: puede quedarse directo si se ejecuta en el hilo de grabación.

## 6. Fase B: GX2DrawDone sin esperar al render (instantáneas)

Está detallada en `docs/prompt-opcion2-drawdone.md`, apartados "Cómo funciona hoy" y "Diseño
propuesto" (fases 1-4: display lists inline, instantáneas de índices/vértices/uniforms, shaders y
texturas, nuevo `drawdone_mode` = 3 "Instantáneas" con como máximo 1 frame de adelanto). Notas
nuevas:
- La Fase 0 de ese documento ya está hecha (apartado 3.2): quitar la espera solo ayuda cuando el
  render baja de ~16 ms. Haz primero la Fase A.
- `draw_indices` (apartado 3.5) ya cachea los índices por envío con memw. En la Fase B, la
  instantánea de índices se haría en el hilo del juego; reutiliza la misma idea.
- La línea más cara de `bind_stage` (`vk_draw.cpp:1504`) lee de la memoria del juego las entradas
  remapeadas de los uniform buffers. En la Fase B, esa lectura tiene que salir de la instantánea.
  Mira el coste en el hilo del juego.

## 7. Optimizaciones pequeñas pendientes (opcionales, ~0,3-0,7 ms cada una)

- `get_shader_uncached`: `g_shaders.find` (5,7 %). Revisa por qué falla tanto el memo de 64
  entradas (colisiones) o sustituye `std::unordered_map` por una tabla abierta (p. ej. de tipo
  robin-hood) en los mapas calientes: `g_shaders`, `g_set_cache`, la caché de `draw_indices` y
  `SubmissionCopies::map`.
- `check_texture`/`sparse_hash` (vk_surfaces.cpp:700-712): 256 lecturas dispersas por textura y
  comprobación. Valora saltárselo cuando memw dice que las páginas no han cambiado.
- Uniform vars (2,1 MB/frame): se hace `memset` + escrituras directas a la memoria de subida. Se
  podrían montar en un buffer local y copiar una vez, o deduplicar contra el anterior del mismo
  shader.

## 8. Fallo conocido, anterior a todo esto

Las sombras parpadean a veces. Ya pasaba antes de estos cambios y siempre ha estado en este port.
No se ha investigado. Hipótesis a comprobar: un shadow map leído antes de terminar de escribirse
(barreras), un uniform block reutilizado obsoleto (hay avisos esporádicos
`[gfx] write tracking: uniform block … outdated`; ver `copy_tracked`, que solo verifica 1 de cada
16 frames), o la interpolación moviendo la cámara de la luz de forma distinta a la del shadow map.
`WWHD_TRACK_WRITES=0` en `wwhd.env` desactiva el seguimiento de escrituras: si el parpadeo
desaparece, la causa está ahí.

## 9. Pruebas en cada fase

- Escenas: menú inicial (la cámara recorre la isla), Isla Taura con NPCs, la vista pesada del
  usuario, navegar por el mar, entrar y salir de casas y mazmorras, menús de pausa/mapa y cargar un
  estado guardado.
- Pide al usuario que confirme visualmente que no hay modelos estirados, texturas cambiadas,
  parpadeos nuevos ni cierres.
- Métricas: `[pace]` (on time, game late / render late), swaps/s, `GX2DrawDone ms/frame`, CPU del
  juego, del render y (nuevo) del hilo de grabación, `[interp] logic steps/s` (debe acercarse a 30).
