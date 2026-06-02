#pragma once
#include <vector>
#include <cstdint>
#include <cmath>

struct Vec3 { float x, y, z; };
struct Color { uint8_t r, g, b; };

// The vertex format stored in emulated RAM
struct GPUVertex { 
    float x, y, z; 
    uint8_t r, g, b, a; 
};

struct Mesh {
    std::vector<Vec3> vertices;
    std::vector<Color> colors;
    std::vector<int> indices;
};

struct Mat4 {
    float m[4][4] = {0};
    Mat4 operator*(const Mat4& right) const {
        Mat4 res;
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) {
                res.m[r][c] = m[r][0] * right.m[0][c] + m[r][1] * right.m[1][c] +
                              m[r][2] * right.m[2][c] + m[r][3] * right.m[3][c];
            }
        }
        return res;
    }
};

inline Mat4 mat4_identity() {
    Mat4 res; res.m[0][0] = 1; res.m[1][1] = 1; res.m[2][2] = 1; res.m[3][3] = 1; return res;
}

inline Mat4 mat4_rotation(float rotX, float rotY, float rotZ) {
    Mat4 rx = mat4_identity(), ry = mat4_identity(), rz = mat4_identity();
    rx.m[1][1] = cosf(rotX); rx.m[1][2] = -sinf(rotX); rx.m[2][1] = sinf(rotX); rx.m[2][2] =  cosf(rotX);
    ry.m[0][0] = cosf(rotY); ry.m[0][2] =  sinf(rotY); ry.m[2][0] = -sinf(rotY); ry.m[2][2] = cosf(rotY);
    rz.m[0][0] = cosf(rotZ); rz.m[0][1] = -sinf(rotZ); rz.m[1][0] = sinf(rotZ); rz.m[1][1] =  cosf(rotZ);
    return rz * ry * rx;
}

inline Mat4 mat4_translation(float x, float y, float z) {
    Mat4 res = mat4_identity();
    res.m[0][3] = x; res.m[1][3] = y; res.m[2][3] = z;
    return res;
}

inline Mat4 mat4_scale(float s) {
    Mat4 res = mat4_identity();
    res.m[0][0] = s; 
    res.m[1][1] = s; 
    res.m[2][2] = s;
    return res;
}

inline Vec3 transform_vec3(const Mat4& mat, const Vec3& v) {
    return {
        v.x * mat.m[0][0] + v.y * mat.m[0][1] + v.z * mat.m[0][2] + mat.m[0][3],
        v.x * mat.m[1][0] + v.y * mat.m[1][1] + v.z * mat.m[1][2] + mat.m[1][3],
        v.x * mat.m[2][0] + v.y * mat.m[2][1] + v.z * mat.m[2][2] + mat.m[2][3]
    };
}