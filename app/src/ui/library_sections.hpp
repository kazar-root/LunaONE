// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "luna/luna_models.hpp"

#include <optional>

namespace lunanx::ui
{
enum class LibrarySection { Top, Recent, All, Premium };

struct LibraryGamePresentation
{
    std::string cover;
    std::string backdrop;
    std::string offer;
    std::string metadata;
    bool included = false;
    bool locked = false;
};

LibraryGamePresentation PresentLibraryGame(const luna::Game& game, luna::AccountPlan plan);
std::optional<luna::Game> FeaturedGame(const std::vector<luna::Shelf>& shelves);
std::vector<luna::Game> LibraryGames(LibrarySection section, const std::vector<luna::Shelf>& shelves,
                                   const std::vector<luna::Game>& catalog,
                                   luna::AccountPlan plan = luna::AccountPlan::Unknown);
}
