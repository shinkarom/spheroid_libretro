#pragma once
#include <cstdint>
#include "math3d.hpp"

struct GPUState {
    bool depth_test_enabled = true;
    bool backface_culling = true;
};

class SpheroidGPU {
private:
    uint32_t width, height;
    uint32_t* color_buffer;
    float* depth_buffer;
    uint8_t* system_ram;
    GPUState state;

    void rasterize_triangle(const GPUVertex& v0, const GPUVertex& v1, const GPUVertex& v2);

public:
    SpheroidGPU();
    ~SpheroidGPU();

    void init(uint32_t w, uint32_t h);
    void shutdown();
    void set_ram_pointer(uint8_t* ram);
    const uint32_t* get_framebuffer() const;
    
    // Command Processor
    void execute_display_list(uint32_t ram_offset);
};