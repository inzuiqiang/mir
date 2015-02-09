/*
 * Copyright © 2012 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Kevin DuBois <kevin.dubois@canonical.com>
 */

#include "mir/graphics/platform.h"
#include "display_configuration.h"
#include "mir/graphics/display_report.h"
#include "mir/graphics/display_buffer.h"
#include "mir/graphics/gl_context.h"
#include "mir/graphics/egl_resources.h"
#include "display.h"
#include "display_component_factory.h"
#include "interpreter_cache.h"
#include "server_render_window.h"
#include "display_buffer.h"
#include "hwc_layerlist.h"
#include "mir/graphics/android/mir_native_window.h"
#include "mir/geometry/rectangle.h"
#include "mir/graphics/event_handler_register.h"
#include "mir/fd.h"

#include <boost/throw_exception.hpp>
#include <fcntl.h>

namespace mga=mir::graphics::android;
namespace mg=mir::graphics;
namespace geom=mir::geometry;

struct mga::DisplayChangePipe
{
    DisplayChangePipe()
    {
        int pipes_raw[2] {-1, -1};
        if (::pipe2(pipes_raw, O_CLOEXEC | O_NONBLOCK))
            BOOST_THROW_EXCEPTION(std::runtime_error("failed to create display change pipe"));
        read_pipe = mir::Fd{pipes_raw[0]}; 
        write_pipe = mir::Fd{pipes_raw[1]}; 
    }

    void notify_change()
    {
        if (::write(write_pipe, &data, sizeof(data)) == -1)
            BOOST_THROW_EXCEPTION(std::runtime_error("failed to write to display change pipe"));
    }

    void ack_change()
    {
        char tmp{'b'};
        if (::read(read_pipe, &tmp, sizeof(tmp)) == -1)
            BOOST_THROW_EXCEPTION(std::runtime_error("failed to read from display change pipe"));
    }
    mir::Fd read_pipe;
    mir::Fd write_pipe;
    char const data{'a'};
};

namespace
{
void power_mode(
    mga::DisplayName name,
    mga::HwcConfiguration& control,
    mg::DisplayConfigurationOutput& config,
    MirPowerMode intended_mode)
{
    if (config.power_mode != intended_mode)
    {
        control.power_mode(name, intended_mode);
        config.power_mode = intended_mode;
    }
}

void power_mode_safe(
    mga::DisplayName name,
    mga::HwcConfiguration& control,
    mg::DisplayConfigurationOutput& config,
    MirPowerMode intended_mode) noexcept
try
{
    power_mode(name, control, config, intended_mode);
} catch (...) {}

void set_powermode_all_displays(
    mga::HwcConfiguration& control,
    mga::DisplayConfiguration& config,
    MirPowerMode intended_mode) noexcept
{
    power_mode_safe(mga::DisplayName::primary, control, config.primary(), intended_mode);
    if (config.external().connected)
        power_mode_safe(mga::DisplayName::external, control, config.external(), intended_mode); 
}

std::unique_ptr<mga::ConfigurableDisplayBuffer> create_display_buffer(
    std::shared_ptr<mga::DisplayDevice> const& display_device,
    mga::DisplayName name,
    mga::DisplayComponentFactory& display_buffer_builder,
    mga::DisplayAttribs const& attribs,
    std::shared_ptr<mg::GLProgramFactory> const& gl_program_factory,
    mga::PbufferGLContext const& gl_context,
    mga::OverlayOptimization overlay_option)
{
    std::shared_ptr<mga::FramebufferBundle> fbs{display_buffer_builder.create_framebuffers(attribs)};
    auto cache = std::make_shared<mga::InterpreterCache>();
    auto interpreter = std::make_shared<mga::ServerRenderWindow>(fbs, cache);
    auto native_window = std::make_shared<mga::MirNativeWindow>(interpreter);
    return std::unique_ptr<mga::ConfigurableDisplayBuffer>(new mga::DisplayBuffer(
        name,
        display_buffer_builder.create_layer_list(),
        fbs,
        display_device,
        native_window,
        gl_context,
        *gl_program_factory,
        mir_orientation_normal,
        overlay_option));
}
}

mga::Display::Display(
    std::shared_ptr<mga::DisplayComponentFactory> const& display_buffer_builder,
    std::shared_ptr<mg::GLProgramFactory> const& gl_program_factory,
    std::shared_ptr<GLConfig> const& gl_config,
    std::shared_ptr<DisplayReport> const& display_report,
    mga::OverlayOptimization overlay_option) :
    display_buffer_builder{display_buffer_builder},
    hwc_config{display_buffer_builder->create_hwc_configuration()},
    hotplug_subscription{hwc_config->subscribe_to_config_changes(std::bind(&mga::Display::on_hotplug, this))},
    primary_attribs(hwc_config->active_attribs_for(mga::DisplayName::primary)),
    external_attribs(hwc_config->active_attribs_for(mga::DisplayName::external)),
    config(
        primary_attribs,
        mir_power_mode_off,
        external_attribs,
        mir_power_mode_off),
    gl_context{config.primary().current_format, *gl_config, *display_report},
    display_device(display_buffer_builder->create_display_device()),
    primary_db{create_display_buffer(
        display_device,
        mga::DisplayName::primary,
        *display_buffer_builder,
        primary_attribs,
        gl_program_factory,
        gl_context,
        overlay_option)},
    display_change_pipe(new DisplayChangePipe),
    gl_program_factory(gl_program_factory)
{
    //Some drivers (depending on kernel state) incorrectly report an error code indicating that the display is already on. Ignore the first failure.
    set_powermode_all_displays(*hwc_config, config, mir_power_mode_on);

    if (config.external().connected)
    {
        external_db = create_display_buffer(
            display_device,
            mga::DisplayName::external,
            *display_buffer_builder,
            external_attribs,
            gl_program_factory,
            gl_context,
            mga::OverlayOptimization::disabled);
    }

    display_report->report_successful_setup_of_native_resources();

    gl_context.make_current();

    display_report->report_successful_egl_make_current_on_construction();
    display_report->report_successful_display_construction();
}

mga::Display::~Display() noexcept
{
    set_powermode_all_displays(*hwc_config, config, mir_power_mode_off);
}

void mga::Display::update_configuration(std::lock_guard<std::mutex> const&) const
{
    if (configuration_dirty)
    {
        auto attribs = hwc_config->active_attribs_for(mga::DisplayName::external);
        if (attribs.connected)
            power_mode(mga::DisplayName::external, *hwc_config, config.external(), mir_power_mode_on);
        else
            config.external().power_mode = mir_power_mode_off;

        config = mga::DisplayConfiguration(
            hwc_config->active_attribs_for(mga::DisplayName::primary),
            config.primary().power_mode,
            attribs,
            config.external().power_mode);
        configuration_dirty = false;

        if (config.external().connected)
            external_db = create_display_buffer(
                display_device,
                mga::DisplayName::external,
                *display_buffer_builder,
                attribs,
                gl_program_factory,
                gl_context,
                mga::OverlayOptimization::disabled);
        else
            external_db.reset();
    }

}

void mga::Display::for_each_display_buffer(std::function<void(mg::DisplayBuffer&)> const& f)
{
    std::lock_guard<decltype(configuration_mutex)> lock{configuration_mutex};
    update_configuration(lock);

    if (external_db && config.external().power_mode == mir_power_mode_on)
        f(*external_db);
    if (config.primary().power_mode == mir_power_mode_on)
        f(*primary_db);
}

std::unique_ptr<mg::DisplayConfiguration> mga::Display::configuration() const
{
    std::lock_guard<decltype(configuration_mutex)> lock{configuration_mutex};
    update_configuration(lock);
    return std::unique_ptr<mg::DisplayConfiguration>(new mga::DisplayConfiguration(config));
}

void mga::Display::configure(mg::DisplayConfiguration const& new_configuration)
{
    if (!new_configuration.valid())
        BOOST_THROW_EXCEPTION(std::logic_error("Invalid or inconsistent display configuration"));

    std::lock_guard<decltype(configuration_mutex)> lock{configuration_mutex};

    new_configuration.for_each_output([this](mg::DisplayConfigurationOutput const& output)
    {
        if (output.current_format != config[output.id].current_format)
            BOOST_THROW_EXCEPTION(std::logic_error("could not change display buffer format"));

        config[output.id].orientation = output.orientation;
        if (config.primary().id == output.id)
        {
            power_mode(mga::DisplayName::primary, *hwc_config, config.primary(), output.power_mode);
            primary_db->configure(output.power_mode, output.orientation);
        }
        else if (config.external().connected)
        {
            power_mode(mga::DisplayName::external, *hwc_config, config.external(), output.power_mode);
            if (external_db) external_db->configure(output.power_mode, output.orientation);
        }
    });
}

//NOTE: We avoid calling back to hwc from within the hotplug callback. Only arrange for an update.
void mga::Display::on_hotplug()
{
    std::lock_guard<decltype(configuration_mutex)> lock{configuration_mutex};
    configuration_dirty = true;
    display_change_pipe->notify_change();
}

void mga::Display::register_configuration_change_handler(
    EventHandlerRegister& event_handler,
    DisplayConfigurationChangeHandler const& change_handler)
{
    event_handler.register_fd_handler({display_change_pipe->read_pipe}, this, [change_handler, this](int){
        change_handler();
        display_change_pipe->ack_change();
    });
}

void mga::Display::register_pause_resume_handlers(
    EventHandlerRegister& /*handlers*/,
    DisplayPauseHandler const& /*pause_handler*/,
    DisplayResumeHandler const& /*resume_handler*/)
{
}

void mga::Display::pause()
{
}

void mga::Display::resume()
{
}

auto mga::Display::create_hardware_cursor(std::shared_ptr<mg::CursorImage> const& /* initial_image */) -> std::shared_ptr<Cursor>
{
    return nullptr;
}

std::unique_ptr<mg::GLContext> mga::Display::create_gl_context()
{
    return std::unique_ptr<mg::GLContext>{new mga::PbufferGLContext(gl_context)};
}