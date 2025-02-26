/*************************************************************************
 * This file is part of input-overlay
 * github.con/univrsal/input-overlay
 * Copyright 2020 univrsal <uni@vrsal.cf>.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *************************************************************************/

#include "input_source.hpp"
#include "../hook/gamepad_hook_helper.hpp"
#include "../util/lang.h"
#include "../util/obs_util.hpp"
#include "../util/settings.h"
#include "../util/config.hpp"
#include "../network/io_server.hpp"
#include "../network/remote_connection.hpp"
#include <QFile>
#include <QJsonDocument>
#include <obs-frontend-api.h>

namespace sources {
input_source::~input_source() {}

inline void input_source::update(obs_data_t *settings)
{
    m_settings.selected_source = obs_data_get_int(settings, S_INPUT_SOURCE);

    const auto *config = obs_data_get_string(settings, S_LAYOUT_FILE);
    m_settings.image_file = obs_data_get_string(settings, S_OVERLAY_FILE);

    if (m_settings.layout_file != config) /* Only reload config file if path changed */
    {
        m_settings.layout_file = config;
        m_overlay->load();
    }

    libgamepad::hook_instance->get_mutex()->lock();
    m_settings.gamepad_id = obs_data_get_string(settings, S_CONTROLLER_ID);
    m_settings.gamepad = libgamepad::hook_instance->get_device_by_id(m_settings.gamepad_id);
    libgamepad::hook_instance->get_mutex()->unlock();

    m_settings.mouse_sens = obs_data_get_int(settings, S_MOUSE_SENS);

    if ((m_settings.use_center = obs_data_get_bool(settings, S_MONITOR_USE_CENTER))) {
        m_settings.monitor_h = obs_data_get_int(settings, S_MONITOR_H_CENTER);
        m_settings.monitor_w = obs_data_get_int(settings, S_MONITOR_V_CENTER);
        m_settings.mouse_deadzone = obs_data_get_int(settings, S_MOUSE_DEAD_ZONE);
    }
}

inline void input_source::tick(float seconds)
{
    UNUSED_PARAMETER(seconds);
    if (m_overlay->is_loaded())
        m_overlay->refresh_data();

    if (m_settings.layout_flags & OF_GAMEPAD && !m_settings.gamepad) {
        m_settings.gamepad_check_timer += seconds;
        if (m_settings.gamepad_check_timer >= 1) {
            m_settings.gamepad_check_timer = 0.0f;
            libgamepad::hook_instance->get_mutex()->lock();
            m_settings.gamepad = libgamepad::hook_instance->get_device_by_id(m_settings.gamepad_id);
            libgamepad::hook_instance->get_mutex()->unlock();
        }
    }
}

inline void input_source::render(gs_effect_t *effect) const
{
    if (!m_overlay->get_texture() || !m_overlay->get_texture()->texture)
        return;

    if (m_settings.layout_file.empty() || !m_overlay->is_loaded()) {
        gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), m_overlay->get_texture()->texture);
        gs_draw_sprite(m_overlay->get_texture()->texture, 0, cx, cy);
    } else {
        m_overlay->draw(effect);
    }
}

bool use_monitor_center_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *data)
{
    UNUSED_PARAMETER(p);

    const auto use_center = obs_data_get_bool(data, S_MONITOR_USE_CENTER);
    obs_property_set_visible(GET_PROPS(S_MONITOR_H_CENTER), use_center);
    obs_property_set_visible(GET_PROPS(S_MONITOR_V_CENTER), use_center);
    return true;
}

bool reload_connections(obs_properties_t *props, obs_property_t *property, void *data)
{
    UNUSED_PARAMETER(props);
    UNUSED_PARAMETER(data);
    std::lock_guard<std::mutex> lock(network::mutex);
    network::server_instance->get_clients(property, network::local_input);
    return true;
}

bool reload_pads(obs_properties_t *props, obs_property_t *property, void *data)
{
    UNUSED_PARAMETER(props);
    UNUSED_PARAMETER(data);

    obs_property_list_clear(property);
    libgamepad::hook_instance->get_mutex()->lock();
    for (const auto &pad : libgamepad::hook_instance->get_devices()) {
        obs_property_list_add_string(property, pad->get_name().c_str(), pad->get_id().c_str());
    }
    libgamepad::hook_instance->get_mutex()->unlock();
    return true;
}

obs_properties_t *get_properties_for_overlay(void *data)
{
    auto *src = static_cast<input_source *>(data);

    QString img_path, layout_path;
    auto *const props = obs_properties_create();
    const int flags = src->m_settings.layout_flags;

    /* If enabled add dropdown to select input source */
    if (CGET_BOOL(S_REMOTE)) {
        auto *list =
            obs_properties_add_list(props, S_INPUT_SOURCE, T_INPUT_SOURCE, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
        obs_properties_add_button(props, S_RELOAD_CONNECTIONS, T_RELOAD_CONNECTIONS, reload_connections);
        if (network::network_flag) {
            network::server_instance->get_clients(list, network::local_input);
        }
    }

    const auto filter_img = util_file_filter(T_FILTER_IMAGE_FILES, "*.jpg *.png *.bmp");
    const auto filter_text = util_file_filter(T_FILTER_TEXT_FILES, "*.json");

    /* Config and texture file path */
    obs_properties_add_path(props, S_OVERLAY_FILE, T_TEXTURE_FILE, OBS_PATH_FILE, qt_to_utf8(filter_img),
                            qt_to_utf8(img_path));
    obs_properties_add_path(props, S_LAYOUT_FILE, T_LAYOUT_FILE, OBS_PATH_FILE, qt_to_utf8(filter_text),
                            qt_to_utf8(layout_path));

    /* Mouse stuff */
    obs_properties_add_int_slider(props, S_MOUSE_SENS, T_MOUSE_SENS, 1, 500, 1);

    const auto use_center = obs_properties_add_bool(props, S_MONITOR_USE_CENTER, T_MONITOR_USE_CENTER);
    obs_property_set_modified_callback(use_center, use_monitor_center_changed);

    obs_properties_add_int(props, S_MONITOR_H_CENTER, T_MONITOR_H_CENTER, -9999, 9999, 1);
    obs_properties_add_int(props, S_MONITOR_V_CENTER, T_MONITOR_V_CENTER, -9999, 9999, 1);
    obs_properties_add_int_slider(props, S_MOUSE_DEAD_ZONE, T_MOUSE_DEAD_ZONE, 0, 500, 1);

    /* Gamepad stuff */
    obs_property_set_visible(obs_properties_add_list(props, S_CONTROLLER_ID, T_CONTROLLER_ID, OBS_COMBO_TYPE_LIST,
                                                     OBS_COMBO_FORMAT_STRING),
                             false);

    auto *btn = obs_properties_add_button(props, S_RELOAD_PAD_DEVICES, T_RELOAD_PAD_DEVICES, reload_pads);
    obs_property_set_visible(btn, false);

    obs_property_set_visible(GET_PROPS(S_CONTROLLER_L_DEAD_ZONE), flags & OF_LEFT_STICK);
    obs_property_set_visible(GET_PROPS(S_CONTROLLER_R_DEAD_ZONE), flags & OF_RIGHT_STICK);
    obs_property_set_visible(GET_PROPS(S_CONTROLLER_ID),
                             flags & OF_GAMEPAD || (flags & OF_LEFT_STICK || flags & OF_RIGHT_STICK));
    obs_property_set_visible(GET_PROPS(S_MOUSE_SENS), flags & OF_MOUSE);
    obs_property_set_visible(GET_PROPS(S_MONITOR_USE_CENTER), flags & OF_MOUSE);
    obs_property_set_visible(GET_PROPS(S_MOUSE_DEAD_ZONE), flags & OF_MOUSE);
    obs_property_set_visible(GET_PROPS(S_RELOAD_PAD_DEVICES), flags & OF_GAMEPAD);
    reload_pads(nullptr, GET_PROPS(S_CONTROLLER_ID), nullptr);
    return props;
}

void register_overlay_source()
{
    /* Input Overlay */
    obs_source_info si = {};
    si.id = "input-overlay";
    si.type = OBS_SOURCE_TYPE_INPUT;
    si.output_flags = OBS_SOURCE_VIDEO;
    si.get_properties = get_properties_for_overlay;

    si.get_name = [](void *) { return obs_module_text("InputOverlay"); };
    si.create = [](obs_data_t *settings, obs_source_t *source) {
        return static_cast<void *>(new input_source(source, settings));
    };
    si.destroy = [](void *data) { delete static_cast<input_source *>(data); };
    si.get_width = [](void *data) { return static_cast<input_source *>(data)->m_settings.cx; };
    si.get_height = [](void *data) { return static_cast<input_source *>(data)->m_settings.cy; };
    si.get_defaults = [](obs_data_t *settings) { UNUSED_PARAMETER(settings); };
    si.update = [](void *data, obs_data_t *settings) { static_cast<input_source *>(data)->update(settings); };
    si.video_tick = [](void *data, float seconds) { static_cast<input_source *>(data)->tick(seconds); };
    si.video_render = [](void *data, gs_effect_t *effect) { static_cast<input_source *>(data)->render(effect); };
    obs_register_source(&si);
}
}
