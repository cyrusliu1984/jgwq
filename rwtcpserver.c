#include "rwtcpserver.h"
#include "protocol.h"
#include "rwuart.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>

#define TCP_PORT 8001

// --- [全局句柄与状态引用] ---
extern int g_fd_focus; extern int g_fd_ranger; extern int g_fd_illum; extern int g_fd_thp; extern int g_fd_laser;
int g_client_fd = -1;
FocusState_t g_focus; RangerState_t g_ranger; IllumState_t g_illum; ThpState_t g_thp; LaserState_t g_laser;

// --- [TCP 接收环形缓冲区] ---
#define RING_BUF_SIZE 1024
static struct {
    uint8_t buffer[RING_BUF_SIZE];
    int head; int tail; int count;
    pthread_mutex_t lock; pthread_cond_t cond;
} g_q = { .head=0, .tail=0, .count=0, .lock=PTHREAD_MUTEX_INITIALIZER, .cond=PTHREAD_COND_INITIALIZER };

static void Q_Push(uint8_t d) {
    pthread_mutex_lock(&g_q.lock);
    if (g_q.count < RING_BUF_SIZE) { g_q.buffer[g_q.head] = d; g_q.head = (g_q.head + 1) % RING_BUF_SIZE; g_q.count++; pthread_cond_signal(&g_q.cond); }
    pthread_mutex_unlock(&g_q.lock);
}
static uint8_t Q_Peek(int offset) { return g_q.buffer[(g_q.tail + offset) % RING_BUF_SIZE]; }
static void Q_Pop(int n) { g_q.tail = (g_q.tail + n) % RING_BUF_SIZE; g_q.count -= n; }

// --- [调焦电机专项异步任务队列] ---
typedef struct { uint8_t data[10]; } FocusTask_t;
static struct {
    FocusTask_t queue[16];
    int head; int tail; int count;
    pthread_mutex_t lock; pthread_cond_t cond;
} g_focus_q = { .head=0, .tail=0, .count=0, .lock=PTHREAD_MUTEX_INITIALIZER, .cond=PTHREAD_COND_INITIALIZER };

static void Push_FocusTask(uint8_t *data) {
    pthread_mutex_lock(&g_focus_q.lock);
    if (g_focus_q.count < 16) { memcpy(g_focus_q.queue[g_focus_q.head].data, data, 10); g_focus_q.head = (g_focus_q.head + 1) % 16; g_focus_q.count++; pthread_cond_signal(&g_focus_q.cond); }
    pthread_mutex_unlock(&g_focus_q.lock);
}

// --- [协议计算工具] ---
static uint16_t Calc_ModbusCRC(uint8_t *data, uint16_t len) {
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) { if (crc & 1) crc = (crc >> 1) ^ 0xA001; else crc >>= 1; }
    }
    return crc;
}

// --- [底层指令封装] ---
static void Send_FocusWrite(int fd, uint16_t reg, uint16_t val) {
    if (fd < 0) return;
    uint8_t buf[8] = {FOCUS_MODBUS_ADDR, 0x06, (uint8_t)(reg>>8), (uint8_t)(reg&0xFF), (uint8_t)(val>>8), (uint8_t)(val&0xFF), 0, 0};
    uint16_t crc = Calc_ModbusCRC(buf, 6); buf[6] = crc & 0xFF; buf[7] = crc >> 8;
    Uart_Send485(fd, open("/sys/class/gpio/gpio37/value", O_RDWR), buf, 8, "FOCUS_OUT");
}

static void Send_RangerCmd(int fd, uint8_t cmd, uint8_t h, uint8_t l) {
    if (fd < 0) return;
    uint8_t buf[6] = {HEAD_RANGER, cmd, 0x02, h, l, 0};
    uint8_t x = 0; for(int i=0; i<5; i++) x ^= buf[i]; buf[5] = x;
    write(fd, buf, 6); Uart_DumpHex("RANGER_OUT", buf, 6);
}

static void Send_IllumPacket(int fd, uint8_t func, uint8_t vh, uint8_t vl) {
    if (fd < 0) return;
    uint8_t buf[14] = {0x55, 0xAA, 0, 0, 0, 0, 0x03, 0x02, 0x03, func, vh, vl, 0, 0};
    buf[12] = 0xFE; buf[13] = 0xFE;
    uint8_t s = 0; for(int i=3; i<=11; i++) s += buf[i]; buf[11] = s;
    write(fd, buf, 14); Uart_DumpHex("ILLUM_OUT", buf, 14);
}

static void Send_LaserPacket(int fd, uint8_t addr, uint8_t cmd, uint16_t v1, uint16_t v2) {
    if (fd < 0) return;
    uint8_t buf[8] = {HEAD_LASER, addr, cmd, (uint8_t)(v1>>8), (uint8_t)v1, (uint8_t)(v2>>8), (uint8_t)v2, 0};
    uint8_t s = 0; for(int i=0; i<7; i++) s += buf[i]; buf[7] = s;
    write(fd, buf, 8); Uart_DumpHex("LASER_OUT", buf, 8);
}

// --- [到位查询同步逻辑] ---
static int Focus_WaitUntilReady(int fd, uint16_t target, int timeout_ms) {
    if (fd < 0) return -1;
    int elapsed = 0;
    uint8_t q[] = {0x01, 0x03, 0x50, 0x00, 0x00, 0x02, 0xBD, 0x05};
    while (elapsed < timeout_ms) {
        Uart_Send485(fd, -1, q, 8, "FOCUS_QUERY");
        usleep(100000); elapsed += 100;
        int diff = abs((int)g_focus.act_pos - (int)target);
        if (diff <= 15) return 0;
    }
    return -1;
}

// --- [调焦电机异步运动引擎线程] ---
void *Focus_MotionEngineThread(void *arg) {
    uint8_t d[10];
    printf("[System] Focus Motion Engine Thread Started.\n");
    while (1) {
        pthread_mutex_lock(&g_focus_q.lock);
        while (g_focus_q.count == 0) pthread_cond_wait(&g_focus_q.cond, &g_focus_q.lock);
        memcpy(d, g_focus_q.queue[g_focus_q.tail].data, 10);
        g_focus_q.tail = (g_focus_q.tail + 1) % 16; g_focus_q.count--;
        pthread_mutex_unlock(&g_focus_q.lock);

        Focus_WaitUntilReady(g_fd_focus, g_focus.set_pos, 5000);

        uint16_t nt = (d[2]<<8)|d[3];
        g_focus.mode=d[0]; g_focus.en=d[1]; g_focus.set_pos=nt; 
        g_focus.set_spd=(d[4]<<8)|d[5]; g_focus.ctrl=(d[6]<<8)|d[7];

        if(d[9]) Send_FocusWrite(g_fd_focus, 0x6040, 0x0080);
        if(d[0]) Send_FocusWrite(g_fd_focus, 0x6060, d[0]);
        if(d[1]==0x0F) { Send_FocusWrite(g_fd_focus, 0x6040, 0x0006); Send_FocusWrite(g_fd_focus, 0x6040, 0x000F); }
        Send_FocusWrite(g_fd_focus, 0x6101, g_focus.set_pos);
        Send_FocusWrite(g_fd_focus, 0x6102, g_focus.set_spd);
        if(g_focus.ctrl) Send_FocusWrite(g_fd_focus, 0x6040, g_focus.ctrl);

        Focus_WaitUntilReady(g_fd_focus, nt, 5000);
    }
    return NULL;
}

// --- [TCP 上报业务逻辑] ---
void Tcp_ReportStatus(uint8_t id) {
    if (g_client_fd < 0) return;
    uint8_t b[16], len = 0;
    if (id == ID_FOCUS) { b[0]=g_focus.mode; b[1]=g_focus.en; b[2]=g_focus.set_pos>>8; b[3]=g_focus.set_pos; b[4]=g_focus.set_spd>>8; b[5]=g_focus.set_spd; b[6]=g_focus.ctrl>>8; b[7]=g_focus.ctrl; b[8]=g_focus.act_pos>>8; b[9]=g_focus.act_pos; len = 10; }
    else if (id == ID_RANGER) { b[0]=g_ranger.apd; b[1]=g_ranger.state; b[2]=g_ranger.dist>>16; b[3]=g_ranger.dist>>8; b[4]=g_ranger.dist; b[5]=g_ranger.status; b[6]=g_ranger.self; len = 7; }
    else if (id == ID_LASER) { b[0]=g_laser.vin/512; b[1]=g_laser.vout/512; b[2]=g_laser.iout/512; b[3]=g_laser.temp/16; b[4]=g_laser.fault; len = 5; }
    else if (id == ID_ILLUM) { b[0]=g_illum.state; b[1]=g_illum.fault; b[2]=g_illum.t_int; b[3]=g_illum.t_dec; len = 4; }
    else if (id == ID_THP) { b[0]=g_thp.temp>>8; b[1]=g_thp.temp; b[2]=g_thp.humi>>8; b[3]=g_thp.humi; b[4]=g_thp.press>>8; b[5]=g_thp.press; len = 6; }
    uint8_t f[64]; int p=0; f[p++]=0x7F; f[p++]=0x7E; f[p++]=len; f[p++]=id; memcpy(&f[p], b, len); p+=len; uint8_t s = len+id; for(int i=0; i<len; i++) s+=b[i]; f[p++]=s; f[p++]=0x55; f[p++]=0xAA;
    send(g_client_fd, f, p, 0);
}

void *Tcp_DispatchThread(void *arg) {
    uint8_t b[128];
    while (1) {
        pthread_mutex_lock(&g_q.lock);
        while (g_q.count < 7) pthread_cond_wait(&g_q.cond, &g_q.lock);
        if (Q_Peek(0)!=0x7E || Q_Peek(1)!=0x7F) { Q_Pop(1); pthread_mutex_unlock(&g_q.lock); continue; }
        int ilen = Q_Peek(2); int tlen = ilen + 7;
        if (tlen > g_q.count) { pthread_mutex_unlock(&g_q.lock); usleep(5000); continue; }
        uint8_t id = Q_Peek(3); for(int i=0; i<ilen; i++) b[i] = Q_Peek(4+i); Q_Pop(tlen); pthread_mutex_unlock(&g_q.lock);
        
        if (id == ID_FOCUS) Push_FocusTask(b);
        else if (id == ID_RANGER) { if(b[0]) Send_RangerCmd(g_fd_ranger, 0x08, 0, 1); else Send_RangerCmd(g_fd_ranger, 0x08, 0, 0); if(b[1]) Send_RangerCmd(g_fd_ranger, 0x02, 0, 0x64); else Send_RangerCmd(g_fd_ranger, 0x00, 0, 0); }
        else if (id == ID_ILLUM) { if(b[0]>0) Send_IllumPacket(g_fd_illum, 0x20, b[0], 0); if(b[1]) Send_IllumPacket(g_fd_illum, 0x21, 0, 0); else Send_IllumPacket(g_fd_illum, 0x22, 0, 0); if(b[2]) Send_IllumPacket(g_fd_illum, 0x84, 1, 0); if(b[3]) Send_IllumPacket(g_fd_illum, 0x10, 0, 0); }
        else if (id == ID_THP) { if(b[0]) { uint8_t q[]={0x01,0x03,0x00,0x00,0x00,0x03,0x05,0xCB}; Uart_Send485(g_fd_thp, open("/sys/class/gpio/gpio42/value", O_RDWR), q, 8, "THP_QUERY"); } }
        else if (id == ID_LASER) { Send_LaserPacket(g_fd_laser, b[0], 0x04, (uint16_t)b[4]<<8, (uint16_t)b[5]<<8); Send_LaserPacket(g_fd_laser, b[0], 0x01, (uint16_t)b[1]<<8, 0); Send_LaserPacket(g_fd_laser, b[0], 0x02, b[2], 0); if(b[6]) Send_LaserPacket(g_fd_laser, b[0], 0x03, 0, 0); }
    }
}

void *Tcp_ServerThread(void *arg) {
    int sfd, cfd; struct sockaddr_in addr; uint8_t temp[512];
    pthread_t tid, tid_focus; 
    pthread_create(&tid, NULL, Tcp_DispatchThread, NULL);
    pthread_create(&tid_focus, NULL, Focus_MotionEngineThread, NULL);
    sfd = socket(AF_INET, SOCK_STREAM, 0); int opt=1; setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, 4);
    addr.sin_family=AF_INET; addr.sin_port=htons(TCP_PORT); addr.sin_addr.s_addr=INADDR_ANY;
    bind(sfd, (struct sockaddr*)&addr, 16); listen(sfd, 5);
    while (1) {
        cfd = accept(sfd, NULL, NULL); if(cfd < 0) continue;
        g_client_fd = cfd; printf("[TCP] New Client Connected.\n");
        while (1) { int n = recv(cfd, temp, 512, 0); if(n <= 0) break; for (int i=0; i<n; i++) Q_Push(temp[i]); }
        close(cfd); g_client_fd = -1;
    }
}

void Focus_AutoHoming(int fd) { Send_FocusWrite(fd, 0x6060, 6); Send_FocusWrite(fd, 0x6040, 6); Send_FocusWrite(fd, 0x6040, 0x0F); Send_FocusWrite(fd, 0x6040, 0x1F); Focus_WaitUntilReady(fd, 0, 10000); }
void Ranger_AutoInit(int fd) { Send_RangerCmd(fd, 0x04, 0, 0x14); Send_RangerCmd(fd, 0x0B, 0x4E, 0x20); Send_RangerCmd(fd, 0x20, 0, 5); Send_RangerCmd(fd, 0x22, 0, 0); }
void Thp_AutoInit(int fd) { uint8_t w[]={0,0x30,1,0xA4,0}, r[]={0,0x20,0,0x68,0}; write(fd,w,5); usleep(500000); write(fd,r,4); }
void Illum_HeartbeatQuery(int fd) { if(fd<0)return; uint8_t c[13]={0x55,0xAA,0,0,0,0,3,1,1,1,6,0xFE,0xFE}; write(fd,c,13); }
