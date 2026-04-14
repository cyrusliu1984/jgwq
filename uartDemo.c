#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>  
#include <stdbool.h>

#include <fcntl.h>
#include <time.h>
#include <assert.h>
#include <termios.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/input.h>
#include <errno.h>




//#define COM_OUT_PORT	"/dev/ttyS3"//下行COM口
//#define COM_OUT_RATE    115200//下行COM波特率
#define TIME_OUT_MS     1000//下行COM接收发送数据超时时间2s

static int speed_arr[] = { B115200, B57600, B38400, B19200, B9600, B4800, B2400, B1200, B300,  

    B115200, B57600, B38400, B19200, B9600, B4800, B2400, B1200, B300, };  

static int name_arr[] = {115200, 57600, 38400, 19200, 9600, 4800, 2400, 1200, 300,  

    115200, 57600, 38400, 19200, 9600, 4800, 2400, 1200, 300, };  

/*----------------------------------------------------------------------------- 

  函数名:      set_speed 

  参数:        int fd ,int speed 

  返回值:      void 

  描述:        设置fd表述符的串口波特率 

 *-----------------------------------------------------------------------------*/  

static void set_speed(int fd ,int speed)  
{  
    struct termios opt;  
    int i;  
    int status;  

    tcgetattr(fd,&opt);  

    for(i = 0;i < sizeof(speed_arr)/sizeof(int);i++)  

    {  
        if(speed == name_arr[i])                        //找到标准的波特率与用户一致   
        {  
            tcflush(fd,TCIOFLUSH);                      //清除IO输入和输出缓存   
            cfsetispeed(&opt,speed_arr[i]);         //设置串口输入波特率   
            cfsetospeed(&opt,speed_arr[i]);         //设置串口输出波特率   
            status = tcsetattr(fd,TCSANOW,&opt);    //将属性设置到opt的数据结构中，并且立即生效   
            if(status != 0)  
                perror("tcsetattr fd:");                //设置失败   

            return ;  
        }  
        tcflush(fd,TCIOFLUSH);                          //每次清除IO缓存   
    }  
}  

/*----------------------------------------------------------------------------- 

  函数名:      set_parity 

  参数:        int fd 

  返回值:      int 

  描述:        设置fd表述符的奇偶校验 

 *-----------------------------------------------------------------------------*/  

static int set_parity(int fd)  
{  
    struct termios opt;  

    if(tcgetattr(fd,&opt) != 0)                 //或许原先的配置信息   
    {  
        perror("Get opt in parity error:");  
        return -1;  
    }

    /*通过设置opt数据结构，来配置相关功能，以下为八个数据位，不使能奇偶校验*/  

    opt.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);  

    opt.c_oflag &= ~OPOST;  
    opt.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);  
    opt.c_cflag &= ~(CSIZE | PARENB);  
    opt.c_cflag |= CS8;  
    tcflush(fd,TCIFLUSH);                           //清空输入缓存   
  
    if(tcsetattr(fd,TCSANOW,&opt) != 0)  
    {  
        perror("set attr parity error:");  
        return -1;  
    }  

    return 0;  
}  

/*----------------------------------------------------------------------------- 

  函数名:      serial_init 

  参数:        char *dev_path,int speed,int is_block 

  返回值:      初始化成功返回打开的文件描述符 

  描述:        串口初始化，根据串口文件路径名，串口的速度，和串口是否阻塞,block为1表示阻塞 

 *-----------------------------------------------------------------------------*/  

static int serial_init(char *dev_path,int speed,int is_block)  
{  
    int fd;  
    int flag;  

    flag = 0;  
    flag |= O_RDWR;                     //设置为可读写的串口属性文件   

    if(is_block == 0)  
        flag |=O_NONBLOCK;              //若为0则表示以非阻塞方式打开   

    fd = open(dev_path,flag);               //打开设备文件   
    if(fd < 0)  
    {  
        printf("Open device file err:");  
        close(fd);  
        return -1;  
    }  
    else
    {
        printf("Open device file success:");        
    }

    /*打开设备文件后，下面开始设置波特率*/  
    set_speed(fd,speed);                //考虑到波特率可能被单独设置，所以独立成函数   

    /*设置奇偶校验*/  
    if(set_parity(fd) != 0)  
    {  
        printf("set parity error:");  
        close(fd);                      //一定要关闭文件，否则文件一直为打开状态   
        return -1;  
    } 
    else
    {
        printf("set parity success:");        
    } 

    return fd;  
}  

/*----------------------------------------------------------------------------- 

  函数名:      serial_send 

  参数:        int fd,char *str,unsigned int len 

  返回值:      发送成功返回发送长度，否则返回小于0的值 

  描述:        向fd描述符的串口发送数据，长度为len，内容为str 

 *-----------------------------------------------------------------------------*/  

static int serial_send(int fd, unsigned char *str,unsigned int len)  
{  
    int ret;  
/*
    if(len > strlen(str))                    //判断长度是否超过str的最大长度   
        len = strlen(str);  
*/
    ret = write(fd,str,len);  
    if(ret < 0)  
    {  
        printf("serial send err:");  
        return -1;  
    }  
    return ret;  
}  

/*----------------------------------------------------------------------------- 

  函数名:      serial_read 

  参数:        int fd,char *str,unsigned int len,unsigned int timeout 

  返回值:      在规定的时间内读取数据，超时则退出，超时时间为ms级别 

  描述:        向fd描述符的串口接收数据，长度为len，存入str，timeout 为超时时间 

 *-----------------------------------------------------------------------------*/  

static int serial_read(int fd, unsigned char *str, unsigned int len, unsigned int timeout)  
{  
    fd_set rfds;  
    struct timeval tv;  
    int ret;                                //每次读的结果   
    int sret;                               //select监控结果   
    int readlen = 0;                        //实际读到的字节数   
//    char * ptr;  
//    ptr = str;                          //读指针，每次移动，因为实际读出的长度和传入参数可能存在差异   

    FD_ZERO(&rfds);                     //清除文件描述符集合   
    FD_SET(fd,&rfds);                   //将fd加入fds文件描述符，以待下面用select方法监听   

    /*传入的timeout是ms级别的单位，这里需要转换为struct timeval 结构的*/  
    tv.tv_sec  = timeout / 1000;  
    tv.tv_usec = (timeout%1000)*1000;  

    /*开始读*/  
    while(readlen < len)  
    {  
        sret = select(fd+1,&rfds,NULL,NULL,&tv);        //检测串口是否可读   
//        printf("scet=%d\n",sret); 
        if(sret == -1)                              //检测失败   
        {  
            printf("select:");
            printf("select failed.\n");  
            break;  
        }  
        else if(sret > 0)                       
        {  
//            ret = read(fd,ptr,1); 
            ret = read(fd,str + readlen,1);  
//            printf("ret:%d\n",ret);
            if(ret < 0)  
            {  
                printf("read err:");  
                break;  
            }  
            else if(ret == 0)  
                break;  

            readlen += ret;                             //更新读的长度   
//            ptr     += ret;                             //更新读的位置   

        }  
        else                                                    //超时   
        {  
            //printf(".");  
            break;  
        }  
    }   
    return readlen;  
}  

/*----------------------------------------------------------------------------- 

  函数名:      OpenComOut 

  参数:        

  返回值:      打开的串口的标示ID

  描述:        打开下行串口

 *-----------------------------------------------------------------------------*/  
int OpenComOut(char * dev, int speed)
{
	int m_hWriteFile;
	
	printf("OPEN COM:%s, SPEED:%d\r\n",dev,speed);
    //m_hWriteFile =  serial_init(COM_OUT_PORT,COM_OUT_RATE,0);
	m_hWriteFile =  serial_init(dev,speed,0);
    if(m_hWriteFile < -1)
    {
        printf("uart_open error\n");
		return -1;
    }
    else
    {
        printf("uart_open success\n");
    }

	return m_hWriteFile;
}

/*----------------------------------------------------------------------------- 

  函数名:      ComSend 

  参数:        int pCom,char* buf,int buflen

  返回值:      成功发送数据的长度

  描述:        从标示ID为pCom的串口发送buf数据，长度为buflen

 *-----------------------------------------------------------------------------*/
int ComSend(int pCom,char* buf,int buflen)
{
	unsigned long dwBytesWrite = 0;
    dwBytesWrite = serial_send(pCom,buf,buflen);
    
    if(dwBytesWrite == -1)
    {
        printf("uart write failed!\n");
    }
	return dwBytesWrite;
}

/*----------------------------------------------------------------------------- 

  函数名:      ComRecv 

  参数:        int pCom,char* buf,int buflen

  返回值:      接收到数据的长度

  描述:        从标示ID为pCom的串口接收buflen长度的数据保存在buf中

 *-----------------------------------------------------------------------------*/
int ComRecv(int pCom,char* buf,int buflen)
{
    int ret = serial_read(pCom,buf,buflen,TIME_OUT_MS); 
    if(ret == -1)
    {
        printf("uart read failed!\n");
    }
	return ret;	
}


#if 0
/*
	测试方式：每隔1秒，发送定量字节，然后接收相同数量字节。
*/
#define	VERSION	"1.0"
int main(int argc , char *argv[])
{
	unsigned char txBuf[255]={0}, rxBuf[255]={0};
	int txLen=0, rxLen=0;
	int m_ComOut;
	bool isComOutOpen;
	
	int ret;
	int gapSec = atoi(argv[4]);
	/*校验传参*/
	if(5 != argc)
	{
		fprintf(stderr, "usage : %s <uart-dev> <uart-speed> <send-len> <gap-Second>\r\n", argv[0]);
		exit(-1);
	}
	txLen = (atoi(argv[3]) > 255) ? 255 : atoi(argv[3]);
	printf("\n\n");
	printf("###################### UART TEST PROGRAM VER: V%s ########################\r\n",VERSION);
	printf("###### it will send [%d] Bytes /  %d Sec, then read [%d] Bytes ######\r\n\n",txLen,gapSec,txLen);
	m_ComOut = -1;
	isComOutOpen = false;

	/*初始化*/
	for(int i = 0; i < 5; i ++)
	{
		m_ComOut = OpenComOut(argv[1], atoi(argv[2]));		
		if(m_ComOut != -1)
		{
			printf("com open success\n");
			isComOutOpen = true;
			break;
		}
	}
	
	if( isComOutOpen != true )
	{
		printf("com open FAILED,return\r\n");
		return 1;
	}
	
	
	for(int i=0;i<txLen;i++)
		txBuf[i]=i;
	rxLen = 0;

	/*收发测试*/
	int SendFlag=0;
	struct timespec current;
	uint32_t tick=0;
	while(1)
	{		
		// tx
		if(SendFlag)
		{
			ret = ComSend(m_ComOut,txBuf,txLen);
			printf("send [%d] Bytes :\r\n",ret);
			for(int i=0; i<ret; i++)
			{
				printf("%02X ",txBuf[i]);					
			}
			printf("\n\n");					
			
			SendFlag = 0;
		}

		// rx
		rxLen = ComRecv(m_ComOut, rxBuf, txLen);
		if (rxLen > 0)
		{		
			printf("RECV [%d] Bytes : \r\n",rxLen);
			for(int i=0;i<rxLen;i++)
			{
				printf("%02X ",rxBuf[i]);
			}
			printf("\n\n");	
		}
				
		// count 1s		
		clock_gettime(CLOCK_MONOTONIC, &current);
		if(current.tv_sec - tick >= gapSec)
		{
			SendFlag = 1;
			tick = current.tv_sec;
		}		
	}
	
	return 0;
}
#endif

int uartParaSet(unsigned int fd, unsigned int checkEn, unsigned int checkType/*0: 奇校验，1：偶校验*/)
{
    struct termios opt;  

    if(tcgetattr(fd,&opt) != 0)                 //或许原先的配置信息   
    {  
        printf("Get opt in parity error:");  
        return -1;  
    }
	

	if(checkEn)
	{
		if(checkType)//偶校验
		{
			opt.c_cflag |= PARENB;	
			opt.c_cflag &= ~PARODD;
			opt.c_iflag |= INPCK;	

		}
		else
		{
			opt.c_cflag |= PARENB | PARODD;	
			opt.c_iflag |= INPCK;

		}
	}
    tcflush(fd,TCIFLUSH);                           //清空输入缓存   
  
    if(tcsetattr(fd,TCSANOW,&opt) != 0)  
    {  
        printf("set attr parity error:");  
        return -1;  
    }  
	return 0;
}

#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#define TEST_LEN 32
typedef struct{
	unsigned int ch;
	unsigned int speed;
	unsigned int checkEn;
	unsigned int checkType;/*0: 奇校验，1：偶校验*/
}UART_ARG;
	
void _422Task(UART_ARG* arg)
{
	unsigned char txBuf[256];
	unsigned char rxBuf[256];
	unsigned int i = 0;
	int ret = 0,rxLen = 0;

	printf("_422Task ch%d, speed:%d\r\n", arg->ch, arg->speed);
	for(i= 0; i < TEST_LEN; i++)
	{
		txBuf[i] = i + arg->ch * 0x11;
	}
	memset(rxBuf,0,TEST_LEN);
	char name[20] = "/dev/ttyS";	//串口初始化，设置通道号、波特率、奇偶校验
	sprintf(&name[9], "%d", arg->ch);
	printf("dev:%s\r\n", name);
	int m_ComOut= OpenComOut(name, arg->speed);
	uartParaSet(m_ComOut,arg->checkEn, arg->checkType);
	
	while(1)
	{
		ret = ComSend(m_ComOut,txBuf,TEST_LEN);
#if 0
		printf("send [%d] Bytes :\r\n",ret);
		for(int i=0; i<ret; i++)
		{
			printf("%02X ",txBuf[i]);					
		}
		printf("\n\n"); 				
#endif

			// rx
		rxLen = ComRecv(m_ComOut, rxBuf, TEST_LEN);
		if (rxLen > 0)
		{		
			printf("ch%d RECV [%d] Bytes : \r\n", arg->ch, rxLen);
			for(int i=0;i<rxLen;i++)
			{
				printf("%02X ",rxBuf[i]);
			}
			printf("\n\n"); 
		}
		
		usleep(100000);
	}
}

void _485Task(UART_ARG* arg)
{
	unsigned char txBuf[256];
	unsigned char rxBuf[256];
	unsigned int i = 0;
	int ret = 0,rxLen = 0;
	int fd = 0;
	unsigned char switchVal = 0;
	

	printf("_485Task ch%d, speed:%d\r\n", arg->ch, arg->speed);
	for(i= 0; i < TEST_LEN; i++)
	{
		txBuf[i] = i;
	}
	memset(rxBuf,0,TEST_LEN);
	char name[20] = "/dev/ttyS";
	sprintf(&name[9], "%d", arg->ch);
	int m_ComOut= OpenComOut(name, arg->speed);
	uartParaSet(m_ComOut,arg->checkEn, arg->checkType);
	if(arg->ch == 8)
	{
		fd = open("/sys/class/gpio/gpio38/value", O_RDWR|O_NONBLOCK);
		if(fd < 0)
		{
			printf("err, open gpio38 fail\r\n");
			return;
		}

	}
	else if(arg->ch == 9)
	{
		fd = open("/sys/class/gpio/gpio37/value", O_RDWR|O_NONBLOCK);
		if(fd < 0)
		{
			printf("err, open gpio37 fail\r\n");
			return;
		}

	}
	else
	{
		printf("ch err, ch = %d,rs485 ch must equal 8 or 9\r\n", arg->ch);
		return;
	}
	
	while(1)
	{
		write(fd, &switchVal, 1);
		usleep(10000);
		ret = ComSend(m_ComOut,txBuf,TEST_LEN);
		usleep(10000);
		write(fd, &switchVal, 0);
#if 0
		printf("send [%d] Bytes :\r\n",ret);

		for(int i=0; i<ret; i++)
		{
			printf("%02X ",txBuf[i]);					
		}
		printf("\n\n"); 				
#endif
		rxLen = ComRecv(m_ComOut, rxBuf, TEST_LEN);
		if (rxLen > 0)
		{		
			printf("ch%d RECV [%d] Bytes : \r\n", arg->ch, rxLen);
			for(int i=0;i<rxLen;i++)
			{
				printf("%02X ",rxBuf[i]);
			}
			printf("\n\n"); 
		}
		
		usleep(100000);
	}
}


#if 0
int main(int argc , char *argv[])
{
	pthread_t uart_thread[20];
	UART_ARG arg[20];
#if 1
	arg[0].ch = 0;//ZM422
	arg[0].speed = 115200;
	arg[0].checkEn = 0;
	arg[0].checkType = 0;/*0: 奇校验，1：偶校验*/
	if (pthread_create(&uart_thread[0], NULL, (void* (*)(void*))_422Task, &arg[0]) != 0)
	{
		printf("pthread_create rs422_thread error.\n");
		return -1;
	}

	arg[1].ch = 1;//CJ422
	arg[1].speed = 115200;
	arg[1].checkEn = 0;
	arg[1].checkType = 0;/*0: 奇校验，1：偶校验*/
	if (pthread_create(&uart_thread[1], NULL, (void* (*)(void*))_422Task, &arg[1]) != 0)
	{
		printf("pthread_create app_thread error.\n");
		return -1;
	}

	arg[2].ch = 4;//YU1 422
	arg[2].speed = 115200;
	arg[2].checkEn = 0;
	arg[2].checkType = 0;/*0: 奇校验，1：偶校验*/
	if (pthread_create(&uart_thread[2], NULL, (void* (*)(void*))_422Task, &arg[2]) != 0)
	{
		printf("pthread_create app_thread error.\n");
		return -1;
	}

	arg[3].ch = 5;//YU2 422
	arg[3].speed = 115200;
	arg[3].checkEn = 0;
	arg[3].checkType = 0;/*0: 奇校验，1：偶校验*/
	if (pthread_create(&uart_thread[3], NULL, (void* (*)(void*))_422Task, &arg[3]) != 0)
	{
		printf("pthread_create app_thread error.\n");
		return -1;
	}

	arg[4].ch = 6;//JG422
	arg[4].speed = 115200;
	arg[4].checkEn = 0;
	arg[4].checkType = 0;/*0: 奇校验，1：偶校验*/
	if (pthread_create(&uart_thread[4], NULL, (void* (*)(void*))_422Task, &arg[4]) != 0)
	{
		printf("pthread_create app_thread error.\n");
		return -1;
	}
#endif
	arg[5].ch = 7;//RK3588和z7之间通信的422
	arg[5].speed = 115200;
	arg[5].checkEn = 0;
	arg[5].checkType = 0;/*0: 奇校验，1：偶校验*/
	if (pthread_create(&uart_thread[5], NULL, (void* (*)(void*))_422Task, &arg[5]) != 0)
	{
		printf("pthread_create app_thread error.\n");
		return -1;
	}
#if 1
	arg[6].ch = 8;//wsy 485
	arg[6].speed = 115200;
	arg[6].checkEn = 0;
	arg[6].checkType = 0;/*0: 奇校验，1：偶校验*/
	if (pthread_create(&uart_thread[6], NULL, (void* (*)(void*))_485Task, &arg[6]) != 0)
	{
		printf("pthread_create app_thread error.\n");
		return -1;
	}

	arg[7].ch = 9;//tj485
	arg[7].speed = 115200;
	arg[7].checkEn = 0;
	arg[7].checkType = 0;/*0: 奇校验，1：偶校验*/
	if (pthread_create(&uart_thread[7], NULL, (void* (*)(void*))_485Task, &arg[7]) != 0)
	{
		printf("pthread_create app_thread error.\n");
		return -1;
	}
#endif

	while(1);

}
#endif

//计算调焦最后两字节CRC
static uint16_t tj_crc16(uint8_t *data, uint16_t length)
{
	uint16_t crc = 0xffff;	//初始值
	uint16_t i,j;

	for(i = 0; i < length; i++)
	{
		crc ^= (uint16_t)data[i];
		for(j = 0; j < 8; j++)
		{
			if(crc & 0x0001)
			{
				crc >>= 1;
				crc ^= 0xa001;
			}
			else
				crc >>= 1;
		}
	}
	return crc;
}


unsigned char tj_xlmode_frame1[8] = {0x01,0x06,0x60,0x60,0x00,0x06,0xd0,0x98};
unsigned char tj_dis_frame2[8] = {0x01,0x06,0x60,0x40,0x00,0x06,0xe8,0x99};
unsigned char tj_en_frame1[8] = {0x01,0x06,0x60,0x40,0x00,0x0f,0xd6,0x98};
unsigned char tj_start_frame1[8] = {0x01,0x06,0x60,0x40,0x00,0x1f,0xd7,0xd6};
unsigned char tj_stop_frame1[8] = {0x01,0x06,0x60,0x40,0x01,0x0f,0xd7,0x8a};
unsigned char tj_wzmode_frame1[8] = {0x01,0x06,0x60,0x60,0x00,0x01,0x11,0x98};
unsigned char tj_wzlow_frame1[8] = {0x01,0x06,0x61,0x01,0x00,0x0,0x00,0x00};
unsigned char tj_wzhigh_frame1[8] = {0x01,0x06,0x60,0x7a,0x00,0x00,0x00,0x00};
unsigned char tj_splow_frame1[8] = {0x01,0x06,0x61,0x02,0x00,0x00,0x00,0x00};
unsigned char tj_sphigh_frame1[8] = {0x01,0x06,0x60,0x81,0x00,0x00,0x00,0x00};

void tj_cmd_send(unsigned int uartchl,uint8_t *txBuf,uint8_t crccalflag)
{
	unsigned char rxBuf[8] = {0};
	unsigned int i = 0;
	int ret = 0,rxLen = 0;
	int fd = 0;
	unsigned char switchVal = 0;
    uint16_t crc;

    if(crccalflag == 0)	// 0-不计算
    {
		write(fd, &switchVal, 1);
		usleep(10000);
		ret = ComSend(9,txBuf,8);	//m_ComOut
		usleep(10000);
		write(fd, &switchVal, 0);
    }
	else	//1-计算crc
    {
        crc = tj_crc16(txBuf,6);
		txBuf[6] = crc & 0xff;
		txBuf[7] = (crc >> 8) & 0xff;
		write(fd, &switchVal, 1);
		usleep(10000);
		ret = ComSend(9,txBuf,8);	//m_ComOut
		usleep(10000);
		write(fd, &switchVal, 0);
    }

	#if 1
		printf("send [%d] Bytes :\r\n",ret);

		for(i=0; i<ret; i++)
		{
			printf("0x%x",txBuf[i]);					
		}
		printf("\r\n"); 				
	#endif
}

