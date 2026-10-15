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

olive_add_test(General common-tests common-tests.cpp)
olive_add_test(General color-role-tests color-role-tests.cpp)
olive_add_test(General color-hdr-metadata-tests color-hdr-metadata-tests.cpp)
olive_add_test(General cubelut-tests cubelut-tests.cpp)
olive_add_test(General color-equivalence-tests color-equivalence-tests.cpp)
olive_add_test(General legacy-serializer-tests legacy-serializer-tests.cpp)
olive_add_test(General hal-honesty-tests hal-honesty-tests.cpp)
olive_add_test(General vaapi-composite-tests vaapi-composite-tests.cpp)
