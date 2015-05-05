/*
 * Copyright © 2015 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Robert Carr <robert.carr@canonical.com>
 */

#ifndef MIR_FRONTEND_BUFFER_STREAM_H_
#define MIR_FRONTEND_BUFFER_STREAM_H_

#include <mir_toolkit/common.h>

#include <functional>
#include <memory>

namespace mir
{
namespace graphics
{
class Buffer;
}
namespace scene
{
class SurfaceObserver;
}
namespace compositor { class BufferStream; }
namespace frontend
{

class BufferStream
{
public:
    virtual ~BufferStream() = default;
    
    virtual void swap_buffers(graphics::Buffer* old_buffer, std::function<void(graphics::Buffer* new_buffer)> complete) = 0;

    virtual void add_observer(std::shared_ptr<scene::SurfaceObserver> const& observer) = 0;
    virtual void remove_observer(std::weak_ptr<scene::SurfaceObserver> const& observer) = 0;
    
    virtual void with_most_recent_buffer_do(
        std::function<void(graphics::Buffer&)> const& exec) = 0;

    virtual MirPixelFormat pixel_format() const = 0;

protected:
    BufferStream() = default;
    BufferStream(BufferStream const&) = delete;
    BufferStream& operator=(BufferStream const&) = delete;
};

std::shared_ptr<BufferStream> stream_from(std::shared_ptr<compositor::BufferStream> const&);
}
}


#endif /* MIR_FRONTEND_BUFFER_STREAM_H_ */
