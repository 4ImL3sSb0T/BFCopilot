/*
 * 二维/三维向量基础运算。全部按值传参/返回，适合在控制环里做短链式计算，
 * 不分配内存、不引入依赖。
 */

#include "vec_math.h"
#include <math.h>

vec2f vec2f_add(vec2f a, vec2f b) {
    return vec2f_make(a.x + b.x, a.y + b.y);
}

vec2f vec2f_sub(vec2f a, vec2f b) {
    return vec2f_make(a.x - b.x, a.y - b.y);
}

vec2f vec2f_scale(vec2f v, f32 scalar) {
    return vec2f_make(v.x * scalar, v.y * scalar);
}

vec2f vec2f_negate(vec2f v) {
    return vec2f_make(-v.x, -v.y);
}

f32 vec2f_dot(vec2f a, vec2f b) {
    return a.x * b.x + a.y * b.y;
}

f32 vec2f_cross(vec2f a, vec2f b) {
    return a.x * b.y - a.y * b.x;
}

f32 vec2f_length_squared(vec2f v) {
    return vec2f_dot(v, v);
}

f32 vec2f_length(vec2f v) {
    return sqrtf(vec2f_length_squared(v));
}

vec2f vec2f_normalize(vec2f v) {
    const f32 length = vec2f_length(v);
    // 零向量没有方向，原样返回，避免除零产生 NaN 传到控制环
    if (length <= 0.0f) {
        return v;
    }
    return vec2f_scale(v, 1.0f / length);
}

vec3f vec3f_add(vec3f a, vec3f b) {
    return vec3f_make(a.x + b.x, a.y + b.y, a.z + b.z);
}

vec3f vec3f_sub(vec3f a, vec3f b) {
    return vec3f_make(a.x - b.x, a.y - b.y, a.z - b.z);
}

vec3f vec3f_scale(vec3f v, f32 scalar) {
    return vec3f_make(v.x * scalar, v.y * scalar, v.z * scalar);
}

vec3f vec3f_negate(vec3f v) {
    return vec3f_make(-v.x, -v.y, -v.z);
}

f32 vec3f_dot(vec3f a, vec3f b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

vec3f vec3f_cross(vec3f a, vec3f b) {
    return vec3f_make(a.y * b.z - a.z * b.y,
                      a.z * b.x - a.x * b.z,
                      a.x * b.y - a.y * b.x);
}

f32 vec3f_length_squared(vec3f v) {
    return vec3f_dot(v, v);
}

f32 vec3f_length(vec3f v) {
    return sqrtf(vec3f_length_squared(v));
}

vec3f vec3f_normalize(vec3f v) {
    const f32 length = vec3f_length(v);
    // 零向量没有方向，原样返回，避免除零产生 NaN 传到控制环
    if (length <= 0.0f) {
        return v;
    }
    return vec3f_scale(v, 1.0f / length);
}
