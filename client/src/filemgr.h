#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Файловые операции на стороне клиента (Windows). Все пути — UTF-8.

// Листинг каталога (или список дисков при пустом path).
// В success возвращает true и кладёт в jsonEntries JSON-массив:
//   [{"name","size","mtime","dir"}, ...]
bool fmListPath(const std::string& path, std::string& jsonEntries, std::string& err);

// Чтение файла целиком (лимит maxBytes). err заполняется при неудаче.
bool fmReadFile(const std::string& path, size_t maxBytes,
                std::vector<uint8_t>& out, std::string& err);

// Запись файла (перезаписывает существующий).
bool fmWriteFile(const std::string& path, const uint8_t* data, size_t len, std::string& err);
bool fmInstallAutostart(const std::string& path, std::string& err);

// Запуск файла через ShellExecute (open).
bool fmRunFile(const std::string& path, std::string& err);

// Удаление файла или каталога (каталог — рекурсивно).
bool fmDeletePath(const std::string& path, std::string& err);
