#include <format>
#include <string/platform/wayland/presenter.hpp>
#include <string/logger.hpp>

#include <stdexcept>
#include <span>
#include <cstring>

namespace wl
{

presenter::presenter(wl_shm* shm, wl_surface* surface, uint32_t width, uint32_t height)
: shm(shm), surface(surface), width_(width), height_(height)
{
    stride_ = width_ * 4; // ARGB8888
    buffer_size_ = stride_ * height_;

    for (auto& buf : buffers) {
        create_shm_buffer(buf);
    }
}

presenter::~presenter()
{
    STRING_LOG_DEBUG("Shutting down presenter.");
    for (auto& buf : buffers)
    {
        destroy_shm_buffer(buf);
    }
}

void presenter::create_shm_buffer(shm_buffer& buf)
{
    buf.fd = create_shm_fd();
    if (ftruncate(buf.fd, buffer_size_) < 0)
        throw std::runtime_error("ftruncate failed");

    buf.mapped_size = buffer_size_;
    buf.mapped_buffer = static_cast<std::byte*>(mmap(nullptr, buf.mapped_size, PROT_READ | PROT_WRITE, MAP_SHARED, buf.fd, 0));
    if (buf.mapped_buffer == MAP_FAILED)
        throw std::runtime_error("mmap failed");

    wl_shm_pool* pool = wl_shm_create_pool(shm, buf.fd, static_cast<int>(buf.mapped_size));
    buf.wl_buffer = wl_shm_pool_create_buffer(
        pool, 0, static_cast<int>(width_), static_cast<int>(height_),
        static_cast<int>(stride_), format
    );
    wl_shm_pool_destroy(pool);

    static constexpr wl_buffer_listener listener = {
        .release = [](void* data, wl_buffer*)
        {
            static_cast<shm_buffer*>(data)->in_flight = false;
        }
    };
    wl_buffer_add_listener(buf.wl_buffer, &listener, &buf);
}

int presenter::create_shm_fd()
{
    static uint32_t count = 0;
    std::string name = std::format("/str-eng-{}-{}", getpid(), count++);

    int fd = memfd_create(name.c_str(), MFD_CLOEXEC);
    if (fd >= 0) return fd;

    // int fd = shm_open(name.c_str(), O_CREAT | O_RDWR | O_EXCL, 0600);
    // shm_unlink(name.c_str());

    return fd;
}

void presenter::destroy_shm_buffer(shm_buffer& buf)
{
    if (buf.mapped_buffer) munmap(buf.mapped_buffer, buf.mapped_size);
    if (buf.wl_buffer) wl_buffer_destroy(buf.wl_buffer);
    if (buf.fd >= 0) close(buf.fd);
}

std::span<std::byte> presenter::begin_frame()
{
    shm_buffer& buf = buffers[current_index];

    // Wait if busy
    if (buf.in_flight)
        return {}; // could wait/spin instead

    std::memset(buf.mapped_buffer, 0, buffer_size_); // optional clear
    return {buf.mapped_buffer, buffer_size_};
}

void presenter::present()
{
    shm_buffer& buf = buffers[current_index];

    buf.in_flight = true;

    wl_surface_attach(surface, buf.wl_buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, width_, height_);
    wl_surface_commit(surface);

    // Flip buffer
    current_index = (current_index + 1) % 2;
}

}