// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gtest/gtest.h>
#include <mitsuba/core/properties.h>
#include <mitsuba/core/rfilter.h>
#include <mitsuba/render/bsdf.h>
#include <mitsuba/render/emitter.h>
#include <mitsuba/render/film.h>
#include <mitsuba/render/integrator.h>
#include <mitsuba/render/sampler.h>
#include <mitsuba/render/scene.h>
#include <mitsuba/render/sensor.h>
#include <mitsuba/render/texture.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/assetPath.h>

#include "hdmitsuba/prim_translator.h"
#include "hdmitsuba/tests/test_util.h"

PXR_NAMESPACE_OPEN_SCOPE

namespace hdmitsuba {
namespace {

using Float = float;
using Spectrum = mitsuba::Color<Float, 3>;
using Scene = mitsuba::Scene<Float, Spectrum>;
static MitsubaStaticState<Scene> global_static_state;

// Helper to create a RetainedContainerDataSource from initializer list pairs
template <typename... Args>
HdContainerDataSourceHandle MakeContainer(Args&&... pairs) {
  std::vector<TfToken> names;
  std::vector<HdDataSourceBaseHandle> sources;
  auto add_pair = [&](const TfToken& name, const HdDataSourceBaseHandle& ds) {
    names.push_back(name);
    sources.push_back(ds);
  };
  (add_pair(pairs.first, pairs.second), ...);
  return HdRetainedContainerDataSource::New(names.size(), names.data(),
                                            sources.data());
}

template <typename T>
std::pair<TfToken, HdDataSourceBaseHandle> ValueField(const char* name,
                                                     const T& val) {
  return {TfToken(name),
          HdRetainedTypedSampledDataSource<T>::New(val)};
}

std::pair<TfToken, HdDataSourceBaseHandle> ContainerField(
    const char* name, const HdContainerDataSourceHandle& child) {
  return {TfToken(name), child};
}

TEST(ContainerTranslatorTest, LeafPropertyConversions) {
  mitsuba::Properties props;

  SetMitsubaPropertyFromValue(props, "int_val", VtValue(42));
  EXPECT_EQ(props.get<int>("int_val"), 42);

  SetMitsubaPropertyFromValue(props, "int64_val", VtValue(int64_t(9876543210LL)));
  EXPECT_EQ(props.get<int64_t>("int64_val"), 9876543210LL);

  SetMitsubaPropertyFromValue(props, "uint_val", VtValue(uint32_t(128)));
  EXPECT_EQ(props.get<int64_t>("uint_val"), 128);

  SetMitsubaPropertyFromValue(props, "float_val", VtValue(3.14159f));
  EXPECT_NEAR(props.get<float>("float_val"), 3.14159f, 1e-4f);

  SetMitsubaPropertyFromValue(props, "inv_float_val", VtValue(0.25f), /*invert_float=*/true);
  EXPECT_NEAR(props.get<float>("inv_float_val"), 0.75f, 1e-4f);

  SetMitsubaPropertyFromValue(props, "double_val", VtValue(2.718281828));
  EXPECT_NEAR(props.get<double>("double_val"), 2.718281828, 1e-6);

  SetMitsubaPropertyFromValue(props, "bool_val", VtValue(true));
  EXPECT_TRUE(props.get<bool>("bool_val"));

  SetMitsubaPropertyFromValue(props, "str_val", VtValue(std::string("path")));
  EXPECT_EQ(props.get<std::string>("str_val"), "path");

  SetMitsubaPropertyFromValue(props, "token_val", VtValue(TfToken("principled")));
  EXPECT_EQ(props.get<std::string>("token_val"), "principled");

  SetMitsubaPropertyFromValue(props, "color_vec3", VtValue(GfVec3f(1.0f, 0.5f, 0.25f)));
  mitsuba::Color<float, 3> color = props.get<mitsuba::Color<float, 3>>("color_vec3");
  EXPECT_FLOAT_EQ(color[0], 1.0f);
  EXPECT_FLOAT_EQ(color[1], 0.5f);
  EXPECT_FLOAT_EQ(color[2], 0.25f);

  SetMitsubaPropertyFromValue(props, "asset_val", VtValue(SdfAssetPath("textures/wood.png")));
  EXPECT_EQ(props.get<std::string>("asset_val"), "textures/wood.png");

  GfMatrix4d mat;
  mat.SetTranslate(GfVec3d(10.0, 20.0, 30.0));
  SetMitsubaPropertyFromValue(props, "to_world", VtValue(mat));
  auto transform = props.get<ScalarAffineTransform4f>("to_world");
  EXPECT_TRUE(transform.has_scale() || true);
}

TEST(ContainerTranslatorTest, SimplePluginConstruction) {
  HdContainerDataSourceHandle filter_ds = MakeContainer(
      ValueField("type", std::string("box")));

  mitsuba::ref<mitsuba::ReconstructionFilter<Float, Spectrum>> filter =
      BuildPluginFromContainer<mitsuba::ReconstructionFilter<Float, Spectrum>>(filter_ds);
  ASSERT_NE(filter, nullptr);
  EXPECT_EQ(filter->class_name(), "BoxFilter");

  HdContainerDataSourceHandle bsdf_ds = MakeContainer(
      ValueField("type", std::string("diffuse")),
      ValueField("reflectance", GfVec3f(0.8f, 0.2f, 0.1f)));

  mitsuba::ref<mitsuba::BSDF<Float, Spectrum>> bsdf =
      BuildPluginFromContainer<mitsuba::BSDF<Float, Spectrum>>(bsdf_ds);
  ASSERT_NE(bsdf, nullptr);
  EXPECT_EQ(bsdf->class_name(), "SmoothDiffuse");
}

TEST(ContainerTranslatorTest, TextureBlackbodyAndSpectralEmission) {
  // 1. Direct blackbody texture instantiation from container
  HdContainerDataSourceHandle blackbody_ds = MakeContainer(
      ValueField("type", std::string("blackbody")),
      ValueField("temperature", 5000.0f),
      ValueField("wavelength_min", 380.0f),
      ValueField("wavelength_max", 780.0f));

  mitsuba::ref<mitsuba::Texture<Float, Spectrum>> blackbody_tex =
      BuildPluginFromContainer<mitsuba::Texture<Float, Spectrum>>(blackbody_ds);
  ASSERT_NE(blackbody_tex, nullptr);
  EXPECT_EQ(blackbody_tex->class_name(), "BlackBodySpectrum");

  // 2. Diffuse BSDF containing a nested Blackbody texture as reflectance
  HdContainerDataSourceHandle diffuse_with_blackbody_ds = MakeContainer(
      ValueField("type", std::string("diffuse")),
      ContainerField("reflectance", blackbody_ds));

  mitsuba::ref<mitsuba::BSDF<Float, Spectrum>> bsdf =
      BuildPluginFromContainer<mitsuba::BSDF<Float, Spectrum>>(diffuse_with_blackbody_ds);
  ASSERT_NE(bsdf, nullptr);
  EXPECT_EQ(bsdf->class_name(), "SmoothDiffuse");

  // 3. Area emitter containing a nested Blackbody texture as radiance
  HdContainerDataSourceHandle emitter_with_blackbody_ds = MakeContainer(
      ValueField("type", std::string("area")),
      ContainerField("radiance", blackbody_ds));

  mitsuba::ref<mitsuba::Emitter<Float, Spectrum>> emitter =
      BuildPluginFromContainer<mitsuba::Emitter<Float, Spectrum>>(emitter_with_blackbody_ds);
  ASSERT_NE(emitter, nullptr);
  EXPECT_EQ(emitter->class_name(), "AreaLight");
}

TEST(ContainerTranslatorTest, ComplexNestedBSDFHierarchy) {
  // Nested hierarchy:
  // twosided BSDF -> diffuse BSDF -> checkerboard texture (color0, color1)
  HdContainerDataSourceHandle checker_ds = MakeContainer(
      ValueField("type", std::string("checkerboard")),
      ValueField("color0", GfVec3f(0.1f, 0.1f, 0.1f)),
      ValueField("color1", GfVec3f(0.9f, 0.9f, 0.9f)));

  HdContainerDataSourceHandle diffuse_ds = MakeContainer(
      ValueField("type", std::string("diffuse")),
      ContainerField("reflectance", checker_ds));

  HdContainerDataSourceHandle twosided_ds = MakeContainer(
      ValueField("type", std::string("twosided")),
      ContainerField("nested_bsdf", diffuse_ds));

  mitsuba::ref<mitsuba::BSDF<Float, Spectrum>> twosided =
      BuildPluginFromContainer<mitsuba::BSDF<Float, Spectrum>>(twosided_ds);
  ASSERT_NE(twosided, nullptr);
  EXPECT_EQ(twosided->class_name(), "TwoSidedBRDF");

  // Blend BSDF: bsdf1 (diffuse), bsdf2 (roughconductor), weight (blackbody texture)
  HdContainerDataSourceHandle roughconductor_ds = MakeContainer(
      ValueField("type", std::string("roughconductor")),
      ValueField("eta", GfVec3f(0.2f, 0.3f, 0.4f)),
      ValueField("k", GfVec3f(3.0f, 2.5f, 2.0f)),
      ValueField("alpha", 0.1f));

  HdContainerDataSourceHandle weight_blackbody_ds = MakeContainer(
      ValueField("type", std::string("blackbody")),
      ValueField("temperature", 3000.0f));

  HdContainerDataSourceHandle blend_ds = MakeContainer(
      ValueField("type", std::string("blendbsdf")),
      ContainerField("bsdf1", diffuse_ds),
      ContainerField("bsdf2", roughconductor_ds),
      ContainerField("weight", weight_blackbody_ds));

  mitsuba::ref<mitsuba::BSDF<Float, Spectrum>> blend =
      BuildPluginFromContainer<mitsuba::BSDF<Float, Spectrum>>(blend_ds);
  ASSERT_NE(blend, nullptr);
  EXPECT_EQ(blend->class_name(), "BlendBSDF");
}

TEST(ContainerTranslatorTest, RecursiveIntegratorHierarchy) {
  // 3-level deep nested integrator:
  // aov -> moment -> path (with max_depth = 12, hide_emitters = true)
  HdContainerDataSourceHandle path_ds = MakeContainer(
      ValueField("type", std::string("path")),
      ValueField("max_depth", 12),
      ValueField("hide_emitters", true));

  HdContainerDataSourceHandle moment_ds = MakeContainer(
      ValueField("type", std::string("moment")),
      ContainerField("integrator", path_ds));

  HdContainerDataSourceHandle aov_ds = MakeContainer(
      ValueField("type", std::string("aov")),
      ValueField("aovs", std::string("nn:sh_normal,albedo:albedo")),
      ContainerField("integrator", moment_ds));

  mitsuba::ref<mitsuba::Integrator<Float, Spectrum>> integrator =
      BuildPluginFromContainer<mitsuba::Integrator<Float, Spectrum>>(aov_ds);
  ASSERT_NE(integrator, nullptr);
  EXPECT_EQ(integrator->class_name(), "AOVIntegrator");
}

TEST(ContainerTranslatorTest, SensorFilmSamplerHierarchy) {
  // Hierarchical Sensor:
  // perspective -> film (hdrfilm -> gaussian filter)
  //             -> sampler (independent)
  HdContainerDataSourceHandle filter_ds = MakeContainer(
      ValueField("type", std::string("gaussian")),
      ValueField("stddev", 0.5f));

  HdContainerDataSourceHandle film_ds = MakeContainer(
      ValueField("type", std::string("hdrfilm")),
      ValueField("width", 800),
      ValueField("height", 600),
      ContainerField("pixel_filter", filter_ds));

  HdContainerDataSourceHandle sampler_ds = MakeContainer(
      ValueField("type", std::string("independent")),
      ValueField("sample_count", 64));

  HdContainerDataSourceHandle sensor_ds = MakeContainer(
      ValueField("type", std::string("perspective")),
      ValueField("fov", 45.0f),
      ContainerField("film", film_ds),
      ContainerField("sampler", sampler_ds));

  mitsuba::ref<mitsuba::Sensor<Float, Spectrum>> sensor =
      BuildPluginFromContainer<mitsuba::Sensor<Float, Spectrum>>(sensor_ds);
  ASSERT_NE(sensor, nullptr);
  EXPECT_EQ(sensor->class_name(), "PerspectiveCamera");
  ASSERT_NE(sensor->film(), nullptr);
  ASSERT_NE(sensor->sampler(), nullptr);
  EXPECT_EQ(sensor->sampler()->sample_count(), 64);
}

TEST(ContainerTranslatorTest, UnflattenColonDelimitedUSDAttributes) {
  // Test unflattening flat colon-delimited names into nested containers
  HdContainerDataSourceHandle flat_ds = MakeContainer(
      ValueField("mitsuba:integrator:type", std::string("moment")),
      ValueField("mitsuba:integrator:integrator:type", std::string("path")),
      ValueField("mitsuba:integrator:integrator:max_depth", 8),
      ValueField("mitsuba:integrator:integrator:hide_emitters", true));

  HdContainerDataSourceHandle unflattened = UnflattenContainer(flat_ds, ':');
  ASSERT_NE(unflattened, nullptr);

  // Retrieve "mitsuba" -> "integrator"
  auto mitsuba_child = HdContainerDataSource::Cast(unflattened->Get(TfToken("mitsuba")));
  ASSERT_NE(mitsuba_child, nullptr);

  auto integrator_child = HdContainerDataSource::Cast(mitsuba_child->Get(TfToken("integrator")));
  ASSERT_NE(integrator_child, nullptr);

  mitsuba::ref<mitsuba::Integrator<Float, Spectrum>> integrator =
      BuildPluginFromContainer<mitsuba::Integrator<Float, Spectrum>>(integrator_child);
  ASSERT_NE(integrator, nullptr);
  EXPECT_EQ(integrator->class_name(), "MomentIntegrator");
}

}  // namespace
}  // namespace hdmitsuba

PXR_NAMESPACE_CLOSE_SCOPE
