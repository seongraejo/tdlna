#define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <limits.h>
#include <stdarg.h> //가변인자
#include <dlog.h>

#include "upnphttp.h"
#include "upnpglobalvars.h"
#include "upnpsoap.h"
#include "upnpevents.h"
#include "metadata.h"

#define MAX_BUFFER_SIZE 2147483647
#define MIN_BUFFER_SIZE 65536

#define INIT_STR(s, d) { s.data = d; s.size = sizeof(d); s.off = 0; }

enum event_type {
	E_INVALID,
	E_SUBSCRIBE,
	E_RENEW
};

static void ProcessHTTPPOST_upnphttp(struct upnphttp * h);
static void SendResp_dlnafile(struct upnphttp *, char * url);
static void ProcessHttpQuery_upnphttp(struct upnphttp * h);
static void ParseHttpHeaders(struct upnphttp * h);
static void start_dlna_header(struct string_s *str, int respcode, const char *tmode, const char *mime);
static int send_data(struct upnphttp * h, char * header, size_t size, int flags);
static void send_file(struct upnphttp * h, int sendfd, off64_t offset, off64_t end_offset);
static void sendXMLdesc(struct upnphttp * h, int mode);
static void SendResp_icon(struct upnphttp * h);

static void Send406(struct upnphttp * h);
static void Send400(struct upnphttp * h);
static void Send404(struct upnphttp * h);

struct upnphttp *
New_upnphttp(int s)
{
	struct upnphttp * ret;
	if(s<0)
	{
		return NULL;
	}
	ret = (struct upnphttp *)malloc(sizeof(struct upnphttp));
	if(ret == NULL)
		return NULL;
	memset(ret, 0, sizeof(struct upnphttp));
	ret->socket = s;
	return ret;
}

static inline int
__attribute__((__format__ (__printf__, 2, 3)))
strcatf(struct string_s *str, const char *fmt, ...)
{
	int ret;
	int size;
	va_list ap;

	if (str->off >= str->size)
		return 0;

	va_start(ap, fmt);
	size = str->size - str->off;
	ret = vsnprintf(str->data + str->off, size, fmt, ap);
	str->off += MIN(ret, size);
	va_end(ap);

	return ret;
}

char* strstrc(const char *s, const char *p, const char t)
{
	char *endptr;
	size_t slen, plen;


	// strchr - 문자열에서 임의의 문자가 처음으로 발견된 위치를 구합니다.
	// 문자를 못찾으면 null 반환
	endptr = strchr(s, t);
	if (!endptr)
	{
		return strstr(s, p);
	}

	plen = strlen(p);
	slen = endptr - s;
	while (slen >= plen)
	{
		if (*s == *p && strncmp(s+1, p+1, plen-1) == 0)
		{
			return (char*)s;
		}
		s++;
		slen--;
	}

	return NULL;
}

//URL 한글 특수 문자열 디코딩
void UrlDecode(char *src, char *last, char *dest) {
	for(;src<=last;src++,dest++){
		if(*src=='%'){
			int code;
			if(sscanf(src+1, "%2x", &code)!=1) code='?';// 특수문자(한글)인 경우
			*dest = code;
			src+=2;
		}else if(*src=='+') {//공백문자인 경우
			*dest = ' ';
		}else {
			*dest = *src; //영문자인 경우
		}
	}
	*(++dest) = '\0';
}


//URL 문자열 인코딩
char* UrlEncode(char *str, char *result){
	char encstr[512];
	char buf[2+1];
	int i, j;
	unsigned char c;
	if(str == NULL) return NULL;
	//if((encstr = (char *)malloc((strlen(str) * 3) + 1)) == NULL) return NULL;

	for(i = j = 0; str[i] != '\0'; i++){

		c = (unsigned char)str[i];
		if (c == ' ') encstr[j++] = '+';
		else if ((c >= '0') && (c <= '9')) encstr[j++] = c;
		else if ((c >= 'A') && (c <= 'Z')) encstr[j++] = c;
		else if ((c >= 'a') && (c <= 'z')) encstr[j++] = c;
		else if ((c == '@') || (c == '.')) encstr[j++] = c;
		else {
			sprintf(buf, "%02x", c);
			encstr[j++] = '%';
			encstr[j++] = buf[0];
			encstr[j++] = buf[1];
		}
	}
	encstr[j] = '\0';
	strcpy(result, encstr);
	return result;
}

//파일 확장명 비교
static int FileExtCmp (char *filename, char *ext)
{

	int i, ext_len, filename_len, cnt=0;

	ext_len = strlen(ext); //확장자 길이
	filename_len = strlen(filename); //주어진 파일명 길이

	//확장명이 파일명보다 길 경우 리턴
	if(ext_len > filename_len)
		return 0;

	//파일명 끝에서 부터 확장명과 비교
	for(i = ext_len-1; i > 0; i--){
		if(ext[i] == filename[--filename_len])
		cnt++;
	}

	if(cnt == ext_len-1)
		return 1;
	else
		return 0;
}


void Process_upnphttp(struct upnphttp * h)
{
	char buf[2048];
	int n;

	if(!h)return;

	switch(h->state)
	{
	case 0:
		n = recv(h->socket, buf, 2048, 0); //접속된 소켓의 데이터를 수신
		if(n<0)
		{ //수신실패
			dlog_print(DLOG_ERROR, "tdlna_http", "http recv state: 0\n");
			h->state = 100;
		}
		else if(n==0)
		{
			h->state = 100; //처리가 완료된 요청은 state를 100으로 변경
		}
		else
		{
			int new_req_buflen;
			const char * endheaders;
			/* if 1st arg of realloc() is null,
			 * realloc behaves the same as malloc() */
			new_req_buflen = n + h->req_buflen + 1;

			//헤더가 너무 크면 처리하지 않는다. 
			if (new_req_buflen >= 1024 * 1024)
			{
				dlog_print(DLOG_DEBUG, "tdlna_http", "http 헤더가 너무 커서 처리하지 않음(%d byte)", new_req_buflen);
				h->state = 100;
				break;
			}

			h->req_buf = (char *)realloc(h->req_buf, new_req_buflen);
			if (!h->req_buf)
			{
				h->state = 100;
				break;
			}

			memcpy(h->req_buf + h->req_buflen, buf, n);
			h->req_buflen += n;
			h->req_buf[h->req_buflen] = '\0';

			/* search for the string "\r\n\r\n" */
			endheaders = strstr(h->req_buf, "\r\n\r\n");

			if(endheaders)
			{
				h->req_contentoff = endheaders - h->req_buf + 4;
				h->req_contentlen = h->req_buflen - h->req_contentoff;
				
				//★★★ 요청 처리 ★★★
				ProcessHttpQuery_upnphttp(h);
			}
		}
		break;
	case 1:
	case 2:
		n = recv(h->socket, buf, sizeof(buf), 0);

		if(n < 0)
		{
			dlog_print(DLOG_ERROR, "tdlna_http", "recv (state %d)\n", h->state);
			h->state = 100;
		}
		else if(n == 0)
		{
			dlog_print(DLOG_ERROR, "tdlna_http", "2_HTTP Connection closed unexpectedly\n");
			h->state = 100;
		}
		else
		{
			//★★ POST요청의 경우 요청 바디를 수신한다. ★★
	
			buf[sizeof(buf)-1] = '\0';
			/*fwrite(buf, 1, n, stdout);*/	/* debug */
			h->req_buf = (char *)realloc(h->req_buf, n + h->req_buflen);
			if (!h->req_buf)
			{
				dlog_print(DLOG_INFO, "tdlna_http", "http post 요청 본문 수신중 \n");
				h->state = 100;
				break;
			}

			memcpy(h->req_buf + h->req_buflen, buf, n);
			h->req_buflen += n;
			if((h->req_buflen - h->req_contentoff) >= h->req_contentlen)
			{
				/* Need the struct to point to the realloc'd memory locations */
				if( h->state == 1 )
				{
					ParseHttpHeaders(h);
					ProcessHTTPPOST_upnphttp(h);
				}
				else if( h->state == 2 )
				{

	 				ProcessHttpQuery_upnphttp(h);
				}
			}
		}
		break;
	default:
		dlog_print(DLOG_ERROR, "tdlna_http", "Unexpected state: %d\n", h->state);
	}
}

void Delete_upnphttp(struct upnphttp * h)
{
	if(h)
	{
		if(h->socket >= 0)
		{
			//printf("<CloseSocket_upnphttp>\n");
			CloseSocket_upnphttp(h);
		}
		free(h->req_buf);
		free(h->res_buf);
		free(h);
	}
}

void CloseSocket_upnphttp(struct upnphttp * h)
{
	if(close(h->socket) < 0)
	{
		printf("CloseSocket_upnphttp: close(%d)\n", h->socket);
	}
	h->socket = -1;
	h->state = 100;
}

// HTTP POST요청을 처리한다 upnp에서 post요청이 오는 경우는 soap프로토콜이 있다.
/* ProcessHTTPPOST_upnphttp()
 * executes the SOAP query if it is possible */
static void 
ProcessHTTPPOST_upnphttp(struct upnphttp * h)
{
	if((h->req_buflen - h->req_contentoff) >= h->req_contentlen)
	{
		if(h->req_soapAction)
		{
			// *** Post 요청을 처리한다. POST 요청은 여기에서 SOAP처리에 쓰인다.
			dlog_print(DLOG_DEBUG, "tdlna_http", "SOAPAction: %.*s\n", h->req_soapActionLen, h->req_soapAction);

			ExecuteSoapAction(h, h->req_soapAction,	h->req_soapActionLen);
		}
		else
		{
			static const char err400str[] =
				"<html><body>Bad request</body></html>";
			printf("No SOAPAction in HTTP headers\n");
			h->respflags = FLAG_HTML;
			BuildResp2_upnphttp(h, 400, "Bad Request",
			                    err400str, sizeof(err400str) - 1);
			SendResp_upnphttp(h);
			CloseSocket_upnphttp(h);
		}
	}
	else
	{
		//waiting for remaining data (post 본문까지 모두 전송받아야하므로 state를 1로 바꾸고 다음 루프에서 계속 처리)  
		h->state = 1;
	}
}

//Subscribe요청에서 NT헤더와 SID헤더를 보고 요청 타입을 판별
static int
check_event(struct upnphttp *h)
{
	enum event_type type;

	if (h->req_Callback)
	{
		if (h->req_SID || !h->req_NT)
		{
			BuildResp2_upnphttp(h, 400, "Bad Request",
				            "<html><body>Bad request</body></html>", 37);
			type = E_INVALID;
		}
		else if (strncmp(h->req_Callback, "http://", 7) != 0 ||
		         strncmp(h->req_NT, "upnp:event", h->req_NTLen) != 0)
		{
			/* Missing or invalid CALLBACK : 412 Precondition Failed.
			 * If CALLBACK header is missing or does not contain a valid HTTP URL,
			 * the publisher must respond with HTTP error 412 Precondition Failed*/
			BuildResp2_upnphttp(h, 412, "Precondition Failed", 0, 0);
			type = E_INVALID;
		}
		else
			type = E_SUBSCRIBE;
	}
	else if (h->req_SID)
	{
		/* subscription renew */
		if (h->req_NT)
		{
			BuildResp2_upnphttp(h, 400, "Bad Request",
				            "<html><body>Bad request</body></html>", 37);
			type = E_INVALID;
		}
		else
			type = E_RENEW;
	}
	else
	{
		BuildResp2_upnphttp(h, 412, "Precondition Failed", 0, 0);
		type = E_INVALID;
	}

	return type;
}

//Subscribe요청에 대한 처리 
static void
ProcessHTTPSubscribe_upnphttp(struct upnphttp * h, const char * path)
{
	const char * sid;
	enum event_type type;
	dlog_print(DLOG_DEBUG,"tdlna_http", "ProcessHTTPSubscribe %s\n", path);
	dlog_print(DLOG_DEBUG,"tdlna_http", "Callback '%.*s' Timeout=%d\n",
		h->req_CallbackLen, h->req_Callback, h->req_Timeout);
	printf("SID '%.*s'\n", h->req_SIDLen, h->req_SID);

	type = check_event(h);
	if (type == E_SUBSCRIBE)
	{
		/* - add to the subscriber list
		 * - respond HTTP/x.x 200 OK 
		 * - Send the initial event message */
		/* Server:, SID:; Timeout: Second-(xx|infinite) */
		sid = (const char*) upnpevents_addSubscriber(path, h->req_Callback, h->req_CallbackLen, h->req_Timeout);
		h->respflags = FLAG_TIMEOUT;
		if (sid)
		{
			printf("generated sid=%s\n", sid);
			h->respflags |= FLAG_SID;
			h->req_SID = sid;
			h->req_SIDLen = strlen(sid);
		}
		BuildResp_upnphttp(h, 0, 0);
	}
	else if (type == E_RENEW)
	{
		/* subscription renew */
		if (renewSubscription(h->req_SID, h->req_SIDLen, h->req_Timeout) < 0)
		{
			/* Invalid SID
			   412 Precondition Failed. If a SID does not correspond to a known,
			   un-expired subscription, the publisher must respond
			   with HTTP error 412 Precondition Failed. */
			BuildResp2_upnphttp(h, 412, "Precondition Failed", 0, 0);
		}
		else
		{
			/* A DLNA device must enforce a 5 minute timeout */
			h->respflags = FLAG_TIMEOUT;
			h->req_Timeout = 300;
			h->respflags |= FLAG_SID;
			BuildResp_upnphttp(h, 0, 0);
		}
	}
	SendResp_upnphttp(h);
	CloseSocket_upnphttp(h);
}

//UNSubscribe요청에 대한 처리 
static void
ProcessHTTPUnSubscribe_upnphttp(struct upnphttp * h, const char * path)
{

	enum event_type type;
	dlog_print(DLOG_DEBUG,"tdlna_http","ProcessHTTPUnSubscribe %s\n", path);
	dlog_print(DLOG_DEBUG,"tdlna_http","SID '%.*s'\n", h->req_SIDLen, h->req_SID);

	// Remove from the list 
	type = check_event(h);
	if (type != E_INVALID)
	{

		if(upnpevents_removeSubscriber(h->req_SID, h->req_SIDLen) < 0)
			BuildResp2_upnphttp(h, 412, "Precondition Failed", 0, 0);
		else
			BuildResp_upnphttp(h, 0, 0);
	}
	SendResp_upnphttp(h);
	CloseSocket_upnphttp(h);
}


/* Parse and process Http Query
 * 파서 및 http 쿼리 처리
 * called once all the HTTP headers have been received.
 */
static void ProcessHttpQuery_upnphttp(struct upnphttp * h)
{
	char HttpCommand[16];
	char HttpUrl[512];
	char * HttpVer;
	char * p;
	int i;

	p = h->req_buf;
	//h->req_buf not return
	if(!p)
	{
		return;
	}

	// html Require Method
	// 메서드는 요청의 종류를 서버에게 알려주기 위해서 사용한다. 다음은 요청에 사용할 수 있는 메서드들이다.
	// GET, POST, PUT, DELETE, HEAD, OPTIONS, TRACE
	for(i = 0; (i<15) && (*p) && (*p != ' ') && (*p != '\r'); i++)
	{
		HttpCommand[i] = *(p++);
	}
	HttpCommand[i] = '\0';

	//blink del
	while(*p==' ')
	{
		p++;
	}

	// html Require URI
	if(strncmp(p, "http://", 7) == 0)
	{
		p = p+7;
		while(*p!='/')
		{
			p++;
		}
	}
	// p not, blink not, carriage not -> i++
	for(i = 0; i<511 && (*p) && (*p != ' ') && (*p != '\r'); i++)
	{
		HttpUrl[i] = *(p++);
	}
	HttpUrl[i] = '\0';
	while(*p==' ')
	{
		p++;
	}

	// html Require Version
	HttpVer = h->HttpVer;
	for(i = 0; i<15 && *p && *p != '\r'; i++)
	{
		HttpVer[i] = *(p++);
	}
	HttpVer[i] = '\0';
/*
	printf("------------------------------------------\n");
	printf("%s\n",h->req_buf);
	printf("------------------------------------------\n");
	printf("HttpCommand - %s\n",HttpCommand);
	printf("------------------------------------------\n");
	printf("HttpUrl - %s\n",HttpUrl);
	printf("------------------------------------------\n");
	printf("HttpVer - %s\n",HttpVer);
	printf("------------------------------------------\n");
*/
	/*DPRINTF(E_INFO, L_HTTP, "HTTP REQUEST : %s %s (%s)\n",HttpCommand, HttpUrl, HttpVer);*/
	/* set the interface here initially, in case there is no Host header */
	// 처음 여기 인터페이스 설정, 경우에는 호스트 헤더가 없습니다
	for(i = 0; i<n_lan_addr; i++)
	{
		if( (h->clientaddr.s_addr & lan_addr[i].mask.s_addr) == (lan_addr[i].addr.s_addr & lan_addr[i].mask.s_addr))
		{
			h->iface = i;
			break;
		}
	}

	ParseHttpHeaders(h); //http헤더를 파싱하여 h에 각 속성들을 채워넣는다.

	/* see if we need to wait for remaining data */
	if( (h->reqflags & FLAG_CHUNKED) )
		{
			if( h->req_chunklen == -1)
			{
				Send400(h);
				return;
			}
			if( h->req_chunklen )
			{
				h->state = 2;
				return;
			}
			char *chunkstart, *chunk, *endptr, *endbuf;
			chunk = endbuf = chunkstart = h->req_buf + h->req_contentoff;

			while( (h->req_chunklen = strtol(chunk, &endptr, 16)) && (endptr != chunk) )
			{
				endptr = strstr(endptr, "\r\n");
				if (!endptr)
				{
					Send400(h);
					return;
				}
				endptr += 2;

				memmove(endbuf, endptr, h->req_chunklen);

				endbuf += h->req_chunklen;
				chunk = endptr + h->req_chunklen;
			}
			h->req_contentlen = endbuf - chunkstart;
			h->req_buflen = endbuf - h->req_buf;
			h->state = 100;
		}

		if(strcmp("POST", HttpCommand) == 0) //POST 메소드에 대해 처리
		{
			h->req_command = EPost;
			ProcessHTTPPOST_upnphttp(h);
		}
		else if((strcmp("GET", HttpCommand) == 0) || (strcmp("HEAD", HttpCommand) == 0))
		{
			if( ((strcmp(h->HttpVer, "HTTP/1.1")==0) && !(h->reqflags & FLAG_HOST)) || (h->reqflags & FLAG_INVALID_REQ) )
			{
			//	DPRINTF(E_WARN, L_HTTP, "Invalid request, responding ERROR 400.  (No Host specified in HTTP headers?)\n");
				Send400(h);
				return;
			}

			/* 7.3.33.4 */
			else if( (h->reqflags & (FLAG_TIMESEEK|FLAG_PLAYSPEED)) && !(h->reqflags & FLAG_RANGE) )
			{
			//	DPRINTF(E_WARN, L_HTTP, "DLNA %s requested, responding ERROR 406\n", h->reqflags&FLAG_TIMESEEK ? "TimeSeek" : "PlaySpeed");
				Send406(h);
				return;
			}
			else if(strcmp("GET", HttpCommand) == 0)
			{
				h->req_command = EGet;
			}
			else
			{
				h->req_command = EHead;
			}


			//url 경로를 보고 필요한 프로토콜에 대한 처리 or http 요청 처리
			if(strcmp(HttpUrl, ROOTDESC_PATH) == 0) //RootDisc(Discription) Xml 전송
			{
				dlog_print(DLOG_DEBUG, "tdlna_http", "RootDisc.XML 요청 수신 (%s)", inet_ntoa(h->clientaddr));
				sendXMLdesc(h, 0);
			}

			else if(strcmp(HttpUrl, "/ContentDir.xml") == 0)
			{
				dlog_print(DLOG_DEBUG, "tdlna_http", "ContentDir.xml 요청 수신 (%s)", inet_ntoa(h->clientaddr));
				sendXMLdesc(h, 1);
			}
			else if(strcmp(HttpUrl, "/ConnectionMgr.xml") == 0)
			{
				dlog_print(DLOG_DEBUG, "tdlna_http", "ConnectionMgr.xml 요청 수신 (%s)", inet_ntoa(h->clientaddr));
				sendXMLdesc(h, 2);
			}
			else if(strncmp(HttpUrl, "/icons/", 7) == 0)			//아이콘
			{
				//strcpy(filePath, HOME_DIR);
				//strcat(filePath, HttpUrl);
				//SendResp_dlnafile(h, filePath); //파일요청 처리
				SendResp_icon(h); //아이콘 데이터 보내기
				dlog_print(DLOG_DEBUG, "tdlna_http", "아이콘 파일 요청처리 (%s)", HttpUrl, inet_ntoa(h->clientaddr));
			}
			else if(strcmp(HttpUrl, "/") == 0)
			{
				Send404(h);
			}
			else if(FileExtCmp(HttpUrl, "ALBUM")){ //mp3 앨범아트 요청시
				_META* mData;
				int mRet;
				char* mPtr;

				UrlDecode(HttpUrl, HttpUrl+strlen(HttpUrl), HttpUrl);
				mPtr = strstr(HttpUrl, ".ALBUM");

				if(mPtr == NULL){
					Send404(h);
					return;
				}
				*mPtr = '\0'; //확장자인 .ALBUM을 제거한다

				mRet = Meta_Get_from_path(NULL, strcat(HttpUrl,"%"), 1, &mData); //파일명으로 미디어 메타정보를 가져온 후
				dlog_print(DLOG_DEBUG,"tdlna_http","삼성티비 앨범아트 요청 : %d",mRet);
				if(mRet < 1) {
					free(mData);
					Send404(h);
				}else{
					strcpy(HttpUrl, mData[0].thumbnail_path); //섬네일 이미지 경로로 파일을열어 보내준다.
					SendResp_dlnafile(h, HttpUrl); //파일요청 처리
					free(mData);
				}
			}
			else
			{
				//strcpy(filePath, HOME_DIR);
				//strcat(filePath, HttpUrl);
				UrlDecode(HttpUrl, HttpUrl+strlen(HttpUrl), HttpUrl);

				SendResp_dlnafile(h, HttpUrl); //파일요청 처리
				dlog_print(DLOG_DEBUG, "tdlna_http", "파일 요청 %s (%s)", HttpUrl, inet_ntoa(h->clientaddr));
			}
		}
		else if(strcmp("SUBSCRIBE", HttpCommand) == 0)  //SUBSCRIBE 요청이 오면 ssid가 포함된 헤더를 보내준다.
		{
			dlog_print(DLOG_DEBUG, "tdlna_http", "SUBSCRIBE 요청");
			h->req_command = ESubscribe;
			ProcessHTTPSubscribe_upnphttp(h, HttpUrl);
		}
		else if(strcmp("UNSUBSCRIBE", HttpCommand) == 0)
		{
			dlog_print(DLOG_DEBUG, "tdlna_http", "UnSUBSCRIBE 요청");
			h->req_command = EUnSubscribe;
			ProcessHTTPUnSubscribe_upnphttp(h, HttpUrl);
		}
		else
		{
			dlog_print(DLOG_DEBUG, "tdlna_http", "알 수 없는 요청");
			printf("Unsupported HTTP Command %s\n", HttpCommand);
			Send501(h);
		}
}


/* HTTP요청 헤더를 파싱한다 */
static void ParseHttpHeaders(struct upnphttp * h)
{
	int client = 0;
	char * line;
	char * colon;
	char * p;
	int n;
	line = h->req_buf;

	/* TODO : check if req_buf, contentoff are ok */
	while(line < (h->req_buf + h->req_contentoff))
	{
		colon = strchr(line, ':');
		if(colon)
		{
			if(strncasecmp(line, "Content-Length", 14)==0)
			{
				p = colon;
				while(*p && (*p < '0' || *p > '9'))
					p++;
				h->req_contentlen = atoi(p);
			}
			else if(strncasecmp(line, "SOAPAction", 10)==0)
			{
				p = colon;
				n = 0;
				while(*p == ':' || *p == ' ' || *p == '\t')
					p++;
				while(p[n] >= ' ')
					n++;
				if(n >= 2 &&
				   ((p[0] == '"' && p[n-1] == '"') ||
				    (p[0] == '\'' && p[n-1] == '\'')))
				{
					p++;
					n -= 2;
				}
				h->req_soapAction = p;
				h->req_soapActionLen = n;
			}
			else if(strncasecmp(line, "Callback", 8)==0)
			{
				p = colon;
				while(*p && *p != '<' && *p != '\r' )
					p++;
				n = 0;
				while(p[n] && p[n] != '>' && p[n] != '\r' )
					n++;
				h->req_Callback = p + 1;
				h->req_CallbackLen = MAX(0, n - 1);
			}
			else if(strncasecmp(line, "SID", 3)==0)
			{
				//zqiu: fix bug for test 4.0.5
				//Skip extra headers like "SIDHEADER: xxxxxx xxx"
				for(p=line+3;p<colon;p++)
				{
					if(!isspace(*p))
					{
						p = NULL; //unexpected header
						break;
					}
				}
				if(p) {
					p = colon + 1;
					while(isspace(*p))
						p++;
					n = 0;
					while(p[n] && !isspace(p[n]))
						n++;
					h->req_SID = p;
					h->req_SIDLen = n;
				}
			}
			else if(strncasecmp(line, "NT", 2)==0)
			{
				p = colon + 1;
				while(isspace(*p))
					p++;
				n = 0;
				while(p[n] && !isspace(p[n]))
					n++;
				h->req_NT = p;
				h->req_NTLen = n;
			}
			else if(strncasecmp(line, "Timeout", 7)==0)
			{
				p = colon + 1;
				while(isspace(*p))
					p++;
				if(strncasecmp(p, "Second-", 7)==0) {
					h->req_Timeout = atoi(p+7);
				}
			}
			// Range: bytes=xxx-yyy
			else if(strncasecmp(line, "Range", 5)==0)
			{
				p = colon + 1;
				while(isspace(*p))
					p++;
				if(strncasecmp(p, "bytes=", 6)==0) {
					h->reqflags |= FLAG_RANGE;
					h->req_RangeStart = strtoll(p+6, &colon, 10);
					h->req_RangeEnd = colon ? atoll(colon+1) : 0;
				}

				dlog_print(DLOG_DEBUG,"tdlna_http", "Range - %d %d %d\n", h->reqflags, (int)h->req_RangeStart, (int)h->req_RangeEnd);
			}
			else if(strncasecmp(line, "Host", 4)==0)
			{
				int i;
				h->reqflags |= FLAG_HOST;
				p = colon + 1;
				while(isspace(*p))
					p++;
				for(n = 0; n<n_lan_addr; n++)
				{
					for(i=0; lan_addr[n].str[i]; i++)
					{
						if(lan_addr[n].str[i] != p[i])
							break;
					}
					if(!lan_addr[n].str[i])
					{
						h->iface = n;
						break;
					}
				}
			}
			else if(strncasecmp(line, "User-Agent", 10)==0)
			{
				int i;
				/* Skip client detection if we already detected it. */
				if( client )
					goto next_header;
				p = colon + 1;

				//공백 문자(공백, 개행('\n'), 종이넘기('\f'), 탭('\t', '\v'), 복귀('\r') 문자인지를 판별
				while(isspace(*p))
				{
					p++;
				}

				for (i = 0; client_types[i].name; i++)
				{
					if (client_types[i].match_type != EUserAgent)
						continue;
					if (strstrc(p, client_types[i].match, '\r') != NULL)
					{
						client = i;
						break;
					}
				}
//				dlog_print(DLOG_ERROR, "tdlna_http", "클라이언트: %s",  client_types[i].match);
			}
			else if(strncasecmp(line, "X-AV-Client-Info", 16)==0)
			{
				int i;
				// Skip client detection if we already detected it. 
				if( client && client_types[client].type < EStandardDLNA150 )
					goto next_header;
				p = colon + 1;
				while(isspace(*p))
					p++;
				for (i = 0; client_types[i].name; i++)
				{
					if (client_types[i].match_type != EXAVClientInfo)
						continue;
					if (strstrc(p, client_types[i].match, '\r') != NULL)
					{
						client = i;
						break;
					}
				}
			}
			else if(strncasecmp(line, "Transfer-Encoding", 17)==0)
			{
				p = colon + 1;
				while(isspace(*p))
					p++;
				if(strncasecmp(p, "chunked", 7)==0)
				{
					h->reqflags |= FLAG_CHUNKED;
				}
			}
			else if(strncasecmp(line, "Accept-Language", 15)==0)
			{
				h->reqflags |= FLAG_LANGUAGE;
			}
			else if(strncasecmp(line, "getcontentFeatures.dlna.org", 27)==0)
			{
				p = colon + 1;
				while(isspace(*p))
					p++;
				if( (*p != '1') || !isspace(p[1]) )
					h->reqflags |= FLAG_INVALID_REQ;
			}
			else if(strncasecmp(line, "TimeSeekRange.dlna.org", 22)==0)
			{
				h->reqflags |= FLAG_TIMESEEK;
			}
			else if(strncasecmp(line, "PlaySpeed.dlna.org", 18)==0)
			{
				h->reqflags |= FLAG_PLAYSPEED;
			}
			else if(strncasecmp(line, "realTimeInfo.dlna.org", 21)==0)
			{
				h->reqflags |= FLAG_REALTIMEINFO;
			}
			else if(strncasecmp(line, "getAvailableSeekRange.dlna.org", 21)==0)
			{
				p = colon + 1;
				while(isspace(*p))
					p++;
				if( (*p != '1') || !isspace(p[1]) )
					h->reqflags |= FLAG_INVALID_REQ;
			}
			else if(strncasecmp(line, "transferMode.dlna.org", 21)==0)
			{
				p = colon + 1;
				while(isspace(*p))
					p++;
				if(strncasecmp(p, "Streaming", 9)==0)
				{
					h->reqflags |= FLAG_XFERSTREAMING;
				}
				if(strncasecmp(p, "Interactive", 11)==0)
				{
					h->reqflags |= FLAG_XFERINTERACTIVE;
				}
				if(strncasecmp(p, "Background", 10)==0)
				{
					h->reqflags |= FLAG_XFERBACKGROUND;
				}
			}
			else if(strncasecmp(line, "getCaptionInfo.sec", 18)==0)
			{
				h->reqflags |= FLAG_CAPTION;
			}
			else if(strncasecmp(line, "FriendlyName", 12)==0)
			{
				int i;
				p = colon + 1;
				while(isspace(*p))
					p++;
				for (i = 0; client_types[i].name; i++)
				{
					if (client_types[i].match_type != EFriendlyName)
						continue;
					if (strstrc(p, client_types[i].match, '\r') != NULL)
					{
						client = i;
						break;
					}
				}
			}
			else if(strncasecmp(line, "uctt.upnp.org:", 14)==0)
			{
				/* Conformance testing */
				SETFLAG(DLNA_STRICT_MASK);
			}
		}
next_header:
		line = strstr(line, "\r\n");
		if (!line)
		{
			return;
		}
		line += 2;
	}//while

	if( h->reqflags & FLAG_CHUNKED )
	{
		char *endptr;
		h->req_chunklen = -1;
		if( h->req_buflen <= h->req_contentoff )
		{
			return;
		}

		while( (line < (h->req_buf + h->req_buflen)) && (h->req_chunklen = strtol(line, &endptr, 16)) && (endptr != line) )
		{
			endptr = strstr(endptr, "\r\n");
			if (!endptr)
			{
				return;
			}
			line = endptr+h->req_chunklen+2;
		}

		if( endptr == line )
		{
			h->req_chunklen = -1;
			return;
		}
	}


		// If the client type wasn't found, search the cache.
		// This is done because a lot of clients like to send a
		// different User-Agent with different types of requests. /
	//serch_client_cache
		h->req_client = SearchClientCache(h->clientaddr, 0);

	//--Add this client to the cache if it's not there already. /
	//add_client_cache
	if (!h->req_client)
	{
		h->req_client = AddClientCache(h->clientaddr, client);
	}
	else if (client)
	{
		enum client_types type = client_types[client].type;
		enum client_types ctype = h->req_client->type->type;
		//- If we know the client and our new detection is generic, use our cached info /
		//- If we detected a Samsung Series B earlier, don't overwrite it with Series A info /
		if ((ctype && ctype < EStandardDLNA150 && type >= EStandardDLNA150) ||
		    (ctype == ESamsungSeriesB && type == ESamsungSeriesA))
			return;
		h->req_client->type = &client_types[client];
		h->req_client->age = time(NULL);
	}
} 

//400(잘못된 요청): 서버가 요청의 구문을 인식하지 못했다.
static void
Send400(struct upnphttp * h)
{
	dlog_print(DLOG_ERROR,"tdlna_http","HTTP 400 ");

	static const char body400[] =
		"<HTML><HEAD><TITLE>400 Bad Request</TITLE></HEAD>"
		"<BODY><H1>Bad Request</H1>The request is invalid"
		" for this HTTP version.</BODY></HTML>\r\n";
	h->respflags = FLAG_HTML;
	BuildResp2_upnphttp(h, 400, "Bad Request", body400, sizeof(body400) - 1);
	SendResp_upnphttp(h);
	CloseSocket_upnphttp(h);
}

//404(찾을 수 없음): 서버가 요청한 페이지를 찾을 수 없다.
static void
Send404(struct upnphttp * h)
{
	dlog_print(DLOG_ERROR,"tdlna_http","HTTP 404 ");

	static const char body404[] =
		"<HTML><HEAD><TITLE>404 Not Found</TITLE></HEAD>"
		"<BODY><H1>Not Found</H1>The requested URL was not found"
		" on this server.</BODY></HTML>\r\n";
	h->respflags = FLAG_HTML;
	BuildResp2_upnphttp(h, 404, "Not Found", body404, sizeof(body404) - 1);
	SendResp_upnphttp(h);
	CloseSocket_upnphttp(h);
}

//406(허용되지 않음): 요청한 페이지가 요청한 콘텐츠 특성으로 응답할 수 없다.
static void
Send406(struct upnphttp * h)
{
	dlog_print(DLOG_ERROR,"tdlna_http","HTTP 406 ");

	static const char body406[] =
		"<HTML><HEAD><TITLE>406 Not Acceptable</TITLE></HEAD>"
		"<BODY><H1>Not Acceptable</H1>An unsupported operation"
		" was requested.</BODY></HTML>\r\n";
	h->respflags = FLAG_HTML;
	BuildResp2_upnphttp(h, 406, "Not Acceptable", body406, sizeof(body406) - 1);
	SendResp_upnphttp(h);
	CloseSocket_upnphttp(h);
}

//416(처리할 수 없는 요청범위): 요청이 페이지에서 처리할 수 없는 범위에 해당되는 경우 서버는 이 상태 코드를 표시한다.
static void
Send416(struct upnphttp * h)
{
	dlog_print(DLOG_ERROR,"tdlna_http","HTTP 416 ");

	static const char body416[] =
		"<HTML><HEAD><TITLE>416 Requested Range Not Satisfiable</TITLE></HEAD>"
		"<BODY><H1>Requested Range Not Satisfiable</H1>The requested range"
		" was outside the file's size.</BODY></HTML>\r\n";
	h->respflags = FLAG_HTML;
	BuildResp2_upnphttp(h, 416, "Requested Range Not Satisfiable",
	                    body416, sizeof(body416) - 1);
	SendResp_upnphttp(h);
	CloseSocket_upnphttp(h);
}

//500(내부 서버 오류): 서버에 오류가 발생하여 요청을 수행할 수 없다.
void
Send500(struct upnphttp * h)
{
	dlog_print(DLOG_ERROR,"tdlna_http","HTTP 500 ");

	static const char body500[] =
		"<HTML><HEAD><TITLE>500 Internal Server Error</TITLE></HEAD>"
		"<BODY><H1>Internal Server Error</H1>Server encountered "
		"and Internal Error.</BODY></HTML>\r\n";
	h->respflags = FLAG_HTML;
	BuildResp2_upnphttp(h, 500, "Internal Server Errror",
	                    body500, sizeof(body500) - 1);
	SendResp_upnphttp(h);
	CloseSocket_upnphttp(h);
}

//501(구현되지 않음): 서버에 요청을 수행할 수 있는 기능이 없다. (요청 메소드를 인식하지 못할 때)
void
Send501(struct upnphttp * h)
{
	dlog_print(DLOG_ERROR,"tdlna_http","HTTP 501 ");

	static const char body501[] = 
		"<HTML><HEAD><TITLE>501 Not Implemented</TITLE></HEAD>"
		"<BODY><H1>Not Implemented</H1>The HTTP Method "
		"is not implemented by this server.</BODY></HTML>\r\n";
	h->respflags = FLAG_HTML;
	BuildResp2_upnphttp(h, 501, "Not Implemented",
	                    body501, sizeof(body501) - 1);
	SendResp_upnphttp(h);
	CloseSocket_upnphttp(h);
}

// rootDesc.xml 요청에 대해 xml data전송
static void
sendXMLdesc(struct upnphttp * h, int mode)
{
	char * desc;
	int len = 8192;
	desc = malloc(len);

	switch(mode){
	case 1: //ContentDir.xml
		len = sprintf(desc, CONTENT_DIR_XML);
		dlog_print(DLOG_INFO, "tdlna_http", "→ Send ContentDir.xml");
		break;
	case 2: //ConnectionMgr.xml
		len = sprintf(desc, CONNECTION_MGR_XML);
		dlog_print(DLOG_INFO, "tdlna_http", "→ Send ConnectionMgr.xml");
		break;
	default:
		//rootDesc.xml 빌드
		len = sprintf(desc, ROOT_DESC_XML, modelname, uuidvalue, lan_addr[0].str , runtime_vars.port );
		dlog_print(DLOG_DEBUG, "tdlna_http", "→ Send RootDisc.xml");
		break;
	}

	if(len < 1)
	{
		dlog_print(DLOG_DEBUG, "tdlna_http", "Failed to generate XML description");
		Send500(h);
		return;
	}
	BuildResp_upnphttp(h, desc, len);
	SendResp_upnphttp(h);
	CloseSocket_upnphttp(h);
	free(desc);
}

//아이콘 파일 요청시 아이콘 데이터 보내기
static void
SendResp_icon(struct upnphttp * h)
{
	char header[512];
	char mime[12] = "image/png";
	char data[] = ICON_PNG; //미리 정의된 아이콘 png파일 데이터
	int size;
	struct string_s str;

	size = sizeof(data);

	INIT_STR(str, header);

	start_dlna_header(&str, 200, "Interactive", mime);
	strcatf(&str, "Content-Length: %d\r\n\r\n", size);

	if( send_data(h, str.data, str.off, MSG_MORE) == 0 )
	{
		if( h->req_command != EHead )
			send_data(h, data, size, 0);
	}
	CloseSocket_upnphttp(h);
}

//응답헤더 빌드
void
BuildResp2_upnphttp(struct upnphttp * h, int respcode,
                    const char * respmsg,
                    const char * body, int bodylen)
{
	BuildHeader_upnphttp(h, respcode, respmsg, bodylen);
	if( h->req_command == EHead )
		return;
	if(body)
		memcpy(h->res_buf + h->res_buflen, body, bodylen);
	h->res_buflen += bodylen;
}

// 200 OK의 응답 헤더를 만든다.
void
BuildResp_upnphttp(struct upnphttp *h, const char *body, int bodylen)
{
	BuildResp2_upnphttp(h, 200, "OK", body, bodylen);
}

// http 응답 전송 (response)
void
SendResp_upnphttp(struct upnphttp * h)
{
	int n;
	//DPRINTF(E_DEBUG, L_HTTP, "HTTP RESPONSE: %.*s\n", h->res_buflen, h->res_buf);
	n = send(h->socket, h->res_buf, h->res_buflen, 0);
}

// Http 응답 코드와 헤더를 생성
void BuildHeader_upnphttp(struct upnphttp * h, int respcode,
                     const char * respmsg,
                     int bodylen)
{
	static const char httpresphead[] =
		"%s %d %s\r\n"
		"Content-Type: %s\r\n"
		"Connection: close\r\n"
		"Content-Length: %d\r\n"
		"Server: " MINIDLNA_SERVER_STRING "\r\n";
	time_t curtime = time(NULL);
	char date[30];
	int templen;
	struct string_s res;
	if(!h->res_buf)
	{
		templen = sizeof(httpresphead) + 256 + bodylen;
		h->res_buf = (char *)malloc(templen);
		h->res_buf_alloclen = templen;
	}
	res.data = h->res_buf;
	res.size = h->res_buf_alloclen;
	res.off = 0;
	strcatf(&res, httpresphead, "HTTP/1.1",
	              respcode, respmsg,
	              (h->respflags&FLAG_HTML)?"text/html":"text/xml; charset=\"utf-8\"",
							 bodylen);
	/* Additional headers */
	if(h->respflags & FLAG_TIMEOUT) {
		strcatf(&res, "Timeout: Second-");
		if(h->req_Timeout) {
			strcatf(&res, "%d\r\n", h->req_Timeout);
		} else {
			strcatf(&res, "300\r\n");
		}
	}
	if(h->respflags & FLAG_SID) {
		strcatf(&res, "SID: %.*s\r\n", h->req_SIDLen, h->req_SID);
	}
	if(h->reqflags & FLAG_LANGUAGE) {
		strcatf(&res, "Content-Language: en\r\n");
	}
	strftime(date, 30,"%a, %d %b %Y %H:%M:%S GMT" , gmtime(&curtime));
	strcatf(&res, "Date: %s\r\n", date);
	strcatf(&res, "EXT:\r\n");
	strcatf(&res, "\r\n");
	h->res_buflen = res.off;
	if(h->res_buf_alloclen < (h->res_buflen + bodylen))
	{
		h->res_buf = (char *)realloc(h->res_buf, (h->res_buflen + bodylen));
		h->res_buf_alloclen = h->res_buflen + bodylen;
	}
}


//확장자 대문자->소문자
void strLwr(char* str){
	int i;
	for(i=0; str[i] != '\0' && i<20; i++){
		if('A' <= str[i] && str[i]<= 'Z'){
			str[i] = str[i] + ('a'-'A');
		}
	}
}

void SimpleGetMimeStr(char* mimeStr, char* path){

	int i, path_len;
	char* ext = NULL;
	char lExt[10];

	path_len = strlen(path); //주어진 파일명 길이

	//확장자 구하기
	for(i = path_len-1; 1 < i; i--){
		if(path[i] == '.'){
			ext = &path[i];
			break;
		}
	}

	if(ext == NULL){
		strcpy(mimeStr,"text/html"); //http 기본값
	}
	else{
		strcpy(lExt, ext); //원본 유지를 위해 복사해서 비교함 걍
		strLwr(lExt); //확장자 소문자로 변환
		ext = lExt;

		if(!strcmp(ext, ".xml")){
			strcpy(mimeStr,"application/xml");
		}
		else if(!strcmp(ext, ".mkv")){
			strcpy(mimeStr, "video/x-mkv");
		}
		else if(!strcmp(ext, ".mp4")){
			strcpy(mimeStr, "video/mpeg");
		}
		else if(!strcmp(ext, ".avi")){
			strcpy(mimeStr, "video/x-msvideo");
		}
		else if(!strcmp(ext, ".wmv")){
			strcpy(mimeStr, "video/x-ms-wmv");
		}
		else if(!strcmp(ext, ".png")){
			strcpy(mimeStr, "image/png");
		}
		else if(!strcmp(ext, ".jpg") || !strcmp(ext, ".jpeg")){
			strcpy(mimeStr, "image/jpeg");
		}
		else if(!strcmp(ext, ".mp3")){
			strcpy(mimeStr, "audio/mpeg");
		}
		else if(!strcmp(ext, ".ogg")){
			strcpy(mimeStr, "audio/ogg");
		}
		else if(!strcmp(ext, ".smi")){
			strcpy(mimeStr, "application/smil+xml");
		}
		else
		{
			strcpy(mimeStr,"text/html"); //http 기본값
		}
	}

	//dlog_print(DLOG_ERROR, "tdlna_http", "마임타입 %s", mimeStr);
}

void ExtModStrCpy(char* orgPath, char* ext){
	int i, j;
	for(i=0; orgPath[i] != '\0'; i++);
	for(; 0<i; i--) if(orgPath[i] == '.') break;
	i++;

	for(j=0; ext[j] != '\0'; j++){
		orgPath[i++] = ext[j];
	}
	orgPath[i] = '\0';
}

//파일 요청에 대해 해당하는 경로(object로 넘어옴)의 파일을 가져와 네트워크 스트림으로 전송한다.
static void SendResp_dlnafile(struct upnphttp *h, char *object)
{
	char header[1024];
	char captionPath[512];
	struct string_s str;
//	char buf[128];
//	char **result;
//	int rows, ret;
	off64_t total, offset, size;
//	int64_t id;
	int sendfh, captionfh;
	uint32_t dlna_flags = DLNA_FLAG_DLNA_V1_5|DLNA_FLAG_HTTP_STALLING|DLNA_FLAG_TM_B;
//	uint32_t cflags = h->req_client ? h->req_client->type->flags : 0;
	const char *tmode;
//	enum client_types ctype = h->req_client ? h->req_client->type->type : 0;
	static struct { int64_t id;
	                enum client_types client;
	                char path[PATH_MAX];
	                char mime[32];
	                char dlna[96];
	              } last_file = { 0, 0 };
#if USE_FORK
	pid_t newpid = 0;
#endif

#if USE_FORK
	//newpid = process_fork(h->req_client);

	newpid = fork();
	//printf("newpidnewpid - %d\n",newpid);
	if( newpid > 0 )
	{
		CloseSocket_upnphttp(h);
		return;
	}
#endif
	//DPRINTF(E_INFO, L_HTTP, "Serving DetailID: %lld [%s]\n", (long long)id, last_file.path);

	offset = h->req_RangeStart; //range 요청시 시작 바이트만큼 offset 이동
	
	//strcat(last_file.path, HOME_DIR);
	strcpy(last_file.path, object); //파일경로 복사

	sendfh = open(last_file.path, O_RDONLY); //로컬 파일을 여는부분
	
	SimpleGetMimeStr(last_file.mime, last_file.path); //mimetype구하기

	dlog_print(DLOG_DEBUG, "tdlna_http", "HTTP MIME = %s", last_file.mime);
	//strcpy(last_file.dlna,"DLNA.ORG_PN=AVC_MP4_HP_HD_AAC;");

	if( sendfh < 0 ) {
	//	DPRINTF(E_ERROR, L_HTTP, "Error opening %s\n", last_file.path);
		Send404(h);
		dlog_print(DLOG_ERROR, "tdlna_http", "http로 요청한 파일을 로컬에서 열지못함 ㅠ %s", last_file.path);
		goto error;
	}
	size = lseek64(sendfh, 0, SEEK_END);
	lseek64(sendfh, 0, SEEK_SET); //파일사이즈 구하기
	if(size < 0){
		dlog_print(DLOG_ERROR, "tdlna_http", "fileSize: %jd", (intmax_t)size);
		Send500(h);
	}
	INIT_STR(str, header);

#if USE_FORK
	if( (h->reqflags & FLAG_XFERBACKGROUND) && (setpriority(PRIO_PROCESS, 0, 19) == 0) )
		tmode = "Background";
	else
#endif
	if( strncmp(last_file.mime, "image", 5) == 0 )
		tmode = "Interactive";
	else
		tmode = "Streaming";

	start_dlna_header(&str, (h->reqflags & FLAG_RANGE ? 206 : 200), tmode, last_file.mime);

	if( h->reqflags & FLAG_RANGE )
	{
		if( !h->req_RangeEnd || h->req_RangeEnd == size )
		{
			h->req_RangeEnd = size - 1;
		}
		if( (h->req_RangeStart > h->req_RangeEnd))
		{
		//	DPRINTF(E_WARN, L_HTTP, "Specified range was invalid!\n");
			Send400(h);
			close(sendfh);
			goto error;
		}
		if( h->req_RangeEnd >= size )
		{
		//	DPRINTF(E_WARN, L_HTTP, "Specified range was outside file boundaries!\n");
			Send416(h);
			close(sendfh);
			goto error;
		}

		total = h->req_RangeEnd - h->req_RangeStart + 1;
		strcatf(&str, "Content-Length: %jd\r\n"
		              "Content-Range: bytes %jd-%jd/%jd\r\n",
		              (intmax_t)total, (intmax_t)h->req_RangeStart,
		              (intmax_t)h->req_RangeEnd, (intmax_t)size);
	}
	else
	{
		h->req_RangeEnd = size - 1;
		total = size;
		strcatf(&str, "Content-Length: %jd\r\n", (intmax_t)total);
	}

	switch( *last_file.mime )
	{
		case 'i':
			dlna_flags |= DLNA_FLAG_TM_I;
			break;
		case 'a':
		case 'v':
		default:
			dlna_flags |= DLNA_FLAG_TM_S;
			break;
	}

	//자막요청 헤더가 있을 시 자막파일 정보를 헤더에 포함
	if(h->reqflags & FLAG_CAPTION)
	{ //UrlDecode(filePath, filePath+strlen(filePath), filePath);
		strcpy(captionPath, last_file.path);
		ExtModStrCpy(captionPath, "smi");
		//dlog_print(DLOG_INFO, "tdlna_http", "♣ 자막파일 확인 %s",captionPath);
		captionfh = open(captionPath, O_RDONLY); //자막파일 존재여부 확인
		if(!(captionfh < 0)){
			dlog_print(DLOG_INFO, "tdlna_http", "♣ 자막파일 존재[%d] %s", captionfh, captionPath);
			UrlEncode(captionPath, captionPath); //자막주소 URL인코딩
			strcatf(&str, "CaptionInfo.sec: http://%s:%d/%s\r\n", lan_addr[h->iface].str, runtime_vars.port, captionPath);
			dlog_print(DLOG_INFO, "tdlna_http", "♣ 자막파일 존재[%d] %s", captionfh, captionPath);
			close(captionfh);
		}
	}

	strcatf(&str, "Accept-Ranges: bytes\r\n"
	              "contentFeatures.dlna.org: %sDLNA.ORG_OP=%02X;DLNA.ORG_CI=%X;DLNA.ORG_FLAGS=%08X%024X\r\n\r\n",
	              last_file.dlna, 1, 0, dlna_flags, 0);

	//DEBUG DPRINTF(E_DEBUG, L_HTTP, "RESPONSE: %s\n", str.data);
	if( send_data(h, str.data, str.off, MSG_MORE) == 0 )
	{
		if( h->req_command != EHead )
		{
			send_file(h, sendfh, offset, h->req_RangeEnd);
		}
	}

	close(sendfh);
	CloseSocket_upnphttp(h);
error:
#if USE_FORK
	if( newpid == 0 )
	{
		//return ;
		_exit(0);
		//printf("자식프로세스 종료됨 - 이 메시지가 보이면 종료가 안됨\n");
	}
#endif
	return;
}

static void
start_dlna_header(struct string_s *str, int respcode, const char *tmode, const char *mime)
{
	char date[30];
	time_t now;

	now = time(NULL);
	strftime(date, sizeof(date),"%a, %d %b %Y %H:%M:%S GMT" , gmtime(&now));
	strcatf(str, "HTTP/1.1 %d OK\r\n"
	             "Connection: close\r\n"
	             "Date: %s\r\n"
	             "Server: " MINIDLNA_SERVER_STRING "\r\n"
	             "EXT:\r\n"
	             "realTimeInfo.dlna.org: DLNA.ORG_TLAG=*\r\n"
	             "transferMode.dlna.org: %s\r\n"
	             "Content-Type: %s\r\n",
	             respcode, date, tmode, mime);
}

static int
send_data(struct upnphttp * h, char * header, size_t size, int flags)
{
	int n;

	n = send(h->socket, header, size, flags);
	if(n<0)
	{
	//	DPRINTF(E_ERROR, L_HTTP, "send(res_buf): %s\n", strerror(errno));
	}
	else if(n < h->res_buflen)
	{
		/* TODO : handle correctly this case */
	//	DPRINTF(E_ERROR, L_HTTP, "send(res_buf): %d bytes sent (out of %d)\n", n, h->res_buflen);
	}
	else
	{
		return 0;
	}
	return 1;
}

//파일 스트림에서 네트워크 스트림으로 기록
static void
send_file(struct upnphttp * h, int sendfd, off64_t offset, off64_t end_offset)
{
	off64_t send_size;
	off64_t ret;
	char *buf = NULL;
#if HAVE_SENDFILE
	int try_sendfile = 1;
#endif

	while( offset <= end_offset )
	{
	//	printf("file - %d\n",offset); 

		if( !buf )
			buf = malloc(MIN_BUFFER_SIZE);
		send_size = (((end_offset - offset) < MIN_BUFFER_SIZE) ? (end_offset - offset + 1) : MIN_BUFFER_SIZE);
		lseek64(sendfd, offset, SEEK_SET);
		ret = read(sendfd, buf, send_size);
		if( ret == -1 ) {
		//	DPRINTF(E_DEBUG, L_HTTP, "read error :: error no. %d [%s]\n", errno, strerror(errno));
			if( errno == EAGAIN )
				continue;
			else
				break;
		}
		ret = write(h->socket, buf, ret);
		if( ret == -1 ) {
		//	DPRINTF(E_DEBUG, L_HTTP, "write error :: error no. %d [%s]\n", errno, strerror(errno));
			if( errno == EAGAIN )
				continue;
			else
				break;
		}
		offset += ret;
	}
	free(buf);
}

