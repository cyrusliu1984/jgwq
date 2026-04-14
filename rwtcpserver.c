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

// --- [全局句柄引用] ---
extern int g_fd_focus; extern int g_fd_ranger; extern int g_fd_illum; extern int g_fd_thp; extern int g_fd_laser;

// --- [全局状态变量定义] ---
int g_client_fd = -1;
FocusState_t g_focus; RangerState_t g_ranger; IllumState_t g_illum; ThpState_t g_thp; LaserState_t g_laser;

// --- [环形缓冲区系统] ---
#define RING_BUF_SIZE 1024
static struct {
    uint8_t buffer[RING_BUF_SIZE];
    int head; int tail; int count;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} g_q = { .head=0, .tail=0, .count=0, .lock=PTHREAD_MUTEX_INITIALIZER, .cond=PTHREAD_COND_INITIALIZER };

static void Q_Push(uint8_t d) {
    pthread_mutex_lock(&g_q.lock);
    if (g_q.count < RING_BUF_SIZE) {
        g_q.buffer[g_q.head] = d;
        g_q.head = (g_q.head + 1) % RING_BUF_SIZE;
        g_q.count++;
        pthread_cond_signal(&g_q.cond);
    }
    pthread_mutex_unlock(&g_q.lock);
}

static uint8_t Q_Peek(int offset) {
    return g_q.buffer[(g_q.tail + offset) % RING_BUF_SIZE];
}

static void Q_Pop(int n) {
    g_q.tail = (g_q.tail + n) % RING_BUF_SIZE;
    g_q.count -= n;
}

// --- [协议计算辅助] ---

static uint16_t Calc_ModbusCRC(uint8_t *data, uint16_t len) {
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) crc = (crc >> 1) ^ 0xA001;
            else crc >>= 1;
        }
    }
    return crc;
}

// --- [底层发送封装] ---

static void Send_FocusWrite(int fd, uint16_t reg, uint16_t val) {
    uint8_t buf[8] = {FOCUS_MODBUS_ADDR, 0x06, (uint8_t)(reg>>8), (uint8_t)(reg&0xFF), (uint8_t)(val>>8), (uint8_t)(val&0xFF), 0, 0};
    uint16_t crc = Calc_ModbusCRC(buf, 6);
    buf[6] = crc & 0xFF; buf[7] = crc >> 8;
    Uart_Send485(fd, open("/sys/class/gpio/gpio37/value", O_RDWR), buf, 8, "FOCUS_OUT");
}

static void Send_RangerCmd(int fd, uint8_t cmd, uint8_t h, uint8_t l) {
    uint8_t buf[6] = {HEAD_RANGER, cmd, 0x02, h, l, 0};
    uint8_t x = 0; for(int i=0; i<5; i++) x ^= buf[i]; buf[5] = x;
    write(fd, buf, 6); Uart_DumpHex("RANGER_OUT", buf, 6);
}

static void Send_IllumPacket(int fd, uint8_t func, uint8_t vh, uint8_t vl) {
    uint8_t buf[14] = {0x55, 0xAA, 0, 0, 0, 0, 0x03, 0x02, 0x03, func, vh, vl, 0, 0};
    buf[12] = 0xFE; buf[13] = 0xFE;
    uint8_t s = 0; for(int i=3; i<=11; i++) s += buf[i]; buf[11] = s;
    write(fd, buf, 14); Uart_DumpHex("ILLUM_OUT", buf, 14);
}

static void Send_LaserPacket(int fd, uint8_t addr, uint8_t cmd, uint16_t val1, uint16_t val2) {
    uint8_t buf[8] = {HEAD_LASER, addr, cmd, (uint8_t)(val1>>8), (uint8_t)val1, (uint8_t)(val2>>8), (uint8_t)val2, 0};
    uint8_t s = 0; for(int i=0; i<7; i++) s += buf[i]; buf[7] = s;
    write(fd, buf, 8); Uart_DumpHex("LASER_OUT", buf, 8);
}

// --- [TCP 上报业务逻辑] ---

void Tcp_ReportStatus(uint8_t id) {
    if (g_client_fd < 0) return;
    uint8_t body[16], blen = 0;
    
    if (id == ID_FOCUS) {
        body[0]=g_focus.mode; body[1]=g_focus.en; 
        body[2]=g_focus.set_pos>>8; body[3]=g_focus.set_pos;
        body[4]=g_focus.set_spd>>8; body[5]=g_focus.set_spd;
        body[6]=g_focus.ctrl>>8; body[7]=g_focus.ctrl;
        body[8]=g_focus.act_pos>>8; body[9]=g_focus.act_pos;
        blen = 10;
    } else if (id == ID_RANGER) {
        body[0]=g_ranger.apd; body[1]=g_ranger.state; 
        body[2]=g_ranger.dist>>16; body[3]=g_ranger.dist>>8; body[4]=g_ranger.dist;
        body[5]=g_ranger.status; body[6]=g_ranger.self;
        blen = 7;
    } else if (id == ID_ILLUM) {
        body[0]=g_illum.state; body[1]=g_illum.fault; 
        body[2]=g_illum.t_int; body[3]=g_illum.t_dec;
        blen = 4;
    } else if (id == ID_THP) {
        body[0]=g_thp.temp>>8; body[1]=g_thp.temp;
        body[2]=g_thp.humi>>8; body[3]=g_thp.humi;
        body[4]=g_thp.press>>8; body[5]=g_thp.press;
        blen = 6;
    } else if (id == ID_LASER) {
        body[0]=g_laser.vin/512; body[1]=g_laser.vout/512; 
        body[2]=g_laser.iout/512; body[3]=g_laser.temp/16; 
        body[4]=g_laser.fault;
        blen = 5;
    }

    uint8_t f[64]; int p=0;
    f[p++]=0x7F; f[p++]=0x7E; f[p++]=blen; f[p++]=id;
    memcpy(&f[p], body, blen); p+=blen;
    uint8_t sum = blen + id; for(int i=0; i<blen; i++) sum += body[i];
    f[p++]=sum; f[p++]=0x55; f[p++]=0xAA;
    
    send(g_client_fd, f, p, 0);
    printf("[DEBUG][TCP] Status Frame Uploaded for ID 0x%02X\n", id);
}

// --- [各外设下发分流逻辑] ---

static void Handle_FocusDown(uint8_t *d) {
    g_focus.mode=d[0]; g_focus.en=d[1]; g_focus.set_pos=(d[2]<<8)|d[3]; 
    g_focus.set_spd=(d[4]<<8)|d[5]; g_focus.ctrl=(d[6]<<8)|d[7];
    printf("[Logic] Focus Action: Mode=%d, En=%d, Pos=%d\n", d[0], d[1], g_focus.set_pos);
    if(d[9]) Send_FocusWrite(g_fd_focus, 0x6040, 0x0080);
    if(d[0]) Send_FocusWrite(g_fd_focus, 0x6060, d[0]);
    if(d[1]==0x0F) { Send_FocusWrite(g_fd_focus, 0x6040, 0x0006); Send_FocusWrite(g_fd_focus, 0x6040, 0x000F); }
    Send_FocusWrite(g_fd_focus, 0x6101, g_focus.set_pos);
    Send_FocusWrite(g_fd_focus, 0x6102, g_focus.set_spd);
    if(g_focus.ctrl) Send_FocusWrite(g_fd_focus, 0x6040, g_focus.ctrl);
    if(d[8]) { uint8_t q[]={0x01,0x03,0x50,0x00,0x00,0x02,0xBD,0x05}; Uart_Send485(g_fd_focus, -1, q, 8, "FOCUS_QUERY"); }
}

static void Handle_RangerDown(uint8_t *d) {
    printf("[Logic] Ranger Action: APD=%d, State=%d\n", d[0], d[1]);
    if(d[0]) Send_RangerCmd(g_fd_ranger, 0x08, 0, 1); else Send_RangerCmd(g_fd_ranger, 0x08, 0, 0);
    if(d[2]) Send_RangerCmd(g_fd_ranger, 0x03, 0, 0);
    if(d[1]) Send_RangerCmd(g_fd_ranger, 0x02, 0, 0x64); else Send_RangerCmd(g_fd_ranger, 0x00, 0, 0);
}

static void Handle_IllumDown(uint8_t *d) {
    printf("[Logic] Illuminator Action: Current=%d, Switch=%d\n", d[0], d[1]);
    if(d[0]>0) Send_IllumPacket(g_fd_illum, 0x20, d[0], 0);
    if(d[1]==0x01) Send_IllumPacket(g_fd_illum, 0x21, 0, 0); else if(d[1]==0x00) Send_IllumPacket(g_fd_illum, 0x22, 0, 0);
    if(d[2]==0x01) Send_IllumPacket(g_fd_illum, 0x84, 1, 0);
    if(d[3]==0x01) Send_IllumPacket(g_fd_illum, 0x10, 0, 0);
}

static void Handle_ThpDown(uint8_t *d) {
    if(d[0]==0x01) { printf("[Logic] THP Query Triggered\n"); uint8_t q[]={0x01,0x03,0x00,0x00,0x00,0x03,0x05,0xCB}; Uart_Send485(g_fd_thp, open("/sys/class/gpio/gpio42/value", O_RDWR), q, 8, "THP_QUERY"); }
}

static void Handle_LaserDown(uint8_t *d) {
    uint8_t addr = d[0]; printf("[Logic] Laser Action Group 0x%02X\n", addr);
    Send_LaserPacket(g_fd_laser, addr, 0x04, (uint16_t)d[4]<<8, (uint16_t)d[5]<<8);
    Send_LaserPacket(g_fd_laser, addr, 0x01, (uint16_t)d[1]<<8, 0);
    Send_LaserPacket(g_fd_laser, addr, 0x02, d[2], 0);
    if(d[6]) Send_LaserPacket(g_fd_laser, addr, 0x03, 0, 0);
}

// --- [核心分发线程] ---

void *Tcp_DispatchThread(void *arg) {
    uint8_t body[128];
    while (1) {
        pthread_mutex_lock(&g_q.lock);
        while (g_q.count < 7) pthread_cond_wait(&g_q.cond, &g_q.lock);
        
        if (Q_Peek(0) != 0x7E || Q_Peek(1) != 0x7F) { Q_Pop(1); pthread_mutex_unlock(&g_q.lock); continue; }
        
        int ilen = Q_Peek(2); int tlen = ilen + 7;
        if (tlen > g_q.count) { pthread_mutex_unlock(&g_q.lock); usleep(5000); continue; }
        
        uint8_t id = Q_Peek(3);
        for(int i=0; i<ilen; i++) body[i] = Q_Peek(4+i);
        Q_Pop(tlen);
        pthread_mutex_unlock(&g_q.lock);

        printf("[DEBUG][TCP] Command for ID 0x%02X received\n", id);
        if      (id == ID_FOCUS)   Handle_FocusDown(body);
        else if (id == ID_RANGER)  Handle_RangerDown(body);
        else if (id == ID_ILLUM)   Handle_IllumDown(body);
        else if (id == ID_THP)     Handle_ThpDown(body);
        else if (id == ID_LASER)   Handle_LaserDown(body);
    }
    return NULL;
}

void *Tcp_ServerThread(void *arg) {
    int sfd, cfd; struct sockaddr_in addr; uint8_t tmp[512];
    pthread_t tid; pthread_create(&tid, NULL, Tcp_DispatchThread, NULL);
    
    sfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, 4);
    addr.sin_family = AF_INET; addr.sin_port = htons(TCP_PORT); addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(sfd, (struct sockaddr*)&addr, 16) < 0) { perror("bind"); return NULL; }
    listen(sfd, 5);
    printf("[System] TCP Protocol Gateway Server on port %d\n", TCP_PORT);

    while (1) {
        cfd = accept(sfd, NULL, NULL); if (cfd < 0) continue;
        g_client_fd = cfd; printf("[TCP] Client Session Started.\n");
        while (1) {
            int n = recv(cfd, tmp, 512, 0);
            if (n <= 0) break;
            for (int i=0; i<n; i++) Q_Push(tmp[i]);
        }
        close(cfd); g_client_fd = -1; printf("[TCP] Client Session Ended.\n");
    }
    return NULL;
}

// --- [自动化初始化函数实现] ---
void Focus_AutoHoming(int fd) { Send_FocusWrite(fd, 0x6060, 6); Send_FocusWrite(fd, 0x6040, 6); Send_FocusWrite(fd, 0x6040, 0x0F); Send_FocusWrite(fd, 0x6040, 0x1F); }
void Ranger_AutoInit(int fd) { Send_RangerCmd(fd, 0x04, 0, 0x14); Send_RangerCmd(fd, 0x0B, 0x4E, 0x20); Send_RangerCmd(fd, 0x20, 0, 5); Send_RangerCmd(fd, 0x22, 0, 0); }
void Thp_AutoInit(int fd) { uint8_t w[]={0,0x30,1,0xA4,0}, r[]={0,0x20,0,0x68,0}; write(fd,w,5); usleep(500000); write(fd,r,4); }
void Illum_HeartbeatQuery(int fd) { if(fd<0)return; uint8_t c[13]={0x55,0xAA,0,0,0,0,3,1,1,1,6,0xFE,0xFE}; write(fd,c,13); }
