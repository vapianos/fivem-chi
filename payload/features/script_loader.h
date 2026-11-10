#pragma once
#include <string>
#include <vector>

namespace scripts {
void load_all();
bool run(const std::string& fileName);
std::vector<std::string> list();
size_t pending();
}