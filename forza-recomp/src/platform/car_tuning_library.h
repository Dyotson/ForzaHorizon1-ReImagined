// Local parameter library. Display names never become filesystem paths.
#pragma once
#include "car_tuning_setup.h"
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>
#include <algorithm>
namespace forza::car_tuning::setups {
struct Entry { std::string id; Setup setup; };
class Library {
  std::filesystem::path root_;
  static bool Id(std::string_view id){return id.size()==32&&std::all_of(id.begin(),id.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});}
  bool Directory() const {
    std::error_code ec;
    if(std::filesystem::is_symlink(std::filesystem::symlink_status(root_,ec)))return false;
    ec.clear();std::filesystem::create_directories(root_,ec);return !ec&&std::filesystem::is_directory(root_,ec)&&!ec;
  }
  std::optional<Setup> Read(const std::filesystem::path& path) const {
    std::error_code ec;auto status=std::filesystem::symlink_status(path,ec);
    if(ec||!std::filesystem::is_regular_file(status)||std::filesystem::is_symlink(status))return {};
    auto size=std::filesystem::file_size(path,ec);if(ec||!size||size>kMaxBytes)return {};
    std::ifstream in(path,std::ios::binary);std::string bytes(size,'\0');if(!in.read(bytes.data(),size)||in.peek()!=EOF)return {};
    return Decode(bytes);
  }
public:
  explicit Library(std::filesystem::path root):root_(std::move(root)){}
  std::vector<Entry> List() const {
    std::vector<Entry> entries;if(!Directory())return entries;std::error_code ec;size_t scanned=0;
    for(std::filesystem::directory_iterator it(root_,ec),end;!ec&&it!=end&&scanned++<1024;it.increment(ec)){
      if(entries.size()>=256)break;
      auto path=it->path();auto id=path.stem().string();if(path.extension()!=".fh1tune"||!Id(id))continue;
      if(auto setup=Read(path))entries.push_back({std::move(id),std::move(*setup)});
    }
    std::sort(entries.begin(),entries.end(),[](const Entry&a,const Entry&b){return a.setup.name==b.setup.name?a.id<b.id:a.setup.name<b.setup.name;});return entries;
  }
  std::optional<Setup> Get(std::string_view id)const{return Id(id)&&Directory()?Read(root_/(std::string(id)+".fh1tune")):std::nullopt;}
  std::optional<std::string> Save(const Setup& setup) const {
    auto bytes=Encode(setup);if(bytes.empty()||!Directory()||List().size()>=256)return {};
    std::random_device random;std::ostringstream name;name<<std::hex<<std::setfill('0');for(int i=0;i<4;++i)name<<std::setw(8)<<uint32_t(random());
    auto id=name.str();auto path=root_/(id+".fh1tune"),temporary=root_/(id+".tmp");std::error_code ec;
    if(std::filesystem::exists(path,ec)||ec||std::filesystem::exists(temporary,ec)||ec)return {};
    {std::ofstream out(temporary,std::ios::binary|std::ios::trunc);if(!out.write(bytes.data(),bytes.size())||!out.flush()){out.close();std::filesystem::remove(temporary,ec);return {};}}
    std::filesystem::rename(temporary,path,ec);if(ec){std::filesystem::remove(temporary,ec);return {};}return id;
  }
  bool Remove(std::string_view id)const {
    if(!Get(id))return false;std::error_code ec;return std::filesystem::remove(root_/(std::string(id)+".fh1tune"),ec)&&!ec;
  }
  bool Rename(std::string_view id,std::string name)const {
    auto setup=Get(id);if(!setup||!ValidName(name))return false;setup->name=std::move(name);
    // Publish complete replacement metadata before removing the old document.
    auto replacement=Save(*setup);if(!replacement)return false;
    if(Remove(id))return true;Remove(*replacement);return false;
  }
};
}
