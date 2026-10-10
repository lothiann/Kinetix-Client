/* (c) Kinetix. SOCKS5 client-side proxy implementation — per-connection UDP
 * proxying for the Dummy Manager. Ported from HDDNet DDNet-19.9 and extended
 * with username/password auth (RFC 1929). Routing and hook installation live
 * in CClient (see socks5_proxy.h). */
#include "socks5_proxy.h"

#include <base/log.h>
#include <base/math.h>
#include <base/mem.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/console.h>
#include <engine/shared/network.h>

#include <cstring>

#if defined(CONF_FAMILY_UNIX)
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#elif defined(CONF_FAMILY_WINDOWS)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#error NOT IMPLEMENTED
#endif

// SOCKS5 protocol constants (RFC 1928 / RFC 1929)
static const unsigned char SOCKS5_VER = 0x05;
static const unsigned char SOCKS5_METHOD_NOAUTH = 0x00;
static const unsigned char SOCKS5_METHOD_USERPASS = 0x02;
static const unsigned char SOCKS5_METHOD_NONE_ACCEPTABLE = 0xFF;
static const unsigned char SOCKS5_USERPASS_VER = 0x01;
static const unsigned char SOCKS5_USERPASS_STATUS_OK = 0x00;
static const unsigned char SOCKS5_CMD_UDP_ASSOCIATE = 0x03;
static const unsigned char SOCKS5_ATYP_IPV4 = 0x01;
static const unsigned char SOCKS5_ATYP_DOMAIN = 0x03;
static const unsigned char SOCKS5_ATYP_IPV6 = 0x04;
static const unsigned char SOCKS5_REP_SUCCESS = 0x00;

// ----------------------------------------------------------------------------
// Helpers for non-blocking I/O on the raw underlying socket fd.
// For select / TCP_NODELAY we need the raw fd, which we obtain through
// net_socket_fd() (implemented in base/net.cpp). For send/recv on the control
// socket we use net_tcp_send / net_tcp_recv (which are already
// non-blocking-aware).
// ----------------------------------------------------------------------------

// Wait up to TimeoutMs for the socket to become readable and/or writable.
// Returns: 0 = timeout, >0 = ready (bit 1 = readable, bit 2 = writable),
//          -1 = select error.
static int sock_wait(NETSOCKET sock, bool WantRead, bool WantWrite, int TimeoutMs)
{
	int fd = net_socket_fd(sock);
	if(fd < 0)
		return -1;

	fd_set Rfds, Wfds;
	FD_ZERO(&Rfds);
	FD_ZERO(&Wfds);
	int Nfds = fd + 1;
	if(WantRead)
		FD_SET(fd, &Rfds);
	if(WantWrite)
		FD_SET(fd, &Wfds);

	struct timeval Tv;
	Tv.tv_sec = TimeoutMs / 1000;
	Tv.tv_usec = (TimeoutMs % 1000) * 1000;

	int r = select(Nfds, WantRead ? &Rfds : nullptr, WantWrite ? &Wfds : nullptr, nullptr, &Tv);
	if(r < 0)
		return -1;
	if(r == 0)
		return 0;
	int Ready = 0;
	if(WantRead && FD_ISSET(fd, &Rfds))
		Ready |= 1;
	if(WantWrite && FD_ISSET(fd, &Wfds))
		Ready |= 2;
	return Ready ? Ready : 0;
}

// Set TCP_NODELAY on the underlying socket fd (disables Nagle — important for
// low-latency SOCKS5 control messages).
static void sock_set_nodelay(NETSOCKET sock)
{
	int fd = net_socket_fd(sock);
	if(fd < 0)
		return;
	int flag = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&flag, sizeof(flag));
}

// Check the pending socket error (for non-blocking connect completion).
// Returns 0 if connected, nonzero error code otherwise.
static int sock_pending_error(NETSOCKET sock)
{
	int fd = net_socket_fd(sock);
	if(fd < 0)
		return -1;
	int err = 0;
	socklen_t len = sizeof(err);
	getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
	return err;
}

// ----------------------------------------------------------------------------
// CSocks5Proxy
// ----------------------------------------------------------------------------

CSocks5Proxy::CSocks5Proxy()
{
}

CSocks5Proxy::~CSocks5Proxy()
{
	Disable();
}

void CSocks5Proxy::Log(int Level, const char *pMsg) const
{
	if(m_pConsole)
		m_pConsole->Print(Level, "socks5", pMsg);
	else
		log_info("socks5", "%s", pMsg);
}

bool CSocks5Proxy::ParseProxyAddr(const char *pStr)
{
	if(!pStr || !pStr[0])
		return false;

	str_copy(m_aProxyAddrStr, pStr, sizeof(m_aProxyAddrStr));

	// Optional "user:pass@" prefix (Dummy Manager format "u:p@socks5:proxy").
	// No '@' → no authentication. The FIRST '@' and the FIRST ':' inside the
	// credentials split user/password (a password containing '@' is not
	// supported).
	const char *pAddrPart = pStr;
	m_HasAuth = false;
	m_aUser[0] = '\0';
	m_aPass[0] = '\0';
	const char *pAt = strchr(pStr, '@');
	if(pAt)
	{
		const size_t UserPassLen = (size_t)(pAt - pStr);
		if(UserPassLen == 0 || UserPassLen >= sizeof(m_aUser) + sizeof(m_aPass))
			return false;
		char aUserPass[160];
		str_truncate(aUserPass, sizeof(aUserPass), pStr, UserPassLen);
		char *pColon = strchr(aUserPass, ':');
		if(pColon)
		{
			*pColon = '\0';
			str_copy(m_aPass, pColon + 1, sizeof(m_aPass));
		}
		str_copy(m_aUser, aUserPass, sizeof(m_aUser));
		if(!m_aUser[0])
			return false;
		m_HasAuth = true;
		pAddrPart = pAt + 1;
		if(!pAddrPart[0])
			return false;
	}

	// Try direct parse first (handles "IP:PORT" and "[IPv6]:PORT").
	if(net_addr_from_str(&m_ProxyAddr, pAddrPart) == 0)
		return true;

	// Fallback: maybe it's "host:PORT" (hostname). Split host/port and resolve.
	char aHost[256];
	str_copy(aHost, pAddrPart, sizeof(aHost));
	char *pColon = nullptr;
	for(char *p = aHost; *p; ++p)
	{
		if(*p == ':')
		{
			pColon = p;
			break;
		}
	}
	if(!pColon)
		return false;
	*pColon = '\0';
	const char *pHost = aHost;
	const char *pPortStr = pColon + 1;
	int Port = 0;
	for(const char *p = pPortStr; *p; ++p)
	{
		if(*p < '0' || *p > '9')
			return false;
		Port = Port * 10 + (*p - '0');
		if(Port > 65535)
			return false;
	}
	if(Port <= 0 || Port > 65535)
		return false;

	NETADDR Resolved;
	if(net_host_lookup(pHost, &Resolved, NETTYPE_IPV4) != 0 &&
		net_host_lookup(pHost, &Resolved, NETTYPE_IPV6) != 0)
		return false;

	Resolved.port = (unsigned short)Port;
	m_ProxyAddr = Resolved;
	return true;
}

void CSocks5Proxy::CloseSockets()
{
	if(m_UdpSocket)
	{
		net_udp_close(m_UdpSocket);
		m_UdpSocket = nullptr;
	}
	if(m_TcpControl)
	{
		net_tcp_close(m_TcpControl);
		m_TcpControl = nullptr;
	}
	m_RelayAddr = NETADDR_ZEROED;
	m_RelayAddrIsProxy = false;
}

void CSocks5Proxy::Fail(const char *pReason)
{
	Log(IConsole::OUTPUT_LEVEL_STANDARD, pReason);
	CloseSockets();
	m_State = EState::BROKEN;
	m_NextReconnectTime = time_get() + time_freq() * RECONNECT_BACKOFF_SEC;
}

bool CSocks5Proxy::StartConnect()
{
	CloseSockets();

	// Create TCP socket bound to the right family.
	NETADDR BindAddr = NETADDR_ZEROED;
	BindAddr.type = (m_ProxyAddr.type & NETTYPE_IPV6) ? NETTYPE_IPV6 : NETTYPE_IPV4;
	m_TcpControl = net_tcp_create(BindAddr);
	if(!m_TcpControl)
	{
		Log(IConsole::OUTPUT_LEVEL_STANDARD, "tcp: failed to create control socket");
		return false;
	}

	// Non-blocking from the start — connect() will return EINPROGRESS /
	// WSAEWOULDBLOCK, which is expected.
	net_set_non_blocking(m_TcpControl);

	int r = net_tcp_connect(m_TcpControl, &m_ProxyAddr);
	if(r == 0)
	{
		// Immediate connect (rare for non-blocking, but possible on localhost).
		sock_set_nodelay(m_TcpControl);
		m_HsState = EHsState::AUTH_SEND;
		PrepareAuthSend();
		m_State = EState::HANDSHAKING;
		m_HsStartTime = time_get();
	}
	else
	{
		// In-progress is the normal non-blocking connect path.
#if defined(CONF_FAMILY_WINDOWS)
		int err = WSAGetLastError();
		if(err != WSAEWOULDBLOCK)
#else
		int err = errno;
		if(err != EINPROGRESS)
#endif
		{
			Log(IConsole::OUTPUT_LEVEL_STANDARD, "tcp: connect failed immediately");
			CloseSockets();
			return false;
		}
		m_State = EState::CONNECTING;
		m_HsStartTime = time_get();
	}

	return true;
}

void CSocks5Proxy::PrepareAuthSend()
{
	// VER NMETHODS METHODS...
	m_aHsSendBuf[0] = SOCKS5_VER;
	if(m_HasAuth)
	{
		// Prefer username/password when credentials are configured.
		m_aHsSendBuf[1] = 2;
		m_aHsSendBuf[2] = SOCKS5_METHOD_USERPASS;
		m_aHsSendBuf[3] = SOCKS5_METHOD_NOAUTH;
		m_HsSendLen = 4;
	}
	else
	{
		m_aHsSendBuf[1] = 1;
		m_aHsSendBuf[2] = SOCKS5_METHOD_NOAUTH;
		m_HsSendLen = 3;
	}
	m_HsSendPos = 0;
}

void CSocks5Proxy::PrepareUserPassSend()
{
	// VER(1) ULEN(1) UNAME PLEN(1) PASSWD (RFC 1929)
	int Pos = 0;
	m_aHsSendBuf[Pos++] = SOCKS5_USERPASS_VER;
	const int UserLen = str_length(m_aUser);
	m_aHsSendBuf[Pos++] = (unsigned char)UserLen;
	mem_copy(m_aHsSendBuf + Pos, m_aUser, UserLen);
	Pos += UserLen;
	const int PassLen = str_length(m_aPass);
	m_aHsSendBuf[Pos++] = (unsigned char)PassLen;
	mem_copy(m_aHsSendBuf + Pos, m_aPass, PassLen);
	Pos += PassLen;
	m_HsSendLen = Pos;
	m_HsSendPos = 0;
}

void CSocks5Proxy::PrepareAssocSend()
{
	// VER CMD RSV ATYP DST.ADDR DST.PORT
	// UDP ASSOCIATE with DST = 0.0.0.0:0 (let proxy pick the relay).
	m_aHsSendBuf[0] = SOCKS5_VER;
	m_aHsSendBuf[1] = SOCKS5_CMD_UDP_ASSOCIATE;
	m_aHsSendBuf[2] = 0x00; // RSV
	m_aHsSendBuf[3] = SOCKS5_ATYP_IPV4;
	m_aHsSendBuf[4] = 0;
	m_aHsSendBuf[5] = 0;
	m_aHsSendBuf[6] = 0;
	m_aHsSendBuf[7] = 0;
	m_aHsSendBuf[8] = 0;
	m_aHsSendBuf[9] = 0;
	m_HsSendLen = 10;
	m_HsSendPos = 0;
}

bool CSocks5Proxy::ProcessAuthReply()
{
	// Need 2 bytes: VER METHOD
	if(m_HsRecvPos < 2)
		return false; // not enough yet
	if(m_aHsRecvBuf[0] != SOCKS5_VER || m_aHsRecvBuf[1] == SOCKS5_METHOD_NONE_ACCEPTABLE)
	{
		Fail("auth: proxy rejected all offered methods");
		return false;
	}
	if(m_aHsRecvBuf[1] == SOCKS5_METHOD_NOAUTH)
	{
		return true;
	}
	if(m_aHsRecvBuf[1] == SOCKS5_METHOD_USERPASS)
	{
		if(!m_HasAuth)
		{
			Fail("auth: proxy requires username/password but none configured");
			return false;
		}
		// Switch to the RFC 1929 sub-negotiation.
		m_HsState = EHsState::USERPASS_SEND;
		PrepareUserPassSend();
		return true;
	}
	Fail("auth: proxy selected unsupported method");
	return false;
}

bool CSocks5Proxy::ProcessUserPassReply()
{
	// Need 2 bytes: VER STATUS
	if(m_HsRecvPos < 2)
		return false; // not enough yet
	if(m_aHsRecvBuf[0] != SOCKS5_USERPASS_VER || m_aHsRecvBuf[1] != SOCKS5_USERPASS_STATUS_OK)
	{
		Fail("auth: username/password rejected by proxy");
		return false;
	}
	return true;
}

bool CSocks5Proxy::ProcessAssocReplyHdr()
{
	// Need 4 bytes: VER REP RSV ATYP
	if(m_HsRecvPos < 4)
		return false;
	if(m_aHsRecvBuf[0] != SOCKS5_VER)
	{
		Fail("udp-assoc: bad version in reply");
		return false;
	}
	if(m_aHsRecvBuf[1] != SOCKS5_REP_SUCCESS)
	{
		char aBuf[64];
		str_format(aBuf, sizeof(aBuf), "udp-assoc: proxy refused (REP=%d)", m_aHsRecvBuf[1]);
		Fail(aBuf);
		return false;
	}
	unsigned char ATYP = m_aHsRecvBuf[3];
	int AddrLen = 0;
	switch(ATYP)
	{
	case SOCKS5_ATYP_IPV4:
		AddrLen = 4;
		break;
	case SOCKS5_ATYP_IPV6:
		AddrLen = 16;
		break;
	case SOCKS5_ATYP_DOMAIN:
		// Domain length is read from the first addr byte — handled in
		// ProcessAssocReplyFull, but we need at least 1 byte to know.
		AddrLen = -1;
		break;
	default:
		Fail("udp-assoc: unknown ATYP in reply");
		return false;
	}
	if(AddrLen > 0)
	{
		// We already have 4 header bytes; need AddrLen + 2 (port) more.
		m_HsRecvNeed = 4 + AddrLen + 2;
	}
	else
	{
		// Domain: need at least 5 bytes to read the length byte.
		m_HsRecvNeed = 5;
	}
	return true;
}

bool CSocks5Proxy::ProcessAssocReplyFull()
{
	unsigned char ATYP = m_aHsRecvBuf[3];
	unsigned char aPort[2];

	if(ATYP == SOCKS5_ATYP_IPV4)
	{
		// 4 header + 4 addr + 2 port = 10
		if(m_HsRecvPos < 10)
			return false;
		mem_copy(aPort, m_aHsRecvBuf + 8, 2);
		unsigned char *aAddr = m_aHsRecvBuf + 4;
		bool IsZero = (aAddr[0] | aAddr[1] | aAddr[2] | aAddr[3]) == 0;
		if(IsZero)
		{
			m_RelayAddr = m_ProxyAddr;
			m_RelayAddr.port = (unsigned short)((aPort[0] << 8) | aPort[1]);
			m_RelayAddrIsProxy = true;
		}
		else
		{
			mem_zero(&m_RelayAddr, sizeof(m_RelayAddr));
			m_RelayAddr.type = NETTYPE_IPV4;
			mem_copy(m_RelayAddr.ip, aAddr, 4);
			m_RelayAddr.port = (unsigned short)((aPort[0] << 8) | aPort[1]);
			m_RelayAddrIsProxy = false;
		}
		return true;
	}
	if(ATYP == SOCKS5_ATYP_IPV6)
	{
		// 4 header + 16 addr + 2 port = 22
		if(m_HsRecvPos < 22)
			return false;
		mem_copy(aPort, m_aHsRecvBuf + 20, 2);
		unsigned char *aAddr = m_aHsRecvBuf + 4;
		bool IsZero = true;
		for(int i = 0; i < 16; ++i)
			if(aAddr[i] != 0)
			{
				IsZero = false;
				break;
			}
		if(IsZero)
		{
			m_RelayAddr = m_ProxyAddr;
			m_RelayAddr.port = (unsigned short)((aPort[0] << 8) | aPort[1]);
			m_RelayAddrIsProxy = true;
		}
		else
		{
			mem_zero(&m_RelayAddr, sizeof(m_RelayAddr));
			m_RelayAddr.type = NETTYPE_IPV6;
			mem_copy(m_RelayAddr.ip, aAddr, 16);
			m_RelayAddr.port = (unsigned short)((aPort[0] << 8) | aPort[1]);
			m_RelayAddrIsProxy = false;
		}
		return true;
	}
	if(ATYP == SOCKS5_ATYP_DOMAIN)
	{
		// 4 header + 1 len + N domain + 2 port
		if(m_HsRecvPos < 5)
			return false;
		unsigned char DomainLen = m_aHsRecvBuf[4];
		int Total = 4 + 1 + DomainLen + 2;
		if(m_HsRecvPos < Total)
		{
			// Need more bytes — extend the need and return false to keep receiving.
			m_HsRecvNeed = Total;
			return false;
		}
		char aDomain[256];
		if(DomainLen >= sizeof(aDomain))
		{
			Fail("udp-assoc: relay domain too long");
			return false;
		}
		mem_copy(aDomain, m_aHsRecvBuf + 5, DomainLen);
		aDomain[DomainLen] = '\0';
		mem_copy(aPort, m_aHsRecvBuf + 5 + DomainLen, 2);
		NETADDR Resolved;
		if(net_host_lookup(aDomain, &Resolved, NETTYPE_IPV4) != 0 &&
			net_host_lookup(aDomain, &Resolved, NETTYPE_IPV6) != 0)
		{
			Fail("udp-assoc: cannot resolve relay domain");
			return false;
		}
		Resolved.port = (unsigned short)((aPort[0] << 8) | aPort[1]);
		m_RelayAddr = Resolved;
		m_RelayAddrIsProxy = false;
		return true;
	}
	Fail("udp-assoc: unsupported ATYP");
	return false;
}

void CSocks5Proxy::UpdateHandshake()
{
	// Total handshake timeout check.
	if(time_get() - m_HsStartTime > time_freq() * HANDSHAKE_TOTAL_TIMEOUT_SEC)
	{
		Fail("handshake: total timeout exceeded");
		return;
	}

	switch(m_HsState)
	{
	case EHsState::AUTH_SEND:
	case EHsState::USERPASS_SEND:
	case EHsState::ASSOC_SEND:
	{
		// Try to send remaining bytes. First wait for writability (non-blocking).
		int Ready = sock_wait(m_TcpControl, false, true, SELECT_TIMEOUT_MS);
		if(Ready < 0)
		{
			Fail("handshake: select error (send)");
			return;
		}
		if(Ready == 0)
			return; // not writable yet, try next tick

		int Remaining = m_HsSendLen - m_HsSendPos;
		int r = net_tcp_send(m_TcpControl, m_aHsSendBuf + m_HsSendPos, Remaining);
		if(r <= 0)
		{
			// Would-block is fine; anything else is fatal.
#if defined(CONF_FAMILY_WINDOWS)
			int err = WSAGetLastError();
			if(err != WSAEWOULDBLOCK)
#else
			int err = errno;
			if(err != EAGAIN && err != EWOULDBLOCK)
#endif
			{
				Fail("handshake: send failed");
			}
			return;
		}
		m_HsSendPos += r;
		if(m_HsSendPos >= m_HsSendLen)
		{
			// Done sending this phase — advance to the recv phase.
			if(m_HsState == EHsState::AUTH_SEND)
			{
				m_HsState = EHsState::AUTH_RECV;
				m_HsRecvNeed = 2;
				m_HsRecvPos = 0;
			}
			else if(m_HsState == EHsState::USERPASS_SEND)
			{
				m_HsState = EHsState::USERPASS_RECV;
				m_HsRecvNeed = 2;
				m_HsRecvPos = 0;
			}
			else // ASSOC_SEND
			{
				m_HsState = EHsState::ASSOC_RECV_HDR;
				m_HsRecvNeed = 4;
				m_HsRecvPos = 0;
			}
		}
		break;
	}

	case EHsState::AUTH_RECV:
	{
		int Ready = sock_wait(m_TcpControl, true, false, SELECT_TIMEOUT_MS);
		if(Ready < 0)
		{
			Fail("handshake: select error (auth recv)");
			return;
		}
		if(Ready == 0)
			return;
		int r = net_tcp_recv(m_TcpControl, m_aHsRecvBuf + m_HsRecvPos, m_HsRecvNeed - m_HsRecvPos);
		if(r == 0)
		{
			Fail("handshake: proxy closed during auth");
			return;
		}
		if(r < 0)
		{
#if defined(CONF_FAMILY_WINDOWS)
			int err = WSAGetLastError();
			if(err != WSAEWOULDBLOCK)
#else
			int err = errno;
			if(err != EAGAIN && err != EWOULDBLOCK)
#endif
			{
				Fail("handshake: auth recv error");
			}
			return;
		}
		m_HsRecvPos += r;
		if(m_HsRecvPos >= m_HsRecvNeed)
		{
			if(ProcessAuthReply())
			{
				// Auth OK — send UDP ASSOCIATE. ProcessAuthReply may have
				// switched us to USERPASS_SEND instead (proxy picked
				// username/password auth) — only advance when it did not.
				if(m_HsState == EHsState::AUTH_RECV)
				{
					m_HsState = EHsState::ASSOC_SEND;
					PrepareAssocSend();
				}
			}
			// On failure ProcessAuthReply already called Fail().
		}
		break;
	}

	case EHsState::USERPASS_RECV:
	{
		int Ready = sock_wait(m_TcpControl, true, false, SELECT_TIMEOUT_MS);
		if(Ready < 0)
		{
			Fail("handshake: select error (userpass recv)");
			return;
		}
		if(Ready == 0)
			return;
		int r = net_tcp_recv(m_TcpControl, m_aHsRecvBuf + m_HsRecvPos, m_HsRecvNeed - m_HsRecvPos);
		if(r == 0)
		{
			Fail("handshake: proxy closed during userpass auth");
			return;
		}
		if(r < 0)
		{
#if defined(CONF_FAMILY_WINDOWS)
			int err = WSAGetLastError();
			if(err != WSAEWOULDBLOCK)
#else
			int err = errno;
			if(err != EAGAIN && err != EWOULDBLOCK)
#endif
			{
				Fail("handshake: userpass recv error");
			}
			return;
		}
		m_HsRecvPos += r;
		if(m_HsRecvPos >= m_HsRecvNeed)
		{
			if(ProcessUserPassReply())
			{
				// Credentials accepted — send UDP ASSOCIATE.
				m_HsState = EHsState::ASSOC_SEND;
				PrepareAssocSend();
			}
			// On failure ProcessUserPassReply already called Fail().
		}
		break;
	}

	case EHsState::ASSOC_RECV_HDR:
	{
		int Ready = sock_wait(m_TcpControl, true, false, SELECT_TIMEOUT_MS);
		if(Ready < 0)
		{
			Fail("handshake: select error (assoc hdr)");
			return;
		}
		if(Ready == 0)
			return;
		int r = net_tcp_recv(m_TcpControl, m_aHsRecvBuf + m_HsRecvPos, m_HsRecvNeed - m_HsRecvPos);
		if(r == 0)
		{
			Fail("handshake: proxy closed during assoc hdr");
			return;
		}
		if(r < 0)
		{
#if defined(CONF_FAMILY_WINDOWS)
			int err = WSAGetLastError();
			if(err != WSAEWOULDBLOCK)
#else
			int err = errno;
			if(err != EAGAIN && err != EWOULDBLOCK)
#endif
			{
				Fail("handshake: assoc hdr recv error");
			}
			return;
		}
		m_HsRecvPos += r;
		if(m_HsRecvPos >= m_HsRecvNeed)
		{
			if(ProcessAssocReplyHdr())
			{
				m_HsState = EHsState::ASSOC_RECV_ADDR;
				// m_HsRecvNeed already set by ProcessAssocReplyHdr (may be
				// larger than 4 if addr is fixed-length, or 5 for domain).
				// m_HsRecvPos stays — we keep accumulating.
			}
		}
		break;
	}

	case EHsState::ASSOC_RECV_ADDR:
	{
		int Ready = sock_wait(m_TcpControl, true, false, SELECT_TIMEOUT_MS);
		if(Ready < 0)
		{
			Fail("handshake: select error (assoc addr)");
			return;
		}
		if(Ready == 0)
			return;
		int r = net_tcp_recv(m_TcpControl, m_aHsRecvBuf + m_HsRecvPos, m_HsRecvNeed - m_HsRecvPos);
		if(r == 0)
		{
			Fail("handshake: proxy closed during assoc addr");
			return;
		}
		if(r < 0)
		{
#if defined(CONF_FAMILY_WINDOWS)
			int err = WSAGetLastError();
			if(err != WSAEWOULDBLOCK)
#else
			int err = errno;
			if(err != EAGAIN && err != EWOULDBLOCK)
#endif
			{
				Fail("handshake: assoc addr recv error");
			}
			return;
		}
		m_HsRecvPos += r;
		if(m_HsRecvPos >= m_HsRecvNeed)
		{
			if(ProcessAssocReplyFull())
			{
				// Handshake complete — create the UDP relay socket.
				NETADDR UdpBind = NETADDR_ZEROED;
				UdpBind.type = (m_RelayAddr.type & NETTYPE_IPV6) ? NETTYPE_IPV6 : NETTYPE_IPV4;
				m_UdpSocket = net_udp_create(UdpBind);
				if(!m_UdpSocket)
				{
					Fail("udp: failed to create relay socket");
					return;
				}
				m_State = EState::READY;
				m_LastKeepaliveCheck = time_get();
				{
					char aAddr[NETADDR_MAXSTRSIZE];
					net_addr_str(&m_RelayAddr, aAddr, sizeof(aAddr), true);
					char aBuf[400];
					str_format(aBuf, sizeof(aBuf), "SOCKS5 ready: relay=%s (%s). UDP traffic of this connection is now routed via the proxy.",
						aAddr, m_RelayAddrIsProxy ? "proxy-addr" : "dedicated");
					Log(IConsole::OUTPUT_LEVEL_STANDARD, aBuf);
				}
			}
			// On failure ProcessAssocReplyFull already called Fail() or
			// extended m_HsRecvNeed for domain case.
		}
		break;
	}
	}
}

bool CSocks5Proxy::Enable(const char *pAddrStr)
{
	if(m_State == EState::READY || m_State == EState::CONNECTING || m_State == EState::HANDSHAKING)
	{
		Log(IConsole::OUTPUT_LEVEL_ADDINFO, "already active, disabling first");
		Disable();
	}

	if(!ParseProxyAddr(pAddrStr))
	{
		char aBuf[300];
		str_format(aBuf, sizeof(aBuf), "invalid address '%s' (expected [user:pass@]IP:PORT or [user:pass@]host:PORT)", pAddrStr ? pAddrStr : "(null)");
		Log(IConsole::OUTPUT_LEVEL_STANDARD, aBuf);
		m_State = EState::DISABLED;
		return false;
	}

	{
		char aBuf[300];
		str_format(aBuf, sizeof(aBuf), "connecting to SOCKS5 proxy %s ...", m_aProxyAddrStr);
		Log(IConsole::OUTPUT_LEVEL_STANDARD, aBuf);
	}

	if(!StartConnect())
	{
		m_State = EState::BROKEN;
		m_NextReconnectTime = time_get() + time_freq() * RECONNECT_BACKOFF_SEC;
		Log(IConsole::OUTPUT_LEVEL_STANDARD, "proxy connect failed to start, will retry");
		return false;
	}

	return true;
}

void CSocks5Proxy::Disable()
{
	if(m_State == EState::DISABLED && !m_TcpControl && !m_UdpSocket)
		return;

	CloseSockets();
	m_State = EState::DISABLED;
	m_NextReconnectTime = 0;
	Log(IConsole::OUTPUT_LEVEL_STANDARD, "proxy disabled; traffic now goes direct");
}

void CSocks5Proxy::Update()
{
	if(m_State == EState::DISABLED)
		return;

	int64_t Now = time_get();

	// BROKEN: wait for backoff, then retry.
	if(m_State == EState::BROKEN)
	{
		if(Now >= m_NextReconnectTime)
		{
			Log(IConsole::OUTPUT_LEVEL_ADDINFO, "reconnecting to proxy...");
			if(!StartConnect())
			{
				m_NextReconnectTime = Now + time_freq() * RECONNECT_BACKOFF_SEC;
			}
		}
		return;
	}

	// CONNECTING: check if the non-blocking TCP connect has completed.
	if(m_State == EState::CONNECTING)
	{
		// Total timeout for the connect phase.
		if(Now - m_HsStartTime > time_freq() * HANDSHAKE_TOTAL_TIMEOUT_SEC)
		{
			Fail("tcp: connect timeout");
			return;
		}
		int Ready = sock_wait(m_TcpControl, false, true, SELECT_TIMEOUT_MS);
		if(Ready < 0)
		{
			Fail("tcp: select error during connect");
			return;
		}
		if(Ready == 0)
			return; // still connecting, try next tick
		// Writable — check whether connect succeeded.
		int err = sock_pending_error(m_TcpControl);
		if(err != 0)
		{
			char aBuf[80];
			str_format(aBuf, sizeof(aBuf), "tcp: connect failed (err=%d)", err);
			Fail(aBuf);
			return;
		}
		// Connected — disable Nagle and start the SOCKS5 handshake.
		sock_set_nodelay(m_TcpControl);
		m_HsState = EHsState::AUTH_SEND;
		PrepareAuthSend();
		m_State = EState::HANDSHAKING;
		m_HsStartTime = Now; // reset for handshake-phase total timeout
		return;
	}

	// HANDSHAKING: drive one step of the state machine.
	if(m_State == EState::HANDSHAKING)
	{
		UpdateHandshake();
		return;
	}

	// READY: keepalive probing.
	if(m_State != EState::READY)
		return;

	if(Now - m_LastKeepaliveCheck < time_freq() * KEEPALIVE_CHECK_INTERVAL_SEC)
		return;
	m_LastKeepaliveCheck = Now;

	if(!m_TcpControl)
	{
		Fail("control socket vanished");
		return;
	}

	// Non-blocking recv: 0 = peer closed, <0 EAGAIN = healthy, <0 other = error,
	// >0 = unexpected data (discard).
	unsigned char aProbe[16];
	int r = net_tcp_recv(m_TcpControl, aProbe, sizeof(aProbe));
	if(r == 0)
	{
		Fail("control connection closed by proxy");
		return;
	}
	if(r < 0)
	{
#if defined(CONF_FAMILY_WINDOWS)
		int err = WSAGetLastError();
		if(err != WSAEWOULDBLOCK)
#else
		int err = errno;
		if(err != EAGAIN && err != EWOULDBLOCK)
#endif
		{
			Fail("control connection error");
		}
	}
	// r > 0: unexpected data on the control channel — just ignore it.
}

// ----------------------------------------------------------------------------
// SOCKS5 UDP header build / parse
// ----------------------------------------------------------------------------

int CSocks5Proxy::BuildUdpHeader(const NETADDR *pDst, unsigned char *pOut, int OutMax) const
{
	// RSV(2) FRAG(1) ATYP(1) DST.ADDR DST.PORT(2)
	int HdrLen = 0;
	if(pDst->type & NETTYPE_IPV6)
	{
		HdrLen = 2 + 1 + 1 + 16 + 2;
		if(OutMax < HdrLen)
			return 0;
		pOut[0] = 0; // RSV high
		pOut[1] = 0; // RSV low
		pOut[2] = 0; // FRAG
		pOut[3] = SOCKS5_ATYP_IPV6;
		mem_copy(pOut + 4, pDst->ip, 16);
		pOut[20] = (unsigned char)(pDst->port >> 8);
		pOut[21] = (unsigned char)(pDst->port & 0xFF);
	}
	else
	{
		HdrLen = 2 + 1 + 1 + 4 + 2;
		if(OutMax < HdrLen)
			return 0;
		pOut[0] = 0;
		pOut[1] = 0;
		pOut[2] = 0;
		pOut[3] = SOCKS5_ATYP_IPV4;
		mem_copy(pOut + 4, pDst->ip, 4);
		pOut[8] = (unsigned char)(pDst->port >> 8);
		pOut[9] = (unsigned char)(pDst->port & 0xFF);
	}
	return HdrLen;
}

bool CSocks5Proxy::ParseUdpHeader(const unsigned char *pData, int Size, NETADDR *pSrc, int *pHeaderLen) const
{
	if(Size < 4)
		return false;
	if(pData[0] != 0 || pData[1] != 0 || pData[2] != 0)
		return false;

	unsigned char ATYP = pData[3];
	mem_zero(pSrc, sizeof(NETADDR));
	switch(ATYP)
	{
	case SOCKS5_ATYP_IPV4:
		if(Size < 4 + 4 + 2)
			return false;
		pSrc->type = NETTYPE_IPV4;
		mem_copy(pSrc->ip, pData + 4, 4);
		pSrc->port = (unsigned short)((pData[8] << 8) | pData[9]);
		*pHeaderLen = 10;
		return true;
	case SOCKS5_ATYP_IPV6:
		if(Size < 4 + 16 + 2)
			return false;
		pSrc->type = NETTYPE_IPV6;
		mem_copy(pSrc->ip, pData + 4, 16);
		pSrc->port = (unsigned short)((pData[20] << 8) | pData[21]);
		*pHeaderLen = 22;
		return true;
	case SOCKS5_ATYP_DOMAIN:
	{
		if(Size < 5)
			return false;
		unsigned char DomainLen = pData[4];
		if(Size < 5 + DomainLen + 2)
			return false;
		char aDomain[256];
		if(DomainLen >= sizeof(aDomain))
			return false;
		mem_copy(aDomain, pData + 5, DomainLen);
		aDomain[DomainLen] = '\0';
		NETADDR Resolved;
		if(net_host_lookup(aDomain, &Resolved, NETTYPE_IPV4) != 0 &&
			net_host_lookup(aDomain, &Resolved, NETTYPE_IPV6) != 0)
		{
			return false; // cannot resolve — drop packet
		}
		Resolved.port = (unsigned short)((pData[5 + DomainLen] << 8) | pData[5 + DomainLen + 1]);
		*pSrc = Resolved;
		*pHeaderLen = 5 + DomainLen + 2;
		return true;
	}
	default:
		return false;
	}
}

// ----------------------------------------------------------------------------
// UDP relay send / recv (called from CClient's net.cpp routing)
// ----------------------------------------------------------------------------

int CSocks5Proxy::UdpSend(const NETADDR *pDst, const void *pData, int Size)
{
	if(m_State != EState::READY || !m_UdpSocket)
		return -1;
	if(Size <= 0 || Size > NET_MAX_PACKETSIZE)
		return -1;

	int HdrLen = BuildUdpHeader(pDst, m_aSendBuf, sizeof(m_aSendBuf));
	if(HdrLen == 0)
		return -1;
	if(HdrLen + Size > (int)sizeof(m_aSendBuf))
		return -1;
	mem_copy(m_aSendBuf + HdrLen, pData, Size);

	// Send directly to the relay, bypassing the global hook (which would
	// otherwise recurse back into UdpSend).
	int r = net_udp_send_direct(m_UdpSocket, &m_RelayAddr, m_aSendBuf, HdrLen + Size);
	if(r <= 0)
		return -1;
	return Size;
}

int CSocks5Proxy::UdpRecv(NETADDR *pSrc, unsigned char **ppData)
{
	if(m_State != EState::READY || !m_UdpSocket)
		return 0;

	// Drain the relay socket. Multiple packets may be queued; we return one
	// per call (the caller polls in a loop).
	for(;;)
	{
		NETADDR From = NETADDR_ZEROED;
		unsigned char *pRaw = nullptr;
		int Bytes = net_udp_recv_direct(m_UdpSocket, &From, &pRaw);
		if(Bytes <= 0)
			return 0;

		// The packet must come from our relay. Be lenient on the port.
		bool FromRelay = false;
		if(net_addr_comp_noport(&From, &m_RelayAddr) == 0)
		{
			FromRelay = true;
		}
		else if(m_RelayAddrIsProxy && net_addr_comp_noport(&From, &m_ProxyAddr) == 0)
		{
			FromRelay = true;
		}
		if(!FromRelay)
			continue;

		int HeaderLen = 0;
		NETADDR RealSrc;
		if(!ParseUdpHeader(pRaw, Bytes, &RealSrc, &HeaderLen))
			continue; // malformed — drop

		*pSrc = RealSrc;
		*ppData = pRaw + HeaderLen;
		return Bytes - HeaderLen;
	}
}