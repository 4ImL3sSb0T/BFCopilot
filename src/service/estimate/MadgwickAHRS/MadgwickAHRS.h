//=====================================================================================================
// MadgwickAHRS.h
//=====================================================================================================
//
// Madgwick的IMU和AHRS算法的实现。
// 详见：http://www.x-io.co.uk/node/8#open_source_ahrs_and_imu_algorithms
//
// 日期			作者          		备注
// 29/09/2011	SOH Madgwick    初始版本
// 02/10/2011	SOH Madgwick	为减少CPU负载进行了优化
//
//=====================================================================================================
#ifndef MadgwickAHRS_h
#define MadgwickAHRS_h

#ifdef __cplusplus
extern "C" {
#endif

//----------------------------------------------------------------------------------------------------
// 变量声明

extern volatile float beta;				// 算法增益
extern volatile float q0, q1, q2, q3;	// 表示传感器坐标系相对于辅助坐标系的四元数

//---------------------------------------------------------------------------------------------------
// 函数声明

void MadgwickAHRSupdate(float gx, float gy, float gz, float ax, float ay, float az, float mx, float my, float mz);
void MadgwickAHRSupdateIMU(float gx, float gy, float gz, float ax, float ay, float az);
void MadgwickAHRS_getEuler(float* roll, float* pitch, float* yaw);

#endif
//=====================================================================================================
// 文件结束
//=====================================================================================================
#ifdef __cplusplus
}
#endif