/*
 * JACK Backend code for Carla
 * Copyright (C) 2011-2012 Filipe Coelho <falktx@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * For a full copy of the GNU General Public License see the COPYING file
 */

#ifndef CARLA_PLUGIN_H
#define CARLA_PLUGIN_H

#include "carla_jack.h"
#include "carla_midi.h"
#include "carla_shared.h"

#ifdef BUILD_BRIDGE
#include <QtCore/QThread>
#include "carla_bridge_osc.h"
#else
#include "carla_osc.h"
#include "carla_threads.h"
#endif

// common includes
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <QtCore/QList>
#include <QtCore/QMutex>
#include <QtCore/QString>

#define CARLA_PROCESS_CONTINUE_CHECK if (m_id != plugin_id) { return callback_action(CALLBACK_DEBUG, plugin_id, m_id, 0, 0.0); }

const unsigned short MAX_MIDI_EVENTS = 512;
const unsigned short MAX_POST_EVENTS = 152;

typedef jack_default_audio_sample_t jack_audio_sample_t;

enum PluginPostEventType {
    PostEventDebug,
    PostEventParameterChange,
    PostEventProgramChange,
    PostEventMidiProgramChange,
    PostEventNoteOn,
    PostEventNoteOff,
    PostEventCustom
};

enum PluginBridgeInfoType {
    PluginBridgeAudioCountInfo,
    PluginBridgeMidiCountInfo,
    PluginBridgeParameterCountInfo,
    PluginBridgeProgramCountInfo,
    PluginBridgeMidiProgramCountInfo,
    PluginBridgePluginInfo,
    PluginBridgeParameterInfo,
    PluginBridgeParameterDataInfo,
    PluginBridgeParameterRangesInfo,
    PluginBridgeProgramInfo,
    PluginBridgeMidiProgramInfo,
    PluginBridgeUpdateNow
};

struct midi_program_t {
    uint32_t bank;
    uint32_t program;
    const char* name;
};

struct PluginAudioData {
    uint32_t count;
    uint32_t* rindexes;
    jack_port_t** ports;
};

struct PluginMidiData {
    jack_port_t* port_min;
    jack_port_t* port_mout;
};

struct PluginParameterData {
    uint32_t count;
    ParameterData* data;
    ParameterRanges* ranges;
    jack_port_t* port_cin;
    jack_port_t* port_cout;
};

struct PluginProgramData {
    uint32_t count;
    int32_t current;
    const char** names;
};

struct PluginMidiProgramData {
    uint32_t count;
    int32_t current;
    midi_program_t* data;
};

struct PluginPostEvent {
    bool valid;
    PluginPostEventType type;
    int32_t index;
    double value;
    const void* cdata;
};

struct ExternalMidiNote {
    bool valid;
    bool onoff;
    uint8_t note;
    uint8_t velo;
};

class CarlaPlugin
{
public:
    CarlaPlugin()
    {
        qDebug("CarlaPlugin::CarlaPlugin()");

        m_type = PLUGIN_NONE;
        m_id   = -1;
        m_hints = 0;

        m_active = false;
        m_active_before = false;

        m_lib  = nullptr;
        m_name = nullptr;
        m_filename = nullptr;

        cin_channel = 0;

        x_drywet = 1.0;
        x_vol    = 1.0;
        x_bal_left = -1.0;
        x_bal_right = 1.0;

        jack_client = nullptr;

        ain.count = 0;
        ain.ports = nullptr;
        ain.rindexes = nullptr;

        aout.count = 0;
        aout.ports = nullptr;
        aout.rindexes = nullptr;

        midi.port_min  = nullptr;
        midi.port_mout = nullptr;

        param.count  = 0;
        param.data   = nullptr;
        param.ranges = nullptr;
        param.port_cin  = nullptr;
        param.port_cout = nullptr;

        prog.count   = 0;
        prog.current = -1;
        prog.names   = nullptr;

        midiprog.count   = 0;
        midiprog.current = -1;
        midiprog.data    = nullptr;

        custom.clear();

#ifndef BUILD_BRIDGE
        osc.data.path = nullptr;
        osc.data.source = nullptr;
        osc.data.target = nullptr;
        osc.thread = nullptr;
#endif

        for (unsigned short i=0; i < MAX_POST_EVENTS; i++)
            post_events.data[i].valid = false;

        for (unsigned short i=0; i < MAX_MIDI_EVENTS; i++)
            ext_midi_notes[i].valid = false;
    }

    virtual ~CarlaPlugin()
    {
        qDebug("CarlaPlugin::~CarlaPlugin()");

        // Unregister jack client and ports
        remove_from_jack();

        // Delete data
        delete_buffers();

        // Unload DLL
        lib_close();

        if (m_name)
            free((void*)m_name);

        if (m_filename)
            free((void*)m_filename);

        if (prog.count > 0)
        {
            for (uint32_t i=0; i < prog.count; i++)
            {
                if (prog.names[i])
                    free((void*)prog.names[i]);
            }

            delete[] prog.names;
        }

        if (midiprog.count > 0)
        {
            for (uint32_t i=0; i < midiprog.count; i++)
            {
                if (midiprog.data[i].name)
                    free((void*)midiprog.data[i].name);
            }

            delete[] midiprog.data;
        }

        if (custom.count() > 0)
        {
            for (int i=0; i < custom.count(); i++)
            {
                if (custom[i].key)
                    free((void*)custom[i].key);

                if (custom[i].value)
                    free((void*)custom[i].value);
            }

            custom.clear();
        }

#ifdef BUILD_BRIDGE
        if (jack_client)
#else
        if (jack_client && carla_options.global_jack_client == false)
#endif
            jack_client_close(jack_client);
    }

    PluginType type()
    {
        return m_type;
    }

    short id()
    {
        return m_id;
    }

    unsigned int hints()
    {
        return m_hints;
    }

    const char* name()
    {
        return m_name;
    }

    const char* filename()
    {
        return m_filename;
    }

    virtual PluginCategory category()
    {
        return PLUGIN_CATEGORY_NONE;
    }

    virtual long unique_id()
    {
        return 0;
    }

    virtual uint32_t ain_count()
    {
        return ain.count;
    }

    virtual uint32_t aout_count()
    {
        return aout.count;
    }

    virtual uint32_t min_count()
    {
        return (midi.port_min) ? 1 : 0;
    }

    virtual uint32_t mout_count()
    {
        return (midi.port_mout) ? 1 : 0;
    }

    uint32_t param_count()
    {
        return param.count;
    }

    virtual uint32_t param_scalepoint_count(uint32_t /*param_id*/)
    {
        return 0;
    }

    uint32_t custom_count()
    {
        return custom.count();
    }

    uint32_t prog_count()
    {
        return prog.count;
    }

    uint32_t midiprog_count()
    {
        return midiprog.count;
    }

    int32_t prog_current()
    {
        return prog.current;
    }

    int32_t midiprog_current()
    {
        return midiprog.current;
    }

    ParameterData* param_data(uint32_t index)
    {
        return &param.data[index];
    }

    ParameterRanges* param_ranges(uint32_t index)
    {
        return &param.ranges[index];
    }

    CustomData* custom_data(uint32_t index)
    {
        return &custom[index];
    }

    virtual int32_t chunk_data(void** /*data_ptr*/)
    {
        return 0;
    }

#ifndef BUILD_BRIDGE
    OscData* osc_data()
    {
        return &osc.data;
    }
#endif

    virtual double get_parameter_value(uint32_t /*param_id*/)
    {
        return 0.0;
    }

    virtual double get_parameter_scalepoint_value(uint32_t /*param_id*/, uint32_t /*scalepoint_id*/)
    {
        return 0.0;
    }

    virtual void get_label(char* buf_str)
    {
        *buf_str = 0;
    }

    virtual void get_maker(char* buf_str)
    {
        *buf_str = 0;
    }

    virtual void get_copyright(char* buf_str)
    {
        *buf_str = 0;
    }

    virtual void get_real_name(char* buf_str)
    {
        *buf_str = 0;
    }

    virtual void get_parameter_name(uint32_t /*param_id*/, char* buf_str)
    {
        *buf_str = 0;
    }

    virtual void get_parameter_symbol(uint32_t /*param_id*/, char* buf_str)
    {
        *buf_str = 0;
    }

    virtual void get_parameter_unit(uint32_t /*param_id*/, char* buf_str)
    {
        *buf_str = 0;
    }

    virtual void get_parameter_text(uint32_t /*param_id*/, char* buf_str)
    {
        *buf_str = 0;
    }

    virtual void get_parameter_scalepoint_label(uint32_t /*param_id*/, uint32_t /*scalepoint_id*/, char* buf_str)
    {
        *buf_str = 0;
    }

    void get_program_name(uint32_t program_id, char* buf_str)
    {
        strncpy(buf_str, prog.names[program_id], STR_MAX);
    }

    void get_midi_program_name(uint32_t midiprogram_id, char* buf_str)
    {
        strncpy(buf_str, midiprog.data[midiprogram_id].name, STR_MAX);
    }

    void get_parameter_count_info(PortCountInfo* info)
    {
        info->ins   = 0;
        info->outs  = 0;
        info->total = param.count;

        for (uint32_t i=0; i < param.count; i++)
        {
            if (param.data[i].type == PARAMETER_INPUT)
                info->ins += 1;
            else if (param.data[i].type == PARAMETER_OUTPUT)
                info->outs += 1;
        }
    }

    void get_midi_program_info(MidiProgramInfo* info, uint32_t index)
    {
        info->bank    = midiprog.data[index].bank;
        info->program = midiprog.data[index].program;
        info->label   = midiprog.data[index].name;
    }

    virtual void get_gui_info(GuiInfo* info)
    {
        info->type = GUI_NONE;
        info->resizable = false;
    }

    void set_id(short id)
    {
        m_id = id;
    }

    void set_active(bool active, bool osc_send, bool callback_send)
    {
        m_active = active;

#ifndef BUILD_BRIDGE
        double value = active ? 1.0 : 0.0;

        if (osc_send)
        {
            osc_global_send_set_parameter_value(m_id, PARAMETER_ACTIVE, value);

            if (m_hints & PLUGIN_IS_BRIDGE)
                osc_send_control(&osc.data, PARAMETER_ACTIVE, value);
        }

        if (callback_send)
            callback_action(CALLBACK_PARAMETER_CHANGED, m_id, PARAMETER_ACTIVE, 0, value);
#else
        Q_UNUSED(osc_send);
        Q_UNUSED(callback_send);
#endif
    }

    void set_drywet(double value, bool osc_send, bool callback_send)
    {
        if (value < 0.0)
            value = 0.0;
        else if (value > 1.0)
            value = 1.0;

        x_drywet = value;

#ifndef BUILD_BRIDGE
        if (osc_send)
        {
            osc_global_send_set_parameter_value(m_id, PARAMETER_DRYWET, value);

            if (m_hints & PLUGIN_IS_BRIDGE)
                osc_send_control(&osc.data, PARAMETER_DRYWET, value);
        }

        if (callback_send)
            callback_action(CALLBACK_PARAMETER_CHANGED, m_id, PARAMETER_DRYWET, 0, value);
#else
        Q_UNUSED(osc_send);
        Q_UNUSED(callback_send);
#endif
    }

    void set_volume(double value, bool osc_send, bool callback_send)
    {
        if (value < 0.0)
            value = 0.0;
        else if (value > 1.27)
            value = 1.27;

        x_vol = value;

#ifndef BUILD_BRIDGE
        if (osc_send)
        {
            osc_global_send_set_parameter_value(m_id, PARAMETER_VOLUME, value);

            if (m_hints & PLUGIN_IS_BRIDGE)
                osc_send_control(&osc.data, PARAMETER_VOLUME, value);
        }

        if (callback_send)
            callback_action(CALLBACK_PARAMETER_CHANGED, m_id, PARAMETER_VOLUME, 0, value);
#else
        Q_UNUSED(osc_send);
        Q_UNUSED(callback_send);
#endif
    }

    void set_balance_left(double value, bool osc_send, bool callback_send)
    {
        if (value < -1.0)
            value = -1.0;
        else if (value > 1.0)
            value = 1.0;

        x_bal_left = value;

#ifndef BUILD_BRIDGE
        if (osc_send)
        {
            osc_global_send_set_parameter_value(m_id, PARAMETER_BALANCE_LEFT, value);

            if (m_hints & PLUGIN_IS_BRIDGE)
                osc_send_control(&osc.data, PARAMETER_BALANCE_LEFT, value);
        }

        if (callback_send)
            callback_action(CALLBACK_PARAMETER_CHANGED, m_id, PARAMETER_BALANCE_LEFT, 0, value);
#else
        Q_UNUSED(osc_send);
        Q_UNUSED(callback_send);
#endif
    }

    void set_balance_right(double value, bool osc_send, bool callback_send)
    {
        if (value < -1.0)
            value = -1.0;
        else if (value > 1.0)
            value = 1.0;

        x_bal_right = value;

#ifndef BUILD_BRIDGE
        if (osc_send)
        {
            osc_global_send_set_parameter_value(m_id, PARAMETER_BALANCE_RIGHT, value);

            if (m_hints & PLUGIN_IS_BRIDGE)
                osc_send_control(&osc.data, PARAMETER_BALANCE_RIGHT, value);
        }

        if (callback_send)
            callback_action(CALLBACK_PARAMETER_CHANGED, m_id, PARAMETER_BALANCE_RIGHT, 0, value);
#else
        Q_UNUSED(osc_send);
        Q_UNUSED(callback_send);
#endif
    }

    virtual void set_parameter_value(uint32_t param_id, double value, bool /*gui_send*/, bool osc_send, bool callback_send)
    {
#ifndef BUILD_BRIDGE
        if (osc_send && param.data[param_id].type == PARAMETER_INPUT)
        {
            osc_global_send_set_parameter_value(m_id, param_id, value);

            if (m_hints & PLUGIN_IS_BRIDGE)
                osc_send_control(&osc.data, param_id, value);
        }

        if (callback_send)
            callback_action(CALLBACK_PARAMETER_CHANGED, m_id, param_id, 0, value);
#else
        Q_UNUSED(param_id);
        Q_UNUSED(value);
        Q_UNUSED(osc_send);
        Q_UNUSED(callback_send);
#endif
    }

    void set_parameter_value_rindex(int32_t rindex, double value, bool gui_send, bool osc_send, bool callback_send)
    {
        if (m_hints & PLUGIN_IS_BRIDGE)
        {
            if (rindex == PARAMETER_ACTIVE)
                return set_active(value > 0.0, osc_send, callback_send);
            else if (rindex == PARAMETER_DRYWET)
                return set_drywet(value, osc_send, callback_send);
            else if (rindex == PARAMETER_VOLUME)
                return set_volume(value, osc_send, callback_send);
            else if (rindex == PARAMETER_BALANCE_LEFT)
                return set_balance_left(value, osc_send, callback_send);
            else if (rindex == PARAMETER_BALANCE_LEFT)
                return set_balance_right(value, osc_send, callback_send);
        }

        for (uint32_t i=0; i < param.count; i++)
        {
            if (param.data[i].rindex == rindex)
            {
                set_parameter_value(i, value, gui_send, osc_send, callback_send);
                break;
            }
        }
    }

    void set_parameter_midi_channel(uint32_t index, uint8_t channel)
    {
        param.data[index].midi_channel = channel;

#ifndef BUILD_BRIDGE
        // FIXME
        //if (m_hints & PLUGIN_IS_BRIDGE)
        //    osc_send_set_parameter_midi_channel(&osc.data, m_id, index, channel);
#endif
    }

    void set_parameter_midi_cc(uint32_t index, int16_t midi_cc)
    {
        param.data[index].midi_cc = midi_cc;

#ifndef BUILD_BRIDGE
        // FIXME
        //if (m_hints & PLUGIN_IS_BRIDGE)
        //    osc_send_set_parameter_midi_cc(&osc.data, m_id, index, midi_cc);
#endif
    }

    virtual void set_custom_data(CustomDataType dtype, const char* key, const char* value, bool /*gui_send*/)
    {
        qDebug("set_custom_data(%i, %s, %s)", dtype, key, value);

        bool save_data = true;

        switch (dtype)
        {
        case CUSTOM_DATA_INVALID:
            save_data = false;
            break;
        case CUSTOM_DATA_STRING:
            // Ignore some keys
            if (strncmp(key, "OSC:", 4) == 0 || strcmp(key, "guiVisible") == 0)
                save_data = false;
            break;
        default:
            break;
        }

        if (save_data)
        {
            // Check if we already have this key
            bool already_have = false;

            for (int i=0; i < custom.count(); i++)
            {
                if (strcmp(custom[i].key, key) == 0)
                {
                    free((void*)custom[i].value);
                    custom[i].value = strdup(value);
                    already_have = true;
                    break;
                }
            }

            if (already_have == false)
            {
                CustomData new_data;
                new_data.type  = dtype;
                new_data.key   = strdup(key);
                new_data.value = strdup(value);
                custom.append(new_data);
            }
        }
    }

    virtual void set_chunk_data(const char* /*string_data*/)
    {
    }

    virtual void set_program(int32_t index, bool /*gui_send*/, bool osc_send, bool callback_send, bool /*block*/)
    {
        prog.current = index;

#ifndef BUILD_BRIDGE
        if (osc_send)
        {
            osc_global_send_set_program(m_id, prog.current);

            if (m_hints & PLUGIN_IS_BRIDGE)
                osc_send_program(&osc.data, prog.current);
        }

        if (callback_send)
            callback_action(CALLBACK_PROGRAM_CHANGED, m_id, prog.current, 0, 0.0);
#else
        Q_UNUSED(osc_send);
        Q_UNUSED(callback_send);
#endif

        // Change default value
        for (uint32_t i=0; i < param.count; i++)
        {
            param.ranges[i].def = get_parameter_value(i);

#ifndef BUILD_BRIDGE
            if (osc_send)
                osc_global_send_set_default_value(m_id, i, param.ranges[i].def);
#endif
        }
    }

    virtual void set_midi_program(int32_t index, bool /*gui_send*/, bool osc_send, bool callback_send, bool /*block*/)
    {
        midiprog.current = index;

#ifndef BUILD_BRIDGE
        if (osc_send)
        {
            osc_global_send_set_midi_program(m_id, midiprog.current);

            if (m_hints & PLUGIN_IS_BRIDGE)
                osc_send_program(&osc.data, midiprog.current);
        }

        if (callback_send)
            callback_action(CALLBACK_MIDI_PROGRAM_CHANGED, m_id, midiprog.current, 0, 0.0);
#else
        Q_UNUSED(osc_send);
        Q_UNUSED(callback_send);
#endif

        // SF2 never change defaults
        if (m_type != PLUGIN_SF2)
            return;

        // Change default value
        for (uint32_t i=0; i < param.count; i++)
        {
            param.ranges[i].def = get_parameter_value(i);

#ifndef BUILD_BRIDGE
            if (osc_send)
                osc_global_send_set_default_value(m_id, i, param.ranges[i].def);
#endif
        }
    }

    void set_midi_program_full(uint32_t bank_id, uint32_t program_id, bool gui_send, bool osc_send, bool callback_send, bool block)
    {
        for (uint32_t i=0; i < midiprog.count; i++)
        {
            if (midiprog.data[i].bank == bank_id && midiprog.data[i].program == program_id)
                return set_midi_program(i, gui_send, osc_send, callback_send, block);
        }
    }

    virtual void set_gui_data(int /*data*/, void* /*ptr*/)
    {
    }

    virtual void show_gui(bool /*yesno*/)
    {
    }

    virtual void idle_gui()
    {
    }

    virtual void reload()
    {
    }

    virtual void reload_programs(bool /*init*/)
    {
    }

    virtual void prepare_for_save()
    {
    }

    virtual void process(jack_nframes_t)
    {
    }

    virtual void buffer_size_changed(jack_nframes_t)
    {
    }

    virtual void send_midi_note(bool onoff, uint8_t note, uint8_t velo, bool /*gui_send*/, bool osc_send, bool callback_send)
    {
        carla_midi_lock();
        for (unsigned int i=0; i<MAX_MIDI_EVENTS; i++)
        {
            if (ext_midi_notes[i].valid == false)
            {
                ext_midi_notes[i].valid = true;
                ext_midi_notes[i].onoff = onoff;
                ext_midi_notes[i].note = note;
                ext_midi_notes[i].velo = velo;
                break;
            }
        }
        carla_midi_unlock();

#ifndef BUILD_BRIDGE
        if (osc_send)
        {
            if (onoff)
                osc_global_send_note_on(m_id, note, velo);
            else
                osc_global_send_note_off(m_id, note);

            // FIXME, send midi
            //if (m_hints & PLUGIN_IS_BRIDGE)
            //{
            //    if (onoff)
            //        osc_send_note_on(&osc.data, m_id, note, velo);
            //    else
            //        osc_send_note_off(&osc.data, m_id, note);
            //}
        }

        if (callback_send)
            callback_action(onoff ? CALLBACK_NOTE_ON : CALLBACK_NOTE_OFF, m_id, note, velo, 0.0);
#else
        Q_UNUSED(osc_send);
        Q_UNUSED(callback_send);
#endif
    }

    void send_midi_all_notes_off()
    {
        carla_midi_lock();
        post_events.lock.lock();

        unsigned short pe_pad = 0;

        for (unsigned short i=0; i < MAX_POST_EVENTS; i++)
        {
            if (post_events.data[i].valid == false)
            {
                pe_pad = i;
                break;
            }
            else if (i + MAX_MIDI_EVENTS == MAX_POST_EVENTS)
            {
                qWarning("post-events buffer full, making room for all notes off now");
                pe_pad = i - 1;
                break;
            }
        }

        for (unsigned short i=0; i < 128 && i < MAX_MIDI_EVENTS && i < MAX_POST_EVENTS; i++)
        {
            //if (notes_off)
            //{
            ext_midi_notes[i].valid = true;
            ext_midi_notes[i].onoff = false;
            ext_midi_notes[i].note  = i;
            ext_midi_notes[i].velo  = 0;
            //}

            post_events.data[i+pe_pad].valid = true;
            post_events.data[i+pe_pad].type  = PostEventNoteOff;
            post_events.data[i+pe_pad].index = i;
            post_events.data[i+pe_pad].value = 0.0;
        }

        post_events.lock.unlock();
        carla_midi_unlock();
    }

    void postpone_event(PluginPostEventType type, int32_t index, double value, const void* cdata = nullptr)
    {
        post_events.lock.lock();

        for (unsigned short i=0; i<MAX_POST_EVENTS; i++)
        {
            if (post_events.data[i].valid == false)
            {
                post_events.data[i].valid = true;
                post_events.data[i].type  = type;
                post_events.data[i].index = index;
                post_events.data[i].value = value;
                post_events.data[i].cdata = cdata;
                break;
            }
        }

        post_events.lock.unlock();
    }

    void post_events_copy(PluginPostEvent* post_events_dst)
    {
        post_events.lock.lock();

        memcpy(post_events_dst, post_events.data, sizeof(PluginPostEvent)*MAX_POST_EVENTS);

        for (unsigned short i=0; i < MAX_POST_EVENTS; i++)
            post_events.data[i].valid = false;

        post_events.lock.unlock();
    }

    virtual void run_custom_event(PluginPostEvent* /*event*/)
    {
    }

    virtual int set_osc_bridge_info(PluginBridgeInfoType /*intoType*/, lo_arg** /*argv*/)
    {
        return 1;
    }

    void osc_global_register_new()
    {
#ifdef BUILD_BRIDGE
        // Base data
        //const PluginInfo* info = get_plugin_info(m_id);
        //osc_send_bridge_plugin_info(m_type, category(), m_hints, get_real_plugin_name(m_id), info->label, info->maker, info->copyright, unique_id());

        osc_send_bridge_audio_count(ain_count(), aout_count(), ain_count() + aout_count());
        osc_send_bridge_midi_count(min_count(), mout_count(), min_count() + mout_count());

        PortCountInfo param_info = { false, 0, 0, 0 };
        get_parameter_count_info(&param_info);
        osc_send_bridge_param_count(param_info.ins, param_info.outs, param_info.total);

        // Parameters
        uint32_t i;
        char buf_name[STR_MAX], buf_unit[STR_MAX];

        if (param.count > 0 && param.count < MAX_PARAMETERS)
        {
            for (i=0; i < param.count; i++)
            {
                get_parameter_name(i, buf_name);
                get_parameter_unit(i, buf_unit);
                osc_send_bridge_param_info(i, buf_name, buf_unit);
                osc_send_bridge_param_data(param.data[i].type, i, param.data[i].rindex, param.data[i].hints, param.data[i].midi_channel, param.data[i].midi_cc);
                osc_send_bridge_param_ranges(i, param.ranges[i].def, param.ranges[i].min, param.ranges[i].max, param.ranges[i].step, param.ranges[i].step_small, param.ranges[i].step_large);

                set_parameter_value(i, param.ranges[i].def, false, false, false);
            }
        }
#else
        if (osc_global_registered())
        {
            // Base data
            osc_global_send_add_plugin(m_id, m_name);

            const PluginInfo* info = get_plugin_info(m_id);
            osc_global_send_set_plugin_data(m_id, m_type, category(), m_hints, get_real_plugin_name(m_id), info->label, info->maker, info->copyright, unique_id());

            PortCountInfo param_info = { false, 0, 0, 0 };
            get_parameter_count_info(&param_info);
            osc_global_send_set_plugin_ports(m_id, ain.count, aout.count, min_count(), mout_count(), param_info.ins, param_info.outs, param_info.total);

            // Parameters
            osc_global_send_set_parameter_value(m_id, PARAMETER_ACTIVE, m_active ? 1.0f : 0.0f);
            osc_global_send_set_parameter_value(m_id, PARAMETER_DRYWET, x_drywet);
            osc_global_send_set_parameter_value(m_id, PARAMETER_VOLUME, x_vol);
            osc_global_send_set_parameter_value(m_id, PARAMETER_BALANCE_LEFT, x_bal_left);
            osc_global_send_set_parameter_value(m_id, PARAMETER_BALANCE_RIGHT, x_bal_right);

            uint32_t i;

            if (param.count > 0 && param.count < MAX_PARAMETERS)
            {
                for (i=0; i < param.count; i++)
                {
                    const ParameterInfo* info = get_parameter_info(m_id, i);

                    osc_global_send_set_parameter_data(m_id, i, param.data[i].type, param.data[i].hints, info->name, info->unit, get_parameter_value(i));
                    osc_global_send_set_parameter_ranges(m_id, i, param.ranges[i].min, param.ranges[i].max, param.ranges[i].def, param.ranges[i].step, param.ranges[i].step_small, param.ranges[i].step_large);
                }
            }

            // Programs
            osc_global_send_set_program_count(m_id, prog.count);

            for (i=0; i < prog.count; i++)
                osc_global_send_set_program_name(m_id, i, prog.names[i]);

            osc_global_send_set_program(m_id, prog.current);

            // MIDI Programs
            osc_global_send_set_midi_program_count(m_id, midiprog.count);

            for (i=0; i < midiprog.count; i++)
                osc_global_send_set_midi_program_data(m_id, i, midiprog.data[i].bank, midiprog.data[i].program, midiprog.data[i].name);

            osc_global_send_set_midi_program(m_id, midiprog.current);
        }
#endif
    }

#ifndef BUILD_BRIDGE
    void update_osc_data(lo_address source, const char* url)
    {
        const char* host;
        const char* port;

        osc_clear_data(&osc.data);

        host = lo_address_get_hostname(source);
        port = lo_address_get_port(source);
        osc.data.source = lo_address_new(host, port);

        host = lo_url_get_hostname(url);
        port = lo_url_get_port(url);

        osc.data.path = lo_url_get_path(url);
        osc.data.target = lo_address_new(host, port);

        free((void*)host);
        free((void*)port);

        for (int i=0; i < custom.count(); i++)
        {
            if (m_type == PLUGIN_LV2)
                osc_send_lv2_event_transfer(&osc.data, customdatatype2str(custom[i].type), custom[i].key, custom[i].value);
            else if (custom[i].type == CUSTOM_DATA_STRING)
                osc_send_configure(&osc.data, custom[i].key, custom[i].value);
        }

        if (prog.current >= 0)
            osc_send_program(&osc.data, prog.current);

        if (midiprog.current >= 0)
        {
            int32_t midi_id = midiprog.current;
            osc_send_midi_program(&osc.data, midiprog.data[midi_id].bank, midiprog.data[midi_id].program, (m_type == PLUGIN_DSSI));
        }

        for (uint32_t i=0; i < param.count; i++)
            osc_send_control(&osc.data, param.data[i].rindex, get_parameter_value(i));

        if (m_hints & PLUGIN_IS_BRIDGE)
        {
            osc_send_control(&osc.data, PARAMETER_ACTIVE, m_active ? 1.0 : 0.0);
            osc_send_control(&osc.data, PARAMETER_DRYWET, x_drywet);
            osc_send_control(&osc.data, PARAMETER_VOLUME, x_vol);
            osc_send_control(&osc.data, PARAMETER_BALANCE_LEFT, x_bal_left);
            osc_send_control(&osc.data, PARAMETER_BALANCE_RIGHT, x_bal_right);
        }
    }

    bool update_osc_gui()
    {
        // wait for UI 'update' call; 40 re-tries, 4 secs
        for (short i=1; i<40; i++)
        {
            if (osc.data.target)
            {
                osc_send_show(&osc.data);
                return true;
            }
            else
                carla_msleep(100);
        }
        return false;
    }
#endif

    // TODO, remove = true
    virtual void remove_from_jack(bool deactivate = true)
    {
        qDebug("CarlaPlugin::remove_from_jack(%s) - start", bool2str(deactivate));

        if (jack_client == nullptr)
        {
            qDebug("CarlaPlugin::remove_from_jack(%s) - return", bool2str(deactivate));
            return;
        }

#ifdef BUILD_BRIDGE
        if (deactivate)
#else
        if (carla_options.global_jack_client == false && deactivate)
#endif
            jack_deactivate(jack_client);

        for (uint32_t i=0; i < ain.count; i++)
            jack_port_unregister(jack_client, ain.ports[i]);

        for (uint32_t i=0; i < aout.count; i++)
            jack_port_unregister(jack_client, aout.ports[i]);

        if (midi.port_min)
            jack_port_unregister(jack_client, midi.port_min);

        if (midi.port_mout)
            jack_port_unregister(jack_client, midi.port_mout);

        if (param.port_cin)
            jack_port_unregister(jack_client, param.port_cin);

        if (param.port_cout)
            jack_port_unregister(jack_client, param.port_cout);

        qDebug("CarlaPlugin::remove_from_jack(%s) - end", bool2str(deactivate));
    }

    virtual void delete_buffers()
    {
        qDebug("CarlaPlugin::delete_buffers() - start");

        if (ain.count > 0)
        {
            delete[] ain.ports;
            delete[] ain.rindexes;
        }

        if (aout.count > 0)
        {
            delete[] aout.ports;
            delete[] aout.rindexes;
        }

        if (param.count > 0)
        {
            delete[] param.data;
            delete[] param.ranges;
        }

        ain.count = 0;
        ain.ports = nullptr;
        ain.rindexes = nullptr;

        aout.count = 0;
        aout.ports = nullptr;
        aout.rindexes = nullptr;

        midi.port_min  = nullptr;
        midi.port_mout = nullptr;

        param.count    = 0;
        param.data     = nullptr;
        param.ranges   = nullptr;
        param.port_cin  = nullptr;
        param.port_cout = nullptr;

        qDebug("CarlaPlugin::delete_buffers() - end");
    }

    bool lib_open(const char* filename)
    {
#ifdef Q_OS_WIN
        m_lib = LoadLibraryA(filename);
#else
        m_lib = dlopen(filename, RTLD_NOW);
#endif
        return bool(m_lib);
    }

    bool lib_close()
    {
        if (m_lib)
#ifdef Q_OS_WIN
            return FreeLibrary((HMODULE)m_lib) != 0;
#else
            return dlclose(m_lib) != 0;
#endif
        else
            return false;
    }

    void* lib_symbol(const char* symbol)
    {
        if (m_lib)
#ifdef Q_OS_WIN
            return (void*)GetProcAddress((HMODULE)m_lib, symbol);
#else
            return dlsym(m_lib, symbol);
#endif
        else
            return nullptr;
    }

    const char* lib_error()
    {
#ifdef Q_OS_WIN
        static char libError[2048];
        memset(libError, 0, sizeof(char)*2048);

        LPVOID winErrorString;
        DWORD  winErrorCode = GetLastError();
        FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER |  FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, winErrorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPTSTR)&winErrorString, 0, nullptr);

        snprintf(libError, 2048, "%s: error code %i: %s", m_filename, winErrorCode, (const char*)winErrorString);
        LocalFree(winErrorString);

        return libError;
#else
        return dlerror();
#endif
    }

protected:
    PluginType m_type;
    short m_id;
    unsigned int m_hints;

    bool m_active;
    bool m_active_before;

    void* m_lib;
    const char* m_name;
    const char* m_filename;

    int8_t cin_channel;

    double x_drywet, x_vol, x_bal_left, x_bal_right;
    jack_client_t* jack_client;

    // Storage Data
    PluginAudioData ain;
    PluginAudioData aout;
    PluginMidiData midi;
    PluginParameterData param;
    PluginProgramData prog;
    PluginMidiProgramData midiprog;
    QList<CustomData> custom;

    // Extra
#ifndef BUILD_BRIDGE
    struct {
        OscData data;
        CarlaPluginThread* thread;
    } osc;
#endif

    struct {
        QMutex lock;
        PluginPostEvent data[MAX_POST_EVENTS];
    } post_events;

    ExternalMidiNote ext_midi_notes[MAX_MIDI_EVENTS];

    // utilities
    static double fix_parameter_value(double& value, const ParameterRanges& ranges)
    {
        if (value < ranges.min)
            value = ranges.min;
        else if (value > ranges.max)
            value = ranges.max;
        return value;
    }

    static float fix_parameter_value(float& value, const ParameterRanges& ranges)
    {
        if (value < ranges.min)
            value = ranges.min;
        else if (value > ranges.max)
            value = ranges.max;
        return value;
    }

    static double abs_d(const double& value)
    {
        return (value < 0.0) ? -value : value;
    }
};

#endif // CARLA_PLUGIN_H