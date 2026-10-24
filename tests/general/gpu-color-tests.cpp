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

#include "testutil.h"

#include <cmath>
#include <vector>

#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QFile>
#include <QTextStream>

#include "common/ociobridge.h"
#include "node/color/colormanager/colormanager.h"
#include "node/keying/chromakey/chromakey.h"
#include "node/project.h"
#include "render/colorprocessor.h"
#include "render/job/colortransformjob.h"
#include "render/opengl/openglrenderer.h"
#include "render/texture.h"

namespace olive {

OLIVE_ADD_TEST(ColorManagedGpuMatchesCpu)
{
  int argc = 1;
  char arg0[] = "gpu-color-tests";
  char* argv[] = {arg0, nullptr};
  if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  QSurfaceFormat format;
  format.setVersion(3, 2);
  format.setProfile(QSurfaceFormat::CoreProfile);
  format.setRenderableType(QSurfaceFormat::OpenGL);
  QSurfaceFormat::setDefaultFormat(format);
  QGuiApplication app(argc, argv);

  QOpenGLContext ctx;
  if (!ctx.create()) {
    std::cout << " - Skipped (headless / no GL context)";
    OLIVE_TEST_END;
  }
  QOffscreenSurface surf;
  surf.create();
  if (!ctx.makeCurrent(&surf)) {
    std::cout << " - Skipped (makeCurrent failed)";
    OLIVE_TEST_END;
  }

  ColorManager::SetUpDefaultConfig();
  Project project;

  OpenGLRenderer renderer;
  renderer.Init(&ctx);
  renderer.PostInit();

  struct GlCleanup {
    OpenGLRenderer *r;
    QOpenGLContext *c;
    ~GlCleanup() {
      if (r) {
        r->Destroy();
        r->PostDestroy();
      }
      if (c) {
        c->doneCurrent();
      }
    }
  } cleanup{&renderer, &ctx};

  VideoParams params(1, 1, PixelFormat::F32, VideoParams::kRGBAChannelCount);
  const float input[4] = {0.18f, 0.18f, 0.18f, 1.0f};
  TexturePtr source = renderer.CreateTexture(params, input);
  TexturePtr destination = renderer.CreateTexture(params);

  ColorProcessorPtr processor = ColorProcessor::Create(
        project.color_manager(),
        project.color_manager()->GetReferenceColorSpace(),
        ColorTransform(QStringLiteral("sRGB"), QStringLiteral("sRGB OETF"), QString()));

  OLIVE_ASSERT(!source->IsDummy());
  OLIVE_ASSERT(!destination->IsDummy());
  OLIVE_ASSERT(processor);

  ColorTransformJob job;
  job.SetColorProcessor(processor);
  job.SetInputTexture(source);
  renderer.BlitColorManaged(job, destination.get());

  float output[4] = {0, 0, 0, 0};
  renderer.DownloadFromTexture(destination->id(), params, output, sizeof(float) * 4);

  const Color cpu = processor->ConvertColor(Color(0.18f, 0.18f, 0.18f, 1.0f));
  if (std::fabs(double(output[0]) - cpu.red()) > 0.02) {
    std::cout << " - GPU OCIO display output " << output[0]
              << " differs from CPU reference " << cpu.red();
    return __LINE__;
  }

  // HDR view: decode PQ 1.0 (10000-nit code) and run the OCIO HDR-to-SDR
  // view on the GPU; compare with the OCIO CPU reference.
  const float hdr_input[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  TexturePtr hdr_source = renderer.CreateTexture(params, hdr_input);
  TexturePtr hdr_destination = renderer.CreateTexture(params);
  ColorProcessorPtr hdr_processor = ColorProcessor::Create(
        project.color_manager(),
        QStringLiteral("Rec.2020 PQ"),
        ColorTransform(QStringLiteral("sRGB"), QStringLiteral("HDR to SDR"), QString()));

  OLIVE_ASSERT(hdr_processor);
  ColorTransformJob hdr_job;
  hdr_job.SetColorProcessor(hdr_processor);
  hdr_job.SetInputTexture(hdr_source);
  renderer.BlitColorManaged(hdr_job, hdr_destination.get());

  float hdr_output[4] = {0, 0, 0, 0};
  renderer.DownloadFromTexture(hdr_destination->id(), params, hdr_output, sizeof(float) * 4);
  const Color hdr_cpu = hdr_processor->ConvertColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
  if (std::fabs(double(hdr_output[0]) - hdr_cpu.red()) > 0.02) {
    std::cout << " - GPU HDR-to-SDR output " << hdr_output[0]
              << " differs from CPU reference " << hdr_cpu.red();
    return __LINE__;
  }
  if (hdr_output[0] > 1.01f) {
    std::cout << " - GPU HDR-to-SDR output is still clipping: " << hdr_output[0];
    return __LINE__;
  }

  // Primary grading: verify the OCIO GradingPrimary GPU shader applies +1 stop
  // exposure, using the same CPU reference comparison.
  OCIO::GradingPrimaryTransformRcPtr gp =
      OCIO::GradingPrimaryTransform::Create(OCIO::GRADING_LIN);
  gp->setDirection(OCIO::TRANSFORM_DIR_FORWARD);
  OCIO::GradingPrimary primary(OCIO::GRADING_LIN);
  primary.m_exposure = OCIO::GradingRGBM(1.0, 1.0, 1.0, 0.0);
  gp->setValue(primary);

  ColorProcessorPtr primary_processor = ColorProcessorOCIO::Create(
        ColorManagerOCIO::GetConfig(project.color_manager())->getProcessor(gp));
  OLIVE_ASSERT(primary_processor);

  const float mid_grey[4] = {0.18f, 0.18f, 0.18f, 1.0f};
  TexturePtr primary_source = renderer.CreateTexture(params, mid_grey);
  TexturePtr primary_destination = renderer.CreateTexture(params);
  ColorTransformJob primary_job;
  primary_job.SetColorProcessor(primary_processor);
  primary_job.SetInputTexture(primary_source);
  renderer.BlitColorManaged(primary_job, primary_destination.get());

  float primary_output[4] = {0, 0, 0, 0};
  renderer.DownloadFromTexture(primary_destination->id(), params, primary_output,
                               sizeof(float) * 4);
  const Color primary_cpu = primary_processor->ConvertColor(Color(0.18f, 0.18f, 0.18f, 1.0f));
  if (std::fabs(double(primary_output[0]) - primary_cpu.red()) > 0.02) {
    std::cout << " - GPU Primary output " << primary_output[0]
              << " differs from CPU reference " << primary_cpu.red();
    return __LINE__;
  }
  if (std::fabs(double(primary_output[0]) - 0.36) > 0.02) {
    std::cout << " - GPU Primary +1 stop did not produce the expected mid-grey: "
              << primary_output[0];
    return __LINE__;
  }

  // ChromaKey: run the real OCIO-backed ChromaKey shader with a green key and
  // red/green solid inputs, then read the mask-only output. This exercises the
  // CIE-XYZ OCIO stub and the key shader on the GPU, not just shader text.
  ColorProcessorPtr chroma_processor = ColorProcessor::Create(
        project.color_manager(),
        project.color_manager()->GetReferenceColorSpace(),
        ColorTransform(QStringLiteral("cie_xyz_d65_interchange")));
  OLIVE_ASSERT(chroma_processor);

  ChromaKeyNode chroma;
  auto render_mask = [&](const Color &input_color) -> float {
    const float in[4] = {input_color.red(), input_color.green(), input_color.blue(), input_color.alpha()};
    TexturePtr src = renderer.CreateTexture(params, in);
    TexturePtr dst = renderer.CreateTexture(params);

    ColorTransformJob job;
    job.SetColorProcessor(chroma_processor);
    job.SetInputTexture(src);
    // The real ChromaKey node receives this through ColorTransformJob(value);
    // the custom shader samples the named tex_in input directly.
    job.Insert(ChromaKeyNode::kTextureInput,
               NodeValue(NodeValue::kTexture, QVariant::fromValue(src)));
    job.SetNeedsCustomShader(&chroma, QStringLiteral("main"));
    job.SetFunctionName(QStringLiteral("SceneLinearToCIEXYZ_d65"));
    job.Insert(ChromaKeyNode::kColorInput,
               NodeValue(NodeValue::kColor, QVariant::fromValue(Color(0.0f, 1.0f, 0.0f, 1.0f))));
    job.Insert(ChromaKeyNode::kMaskOnlyInput, NodeValue(NodeValue::kBoolean, true));
    job.Insert(ChromaKeyNode::kInvertInput, NodeValue(NodeValue::kBoolean, false));
    job.Insert(ChromaKeyNode::kLowerToleranceInput, NodeValue(NodeValue::kFloat, 5.0));
    job.Insert(ChromaKeyNode::kUpperToleranceInput, NodeValue(NodeValue::kFloat, 25.0));
    job.Insert(ChromaKeyNode::kShadowsInput, NodeValue(NodeValue::kFloat, 100.0));
    job.Insert(ChromaKeyNode::kHighlightsInput, NodeValue(NodeValue::kFloat, 100.0));
    job.Insert(QStringLiteral("garbage_in_enabled"), NodeValue(NodeValue::kBoolean, false));
    job.Insert(QStringLiteral("core_in_enabled"), NodeValue(NodeValue::kBoolean, false));

    renderer.BlitColorManaged(job, dst.get());
    float out[4] = {0, 0, 0, 0};
    renderer.DownloadFromTexture(dst->id(), params, out, sizeof(float) * 4);
    return out[0];
  };

  const float green_mask = render_mask(Color(0.0f, 1.0f, 0.0f, 1.0f));
  const float red_mask = render_mask(Color(1.0f, 0.0f, 0.0f, 1.0f));
  const float soft_mask = render_mask(Color(0.2f, 0.8f, 0.2f, 1.0f));
  if (green_mask > 0.05f || red_mask < 0.85f
      || soft_mask <= 0.02f || soft_mask >= 0.98f) {
    std::cout << " - ChromaKey GPU mask failed: green=" << green_mask
              << " red=" << red_mask << " soft=" << soft_mask;
    return __LINE__;
  }

  // Spatial edge: 16x1 green -> red ramp. The ChromaKey shader must produce a
  // keyed left region, a foreground right region, and at least one soft pixel
  // in between; no NaN/out-of-range values are allowed across the row.
  const int edge_width = 16;
  VideoParams edge_params(edge_width, 1, PixelFormat::F32,
                          VideoParams::kRGBAChannelCount);
  std::vector<float> edge_input(edge_width * 4);
  for (int x = 0; x < edge_width; x++) {
    float t = float(x) / float(edge_width - 1);
    float r = (x < 6) ? 0.0f : (x > 9 ? 1.0f : t);
    float g = (x < 6) ? 1.0f : (x > 9 ? 0.0f : 1.0f - t);
    edge_input[x * 4 + 0] = r;
    edge_input[x * 4 + 1] = g;
    edge_input[x * 4 + 2] = 0.0f;
    edge_input[x * 4 + 3] = 1.0f;
  }

  TexturePtr edge_src = renderer.CreateTexture(edge_params, edge_input.data());
  TexturePtr edge_dst = renderer.CreateTexture(edge_params);
  ColorTransformJob edge_job;
  edge_job.SetColorProcessor(chroma_processor);
  edge_job.SetInputTexture(edge_src);
  edge_job.Insert(ChromaKeyNode::kTextureInput,
                  NodeValue(NodeValue::kTexture, QVariant::fromValue(edge_src)));
  edge_job.SetNeedsCustomShader(&chroma, QStringLiteral("main"));
  edge_job.SetFunctionName(QStringLiteral("SceneLinearToCIEXYZ_d65"));
  edge_job.Insert(ChromaKeyNode::kColorInput,
                  NodeValue(NodeValue::kColor, QVariant::fromValue(Color(0.0f, 1.0f, 0.0f, 1.0f))));
  edge_job.Insert(ChromaKeyNode::kMaskOnlyInput, NodeValue(NodeValue::kBoolean, true));
  edge_job.Insert(ChromaKeyNode::kInvertInput, NodeValue(NodeValue::kBoolean, false));
  edge_job.Insert(ChromaKeyNode::kLowerToleranceInput, NodeValue(NodeValue::kFloat, 5.0));
  edge_job.Insert(ChromaKeyNode::kUpperToleranceInput, NodeValue(NodeValue::kFloat, 25.0));
  edge_job.Insert(ChromaKeyNode::kShadowsInput, NodeValue(NodeValue::kFloat, 100.0));
  edge_job.Insert(ChromaKeyNode::kHighlightsInput, NodeValue(NodeValue::kFloat, 100.0));
  edge_job.Insert(QStringLiteral("garbage_in_enabled"), NodeValue(NodeValue::kBoolean, false));
  edge_job.Insert(QStringLiteral("core_in_enabled"), NodeValue(NodeValue::kBoolean, false));
  renderer.BlitColorManaged(edge_job, edge_dst.get());

  std::vector<float> edge_output(edge_width * 4, 0.0f);
  renderer.DownloadFromTexture(edge_dst->id(), edge_params, edge_output.data(), edge_width);

  int soft_pixels = 0;
  for (int x = 0; x < edge_width; x++) {
    const float mask = edge_output[x * 4];
    if (!std::isfinite(mask) || mask < -0.001f || mask > 1.001f) {
      std::cout << " - ChromaKey spatial mask invalid at x=" << x << ": " << mask;
      return __LINE__;
    }
    if (mask > 0.02f && mask < 0.98f) {
      soft_pixels++;
    }
  }

  if (edge_output[0] > 0.05f
      || edge_output[(edge_width - 1) * 4] < 0.85f
      || soft_pixels == 0) {
    std::cout << " - ChromaKey spatial edge failed: left=" << edge_output[0]
              << " right=" << edge_output[(edge_width - 1) * 4]
              << " soft_pixels=" << soft_pixels;
    return __LINE__;
  }

  // 6. Camera Log GPU shader test: S-Log3 and ARRI LogC3 shader generation and pixel match
  {
    ColorProcessorPtr slog_proc = ColorProcessor::Create(
        project.color_manager(),
        QStringLiteral("Sony S-Gamut3.Cine / S-Log3"),
        project.color_manager()->GetReferenceColorSpace());
    if (!slog_proc) {
      std::cout << " - GPU S-Log3 processor could not be created";
      return __LINE__;
    }

    // Gray card input: 0.410557 log
    const std::vector<float> slog_input = {0.410557f, 0.410557f, 0.410557f, 1.0f};
    TexturePtr slog_src = renderer.CreateTexture(params, slog_input.data());
    TexturePtr slog_dst = renderer.CreateTexture(params);
    ColorTransformJob slog_job;
    slog_job.SetColorProcessor(slog_proc);
    slog_job.SetInputTexture(slog_src);
    renderer.BlitColorManaged(slog_job, slog_dst.get());

    std::vector<float> slog_output(4, 0.0f);
    renderer.DownloadFromTexture(slog_dst->id(), params, slog_output.data(), 1);

    Color cpu_lin = slog_proc->ConvertColor(Color(0.410557, 0.410557, 0.410557, 1.0));
    if (std::abs(slog_output[0] - static_cast<float>(cpu_lin.red())) > 0.005f) {
      std::cout << " - S-Log3 GPU pixel output " << slog_output[0]
                << " differs from CPU output " << cpu_lin.red();
      return __LINE__;
    }
  }

  // 7. .cube FileTransform GPU 3D texture and pixel match test
  {
    QTemporaryDir temp_dir;
    OLIVE_ASSERT(temp_dir.isValid());
    QString cube_path = temp_dir.filePath(QStringLiteral("gpu_test_lut.cube"));
    {
      QFile f(cube_path);
      OLIVE_ASSERT(f.open(QIODevice::WriteOnly | QIODevice::Text));
      QTextStream out(&f);
      out << "TITLE \"Invert Red\"\n"
          << "LUT_3D_SIZE 2\n"
          << "1 0 0\n"
          << "0 0 0\n"
          << "1 1 0\n"
          << "0 1 0\n"
          << "1 0 1\n"
          << "0 0 1\n"
          << "1 1 1\n"
          << "0 1 1\n";
    }

    auto ocio_config = ColorManagerOCIO::GetConfig(project.color_manager());
    auto file_transform = OCIO::FileTransform::Create();
    file_transform->setSrc(cube_path.toUtf8().constData());
    file_transform->setDirection(OCIO::TRANSFORM_DIR_FORWARD);
    file_transform->setInterpolation(OCIO::INTERP_LINEAR);

    auto ocio_proc = ocio_config->getProcessor(file_transform);
    OLIVE_ASSERT(ocio_proc != nullptr);

    // Verify 3D texture dimensions from GpuShaderDesc
    auto shader_desc = OCIO::GpuShaderDesc::CreateShaderDesc();
    shader_desc->setLanguage(OCIO::GPU_LANGUAGE_GLSL_1_2);
    shader_desc->setFunctionName("OCIO_Test_Cube");
    auto gpu_proc = ocio_proc->getDefaultGPUProcessor();
    OLIVE_ASSERT(gpu_proc != nullptr);
    gpu_proc->extractGpuShaderInfo(shader_desc);
    if (shader_desc->getNum3DTextures() != 1) {
      std::cout << " - Expected 1 3D texture from .cube FileTransform, got " << shader_desc->getNum3DTextures();
      return __LINE__;
    }
    const char* tname = nullptr;
    const char* sname = nullptr;
    unsigned int edge_len = 0;
    OCIO::Interpolation interp = OCIO::INTERP_LINEAR;
    shader_desc->get3DTexture(0, tname, sname, edge_len, interp);
    if (edge_len != 2) {
      std::cout << " - Expected edge_len 2 from 2x2x2 .cube, got " << edge_len;
      return __LINE__;
    }

    ColorProcessorPtr lut_proc = ColorProcessorOCIO::Create(ocio_proc);
    OLIVE_ASSERT(lut_proc != nullptr);

    const float lut_in[4] = {0.0f, 1.0f, 0.5f, 1.0f};
    TexturePtr lut_src = renderer.CreateTexture(params, lut_in);
    TexturePtr lut_dst = renderer.CreateTexture(params);
    ColorTransformJob lut_job;
    lut_job.SetColorProcessor(lut_proc);
    lut_job.SetInputTexture(lut_src);
    renderer.BlitColorManaged(lut_job, lut_dst.get());

    float lut_out[4] = {0, 0, 0, 0};
    renderer.DownloadFromTexture(lut_dst->id(), params, lut_out, sizeof(float) * 4);
    Color lut_cpu = lut_proc->ConvertColor(Color(0.0, 1.0, 0.5, 1.0));
    if (std::abs(double(lut_out[0]) - lut_cpu.red()) > 0.02 ||
        std::abs(double(lut_out[1]) - lut_cpu.green()) > 0.02 ||
        std::abs(double(lut_out[2]) - lut_cpu.blue()) > 0.02) {
      std::cout << " - .cube GPU output (" << lut_out[0] << "," << lut_out[1] << "," << lut_out[2]
                << ") differs from CPU reference (" << lut_cpu.red() << "," << lut_cpu.green() << "," << lut_cpu.blue() << ")";
      return __LINE__;
    }
  }

  // 8. ACEScg -> sRGB display view GPU pixel output vs CPU reference
  {
    ColorTransform acescg_disp(QStringLiteral("sRGB"), QStringLiteral("sRGB OETF"), QString());
    ColorProcessorPtr acescg_proc = ColorProcessor::Create(
        project.color_manager(),
        QStringLiteral("ACEScg"),
        acescg_disp);
    OLIVE_ASSERT(acescg_proc != nullptr);

    const float aces_in[4] = {0.3f, 0.6f, 0.1f, 1.0f};
    TexturePtr aces_src = renderer.CreateTexture(params, aces_in);
    TexturePtr aces_dst = renderer.CreateTexture(params);
    ColorTransformJob aces_job;
    aces_job.SetColorProcessor(acescg_proc);
    aces_job.SetInputTexture(aces_src);
    renderer.BlitColorManaged(aces_job, aces_dst.get());

    float aces_out[4] = {0, 0, 0, 0};
    renderer.DownloadFromTexture(aces_dst->id(), params, aces_out, sizeof(float) * 4);
    Color aces_cpu = acescg_proc->ConvertColor(Color(0.3, 0.6, 0.1, 1.0));
    if (std::abs(double(aces_out[0]) - aces_cpu.red()) > 0.02 ||
        std::abs(double(aces_out[1]) - aces_cpu.green()) > 0.02 ||
        std::abs(double(aces_out[2]) - aces_cpu.blue()) > 0.02) {
      std::cout << " - ACEScg GPU output (" << aces_out[0] << "," << aces_out[1] << "," << aces_out[2]
                << ") differs from CPU reference (" << aces_cpu.red() << "," << aces_cpu.green() << "," << aces_cpu.blue() << ")";
      return __LINE__;
    }
  }

  OLIVE_TEST_END;
}

}
