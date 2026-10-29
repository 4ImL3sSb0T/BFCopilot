#ifndef _VEC_MATH_H_
#define _VEC_MATH_H_

#include "common_def.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 二维浮点向量
 *
 * 给只关心平面的量用：水平位置/速度、航向误差、地面投影等。
 * z 轴语义由上层决定，这里只做平面内的代数运算。
 */
typedef struct vec2f
{
    f32 x;
    f32 y;
} vec2f;

static inline vec2f vec2f_make(f32 x, f32 y) {
    vec2f v = {x, y};
    return v;
}

vec2f vec2f_add(vec2f a, vec2f b);
vec2f vec2f_sub(vec2f a, vec2f b);
vec2f vec2f_scale(vec2f v, f32 scalar);
vec2f vec2f_negate(vec2f v);

f32 vec2f_dot(vec2f a, vec2f b);
/// @brief 二维叉积的 z 分量，等价于 (a.x*b.y - a.y*b.x)
/// @note 结果为正是 b 在 a 逆时针一侧，是判断左右/绕向的常用手段
f32 vec2f_cross(vec2f a, vec2f b);

f32 vec2f_length_squared(vec2f v);
f32 vec2f_length(vec2f v);
vec2f vec2f_normalize(vec2f v);

static inline bool vec2f_is_zero(vec2f v) {
    return (v.x == 0.0f) && (v.y == 0.0f);
}

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
