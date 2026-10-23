// forza - VR step 1 (#333, docs/vr.md): a test head pose on the in-car camera, without a
// headset. Off unless the cvar vr_head_pose_test is on; normal play never reaches the code
// below the cvar check.
//
// The camera system: each split-screen view (CSplitView, manager + 244 + 172 * n; the
// manager is *(*(*0x832DF024 + 4) + 4)) points at +8 to its player's camera director. The
// director holds the active camera at +20 (CCamDriver is the in-car view with the driver's
// hands, CCamCockpit is not used by the player), the camera's world position at +176, and
// the view matrix the game derived for this frame at +240 (row-vector, world to camera)
// with the vertical FOV at +304. Two functions read that matrix through its accessor
// sub_8284EF88, and nothing else calls the accessor:
//   sub_825A9D20 (view, ...)  hands the matrix, FOV, aspect and clip planes to the view's
//                             presentation camera (CSplitView + 40, a deferred
//                             IPresentationCamera: SetViewTransform is vtable +168) and the
//                             same set again to sub_82DBADC0 (the render-side view state);
//   sub_829A4860 (?, out)     builds view 0's frustum for something game-side
//                             (sub_823EAED0; posing it or not changes nothing visible).
// Both run on the main loop thread right after the director update (sub_82858288), from
// CSplitView::Update (sub_825AA1B0) and the world update (sub_823E3218).
// Both hooks put the posed matrix in place, run the original and put the game's matrix back
// (unless the game wrote a new one meanwhile), so the camera's own state never carries the
// pose and nothing drifts. The director's position/forward/up (+176/+208/+224) are left
// alone: whatever reads them sees the car's camera.
//
// Pose source (yaw pitch roll in degrees, x y z in metres in camera space: x right, y up,
// z forward; positive yaw looks left, pitch up, roll tilts the head left):
//   FORZA_VR_POSE="yaw,pitch,roll,x,y,z"   a fixed pose from the start (missing values 0)
//   FORZA_VR_POSE_FIFO=<path>             lines "yaw pitch roll x y z" (spaces or commas)
//                                          replace the pose as they arrive (Linux; mkfifo)
//   FORZA_VR_POSE_ALL=1                    every camera, not only the in-car ones
//   FORZA_VR_POSE_SKIP=view|frustum        leave one of the two readers unposed (to see
//                                          what each one feeds); a FIFO line "skip view",
//                                          "skip frustum" or "skip none" changes it
//   FORZA_VR_POSE_TRACE=1                  log the calling thread every 300th call
#include "../platform/guest_addr.h"
#include "../platform/guest_memory.h"
#include "../platform/vr_head_pose_math.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

REXCVAR_DEFINE_BOOL(vr_head_pose_test, false, "VR",
                    "Experimental: apply a test head pose (FORZA_VR_POSE / FORZA_VR_POSE_FIFO) "
                    "to the in-car camera");

namespace {
using forza::vr::HeadPose;

constexpr uint32_t kViewMatrix = 240;  // director: world-to-camera matrix, 4x4 floats
constexpr uint32_t kActiveCamera = 20;

std::mutex pose_mutex;
HeadPose pose;
std::atomic<bool> pose_set{false};
std::atomic<int> skip_hook{0};  // 0 none, 1 view, 2 frustum
int SkipCode(const char* name) {
  return !name ? 0 : std::strcmp(name, "view") == 0 ? 1 : std::strcmp(name, "frustum") == 0 ? 2 : 0;
}

uint32_t Load32(uint8_t* base, uint32_t address) {
  const uint8_t* p = forza::GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
bool Valid(uint32_t address) { return address >= 0x00100000 && address < 0xF0000000; }

bool Parse(const char* text, HeadPose& out) {
  float v[6] = {};
  int n = 0;
  const char* p = text;
  while (n < 6 && *p) {
    char* end = nullptr;
    const float f = std::strtof(p, &end);
    if (end == p) {
      ++p;  // separator
      continue;
    }
    v[n++] = f;
    p = end;
  }
  if (!n) return false;
  out = HeadPose{v[0], v[1], v[2], v[3], v[4], v[5]};
  return true;
}

void SetPose(const HeadPose& p, const char* from) {
  {
    std::lock_guard lock(pose_mutex);
    pose = p;
  }
  pose_set = !p.IsIdentity();
  REXLOG_INFO("VR head pose ({}): yaw {:.1f} pitch {:.1f} roll {:.1f}, offset {:.3f} {:.3f} {:.3f} m",
              from, p.yaw, p.pitch, p.roll, p.x, p.y, p.z);
}

void ReadFifo(std::string path) {
#ifdef _WIN32
  REXLOG_WARN("FORZA_VR_POSE_FIFO is not supported on Windows ({})", path);
#else
  const int fd = open(path.c_str(), O_RDWR);  // O_RDWR: no EOF when a writer goes away
  if (fd < 0) {
    REXLOG_WARN("FORZA_VR_POSE_FIFO: cannot open {}", path);
    return;
  }
  std::string line;
  char c;
  while (read(fd, &c, 1) == 1) {
    if (c != '\n') {
      line += c;
      continue;
    }
    HeadPose p;
    if (line.rfind("skip ", 0) == 0) {
      skip_hook = SkipCode(line.c_str() + 5);
      REXLOG_INFO("VR head pose: skip {}", line.c_str() + 5);
    } else if (Parse(line.c_str(), p)) {
      SetPose(p, "fifo");
    }
    line.clear();
  }
#endif
}

void InitOnce() {
  static std::once_flag once;
  std::call_once(once, [] {
    HeadPose p;
    if (const char* text = std::getenv("FORZA_VR_POSE"); text && Parse(text, p)) SetPose(p, "env");
    skip_hook = SkipCode(std::getenv("FORZA_VR_POSE_SKIP"));
    if (const char* fifo = std::getenv("FORZA_VR_POSE_FIFO"); fifo && *fifo)
      std::thread(ReadFifo, std::string(fifo)).detach();
    REXLOG_INFO("VR head pose test on: {}", std::getenv("FORZA_VR_POSE_ALL") ? "every camera"
                                                                             : "in-car cameras");
  });
}

bool InCar(uint8_t* base, uint32_t camera) {
  static const bool all = std::getenv("FORZA_VR_POSE_ALL") != nullptr;
  if (all) return true;
  const uint32_t vtable = Load32(base, camera);
  return vtable == FORZA_ADDR(0x82075DD4)     // Camera::CCamDriver (the cockpit view)
         || vtable == FORZA_ADDR(0x82075CD4);  // Camera::CCamCockpit
}

// FORZA_VR_POSE_TRACE=1: which thread calls each hook, every 300th call.
void Trace(const char* hook, uint32_t director) {
  static const bool on = std::getenv("FORZA_VR_POSE_TRACE") != nullptr;
  if (!on) return;
  static std::atomic<uint32_t> calls{0};
  if (calls++ % 300) return;
#ifdef _WIN32
  const long tid = 0;
#else
  const long tid = long(syscall(SYS_gettid));
#endif
  REXLOG_INFO("VR head pose trace: {} director {:08X} thread {}", hook, director, tid);
}

// Swaps the posed view matrix into the director for the length of one guest call.
class PosedView {
 public:
  PosedView(uint8_t* base, uint32_t director, const char* hook) {
    if (!REXCVAR_GET(vr_head_pose_test)) return;
    InitOnce();
    Trace(hook, director);
    if (skip_hook && skip_hook == SkipCode(hook)) return;
    if (!pose_set || !Valid(director)) return;
    const uint32_t camera = Load32(base, director + kActiveCamera);
    if (!Valid(camera) || !InCar(base, camera)) return;
    HeadPose p;
    {
      std::lock_guard lock(pose_mutex);
      p = pose;
    }
    matrix_ = forza::GuestPtr(base, director + kViewMatrix);
    std::memcpy(original_, matrix_, sizeof(original_));
    float view[16];
    for (int i = 0; i < 16; ++i) {
      const uint8_t* q = original_ + 4 * i;
      const uint32_t bits = uint32_t(q[0]) << 24 | uint32_t(q[1]) << 16 | uint32_t(q[2]) << 8 | q[3];
      std::memcpy(&view[i], &bits, 4);
    }
    forza::vr::ApplyHeadPose(view, p);
    for (int i = 0; i < 16; ++i) {
      uint32_t bits;
      std::memcpy(&bits, &view[i], 4);
      uint8_t* q = posed_ + 4 * i;
      q[0] = uint8_t(bits >> 24), q[1] = uint8_t(bits >> 16), q[2] = uint8_t(bits >> 8), q[3] = uint8_t(bits);
    }
    std::memcpy(matrix_, posed_, sizeof(posed_));
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
      REXLOG_INFO("VR head pose: first posed view (director {:08X}, camera {:08X})", director, camera);
  }
  ~PosedView() {
    // The game may have computed the next view meanwhile (another thread): keep that one.
    if (matrix_ && std::memcmp(matrix_, posed_, sizeof(posed_)) == 0)
      std::memcpy(matrix_, original_, sizeof(original_));
  }
  PosedView(const PosedView&) = delete;
  PosedView& operator=(const PosedView&) = delete;

 private:
  uint8_t* matrix_ = nullptr;
  uint8_t original_[64];
  uint8_t posed_[64];
};

// View 0's director, found as sub_829A4860 finds it.
uint32_t FirstViewDirector(uint8_t* base) {
  const uint32_t root = Load32(base, FORZA_ADDR(0x832DF024));
  if (!Valid(root)) return 0;
  const uint32_t holder = Load32(base, root + 4);
  if (!Valid(holder)) return 0;
  const uint32_t manager = Load32(base, holder + 4);
  if (!Valid(manager)) return 0;
  return Load32(base, manager + 244 + 8);
}
}  // namespace

// The view's presentation camera update (view = CSplitView: +8 director, +40 camera).
extern "C" REX_FUNC(__imp__sub_825A9D20);
extern "C" REX_FUNC(sub_825A9D20) {
  const uint32_t view = ctx.r3.u32;
  PosedView posed(base, Valid(view) ? Load32(base, view + 8) : 0, "view");
  __imp__sub_825A9D20(ctx, base);
}

// View 0's frustum for the game-side visibility.
extern "C" REX_FUNC(__imp__sub_829A4860);
extern "C" REX_FUNC(sub_829A4860) {
  PosedView posed(base, REXCVAR_GET(vr_head_pose_test) ? FirstViewDirector(base) : 0, "frustum");
  __imp__sub_829A4860(ctx, base);
}
