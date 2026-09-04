#include "gpu/scene/native_mesh_data.h"
#include <cstdlib>
#include <iostream>

using namespace bd::gpu::scene;
static void Check(bool ok, const char *name) {
  if (!ok) {
    std::cerr << "FAIL: " << name << '\n';
    std::exit(1);
  }
}
int main() {
  std::vector<uint32_t> tris;
  const std::vector<uint8_t> strip = {
      0,0, 0,1, 0,2, 0,3, 255,255, 0,4, 0,5, 0,6, 0,7};
  Check(ImportMeshIndices(strip, false, MeshTopology::Strip, tris), "strip import");
  Check(tris == std::vector<uint32_t>({0,1,2, 1,3,2, 4,5,6, 5,7,6}),
        "strip winding and restart parity");
  const std::vector<uint8_t> degenerates = {0,0, 0,1, 0,1, 0,2, 0,3};
  Check(ImportMeshIndices(degenerates, false, MeshTopology::Strip, tris) &&
        tris == std::vector<uint32_t>({1,2,3}), "degenerates advance parity");
  const std::vector<uint8_t> list = {0,0,0,1, 0,0,0,2, 0,0,255,255};
  Check(ImportMeshIndices(list, true, MeshTopology::Triangles, tris) &&
        tris == std::vector<uint32_t>({1,2,65535}), "32-bit indices and list 65535");
  Check(!ImportMeshIndices(std::span(list).first(11), true,
                          MeshTopology::Triangles, tris), "partial index rejected");

  NativeMeshData mesh;
  mesh.layout = 0x1020304050607080ull;
  mesh.base_vertex = -4;
  mesh.indices = {4,5,6};
  mesh.streams.push_back({2, 12, std::vector<uint8_t>(36, 0x3f)});
  mesh.streams.push_back({7, 4, std::vector<uint8_t>(12, 0x80)});
  Check(ValidateNativeMesh(mesh), "valid negative base vertex");
  std::vector<uint8_t> file;
  Check(EncodeNativeMesh(mesh, file), "encode");
  NativeMeshData decoded;
  Check(DecodeNativeMesh(file, decoded), "decode");
  Check(decoded.layout == mesh.layout && decoded.base_vertex == -4 &&
        decoded.indices == mesh.indices && decoded.streams.size() == 2 &&
        decoded.streams[0].slot == 2 && decoded.streams[1].stride == 4 &&
        decoded.streams[0].bytes == mesh.streams[0].bytes &&
        decoded.streams[1].bytes == mesh.streams[1].bytes, "payload round trip");
  for (size_t size = 0; size < file.size(); ++size)
    Check(!DecodeNativeMesh(std::span(file).first(size), decoded), "truncation rejected");
  for (size_t i = 0; i < file.size(); ++i) {
    file[i] ^= 1;
    Check(!DecodeNativeMesh(file, decoded), "corruption rejected");
    file[i] ^= 1;
  }
  file.push_back(0);
  Check(!DecodeNativeMesh(file, decoded), "trailing bytes rejected");
  mesh.streams[1].slot = 2;
  Check(!ValidateNativeMesh(mesh), "duplicate binding rejected");
  mesh.streams[1].slot = 7;
  mesh.streams[0].bytes.pop_back();
  Check(!ValidateNativeMesh(mesh), "vertex range outside stream rejected");
  mesh.streams[0].bytes.push_back(0);
  mesh.base_vertex = -5;
  Check(!ValidateNativeMesh(mesh), "negative effective index rejected");
  std::cout << "native mesh: topology, bounds, round trip, truncation and corruption passed\n";
}
