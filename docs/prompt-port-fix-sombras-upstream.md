# Prompt: portar el arreglo de sombras del upstream oficial al port Android funcional

Estamos trabajando en Zelda: Wind Waker HD recompilado para Android. Hay dos árboles locales:

- **Versión Android funcional (base que debemos conservar):**
  `/home/sunking/Documentos/antigravity/ZeldaWWHDRecompAndroid`
  - Rama: `optimizaciones`
  - HEAD actual: `fc5b79c`
  - APK funcional instalado/restaurado: versión 52, `0.52-shadow-receiver-bias`
  - Renderer Vulkan antiguo/custom: `runtime/src/vk/`
  - Tiene el frontend Android que debemos mantener: menú nativo, controles táctiles, layout editable, extracción de iconos desde los assets del juego, drivers personalizados, opciones, HUD de rendimiento, GamePad, gyro, rumble, etc.
  - El árbol está sucio con trabajo previo. No descartes, reviertas ni sobrescribas cambios existentes.

- **Upstream oficial actualizado usado como referencia:**
  `/home/sunking/Documentos/antigravity/ZeldaWWHDRecompAndroid-updated`
  - Rama local: `official-touch-migration`
  - Base oficial: `official/main`, commit `488b1fd`, tag `v0.2.2`
  - Renderer Vulkan nuevo: `runtime/src/gfx/vulkan/`
  - Esta versión se probó en el móvil y **solucionó visualmente el parpadeo de sombras y los artefactos/patrones del suelo**.
  - Sin embargo, rindió bastante peor y perdió el frontend Android anterior, su menú y los iconos extraídos del juego. No queremos usarla como nueva base ni volver a instalarla tal cual.
  - Este árbol también contiene cambios experimentales de una migración táctil fallida. Úsalo para consultar el código oficial, no como fuente ciega de todos los cambios.

## Objetivo

Investiga exactamente qué cambios del renderer Vulkan oficial corrigen:

1. Sombras que parpadean al mover la cámara o a Link, incluso a 30 FPS y con AO desactivado.
2. Patrones, bandas o aspecto cuadriculado/moire sobre arena y otros suelos según el ángulo y la distancia.
3. El cambio aparente entre dos estados de iluminación/sombra en montañas y partes del entorno.

Después, porta **la corrección mínima y correcta** al renderer `runtime/src/vk/` de la versión Android funcional. Debemos conservar íntegramente el frontend Android actual y su rendimiento.

## Evidencia importante

- El fallo ocurre también a 30 FPS: no es causado por el modo 60 FPS.
- Ocurre con AO activado y desactivado: no es exclusivamente AO.
- No es filtrado anisotrópico; se comprobó con el filtrado activado y desactivado.
- Aumentar la resolución interna reduce algo el patrón del suelo, pero no lo elimina. Esto apunta a precisión/coordenadas de shadow maps, depth bias, escalado o sincronización de superficies.
- El upstream oficial v0.2.2 lo corrigió en la prueba real del mismo dispositivo y escena.
- La base Android ya contiene intentos locales en `runtime/src/vk/vk_draw.cpp`, entre ellos comentarios y lógica relacionados con:
  - sesgo de comparación de la cascada D16 de Isla Initia;
  - depth bias dinámico y su escala;
  - AO privado/hires;
  - bilinear del centro de profundidad de AO.
  No asumas que esos parches son correctos; compáralos con el comportamiento real del upstream.

## Restricciones obligatorias

- Trabaja sobre la versión funcional: `/home/sunking/Documentos/antigravity/ZeldaWWHDRecompAndroid`.
- No reemplaces el frontend Android por SDLActivity ni por el frontend oficial.
- No elimines ni reduzcas funciones de controles táctiles, menú, extracción de iconos, drivers, gyro, rumble, GamePad o configuración.
- No reviertas cambios ajenos del árbol sucio.
- No hagas `git reset`, `git checkout --`, limpiezas destructivas ni una fusión masiva del upstream.
- No hagas commit hasta que yo confirme visualmente que el arreglo funciona.
- Evita parches específicos para una escena si el upstream contiene una solución general.
- No des por resuelto el problema únicamente porque compile: hay que probarlo en el móvil y comparar visualmente la escena problemática.
- No sacrifiques rendimiento de manera apreciable. Mide FPS/frame time antes y después en la misma escena.

## Investigación requerida

1. Lee primero:
   - `docs/plan-60fps-render.md`
   - `docs/prompt-opcion2-drawdone.md`
   - el estado y diff actual de la rama funcional.

2. Compara semánticamente, no solo por nombre de archivos:
   - funcional: `runtime/src/vk/vk_surfaces.cpp`, `vk_draw.cpp`, `vk_device.cpp`, `vk_shader.cpp`/traducción de shaders, `runtime/src/gx2/gx2_core.cpp`;
   - oficial: `runtime/src/gfx/vulkan/surfaces.cpp`, `draw.cpp`, `shaders.cpp`, `backend.cpp`, `present.cpp`, y `runtime/src/gx2/gx2_core.cpp`.

3. Revisa especialmente el historial oficial posterior al renderer del port Android. Entre los cambios potencialmente relevantes están:
   - seguimiento exacto de escrituras CPU y versiones de superficies (`5089724`, write tracking);
   - sincronización de `GX2CopySurface` (`5070881`);
   - retención/reutilización de feedback images (`7093ff4` y PR relacionado);
   - cambios de escalado y clasificación de render targets/shadow maps;
   - creación de vistas de profundidad, arrays/cascadas, selección de slices y transiciones de layout;
   - traducción de samplers de comparación y coordenadas/escala de texturas;
   - conversión de `PA_SU_POLY_OFFSET_*` a `vkCmdSetDepthBias`, incluyendo unidades y signo en Adreno;
   - invalidación de cachés, aliasing de memoria y copias render-target → sampled texture.

4. Usa `git log`, `git show`, `git blame` y diffs entre revisiones oficiales para identificar cuándo apareció la corrección. No copies todo v0.2.2. Si no hay un commit con título explícito sobre sombras, biseca por código/historial o construye hipótesis verificables.

5. Antes de modificar, entrega un diagnóstico concreto que responda:
   - qué dato o superficie estaba obsoleto/equivocado;
   - por qué depende del movimiento de cámara y del ángulo;
   - por qué subir resolución disminuye el patrón;
   - qué hace distinto el renderer oficial;
   - qué cambio mínimo debe recibir el renderer Android.

## Implementación y validación

1. Implementa el cambio con `apply_patch`, de forma localizada y comentada.
2. Compila el APK release con el flujo ya usado por el proyecto y aumenta temporalmente `versionCode`/`versionName` para instalarlo como actualización sobre `org.wwhdrecomp.app` sin borrar datos.
3. El móvil se conecta por ADB inalámbrico; la dirección puede cambiar. Pregunta por ella si `adb devices` no muestra el dispositivo.
4. Instala con `adb install -r`; no desinstales la app porque eso borraría datos.
5. Captura logcat y comprueba que no hay errores Vulkan, shaders, JNI ni crashes.
6. Haz capturas consecutivas o una ráfaga cada frame en la escena de Isla Initia. Compara:
   - montaña del fondo;
   - sombras sobre hierba y suelo;
   - arena/playa desde varios ángulos y distancias;
   - AO on/off;
   - 30 FPS y 60 FPS.
7. Comprueba que siguen funcionando:
   - menú Android anterior;
   - botones y sticks táctiles;
   - iconos dinámicos extraídos del juego;
   - layout táctil guardado;
   - resolución y opciones gráficas;
   - rendimiento comparable con la v52.
8. Si el arreglo no funciona, no encadenes sesgos empíricos. Instrumenta ambos renderers para comparar descriptores de la shadow map, formatos, tamaños, slices, samplers, matrices/uniformes, layouts y versiones de escritura en los mismos draws.

## Estado de seguridad

La v52 funcional fue restaurada en el móvil después de la prueba fallida del frontend oficial. Debe seguir siendo la referencia y el fallback. Existe un APK funcional en:

`/home/sunking/Documentos/antigravity/ZeldaWWHDRecompAndroid/android/app/build/outputs/apk/release/app-release.apk`

No hagas commit ni declares terminado el trabajo hasta que yo pruebe visualmente la escena y confirme que las sombras y el suelo están corregidos sin regresiones de rendimiento o interfaz.
