/**
 * Copyright (C) 2006  Ole Andr� Vadla Ravn�s <oleavr@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "stdafx.h"
#include "hooking.h"
#include "logging.h"
#include "overlapped.h"
#include <Ws2tcpip.h>
#include <psapi.h>

#pragma managed(push, off)
#include <strsafe.h>

static MODULEINFO wsock32_info;

#define CLOSESOCKET_ARGS_SIZE (1 * 4)
#define ACCEPT_ARGS_SIZE (3 * 4)
#define CONNECT_ARGS_SIZE (3 * 4)
#define WSA_ACCEPT_ARGS_SIZE (5 * 4)

CHookContext g_getaddrinfoHookContext;
CHookContext g_recvHookContext;
CHookContext g_sendHookContext;
CHookContext g_connectHookContext;

static int __cdecl
getaddrinfo_called(BOOL carry_on,
                   CpuContext ctx_before,
                   void *bt_addr,
                   void *ret_addr,
                   const char *nodename,
                   const char *servname,
                   const struct addrinfo *hints,
                   struct addrinfo **res)
{
    return 0;
}

static void
dump_addrinfo(const struct addrinfo *first_ai,
              ByteBuffer *buf)
{
    const struct addrinfo *el;

    for (el = first_ai; el; el = el->ai_next)
    {
        /*
         * ai_flags
         */
        byte_buffer_append_printf(buf, "\r\n  <flags=");

        if (el->ai_flags != 0)
        {
            bool empty = true;

            if (el->ai_flags & AI_PASSIVE)
            {
                empty = false;
                byte_buffer_append_printf(buf, "PASSIVE");
            }

            if (el->ai_flags & AI_CANONNAME)
            {
                if (!empty)  byte_buffer_append_printf(buf, "|");
                empty = false;
                byte_buffer_append_printf(buf, "CANONNAME");
            }

            if (el->ai_flags & AI_NUMERICHOST)
            {
                if (!empty)  byte_buffer_append_printf(buf, "|");
                byte_buffer_append_printf(buf, "NUMERICHOST");
            }
        }
        else
        {
            byte_buffer_append_printf(buf, "0");
        }

        /*
         * ai_family
         */
        byte_buffer_append_printf(buf, ", family=");

        switch (el->ai_family)
        {
            case PF_INET:
                byte_buffer_append_printf(buf, "INET");
                break;
            case PF_INET6:
                byte_buffer_append_printf(buf, "INET6");
                break;
            default:
                byte_buffer_append_printf(buf,
                    (el->ai_family != 0) ? "0x%08x" : "%d", el->ai_family);
                break;
        }

        /*
         * ai_socktype
         */
        byte_buffer_append_printf(buf, ", socktype=");

        switch (el->ai_socktype)
        {
            case SOCK_STREAM:
                byte_buffer_append_printf(buf, "STREAM");
                break;
            case SOCK_DGRAM:
                byte_buffer_append_printf(buf, "DGRAM");
                break;
            case SOCK_RAW:
                byte_buffer_append_printf(buf, "RAW");
                break;
            default:
                byte_buffer_append_printf(buf,
                    (el->ai_socktype != 0) ? "0x%08x" : "%d", el->ai_socktype);
                break;
        }

        /*
         * ai_protocol
         */
        byte_buffer_append_printf(buf, ", protocol=");

        switch (el->ai_protocol)
        {
            case IPPROTO_TCP:
                byte_buffer_append_printf(buf, "TCP");
                break;
            case IPPROTO_UDP:
                byte_buffer_append_printf(buf, "UDP");
                break;
            default:
                byte_buffer_append_printf(buf,
                    (el->ai_protocol != 0) ? "0x%08x" : "%d", el->ai_protocol);
                break;
        }

        /*
         * ai_canonname
         */
        byte_buffer_append_printf(buf, ", canonname=");

        if (el->ai_canonname != NULL)
        {
            byte_buffer_append_printf(buf, "\"%s\"", el->ai_canonname);
        }
        else
        {
            byte_buffer_append_printf(buf, "NULL");
        }

        /*
         * ai_addr
         */
        byte_buffer_append_printf(buf, ", addr=");

        if (el->ai_addr != NULL)
        {
            struct sockaddr_in *sin;
            int port;

            switch (el->ai_family)
            {
                case PF_INET:
                    sin = (struct sockaddr_in *) el->ai_addr;

                    byte_buffer_append_printf(buf, "%s", inet_ntoa(sin->sin_addr));

                    port = ntohs(sin->sin_port);
                    if (port != 0)
                    {
                        byte_buffer_append_printf(buf, ":%d", port);
                    }

                    break;
                default:
                    byte_buffer_append_printf(buf, "?");
                    break;
            }
        }
        else
        {
            byte_buffer_append_printf(buf, "NULL");
        }

        byte_buffer_append_printf(buf, ">");
    }
}

static int __stdcall
getaddrinfo_done(int retval,
                 CpuContext ctx_after,
                 CpuContext ctx_before,
                 void *bt_addr,
                 void *ret_addr,
                 const char *nodename,
                 const char *servname,
                 const struct addrinfo *hints,
                 struct addrinfo **res)
{
    DWORD err = GetLastError();

    if (g_getaddrinfoHookContext.ShouldLog(ret_addr, &ctx_before))
    {
        int ret_addr = *((DWORD *) ((DWORD) &retval - 4));
        TCHAR msg[64];
        ByteBuffer *body = byte_buffer_sized_new(256);
        const char *nn, *sn;

        nn = (nodename != NULL) ? nodename : "NULL";
        sn = (servname != NULL) ? servname : "NULL";

        StringCbPrintfW(msg, sizeof(msg), _T("nodename=%S, servname=%S"), nn, sn);

        byte_buffer_append_printf(body, "nodename: %s\r\nservname: %s\r\nhints:",
            nn, sn);

        dump_addrinfo(hints, body);

        byte_buffer_append_printf(body, "\r\nresult:");

        if (retval == 0)
        {
            dump_addrinfo(*res, body);
        }
        else
        {
            char *str = NULL;

            switch (err)
            {
                case EAI_AGAIN:    str = "EAI_AGAIN";    break;
                case EAI_BADFLAGS: str = "EAI_BADFLAGS"; break;
                case EAI_FAIL:     str = "EAI_FAIL";     break;
                case EAI_FAMILY:   str = "EAI_FAMILY";   break;
                case EAI_MEMORY:   str = "EAI_MEMORY";   break;
                //case EAI_NODATA:   str = "EAI_NODATA";   break;
                case EAI_NONAME:   str = "EAI_NONAME";   break;
                case EAI_SERVICE:  str = "EAI_SERVICE";  break;
                case EAI_SOCKTYPE: str = "EAI_SOCKTYPE"; break;
                default:                                 break;
            }

            byte_buffer_append_printf(body, "\r\n  ");

            if (str != NULL)
            {
                byte_buffer_append_printf(body, "%s", str);
            }
            else
            {
                byte_buffer_append_printf(body, "ERROR_0x%08x", err);
            }
        }

        message_logger_log(_T("getaddrinfo"), (char *) &retval - 4, 0,
            MESSAGE_TYPE_PACKET, MESSAGE_CTX_INFO, PACKET_DIRECTION_INVALID,
            NULL, NULL, (const char *) body->buf, (int) body->offset,
            msg);

        byte_buffer_free(body);
    }

    SetLastError(err);
    return retval;
}

static int __cdecl
closesocket_called(BOOL carry_on,
                   DWORD ret_addr,
                   SOCKET s)
{
    void *bt_address = (char *) &carry_on + 8 + CLOSESOCKET_ARGS_SIZE;

    log_socket_closed(bt_address, s);
    return 0;
}

static int __stdcall
closesocket_done(int retval,
                 SOCKET s)
{
    return retval;
}

static unsigned long localhost_addr;

static bool
is_stupid_rpc(SOCKET s, const char *buf, int len)
{
    if (len != 1)
        return false;

    if (buf[0] != '!')
        return false;

    struct sockaddr_in local_addr, peer_addr;
    int sin_len;

    sin_len = sizeof(local_addr);
    getsockname(s, (struct sockaddr *) &local_addr, &sin_len);
    if (local_addr.sin_addr.s_addr != localhost_addr)
        return false;

    sin_len = sizeof(peer_addr);
    getpeername(s, (struct sockaddr *) &peer_addr, &sin_len);
    if (peer_addr.sin_addr.s_addr != localhost_addr)
        return false;

    return true;
}

static int __cdecl
recv_called(BOOL carry_on,
            CpuContext ctx_before,
            void *bt_addr,
            void *ret_addr,
            SOCKET s,
            char *buf,
            int len,
            int flags)
{
    return softwall_decide_from_socket(_T("recv"), (DWORD) ret_addr, s, &carry_on);
}

static int __stdcall
recv_done(int retval,
          CpuContext ctx_after,
          CpuContext ctx_before,
          void *bt_addr,
          void *ret_addr,
          SOCKET s,
          char *buf,
          int len,
          int flags)
{
    DWORD err = GetLastError();

    if (retval > 0)
    {
        if (g_recvHookContext.ShouldLog(ret_addr, &ctx_before) && !is_stupid_rpc(s, buf, retval))
        {
            log_tcp_packet(_T("recv"), bt_addr, PACKET_DIRECTION_INCOMING, s, buf, retval);
        }
    }
    else if (retval == 0)
    {
        log_tcp_disconnected(_T("recv"), bt_addr, s, NULL);
    }
    else if (retval == SOCKET_ERROR)
    {
        if (err != WSAEWOULDBLOCK)
        {
            log_tcp_disconnected(_T("recv"), bt_addr, s, &err);
        }
    }

    SetLastError(err);
    return retval;
}

static int __cdecl
send_called(BOOL carry_on,
            CpuContext ctx_before,
            void *bt_addr,
            void *ret_addr,
            SOCKET s,
            const char *buf,
            int len,
            int flags)
{
    return softwall_decide_from_socket(_T("send"), (DWORD) ret_addr, s, &carry_on);
}

static int __stdcall
send_done(int retval,
          CpuContext ctx_after,
          CpuContext ctx_before,
          void *bt_addr,
          void *ret_addr,
          SOCKET s,
          const char *buf,
          int len,
          int flags)
{
    DWORD err = GetLastError();

    if (retval > 0)
    {
        if (g_sendHookContext.ShouldLog(ret_addr, &ctx_before) && !is_stupid_rpc(s, buf, retval))
        {
            log_tcp_packet(_T("send"), bt_addr, PACKET_DIRECTION_OUTGOING, s, buf, retval);
        }
    }
    else if (retval == SOCKET_ERROR)
    {
        if (err != WSAEWOULDBLOCK)
        {
            log_tcp_disconnected(_T("send"), bt_addr, s, &err);
        }
    }

    SetLastError(err);
    return retval;
}

static int __cdecl
recvfrom_called(BOOL carry_on,
                DWORD ret_addr,
                SOCKET s,
                char *buf,
                int len,
                int flags,
                struct sockaddr *from,
                int *fromlen)
{
    return 0;
}

static int __stdcall
recvfrom_done(int retval,
              SOCKET s,
              char *buf,
              int len,
              int flags,
              struct sockaddr *from,
              int *fromlen)
{
    DWORD err = GetLastError();
    int ret_addr = *((DWORD *) ((DWORD) &retval - 4));
    int overridden_retval;
    BOOL carry_on;

    overridden_retval =
        softwall_decide_from_socket_and_remote_address(_T("recvfrom"), ret_addr,
                                                       s, (const sockaddr_in *) from,
                                                       &carry_on);

    if (!carry_on)
        return overridden_retval;

    if (retval > 0)
    {
        log_udp_packet(_T("recvfrom"), (char *) &retval - 4, PACKET_DIRECTION_INCOMING, s, from, buf, retval);
    }

    SetLastError(err);
    return retval;
}

static int __cdecl
sendto_called(BOOL carry_on,
              DWORD ret_addr,
              SOCKET s,
              const char *buf,
              int len,
              int flags,
              const struct sockaddr *to,
              int tolen)
{
    return 0;
}

static int __stdcall
sendto_done(int retval,
            SOCKET s,
            const char* buf,
            int len,
            int flags,
            const struct sockaddr *to,
            int tolen)
{
    DWORD err = GetLastError();
    int ret_addr = *((DWORD *) ((DWORD) &retval - 4));
    int overridden_retval;
    BOOL carry_on;

    overridden_retval =
        softwall_decide_from_socket_and_remote_address(_T("sendto"), ret_addr,
                                                       s, (const sockaddr_in *) to,
                                                       &carry_on);

    if (!carry_on)
        return overridden_retval;

    if (retval > 0)
    {
        log_udp_packet(_T("sendto"), (char *) &retval - 4, PACKET_DIRECTION_OUTGOING, s, to, buf, retval);
    }

    SetLastError(err);
    return retval;
}

static SOCKET __cdecl
accept_called(BOOL carry_on,
              DWORD ret_addr,
              SOCKET s,
              struct sockaddr *addr,
              int *addrlen)
{
    int retval =
        softwall_decide_from_socket_and_remote_address(_T("accept"), ret_addr, s, NULL, &carry_on);

    if (carry_on)
    {
        void *bt_address = (char *) &carry_on + 8 + ACCEPT_ARGS_SIZE;
        log_tcp_listening(_T("accept"), bt_address, s);
    }

    return retval;
}

static SOCKET __stdcall
accept_done(SOCKET retval,
            SOCKET s,
            struct sockaddr *addr,
            int *addrlen)
{
    DWORD err = GetLastError();
    int ret_addr = *((DWORD *) ((DWORD) &retval - 4));
    int overridden_retval;
    BOOL carry_on;

    overridden_retval =
        softwall_decide_from_socket_and_remote_address(_T("accept"), ret_addr, s,
                                                       (const sockaddr_in *) addr,
                                                       &carry_on);

    if (!carry_on)
        return overridden_retval;

    if (retval != INVALID_SOCKET)
        log_tcp_client_connected(_T("accept"), (char *) &retval - 4, s, retval);

    SetLastError(err);
    return retval;
}

static int __cdecl
connect_called(BOOL carry_on,
               CpuContext ctx_before,
               void *bt_addr,
               void *ret_addr,
               SOCKET s,
               const struct sockaddr *name,
               int namelen)
{
    int retval =
        softwall_decide_from_socket_and_remote_address(_T("connect"), (DWORD) ret_addr, s,
                                                       (const sockaddr_in *) name,
                                                       &carry_on);

    if (carry_on && g_connectHookContext.ShouldLog(ret_addr, &ctx_before))
    {
        void *bt_address = (char *) &carry_on + 8 + CONNECT_ARGS_SIZE;
        log_tcp_connecting(_T("connect"), bt_address, s, name);
    }

    return retval;
}

static int __stdcall
connect_done(int retval,
             CpuContext ctx_after,
             CpuContext ctx_before,
             void *bt_addr,
             void *ret_addr,
             SOCKET s,
             const struct sockaddr *name,
             int namelen)
{
    DWORD err = GetLastError();

    if (retval != SOCKET_ERROR && g_connectHookContext.ShouldLog(ret_addr, &ctx_before))
    {
        log_tcp_connected(_T("connect"), (char *) &retval - 4, s, name);
    }

    SetLastError(err);
    return retval;
}

static BOOL
called_from_wsock(DWORD ret_addr)
{
    return (ret_addr >= (DWORD) wsock32_info.lpBaseOfDll &&
            ret_addr < (DWORD) wsock32_info.lpBaseOfDll + wsock32_info.SizeOfImage);
}

typedef struct {
    void *btAddr;
    SOCKET sock;
    LPWSABUF lpBuffers;
    DWORD dwBufferCount;
    LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine;
} AsyncRecvContext;

static void
wsaRecvCompletedHandler(COverlappedOperation *operation)
{
    AsyncRecvContext *ctx = (AsyncRecvContext *) operation->GetData();
    DWORD transferred, flags;

    BOOL success = WSAGetOverlappedResult(ctx->sock, operation->GetRealOverlapped(), &transferred, TRUE, &flags);
    DWORD wsaLastError = WSAGetLastError();

    if (success)
    {
        int bytesLeft = transferred;

        for (DWORD i = 0; i < ctx->dwBufferCount && bytesLeft > 0; i++)
        {
            WSABUF *buf = &ctx->lpBuffers[i];

            ULONG n = bytesLeft;
            if (n > buf->len)
                n = buf->len;

            log_tcp_packet(_T("WSARecv"), ctx->btAddr, PACKET_DIRECTION_INCOMING, ctx->sock,
                           buf->buf, n);

            bytesLeft -= n;
        }
    }

    if (ctx->lpCompletionRoutine != NULL)
    {
        ctx->lpCompletionRoutine((success) ? 0 : wsaLastError, transferred, operation->GetClientOverlapped(), flags);
    }
    else
    {
        HANDLE clientEvent = operation->GetClientOverlapped()->hEvent;
        if (clientEvent != 0 && clientEvent != INVALID_HANDLE_VALUE)
        {
            SetEvent(clientEvent);
        }
    }
}

int __cdecl
WSARecv_called(BOOL carry_on,
               CpuContext ctx_before,
               void *bt_addr,
               void *ret_addr,
               SOCKET s,
               LPWSABUF lpBuffers,
               DWORD dwBufferCount,
               LPDWORD lpNumberOfBytesRecvd,
               LPDWORD lpFlags,
               LPWSAOVERLAPPED lpOverlapped,
               LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
    /*
    if (lpOverlapped != NULL || lpCompletionRoutine != NULL)
    {
        AsyncRecvContext *ctx = (AsyncRecvContext *) sspy_malloc(sizeof(AsyncRecvContext));
        ctx->btAddr = bt_addr;
        ctx->sock = s;
        ctx->lpBuffers = lpBuffers;
        ctx->dwBufferCount = dwBufferCount;
        ctx->lpCompletionRoutine = lpCompletionRoutine;

        lpCompletionRoutine = NULL; // we'll take care of this ourselves

        COverlappedManager::TrackOperation(&lpOverlapped, ctx, wsaRecvCompletedHandler);
        return 0;
    }*/

    return softwall_decide_from_socket(_T("WSARecv"), (DWORD) ret_addr, s, &carry_on);
}

static int __stdcall
WSARecv_done(int retval,
             CpuContext ctx_after,
             CpuContext ctx_before,
             void *bt_addr,
             void *ret_addr,
             SOCKET s,
             LPWSABUF lpBuffers,
             DWORD dwBufferCount,
             LPDWORD lpNumberOfBytesRecvd,
             LPDWORD lpFlags,
             LPWSAOVERLAPPED lpOverlapped,
             LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
    DWORD err = GetLastError();
    DWORD wsa_err = WSAGetLastError();

    // FIXME: check the return value here in case of overlapped ops

    if (called_from_wsock((DWORD) ret_addr))
        return retval;

    if (retval == 0)
    {
        int bytes_left = *lpNumberOfBytesRecvd;

        for (DWORD i = 0; i < dwBufferCount && bytes_left > 0; i++)
        {
            WSABUF *buf = &lpBuffers[i];

            ULONG n = bytes_left;
            if (n > buf->len)
                n = buf->len;

            log_tcp_packet(_T("WSARecv"), bt_addr, PACKET_DIRECTION_INCOMING, s,
                           buf->buf, n);

            bytes_left -= n;
        }
    }
    else if (retval == SOCKET_ERROR)
    {
        if (wsa_err == WSAEWOULDBLOCK)
        {
            message_logger_log_message(_T("WSARecv"), bt_addr, MESSAGE_CTX_WARNING,
                                       _T("non-blocking mode not yet supported"));
        }

        if (wsa_err != WSAEWOULDBLOCK && wsa_err != WSA_IO_PENDING)
        {
            log_tcp_disconnected(_T("WSARecv"), bt_addr, s,
                                 (err == WSAECONNRESET) ? NULL : &err);
        }
    }

    WSASetLastError(wsa_err);
    SetLastError(err);
    return retval;
}

static int __cdecl
WSASend_called(BOOL carry_on,
               DWORD ret_addr,
               SOCKET s,
               LPWSABUF lpBuffers,
               DWORD dwBufferCount,
               LPDWORD lpNumberOfBytesSent,
               DWORD dwFlags,
               LPWSAOVERLAPPED lpOverlapped,
               LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
    return softwall_decide_from_socket(_T("WSASend"), ret_addr, s, &carry_on);
}

static int __stdcall
WSASend_done(int retval,
             SOCKET s,
             LPWSABUF lpBuffers,
             DWORD dwBufferCount,
             LPDWORD lpNumberOfBytesSent,
             DWORD dwFlags,
             LPWSAOVERLAPPED lpOverlapped,
             LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
    DWORD err = GetLastError();
    DWORD wsa_err = WSAGetLastError();
    DWORD ret_addr = *((DWORD *) ((DWORD) &retval - 4));
    void *bt_address = (char *) &retval - 4;

    if (called_from_wsock(ret_addr))
        return retval;

    if (lpOverlapped != NULL)
    {
        message_logger_log_message(_T("WSASend"), bt_address, MESSAGE_CTX_WARNING,
                                   _T("overlapped I/O not yet supported"));
    }

    if (retval == 0)
    {
        int bytes_left = *lpNumberOfBytesSent;

        for (DWORD i = 0; i < dwBufferCount && bytes_left > 0; i++)
        {
            WSABUF *buf = &lpBuffers[i];

            ULONG n = bytes_left;
            if (n > buf->len)
                n = buf->len;

            log_tcp_packet(_T("WSASend"), bt_address, PACKET_DIRECTION_OUTGOING, s,
                           buf->buf, n);

            bytes_left -= n;
        }
    }
    else if (retval == SOCKET_ERROR)
    {
        if (wsa_err == WSAEWOULDBLOCK)
        {
            message_logger_log_message(_T("WSASend"), bt_address, MESSAGE_CTX_WARNING,
                                       _T("non-blocking mode not yet supported"));
        }

        if (wsa_err != WSAEWOULDBLOCK && wsa_err != WSA_IO_PENDING)
        {
            log_tcp_disconnected(_T("WSASend"), bt_address, s,
                                 (err == WSAECONNRESET) ? NULL : &err);
        }
    }

    WSASetLastError(wsa_err);
    SetLastError(err);
    return retval;
}

static SOCKET __cdecl
WSAAccept_called(BOOL carry_on,
                 DWORD ret_addr,
                 SOCKET s,
                 struct sockaddr *addr,
                 LPINT addrlen,
                 LPCONDITIONPROC lpfnCondition,
                 DWORD dwCallbackData)
{
    int retval =
        softwall_decide_from_socket_and_remote_address(_T("WSAAccept"), ret_addr, s, NULL, &carry_on);

    if (carry_on)
    {
        void *bt_address = (char *) &carry_on + 8 + WSA_ACCEPT_ARGS_SIZE;

        /* FIXME: only issue this once for non-blocking sockets */
        log_tcp_listening(_T("WSAAccept"), bt_address, s);
    }

    return retval;
}

static SOCKET __stdcall
WSAAccept_done(SOCKET retval,
               SOCKET s,
               struct sockaddr *addr,
               LPINT addrlen,
               LPCONDITIONPROC lpfnCondition,
               DWORD dwCallbackData)
{
    DWORD err = GetLastError();
    int ret_addr = *((DWORD *) ((DWORD) &retval - 4));
    int overridden_retval;
    BOOL carry_on;

    overridden_retval =
        softwall_decide_from_socket_and_remote_address(_T("WSAAccept"), ret_addr, s,
                                                       (const sockaddr_in *) addr,
                                                       &carry_on);

    if (!carry_on)
        return overridden_retval;

    if (retval != INVALID_SOCKET)
    {
        log_tcp_client_connected(_T("WSAAccept"), (char *) &retval - 4, s, retval);
    }

    SetLastError(err);
    return retval;
}

static int __cdecl
wsock32_recv_called(BOOL carry_on,
                    DWORD ret_addr,
                    SOCKET s,
                    char *buf,
                    int len,
                    int flags)
{
    return softwall_decide_from_socket(_T("wsock32_recv"), ret_addr, s, &carry_on);
}

static int __stdcall
wsock32_recv_done(int retval,
                  SOCKET s,
                  char *buf,
                  int len,
                  int flags)
{
  DWORD err = GetLastError();
  int ret_addr = *((DWORD *) ((DWORD) &retval - 4));
  void *bt_address = (char *) &retval - 4;

  if (retval > 0)
  {
    log_tcp_packet(_T("wsock32_recv"), bt_address, PACKET_DIRECTION_INCOMING, s, buf, retval);
  }
  else if (retval == 0)
  {
    log_tcp_disconnected(_T("wsock32_recv"), bt_address, s, NULL);
  }
  else if (retval == SOCKET_ERROR)
  {
    if (err != WSAEWOULDBLOCK)
    {
      log_tcp_disconnected(_T("wsock32_recv"), bt_address, s, &err);
    }
  }

  SetLastError(err);
  return retval;
}

HOOK_GLUE_EXTENDED(getaddrinfo, (4 * 4))

HOOK_GLUE_INTERRUPTIBLE(closesocket, CLOSESOCKET_ARGS_SIZE)

HOOK_GLUE_EXTENDED(recv, (4 * 4))
HOOK_GLUE_EXTENDED(send, (4 * 4))
HOOK_GLUE_INTERRUPTIBLE(recvfrom, (6 * 4))
HOOK_GLUE_INTERRUPTIBLE(sendto, (6 * 4))
HOOK_GLUE_INTERRUPTIBLE(accept, ACCEPT_ARGS_SIZE)
HOOK_GLUE_EXTENDED(connect, CONNECT_ARGS_SIZE)
HOOK_GLUE_EXTENDED(WSARecv, (7 * 4))
HOOK_GLUE_INTERRUPTIBLE(WSASend, (7 * 4))
HOOK_GLUE_INTERRUPTIBLE(WSAAccept, WSA_ACCEPT_ARGS_SIZE)
HOOK_GLUE_INTERRUPTIBLE(wsock32_recv, (4 * 4))

void
hook_winsock()
{
    HookManager *mgr = HookManager::Obtain();

    HMODULE h = mgr->OpenLibrary(_T("ws2_32.dll"));
    if (h == NULL)
    {
        MessageBox(0, _T("Failed to load 'ws2_32.dll'."),
                   _T("oSpy"), MB_ICONERROR | MB_OK);
      return;
    }

    localhost_addr = inet_addr("127.0.0.1");

    HOOK_FUNCTION(h, getaddrinfo);

    HOOK_FUNCTION(h, closesocket);
    HOOK_FUNCTION(h, recv);
    HOOK_FUNCTION(h, send);
    HOOK_FUNCTION(h, recvfrom);
    HOOK_FUNCTION(h, sendto);
    HOOK_FUNCTION(h, accept);
    HOOK_FUNCTION(h, connect);
    HOOK_FUNCTION(h, WSARecv);
    HOOK_FUNCTION(h, WSASend);
    HOOK_FUNCTION(h, WSAAccept);

    h = mgr->OpenLibrary(_T("wsock32.dll"));
    if (h == NULL)
    {
        MessageBox(0, _T("Failed to load 'wsock32.dll'."),
                   _T("oSpy"), MB_ICONERROR | MB_OK);
        return;
    }

    if (GetModuleInformation(GetCurrentProcess(), h, &wsock32_info,
                             sizeof(wsock32_info)) == 0)
    {
        message_logger_log_message(_T("DllMain"), 0, MESSAGE_CTX_WARNING,
                                   _T("GetModuleInformation failed with errno %u"),
                                   GetLastError());
    }

    HOOK_FUNCTION_BY_ALIAS(h, recv, wsock32_recv);
}

#pragma managed(pop)
