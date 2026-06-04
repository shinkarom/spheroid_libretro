#pragma once
#include <cstdint>
#include <vector>
#include "HandmadeMath.h"

#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

// The vertex format stored in emulated RAM
struct GPUVertex { 
    float x, y, z; 
    float u, v; 
    uint8_t r, g, b, a; 
};

struct ScreenVertex {
    float x, y, z;
    float inv_w;
    float r, g, b;
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
    std::vector<BinnedTriangle> triangles;
};

struct GPUState {
    bool depth_test_enabled = true;
    bool backface_culling = true;
    
    uint32_t* texture_ptr = nullptr;
    uint32_t tex_width = 0;
    uint32_t tex_height = 0;
    bool texturing_enabled = false;
    
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
    
    static const int TILE_SIZE = 64;
    int num_tiles_x, num_tiles_y;
    std::vector<Tile> tiles;

    // Deferred clear state (for TBDR-style tile clearing)
    bool pending_clear;
    uint32_t clear_color_val;
    float clear_depth_val;

    // Threading sync
    std::vector<std::thread> workers;
    std::atomic<int> current_tile;
    std::atomic<int> completed_tiles;
    std::atomic<bool> stop_threads;
    std::mutex cv_m;
    std::condition_variable cv_start;
    std::condition_variable cv_done; // NEW: Allows main thread to sleep

    void bin_triangle(const ScreenVertex& v0, const ScreenVertex& v1, const ScreenVertex& v2);
    void flush_tiles();
    
    // Templated rasterizer to eliminate inner-loop branching
    template <bool TEXTURED, bool DEPTH_TEST>
    void rasterize_binned_impl(const BinnedTriangle& tri, int tile_min_x, int tile_min_y, int tile_max_x, int tile_max_y);
    
    void rasterize_binned(const BinnedTriangle& tri, int tile_min_x, int tile_min_y, int tile_max_x, int tile_max_y);
    void rasterize_triangle(const ScreenVertex& v0, const ScreenVertex& v1, const ScreenVertex& v2);

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