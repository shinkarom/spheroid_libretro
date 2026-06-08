#include "gpu.hpp"
#include <algorithm>
#include <cfloat> 
#include <cstring>
#include <cmath> 
#include <cstdio>
#include "HandmadeMath.h"

SpheroidGPU::SpheroidGPU() : color_buffer(nullptr), pending_clear(false) {}
SpheroidGPU::~SpheroidGPU() { shutdown(); }

void SpheroidGPU::init(uint32_t w, uint32_t h) {
    width = w; height = h;
    color_buffer = new uint32_t[w * h];
    std::memset(color_buffer, 0, w * h * sizeof(uint32_t));

    num_tiles_x = (w + TILE_SIZE - 1) / TILE_SIZE;
    num_tiles_y = (h + TILE_SIZE - 1) / TILE_SIZE;
    tiles.resize(num_tiles_x * num_tiles_y);

    screen_vertex_pool.reserve(500000); 
    triangle_pool.reserve(200000); 
    node_arena.resize(5000000);    
    command_queue.reserve(10000); // Prevent reallocation during frame building

    if (workers.empty()) {
        stop_threads.store(false);
        current_tile.store(num_tiles_x * num_tiles_y); 
        unsigned int num_cores = std::thread::hardware_concurrency();
        if (num_cores == 0) num_cores = 4; 
        for (unsigned int i = 0; i < num_cores - 1; i++) workers.emplace_back(&SpheroidGPU::worker_thread, this);
    }
}

void SpheroidGPU::shutdown() {
    stop_threads.store(true);
    cv_start.notify_all();
    for (auto& t : workers) if (t.joinable()) t.join();
    workers.clear();
    if (color_buffer) { delete[] color_buffer; color_buffer = nullptr; }
}

const uint32_t* SpheroidGPU::get_framebuffer() const { return color_buffer; }

// =============================================================================
// Resource Management
// =============================================================================

uint32_t SpheroidGPU::load_texture(uint32_t w, uint32_t h, const uint32_t* pixels) {
    TextureResource tex;
    tex.width = w;
    tex.height = h;
    tex.pixels.assign(pixels, pixels + (w * h));
    textures.push_back(std::move(tex));
    return textures.size(); // 1-based ID
}

uint32_t SpheroidGPU::load_mesh(const std::vector<GPUVertex>& verts, const std::vector<uint16_t>& indices) {
    MeshResource mesh;
    mesh.vertices = verts;
    mesh.indices = indices;
    meshes.push_back(std::move(mesh));
    return meshes.size(); // 1-based ID
}

// =============================================================================
// Command Queue Builders
// =============================================================================

void SpheroidGPU::cmd_clear(uint32_t color) {
    RenderCommand cmd; cmd.type = CommandType::CLEAR; cmd.clear_color = color;
    command_queue.push_back(cmd);
}
void SpheroidGPU::cmd_set_render_list(RenderListType type) {
    RenderCommand cmd; cmd.type = CommandType::SET_RENDER_LIST; cmd.list_type = type;
    command_queue.push_back(cmd);
}
void SpheroidGPU::cmd_push_matrix() {
    RenderCommand cmd; cmd.type = CommandType::PUSH_MATRIX;
    command_queue.push_back(cmd);
}
void SpheroidGPU::cmd_pop_matrix() {
    RenderCommand cmd; cmd.type = CommandType::POP_MATRIX;
    command_queue.push_back(cmd);
}
void SpheroidGPU::cmd_load_identity() {
    RenderCommand cmd; cmd.type = CommandType::LOAD_IDENTITY;
    command_queue.push_back(cmd);
}
void SpheroidGPU::cmd_load_matrix(const HMM_Mat4& mat) {
    RenderCommand cmd; cmd.type = CommandType::LOAD_MATRIX; 
    std::memcpy(cmd.matrix.f, &mat.Elements[0][0], sizeof(float) * 16);
    command_queue.push_back(cmd);
}
void SpheroidGPU::cmd_translate(float x, float y, float z) {
    RenderCommand cmd; cmd.type = CommandType::TRANSLATE; cmd.vec3 = {x, y, z};
    command_queue.push_back(cmd);
}
void SpheroidGPU::cmd_rotate_x(float angle) {
    RenderCommand cmd; cmd.type = CommandType::ROTATE_X; cmd.angle = angle;
    command_queue.push_back(cmd);
}
void SpheroidGPU::cmd_rotate_y(float angle) {
    RenderCommand cmd; cmd.type = CommandType::ROTATE_Y; cmd.angle = angle;
    command_queue.push_back(cmd);
}
void SpheroidGPU::cmd_rotate_z(float angle) {
    RenderCommand cmd; cmd.type = CommandType::ROTATE_Z; cmd.angle = angle;
    command_queue.push_back(cmd);
}
void SpheroidGPU::cmd_scale(float x, float y, float z) {
    RenderCommand cmd; cmd.type = CommandType::SCALE; cmd.vec3 = {x, y, z};
    command_queue.push_back(cmd);
}
void SpheroidGPU::cmd_set_projection(float fov, float aspect, float near_z, float far_z) {
    RenderCommand cmd; cmd.type = CommandType::SET_PROJECTION; 
    cmd.perspective = {fov, aspect, near_z, far_z};
    command_queue.push_back(cmd);
}
void SpheroidGPU::cmd_draw_mesh(uint32_t mesh_id, uint32_t tex_id) {
    RenderCommand cmd; cmd.type = CommandType::DRAW_MESH; cmd.draw = {mesh_id, tex_id};
    command_queue.push_back(cmd);
}

// =============================================================================
// Execution Pipeline
// =============================================================================

void SpheroidGPU::flush_command_queue() {
    screen_vertex_pool.clear();
    triangle_pool.clear();
    node_count = 0;

    for (auto& tile : tiles) {
        tile.opaque_head = 0xFFFFFFFF;
        tile.punchthrough_head = 0xFFFFFFFF;
        tile.translucent_head = 0xFFFFFFFF;
    }

    state.current_render_list = RenderListType::OPAQUE_LIST;
    state.modelview_matrix = HMM_M4D(1.0f);
    state.matrix_stack.clear();

    for (const auto& cmd : command_queue) {
        switch (cmd.type) {
            case CommandType::CLEAR:
                clear_color_val = cmd.clear_color; clear_depth_val = -1.0f; pending_clear = true; break;
            case CommandType::SET_RENDER_LIST:
                state.current_render_list = cmd.list_type; break;
            case CommandType::PUSH_MATRIX:
                state.matrix_stack.push_back(state.modelview_matrix); break;
            case CommandType::POP_MATRIX:
                if (!state.matrix_stack.empty()) { 
                    state.modelview_matrix = state.matrix_stack.back(); 
                    state.matrix_stack.pop_back(); 
                } break;
            case CommandType::LOAD_IDENTITY:
                state.modelview_matrix = HMM_M4D(1.0f); break;
            case CommandType::LOAD_MATRIX:
                std::memcpy(&state.modelview_matrix, cmd.matrix.f, sizeof(float) * 16); break;
            case CommandType::TRANSLATE:
                state.modelview_matrix = state.modelview_matrix * HMM_Translate(HMM_V3(cmd.vec3.x, cmd.vec3.y, cmd.vec3.z)); break;
            case CommandType::ROTATE_X:
                state.modelview_matrix = state.modelview_matrix * HMM_Rotate_LH(cmd.angle, HMM_V3(1.0f, 0.0f, 0.0f)); break;
            case CommandType::ROTATE_Y:
                state.modelview_matrix = state.modelview_matrix * HMM_Rotate_LH(cmd.angle, HMM_V3(0.0f, 1.0f, 0.0f)); break;
            case CommandType::ROTATE_Z:
                state.modelview_matrix = state.modelview_matrix * HMM_Rotate_LH(cmd.angle, HMM_V3(0.0f, 0.0f, 1.0f)); break;
            case CommandType::SCALE:
                state.modelview_matrix = state.modelview_matrix * HMM_Scale(HMM_V3(cmd.vec3.x, cmd.vec3.y, cmd.vec3.z)); break;
            case CommandType::SET_PROJECTION:
                state.projection_matrix = HMM_Perspective_LH_ZO(cmd.perspective.fov, cmd.perspective.aspect, cmd.perspective.near_z, cmd.perspective.far_z); break;
            
            case CommandType::DRAW_MESH: {
                if (cmd.draw.mesh_id == 0 || cmd.draw.mesh_id > meshes.size()) break;
                
                const MeshResource& mesh = meshes[cmd.draw.mesh_id - 1];

                if (cmd.draw.tex_id > 0 && cmd.draw.tex_id <= textures.size()) {
                    const TextureResource& tex = textures[cmd.draw.tex_id - 1];
                    state.texturing_enabled = true;
                    state.texture_ptr = tex.pixels.data();
                    state.tex_width = tex.width;
                    state.tex_height = tex.height;
                } else {
                    state.texturing_enabled = false;
                    state.texture_ptr = nullptr;
                }

                HMM_Mat4 mvp = state.projection_matrix * state.modelview_matrix;

                if (vertex_cache.size() < mesh.vertices.size()) {
                    vertex_cache.resize(mesh.vertices.size());
                }

                for (size_t v = 0; v < mesh.vertices.size(); v++) {
                    const GPUVertex& in = mesh.vertices[v];
                    vertex_cache[v].pos = mvp * HMM_V4(in.x, in.y, in.z, 1.0f);
                    vertex_cache[v].u = in.u; vertex_cache[v].v = in.v;
                    vertex_cache[v].r = (float)in.r; vertex_cache[v].g = (float)in.g; 
                    vertex_cache[v].b = (float)in.b; vertex_cache[v].a = (float)in.a; 
                }

                for (size_t i = 0; i < mesh.indices.size(); i += 3) {
                    process_clip_triangle(
                        vertex_cache[mesh.indices[i]], 
                        vertex_cache[mesh.indices[i+1]], 
                        vertex_cache[mesh.indices[i+2]]
                    );
                }
                break;
            }
        }
    }

    command_queue.clear();
    flush_tiles();
}

// =============================================================================
// Clipper & Rasterizer Math (Unchanged inner workings)
// =============================================================================

void SpheroidGPU::process_clip_triangle(const ClipVertex& in_v0, const ClipVertex& in_v1, const ClipVertex& in_v2) {
    ClipVertex in_verts[3] = { in_v0, in_v1, in_v2 };
    ClipVertex out_verts[4]; 
    int out_count = 0;
    const float W_PLANE = 0.01f; 

    for (int e = 0; e < 3; e++) {
        int next = (e + 1) % 3;
        const ClipVertex& v1 = in_verts[e];
        const ClipVertex& v2 = in_verts[next];

        float d1 = v1.pos.W - W_PLANE;
        float d2 = v2.pos.W - W_PLANE;

        if (d1 >= 0.0f) out_verts[out_count++] = v1;

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
            intersect.a = v1.a + (v2.a - v1.a) * t; 
            out_verts[out_count++] = intersect;
        }
    }

    for (int tri = 1; tri < out_count - 1; tri++) {
        ClipVertex* poly[3] = { &out_verts[0], &out_verts[tri], &out_verts[tri+1] };
        
        uint32_t start_idx = (uint32_t)screen_vertex_pool.size();
        for (int v = 0; v < 3; v++) {
            float inv_w = 1.0f / poly[v]->pos.W;
            float ndc_x = poly[v]->pos.X * inv_w;
            float ndc_y = poly[v]->pos.Y * inv_w;

            ScreenVertex sv;
            sv.x = (ndc_x + 1.0f) * 0.5f * (float)width;
            sv.y = (1.0f - ndc_y) * 0.5f * (float)height;
            sv.z = poly[v]->pos.Z * inv_w;
            sv.inv_w = inv_w;
            sv.r = poly[v]->r * inv_w; sv.g = poly[v]->g * inv_w;
            sv.b = poly[v]->b * inv_w; sv.a = poly[v]->a * inv_w; 
            sv.u = poly[v]->u * inv_w; sv.v = poly[v]->v * inv_w;
            screen_vertex_pool.push_back(sv);
        }
        bin_triangle(start_idx, start_idx + 1, start_idx + 2);
    }
}

void SpheroidGPU::bin_triangle(uint32_t v0_idx, uint32_t v1_idx, uint32_t v2_idx) {
    const ScreenVertex& v0 = screen_vertex_pool[v0_idx];
    const ScreenVertex& v1 = screen_vertex_pool[v1_idx];
    const ScreenVertex& v2 = screen_vertex_pool[v2_idx];

    const int SUB_BITS = 4;
    const float SUB_MULT = 16.0f; 

    int32_t x0 = (int32_t)(v0.x * SUB_MULT + 0.5f), y0 = (int32_t)(v0.y * SUB_MULT + 0.5f);
    int32_t x1 = (int32_t)(v1.x * SUB_MULT + 0.5f), y1 = (int32_t)(v1.y * SUB_MULT + 0.5f);
    int32_t x2 = (int32_t)(v2.x * SUB_MULT + 0.5f), y2 = (int32_t)(v2.y * SUB_MULT + 0.5f);

    int32_t area = (x2 - x0) * (y1 - y0) - (y2 - y0) * (x1 - x0);
    if (state.backface_culling && area <= 0) return;
    if (area == 0) return;

    int minX = std::max(0, (int)(std::min({x0, x1, x2}) >> SUB_BITS));
    int minY = std::max(0, (int)(std::min({y0, y1, y2}) >> SUB_BITS));
    int maxX = std::min((int)width - 1,  (int)((std::max({x0, x1, x2}) + 15) >> SUB_BITS));
    int maxY = std::min((int)height - 1, (int)((std::max({y0, y1, y2}) + 15) >> SUB_BITS));

    if (minX >= (int)width || maxX < 0 || minY >= (int)height || maxY < 0) return;

    int start_tx = minX / TILE_SIZE; int end_tx = maxX / TILE_SIZE;
    int start_ty = minY / TILE_SIZE; int end_ty = maxY / TILE_SIZE;

    BinnedTriangle tri;
    tri.v0_idx = v0_idx; tri.v1_idx = v1_idx; tri.v2_idx = v2_idx;
    tri.minX = (int16_t)minX; tri.minY = (int16_t)minY; tri.maxX = (int16_t)maxX; tri.maxY = (int16_t)maxY;
    tri.texturing_enabled = state.texturing_enabled;
    tri.tex_width = (uint16_t)state.tex_width;
    tri.tex_height = (uint16_t)state.tex_height;
    tri.texture_ptr = state.texture_ptr;
    tri.depth_test_enabled = state.depth_test_enabled;

    uint32_t tri_idx = (uint32_t)triangle_pool.size();
    triangle_pool.push_back(tri);

    for (int ty = start_ty; ty <= end_ty; ty++) {
        for (int tx = start_tx; tx <= end_tx; tx++) {
            auto& tile = tiles[ty * num_tiles_x + tx];
            if (node_count >= node_arena.size()) return; 
            
            uint32_t n = node_count++;
            node_arena[n].tri_idx = tri_idx;

            if (state.current_render_list == RenderListType::OPAQUE_LIST) {
                node_arena[n].next = tile.opaque_head; tile.opaque_head = n;
            } else if (state.current_render_list == RenderListType::PUNCH_THROUGH_LIST) {
                node_arena[n].next = tile.punchthrough_head; tile.punchthrough_head = n;
            } else if (state.current_render_list == RenderListType::TRANSLUCENT_LIST) {
                node_arena[n].next = tile.translucent_head; tile.translucent_head = n;
            }
        }
    }
}

template <bool TEXTURED, bool DEPTH_TEST, RenderListType LIST_TYPE>
void SpheroidGPU::rasterize_binned_impl(const BinnedTriangle& tri, int tile_min_x, int tile_min_y, int tile_max_x, int tile_max_y, uint32_t* local_color, float* local_depth) {
    const ScreenVertex& v0 = screen_vertex_pool[tri.v0_idx];
    const ScreenVertex& v1 = screen_vertex_pool[tri.v1_idx];
    const ScreenVertex& v2 = screen_vertex_pool[tri.v2_idx];

    const int SUB_BITS = 4;
    int32_t x0 = (int32_t)(v0.x * 16.0f + 0.5f), y0 = (int32_t)(v0.y * 16.0f + 0.5f);
    int32_t x1 = (int32_t)(v1.x * 16.0f + 0.5f), y1 = (int32_t)(v1.y * 16.0f + 0.5f);
    int32_t x2 = (int32_t)(v2.x * 16.0f + 0.5f), y2 = (int32_t)(v2.y * 16.0f + 0.5f);

    int32_t area = (x2 - x0) * (y1 - y0) - (y2 - y0) * (x1 - x0);
    if (area < 0) area = -area;

    int minX = std::max((int)tri.minX, tile_min_x);
    int minY = std::max((int)tri.minY, tile_min_y);
    int maxX = std::min((int)tri.maxX, tile_max_x);
    int maxY = std::min((int)tri.maxY, tile_max_y);

    if (minX > maxX || minY > maxY) return; 

    int32_t A0 = y2 - y1, B0 = x1 - x2;
    int32_t A1 = y0 - y2, B1 = x2 - x0;
    int32_t A2 = y1 - y0, B2 = x0 - x1;

    int32_t px = (minX << SUB_BITS) + 8;
    int32_t py = (minY << SUB_BITS) + 8;

    int32_t row_w0 = (px - x1) * A0 + (py - y1) * B0;
    int32_t row_w1 = (px - x2) * A1 + (py - y2) * B1;
    int32_t row_w2 = (px - x0) * A2 + (py - y0) * B2;

    int32_t stepX_w0 = A0 * 16, stepY_w0 = B0 * 16;
    int32_t stepX_w1 = A1 * 16, stepY_w1 = B1 * 16;
    int32_t stepX_w2 = A2 * 16, stepY_w2 = B2 * 16;

    auto edge_bias = [](int32_t dy, int32_t dx) -> int32_t { return ((dx > 0) || (dx == 0 && dy < 0)) ? 0 : -1; };
    row_w0 += edge_bias(A0, B0); row_w1 += edge_bias(A1, B1); row_w2 += edge_bias(A2, B2);

    double inv_area = 1.0 / (double)area;
    double inv_w0_step = inv_area * v0.inv_w, u0_step = inv_area * v0.u, v0_step = inv_area * v0.v;
    double inv_w1_step = inv_area * v1.inv_w, u1_step = inv_area * v1.u, v1_step = inv_area * v1.v;
    double inv_w2_step = inv_area * v2.inv_w, u2_step = inv_area * v2.u, v2_step = inv_area * v2.v;
    double r0_step = inv_area * v0.r, g0_step = inv_area * v0.g, b0_step = inv_area * v0.b;
    double r1_step = inv_area * v1.r, g1_step = inv_area * v1.g, b1_step = inv_area * v1.b;
    double r2_step = inv_area * v2.r, g2_step = inv_area * v2.g, b2_step = inv_area * v2.b;
    double a0_step = inv_area * v0.a, a1_step = inv_area * v1.a, a2_step = inv_area * v2.a;

    for (int y = minY; y <= maxY; y++) {
        int32_t w0 = row_w0, w1 = row_w1, w2 = row_w2;
        int local_y = y - tile_min_y;
        int x = minX;

        while (x <= maxX) {
            int span_len = std::min(8, maxX - x + 1);

            float inv_w_s = (w0 * inv_w0_step + w1 * inv_w1_step + w2 * inv_w2_step);
            float inv_w_e = ((w0 + span_len*stepX_w0) * inv_w0_step + (w1 + span_len*stepX_w1) * inv_w1_step + (w2 + span_len*stepX_w2) * inv_w2_step);

            float w_s = 1.0f / inv_w_s;
            float w_e = 1.0f / inv_w_e;

            float u_s = (w0 * u0_step + w1 * u1_step + w2 * u2_step) * w_s;
            float v_s = (w0 * v0_step + w1 * v1_step + w2 * v2_step) * w_s;
            float r_s = (w0 * r0_step + w1 * r1_step + w2 * r2_step) * w_s;
            float g_s = (w0 * g0_step + w1 * g1_step + w2 * g2_step) * w_s;
            float b_s = (w0 * b0_step + w1 * b1_step + w2 * b2_step) * w_s;
            float a_s = (w0 * a0_step + w1 * a1_step + w2 * a2_step) * w_s;

            float u_e = ((w0 + span_len*stepX_w0) * u0_step + (w1 + span_len*stepX_w1) * u1_step + (w2 + span_len*stepX_w2) * u2_step) * w_e;
            float v_e = ((w0 + span_len*stepX_w0) * v0_step + (w1 + span_len*stepX_w1) * v1_step + (w2 + span_len*stepX_w2) * v2_step) * w_e;
            float r_e = ((w0 + span_len*stepX_w0) * r0_step + (w1 + span_len*stepX_w1) * r1_step + (w2 + span_len*stepX_w2) * r2_step) * w_e;
            float g_e = ((w0 + span_len*stepX_w0) * g0_step + (w1 + span_len*stepX_w1) * g1_step + (w2 + span_len*stepX_w2) * g2_step) * w_e;
            float b_e = ((w0 + span_len*stepX_w0) * b0_step + (w1 + span_len*stepX_w1) * b1_step + (w2 + span_len*stepX_w2) * b2_step) * w_e;
            float a_e = ((w0 + span_len*stepX_w0) * a0_step + (w1 + span_len*stepX_w1) * a1_step + (w2 + span_len*stepX_w2) * a2_step) * w_e;

            float inv_span = 1.0f / span_len;
            float inv_w_step_span = (inv_w_e - inv_w_s) * inv_span;

            int32_t u_fixed = 0, v_fixed = 0, u_step_fixed = 0, v_step_fixed = 0;
            if constexpr (TEXTURED) {
                u_fixed = (int32_t)(u_s * tri.tex_width * 65536.0f);
                v_fixed = (int32_t)(v_s * tri.tex_height * 65536.0f);
                u_step_fixed = (int32_t)((u_e - u_s) * tri.tex_width * inv_span * 65536.0f);
                v_step_fixed = (int32_t)((v_e - v_s) * tri.tex_height * inv_span * 65536.0f);
            }

            int32_t r_fixed = (int32_t)(r_s * 65536.0f), g_fixed = (int32_t)(g_s * 65536.0f);
            int32_t b_fixed = (int32_t)(b_s * 65536.0f), a_fixed = (int32_t)(a_s * 65536.0f);
            int32_t r_step_fixed = (int32_t)((r_e - r_s) * inv_span * 65536.0f);
            int32_t g_step_fixed = (int32_t)((g_e - g_s) * inv_span * 65536.0f);
            int32_t b_step_fixed = (int32_t)((b_e - b_s) * inv_span * 65536.0f);
            int32_t a_step_fixed = (int32_t)((a_e - a_s) * inv_span * 65536.0f);

            float cur_inv_w = inv_w_s;
            int local_idx = local_y * TILE_SIZE + (x - tile_min_x);

            for (int s = 0; s < span_len; s++) {
                if ((w0 | w1 | w2) >= 0) {
                    bool pass_depth = true;
                    if constexpr (DEPTH_TEST) pass_depth = (cur_inv_w > local_depth[local_idx]);

                    if (pass_depth) {
                        int tr = std::max(0, std::min(255, r_fixed >> 16));
                        int tg = std::max(0, std::min(255, g_fixed >> 16));
                        int tb = std::max(0, std::min(255, b_fixed >> 16));
                        int ta = std::max(0, std::min(255, a_fixed >> 16));
                        
                        if constexpr (TEXTURED) {
                            int tx = std::max(0, std::min((int)tri.tex_width - 1, u_fixed >> 16));
                            int ty = std::max(0, std::min((int)tri.tex_height - 1, v_fixed >> 16));
                            uint32_t texel = tri.texture_ptr[ty * tri.tex_width + tx];
                            
                            tr = (tr * ((texel >> 16) & 0xFF)) >> 8; 
                            tg = (tg * ((texel >> 8) & 0xFF)) >> 8;
                            tb = (tb * (texel & 0xFF)) >> 8;
                            ta = (ta * ((texel >> 24) & 0xFF)) >> 8;
                        }

                        if constexpr (LIST_TYPE == RenderListType::PUNCH_THROUGH_LIST) {
                            if (ta >= 128) {
                                if constexpr (DEPTH_TEST) local_depth[local_idx] = cur_inv_w;
                                local_color[local_idx] = (255 << 24) | (tr << 16) | (tg << 8) | tb;
                            }
                        } else {
                            if constexpr (LIST_TYPE != RenderListType::TRANSLUCENT_LIST) {
                                if constexpr (DEPTH_TEST) local_depth[local_idx] = cur_inv_w;
                            }
                            if constexpr (LIST_TYPE == RenderListType::TRANSLUCENT_LIST) {
                                uint32_t bg = local_color[local_idx];
                                tr = (tr * ta + ((bg >> 16) & 0xFF) * (255 - ta)) >> 8;
                                tg = (tg * ta + ((bg >> 8) & 0xFF) * (255 - ta)) >> 8;
                                tb = (tb * ta + (bg & 0xFF) * (255 - ta)) >> 8;
                            }
                            local_color[local_idx] = (255 << 24) | (tr << 16) | (tg << 8) | tb;
                        }
                    }
                }
                
                if constexpr (TEXTURED) { u_fixed += u_step_fixed; v_fixed += v_step_fixed; }
                r_fixed += r_step_fixed; g_fixed += g_step_fixed; 
                b_fixed += b_step_fixed; a_fixed += a_step_fixed;
                
                cur_inv_w += inv_w_step_span;
                w0 += stepX_w0; w1 += stepX_w1; w2 += stepX_w2;
                local_idx++;
            }
            x += span_len;
        }
        row_w0 += stepY_w0; row_w1 += stepY_w1; row_w2 += stepY_w2;
    }
}

template <bool TEXTURED, bool DEPTH_TEST>
void SpheroidGPU::dispatch_list_type(const BinnedTriangle& tri, int min_x, int min_y, int max_x, int max_y, RenderListType list_type, uint32_t* local_color, float* local_depth) {
    switch (list_type) {
        case RenderListType::OPAQUE_LIST: rasterize_binned_impl<TEXTURED, DEPTH_TEST, RenderListType::OPAQUE_LIST>(tri, min_x, min_y, max_x, max_y, local_color, local_depth); break;
        case RenderListType::PUNCH_THROUGH_LIST: rasterize_binned_impl<TEXTURED, DEPTH_TEST, RenderListType::PUNCH_THROUGH_LIST>(tri, min_x, min_y, max_x, max_y, local_color, local_depth); break;
        case RenderListType::TRANSLUCENT_LIST: rasterize_binned_impl<TEXTURED, DEPTH_TEST, RenderListType::TRANSLUCENT_LIST>(tri, min_x, min_y, max_x, max_y, local_color, local_depth); break;
    }
}

void SpheroidGPU::rasterize_binned(const BinnedTriangle& tri, int min_x, int min_y, int max_x, int max_y, RenderListType list_type, uint32_t* local_color, float* local_depth) {
    if (tri.texturing_enabled && tri.texture_ptr) {
        if (tri.depth_test_enabled) dispatch_list_type<true, true>(tri, min_x, min_y, max_x, max_y, list_type, local_color, local_depth);
        else                        dispatch_list_type<true, false>(tri, min_x, min_y, max_x, max_y, list_type, local_color, local_depth);
    } else {
        if (tri.depth_test_enabled) dispatch_list_type<false, true>(tri, min_x, min_y, max_x, max_y, list_type, local_color, local_depth);
        else                        dispatch_list_type<false, false>(tri, min_x, min_y, max_x, max_y, list_type, local_color, local_depth);
    }
}

void SpheroidGPU::process_tiles_loop() {
    int total_tiles = num_tiles_x * num_tiles_y;
    uint32_t local_color[TILE_SIZE * TILE_SIZE];
    float local_depth[TILE_SIZE * TILE_SIZE];

    while (true) {
        int tile_idx = current_tile.fetch_add(1);
        if (tile_idx >= total_tiles) break;

        int tx = tile_idx % num_tiles_x;
        int ty = tile_idx / num_tiles_x;
        int tile_min_x = tx * TILE_SIZE;
        int tile_min_y = ty * TILE_SIZE;
        int tile_max_x = std::min((int)width - 1, tile_min_x + TILE_SIZE - 1);
        int tile_max_y = std::min((int)height - 1, tile_min_y + TILE_SIZE - 1);

        for (int i = 0; i < TILE_SIZE * TILE_SIZE; i++) {
            local_color[i] = pending_clear ? clear_color_val : 0xFF000000;
            local_depth[i] = pending_clear ? clear_depth_val : -1.0f;
        }

        uint32_t curr = tiles[tile_idx].opaque_head;
        while (curr != 0xFFFFFFFF) {
            rasterize_binned(triangle_pool[node_arena[curr].tri_idx], tile_min_x, tile_min_y, tile_max_x, tile_max_y, RenderListType::OPAQUE_LIST, local_color, local_depth);
            curr = node_arena[curr].next;
        }
        
        curr = tiles[tile_idx].punchthrough_head;
        while (curr != 0xFFFFFFFF) {
            rasterize_binned(triangle_pool[node_arena[curr].tri_idx], tile_min_x, tile_min_y, tile_max_x, tile_max_y, RenderListType::PUNCH_THROUGH_LIST, local_color, local_depth);
            curr = node_arena[curr].next;
        }

        std::vector<uint32_t> sort_buf;
        curr = tiles[tile_idx].translucent_head;
        while (curr != 0xFFFFFFFF) {
            sort_buf.push_back(node_arena[curr].tri_idx);
            curr = node_arena[curr].next;
        }

        std::sort(sort_buf.begin(), sort_buf.end(), [this](uint32_t a_idx, uint32_t b_idx) {
            const BinnedTriangle& a = triangle_pool[a_idx]; const BinnedTriangle& b = triangle_pool[b_idx];
            float a_w = screen_vertex_pool[a.v0_idx].inv_w + screen_vertex_pool[a.v1_idx].inv_w + screen_vertex_pool[a.v2_idx].inv_w;
            float b_w = screen_vertex_pool[b.v0_idx].inv_w + screen_vertex_pool[b.v1_idx].inv_w + screen_vertex_pool[b.v2_idx].inv_w;
            return a_w < b_w;
        });

        for (uint32_t idx : sort_buf) {
            rasterize_binned(triangle_pool[idx], tile_min_x, tile_min_y, tile_max_x, tile_max_y, RenderListType::TRANSLUCENT_LIST, local_color, local_depth);
        }

        int copy_width = tile_max_x - tile_min_x + 1;
        for (int y = 0; y <= (tile_max_y - tile_min_y); y++) {
            std::memcpy(&color_buffer[(tile_min_y + y) * width + tile_min_x], &local_color[y * TILE_SIZE], copy_width * sizeof(uint32_t));
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
        cv_start.wait(lock, [this, total_tiles]() { return stop_threads.load() || (current_tile.load() < total_tiles); });
        if (stop_threads.load()) break;
        lock.unlock();
        process_tiles_loop();
    }
}

void SpheroidGPU::flush_tiles() {
    current_tile.store(0); completed_tiles.store(0);
    int total = num_tiles_x * num_tiles_y;
    cv_start.notify_all();
    process_tiles_loop();
    std::unique_lock<std::mutex> lock(cv_m);
    cv_done.wait(lock, [this, total] { return completed_tiles.load() == total; });
    pending_clear = false;
}