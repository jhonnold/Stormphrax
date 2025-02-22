/*
 * Stormphrax, a UCI chess engine
 * Copyright (C) 2023 Ciekce
 *
 * Stormphrax is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Stormphrax is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Stormphrax. If not, see <https://www.gnu.org/licenses/>.
 */

#include "wdl.h"

#include <array>
#include <numeric>

namespace stormphrax::wdl
{
	auto winRateModel(Score povScore, u32 ply) -> std::pair<i32, i32>
	{
		constexpr auto As = std::array {
			4.64264993, -21.12065376, 56.21802732, 184.04136881
		};
		constexpr auto Bs = std::array {
			-1.53902952, 17.05344734, -51.89919098, 99.96350373
		};

		static_assert(NormalizationK == static_cast<i32>(std::reduce(As.begin(), As.end())));

		const auto m = std::min(240.0, static_cast<f64>(ply)) / 64.0;

		const auto a = (((As[0] * m + As[1]) * m + As[2]) * m) + As[3];
		const auto b = (((Bs[0] * m + Bs[1]) * m + Bs[2]) * m) + Bs[3];

		const auto x = std::clamp(static_cast<f64>(povScore), -4000.0, 4000.0);

		return {
			static_cast<i32>(std::round(1000.0 / (1.0 + std::exp((a - x) / b)))),
			static_cast<i32>(std::round(1000.0 / (1.0 + std::exp((a + x) / b))))
		};
	}
}
