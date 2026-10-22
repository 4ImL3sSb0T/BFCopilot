/*
 * 三维向量基础运算。全部按值传参/返回，适合在控制环里做短链式计算，
 * 不分配内存、不引入依赖。
 */

#include "vec_math.h"
#include <math.h>

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
