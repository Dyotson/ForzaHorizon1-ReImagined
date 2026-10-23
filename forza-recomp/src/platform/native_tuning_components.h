// Original tuning composition from independently owned FH1 paper/text widgets.
#pragma once
#include "fh1_menu_widgets.h"
#include "fh1_menu_components.h"
#include "fh1_current_car.h"
#include "ultrawide.h"
#if FORZA_TU4
#include <array>
#include <chrono>
#include <unordered_map>
namespace forza::native_tuning_components {
using namespace fh1_menu_widgets;
using Rect=fh1_menu_components::Rect;
struct Row { std::string label,saved,draft; float value=0,saved_value=0; bool selected=false; };
struct Metric { std::string label,saved,draft; };
struct View {
  fh1_menu_components::TabBar tabs; uint32_t model_id=0;
  unsigned category=0;
  std::vector<Row> rows; std::array<Metric,7> metrics{};
  std::string status,help,range; bool dirty=false;
  bool library=false,keyboard=false;unsigned key=0;std::string entry;
  std::vector<std::string> keys;
};
class Workspace {
  struct Piece {
    Donor donor; uint32_t paper=0,ink=0,track=0,fill=0,tick=0,thumb=0; bool panel=false, plain_labels=false;
    std::vector<uint32_t> text;
  };
  std::array<Piece,22> pieces;
  Donor footer;uint32_t named_model=0;
  uint32_t class_badge=0,class_controller=0;
  uint32_t manufacturer_logo=0,logo_material=0;
  bool logo_ready=false;
  bool class_ready=false;
  uint64_t bound_class=0;
  std::chrono::steady_clock::time_point class_refresh{};
  uint32_t screen=0,engine=0,stock_root=0; uint8_t suppression=0,visibility=0;
  bool ready=false,failed=false;
  uint32_t parent_engine=0,parent_root=0; uint8_t parent_suppression=0,parent_visibility=0;
  std::chrono::steady_clock::time_point started;
  std::unordered_map<uint32_t,std::string> cache;
  static uint32_t Named(Native& ui,uint32_t root,const char* name) {
    auto nodes=Descendants(ui.base,root,Hash(name)); return nodes.empty()?0:nodes.front();
  }
  static void Depth(Native& ui,const Donor& d,uint32_t node,float z) {
    if(ui.Scalar(d.presentation,node,"position.z",z)) return;
    auto instance=Read(ui.base,node+4); if(!instance || Read(ui.base,instance+4)!=node)return;
    Write(ui.base,instance+0x14,std::bit_cast<uint32_t>(z)); GuestPtr(ui.base,instance+0xfb)[0]|=0x20;
    ui.Dirty(d.presentation,node);
  }
  static void Normalize(Native& ui,const Donor& d,uint32_t node) {
    unsigned depth=0;
    for(auto p=Read(ui.base,node+12);p && p!=d.root && depth++<100;p=Read(ui.base,p+12)) {
      ui.Position(d.presentation,p,0,0);ui.Scale(d.presentation,p,1,1);
      ui.RotationZ(d.presentation,p,0);ui.Opacity(d.presentation,p,100);Depth(ui,d,p,0);
    }
  }
  static void Point(Native& ui,const Donor& d,uint32_t n,float x,float y) {
    ui.Position(d.presentation,n,x-640,360-y);
  }
  void Text(Native& ui,Piece& p,unsigned index,Rect r,float size,const std::string& value,uint32_t color=0xffffff,unsigned align=0) {
    if(index>=p.text.size())return; auto n=p.text[index];auto& d=p.donor;
    Normalize(ui,d,n);ui.Scale(d.presentation,n,1,1);ui.RotationZ(d.presentation,n,0);Depth(ui,d,n,-5);
    Point(ui,d,n,align==2?r.x+r.w:align==1?r.x+r.w/2:r.x,r.y+r.h/2);
    ui.TextStyle(d.access,d.presentation,n,size,r.w/1200,r.h/720,align);
    ui.Color(d.presentation,n,color,true);ui.Opacity(d.presentation,n,100);
    auto [it,inserted]=cache.try_emplace(n,value);
    if(inserted || it->second!=value){ui.Text(d.presentation,n,value);it->second=value;}
  }
  static void Mesh(Native& ui,Piece& p,uint32_t node,Rect r,uint32_t color,uint32_t material=0) {
    if(!node)return;auto& d=p.donor;Normalize(ui,d,node);Point(ui,d,node,r.x+r.w/2,r.y+r.h/2);
    ui.RotationZ(d.presentation,node,0);Depth(ui,d,node,node==p.paper?3.f:-2.f);
    ui.Scale(d.presentation,node,r.w/100,r.h/100);ui.Opacity(d.presentation,node,100);
    ui.Color(d.presentation,material?material:node,color);ui.Show(d.presentation,node,r.w>0);
  }
  static void Uncrop(Native& ui,const Donor& d,uint32_t material) {
    // Original FH1 papers crop narrow tape/timer strips through their image UV
    // records. Restore the full authored paper before setting component bounds.
    for(auto hash:{0xde7bb086u,0xebf2ce08u,0x2b443d36u,0x660a8466u,0xa0110228u,0xe0dc09b7u,0x54741897u})
      for(auto image:Descendants(ui.base,material,hash)) {
        auto wrapper=Read(ui.base,image+4);if(!wrapper||Read(ui.base,wrapper+4)!=image||GuestPtr(ui.base,wrapper+0xa)[0]!=7)continue;
        auto object=Read(ui.base,wrapper);if(!object)continue;
        bool u=ui.Scalar(d.presentation,image,"positionu",0),v=ui.Scalar(d.presentation,image,"positionv",0);
        if(!u)Write(ui.base,object+0xb4,std::bit_cast<uint32_t>(0.f));if(!v)Write(ui.base,object+0xb8,std::bit_cast<uint32_t>(0.f));
        if(!u||!v)ui.Call(0x82f1d8f0,{0,wrapper,image});
      }
  }
  bool Bind(Native& ui,Piece& p) {
    auto& d=p.donor;auto tape=Named(ui,d.root,"TitleTape");
    auto papers=Descendants(ui.base,p.panel?Named(ui,Named(ui,d.root,"CAR_LIST"),"HIGHLIGHT"):tape,p.panel?0xe0dc09b7:0x43a6b655);p.paper=papers.empty()?0:papers.front();
    p.ink=Named(ui,p.paper,p.panel?"Material866":"Material90");
    p.text.push_back(p.plain_labels?Named(ui,Named(ui,d.root,"CAR_STATS"),"TEXT_CAR_NAME"):Named(ui,tape,"TitleTapeText"));
    p.text.push_back(p.plain_labels?Named(ui,Named(ui,d.root,"CAR_STATS"),"TEXT_MANUFACTURER"):Named(ui,d.root,"TEXT_STATUS"));
    auto stats=Named(ui,d.root,"STATS");
    for(unsigned i=0;i<5;++i){auto row=Named(ui,stats,(std::string("CAR_STATS_PERFORMANCE_BARS")+(i?std::to_string(i+1):"")).c_str());
      p.text.push_back(Named(ui,row,"TEXT_NAME"));p.text.push_back(Named(ui,row,"TEXT_VALUE"));}
    p.fill=Named(ui,Named(ui,d.root,"HelpButton2"),"Pill");
    p.track=Named(ui,Named(ui,d.root,"HelpButton0"),"Pill");
    p.tick=Named(ui,Named(ui,d.root,"HelpButton1"),"Pill");
    auto thumbs=Descendants(ui.base,tape,0x747ffda5);p.thumb=thumbs.empty()?0:thumbs.front();
    if(!p.paper||!p.ink||!p.track||!p.fill||!p.tick||!p.thumb || std::any_of(p.text.begin(),p.text.end(),[](auto n){return !n;}))return false;
    // Header uses the same native localized car-name fields as the pause car card.
    if(&p==&pieces[20]){
      const auto stats_root=Named(ui,d.root,"CAR_STATS");
      p.text[9]=Named(ui,stats_root,"TEXT_MANUFACTURER");p.text[10]=Named(ui,stats_root,"TEXT_CAR_NAME");
      class_controller=0;
      class_badge=Named(ui,stats_root,"CLASS_SM");const auto pi=Named(ui,class_badge,"TEXT_PI");
      manufacturer_logo=Named(ui,stats_root,"LOGO");
      logo_material=Named(ui,manufacturer_logo,"Material_Car_Logo");
      const auto manager=d.Components(ui);const auto begin=manager?Read(ui.base,manager+0x28):0,end=manager?Read(ui.base,manager+0x2c):0;
      if(begin&&end>=begin&&end-begin<=8192&&(end-begin)%8==0)
        for(auto item=begin;item<end;item+=8){auto controller=Read(ui.base,item+4);
          if(controller&&Read(ui.base,controller)==0x8207513C&&Read(ui.base,controller+0xA8)==pi)class_controller=controller;}
      if(!class_badge||!pi||!class_controller||!manufacturer_logo||!logo_material)return false;
    }
    Uncrop(ui,d,p.ink);Uncrop(ui,d,p.track);Uncrop(ui,d,p.fill);Uncrop(ui,d,p.tick);Uncrop(ui,d,p.thumb);
    auto retain=p.text;retain.insert(retain.end(),{p.paper,p.track,p.fill,p.tick,p.thumb});
    if(&p==&pieces[20]){retain.push_back(class_badge);retain.push_back(manufacturer_logo);}
    return d.Isolate(ui,retain);
  }
  void Restore(Native& ui) {
    if(!stock_root)return;ui.Call(0x82FEB480,{engine,suppression});GuestPtr(ui.base,stock_root+0xfc)[0]=visibility;
    auto manager=ui.Virtual(engine,0x54,{engine});auto adapter=ui.Virtual(manager,0x3c,{manager});
    if(adapter)GuestPtr(ui.base,adapter+0x110)[0]=1;
    stock_root=0;
  }
public:
  bool Start(Native& ui,uint32_t parent,uint32_t pause_parent) {
    if(screen || !Read(ui.base,parent+0x110)){failed=true;return false;}screen=parent;engine=Read(ui.base,screen+8);started=std::chrono::steady_clock::now();
    parent_engine=Read(ui.base,pause_parent+8);
    for(unsigned i=0;i<pieces.size();++i){pieces[i].panel=true;pieces[i].plain_labels=i==19||i<9;if(!pieces[i].donor.Start(ui,"game:\\media\\ui\\scenes\\ui4\\NATIVE_MENU_TUNING_"+std::to_string(i)+".bgf"))failed=true;}
    return !failed;
  }
  bool Ready()const{return ready;}
  void SetVisible(Native& ui,bool visible){if(ready)for(auto& p:pieces)p.donor.SetVisible(ui,visible);}
  bool Failed()const{return failed;}
  void Release(Native& ui) {
    Restore(ui);
    if(parent_root){ui.Call(0x82FEB480,{parent_engine,parent_suppression});GuestPtr(ui.base,parent_root+0xfc)[0]=parent_visibility;parent_root=parent_engine=0;}
    for(auto& p:pieces){p.donor.Release(ui);p.text.clear();p.paper=p.ink=p.track=p.fill=p.tick=p.thumb=0;}
    screen=engine=0;ready=failed=false;cache.clear();footer=Donor{};named_model=0;
    class_badge=class_controller=0;class_ready=false;class_refresh={};
    bound_class=0;
    manufacturer_logo=logo_material=0;logo_ready=false;
  }
  void Tick(Native& ui,const View& view) {
    if(!screen||failed)return;
    if(!ready){
      bool loaded=true;for(unsigned i=0;i<pieces.size();++i){auto& p=pieces[i];
        // Native 82F1F2B0 sorts context*100 + owner layer. Separate the
        // backdrop below the original blur/footer layer, then panel and foreground.
        loaded=p.donor.Poll(ui,screen,[i,&ui](uint32_t,uint32_t controller){
          return i==20 && controller && Read(ui.base,controller)==0x8207513C;
        },i==21?-1:i==19?1:2)&&loaded;
        failed|=p.donor.status==Donor::Status::Failed;}
      if(failed||std::chrono::steady_clock::now()-started>std::chrono::seconds(12)){failed=true;Release(ui);failed=true;return;}
      if(!loaded)return;
      for(auto& p:pieces)if(!Bind(ui,p)){Release(ui);failed=true;return;}
      auto runtime=ui.Virtual(engine,8,{engine}),metadata=ui.Virtual(runtime,0x1c,{runtime});
      stock_root=metadata?Read(ui.base,metadata+4):0;if(!stock_root){Release(ui);failed=true;return;}
      suppression=GuestPtr(ui.base,engine+0x370)[0];visibility=GuestPtr(ui.base,stock_root+0xfc)[0];
      footer.presentation=engine;footer.access=ui.Virtual(engine,0x1c,{engine});footer.root=ui.Virtual(footer.access,0x48,{footer.access});footer.status=Donor::Status::Ready;
      auto button_bar=Named(ui,footer.root,"ButtonBar");
      if(!button_bar || !footer.Isolate(ui,{button_bar})){Release(ui);failed=true;return;}
      ui.Call(0x82FEB480,{engine,0});GuestPtr(ui.base,stock_root+0xfc)[0]=1;
      auto parent_runtime=ui.Virtual(parent_engine,8,{parent_engine}),parent_metadata=ui.Virtual(parent_runtime,0x1c,{parent_runtime});
      parent_root=parent_metadata?Read(ui.base,parent_metadata+4):0;
      if(parent_root){parent_suppression=GuestPtr(ui.base,parent_engine+0x370)[0];parent_visibility=GuestPtr(ui.base,parent_root+0xfc)[0];
        ui.Call(0x82FEB480,{parent_engine,1});GuestPtr(ui.base,parent_root+0xfc)[0]=0;}
      auto manager=Read(ui.base,0x833CAAC0);if(manager)GuestPtr(ui.base,manager+0x5d)[0]=1;
      for(auto& p:pieces)p.donor.SetVisible(ui,true);ready=true;
    }
    for(auto& p:pieces){ui.Call(0x82FEE208,{p.donor.presentation});p.donor.ApplyVisibility(ui);}
    // Each donor contributes isolated artwork and twelve independent text anchors.
    // Hide every unused anchor explicitly, avoiding inherited animated row grids.
    for(auto& p:pieces){for(auto n:p.text)ui.Show(p.donor.presentation,n,false);
      for(auto n:{p.paper,p.track,p.fill,p.tick,p.thumb})ui.Show(p.donor.presentation,n,false);}
    const auto label=[&](Piece& p,unsigned i,Rect r,float size,const std::string& s,uint32_t c=0xffffff,unsigned a=0){
      ui.Show(p.donor.presentation,p.text[i],true);Text(ui,p,i,r,size,s,c,a);};
    auto& backing=pieces[21];
    // Only the backdrop bleeds beyond the canvas: its donor texture has a
    // transparent perimeter. Foreground panels retain their safe-area bounds.
    const auto extent=ultrawide::OutputSize();
    Mesh(ui,backing,backing.paper,fh1_menu_components::BackdropBounds(extent.width,extent.height),0x161117,backing.ink);
    ui.Opacity(backing.donor.presentation,backing.paper,91);
    auto& header=pieces[20];
    fh1_menu_components::Header heading{view.library?"TUNE LIBRARY":"CAR TUNING",view.dirty?"UNSAVED DRAFT":"SAVED SETUP",view.dirty};
    const auto native_name=[&](unsigned index,Rect r,float size){auto n=header.text[index];auto& d=header.donor;
      ui.Show(d.presentation,n,true);Normalize(ui,d,n);ui.Scale(d.presentation,n,1,1);Depth(ui,d,n,-5);
      Point(ui,d,n,r.x,r.y+r.h/2);ui.TextStyle(d.access,d.presentation,n,size,r.w/1200,r.h/720,0);ui.Color(d.presentation,n,0xc9c2c9,true);ui.Opacity(d.presentation,n,100);};
    heading.Paint([&](Rect r,uint32_t c){Mesh(ui,header,header.paper,r,c,header.ink);},
      [&](unsigned i,Rect r,float z,const std::string& text,uint32_t color,unsigned align){label(header,i,r,z,text,color,align);},
      [&](Rect make,Rect name,float size){native_name(9,make,size);native_name(10,name,size);
        if(named_model!=view.model_id){fh1_current_car::BindNames(ui,header.donor.presentation,header.text[10],header.text[9],view.model_id,logo_material,&logo_ready);named_model=view.model_id;}});
    ui.Show(header.donor.presentation,manufacturer_logo,logo_ready);
    if(logo_ready){Normalize(ui,header.donor,manufacturer_logo);Depth(ui,header.donor,manufacturer_logo,-5);
      ui.Opacity(header.donor.presentation,manufacturer_logo,100);ui.Color(header.donor.presentation,logo_material,0xffffff);
      Point(ui,header.donor,manufacturer_logo,88,89);ui.Scale(header.donor.presentation,manufacturer_logo,.44f,.26f);
      ui.RotationZ(header.donor.presentation,manufacturer_logo,0);}
    const auto class_bounds=heading.CurrentClassBounds();
    if(std::chrono::steady_clock::now()>=class_refresh){
      class_ready=fh1_current_car::BindCurrentClass(ui,class_controller,view.model_id,&bound_class);
      class_refresh=std::chrono::steady_clock::now()+std::chrono::seconds(1);
    }
    label(header,11,{class_bounds.x,class_bounds.y,126,class_bounds.h},12,class_ready?"CURRENT CLASS":"CLASS UNAVAILABLE",0xc9c2c9);
    ui.Show(header.donor.presentation,class_badge,class_ready);
    if(class_ready){Normalize(ui,header.donor,class_badge);Depth(ui,header.donor,class_badge,-5);
      Point(ui,header.donor,class_badge,class_bounds.x+176,class_bounds.y+class_bounds.h/2);
      ui.Scale(header.donor.presentation,class_badge,.8f,.8f);ui.RotationZ(header.donor.presentation,class_badge,0);}
    view.tabs.Paint({44,120,1192,48},9,
      [&](size_t slot,Rect r,uint32_t color){auto& p=pieces[slot];Mesh(ui,p,p.paper,r,color,p.ink);ui.RotationZ(p.donor.presentation,p.paper,fh1_menu_components::TabBar::kSlantDegrees);},
      [&](size_t slot,unsigned index,Rect r,float size,const std::string& text,uint32_t color){label(pieces[slot],index,r,size,text,color,1);});
    const auto category=view.tabs.tabs().empty()?std::string{}:view.tabs.tabs()[view.tabs.selected_index()].label;
    Mesh(ui,header,header.track,{48,191,726,36},0x302b30,Named(ui,header.track,"Material100"));
    label(header,2,{58,194,340,30},19,category);
    label(header,3,{442,194,140,30},14,view.library?"CAR":"SAVED",0xc9c2c9,2);
    label(header,4,{594,194,170,30},14,view.library?"STATUS":"DRAFT",0xff83bf,2);
    unsigned count=std::min<unsigned>(9,unsigned(view.rows.size()));
    for(unsigned i=0;i<count;++i){auto& p=pieces[9+i];const auto& row=view.rows[i];float y=235+i*34.f;
      Mesh(ui,p,p.paper,{48,y,726,31},row.selected?0x5b2342:0x302b30,p.ink);
      label(p,0,{62,y,view.library?390.f:225.f,28},16,row.label);
      label(p,1,{462,y,119,28},15,row.saved,0xc9c2c9,2);label(p,2,{597,y,163,28},16,row.draft,0xffffff,2);
      if(!view.library){Mesh(ui,p,p.track,{302,y+15,140,4},0x888088,Named(ui,p.track,"Material100"));
      Mesh(ui,p,p.fill,{302,y+15,std::max(1.f,140*row.value),4},0xff007a,Named(ui,p.fill,"Material100"));
      Mesh(ui,p,p.tick,{301+140*row.saved_value,y+10,2,14},0xffffff,Named(ui,p.tick,"Material100"));
      Mesh(ui,p,p.thumb,{300+140*row.value,y+9,4,16},0xff83bf,Named(ui,p.thumb,"Material90"));}
    }
    if(!count&&!view.keyboard){label(header,5,{66,245,685,38},22,view.library?"NO SAVED SETUPS":"NO ADJUSTABLE SETTINGS",0xffffff);
      label(header,6,{66,289,685,40},16,view.library?"Save your draft or add a shared tune to the inbox.":"Installed parts lock this category.",0xc9c2c9);}
    if(view.keyboard){
      label(header,5,{66,237,685,43},24,view.entry+"_",0xff83bf);
      for(unsigned i=0;i<view.keys.size();++i){unsigned row=i/9,col=i%9;auto& p=pieces[9+row];
        float x=58+col*79.f,y=300+row*40.f;
        const auto key=view.keys[i]==" "?std::string("SPACE"):view.keys[i];
        label(p,col,{x,y,74,34},key.size()>1?12.f:20.f,key,i==view.key?0xff007a:0xffffff,1);
      }
    }
    auto& metrics=pieces[18];auto& metrics2=pieces[19];
    Mesh(ui,metrics2,metrics2.paper,{802,195,434,361},0x302b30,metrics2.ink);
    label(metrics,0,{822,203,390,31},21,"PERFORMANCE SIMULATION");
    label(metrics,1,{822,236,390,24},13,view.status,0xc9c2c9);
    label(pieces[17],8,{949,270,119,25},13,"SAVED",0xc9c2c9,2);label(pieces[17],9,{1080,270,134,25},13,"DRAFT",0xff83bf,2);
    for(unsigned i=0;i<7;++i){auto& p=i<3?metrics:metrics2;unsigned index=i<3?2+i*3:(i-3)*3;
      float y=305+i*33.f;const auto& m=view.metrics[i];
      label(p,index,{821,y,136,29},14,m.label);label(p,index+1,{961,y,107,29},15,m.saved,0xc9c2c9,2);
      label(p,index+2,{1080,y,134,29},15,m.draft,0xffffff,2);
    }
    label(header,7,{63,570,1150,29},17,view.help);
    label(header,8,{63,604,1150,26},13,view.range+(view.library?"":" Changes are saved only with Apply."),0xc9c2c9);
    // Actual native footer controller owns its button atlas and label spacing.
    footer.ApplyVisibility(ui);
    const auto footer_runtime=ui.Virtual(engine,8,{engine});ui.Call(0x82FE39A0,{footer_runtime});
    for(auto& p:pieces){auto& d=p.donor;ui.Call(0x82FE39A0,{d.runtime});auto manager=Read(ui.base,d.presentation+0x14);
      if(manager){auto clock=ui.Virtual(manager+8,0x10,{manager+8});ui.Call(0x82FECE38,{d.presentation,clock});}}

  }
};
}
#endif
