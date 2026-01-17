#pragma once
#include <string>

constexpr int32_t DEFAULT_UUID_SIZE = 16;

std::string generate_uuid_bytes(int size = DEFAULT_UUID_SIZE);

void write_uuid_bytes(std::string &out_buffer, int size = DEFAULT_UUID_SIZE);
