#pragma once

#include <wayland-client.h>
#include <cstdint>
#include <cstddef>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <span>

static void frame_ready_callback(void* data, wl_callback* cb, uint32_t);

namespace wl
{

struct shm_buffer
{
    int fd = -1;
    wl_buffer* wl_buffer = nullptr;
    std::byte* mapped_buffer = nullptr;
    size_t mapped_size = 0;
    bool in_flight = false;
};

class presenter
{
    wl_shm* shm = nullptr;
    wl_surface* surface = nullptr;

    static constexpr uint32_t format = WL_SHM_FORMAT_ARGB8888;
    uint32_t width_;
    uint32_t height_;
    uint32_t stride_;
    size_t buffer_size_;

    // double buffering
    shm_buffer buffers[2];
    int current_index = 0;
    
    bool frame_ready = true;
    wl_callback* frame_callback = nullptr;
    const wl_callback_listener frame_ready_listener;
    
public:
    presenter(wl_shm* shm, wl_surface* surface, uint32_t width, uint32_t height);
    ~presenter();

    // Returns pointer to writable pixel memory (RGBA8888)
    std::span<std::byte> begin_frame();
    // Attaches current buffer and commits
    void present();

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

private:
    void create_shm_buffer(shm_buffer& buf);
    int create_shm_fd();
    void destroy_shm_buffer(shm_buffer& buf);

    static void buffer_release(void* data, wl_buffer* buffer);

    friend void ::frame_ready_callback(void* data, wl_callback* cb, uint32_t);
};

}