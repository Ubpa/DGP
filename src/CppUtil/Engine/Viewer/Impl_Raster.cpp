#include "Impl_Raster.h"

#include <CppUtil/Engine/Scene.h>
#include <CppUtil/Engine/SObj.h>

#include <CppUtil/Engine/Geometry.h>
#include <CppUtil/Engine/Material.h>
#include <CppUtil/Engine/Transform.h>

#include <CppUtil/Engine/Sphere.h>
#include <CppUtil/Engine/Plane.h>
#include <CppUtil/Engine/TriMesh.h>
#include <CppUtil/Engine/AllBSDFs.h>

#include <CppUtil/Engine/Light.h>
#include <CppUtil/Engine/PointLight.h>

#include <CppUtil/Qt/RawAPI_OGLW.h>
#include <CppUtil/Qt/RawAPI_Define.h>

#include <CppUtil/OpenGL/VAO.h>
#include <CppUtil/OpenGL/CommonDefine.h>
#include <CppUtil/OpenGL/Texture.h>
#include <CppUtil/OpenGL/Shader.h>
#include <CppUtil/OpenGL/Camera.h>
#include <CppUtil/OpenGL/FBO.h>

#include <CppUtil/Basic/LambdaOp.h>
#include <CppUtil/Basic/Sphere.h>
#include <CppUtil/Basic/Plane.h>
#include <CppUtil/Basic/Image.h>

#include <ROOT_PATH.h>

using namespace CppUtil::Engine;
using namespace CppUtil::Qt;
using namespace CppUtil::OpenGL;
using namespace CppUtil::Basic;
using namespace CppUtil;
using namespace Define;
using namespace glm;
using namespace std;

const string rootPath = ROOT_PATH;

Impl_Raster::Impl_Raster(Scene::Ptr scene)
	: scene(scene) {
	Reg<SObj>();

	// primitive
	Reg<Engine::Sphere>();
	Reg<Engine::Plane>();
	Reg<TriMesh>();

	// bsdf
	Reg<BSDF_Diffuse>();
	Reg<BSDF_Emission>();
	Reg<BSDF_Glass>();
	Reg<BSDF_Mirror>();
	Reg<BSDF_CookTorrance>();
	Reg<BSDF_MetalWorkflow>();
}

void Impl_Raster::Init() {
	//------------- light UBO
	glGenBuffers(1, &lightsUBO);
	glBindBuffer(GL_UNIFORM_BUFFER, lightsUBO);
	glBufferData(GL_UNIFORM_BUFFER, 1552, NULL, GL_DYNAMIC_DRAW);
	glBindBufferBase(GL_UNIFORM_BUFFER, 1, lightsUBO);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);

	//------------ 模型 . P3_Sphere
	Basic::Sphere sphere(50);
	vector<VAO::VBO_DataPatch> P3_Sphere_Vec_VBO_Data_Patch = {
		{sphere.GetPosArr(), sphere.GetPosArrSize(), 3},
		{sphere.GetNormalArr(), sphere.GetNormalArrSize(), 3},
		{sphere.GetTexCoordsArr(), sphere.GetTexCoordsArrSize(), 2},
	};
	VAO_P3_Sphere = VAO(P3_Sphere_Vec_VBO_Data_Patch, sphere.GetIndexArr(), sphere.GetIndexArrSize());


	//------------ 模型 . P3_Plane
	Basic::Plane plane;
	vector<VAO::VBO_DataPatch> P3_Plane_Vec_VBO_Data_Patch = {
		{plane.GetPosArr(), plane.GetPosArrSize(), 3},
		{plane.GetNormalArr(), plane.GetNormalArrSize(), 3},
		{plane.GetTexCoordsArr(), plane.GetTexCoordsArrSize(), 2},
	};
	VAO_P3_Plane = VAO(P3_Plane_Vec_VBO_Data_Patch, plane.GetIndexArr(), plane.GetIndexArrSize());

	//------------ 着色器 . basic
	shader_basic = Shader(rootPath + str_Basic_P3_vs, rootPath + str_Basic_fs);
	shader_basic.UniformBlockBind("CameraMatrixs", 0);
	//shader_basic.UniformBlockBind("PointLights", 1);

	auto geos = scene->GetRoot()->GetComponentsInChildren<Geometry>();
	for (auto geo : geos) {
		auto mesh = TriMesh::Ptr::Cast(geo->GetPrimitive());
		if (mesh == nullptr)
			continue;

		vector<VAO::VBO_DataPatch> P3_Mesh_Vec_VBO_Data_Patch = {
			{ &(mesh->GetPositions()[0][0]), static_cast<uint>(mesh->GetPositions().size() * 3 * sizeof(float)), 3 },
		};

		VAO VAO_P3_Mesh(P3_Mesh_Vec_VBO_Data_Patch, mesh->GetIndice().data(), mesh->GetIndice().size() * sizeof(size_t));
		// 用 TriMesh::Ptr 做 ID
		meshVAOs[mesh] = VAO_P3_Mesh;
	}

	//------------- 着色器 Diffuse
	shader_diffuse = Shader(rootPath + str_Basic_P3N3T2_vs, rootPath + "data/shaders/Engine/BSDF_Diffuse.fs");
	shader_diffuse.UniformBlockBind("CameraMatrixs", 0);
	shader_diffuse.UniformBlockBind("PointLights", 1);
}

void Impl_Raster::Draw() {
	glEnable(GL_DEPTH_TEST);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	modelVec.clear();
	modelVec.push_back(mat4(1.0f));
	scene->GetRoot()->Accept(This());

	scene->Init();
	int numLights = 0;
	glBindBuffer(GL_UNIFORM_BUFFER, lightsUBO);
	for (auto lightComponent : scene->GetLights()) {
		auto pointLight = PointLight::Ptr::Cast(lightComponent->GetLight());
		if (!pointLight)
			continue;

		numLights++;
		vec3 position = lightComponent->GetSObj()->GetLocalToWorldMatrix()*vec4(0, 0, 0, 1);

		int base = 16 + 48 * (numLights-1);
		glBufferSubData(GL_UNIFORM_BUFFER, base, 12, glm::value_ptr(position));
		glBufferSubData(GL_UNIFORM_BUFFER, base + 16, 12, glm::value_ptr(pointLight->intensity * pointLight->color));
		glBufferSubData(GL_UNIFORM_BUFFER, base + 28, 4, &pointLight->linear);
		glBufferSubData(GL_UNIFORM_BUFFER, base + 32, 4, &pointLight->quadratic);
	}
	glBufferSubData(GL_UNIFORM_BUFFER, 0, 4, &numLights);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

void Impl_Raster::Draw(SObj::Ptr sobj) {
	auto geometry = sobj->GetComponent<Geometry>();
	auto children = sobj->GetChildren();
	// 这种情况下不需要 transform
	if (geometry == nullptr && children.size() == 0)
		return;

	auto transform = sobj->GetComponent<Transform>();
	if (transform != nullptr)
		modelVec.push_back(modelVec.back() * transform->GetMat());

	if (geometry) {
		auto primitive = geometry->GetPrimitive();
		if (primitive) {
			auto material = sobj->GetComponent<Material>();
			if (material && material->GetMat())
				material->GetMat()->Accept(This());
			else {
				curShader = shader_basic;
				shader_basic.SetVec3f("color", vec3(1, 0, 1));
			}

			primitive->Accept(This());
		}
	}

	for (auto child : children)
		child->Accept(This());

	if (transform != nullptr)
		modelVec.pop_back();
}

void Impl_Raster::Draw(Engine::Sphere::Ptr sphere) {
	mat4 model = modelVec.back();
	model = translate(model, sphere->center);
	model = scale(model, vec3(sphere->r));

	curShader.SetMat4f("model", model);
	
	VAO_P3_Sphere.Draw(curShader);
}

void Impl_Raster::Draw(Engine::Plane::Ptr plane) {
	curShader.SetMat4f("model", modelVec.back());
	VAO_P3_Plane.Draw(curShader);
}

void Impl_Raster::Draw(TriMesh::Ptr mesh) {
	curShader.SetMat4f("model", modelVec.back());

	auto target = meshVAOs.find(mesh);
	if (target == meshVAOs.end()) {
		vector<VAO::VBO_DataPatch> P3_Mesh_Vec_VBO_Data_Patch = {
				{ &(mesh->GetPositions()[0][0]), static_cast<uint>(mesh->GetPositions().size() * 3 * sizeof(float)), 3 },
		};

		VAO VAO_P3_Mesh(P3_Mesh_Vec_VBO_Data_Patch, mesh->GetIndice().data(), mesh->GetIndice().size() * sizeof(size_t));
		// 用 TriMesh::Ptr 做 ID
		meshVAOs[mesh] = VAO_P3_Mesh;
	}

	meshVAOs[mesh].Draw(curShader);
}

void Impl_Raster::Draw(BSDF_Diffuse::Ptr bsdf) {
	curShader = shader_diffuse;
	string strBSDF = "bsdf.";
	shader_diffuse.SetVec3f(strBSDF + "albedoColor", bsdf->albedoColor);
	if (bsdf->albedoTexture && bsdf->albedoTexture->IsValid()) {
		shader_diffuse.SetBool(strBSDF + "haveAlbedoTexture", true);
		shader_diffuse.SetInt(strBSDF + "salbedoTexture", 0);
		GetTex(bsdf->albedoTexture).Use(0);
	}else
		shader_diffuse.SetBool(strBSDF + "haveAlbedoTexture", false);
}

void Impl_Raster::Draw(BSDF_Glass::Ptr bsdf) {
	curShader = shader_basic;
	shader_basic.SetVec3f("color", bsdf->transmittance);
}

void Impl_Raster::Draw(BSDF_Mirror::Ptr bsdf) {
	curShader = shader_basic;
	shader_basic.SetVec3f("color", bsdf->reflectance);
}

void Impl_Raster::Draw(BSDF_Emission::Ptr bsdf) {
	curShader = shader_basic;
	shader_basic.SetVec3f("color", bsdf->GetEmission());
}

void Impl_Raster::Draw(BSDF_CookTorrance::Ptr bsdf) {
	curShader = shader_basic;
	shader_basic.SetVec3f("color", bsdf->refletance);
}

void Impl_Raster::Draw(BSDF_MetalWorkflow::Ptr bsdf) {
	curShader = shader_basic;
	shader_basic.SetVec3f("color", bsdf->albedoColor);
}

Texture Impl_Raster::GetTex(Image::CPtr img) {
	auto target = img2tex.find(img);
	if (target != img2tex.end())
		return target->second;

	auto tex = Texture(img);
	img2tex[img] = tex;
	return tex;
}
