#pragma once
#include <cstdint>
#include <vector>
#include "HandmadeMath.h"

#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

enum class RenderListType {
    OPAQUE_LIST = 0,
    PUNCH_THROUGH_LIST = 1,
    TRANSLUCENT_LIST = 2
};

struct GPUVertex { 
    float x, y, z; 
    float u, v; 
    uint8_t r, g, b, a; 
};

struct ScreenVertex {
    float x, y, z;
    float inv_w;
    float r, g, b, a; 
    float u, v;       
};

struct ClipVertex {
    HMM_Vec4 pos;
    float u, v;
    float r, g, b, a; 
};

// --- OPTIMIZATION: BinnedTriangle is now extremely compact (approx 32 bytes) ---
// It stores indices instead of full 40-byte vertex copies, massively improving cache hits.
struct BinnedTriangle {
    uint32_t v0_idx, v1_idx, v2_idx;
    int16_t minX, minY, maxX, maxY;
    
    bool texturing_enabled;
    bool depth_test_enabled;
    uint16_t tex_width, tex_height;
    uint32_t* texture_ptr;
};

struct TriNode {
    uint32_t tri_idx;
    uint32_t next; 
};

struct Tile {
    uint32_t opaque_head = 0xFFFFFFFF;
    uint32_t punchthrough_head = 0xFFFFFFFF;
    uint32_t translucent_head = 0xFFFFFFFF;
};

struct GPUState {
    bool depth_test_enabled = true;
    bool backface_culling = true;
    uint32_t* texture_ptr = nullptr;
    uint32_t tex_width = 0;
    uint32_t tex_height = 0;
    bool texturing_enabled = false;
    RenderListType current_render_list = RenderListType::OPAQUE_LIST;
    HMM_Mat4 projection_matrix = HMM_M4D(1.0f); 
    HMM_Mat4 modelview_matrix = HMM_M4D(1.0f);  
    std::vector<HMM_Mat4> matrix_stack;         
};

class SpheroidGPU {
private:
    uint32_t width, height;
    uint32_t* color_buffer;
    float* depth_buffer;
    uint8_t* system_ram;
    GPUState state;
    
    static const int TILE_SIZE = 32; 
    int num_tiles_x, num_tiles_y;
    std::vector<Tile> tiles;
    
    // --- NEW: Global pool for generated screen vertices ---
    std::vector<ScreenVertex> screen_vertex_pool;
    std::vector<BinnedTriangle> triangle_pool;
    std::vector<TriNode> node_arena;
    uint32_t node_count = 0;

    std::vector<ClipVertex> vertex_cache;

    bool pending_clear;
    uint32_t clear_color_val;
    float clear_depth_val;

    std::vector<std::thread> workers;
    std::atomic<int> current_tile;
    std::atomic<int> completed_tiles;
    std::atomic<bool> stop_threads;
    std::mutex cv_m;
    std::condition_variable cv_start;
    std::condition_variable cv_done; 

    void process_clip_triangle(const ClipVertex& v0, const ClipVertex& v1, const ClipVertex& v2);

    // --- OPTIMIZATION: bin_triangle now takes indices to save memory bandwidth ---
    void bin_triangle(uint32_t v0_idx, uint32_t v1_idx, uint32_t v2_idx);
    void flush_tiles();
    
    template <bool TEXTURED, bool DEPTH_TEST, RenderListType LIST_TYPE>
    void rasterize_binned_impl(const BinnedTriangle& tri, int tile_min_x, int tile_min_y, int tile_max_x, int tile_max_y, uint32_t* local_color, float* local_depth);
    
    template <bool TEXTURED, bool DEPTH_TEST>
    void dispatch_list_type(const BinnedTriangle& tri, int min_x, int min_y, int max_x, int max_y, RenderListType list_type, uint32_t* local_color, float* local_depth);
    
    void rasterize_binned(const BinnedTriangle& tri, int tile_min_x, int tile_min_y, int tile_max_x, int tile_max_y, RenderListType list_type, uint32_t* local_color, float* local_depth);

    void worker_thread();
    void process_tiles_loop();
    
public:
    SpheroidGPU();
    ~SpheroidGPU();

    void init(uint32_t w, uint32_t h);
    void shutdown();
    void set_ram_pointer(uint8_t* ram);
    const uint32_t* get_framebuffer() const;
    
    void execute_display_list(uint32_t ram_offset);
};