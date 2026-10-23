#include "../src/platform/car_tuning_library.h"
#include <iostream>
#include <cstdlib>
using namespace forza::car_tuning;
using namespace forza::car_tuning::setups;
void Check(bool v,const char* message){if(!v){std::cerr<<message<<'\n';std::exit(1);}}
int main(){
 Identity id{1,404,123,456};id.parts.fill(17);Payload payload;payload.fill(std::bit_cast<uint32_t>(kStock));payload[10]=0xDEADBEEF;
 FieldSchema schema{};schema[size_t(Field::FrontPressure)]={15,55,30};schema[size_t(Field::RearPressure)]={15,55,30};
 schema[size_t(Field::RearAcceleration)]={0,1,.5,Field::RearDeceleration};
 EditSession edit;Check(edit.BeginSchema(id,payload,schema),"begin");edit.SetValue(Field::FrontPressure,31);edit.SetValue(Field::RearAcceleration,.7);
 auto s=Capture("Road Setup 1",edit);auto bytes=Encode(s);auto parsed=Decode(bytes);Check(parsed&&Encode(*parsed)==bytes,"roundtrip");
 std::string crlf;for(char c:bytes){if(c=='\n')crlf+='\r';crlf+=c;}Check(Decode(crlf).has_value(),"portable CRLF import");
 EditSession receiver;payload[10]=0xCAFEBABE;Check(receiver.BeginSchema(id,payload,schema)&&LoadDraft(*parsed,receiver),"load draft");
 Check(receiver.pending()[10]==0xCAFEBABE&&receiver.original()==payload&&receiver.dirty(),"receiver opaque/original preserved");
 Check(receiver.pending()[32]==receiver.pending()[33],"sport linked pair");Check(receiver.IsStock(Field::RearPressure),"stock sentinel");
 auto before=receiver.pending();s.model++;Check(!LoadDraft(s,receiver)&&receiver.pending()==before,"wrong model atomic");s.model--;s.parts[0]++;Check(!LoadDraft(s,receiver),"wrong parts");s.parts[0]--;s.schema++;Check(!LoadDraft(s,receiver),"wrong schema");s.schema--;
 s.fields[0]=0x7FC00000;Check(!Valid(s)&&!LoadDraft(s,receiver),"NaN");s.fields[0]=std::bit_cast<uint32_t>(1.1f);Check(!Valid(s),"out of range");
 for(auto name:{"../escape","a/b","a\\b","bad\nname",".hidden"," name","name ",""})Check(!ValidName(name),"unsafe name");
 Check(!Decode(bytes+"extra\n")&&!Decode(std::string(kMaxBytes+1,'x')),"trailing/size");auto unknown=bytes;unknown.replace(8,1,"2");Check(!Decode(unknown),"unknown version");
 auto duplicate=bytes;auto field=duplicate.find("\n1 bf800000");Check(field!=std::string::npos,"field fixture");duplicate.replace(field+1,1,"0");Check(!Decode(duplicate),"duplicate fields");
 auto unknown_field=bytes;unknown_field.replace(unknown_field.find("\n0 ")+1,1,"99");Check(!Decode(unknown_field),"unknown field ID");
 auto extra_field=*parsed;extra_field.fields[size_t(Field::Gear8)]=0;Check(!LoadDraft(extra_field,receiver)&&receiver.pending()==before,"unavailable field atomic");
 FieldSchema gears{};gears[size_t(Field::Gear1)]={1,5,3};gears[size_t(Field::Gear2)]={1,5,2};
 EditSession gear_receiver;Check(gear_receiver.BeginSchema(id,payload,gears),"gears begin");auto bad_gears=Capture("Bad Gears",gear_receiver);
 bad_gears.fields[size_t(Field::Gear1)]=std::bit_cast<uint32_t>(0.f);bad_gears.fields[size_t(Field::Gear2)]=std::bit_cast<uint32_t>(1.f);
 Check(!LoadDraft(bad_gears,gear_receiver)&&gear_receiver.pending()==payload,"crossing gear set atomic");
 auto path=std::filesystem::temp_directory_path()/("fh1-setup-test-"+std::to_string(std::random_device{}()));Library lib(path);
 auto saved=lib.Save(*parsed);Check(saved&&lib.List().size()==1&&lib.Get(*saved),"library save");Check(!lib.Get("../escape"),"id path");
 Check(lib.Rename(*saved,"Renamed Setup")&&lib.List().size()==1,"rename");auto renamed=lib.List().front();Check(renamed.setup.name=="Renamed Setup"&&lib.Remove(renamed.id)&&lib.List().empty(),"delete");
 std::filesystem::remove(path);std::cout<<"setup validation/library passed\n";
}
