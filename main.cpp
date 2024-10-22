#define WIN32_LEAN_AND_MEAN

#include "windows.h"
#include "winsock2.h"
#include "ws2tcpip.h"
#include "iphlpapi.h"
#include <stdio.h>
#include <string>
#include <locale>
#include <codecvt>

#include "audio/audio_mixer.hpp"

#include "tts.h"

#pragma comment(lib, "ws2_32.lib")

std::string wsaErrorToString(int err) {
    char* s = 0;
    FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        0, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&s, 0, 0
    );
    std::string out = s;
    LocalFree(s);
    return out;
}

struct IRC_MESSAGE {
    std::string tags;
    std::string servername_or_nick;
    std::string user;
    std::string host;
    std::string command;
    std::string params;
};

void ircPrintMsg(const IRC_MESSAGE& msg) {
    printf("MESSAGE:\n");
    printf("\ttags: %s\n", msg.tags.c_str());
    printf("\tservername_or_nick: %s\n", msg.servername_or_nick.c_str());
    printf("\tuser: %s\n", msg.user.c_str());
    printf("\thost: %s\n", msg.host.c_str());
    printf("\tcommand: %s\n", msg.command.c_str());
    printf("\tparams: %s\n", msg.params.c_str());
}

class irc_parse_exception : public std::exception {
public:
    irc_parse_exception(const char* msg)
    : std::exception(msg) {}
};

struct irc_parse_state {
    const char* str;
    const char* cur;
};

bool ircParseAccept(irc_parse_state& ps, char ch) {
    if (*ps.cur == ch) {
        ++ps.cur;
        return true;
    }
    return false;
}
bool ircParseAccept(irc_parse_state& ps, const char* str) {
    if (0 == strncmp(ps.cur, str, strlen(str))) {
        ps.cur += strlen(str);
        return true;
    }
    return false;
}
bool ircParseAcceptAnyNotOf(irc_parse_state& ps, const char* chars) {
    for (int i = 0; i < strlen(chars); ++i) {
        if (*ps.cur == chars[i]) {
            return false;
        }
    }

    ++ps.cur;
    return true;
}
bool ircParseEatAnyNotOf(irc_parse_state& ps, std::string& out, const char* chars) {
    const char* start = ps.cur;
    while (1) {
        bool done = false;
        for (int i = 0; i < strlen(chars); ++i) {
            if (*ps.cur == '\0' || *ps.cur == chars[i]) {
                done = true;
                break;
            }
        }
        if (done) {
            break;
        }
        ++ps.cur;
    }
    if (start == ps.cur) {
        return false;
    }
    out = std::string(start, ps.cur);
    return true;
}

void ircParseExpect(irc_parse_state& ps, char ch) {
    if (*ps.cur == ch) {
        ++ps.cur;
        return;
    }

    char buf[256];
    snprintf(buf, 256, "Expected a '%c', got '%c'", ch, *ps.cur);
    throw irc_parse_exception(buf);
}
void ircParseExpect(irc_parse_state& ps, const char* str) {
    if (0 == strncmp(ps.cur, str, strlen(str))) {
        ps.cur += strlen(str);
        return;
    }
    throw irc_parse_exception("Expected <crlf>");
}

bool ircParseTags(irc_parse_state& ps, IRC_MESSAGE* irc_msg) {
    if (!ircParseAccept(ps, '@')) {
        return false;
    }

    const char* start = ps.cur;

    const char* p = strstr(ps.cur, " ");
    if (p == 0) {
        throw irc_parse_exception("Expected a space");
    }

    irc_msg->tags = std::string(start, p);

    ps.cur = p + 1;

    return true;
}
bool ircParsePrefix(irc_parse_state& ps, IRC_MESSAGE* irc_msg) {
    const char* start = ps.cur;
    if (!ircParseEatAnyNotOf(ps, irc_msg->servername_or_nick, " !")) {
        throw irc_parse_exception("Expected <servername> or <nick>");
    } 

    if (ircParseAccept(ps, '!')) {
        if (!ircParseEatAnyNotOf(ps, irc_msg->user, " @")) {
            throw irc_parse_exception("Expected <user>");
        }
    }
    if (ircParseAccept(ps, '@')) {
        if (!ircParseEatAnyNotOf(ps, irc_msg->host, " ")) {
            throw irc_parse_exception("Expected <host>");
        }
    }

    return true;
}
bool ircParsePrefixPart(irc_parse_state& ps, IRC_MESSAGE* irc_msg) {
    if (!ircParseAccept(ps, ':')) {
        return false;
    }
    if (!ircParsePrefix(ps, irc_msg)) {
        throw irc_parse_exception("Expected a prefix");
    }
    ircParseExpect(ps, ' ');
}

bool ircParseCommand(irc_parse_state& ps, IRC_MESSAGE* irc_msg) {
    const char* start = ps.cur;
    if (isalpha(*ps.cur)) {
        while (isalpha(*ps.cur)) {
            ps.cur++;
        }
    } else if(isdigit(*ps.cur)) {
        while (isdigit(*ps.cur)) {
            ps.cur++;
        }
    } else {
        return false;
    }
    irc_msg->command = std::string(start, ps.cur);
    return true;
}
bool ircParseParams(irc_parse_state& ps, IRC_MESSAGE* irc_msg) {
    if (!ircParseAccept(ps, ' ')) {
        return false;
    }

    ircParseEatAnyNotOf(ps, irc_msg->params, "\r");
    return true;
}

bool ircParsePrivmsgReceiver(irc_parse_state& ps, std::string& out) {
    ircParseExpect(ps, '#');
    ircParseEatAnyNotOf(ps, out, " \r\n");
    return true;
}


class TwitchIrcSocket;
void ircHandleMessage(TwitchIrcSocket& sock, const std::string& msg);


class Socket {
    SOCKET sock = INVALID_SOCKET;
    std::string addr;
    std::string port;
public:
    virtual ~Socket() {
        if (sock != INVALID_SOCKET) {
            closesocket(sock);
            sock = INVALID_SOCKET;
        }
    }

    const std::string& getAddr() const { return addr; }
    const std::string& getPort() const { return port; }
    SOCKET getSock() { return sock; }

    virtual void onSocketConnected() = 0;

    void close() {
        closesocket(sock);
        sock = INVALID_SOCKET;
    }

    bool conn(const char* addr, const char* port) {
        this->addr = addr;
        this->port = port;

        addrinfo* result = 0;
        addrinfo hints = { 0 };
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        int iResult = getaddrinfo(addr, port, &hints, &result);
        if (iResult != 0) {
            printf("getaddrinfo failed: %i\n", iResult);
            return false;
        }

        addrinfo* ptr = result;
        sock = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (sock == INVALID_SOCKET) {
            printf("socket() failed: %ld\n", WSAGetLastError());
            freeaddrinfo(result);
            return false;
        }

        bool connected = false;
        while (ptr) {
            char saddr[128];
            inet_ntop(AF_INET, &ptr->ai_addr, saddr, 128);
            printf("Trying %s:%i\n", saddr, ntohs(((sockaddr_in*)ptr->ai_addr)->sin_port));
            iResult = connect(sock, ptr->ai_addr, (int)ptr->ai_addrlen);
            if (iResult == SOCKET_ERROR) {
                printf("connect() failed: %ld, %s\n", WSAGetLastError(), wsaErrorToString(WSAGetLastError()).c_str());
                ptr = ptr->ai_next;
                continue;
            }
            connected = true;
            break;
        }
        if (!connected) {
            LOG_ERR("Failed to connect");
            freeaddrinfo(result);
            closesocket(sock);
            sock = INVALID_SOCKET;
            return false;
        }
        LOG("Socket connected");

        onSocketConnected();

        freeaddrinfo(result);
        return true;
    }

    bool reconnect() {
        return conn(addr.c_str(), port.c_str());
    }
    
    bool sendRaw(const std::string& data) {
        int iResult = send(sock, data.c_str(), data.size(), 0);
        if (iResult == SOCKET_ERROR) {
            printf("send() failed: %ld, %s", WSAGetLastError(), wsaErrorToString(WSAGetLastError()).c_str());
            return false;
        }
        return true;
    }

    int hasData() {
        fd_set fdset;
        fdset.fd_array[0] = sock;
        fdset.fd_count = 1;
        int iResult = select(0, &fdset, 0, 0, 0);
        if (iResult == SOCKET_ERROR) {
            printf("select() failed: %ld, %s", WSAGetLastError(), wsaErrorToString(WSAGetLastError()).c_str());
        }
        return iResult;
    }

};


#include "lib/openssl/include/openssl/ssl.h"
#include "lib/openssl/include/openssl/err.h"
#pragma comment(lib, "lib/openssl/lib/libssl_static.lib")
#pragma comment(lib, "lib/openssl/lib/libcrypto_static.lib")
#pragma comment(lib, "crypt32.lib")

class TLSSocket {
    SOCKET sock = INVALID_SOCKET;
    std::string addr;
    std::string port;
    SSL_CTX* ssl_ctx = 0;
protected:
    SSL* ssl = 0;
public:
    TLSSocket() {
        const SSL_METHOD* method = TLS_client_method();
        ssl_ctx = SSL_CTX_new(method);
        if (ssl_ctx == nullptr) {
            LOG_ERR("SSL_CTX_new error");
            return;
        }
        ssl = SSL_new(ssl_ctx);
        //SSL_set_min_proto_version(ssl, SSL2_VERSION);
        //SSL_set_max_proto_version(ssl, TLS1_3_VERSION);
    }
    virtual ~TLSSocket() {
        if (sock != INVALID_SOCKET) {
            closesocket(sock);
            sock = INVALID_SOCKET;
        }
        SSL_free(ssl);
        SSL_CTX_free(ssl_ctx);
    }

    const std::string& getAddr() const { return addr; }
    const std::string& getPort() const { return port; }
    SOCKET getSock() { return sock; }

    virtual void onSocketConnected() = 0;
    
    void close() {
        closesocket(sock);
        sock = INVALID_SOCKET;
    }

    bool conn(const char* addr, const char* port) {
        this->addr = addr;
        this->port = port;

        addrinfo* result = 0;
        addrinfo hints = { 0 };
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        int iResult = getaddrinfo(addr, port, &hints, &result);
        if (iResult != 0) {
            printf("getaddrinfo failed: %i\n", iResult);
            return false;
        }

        addrinfo* ptr = result;
        sock = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (sock == INVALID_SOCKET) {
            printf("socket() failed: %ld\n", WSAGetLastError());
            freeaddrinfo(result);
            return false;
        }

        bool connected = false;
        while (ptr) {
            char saddr[128];
            inet_ntop(AF_INET, &ptr->ai_addr, saddr, 128);
            printf("Trying %s:%i\n", saddr, ntohs(((sockaddr_in*)ptr->ai_addr)->sin_port));
            iResult = connect(sock, ptr->ai_addr, (int)ptr->ai_addrlen);
            if (iResult == SOCKET_ERROR) {
                printf("connect() failed: %ld, %s\n", WSAGetLastError(), wsaErrorToString(WSAGetLastError()).c_str());
                ptr = ptr->ai_next;
                continue;
            }
            connected = true;
            break;
        }
        if (!connected) {
            LOG_ERR("Failed to connect");
            freeaddrinfo(result);
            closesocket(sock);
            sock = INVALID_SOCKET;
            return false;
        }
        LOG("Socket connected");

        SSL_set_fd(ssl, sock);
        SSL_set_tlsext_host_name(ssl, addr);
        iResult = SSL_connect(ssl);
        if (iResult != 1) {
            ERR_print_errors_fp(stderr);
            iResult = SSL_get_error(ssl, iResult);
            LOG_ERR("SSL_connect failed with: " << iResult);
            
            freeaddrinfo(result);
            closesocket(sock);
            sock = INVALID_SOCKET;
            return false;
        }

        onSocketConnected();

        freeaddrinfo(result);
        return true;
    }
    
    bool sendRaw(const std::string& data) {
        int iResult = SSL_write(ssl, data.c_str(), data.size());
        //int iResult = send(sock, data.c_str(), data.size(), 0);
        if (iResult == SOCKET_ERROR) {
            printf("SSL_write() failed: %ld, %s", WSAGetLastError(), wsaErrorToString(WSAGetLastError()).c_str());
            return false;
        }
        return true;
    }

    int hasData() {
        fd_set fdset;
        fdset.fd_array[0] = sock;
        fdset.fd_count = 1;
        int iResult = select(0, &fdset, 0, 0, 0);
        if (iResult == SOCKET_ERROR) {
            printf("select() failed: %ld, %s", WSAGetLastError(), wsaErrorToString(WSAGetLastError()).c_str());
        }
        return iResult;
    }
};


enum HTTP_METHOD {
    HTTP_METHOD_GET,
    HTTP_METHOD_POST
};
enum HTTP_VERSION {
    HTTP_1_0,
    HTTP_1_1
};

const char* httpMethodToString(HTTP_METHOD method) {
    switch (method) {
    case HTTP_METHOD_GET:
        return "GET";
    case HTTP_METHOD_POST:
        return "POST";
    default:
        return "UNKNOWN";
    }
}
const char* httpVersionToString(HTTP_VERSION version) {
    switch (version) {
    case HTTP_1_0:
        return "HTTP/1.0";
    case HTTP_1_1:
        return "HTTP/1.1";
    default:
        return "UNKNOWN";
    }
}

#include <map>

struct HttpRequest {
    HTTP_METHOD method;
    std::string request_target;
    HTTP_VERSION http_version;
    std::map<std::string, std::string> headers;
    std::vector<unsigned char> body;

    HttpRequest(HTTP_METHOD method, const std::string& request_target, HTTP_VERSION http_version)
        : method(method), request_target(request_target), http_version(http_version) 
    {}

    std::string toString() const {
        std::string str;
        str.append(httpMethodToString(method));
        str.append(" ");
        str.append(request_target);
        str.append(" ");
        str.append(httpVersionToString(http_version));
        str.append("\r\n");
        for (auto& kv : headers) {
            str.append(kv.first);
            str.append(": ");
            str.append(kv.second);
            str.append("\r\n");
        }
        str.append("\r\n");
        if (!body.empty()) {
            str.append(std::string(body.begin(), body.end()));
        }
        return str;
    }

    HttpRequest& addHeader(const std::string& key, const std::string& value) {
        headers[key] = value;
        return *this;
    }
    HttpRequest& setBody(const std::string& data) {
        body.clear();
        body.insert(body.end(), data.begin(), data.end());
        return *this;
    }
};

struct HttpResponse {
    HTTP_VERSION http_version;
    int status_code;
    std::string status_text;
    std::map<std::string, std::string> headers;
    std::vector<unsigned char> body;
};

#include "base64.hpp"

const char* strnstr(const char* str, const char* substring, int len) {
    int substrlen = strlen(substring);
    bool found = false;
    for (int i = 0; i < len - (substrlen - 1); ++i) {
        for (int j = 0; j < substrlen; ++j) {
            if (str[i + j] != substring[j]) {
                goto notmatched;
            }
        }
    matched:
        return str + i;
    notmatched:
        continue;
    }
    return 0;
}
/*
#define WS_FIN          0b0000000000000001
#define WS_RSV1         0b0000000000000010
#define WS_RSV2         0b0000000000000100
#define WS_RSV3         0b0000000000001000
#define WS_OPCODE       0b0000000011110000
#define WS_MASK         0b0000000100000000
#define WS_PAYLOAD_LEN  0b1111111000000000*/
#define WS_FIN          0b1000000000000000
#define WS_RSV1         0b0100000000000000
#define WS_RSV2         0b0010000000000000
#define WS_RSV3         0b0001000000000000
#define WS_OPCODE       0b0000111100000000
#define WS_MASK         0b0000000010000000
#define WS_PAYLOAD_LEN  0b0000000001111111
struct WebsocketsHeader {
    int fin : 1;
    int rsv1 : 1;
    int rsv2 : 1;
    int rsv3 : 1;
    int opcode : 4;
    int mask : 1;
    int payload_length : 7;
};

uint16_t flip_bytes(uint16_t ui16) {
    uint16_t r = 0;
    r |= (ui16 & 0x00FF) << 8;
    r |= (ui16 & 0xFF00) >> 8;
    return r;
}
uint32_t flip_bytes(uint32_t ui32) {
    uint32_t r = 0;
    r |= (ui32 & 0x0000'00FF) << 24;
    r |= (ui32 & 0x0000'FF00) << 8;
    r |= (ui32 & 0x00FF'0000) >> 8;
    r |= (ui32 & 0xFF00'0000) >> 24;
    return r;
}
uint64_t flip_bytes(uint64_t ui64) {
    uint64_t r = 0;
    r |= (ui64 & 0x0000'0000'0000'00FF) << 56;
    r |= (ui64 & 0x0000'0000'0000'FF00) << 40;
    r |= (ui64 & 0x0000'0000'00FF'0000) << 24;
    r |= (ui64 & 0x0000'0000'FF00'0000) << 8;
    r |= (ui64 & 0x0000'00FF'0000'0000) >> 8;
    r |= (ui64 & 0x0000'FF00'0000'0000) >> 24;
    r |= (ui64 & 0x00FF'0000'0000'0000) >> 40;
    r |= (ui64 & 0xFF00'0000'0000'0000) >> 56;
    return r;
}
std::string bits_to_str(uint16_t ui16) {
    return MKSTR(
        ((ui16 & 0b1000000000000000) ? "1" : "0")
        << ((ui16 & 0b0100000000000000) ? "1" : "0")
        << ((ui16 & 0b0010000000000000) ? "1" : "0")
        << ((ui16 & 0b0001000000000000) ? "1" : "0")
        << " "
        << ((ui16 & 0b0000100000000000) ? "1" : "0")
        << ((ui16 & 0b0000010000000000) ? "1" : "0")
        << ((ui16 & 0b0000001000000000) ? "1" : "0")
        << ((ui16 & 0b0000000100000000) ? "1" : "0")
        << " "
        << ((ui16 & 0b0000000010000000) ? "1" : "0")
        << ((ui16 & 0b0000000001000000) ? "1" : "0")
        << ((ui16 & 0b0000000000100000) ? "1" : "0")
        << ((ui16 & 0b0000000000010000) ? "1" : "0")
        << " "
        << ((ui16 & 0b0000000000001000) ? "1" : "0")
        << ((ui16 & 0b0000000000000100) ? "1" : "0")
        << ((ui16 & 0b0000000000000010) ? "1" : "0")
        << ((ui16 & 0b0000000000000001) ? "1" : "0")
    );
}

class TwitchEventSubSocket : public TLSSocket {
public:
    void onSocketConnected() override {
        unsigned char key[16];
        for (int i = 0; i < 16; ++i) {
            key[i] = rand() % 256;
        }
        std::string b64key;
        base64_encode(key, 16, b64key);

        HttpRequest request = HttpRequest(HTTP_METHOD_GET, "/ws", HTTP_1_1)
            .addHeader("Host", getAddr())
            .addHeader("Upgrade", "websocket")
            .addHeader("Connection", "keep-alive, Upgrade")
            .addHeader("Sec-WebSocket-Key", b64key)
            .addHeader("Sec-WebSocket-Version", "13");
        LOG_DBG(request.toString());
        sendRaw(request.toString());

        readWebsocketsHandshakeResponse();
    }

    bool readWebsocketsHandshakeResponse() {
        std::string response;
        int iResult = 0;
        do {
            iResult = hasData();
            if (iResult == SOCKET_ERROR) {
                return false;
            }
            if (iResult == 0) {
                continue;
            }

            char buf[512];
            iResult = SSL_peek(ssl, buf, 512);
            if (iResult == 0 || iResult == SOCKET_ERROR) {
                return false;
            }
            const char* separator = strnstr(buf, "\r\n\r\n", iResult);
            if (separator) {
                response.insert(response.end(), buf, (char*)separator + 4);
                SSL_read(ssl, buf, int((separator + 4) - buf));
                LOG_DBG(response);
                break;
            } else {
                response.insert(response.end(), buf, buf + iResult);
                SSL_read(ssl, buf, iResult);
            }
        } while(iResult != 0 && iResult != SOCKET_ERROR);
    }
    
    void runReceiveLoop() {
        int iResult = 0;
        do {
            iResult = hasData();
            if (iResult == SOCKET_ERROR) {
                return;
            }
            if (iResult < 0) {
                continue;
            }

            uint16_t ws_head = 0;
            iResult = SSL_peek(ssl, &ws_head, sizeof(ws_head));
            if (iResult < sizeof(ws_head)) {
                continue;
            }
            iResult = SSL_read(ssl, &ws_head, sizeof(ws_head));
            if (iResult == 0 || iResult == SOCKET_ERROR) {
                LOG_ERR("SSL_read error: " << iResult);
                return;
            }

            ws_head = flip_bytes(ws_head);
            LOG_DBG(bits_to_str(ws_head));
            uint64_t payload_len = ((unsigned char)(ws_head & WS_PAYLOAD_LEN));
            int opcode = (unsigned char)((ws_head & WS_OPCODE) >> 8);

            if (payload_len == 126) {
                while (1) {
                    uint16_t payload_len16 = 0;
                    iResult = SSL_peek(ssl, &payload_len16, sizeof(payload_len16));
                    if (iResult < sizeof(payload_len16)) {
                        continue;
                    }
                    iResult = SSL_read(ssl, &payload_len16, sizeof(payload_len16));
                    LOG_DBG(bits_to_str(flip_bytes(payload_len16)));
                    payload_len = flip_bytes(payload_len16);
                    break;
                }
            } else if(payload_len == 127) {
                while (1) {
                    uint64_t payload_len64 = 0;
                    iResult = SSL_peek(ssl, &payload_len64, sizeof(payload_len64));
                    if (iResult < sizeof(payload_len64)) {
                        continue;
                    }
                    iResult = SSL_read(ssl, &payload_len64, sizeof(payload_len64));
                    payload_len = flip_bytes(payload_len64);
                    break;
                }
            }
            LOG("FIN: " << ((ws_head & WS_FIN) >> 15));
            LOG("RSV1: " << ((ws_head & WS_RSV1) >> 14));
            LOG("RSV2: " << ((ws_head & WS_RSV2) >> 13));
            LOG("RSV3: " << ((ws_head & WS_RSV3) >> 12));
            LOG("OPCODE: " << opcode);
            LOG("MASK: " << ((ws_head & WS_MASK) >> 7));
            LOG("PAYLOAD_LEN: " << payload_len);

            /*if (opcode == 8) { // CLOSE
                // close properly
                break;
            } else */if (opcode == 9) { // PING
                LOG("WS: Sending pong");
                //ws_head |= 0x1 << 7;
                ws_head |= 0xA << 4;
                ws_head = flip_bytes(ws_head);
                LOG(bits_to_str(ws_head));
                iResult = SSL_write(ssl, &ws_head, sizeof(ws_head));
                if (iResult == 0 || iResult == SOCKET_ERROR) {
                    LOG_ERR("SSL_write error while sending pong: " << iResult);
                    return;
                }
                continue;
            } else {
                if (payload_len) {
                    std::string payload;
                    char buf[512];
                    uint64_t read = 0;
                    while (read < payload_len) {
                        iResult = SSL_read(ssl, buf, std::min(payload_len - read, (uint64_t)512));
                        if (iResult == 0 || iResult == SOCKET_ERROR) {
                            LOG_ERR("SSL_read error while reading payload: " << iResult);
                            return;
                        }
                        read += iResult;
                        payload.insert(payload.end(), buf, buf + iResult);
                    }
                    LOG("Payload: " << payload);
                }
            }

        } while (iResult != 0 && iResult != SOCKET_ERROR);
    }
};


class TwitchIrcSocket : public Socket {
    std::queue<std::string> msg_send_queue;
public:
    void onSocketConnected() override {
        // TODO: Actually can remove joinChat() and authenticate here
    }

    void joinChat(const char* auth_token, const char* nick, const char* channel) {
        sendRaw("CAP REQ :twitch.tv/membership twitch.tv/tags twitch.tv/commands\r\n");
        sendRaw(MKSTR("PASS oauth:" << auth_token << "\r\n").c_str());
        sendRaw(MKSTR("NICK " << nick << "\r\n").c_str());
        sendRaw(MKSTR("JOIN #" << channel << "\r\n").c_str());
    }

    bool sendMessage(const std::string& str) {
        int len = str.length();
        int at = 0;
        while (at < len) {
            msg_send_queue.push(str.substr(at, 500));
            at += 500;
        }

    }
    bool sendMessageImpl(const std::string& sender, const std::string& str) {
        std::string data = "PRIVMSG #" + sender + " :" + str + "\r\n";
        return sendRaw(data);
    }

    void sendMessageF(const char* format, ...) {
        static const int Size = 4096;
        char str[Size];
        ::memset(str, '\0', Size);
        va_list va;
        va_start(va, format);
        const unsigned int nSize = vsnprintf(str, Size - 1, format, va);
        assert(nSize < Size);
        va_end(va);
        sendMessage(str);
    }

    bool sendPong(const std::string& str) {
        std::string data = "PONG :" + str + "\r\n";
        printf("Sending PONG...\n");
        return sendRaw(data);
    }

    void runReceiveLoop() {
        std::string msg_cache; // Contains an irc message or a part of it, if not complete
        std::string received;  // Contains what's left after \r\n
        int iResult = 0;
        do {
            // Send outgoing messages if there are any
            while (!msg_send_queue.empty()) {
                sendMessageImpl("milk2b", msg_send_queue.front());
                msg_send_queue.pop();
            }

            // Receive
            int r = hasData();
            if (r == SOCKET_ERROR) {
                break;
            }
            if (r == 0) {
                continue;
            }

            char buf[512];
            iResult = recv(getSock(), buf, 512, 0);
            if (iResult == 0 || iResult == SOCKET_ERROR) {
                break;
            }

            received.insert(received.end(), buf, buf + iResult);

            while (1) {
                const char* p = strstr(received.c_str(), "\r\n");
                if (p == 0) {
                    msg_cache.insert(msg_cache.end(), received.begin(), received.end());
                    received.clear();
                    break;
                } else {
                    msg_cache.insert(msg_cache.end(), received.c_str(), p + 2);
                    received.erase(received.begin(), received.begin() + int(p + 2 - received.c_str()));
                    ircHandleMessage(*this, msg_cache);
                    msg_cache.clear();
                }
            }
        } while (iResult != 0 && iResult != SOCKET_ERROR);
    }
};


std::string makeSoundFileList() {
    std::string ret;
    HANDLE hFind = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATA ffd = { 0 };
    hFind = FindFirstFile("data\\*.ogg", &ffd);
    if (hFind != INVALID_HANDLE_VALUE) {
        while (FindNextFile(hFind, &ffd) != 0) {
            if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                continue;
            }
            std::string fname = std::string(ffd.cFileName, ffd.cFileName + strlen(ffd.cFileName) - 4);
            ret += fname + " ";
        }
        FindClose(hFind);
    }
    return ret;
}


#include <map>
#include <memory>
#include "audio/audio_clip.hpp"

std::map<std::string, std::shared_ptr<AudioClip>> clips;

static std::vector<std::string> banlist = {
    /*
    "sumeraga",
    "trusiki_mei"
    */
};

bool playSound(TwitchIrcSocket& sock, const IRC_MESSAGE& irc_msg, const std::string& sound_name, bool respond_to_missing_file = true) {
    for (int i = 0; i < banlist.size(); ++i) {
        if (irc_msg.user == banlist[i]) {
            return false;
        }
    }

    auto it = clips.find(sound_name);
    if (it == clips.end()) {
        FILE* f = fopen((std::string("data\\") + sound_name + ".ogg").c_str(), "rb");
        if (!f) {
            if (respond_to_missing_file) {
                sock.sendMessageF("%s, can't find sound clip '%s'", irc_msg.user.c_str(), sound_name.c_str());
            }
            return false;
        }
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::vector<unsigned char> buffer;
        buffer.resize(len);
        fread(buffer.data(), len, 1, f);
        fclose(f);

        std::shared_ptr<AudioClip> clip(new AudioClip);
        if (!clip->deserialize(buffer.data(), buffer.size())) {
            sock.sendMessageF("%s, failed to read sound clip '%s'", irc_msg.user.c_str(), sound_name.c_str());
            return false;
        }
        it = clips.insert(std::make_pair(sound_name, clip)).first;
    }
    audio().playOnce(it->second->getBuffer(), .75f, .0f);
    return true;
}

bool ircHandleBotCmd(TwitchIrcSocket& sock, const IRC_MESSAGE& irc_msg, irc_parse_state& ps) {
    if (!ircParseAccept(ps, '!')) {
        return false;
    }
    std::string cmd;
    ircParseEatAnyNotOf(ps, cmd, " \r\n");
    for (int i = 0; i < cmd.size(); ++i) {
        cmd[i] = tolower(cmd[i]);
    }
    printf("BOT COMMAND: %s\n", cmd.c_str());
    if (cmd == "snd") {
        while (ircParseAccept(ps, ' ')) {}
        std::string sound_name;
        ircParseEatAnyNotOf(ps, sound_name, " \r\n");
        if (sound_name.empty()) {
            sock.sendMessageF("%s, please provide a sound clip name", irc_msg.user.c_str());
            return true;
        }
        playSound(sock, irc_msg, sound_name);
        return true;
    } else if(cmd == "tts") {
        ircParseAccept(ps, ' ');
        std::string text;
        ircParseEatAnyNotOf(ps, text, "\r\n");
        if (text.empty()) {
            sock.sendMessageF("%s, please provide something to say", irc_msg.user.c_str());
            return true;
        }
        ttsSay(text.c_str());
    } else if (cmd == "sndlist") {
        sock.sendMessage(makeSoundFileList());
    } else {
        // Not an existing command, treat as a sound name
        playSound(sock, irc_msg, cmd, false);
    }
    return true;
}

bool ircHandleNightbotRoulette(TwitchIrcSocket& sock, const IRC_MESSAGE& irc_msg, const std::string& text) {
    if (irc_msg.user != "nightbot") {
        return false;
    }
    wchar_t buf[512];
    MultiByteToWideChar(CP_UTF8, MB_PRECOMPOSED, text.c_str(), -1, buf, 512);
    std::wstring wstr = buf;
    if (wstr.find(L"застрелился") != std::wstring::npos) {
        playSound(sock, irc_msg, "roulette\\shot");
        return true;
    } else if(wstr.find(L"*щелк*") != std::wstring::npos) {
        playSound(sock, irc_msg, "roulette\\click");
        return true;
    }
    return false;
}


#include "auth_token.h"

void ircHandleMessage(TwitchIrcSocket& sock, const std::string& msg) {
    //printf(msg.c_str());

    irc_parse_state ps = { msg.c_str(), msg.c_str() };

    IRC_MESSAGE irc_msg;

    try {
        ircParseTags(ps, &irc_msg);

        ircParsePrefixPart(ps, &irc_msg);

        if (!ircParseCommand(ps, &irc_msg)) {
            throw irc_parse_exception("Expected <command>");
        }
        ircParseParams(ps, &irc_msg);

        ircParseExpect(ps, "\r\n");
    } catch(const irc_parse_exception& ex) {
        printf("SOURCE: %s\n", msg.c_str());
        printf("error: %s\n", ex.what());
        ircPrintMsg(irc_msg);
        return;
    }
    ircPrintMsg(irc_msg);

    try {
        if (irc_msg.command == "PING") {
            sock.sendPong(irc_msg.params);
        } else if(irc_msg.command == "RECONNECT") {
            LOG("Trying to reconnect due to RECONNECT message");
            if (!sock.reconnect()) {
                LOG_ERR("Reconnect failed");
                return;
            }
            sock.joinChat(AUTH_TOKEN, "milk2b", "milk2b");
            sock.sendMessage("milkbot reconnected in response to a RECONNECT message");
        } else if (irc_msg.command == "PRIVMSG") {
            irc_parse_state ps = { irc_msg.params.c_str(), irc_msg.params.c_str() };
            std::string receiver;
            ircParsePrivmsgReceiver(ps, receiver);
            ircParseExpect(ps, ' ');
            ircParseExpect(ps, ':');
            std::string text;
            ircParseEatAnyNotOf(ps, text, "\r\n\0");
            irc_parse_state cmd_ps{ text.c_str(), text.c_str() };
            if (ircHandleBotCmd(sock, irc_msg, cmd_ps)) {
                return;
            }
            if (ircHandleNightbotRoulette(sock, irc_msg, text)) {
                return;
            }
        }
    } catch(const irc_parse_exception& ex) {
        printf("error: failed to parse PRIVMSG parameters");
        return;
    }
}


bool netInit() {
    LOG("WSAStartup()...\n");
    WSADATA wsaData;
    int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != 0) {
        printf("WSAStartup failed: %i\n", iResult);
        return false;
    }
    LOG("WSAStartup() successfull\n");
    return true;
}
bool netCleanup() {
    LOG("Cleaning up...\n");
    int iResult = WSACleanup();
    if (iResult != 0) {
        printf("WSACleanup failed: %i\n", iResult);
        return false;
    }
    LOG("Cleanup done\n");
    return true;
}


int main() {

    audio().init(48000, 16);

    ttsInit();
    //pVoice->Speak(L"Hello", SPF_ASYNC | SPF_IS_NOT_XML, 0);
    //pVoice->Speak(L"привет", SPF_ASYNC | SPF_IS_NOT_XML, 0);
    //pVoice->Speak(L"こんにちは", SPF_ASYNC | SPF_IS_NOT_XML, 0);

    SSL_library_init();
    netInit();
    /*
    {
        TwitchEventSubSocket eventSubSocket;
        eventSubSocket.conn("eventsub.wss.twitch.tv", "443");
        eventSubSocket.runReceiveLoop();
        return 0;
    }*/

    TwitchIrcSocket ircsock;
    if (!ircsock.conn("irc.chat.twitch.tv", "6667")) {
        netCleanup();
        return 1;
    }

    ircsock.joinChat(AUTH_TOKEN, "milk2b", "milk2b");

    ircsock.sendMessage(
        "Beep boop, milkbot is online! milk2bJelly"
    );

    ircsock.runReceiveLoop();
    
    ircsock.close();
    
    netCleanup();

    ttsCleanup();

    audio().cleanup();
    return 0;
}