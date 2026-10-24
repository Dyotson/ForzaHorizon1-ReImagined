// Authored ZIP-directory fixtures; no game scenes or payloads.
#include "../src/platform/native_tuning_assets.h"
#include <cstdlib>
#include <iostream>
#include <random>

namespace {
void Check(bool ok, const char* message) {
  if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
void Word(std::vector<uint8_t>& b, uint32_t n, unsigned bytes) {
  for (unsigned i=0; i<bytes; ++i) b.push_back(uint8_t(n >> (i*8)));
}
// Only metadata is inspected by preflight. These describe 66 independently
// authored, nonempty members, without purporting to be native scene data.
std::vector<uint8_t> Directory(unsigned count=66, unsigned codec=0, unsigned flags=0) {
  std::vector<uint8_t> b;
  for (unsigned i=0;i<count;++i) {
    const char* ext[]={".bgf", ".bsg", ".fbf"};
    const auto name="ui/scenes/ui4/native_menu_tuning_"+std::to_string(i/3)+ext[i%3];
    Word(b,0x02014b50,4); Word(b,20,2); Word(b,20,2);
    Word(b,flags,2); Word(b,codec,2); Word(b,0,4); Word(b,0,4);
    Word(b,1,4); Word(b,1,4); Word(b,uint32_t(name.size()),2);
    Word(b,0,2);Word(b,0,2);Word(b,0,2);Word(b,0,2);Word(b,0,4);Word(b,0,4);
    b.insert(b.end(),name.begin(),name.end());
  }
  const auto size=uint32_t(b.size());
  Word(b,0x06054b50,4);Word(b,0,2);Word(b,0,2);
  Word(b,count,2);Word(b,count,2);Word(b,size,4);Word(b,0,4);Word(b,0,2);
  return b;
}
}
int main() {
  const auto root=std::filesystem::temp_directory_path()/
      ("fh1-tuning-preflight-"+std::to_string(std::random_device{}()));
  std::filesystem::create_directory(root);
  const auto file=root/"media.zip";
  const auto accepts=[&](const std::vector<uint8_t>& bytes) {
    { std::ofstream out(file,std::ios::binary|std::ios::trunc);
      out.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size())); }
    return forza::native_tuning_assets::DonorsAvailable(file);
  };
  Check(!forza::native_tuning_assets::DonorsAvailable(file),"missing archive");
  Check(accepts(Directory()),"complete directory");
  Check(accepts(Directory(66,21)),"native codec metadata");
  Check(!accepts(Directory(65)),"missing donor member");
  Check(!accepts(Directory(66,99)),"unsupported codec");
  Check(!accepts(Directory(66,0,1)),"encrypted member");
  auto truncated=Directory();truncated.pop_back();
  Check(!accepts(truncated),"truncated directory");
  auto empty_member=Directory();empty_member[24]=0;
  Check(!accepts(empty_member),"empty donor member");
  std::filesystem::remove(file);std::filesystem::remove(root);
  std::cout<<"tuning donor preflight passed\n";
}
