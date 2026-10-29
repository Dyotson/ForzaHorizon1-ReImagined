#pragma once
#include <cstdint>
#include <string>
#include <algorithm>
namespace forza::record_admission {
inline std::string Certificate(uint32_t sealed,uint32_t captured_revision,uint32_t current_revision,const std::string& captured,const std::string& record_source){
 if(!sealed||!captured_revision||captured_revision!=current_revision||captured.empty()||captured!=record_source||std::count(captured.begin(),captured.end(),',')!=7)return {};
 return std::to_string(sealed)+","+std::to_string(captured_revision)+","+captured;
}
}
