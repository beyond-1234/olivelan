# Olive - Non-Linear Video Editor
# Copyright (C) 2022 Olive Team
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

olive_add_test(Codec color-golden-tests color-golden-tests.cpp)
olive_add_test(Codec image-export-tests image-export-tests.cpp)
olive_add_test(Codec frame-cache-tests frame-cache-tests.cpp)

# Patch 0056: the only audio coverage in the suite. Drives FFmpegEncoder and FFmpegDecoder
# end to end through a real .wav round trip, which is what exercises the two swr_alloc_set_opts2
# call sites from patch 0054 (encoder E2, decoder D3).
olive_add_test(Codec audio-conform-tests audio-conform-tests.cpp)

# Patch 0058: interlaced encode -> FFmpegDecoder::Probe() round trip. Also the regression gate for
# the top-field-first fix in FFmpegEncoder (requested tff used to be written as bff).
olive_add_test(Codec interlace-tests interlace-tests.cpp)

# The Golden snapshots live in the source tree (tests/golden) so they are
# version-controlled and reviewable, not regenerated per build directory.
target_compile_definitions(
  color-golden-tests
  PRIVATE OLIVE_TEST_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden"
)
