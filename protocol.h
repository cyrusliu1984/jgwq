#ifndef __PROTOCOL_H__
#define __PROTOCOL_H__

#include <stdint.h>

// ==================== 外设 ID 定义 ====================
#define ID_FOCUS        0x01        // 调焦电机
#define ID_RANGER       0x02        // 激光测距机
#define ID_ILLUM        0x04        // 激光照明器
#define ID_THP          0x08        // 温湿压模块
#define ID_LASER        0x10        // 激光电源控制器

// ==================== TCP 通讯常量 ====================
#define TCP_HEAD_DOWN   0x7E7F      
#define TCP_TAIL_DOWN   0xAA55      
#define TCP_HEAD_UP     0x7F7E      
#define TCP_TAIL_UP     0x55AA      

// ==================== 串口协议常量 ====================
#define HEAD_RANGER         0x55
#define HEAD_LASER          0xA5
#define HEAD_ILLUM_QUERY    0x55AA
#define HEAD_ILLUM_RESP     0xAA55

#define FOCUS_MODBUS_ADDR   0x01

#pragma pack(1)
// 各外设实时状态缓存结构体
typedef struct {
    uint8_t  mode; uint8_t  en; uint16_t set_pos; uint16_t set_spd;
    uint16_t ctrl; uint16_t act_pos;
} FocusState_t;

typedef struct {
    uint8_t  apd; uint8_t  state; uint32_t dist; 
    uint8_t  status; uint8_t  self;
} RangerState_t;

typedef struct {
    uint8_t  state; uint8_t  fault; uint8_t  t_int; uint8_t  t_dec;
} IllumState_t;

typedef struct {
    uint16_t temp; uint16_t humi; uint16_t press;
} ThpState_t;

typedef struct {
    uint16_t vin; uint16_t vout; uint16_t iout; int16_t  temp; uint8_t  fault;
} LaserState_t;

// TCP 帧映射结构
typedef struct {
    uint16_t head;
    uint8_t  len;
    uint8_t  id;
    uint8_t  data[32];
} TcpFrame_t;
#pragma pack()

#endif
