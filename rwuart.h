#pragma once
#include <stdint.h>
#include <sys/types.h>

#define UART_BUF_SIZE 1024

typedef struct {
    const char *dev;
    int fd;
} UartDevice_t;

int     Uart_Init(const char *dev, int baud);
void*   Uart_MonitorThread(void *arg);
void    Uart_485_GpioInit(void);
ssize_t Uart_Send485(int fd, int gpio_fd, const uint8_t *data, size_t len, const char* tag);
void    Uart_DumpHex(const char* tag, const uint8_t* buf, int len);
