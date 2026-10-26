#include "../src/platform/event_rewards.h"
#include <cassert>
#include <vector>
using namespace forza;
std::vector<uint8_t> Reply(uint64_t nonce=123, uint32_t rev=1, uint16_t epoch=7,
                           uint32_t peer=9, uint8_t auth=1, uint8_t required=1, uint8_t mask=16) {
  std::vector<uint8_t> out{2};
  auto put=[&](uint64_t n,unsigned count) { for(unsigned i=count;i;i--)out.push_back(uint8_t(n>>((i-1)*8))); };
  put(nonce,8);put(rev,4);put(epoch,2);put(peer,4);put(auth,1);put(required,1);put(mask,1);return out;
}
int main() {
  EventRewardCache cache;
  cache.Session(7,9,100);
  assert(cache.Permission(1197,100)==EventPermission::Unrestricted);
  const uint8_t policy[]{0,0,7,1};
  assert(cache.Receive(policy,4,100));
  assert(cache.Permission(1197,100)==EventPermission::Pending);
  assert(cache.Permission(1197,12101)==EventPermission::Unavailable);
  cache.Request(123,200);
  auto reply=Reply();
  for(unsigned i=0;i<reply.size();++i) assert(!cache.Receive(reply.data(),i,201));
  auto wrong=Reply(124); assert(!cache.Receive(wrong.data(),wrong.size(),201));
  wrong=Reply(123,1,8); assert(!cache.Receive(wrong.data(),wrong.size(),201));
  wrong=Reply(123,1,7,10); assert(!cache.Receive(wrong.data(),wrong.size(),201));
  assert(!cache.Receive(reply.data(),reply.size(),5201));
  assert(cache.Receive(reply.data(),reply.size(),201));
  assert(!cache.Receive(reply.data(),reply.size(),202)); // consumed nonce
  assert(cache.Permission(1197,202)==EventPermission::Allowed);
  assert(cache.Permission(320,202)==EventPermission::Denied);
  assert(cache.Permission(1009,50000)==EventPermission::Unrestricted);
  assert(cache.Permission(1197,12202)==EventPermission::Unavailable);
  cache.Request(125,300);reply=Reply(125,2,7,9,1,1,0);
  assert(cache.Receive(reply.data(),reply.size(),301));
  assert(cache.Permission(1197,302)==EventPermission::Denied); // revocation
  cache.Request(126,400);reply=Reply(126,1);assert(!cache.Receive(reply.data(),reply.size(),401));
  reply=Reply(126,3,7,9,0);assert(cache.Receive(reply.data(),reply.size(),401));
  assert(cache.Permission(1197,402)==EventPermission::EnrollmentRequired);
  cache.Session(8,9,500);
  assert(cache.Permission(1197,501)==EventPermission::Pending);
  cache.Request(127,600);reply=Reply(127,1,7);assert(!cache.Receive(reply.data(),reply.size(),601));
  reply=Reply(127,1,8,9,1,0);assert(cache.Receive(reply.data(),reply.size(),601));
  assert(cache.Permission(1197,602)==EventPermission::Unrestricted);
  cache.Request(128,700);reply=Reply(128,2,8,9,1,1,127);
  assert(cache.Receive(reply.data(),reply.size(),701));
  for(auto model:{320u,326u,330u,1082u,1197u,1233u,1488u})assert(cache.Permission(model,702)==EventPermission::Allowed);
}
