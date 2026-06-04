#include "gpu.hpp"
#include <algorithm>
#include <cfloat> 
#include <cstring>
#include <cmath> 
#include <cstdio>
#include "HandmadeMath.h"

struct ClipVertex {
    HMM_Vec4 pos;
    float u, v;
    float r, g, b, a; // NEW: Alpha Channel
};

static inline uint32_t read_u32(const uint8_t* ram, uint32_t offset) {
    uint32_t val;
    std::memcpy(&val, &ram[offset], sizeof(uint32_t));
    return val;
}

static inline float read_f32(const uint8_t* ram, uint32_t offset) {
    float val;
    std::memcpy(&val, &ram[offset], sizeof(float));
    return val;
}

SpheroidGPU::SpheroidGPU() : color_buffer(nullptr), depth_buffer(nullptr), system_ram(nullptr), pending_clear(false) {}
SpheroidGPU::~SpheroidGPU() { shutdown(); }

void SpheroidGPU::init(uint32_t w, uint32_t h) {
    width = w; height = h;
    color_buffer = new uint32_t[w * h];
    depth_buffer = new float[w * h];
    
    std::memset(color_buffer, 0, w * h * sizeof(uint32_t));
    std::memset(depth_buffer, 0, w * h * sizeof(float));

    num_tiles_x = (w + TILE_SIZE - 1) / TILE_SIZE;
    num_tiles_y = (h + TILE_SIZE - 1) / TILE_SIZE;
    tiles.resize(num_tiles_x * num_tiles_y);

    if (workers.empty()) {
        stop_threads.store(false);
        current_tile.store(num_tiles_x * num_tiles_y); 
        
        unsigned int num_cores = std::thread::hardware_concurrency();
        if (num_cores == 0) num_cores = 4; 
        
        for (unsigned int i = 0; i < num_cores - 1; i++) {
            workers.emplace_back(&SpheroidGPU::worker_thread, this);
        }
    }
}

void SpheroidGPU::shutdown() {
    stop_threads.store(true);
    cv_start.notify_all();
    
    for (auto& t : workers) {
        if (t.joinable()) t.join();
    }
    workers.clear();
    
    if (color_buffer) { delete[] color_buffer; color_buffer = nullptr; }
    if (depth_buffer) { delete[] depth_buffer; depth_buffer = nullptr; }
}

void SpheroidGPU::set_ram_pointer(uint8_t* ram) { system_ram = ram; }
const uint32_t* SpheroidGPU::get_framebuffer() const { return color_buffer; }

void SpheroidGPU::execute_display_list(uint32_t ram_offset) {
    if (!system_ram) return;
    uint32_t ptr = ram_offset;
    bool done = false;

    // Reset all buckets for the new frame
    for (auto& tile : tiles) {
        tile.opaque_triangles.clear();
        tile.punchthrough_triangles.clear();
        tile.translucent_triangles.clear();
    }

    // Default to opaque at the start of a display list
    state.current_render_list = RenderListType::OPAQUE_LIST;

    while (!done) {
        uint32_t cmd = read_u32(system_ram, ptr);
        ptr += 4;

        switch (cmd) {
            case 0x01: { // CLEAR
                clear_color_val = read_u32(system_ram, ptr);
                clear_depth_val = -1.0f;
                pending_clear = true;
                ptr += 4;
                break;
            }
            case 0x02: { // LOAD_MATRIX
                std::memcpy(&state.modelview_matrix, &system_ram[ptr], sizeof(HMM_Mat4));
                ptr += 64; 
                break;
            }
            case 0x03: { // BIND_TEXTURE
                uint32_t tex_offset = read_u32(system_ram, ptr);
                state.tex_width     = read_u32(system_ram, ptr + 4);
                state.tex_height    = read_u32(system_ram, ptr + 8);
                if (tex_offset != 0) {
                    state.texture_ptr = (uint32_t*)&system_ram[tex_offset];
                    state.texturing_enabled = true;
                } else {
                    state.texture_ptr = nullptr;
                    state.texturing_enabled = false;
                }
                ptr += 12;
                break;
            }
            case 0x05: { // PUSH_MATRIX
                state.matrix_stack.push_back(state.modelview_matrix);
                break;
            }
            case 0x06: { // POP_MATRIX
                if (!state.matrix_stack.empty()) {
                    state.modelview_matrix = state.matrix_stack.back();
                    state.matrix_stack.pop_back();
                }
                break;
            }
            case 0x07: { // LOAD_IDENTITY
                state.modelview_matrix = HMM_M4D(1.0f);
                break;
            }
            case 0x08: { // TRANSLATE
                float x = read_f32(system_ram, ptr);
                float y = read_f32(system_ram, ptr + 4);
                float z = read_f32(system_ram, ptr + 8);
                state.modelview_matrix = state.modelview_matrix * HMM_Translate(HMM_V3(x, y, z));
                ptr += 12;
                break;
            }
            case 0x09: { // ROTATE_X
                float angle = read_f32(system_ram, ptr);
                state.modelview_matrix = state.modelview_matrix * HMM_Rotate_LH(angle, HMM_V3(1.0f, 0.0f, 0.0f));
                ptr += 4; break;
            }
            case 0x0A: { // ROTATE_Y
                float angle = read_f32(system_ram, ptr);
                state.modelview_matrix = state.modelview_matrix * HMM_Rotate_LH(angle, HMM_V3(0.0f, 1.0f, 0.0f));
                ptr += 4; break;
            }
            case 0x0B: { // SCALE
                float x = read_f32(system_ram, ptr);
                float y = read_f32(system_ram, ptr + 4);
                float z = read_f32(system_ram, ptr + 8);
                state.modelview_matrix = state.modelview_matrix * HMM_Scale(HMM_V3(x, y, z));
                ptr += 12; break;
            }
            case 0x0C: { // SET_PERSPECTIVE
                float fov = read_f32(system_ram, ptr);
                float aspect = read_f32(system_ram, ptr + 4);
                float near_z = read_f32(system_ram, ptr + 8);
                float far_z = read_f32(system_ram, ptr + 12);
                state.projection_matrix = HMM_Perspective_LH_ZO(fov, aspect, near_z, far_z);
                ptr += 16; break;
            }
            case 0x0D: { // LOAD_PROJECTION
                std::memcpy(&state.projection_matrix, &system_ram[ptr], sizeof(HMM_Mat4));
                ptr += 64; 
                break;
            }
            case 0x0E: { // SET_RENDER_LIST
                uint32_t list_type = read_u32(system_ram, ptr);
                if (list_type <= 2) {
                    state.current_render_list = static_cast<RenderListType>(list_type);
                }
                ptr += 4;
                break;
            }
            case 0x04: { // DRAW_ARRAYS
                uint32_t v_offset = read_u32(system_ram, ptr);
                uint32_t v_count  = read_u32(system_ram, ptr + 4);
                ptr += 8;
                
                HMM_Mat4 mvp = state.projection_matrix * state.modelview_matrix;

                for (uint32_t i = 0; i < v_count; i += 3) {
                    ClipVertex in_verts[3];

                    for (int v = 0; v < 3; v++) {
                        GPUVertex in;
                        std::memcpy(&in, &system_ram[v_offset + (i + v) * sizeof(GPUVertex)], sizeof(GPUVertex));

                        in_verts[v].pos = mvp * HMM_V4(in.x, in.y, in.z, 1.0f);
                        in_verts[v].u = in.u;
                        in_verts[v].v = in.v;
                        in_verts[v].r = (float)in.r;
                        in_verts[v].g = (float)in.g;
                        in_verts[v].b = (float)in.b;
                        in_verts[v].a = (float)in.a; // Pass alpha
                    }

                    ClipVertex out_verts[4]; 
                    int out_count = 0;
                    const float W_PLANE = 0.01f; 

                    for (int e = 0; e < 3; e++) {
                        int next = (e + 1) % 3;
                        const ClipVertex& v1 = in_verts[e];
                        const ClipVertex& v2 = in_verts[next];

                        float d1 = v1.pos.W - W_PLANE;
                        float d2 = v2.pos.W - W_PLANE;

                        if (d1 >= 0.0f) {
                            out_verts[out_count++] = v1;
                        }

                        if ((d1 >= 0.0f && d2 < 0.0f) || (d1 < 0.0f && d2 >= 0.0f)) {
                            float t = d1 / (d1 - d2);
                            
                            ClipVertex intersect;
                            intersect.pos.X = v1.pos.X + (v2.pos.X - v1.pos.X) * t;
                            intersect.pos.Y = v1.pos.Y + (v2.pos.Y - v1.pos.Y) * t;
                            intersect.pos.Z = v1.pos.Z + (v2.pos.Z - v1.pos.Z) * t;
                            intersect.pos.W = v1.pos.W + (v2.pos.W - v1.pos.W) * t;
                            intersect.u = v1.u + (v2.u - v1.u) * t;
                            intersect.v = v1.v + (v2.v - v1.v) * t;
                            intersect.r = v1.r + (v2.r - v1.r) * t;
                            intersect.g = v1.g + (v2.g - v1.g) * t;
                            intersect.b = v1.b + (v2.b - v1.b) * t;
                            intersect.a = v1.a + (v2.a - v1.a) * t; // Interpolate alpha

                            out_verts[out_count++] = intersect;
                        }
                    }

                    for (int tri = 1; tri < out_count - 1; tri++) {
                        ScreenVertex screen_verts[3];
                        ClipVertex* poly[3] = { &out_verts[0], &out_verts[tri], &out_verts[tri+1] };

                        for (int v = 0; v < 3; v++) {
                            float inv_w = 1.0f / poly[v]->pos.W;
                            float ndc_x = poly[v]->pos.X * inv_w;
                            float ndc_y = poly[v]->pos.Y * inv_w;

                            screen_verts[v].x = (ndc_x + 1.0f) * 0.5f * (float)width;
                            screen_verts[v].y = (1.0f - ndc_y) * 0.5f * (float)height;
                            screen_verts[v].z = poly[v]->pos.Z * inv_w;
                            screen_verts[v].inv_w = inv_w;

                            screen_verts[v].r = poly[v]->r * inv_w;
                            screen_verts[v].g = poly[v]->g * inv_w;
                            screen_verts[v].b = poly[v]->b * inv_w;
                            screen_verts[v].a = poly[v]->a * inv_w; // Multiply alpha
                            screen_verts[v].u = poly[v]->u * inv_w;
                            screen_verts[v].v = poly[v]->v * inv_w;
                        }

                        bin_triangle(screen_verts[0], screen_verts[1], screen_verts[2]);
                    }
                }
                break;
            }
            case 0xFF: done = true; break;
            default: done = true; break; 
        }
    }
    flush_tiles();
}

void SpheroidGPU::bin_triangle(const ScreenVertex& v0, const ScreenVertex& v1, const ScreenVertex& v2) {
    const int SUB_BITS = 4;
    const float SUB_MULT = 16.0f; 

    int64_t x0 = (int64_t)std::round(v0.x * SUB_MULT), y0 = (int64_t)std::round(v0.y * SUB_MULT);
    int64_t x1 = (int64_t)std::round(v1.x * SUB_MULT), y1 = (int64_t)std::round(v1.y * SUB_MULT);
    int64_t x2 = (int64_t)std::round(v2.x * SUB_MULT), y2 = (int64_t)std::round(v2.y * SUB_MULT);

    int64_t area = (x2 - x0) * (y1 - y0) - (y2 - y0) * (x1 - x0);
    if (state.backface_culling && area <= 0) return;
    if (area == 0) return;

    int minX = std::max(0LL, std::min({x0, x1, x2}) >> SUB_BITS);
    int minY = std::max(0LL, std::min({y0, y1, y2}) >> SUB_BITS);
    int maxX = std::min((int64_t)width - 1,  (std::max({x0, x1, x2}) + 15) >> SUB_BITS);
    int maxY = std::min((int64_t)height - 1, (std::max({y0, y1, y2}) + 15) >> SUB_BITS);

    if (minX >= (int)width || maxX < 0 || minY >= (int)height || maxY < 0) return;

    int start_tx = minX / TILE_SIZE;
    int end_tx   = maxX / TILE_SIZE;
    int start_ty = minY / TILE_SIZE;
    int end_ty   = maxY / TILE_SIZE;

    BinnedTriangle tri;
    tri.v0 = v0; tri.v1 = v1; tri.v2 = v2;
    tri.minX = minX; tri.minY = minY; tri.maxX = maxX; tri.maxY = maxY;
    tri.texturing_enabled = state.texturing_enabled;
    tri.tex_width = state.tex_width;
    tri.tex_height = state.tex_height;
    tri.texture_ptr = state.texture_ptr;
    tri.depth_test_enabled = state.depth_test_enabled;

    for (int ty = start_ty; ty <= end_ty; ty++) {
        for (int tx = start_tx; tx <= end_tx; tx++) {
            auto& tile = tiles[ty * num_tiles_x + tx];
            if (state.current_render_list == RenderListType::OPAQUE_LIST) {
                tile.opaque_triangles.push_back(tri);
            } else if (state.current_render_list == RenderListType::PUNCH_THROUGH_LIST) {
                tile.punchthrough_triangles.push_back(tri);
            } else if (state.current_render_list == RenderListType::TRANSLUCENT_LIST) {
                tile.translucent_triangles.push_back(tri);
            }
        }
    }
}

// ==============================================================================
// TEMPLATED TBDR RASTERIZER
// ==============================================================================
template <bool TEXTURED, bool DEPTH_TEST, RenderListType LIST_TYPE>
void SpheroidGPU::rasterize_binned_impl(const BinnedTriangle& tri, int tile_min_x, int tile_min_y, int tile_max_x, int tile_max_y) {
    const int SUB_BITS = 4;
    const float SUB_MULT = 16.0f; 

    int64_t x0 = (int64_t)std::round(tri.v0.x * SUB_MULT), y0 = (int64_t)std::round(tri.v0.y * SUB_MULT);
    int64_t x1 = (int64_t)std::round(tri.v1.x * SUB_MULT), y1 = (int64_t)std::round(tri.v1.y * SUB_MULT);
    int64_t x2 = (int64_t)std::round(tri.v2.x * SUB_MULT), y2 = (int64_t)std::round(tri.v2.y * SUB_MULT);

    int64_t area = (x2 - x0) * (y1 - y0) - (y2 - y0) * (x1 - x0);
    if (area < 0) area = -area;

    int minX = std::max(tri.minX, tile_min_x);
    int minY = std::max(tri.minY, tile_min_y);
    int maxX = std::min(tri.maxX, tile_max_x);
    int maxY = std::min(tri.maxY, tile_max_y);

    if (minX > maxX || minY > maxY) return; 

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

    auto edge_bias = [](int64_t dy, int64_t dx) -> int64_t {
        return ((dx > 0) || (dx == 0 && dy < 0)) ? 0 : -1;
    };
    row_w0 += edge_bias(A0, B0);
    row_w1 += edge_bias(A1, B1);
    row_w2 += edge_bias(A2, B2);

    double inv_area = 1.0 / (double)area;
    double inv_w0_step = inv_area * tri.v0.inv_w, u0_step = inv_area * tri.v0.u, v0_step = inv_area * tri.v0.v;
    double inv_w1_step = inv_area * tri.v1.inv_w, u1_step = inv_area * tri.v1.u, v1_step = inv_area * tri.v1.v;
    double inv_w2_step = inv_area * tri.v2.inv_w, u2_step = inv_area * tri.v2.u, v2_step = inv_area * tri.v2.v;
    double r0_step = inv_area * tri.v0.r, g0_step = inv_area * tri.v0.g, b0_step = inv_area * tri.v0.b;
    double r1_step = inv_area * tri.v1.r, g1_step = inv_area * tri.v1.g, b1_step = inv_area * tri.v1.b;
    double r2_step = inv_area * tri.v2.r, g2_step = inv_area * tri.v2.g, b2_step = inv_area * tri.v2.b;
    double a0_step = inv_area * tri.v0.a, a1_step = inv_area * tri.v1.a, a2_step = inv_area * tri.v2.a;

    for (int y = minY; y <= maxY; y++) {
        int64_t w0 = row_w0, w1 = row_w1, w2 = row_w2;
        int pixel_idx = y * width + minX;

        for (int x = minX; x <= maxX; x++) {
            if ((w0 | w1 | w2) >= 0) {
                float interp_inv_w = (float)(w0 * inv_w0_step + w1 * inv_w1_step + w2 * inv_w2_step);

                bool pass_depth = true;
                if constexpr (DEPTH_TEST) {
                    pass_depth = (interp_inv_w > depth_buffer[pixel_idx]);
                }

                if (pass_depth) {
                    float w = 1.0f / interp_inv_w;
                    int r = (int)((float)(w0 * r0_step + w1 * r1_step + w2 * r2_step) * w);
                    int g = (int)((float)(w0 * g0_step + w1 * g1_step + w2 * g2_step) * w);
                    int b = (int)((float)(w0 * b0_step + w1 * b1_step + w2 * b2_step) * w);
                    int a = (int)((float)(w0 * a0_step + w1 * a1_step + w2 * a2_step) * w);

                    if constexpr (TEXTURED) {
                        float u = (float)(w0 * u0_step + w1 * u1_step + w2 * u2_step) * w;
                        float v = (float)(w0 * v0_step + w1 * v1_step + w2 * v2_step) * w;

                        int tx = (int)(u * tri.tex_width) & (tri.tex_width - 1);
                        int ty = (int)(v * tri.tex_height) & (tri.tex_height - 1);

                        uint32_t texel = tri.texture_ptr[ty * tri.tex_width + tx];
                        r = (r * ((texel >> 16) & 0xFF)) / 255;
                        g = (g * ((texel >> 8) & 0xFF)) / 255;
                        b = (b * (texel & 0xFF)) / 255;
                        a = (a * ((texel >> 24) & 0xFF)) / 255;
                    }

                    if constexpr (LIST_TYPE == RenderListType::PUNCH_THROUGH_LIST) {
                        if (a < 128) {
                            w0 += stepX_w0; w1 += stepX_w1; w2 += stepX_w2; pixel_idx++;
                            continue; 
                        }
                    }

                    r = std::clamp(r, 0, 255);
                    g = std::clamp(g, 0, 255);
                    b = std::clamp(b, 0, 255);
                    a = std::clamp(a, 0, 255);

                    if constexpr (LIST_TYPE != RenderListType::TRANSLUCENT_LIST) {
                        if constexpr (DEPTH_TEST) depth_buffer[pixel_idx] = interp_inv_w;
                    }

                    if constexpr (LIST_TYPE == RenderListType::TRANSLUCENT_LIST) {
                        uint32_t bg = color_buffer[pixel_idx];
                        int bg_r = (bg >> 16) & 0xFF;
                        int bg_g = (bg >> 8) & 0xFF;
                        int bg_b = bg & 0xFF;
                        
                        r = (r * a + bg_r * (255 - a)) / 255;
                        g = (g * a + bg_g * (255 - a)) / 255;
                        b = (b * a + bg_b * (255 - a)) / 255;
                    }

                    color_buffer[pixel_idx] = (255 << 24) | (r << 16) | (g << 8) | b;
                }
            }
            w0 += stepX_w0; w1 += stepX_w1; w2 += stepX_w2;
            pixel_idx++;
        }
        row_w0 += stepY_w0; row_w1 += stepY_w1; row_w2 += stepY_w2;
    }
}

template <bool TEXTURED, bool DEPTH_TEST>
void SpheroidGPU::dispatch_list_type(const BinnedTriangle& tri, int min_x, int min_y, int max_x, int max_y, RenderListType list_type) {
    switch (list_type) {
        case RenderListType::OPAQUE_LIST:
            rasterize_binned_impl<TEXTURED, DEPTH_TEST, RenderListType::OPAQUE_LIST>(tri, min_x, min_y, max_x, max_y); break;
        case RenderListType::PUNCH_THROUGH_LIST:
            rasterize_binned_impl<TEXTURED, DEPTH_TEST, RenderListType::PUNCH_THROUGH_LIST>(tri, min_x, min_y, max_x, max_y); break;
        case RenderListType::TRANSLUCENT_LIST:
            rasterize_binned_impl<TEXTURED, DEPTH_TEST, RenderListType::TRANSLUCENT_LIST>(tri, min_x, min_y, max_x, max_y); break;
    }
}

void SpheroidGPU::rasterize_binned(const BinnedTriangle& tri, int min_x, int min_y, int max_x, int max_y, RenderListType list_type) {
    if (tri.texturing_enabled && tri.texture_ptr) {
        if (tri.depth_test_enabled) dispatch_list_type<true, true>(tri, min_x, min_y, max_x, max_y, list_type);
        else                        dispatch_list_type<true, false>(tri, min_x, min_y, max_x, max_y, list_type);
    } else {
        if (tri.depth_test_enabled) dispatch_list_type<false, true>(tri, min_x, min_y, max_x, max_y, list_type);
        else                        dispatch_list_type<false, false>(tri, min_x, min_y, max_x, max_y, list_type);
    }
}

void SpheroidGPU::process_tiles_loop() {
    int total_tiles = num_tiles_x * num_tiles_y;

    while (true) {
        int tile_idx = current_tile.fetch_add(1);
        if (tile_idx >= total_tiles) break;

        int tx = tile_idx % num_tiles_x;
        int ty = tile_idx / num_tiles_x;
        
        int tile_min_x = tx * TILE_SIZE;
        int tile_min_y = ty * TILE_SIZE;
        int tile_max_x = std::min((int)width - 1, tile_min_x + TILE_SIZE - 1);
        int tile_max_y = std::min((int)height - 1, tile_min_y + TILE_SIZE - 1);

        if (pending_clear) {
            for (int y = tile_min_y; y <= tile_max_y; y++) {
                int row_idx = y * width;
                for (int x = tile_min_x; x <= tile_max_x; x++) {
                    color_buffer[row_idx + x] = clear_color_val;
                    depth_buffer[row_idx + x] = clear_depth_val;
                }
            }
        }

        // PHASE 1: OPAQUE
        for (const auto& tri : tiles[tile_idx].opaque_triangles) {
            rasterize_binned(tri, tile_min_x, tile_min_y, tile_max_x, tile_max_y, RenderListType::OPAQUE_LIST);
        }
        
        // PHASE 2: PUNCH-THROUGH
        for (const auto& tri : tiles[tile_idx].punchthrough_triangles) {
            rasterize_binned(tri, tile_min_x, tile_min_y, tile_max_x, tile_max_y, RenderListType::PUNCH_THROUGH_LIST);
        }

        // PHASE 3: TRANSLUCENT (Sorted Back-To-Front by average 1/W)
        std::sort(tiles[tile_idx].translucent_triangles.begin(), 
                  tiles[tile_idx].translucent_triangles.end(),
                  [](const BinnedTriangle& a, const BinnedTriangle& b) {
                      float avg_a = a.v0.inv_w + a.v1.inv_w + a.v2.inv_w;
                      float avg_b = b.v0.inv_w + b.v1.inv_w + b.v2.inv_w;
                      return avg_a < avg_b; 
                  });

        for (const auto& tri : tiles[tile_idx].translucent_triangles) {
            rasterize_binned(tri, tile_min_x, tile_min_y, tile_max_x, tile_max_y, RenderListType::TRANSLUCENT_LIST);
        }

        if (completed_tiles.fetch_add(1) + 1 == total_tiles) {
            std::unique_lock<std::mutex> lock(cv_m);
            cv_done.notify_one();
        }
    }
}

void SpheroidGPU::worker_thread() {
    int total_tiles = num_tiles_x * num_tiles_y;

    while (true) {
        std::unique_lock<std::mutex> lock(cv_m);
        cv_start.wait(lock, [this, total_tiles]() { 
            return stop_threads.load() || (current_tile.load() < total_tiles); 
        });

        if (stop_threads.load()) break;
        lock.unlock();

        process_tiles_loop();
    }
}

void SpheroidGPU::flush_tiles() {
    current_tile.store(0);
    completed_tiles.store(0);
    int total = num_tiles_x * num_tiles_y;

    cv_start.notify_all();
    process_tiles_loop();

    std::unique_lock<std::mutex> lock(cv_m);
    cv_done.wait(lock, [this, total] { 
        return completed_tiles.load() == total; 
    });

    pending_clear = false;
}