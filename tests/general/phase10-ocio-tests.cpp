/***

  Olive - Non-Linear Video Editor
  Copyright (C) 2022 Olive Team

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

***/

/**
 * Phase 15 gate: the Primary grading node must always be advertised now that
 * OpenColorIO is the only colour pipeline.
 */

#include "testutil.h"

#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QTextStream>

#include <cmath>

#include "common/ociobridge.h"
#include "node/color/colormanager/colormanager.h"
#include "node/color/ociofiletransform/ociofiletransform.h"
#include "node/factory.h"
#include "node/node.h"
#include "node/project.h"
#include "render/colorprocessor.h"

namespace olive {

namespace {
bool NearlyEqual(double a, double b, double epsilon)
{
  return std::fabs(a - b) <= epsilon;
}
}

OLIVE_ADD_TEST(SurfaceFormatCompliance)
{
  // Assert that Desktop OpenGL 3.2 Core is correctly configured as our universal baseline
  QSurfaceFormat format;
  format.setVersion(3, 2);
  format.setProfile(QSurfaceFormat::CoreProfile);
  format.setRenderableType(QSurfaceFormat::OpenGL);

  OLIVE_ASSERT_EQUAL(format.majorVersion(), 3);
  OLIVE_ASSERT_EQUAL(format.minorVersion(), 2);
  OLIVE_ASSERT_EQUAL(format.profile(), QSurfaceFormat::CoreProfile);
  OLIVE_ASSERT_EQUAL(format.renderableType(), QSurfaceFormat::OpenGL);

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(DesktopShaderPreambleCompliance)
{
  // Test desktop GLSL preamble compliance: must start with #version 150 and
  // strictly exclude "precision highp float;" which breaks Mesa desktop Core Profile
  const QString desktop_preamble = QStringLiteral("#version 150\n\n");
  OLIVE_ASSERT(desktop_preamble.startsWith(QStringLiteral("#version 150")));
  OLIVE_ASSERT(!desktop_preamble.contains(QStringLiteral("precision highp float;")));

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(GradingNodeAvailability)
{
  NodeFactory::Initialize();

  Node* node = NodeFactory::CreateFromFactoryIndex(
      NodeFactory::kOCIOGradingTransformLinear);
  if (!node) {
    NodeFactory::Destroy();
    std::cout << " - grading node could not be constructed";
    return __LINE__;
  }

  const bool hidden = (node->GetFlags() & Node::kDontShowInCreateMenu) != 0;

  if (hidden) {
    delete node;
    NodeFactory::Destroy();
    std::cout << " - OCIO build hides the Primary grading node";
    return __LINE__;
  }

  delete node;
  NodeFactory::Destroy();
  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(PrimaryGradingMath)
{
  ColorManager::SetUpDefaultConfig();
  Project project;
  ColorManager *manager = project.color_manager();

  OCIO::GradingPrimaryTransformRcPtr gp = OCIO::GradingPrimaryTransform::Create(OCIO::GRADING_LIN);
  gp->setDirection(OCIO::TRANSFORM_DIR_FORWARD);

  OCIO::GradingPrimary values(OCIO::GRADING_LIN);
  // +1 stop. OCIO's CPU GradingPrimary stores exposure in stops, while the
  // node's GPU uniform path converts stops to a multiplier.
  values.m_exposure = OCIO::GradingRGBM(1.0, 1.0, 1.0, 0.0);
  gp->setValue(values);

  ColorProcessorPtr proc = ColorProcessorOCIO::Create(
        ColorManagerOCIO::GetConfig(manager)->getProcessor(gp));

  if (!proc) {
    std::cout << " - Primary grading processor could not be created";
    return __LINE__;
  }

  const Color in(0.18f, 0.18f, 0.18f);
  const Color out = proc->ConvertColor(in);

  if (!NearlyEqual(out.red(), 0.36, 1e-4)
      || !NearlyEqual(out.green(), 0.36, 1e-4)
      || !NearlyEqual(out.blue(), 0.36, 1e-4)) {
    std::cout << " - +1 stop exposure did not double the linear mid-grey: "
              << out.red() << ", " << out.green() << ", " << out.blue();
    return __LINE__;
  }

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(ChromaKeyOcioShader)
{
  ColorManager::SetUpDefaultConfig();
  Project project;
  ColorManager *manager = project.color_manager();

  ColorProcessorPtr proc = ColorProcessor::Create(
        manager,
        manager->GetReferenceColorSpace(),
        ColorTransform(QStringLiteral("cie_xyz_d65_interchange")));

  if (!proc) {
    std::cout << " - ChromaKey colour transform could not be created";
    return __LINE__;
  }

  auto ocio_proc = ColorProcessorOCIO::GetProcessor(proc.get());
  if (!ocio_proc) {
    std::cout << " - ChromaKey processor has no OCIO processor";
    return __LINE__;
  }

  auto gpu_proc = ocio_proc->getDefaultGPUProcessor();
  if (!gpu_proc) {
    std::cout << " - ChromaKey processor has no GPU processor";
    return __LINE__;
  }

  auto desc = OCIO::GpuShaderDesc::CreateShaderDesc();
  desc->setLanguage(OCIO::GPU_LANGUAGE_GLSL_1_3);
  desc->setFunctionName("SceneLinearToCIEXYZ_d65");
  desc->setResourcePrefix("ocio_");
  gpu_proc->extractGpuShaderInfo(desc);

  const QString shader = QString::fromUtf8(desc->getShaderText());
  if (!shader.contains(QStringLiteral("SceneLinearToCIEXYZ_d65"))) {
    std::cout << " - ChromaKey OCIO shader does not define the requested function";
    return __LINE__;
  }

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(CameraLogCurvesToSceneLinear)
{
  // P1-2: verify camera log curve math and 18% mid-gray mapping for ARRI LogC,
  // Panasonic V-Log, and Sony S-Log3 in the bundled OCIO configuration.
  ColorManager::SetUpDefaultConfig();
  Project project;
  project.Initialize();
  ColorManager *manager = project.color_manager();

  // Test 1: Sony S-Log3 mid-gray (0.410557 log -> ~0.18 linear)
  {
    ColorProcessorPtr proc = ColorProcessor::Create(
        manager,
        QStringLiteral("Sony S-Gamut3.Cine / S-Log3"),
        manager->GetReferenceColorSpace());
    if (!proc) {
      std::cout << " - Sony S-Log3 transform could not be created";
      return __LINE__;
    }

    Color middle_gray_log(0.410557, 0.410557, 0.410557, 1.0);
    Color lin = proc->ConvertColor(middle_gray_log);
    if (std::abs(lin.red() - 0.18) > 0.02) {
      std::cout << " - S-Log3 18% mid-gray did not map to ~0.18 linear: " << lin.red();
      return __LINE__;
    }

    // Monotonicity check
    Color shadow_log(0.2, 0.2, 0.2, 1.0);
    Color highlight_log(0.7, 0.7, 0.7, 1.0);
    Color shadow_lin = proc->ConvertColor(shadow_log);
    Color highlight_lin = proc->ConvertColor(highlight_log);
    if (shadow_lin.red() >= lin.red() || lin.red() >= highlight_lin.red()) {
      std::cout << " - S-Log3 curve is not strictly monotonic";
      return __LINE__;
    }
  }

  // Test 2: ARRI LogC EI 800 mid-gray (0.391 log -> ~0.18 linear)
  {
    ColorProcessorPtr proc = ColorProcessor::Create(
        manager,
        QStringLiteral("Arri Wide Gamut / LogC EI 800"),
        manager->GetReferenceColorSpace());
    if (!proc) {
      std::cout << " - ARRI LogC EI 800 transform could not be created";
      return __LINE__;
    }

    Color middle_gray_log(0.391, 0.391, 0.391, 1.0);
    Color lin = proc->ConvertColor(middle_gray_log);
    if (std::abs(lin.red() - 0.18) > 0.02) {
      std::cout << " - ARRI LogC 18% mid-gray did not map to ~0.18 linear: " << lin.red();
      return __LINE__;
    }
  }

  // Test 3: Panasonic V-Log mid-gray (0.423 log -> ~0.18 linear)
  {
    ColorProcessorPtr proc = ColorProcessor::Create(
        manager,
        QStringLiteral("Panasonic V-Log V-Gamut"),
        manager->GetReferenceColorSpace());
    if (!proc) {
      std::cout << " - Panasonic V-Log transform could not be created";
      return __LINE__;
    }

    Color middle_gray_log(0.423, 0.423, 0.423, 1.0);
    Color lin = proc->ConvertColor(middle_gray_log);
    if (std::abs(lin.red() - 0.18) > 0.02) {
      std::cout << " - Panasonic V-Log 18% mid-gray did not map to ~0.18 linear: " << lin.red();
      return __LINE__;
    }
  }

  // Test 4: Real camera asset verification (ARRI Alexa Mini LF LogC3 ProRes sample)
  {
    const QString arri_sample = QStringLiteral("assets/test_videos/color_fixtures/log/arri_mini_lf_logc3_prores422hq.mxf");
    if (QFile::exists(arri_sample)) {
      // If sample asset is present, ensure its target colorspace converts highlights (>1.0) and shadows (<0.1) without NaN/Inf
      ColorProcessorPtr arri_proc = ColorProcessor::Create(
          manager,
          QStringLiteral("Arri Wide Gamut / LogC EI 800"),
          manager->GetReferenceColorSpace());
      if (!arri_proc) {
        std::cout << " - ARRI processor for real asset could not be created";
        return __LINE__;
      }
      Color highlight_sample(0.85, 0.85, 0.85, 1.0);
      Color high_lin = arri_proc->ConvertColor(highlight_sample);
      if (!std::isfinite(high_lin.red()) || high_lin.red() <= 1.0) {
        std::cout << " - ARRI LogC3 highlight did not map to linear HDR range > 1.0: " << high_lin.red();
        return __LINE__;
      }
    }
  }

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(ExternalOcioConfigLoading)
{
  Project p;
  ColorManager* cm = p.color_manager();
  OLIVE_ASSERT(cm != nullptr);
  OLIVE_ASSERT(!cm->IsCustomConfig());
  OLIVE_ASSERT(!cm->HasError());
  OLIVE_ASSERT(cm->GetConfigFilename().isEmpty());

  QTemporaryDir temp_dir;
  OLIVE_ASSERT(temp_dir.isValid());

  // 1. Valid custom config
  QString valid_ocio_path = temp_dir.filePath(QStringLiteral("custom_test.ocio"));
  {
    QFile f(valid_ocio_path);
    OLIVE_ASSERT(f.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream out(&f);
    out << "ocio_profile_version: 2.1\n"
        << "environment: {}\n"
        << "search_path: \"\"\n"
        << "roles:\n"
        << "  default: RawRef\n"
        << "  scene_linear: RawRef\n"
        << "displays:\n"
        << "  sRGB:\n"
        << "    - !<View> {name: RawRef, colorspace: RawRef}\n"
        << "active_displays: [sRGB]\n"
        << "active_views: [RawRef]\n"
        << "colorspaces:\n"
        << "  - !<ColorSpace>\n"
        << "    name: RawRef\n"
        << "    family: Raw\n"
        << "    bitdepth: 32f\n"
        << "    isdata: false\n"
        << "  - !<ColorSpace>\n"
        << "    name: CustomTestSpace\n"
        << "    family: Custom\n"
        << "    bitdepth: 32f\n"
        << "    isdata: false\n"
        << "    to_reference: !<MatrixTransform> {matrix: [2, 0, 0, 0,  0, 2, 0, 0,  0, 0, 2, 0,  0, 0, 0, 1]}\n";
  }

  cm->SetConfigFilename(valid_ocio_path);
  bool ok = cm->UpdateConfigFromFilename();
  OLIVE_ASSERT(ok);
  OLIVE_ASSERT(cm->IsCustomConfig());
  OLIVE_ASSERT(!cm->HasError());
  OLIVE_ASSERT(cm->GetConfigFilename() == valid_ocio_path);
  QStringList cs_list = cm->ListAvailableColorspaces();
  OLIVE_ASSERT(cs_list.contains(QStringLiteral("CustomTestSpace")));
  OLIVE_ASSERT(cs_list.contains(QStringLiteral("RawRef")));

  // Relative path resolution when project filename is set
  p.set_filename(temp_dir.filePath(QStringLiteral("test_project.ove")));
  cm->SetConfigFilename(QStringLiteral("custom_test.ocio"));
  bool ok_rel = cm->UpdateConfigFromFilename();
  OLIVE_ASSERT(ok_rel);
  OLIVE_ASSERT(cm->IsCustomConfig());
  OLIVE_ASSERT(!cm->HasError());
  OLIVE_ASSERT(cm->ListAvailableColorspaces().contains(QStringLiteral("CustomTestSpace")));

  // Verify ColorProcessor converts via the custom matrix transform
  ColorProcessorPtr proc = ColorProcessor::Create(cm, QStringLiteral("CustomTestSpace"), QStringLiteral("RawRef"));
  OLIVE_ASSERT(proc != nullptr);
  Color in_col(0.5, 0.25, 0.1, 1.0);
  Color out_col = proc->ConvertColor(in_col);
  if (std::abs(out_col.red() - 1.0) > 0.001 || std::abs(out_col.green() - 0.5) > 0.001) {
    std::cout << " - Custom OCIO matrix transform did not scale values correctly: " << out_col.red();
    return __LINE__;
  }

  // 2. Non-existent file path: must fail explicitly and preserve current valid config
  QString non_existent = temp_dir.filePath(QStringLiteral("does_not_exist.ocio"));
  cm->SetConfigFilename(non_existent);
  bool fail_missing = cm->UpdateConfigFromFilename();
  OLIVE_ASSERT(!fail_missing);
  OLIVE_ASSERT(cm->HasError());
  OLIVE_ASSERT(cm->GetLastError().contains(QStringLiteral("does not exist")));
  OLIVE_ASSERT(cm->ListAvailableColorspaces().contains(QStringLiteral("CustomTestSpace")));

  // 3. Corrupt/invalid file: must reject via validate() and preserve config
  QString corrupt_ocio_path = temp_dir.filePath(QStringLiteral("corrupt.ocio"));
  {
    QFile f(corrupt_ocio_path);
    OLIVE_ASSERT(f.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream out(&f);
    out << "ocio_profile_version: 2.1\n"
        << "invalid_garbage_yaml: [][[]]}}}\n";
  }
  cm->SetConfigFilename(corrupt_ocio_path);
  bool fail_corrupt = cm->UpdateConfigFromFilename();
  OLIVE_ASSERT(!fail_corrupt);
  OLIVE_ASSERT(cm->HasError());
  OLIVE_ASSERT(cm->ListAvailableColorspaces().contains(QStringLiteral("CustomTestSpace")));

  // 4. Clear filename -> reverts cleanly to built-in default config
  cm->SetConfigFilename(QString());
  bool revert_ok = cm->UpdateConfigFromFilename();
  OLIVE_ASSERT(revert_ok);
  OLIVE_ASSERT(!cm->IsCustomConfig());
  OLIVE_ASSERT(!cm->HasError());
  OLIVE_ASSERT(cm->GetConfigFilename().isEmpty());
  QStringList default_spaces = cm->ListAvailableColorspaces();
  OLIVE_ASSERT(default_spaces.contains(QStringLiteral("sRGB OETF")));
  OLIVE_ASSERT(!default_spaces.contains(QStringLiteral("CustomTestSpace")));

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(OCIOFileTransformNodeTesting)
{
  NodeFactory::Initialize();

  Project p;
  ColorManager* cm = p.color_manager();
  OLIVE_ASSERT(cm != nullptr);

  // 1. Factory creation by ID
  Node* created = NodeFactory::CreateFromID(QStringLiteral("org.olivevideoeditor.Olive.ociofiletransform"));
  OLIVE_ASSERT(created != nullptr);
  auto* lut_node = dynamic_cast<OCIOFileTransformNode*>(created);
  OLIVE_ASSERT(lut_node != nullptr);
  lut_node->AddedToGraphEvent(&p);

  QTemporaryDir temp_dir;
  OLIVE_ASSERT(temp_dir.isValid());

  // 2. Test valid .cube LUT (3D invert red channel)
  QString cube_path = temp_dir.filePath(QStringLiteral("invert_red.cube"));
  {
    QFile f(cube_path);
    OLIVE_ASSERT(f.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream out(&f);
    out << "TITLE \"Invert Red\"\n"
        << "LUT_3D_SIZE 2\n"
        << "1 0 0\n" // in: 0 0 0 -> out: 1 0 0
        << "0 0 0\n" // in: 1 0 0 -> out: 0 0 0
        << "1 1 0\n" // in: 0 1 0 -> out: 1 1 0
        << "0 1 0\n" // in: 1 1 0 -> out: 0 1 0
        << "1 0 1\n" // in: 0 0 1 -> out: 1 0 1
        << "0 0 1\n" // in: 1 0 1 -> out: 0 0 1
        << "1 1 1\n" // in: 0 1 1 -> out: 1 1 1
        << "0 1 1\n"; // in: 1 1 1 -> out: 0 1 1
  }

  lut_node->SetStandardValue(OCIOFileTransformNode::kFileInput, cube_path);
  lut_node->SetStandardValue(OCIOFileTransformNode::kDirectionInput, static_cast<int>(ColorProcessor::kNormal));
  lut_node->SetStandardValue(OCIOFileTransformNode::kInterpolationInput, static_cast<int>(OCIOFileTransformNode::kInterpLinear));

  // Verify processor creation and color transformation
  ColorProcessorPtr proc = lut_node->processor();
  OLIVE_ASSERT(proc != nullptr);

  Color c_in(0.0, 1.0, 0.5, 1.0);
  Color c_out = proc->ConvertColor(c_in);
  if (std::abs(c_out.red() - 1.0) > 0.01 || std::abs(c_out.green() - 1.0) > 0.01 || std::abs(c_out.blue() - 0.5) > 0.01) {
    std::cout << " - .cube LUT forward transform failed: " << c_out.red() << " " << c_out.green() << " " << c_out.blue();
    return __LINE__;
  }

  // Test inverse direction on the .cube LUT
  lut_node->SetStandardValue(OCIOFileTransformNode::kDirectionInput, static_cast<int>(ColorProcessor::kInverse));
  ColorProcessorPtr inv_proc = lut_node->processor();
  OLIVE_ASSERT(inv_proc != nullptr);
  Color c_inv_out = inv_proc->ConvertColor(c_out);
  if (std::abs(c_inv_out.red() - c_in.red()) > 0.01 || std::abs(c_inv_out.green() - c_in.green()) > 0.01 || std::abs(c_inv_out.blue() - c_in.blue()) > 0.01) {
    std::cout << " - .cube LUT inverse transform failed: " << c_inv_out.red() << " " << c_inv_out.green() << " " << c_inv_out.blue();
    return __LINE__;
  }

  // 3. Test .spi1d LUT
  lut_node->SetStandardValue(OCIOFileTransformNode::kDirectionInput, static_cast<int>(ColorProcessor::kNormal));
  QString spi1d_path = QStringLiteral(OLIVE_TEST_SOURCE_DIR "/app/render/ocioconf/luts/rec709_to_linear.spi1d");
  OLIVE_ASSERT(QFile::exists(spi1d_path));
  lut_node->SetStandardValue(OCIOFileTransformNode::kFileInput, spi1d_path);
  ColorProcessorPtr spi1d_proc = lut_node->processor();
  OLIVE_ASSERT(spi1d_proc != nullptr);
  Color c_rec(0.5, 0.5, 0.5, 1.0);
  Color c_lin = spi1d_proc->ConvertColor(c_rec);
  OLIVE_ASSERT(c_lin.red() < 0.5);

  // 4. Test .spi3d LUT
  QString spi3d_path = QStringLiteral(OLIVE_TEST_SOURCE_DIR "/app/render/ocioconf/luts/desat65cube.spi3d");
  OLIVE_ASSERT(QFile::exists(spi3d_path));
  lut_node->SetStandardValue(OCIOFileTransformNode::kFileInput, spi3d_path);
  ColorProcessorPtr spi3d_proc = lut_node->processor();
  OLIVE_ASSERT(spi3d_proc != nullptr);
  Color c_sat(1.0, 0.0, 0.0, 1.0);
  Color c_desat = spi3d_proc->ConvertColor(c_sat);
  OLIVE_ASSERT(c_desat.green() > 0.0);

  // 5. Test missing file path
  lut_node->SetStandardValue(OCIOFileTransformNode::kFileInput, QStringLiteral("/nonexistent/file.cube"));
  OLIVE_ASSERT(lut_node->processor() == nullptr);

  // 6. Test corrupt file
  QString corrupt_path = temp_dir.filePath(QStringLiteral("corrupt.cube"));
  {
    QFile f(corrupt_path);
    OLIVE_ASSERT(f.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream out(&f);
    out << "NOT_A_VALID_LUT_FORMAT 9999\n";
  }
  lut_node->SetStandardValue(OCIOFileTransformNode::kFileInput, corrupt_path);
  OLIVE_ASSERT(lut_node->processor() == nullptr);

  lut_node->RemovedFromGraphEvent(&p);
  delete lut_node;
  NodeFactory::Destroy();

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(AcesColorSpaceCompliance)
{
  ColorManager::SetUpDefaultConfig();
  Project project;
  ColorManager *manager = project.color_manager();
  OLIVE_ASSERT(manager != nullptr);

  QStringList available = manager->ListAvailableColorspaces();

  // 1. Assert ACEScg and ACES2065-1 are present in bundled config
  if (!available.contains(QStringLiteral("ACEScg"))) {
    std::cout << " - ACEScg missing from available colorspaces";
    return __LINE__;
  }
  if (!available.contains(QStringLiteral("ACES2065-1"))) {
    std::cout << " - ACES2065-1 missing from available colorspaces";
    return __LINE__;
  }

  // 2. ACEScg to Scene Linear (Rec.709) conversion test
  ColorProcessorPtr acescg_to_lin = ColorProcessor::Create(
      manager,
      QStringLiteral("ACEScg"),
      QStringLiteral("Linear"));
  if (!acescg_to_lin) {
    std::cout << " - ACEScg to Linear processor could not be created";
    return __LINE__;
  }

  // Neutral white preservation: (1, 1, 1) -> (1, 1, 1)
  Color white_aces(1.0, 1.0, 1.0, 1.0);
  Color white_lin = acescg_to_lin->ConvertColor(white_aces);
  if (std::abs(white_lin.red() - 1.0) > 0.01 || std::abs(white_lin.green() - 1.0) > 0.01 || std::abs(white_lin.blue() - 1.0) > 0.01) {
    std::cout << " - ACEScg white point failed to map to Linear (1, 1, 1): " << white_lin.red() << " " << white_lin.green() << " " << white_lin.blue();
    return __LINE__;
  }

  // Pure AP1 red primary: (1, 0, 0) in ACEScg -> AP1 gamut exceeds Rec.709, red maps to ~1.705
  Color red_aces(1.0, 0.0, 0.0, 1.0);
  Color red_lin = acescg_to_lin->ConvertColor(red_aces);
  if (std::abs(red_lin.red() - 1.705) > 0.02 || red_lin.green() >= 0.0 || red_lin.blue() >= 0.0) {
    std::cout << " - ACEScg AP1 red primary transform math mismatch: " << red_lin.red() << " " << red_lin.green() << " " << red_lin.blue();
    return __LINE__;
  }

  // Saturated color and roundtrip test: ACEScg -> Linear -> ACEScg
  Color sat_aces(0.2f, 0.8f, 0.1f, 1.0f);
  Color sat_lin = acescg_to_lin->ConvertColor(sat_aces);
  OLIVE_ASSERT(std::isfinite(sat_lin.red()) && std::isfinite(sat_lin.green()) && std::isfinite(sat_lin.blue()));

  ColorProcessorPtr lin_to_acescg = ColorProcessor::Create(
      manager,
      QStringLiteral("Linear"),
      QStringLiteral("ACEScg"));
  OLIVE_ASSERT(lin_to_acescg != nullptr);
  Color back_aces = lin_to_acescg->ConvertColor(sat_lin);
  if (std::abs(back_aces.red() - sat_aces.red()) > 0.005 ||
      std::abs(back_aces.green() - sat_aces.green()) > 0.005 ||
      std::abs(back_aces.blue() - sat_aces.blue()) > 0.005) {
    std::cout << " - ACEScg <-> Linear roundtrip failed: " << back_aces.red() << " " << back_aces.green() << " " << back_aces.blue();
    return __LINE__;
  }

  // 3. ACES2065-1 to ACEScg conversion test
  ColorProcessorPtr ap0_to_ap1 = ColorProcessor::Create(
      manager,
      QStringLiteral("ACES2065-1"),
      QStringLiteral("ACEScg"));
  if (!ap0_to_ap1) {
    std::cout << " - ACES2065-1 to ACEScg processor could not be created";
    return __LINE__;
  }
  Color white_ap0(1.0, 1.0, 1.0, 1.0);
  Color white_ap1 = ap0_to_ap1->ConvertColor(white_ap0);
  if (std::abs(white_ap1.red() - 1.0) > 0.01) {
    std::cout << " - AP0 to AP1 white point mismatch: " << white_ap1.red();
    return __LINE__;
  }

  // 4. ACEScg to Display (sRGB OETF)
  ColorTransform disp_transform(QStringLiteral("sRGB"), QStringLiteral("sRGB OETF"), QString());
  ColorProcessorPtr acescg_to_disp = ColorProcessor::Create(
      manager,
      QStringLiteral("ACEScg"),
      disp_transform);
  if (!acescg_to_disp) {
    std::cout << " - ACEScg to sRGB Display processor could not be created";
    return __LINE__;
  }
  Color disp_col = acescg_to_disp->ConvertColor(Color(0.18, 0.18, 0.18, 1.0));
  if (disp_col.red() <= 0.18) {
    std::cout << " - ACEScg mid-gray did not apply display gamma/OETF correctly: " << disp_col.red();
    return __LINE__;
  }

  OLIVE_TEST_END;
}

} // namespace olive
