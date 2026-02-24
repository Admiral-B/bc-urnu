// Ported from WickedEngine Editor/ModelImporter_GLTF.cpp
// Provides glTF/GLB model loading into WickedEngine scenes
#include "wiScene.h"
#include "wiRandom.h"
#include <wiHelper.h>
#include <wiUnorderedSet.h>
#include <wiBacklog.h>

#include "../../libs/nlohmann/json.hpp"

#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_NO_INCLUDE_JSON
#include "../../libs/tinygltf/tiny_gltf.h"

#ifndef _WIN32
#  include <wordexp.h>
#endif

using namespace wi::graphics;
using namespace wi::scene;
using namespace wi::ecs;
using json = nlohmann::json;

namespace wi::tinygltf
{

	using namespace ::tinygltf;

	bool FileExists(const std::string& abs_filename, void*) {
		return wi::helper::FileExists(abs_filename);
	}

	std::string ExpandFilePath(const std::string& filepath, void*) {
#ifdef _WIN32
		DWORD len = ExpandEnvironmentStringsA(filepath.c_str(), NULL, 0);
		char* str = new char[len];
		ExpandEnvironmentStringsA(filepath.c_str(), str, len);

		std::string s(str);

		delete[] str;

		return s;
#else

#if defined(TARGET_OS_IPHONE) || defined(TARGET_IPHONE_SIMULATOR) || \
	defined(__ANDROID__) || defined(__EMSCRIPTEN__)
		// no expansion
		std::string s = filepath;
#else
		std::string s;
		wordexp_t p;

		if (filepath.empty()) {
			return "";
		}

		// char** w;
		int ret = wordexp(filepath.c_str(), &p, 0);
		if (ret) {
			// err
			s = filepath;
			return s;
		}

		// Use first element only.
		if (p.we_wordv) {
			s = std::string(p.we_wordv[0]);
			wordfree(&p);
		}
		else {
			s = filepath;
		}

#endif

		return s;
#endif
	}

	bool ReadWholeFile(std::vector<unsigned char>* out, std::string* err,
		const std::string& filepath, void*) {
		return wi::helper::FileRead(filepath, *out);
	}

	bool WriteWholeFile(std::string* err, const std::string& filepath,
		const std::vector<unsigned char>& contents, void*) {
		return wi::helper::FileWrite(filepath, contents.data(), contents.size());
	}

	bool GetFileSizeInBytes(size_t* filesize_out, std::string* err,
							const std::string& filepath, void* userdata)
	{
		*filesize_out = wi::helper::FileSize(filepath);
		return true;
	}

	bool LoadImageData(Image *image, const int image_idx, std::string *err,
		std::string *warn, int req_width, int req_height,
		const unsigned char *bytes, int size, void *userdata)
	{
		(void)warn;

		if (image->uri.empty())
		{
			// Force some image resource name:
			image->uri = "gltfimport_" + std::to_string(wi::helper::HashByteData(bytes, size)) + ".png";
		}

		auto resource = wi::resourcemanager::Load(
			image->uri,
			wi::resourcemanager::Flags::IMPORT_RETAIN_FILEDATA | wi::resourcemanager::Flags::IMPORT_DELAY,
			(const uint8_t*)bytes,
			(size_t)size
		);

		if (!resource.IsValid())
		{
			return false;
		}

		wi::resourcemanager::ResourceSerializer* seri = (wi::resourcemanager::ResourceSerializer*)userdata;
		seri->resources.push_back(resource);

		return true;
	}

	bool WriteImageData(
		const std::string* basepath, const std::string* filename,
		const Image* image, bool embedImages, const FsCallbacks*, const URICallbacks*,
		std::string* out_uri, void*)
	{
		assert(0); // TODO
		return false;
	}
}



struct LoaderState
{
	std::string name;
	tinygltf::Model gltfModel;
	Scene* scene;
	wi::unordered_map<int, Entity> entityMap;  // node -> entity
	std::unordered_multimap<int, Entity> punctualLightMap;  // KHR_lights_punctual -> entities
	Entity rootEntity = INVALID_ENTITY;

	//Export states
	wi::unordered_map<std::string, int> textureMap; // path -> textureid
	wi::unordered_map<Entity, int> nodeMap;// entity -> node
	wi::unordered_map<size_t, TransformComponent> transforms_original; // original transform states
};

// VRM/Mixamo extensions not needed for Bridge Command
static void Import_Extension_VRM(LoaderState&) {}
static void Import_Extension_VRMC(LoaderState&) {}
static void VRM_ToonMaterialCustomize(const std::string&, MaterialComponent&) {}
static void Import_VRMC_Bone(LoaderState&, Entity, const std::string&) {}
static void Import_Mixamo_Bone(LoaderState&, Entity, const tinygltf::Node&) {}
static void Import_Makehuman_Bone(LoaderState&, Entity, const tinygltf::Node&) {}

static void ImportMetadata(LoaderState& state, Entity entity, const tinygltf::Value& extras)
{
	if (extras.IsObject())
	{
		MetadataComponent& metadata = state.scene->metadatas.Create(entity);

		for (auto& key : extras.Keys())
		{
			auto& value = extras.Get(key);
			switch (value.Type())
			{
			case tinygltf::Type::BOOL_TYPE:
				metadata.bool_values.set(key, value.Get<bool>());
				break;
			case tinygltf::Type::INT_TYPE:
				metadata.int_values.set(key, value.Get<int>());
				break;
			case tinygltf::Type::REAL_TYPE:
				metadata.float_values.set(key, float(value.Get<double>()));
				break;
			case tinygltf::Type::STRING_TYPE:
				metadata.string_values.set(key, value.Get<std::string>());
				break;
			default:
				json j;
				tinygltf::ValueToJson(value, &j);
				metadata.string_values.set(key, j.dump());
			}
		}
	}

}

// Recursively loads nodes and resolves hierarchy:
void LoadNode(int nodeIndex, Entity parent, LoaderState& state)
{
	if (nodeIndex < 0 || state.entityMap.count(nodeIndex) != 0)
	{
		return;
	}
	auto& node = state.gltfModel.nodes[nodeIndex];
	Scene& scene = *state.scene;
	Entity entity = INVALID_ENTITY;

	if(node.mesh >= 0)
	{
		assert(node.mesh < (int)scene.meshes.GetCount());

		if (node.skin >= 0)
		{
			// This node is an armature:
			entity = scene.armatures.GetEntity(node.skin);
			MeshComponent* mesh = &scene.meshes[node.mesh];
			Entity meshEntity = scene.meshes.GetEntity(node.mesh);
			assert(!mesh->vertex_boneindices.empty());
			if (mesh->armatureID != INVALID_ENTITY)
			{
				// Reuse mesh with different skin is not possible currently, so we create a new one:
				meshEntity = entity;
				MeshComponent& newMesh = scene.meshes.Create(meshEntity);
				newMesh = scene.meshes[node.mesh];
				newMesh.CreateRenderData();
				mesh = &newMesh;
			}
			mesh->armatureID = entity;

			// The object component will use an identity transform but will be parented to the armature:
			Entity objectEntity = scene.Entity_CreateObject(node.name);
			ObjectComponent& object = *scene.objects.GetComponent(objectEntity);
			object.meshID = meshEntity;
			scene.Component_Attach(objectEntity, entity, true);
		}
		else
		{
			// This node is a mesh instance:
			entity = scene.Entity_CreateObject(node.name);
			ObjectComponent& object = *scene.objects.GetComponent(entity);
			object.meshID = scene.meshes.GetEntity(node.mesh);
		}
	}
	else if (node.camera >= 0)
	{
		if (node.name.empty())
		{
			static int camID = 0;
			node.name = "cam" + std::to_string(camID++);
		}

		entity = scene.Entity_CreateCamera(node.name, 16, 9);
	}

	auto ext_lights_punctual = node.extensions.find("KHR_lights_punctual");
	if (ext_lights_punctual != node.extensions.end())
	{
		// https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_lights_punctual
		entity = scene.Entity_CreateLight(""); // light component will be filled later
		int index = ext_lights_punctual->second.Get("light").Get<int>();
		state.punctualLightMap.insert({ index, entity });
	}

	if (entity == INVALID_ENTITY)
	{
		entity = CreateEntity();
		scene.transforms.Create(entity);
		scene.names.Create(entity) = node.name;
	}

	state.entityMap[nodeIndex] = entity;

	TransformComponent& transform = *scene.transforms.GetComponent(entity);
	if (!node.scale.empty())
	{
		// Note: limiting min scale because scale <= 0.0001 will break matrix decompose and mess up the model (float precision issue?)
		for (int idx = 0; idx < 3; ++idx)
		{
			if (std::abs(node.scale[idx]) <= 0.0001)
			{
				const double sign = node.scale[idx] < 0 ? -1 : 1;
				node.scale[idx] = 0.0001001 * sign;
			}
		}
		transform.scale_local = XMFLOAT3(float(node.scale[0]), float(node.scale[1]), float(node.scale[2]));
	}
	if (!node.rotation.empty())
	{
		transform.rotation_local = XMFLOAT4((float)node.rotation[0], (float)node.rotation[1], (float)node.rotation[2], (float)node.rotation[3]);
	}
	if (!node.translation.empty())
	{
		transform.translation_local = XMFLOAT3((float)node.translation[0], (float)node.translation[1], (float)node.translation[2]);
	}
	if (!node.matrix.empty())
	{
		transform.world._11 = (float)node.matrix[0];
		transform.world._12 = (float)node.matrix[1];
		transform.world._13 = (float)node.matrix[2];
		transform.world._14 = (float)node.matrix[3];
		transform.world._21 = (float)node.matrix[4];
		transform.world._22 = (float)node.matrix[5];
		transform.world._23 = (float)node.matrix[6];
		transform.world._24 = (float)node.matrix[7];
		transform.world._31 = (float)node.matrix[8];
		transform.world._32 = (float)node.matrix[9];
		transform.world._33 = (float)node.matrix[10];
		transform.world._34 = (float)node.matrix[11];
		transform.world._41 = (float)node.matrix[12];
		transform.world._42 = (float)node.matrix[13];
		transform.world._43 = (float)node.matrix[14];
		transform.world._44 = (float)node.matrix[15];
		transform.ApplyTransform(); // this creates S, R, T vectors from world matrix
	}

	ImportMetadata(state, entity, node.extras);

	transform.UpdateTransform();

	if (parent != INVALID_ENTITY)
	{
		scene.Component_Attach(entity, parent, true);
	}

	if (!node.children.empty())
	{
		for (int child : node.children)
		{
			LoadNode(child, entity, state);
		}
	}
}

void FlipZAxis(LoaderState& state)
{
	Scene& wiscene = *state.scene;

	// Flip mesh data first
	for(size_t i = 0; i < wiscene.meshes.GetCount(); ++i)
	{
		auto& mesh = wiscene.meshes[i];
		for(auto& v_pos : mesh.vertex_positions)
		{
			v_pos.z *= -1.f;
		}
		for(auto& v_norm : mesh.vertex_normals)
		{
			v_norm.z *= -1.f;
		}
		for(auto& v_tan : mesh.vertex_tangents)
		{
			v_tan.z *= -1.f;
		}
		for(auto& v_morph : mesh.morph_targets)
		{
			for(auto& v_morph_norm : v_morph.vertex_normals)
			{
				v_morph_norm.z *= -1.f;
			}
			for(auto& v_morph_pos : v_morph.vertex_positions)
			{
				v_morph_pos.z *= -1.f;
			}
		}
		mesh.FlipCulling(); // calls CreateRenderData
	}

	// Flip scene's transformComponents
	bool state_restore = (state.transforms_original.size() > 0);
	if(!state_restore)
	{
		wi::unordered_map<wi::ecs::Entity, wi::ecs::Entity> hierarchy_list;
		wi::unordered_map<size_t, wi::scene::TransformComponent> correction_queue;

		for(size_t i = 0; i < wiscene.transforms.GetCount(); ++i)
		{
			auto transformEntity = wiscene.transforms.GetEntity(i);
			if(transformEntity == state.rootEntity)
				continue;

			correction_queue[i] = wiscene.transforms[i];

			auto hierarchy = wiscene.hierarchy.GetComponent(transformEntity);
			if(transformEntity != state.rootEntity && hierarchy != nullptr)
			{
				hierarchy_list[transformEntity] = hierarchy->parentID;
				wiscene.Component_Detach(transformEntity);
			}
		}
		state.transforms_original.insert(correction_queue.begin(), correction_queue.end());
		for(auto& correction_pair : correction_queue)
		{
			auto& transform = wiscene.transforms[correction_pair.first];
			auto& transform_original = correction_pair.second;

			XMVECTOR V_S,V_R,V_T;
			XMMatrixDecompose(&V_S, &V_R, &V_T, XMLoadFloat4x4(&transform_original.world));
			XMFLOAT3 pos, scale;
			XMFLOAT4 rot;
			XMStoreFloat3(&scale, V_S);
			XMStoreFloat3(&pos, V_T);
			XMStoreFloat4(&rot, V_R);
			pos.z *= -1.f;
			rot.x *= -1.f;
			rot.y *= -1.f;

			auto build_m = 
				XMMatrixScalingFromVector(XMLoadFloat3(&scale)) *
				XMMatrixRotationQuaternion(XMLoadFloat4(&rot)) *
				XMMatrixTranslationFromVector(XMLoadFloat3(&pos));

			XMFLOAT4X4 build_m4;
			XMStoreFloat4x4(&build_m4, build_m);

			transform.world = build_m4;
			transform.ApplyTransform();
		}
		for(auto& hierarchy_pair : hierarchy_list)
		{
			wiscene.Component_Attach(hierarchy_pair.first, hierarchy_pair.second);
		}
	}
	else
	{
		for(size_t i = 0; i < wiscene.transforms.GetCount(); ++i)
		{
			auto transform_original_find = state.transforms_original.find(i);
			if(transform_original_find != state.transforms_original.end())
			{
				auto& transform = wiscene.transforms[i];
				transform = transform_original_find->second;
			}
		}
		state.transforms_original.clear();
	}

	// Flip armature's bind pose
	for(size_t i = 0; i < wiscene.armatures.GetCount(); ++i)
	{
		auto& armature = wiscene.armatures[i];
		for(int i = 0; i < armature.inverseBindMatrices.size(); ++i)
		{
			auto& bind = armature.inverseBindMatrices[i];
			
			XMVECTOR V_S,V_R,V_T;
			XMMatrixDecompose(&V_S, &V_R, &V_T, XMLoadFloat4x4(&bind));
			XMFLOAT3 pos, scale;
			XMFLOAT4 rot;
			XMStoreFloat3(&scale, V_S);
			XMStoreFloat3(&pos, V_T);
			XMStoreFloat4(&rot, V_R);
			pos.z *= -1.f;
			rot.x *= -1.f;
			rot.y *= -1.f;

			auto build_m = 
				XMMatrixScalingFromVector(XMLoadFloat3(&scale)) *
				XMMatrixRotationQuaternion(XMLoadFloat4(&rot)) *
				XMMatrixTranslationFromVector(XMLoadFloat3(&pos));
			XMFLOAT4X4 build_m4;
			XMStoreFloat4x4(&build_m4, build_m);

			bind = build_m4;
		}
	}

	// Flip animation data for translation and rotation
	for(size_t i = 0; i < wiscene.animations.GetCount(); ++i)
	{
		auto& animation = wiscene.animations[i];
		
		for(auto& channel : animation.channels)
		{
			auto data = wiscene.animation_datas.GetComponent(animation.samplers[channel.samplerIndex].data);
			
			if(channel.path == wi::scene::AnimationComponent::AnimationChannel::Path::TRANSLATION)
			{
				for(size_t k = 0; k < data->keyframe_data.size()/3; ++k)
				{
					data->keyframe_data[k*3+2] *= -1.f;
				}
			}
			if(channel.path == wi::scene::AnimationComponent::AnimationChannel::Path::ROTATION)
			{
				for(size_t k = 0; k < data->keyframe_data.size()/4; ++k)
				{
					data->keyframe_data[k*4] *= -1.f;
					data->keyframe_data[k*4+1] *= -1.f;
				}
			}
		}
	}
}

static const wi::unordered_set<std::string> SUPPORTED_EXTENSIONS = {
	"EXT_lights_image_based",
	"KHR_lights_punctual",
	"KHR_materials_anisotropy",
	"KHR_materials_clearcoat",
	"KHR_materials_emissive_strength",
	"KHR_materials_ior",
	"KHR_materials_pbrSpecularGlossiness",
	"KHR_materials_sheen",
	"KHR_materials_specular",
	"KHR_materials_transmission",
	"KHR_materials_unlit",
	"VRM",
	"VRMC_materials_mtoon",
	"VRMC_springBone",
	"VRMC_springBone_extended_collider",
	"VRMC_vrm",
	"VRMC_vrm_animation",
};

Entity ImportModel_GLTF(const std::string& fileName, Scene& targetScene)
{
	// Import into a temporary scene first, then merge.
	// FlipZAxis operates on ALL meshes/transforms in the scene, so importing
	// directly into a populated scene would corrupt existing entities (terrain, camera, etc.)
	Scene scene;

	std::string directory = wi::helper::GetDirectoryFromPath(fileName);
	std::string name = wi::helper::GetFileNameFromPath(fileName);
	std::string extension = wi::helper::toUpper(wi::helper::GetExtensionFromFileName(name));

	tinygltf::TinyGLTF loader;
	std::string err;
	std::string warn;

	tinygltf::FsCallbacks callbacks;
	callbacks.ReadWholeFile = wi::tinygltf::ReadWholeFile;
	callbacks.WriteWholeFile = wi::tinygltf::WriteWholeFile;
	callbacks.FileExists = wi::tinygltf::FileExists;
	callbacks.GetFileSizeInBytes = wi::tinygltf::GetFileSizeInBytes;
	callbacks.ExpandFilePath = wi::tinygltf::ExpandFilePath;

	bool ret = loader.SetFsCallbacks(callbacks);
	assert(ret);

	wi::resourcemanager::ResourceSerializer seri; // keep this alive to not delete loaded images while importing gltf
	loader.SetImageLoader(wi::tinygltf::LoadImageData, &seri);
	loader.SetImageWriter(wi::tinygltf::WriteImageData, nullptr);

	LoaderState state;
	state.scene = &scene;

	wi::vector<uint8_t> filedata;
	ret = wi::helper::FileRead(fileName, filedata);

	if (ret)
	{
		std::string basedir = tinygltf::GetBaseDir(fileName);

		if (!extension.compare("GLTF"))
		{
			ret = loader.LoadASCIIFromString(
				&state.gltfModel,
				&err,
				&warn, 
				reinterpret_cast<const char*>(filedata.data()),
				static_cast<unsigned int>(filedata.size()),
				basedir
			);
		}
		else
		{
			ret = loader.LoadBinaryFromMemory(
				&state.gltfModel,
				&err,
				&warn,
				filedata.data(),
				static_cast<unsigned int>(filedata.size()),
				basedir
			);
		}
	}
	else
	{
		err = "Failed to read file: " + fileName;
	}

	if (!ret)
	{
		wi::helper::messageBox(err, "glTF error!");
		return INVALID_ENTITY;
	}

	for (auto& ext : state.gltfModel.extensionsRequired)
	{
		if (SUPPORTED_EXTENSIONS.find(ext) == SUPPORTED_EXTENSIONS.end())
		{
			wi::backlog::post("The glTF file " + fileName + " requires the unsupported extension " + ext + ". Trying to import it anyway, some objects might be missing!", wi::backlog::LogLevel::Warning);
		}
	}

	state.rootEntity = CreateEntity();
	scene.transforms.Create(state.rootEntity);
	scene.names.Create(state.rootEntity) = name;
	state.name = name;

	// Create materials:
	for (auto& x : state.gltfModel.materials)
	{
		Entity materialEntity = scene.Entity_CreateMaterial(x.name);
		scene.Component_Attach(materialEntity, state.rootEntity);

		MaterialComponent& material = *scene.materials.GetComponent(materialEntity);

		material.baseColor = XMFLOAT4(1, 1, 1, 1);
		material.roughness = 1.0f;
		material.metalness = 1.0f;
		material.reflectance = 0.04f;

		material.SetDoubleSided(x.doubleSided);

		// metallic-roughness workflow:
		auto baseColorTexture = x.values.find("baseColorTexture");
		auto metallicRoughnessTexture = x.values.find("metallicRoughnessTexture");
		auto baseColorFactor = x.values.find("baseColorFactor");
		auto roughnessFactor = x.values.find("roughnessFactor");
		auto metallicFactor = x.values.find("metallicFactor");

		// common workflow:
		auto normalTexture = x.additionalValues.find("normalTexture");
		auto emissiveTexture = x.additionalValues.find("emissiveTexture");
		auto occlusionTexture = x.additionalValues.find("occlusionTexture");
		auto emissiveFactor = x.additionalValues.find("emissiveFactor");
		auto alphaCutoff = x.additionalValues.find("alphaCutoff");
		auto alphaMode = x.additionalValues.find("alphaMode");

		if (baseColorTexture != x.values.end())
		{
			auto& tex = state.gltfModel.textures[baseColorTexture->second.TextureIndex()];
			int img_source = tex.source;
			auto& img = state.gltfModel.images[img_source];
			material.textures[MaterialComponent::BASECOLORMAP].name = img.uri;
			material.textures[MaterialComponent::BASECOLORMAP].uvset = baseColorTexture->second.TextureTexCoord();
		}
		if (normalTexture != x.additionalValues.end())
		{
			auto& tex = state.gltfModel.textures[normalTexture->second.TextureIndex()];
			int img_source = tex.source;
			auto& img = state.gltfModel.images[img_source];
			material.textures[MaterialComponent::NORMALMAP].name = img.uri;
			material.textures[MaterialComponent::NORMALMAP].uvset = normalTexture->second.TextureTexCoord();
		}
		if (metallicRoughnessTexture != x.values.end())
		{
			auto& tex = state.gltfModel.textures[metallicRoughnessTexture->second.TextureIndex()];
			int img_source = tex.source;
			auto& img = state.gltfModel.images[img_source];
			material.textures[MaterialComponent::SURFACEMAP].name = img.uri;
			material.textures[MaterialComponent::SURFACEMAP].uvset = metallicRoughnessTexture->second.TextureTexCoord();
		}
		if (emissiveTexture != x.additionalValues.end())
		{
			auto& tex = state.gltfModel.textures[emissiveTexture->second.TextureIndex()];
			int img_source = tex.source;
			auto& img = state.gltfModel.images[img_source];
			material.textures[MaterialComponent::EMISSIVEMAP].name = img.uri;
			material.textures[MaterialComponent::EMISSIVEMAP].uvset = emissiveTexture->second.TextureTexCoord();
		}
		if (occlusionTexture != x.additionalValues.end())
		{
			auto& tex = state.gltfModel.textures[occlusionTexture->second.TextureIndex()];
			int img_source = tex.source;
			auto& img = state.gltfModel.images[img_source];
			material.textures[MaterialComponent::OCCLUSIONMAP].name = img.uri;
			material.textures[MaterialComponent::OCCLUSIONMAP].uvset = occlusionTexture->second.TextureTexCoord();
			material.SetOcclusionEnabled_Secondary(true);
		}

		if (baseColorFactor != x.values.end())
		{
			material.baseColor.x = float(baseColorFactor->second.ColorFactor()[0]);
			material.baseColor.y = float(baseColorFactor->second.ColorFactor()[1]);
			material.baseColor.z = float(baseColorFactor->second.ColorFactor()[2]);
			material.baseColor.w = float(baseColorFactor->second.ColorFactor()[3]);
		}
		if (roughnessFactor != x.values.end())
		{
			material.roughness = float(roughnessFactor->second.Factor());
		}
		if (metallicFactor != x.values.end())
		{
			material.metalness = float(metallicFactor->second.Factor());
		}
		if (emissiveFactor != x.additionalValues.end())
		{
			material.emissiveColor.x = float(emissiveFactor->second.ColorFactor()[0]);
			material.emissiveColor.y = float(emissiveFactor->second.ColorFactor()[1]);
			material.emissiveColor.z = float(emissiveFactor->second.ColorFactor()[2]);
			material.emissiveColor.w = float(emissiveFactor->second.ColorFactor()[3]);
		}
		if (alphaMode != x.additionalValues.end())
		{
			if (alphaMode->second.string_value.compare("BLEND") == 0)
			{
				material.userBlendMode = wi::enums::BLENDMODE_ALPHA;
			}
			if (alphaMode->second.string_value.compare("MASK") == 0)
			{
				material.alphaRef = 0.5f;
			}
		}
		if (alphaCutoff != x.additionalValues.end())
		{
			material.alphaRef = 1 - float(alphaCutoff->second.Factor());
		}

		auto ext_unlit = x.extensions.find("KHR_materials_unlit");
		if (ext_unlit != x.extensions.end())
		{
			// https://github.com/KhronosGroup/glTF/tree/master/extensions/2.0/Khronos/KHR_materials_unlit

			material.shaderType = MaterialComponent::SHADERTYPE_UNLIT;
		}

		auto ext_mtoon = x.extensions.find("VRMC_materials_mtoon");
		if (ext_mtoon != x.extensions.end())
		{
			// https://github.com/vrm-c/vrm-specification/tree/master/specification/VRMC_materials_mtoon-1.0
			VRM_ToonMaterialCustomize(x.name, material);
		}

		auto ext_emissiveStrength = x.extensions.find("KHR_materials_emissive_strength");
		if (ext_emissiveStrength != x.extensions.end())
		{
			// https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_materials_emissive_strength/README.md
			if (ext_emissiveStrength->second.Has("emissiveStrength"))
			{
				auto& factor = ext_emissiveStrength->second.Get("emissiveStrength");
				material.SetEmissiveStrength(float(factor.IsNumber() ? factor.Get<double>() : factor.Get<int>()));
			}
		}

		auto ext_transmission = x.extensions.find("KHR_materials_transmission");
		if (ext_transmission != x.extensions.end())
		{
			// https://github.com/KhronosGroup/glTF/tree/master/extensions/2.0/Khronos/KHR_materials_transmission

			if (ext_transmission->second.Has("transmissionFactor"))
			{
				auto& factor = ext_transmission->second.Get("transmissionFactor");
				material.transmission = float(factor.IsNumber() ? factor.Get<double>() : factor.Get<int>());
			}
			if (ext_transmission->second.Has("transmissionTexture"))
			{
				int index = ext_transmission->second.Get("transmissionTexture").Get("index").Get<int>();
				auto& tex = state.gltfModel.textures[index];
				int img_source = tex.source;
				auto& img = state.gltfModel.images[img_source];
				material.textures[MaterialComponent::TRANSMISSIONMAP].name = img.uri;
				material.textures[MaterialComponent::TRANSMISSIONMAP].uvset = (uint32_t)ext_transmission->second.Get("transmissionTexture").Get("texCoord").Get<int>();
			}
		}

		// specular-glossiness workflow:
		auto specularGlossinessWorkflow = x.extensions.find("KHR_materials_pbrSpecularGlossiness");
		if (specularGlossinessWorkflow != x.extensions.end())
		{
			// https://github.com/KhronosGroup/glTF/tree/master/extensions/2.0/Khronos/KHR_materials_pbrSpecularGlossiness

			material.SetUseSpecularGlossinessWorkflow(true);

			if (specularGlossinessWorkflow->second.Has("diffuseTexture"))
			{
				int index = specularGlossinessWorkflow->second.Get("diffuseTexture").Get("index").Get<int>();
				auto& tex = state.gltfModel.textures[index];
				int img_source = tex.source;
				auto& img = state.gltfModel.images[img_source];
				material.textures[MaterialComponent::BASECOLORMAP].name = img.uri;
				material.textures[MaterialComponent::BASECOLORMAP].uvset = (uint32_t)specularGlossinessWorkflow->second.Get("diffuseTexture").Get("texCoord").Get<int>();
			}
			if (specularGlossinessWorkflow->second.Has("specularGlossinessTexture"))
			{
				int index = specularGlossinessWorkflow->second.Get("specularGlossinessTexture").Get("index").Get<int>();
				auto& tex = state.gltfModel.textures[index];
				int img_source = tex.source;
				auto& img = state.gltfModel.images[img_source];
				material.textures[MaterialComponent::SURFACEMAP].name = img.uri;
				material.textures[MaterialComponent::SURFACEMAP].uvset = (uint32_t)specularGlossinessWorkflow->second.Get("specularGlossinessTexture").Get("texCoord").Get<int>();
			}

			if (specularGlossinessWorkflow->second.Has("diffuseFactor"))
			{
				auto& factor = specularGlossinessWorkflow->second.Get("diffuseFactor");
				material.baseColor.x = factor.ArrayLen() > 0 ? float(factor.Get(0).IsNumber() ? factor.Get(0).Get<double>() : factor.Get(0).Get<int>()) : 1.0f;
				material.baseColor.y = factor.ArrayLen() > 1 ? float(factor.Get(1).IsNumber() ? factor.Get(1).Get<double>() : factor.Get(1).Get<int>()) : 1.0f;
				material.baseColor.z = factor.ArrayLen() > 2 ? float(factor.Get(2).IsNumber() ? factor.Get(2).Get<double>() : factor.Get(2).Get<int>()) : 1.0f;
				material.baseColor.w = factor.ArrayLen() > 3 ? float(factor.Get(3).IsNumber() ? factor.Get(3).Get<double>() : factor.Get(3).Get<int>()) : 1.0f;
			}
			if (specularGlossinessWorkflow->second.Has("specularFactor"))
			{
				auto& factor = specularGlossinessWorkflow->second.Get("specularFactor");
				material.specularColor.x = factor.ArrayLen() > 0 ? float(factor.Get(0).IsNumber() ? factor.Get(0).Get<double>() : factor.Get(0).Get<int>()) : 1.0f;
				material.specularColor.y = factor.ArrayLen() > 0 ? float(factor.Get(1).IsNumber() ? factor.Get(1).Get<double>() : factor.Get(1).Get<int>()) : 1.0f;
				material.specularColor.z = factor.ArrayLen() > 0 ? float(factor.Get(2).IsNumber() ? factor.Get(2).Get<double>() : factor.Get(2).Get<int>()) : 1.0f;
				material.specularColor.w = factor.ArrayLen() > 0 ? float(factor.Get(3).IsNumber() ? factor.Get(3).Get<double>() : factor.Get(3).Get<int>()) : 1.0f;
			}
			if (specularGlossinessWorkflow->second.Has("glossinessFactor"))
			{
				auto& factor = specularGlossinessWorkflow->second.Get("glossinessFactor");
				material.roughness = 1 - float(factor.IsNumber() ? factor.Get<double>() : factor.Get<int>());
			}
		}

		auto ext_sheen = x.extensions.find("KHR_materials_sheen");
		if (ext_sheen != x.extensions.end())
		{
			// https://github.com/KhronosGroup/glTF/tree/master/extensions/2.0/Khronos/KHR_materials_sheen

			material.shaderType = MaterialComponent::SHADERTYPE_PBR_CLOTH;

			if (ext_sheen->second.Has("sheenColorFactor"))
			{
				auto& factor = ext_sheen->second.Get("sheenColorFactor");
				material.sheenColor.x = factor.ArrayLen() > 0 ? float(factor.Get(0).IsNumber() ? factor.Get(0).Get<double>() : factor.Get(0).Get<int>()) : 1.0f;
				material.sheenColor.y = factor.ArrayLen() > 0 ? float(factor.Get(1).IsNumber() ? factor.Get(1).Get<double>() : factor.Get(1).Get<int>()) : 1.0f;
				material.sheenColor.z = factor.ArrayLen() > 0 ? float(factor.Get(2).IsNumber() ? factor.Get(2).Get<double>() : factor.Get(2).Get<int>()) : 1.0f;
				material.sheenColor.w = factor.ArrayLen() > 0 ? float(factor.Get(3).IsNumber() ? factor.Get(3).Get<double>() : factor.Get(3).Get<int>()) : 1.0f;
			}
			if (ext_sheen->second.Has("sheenColorTexture"))
			{
				auto& param = ext_sheen->second.Get("sheenColorTexture");
				int index = param.Get("index").Get<int>();
				auto& tex = state.gltfModel.textures[index];
				int img_source = tex.source;
				auto& img = state.gltfModel.images[img_source];
				material.textures[MaterialComponent::SHEENCOLORMAP].name = img.uri;
				material.textures[MaterialComponent::SHEENCOLORMAP].uvset = (uint32_t)param.Get("texCoord").Get<int>();
			}
			if (ext_sheen->second.Has("sheenRoughnessFactor"))
			{
				auto& factor = ext_sheen->second.Get("sheenRoughnessFactor");
				material.sheenRoughness = float(factor.IsNumber() ? factor.Get<double>() : factor.Get<int>());
			}
			if (ext_sheen->second.Has("sheenRoughnessTexture"))
			{
				auto& param = ext_sheen->second.Get("sheenRoughnessTexture");
				int index = param.Get("index").Get<int>();
				auto& tex = state.gltfModel.textures[index];
				int img_source = tex.source;
				auto& img = state.gltfModel.images[img_source];
				material.textures[MaterialComponent::SHEENROUGHNESSMAP].name = img.uri;
				material.textures[MaterialComponent::SHEENROUGHNESSMAP].uvset = (uint32_t)param.Get("texCoord").Get<int>();
			}
		}

		auto ext_clearcoat = x.extensions.find("KHR_materials_clearcoat");
		if (ext_clearcoat != x.extensions.end())
		{
			// https://github.com/KhronosGroup/glTF/tree/master/extensions/2.0/Khronos/KHR_materials_clearcoat

			if (material.shaderType == MaterialComponent::SHADERTYPE_PBR_CLOTH)
			{
				material.shaderType = MaterialComponent::SHADERTYPE_PBR_CLOTH_CLEARCOAT;
			}
			else
			{
				material.shaderType = MaterialComponent::SHADERTYPE_PBR_CLEARCOAT;
			}

			if (ext_clearcoat->second.Has("clearcoatFactor"))
			{
				auto& factor = ext_clearcoat->second.Get("clearcoatFactor");
				material.clearcoat = float(factor.IsNumber() ? factor.Get<double>() : factor.Get<int>());
			}
			if (ext_clearcoat->second.Has("clearcoatTexture"))
			{
				auto& param = ext_clearcoat->second.Get("clearcoatTexture");
				int index = param.Get("index").Get<int>();
				auto& tex = state.gltfModel.textures[index];
				int img_source = tex.source;
				auto& img = state.gltfModel.images[img_source];
				material.textures[MaterialComponent::CLEARCOATMAP].name = img.uri;
				material.textures[MaterialComponent::CLEARCOATMAP].uvset = (uint32_t)param.Get("texCoord").Get<int>();
			}
			if (ext_clearcoat->second.Has("clearcoatRoughnessFactor"))
			{
				auto& factor = ext_clearcoat->second.Get("clearcoatRoughnessFactor");
				material.clearcoatRoughness = float(factor.IsNumber() ? factor.Get<double>() : factor.Get<int>());
			}
			if (ext_clearcoat->second.Has("clearcoatRoughnessTexture"))
			{
				auto& param = ext_clearcoat->second.Get("clearcoatRoughnessTexture");
				int index = param.Get("index").Get<int>();
				auto& tex = state.gltfModel.textures[index];
				int img_source = tex.source;
				auto& img = state.gltfModel.images[img_source];
				material.textures[MaterialComponent::CLEARCOATROUGHNESSMAP].name = img.uri;
				material.textures[MaterialComponent::CLEARCOATROUGHNESSMAP].uvset = (uint32_t)param.Get("texCoord").Get<int>();
			}
			if (ext_clearcoat->second.Has("clearcoatNormalTexture"))
			{
				auto& param = ext_clearcoat->second.Get("clearcoatNormalTexture");
				int index = param.Get("index").Get<int>();
				auto& tex = state.gltfModel.textures[index];
				int img_source = tex.source;
				auto& img = state.gltfModel.images[img_source];
				material.textures[MaterialComponent::CLEARCOATNORMALMAP].name = img.uri;
				material.textures[MaterialComponent::CLEARCOATNORMALMAP].uvset = (uint32_t)param.Get("texCoord").Get<int>();
			}
		}

		auto ext_ior = x.extensions.find("KHR_materials_ior");
		if (ext_ior != x.extensions.end())
		{
			// https://github.com/KhronosGroup/glTF/tree/master/extensions/2.0/Khronos/KHR_materials_ior

			if (ext_ior->second.Has("ior"))
			{
				auto& factor = ext_ior->second.Get("ior");
				float ior = float(factor.IsNumber() ? factor.Get<double>() : factor.Get<int>());

				material.reflectance = std::pow((ior - 1.0f) / (ior + 1.0f), 2.0f);
			}
		}

		auto ext_specular = x.extensions.find("KHR_materials_specular");
		if (ext_specular != x.extensions.end())
		{
			// https://github.com/KhronosGroup/glTF/tree/master/extensions/2.0/Khronos/KHR_materials_specular

			material.specularColor = XMFLOAT4(1, 1, 1, 1);

			if (ext_specular->second.Has("specularFactor"))
			{
				auto& factor = ext_specular->second.Get("specularFactor");
				material.specularColor.w = float(factor.IsNumber() ? factor.Get<double>() : factor.Get<int>());
			}
			if (ext_specular->second.Has("specularTexture"))
			{
				if (!material.textures[MaterialComponent::SURFACEMAP].resource.IsValid())
				{
					auto& param = ext_specular->second.Get("specularTexture");
					int index = param.Get("index").Get<int>();
					auto& tex = state.gltfModel.textures[index];
					int img_source = tex.source;
					auto& img = state.gltfModel.images[img_source];
					material.textures[MaterialComponent::SURFACEMAP].name = img.uri;
					material.textures[MaterialComponent::SURFACEMAP].uvset = (uint32_t)param.Get("texCoord").Get<int>();
				}
				else if (!material.textures[MaterialComponent::SPECULARMAP].resource.IsValid())
				{
					auto& param = ext_specular->second.Get("specularTexture");
					int index = param.Get("index").Get<int>();
					auto& tex = state.gltfModel.textures[index];
					int img_source = tex.source;
					auto& img = state.gltfModel.images[img_source];
					material.textures[MaterialComponent::SPECULARMAP].name = img.uri;
					material.textures[MaterialComponent::SPECULARMAP].uvset = (uint32_t)param.Get("texCoord").Get<int>();
				}
				else
				{
					wi::backlog::post("[KHR_materials_specular warning] specularTexture must be either in surfaceMap.a or specularColorTexture.a! specularTexture discarded!", wi::backlog::LogLevel::Warning);
				}
			}
			if (ext_specular->second.Has("specularColorTexture"))
			{
				auto& param = ext_specular->second.Get("specularColorTexture");
				int index = param.Get("index").Get<int>();
				auto& tex = state.gltfModel.textures[index];
				int img_source = tex.source;
				auto& img = state.gltfModel.images[img_source];
				material.textures[MaterialComponent::SPECULARMAP].name = img.uri;
				material.textures[MaterialComponent::SPECULARMAP].uvset = (uint32_t)param.Get("texCoord").Get<int>();
			}
			if (ext_specular->second.Has("specularColorFactor"))
			{
				auto& factor = ext_specular->second.Get("specularColorFactor");
				material.specularColor.x = factor.ArrayLen() > 0 ? float(factor.Get(0).IsNumber() ? factor.Get(0).Get<double>() : factor.Get(0).Get<int>()) : 1.0f;
				material.specularColor.y = factor.ArrayLen() > 0 ? float(factor.Get(1).IsNumber() ? factor.Get(1).Get<double>() : factor.Get(1).Get<int>()) : 1.0f;
				material.specularColor.z = factor.ArrayLen() > 0 ? float(factor.Get(2).IsNumber() ? factor.Get(2).Get<double>() : factor.Get(2).Get<int>()) : 1.0f;
			}
		}

		auto ext_aniso = x.extensions.find("KHR_materials_anisotropy");
		if (ext_aniso != x.extensions.end())
		{
			// https://github.com/ux3d/glTF/tree/extensions/KHR_materials_anisotropy/extensions/2.0/Khronos/KHR_materials_anisotropy

			material.shaderType = MaterialComponent::SHADERTYPE_PBR_ANISOTROPIC;

			if (ext_aniso->second.Has("anisotropyStrength"))
			{
				auto& factor = ext_aniso->second.Get("anisotropyStrength");
				material.anisotropy_strength = float(factor.IsNumber() ? factor.Get<double>() : factor.Get<int>());
			}
			if (ext_aniso->second.Has("anisotropyRotation"))
			{
				auto& factor = ext_aniso->second.Get("anisotropyRotation");
				material.anisotropy_rotation = float(factor.IsNumber() ? factor.Get<double>() : factor.Get<int>());
			}
			if (ext_aniso->second.Has("anisotropyTexture"))
			{
				auto& param = ext_aniso->second.Get("anisotropyTexture");
				int index = param.Get("index").Get<int>();
				auto& tex = state.gltfModel.textures[index];
				int img_source = tex.source;
				auto& img = state.gltfModel.images[img_source];
				material.textures[MaterialComponent::ANISOTROPYMAP].name = img.uri;
				material.textures[MaterialComponent::ANISOTROPYMAP].uvset = (uint32_t)param.Get("texCoord").Get<int>();
			}

			// Differently from the proposed spec, in the proposed sample model, I see different namings: https://github.com/KhronosGroup/glTF-Sample-Models/tree/Anisotropy-Barn-Lamp/2.0/AnisotropyBarnLamp
			if (ext_aniso->second.Has("anisotropy"))
			{
				auto& factor = ext_aniso->second.Get("anisotropy");
				material.anisotropy_strength = float(factor.IsNumber() ? factor.Get<double>() : factor.Get<int>());
			}
			if (ext_aniso->second.Has("anisotropyDirection"))
			{
				auto& factor = ext_aniso->second.Get("anisotropyDirection");
				material.anisotropy_rotation = float(factor.IsNumber() ? factor.Get<double>() : factor.Get<int>());
			}
			if (ext_aniso->second.Has("anisotropyDirectionTexture"))
			{
				auto& param = ext_aniso->second.Get("anisotropyDirectionTexture");
				int index = param.Get("index").Get<int>();
				auto& tex = state.gltfModel.textures[index];
				int img_source = tex.source;
				auto& img = state.gltfModel.images[img_source];
				material.textures[MaterialComponent::ANISOTROPYMAP].name = img.uri;
				material.textures[MaterialComponent::ANISOTROPYMAP].uvset = (uint32_t)param.Get("texCoord").Get<int>();
			}
		}
		ImportMetadata(state, materialEntity, x.extras);

		material.CreateRenderData();
	}

	// Create meshes:
	for (auto& x : state.gltfModel.meshes)
	{
		Entity meshEntity = scene.Entity_CreateMesh(x.name);
		scene.Component_Attach(meshEntity, state.rootEntity);
		MeshComponent& mesh = *scene.meshes.GetComponent(meshEntity);

		for (auto& prim : x.primitives)
		{
			mesh.subsets.push_back(MeshComponent::MeshSubset());
			if (scene.materials.GetCount() == 0)
			{
				// Create a material last minute if there was none
				scene.materials.Create(CreateEntity());
			}
			mesh.subsets.back().materialID = scene.materials.GetEntity(std::max(0, prim.material));
			MaterialComponent* material = scene.materials.GetComponent(mesh.subsets.back().materialID);
			uint32_t vertexOffset = (uint32_t)mesh.vertex_positions.size();

			const size_t index_remap[] = {
				0,2,1
			};

			if (prim.indices >= 0)
			{
				// Fill indices:
				const tinygltf::Accessor& accessor = state.gltfModel.accessors[prim.indices];
				const tinygltf::BufferView& bufferView = state.gltfModel.bufferViews[accessor.bufferView];
				const tinygltf::Buffer& buffer = state.gltfModel.buffers[bufferView.buffer];

				int stride = accessor.ByteStride(bufferView);
				size_t indexCount = align(accessor.count, size_t(3)); // there was a model with invalid index count, this is a safety fix for it
				size_t indexOffset = mesh.indices.size();
				mesh.indices.resize(indexOffset + indexCount);
				mesh.subsets.back().indexOffset = (uint32_t)indexOffset;
				mesh.subsets.back().indexCount = (uint32_t)indexCount;

				const uint8_t* data = buffer.data.data() + accessor.byteOffset + bufferView.byteOffset;

				if (stride == 1)
				{
					for (size_t i = 0; i < indexCount; i += 3)
					{
						mesh.indices[indexOffset + i + 0] = vertexOffset + data[i + index_remap[0]];
						mesh.indices[indexOffset + i + 1] = vertexOffset + data[i + index_remap[1]];
						mesh.indices[indexOffset + i + 2] = vertexOffset + data[i + index_remap[2]];
					}
				}
				else if (stride == 2)
				{
					for (size_t i = 0; i < indexCount; i += 3)
					{
						mesh.indices[indexOffset + i + 0] = vertexOffset + ((uint16_t*)data)[i + index_remap[0]];
						mesh.indices[indexOffset + i + 1] = vertexOffset + ((uint16_t*)data)[i + index_remap[1]];
						mesh.indices[indexOffset + i + 2] = vertexOffset + ((uint16_t*)data)[i + index_remap[2]];
					}
				}
				else if (stride == 4)
				{
					for (size_t i = 0; i < indexCount; i += 3)
					{
						mesh.indices[indexOffset + i + 0] = vertexOffset + ((uint32_t*)data)[i + index_remap[0]];
						mesh.indices[indexOffset + i + 1] = vertexOffset + ((uint32_t*)data)[i + index_remap[1]];
						mesh.indices[indexOffset + i + 2] = vertexOffset + ((uint32_t*)data)[i + index_remap[2]];
					}
				}
				else
				{
					assert(0 && "unsupported index stride!");
				}
			}

			for (auto& attr : prim.attributes)
			{
				const std::string& attr_name = attr.first;
				int attr_data = attr.second;

				const tinygltf::Accessor& accessor = state.gltfModel.accessors[attr_data];
				const tinygltf::BufferView& bufferView = state.gltfModel.bufferViews[accessor.bufferView];
				const tinygltf::Buffer& buffer = state.gltfModel.buffers[bufferView.buffer];

				int stride = accessor.ByteStride(bufferView);
				size_t vertexCount = accessor.count;

				if (mesh.subsets.back().indexCount == 0)
				{
					// Autogen indices:
					//	Note: this is not common, so it is simpler to create a dummy index buffer here than rewrite engine to support this case
					size_t indexOffset = mesh.indices.size();
					mesh.indices.resize(indexOffset + vertexCount);
					for (size_t vi = 0; vi < vertexCount; vi += 3)
					{
						mesh.indices[indexOffset + vi + 0] = uint32_t(vertexOffset + vi + index_remap[0]);
						mesh.indices[indexOffset + vi + 1] = uint32_t(vertexOffset + vi + index_remap[1]);
						mesh.indices[indexOffset + vi + 2] = uint32_t(vertexOffset + vi + index_remap[2]);
					}
					mesh.subsets.back().indexOffset = (uint32_t)indexOffset;
					mesh.subsets.back().indexCount = (uint32_t)vertexCount;
				}

				const uint8_t* data = buffer.data.data() + accessor.byteOffset + bufferView.byteOffset;

				if (!attr_name.compare("POSITION"))
				{
					mesh.vertex_positions.resize(vertexOffset + vertexCount);
					for (size_t i = 0; i < vertexCount; ++i)
					{
						mesh.vertex_positions[vertexOffset + i] = *(const XMFLOAT3*)(data + i * stride);
					}

					if (accessor.sparse.isSparse)
					{
						auto& sparse = accessor.sparse;
						const tinygltf::BufferView& sparse_indices_view = state.gltfModel.bufferViews[sparse.indices.bufferView];
						const tinygltf::BufferView& sparse_values_view = state.gltfModel.bufferViews[sparse.values.bufferView];
						const tinygltf::Buffer& sparse_indices_buffer = state.gltfModel.buffers[sparse_indices_view.buffer];
						const tinygltf::Buffer& sparse_values_buffer = state.gltfModel.buffers[sparse_values_view.buffer];
						const uint8_t* sparse_indices_data = sparse_indices_buffer.data.data() + sparse.indices.byteOffset + sparse_indices_view.byteOffset;
						const uint8_t* sparse_values_data = sparse_values_buffer.data.data() + sparse.values.byteOffset + sparse_values_view.byteOffset;
						switch (sparse.indices.componentType)
						{
						default:
						case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
							for (int s = 0; s < sparse.count; ++s)
							{
								mesh.vertex_positions[sparse_indices_data[s]] = ((const XMFLOAT3*)sparse_values_data)[s];
							}
							break;
						case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
							for (int s = 0; s < sparse.count; ++s)
							{
								mesh.vertex_positions[((const uint16_t*)sparse_indices_data)[s]] = ((const XMFLOAT3*)sparse_values_data)[s];
							}
							break;
						case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
							for (int s = 0; s < sparse.count; ++s)
							{
								mesh.vertex_positions[((const uint32_t*)sparse_indices_data)[s]] = ((const XMFLOAT3*)sparse_values_data)[s];
							}
							break;
						}
					}
				}
				else if (!attr_name.compare("NORMAL"))
				{
					mesh.vertex_normals.resize(vertexOffset + vertexCount);
					for (size_t i = 0; i < vertexCount; ++i)
					{
						mesh.vertex_normals[vertexOffset + i] = *(const XMFLOAT3*)(data + i * stride);
					}

					if (accessor.sparse.isSparse)
					{
						auto& sparse = accessor.sparse;
						const tinygltf::BufferView& sparse_indices_view = state.gltfModel.bufferViews[sparse.indices.bufferView];
						const tinygltf::BufferView& sparse_values_view = state.gltfModel.bufferViews[sparse.values.bufferView];
						const tinygltf::Buffer& sparse_indices_buffer = state.gltfModel.buffers[sparse_indices_view.buffer];
						const tinygltf::Buffer& sparse_values_buffer = state.gltfModel.buffers[sparse_values_view.buffer];
						const uint8_t* sparse_indices_data = sparse_indices_buffer.data.data() + sparse.indices.byteOffset + sparse_indices_view.byteOffset;
						const uint8_t* sparse_values_data = sparse_values_buffer.data.data() + sparse.values.byteOffset + sparse_values_view.byteOffset;
						switch (sparse.indices.componentType)
						{
						default:
						case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
							for (int s = 0; s < sparse.count; ++s)
							{
								mesh.vertex_normals[sparse_indices_data[s]] = ((const XMFLOAT3*)sparse_values_data)[s];
							}
							break;
						case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
							for (int s = 0; s < sparse.count; ++s)
							{
								mesh.vertex_normals[((const uint16_t*)sparse_indices_data)[s]] = ((const XMFLOAT3*)sparse_values_data)[s];
							}
							break;
						case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
							for (int s = 0; s < sparse.count; ++s)
							{
								mesh.vertex_normals[((const uint32_t*)sparse_indices_data)[s]] = ((const XMFLOAT3*)sparse_values_data)[s];
							}
							break;
						}
					}
				}
				else if (!attr_name.compare("TANGENT"))
				{
					mesh.vertex_tangents.resize(vertexOffset + vertexCount);
					for (size_t i = 0; i < vertexCount; ++i)
					{
						mesh.vertex_tangents[vertexOffset + i] = *(const XMFLOAT4*)(data + i * stride);
					}
				}
				else if (!attr_name.compare("TEXCOORD_0"))
				{
					mesh.vertex_uvset_0.resize(vertexOffset + vertexCount);
					if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT)
					{
						for (size_t i = 0; i < vertexCount; ++i)
						{
							const XMFLOAT2& tex = *(const XMFLOAT2*)((size_t)data + i * stride);

							mesh.vertex_uvset_0[vertexOffset + i].x = tex.x;
							mesh.vertex_uvset_0[vertexOffset + i].y = tex.y;
						}
					}
					else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
					{
						for (size_t i = 0; i < vertexCount; ++i)
						{
							const uint8_t& s = *(uint8_t*)((size_t)data + i * stride + 0);
							const uint8_t& t = *(uint8_t*)((size_t)data + i * stride + 1);

							mesh.vertex_uvset_0[vertexOffset + i].x = s / 255.0f;
							mesh.vertex_uvset_0[vertexOffset + i].y = t / 255.0f;
						}
					}
					else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
					{
						for (size_t i = 0; i < vertexCount; ++i)
						{
							const uint16_t& s = *(uint16_t*)((size_t)data + i * stride + 0 * sizeof(uint16_t));
							const uint16_t& t = *(uint16_t*)((size_t)data + i * stride + 1 * sizeof(uint16_t));

							mesh.vertex_uvset_0[vertexOffset + i].x = s / 65535.0f;
							mesh.vertex_uvset_0[vertexOffset + i].y = t / 65535.0f;
						}
					}
				}
				else if (!attr_name.compare("TEXCOORD_1"))
				{
					mesh.vertex_uvset_1.resize(vertexOffset + vertexCount);
					if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT)
					{
						for (size_t i = 0; i < vertexCount; ++i)
						{
							const XMFLOAT2& tex = *(const XMFLOAT2*)((size_t)data + i * stride);

							mesh.vertex_uvset_1[vertexOffset + i].x = tex.x;
							mesh.vertex_uvset_1[vertexOffset + i].y = tex.y;
						}
					}
					else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
					{
						for (size_t i = 0; i < vertexCount; ++i)
						{
							const uint8_t& s = *(uint8_t*)((size_t)data + i * stride + 0);
							const uint8_t& t = *(uint8_t*)((size_t)data + i * stride + 1);

							mesh.vertex_uvset_1[vertexOffset + i].x = s / 255.0f;
							mesh.vertex_uvset_1[vertexOffset + i].y = t / 255.0f;
						}
					}
					else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
					{
						for (size_t i = 0; i < vertexCount; ++i)
						{
							const uint16_t& s = *(uint16_t*)((size_t)data + i * stride + 0 * sizeof(uint16_t));
							const uint16_t& t = *(uint16_t*)((size_t)data + i * stride + 1 * sizeof(uint16_t));

							mesh.vertex_uvset_1[vertexOffset + i].x = s / 65535.0f;
							mesh.vertex_uvset_1[vertexOffset + i].y = t / 65535.0f;
						}
					}
				}
				else if (!attr_name.compare("JOINTS_0"))
				{
					mesh.vertex_boneindices.resize(vertexOffset + vertexCount);
					if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
					{
						struct JointTmp
						{
							uint8_t ind[4];
						};

						for (size_t i = 0; i < vertexCount; ++i)
						{
							const JointTmp& joint = *(const JointTmp*)(data + i * stride);

							mesh.vertex_boneindices[vertexOffset + i].x = joint.ind[0];
							mesh.vertex_boneindices[vertexOffset + i].y = joint.ind[1];
							mesh.vertex_boneindices[vertexOffset + i].z = joint.ind[2];
							mesh.vertex_boneindices[vertexOffset + i].w = joint.ind[3];
						}
					}
					else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
					{
						struct JointTmp
						{
							uint16_t ind[4];
						};

						for (size_t i = 0; i < vertexCount; ++i)
						{
							const JointTmp& joint = *(const JointTmp*)(data + i * stride);

							mesh.vertex_boneindices[vertexOffset + i].x = joint.ind[0];
							mesh.vertex_boneindices[vertexOffset + i].y = joint.ind[1];
							mesh.vertex_boneindices[vertexOffset + i].z = joint.ind[2];
							mesh.vertex_boneindices[vertexOffset + i].w = joint.ind[3];
						}
					}
					else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
					{
						struct JointTmp
						{
							uint32_t ind[4];
						};

						for (size_t i = 0; i < vertexCount; ++i)
						{
							const JointTmp& joint = *(const JointTmp*)(data + i * stride);

							mesh.vertex_boneindices[vertexOffset + i].x = joint.ind[0];
							mesh.vertex_boneindices[vertexOffset + i].y = joint.ind[1];
							mesh.vertex_boneindices[vertexOffset + i].z = joint.ind[2];
							mesh.vertex_boneindices[vertexOffset + i].w = joint.ind[3];
						}
					}
					else
					{
						assert(0);
					}
				}
				else if (!attr_name.compare("WEIGHTS_0"))
				{
					mesh.vertex_boneweights.resize(vertexOffset + vertexCount);
					if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT)
					{
						for (size_t i = 0; i < vertexCount; ++i)
						{
							mesh.vertex_boneweights[vertexOffset + i] = *(XMFLOAT4*)((size_t)data + i * stride);
						}
					}
					else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
					{
						for (size_t i = 0; i < vertexCount; ++i)
						{
							const uint8_t& x = *(uint8_t*)((size_t)data + i * stride + 0);
							const uint8_t& y = *(uint8_t*)((size_t)data + i * stride + 1);
							const uint8_t& z = *(uint8_t*)((size_t)data + i * stride + 2);
							const uint8_t& w = *(uint8_t*)((size_t)data + i * stride + 3);

							mesh.vertex_boneweights[vertexOffset + i].x = x / 255.0f;
							mesh.vertex_boneweights[vertexOffset + i].x = y / 255.0f;
							mesh.vertex_boneweights[vertexOffset + i].x = z / 255.0f;
							mesh.vertex_boneweights[vertexOffset + i].x = w / 255.0f;
						}
					}
					else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
					{
						for (size_t i = 0; i < vertexCount; ++i)
						{
							const uint16_t& x = *(uint8_t*)((size_t)data + i * stride + 0 * sizeof(uint16_t));
							const uint16_t& y = *(uint8_t*)((size_t)data + i * stride + 1 * sizeof(uint16_t));
							const uint16_t& z = *(uint8_t*)((size_t)data + i * stride + 2 * sizeof(uint16_t));
							const uint16_t& w = *(uint8_t*)((size_t)data + i * stride + 3 * sizeof(uint16_t));

							mesh.vertex_boneweights[vertexOffset + i].x = x / 65535.0f;
							mesh.vertex_boneweights[vertexOffset + i].x = y / 65535.0f;
							mesh.vertex_boneweights[vertexOffset + i].x = z / 65535.0f;
							mesh.vertex_boneweights[vertexOffset + i].x = w / 65535.0f;
						}
					}
				}
				else if (!attr_name.compare("COLOR_0"))
				{
					if(material != nullptr)
					{
						material->SetUseVertexColors(true);
					}
					mesh.vertex_colors.resize(vertexOffset + vertexCount);
					if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT)
					{
						if (accessor.type == TINYGLTF_TYPE_VEC3)
						{
							for (size_t i = 0; i < vertexCount; ++i)
							{
								const XMFLOAT3& color = *(XMFLOAT3*)((size_t)data + i * stride);
								uint32_t rgba = wi::math::CompressColor(color);

								mesh.vertex_colors[vertexOffset + i] = rgba;
							}
						}
						else if (accessor.type == TINYGLTF_TYPE_VEC4)
						{
							for (size_t i = 0; i < vertexCount; ++i)
							{
								const XMFLOAT4& color = *(XMFLOAT4*)((size_t)data + i * stride);
								uint32_t rgba = wi::math::CompressColor(color);

								mesh.vertex_colors[vertexOffset + i] = rgba;
							}
						}
					}
					else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
					{
						if (accessor.type == TINYGLTF_TYPE_VEC3)
						{
							for (size_t i = 0; i < vertexCount; ++i)
							{
								const uint8_t& r = *(uint8_t*)((size_t)data + i * stride + 0);
								const uint8_t& g = *(uint8_t*)((size_t)data + i * stride + 1);
								const uint8_t& b = *(uint8_t*)((size_t)data + i * stride + 2);
								const uint8_t a = 0xFF;
								wi::Color color = wi::Color(r, g, b, a);

								mesh.vertex_colors[vertexOffset + i] = color;
							}
						}
						else if (accessor.type == TINYGLTF_TYPE_VEC4)
						{
							for (size_t i = 0; i < vertexCount; ++i)
							{
								const uint8_t& r = *(uint8_t*)((size_t)data + i * stride + 0);
								const uint8_t& g = *(uint8_t*)((size_t)data + i * stride + 1);
								const uint8_t& b = *(uint8_t*)((size_t)data + i * stride + 2);
								const uint8_t& a = *(uint8_t*)((size_t)data + i * stride + 3);
								wi::Color color = wi::Color(r, g, b, a);

								mesh.vertex_colors[vertexOffset + i] = color;
							}
						}
					}
					else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
					{
						if (accessor.type == TINYGLTF_TYPE_VEC3)
						{
							for (size_t i = 0; i < vertexCount; ++i)
							{
								const uint16_t& r = *(uint16_t*)((size_t)data + i * stride + 0 * sizeof(uint16_t));
								const uint16_t& g = *(uint16_t*)((size_t)data + i * stride + 1 * sizeof(uint16_t));
								const uint16_t& b = *(uint16_t*)((size_t)data + i * stride + 2 * sizeof(uint16_t));
								uint32_t rgba = wi::math::CompressColor(XMFLOAT3(r / 65535.0f, g / 65535.0f, b / 65535.0f));

								mesh.vertex_colors[vertexOffset + i] = rgba;
							}
						}
						else if (accessor.type == TINYGLTF_TYPE_VEC4)
						{
							for (size_t i = 0; i < vertexCount; ++i)
							{
								const uint16_t& r = *(uint16_t*)((size_t)data + i * stride + 0 * sizeof(uint16_t));
								const uint16_t& g = *(uint16_t*)((size_t)data + i * stride + 1 * sizeof(uint16_t));
								const uint16_t& b = *(uint16_t*)((size_t)data + i * stride + 2 * sizeof(uint16_t));
								const uint16_t& a = *(uint16_t*)((size_t)data + i * stride + 3 * sizeof(uint16_t));
								uint32_t rgba = wi::math::CompressColor(XMFLOAT4(r / 65535.0f, g / 65535.0f, b / 65535.0f, a / 65535.0f));

								mesh.vertex_colors[vertexOffset + i] = rgba;
							}
						}
					}
				}
			}


			mesh.morph_targets.resize(prim.targets.size());
			for (size_t i = 0; i < prim.targets.size(); i++)
			{
				MeshComponent::MorphTarget& morph_target = mesh.morph_targets[i];
				for (auto& attr : prim.targets[i])
				{
					const std::string& attr_name = attr.first;
					int attr_data = attr.second;

					const tinygltf::Accessor& accessor = state.gltfModel.accessors[attr_data];

					if (!attr_name.compare("POSITION"))
					{
						if (accessor.sparse.isSparse)
						{
							auto& sparse = accessor.sparse;
							const tinygltf::BufferView& sparse_indices_view = state.gltfModel.bufferViews[sparse.indices.bufferView];
							const tinygltf::BufferView& sparse_values_view = state.gltfModel.bufferViews[sparse.values.bufferView];
							const tinygltf::Buffer& sparse_indices_buffer = state.gltfModel.buffers[sparse_indices_view.buffer];
							const tinygltf::Buffer& sparse_values_buffer = state.gltfModel.buffers[sparse_values_view.buffer];
							const uint8_t* sparse_indices_data = sparse_indices_buffer.data.data() + sparse.indices.byteOffset + sparse_indices_view.byteOffset;
							const uint8_t* sparse_values_data = sparse_values_buffer.data.data() + sparse.values.byteOffset + sparse_values_view.byteOffset;
							morph_target.vertex_positions.resize(sparse.count);
							morph_target.sparse_indices_positions.resize(sparse.count);

							switch (sparse.indices.componentType)
							{
							default:
							case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
								for (int s = 0; s < sparse.count; ++s)
								{
									morph_target.sparse_indices_positions[s] = vertexOffset + sparse_indices_data[s];
									morph_target.vertex_positions[s] = ((const XMFLOAT3*)sparse_values_data)[s];
								}
								break;
							case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
								for (int s = 0; s < sparse.count; ++s)
								{
									morph_target.sparse_indices_positions[s] = vertexOffset + ((const uint16_t*)sparse_indices_data)[s];
									morph_target.vertex_positions[s] = ((const XMFLOAT3*)sparse_values_data)[s];
								}
								break;
							case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
								for (int s = 0; s < sparse.count; ++s)
								{
									morph_target.sparse_indices_positions[s] = vertexOffset + ((const uint32_t*)sparse_indices_data)[s];
									morph_target.vertex_positions[s] = ((const XMFLOAT3*)sparse_values_data)[s];
								}
								break;
							}
						}
						else
						{
							const tinygltf::BufferView& bufferView = state.gltfModel.bufferViews[accessor.bufferView];
							const tinygltf::Buffer& buffer = state.gltfModel.buffers[bufferView.buffer];

							int stride = accessor.ByteStride(bufferView);
							size_t vertexCount = accessor.count;

							const unsigned char* data = buffer.data.data() + accessor.byteOffset + bufferView.byteOffset;

							morph_target.vertex_positions.resize(vertexOffset + vertexCount);
							for (size_t j = 0; j < vertexCount; ++j)
							{
								morph_target.vertex_positions[vertexOffset + j] = ((XMFLOAT3*)data)[j];
							}
						}
					}
					else if (!attr_name.compare("NORMAL"))
					{
						if (accessor.sparse.isSparse)
						{
							auto& sparse = accessor.sparse;
							const tinygltf::BufferView& sparse_indices_view = state.gltfModel.bufferViews[sparse.indices.bufferView];
							const tinygltf::BufferView& sparse_values_view = state.gltfModel.bufferViews[sparse.values.bufferView];
							const tinygltf::Buffer& sparse_indices_buffer = state.gltfModel.buffers[sparse_indices_view.buffer];
							const tinygltf::Buffer& sparse_values_buffer = state.gltfModel.buffers[sparse_values_view.buffer];
							const uint8_t* sparse_indices_data = sparse_indices_buffer.data.data() + sparse.indices.byteOffset + sparse_indices_view.byteOffset;
							const uint8_t* sparse_values_data = sparse_values_buffer.data.data() + sparse.values.byteOffset + sparse_values_view.byteOffset;
							morph_target.vertex_normals.resize(sparse.count);
							morph_target.sparse_indices_normals.resize(sparse.count);

							switch (sparse.indices.componentType)
							{
							default:
							case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
								for (int s = 0; s < sparse.count; ++s)
								{
									morph_target.sparse_indices_normals[s] = vertexOffset + sparse_indices_data[s];
									morph_target.vertex_normals[s] = ((const XMFLOAT3*)sparse_values_data)[s];
								}
								break;
							case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
								for (int s = 0; s < sparse.count; ++s)
								{
									morph_target.sparse_indices_normals[s] = vertexOffset + ((const uint16_t*)sparse_indices_data)[s];
									morph_target.vertex_normals[s] = ((const XMFLOAT3*)sparse_values_data)[s];
								}
								break;
							case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
								for (int s = 0; s < sparse.count; ++s)
								{
									morph_target.sparse_indices_normals[s] = vertexOffset + ((const uint32_t*)sparse_indices_data)[s];
									morph_target.vertex_normals[s] = ((const XMFLOAT3*)sparse_values_data)[s];
								}
								break;
							}
						}
						else
						{
							const tinygltf::BufferView& bufferView = state.gltfModel.bufferViews[accessor.bufferView];
							const tinygltf::Buffer& buffer = state.gltfModel.buffers[bufferView.buffer];

							int stride = accessor.ByteStride(bufferView);
							size_t vertexCount = accessor.count;

							const unsigned char* data = buffer.data.data() + accessor.byteOffset + bufferView.byteOffset;

							morph_target.vertex_normals.resize(vertexOffset + vertexCount);
							for (size_t j = 0; j < vertexCount; ++j)
							{
								morph_target.vertex_normals[vertexOffset + j] = ((XMFLOAT3*)data)[j];
							}
						}
					}
				}
			}
		}

		for (size_t i = 0; i < x.weights.size(); i++)
		{
			mesh.morph_targets[i].weight = static_cast<float_t>(x.weights[i]);
		}

		if (mesh.vertex_normals.empty())
		{
			mesh.vertex_normals.resize(mesh.vertex_positions.size());
			mesh.ComputeNormals(MeshComponent::COMPUTE_NORMALS_SMOOTH_FAST);
		}

		ImportMetadata(state, meshEntity, x.extras);
		mesh.CreateRenderData(); // tangents are generated inside if needed, which must be done before FlipZAxis!
	}

	// Create armatures:
	for (auto& skin : state.gltfModel.skins)
	{
		Entity armatureEntity = CreateEntity();
		scene.names.Create(armatureEntity) = skin.name;
		scene.layers.Create(armatureEntity);
		scene.transforms.Create(armatureEntity);
		scene.Component_Attach(armatureEntity, state.rootEntity);
		ArmatureComponent& armature = scene.armatures.Create(armatureEntity);

		if (skin.inverseBindMatrices >= 0)
		{
			const tinygltf::Accessor &accessor = state.gltfModel.accessors[skin.inverseBindMatrices];
			const tinygltf::BufferView &bufferView = state.gltfModel.bufferViews[accessor.bufferView];
			const tinygltf::Buffer &buffer = state.gltfModel.buffers[bufferView.buffer];
			armature.inverseBindMatrices.resize(accessor.count);
			memcpy(armature.inverseBindMatrices.data(), &buffer.data[accessor.byteOffset + bufferView.byteOffset], accessor.count * sizeof(XMFLOAT4X4));
		}
		else
		{
			assert(0);
		}
		ImportMetadata(state, armatureEntity, skin.extras);
	}

	// Create transform hierarchy, assign objects, meshes, armatures, cameras:
	const tinygltf::Scene &gltfScene = state.gltfModel.scenes[std::max(0, state.gltfModel.defaultScene)];
	for (size_t i = 0; i < gltfScene.nodes.size(); i++)
	{
		LoadNode(gltfScene.nodes[i], state.rootEntity, state);
	}

	// Create armature-bone mappings:
	int armatureIndex = 0;
	for (auto& skin : state.gltfModel.skins)
	{
		Entity armatureEntity = scene.armatures.GetEntity(armatureIndex);
		ArmatureComponent& armature = scene.armatures[armatureIndex++];

		const size_t jointCount = skin.joints.size();

		armature.boneCollection.resize(jointCount);

		// Create bone collection:
		for (size_t i = 0; i < jointCount; ++i)
		{
			int jointIndex = skin.joints[i];
			Entity boneEntity = state.entityMap[jointIndex];

			armature.boneCollection[i] = boneEntity;

			Import_Mixamo_Bone(state, boneEntity, state.gltfModel.nodes[jointIndex]);
			Import_Makehuman_Bone(state, boneEntity, state.gltfModel.nodes[jointIndex]);
		}
	}

	// Invalid humanoids will be removed, this is to not have humanoid if naming convention matched by mistake when trying to import humanoid bone:
	for (size_t i = 0; i < scene.humanoids.GetCount();)
	{
		if (!scene.humanoids[i].IsValid())
		{
			scene.humanoids.Remove(scene.humanoids.GetEntity(i));
		}
		else
		{
			i++;
		}
	}

	// Create animations:
	for (auto& anim : state.gltfModel.animations)
	{
		Entity entity = CreateEntity();
		scene.names.Create(entity) = anim.name;
		scene.Component_Attach(entity, state.rootEntity);
		AnimationComponent& animationcomponent = scene.animations.Create(entity);
		animationcomponent.samplers.resize(anim.samplers.size());
		animationcomponent.channels.resize(anim.channels.size());

		for (size_t i = 0; i < anim.samplers.size(); ++i)
		{
			auto& sam = anim.samplers[i];

			if (!sam.interpolation.compare("LINEAR"))
			{
				animationcomponent.samplers[i].mode = AnimationComponent::AnimationSampler::Mode::LINEAR;
			}
			else if (!sam.interpolation.compare("STEP"))
			{
				animationcomponent.samplers[i].mode = AnimationComponent::AnimationSampler::Mode::STEP;
			}
			else if (!sam.interpolation.compare("CUBICSPLINE"))
			{
				animationcomponent.samplers[i].mode = AnimationComponent::AnimationSampler::Mode::CUBICSPLINE;
			}

			animationcomponent.samplers[i].data = CreateEntity();
			scene.Component_Attach(animationcomponent.samplers[i].data, entity);
			AnimationDataComponent& animationdata = scene.animation_datas.Create(animationcomponent.samplers[i].data);

			// AnimationSampler input = keyframe times
			{
				const tinygltf::Accessor& accessor = state.gltfModel.accessors[sam.input];
				const tinygltf::BufferView& bufferView = state.gltfModel.bufferViews[accessor.bufferView];
				const tinygltf::Buffer& buffer = state.gltfModel.buffers[bufferView.buffer];

				assert(accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT);

				int stride = accessor.ByteStride(bufferView);
				size_t count = accessor.count;

				animationdata.keyframe_times.resize(count);

				const unsigned char* data = buffer.data.data() + accessor.byteOffset + bufferView.byteOffset;

				assert(stride == 4);

				for (size_t j = 0; j < count; ++j)
				{
					float time = ((float*)data)[j];
					animationdata.keyframe_times[j] = time;
					animationcomponent.start = std::min(animationcomponent.start, time);
					animationcomponent.end = std::max(animationcomponent.end, time);
				}

			}

			// AnimationSampler output = keyframe data
			{
				const tinygltf::Accessor& accessor = state.gltfModel.accessors[sam.output];
				const tinygltf::BufferView& bufferView = state.gltfModel.bufferViews[accessor.bufferView];
				const tinygltf::Buffer& buffer = state.gltfModel.buffers[bufferView.buffer];

				int stride = accessor.ByteStride(bufferView);
				size_t count = accessor.count;

				const unsigned char* data = buffer.data.data() + accessor.byteOffset + bufferView.byteOffset;

				switch (accessor.type)
				{
				case TINYGLTF_TYPE_SCALAR:
				{
					assert(stride == sizeof(float));
					animationdata.keyframe_data.resize(count);
					for (size_t j = 0; j < count; ++j)
					{
						animationdata.keyframe_data[j] = ((float*)data)[j];
					}
				}
				break;
				case TINYGLTF_TYPE_VEC3:
				{
					assert(stride == sizeof(XMFLOAT3));
					animationdata.keyframe_data.resize(count * 3);
					for (size_t j = 0; j < count; ++j)
					{
						((XMFLOAT3*)animationdata.keyframe_data.data())[j] = ((XMFLOAT3*)data)[j];
					}
				}
				break;
				case TINYGLTF_TYPE_VEC4:
				{
					assert(stride == sizeof(XMFLOAT4));
					animationdata.keyframe_data.resize(count * 4);
					for (size_t j = 0; j < count; ++j)
					{
						((XMFLOAT4*)animationdata.keyframe_data.data())[j] = ((XMFLOAT4*)data)[j];
					}
				}
				break;
				default: assert(0); break;

				}

			}

		}

		for (size_t i = 0; i < anim.channels.size(); ++i)
		{
			auto& channel = anim.channels[i];

			animationcomponent.channels[i].target = state.entityMap[channel.target_node];
			assert(channel.sampler >= 0);
			animationcomponent.channels[i].samplerIndex = (uint32_t)channel.sampler;

			if (!channel.target_path.compare("scale"))
			{
				animationcomponent.channels[i].path = AnimationComponent::AnimationChannel::Path::SCALE;
			}
			else if (!channel.target_path.compare("rotation"))
			{
				animationcomponent.channels[i].path = AnimationComponent::AnimationChannel::Path::ROTATION;
			}
			else if (!channel.target_path.compare("translation"))
			{
				animationcomponent.channels[i].path = AnimationComponent::AnimationChannel::Path::TRANSLATION;
			}
			else if (!channel.target_path.compare("weights"))
			{
				animationcomponent.channels[i].path = AnimationComponent::AnimationChannel::Path::WEIGHTS;
			}
			else
			{
				animationcomponent.channels[i].path = AnimationComponent::AnimationChannel::Path::UNKNOWN;
			}
		}
		ImportMetadata(state, entity, anim.extras);
	}

	// Create lights:
	int lightIndex = 0;
	for (auto& x : state.gltfModel.lights)
	{
		for (auto [it, end] = state.punctualLightMap.equal_range(lightIndex); it != end; it++)
		{
			Entity entity = it->second;
			LightComponent& light = scene.lights[lightIndex];
			NameComponent& name = *scene.names.GetComponent(entity);
			name = x.name;

			if (!x.type.compare("spot"))
			{
				light.type = LightComponent::LightType::SPOT;
			}
			if (!x.type.compare("point"))
			{
				light.type = LightComponent::LightType::POINT;
			}
			if (!x.type.compare("directional"))
			{
				light.type = LightComponent::LightType::DIRECTIONAL;
			}

			if (!x.color.empty())
			{
				light.color = XMFLOAT3(float(x.color[0]), float(x.color[1]), float(x.color[2]));
			}

			light.intensity = float(x.intensity);
			light.range = x.range > 0 ? float(x.range) : std::numeric_limits<float>::max();
			light.outerConeAngle = float(x.spot.outerConeAngle);
			light.innerConeAngle = float(x.spot.innerConeAngle);
			light.SetCastShadow(true);

			// In gltf, default light direction is forward, in engine, it's downwards, so apply a rotation:
			TransformComponent& transform = *scene.transforms.GetComponent(entity);
			transform.RotateRollPitchYaw(XMFLOAT3(XM_PIDIV2, 0, 0));

			ImportMetadata(state, entity, x.extras);
		}
		lightIndex++;
	}

	int cameraIndex = 0;
	for (auto& x : state.gltfModel.cameras)
	{
		if (!x.type.compare("orthographic"))
			continue;
		Entity entity = scene.cameras.GetEntity(cameraIndex);
		CameraComponent& camera = scene.cameras[cameraIndex++];

		if (x.perspective.aspectRatio > 0)
		{
			camera.width = float(x.perspective.aspectRatio);
			camera.height = 1.f;
		}
		camera.fov = (float)x.perspective.yfov;
		camera.zFarP = (float)x.perspective.zfar;
		camera.zNearP = (float)x.perspective.znear;

		ImportMetadata(state, entity, x.extras);
	}

	// EXT_lights_image_based disabled (requires dds.h and stb_image, not needed)
#if 0
	{
		int counter = 0;
		auto lights = env->second.Get("lights");
		for (int i = 0; i < (int)lights.ArrayLen(); ++i)
		{
			if (scene.weathers.GetCount() == 0)
			{
				Entity entity = CreateEntity();
				scene.weathers.Create(entity);
				scene.names.Create(entity) = "weather";
			}
			WeatherComponent& weather = scene.weathers[0];

			auto light = lights.Get(i);
			if (light.Has("intensity"))
			{
				auto value = light.Get("intensity");
				weather.skyExposure = (float)value.GetNumberAsDouble();
			}
			if (light.Has("rotation"))
			{
				auto value = light.Get("rotation");
				XMFLOAT4 quaternion = {};
				quaternion.x = value.ArrayLen() > 0 ? float(value.Get(0).IsNumber() ? value.Get(0).Get<double>() : value.Get(0).Get<int>()) : 0.0f;
				quaternion.y = value.ArrayLen() > 1 ? float(value.Get(1).IsNumber() ? value.Get(1).Get<double>() : value.Get(1).Get<int>()) : 0.0f;
				quaternion.z = value.ArrayLen() > 2 ? float(value.Get(2).IsNumber() ? value.Get(2).Get<double>() : value.Get(2).Get<int>()) : 0.0f;
				quaternion.w = value.ArrayLen() > 3 ? float(value.Get(3).IsNumber() ? value.Get(3).Get<double>() : value.Get(3).Get<int>()) : 1.0f;
				XMVECTOR Q = XMLoadFloat4(&quaternion);
				float angle;
				XMVECTOR axis;
				XMQuaternionToAxisAngle(&axis, &angle, Q);
				weather.sky_rotation = XM_2PI - angle;
			}
			//if (light.Has("irradianceCoefficients"))
			//{
			//	auto value = light.Get("irradianceCoefficients");
			//	float spherical_harmonics[9][3] = {};
			//	for (int c = 0; c < std::min(9, (int)value.ArrayLen()); ++c)
			//	{
			//		for (int f = 0; f < std::min(3, (int)value.Get(c).ArrayLen()); ++f)
			//		{
			//			spherical_harmonics[c][f] = (float)value.Get(c).Get(f).GetNumberAsDouble();
			//		}
			//	}
			//}
			if (light.Has("specularImages"))
			{
				auto mips = light.Get("specularImages");
				int mip_count = (int)mips.ArrayLen();

				TextureDesc desc;
				desc.format = Format::R9G9B9E5_SHAREDEXP;
				desc.bind_flags = BindFlag::SHADER_RESOURCE;
				if (light.Has("specularImageSize"))
				{
					auto value = light.Get("specularImageSize");
					desc.width = desc.height = (uint32_t)value.GetNumberAsInt();
				}
				desc.array_size = 6;
				desc.mip_levels = (uint32_t)mip_count;
				desc.misc_flags = ResourceMiscFlag::TEXTURECUBE;

				wi::vector<wi::vector<XMFLOAT3SE>> hdr_datas(mip_count * 6);

				for (int m = 0; m < mip_count; ++m)
				{
					auto mip = mips.Get(m);
					int face_count = (int)mip.ArrayLen();
					for (int f = 0; f < face_count; ++f)
					{
						auto index = mip.Get(f).GetNumberAsInt();
						auto& image = state.gltfModel.images[index];
						int idx = f * mip_count + m;
						wi::Resource res = wi::resourcemanager::Load(image.uri, wi::resourcemanager::Flags::IMPORT_RETAIN_FILEDATA);
						auto& imagefiledata = res.GetFileData();
						const stbi_uc* filedata = imagefiledata.data();
						size_t filesize = imagefiledata.size();
						int width, height, bpp;
						wi::Color* rgba = (wi::Color*)stbi_load_from_memory(filedata, (int)filesize, &width, &height, &bpp, 4);
						if (rgba == nullptr)
							continue;
						wi::vector<XMFLOAT3SE>& hdr_data = hdr_datas[idx];
						hdr_data.resize(width * height);
						for (int y = 0; y < height; ++y)
						{
							for (int x = 0; x < width; ++x)
							{
								int y_flip = height - 1 - y;
								wi::Color color = rgba[x + y_flip * width];
								XMFLOAT4 unpk = color.toFloat4();
								// Remove SRGB curve:
								unpk.x = std::pow(unpk.x, 2.2f);
								unpk.y = std::pow(unpk.y, 2.2f);
								unpk.z = std::pow(unpk.z, 2.2f);
								if (bpp == 4) // if has alpha channel, then it is assumed to have RGBD encoding
								{
									// RGBD conversion: https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Vendor/EXT_lights_image_based/README.md#rgbd
									unpk.x /= unpk.w;
									unpk.y /= unpk.w;
									unpk.z /= unpk.w;
								}
								hdr_data[x + y * width] = XMFLOAT3SE(unpk.x, unpk.y, unpk.z);
							}
						}
						stbi_image_free(rgba);
					}
				}

				size_t wholeDataSize = 0;
				for (auto& x : hdr_datas)
				{
					wholeDataSize += x.size() * sizeof(XMFLOAT3SE);
				}

				wi::vector<uint8_t> dds;
				dds.resize(sizeof(dds::Header) + wholeDataSize);
				dds::write_header(
					dds.data(),
					dds::DXGI_FORMAT_R9G9B9E5_SHAREDEXP,
					desc.width,
					desc.height,
					desc.mip_levels,
					desc.array_size,
					true
				);

				size_t offset = sizeof(dds::Header);
				for (auto& x : hdr_datas)
				{
					std::memcpy(dds.data() + offset, x.data(), x.size() * sizeof(XMFLOAT3SE));
					offset += x.size() * sizeof(XMFLOAT3SE);
				}
				
				weather.skyMapName = wi::helper::RemoveExtension(wi::helper::GetFileNameFromPath(fileName)) + "/EXT_lights_image_based_" + std::to_string(counter++) + ".dds";
				weather.skyMap = wi::resourcemanager::Load(
					weather.skyMapName,
					wi::resourcemanager::Flags::IMPORT_RETAIN_FILEDATA,
					dds.data(),
					dds.size()
				);
				weather.ambient = {}; // remove ambient if gltf has env lighting
			}
		}
	}
#endif // EXT_lights_image_based

	Import_Extension_VRM(state);
	Import_Extension_VRMC(state);

	//Correct orientation after importing (safe: only imported entities in temp scene are affected)
	scene.Update(0);
	FlipZAxis(state);

	// Update the temp scene before merging
	scene.Update(0);

	// Clean up duplicate colliders
	scene.DeleteDuplicateColliders();

	// Merge the fully-imported and Z-flipped model into the target scene.
	// Entity IDs are globally unique (from CreateEntity()) so they survive Merge unchanged.
	Entity rootResult = state.rootEntity;
	targetScene.Merge(scene);
	return rootResult;
}

