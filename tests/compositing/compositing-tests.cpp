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

#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLShader>
#include <QOpenGLShaderProgram>

#include "node/filter/blur/blur.h"
#include "render/job/shaderjob.h"

namespace olive {

OLIVE_ADD_TEST(BlurShaderCompileAndLink)
{
  int argc = 1;
  char arg0[] = "compositing-tests";
  char* argv[] = {arg0, nullptr};
  if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
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
  auto* f = ctx.functions();

  // Test blur.frag compilation with GL 3.2 header
  static const QString header = QStringLiteral("#version 150\nprecision highp float;\n");
  QFile vert_file(QStringLiteral(":/shaders/default.vert"));
  OLIVE_ASSERT(vert_file.open(QFile::ReadOnly));
  QString vert_src = header + QString::fromUtf8(vert_file.readAll());
  vert_file.close();

  QFile frag_file(QStringLiteral(":/shaders/blur.frag"));
  OLIVE_ASSERT(frag_file.open(QFile::ReadOnly));
  QString frag_src = header + QString::fromUtf8(frag_file.readAll());
  frag_file.close();

  GLuint vs = f->glCreateShader(GL_VERTEX_SHADER);
  QByteArray vert_utf8 = vert_src.toUtf8();
  const char* vert_cstr = vert_utf8.constData();
  f->glShaderSource(vs, 1, &vert_cstr, nullptr);
  f->glCompileShader(vs);

  GLuint fs = f->glCreateShader(GL_FRAGMENT_SHADER);
  QByteArray frag_utf8 = frag_src.toUtf8();
  const char* frag_cstr = frag_utf8.constData();
  f->glShaderSource(fs, 1, &frag_cstr, nullptr);
  f->glCompileShader(fs);

  GLint vs_ok = 0, fs_ok = 0;
  f->glGetShaderiv(vs, GL_COMPILE_STATUS, &vs_ok);
  f->glGetShaderiv(fs, GL_COMPILE_STATUS, &fs_ok);

  OLIVE_ASSERT(vs_ok == GL_TRUE);
  OLIVE_ASSERT(fs_ok == GL_TRUE);

  GLuint prog = f->glCreateProgram();
  f->glAttachShader(prog, vs);
  f->glAttachShader(prog, fs);
  f->glLinkProgram(prog);

  GLint link_ok = 0;
  f->glGetProgramiv(prog, GL_LINK_STATUS, &link_ok);
  OLIVE_ASSERT(link_ok == GL_TRUE);

  // Validate uniform existence
  GLint iteration_loc = f->glGetUniformLocation(prog, "ove_iteration");
  GLint radius_loc = f->glGetUniformLocation(prog, "radius_in");
  GLint method_loc = f->glGetUniformLocation(prog, "method_in");
  OLIVE_ASSERT(iteration_loc > -1);
  OLIVE_ASSERT(radius_loc > -1);
  OLIVE_ASSERT(method_loc > -1);

  f->glDeleteProgram(prog);
  f->glDeleteShader(vs);
  f->glDeleteShader(fs);

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(BlurNodeIterationConfig)
{
  BlurFilterNode blur_node;
  OLIVE_ASSERT(blur_node.id() == QStringLiteral("org.olivevideoeditor.Olive.blur"));

  // Verify that ShaderCode retrieves blur.frag
  Node::ShaderRequest req(QStringLiteral("main"));
  ShaderCode code = blur_node.GetShaderCode(req);
  OLIVE_ASSERT(!code.frag_code().isEmpty());
  OLIVE_ASSERT(code.frag_code().contains("determine_mode"));

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(DropShadowAndStrokeShaderCompile)
{
  int argc = 1;
  char arg0[] = "compositing-tests";
  char* argv[] = {arg0, nullptr};
  if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
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
  auto* f = ctx.functions();

  static const QString header = QStringLiteral("#version 150\nprecision highp float;\n");
  QFile vert_file(QStringLiteral(":/shaders/default.vert"));
  OLIVE_ASSERT(vert_file.open(QFile::ReadOnly));
  QString vert_src = header + QString::fromUtf8(vert_file.readAll());
  vert_file.close();

  const char* frags[] = {":/shaders/dropshadow.frag", ":/shaders/stroke.frag", nullptr};
  for (int i = 0; frags[i]; ++i) {
    QFile frag_file(QString::fromUtf8(frags[i]));
    OLIVE_ASSERT(frag_file.open(QFile::ReadOnly));
    QString frag_src = header + QString::fromUtf8(frag_file.readAll());
    frag_file.close();

    GLuint vs = f->glCreateShader(GL_VERTEX_SHADER);
    QByteArray vert_utf8 = vert_src.toUtf8();
    const char* vert_cstr = vert_utf8.constData();
    f->glShaderSource(vs, 1, &vert_cstr, nullptr);
    f->glCompileShader(vs);

    GLuint fs = f->glCreateShader(GL_FRAGMENT_SHADER);
    QByteArray frag_utf8 = frag_src.toUtf8();
    const char* frag_cstr = frag_utf8.constData();
    f->glShaderSource(fs, 1, &frag_cstr, nullptr);
    f->glCompileShader(fs);

    GLint vs_ok = 0, fs_ok = 0;
    f->glGetShaderiv(vs, GL_COMPILE_STATUS, &vs_ok);
    f->glGetShaderiv(fs, GL_COMPILE_STATUS, &fs_ok);
    OLIVE_ASSERT(vs_ok == GL_TRUE);
    OLIVE_ASSERT(fs_ok == GL_TRUE);

    GLuint prog = f->glCreateProgram();
    f->glAttachShader(prog, vs);
    f->glAttachShader(prog, fs);
    f->glLinkProgram(prog);
    GLint link_ok = 0;
    f->glGetProgramiv(prog, GL_LINK_STATUS, &link_ok);
    OLIVE_ASSERT(link_ok == GL_TRUE);

    f->glDeleteProgram(prog);
    f->glDeleteShader(vs);
    f->glDeleteShader(fs);
  }

  OLIVE_TEST_END;
}

}
