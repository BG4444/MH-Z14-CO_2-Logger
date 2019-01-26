#include "ets_sys.h"
#include "osapi.h"
#include "os_type.h"
#include "user_interface.h"
#include "espconn.h"
#include "mem.h"
#include "gpio.h"
#include "user_config.h"
#include "net_config.h"
#include "tcp_streamer.h"
#include "strbuf.h"
#include "websrvr.h"
#include "tar.h"

static void at_tcpclient_connect_cb(void *arg);

static volatile os_timer_t WiFiLinker;

volatile unsigned int nClients=0;
bool connected=false;

static void ICACHE_FLASH_ATTR at_tcpclient_recon_cb(void *arg, sint8 err)
{
    struct espconn *pespconn = (struct espconn *)arg;

    tcp_streamer* cur1=find_item(streamsOut,pespconn);

    if(cur1)
    {
        delete_tcp_streamer_item(&streamsOut, cur1);
    }

    tcp_streamer* cur2=find_item(streamsInp,pespconn);

    if(cur2)
    {
        delete_tcp_streamer_item(&streamsInp, cur2);
    }

     LOG_CLIENT("TCP error",pespconn);
}

static void ICACHE_FLASH_ATTR senddata()
{
        char info[150];
        char tcpserverip[15];
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

static void ICACHE_FLASH_ATTR wifi_check_ip(void *arg)
{        
        for(tcp_streamer* current=streamsOut;current;current=current->next)
        {
            if(current->mode==LongPoll && --current->timer == 0)
            {
                const char status ='0'+getPinStatus();
                strBuf send;

                sendStatus(status,&send);
                sendStringNoCopy(current,&send);
            }
        }


        ++nTicks;
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

                if(!connected)
                {
                    senddata();
                }
                connected=true;
                // Запускаем таймер проверки соединения и отправки данных уже раз в 5 сек, см. тех.задание
                os_timer_setfn(&WiFiLinker, (os_timer_func_t *)wifi_check_ip, NULL);
                os_timer_arm(&WiFiLinker, 5000, 0);
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
    struct espconn *pespconn = (struct espconn *)arg;

    tcp_streamer* cur=find_item(streamsOut,pespconn);

    if(cur)
    {
        switch(cur->mode)
        {
            case File:
                send_item(cur);
            break;
            case LogDump:
                espconn_sent(cur->pCon,  cur->logPos->message.begin, cur->logPos->message.len);                
                cur->logLen-=cur->logPos->message.len;

                const log_entry* tnext=cur->logPos->next;

                log_free(cur->logPos->message.begin);
                log_free(cur->logPos);

                if(cur->logLen)
                {                    
                    cur->logPos=tnext;
                }
                else
                {                    
                    cur->mode=KillMeNoDisconnect;
                }


            break;
            case KillMe:
                espconn_disconnect(pespconn);
            case KillMeNoDisconnect:
                delete_tcp_streamer_item(&streamsOut, cur);
                LOG_CLIENT("Query done",pespconn);


                tcp_streamer* current=streamsOut;
                for(; current; current=current->next)
                {
                    switch(current->mode)
                    {
                        case SendString:
                            current->mode=KillMeNoDisconnect;
                        break;
                        case SendFile:
                            current->mode=File;
                        break;
                        case LogDumpHeaders:
                            current->mode=LogDump;
                        break;
                        default:
                            continue;
                    }
                    espconn_sent(current->pCon, current->string.begin, current->string.len);
                    log_free(current->string.begin);
                    break;
                }
                if(!current)
                {
                    is_sending=false;
                }
            break;
        }
    }
}


static void ICACHE_FLASH_ATTR at_tcpclient_recv_cb(void *arg,char *pdata, unsigned short len)
{
    struct espconn *pespconn = (struct espconn *)arg;

    tcp_streamer* f=find_item(streamsInp,pespconn);

    const strBuf inputMessage={pdata,len};



    if(f)
    {
        strBuf tbuf;
        append(2,&tbuf, &f->string, &inputMessage);
        log_free(f->string.begin);

        if(doReply(&tbuf, pespconn))
        {
            f->string=tbuf;
            LOG_CLIENT("New partial query part",pespconn);
        }
        else
        {
            log_free(tbuf.begin);
            delete_tcp_streamer_item(&streamsInp,f);
            LOG_CLIENT("New partial query assembled",pespconn);
        }
    }
    else
    {
        if(doReply(&inputMessage,pespconn))
        {
            LOG_CLIENT("New partial query",pespconn);

            tcp_streamer* s = add_tcp_streamer_item(&streamsInp);

            s->pCon=pespconn;

            s->mode=Head;

            copy(&inputMessage,&s->string);
        }
        else
        {
            LOG_CLIENT("New full query",pespconn);
        }
    }
}


static void ICACHE_FLASH_ATTR at_tcpclient_discon_cb(void *arg)
{
        struct espconn *pespconn = (struct espconn *)arg;
        // Отключились, освобождаем память
       // log_free(pespconn->proto.tcp);
       // log_free(pespconn);
        #ifdef PLATFORM_DEBUG
        uart0_sendStr("Disconnect callback\r\n");
        #endif

        --nClients;

        LOG_CLIENT("Client disconnect",pespconn);

}



static void ICACHE_FLASH_ATTR at_tcpclient_connect_cb(void *arg)
{
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

        LOG_CLIENT("Client connected",pespconn);

}


void BtnInit()
{


}

//Init function 
void ICACHE_FLASH_ATTR user_init()
{
            MAKE_STR_BUF(systemInit,"Init system\n");
            MAKE_STR_BUF(systemInitDone,"Init done\n");
            MAKE_STR_BUF(wifiConfigSet,"WiFi SET\n");

            add_message(&systemInit, nTicks);

            uart0_sendStr("ESP8266 platform starting...\r\n");

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

            add_message(&wifiConfigSet, nTicks);
            // Для отладки выводим в uart данные о настройке режима STA

            // Запускаем таймер проверки соединения по Wi-Fi, проверяем соединение раз в 1 сек., если соединение установлено, то запускаем TCP-клиент и отправляем тестовую строку.
            os_timer_disarm(&WiFiLinker);
            os_timer_setfn(&WiFiLinker, (os_timer_func_t *)wifi_check_ip, NULL);
            os_timer_arm(&WiFiLinker, 1000, 0);
            // Инициализируем GPIO,
            BtnInit();
            // Выводим сообщение о успешном запуске
            add_message(&systemInitDone, nTicks);
}
