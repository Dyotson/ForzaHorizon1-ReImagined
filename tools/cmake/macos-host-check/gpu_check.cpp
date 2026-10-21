// No game code/data: check native plugin loading and an emulation-capable device.
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/gpu_plugin.h>
#include <rex/ui/vulkan/provider.h>

int main(int argc, char** argv) {
  rex::InitLogging();
  rex::cvar::Init(argc, argv);
  auto gpu = rex::system::LoadGpuPlugin("xenos", "vulkan");
  if (!gpu) {
    REXLOG_ERROR("macOS GPU check: plugin load failed");
    return 1;
  }
  auto provider = rex::ui::vulkan::VulkanProvider::Create(true, false);
  if (!provider) {
    REXLOG_ERROR("macOS GPU check: emulation-capable Vulkan device creation failed");
    return 2;
  }
  REXLOG_INFO("macOS GPU check passed: native Xenos plugin loaded and Vulkan device created");
  provider.reset();
  gpu.reset();
  rex::ShutdownLogging();
  return 0;
}
