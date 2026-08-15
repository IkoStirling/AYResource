# Phase 0 — Known follow-ups (not fixed in Phase 0)

This document captures deferred items that surfaced during Phase 0 R-01 / RD-02 / chunked .aymesh work but are intentionally **out of scope** for the Phase 0 exit criteria. Each entry lists the symptom, the root cause, where it lives, and what Phase work item should pick it up.

---

## F-01: `SubmeshData::vertexOffset` is dropped by `MeshConverter`

**Symptoms:**
- A submesh in an FBX/glTF input whose vertices do not start at index 0 will draw with the wrong vertex range after conversion.

**Root cause:**
- `AYResource/IntermediateAsset.h::SubmeshData` has 4 fields:
  ```
  uint32_t startIndex, indexCount, vertexOffset, materialIndex;
  ```
- `interface/AYResource/assetsDefs/IMesh.h::IMesh::Submesh` has 3 fields:
  ```
  UInt32 indexOffset, indexCount, materialIndex;
  ```
- `src/Converter/MeshConverter.cpp` saveToBinary forward-maps only 3 fields:
  ```
  startIndex      → indexOffset       ✓
  indexCount      → indexCount        ✓
  materialIndex   → materialIndex     ✓
  vertexOffset    → (dropped)         ✗
  ```
- Today every converter input happens to have `vertexOffset == 0` (cubes, spheres, single-skin meshes), so no test fails. But the moment R-02 / AN-01 partitions meshes (skin-LOD, partition into bone ranges), this will silently misbehave.

**Where it lives:**
- `D:\Projects\AYRuntime\AYResource\interface\AYResource\assetsDefs\IMesh.h` (struct Submesh)
- `D:\Projects\AYRuntime\AYResource\include\AYResource/IntermediateAsset.h` (struct SubmeshData)
- `D:\Projects\AYRuntime\AYResource\src\Converter\MeshConverter.cpp` (lines mapping fields)

**Fix path:**
1. Add `UInt32 vertexOffset` to `IMesh::Submesh`.
2. Update `AYResource/assetsDefs/IMesh.h::AttributeInfo` consumers and any layout-sensitive code in AYRenderer (none yet — RenderAssetBridge does not consume Submesh today).
3. Update `Mesh::saveToBinary` / `loadFromBinary` SUBM chunk:
   - bump chunk size by 4 bytes per submesh (16 instead of 12)
   - already uses `sizeof(Submesh)`, no manual offsets
4. Update `MeshConverter::saveToBinary` to forward `vertexOffset`.
5. Add a converter-level round-trip test with non-zero `vertexOffset`.

**Phase work item to pick up:**
- Phase 1 R-02 (FBX animation / mesh partitioning). Marker: any task that needs skin-LOD or per-bone vertex range.

---

## F-02: `IMesh::Submesh` has no `#pragma pack` — future 8-byte fields will silently pad the SUBM chunk

**Symptoms:**
- If someone adds `int64_t` / `double` (or any 8-byte-aligned field) to `IMesh::Submesh`, the compiler inserts 4 bytes of padding between adjacent fields. `sizeof(Submesh)` becomes `fields + padding`, and the SUBM chunk's on-disk size grows by the padding total.
- Today: 3 × UInt32 = 12 bytes, no padding, `sizeof(Submesh) == 12`, no bug.

**Why it matters:**
- `Mesh::saveToBinary` writes `_submeshes.size() * sizeof(Submesh)` to the SUBM chunk and `loadFromBinary` reads the same size — fields round-trip correctly, **but the chunk contains uninitialized padding bytes**.
- If anyone later adds content hashing, chunk dedup, compression on the chunk, or wires a debug overlay that hex-dumps the chunk, the padding bytes will be observable noise.

**Where it lives:**
- `D:\Projects\AYRuntime\AYResource\interface\AYResource\assetsDefs\IMesh.h` (struct Submesh)

**Fix path:**
- Either:
  - Wrap `Submesh` in `#pragma pack(push, 1) ... #pragma pack(pop)` so the on-disk layout is deterministic, OR
  - Document the rule "all Submesh fields must be 4-byte-aligned types" and add a `static_assert` for total size in tests.

---

## F-03: `_setForTest*` family is public on `Mesh` for production-grade access

**Symptoms:**
- `Mesh` exposes `_setForTestAttributeMask`, `_setForTestVertexLayout`, `_setForTestVertexData`, `_setForTestIndices`, `_setForTestSubmeshes`, `_addForTestMaterialSlot`, `_setForTestSkinWeights`, `_setForTestBounds`.
- These let any caller build a partially-constructed Mesh without going through `loadFromBinary` / FBX / glTF. Today they are used by:
  - `MeshConverter::saveToBinary` (legitimate, but still a side door past the loader contract)
  - `AYTest_MeshBinaryChunk::submesh_multi_chunk_round_trip` (legitimate test)
- If a future task uses these by accident (e.g. someone calls `_setForTestVertexLayout` from a runtime path), the Mesh will be in a half-loaded state — `hasBounds() == false` / `_hasSkinWeights` may be inconsistent with `_attributeMask` / `_vertexData`.

**Where it lives:**
- `D:\Projects\AYRuntime\AYResource\include\AYResource/assetsImpl/Mesh.h` (public test setters, lines ~143-149)
- `D:\Projects\AYRuntime\AYResource\src\AssetsImpl\AYMesh.cpp` (their bodies)

**Fix path:**
- Demote to `friend class MeshTestHooks;` or namespace a `mesh_test::buildFromData(...)` free function with a tag type. Phase 1 can do this when the project has more Mesh constructors.
- Untouched in Phase 0 — keeping the door open is the pragmatic choice here.

---

## Notes on why these remain open

Phase 0 exit criterion (from `ENGINE-FOUNDATION-PLAN.md` §exit-criteria):
> Phase 0 complete when an FBX mesh + skeleton loads, the debug overlay shows the per-attribute vertex layout, and the renderer can draw a skinned mesh on the GPU.

That implies:
- Vertex attributes and skin weights reach GpuMesh (RD-02 ✓).
- The chunked .aymesh binary format round-trips correctly (R-01 ✓, including new `submesh_multi_chunk_round_trip` test).
- The debug overlay works (not part of this audit).

Submesh partitioning, padding hygiene, and test-only API scoping are all **Phase 1 hygiene** — they would all be solved by adding a Submesh `vertexOffset` and a small Mesh-construction API in the same revision.
