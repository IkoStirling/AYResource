// Assimp source audit helper. Kept outside AYResource's src/ glob so it is
// built only when an importer investigation explicitly needs it.
#include <assimp/scene.h>
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <iostream>
#include <cstdio>

void printNodeTree(const aiNode* node, int depth, const aiScene* scene) {
    if (!node) return;
    
    // 缩进
    for (int i = 0; i < depth; i++) printf("  ");
    
    printf("Node: '%s' (mNumMeshes=%u, mNumChildren=%u)\n", 
           node->mName.C_Str(), node->mNumMeshes, node->mNumChildren);
    
    // 打印此节点引用的 mesh
    if (node->mNumMeshes > 0) {
        for (int i = 0; i < depth; i++) printf("  ");
        printf("  Mesh indices: ");
        for (unsigned int i = 0; i < node->mNumMeshes; i++) {
            printf("%u ", node->mMeshes[i]);
        }
        printf("\n");
        
        // 验证这些 mesh 确实存在
        for (unsigned int i = 0; i < node->mNumMeshes; i++) {
            unsigned int mi = node->mMeshes[i];
            if (mi < scene->mNumMeshes) {
                const aiMesh* m = scene->mMeshes[mi];
                for (int j = 0; j < depth; j++) printf("  ");
                printf("    -> Mesh[%u] '%s': material=%u vertices=%u faces=%u "
                       "bones=%u morphTargets=%u\n",
                       mi, m->mName.C_Str(), m->mMaterialIndex,
                       m->mNumVertices, m->mNumFaces, m->mNumBones,
                       m->mNumAnimMeshes);
                for (unsigned int ai = 0; ai < m->mNumAnimMeshes; ++ai) {
                    const aiAnimMesh* morph = m->mAnimMeshes[ai];
                    if (!morph) continue;
                    printf("      morph[%u] '%s' vertices=%u weight=%.6f\n",
                           ai, morph->mName.C_Str(), morph->mNumVertices,
                           morph->mWeight);
                }
            }
        }
    }
    
    // 递归子节点
    for (unsigned int i = 0; i < node->mNumChildren; i++) {
        printNodeTree(node->mChildren[i], depth + 1, scene);
    }
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: assimp_audit <source.fbx>\n");
        return 2;
    }

    const char* paths[] = { argv[1] };
    for (const char* path : paths) {
        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(
            path, aiProcess_Triangulate | aiProcess_JoinIdenticalVertices);
        if (!scene) {
            printf("Failed to load: %s (%s)\n", path,
                   importer.GetErrorString());
            continue;
        }
        
        printf("=== %s ===\n", path);
        printf("scene->mNumMeshes = %u\n", scene->mNumMeshes);
        printf("scene->mRootNode->mNumMeshes = %u\n", scene->mRootNode->mNumMeshes);
        printf("scene->mRootNode->mNumChildren = %u\n", scene->mRootNode->mNumChildren);

        printf("\nMaterials:\n");
        for (unsigned int i = 0; i < scene->mNumMaterials; ++i) {
            aiString name;
            scene->mMaterials[i]->Get(AI_MATKEY_NAME, name);
            printf("  material[%u] '%s'\n", i, name.C_Str());
        }

        printf("\nAnimations:\n");
        for (unsigned int i = 0; i < scene->mNumAnimations; ++i) {
            const aiAnimation* anim = scene->mAnimations[i];
            printf("  animation[%u] '%s' duration=%.6f tps=%.6f "
                   "nodeChannels=%u morphChannels=%u\n",
                   i, anim->mName.C_Str(), anim->mDuration,
                   anim->mTicksPerSecond, anim->mNumChannels,
                   anim->mNumMorphMeshChannels);
            for (unsigned int c = 0; c < anim->mNumMorphMeshChannels; ++c) {
                const aiMeshMorphAnim* channel = anim->mMorphMeshChannels[c];
                if (!channel) continue;
                printf("    morphChannel[%u] '%s' keys=%u\n", c,
                       channel->mName.C_Str(), channel->mNumKeys);
                for (unsigned int k = 0; k < channel->mNumKeys; ++k) {
                    const aiMeshMorphKey& key = channel->mKeys[k];
                    printf("      key[%u] time=%.6f values=%u", k,
                           key.mTime, key.mNumValuesAndWeights);
                    const unsigned int shown =
                        key.mNumValuesAndWeights < 8
                            ? key.mNumValuesAndWeights : 8;
                    for (unsigned int v = 0; v < shown; ++v) {
                        printf(" [%u]=%.6f", key.mValues[v], key.mWeights[v]);
                    }
                    if (shown < key.mNumValuesAndWeights) printf(" ...");
                    printf("\n");
                }
            }
        }
        
        // 计算所有节点持有的 mesh 引用总数
        printf("\nNode tree:\n");
        printNodeTree(scene->mRootNode, 0, scene);
        
        // 统计
        printf("\nSummary:\n");
        printf("  Total meshes in scene: %u\n", scene->mNumMeshes);
        
        // 直接遍历 scene->mMeshes[] 获取总顶点数
        unsigned int totalVerticesFromScene = 0;
        unsigned int totalFacesFromScene = 0;
        for (unsigned int mi = 0; mi < scene->mNumMeshes; mi++) {
            totalVerticesFromScene += scene->mMeshes[mi]->mNumVertices;
            totalFacesFromScene += scene->mMeshes[mi]->mNumFaces;
        }
        printf("  Total vertices (from scene->mMeshes[]): %u\n", totalVerticesFromScene);
        printf("  Total faces (from scene->mMeshes[]): %u\n", totalFacesFromScene);
        
        importer.FreeScene();
    }
    return 0;
}
