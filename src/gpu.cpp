#include "gpu.hpp"
#include <algorithm>
#include <cfloat> 

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
        uint32_t cmd = *(uint32_t*)&system_ram[ptr];
        ptr += 4;

        switch (cmd) {
            case 0x01: { // CLEAR
                uint32_t color = *(uint32_t*)&system_ram[ptr];
                ptr += 4;
                for (uint32_t i = 0; i < width * height; i++) {
                    color_buffer[i] = color;
                    depth_buffer[i] = FLT_MAX;
                }
                break;
            }
            case 0x04: { // DRAW_ARRAYS
                uint32_t v_offset = *(uint32_t*)&system_ram[ptr];
                uint32_t v_count  = *(uint32_t*)&system_ram[ptr + 4];
                ptr += 8;

                GPUVertex* verts = (GPUVertex*)&system_ram[v_offset];
                for (uint32_t i = 0; i < v_count; i += 3) {
                    rasterize_triangle(verts[i], verts[i+1], verts[i+2]);
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

void SpheroidGPU::rasterize_triangle(const GPUVertex& v0, const GPUVertex& v1, const GPUVertex& v2) {
    int x0 = (int)v0.x, y0 = (int)v0.y;
    int x1 = (int)v1.x, y1 = (int)v1.y;
    int x2 = (int)v2.x, y2 = (int)v2.y;

    int area = (x2 - x0) * (y1 - y0) - (y2 - y0) * (x1 - x0);
    
    if (state.backface_culling && area <= 0) return;
    if (area == 0) return;

    int A0 = (y2 - y1), B0 = (x1 - x2);
    int A1 = (y0 - y2), B1 = (x2 - x0);
    int A2 = (y1 - y0), B2 = (x0 - x1);

    int minX = std::max(0, std::min({x0, x1, x2}));
    int minY = std::max(0, std::min({y0, y1, y2}));
    int maxX = std::min((int)width - 1,  std::max({x0, x1, x2}));
    int maxY = std::min((int)height - 1, std::max({y0, y1, y2}));

    int row_w0 = (minX - x1) * A0 + (minY - y1) * B0;
    int row_w1 = (minX - x2) * A1 + (minY - y2) * B1;
    int row_w2 = (minX - x0) * A2 + (minY - y0) * B2;

    if (area < 0) {
        area = -area;
        row_w0 = -row_w0; A0 = -A0; B0 = -B0;
        row_w1 = -row_w1; A1 = -A1; B1 = -B1;
        row_w2 = -row_w2; A2 = -A2; B2 = -B2;
    }

    for (int y = minY; y <= maxY; y++) {
        int w0 = row_w0, w1 = row_w1, w2 = row_w2;
        int pixel_idx = y * width + minX;

        for (int x = minX; x <= maxX; x++) {
            if ((w0 | w1 | w2) >= 0) {
                float z = (w0 * v0.z + w1 * v1.z + w2 * v2.z) / area;
                
                if (!state.depth_test_enabled || z < depth_buffer[pixel_idx]) {
                    depth_buffer[pixel_idx] = z;
                    int r = (w0 * v0.r + w1 * v1.r + w2 * v2.r) / area;
                    int g = (w0 * v0.g + w1 * v1.g + w2 * v2.g) / area;
                    int b = (w0 * v0.b + w1 * v1.b + w2 * v2.b) / area;
                    color_buffer[pixel_idx] = (255 << 24) | (r << 16) | (g << 8) | b;
                }
            }
            w0 += A0; w1 += A1; w2 += A2;
            pixel_idx++;
        }
        row_w0 += B0; row_w1 += B1; row_w2 += B2;
    }
}