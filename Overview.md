# LV2JackX11Host.hpp - Architecture Overview

## Introduction
Luma is a minimal LV2 plugin host for Linux combining LV2 + JACK + X11 integration. The codebase prioritizes clarity and correctness over feature completeness—it's intentionally small (~1000 lines) to serve as a reference implementation.

The entire host logic is contained in a single header file `LV2JackX11Host.hpp` with one main class `LV2X11JackHost`.

## Class Structure: LV2X11JackHost

### Public Interface

#### Lifecycle Methods
- **`LV2X11JackHost(const char* uri)`** - Constructor taking plugin URI
- **`~LV2X11JackHost()`** - Destructor calls `closeHost()`
- **`bool init()`** - Initialization chain: `init_lilv() → init_jack() → init_ports() → init_instance()`
- **`bool initUi()`** - UI initialization and JACK activation: `init_ui() → jack_activate()`
- **`void closeHost()`** - Ordered cleanup: deactivate instance → stop worker → disconnect JACK → free resources

#### Runtime Methods
- **`void run_ui_loop()`** - Main event loop (~60 FPS)
  - Processes X11 events (window close, resize)
  - Syncs DSP/UI via atomics (`ui_dirty`, `ui_needs_initial_update`, `ui_needs_control_update`)
  - Reads atom messages from `dsp_to_ui` ringbuffer
  - Calls plugin UI idle callback
  - Enables window resize after 30 frames

#### Preset Management
- **`std::vector<PresetInfo> get_presets(const char* plugin_uri)`**
  - Queries Lilv for `lv2:Preset` resources
  - Loads preset metadata into world
  - Returns sorted list by label
- **`void apply_preset(std::string presetUri, std::string presetLabel)`**
  - Loads preset state from world or file
  - Restores state via `lilv_state_restore()` with `set_port_value` callback
  - Triggers UI control update

## Data Structures

### Port System
```cpp
struct Port {
    uint32_t index;
    bool is_audio, is_input, is_control, is_atom, is_midi;
    float control;              // Current control value
    float defvalue;             // Default value
    jack_port_t* jack_port;     // JACK port handle
    LV2_Atom_Sequence* atom;    // Atom buffer (aligned to 64 bytes)
    uint32_t atom_buf_size;     // Buffer size (default 8192, can be larger)
    AtomState* atom_state;      // Communication state
    std::string uri;
    const char* symbol;
};
```

**Port Types & Routing:**
- **Audio ports** → JACK audio ports
- **MIDI ports** → JACK MIDI ports + atom sequences with `LV2_MIDI__MidiEvent`
- **Control ports** → Direct float values
- **Atom ports** → Allocated buffers for arbitrary messages

### Atom Communication
```cpp
struct AtomState {
    std::vector<uint8_t> ui_to_dsp;         // Buffer for UI → DSP messages
    uint32_t ui_to_dsp_type;                // Message type URID
    std::atomic<bool> ui_to_dsp_pending;    // Atomic flag for synchronization
    lv2_ringbuffer_t* dsp_to_ui;            // Ringbuffer for DSP → UI messages
};
```

**Data Flow:**
- **UI → DSP**: UI writes to buffer → sets atomic flag → DSP reads in `process()` → clears flag
- **DSP → UI**: Plugin writes atom → DSP copies to ringbuffer → UI reads in event loop

### Worker Thread
```cpp
struct LV2HostWorker {
    lv2_ringbuffer_t* requests;             // Work requests (UI/DSP → Worker)
    lv2_ringbuffer_t* responses;            // Work responses (Worker → DSP)
    LV2_Worker_Schedule schedule;           // LV2 schedule interface
    LV2_Feature feature;                    // Feature passed to plugin
    const LV2_Worker_Interface* iface;      // Plugin worker interface
    LV2_Handle dsp_handle;                  // Plugin DSP handle
    std::atomic<bool> running;              // Worker thread state
    std::atomic<bool> work_pending;         // Work available flag
    std::thread worker_thread;              // Worker thread handle
};
```

**Worker Flow:**
1. Plugin schedules work via `host_schedule_work()` → writes to `requests` ringbuffer
2. Worker thread reads from `requests` → calls `iface->work()` → writes to `responses`
3. DSP thread calls `deliver_worker_responses()` in `process()` → calls `iface->work_response()`

### URID Mapping
```cpp
struct {
    LV2_URID atom_eventTransfer, atom_Sequence, atom_Object, atom_Float, atom_Int;
    LV2_URID midi_Event, buf_maxBlock, atom_Path;
    LV2_URID patch_Get, patch_Set, patch_property, patch_value;
    LV2_URID atom_Blank, atom_Chunk;
} urids;
```

Maintains bidirectional URI ↔ URID mapping via:
- `map_uri()` - Assigns sequential IDs, stores in `urid_map`/`urid_unmap`
- `unmap_uri()` - Retrieves URI from URID

## Threading Model

### Three Threads

#### 1. DSP Thread (JACK Callback)
**Function:** `int process(jack_nframes_t nframes)`

**Real-Time Safe Operations:**
- Connect audio port buffers via `jack_port_get_buffer()`
- Convert JACK MIDI → LV2 atom sequences
- Check `ui_to_dsp_pending` atomics → copy messages to atom input ports
- Call `lilv_instance_run(instance, nframes)`
- Deliver worker responses via `deliver_worker_responses()`
- Copy atom output → `dsp_to_ui` ringbuffer
- Convert LV2 atoms → JACK MIDI output
- Set `ui_dirty` flag for control outputs

**Critical Constraints:**
- No allocations (uses stack or pre-allocated buffers)
- No locks/mutexes
- Lock-free communication via atomics and ringbuffers
- Minimal conditionals for deterministic timing

#### 2. Worker Thread
**Function:** `worker_thread_func(LV2HostWorker* w)`

**Purpose:** Non-RT tasks requested by plugin (file I/O, heavy computation)

**Operation:**
- Polls `requests` ringbuffer (1ms sleep when empty)
- Reads work data → calls `iface->work()` → writes response
- Responses delivered in next DSP cycle

#### 3. UI Thread (Main)
**Function:** `run_ui_loop()`

**Purpose:** X11 event handling and UI synchronization (~60 FPS)

**Operations:**
- Process X11 events (ClientMessage, ConfigureNotify)
- Exchange atomic flags: `ui_dirty.exchange(false)`
- Send control values: `send_control_outputs()`, `send_initial_ui_values()`
- Read `dsp_to_ui` ringbuffer → forward to `ui_desc->port_event()`
- Call plugin idle: `idle->idle(ui_handle)`

## Initialization Sequence

### 1. `init_lilv()` - LV2 World Setup
- Create Lilv world, load all plugins
- Find plugin by URI
- Create Lilv nodes for port classes (audio, control, atom, input, X11 UI)
- Initialize URIDs via `init_urids()`
- Initialize LV2 features via `init_features()`
- Check resize-port requirements (updates `required_atom_size`)

### 2. `init_jack()` - JACK Client
- Open JACK client with plugin name
- Set process callback: `jack_set_process_callback(jack, jack_process, this)`
- Store buffer size: `max_block_length = jack_get_buffer_size(jack)`

### 3. `init_ports()` - Port Discovery & Registration
- Iterate all plugin ports via `lilv_plugin_get_num_ports()`
- Create `Port` structs with flags (`is_audio`, `is_control`, `is_atom`, `is_midi`)
- Register JACK ports for audio/MIDI
- Allocate atom buffers (64-byte aligned via `aligned_alloc(64, size)`)
- Create `AtomState` for atom ports
- Extract default values for control inputs

### 4. `init_instance()` - Plugin Instantiation
- Build LV2 options (buffer size, etc.)
- Check required features via `checkFeatures()`
- Instantiate plugin: `lilv_plugin_instantiate(plugin, sample_rate, features)`
- Query worker interface: `lilv_instance_get_extension_data()`
- Start worker thread if needed (creates 8KB ringbuffers)
- Connect control/atom ports: `lilv_instance_connect_port()`
- Activate: `lilv_instance_activate(instance)`

### 5. `init_ui()` - UI Setup
- Find X11 UI via `lilv_plugin_get_uis()`
- Load UI shared library: `dlopen(so, RTLD_NOW)`
- Get UI descriptor: `dlsym(ui_dl, "lv2ui_descriptor")`
- Create X11 window: `XCreateSimpleWindow()`
- Enable XdndAware for drag-and-drop
- Setup UI features (parent window, resize, port map)
- Instantiate UI: `ui_desc->instantiate()`
- Set XdndProxy on plugin widget: `set_xdnd_proxy()`
- Copy window size hints from plugin widget

## LV2 Feature Support

### Implemented Features
1. **LV2_URID__map / LV2_URID__unmap** - URI ↔ URID mapping
2. **LV2_WORKER__schedule** - Worker thread for non-RT tasks
3. **LV2_STATE__mapPath / makePath / freePath** - Preset file path handling
4. **LV2_OPTIONS__options** - Buffer size options
5. **LV2_BUF_SIZE__boundedBlockLength** - Bounded block length guarantee
6. **LV2_UI__parent** - Parent window handle for UI
7. **LV2_UI__resize** - UI resize callback
8. **LV2_UI__portMap** - Port symbol → index mapping

### Feature Checking
- **`checkFeatures()`** - Validates all required features are provided
- Returns `false` if unsupported required feature found
- Logs missing feature URIs to stderr

## Memory Management

### Alignment Requirements
- **Atom buffers**: 64-byte aligned (`aligned_alloc(64, size)`)
- **Ringbuffer atomics**: 64-byte aligned (cache line size)
- Prevents false sharing between threads

### Cleanup Order (Critical)
1. Deactivate instance: `lilv_instance_deactivate()`
2. Stop worker: `host_worker.running = false` → join thread
3. Destroy UI: `ui_desc->cleanup(ui_handle)`
4. Close UI library: `dlclose(ui_dl)`
5. Disconnect & unregister JACK ports
6. Deactivate & close JACK client
7. Free plugin instance: `lilv_instance_free()`
8. Close X11 display
9. Free port atom buffers and atom states
10. Free Lilv nodes: `freeNodes()`
11. Free Lilv world: `lilv_world_free()`

### RAII Patterns
- Most resources use C APIs (Lilv, JACK, X11)
- Cleanup happens in `closeHost()` called by destructor
- Atomics and `std::vector` provide automatic memory safety

## X11 Integration

### Window Management
- **Main window**: Created with `XCreateSimpleWindow()`
- **Plugin widget**: Provided by UI instantiation, embedded in main window
- **Resize handling**: Bidirectional
  - Host window resize → plugin resize (if `resize_enabled`)
  - Plugin resize request → `ui_resize()` callback

### Drag-and-Drop Support
- **XdndAware**: Set on main window to indicate DnD support
- **XdndProxy**: Set on plugin widget and all ancestors
  - Allows plugin to receive drag-and-drop events
  - Implemented in `set_xdnd_proxy()` - walks window hierarchy to root

### Thread Safety
- **`XInitThreads()`** - Called in `main()` before any X11 operations
- **`XLockDisplay()` / `XUnlockDisplay()`** - Used in `ui_resize()`
- Required because X11 calls from multiple threads (UI event loop + possible plugin threads)

## Communication Patterns

### Control Ports (Float Values)
**DSP → UI:**
1. Plugin writes to `Port::control` in `process()`
2. DSP sets `ui_dirty.store(true)`
3. UI checks `ui_dirty.exchange(false)` in event loop
4. UI calls `send_control_outputs()` → `ui_desc->port_event()`

**UI → DSP:**
1. UI writes to `Port::control` via `ui_write()` callback
2. DSP reads value in next `process()` cycle
3. No explicit synchronization needed (atomic float operations)

### Atom Ports (Variable-Size Messages)
**UI → DSP:**
1. UI calls `ui_write()` with atom data
2. Copies data to `AtomState::ui_to_dsp` buffer
3. Sets `AtomState::ui_to_dsp_pending.store(true, memory_order_release)`
4. DSP checks `ui_to_dsp_pending.exchange(false)` in `process()`
5. Appends atom to sequence via `lv2_atom_sequence_append_event()`

**DSP → UI:**
1. Plugin writes atom sequence to output port
2. DSP iterates via `LV2_ATOM_SEQUENCE_FOREACH()`
3. Writes atoms to `AtomState::dsp_to_ui` ringbuffer
4. UI reads ringbuffer via `lv2_ringbuffer_read()`
5. Forwards to `ui_desc->port_event()`

### MIDI Handling
**Input (JACK → Plugin):**
1. `jack_midi_get_event_count()` → iterate events
2. Wrap each in `LV2_Atom_Event` with `midi_Event` URID
3. Append to atom sequence via `lv2_atom_sequence_append_event()`

**Output (Plugin → JACK):**
1. Iterate atom sequence from plugin
2. Extract MIDI data from atoms with `midi_Event` type
3. Write to JACK MIDI buffer via `jack_midi_event_write()`

## Ringbuffer Implementation

### File: `lv2_ringbuffer.h`
Lock-free single-producer single-consumer ringbuffer inspired by JACK.

### Key Properties
- **Size**: Must be power-of-two (verified by `is_power_of_two()`)
- **Masking**: Uses `size_mask = size - 1` for fast modulo
- **Alignment**: 64-byte aligned atomics to prevent false sharing

### Atomics
```cpp
alignas(64) std::atomic<size_t> write_ptr;
alignas(64) std::atomic<size_t> read_ptr;
```

- **Write**: `memory_order_relaxed` load, `memory_order_release` store
- **Read**: `memory_order_acquire` load, `memory_order_release` store
- Ensures visibility of data written before pointer update

### Operations
- **`lv2_ringbuffer_create(size)`** - Allocate ringbuffer (power-of-two check)
- **`lv2_ringbuffer_free(rb)`** - Free ringbuffer
- **`lv2_ringbuffer_read_space(rb)`** - Available bytes to read
- **`lv2_ringbuffer_write_space(rb)`** - Available bytes to write
- **`lv2_ringbuffer_peek(rb, dst, cnt)`** - Read without advancing pointer
- **`lv2_ringbuffer_read(rb, dst, cnt)`** - Read and advance pointer
- **`lv2_ringbuffer_write(rb, src, cnt)`** - Write and advance pointer

## Preset System

### Discovery: `get_presets()`
1. Query plugin for related resources with `lv2:Preset` class
2. Load each preset resource into world: `lilv_world_load_resource()`
3. Extract label from RDF predicate: `rdfs:label`
4. Build `PresetInfo` vector, sort alphabetically

### Application: `apply_preset()`
1. Create Lilv node from preset URI
2. Attempt load from world: `lilv_state_new_from_world()`
3. Fallback to file: `lilv_state_new_from_file()` if world fails
4. Restore state: `lilv_state_restore(state, instance, set_port_value, ...)`
5. Callback `set_port_value()` receives port symbol and value
6. Matches symbol to port, updates `Port::control`
7. Set `ui_needs_control_update` flag for UI sync

### State Features
Required for preset loading:
- **LV2_State_Map_Path**: Abstract path → filesystem path
- **LV2_State_Make_Path**: Create directories for state files
- **LV2_State_Free_Path**: Free path strings

Current implementation uses simple `strdup()` (no actual path mapping).

## Error Handling

### Initialization Failures
- All `init_*()` methods return `bool`
- Chain stops on first failure: `init_lilv() && init_jack() && ...`
- `main.cpp` exits with return code 1 on failure

### Runtime Errors
- **JACK callback**: Cannot report errors (real-time context)
- **Worker**: Ringbuffer full → return `LV2_WORKER_ERR_NO_SPACE`
- **UI events**: Window close → sets `run = false`, calls `closeHost()`

### Diagnostics
- Stderr logging for critical errors
- Commented-out debug logs in `process()` (real-time unsafe)
- No logging library (keeps dependencies minimal)

## Design Principles

### Minimalism
- Single header file (~1000 lines)
- Only essential LV2 features
- No plugin chaining, routing, or session management
- No preset saving (load-only)

### Clarity Over Performance
- Explicit code over clever abstractions
- Comments mark major sections
- Readable variable names
- Linear control flow where possible

### Real-Time Safety
- Strict separation of RT and non-RT code
- Lock-free communication via atomics and ringbuffers
- Pre-allocated buffers in audio thread
- Worker thread for blocking operations

### Reference Implementation
- Demonstrates correct LV2 host patterns
- Shows proper threading model
- Documents critical timing and ordering

## Limitations & Non-Goals

### What's NOT Supported
- **UI Types**: Only X11 (no GTK, Qt, external UIs)
- **Session**: No JACK session, NSM, or state save
- **Graph**: Single plugin instance, no routing
- **Features**: Limited to core + worker + state
- **Presets**: Load-only, no user preset creation

### Intentional Omissions
These keep the codebase small and focused:
- No configuration system
- No MIDI learn
- No automation recording
- No plugin scaning/caching
- No latency compensation

## Testing Recommendations

### Plugin Compatibility
- Test with synthesizers (e.g., `urn:brummer:neuralrack`)
- Test with effects (reverb, delay with large atom buffers)
- Test worker-requiring plugins (convolution, samplers)

### Real-Time Validation
- Run with `jack_cpu_load` monitoring
- Use `valgrind --leak-check=full` in debug builds
- Check for xruns with JACK under load

### Port Verification
- Use `jack_lsp` to verify port registration
- Test MIDI input with virtual keyboard
- Verify automatic port connections

## Future Extensions

Potential additions that maintain the minimal philosophy:
- GTK3 UI support (small patch)
- Basic state save/restore (extend preset system)
- JACK transport support (few lines)
- Command-line port mapping
- Simple plugin scanning tool
