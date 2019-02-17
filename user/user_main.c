#include "ets_sys.h"
#include "osapi.h"
#include "os_type.h"
#include "user_interface.h"
#include "espconn.h"
#include "mem.h"
#include "gpio.h"
#include "user_config.h"
#include "net_config.h"
#include "driver/uart.h"

#include "driver/uart_register.h"
volatile uint32_t addr=0;


static void at_tcpclient_connect_cb(void *arg);
static void ICACHE_FLASH_ATTR at_tcpclient_sent_cb(void *arg);

static volatile os_timer_t WiFiLinker;

volatile unsigned int nClients=0;

bool connected=false;

size_t BUFFER_MAX = 16384;
size_t headerLen;

char* buffer;
char* bufferTitle;
size_t bufferLen=0;

extern UartDevice UartDev;


static void ICACHE_FLASH_ATTR senddata()
{
        struct espconn *pCon = (struct espconn *)os_zalloc(sizeof(struct espconn));
        if (pCon == NULL)
        {
                #ifdef PLATFORM_DEBUG
                uart0_sendStr("TCP connect failed\r\n");
                #endif
                return;
        }
        pCon->type = ESPCONN_TCP;
        pCon->state = ESPCONN_NONE;
        pCon->proto.tcp = (esp_tcp *)os_zalloc(sizeof(esp_tcp));
        pCon->proto.tcp->local_port = 80;
        espconn_regist_connectcb(pCon, at_tcpclient_connect_cb);
        // Можно зарегистрировать callback функцию, вызываемую при реконекте, но нам этого пока не нужно
    //    espconn_regist_reconcb(pCon, at_tcpclient_recon_cb);
        // Вывод отладочной информации
        // Установить соединение с TCP-сервером
        espconn_accept(pCon);
        espconn_regist_time(pCon, 60*60, 0);
}

static void ICACHE_FLASH_ATTR append_buffer(char* text)
{

    const size_t len=strlen(text);

    size_t i=0;
    while(bufferLen + len -i > BUFFER_MAX )
    {
        while(buffer[i++] !='\n');
    }

    bufferLen-=i;
    memmove(buffer,buffer+i,bufferLen);
    memcpy(buffer+bufferLen,text,len);
    bufferLen+=len;
}

static uint8 ICACHE_FLASH_ATTR computeCRC(uint8* command)
{
    uint8 ret=0;
    for(size_t i=1;i<8;++i)
    {
        ret+=command[i];
    }
    return ~ret+1;
}

static void ICACHE_FLASH_ATTR setCRC(uint8* command)
{
    command[8]=computeCRC(command);
}

static int ICACHE_FLASH_ATTR uart1_tx_one_char(uint8 TxChar)
{
    while (true)
    {
        uint32 fifo_cnt = READ_PERI_REG(UART_STATUS(1)) & (UART_TXFIFO_CNT<<UART_TXFIFO_CNT_S);
        if ((fifo_cnt >> UART_TXFIFO_CNT_S & UART_TXFIFO_CNT) < 126) {
            break;
        }
    }

    WRITE_PERI_REG(UART_FIFO(1) , TxChar);
    return OK;
}

void ICACHE_FLASH_ATTR sendScreen(uint8_t chr,uint8_t  cmd)
{
    const uint16_t controlWord= ((((uint16_t)cmd<<8) | (uint16_t)chr))<<4;
    const uint8_t hi=controlWord >> 8;
    const uint8_t lo=controlWord & 0xFF;
    const uint8_t crc=hi ^ lo;
    uart1_tx_one_char(0xFF);
    uart1_tx_one_char(hi);
    uart1_tx_one_char(crc);
    uart1_tx_one_char(lo);
}

void clrscr()
{
    sendScreen(0x01,1);
}

void line2()
{
    sendScreen(0x40 | 0b10000000,1);
}

void printStr(const char* str)
{
    for(;*str;++str)
    {
          sendScreen(*str,0);
    }
}

void alignPrint(const char* buf)
{
    const int slen=strlen(buf);

    for(int i=0;i<(16-slen)/2;++i)
    {
        sendScreen(' ',0);
    }
    printStr(buf);
}

void showIP(uint32_t ip)
{
    char buf[32]={0};
    for(size_t i=0;i<4;++i,ip>>=8)
    {
        const uint8_t octet=ip & 0xFF;
        char lbuf[5];
        os_sprintf(lbuf,"%u",octet);
        strcat(buf,lbuf);

        if(i<3)
        {
            strcat(buf,".");
        }
    }
    alignPrint(buf);
}

static void ICACHE_FLASH_ATTR getMeasurements()
{
    static const uint8 command[9]={0xFF,0x01,0x86,1,2,3,4,5,6};
    static uint16_t COLevel=0;
    setCRC(command);
    static size_t nTicks=0;

    if(COLevel>2500)
    {
        sendScreen(nTicks & 0x01 ? 0 : 7,2);
    }
    else if(COLevel>1500)
    {
        sendScreen(nTicks & 0x01 ? 3 : 4,2);
    }
    else  if(COLevel>1200)
    {
        sendScreen(nTicks & 0x01 ? 0 : 3,2);
    }
    else if(COLevel>800)
    {
        sendScreen(nTicks & 0x01 ? 1 : 2,2);
    }
    else
    {
        sendScreen(0,2);
    }


    if(++nTicks == 10)
    {

        nTicks=0;
        uart0_tx_buffer(command,9);
        while((READ_PERI_REG(UART_STATUS(0))>>UART_TXFIFO_CNT_S) &UART_TXFIFO_CNT);
    }

    if( ((READ_PERI_REG(UART_STATUS(0))>>UART_RXFIFO_CNT_S) &UART_RXFIFO_CNT) >= 9)
    {
        uint8 reply[9];
        for(size_t i=0;i<9;++i)
        {
            reply[i]=READ_PERI_REG(UART_FIFO(0)) & 0xFF;
        }
        char buf[128];
        clrscr();
        showIP(addr);
        line2();
        if(reply[8]==computeCRC(reply))
        {
            char bufScr[128];
            COLevel=reply[2]*256+reply[3];
            const int8_t temp=reply[4]-40;
            os_sprintf(buf,"%d %d OK\n",COLevel,temp);
            os_sprintf(bufScr,"%d ppm %d""\xef""c",COLevel,temp);
            alignPrint(bufScr);
        }
        else
        {
            os_sprintf(buf,"~ ~ ERROR\n");    
            while((READ_PERI_REG(UART_STATUS(0))>>UART_RXFIFO_CNT_S) &UART_RXFIFO_CNT)
            {
                volatile int t=READ_PERI_REG(UART_FIFO(0));
            }
            printStr("--==COMM ERR==--");
        }

        append_buffer(buf);
    }   
}

static void ICACHE_FLASH_ATTR wifi_check_ip(void *arg)
{

            getMeasurements();



        // Структура с информацией о полученном, ip адресе клиента STA, маске подсети, шлюзе.
        struct ip_info ipConfig;
        // Отключаем таймер проверки wi-fi
        os_timer_disarm(&WiFiLinker);
        // Получаем данные о сетевых настройках
        wifi_get_ip_info(STATION_IF, &ipConfig);
        // Проверяем статус wi-fi соединения и факт получения ip адреса
        if (wifi_station_get_connect_status() == STATION_GOT_IP && ipConfig.ip.addr != 0)
        {
                // Соединения по wi-fi установлено
//                connState = WIFI_CONNECTED;
                #ifdef PLATFORM_DEBUG
                uart0_sendStr("WiFi connected\r\n");
        #endif
                #ifdef PLATFORM_DEBUG
                uart0_sendStr("Start TCP connecting...\r\n");
        #endif

                addr=ipConfig.ip.addr;
                if(!connected)
                {
                    clrscr();
                    showIP(addr);
                    senddata();
                }
                connected=true;




                // Запускаем таймер проверки соединения и отправки данных уже раз в 5 сек, см. тех.задание
                os_timer_setfn(&WiFiLinker, (os_timer_func_t *)wifi_check_ip, NULL);
                os_timer_arm(&WiFiLinker, 1000, 0);

        }
        else
        {
                connected=false;
                // Неправильный пароль
                if(wifi_station_get_connect_status() == STATION_WRONG_PASSWORD)
                {
//                        connState = WIFI_CONNECTING_ERROR;
                        #ifdef PLATFORM_DEBUG
                        uart0_sendStr("WiFi connecting error, wrong password\r\n");
                        #endif
                        os_timer_setfn(&WiFiLinker, (os_timer_func_t *)wifi_check_ip, NULL);
                        os_timer_arm(&WiFiLinker, 1000, 0);
                }
                // AP не найдены
                else if(wifi_station_get_connect_status() == STATION_NO_AP_FOUND)
                {
//                        connState = WIFI_CONNECTING_ERROR;
                        #ifdef PLATFORM_DEBUG
                        uart0_sendStr("WiFi connecting error, ap not found\r\n");
                        #endif
                        os_timer_setfn(&WiFiLinker, (os_timer_func_t *)wifi_check_ip, NULL);
                        os_timer_arm(&WiFiLinker, 1000, 0);
                }
                // Ошибка подключения к AP
                else if(wifi_station_get_connect_status() == STATION_CONNECT_FAIL)
                {
//                        connState = WIFI_CONNECTING_ERROR;
                        #ifdef PLATFORM_DEBUG
                        uart0_sendStr("WiFi connecting fail\r\n");
                        #endif
                        os_timer_setfn(&WiFiLinker, (os_timer_func_t *)wifi_check_ip, NULL);
                        os_timer_arm(&WiFiLinker, 1000, 0);
                }
                // Другая ошибка
                else
                {
                        os_timer_setfn(&WiFiLinker, (os_timer_func_t *)wifi_check_ip, NULL);
                        os_timer_arm(&WiFiLinker, 1000, 0);
//                        connState = WIFI_CONNECTING;
                        #ifdef PLATFORM_DEBUG
                        uart0_sendStr("WiFi connecting...\r\n");
                        #endif
                }
        }
}


static void ICACHE_FLASH_ATTR at_tcpclient_sent_cb(void *arg)
{
    --nClients;
    espconn_disconnect(arg);
}


static void ICACHE_FLASH_ATTR at_tcpclient_recv_cb(void *arg,char *pdata, unsigned short len)
{
    espconn_sent(arg,bufferTitle,bufferLen+headerLen);
}


static void ICACHE_FLASH_ATTR at_tcpclient_discon_cb(void *arg)
{     

   //     os_printf("client disconnect finally\n");
}



static void ICACHE_FLASH_ATTR at_tcpclient_connect_cb(void *arg)
{
     //   os_printf("new connect");
        struct espconn *pespconn = (struct espconn *)arg;
        #ifdef PLATFORM_DEBUG
        uart0_sen mem_usage+=size;
        char buf[512];
        os_sprintf(buf,"Memory free is %u",system_get_free_heap_size());
        add_log_buffer(buf);dStr("TCP client connect\r\n");
        #endif

        espconn_regist_recvcb(pespconn, at_tcpclient_recv_cb);

        // callback функция, вызываемая после отправки данных
        espconn_regist_sentcb(pespconn, at_tcpclient_sent_cb);

        // callback функция, вызываемая после отключения
        espconn_regist_disconcb(pespconn, at_tcpclient_discon_cb);

        ++nClients;
}

//Init function 
void ICACHE_FLASH_ATTR user_init()
{
            UartDev.baut_rate = 9600;
            uart_config(0);
            uart_config(1);
            clrscr();
            printStr("Looking for WiFi");

            buffer=os_zalloc(BUFFER_MAX);

            static const char header[]="ppm\t\C\tStatus\n";
            headerLen=strlen(header);

            BUFFER_MAX-=headerLen;

            memcpy(buffer,header,headerLen);

            bufferTitle=buffer;

            buffer+=headerLen;

            memset(buffer,'\n',BUFFER_MAX);
            system_set_os_print(0);


            // Структура с информацией о конфигурации STA (в режиме клиента AP)
            struct station_config stationConfig;

            // Проверяем если платы была не в режиме клиента AP, то переводим её в этот режим
            // В версии SDK ниже 0.9.2 после wifi_set_opmode нужно было делать system_restart
            if(wifi_get_opmode() != STATION_MODE)
            {
                    wifi_set_opmode(STATION_MODE);
            }
            // Если плата в режиме STA, то устанавливаем конфигурацию, имя AP, пароль, см. user_config.h
            // Дополнительно читаем MAC адрес нашей платы для режима AP, см. wifi_get_macaddr(SOFTAP_IF, macaddr);
            // В режиме STA у платы будет другой MAC адрес, как у клиента, но мы для себя читаем адрес который у неё был если бы она выступала как точка доступа
            if(wifi_get_opmode() == STATION_MODE)
            {
                    wifi_station_get_config(&stationConfig);
                    os_memset(stationConfig.ssid, 0, sizeof(stationConfig.ssid));
                    os_memset(stationConfig.password, 0, sizeof(stationConfig.password));
                    os_sprintf(stationConfig.ssid, "%s", WIFI_CLIENTSSID);
                    os_sprintf(stationConfig.password, "%s", WIFI_CLIENTPASSWORD);
                    wifi_station_set_config(&stationConfig);
            }

            // Для отладки выводим в uart данные о настройке режима STA

            // Запускаем таймер проверки соединения по Wi-Fi, проверяем соединение раз в 1 сек., если соединение установлено, то запускаем TCP-клиент и отправляем тестовую строку.
            os_timer_disarm(&WiFiLinker);
            os_timer_setfn(&WiFiLinker, (os_timer_func_t *)wifi_check_ip, NULL);
            os_timer_arm(&WiFiLinker, 1000, 0);            
}
