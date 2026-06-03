#include "gpu.hpp"
#include <algorithm>
#include <cfloat> 
#include <cstring>
#include <cmath> // Added for std::round
#include <cstdio>

// Helper for safe memory reading (fixes strict aliasing / unaligned access crashes)
static inline uint32_t read_u32(const uint8_t* ram, uint32_t offset) {
    uint32_t val;
    std::memcpy(&val, &ram[offset], sizeof(uint32_t));
    return val;
}

SpheroidGPU::SpheroidGPU() : color_buffer(nullptr), depth_buffer(nullptr), system_ram(nullptr) {}
SpheroidGPU::~SpheroidGPU() { shutdown(); }

void SpheroidGPU::init(uint32_t w, uint32_t h) {
    width = w; height = h;
    color_buffer = new uint32_t[w * h];
    depth_buffer = new float[w * h];
}

void SpheroidGPU::shutdown() {
    if (color_buffer) { delete[] color_buffer; color_buffer = nullptr; }
    if (depth_buffer) { delete[] depth_buffer; depth_buffer = nullptr; }
}

void SpheroidGPU::set_ram_pointer(uint8_t* ram) { system_ram = ram; }
const uint32_t* SpheroidGPU::get_framebuffer() const { return color_buffer; }

void SpheroidGPU::execute_display_list(uint32_t ram_offset) {
    if (!system_ram) return;
    uint32_t ptr = ram_offset;
    bool done = false;

    while (!done) {
        uint32_t cmd = read_u32(system_ram, ptr);
        ptr += 4;

        switch (cmd) {
            case 0x01: { // CLEAR
                uint32_t color = read_u32(system_ram, ptr);
                ptr += 4;
                for (uint32_t i = 0; i < width * height; i++) {
                    color_buffer[i] = color;
                    // Because we store 1/W instead of Z, -1.0f means "infinitely far away"
                    depth_buffer[i] = -1.0f; 
                }
				printf("clearing\n");
                break;
            }
            case 0x02: { // SET_MATRIX
                // memcpy is safe for unaligned blocks
                std::memcpy(&state.transform_matrix, &system_ram[ptr], sizeof(HMM_Mat4));
                ptr += 64; 
                break;
            }
            case 0x03: { // BIND_TEXTURE
                uint32_t tex_offset = read_u32(system_ram, ptr);
                state.tex_width     = read_u32(system_ram, ptr + 4);
                state.tex_height    = read_u32(system_ram, ptr + 8);
                
                if (tex_offset != 0) {
                    // Safe to keep pointer if tex array is properly aligned by the allocator, 
                    // otherwise texture reads in rasterize_triangle should use memcpy too.
                    state.texture_ptr = (uint32_t*)&system_ram[tex_offset];
                    state.texturing_enabled = true;
                } else {
                    state.texture_ptr = nullptr;
                    state.texturing_enabled = false;
                }
                ptr += 12;
                break;
            }
            case 0x04: { // DRAW_ARRAYS
                uint32_t v_offset = read_u32(system_ram, ptr);
                uint32_t v_count  = read_u32(system_ram, ptr + 4);
                ptr += 8;
                
                for (uint32_t i = 0; i < v_count; i += 3) {
                    ScreenVertex screen_verts[3];
                    bool reject = false;

                    for (int v = 0; v < 3; v++) {
                        // Safely extract vertex without violating strict aliasing
                        GPUVertex in;
                        std::memcpy(&in, &system_ram[v_offset + (i + v) * sizeof(GPUVertex)], sizeof(GPUVertex));

                        HMM_Vec4 clip = state.transform_matrix * HMM_V4(in.x, in.y, in.z, 1.0f);
                        
                        // Simple Near-Plane Rejection 
                        // (TODO: Implement Sutherland-Hodgman Frustum Clipping here to prevent polygon popping)
                        if (clip.W < 0.1f) { reject = true; break; }

                        float inv_w = 1.0f / clip.W;
                        float ndc_x = clip.X * inv_w;
                        float ndc_y = clip.Y * inv_w;

                        screen_verts[v].x = (ndc_x + 1.0f) * 0.5f * (float)width;
                        screen_verts[v].y = (1.0f - ndc_y) * 0.5f * (float)height;
                        screen_verts[v].z = clip.Z * inv_w; 
                        screen_verts[v].inv_w = inv_w;
                        
                        // Pre-multiply Colors and UVs by 1/W
                        screen_verts[v].r = (float)in.r * inv_w;
                        screen_verts[v].g = (float)in.g * inv_w; 
                        screen_verts[v].b = (float)in.b * inv_w; 
                        screen_verts[v].u = in.u * inv_w; 
                        screen_verts[v].v = in.v * inv_w; 
                    }

                    if (!reject) rasterize_triangle(screen_verts[0], screen_verts[1], screen_verts[2]);
                }
                break;
            }
            case 0xFF: { // END LIST
                done = true;
                break;
            }
            default: done = true; break; 
        }
    }
}

void SpheroidGPU::rasterize_triangle(const ScreenVertex& v0, const ScreenVertex& v1, const ScreenVertex& v2) {
    const int SUB_BITS = 4;
    const float SUB_MULT = 16.0f; 

    int64_t x0 = (int64_t)std::round(v0.x * SUB_MULT), y0 = (int64_t)std::round(v0.y * SUB_MULT);
    int64_t x1 = (int64_t)std::round(v1.x * SUB_MULT), y1 = (int64_t)std::round(v1.y * SUB_MULT);
    int64_t x2 = (int64_t)std::round(v2.x * SUB_MULT), y2 = (int64_t)std::round(v2.y * SUB_MULT);

    // 64-bit Area (Prevents overflow tearing)
    int64_t area = (x2 - x0) * (y1 - y0) - (y2 - y0) * (x1 - x0);
    if (state.backface_culling && area <= 0) return;
    if (area == 0) return;

    // Bounding Box
    int minX = std::max(0LL, std::min({x0, x1, x2}) >> SUB_BITS);
    int minY = std::max(0LL, std::min({y0, y1, y2}) >> SUB_BITS);
    int maxX = std::min((int64_t)width - 1,  (std::max({x0, x1, x2}) + 15) >> SUB_BITS);
    int maxY = std::min((int64_t)height - 1, (std::max({y0, y1, y2}) + 15) >> SUB_BITS);

    // Bounding Box Early-Out (Optimization)
    if (minX >= (int)width || maxX < 0 || minY >= (int)height || maxY < 0) return;

    int64_t A0 = y2 - y1, B0 = x1 - x2;
    int64_t A1 = y0 - y2, B1 = x2 - x0;
    int64_t A2 = y1 - y0, B2 = x0 - x1;

    int64_t px = (minX << SUB_BITS) + 8;
    int64_t py = (minY << SUB_BITS) + 8;

    int64_t row_w0 = (px - x1) * A0 + (py - y1) * B0;
    int64_t row_w1 = (px - x2) * A1 + (py - y2) * B1;
    int64_t row_w2 = (px - x0) * A2 + (py - y0) * B2;

    int64_t stepX_w0 = A0 * 16, stepY_w0 = B0 * 16;
    int64_t stepX_w1 = A1 * 16, stepY_w1 = B1 * 16;
    int64_t stepX_w2 = A2 * 16, stepY_w2 = B2 * 16;

    if (area < 0) {
        area = -area;
        row_w0 = -row_w0; stepX_w0 = -stepX_w0; stepY_w0 = -stepY_w0;
        row_w1 = -row_w1; stepX_w1 = -stepX_w1; stepY_w1 = -stepY_w1;
        row_w2 = -row_w2; stepX_w2 = -stepX_w2; stepY_w2 = -stepY_w2;
    }

    // Top-Left Rule bias to prevent overdraw on shared edges
    auto edge_bias = [](int64_t dy, int64_t dx) -> int64_t {
        return ((dx > 0) || (dx == 0 && dy < 0)) ? 0 : -1;
    };
    row_w0 += edge_bias(A0, B0);
    row_w1 += edge_bias(A1, B1);
    row_w2 += edge_bias(A2, B2);

    // Precalculate Barycentric Steps outside the loop (Massive optimization)
    // Kept as double precision to prevent texture warping on huge screens
    double inv_area = 1.0 / (double)area;
    
    double inv_w0_step = inv_area * v0.inv_w;
    double inv_w1_step = inv_area * v1.inv_w;
    double inv_w2_step = inv_area * v2.inv_w;

    double u0_step = inv_area * v0.u, v0_step = inv_area * v0.v;
    double u1_step = inv_area * v1.u, v1_step = inv_area * v1.v;
    double u2_step = inv_area * v2.u, v2_step = inv_area * v2.v;

    double r0_step = inv_area * v0.r, g0_step = inv_area * v0.g, b0_step = inv_area * v0.b;
    double r1_step = inv_area * v1.r, g1_step = inv_area * v1.g, b1_step = inv_area * v1.b;
    double r2_step = inv_area * v2.r, g2_step = inv_area * v2.g, b2_step = inv_area * v2.b;

    for (int y = minY; y <= maxY; y++) {
        int64_t w0 = row_w0, w1 = row_w1, w2 = row_w2;
        int pixel_idx = y * width + minX;

        for (int x = minX; x <= maxX; x++) {
            if ((w0 | w1 | w2) >= 0) {
                
                // Fast float evaluation from precalculated double steps
                float interp_inv_w = (float)(w0 * inv_w0_step + w1 * inv_w1_step + w2 * inv_w2_step);

                if (!state.depth_test_enabled || interp_inv_w > depth_buffer[pixel_idx]) {
                    depth_buffer[pixel_idx] = interp_inv_w;

                    float w = 1.0f / interp_inv_w;

                    float u = (float)(w0 * u0_step + w1 * u1_step + w2 * u2_step) * w;
                    float v = (float)(w0 * v0_step + w1 * v1_step + w2 * v2_step) * w;

                    int r = (int)((float)(w0 * r0_step + w1 * r1_step + w2 * r2_step) * w);
                    int g = (int)((float)(w0 * g0_step + w1 * g1_step + w2 * g2_step) * w);
                    int b = (int)((float)(w0 * b0_step + w1 * b1_step + w2 * b2_step) * w);

                    if (state.texturing_enabled) {
                        // Fast Bitwise wrapping (ASSUMES textures are a Power of Two, e.g., 64, 128, 256)
                        int tx = (int)(u * state.tex_width) & (state.tex_width - 1);
                        int ty = (int)(v * state.tex_height) & (state.tex_height - 1);

                        uint32_t texel = state.texture_ptr[ty * state.tex_width + tx];
                        
                        int tr = (texel >> 16) & 0xFF;
                        int tg = (texel >> 8) & 0xFF;
                        int tb = texel & 0xFF;

                        r = (r * tr) / 255;
                        g = (g * tg) / 255;
                        b = (b * tb) / 255;
                    }

                    r = std::clamp(r, 0, 255);
                    g = std::clamp(g, 0, 255);
                    b = std::clamp(b, 0, 255);

                    color_buffer[pixel_idx] = (255 << 24) | (r << 16) | (g << 8) | b;
                }
            }
            w0 += stepX_w0; w1 += stepX_w1; w2 += stepX_w2;
            pixel_idx++;
        }
        row_w0 += stepY_w0; row_w1 += stepY_w1; row_w2 += stepY_w2;
    }
}