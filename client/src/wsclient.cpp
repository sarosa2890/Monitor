#include "wsclient.h"

#include <cstdio>
#include <ctime>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "secur32.lib")

// ------------------------------------------------------------ base64 ----
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64Encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = data[i] << 16;
        if (i + 1 < len) v |= data[i + 1] << 8;
        if (i + 2 < len) v |= data[i + 2];
        out += B64[(v >> 18) & 63];
        out += B64[(v >> 12) & 63];
        out += (i + 1 < len) ? B64[(v >> 6) & 63] : '=';
        out += (i + 2 < len) ? B64[v & 63] : '=';
    }
    return out;
}

WsClient::WsClient() = default;

WsClient::~WsClient() { close(); }

void WsClient::close() {
    if (m_tls) {
        if (m_ctxInit) {
            DeleteSecurityContext(&m_hCtx);
            m_ctxInit = false;
        }
        if (m_hCred.dwLower || m_hCred.dwUpper) {
            FreeCredentialsHandle(&m_hCred);
            m_hCred = {};
        }
    }
    if (m_sock != INVALID_SOCKET) {
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
    }
    m_rbuf.clear();
    m_tlsIn.clear();
}

bool WsClient::connect(const std::string& host, uint16_t port, const std::string& path,
                       bool tls) {
    close();
    m_tls = tls;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%u", port);

    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), portStr, &hints, &res) != 0) return false;

    SOCKET s = INVALID_SOCKET;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;

        // неблокирующий connect с таймаутом 5 сек
        u_long mode = 1;
        ioctlsocket(s, FIONBIO, &mode);
        int rc = ::connect(s, ai->ai_addr, (int)ai->ai_addrlen);
        if (rc == SOCKET_ERROR) {
            if (WSAGetLastError() != WSAEWOULDBLOCK && WSAGetLastError() != WSAEINPROGRESS) {
                closesocket(s);
                s = INVALID_SOCKET;
                continue;
            }
            fd_set wf;
            FD_ZERO(&wf);
            FD_SET(s, &wf);
            timeval tv{5, 0};
            if (select(0, nullptr, &wf, nullptr, &tv) <= 0) {
                closesocket(s);
                s = INVALID_SOCKET;
                continue;
            }
        }
        mode = 0;
        ioctlsocket(s, FIONBIO, &mode);
        // блокирующий send не должен висеть вечно: таймаут 5 сек
        DWORD sndTmo = 5000;
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sndTmo, sizeof(sndTmo));
        break;
    }
    freeaddrinfo(res);
    if (s == INVALID_SOCKET) return false;
    m_sock = s;

    if (m_tls) {
        // таймаут приёма для рукопожатия
        DWORD rcvTmo = 5000;
        setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rcvTmo, sizeof(rcvTmo));
        if (!tlsHandshake(host)) { close(); return false; }
    }

    // ----------------------------------------------------- handshake -----
    uint8_t raw[16];
    for (int i = 0; i < 16; i++) raw[i] = (uint8_t)(rand() & 0xFF);
    std::string key = base64Encode(raw, 16);

    std::string req = "GET " + path + " HTTP/1.1\r\n"
                      "Host: " + host + ":" + portStr + "\r\n"
                      "Upgrade: websocket\r\n"
                      "Connection: Upgrade\r\n"
                      "Sec-WebSocket-Key: " + key + "\r\n"
                      "Sec-WebSocket-Version: 13\r\n"
                      "User-Agent: FoxMonitor/1.0\r\n\r\n";

    if (!rawSend(req.data(), req.size())) { close(); return false; }

    // таймаут приёма только для handshake (дальше вернём блокирующий режим)
    DWORD rcvTmo = 5000;
    setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rcvTmo, sizeof(rcvTmo));
    std::string resp;
    char buf[512];
    while (resp.find("\r\n\r\n") == std::string::npos) {
        int n = m_tls ? tlsRecv(buf, sizeof(buf)) : rawRecv(buf, sizeof(buf));
        if (n <= 0) { printf("[net] handshake recv failed (%d)\n", n); fflush(stdout); close(); return false; }
        resp.append(buf, (size_t)n);
        if (resp.size() > 16384) { close(); return false; }
    }
    if (resp.find(" 101 ") == std::string::npos) { close(); return false; }
    // приём с таймаутом 5 сек: main-цикл должен регулярно проверять g_wsDead,
    // иначе молчаливое зависание, если сервер перестал отвечать
    DWORD rcvTmo2 = 5000;
    setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rcvTmo2, sizeof(rcvTmo2));
    return true;
}

// --------------------------------------------------------------- TLS -------
// Schannel: рукопожатие (TLS 1.2/1.3), затем каждое send/recv идёт через
// EncryptMessage/DecryptMessage. Сертификат сервера проверяется по системе.

bool WsClient::rawSend(const void* data, size_t len) {
    if (!m_tls)
        return send(m_sock, (const char*)data, (int)len, 0) != SOCKET_ERROR;

    const uint8_t* p = (const uint8_t*)data;
    size_t chunk = m_tlsMaxMsg ? m_tlsMaxMsg : 1400;
    while (len > 0) {
        size_t take = len < chunk ? len : chunk;
        std::vector<uint8_t> rec(m_tlsHeader + take + m_tlsTrailer);
        SecBuffer bufs[4] = {};
        bufs[0].BufferType = SECBUFFER_STREAM_HEADER;
        bufs[0].pvBuffer = rec.data();
        bufs[0].cbBuffer = m_tlsHeader;
        bufs[1].BufferType = SECBUFFER_DATA;
        bufs[1].pvBuffer = rec.data() + m_tlsHeader;
        bufs[1].cbBuffer = (ULONG)take;
        memcpy(bufs[1].pvBuffer, p, take);
        bufs[2].BufferType = SECBUFFER_STREAM_TRAILER;
        bufs[2].pvBuffer = rec.data() + m_tlsHeader + take;
        bufs[2].cbBuffer = m_tlsTrailer;
        bufs[3].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc desc = {SECBUFFER_VERSION, 4, bufs};
        SECURITY_STATUS ss = EncryptMessage(&m_hCtx, 0, &desc, 0);
        if (ss != SEC_E_OK) return false;
        size_t total = bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer;
        const char* out = (const char*)rec.data();
        size_t sent = 0;
        while (sent < total) {
            int n = send(m_sock, out + sent, (int)(total - sent), 0);
            if (n == SOCKET_ERROR) return false;
            sent += (size_t)n;
        }
        p += take;
        len -= take;
    }
    return true;
}

int WsClient::rawRecv(char* buf, size_t len) {
    int n = recv(m_sock, buf, (int)len, 0);
    if (n == SOCKET_ERROR) {
        if (WSAGetLastError() == WSAETIMEDOUT) return 0; // тишина — не обрыв
        return -1;
    }
    if (n == 0) return -1;
    return n;
}

bool WsClient::tlsHandshake(const std::string& host) {
    SCHANNEL_CRED cred{};
    cred.dwVersion = SCHANNEL_CRED_VERSION;
    cred.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT | SP_PROT_TLS1_3_CLIENT;

    SECURITY_STATUS ss = AcquireCredentialsHandleW(
        nullptr, (LPWSTR)UNISP_NAME, SECPKG_CRED_OUTBOUND, nullptr, &cred,
        nullptr, nullptr, &m_hCred, nullptr);
    if (ss != SEC_E_OK) {
        printf("[net] tls: AcquireCredentialsHandle failed 0x%x\n", ss);
        fflush(stdout);
        return false;
    }

    DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
                  ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY |
                  ISC_REQ_STREAM;

    std::wstring whost(host.begin(), host.end());
    bool first = true;
    for (;;) {
        SecBufferDesc outDesc{};
        SecBuffer outBuf{};
        outBuf.BufferType = SECBUFFER_TOKEN;
        outBuf.pvBuffer = nullptr;
        outBuf.cbBuffer = 0;
        outDesc.ulVersion = SECBUFFER_VERSION;
        outDesc.cBuffers = 1;
        outDesc.pBuffers = &outBuf;

        SecBufferDesc inDesc{};
        SecBuffer inBufs[2] = {};
        SECURITY_STATUS r = SEC_E_OK;
        if (first) {
            r = InitializeSecurityContextW(&m_hCred, nullptr,
                (SEC_WCHAR*)whost.c_str(), flags, 0, SECURITY_NATIVE_DREP,
                nullptr, 0, &m_hCtx, &outDesc, &flags, nullptr);
            first = false;
        } else {
            if (!m_tlsIn.empty()) {
                inBufs[0].BufferType = SECBUFFER_TOKEN;
                inBufs[0].pvBuffer = m_tlsIn.data();
                inBufs[0].cbBuffer = (ULONG)m_tlsIn.size();
                inBufs[1].BufferType = SECBUFFER_EMPTY;
                inDesc.ulVersion = SECBUFFER_VERSION;
                inDesc.cBuffers = 2;
                inDesc.pBuffers = inBufs;
            }
            r = InitializeSecurityContextW(&m_hCred, &m_hCtx,
                (SEC_WCHAR*)whost.c_str(), flags, 0, SECURITY_NATIVE_DREP,
                inDesc.cBuffers ? &inDesc : nullptr, 0, &m_hCtx,
                &outDesc, &flags, nullptr);
            m_tlsIn.clear();
        }

        if (outBuf.cbBuffer > 0 && outBuf.pvBuffer) {
            size_t sent = 0;
            const char* p = (const char*)outBuf.pvBuffer;
            while (sent < outBuf.cbBuffer) {
                int n = send(m_sock, p + sent, outBuf.cbBuffer - (int)sent, 0);
                if (n == SOCKET_ERROR) break;
                sent += (size_t)n;
            }
            FreeContextBuffer(outBuf.pvBuffer);
            if (sent != outBuf.cbBuffer) return false;
        }

        if (r == SEC_E_OK) {
            SecPkgContext_StreamSizes sizes{};
            if (QueryContextAttributesW(&m_hCtx, SECPKG_ATTR_STREAM_SIZES,
                                        &sizes) != SEC_E_OK) {
                printf("[net] tls: stream sizes query failed\n");
                fflush(stdout);
                return false;
            }
            m_tlsHeader = sizes.cbHeader;
            m_tlsTrailer = sizes.cbTrailer;
            m_tlsMaxMsg = sizes.cbMaximumMessage;
            printf("[net] tls: handshake ok (hdr=%u trl=%u max=%u)\n",
                   m_tlsHeader, m_tlsTrailer, m_tlsMaxMsg);
            fflush(stdout);
            return true;
        }
        if (r == SEC_E_INCOMPLETE_MESSAGE || r == SEC_I_CONTINUE_NEEDED ||
            r == SEC_I_INCOMPLETE_CREDENTIALS) {
            char buf[8192];
            int n = rawRecv(buf, sizeof(buf));
            if (n <= 0) return false;
            m_tlsIn.insert(m_tlsIn.end(), buf, buf + n);
            continue;
        }
        printf("[net] tls: InitializeSecurityContext 0x%x\n", r);
        fflush(stdout);
        return false;
    }
}

bool WsClient::tlsDecrypt() {
    while (!m_tlsIn.empty()) {
        SecBuffer bufs[4] = {};
        bufs[0].BufferType = SECBUFFER_DATA;
        bufs[0].pvBuffer = m_tlsIn.data();
        bufs[0].cbBuffer = (ULONG)m_tlsIn.size();
        bufs[1].BufferType = SECBUFFER_EMPTY;
        bufs[2].BufferType = SECBUFFER_EMPTY;
        bufs[3].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc desc = {SECBUFFER_VERSION, 4, bufs};

        SECURITY_STATUS ss = DecryptMessage(&m_hCtx, &desc, 0, nullptr);
        if (ss == SEC_E_OK) {
            std::vector<uint8_t> extra;
            for (int i = 0; i < 4; i++) {
                if (bufs[i].BufferType == SECBUFFER_DATA && bufs[i].cbBuffer) {
                    const uint8_t* d = (const uint8_t*)bufs[i].pvBuffer;
                    m_rbuf.insert(m_rbuf.end(), d, d + bufs[i].cbBuffer);
                } else if (bufs[i].BufferType == SECBUFFER_EXTRA &&
                           bufs[i].cbBuffer) {
                    const uint8_t* d = (const uint8_t*)bufs[i].pvBuffer;
                    extra.assign(d, d + bufs[i].cbBuffer);
                }
            }
            m_tlsIn.swap(extra);
            continue;
        }
        if (ss == SEC_E_INCOMPLETE_MESSAGE) return true; // ждём ещё байтов
        printf("[net] tls: DecryptMessage 0x%x\n", ss);
        fflush(stdout);
        return false;
    }
    return true;
}

int WsClient::tlsRecv(char* buf, size_t len) {
    // Вначале отдаём уже дешифрованные байты, затем дочитываем сокет.
    for (;;) {
        if (!m_rbuf.empty()) {
            size_t n = len < m_rbuf.size() ? len : m_rbuf.size();
            memcpy(buf, m_rbuf.data(), n);
            m_rbuf.erase(m_rbuf.begin(), m_rbuf.begin() + (long long)n);
            return (int)n;
        }
        char tmp[8192];
        int n = rawRecv(tmp, sizeof(tmp));
        if (n <= 0) return n;
        m_tlsIn.insert(m_tlsIn.end(), tmp, tmp + n);
        if (!tlsDecrypt()) return -1;
    }
}

// ------------------------------------------------------------- frames ----
bool WsClient::sendFrame(int opcode, const void* data, size_t len) {
    if (m_sock == INVALID_SOCKET) return false;
    std::lock_guard<std::mutex> lk(m_sendMtx);

    uint8_t hdr[14];
    size_t h = 0;
    hdr[h++] = (uint8_t)(0x80 | (opcode & 0x0F));      // FIN + opcode

    if (len <= 125) {
        hdr[h++] = (uint8_t)(0x80 | len);              // mask + len7
    } else if (len <= 0xFFFF) {
        hdr[h++] = 0x80 | 126;
        hdr[h++] = (uint8_t)(len >> 8);
        hdr[h++] = (uint8_t)(len & 0xFF);
    } else {
        hdr[h++] = 0x80 | 127;
        uint64_t l = len;
        for (int i = 7; i >= 0; i--) hdr[h++] = (uint8_t)(l >> (i * 8));
    }

    uint8_t maskKey[4];
    for (int i = 0; i < 4; i++) maskKey[i] = (uint8_t)(rand() & 0xFF);
    memcpy(hdr + h, maskKey, 4);
    h += 4;

    std::vector<uint8_t> frame(h + len);
    memcpy(frame.data(), hdr, h);
    const uint8_t* src = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) frame[h + i] = src[i] ^ maskKey[i % 4];

    return rawSend(frame.data(), frame.size());
}

bool WsClient::sendText(const std::string& text) {
    return sendFrame(0x1, text.data(), text.size());
}

bool WsClient::sendBinary(int kind, const void* data, size_t len) {
    std::vector<uint8_t> pkt(5 + len);
    pkt[0] = (uint8_t)kind;
    uint32_t l = (uint32_t)len;
    pkt[1] = (uint8_t)(l >> 24);
    pkt[2] = (uint8_t)(l >> 16);
    pkt[3] = (uint8_t)(l >> 8);
    pkt[4] = (uint8_t)(l & 0xFF);
    memcpy(pkt.data() + 5, data, len);
    return sendFrame(0x2, pkt.data(), pkt.size());
}

bool WsClient::pump() {
    if (m_sock == INVALID_SOCKET) return false;
    if (m_tls) {
        char tmp[8192];
        int n = rawRecv(tmp, sizeof(tmp));
        if (n == 0) return true; // таймаут — тишина, не обрыв
        if (n < 0) {
            printf("[net] tls recv error %d\n", WSAGetLastError()); fflush(stdout);
            return false;
        }
        m_tlsIn.insert(m_tlsIn.end(), tmp, tmp + n);
        if (!tlsDecrypt()) return false;
        return parseFrames();
    }
    uint8_t buf[8192];
    int n = recv(m_sock, (char*)buf, sizeof(buf), 0);
    if (n == SOCKET_ERROR) {
        if (WSAGetLastError() == WSAETIMEDOUT) return true; // тишина — не обрыв
        printf("[net] recv error %d\n", WSAGetLastError()); fflush(stdout);
        return false;
    }
    if (n <= 0) {
        printf("[net] recv closed by server\n"); fflush(stdout);
        return false;
    }
    m_rbuf.insert(m_rbuf.end(), buf, buf + n);
    return parseFrames();
}

bool WsClient::parseFrames() {
    size_t off = 0;
    for (;;) {
        if (m_rbuf.size() - off < 2) break;

        const uint8_t* p = m_rbuf.data() + off;
        bool fin = (p[0] & 0x80) != 0;
        int opcode = p[0] & 0x0F;
        bool masked = (p[1] & 0x80) != 0;
        uint64_t len = p[1] & 0x7F;

        size_t hdr = 2;
        if (len == 126) {
            if (m_rbuf.size() - off < 4) break;
            len = ((uint64_t)m_rbuf[off + 2] << 8) | m_rbuf[off + 3];
            hdr = 4;
        } else if (len == 127) {
            if (m_rbuf.size() - off < 10) break;
            len = 0;
            for (int i = 0; i < 8; i++) len = (len << 8) | m_rbuf[off + 2 + i];
            hdr = 10;
        }
        if (len > 0xFFFFFFFFull) return false; // защита от мусора (потолок uint32 BE32)

        uint8_t maskKey[4] = {0, 0, 0, 0};
        if (masked) {
            if (m_rbuf.size() - off < hdr + 4) break;
            memcpy(maskKey, m_rbuf.data() + off + hdr, 4);
            hdr += 4;
        }
        if (m_rbuf.size() - off < hdr + len) break;

        const uint8_t* payload = m_rbuf.data() + off + hdr;
        std::vector<uint8_t> data(payload, payload + len);
        if (masked) {
            for (size_t i = 0; i < data.size(); i++) data[i] ^= maskKey[i % 4];
        }
        off += hdr + len;

        switch (opcode) {
            case 0x1: // text
                if (m_text) m_text(std::string((const char*)data.data(), data.size()));
                break;
            case 0x2: // binary: [kind][len BE32][payload]
                if (m_bin && data.size() >= 5) {
                    uint32_t pl = ((uint32_t)data[1] << 24) | ((uint32_t)data[2] << 16) |
                                  ((uint32_t)data[3] << 8) | data[4];
                    if (pl <= data.size() - 5)
                        m_bin(data[0], data.data() + 5, pl);
                }
                break;
            case 0x8: // close
                printf("[net] close frame (code=%u)\n",
                       (data.size() >= 2) ? (((uint16_t)data[0] << 8) | data[1]) : 0u);
                fflush(stdout);
                return false;
            case 0x9: // ping -> pong
                sendFrame(0xA, data.data(), data.size());
                break;
            case 0xA: // pong
            default:
                break;
        }
    }
    m_rbuf.erase(m_rbuf.begin(), m_rbuf.begin() + (long long)off);
    return true;
}
