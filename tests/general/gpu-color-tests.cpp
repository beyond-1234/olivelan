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

#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QSurfaceFormat>

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

  OLIVE_TEST_END;
}

}
