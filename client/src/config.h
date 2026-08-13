#pragma once

#include <string>

struct Config {
    std::string host = "127.0.0.1";
    int         port = 8080;
    std::string client_id = "pc1";
    std::string client_key = "secret123";

    bool tls = false;   // wss:// (TLS через Schannel), порт по умолчанию 443

    int  screen_fps   = 10;   // кадров/сек для экрана
    int  camera_fps   = 5;    // кадров/сек для камеры
    int  jpeg_quality = 75;   // 5..100

    bool capture_screen = true;
    bool capture_camera = true;

    std::string location;   // местоположение машины (свободный текст, для вкладки «Инфо»)
};

// Парсит простой INI-подобный конфиг (строки "ключ=значение", комментарии ;/#)
bool loadConfig(Config& cfg, const std::string& path);
