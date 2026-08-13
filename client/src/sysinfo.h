#pragma once

#include <string>

struct Config;

// Собирает информацию о ПК, мониторах и местоположении в JSON-строку вида:
// {"type":"info","pc":{...},"monitors":[{...}],"location":"..."}
std::string collectSystemInfo(const Config& cfg);
