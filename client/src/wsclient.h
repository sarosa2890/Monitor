#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define SECURITY_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <security.h>
#include <schannel.h>

// Минимальный WebSocket-клиент (RFC 6455): handshake, текстовые и бинарные
// фреймы, фрагментация не требуется (пишем с FIN=1), пинги отвечаем понгом.
// Поддерживает ws:// (обычный сокет) и wss:// (TLS 1.2/1.3 через Schannel).
class WsClient {
public:
    using TextHandler   = std::function<void(const std::string&)>;
    using BinaryHandler = std::function<void(int kind, const uint8_t* data, size_t len)>;

    WsClient();
    ~WsClient();

    bool connect(const std::string& host, uint16_t port, const std::string& path,
                 bool tls = false);
    void close();
    bool isConnected() const { return m_sock != INVALID_SOCKET; }

    bool sendText(const std::string& text);
    bool sendBinary(int kind, const void* data, size_t len); // kind закодирован первым байтом

    void setTextHandler(TextHandler h)   { m_text = std::move(h); }
    void setBinaryHandler(BinaryHandler h) { m_bin = std::move(h); }

    // Читает входящие фреймы. Возвращает false при разрыве/ошибке.
    bool pump();

private:
    bool sendFrame(int opcode, const void* data, size_t len);
    bool parseFrames();

    // ---- Schannel (TLS) ----
    bool tlsHandshake(const std::string& host);
    int  rawRecv(char* buf, size_t len);   // recv с сокета: >0 байт, 0 = таймаут, -1 = ошибка
    int  tlsRecv(char* buf, size_t len);   // дешифрованные байты (для HTTP-handshake)
    bool tlsDecrypt();                     // m_tlsIn (зашифр.) -> m_rbuf (прозрачно)
    bool rawSend(const void* data, size_t len); // ws: send; wss: TLS-записи

    SOCKET           m_sock = INVALID_SOCKET;
    std::vector<uint8_t> m_rbuf;   // дешифрованный поток для parseFrames
    std::vector<uint8_t> m_tlsIn;  // сырые TLS-записи с сокета

    bool             m_tls = false;
    CredHandle       m_hCred{};
    CtxtHandle       m_hCtx{};
    bool             m_ctxInit = false;
    DWORD            m_tlsHeader = 0, m_tlsTrailer = 0, m_tlsMaxMsg = 0;

    // Сериализация записи в сокет: «поштучно» голоза+payload отправляются под
    // одним мьютексом, чтобы пинг/понг (main-поток) не перемежался с большим
    // кадром экрана (поток захвата) — иначе сервер видит битый фрейм (1002).
    std::mutex       m_sendMtx;
    TextHandler      m_text;
    BinaryHandler    m_bin;
};
