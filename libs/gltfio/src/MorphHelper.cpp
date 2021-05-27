/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "MorphHelper.h"

#include <filament/BufferObject.h>
#include <filament/RenderableManager.h>
#include <filament/VertexBuffer.h>

#include "GltfEnums.h"
#include "TangentsJob.h"

using namespace filament;
using namespace filament::math;
using namespace utils;

static constexpr uint8_t kUnused = 0xff;

namespace gltfio {

uint32_t computeBindingSize(const cgltf_accessor* accessor);
uint32_t computeBindingOffset(const cgltf_accessor* accessor);

static const auto FREE_CALLBACK = [](void* mem, size_t, void*) { free(mem); };

// Returns true if a is a subset of b.
static bool isSubsetOf(ubyte4 a, ubyte4 b) {
    if (a.x != kUnused && a.x != b.x && a.x != b.y && a.x != b.z && a.x != b.w) return false;
    if (a.y != kUnused && a.y != b.x && a.y != b.y && a.y != b.z && a.y != b.w) return false;
    if (a.z != kUnused && a.z != b.x && a.z != b.y && a.z != b.z && a.z != b.w) return false;
    if (a.w != kUnused && a.w != b.x && a.w != b.y && a.w != b.z && a.w != b.w) return false;
    return true;
}

static int indexOf(int a, ubyte4 b) {
    if (a == b.x) return 0;
    if (a == b.y) return 1;
    if (a == b.z) return 2;
    if (a == b.w) return 3;
    return -1;
}

MorphHelper::MorphHelper(FFilamentAsset* asset, FFilamentInstance* inst) : mAsset(asset),
        mInstance(inst) {
    NodeMap& sourceNodes = asset->isInstanced() ? asset->mInstances[0]->nodeMap : asset->mNodeMap;
    for (auto pair : sourceNodes) {
        cgltf_node const* node = pair.first;
        cgltf_mesh const* mesh = node->mesh;
        if (mesh) {
            cgltf_primitive const* prims = mesh->primitives;
            for (cgltf_size pi = 0, count = mesh->primitives_count; pi < count; ++pi) {
                addPrimitive(mesh, pi, &mMorphTable[pair.second]);
            }
        }
    }
}

MorphHelper::~MorphHelper() {
    auto engine = mAsset->mEngine;
    for (auto& entry : mMorphTable) {
        for (auto& prim : entry.second.primitives) {
            for (auto& target : prim.targets) {
                engine->destroy(target.bufferObject);
            }
        }
    }
}

std::vector<std::string> MorphHelper::getTargetNames(Entity entity) noexcept {
    if (mMorphTable.find(entity) == mMorphTable.end()) {
        return std::vector<std::string>();
    }
    return mMorphTable[entity].targetNames;
}

size_t MorphHelper::getTargetCount(Entity entity) noexcept {
    if (mMorphTable.find(entity) == mMorphTable.end()) {
        return 0;
    }
    return mMorphTable[entity].targetNames.size();
}

bool MorphHelper::applyWeight(Entity entity, size_t index, float weight, bool commit) noexcept {
    if (mMorphTable.find(entity) == mMorphTable.end() ||
        mMorphTable[entity].targetWeights.size() <= index) {
        return false;
    }
    mMorphTable[entity].targetWeights[index] = weight;

    if (commit && !commitWeights(entity)) {
        return false;
    }

    return true;
}

bool MorphHelper::commitWeights(Entity entity) noexcept {
    if (mMorphTable.find(entity) == mMorphTable.end()) {
        return false;
    }
    const auto targetWeights = mMorphTable[entity].targetWeights;
    applyWeights(entity, targetWeights.data(), targetWeights.size());
    return true;
}

void MorphHelper::applyWeights(Entity entity, float const* weights, size_t count) noexcept {
    auto& engine = *mAsset->mEngine;
    auto renderableManager = &engine.getRenderableManager();
    auto renderable = renderableManager->getInstance(entity);

    // Let MorphHelper maintain entity weights
    if (mMorphTable.find(entity) != mMorphTable.end()) {
        auto targetWeights = mMorphTable[entity].targetWeights;
        count = std::min(count, targetWeights.size());
        std::copy(weights, weights + count, targetWeights.begin());
    }

    // If there are 4 or fewer targets, we can simply re-use the original VertexBuffer.
    if (count <= 4) {
        float4 vec{};
        for (size_t i = 0; i < count; i++) {
            vec[i] = weights[i];
        }
        renderableManager->setMorphWeights(renderable, vec);
        return;
    }

    // We honor up to 255 weights because our set representation is a 4-tuple of bytes, with one
    // slot reserved for a sentinel value. Note that 255 is much more than the glTF min spec of 4.
    count = std::min(count, size_t(255));

    // Make a copy of the weights because we want to re-order them.
    auto& sorted = mPartiallySortedWeights;
    sorted.clear();
    sorted.insert(sorted.begin(), weights, weights + count);

    // Find the four highest weights in O(n) by doing a partial sort.
    std::nth_element(sorted.begin(), sorted.begin() + 4, sorted.end(), [](float a, float b) {
        return a > b;
    });

    // Find the "primary indices" which are the indices of the four highest weights. This is O(n).
    ubyte4 primaryIndices = {kUnused, kUnused, kUnused, kUnused};
    for (size_t index = 0, primary = 0; index < count && primary < 4; ++index) {
        const float w = weights[index];
        if (w > 0 && (w == sorted[0] || w == sorted[1] || w == sorted[2] || w == sorted[3])) {
            primaryIndices[primary++] = index;
        }
    }

    // Swap out the buffer objects for the primary indices.
    for (const auto& prim : mMorphTable[entity].primitives) {
        for (const auto& target : prim.targets) {
            const int index = indexOf(target.morphTargetIndex, primaryIndices);
            if (index > -1) {
                assert_invariant(primaryIndices[index] == target.morphTargetIndex);
                VertexBuffer* vb = prim.vertexBuffer;
                const int bufferObjectSlot = target.type == cgltf_attribute_type_position ?
                        prim.positions[index] : prim.tangents[index];

                // Slot 0 is always used for the base position so if it's getting clobbered, then
                // something is seriously wrong. Should never occur, assert just in case.
                assert_invariant(bufferObjectSlot != 0);

                vb->setBufferObjectAt(engine, bufferObjectSlot, target.bufferObject);

                // Do not break out early because there could be more than one target entry for this
                // particular target index (e.g. positions + tangents).
            }
        }
    }

    // Finally, set the 4-tuple uniform for the weight values by derefing the primary indices.
    // Note that we first create a "safe set" by replacing the unused sentinel with zero.
    float4 highest;
    for (int i = 0; i < 4; i++) {
        highest[i] = (primaryIndices[i] == kUnused) ? 0 : weights[primaryIndices[i]];
    }
    renderableManager->setMorphWeights(renderable, highest);
}

static std::string getMorphTargetName(cgltf_mesh const* mesh, cgltf_primitive const* prim, int targetIndex) noexcept {
    if (mesh->target_names_count > 0 && mesh->target_names_count == prim->targets_count) {
        return mesh->target_names[targetIndex];
    }
    if (prim->target_names_count > 0 && prim->target_names_count == prim->targets_count) {
        return prim->target_names[targetIndex];
    }
    return "target_" + std::to_string(targetIndex);
}

// This method copies various morphing-related data from the FilamentAsset MeshCache primitive
// (which lives in transient memory) into the MorphHelper primitive (which will stay resident).
void MorphHelper::addPrimitive(cgltf_mesh const* mesh, int primitiveIndex, TableEntry* entry) {
    auto& engine = *mAsset->mEngine;
    const cgltf_primitive& prim = mesh->primitives[primitiveIndex];
    const auto& gltfioPrim = mAsset->mMeshCache.at(mesh)[primitiveIndex];
    VertexBuffer* vertexBuffer = gltfioPrim.vertices;

    entry->primitives.push_back({ vertexBuffer });
    auto& morphHelperPrim = entry->primitives.back();

    bool fillTargetNames = (entry->targetNames.size() == 0);

    for (int i = 0; i < 4; i++) {
        morphHelperPrim.positions[i] = gltfioPrim.morphPositions[i];
        morphHelperPrim.tangents[i] = gltfioPrim.morphTangents[i];
    }

    const cgltf_accessor* previous = nullptr;
    for (int targetIndex = 0; targetIndex < prim.targets_count; targetIndex++) {
        if (fillTargetNames) {
            entry->targetNames.push_back(getMorphTargetName(mesh, &prim, targetIndex));
            entry->targetWeights.push_back(0.f);
        }
        const cgltf_morph_target& morphTarget = prim.targets[targetIndex];
        for (cgltf_size aindex = 0; aindex < morphTarget.attributes_count; aindex++) {
            const cgltf_attribute& attribute = morphTarget.attributes[aindex];
            const cgltf_accessor* accessor = attribute.data;
            const cgltf_attribute_type atype = attribute.type;
            if (atype == cgltf_attribute_type_tangent) {
                continue;
            }
            if (atype == cgltf_attribute_type_normal) {

                // TODO: use JobSystem for this, like what we do for non-morph tangents.
                TangentsJob job;
                TangentsJob::Params params = { .in = { &prim, targetIndex } };
                TangentsJob::run(&params);

                if (params.out.results) {
                    const size_t size = params.out.vertexCount * sizeof(short4);
                    BufferObject* bufferObject = BufferObject::Builder().size(size).build(engine);
                    VertexBuffer::BufferDescriptor bd(params.out.results, size, FREE_CALLBACK);
                    bufferObject->setBuffer(engine, std::move(bd));
                    params.out.results = nullptr;
                    morphHelperPrim.targets.push_back({bufferObject, targetIndex, atype});
                }
                continue;
            }
            if (atype == cgltf_attribute_type_position) {
                // All position attributes must have the same data type.
                assert_invariant(!previous || previous->component_type == accessor->component_type);
                assert_invariant(!previous || previous->type == accessor->type);
                previous = accessor;

                // This should always be non-null, but don't crash if the glTF is malformed.
                if (accessor->buffer_view) {
                    auto bufferData = (const uint8_t*) accessor->buffer_view->buffer->data;
                    assert_invariant(bufferData);
                    const uint8_t* data = computeBindingOffset(accessor) + bufferData;
                    const uint32_t size = computeBindingSize(accessor);

                    // This creates a copy because we don't know when the user will free the cgltf
                    // source data. For non-morphed vertex buffers, we use a sharing mechanism to
                    // prevent copies, but here we just want to keep it as simple as possible.
                    uint8_t* clone = (uint8_t*) malloc(size);
                    memcpy(clone, data, size);

                    BufferObject* bufferObject = BufferObject::Builder().size(size).build(engine);
                    VertexBuffer::BufferDescriptor bd(clone, size, FREE_CALLBACK);
                    bufferObject->setBuffer(engine, std::move(bd));
                    morphHelperPrim.targets.push_back({bufferObject, targetIndex, atype});
                }
            }
        }
    }
}

}  // namespace gltfio
