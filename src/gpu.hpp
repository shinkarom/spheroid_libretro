#pragma once
#include <cstdint>
#include <vector>
#include "HandmadeMath.h"

#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

// --- Render Lists Enum ---
enum class RenderListType {
    OPAQUE_LIST = 0,
    PUNCH_THROUGH_LIST = 1,
    TRANSLUCENT_LIST = 2
};

// The vertex format stored in emulated RAM
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

struct BinnedTriangle {
    ScreenVertex v0, v1, v2;
    int minX, minY, maxX, maxY;
    
    bool texturing_enabled;
    uint32_t tex_width, tex_height;
    uint32_t* texture_ptr;
    bool depth_test_enabled;
};

struct Tile {
    // OPTIMIZATION 2: Hold integers (indices) instead of copying massive structs
    std::vector<uint32_t> opaque_indices;
    std::vector<uint32_t> punchthrough_indices;
    std::vector<uint32_t> translucent_indices;
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
    
    // OPTIMIZATION 3: Smaller 32x32 Tiles for better multithreading load balance
    static const int TILE_SIZE = 32; 
    int num_tiles_x, num_tiles_y;
    std::vector<Tile> tiles;
    
    // MASTER TRIANGLE POOL
    std::vector<BinnedTriangle> triangle_pool;

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

    void bin_triangle(const ScreenVertex& v0, const ScreenVertex& v1, const ScreenVertex& v2);
    void flush_tiles();
    
    // Templated rasterizer logic (Branchless inner loops)
    template <bool TEXTURED, bool DEPTH_TEST, RenderListType LIST_TYPE>
    void rasterize_binned_impl(const BinnedTriangle& tri, int tile_min_x, int tile_min_y, int tile_max_x, int tile_max_y);
    
    // Dispatchers
    template <bool TEXTURED, bool DEPTH_TEST>
    void dispatch_list_type(const BinnedTriangle& tri, int min_x, int min_y, int max_x, int max_y, RenderListType list_type);
    
    void rasterize_binned(const BinnedTriangle& tri, int tile_min_x, int tile_min_y, int tile_max_x, int tile_max_y, RenderListType list_type);

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