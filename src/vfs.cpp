#include "vfs.hpp"
#include <cstring>

// =============================================================================
// Libretro VFS File Implementation
// =============================================================================

LibretroVFSFile::LibretroVFSFile(struct retro_vfs_interface* vfs, const char* path, unsigned access, unsigned hints) {
    vfs_cb = vfs;
    if (vfs_cb && vfs_cb->open) {
        handle = vfs_cb->open(path, access, hints);
    }
}

LibretroVFSFile::~LibretroVFSFile() {
    if (handle && vfs_cb && vfs_cb->close) {
        vfs_cb->close(handle);
        handle = nullptr;
    }
}

int64_t LibretroVFSFile::read(void* buffer, uint64_t len) {
    if (!handle || !vfs_cb || !vfs_cb->read) return -1;
    return vfs_cb->read(handle, buffer, len);
}

int64_t LibretroVFSFile::write(const void* buffer, uint64_t len) {
    if (!handle || !vfs_cb || !vfs_cb->write) return -1;
    return vfs_cb->write(handle, buffer, len);
}

int64_t LibretroVFSFile::seek(int64_t offset, int whence) {
    if (!handle || !vfs_cb || !vfs_cb->seek) return -1;
    
    int vfs_whence = RETRO_VFS_SEEK_POSITION_START;
    if (whence == 1) vfs_whence = RETRO_VFS_SEEK_POSITION_CURRENT;
    else if (whence == 2) vfs_whence = RETRO_VFS_SEEK_POSITION_END;

    return vfs_cb->seek(handle, offset, vfs_whence);
}

int64_t LibretroVFSFile::tell() {
    if (!handle || !vfs_cb || !vfs_cb->tell) return -1;
    return vfs_cb->tell(handle);
}

// =============================================================================
// VFS Manager Implementation
// =============================================================================

void VFSManager::init(struct retro_vfs_interface* vfs, const char* save_directory) {
    vfs_cb = vfs;
    if (save_directory) {
        save_dir = save_directory;
        // Ensure trailing slash
        if (!save_dir.empty() && save_dir.back() != '/' && save_dir.back() != '\\') {
            save_dir += "/";
        }
    }
}

void VFSManager::mount_game(const char* game_path) {
    if (!game_path) return;
    
    std::string path_str = game_path;
    size_t last_slash = path_str.find_last_of("/\\");
    
    if (last_slash != std::string::npos) {
        game_dir = path_str.substr(0, last_slash + 1);
    } else {
        game_dir = ""; // Fallback
    }
}

int VFSManager::open(const char* path, const char* mode) {
    if (!path || !mode || !vfs_cb) return -1;

    // 1. Find free slot
    int fd = -1;
    for (int i = 0; i < MAX_FILES; i++) {
        if (!open_files[i]) {
            fd = i;
            break;
        }
    }
    if (fd == -1) return -1; // Max files reached

    std::string full_path;
    std::string req_path = path;
    unsigned access = 0;
    
    // 2. Parse mode & route mounts
    if (req_path.rfind("save://", 0) == 0) {
        // --- SAVE DATA MOUNT ---
        full_path = save_dir + req_path.substr(7);
        
        // Parse permissions for saves
        if (strchr(mode, 'r')) access |= RETRO_VFS_FILE_ACCESS_READ;
        if (strchr(mode, 'w')) access |= RETRO_VFS_FILE_ACCESS_WRITE;
        if (strchr(mode, '+')) {
            access |= RETRO_VFS_FILE_ACCESS_READ | RETRO_VFS_FILE_ACCESS_WRITE;
            access |= RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING;
        }
    } else {
        // --- GAME ASSET MOUNT ---
        full_path = game_dir + req_path;
        
        // Strictly deny write access to game folder!
        if (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+')) {
            return -1; // Unauthorized write attempt
        }
        access = RETRO_VFS_FILE_ACCESS_READ;
    }

    // 3. Open via Libretro VFS
    unsigned hints = RETRO_VFS_FILE_ACCESS_HINT_NONE;
    LibretroVFSFile* file = new LibretroVFSFile(vfs_cb, full_path.c_str(), access, hints);
    
    if (file->is_valid()) {
        open_files[fd] = file;
        return fd;
    } else {
        delete file;
        return -1;
    }
}

int64_t VFSManager::read(int fd, void* buffer, uint64_t len) {
    if (fd >= 0 && fd < MAX_FILES && open_files[fd]) {
        return open_files[fd]->read(buffer, len);
    }
    return -1;
}

int64_t VFSManager::write(int fd, const void* buffer, uint64_t len) {
    if (fd >= 0 && fd < MAX_FILES && open_files[fd]) {
        return open_files[fd]->write(buffer, len);
    }
    return -1;
}

int64_t VFSManager::seek(int fd, int64_t offset, int whence) {
    if (fd >= 0 && fd < MAX_FILES && open_files[fd]) {
        return open_files[fd]->seek(offset, whence);
    }
    return -1;
}

int64_t VFSManager::tell(int fd) {
    if (fd >= 0 && fd < MAX_FILES && open_files[fd]) {
        return open_files[fd]->tell();
    }
    return -1;
}

void VFSManager::close(int fd) {
    if (fd >= 0 && fd < MAX_FILES && open_files[fd]) {
        delete open_files[fd];
        open_files[fd] = nullptr;
    }
}

void VFSManager::close_all() {
    for (int i = 0; i < MAX_FILES; i++) {
        close(i);
    }
}