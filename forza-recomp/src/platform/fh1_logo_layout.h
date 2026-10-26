// Bounds of the nontransparent artwork in FH1's padded native LG logos.
// This describes layout only; textures remain in the user's game archives.
#pragma once
#include <algorithm>
#include <optional>
#include <string_view>
namespace forza::fh1_logo_layout {
struct Fit {float sx=.72f,sy=.44f,dx=0,dy=0;};
inline std::optional<Fit> Large(std::string_view path) {
  const auto slash=path.find_last_of("/\\");if(slash!=path.npos)path.remove_prefix(slash+1);
  if(const auto dot=path.find_last_of('.');dot!=path.npos)path=path.substr(0,dot);
  struct Bounds {std::string_view name;unsigned w,h,l,t,r,b;};
  static constexpr Bounds bounds[]{
    {"L_MAN_ABA_Logo",882,315,275,0,603,315},
    {"L_MAN_AMC_Logo",882,315,262,9,619,315},
    {"L_MAN_AMG_Logo",882,315,307,1,577,315},
    {"L_MAN_ASKLON_Logo",882,315,294,24,588,315},
    {"L_MAN_AUS_Logo",882,315,41,98,839,235},
    {"L_MAN_Abarth_Logo",882,315,305,25,572,315},
    {"L_MAN_Acura_Logo",882,315,223,43,659,272},
    {"L_MAN_AlfaRomeo_Logo",882,315,300,26,581,307},
    {"L_MAN_Ascari_Logo",882,315,296,5,584,315},
    {"L_MAN_AstonMartin_Logo",882,315,150,27,733,308},
    {"L_MAN_Audi_Logo",882,315,200,25,681,307},
    {"L_MAN_BMW_Logo",882,315,307,33,575,301},
    {"L_MAN_Bentley_Logo",882,315,77,14,826,315},
    {"L_MAN_Bertone_Logo",882,315,232,0,647,315},
    {"L_MAN_Bugatti_Logo",882,315,176,25,706,308},
    {"L_MAN_Buick_Logo",882,315,275,1,607,313},
    {"L_MAN_Cadillac_Logo",882,315,287,27,594,308},
    {"L_MAN_Chevrolet_Logo",882,315,136,4,732,311},
    {"L_MAN_Chrysler_Logo",882,315,91,60,791,258},
    {"L_MAN_Chrysler_Logo_It",882,315,0,39,874,311},
    {"L_MAN_Citroen_Logo",882,315,247,26,632,307},
    {"L_MAN_DeLorean_Logo",882,315,86,59,796,275},
    {"L_MAN_DeTomaso_Logo",882,315,307,0,574,315},
    {"L_MAN_Devon_Logo",882,315,265,0,616,315},
    {"L_MAN_Dodge_Logo",882,315,79,75,802,254},
    {"L_MAN_EAG_Logo",882,315,145,26,745,306},
    {"L_MAN_Ferrari_Logo",882,315,348,8,536,315},
    {"L_MAN_Fiat_Logo",882,315,265,0,616,315},
    {"L_MAN_Ford_Logo",882,315,112,17,770,315},
    {"L_MAN_GMC_Logo",882,315,89,58,790,285},
    {"L_MAN_Gumpert_Logo",882,315,328,26,554,306},
    {"L_MAN_HEN_Logo",882,315,114,118,766,215},
    {"L_MAN_HSV_Logo",882,315,171,50,696,295},
    {"L_MAN_HUM_Logo",882,315,150,135,758,197},
    {"L_MAN_Holden_Logo",882,315,289,25,592,307},
    {"L_MAN_Honda_Logo",882,315,212,26,668,309},
    {"L_MAN_Hummer_LOGO",882,315,150,135,758,197},
    {"L_MAN_Hyundai_Logo",882,315,223,25,652,303},
    {"L_MAN_Infiniti_Logo",882,315,193,37,684,308},
    {"L_MAN_Jaguar_Logo",882,315,206,54,672,280},
    {"L_MAN_Jeep_Logo",882,315,191,57,691,257},
    {"L_MAN_Joss_Logo",882,315,167,110,741,195},
    {"L_MAN_Kia_Logo",882,315,146,0,737,315},
    {"L_MAN_Koenigsegg_Logo",882,315,294,0,588,313},
    {"L_MAN_Lamborghini_Logo",882,315,318,30,562,299},
    {"L_MAN_Lancia_Logo",882,315,300,25,580,306},
    {"L_MAN_LandRover_Logo",882,315,189,29,697,306},
    {"L_MAN_Lexus_Logo",882,315,242,25,628,307},
    {"L_MAN_Lotus_Logo",882,315,294,20,587,314},
    {"L_MAN_Maserati_Logo",882,315,336,16,547,315},
    {"L_MAN_Mazda_Logo",882,315,307,24,578,309},
    {"L_MAN_McLaren_Logo",882,315,80,77,806,268},
    {"L_MAN_Mercedes_Logo",882,315,114,52,733,285},
    {"L_MAN_Mercury_Logo",882,315,271,0,611,315},
    {"L_MAN_Mitsubishi_Logo",882,315,302,26,565,309},
    {"L_MAN_Morgan_Logo",882,315,79,12,797,315},
    {"L_MAN_Mosler_Logo",882,315,253,27,628,307},
    {"L_MAN_Nissan_Logo",882,315,272,25,602,309},
    {"L_MAN_Noble_Logo",882,315,164,6,717,308},
    {"L_MAN_Oldsmobile_LOGO",882,315,126,67,752,239},
    {"L_MAN_Opel_Logo",882,315,236,0,645,315},
    {"L_MAN_Pagani_Logo",882,315,181,22,696,312},
    {"L_MAN_Panoz_Logo",882,315,303,0,577,315},
    {"L_MAN_Peugeot_Logo",882,315,247,28,633,307},
    {"L_MAN_Plymouth_Logo",882,315,133,55,748,304},
    {"L_MAN_Pontiac_Logo",882,315,94,20,786,312},
    {"L_MAN_Porsche_Logo",512,256,30,6,481,250},
    {"L_MAN_RUF_Logo",882,315,155,36,735,298},
    {"L_MAN_Radical_Logo",882,315,308,26,572,306},
    {"L_MAN_Rossion_Logo",882,315,293,7,589,315},
    {"L_MAN_SSC_Logo",882,315,53,25,823,306},
    {"L_MAN_Saab_Logo",882,315,266,0,617,315},
    {"L_MAN_Saleen_Logo",882,315,273,45,607,291},
    {"L_MAN_Saturn_Logo",882,315,278,7,604,315},
    {"L_MAN_Scion_Logo",882,315,241,26,639,308},
    {"L_MAN_Seat_Logo",882,315,224,0,654,315},
    {"L_MAN_Shelby_Logo",882,315,298,24,584,309},
    {"L_MAN_Spada_Logo",882,315,262,0,615,315},
    {"L_MAN_Spyker_Logo",882,315,70,77,809,265},
    {"L_MAN_Subaru_Logo",882,315,239,77,644,281},
    {"L_MAN_Suzuki_Logo",882,315,198,0,674,315},
    {"L_MAN_TES_Logo",882,315,145,73,737,241},
    {"L_MAN_TVR_Logo",882,315,155,65,709,271},
    {"L_MAN_Toyota_Logo",882,315,224,25,656,307},
    {"L_MAN_Ultima_Logo",882,315,109,80,771,256},
    {"L_MAN_VIP_Logo",882,315,285,24,600,315},
    {"L_MAN_Vauxhall_Logo",882,315,264,0,616,315},
    {"L_MAN_Volkswagen_Logo",882,315,288,14,587,313},
    {"L_MAN_Volvo_Logo",882,315,259,0,618,315},
    {"L_MAN_Wiesmann_Logo",882,315,92,28,790,315},
  };
  for(const auto& b:bounds){
    if(path.size()!=b.name.size())continue;
    bool match=true;for(unsigned i=0;i<path.size();++i){const auto lower=[](char c){return c>='A'&&c<='Z'?char(c+32):c;};if(lower(path[i])!=lower(b.name[i])){match=false;break;}}
    if(!match)continue;
    const float fit=std::min(64.f/(b.r-b.l),44.f/(b.b-b.t));
    return Fit{b.w*fit/100,b.h*fit/100,(b.w-float(b.l+b.r))*fit/2,(b.h-float(b.t+b.b))*fit/2};
  }
  return {};
}
}
