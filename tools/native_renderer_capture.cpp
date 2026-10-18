// Read-only RenderDoc capture inventory for the native renderer bring-up.
// Build against the official RenderDoc replay headers/library. No shader code,
// texture bytes, vertex data or saved-game content is exported.
#include "../forza-recomp/src/platform/native_renderer/render_graph.h"
#include "renderdoc_replay.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>

REPLAY_PROGRAM_MARKER();

namespace nr = forza::native_renderer;
using Id = nr::ResourceId;

Id Number(ResourceId id) {
  Id number;
  static_assert(sizeof(number) == sizeof(id));
  std::memcpy(&number, &id, sizeof(id));
  return number;
}

std::string Json(const std::string &value) {
  std::ostringstream out;
  out << '"';
  for (unsigned char c : value) {
    if (c == '"' || c == '\\')
      out << '\\' << char(c);
    else if (c == '\n')
      out << "\\n";
    else if (c == '\r')
      out << "\\r";
    else if (c == '\t')
      out << "\\t";
    else if (c < 32) {
      constexpr char hex[] = "0123456789abcdef";
      out << "\\u00" << hex[c >> 4] << hex[c & 15];
    } else
      out << char(c);
  }
  out << '"';
  return out.str();
}

std::optional<nr::Access> Access(ResourceUsage usage) {
  switch (usage) {
  case ResourceUsage::Unused:
  case ResourceUsage::Barrier:
    return {};
  case ResourceUsage::Clear:
  case ResourceUsage::CPUWrite:
  case ResourceUsage::CopyDst:
  case ResourceUsage::ResolveDst:
  case ResourceUsage::StreamOut:
    return nr::Access::write;
  case ResourceUsage::Discard:
    return nr::Access::discard;
  case ResourceUsage::ColorTarget:
  case ResourceUsage::DepthStencilTarget:
  case ResourceUsage::GenMips:
  case ResourceUsage::Copy:
  case ResourceUsage::Resolve:
    return nr::Access::read_write;
  default:
    if (usage >= ResourceUsage::VS_RWResource &&
        usage <= ResourceUsage::All_RWResource)
      return nr::Access::read_write;
    if ((usage >= ResourceUsage::VS_Constants &&
         usage <= ResourceUsage::All_Constants) ||
        (usage >= ResourceUsage::VS_Resource &&
         usage <= ResourceUsage::All_Resource) ||
        usage == ResourceUsage::VertexBuffer ||
        usage == ResourceUsage::IndexBuffer ||
        usage == ResourceUsage::Indirect ||
        usage == ResourceUsage::InputTarget ||
        usage == ResourceUsage::CopySrc || usage == ResourceUsage::ResolveSrc)
      return nr::Access::read;
    // A future/unknown usage must not silently lose a write dependency.
    return nr::Access::read_write;
  }
}

struct Operation {
  uint32_t event = 0, flags = 0, indices = 0;
  std::string name;
  std::vector<Id> targets;
  Id depth = 0;
  std::vector<std::pair<Id, nr::Access>> uses;
};

void Actions(const rdcarray<ActionDescription> &actions, const SDFile &file,
             std::map<uint32_t, Operation> &operations) {
  for (const auto &action : actions) {
    auto &op = operations[action.eventId];
    op.event = action.eventId;
    op.flags = uint32_t(action.flags);
    op.indices = action.numIndices;
    op.name = action.GetName(file).c_str();
    for (const auto &target : action.outputs)
      if (Number(target))
        op.targets.push_back(Number(target));
    op.depth = Number(action.depthOut);
    Actions(action.children, file, operations);
  }
}

int main(int argc, char **argv) {
  if (argc < 3) {
    std::cerr << "Usage: native_renderer_capture capture.rdc output-directory "
                 "[event]\n";
    return 2;
  }
  std::filesystem::path folder(argv[2]);
  std::filesystem::create_directories(folder);
  GlobalEnvironment environment;
  environment.enumerateGPUs = false;
  RENDERDOC_InitialiseReplay(environment, {});
  ICaptureFile *capture = RENDERDOC_OpenCaptureFile();
  auto status = capture->OpenFile(argv[1], "", nullptr);
  if (!status.OK()) {
    std::cerr << "Could not open capture: " << uint32_t(status.code) << '\n';
    capture->Shutdown();
    RENDERDOC_ShutdownReplay();
    return 3;
  }
  auto opened = capture->OpenCapture(ReplayOptions(), nullptr);
  if (!opened.first.OK()) {
    std::cerr << "Could not replay capture: " << uint32_t(opened.first.code)
              << '\n';
    capture->Shutdown();
    RENDERDOC_ShutdownReplay();
    return 4;
  }
  auto *controller = opened.second;
  int result = 0;
  try {
    nr::RenderGraph graph;
    std::map<uint32_t, Operation> operations;
    Actions(controller->GetRootActions(), controller->GetStructuredFile(),
            operations);
    std::map<Id, ResourceId> resources;
    std::ofstream images(folder / "images.tsv"),
        buffers(folder / "buffers.tsv");
    images << "id\twidth\theight\tdepth\tmips\tlayers\tsamples\tformat\tmemory"
              "\toffset\tbytes\n";
    buffers << "id\tbytes\tmemory\toffset\n";
    for (const auto &image : controller->GetTextures()) {
      const Id id = Number(image.resourceId);
      resources.emplace(id, image.resourceId);
      // A captured frame imports its initial resources. This does not establish
      // how a native frontend will populate them, or reconstruct EDRAM aliases.
      graph.Declare({id, "image " + std::to_string(id), 0, true});
      images << id << '\t' << image.width << '\t' << image.height << '\t'
             << image.depth << '\t' << image.mips << '\t' << image.arraysize
             << '\t' << image.msSamp << '\t' << image.format.Name().c_str()
             << '\t' << Number(image.memory) << '\t' << image.memoryOffset
             << '\t' << image.byteSize << '\n';
    }
    for (const auto &buffer : controller->GetBuffers()) {
      const Id id = Number(buffer.resourceId);
      resources.emplace(id, buffer.resourceId);
      graph.Declare({id, "buffer " + std::to_string(id), 0, true});
      buffers << id << '\t' << buffer.length << '\t' << Number(buffer.memory)
              << '\t' << buffer.memoryOffset << '\n';
    }
    for (const auto &[id, handle] : resources) {
      const Id used_id = id;
      for (const auto &use : controller->GetUsage(handle)) {
        const auto access = Access(use.usage);
        if (!access)
          continue;
        auto &op = operations[use.eventId];
        op.event = use.eventId;
        if (op.name.empty())
          op.name = "resource operation";
        // Discard is a separate state transition. A clear/write reported at the
        // same event defines the contents; do not retain the discarded version.
        auto previous = std::find_if(
            op.uses.begin(), op.uses.end(),
            [used_id](const auto &item) { return item.first == used_id; });
        if (previous != op.uses.end() &&
            previous->second == nr::Access::discard)
          previous->second = *access;
        else if (*access != nr::Access::discard || previous == op.uses.end())
          op.uses.emplace_back(id, *access);
      }
    }
    std::ofstream output(folder / "graph.json");
    output
        << "{\n\"schema\":1,\n\"granularity\":\"whole_resource\",\n"
           "\"execution_ready\":false,\n\"aliasing\":\"not_reconstructed\",\n"
           "\"operations\":[\n";
    bool first = true;
    uint64_t edges = 0;
    uint32_t undefined = 0;
    for (const auto &[event, op] : operations) {
      if (op.uses.empty())
        continue;
      const auto &pass = graph.AddPass(op.name, op.uses);
      edges += pass.dependencies.size();
      undefined += uint32_t(pass.undefined_reads.size());
      if (!first)
        output << ",\n";
      first = false;
      output << "{\"id\":" << pass.id << ",\"event\":" << event
             << ",\"flags\":" << op.flags << ",\"indices\":" << op.indices
             << ",\"name\":" << Json(op.name)
             << ",\"depth_target\":" << op.depth << ",\"color_targets\":[";
      for (size_t i = 0; i < op.targets.size(); ++i) {
        if (i)
          output << ',';
        output << op.targets[i];
      }
      output << "],\"uses\":[";
      for (size_t i = 0; i < pass.uses.size(); ++i) {
        const auto &use = pass.uses[i];
        if (i)
          output << ',';
        output << "{\"resource\":" << use.resource
               << ",\"access\":" << int(use.access)
               << ",\"read_version\":" << use.read_version
               << ",\"write_version\":" << use.write_version << '}';
      }
      output << "],\"dependencies\":[";
      for (size_t i = 0; i < pass.dependencies.size(); ++i) {
        const auto &dep = pass.dependencies[i];
        if (i)
          output << ',';
        output << "{\"before\":" << dep.before
               << ",\"resource\":" << dep.resource
               << ",\"hazard\":" << int(dep.hazard) << '}';
      }
      output << "],\"undefined_reads\":[";
      for (size_t i = 0; i < pass.undefined_reads.size(); ++i) {
        if (i)
          output << ',';
        output << pass.undefined_reads[i];
      }
      output << "]}";
    }
    output << "\n],\n\"resources\":" << resources.size()
           << ",\n\"dependency_edges\":" << edges
           << ",\n\"undefined_reads\":" << undefined << "\n}\n";
    output.close();
    images.close();
    buffers.close();
    if (!output || !images || !buffers)
      throw std::runtime_error("Could not write capture inventory");
    for (int event_arg = 3; event_arg < argc; ++event_arg) {
      const uint32_t event = uint32_t(std::stoul(argv[event_arg]));
      controller->SetFrameEvent(event, true);
      const auto *state = controller->GetVulkanPipelineState();
      std::ofstream detail(folder /
                           ("event-" + std::to_string(event) + ".json"));
      const bool compute =
          (operations.at(event).flags & uint32_t(ActionFlags::Dispatch)) != 0;
      detail << "{\"event\":" << event << ",\"vertex_shader\":"
             << (compute ? 0 : Number(state->vertexShader.resourceId))
             << ",\"fragment_shader\":"
             << (compute ? 0 : Number(state->fragmentShader.resourceId))
             << ",\"compute_shader\":"
             << (compute ? Number(state->computeShader.resourceId) : 0)
             << ",\"viewports\":[";
      bool first_viewport = true;
      for (const auto &vp : state->viewportScissor.viewportScissors) {
        if (!first_viewport)
          detail << ',';
        first_viewport = false;
        detail << "{\"x\":" << vp.vp.x << ",\"y\":" << vp.vp.y
               << ",\"width\":" << vp.vp.width << ",\"height\":" << vp.vp.height
               << ",\"scissor_width\":" << vp.scissor.width
               << ",\"scissor_height\":" << vp.scissor.height << '}';
      }
      detail << "],\"descriptors\":[";
      bool first_descriptor = true;
      for (const auto &access : controller->GetDescriptorAccess()) {
        if (access.staticallyUnused ||
            ((access.stage == ShaderStage::Compute) != compute))
          continue;
        for (const auto &descriptor : controller->GetDescriptors(
                 access.descriptorStore, {DescriptorRange(access)})) {
          if (!first_descriptor)
            detail << ',';
          first_descriptor = false;
          detail << "{\"stage\":" << int(access.stage)
                 << ",\"binding\":" << access.index
                 << ",\"type\":" << int(access.type)
                 << ",\"resource\":" << Number(descriptor.resource)
                 << ",\"offset\":" << descriptor.byteOffset
                 << ",\"bytes\":" << descriptor.byteSize
                 << ",\"first_mip\":" << uint32_t(descriptor.firstMip)
                 << ",\"first_layer\":" << uint32_t(descriptor.firstSlice)
                 << '}';
        }
      }
      detail << "]}\n";
      detail.close();
      if (!detail)
        throw std::runtime_error("Could not write event detail");
    }
    std::cout << graph.passes().size() << " operations, " << resources.size()
              << " resources, " << edges << " dependencies, " << undefined
              << " undefined reads\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    result = 5;
  }
  controller->Shutdown();
  capture->Shutdown();
  RENDERDOC_ShutdownReplay();
  return result;
}
