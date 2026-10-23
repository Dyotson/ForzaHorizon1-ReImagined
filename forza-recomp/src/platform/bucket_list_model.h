#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace forza::bucket_list {
enum class Phase { Idle, Countdown, Running, Complete, Failed, Cancelled };
enum class Kind { Speed, HoldSpeed, Distance };
struct Challenge { const char* id; const char* name; const char* goal; Kind kind; double target, limit; };
inline constexpr std::array<Challenge,3> kChallenges{{
  {"bucket_speed","COLORADO SPRINT","Reach 100 mph within 60 seconds",Kind::Speed,44.704,60},
  {"bucket_cruise","STEADY SIXTY","Hold 60 mph for 10 seconds within 60 seconds",Kind::HoldSpeed,10,60},
  {"bucket_distance","MILE MARKER","Drive one mile within 120 seconds",Kind::Distance,1609.344,120},
}};
struct Sample {
  uint32_t car=0;
  double x=0,y=0,z=0;
};
class Run {
 public:
  bool Start(size_t index) {
    if(index>=kChallenges.size()||Active())return false;
    index_=index;phase_=Phase::Countdown;countdown_=3;elapsed_=progress_=0;last_.reset();return true;
  }
  bool Active() const {return phase_==Phase::Countdown||phase_==Phase::Running;}
  void Cancel(){if(Active())phase_=Phase::Cancelled;}
  void Step(double dt,const Sample& sample) {
    if(!Active())return;
    if(!std::isfinite(dt)||dt<0||!sample.car||!std::isfinite(sample.x)||!std::isfinite(sample.y)||!std::isfinite(sample.z)){
      phase_=Phase::Cancelled;return;
    }
    if(!last_){last_=sample;return;}
    if(sample.car!=last_->car){phase_=Phase::Cancelled;return;}
    const double distance=std::hypot(sample.x-last_->x,sample.y-last_->y,sample.z-last_->z);
    last_=sample;
    if(!dt)return;
    if(distance>dt*150.+.5){phase_=Phase::Cancelled;return;}
    if(phase_==Phase::Countdown){countdown_=std::max(0.,countdown_-dt);if(!countdown_)phase_=Phase::Running;return;}
    const auto& c=kChallenges[index_];
    elapsed_+=dt;
    if(elapsed_>c.limit){phase_=Phase::Failed;return;}
    const double speed=distance/dt;
    switch(c.kind){
      case Kind::Speed:progress_=std::max(progress_,speed);break;
      case Kind::HoldSpeed:progress_=speed>=26.8224?progress_+dt:0;break;
      case Kind::Distance:progress_+=distance;break;
    }
    if(progress_+1e-6>=c.target){progress_=std::max(progress_,c.target);phase_=Phase::Complete;}
    else if(elapsed_>=c.limit)phase_=Phase::Failed;
  }
  Phase phase()const{return phase_;}
  size_t index()const{return index_;}
  double remaining()const{return phase_==Phase::Countdown?countdown_:std::max(0.,kChallenges[index_].limit-elapsed_);}
  double progress()const{return progress_;}
  double fraction()const{return std::clamp(progress_/kChallenges[index_].target,0.,1.);}
 private:
  Phase phase_=Phase::Idle;
  size_t index_=0;
  double countdown_=3,elapsed_=0,progress_=0;
  std::optional<Sample> last_;
};
}
