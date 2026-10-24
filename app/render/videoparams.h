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

#ifndef VIDEOPARAMS_H
#define VIDEOPARAMS_H

#include <olive/core/core.h>
#include <QVector2D>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

namespace olive {

using namespace core;

class VideoParams {
public:
  enum Interlacing {
    kInterlaceNone,
    kInterlacedTopFirst,
    kInterlacedBottomFirst
  };

  enum Type {
    kVideoTypeVideo,
    kVideoTypeStill,
    kVideoTypeImageSequence
  };
  enum ColorRange
  {
    kColorRangeLimited,   // 16_235
    kColorRangeFull,      // 0-255
    kColorRangeAuto,      // Follow frame metadata, fallback to limited

    kColorRangeDefault = kColorRangeAuto
  };

  /**
   * @brief Patch 0042 (Phase 8): container colour metadata.
   *
   * These mirror the colour description FFmpeg stores in the container
   * (AVColorPrimaries / AVColorTransferCharacteristic / AVColorSpace). VideoParams
   * deliberately does not include FFmpeg headers, so the values travel as plain
   * ints holding the FFmpeg constants - they are part of FFmpeg's stable ABI and
   * are exactly what gets serialised into the project file.
   *
   * NOTE: color_space() is the container's YUV matrix coefficients, which is NOT
   * the same thing as colorspace(). The latter holds the *name* of the colour
   * space ("Rec.709 OETF", "Rec.2020 HLG", ...) that the colour manager works in.
   */
  enum ContainerColorPrimaries
  {
    kColorPrimariesUnspecified = 2, // AVCOL_PRI_UNSPECIFIED
    kColorPrimariesBt709 = 1,       // AVCOL_PRI_BT709
    kColorPrimariesBt2020 = 9,      // AVCOL_PRI_BT2020

    kColorPrimariesDefault = kColorPrimariesUnspecified
  };

  enum ContainerTransferCharacteristic
  {
    kTransferUnspecified = 2, // AVCOL_TRC_UNSPECIFIED
    kTransferBt709 = 1,       // AVCOL_TRC_BT709
    kTransferSmpte2084 = 16,  // AVCOL_TRC_SMPTE2084 (PQ / HDR10)
    kTransferAribStdB67 = 18, // AVCOL_TRC_ARIB_STD_B67 (HLG)

    kTransferDefault = kTransferUnspecified
  };

  enum ContainerColorSpace
  {
    kColorSpaceUnspecified = 2, // AVCOL_SPC_UNSPECIFIED
    kColorSpaceBt709 = 1,       // AVCOL_SPC_BT709
    kColorSpaceBt2020Ncl = 9,   // AVCOL_SPC_BT2020_NCL
    kColorSpaceBt2020Cl = 10,   // AVCOL_SPC_BT2020_CL

    kColorSpaceDefault = kColorSpaceUnspecified
  };


  VideoParams();
  VideoParams(int width, int height, PixelFormat format, int nb_channels,
              const rational& pixel_aspect_ratio = 1,
              Interlacing interlacing = kInterlaceNone, int divider = 1);
  VideoParams(int width, int height, int depth,
              PixelFormat format, int nb_channels,
              const rational& pixel_aspect_ratio = 1,
              Interlacing interlacing = kInterlaceNone, int divider = 1);
  VideoParams(int width, int height, const rational& time_base,
              PixelFormat format, int nb_channels,
              const rational& pixel_aspect_ratio = 1,
              Interlacing interlacing = kInterlaceNone, int divider = 1);

  int width() const
  {
    return width_;
  }

  void set_width(int width)
  {
    width_ = width;
    calculate_effective_size();
  }

  /**
   * @brief Returns width multiplied by pixel aspect ratio where applicable
   */
  int square_pixel_width() const
  {
    return par_width_;
  }

  QVector2D resolution() const
  {
    return QVector2D(width_, height_);
  }

  QVector2D square_resolution() const
  {
    return QVector2D(par_width_, height_);
  }

  int height() const
  {
    return height_;
  }

  void set_height(int height)
  {
    height_ = height;
    calculate_effective_size();
  }

  int depth() const
  {
    return depth_;
  }

  void set_depth(int depth)
  {
    depth_ = depth;
    calculate_effective_size();
  }

  bool is_3d() const { return depth_ > 1; }

  const rational& time_base() const
  {
    return time_base_;
  }

  void set_time_base(const rational& r)
  {
    time_base_ = r;
  }

  rational frame_rate_as_time_base() const
  {
    return frame_rate_.flipped();
  }

  int divider() const
  {
    return divider_;
  }

  void set_divider(int d)
  {
    divider_ = d;
    calculate_effective_size();
  }

  int effective_width() const
  {
    return effective_width_;
  }

  int effective_height() const
  {
    return effective_height_;
  }

  int effective_depth() const
  {
    return effective_depth_;
  }

  PixelFormat format() const
  {
    return format_;
  }

  void set_format(PixelFormat f)
  {
    format_ = f;
  }

  int channel_count() const
  {
    return channel_count_;
  }

  void set_channel_count(int c)
  {
    channel_count_ = c;
  }

  const rational& pixel_aspect_ratio() const
  {
    return pixel_aspect_ratio_;
  }

  void set_pixel_aspect_ratio(const rational& r)
  {
    pixel_aspect_ratio_ = r;
    validate_pixel_aspect_ratio();
  }

  Interlacing interlacing() const
  {
    return interlacing_;
  }

  void set_interlacing(Interlacing i)
  {
    interlacing_ = i;
  }

  static int generate_auto_divider(qint64 width, qint64 height);

  bool is_valid() const;

  bool operator==(const VideoParams& rhs) const;
  bool operator!=(const VideoParams& rhs) const;

  static int GetBytesPerChannel(PixelFormat format);
  int GetBytesPerChannel() const
  {
    return GetBytesPerChannel(format_);
  }

  static int GetBytesPerPixel(PixelFormat format, int channels);
  int GetBytesPerPixel() const
  {
    return GetBytesPerPixel(format_, channel_count_);
  }

  static int GetBufferSize(int width, int height, PixelFormat format, int channels)
  {
    return width * height * GetBytesPerPixel(format, channels);
  }
  int GetBufferSize() const
  {
    return GetBufferSize(width_, height_, format_, channel_count_);
  }

  static QString GetNameForDivider(int div);

  static bool FormatIsFloat(PixelFormat format)
  {
    return format.is_float();
  }

  static QString GetFormatName(PixelFormat format);

  static int GetDividerForTargetResolution(int src_width, int src_height, int dst_width, int dst_height);

  static const int kInternalChannelCount;

  static const rational kPixelAspectSquare;
  static const rational kPixelAspectNTSCStandard;
  static const rational kPixelAspectNTSCWidescreen;
  static const rational kPixelAspectPALStandard;
  static const rational kPixelAspectPALWidescreen;
  static const rational kPixelAspect1080Anamorphic;

  static const QVector<rational> kSupportedFrameRates;
  static const QVector<rational> kStandardPixelAspects;
  static const QVector<int> kSupportedDividers;

  static const int kHSVChannelCount = 3;
  static const int kRGBChannelCount = 3;
  static const int kRGBAChannelCount = 4;

  /**
   * @brief Convert rational frame rate (i.e. flipped timebase) to a user-friendly string
   */
  static QString FrameRateToString(const rational& frame_rate);

  static QStringList GetStandardPixelAspectRatioNames();
  static QString FormatPixelAspectRatioString(const QString& format, const rational& ratio);

  static int GetScaledDimension(int dim, int divider);

  bool enabled() const
  {
    return enabled_;
  }

  void set_enabled(bool e)
  {
    enabled_ = e;
  }

  bool is_opaque() const
  {
    return is_opaque_ || channel_count_ < kRGBAChannelCount;
  }

  void set_opaque(bool o)
  {
    is_opaque_ = o;
  }

  float x() const { return x_; }
  void set_x(float x) { x_ = x; }
  float y() const { return y_; }
  void set_y(float y) { y_ = y; }
  QVector2D offset() const { return QVector2D(x_, y_); }

  int stream_index() const
  {
    return stream_index_;
  }

  void set_stream_index(int s)
  {
    stream_index_ = s;
  }

  Type video_type() const
  {
    return video_type_;
  }

  void set_video_type(Type t)
  {
    video_type_ = t;
  }

  const rational& frame_rate() const
  {
    return frame_rate_;
  }

  void set_frame_rate(const rational& frame_rate)
  {
    frame_rate_ = frame_rate;
  }

  int64_t start_time() const
  {
    return start_time_;
  }

  void set_start_time(int64_t start_time)
  {
    start_time_ = start_time;
  }

  int64_t duration() const
  {
    return duration_;
  }

  void set_duration(int64_t duration)
  {
    duration_ = duration;
  }

  bool premultiplied_alpha() const
  {
    return premultiplied_alpha_;
  }

  void set_premultiplied_alpha(bool premultiplied_alpha)
  {
    premultiplied_alpha_ = premultiplied_alpha;
  }

  const QString& colorspace() const
  {
    return colorspace_;
  }

  void set_colorspace(const QString& c)
  {
    colorspace_ = c;
  }

  const ColorRange &color_range() const { return color_range_; }
  void set_color_range(const ColorRange &color_range) { color_range_ = color_range; }

  /**
   * @brief Whether color_range() is an explicit user override.
   *
   * Imported streams always carry a range (the container's, or the range FFmpeg
   * derived while probing), so color_range() alone cannot distinguish "the user
   * picked this" from "this is what the file says". Only an explicit override may
   * beat the range on the decoded frame; otherwise the frame wins (patch 0104).
   */
  bool color_range_overridden() const { return color_range_overridden_; }
  void set_color_range_overridden(bool overridden) { color_range_overridden_ = overridden; }

  int color_primaries() const { return color_primaries_; }
  void set_color_primaries(int primaries) { color_primaries_ = primaries; }

  int color_trc() const { return color_trc_; }
  void set_color_trc(int trc) { color_trc_ = trc; }

  int color_space() const { return color_space_; }
  void set_color_space(int color_space) { color_space_ = color_space; }

  /**
   * @brief Peak luminance of the mastering display in cd/m2, or 0 when unknown.
   *
   * Populated from the container's mastering-display metadata when present. It
   * doubles as the manual fallback control ("HDR Peak Nits") for streams whose
   * container carries no metadata.
   */
  double mastering_peak_nits() const { return mastering_peak_nits_; }
  void set_mastering_peak_nits(double nits) { mastering_peak_nits_ = nits; }

  /**
   * @brief True when the stream carries HDR content that needs display mapping.
   *
   * Mirrors GpuEnvironmentProbe::NeedsToneMapping() so the CPU-side decision and
   * the GPU probe agree on a single rule: PQ, HLG, or a BT.2020 matrix.
   */
  bool needs_hdr_mapping() const;

  /**
   * @brief Map a container transfer characteristic to the built-in colour
   *        engine's HDR input colour space name.
   *
   * Returns "Rec.2020 PQ" / "Rec.2020 HLG" for PQ / HLG, and an empty string for
   * everything else - in which case the stream keeps whatever colour space the
   * project or the user already assigned.
   */
  static QString GetHdrColorSpaceForTransfer(int trc);

  /**
   * @brief Convert a mastering-display max-luminance value to cd/m2 (nits).
   *
   * ISO 14496-12 stores the field in units of 0.0001 cd/m2, but FFmpeg
   * normalises it while demuxing into a rational whose quotient already *is* the
   * luminance in cd/m2. A degenerate (den == 0) rational means "unknown" and is
   * reported as 0.
   */
  static double MasteringDisplayMaxLuminanceToNits(int64_t num, int64_t den);

  int64_t get_time_in_timebase_units(const rational& time) const;

  void Load(QXmlStreamReader* reader);

  void Save(QXmlStreamWriter* writer) const;

private:
  void calculate_effective_size();

  void validate_pixel_aspect_ratio();

  void set_defaults_for_footage();

  void calculate_square_pixel_width();

  int width_;
  int height_;
  int depth_;
  rational time_base_;

  PixelFormat format_;

  int channel_count_;

  rational pixel_aspect_ratio_;

  Interlacing interlacing_;

  int divider_;

  // Cached values
  int effective_width_;
  int effective_height_;
  int effective_depth_;
  int par_width_;

  bool enabled_;
  bool is_opaque_;
  int stream_index_;
  Type video_type_;
  rational frame_rate_;
  int64_t start_time_;
  int64_t duration_;
  bool premultiplied_alpha_;
  QString colorspace_;
  float x_;
  float y_;
  ColorRange color_range_;
  bool color_range_overridden_;
  int color_primaries_;
  int color_trc_;
  int color_space_;
  double mastering_peak_nits_;

};

}

Q_DECLARE_METATYPE(olive::VideoParams)
Q_DECLARE_METATYPE(olive::VideoParams::Interlacing)

#endif // VIDEOPARAMS_H
