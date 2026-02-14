# Luma Android Port - Reference Implementation Guide

**Luma** is a minimal LV2 plugin host for Android using Google Oboe for low-latency audio. This is a **headless** implementation (no plugin GUIs) - parameter control is exposed via JNI to the Android UI layer.

**Status**: Planning phase - implementation in progress  
**Target**: Android API 34+ (Android 14+)  
**Audio**: Google Oboe (low-latency)  
**Build**: NDK-build (Android.mk/Application.mk)

---

## Architecture Overview

### From Linux to Android

**Linux version** (current):
- X11 window management for plugin GUIs
- JACK audio I/O with automatic port connections
- CLI interface via main.cpp
- Real-time safe DSP + Worker thread

**Android version** (target):
- Headless (no plugin GUIs)
- Oboe audio I/O (interleaved buffers)
- JNI bridge for parameter control
- Same RT-safe DSP + Worker thread architecture

### Core Principle

**Preserve**: LV2 hosting logic, worker thread, atom messaging, ringbuffers  
**Replace**: Platform layer (JACK→Oboe, X11→JNI, CLI→Android UI)  
**Remove**: All UI-related code (X11 windows, UI instantiation, event loop)

---

## Class Rename: LV2X11JackHost → LV2OboeHost

### Removed Components

```cpp
// ❌ Removed from LV2X11JackHost.hpp
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <jack/jack.h>
#include <jack/midiport.h>

// Removed members:
Display* x_display;
Window x_window;
jack_client_t* jack;
LV2UI_Handle ui_handle;
void* ui_dl;

// Removed methods:
bool init_ui();
void run_ui_loop();
static void set_xdnd_proxy();
static int ui_resize();
static int jack_process();
```

### Added Components

```cpp
// ✅ Added to LV2OboeHost.hpp
#include <oboe/Oboe.h>

// Inherit from Oboe callback
class LV2OboeHost : public oboe::AudioStreamDataCallback {
    
    // Added members:
    std::shared_ptr<oboe::AudioStream> audioStream;
    float* leftChannel;   // Pre-allocated buffers
    float* rightChannel;
    
    // Added methods:
    bool initAudio(int sampleRate, int framesPerBurst);
    oboe::DataCallbackResult onAudioReady(
        oboe::AudioStream* stream,
        void* audioData,
        int32_t numFrames) override;
};
```

---

## Threading Model

### Three Threads (Preserved from Linux)

| Thread | Purpose | Communication | RT-Safe? |
|--------|---------|---------------|----------|
| **Oboe DSP** | Audio processing | Atomics, ringbuffers | ✅ YES |
| **Worker** | Non-RT plugin tasks | Lock-free ringbuffers | ❌ No |
| **Android UI** | Lifecycle, parameters | JNI → atomics | ❌ No |

### Critical: Oboe Callback Real-Time Safety

```cpp
oboe::DataCallbackResult onAudioReady(
    oboe::AudioStream* stream,
    void* audioData,
    int32_t numFrames) override {
    
    // ✅ ALLOWED:
    // - Stack buffers
    // - Pre-allocated memory access
    // - Atomics (acquire/release)
    // - Bounded loops
    
    // ❌ FORBIDDEN:
    // - new/delete/malloc/free
    // - mutex/locks
    // - __android_log_print() (except critical errors)
    // - JNI calls
    // - Unbounded loops
    
    return oboe::DataCallbackResult::Continue;
}
```

---

## Port System Changes

### Port Struct (Adapted for Android)

```cpp
struct Port {
    uint32_t index;
    bool is_audio;      // → Oboe buffers (was: JACK ports)
    bool is_control;    // → JNI getters/setters (was: UI events)
    bool is_atom;       // → Ringbuffers (unchanged)
    bool is_input;
    bool is_midi;       // → Deferred (not implemented initially)
    
    float control;              // Current value
    float defvalue;             // Default value
    LV2_Atom_Sequence* atom;    // Aligned buffer (64-byte)
    AtomState* atom_state;      // Communication state
    
    // ❌ Removed:
    // jack_port_t* jack_port;
};
```

### Audio Port Binding

**Linux (JACK)**:
```cpp
// Separate buffers per port, JACK handles routing
void* buf = jack_port_get_buffer(p.jack_port, nframes);
lilv_instance_connect_port(instance, p.index, buf);
```

**Android (Oboe)**:
```cpp
// Interleaved → Deinterleave → Plugin → Interleave
float* buffer = static_cast<float*>(audioData);

// Deinterleave stereo input
for (int i = 0; i < numFrames; i++) {
    leftChannel[i] = buffer[i * 2];
    rightChannel[i] = buffer[i * 2 + 1];
}

// Connect to plugin
lilv_instance_connect_port(instance, leftPortIndex, leftChannel);
lilv_instance_connect_port(instance, rightPortIndex, rightChannel);

// Process
lilv_instance_run(instance, numFrames);

// Interleave stereo output
for (int i = 0; i < numFrames; i++) {
    buffer[i * 2] = leftChannel[i];
    buffer[i * 2 + 1] = rightChannel[i];
}
```

---

## JNI Bridge Design

### Lifecycle Methods

```cpp
// luma_jni.cpp
extern "C" {

JNIEXPORT jlong JNICALL
Java_com_yourpackage_LumaHost_nativeCreate(
    JNIEnv* env, jobject, jstring pluginUri) {
    
    const char* uri = env->GetStringUTFChars(pluginUri, nullptr);
    auto* host = new LV2OboeHost();
    
    if (!host->init(uri)) {
        delete host;
        env->ReleaseStringUTFChars(pluginUri, uri);
        return 0;
    }
    
    env->ReleaseStringUTFChars(pluginUri, uri);
    return reinterpret_cast<jlong>(host);
}

JNIEXPORT void JNICALL
Java_com_yourpackage_LumaHost_nativeStart(
    JNIEnv*, jobject, jlong handle) {
    
    auto* host = reinterpret_cast<LV2OboeHost*>(handle);
    host->startAudio();
}

JNIEXPORT void JNICALL
Java_com_yourpackage_LumaHost_nativeDestroy(
    JNIEnv*, jobject, jlong handle) {
    
    auto* host = reinterpret_cast<LV2OboeHost*>(handle);
    delete host;  // Calls closeHost()
}

}
```

### Parameter Control

```cpp
JNIEXPORT void JNICALL
Java_com_yourpackage_LumaHost_setParameter(
    JNIEnv*, jobject, jlong handle, jint portIndex, jfloat value) {
    
    auto* host = reinterpret_cast<LV2OboeHost*>(handle);
    host->setControlValue(portIndex, value);
}

// In LV2OboeHost.hpp
void setControlValue(int portIndex, float value) {
    if (portIndex >= 0 && portIndex < ports.size()) {
        ports[portIndex].control = value;
        param_dirty.store(true, std::memory_order_release);
    }
}
```

### Java Wrapper

```java
package com.yourpackage;

public class LumaHost {
    static {
        System.loadLibrary("luma");
    }
    
    private long nativeHandle;
    
    public boolean create(String pluginUri) {
        nativeHandle = nativeCreate(pluginUri);
        return nativeHandle != 0;
    }
    
    public void start() {
        if (nativeHandle != 0) nativeStart(nativeHandle);
    }
    
    public void stop() {
        if (nativeHandle != 0) nativeStop(nativeHandle);
    }
    
    public void destroy() {
        if (nativeHandle != 0) {
            nativeDestroy(nativeHandle);
            nativeHandle = 0;
        }
    }
    
    public void setParameter(int portIndex, float value) {
        if (nativeHandle != 0) setParameter(nativeHandle, portIndex, value);
    }
    
    // Native methods
    private native long nativeCreate(String pluginUri);
    private native void nativeStart(long handle);
    private native void nativeStop(long handle);
    private native void nativeDestroy(long handle);
    private native void setParameter(long handle, int portIndex, float value);
}
```

---

## Oboe Integration

### Initialization

```cpp
bool LV2OboeHost::initAudio(int sampleRate, int framesPerBurst) {
    oboe::AudioStreamBuilder builder;
    
    oboe::Result result = builder
        .setDirection(oboe::Direction::Output)
        .setPerformanceMode(oboe::PerformanceMode::LowLatency)
        .setSharingMode(oboe::SharingMode::Exclusive)
        .setFormat(oboe::AudioFormat::Float)
        .setChannelCount(2)  // Stereo
        .setSampleRate(sampleRate)
        .setFramesPerCallback(framesPerBurst)
        .setDataCallback(this)
        .openStream(audioStream);
    
    if (result != oboe::Result::OK) {
        __android_log_print(ANDROID_LOG_ERROR, "Luma",
            "Failed to open stream: %s", oboe::convertToText(result));
        return false;
    }
    
    // Pre-allocate channel buffers
    leftChannel = new float[framesPerBurst];
    rightChannel = new float[framesPerBurst];
    
    return true;
}

void LV2OboeHost::startAudio() {
    if (audioStream) audioStream->start();
}

void LV2OboeHost::stopAudio() {
    if (audioStream) audioStream->stop();
}
```

### Audio Callback (Replaces JACK process())

```cpp
oboe::DataCallbackResult LV2OboeHost::onAudioReady(
    oboe::AudioStream* stream,
    void* audioData,
    int32_t numFrames) {
    
    if (shutdown.load()) return oboe::DataCallbackResult::Stop;
    
    float* buffer = static_cast<float*>(audioData);
    
    // Deinterleave input
    for (int i = 0; i < numFrames; i++) {
        leftChannel[i] = buffer[i * 2];
        rightChannel[i] = buffer[i * 2 + 1];
    }
    
    // Connect audio ports (assume stereo in/out)
    for (Port& p : ports) {
        if (!p.is_audio) continue;
        
        if (p.is_input) {
            // First input = left, second = right (convention)
            lilv_instance_connect_port(instance, p.index,
                p.index == 0 ? leftChannel : rightChannel);
        } else {
            lilv_instance_connect_port(instance, p.index,
                p.index == 2 ? leftChannel : rightChannel);
        }
    }
    
    // Handle atom input (UI → DSP)
    for (Port& p : ports) {
        if (p.is_atom && p.is_input) {
            if (p.atom_state->ui_to_dsp_pending.exchange(false,
                std::memory_order_acquire)) {
                
                lv2_atom_sequence_clear(p.atom);
                // Append atom from UI buffer
                // (implementation omitted for brevity)
            }
        }
    }
    
    // Run plugin
    lilv_instance_run(instance, numFrames);
    
    // Deliver worker responses
    if (host_worker.iface) deliver_worker_responses(&host_worker);
    
    // Handle atom output (DSP → UI)
    for (Port& p : ports) {
        if (p.is_atom && !p.is_input) {
            LV2_Atom_Sequence* seq = p.atom;
            LV2_ATOM_SEQUENCE_FOREACH(seq, ev) {
                if (ev->body.size == 0) break;
                const uint32_t total = sizeof(LV2_Atom) + ev->body.size;
                if (lv2_ringbuffer_write_space(p.atom_state->dsp_to_ui) >= total) {
                    lv2_ringbuffer_write(p.atom_state->dsp_to_ui,
                        (const char*)&ev->body, total);
                }
            }
        }
    }
    
    // Interleave output
    for (int i = 0; i < numFrames; i++) {
        buffer[i * 2] = leftChannel[i];
        buffer[i * 2 + 1] = rightChannel[i];
    }
    
    return oboe::DataCallbackResult::Continue;
}
```

---

## Build System

### Android.mk

```makefile
LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE    := luma
LOCAL_SRC_FILES := luma_jni.cpp
LOCAL_C_INCLUDES := $(LOCAL_PATH)/../libs/oboe/include \
                    $(LOCAL_PATH)/../libs/lilv/include

LOCAL_CPPFLAGS := -std=c++17 -Wall -Wextra -O3 -ffast-math
LOCAL_LDLIBS   := -llog -lOpenSLES
LOCAL_SHARED_LIBRARIES := oboe lilv-0

include $(BUILD_SHARED_LIBRARY)
```

### Application.mk

```makefile
APP_ABI := arm64-v8a armeabi-v7a
APP_PLATFORM := android-34
APP_STL := c++_shared
APP_CPPFLAGS := -std=c++17 -ffast-math
```

### Build Commands

```bash
ndk-build              # Build
ndk-build clean        # Clean
ndk-build NDK_DEBUG=1  # Debug build
```

---

## Memory Management

### Alignment (Guaranteed on API 34)

```cpp
// Atom buffers: 64-byte aligned (cache line)
p.atom = (LV2_Atom_Sequence*)aligned_alloc(64, p.atom_buf_size);

// No fallback needed - aligned_alloc() guaranteed on API 34+
```

### Cleanup Order in closeHost()

```cpp
void LV2OboeHost::closeHost() {
    // 1. Stop audio
    if (audioStream) {
        audioStream->stop();
        audioStream->close();
    }
    
    // 2. Deactivate plugin
    if (instance) lilv_instance_deactivate(instance);
    
    // 3. Stop worker
    stop_worker();
    
    // 4. Free instance
    if (instance) {
        lilv_instance_free(instance);
        instance = nullptr;
    }
    
    // 5. Free buffers
    delete[] leftChannel;
    delete[] rightChannel;
    
    for (auto& p : ports) {
        if (p.atom) free(p.atom);
        delete p.atom_state;
    }
    ports.clear();
    
    // 6. Free Lilv
    if (world) {
        freeNodes();
        lilv_world_free(world);
        world = nullptr;
    }
}
```

---

## LV2 Feature Changes

### Removed (UI-related)

```cpp
// ❌ No longer provided
LV2_UI__parent
LV2_UI__resize
LV2_UI__portMap
LV2_UI__idleInterface
```

### Preserved (Core hosting)

```cpp
// ✅ Still provided
LV2_URID__map
LV2_URID__unmap
LV2_WORKER__schedule
LV2_STATE__mapPath
LV2_STATE__makePath
LV2_STATE__freePath
LV2_OPTIONS__options
LV2_BUF_SIZE__boundedBlockLength
```

---

## Android Specifics

### Plugin Discovery

```cpp
// Point Lilv to Android paths
std::string pluginPath = "/data/data/com.yourpackage/files/lv2";
lilv_world_load_bundle(world,
    lilv_new_file_uri(world, nullptr, pluginPath.c_str()));
```

### Permissions (AndroidManifest.xml)

```xml
<uses-permission android:name="android.permission.RECORD_AUDIO" />
<uses-permission android:name="android.permission.MODIFY_AUDIO_SETTINGS" />
```

### Logging

```cpp
// Replace fprintf(stderr, ...) with:
__android_log_print(ANDROID_LOG_ERROR, "Luma", "Error: %s", msg);
```

---

## Testing Checklist

- [ ] Load simple gain plugin (amp/volume control)
- [ ] Verify `onAudioReady()` callback fires
- [ ] Test parameter changes from Android UI → hear effect
- [ ] Monitor `audioStream->getXRunCount()` (should stay at 0)
- [ ] Test worker thread with convolution/IR loader plugin
- [ ] Verify preset loading via `apply_preset()`
- [ ] Test on physical Android 14+ device
- [ ] Profile CPU usage in Oboe callback
- [ ] Test with USB audio interface (lower latency)
- [ ] Test app backgrounding/foregrounding

---

## Key Differences from Linux Version

| Aspect | Linux (JACK+X11) | Android (Oboe) |
|--------|------------------|----------------|
| **Audio I/O** | JACK separate buffers | Oboe interleaved → deinterleave/interleave |
| **Sample rate** | JACK server determines | Oboe device native rate |
| **Buffer size** | JACK server setting | Oboe `FramesPerBurst` |
| **UI** | X11 plugin windows | Headless (JNI control only) |
| **MIDI** | JACK MIDI ports | Deferred (not implemented) |
| **Lifecycle** | CLI start/stop | Android service foreground |
| **Logging** | `fprintf(stderr)` | `__android_log_print()` |
| **Threading** | JACK RT + worker | Oboe RT + worker |

---

## References

- [Oboe Documentation](https://github.com/google/oboe/tree/main/docs)
- [LV2 Specification](https://lv2plug.in/)
- [Lilv Manual](https://drobilla.net/docs/lilv/)
- [Android NDK Guide](https://developer.android.com/ndk/guides)

---

**License**: BSD-3-Clause

This is a **reference implementation** - intentionally minimal to demonstrate correct LV2 hosting patterns on Android with real-time safety.