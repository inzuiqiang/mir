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

#include "mir/graphics/android/mir_native_window.h"
#include "mir/graphics/android/android_driver_interpreter.h"
#include "syncfence.h"

#include <unistd.h>
#include <sys/ioctl.h>

namespace mga=mir::graphics::android;

namespace
{

class IoctlControl : public mga::IoctlWrapper
{
public:
    int ioctl(int fd, unsigned long int request, int* timeout) const
    {
        return ::ioctl(fd, request, timeout);
    }
    int close(int fd) const
    {
        return ::close(fd);
    }
};

static int query_static(const ANativeWindow* anw, int key, int* value);
static int perform_static(ANativeWindow* anw, int key, ...);
static int setSwapInterval_static (struct ANativeWindow* window, int interval);
static int dequeueBuffer_deprecated_static (struct ANativeWindow* window,
                                 struct ANativeWindowBuffer** buffer);
static int dequeueBuffer_static (struct ANativeWindow* window,
                                 struct ANativeWindowBuffer** buffer, int* fence_fd);
static int lockBuffer_static(struct ANativeWindow* window,
                             struct ANativeWindowBuffer* buffer);
static int queueBuffer_deprecated_static(struct ANativeWindow* window,
                              struct ANativeWindowBuffer* buffer);
static int queueBuffer_static(struct ANativeWindow* window,
                              struct ANativeWindowBuffer* buffer, int fence_fd);
static int cancelBuffer_static(struct ANativeWindow* window,
                               struct ANativeWindowBuffer* buffer, int fence_fd);
static int cancelBuffer_deprecated_static(struct ANativeWindow* window,
                               struct ANativeWindowBuffer* buffer);

static void incRef(android_native_base_t*)
{
}

int query_static(const ANativeWindow* anw, int key, int* value)
{
    auto self = static_cast<const mga::MirNativeWindow*>(anw);
    return self->query(key, value);
}

int perform_static(ANativeWindow* window, int key, ...)
{
    va_list args;
    va_start(args, key);
    auto self = static_cast<mga::MirNativeWindow*>(window);
    auto ret = self->perform(key, args);
    va_end(args);

    return ret;
}

int dequeueBuffer_deprecated_static (struct ANativeWindow* window,
                          struct ANativeWindowBuffer** buffer)
{
    auto self = static_cast<mga::MirNativeWindow*>(window);
    return self->dequeueBuffer(buffer);
}

int dequeueBuffer_static (struct ANativeWindow* window,
                          struct ANativeWindowBuffer** buffer, int* fence_fd)
{
    printf("DEQUE.\n");
    *fence_fd = -1;
    auto self = static_cast<mga::MirNativeWindow*>(window);
    return self->dequeueBuffer(buffer);
}

int queueBuffer_deprecated_static(struct ANativeWindow* window,
                       struct ANativeWindowBuffer* buffer)
{
    auto self = static_cast<mga::MirNativeWindow*>(window);
    auto ioctl_control = std::make_shared<IoctlControl>();
    auto fence = std::make_shared<mga::SyncFence>(-1, ioctl_control);
    return self->queueBuffer(buffer, fence);
}

int queueBuffer_static(struct ANativeWindow* window,
                       struct ANativeWindowBuffer* buffer, int fence_fd)
{
    printf("QUEuue.\n");
    auto self = static_cast<mga::MirNativeWindow*>(window);
    auto ioctl_control = std::make_shared<IoctlControl>();
    auto fence = std::make_shared<mga::SyncFence>(fence_fd, ioctl_control);
    return self->queueBuffer(buffer, fence);

}

int setSwapInterval_static (struct ANativeWindow* window, int interval)
{
    auto self = static_cast<mga::MirNativeWindow*>(window);
    return self->setSwapInterval(interval);
}

/* lockBuffer, and cancelBuffer don't seem to being called by the driver. for now just return without calling into MirNativeWindow */

int lockBuffer_static(struct ANativeWindow* /*window*/,
                      struct ANativeWindowBuffer* /*buffer*/)
{
    printf("LOCK\n");
    return 0;
}

int cancelBuffer_deprecated_static(struct ANativeWindow* /*window*/,
                        struct ANativeWindowBuffer* /*buffer*/)
{
    printf("cancel. dep\n");
    return 0;
}

int cancelBuffer_static(struct ANativeWindow* window,
                        struct ANativeWindowBuffer* buffer, int fd)
{
printf("CANCEL!\n");
    auto ioctl_control = std::make_shared<IoctlControl>();
    auto fence = std::make_shared<mga::SyncFence>(fd, ioctl_control);
    auto self = static_cast<mga::MirNativeWindow*>(window);
    return self->queueBuffer(buffer, fence);
}

}

mga::MirNativeWindow::MirNativeWindow(std::shared_ptr<AndroidDriverInterpreter> const& interpreter)
 : driver_interpreter(interpreter)
{
    ANativeWindow::query = &query_static;
    ANativeWindow::perform = &perform_static;
    ANativeWindow::setSwapInterval = &setSwapInterval_static;
    ANativeWindow::dequeueBuffer_DEPRECATED = &dequeueBuffer_deprecated_static;
    ANativeWindow::dequeueBuffer = &dequeueBuffer_static;
    ANativeWindow::lockBuffer_DEPRECATED = &lockBuffer_static;
    ANativeWindow::queueBuffer_DEPRECATED = &queueBuffer_deprecated_static;
    ANativeWindow::queueBuffer = &queueBuffer_static;
    ANativeWindow::cancelBuffer_DEPRECATED = &cancelBuffer_deprecated_static;
    ANativeWindow::cancelBuffer = &cancelBuffer_static;

    ANativeWindow::common.incRef = &incRef;
    ANativeWindow::common.decRef = &incRef;

    const_cast<int&>(ANativeWindow::minSwapInterval) = 0;
    const_cast<int&>(ANativeWindow::maxSwapInterval) = 1;
}

int mga::MirNativeWindow::setSwapInterval(int interval)
{
    if (interval == 0)
    {
        driver_interpreter->sync_to_display(false);
    }
    else 
    {
        driver_interpreter->sync_to_display(true);
    }
    return 0;
}

int mga::MirNativeWindow::dequeueBuffer (struct ANativeWindowBuffer** buffer_to_driver)
{
  //  if (cancel_queue.empty())
        *buffer_to_driver = driver_interpreter->driver_requests_buffer();
   // else
   // {
   //     *buffer_to_driver = cancel_queue.back();
   //     cancel_queue.pop_back();
   // }
    auto b= *buffer_to_driver;
    printf("DEQUEUE! [0x%X] w %i h %i s %i, f %i u %X ha %X\n", (int)b, b->width, b->height, b->stride, b->format, b->usage, (int)b->handle);
    return 0;
}

int mga::MirNativeWindow::queueBuffer(struct ANativeWindowBuffer* buffer, std::shared_ptr<mga::SyncObject> const& fence)
{
    driver_interpreter->driver_returns_buffer(buffer, fence);
    return 0;
}

int mga::MirNativeWindow::cancelBuffer(struct ANativeWindowBuffer* buffer)
{
    (void) buffer;
    printf("CANCEL!\n");

 //   cancel_queue.push_back(buffer);
    return 0;
}
int mga::MirNativeWindow::query(int key, int* value ) const
{
    *value = driver_interpreter->driver_requests_info(key);
    printf("querying %i returns %i\n", key, *value);
    return 0;
}

int mga::MirNativeWindow::perform(int key, va_list arg_list )
{
    int ret = 0;
    va_list args;
    va_copy(args, arg_list);

    int a,b;
    int driver_format;
    switch(key)
    {
        case NATIVE_WINDOW_SET_BUFFERS_FORMAT:
            driver_format = va_arg(args, int);
            driver_interpreter->dispatch_driver_request_format(driver_format);
            break;
        case NATIVE_WINDOW_SET_BUFFERS_DIMENSIONS:
            a = va_arg(args, int);
            b = va_arg(args, int);
            printf("SIZE CHANGE REQUEST %i %i\n", a, b); 
        default:
            printf("UNSUPPORTED PERFORM! %i\n", key);
            break;
    }

    va_end(args);
    return ret;
}

