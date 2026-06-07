#pragma once

#include <cstdint>
#include <string>
#include "libretro.h"

// =============================================================================
// Abstract VFS File Interface
// =============================================================================
class VFSFile {
public:
    virtual ~VFSFile() = default;
    
    virtual int64_t read(void* buffer, uint64_t len) = 0;
    virtual int64_t write(const void* buffer, uint64_t len) = 0;
    virtual int64_t seek(int64_t offset, int whence) = 0;
    virtual int64_t tell() = 0;
};

// =============================================================================
// Libretro VFS Implementation (Open Folders / Extracted Zips)
// =============================================================================
class LibretroVFSFile : public VFSFile {
private:
    struct retro_vfs_file_handle* handle = nullptr;
    struct retro_vfs_interface* vfs_cb = nullptr;

public:
    LibretroVFSFile(struct retro_vfs_interface* vfs, const char* path, unsigned access, unsigned hints);
    ~LibretroVFSFile() override;

    int64_t read(void* buffer, uint64_t len) override;
    int64_t write(const void* buffer, uint64_t len) override;
    int64_t seek(int64_t offset, int whence) override;
    int64_t tell() override;
    
    bool is_valid() const { return handle != nullptr; }
};

// =============================================================================
// VFS Router / Manager
// =============================================================================
class VFSManager {
private:
    static constexpr int MAX_FILES = 16;
    VFSFile* open_files[MAX_FILES] = {nullptr};
    
    struct retro_vfs_interface* vfs_cb = nullptr;
    
    std::string game_dir;
    std::string save_dir;

public:
    VFSManager() = default;
    ~VFSManager() { close_all(); }

    // Core lifecycle
    void init(struct retro_vfs_interface* vfs, const char* save_directory);
    void mount_game(const char* game_path); 

    // API (returns file descriptor, or -1 on error)
    int open(const char* path, const char* mode);
    int64_t read(int fd, void* buffer, uint64_t len);
    int64_t write(int fd, const void* buffer, uint64_t len);
    int64_t seek(int fd, int64_t offset, int whence);
    int64_t tell(int fd);
    void close(int fd);
    void close_all();
};