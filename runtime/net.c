/* WS2_32, WSOCK32, IPHLPAPI: a PC without a network. Winsock starts, but no socket can be
   made (WSAENETDOWN), names do not resolve and there are no adapters, so the game's online code
   and the Double Fusion advertising module (DFEngine.dll) find no network. Nothing here touches
   the host's network. Pure helpers (byte order, address parsing, fd_set tests) work as usual.
   The ordinals' names are [verified] against the export tables of Wine's i386 ws2_32.dll and
   wsock32.dll (WSOCK32 numbers some functions differently: its #11 is inet_ntoa). */
#include <ctype.h>
#include <stdlib.h>
#include "runtime.h"

#define INVALID_SOCKET 0xffffffffu
#define SOCKET_ERROR 0xffffffffu
#define WSAEINVAL 10022
#define WSAENOTSOCK 10038
#define WSAENETDOWN 10050
#define WSAHOST_NOT_FOUND 11001

static uint32_t sock_fail(int err) { set_last_error(err); return SOCKET_ERROR; }

/* ------------------------------------------------------------------ start-up, errors */
/* #115 WSAStartup: Winsock 2.2 is there */
API(WS2_32, 115, 2)
{
    uint32_t want = ARG(0) & 0xffff, d = ARG(1);
    uint32_t ver = want > 0x0202 || (want & 0xff) > 2 ? 0x0202 : want;
    if (d) {
        memset(gptr(d), 0, 400);
        W16(d, ver);
        W16(d + 2, 0x0202);
        gstrncpy(d + 4, "WinSock 2.0", 257);
        gstrncpy(d + 261, "Running", 129);
    }
    LOG(LOG_API, "WSAStartup(%x)", want);
    return 0;
}
/* #116 WSACleanup */
API(WS2_32, 116, 0) { return 0; }
/* #111 WSAGetLastError */
API(WS2_32, 111, 0) { return get_last_error(); }

/* ------------------------------------------------------------------ byte order, addresses */
/* #8 htonl, #14 ntohl, #9 htons, #15 ntohs */
API(WS2_32, 8, 1) { return __builtin_bswap32(ARG(0)); }
API(WS2_32, 14, 1) { return __builtin_bswap32(ARG(0)); }
API(WS2_32, 9, 1) { return __builtin_bswap16((uint16_t)ARG(0)); }
API(WS2_32, 15, 1) { return __builtin_bswap16((uint16_t)ARG(0)); }

/* a dotted address "a.b.c.d" in network byte order, or INADDR_NONE */
static uint32_t parse_inet(const char *s)
{
    uint32_t r = 0;
    for (int i = 0; i < 4; i++) {
        if (!s || !isdigit((unsigned char)*s)) return 0xffffffffu;
        unsigned long v = strtoul(s, (char **)&s, 10);
        if (v > 255) return 0xffffffffu;
        r |= (uint32_t)v << (8 * i);
        if (i < 3 && *s++ != '.') return 0xffffffffu;
    }
    while (*s == ' ') s++;
    return *s ? 0xffffffffu : r;
}

/* #11 inet_addr (ws2_32) */
API(WS2_32, 11, 1) { return parse_inet(gstr(ARG(0))); }

/* ------------------------------------------------------------------ sockets: none */
/* #23 socket */
API(WS2_32, 23, 3)
{
    LOG(LOG_API, "socket(%u, %u, %u): no network", ARG(0), ARG(1), ARG(2));
    set_last_error(WSAENETDOWN);
    return INVALID_SOCKET;
}
API(WS2_32, WSASocketW, 6)
{
    LOG(LOG_API, "WSASocketW(%u, %u, %u): no network", ARG(0), ARG(1), ARG(2));
    set_last_error(WSAENETDOWN);
    return INVALID_SOCKET;
}
/* #2 bind, #3 closesocket, #4 connect, #6 getsockname, #7 getsockopt, #10 ioctlsocket, #16 recv,
   #17 recvfrom, #19 send, #20 sendto, #21 setsockopt: no socket to act on */
API(WS2_32, 2, 3) { return sock_fail(WSAENOTSOCK); }
API(WS2_32, 3, 1) { return sock_fail(WSAENOTSOCK); }
API(WS2_32, 4, 3) { return sock_fail(WSAENOTSOCK); }
API(WS2_32, 6, 3) { return sock_fail(WSAENOTSOCK); }
API(WS2_32, 7, 5) { return sock_fail(WSAENOTSOCK); }
API(WS2_32, 10, 3) { return sock_fail(WSAENOTSOCK); }
API(WS2_32, 16, 4) { return sock_fail(WSAENOTSOCK); }
API(WS2_32, 17, 6) { return sock_fail(WSAENOTSOCK); }
API(WS2_32, 19, 4) { return sock_fail(WSAENOTSOCK); }
API(WS2_32, 20, 6) { return sock_fail(WSAENOTSOCK); }
API(WS2_32, 21, 5) { return sock_fail(WSAENOTSOCK); }
/* #18 select: none of the sets can hold a valid socket */
API(WS2_32, 18, 5) { return sock_fail(WSAEINVAL); }

/* #151 __WSAFDIsSet: is the socket in the fd_set (count, then up to 64 sockets)? */
API(WS2_32, 151, 2)
{
    uint32_t set = ARG(1), n = R32(set);
    for (uint32_t i = 0; i < n && i < 64; i++)
        if (R32(set + 4 + 4 * i) == ARG(0)) return 1;
    return 0;
}

API(WS2_32, WSAIoctl, 9) { return sock_fail(WSAENOTSOCK); }
API(WS2_32, WSARecvFrom, 9) { return sock_fail(WSAENOTSOCK); }
API(WS2_32, WSASendTo, 9) { return sock_fail(WSAENOTSOCK); }
API(WS2_32, WSAGetOverlappedResult, 5) { set_last_error(WSAENOTSOCK); return 0; }

/* WSA events are ordinary manual-reset events (sync.c) */
extern uint32_t event_create(int manual, int initial);
extern int event_reset(uint32_t h);
API(WS2_32, WSACreateEvent, 0) { return event_create(1, 0); }
API(WS2_32, WSAResetEvent, 1) { return event_reset(ARG(0)); }
API(WS2_32, WSACloseEvent, 1)
{
    if (handle_type(ARG(0)) != H_EVENT) { set_last_error(6); return 0; }
    handle_close(ARG(0));
    return 1;
}
API(WS2_32, WSAWaitForMultipleEvents, 5)
{
    uint32_t n = ARG(0) > 64 ? 64 : ARG(0), hs[64];
    for (uint32_t i = 0; i < n; i++) hs[i] = R32(ARG(1) + 4 * i);
    return (uint32_t)wait_handles((int)n, hs, ARG(2) != 0, ARG(3));  /* WSA_WAIT_EVENT_0 + i, or WSA_WAIT_TIMEOUT */
}

/* ------------------------------------------------------------------ names */
extern const char *computer_name(void);

/* #52 gethostbyname: nothing resolves */
API(WS2_32, 52, 1)
{
    LOG(LOG_API, "gethostbyname(\"%s\"): no network", gstr(ARG(0)) ? gstr(ARG(0)) : "");
    set_last_error(WSAHOST_NOT_FOUND);
    return 0;
}
/* #57 gethostname: the computer's name, as GetComputerName gives it */
API(WS2_32, 57, 2)
{
    const char *n = computer_name();
    if (strlen(n) + 1 > ARG(1)) return sock_fail(10014);            /* WSAEFAULT */
    gstrncpy(ARG(0), n, ARG(1));
    return 0;
}
/* #103 WSAAsyncGetHostByName: no request is started */
API(WS2_32, 103, 5)
{
    LOG(LOG_API, "WSAAsyncGetHostByName(\"%s\"): no network", gstr(ARG(2)) ? gstr(ARG(2)) : "");
    set_last_error(WSAENETDOWN);
    return 0;
}
/* #108 WSACancelAsyncRequest: there are none */
API(WS2_32, 108, 1) { return sock_fail(WSAEINVAL); }

/* ------------------------------------------------------------------ WSOCK32 (Winsock 1.1) */
/* #1 accept, #13 listen, #101 WSAAsyncSelect: no socket */
API(WSOCK32, 1, 3) { set_last_error(WSAENOTSOCK); return INVALID_SOCKET; }
API(WSOCK32, 13, 2) { return sock_fail(WSAENOTSOCK); }
API(WSOCK32, 101, 4) { return sock_fail(WSAENOTSOCK); }
/* #11 inet_ntoa: the address (passed by value) as text, in one buffer of its own */
API(WSOCK32, 11, 1)
{
    static uint32_t buf;
    if (!buf) buf = galloc(16);
    uint32_t a = ARG(0);
    char t[16];
    snprintf(t, sizeof t, "%u.%u.%u.%u", a & 0xff, (a >> 8) & 0xff, (a >> 16) & 0xff, a >> 24);
    gstrncpy(buf, t, 16);
    return buf;
}
/* #151 __WSAFDIsSet */
API(WSOCK32, 151, 2) { return w_WS2_32_151_body(c); }

/* ------------------------------------------------------------------ IPHLPAPI */
/* no network adapters */
API(IPHLPAPI, GetAdaptersInfo, 2)
{
    LOG(LOG_API, "GetAdaptersInfo: no adapters");
    return 232;                          /* ERROR_NO_DATA */
}
