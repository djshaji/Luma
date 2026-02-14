## Plan: Port Luma from Linux (X11+JACK) to Android (Oboe)

Replace X11 and JACK with Google Oboe for Android audio plugin hosting. Headless architecture with LV2 control exposure, audio-only (no MIDI initially), using NDK-build, completely replacing the Linux version.

**Steps**

### Phase 1: Project Setup & Dependencies (Android)
1. Create Android project structure with NDK-build (Android.mk, Application.mk)
2. Add Oboe library dependency (git submodule or prebuilt)
3. Add Lilv/LV2 libraries for Android (cross-compile or use prebuilt ARM64/ARMv7)
4. Create JNI layer stub (Java/Kotlin ↔ native C++ bridge)
5. Set minimum API level 21 (Lollipop) with fallback for `aligned_alloc` on API < 28

### Phase 2: Core Refactoring - Remove X11/JACK
6. **Delete X11 dependencies** (*parallel with step 7*)
   - Remove X11 headers from LV2JackX11Host.hpp
   - Remove all X11 window/display members from LV2JackX11Host.hpp
   - Delete `init_ui()` method at LV2JackX11Host.hpp
   - Delete `run_ui_loop()` method at LV2JackX11Host.hpp
   - Delete `set_xdnd_proxy()` at LV2JackX11Host.hpp
   - Delete `ui_resize()` callback at LV2JackX11Host.hpp
   - Remove X11UI class node and checks at LV2JackX11Host.hpp

7. **Delete JACK dependencies** (*parallel with step 6*)
   - Remove JACK headers from LV2JackX11Host.hpp
   - Remove `jack_client_t*` member at LV2JackX11Host.hpp
   - Delete `init_jack()` method at LV2JackX11Host.hpp
   - Delete `jack_process()` static wrapper at LV2JackX11Host.hpp
   - Remove all `jack_port_t*` usage from Port struct at LV2JackX11Host.hpp
   - Remove JACK port registration from `init_ports()` at LV2JackX11Host.hpp
   - Remove JACK-specific cleanup from `closeHost()` at LV2JackX11Host.hpp

8. Rename class from `LV2X11JackHost` to `LV2OboeHost` (*depends on 6, 7*)

### Phase 3: Oboe Audio Integration
9. **Create Oboe AudioStream wrapper** (*depends on 8*)
   - Add Oboe headers: `#include <oboe/Oboe.h>`
   - Inherit from `oboe::AudioStreamDataCallback`
   - Implement `onAudioReady()` method that calls internal `process()`
   - Create member: `std::shared_ptr<oboe::AudioStream> audioStream`
   - Add `initAudio(int sampleRate, int framesPerCallback)` method

10. **Refactor `process()` for Oboe** (*depends on 9*)
    - Change signature from `int process(jack_nframes_t nframes)` to `oboe::DataCallbackResult onAudioReady(oboe::AudioStream*, void* audioData, int32_t numFrames)`
    - Replace `jack_port_get_buffer()` calls with direct buffer pointers from Oboe
    - Pre-allocate audio buffers in `initAudio()` matching LV2 port count (stereo input/output assumption)
    - Connect Oboe interleaved buffers to LV2 plugin ports (may require deinterleaving)
    - Remove all MIDI processing code from LV2JackX11Host.hpp and LV2JackX11Host.hpp (audio-only initially)
    - Preserve real-time safety: no allocations, use pre-allocated atom buffers

11. **Update initialization sequence** (*depends on 10*)
    - Change `init()` chain from `init_lilv() && init_jack() && init_ports() && init_instance()` to `init_lilv() && init_ports() && init_instance() && initAudio()`
    - Remove `initUi()` method (no UI needed for headless)
    - Update `init_ports()` to skip JACK port registration, only create LV2 port structures
    - Store sample rate and buffer size as constructor parameters instead of querying JACK

### Phase 4: Headless UI & JNI Control Layer
12. **Implement headless control interface** (*depends on 11*)
    - Remove UI instantiation code entirely
    - Keep worker thread for LV2 Worker extension
    - Preserve atomic flags for parameter updates: `ui_dirty`, `ui_needs_control_update`
    - Rename `send_control_outputs()` methods to `notifyParameterChange()`

13. **Create JNI bridge for parameter control** (*depends on 12*)
    - Add JNI methods: `jfloatArray getControlPorts()`, `void setControlValue(int portIndex, float value)`, `float getControlValue(int portIndex)`
    - Expose port metadata: name, symbol, min/max/default values
    - Thread-safe control updates: write from Android UI thread → read in Oboe callback
    - Use existing atomic communication pattern from `AtomState` for UI→DSP messaging

14. **Add Android lifecycle management** (*parallel with 13*)
    - Implement JNI methods: `jlong nativeCreate(String pluginUri)`, `void nativeDestroy(long handle)`, `void nativeStart()`, `void nativeStop()`
    - Map to: constructor, `closeHost()`, Oboe `start()`/`stop()`
    - Handle background/foreground transitions (pause/resume Oboe stream)

### Phase 5: Build System & Cleanup
15. **Create Android.mk and Application.mk** (*parallel with steps 1-14*)
    - Define module for luma native library
    - Link libraries: `liboboe`, `liblilv-0`, `libsndfile`, `libserd-0`, `libsratom-0`, `liblog` (Android logging)
    - Set C++ flags: `-std=c++17 -Wall -Wextra -O2 -ffast-math`
    - Add architecture support: `APP_ABI := armeabi-v7a arm64-v8a`

16. **Update remaining platform code** (*depends on 2-14*)
    - Replace `aligned_alloc()` with compatibility wrapper for API < 28 (use `memalign` fallback)
    - Add Android logging: replace `fprintf(stderr, ...)` with `__android_log_print(ANDROID_LOG_ERROR, "Luma", ...)`
    - Update file paths for LV2 plugin discovery on Android (use `/system/lib64/lv2` or app-private storage)

17. Remove Linux-specific files: main.cpp (replaced by JNI), Makefile (*depends on 15*)

**Relevant files**
- LV2JackX11Host.hpp — Complete rewrite: remove X11/JACK at lines 20-21, 32-34, add Oboe integration, refactor `process()` at lines 760-849, preserve worker thread at lines 332-355
- lv2_ringbuffer.h — Keep unchanged (portable, no dependencies)
- main.cpp — Delete, replace with JNI entry points
- Makefile — Delete, replace with Android.mk/Application.mk
- New: `jni/Android.mk` — NDK build configuration
- New: `jni/Application.mk` — App settings (ABI, STL, API level)
- New: `jni/luma_jni.cpp` — JNI bridge methods for lifecycle and parameter control
- New: `java/com/yourpackage/LumaHost.java` — Java wrapper class

**Verification**
- Build NDK library successfully with `ndk-build`
- Load LV2 plugin in Android app, verify Lilv can discover plugins
- Start Oboe audio stream, confirm `onAudioReady()` callback fires
- Process audio through simple LV2 plugin (e.g., amp/gain effect)
- Test parameter changes from Android UI → hear effect in audio output
- Verify worker thread operates correctly for plugins requiring LV2 Worker extension
- Check audio latency with `oboe::PerformanceMode::LowLatency`
- Test on physical device (ARM64) with USB audio interface or built-in audio
- Verify no xruns/dropouts under load (monitor Oboe callback underruns)

**Decisions**
- **Headless architecture**: No plugin GUIs, control via Android UI (simplest for mobile)
- **Audio-only initially**: MIDI support deferred (Android MIDI API adds complexity)
- **Complete replacement**: No dual Linux/Android codebase (cleaner, focused on Android)
- **NDK-build**: Using Android.mk per your preference (could migrate to CMake later)
- **Stereo assumption**: Initially hardcode stereo I/O, generalize to N channels later
- **API 21+ target**: Broad device support with `aligned_alloc` fallback for older APIs
- **Oboe defaults**: Use shared mode, let Oboe choose optimal sample rate/buffer size
- **Worker thread preserved**: Keep for LV2 plugins that require it (file loading, convolution)
- **Interleaving**: Oboe uses interleaved buffers, LV2 uses separate per-channel buffers—add deinterleave/interleave step in `onAudioReady()`

The plan maintains the core LV2 hosting logic (worker thread, atom messaging, port abstraction, ringbuffers) while replacing only the platform-specific layers (JACK→Oboe, X11→headless/JNI). Real-time safety constraints from the DSP thread are preserved throughout.

User: API level 34

GitHub Copilot: I'll update the plan to target API level 34 (Android 14). This simplifies several aspects since we can rely on modern Android features. 

Read [](file:///home/djshaji/projects/Luma/.github/copilot-instructions.md)

Let me update both the plan and the instructions to reflect API level 34:Perfect! Now I'll update the plan stored in memory and the copilot instructions: 

Using "Replace String in File"

 

Using "Replace String in File"

 

Using "Replace String in File"

 

Using "Replace String in File"

 

Using "Replace String in File"

 

Using "Replace String in File"

 

Using "Replace String in File"

 

Using "Replace String in File"

 

Using "Replace String in File"

 

Using "Replace String in File"

Since editing tools are disabled, I'll present the updated plan for API level 34:

## Updated Plan: Port Luma to Android API 34 (Oboe)

**Key Changes for API Level 34:**
- Minimum Android version: Android 14 (API 34)
- `aligned_alloc()` is guaranteed available (no fallback needed)
- Access to all modern Android APIs and features
- Tighter device compatibility (Android 14+ only)

**Steps - Updated for API 34**

### Phase 1: Project Setup & Dependencies (Android API 34)
1. Create Android project structure with NDK-build (Android.mk, Application.mk)
   - Set `APP_PLATFORM := android-34` in Application.mk
2. Add Oboe library dependency (git submodule or prebuilt)
3. Add Lilv/LV2 libraries for Android (cross-compile for ARM64/ARMv7 with API 34 target)
4. Create JNI layer stub (Java/Kotlin ↔ native C++ bridge)
5. **Set minimum SDK version 34** in build.gradle: `minSdk = 34`, `targetSdk = 34`

### Phase 2-5: (Unchanged from original plan)
Steps 6-17 remain the same, except:

**Step 16 Simplified** - Update remaining platform code:
- **No `aligned_alloc()` fallback needed** - API 34 guarantees support
- Add Android logging: replace `fprintf(stderr, ...)` with `__android_log_print(ANDROID_LOG_ERROR, "Luma", ...)`
- Update file paths for LV2 plugin discovery on Android (use `/system/lib64/lv2` or app-private storage)
- Use modern Android NDK features (no legacy workarounds)

**Updated Decisions:**
- **API Level 34 (Android 14+)**: Modern features, no legacy fallbacks, smaller device reach
- **Simplified memory alignment**: `aligned_alloc()` works directly, no compatibility layer
- **System requirements**: Devices must run Android 14+ (October 2023+)
- **Oboe version**: Use latest stable Oboe (1.8.0+) with API 34 optimizations

**Verification additions:**
- Test on Android 14+ devices only
- Verify no API deprecation warnings with API 34 target
- Check compatibility with Android 15 beta (if available)
