# Luma: LV2 Plugin Host - AI Coding Instructions

## Project Overview
Luma is a minimal LV2 plugin host for Android using Google Oboe for audio. The codebase prioritizes clarity and correctness over feature completeness—it's intentionally small to serve as a reference implementation.

**Target Platform**: Android (API 34+) with NDK
**Audio Engine**: Google Oboe (low-latency audio)
**UI Mode**: Headless (parameter control via JNI, no plugin GUIs)
**Build System**: NDK-build (Android.mk/Application.mk)

## Architecture

### Single-Header Design
- Main logic in `LV2OboeHost.hpp` as a single class `LV2OboeHost`
- Entry point via JNI bridge (`luma_jni.cpp`)
- Custom lock-free ringbuffer in `lv2_ringbuffer.h` (JACK-inspired, fully portable)

### Threading Model (CRITICAL)
**DSP Thread** (Oboe callback): Real-time safe, no allocations, no locks
- Runs in `onAudioReady()` method (wraps internal `process()`)
- Reads/writes audio buffers from Oboe streams
- Processes LV2 plugin audio (currently audio-only, no MIDI)
- Communicates via lock-free ringbuffers and atomics

**Worker Thread**: Background non-RT tasks (plugin requests)
- Implements LV2 Worker extension
- Request/response via separate ringbuffers
- Started only if plugin provides `LV2_Worker_Interface`

**Android UI Thread**: Parameter control and lifecycle
- JNI calls from Java/Kotlin
- Updates plugin parameters via atomic communication
- Syncs with DSP via atomics: `param_dirty`, `param_needs_update`
- Reads atom output via ringbuffer `dsp_to_ui` (if needed)

### Port System
The `Port` struct abstracts LV2 ports with flags:
- `is_audio`, `is_control`, `is_atom`, `is_input`
- Audio ports connected to Oboe buffer pointers (may require deinterleaving)
- Control ports use `float control` for current value (exposed via JNI)
- Atom ports allocate aligned buffers (`LV2_Atom_Sequence*`) with companion `AtomState` containing:
  - `dsp_to_ui` ringbuffer: plugin → Android UI messages (optional)
  - `ui_to_dsp` buffer + `ui_to_dsp_pending` atomic: Android UI → plugin messages

### Key Data Flows
1. **Audio I/O**: Oboe interleaved buffers → deinterleave → LV2 plugin → interleave → Oboe
2. **Parameter Control**: JNI call → atomic flag set → DSP reads in `onAudioReady()`
3. **Atom UI→DSP**: Android UI writes buffer → sets atomic → DSP reads in `process()`
4. **Worker**: DSP schedules work → worker processes → response delivered in next `process()` cycle

## Build & Development

### Dependencies
- **Android NDK** (r25c or later)
- **Oboe library** (Google's low-latency audio library, v1.8.0+)
- **Lilv/LV2 libraries** (cross-compiled for Android ARM64/ARMv7)
- **API Level 34+** (Android 14+)

### Build Commands
- **Build**: `ndk-build` (uses Android.mk/Application.mk)
- **Clean**: `ndk-build clean`
- C++17 required: `LOCAL_CPPFLAGS := -std=c++17 -Wall -Wextra -ffast-math`
- Set `APP_PLATFORM := android-34` in Application.mk

### JNI Usage Pattern
```java
LumaHost host = new LumaHost();
long handle = host.create("http://plugin.uri");
host.setParameter(handle, portIndex, value);
host.start(handle);
// ... audio processing via Oboe ...
host.stop(handle);
host.destroy(handle);
```

## Code Conventions

### Memory Management
- **Alignment**: Use `aligned_alloc(64, size)` for atom buffers (cache line alignment, available API 28+, guaranteed API 34+)
- **Cleanup Order** (in `closeHost()`): Stop Oboe → deactivate instance → stop worker → free instance → free Lilv nodes
- **No manual delete**: Use RAII wrappers or Lilv cleanup functions
- **JNI lifecycle**: Long handle passed between Java/C++, must call `destroy()` to free

### Ringbuffer Rules
- **Size**: MUST be power-of-two (checked by `is_power_of_two()`)
- **Atomics**: `write_ptr`/`read_ptr` use acquire/release semantics
- **Peek vs Read**: `peek` doesn't advance read pointer, `read` does
- Created via `lv2_ringbuffer_create()`, freed via `lv2_ringbuffer_free()`

### Real-Time Safety in `onAudioReady()`
**NEVER** in Oboe callback:
- Allocate/deallocate memory
- Use locks/mutexes
- Call `__android_log_print()` (use sparingly, only for critical errors)
- Block on I/O or JNI calls

**ALWAYS**:
- Use stack buffers or pre-allocated memory
- Communicate via atomics (`std::atomic`) with `memory_order_acquire`/`release`
- Keep code deterministic and bounded
- Deinterleave/interleave efficiently (consider NEON SIMD on ARM)

### LV2 Feature Support
Check features in `checkFeatures()` before instantiation:
- Required features MUST be provided (function returns false otherwise)
- Implemented features: URID map/unmap, Worker, State, Options, buf-size
- Feature array passed to `lilv_plugin_instantiate()`

### Oboe Integration
- **Callback**: Inherit from `oboe::AudioStreamDataCallback`, implement `onAudioReady()`
- **Buffer management**: Oboe uses interleaved buffers; LV2 expects separate channel buffers
- **Performance mode**: Use `oboe::PerformanceMode::LowLatency` for minimal latency
- **Sample rate**: Let Oboe choose optimal rate, pass to plugin during instantiation
- **Error handling**: Implement `onErrorAfterClose()` for stream disconnection recovery
- **Buffer size**: Use `oboe::DefaultStreamValues::FramesPerBurst` or let Oboe decide

## Common Tasks

### Adding New LV2 Feature Support
1. Define URID in `urids` struct if needed (add to `init_urids()`)
2. Create feature struct in `features` or as member variable
3. Initialize in `init_features()`
4. Add to feature array in `init_instance()`
5. Update `checkFeatures()` if feature is critical

### Debugging Plugin Communication
- **Control ports**: Check `param_dirty` atomic flag, trace parameter updates via JNI
- **Atom messages**: Inspect ringbuffer with `lv2_ringbuffer_read_space()`
- **Worker issues**: Verify `host_worker.iface` is set, check ringbuffer sizes
- **Android logging**: Use `__android_log_print(ANDROID_LOG_DEBUG, "Luma", ...)` outside RT context
- **Oboe**: Monitor underruns with `audioStream->getXRunCount()`

### Preset Handling
- List presets: `get_presets()` queries Lilv for `lv2:Preset` resources
- Apply preset: `apply_preset()` uses `lilv_state_restore()` with `set_port_value` callback
- State features required: `LV2_State_Map_Path`, `Make_Path`, `Free_Path`

### Audio Buffer Conversion
- **Deinterleaving** (Oboe → LV2): Split interleaved stereo into separate L/R buffers
  ```cpp
  for (int i = 0; i < numFrames; i++) {
      leftBuffer[i] = interleavedInput[i * 2];
      rightBuffer[i] = interleavedInput[i * 2 + 1];
  }
  ```
- **Interleaving** (LV2 → Oboe): Merge separate L/R buffers into interleaved stereo
  ```cpp
  for (int i = 0; i < numFrames; i++) {
      interleavedOutput[i * 2] = leftBuffer[i];
      interleavedOutput[i * 2 + 1] = rightBuffer[i];
  }
  ```
- Consider NEON optimization for ARM processors on large buffers

## Limitations & Non-Goals
- **UI**: Headless only—no plugin GUIs, control via Android UI
- **MIDI**: Not currently supported (audio effects only)
- **Session**: No state save/restore yet
- **Graph**: Single plugin, no routing or chaining
- **Presets**: Load-only, no saving functionality
- **Platform**: Android only (API 34+), no Linux/iOS support

These are intentional design choices for simplicity. Don't add features that violate the "minimal reference host" philosophy without discussion.

## Testing & Validation
- Test with audio effects: gain, EQ, reverb, delay
- Verify plugin discovery with Lilv on Android filesystem
- Monitor Oboe underruns: `audioStream->getXRunCount()` should stay at 0
- Test parameter changes from Android UI during audio processing
- Check memory with Android Studio profiler or AddressSanitizer
- Test on physical device running Android 14+ (emulator audio has high latency)
- Verify worker thread with plugins requiring LV2 Worker extension (convolution, IR loaders)
- Test with USB audio interfaces for lower latency
- Profile CPU usage in Oboe callback with Android Profiler
- Test app backgrounding/foregrounding (Oboe stream pause/resume)

## Android-Specific Considerations

### LV2 Plugin Discovery
- Plugins can be bundled in APK assets or downloaded to app-private storage
- Expected path: `/data/data/<package>/files/lv2/` or similar
- Lilv world must be pointed to Android plugin directories
- Bundle `.lv2` directories with manifest.ttl and plugin binaries

### Permission Requirements
- `RECORD_AUDIO` permission for input processing
- Handle runtime permissions in Activity before starting Oboe

### Performance Optimization
- Use `android:hardwareAccelerated="true"` in manifest
- Set thread affinity for audio thread: `sched_setaffinity()` to performance cores
- Consider AAudio backend (Oboe will choose automatically on API 27+)
- Monitor thermal throttling on extended audio processing

### Build Configuration
Application.mk essentials:
```makefile
APP_ABI := arm64-v8a armeabi-v7a
APP_PLATFORM := android-34
APP_STL := c++_shared
APP_CPPFLAGS := -std=c++17 -ffast-math
```

Android.mk essentials:
```makefile
LOCAL_LDLIBS := -llog -lOpenSLES
LOCAL_SHARED_LIBRARIES := oboe lilv-0
LOCAL_CFLAGS := -Wall -Wextra -O3
```
