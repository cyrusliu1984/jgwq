#include "rwuart.h"
#include "protocol.h"
#include "rwtcpserver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>

static int g_gpio_motor = -1;
static int g_gpio_thp   = -1;

// 外部状态引用
extern FocusState_t  g_focus;
extern RangerState_t g_ranger;
extern IllumState_t  g_illum;
extern ThpState_t    g_thp;
extern LaserState_t  g_laser;

// 调试辅助：打印十六进制数据
void Uart_DumpHex(const char* tag, const uint8_t* buf, int len) {
    printf("[DEBUG][UART][%s] ", tag);
    for(int i=0; i<len; i++) printf("%02X ", buf[i]);
    printf("\n");
}

void Uart_485_GpioInit(void) {
    g_gpio_motor = open("/sys/class/gpio/gpio37/value", O_RDWR);
    g_gpio_thp   = open("/sys/class/gpio/gpio42/value", O_RDWR);
}

int Uart_Init(const char *dev, int baud) {
    int fd = open(dev, O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd < 0) return -1;
    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) return -1;
    speed_t speed = (baud == 9600) ? B9600 : B115200;
    cfsetospeed(&tty, speed); cfsetispeed(&tty, speed);
    tty.c_cflag |= (CLOCAL | CREAD | CS8);
    tty.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_oflag &= ~OPOST;
    tty.c_cc[VMIN] = 0; tty.c_cc[VTIME] = 1;
    tcsetattr(fd, TCSANOW, &tty);
    return fd;
}

ssize_t Uart_Send485(int fd, int gpio_fd, const uint8_t *data, size_t len, const char* tag) {
    if (fd < 0) return -1;
    if (gpio_fd >= 0) { write(gpio_fd, "1", 1); usleep(500); }
    Uart_DumpHex(tag, data, (int)len);
    ssize_t ret = write(fd, data, len);
    tcdrain(fd);
    if (gpio_fd >= 0) { usleep(500); write(gpio_fd, "0", 1); }
    return ret;
}

// --- [外设私有解析函数] ---

static void Parse_Focus(uint8_t *buf, int len) {
    if (len >= 7 && buf[0] == 0x01 && buf[1] == 0x03 && buf[2] == 0x04) {
        g_focus.act_pos = (buf[3] << 8) | buf[4];
        printf("[System] Focus Pos Updated: %d\n", g_focus.act_pos);
        Tcp_ReportStatus(ID_FOCUS);
    }
}

static void Parse_Ranger(uint8_t *buf, int len) {
    if (len < 6 || buf[0] != HEAD_RANGER) return;
    uint8_t x = 0; for(int i=0; i<len-1; i++) x ^= buf[i];
    if (x != buf[len-1]) return;
    if (len >= 14) {
        g_ranger.status = buf[3];
        g_ranger.dist = (buf[4] << 16) | (buf[5] << 8) | buf[6];
        printf("[System] Ranger Distance: %d\n", g_ranger.dist);
        Tcp_ReportStatus(ID_RANGER);
    }
}

static void Parse_Illum(uint8_t *buf, int len) {
    if (len < 13 || buf[0] != 0xAA || buf[1] != 0x55) return;
    if (buf[7] == 0x01 && len >= 14) {
        g_illum.state = buf[9]; g_illum.fault = buf[10];
        Tcp_ReportStatus(ID_ILLUM);
    } else if (buf[9] == 0x10 && len >= 14) {
        g_illum.t_int = buf[10]; g_illum.t_dec = buf[11];
        Tcp_ReportStatus(ID_ILLUM);
    }
}

static void Parse_Thp(uint8_t *buf, int len) {
    if (len >= 11 && buf[0] == 0x01 && buf[1] == 0x03) {
        g_thp.temp = (buf[3]<<8)|buf[4]; g_thp.humi = (buf[5]<<8)|buf[6]; g_thp.press = (buf[7]<<8)|buf[8];
        Tcp_ReportStatus(ID_THP);
    }
}

static void Parse_Laser(uint8_t *buf, int len) {
    if (len >= 14 && buf[0] == HEAD_LASER && buf[2] == 0x03) {
        g_laser.vin = (buf[3]<<8)|buf[4]; g_laser.vout = (buf[5]<<8)|buf[6];
        g_laser.iout = (buf[7]<<8)|buf[8]; g_laser.temp = (int16_t)((buf[9]<<8)|buf[10]);
        g_laser.fault = buf[12];
        Tcp_ReportStatus(ID_LASER);
    }
}

void *Uart_MonitorThread(void *arg) {
    UartDevice_t *dev = (UartDevice_t *)arg;
    uint8_t buf[UART_BUF_SIZE];
    printf("[System] Monitor Thread Started for %s\n", dev->dev);
    while (1) {
        int n = read(dev->fd, buf, UART_BUF_SIZE);
        if (n > 0) {
            Uart_DumpHex(dev->dev, buf, n); // 每一包收到的原始串口数据都打印出来
            if (strstr(dev->dev, "ttyS9") || strstr(dev->dev, "USB1"))      Parse_Focus(buf, n);
            else if (strstr(dev->dev, "ttyS8"))                             Parse_Ranger(buf, n);
            else if (strstr(dev->dev, "ttyS7") || strstr(dev->dev, "USB2")) Parse_Illum(buf, n);
            else if (strstr(dev->dev, "ttyS4"))                             Parse_Thp(buf, n);
            else if (strstr(dev->dev, "ttyUSB0") || strstr(dev->dev, "ttyS3")) Parse_Laser(buf, n);
        }
        usleep(10000);
    }
    return NULL;
}
