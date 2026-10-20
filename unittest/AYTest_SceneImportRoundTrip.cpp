#include <AYTest.h>
#include <AYResource/ImportJob.h>
#include <AYResource/Loader/MeshLoader.h>
#include <AYResource/Loader/SkeletonLoader.h>
#include <AYResource/Loader/AnimationLoader.h>
#include <AYResource/assetsImpl/Mesh.h>
#include <AYResource/assetsImpl/Skeleton.h>
#include <AYResource/assetsImpl/Animation.h>
#include <assimp/Exporter.hpp>
#include <assimp/scene.h>
#include <assimp/material.h>
#include <chrono>
#include <filesystem>
#include <string>

using namespace ayt::resource;
namespace {
namespace fs=std::filesystem;
struct RigFixture {
    fs::path root=fs::temp_directory_path()/("ay-rig-import-"+std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    ~RigFixture(){std::error_code error;fs::remove_all(root,error);}
};
}

TEST_SUITE(SceneImportRoundTripTests)
TEST_CASE(real_fbx_rig_preserves_skin_skeleton_animation_and_namespaced_dependencies)
{
    RigFixture fixture;
    fs::create_directories(fixture.root/"Source");
    Assimp::Exporter exporter;
    std::string format;
    for(std::size_t i=0;i<exporter.GetExportFormatCount();++i)
        if(std::string(exporter.GetExportFormatDescription(i)->id)=="fbxa")format="fbxa";
    if(format.empty())SKIP_TEST("This Assimp build has no FBX fixture exporter");
    aiScene source;
    source.mRootNode=new aiNode("SceneRoot");
    source.mNumMaterials=1;source.mMaterials=new aiMaterial*[1]{new aiMaterial()};
    aiString materialName("Rig surface");
    source.mMaterials[0]->AddProperty(&materialName,AI_MATKEY_NAME);
    const aiColor3D diffuse(.6f,.2f,.1f);
    source.mMaterials[0]->AddProperty(&diffuse,1,AI_MATKEY_COLOR_DIFFUSE);
    source.mNumMeshes=1;source.mMeshes=new aiMesh*[1]{new aiMesh()};
    auto* mesh=source.mMeshes[0];
    mesh->mName.Set("TriangleRig");
    mesh->mPrimitiveTypes=aiPrimitiveType_TRIANGLE;
    mesh->mNumVertices=3;
    mesh->mVertices=new aiVector3D[3]{{0,0,0},{1,0,0},{0,1,0}};
    mesh->mNormals=new aiVector3D[3]{{0,0,1},{0,0,1},{0,0,1}};
    mesh->mNumFaces=1;mesh->mFaces=new aiFace[1];
    mesh->mFaces[0].mNumIndices=3;mesh->mFaces[0].mIndices=new unsigned[3]{0,1,2};
    mesh->mNumBones=1;mesh->mBones=new aiBone*[1]{new aiBone()};
    mesh->mBones[0]->mName.Set("Bone");
    mesh->mBones[0]->mNumWeights=3;
    mesh->mBones[0]->mWeights=new aiVertexWeight[3]{{0,1},{1,1},{2,1}};
    auto* meshNode=new aiNode("RigMesh");
    meshNode->mNumMeshes=1;meshNode->mMeshes=new unsigned[1]{0};
    auto* boneNode=new aiNode("Bone");
    source.mRootNode->mNumChildren=2;
    source.mRootNode->mChildren=new aiNode*[2]{meshNode,boneNode};
    meshNode->mParent=boneNode->mParent=source.mRootNode;
    source.mNumAnimations=1;source.mAnimations=new aiAnimation*[1]{new aiAnimation()};
    auto* animation=source.mAnimations[0];
    animation->mName.Set("Move");
    animation->mDuration=1;animation->mTicksPerSecond=1;
    animation->mNumChannels=1;animation->mChannels=new aiNodeAnim*[1]{new aiNodeAnim()};
    auto* track=animation->mChannels[0];
    track->mNodeName.Set("Bone");
    track->mNumPositionKeys=2;
    track->mPositionKeys=new aiVectorKey[2]{{0,{0,0,0}},{1,{1,0,0}}};
    track->mNumRotationKeys=1;track->mRotationKeys=new aiQuatKey[1]{{0,aiQuaternion()}};
    track->mNumScalingKeys=1;track->mScalingKeys=new aiVectorKey[1]{{0,{1,1,1}}};
    const auto path=fixture.root/"Source/Rig.fbx";
    CHECK_MSG(exporter.Export(&source,format,path.string())==aiReturn_SUCCESS,exporter.GetErrorString());
    ImportOptions options;
    options.sourcePath=path.string();options.outputDir=(fixture.root/"Imported").string();
    options.sourceId="source-rig";options.outputNamespace="source-rig";
    const auto imported=importAsset(options);
    CHECK_MSG(imported.ok,imported.error.c_str());
    if(!imported.ok)return;
    std::string meshPath,skeletonPath,animationPath;
    for(const auto& resource:imported.conversion.resources){
        if(resource.type=="Mesh")meshPath=resource.path;
        if(resource.type=="Skeleton")skeletonPath=resource.path;
        if(resource.type=="Animation")animationPath=resource.path;
    }
    CHECK(meshPath.rfind("meshes/source-rig/",0)==0);
    CHECK(skeletonPath.rfind("skeletons/source-rig/",0)==0);
    CHECK(animationPath.rfind("animations/source-rig/",0)==0);
    MeshLoader meshLoader;SkeletonLoader skeletonLoader;AnimationLoader animationLoader;
    const auto loadedMesh=std::dynamic_pointer_cast<Mesh>(meshLoader.load((fixture.root/"Imported"/meshPath).string()));
    const auto skeleton=std::dynamic_pointer_cast<Skeleton>(skeletonLoader.load((fixture.root/"Imported"/skeletonPath).string()));
    const auto clip=std::dynamic_pointer_cast<Animation>(animationLoader.load((fixture.root/"Imported"/animationPath).string()));
    CHECK(loadedMesh && loadedMesh->hasSkinWeights());
    CHECK(skeleton && skeleton->getBoneCount()>0);
    CHECK(clip && clip->getTrackCount()>0);
    bool meshSkeleton=false,skeletonAnimation=false;
    for(const auto& dependency:imported.conversion.dependencies){
        meshSkeleton=meshSkeleton||(dependency.from==meshPath && dependency.to==skeletonPath);
        skeletonAnimation=skeletonAnimation||(dependency.from==skeletonPath && dependency.to==animationPath);
    }
    CHECK(meshSkeleton);CHECK(skeletonAnimation);
    CHECK(importAsset(options).usedCache);
}
TEST_SUITE_END
