#pragma once
#include <cstdint>
#include "HandmadeMath.h"

// The vertex format stored in emulated RAM (exactly 16 bytes)
struct GPUVertex { 
    float x, y, z; 
	float u, v; 
    uint8_t r, g, b, a; 
};

struct ScreenVertex {
    float x, y, z;
    float inv_w;
    float r, g, b;
    float u, v;       // NEW: U and V pre-divided by W
};

struct GPUState {
    bool depth_test_enabled = true;
    bool backface_culling = true;
	HMM_Mat4 transform_matrix;
	
	uint32_t* texture_ptr = nullptr;
    uint32_t tex_width = 0;
    uint32_t tex_height = 0;
    bool texturing_enabled = false;
};

class SpheroidGPU {
private:
    uint32_t width, height;
    uint32_t* color_buffer;
    float* depth_buffer;
    uint8_t* system_ram;
    GPUState state;

    void rasterize_triangle(const ScreenVertex& v0, const ScreenVertex& v1, const ScreenVertex& v2);

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