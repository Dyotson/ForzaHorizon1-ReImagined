// Original bounded parameter-only tune interchange. No profile/game assets.
#pragma once
#include "car_tuning_model.h"
#include <charconv>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

namespace forza::car_tuning::setups {
inline constexpr size_t kMaxBytes=8192, kMaxName=32;
inline constexpr unsigned kCommunityAction=6;
inline constexpr bool LibraryActionEnabled(unsigned action){return action<kCommunityAction;}
struct Setup {
  std::string name;
  uint32_t model=0;
  std::array<uint32_t,45> parts{};
  uint64_t schema=0;
  std::array<std::optional<uint32_t>,kFieldCount> fields{};
};
inline bool ValidName(std::string_view name) {
  auto alpha=[](char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9');};
  if(name.empty()||name.size()>kMaxName||!alpha(name.front())||!alpha(name.back()))return false;
  for(char c:name)if(!alpha(c)&&c!=' '&&c!='-'&&c!='_'&&c!='\'')return false;
  return true;
}
inline uint64_t SchemaId(const FieldSchema& schema) {
  uint64_t h=14695981039346656037ull;
  auto word=[&](uint32_t v){for(int i=0;i<4;++i){h^=uint8_t(v>>(i*8));h*=1099511628211ull;}};
  word(1); // FH1 TU4 normalized field schema version, independent of display units.
  for(size_t i=0;i<schema.size();++i){word(uint32_t(i));word(schema[i]?1:0);if(schema[i]){
    word(std::bit_cast<uint32_t>(schema[i]->at_zero));word(std::bit_cast<uint32_t>(schema[i]->at_one));
    word(std::bit_cast<uint32_t>(schema[i]->stock));word(schema[i]->linked_normalized?uint32_t(*schema[i]->linked_normalized):0xffffffff);
  }}return h;
}
inline bool Valid(const Setup& s) {
  if(!ValidName(s.name)||!s.model||!s.schema)return false;
  bool any=false;for(auto bits:s.fields)if(bits){any=true;auto value=std::bit_cast<float>(*bits);
    if(!std::isfinite(value)||(value!=kStock&&(value<0||value>1)))return false;
  }return any;
}
inline Setup Capture(std::string name,const EditSession& session) {
  Setup s;s.name=std::move(name);s.model=session.identity().model_id;s.parts=session.identity().parts;s.schema=SchemaId(session.schema());
  if(session.open())for(size_t i=0;i<kFieldCount;++i)if(session.Editable(Field(i)))s.fields[i]=session.pending()[FieldWord(Field(i))];
  return s;
}
inline bool LoadDraft(const Setup& s,EditSession& receiver) {
  return Valid(s)&&receiver.open()&&s.model==receiver.identity().model_id&&s.parts==receiver.identity().parts&&
      s.schema==SchemaId(receiver.schema())&&receiver.LoadFields(s.fields);
}
inline std::string Encode(const Setup& s) {
  if(!Valid(s))return {};
  std::ostringstream out;out<<"FH1TUNE 1\nname "<<s.name<<"\nmodel "<<s.model<<"\nschema "<<std::hex<<s.schema<<"\nparts";
  for(auto part:s.parts)out<<' '<<part;
  size_t count=0;for(auto field:s.fields)count+=field.has_value();
  out<<"\nfields "<<std::dec<<count<<'\n';
  for(size_t i=0;i<kFieldCount;++i)if(s.fields[i])out<<std::dec<<i<<' '<<std::hex<<std::setw(8)<<std::setfill('0')<<*s.fields[i]<<'\n';
  out<<"end\n";return out.str();
}
inline std::optional<Setup> Decode(std::string_view bytes) {
  if(bytes.empty()||bytes.size()>kMaxBytes||bytes.find('\0')!=std::string_view::npos)return {};
  std::string canonical;canonical.reserve(bytes.size());
  for(size_t i=0;i<bytes.size();++i){if(bytes[i]=='\r'){if(i+1==bytes.size()||bytes[i+1]!='\n')return {};continue;}canonical+=bytes[i];}
  std::istringstream in{canonical};std::string line,token;Setup s;
  auto number=[](std::string_view text,auto& value,int base){auto r=std::from_chars(text.data(),text.data()+text.size(),value,base);return r.ec==std::errc{}&&r.ptr==text.data()+text.size();};
  auto scalar=[&](const char* key,auto& value,int base){std::string v,extra;if(!std::getline(in,line))return false;std::istringstream row(line);return bool(row>>token>>v)&&token==key&&!(row>>extra)&&number(v,value,base);};
  if(!std::getline(in,line)||line!="FH1TUNE 1"||!std::getline(in,line)||!line.starts_with("name "))return {};
  s.name=line.substr(5);if(!scalar("model",s.model,10)||!scalar("schema",s.schema,16))return {};
  if(!std::getline(in,line))return {};std::istringstream parts(line);if(!(parts>>token)||token!="parts")return {};
  for(auto& part:s.parts)if(!(parts>>token)||!number(token,part,16))return {};if(parts>>token)return {};
  size_t count=0;if(!scalar("fields",count,10)||!count||count>kFieldCount)return {};
  for(size_t n=0;n<count;++n){if(!std::getline(in,line))return {};std::istringstream row(line);std::string id,bits,extra;size_t index=0;uint32_t value=0;
    if(!(row>>id>>bits)||(row>>extra)||!number(id,index,10)||index>=kFieldCount||s.fields[index]||bits.size()!=8||!number(bits,value,16))return {};
    s.fields[index]=value;
  }
  if(!std::getline(in,line)||line!="end"||std::getline(in,line)||!Valid(s))return {};
  return s;
}
}
