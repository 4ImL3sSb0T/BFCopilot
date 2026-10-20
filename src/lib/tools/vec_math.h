#ifndef _VEC_MATH_H_
#define _VEC_MATH_H_

#include "common_def.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 三维浮点向量
 *
 * 在 BFCopilot 里主要用于 IMU 采样：加速度用 g、角速度用 °/s。
 * 坐标系跟随芯片（右手系，X/Y/Z 见 LSM6DS3TR-C 数据手册），
 * 机体系变换属于上层算法，不在这里做。
 */
typedef struct vec3f
{
    f32 x;
    f32 y;
    f32 z;
} vec3f;

static inline vec3f vec3f_make(f32 x, f32 y, f32 z) {
    vec3f v = {x, y, z};
    return v;
}

vec3f vec3f_add(vec3f a, vec3f b);
vec3f vec3f_sub(vec3f a, vec3f b);
vec3f vec3f_scale(vec3f v, f32 scalar);
vec3f vec3f_negate(vec3f v);

f32 vec3f_dot(vec3f a, vec3f b);
vec3f vec3f_cross(vec3f a, vec3f b);

f32 vec3f_length_squared(vec3f v);
f32 vec3f_length(vec3f v);
vec3f vec3f_normalize(vec3f v);

static inline bool vec3f_is_zero(vec3f v) {
    return (v.x == 0.0f) && (v.y == 0.0f) && (v.z == 0.0f);
}

#ifdef __cplusplus
}
#endif

#endif // _VEC_MATH_H_
