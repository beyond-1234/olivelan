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

#include "node/color/colormanager/colormanager.h"
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

  OLIVE_TEST_END;
}

}
