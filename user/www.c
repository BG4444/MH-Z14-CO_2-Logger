#include "websrvr.h"

#include "ets_sys.h"
#include "osapi.h"
#include "os_type.h"
#include "user_interface.h"
#include "espconn.h"
#include "mem.h"
#include "gpio.h"
#include "tcp_streamer.h"
#include "tar.h"
#include <stdlib.h>
#include "logger.h"

unsigned int getPinStatus()
{
    return ( (gpio_input_get() & BIT1) >> 1 ) | ( (gpio_input_get() & BIT3) >> 2);
}

void setPinStatus(size_t a)
{

    const size_t mask = ( (a & BIT0) << 1 ) | ( (a & BIT1) << 2);
    gpio_output_set(mask,~mask, BIT1 | BIT3 ,0);
}

bool doReply(const strBuf *inputMessageAll, struct espconn* conn)
{
    os_printf("parsing HTTP query\n");
    MAKE_STR_BUF(httpStr,"HTTP/1.1\r\n");
    MAKE_STR_BUF(logStr,"log.html");

    const strBuf indexStr=    {"index.html",10};
    const strBuf statusStr=   {"status.cgi",10};
    const strBuf _404Str=     {"404.html",8};
    const strBuf pollStr=     {"poll.cgi",8};
    const strBuf getStr=      {"GET",3};
    const strBuf putStr=      {"PUT",3};
    const strBuf strrnrn ={"\r\n\r\n",4};

    //находим конец заголовка запросаb
    const unsigned short int  firstEOL=find(inputMessageAll,0,'\n');



    if(firstEOL==inputMessageAll->len)
    {
        answer500(conn, 0);
        return false;
    }

    os_printf("query length ok\n");

    const strBuf inputMessage={inputMessageAll->begin, 1+firstEOL};

    strBuf requestString[3];

    //бьем заголовок по пробелам на части 0 - команда, 1 - путь, 2 - протокол
    if(!split(&inputMessage, requestString, 3, ' '))
    {
        answer500(conn, 1);
        return false;
    }

    os_printf("query split ok\n");
    //проверяем, что протокол - это HTTP1.1
    if(!compare(&httpStr, &requestString[2]))
    {
        answer500(conn, 2);
        return false;
    }

    os_printf("proto name OK\n");
    strBuf path[2];
    if(!split(&requestString[1],path,2,'/'))
    {
        answer500(conn, 3);
        return false;
    }

    if(path[0].len)
    {
        answer500(conn, 4);
        return false;
    }

    uint32 tsize;
    uint32 spos;
    strBuf mtime;

    char mtimeBuf[12];

    mtime.begin=mtimeBuf;
    mtime.len=12;

    bool status;

    //проверяем, что команда - это GET
    if(compare(&getStr, &requestString[0]))
    {
        os_printf("GET command\n");
        if(path[1].len==0)
        {
            path[1]=indexStr;
        }

        if(compare(&path[1], &statusStr))
        {
            os_printf("status\n");
            const char status ='0'+getPinStatus();

            strBuf send;

            sendStatus(status,&send);

            sendStringCreateStreamerNoCopy(&streamsOut,conn,&send);
            return false;
        }

        if(compare(&path[1], &pollStr))
        {
            os_printf("longpoll\n");
            tcp_streamer* s = add_tcp_streamer_item(&streamsOut);

            setCon(s,conn);

            s->mode=LongPoll;
            s->timer=3;
            return false;
        }

        if(compare(&path[1], &logStr))
        {
            tcp_streamer* s = add_tcp_streamer_item(&streamsOut);
            setCon(s,conn);

            MAKE_STR_BUF(logClean,"Log cleaned\n");
            add_message_copy(&logClean,nTicks);

            os_printf("log depth is %d\n",getCurrentDepth());

            const size_t len=getCurrentLength();


            s->logPos=log_entries;
            log_entries=0;            

            makeHTTPReply(&status200,len,0,&s->string );

            if(is_sending)
            {
                s->mode=LogDumpHeaders;
                os_printf("log dump headers mode set");
            }
            else
            {
                os_printf("log dump mode set");
                s->mode=LogDump;
                is_sending=true;                  
                current_sending=conn;
                espconn_sent(conn, s->string.begin, s->string.len);
                log_free(s->string.begin);
                os_printf("data was sent");
            }
            return false;
        }
        os_printf("look in tar\n");
        status = find_file_in_tar(&path[1],&spos,&tsize,&mtime);
    }
    else if(compare(&putStr, &requestString[0]))
    {
        os_printf("PUT command\n");
        if(compare(&path[1], &statusStr))
        {

            const strBuf inputBody={inputMessageAll->begin+1+firstEOL, inputMessageAll->len-1-firstEOL};
            const size_t dataStart=substr(&inputBody,&strrnrn);
            if(dataStart==(size_t)-1)
            {
                answer500(conn, 5);
                return false;
            }

            const strBuf data={inputBody.begin+dataStart+strrnrn.len,inputBody.len-dataStart-strrnrn.len};

            if(!data.len)
            {
                return true;
            }

            if(data.begin[0]<'0' || data.begin[0]>'3')
            {
                answer500(conn, 7);
                return false;
            }

            setPinStatus(data.begin[0]-'0');


            strBuf send;

            sendStatus(data.begin[0],&send);

            sendStringCreateStreamerNoCopy(&streamsOut,conn,&send);

            for(tcp_streamer* cur=streamsOut; cur; cur=cur->next)
            {
                if(cur->mode==LongPoll)
                {
                    sendString(cur,&send);
                }
            }

            return false;
        }
        status = false;
    }

    if(!status)
    {
        os_printf("looking in tar image\n");
        if(!find_file_in_tar(&_404Str,&spos,&tsize,&mtime))
        {
            answer500(conn, 8);
            return false;
        }
    }

    tcp_streamer* s = add_tcp_streamer_item(&streamsOut);

    setCon(s,conn);

    strBuf stage4;

    makeHTTPReply( status ? &status200 : &status404, tsize, &mtime, &stage4);
    os_printf("send file from tar\n");
    sendFileNoCopy(s, &stage4, spos, spos+tsize);

    return false;
}

void sendStatus(char status, strBuf* send)
{
    strBuf headers;
    makeHTTPReply(&status200, 1, 0, &headers);

    strBuf value={&status,1};

    append(2, send, &headers, &value);

    log_free(headers.begin);
}
