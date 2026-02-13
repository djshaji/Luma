
/*
 * LV2JackX11Host.hpp
 *
 * SPDX-License-Identifier:  BSD-3-Clause
 *
 * Copyright (C) 2026 brummer <brummer@web.de>
 */


/****************************************************************
        LV2JackX11Host.h - a LV2 Host for X11 based plugins

****************************************************************/

//  g++ -g main.cpp -o lv2host `pkg-config --cflags --libs jack lilv-0 x11` -ldl

#pragma once

#include <jack/jack.h>
#include <jack/midiport.h>

#include "lv2_ringbuffer.h"
#include <lilv/lilv.h>

#include <lv2/ui/ui.h>
#include <lv2/urid/urid.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/atom/forge.h>
#include <lv2/midi/midi.h>
#include <lv2/options/options.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/patch/patch.h>
#include <lv2/worker/worker.h>
#include <lv2/state/state.h>
#include <lv2/resize-port/resize-port.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <dlfcn.h>
#include <unistd.h>

#include <vector>
#include <string>
#include <atomic>
#include <unordered_map>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <cassert>
#include <ctime>
#include <algorithm>

/****************************************************************
        LV2X11JackHost - class to host LV2 plugins with X11 GUI's

****************************************************************/

class LV2X11JackHost {
public:
    explicit LV2X11JackHost(const char* uri)
        : plugin_uri(uri) {}

    ~LV2X11JackHost() {
        closeHost();
    }

    bool init() {
        return init_lilv()
            && init_jack()
            && init_ports()
            && init_instance();
    }

    bool initUi() {
        return init_ui()
            && jack_activate(jack) == 0;
    }

    void closeHost() {
        if (instance) {
            lilv_instance_deactivate(instance);
        }
        stop_worker();
        destroy_ui();
        if (ui_dl) {
            dlclose(ui_dl);
            ui_dl = nullptr;
        }

        if (jack) {
            for (auto& p : ports) {
                if (p.is_audio) {
                    if (jack_port_connected(p.jack_port)) {
                        jack_port_disconnect(jack,p.jack_port);
                    }
                    jack_port_unregister(jack,p.jack_port);
                }
                if (p.is_atom &&  p.is_midi) {
                    if (jack_port_connected(p.jack_port)) {
                        jack_port_disconnect(jack,p.jack_port);
                    }
                    jack_port_unregister(jack,p.jack_port);
                }
            }

            jack_deactivate(jack);
            jack_client_close(jack);
            jack = nullptr;
        }

        if (instance) {
            lilv_instance_free(instance);
            instance = nullptr;
        }

        if (x_display) {
            if (x_window) {
                XDestroyWindow(x_display, x_window);
            }
            XCloseDisplay(x_display);
            x_window = 0;
            x_display = nullptr;
        }

        for (auto& p : ports) {
            if (p.atom)
                free(p.atom);
            delete p.atom_state;
        }
        ports.clear();

        if (world) {
            freeNodes();
            lilv_world_free(world);
            world = nullptr;
        }        
    }

/****************************************************************
                        UI LOOP

****************************************************************/

    void run_ui_loop() {
        const LV2UI_Idle_Interface* idle = (const LV2UI_Idle_Interface*)
                            ui_desc->extension_data(LV2_UI__idleInterface);

        Atom WM_DELETE_WINDOW = XInternAtom(x_display, "WM_DELETE_WINDOW", False);
        Atom WM_PROTOCOLS = XInternAtom(x_display, "WM_PROTOCOLS", False);
        XSetWMProtocols(x_display, x_window, &WM_DELETE_WINDOW, 1);
        run.store(true, std::memory_order_release);

        while (run.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            while (XPending(x_display)) {
                XEvent ev;
                XNextEvent(x_display, &ev);
                if (ev.type == ClientMessage) {
                    if ((Atom)ev.xclient.message_type == WM_PROTOCOLS &&
                        (Atom)ev.xclient.data.l[0] == WM_DELETE_WINDOW) {
                        fprintf(stderr, "Exit\n");
                        shutdown.store(true, std::memory_order_release);
                        run.store(false, std::memory_order_release);
                        closeHost();
                        return;
                    }
                }
            }

            if (ui_dirty.exchange(false)) send_control_outputs();
            if (ui_needs_initial_update.exchange(false))
                send_initial_ui_values();
            if (ui_needs_control_update.exchange(false))
                send_control_values();

            for (auto& p : ports) {
                if (!p.is_atom || p.is_input) continue;

                auto* rb = p.atom_state->dsp_to_ui;
                while (lv2_ringbuffer_read_space(rb) >= sizeof(LV2_Atom)) {
                    LV2_Atom hdr;
                    lv2_ringbuffer_peek(rb, (char*)&hdr, sizeof(LV2_Atom));
                    const uint32_t total = sizeof(LV2_Atom) + hdr.size;
                    if (lv2_ringbuffer_read_space(rb) < total) break;
                    std::vector<uint8_t> buf(total);
                    lv2_ringbuffer_read(rb, (char*)buf.data(), total);
                    ui_desc->port_event(ui_handle, p.index, total,
                                urids.atom_eventTransfer, buf.data());
                }
            }
            // run plugin UI idle loop
            if (idle) idle->idle(ui_handle);
        }
    }
/****************************************************************
            Preset - list available presets for plugin

****************************************************************/

    struct PresetInfo {
        std::string uri;
        std::string label;
    };

    std::vector<PresetInfo> get_presets(const char* plugin_uri) {

        std::vector<PresetInfo> result;
        LilvNode* uri = lilv_new_uri(world, plugin_uri);

        const LilvPlugin* plugin =
            lilv_plugins_get_by_uri(
                lilv_world_get_all_plugins(world),
                uri);

        if (!plugin) {
            std::cerr << "Plugin not found\n";
            lilv_node_free(uri);
            return result;
        }

        LilvNode* preset_class = lilv_new_uri(world,
                    "http://lv2plug.in/ns/ext/presets#Preset");

        const LilvNodes* presets = lilv_plugin_get_related(plugin, preset_class);

        if (!presets || lilv_nodes_size(presets) == 0) {
            lilv_node_free(preset_class);
            lilv_node_free(uri);
            return result;
        }

        LilvNode* label_pred = lilv_new_uri(world,
                    "http://www.w3.org/2000/01/rdf-schema#label");

        LILV_FOREACH(nodes, i, presets) {
            const LilvNode* preset = lilv_nodes_get(presets, i);
            // load preset into world
            lilv_world_load_resource(world, preset);
            PresetInfo info;
            info.uri = lilv_node_as_uri(preset);
            LilvNode* label = lilv_world_get(world, preset, label_pred, nullptr);

            if (label && lilv_node_is_string(label)) {
                info.label = lilv_node_as_string(label);
                lilv_node_free(label);
            } else {
                info.label = "(no label)";
            }
            result.push_back(info);
        }

        lilv_node_free(label_pred);
        lilv_node_free(preset_class);
        lilv_node_free(uri);

        std::sort(result.begin(), result.end(),
            [](const PresetInfo& a, const PresetInfo& b) {
                return a.label < b.label;
            });
        return result;
    }

/****************************************************************
            STATE - load a preset

****************************************************************/

    static void set_port_value(const char* port_symbol, void* user_data,
                   const void* value, uint32_t size, uint32_t type) {

        (void) size;
        (void) type;
        auto* self = static_cast<LV2X11JackHost*>(user_data);
        for (auto& p : self->ports) {
            if (!p.is_control) continue;
            if (strcmp(p.symbol, port_symbol) == 0) {
                if (size == sizeof(float)) p.control = *(const float*)value;
                break;
            }
        }
    }

    static char* make_path_func(LV2_State_Make_Path_Handle, const char* path) {
        return strdup(path);
    }

    static char* map_path_func(LV2_State_Map_Path_Handle, const char* abstract_path) {
        return strdup(abstract_path);
    }

    static void free_path_func(LV2_State_Free_Path_Handle, char* path) {
        free(path);
    }

    LV2_State_Map_Path map_path;
    LV2_State_Make_Path make_path;
    LV2_State_Free_Path free_path;

    void apply_preset(std::string presetUri, std::string presetLabel) {
        preset_uri = presetUri;
        preset_label = presetLabel;

        LilvNode* preset = lilv_new_uri(world, preset_uri.c_str());
        if (!preset) {
            fprintf(stderr, "Invalid preset URI\n");
            ui_needs_initial_update.store(true);
            return ;
        }

        LilvState* state = lilv_state_new_from_world(world, &um, preset);

        if (!state) {
            char* path = lilv_file_uri_parse(preset_uri.c_str(), nullptr);
            if (!path) {
                fprintf(stderr, "Preset not found\n");
                lilv_node_free(preset);
                ui_needs_initial_update.store(true);
                return ;
            }

            LilvState* state = lilv_state_new_from_file(world, &um, nullptr, path);
            free(path);

            if (!state) {
                fprintf(stderr, "Failed to load preset\n");
                lilv_node_free(preset);
                ui_needs_initial_update.store(true);
                return ;
            }
        }

        const LV2_Feature* feat[] = {
            &features.um_f,
            &features.unm_f,
            &features.map_path_feature,
            &features.make_path_feature,
            &features.free_path_feature,
            &host_worker.feature,
            nullptr
        };

        lilv_state_restore(state, instance, set_port_value, this, 0, feat);

        lilv_state_free(state);
        lilv_node_free(preset);

        ui_needs_control_update.store(true);
        ui_needs_initial_update.store(false);
    }

private:

/****************************************************************
                        WORKER

****************************************************************/

    struct WorkerRequest {
        uint32_t size;
        uint8_t  data[0];
    };

    struct WorkerResponse {
        uint32_t size;
        uint8_t  data[0];
    };

    struct LV2HostWorker {
        lv2_ringbuffer_t* requests;
        lv2_ringbuffer_t* responses;

        LV2_Worker_Schedule schedule;
        LV2_Feature feature;
        const LV2_Worker_Interface* iface;
        LV2_Handle dsp_handle;

        std::atomic<bool> running;
        std::atomic<bool> work_pending;
        std::thread worker_thread;
    };

    // store work request in ringbuffer
    static LV2_Worker_Status host_schedule_work(
                        LV2_Worker_Schedule_Handle handle,
                        uint32_t size, const void* data) {

        auto* w = (LV2HostWorker*)handle;
        const size_t total = sizeof(uint32_t) + size;
        if (lv2_ringbuffer_write_space(w->requests) < total)
            return LV2_WORKER_ERR_NO_SPACE;

        lv2_ringbuffer_write(w->requests, (const char*)&size, sizeof(uint32_t));
        lv2_ringbuffer_write(w->requests, (const char*)data, size);
        w->work_pending.store(true, std::memory_order_release); 

        return LV2_WORKER_SUCCESS;
    }

    // worker thread, check if work is to be done, and do it
    static void worker_thread_func(LV2HostWorker* w) {
        while (w->running.load()) {
            if (lv2_ringbuffer_read_space(w->requests) < sizeof(uint32_t)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            if (lv2_ringbuffer_read_space(w->requests) < sizeof(uint32_t)) {
                continue;
            }

            uint32_t size;
            lv2_ringbuffer_peek(w->requests, (char*)&size, sizeof(uint32_t));

            if (lv2_ringbuffer_read_space(w->requests) < sizeof(uint32_t) + size) {
                 continue;
            }

            lv2_ringbuffer_read(w->requests, (char*)&size, sizeof(uint32_t));
            std::vector<uint8_t> buf(size);
            lv2_ringbuffer_read(w->requests, (char*)buf.data(), size);
            w->iface->work(w->dsp_handle, host_respond, w, size, buf.data());
        }
    }

    // store response in ringbuffer when work is done
    static LV2_Worker_Status host_respond(
                        LV2_Worker_Respond_Handle handle,
                        uint32_t size, const void* data) {

        auto* w = (LV2HostWorker*)handle;
        const size_t total = sizeof(uint32_t) + size;

        if (lv2_ringbuffer_write_space(w->responses) < total)
            return LV2_WORKER_ERR_NO_SPACE;

        lv2_ringbuffer_write(w->responses, (const char*)&size, sizeof(uint32_t));

        lv2_ringbuffer_write(w->responses,(const char*)data, size);

        return LV2_WORKER_SUCCESS;
    }

    // inform plugin when work is done
    void deliver_worker_responses(LV2HostWorker* w) {
        while (true) {
            if (lv2_ringbuffer_read_space(w->responses) < sizeof(uint32_t)) break;

            uint32_t size;
            lv2_ringbuffer_peek(w->responses, (char*)&size, sizeof(uint32_t));

            if (lv2_ringbuffer_read_space(w->responses) < sizeof(uint32_t) + size) break;

            lv2_ringbuffer_read(w->responses, (char*)&size, sizeof(uint32_t));

            std::vector<uint8_t> buf(size);
            lv2_ringbuffer_read(w->responses, (char*)buf.data(), size);

            w->iface->work_response(w->dsp_handle, size, buf.data());
        }
    }

    // stop worker thread on exit
    void stop_worker() {
        if (!host_worker.running.exchange(false))
            return;

        if (host_worker.worker_thread.joinable())
            host_worker.worker_thread.join();

        if (host_worker.requests) {
            lv2_ringbuffer_free(host_worker.requests);
            host_worker.requests = nullptr;
        }

        if (host_worker.responses) {
            lv2_ringbuffer_free(host_worker.responses);
            host_worker.responses = nullptr;
        }

        host_worker.iface = nullptr;
        host_worker.dsp_handle = nullptr;
    }

    LV2HostWorker host_worker;

/****************************************************************
                        PORT DATA

****************************************************************/

    struct AtomState {
        std::vector<uint8_t> ui_to_dsp;
        uint32_t ui_to_dsp_type = 0;
        std::atomic<bool> ui_to_dsp_pending{false};

        lv2_ringbuffer_t* dsp_to_ui = nullptr;

        AtomState(size_t sz = 16384) {
            dsp_to_ui = lv2_ringbuffer_create(sz);
        }

        ~AtomState() {
            lv2_ringbuffer_free(dsp_to_ui);
        }
    };

    struct Port {
        uint32_t index = 0;
        bool is_audio = false;
        bool is_input = false;
        bool is_control = false;
        bool is_atom = false;
        bool is_midi = false;

        float control = 0.0f;
        float defvalue = 0.0f;
        jack_port_t* jack_port = nullptr;

        LV2_Atom_Sequence* atom = nullptr;
        uint32_t atom_buf_size = 8192;
        AtomState* atom_state = nullptr;

        std::string uri;
        const char* symbol = nullptr;
    };

/****************************************************************
                        URIDs

****************************************************************/

    struct {
        LV2_URID atom_eventTransfer;
        LV2_URID atom_Sequence;
        LV2_URID atom_Object;
        LV2_URID atom_Float;
        LV2_URID atom_Int;
        LV2_URID midi_Event;
        LV2_URID buf_maxBlock;
        LV2_URID atom_Path;
        LV2_URID patch_Get;
        LV2_URID patch_Set;
        LV2_URID patch_property;
        LV2_URID patch_value;
        LV2_URID atom_Blank;
        LV2_URID atom_Chunk;
    } urids;

    void init_urids() {
        urids.atom_eventTransfer = map_uri(this, LV2_ATOM__eventTransfer);
        urids.atom_Sequence = map_uri(this, LV2_ATOM__Sequence);
        urids.atom_Blank    = map_uri(this, LV2_ATOM__Blank);
        urids.atom_Chunk    = map_uri(this, LV2_ATOM__Chunk);
        urids.atom_Object   = map_uri(this, LV2_ATOM__Object);
        urids.atom_Float    = map_uri(this, LV2_ATOM__Float);
        urids.atom_Int      = map_uri(this, LV2_ATOM__Int);
        urids.midi_Event    = map_uri(this, LV2_MIDI__MidiEvent);
        urids.buf_maxBlock  = map_uri(this, LV2_BUF_SIZE__maxBlockLength);
        urids.atom_Path     = map_uri(this, LV2_ATOM__Path);

        urids.patch_Get     = map_uri(this, LV2_PATCH__Get);
        urids.patch_Set     = map_uri(this, LV2_PATCH__Set);
        urids.patch_property= map_uri(this, LV2_PATCH__property);
        urids.patch_value   = map_uri(this, LV2_PATCH__value);
    }

    static LV2_URID map_uri(LV2_URID_Map_Handle h, const char* uri) {
        auto* self = static_cast<LV2X11JackHost*>(h);
        auto it = self->urid_map.find(uri);
        if (it != self->urid_map.end())
            return it->second;

        LV2_URID id = self->urid_map.size() + 1;
        self->urid_map[uri] = id;
        self->urid_unmap[id]  = uri;
        return id;
    }

    static const char* unmap_uri(LV2_URID_Unmap_Handle h, LV2_URID urid) {
        auto* self = static_cast<LV2X11JackHost*>(h);

        auto it = self->urid_unmap.find(urid);
        if (it == self->urid_unmap.end())
            return nullptr;

        return it->second.c_str();
    }

    std::unordered_map<std::string, LV2_URID> urid_map;
    std::unordered_map<LV2_URID, std::string> urid_unmap;

    LV2_URID_Map um;
    LV2_URID_Unmap unm;

/****************************************************************
                        FEATURES

****************************************************************/
    struct {
        LV2_Feature um_f;
        LV2_Feature unm_f;

        LV2_Feature map_path_feature;
        LV2_Feature make_path_feature;
        LV2_Feature free_path_feature;
        LV2_Feature bbl_feature;
    } features;

    void init_features() {
        um.handle = this;
        um.map = map_uri;
        unm.handle = this;
        unm.unmap = unmap_uri;

        map_path.handle      = nullptr;
        map_path.abstract_path = map_path_func;
        make_path.handle = nullptr;
        make_path.path   = make_path_func;
        free_path.handle = nullptr;
        free_path.free_path = free_path_func;
        
        features.bbl_feature.URI  = LV2_BUF_SIZE__boundedBlockLength;
        features.bbl_feature.data = NULL;

        features.um_f.URI = LV2_URID__map;
        features.um_f.data = &um;

        features.unm_f.URI = LV2_URID__unmap;
        features.unm_f.data = &unm;

        features.map_path_feature.URI = LV2_STATE__mapPath;
        features.map_path_feature.data = &map_path;

        features.make_path_feature.URI = LV2_STATE__makePath;
        features.make_path_feature.data = &make_path;

        features.free_path_feature.URI = LV2_STATE__freePath;
        features.free_path_feature.data = &free_path;

        host_worker.schedule.handle = &host_worker;
        host_worker.schedule.schedule_work = host_schedule_work;
        host_worker.feature.URI  = LV2_WORKER__schedule;
        host_worker.feature.data = &host_worker.schedule;
    }

/****************************************************************
        LILV - init world and check if plugin is supported

****************************************************************/

    bool feature_is_supported(const char* uri, const LV2_Feature*const* f) {
        for (; *f; ++f)
            if (!strcmp(uri, (*f)->URI)) return true;
        return false;
    }

    bool check_resize_port_requirements(const LilvPlugin* plugin) {
        uint32_t n = lilv_plugin_get_num_ports(plugin);
        LilvNode* min_size =
            lilv_new_uri(world, LV2_RESIZE_PORT__minimumSize);
        bool ok = true;

        for (uint32_t i = 0; i < n; ++i) {
            const LilvPort* port = lilv_plugin_get_port_by_index(plugin, i);
            if (!lilv_port_is_a(plugin, port, atom_class)) continue;
            LilvNodes* sizes = lilv_port_get_value(plugin, port, min_size);
            if (!sizes || lilv_nodes_size(sizes) == 0) continue;
            const LilvNode* n = lilv_nodes_get_first(sizes);
            uint32_t required = lilv_node_as_int(n);

            //fprintf(stderr, "Atom port %s requires minimumSize = %u\n",
            //    lilv_node_as_string(lilv_port_get_symbol(plugin, port)),required);

            if (required > required_atom_size) {
                required_atom_size = required;
                //ok = false; // in case we don't want support resize
            }
            lilv_nodes_free(sizes);
        }
        lilv_node_free(min_size);
        return ok;
    }

    bool checkFeatures(const LilvPlugin* plugin, const LV2_Feature*const* feat) {
        LilvNodes* requests = lilv_plugin_get_required_features(plugin);
        LILV_FOREACH(nodes, f, requests) {
            const char* uri = lilv_node_as_uri(lilv_nodes_get(requests, f));
            if (!feature_is_supported(uri, feat)) {
                //fprintf(stderr, "Plugin %s \n", lilv_node_as_string(lilv_plugin_get_uri(plugin)));
                fprintf(stderr, "Feature %s is not supported\n", uri);
                lilv_nodes_free(requests);
                return false;
            }
        }
        lilv_nodes_free(requests);
        return true;
    }

    bool init_lilv() {
        world = lilv_world_new();
        lilv_world_load_all(world);

        const LilvPlugins* plugs = lilv_world_get_all_plugins(world);
        plugin = lilv_plugins_get_by_uri(plugs, lilv_new_uri(world, plugin_uri));
        if (!plugin) return false;
        plugin_name = "lv2-x11-host";
        LilvNode* nd = nullptr;
        nd = lilv_plugin_get_name(plugin);
        plugin_name = lilv_node_as_string(nd);
        lilv_node_free(nd);

        audio_class     = lilv_new_uri(world, LV2_CORE__AudioPort);
        control_class   = lilv_new_uri(world, LV2_CORE__ControlPort);
        atom_class      = lilv_new_uri(world, LV2_ATOM__AtomPort);
        input_class     = lilv_new_uri(world, LV2_CORE__InputPort);
        x11_class       = lilv_new_uri(world, LV2_UI__X11UI);
        rsz_minimumSize = lilv_new_uri (world, LV2_RESIZE_PORT__minimumSize);
        init_urids();
        init_features();
        if (!check_resize_port_requirements(plugin)) {
            fprintf(stderr,"%s requires resize-port support – not supported\n", plugin_name.data());
            return false;
            }

        return true;
    }

    void freeNodes() {
        lilv_node_free (audio_class);
        lilv_node_free (control_class);
        lilv_node_free (atom_class);
        lilv_node_free (input_class);
        lilv_node_free (x11_class);
        lilv_node_free (rsz_minimumSize);
    }

/****************************************************************
        JACK - check if jack is running and open client

****************************************************************/

    static int jack_process(jack_nframes_t n, void* arg) {
        return static_cast<LV2X11JackHost*>(arg)->process(n);
    }

    bool init_jack() {
        jack = jack_client_open(plugin_name.data(), JackNullOption, nullptr);
        if (!jack) return false;

        jack_set_process_callback(jack, jack_process, this);
        max_block_length = jack_get_buffer_size(jack);
        return true;
    }

/****************************************************************
                PORTS - init plugin ports

****************************************************************/

    bool init_ports() {
        uint32_t n = lilv_plugin_get_num_ports(plugin);
        ports.reserve(n);
        LilvNode* midi_event = lilv_new_uri(world, LV2_MIDI__MidiEvent);

        for (uint32_t i = 0; i < n; ++i) {
            const LilvPort* lp = lilv_plugin_get_port_by_index(plugin, i);
            Port p;
            p.index = i;

            p.is_audio   = lilv_port_is_a(plugin, lp, audio_class);
            p.is_control = lilv_port_is_a(plugin, lp, control_class);
            p.is_atom    = lilv_port_is_a(plugin, lp, atom_class);
            p.is_input   = lilv_port_is_a(plugin, lp, input_class);
            p.is_midi    = lilv_port_supports_event(plugin, lp, midi_event);

            const LilvNode* sym = lilv_port_get_symbol(plugin, lp);
            if (sym) {
                p.uri = std::string(lilv_node_as_uri(lilv_plugin_get_uri(plugin)))
                      + "#" + lilv_node_as_string(sym);
                p.symbol = lilv_node_as_string(sym);
                }

            if (p.is_audio) {
                p.jack_port = jack_port_register(
                    jack,
                    sym ? lilv_node_as_string(sym) : "audio",
                    JACK_DEFAULT_AUDIO_TYPE,
                    p.is_input ? JackPortIsInput : JackPortIsOutput,
                    0
                );
            }

            if (p.is_atom && p.is_input && p.is_midi) {
                p.jack_port = jack_port_register(
                    jack,
                    sym ? lilv_node_as_string(sym) : "midi",
                    JACK_DEFAULT_MIDI_TYPE,
                    JackPortIsInput,
                    0
                );
            }

            if (p.is_atom && !p.is_input && p.is_midi) {
                p.jack_port = jack_port_register(
                    jack,
                    sym ? lilv_node_as_string(sym) : "midi",
                    JACK_DEFAULT_MIDI_TYPE,
                    JackPortIsOutput,
                    0
                );
            }

            if (p.is_atom) {
                p.atom_buf_size = required_atom_size;

                p.atom = (LV2_Atom_Sequence*)aligned_alloc(64, p.atom_buf_size);
                memset(p.atom, 0, p.atom_buf_size);
                p.atom->atom.type = urids.atom_Sequence;

                if (p.is_input) {
                    p.atom->atom.size = sizeof(LV2_Atom_Sequence_Body);
                    p.atom->body.unit = 0;
                    p.atom->body.pad  = 0;
                } else {
                    p.atom->atom.size = 0;
                }

                p.atom_state = new AtomState;
            }

            if (p.is_control && p.is_input) {
                LilvNode *pdflt, *pmin, *pmax;
                lilv_port_get_range(plugin, lp, &pdflt, &pmin, &pmax);
                if (pmin) lilv_node_free(pmin);
                if (pmax) lilv_node_free(pmax);
                if (pdflt) {
                    p.defvalue = lilv_node_as_float(pdflt);
                    lilv_node_free(pdflt);
                }
            }

            ports.push_back(p);
        }
        lilv_node_free(midi_event);
        return true;
    }

/****************************************************************
                DSP - init plugin instance

****************************************************************/

    bool init_instance() {

        LV2_Options_Option options[] = {
            {
                LV2_OPTIONS_INSTANCE,
                0,
                urids.buf_maxBlock,
                sizeof(uint32_t),
                urids.atom_Int,
                &max_block_length
            },
            { LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr }
        };

        LV2_Feature opt_f { LV2_OPTIONS__options, options };

        LV2_Feature* feats[] = { &features.um_f, &features.unm_f, &opt_f,
                            &features.bbl_feature, &host_worker.feature, nullptr };

        if (!checkFeatures(plugin, feats)) return false;

        // instantiate the plugin dsp instance 
        instance = lilv_plugin_instantiate(plugin,
                    jack_get_sample_rate(jack), feats);

        if (!instance) return false;

        // check if plugin require a worker thread 
        const LV2_Worker_Interface* iface = (const LV2_Worker_Interface*)
            lilv_instance_get_extension_data( instance, LV2_WORKER__interface);

        // start worker when plugin wants it
        if (iface) {
            host_worker.iface = iface;
            host_worker.dsp_handle = lilv_instance_get_handle(instance);
            host_worker.requests  = lv2_ringbuffer_create(8192);
            host_worker.responses = lv2_ringbuffer_create(8192);
            host_worker.running.store(true);
            host_worker.worker_thread =
                std::thread(worker_thread_func, &host_worker);
        }

        // connect control and atom ports
        for (auto& p : ports) {
            if (p.is_audio) continue;
            if (p.is_control)
                lilv_instance_connect_port(instance, p.index, &p.control);
            if (p.is_atom)
                lilv_instance_connect_port(instance, p.index, p.atom);
        }
        lilv_instance_activate(instance);
        return true;
    }

/****************************************************************
            PROCESS - run the audio/midi process
                      deliver and read atom ports

****************************************************************/

    int process(jack_nframes_t nframes) {
        if (shutdown.load()) return 0;
        for (Port& p : ports) {
            // connect all audio ports
            if (p.is_audio) {
                void* buf = jack_port_get_buffer(p.jack_port, nframes);
                lilv_instance_connect_port(instance, p.index, buf);
            }
            // prepare atom output buffers for plugin write  
            if (p.is_atom && !p.is_input) {
                p.atom->atom.type = 0;
                p.atom->atom.size = p.atom_buf_size - sizeof(LV2_Atom);
            }
            // handle atom input ports
            if (p.is_atom && p.is_input) {
                // handle midi input
                if (p.is_midi) {
                    void* midi_buf = jack_port_get_buffer(p.jack_port, nframes);
                    uint32_t event_count = jack_midi_get_event_count(midi_buf);
                    lv2_atom_sequence_clear(p.atom);
                    p.atom->atom.type = urids.atom_Sequence;
                    p.atom->atom.size = sizeof(LV2_Atom_Sequence_Body);
                    for (uint32_t i = 0; i < event_count; ++i) {
                        jack_midi_event_t ev;
                        jack_midi_event_get(&ev, midi_buf, i);
                        uint8_t evbuf[sizeof(LV2_Atom_Event) + required_atom_size];
                        LV2_Atom_Event* aev = (LV2_Atom_Event*)evbuf;
                        aev->time.frames = ev.time;
                        aev->body.type  = urids.midi_Event;
                        aev->body.size  = ev.size;
                        memcpy(LV2_ATOM_BODY(&aev->body), ev.buffer, ev.size);
                        lv2_atom_sequence_append_event(p.atom, p.atom_buf_size, aev);
                    }
                }
                // handle atom messages from GUI to dsp
                if (p.atom_state->ui_to_dsp_pending.exchange(false)) {
                    p.atom->atom.type = urids.atom_Sequence;
                    p.atom->atom.size = 0;
                    const uint32_t body_size = p.atom_state->ui_to_dsp.size();
                    uint8_t evbuf[sizeof(LV2_Atom_Event) + required_atom_size];
                    LV2_Atom_Event* ev = (LV2_Atom_Event*)evbuf;
                    ev->time.frames = 0;
                    ev->body.type  = p.atom_state->ui_to_dsp_type;
                    ev->body.size  = body_size;
                    memcpy((uint8_t*)LV2_ATOM_BODY(&ev->body),
                        p.atom_state->ui_to_dsp.data(), body_size);
                    lv2_atom_sequence_append_event( p.atom, p.atom_buf_size, ev);
                }
            }
        }
        // run the plugin
        lilv_instance_run(instance, nframes);
        // deliver worker response (work done)
        if (host_worker.iface ) deliver_worker_responses(&host_worker);
        // handle atom output ports (dsp to GUI)
        for (Port& p : ports) {
            // send control output port values to the UI
            if (p.is_control && !p.is_input) ui_dirty.store(true);
            // reset atom input port buffer after dsp have read it
            if (p.is_atom && p.is_input) p.atom->atom.size = 0;
            // init a midi output buffer when needed
            void* midi_buf = nullptr;
            if (p.is_atom && !p.is_input && p.is_midi) {
                midi_buf = jack_port_get_buffer(p.jack_port, nframes);
                jack_midi_clear_buffer(midi_buf);
            }
            // handle atom messages from dsp to UI (using a ringbuffer)
            if (p.is_atom && !p.is_input) {
                LV2_Atom_Sequence* seq = p.atom;
                LV2_ATOM_SEQUENCE_FOREACH(seq, ev) {
                    if (ev->body.size == 0)break;
                    if (p.atom->atom.type == 0) break;
                    const uint32_t total = sizeof(LV2_Atom) + ev->body.size;
                    if (lv2_ringbuffer_write_space(p.atom_state->dsp_to_ui) >= total) {
                        lv2_ringbuffer_write(p.atom_state->dsp_to_ui, (const char*)&ev->body, total);
                    }
                    // forward midi output to jack
                    if (ev->body.type == urids.midi_Event) {
                        const uint8_t* midi = (const uint8_t*)LV2_ATOM_BODY(&ev->body);
                        const uint32_t size = ev->body.size;
                        const uint32_t frame = ev->time.frames;
                        jack_midi_event_write(midi_buf, frame, midi, size);
                    }
                }
                p.atom->atom.type = 0;
                p.atom->atom.size = required_atom_size;
            }
        }
        return 0;
    }

/****************************************************************
                UI - Helper functions 

****************************************************************/

    static void set_xdnd_proxy(Display* dpy, Window plugin_window) {
        if (!dpy || !plugin_window) return;
        Atom xdnd_proxy = XInternAtom(dpy, "XdndProxy", False);
        if (xdnd_proxy == None) return;
        Window root, parent;
        Window* children = nullptr;
        unsigned int nchildren = 0;
        Window w = plugin_window;

        while (w != None) {
            XChangeProperty(dpy, w, xdnd_proxy, XA_WINDOW, 32, PropModeReplace,
                            (unsigned char*)&plugin_window, 1);

            if (!XQueryTree(dpy, w, &root, &parent, &children, &nchildren)) break;
            if (children) XFree(children);
            if (parent == root || parent == None) break;
            w = parent;
        }
        XFlush(dpy);
    }

    static void ui_write(LV2UI_Controller c, uint32_t port,
                uint32_t size, uint32_t type, const void* buf) {

        auto* self = static_cast<LV2X11JackHost*>(c);
        auto& p = self->ports[port];

        if (p.is_control && size == sizeof(float)) {
            p.control = *(const float*)buf;
            return;
        }

        if (p.is_atom) {
            p.atom_state->ui_to_dsp.resize(size);
            memcpy(p.atom_state->ui_to_dsp.data(), buf, size);
            p.atom_state->ui_to_dsp_type = type;
            p.atom_state->ui_to_dsp_pending.store(true, std::memory_order_release);
        }
    }

    static uint32_t ui_port_map(LV2UI_Feature_Handle h, const char* uri) {
        auto* self = static_cast<LV2X11JackHost*>(h);
        for (auto& p : self->ports)
            if (p.uri == uri) return p.index;
        return LV2UI_INVALID_PORT_INDEX;
    }

    static int ui_resize(LV2UI_Feature_Handle h, int w, int hgt) {
        auto* self = static_cast<LV2X11JackHost*>(h);
        if (!self->x_window || !self->x_display) return 1;
        XLockDisplay(self->x_display);
        XResizeWindow(self->x_display, self->x_window, w, hgt);
        XFlush(self->x_display);
        XUnlockDisplay(self->x_display);
        return 0;
    }

    void send_initial_ui_values() {
        for (auto& p : ports)
            if (p.is_control && p.is_input) {
                p.control = p.defvalue;
                ui_desc->port_event(
                    ui_handle, p.index, sizeof(float), 0, &p.defvalue);
            }
    }

    void send_control_values() {
        for (auto& p : ports)
            if (p.is_control && p.is_input) {
                ui_desc->port_event(
                    ui_handle, p.index, sizeof(float), 0, &p.control);
            }
    }

    void send_control_outputs() {
        for (auto& p : ports)
            if (p.is_control && !p.is_input) {
                ui_desc->port_event(
                    ui_handle, p.index, sizeof(float), 0, &p.control);
            }
    }

    void destroy_ui() {
        if (ui_desc && ui_handle) {
            ui_desc->cleanup(ui_handle);
            ui_handle = nullptr;
        }
    }

/****************************************************************
            UI -init the UI (Host and Plugin)

****************************************************************/

    bool init_ui() {
        const LilvUIs* uis = lilv_plugin_get_uis(plugin);
        const LilvUI* ui = nullptr;

        char* gui_uri = nullptr;
        // only X11 UI's been supported
        LILV_FOREACH(uis, i, uis)
            if (lilv_ui_is_a(lilv_uis_get(uis, i), x11_class)) {
                ui = lilv_uis_get(uis, i);
                gui_uri = strdup (lilv_node_as_uri (lilv_ui_get_uri(ui)));
            }

        if (!ui) return false;

        char* so = lilv_node_get_path(lilv_ui_get_binary_uri(ui), nullptr);
        char* bundle = lilv_node_get_path(lilv_ui_get_bundle_uri(ui), nullptr);

        ui_dl = dlopen(so, RTLD_NOW);
        free(so);

        auto fn = (const LV2UI_Descriptor* (*)(uint32_t))
            dlsym(ui_dl, "lv2ui_descriptor");

        const LV2UI_Descriptor* plugin_gui = nullptr;
        uint32_t index = 0;
        while (fn) {
            plugin_gui = fn(index);
            if (!plugin_gui) { break; }
            if (!strcmp (plugin_gui->URI, gui_uri)) { break; }
            ++index;
        }
        free(gui_uri);
        if (!plugin_gui) return false;

        ui_desc = fn(index);

        x_display = XOpenDisplay(nullptr);
        x_window = XCreateSimpleWindow(
            x_display, DefaultRootWindow(x_display),
            100, 100, 640, 480, 0, 0, 0);

        XMapWindow(x_display, x_window);
        Atom dnd_version = 5;
        Atom XdndAware = XInternAtom (x_display, "XdndAware", False);
        XChangeProperty (x_display, x_window, XdndAware, XA_ATOM,
                        32, PropModeReplace, (unsigned char*)&dnd_version, 1);
        XFlush(x_display);

        resize.handle    = this;
        resize.ui_resize = ui_resize;

        LV2UI_Port_Map pm { this, ui_port_map };

        LV2_Feature pm_f { LV2_UI__portMap, &pm };
        LV2_Feature parent { LV2_UI__parent, (void*)x_window };
        LV2_Feature resize_f { LV2_UI__resize, &resize };

        LV2_Feature* feats[] = { &parent, &resize_f, &pm_f,
                                &features.um_f, &features.unm_f, nullptr };

        ui_handle = ui_desc->instantiate( ui_desc, plugin_uri, bundle,
                                          ui_write, this, &ui_widget, feats);

        free(bundle);
        //fprintf(stderr, "name %s\n", plugin_name.data());
        std::string name = plugin_name;
        if (!preset_label.empty()) name += " - " + preset_label;
        XStoreName(x_display, x_window, name.data()); 
        set_xdnd_proxy(x_display, (Window)ui_widget);
        if (preset_uri.empty()) ui_needs_initial_update.store(true);
        return ui_handle;
    }

/****************************************************************
        HOST DATA - private host data members

****************************************************************/

    const char* plugin_uri;
    std::string preset_uri;
    std::string preset_label;
    std::string plugin_name;

    LilvWorld* world = nullptr;
    const LilvPlugin* plugin = nullptr;
    LilvInstance* instance = nullptr;

    LilvNode *audio_class, *control_class, *atom_class,
             *input_class, *x11_class,*rsz_minimumSize;

    jack_client_t* jack = nullptr;
    std::vector<Port> ports;

    LV2UI_Resize resize;
    void* ui_dl = nullptr;
    const LV2UI_Descriptor* ui_desc = nullptr;
    LV2UI_Handle ui_handle = nullptr;
    LV2UI_Widget ui_widget = nullptr;

    Display* x_display = nullptr;
    Window x_window = 0;

    uint32_t max_block_length = 4096;
    uint32_t required_atom_size = 8192;

    std::atomic<bool> ui_dirty{false};
    std::atomic<bool> ui_needs_initial_update{false};
    std::atomic<bool> ui_needs_control_update{false};
    std::atomic<bool> run{false};
    std::atomic<bool> shutdown{false};
};

