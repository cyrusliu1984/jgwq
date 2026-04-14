#pragma once
#include <stdint.h>

void* Tcp_ServerThread(void *arg);
void  Tcp_ReportStatus(uint8_t id);

// 自动初始化与心跳逻辑
void Focus_AutoHoming(int fd);
void Ranger_AutoInit(int fd);
void Thp_AutoInit(int fd);
void Illum_HeartbeatQuery(int fd);
