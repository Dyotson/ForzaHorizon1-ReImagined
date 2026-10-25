#include "../src/platform/native_renderer/scene_transfer.h"
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace forza::native_renderer;
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(false)
int main(){try{
 SceneSnapshotKey k{0x100000,1280*736*4,1280,720,1280,2,2,2};CHECK(Valid(k));
 auto bad=k;bad.address=0x1ffff000;CHECK(!Valid(bad));bad=k;bad.pitch=1312;CHECK(!Valid(bad));
 bad=k;bad.scale_y=1;CHECK(!Valid(bad));bad=k;bad.endian=4;CHECK(!Valid(bad));
 SceneSnapshotTracker t;CHECK(!t.Reserve(0,0));auto a=t.Reserve(1,0);CHECK(a);CHECK(t.Publish(*a,k));
 CHECK(!t.Find(k,0));CHECK(t.Find(k,2)==a);bad=k;bad.endian=3;CHECK(!t.Find(bad,2));
 t.Invalidate(k.address-4096,k.address-1);CHECK(t.Find(k,2)==a);
 t.Invalidate(k.address+1,k.address+1);CHECK(!t.Find(k,2));
 // Invalidation does not allow reuse of a GPU image before its reader retires.
 for(int i=0;i<7;++i)CHECK(t.Reserve(2,0));CHECK(!t.Reserve(2,0));
 CHECK(!t.Reserve(1,0));CHECK(t.Reserve(3,2));CHECK(!t.Reserve(4,1));
 SceneSnapshotTracker versions;auto old=versions.Reserve(1,0);CHECK(versions.Publish(*old,k));
 auto newer=versions.Reserve(2,0);CHECK(versions.Publish(*newer,k));CHECK(versions.Find(k,2)==newer);
 versions.Clear();CHECK(!versions.Find(k,2));
 std::cout<<"Scene identity, invalidation, fence retirement passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
