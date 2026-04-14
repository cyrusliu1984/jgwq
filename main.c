#include "protocol.h"
#include "rwuart.h"
#include "rwtcpserver.h"
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

// 全局文件描述符
int g_fd_focus = -1;
int g_fd_ranger = -1;
int g_fd_illum = -1;
int g_fd_thp = -1;
int g_fd_laser = -1;

// 照明器心跳定时线程
void *Heartbeat_TimerThread(void *arg) {
    while (1) {
        if (g_fd_illum >= 0) Illum_HeartbeatQuery(g_fd_illum);
        sleep(1);
    }
    return NULL;
}

int main(void) {
    printf("\n[System] RK3588 Multi-Peripheral Communication Gateway\n");
    printf("[System] Standard: Modular Architecture + Hex Debug Logs\n\n");

    // 1. 硬件 IO 初始化
    Uart_485_GpioInit();

    // 2. 串口设备定义
    UartDevice_t dev_focus  = {"/dev/ttyS9", -1}; 
    UartDevice_t dev_ranger = {"/dev/ttyS8", -1}; 
    UartDevice_t dev_illum  = {"/dev/ttyS7", -1}; 
    UartDevice_t dev_thp    = {"/dev/ttyS4", -1}; 
    UartDevice_t dev_laser  = {"/dev/ttyS3", -1};

    // 3. 串口初始化
    dev_focus.fd  = Uart_Init(dev_focus.dev, 115200);
    dev_ranger.fd = Uart_Init(dev_ranger.dev, 115200);
    dev_illum.fd  = Uart_Init(dev_illum.dev, 115200);
    dev_thp.fd    = Uart_Init(dev_thp.dev, 9600);
    dev_laser.fd  = Uart_Init(dev_laser.dev, 115200);

    g_fd_focus = dev_focus.fd;
    g_fd_ranger = dev_ranger.fd;
    g_fd_illum = dev_illum.fd;
    g_fd_thp = dev_thp.fd;
    g_fd_laser = dev_laser.fd;

    // 4. 启动核心服务线程
    pthread_t t_tcp, t_timer;
    pthread_create(&t_tcp, NULL, Tcp_ServerThread, NULL);
    pthread_create(&t_timer, NULL, Heartbeat_TimerThread, NULL);

    // 5. 启动串口监控线程 (每个串口独立线程)
    pthread_t m_focus, m_ranger, m_illum, m_thp, m_laser;
    if (g_fd_focus >= 0)  pthread_create(&m_focus, NULL, Uart_MonitorThread, &dev_focus);
    if (g_fd_ranger >= 0) pthread_create(&m_ranger, NULL, Uart_MonitorThread, &dev_ranger);
    if (g_fd_illum >= 0)  pthread_create(&m_illum, NULL, Uart_MonitorThread, &dev_illum);
    if (g_fd_thp >= 0)    pthread_create(&m_thp, NULL, Uart_MonitorThread, &dev_thp);
    if (g_fd_laser >= 0)  pthread_create(&m_laser, NULL, Uart_MonitorThread, &dev_laser);

    // 6. 执行硬件自动初始化
    usleep(100000); // 等待监控线程就绪
    if (g_fd_thp >= 0)    Thp_AutoInit(g_fd_thp);
    if (g_fd_ranger >= 0) Ranger_AutoInit(g_fd_ranger);
    if (g_fd_focus >= 0)  Focus_AutoHoming(g_fd_focus);

    printf("[System] All modules initialized. Gateway is online.\n");

    // 永远等待 TCP 线程
    pthread_join(t_tcp, NULL);

    return 0;
}
