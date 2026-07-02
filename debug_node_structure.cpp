// 临时调试文件 - 打印节点树结构
#include <assimp/scene.h>
#include <assimp/Importer.hpp>
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
                printf("    -> Mesh[%u]: %u vertices, %u faces\n", 
                       mi, m->mNumVertices, m->mNumFaces);
            }
        }
    }
    
    // 递归子节点
    for (unsigned int i = 0; i < node->mNumChildren; i++) {
        printNodeTree(node->mChildren[i], depth + 1, scene);
    }
}

int main() {
    const char* paths[] = {
        "D:/Projects/AliyatRenderer/assets/core/models/sour-miku-Creamy/Sour.fbx"
    };
    
    for (const char* path : paths) {
        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(path, aiProcess_Triangulate | aiProcess_JoinIdenticalVertices);
        if (!scene) {
            printf("Failed to load: %s\n", path);
            continue;
        }
        
        printf("=== %s ===\n", path);
        printf("scene->mNumMeshes = %u\n", scene->mNumMeshes);
        printf("scene->mRootNode->mNumMeshes = %u\n", scene->mRootNode->mNumMeshes);
        printf("scene->mRootNode->mNumChildren = %u\n", scene->mRootNode->mNumChildren);
        
        // 计算所有节点持有的 mesh 引用总数
        unsigned int totalMeshRefs = 0;
        
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
