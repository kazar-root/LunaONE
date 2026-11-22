// SPDX-License-Identifier: GPL-3.0-only

#include "library_sections.hpp"
#include "core/i18n.hpp"
#include "luna/entitlement_policy.hpp"
#include "luna/recent_games.hpp"

#include <algorithm>
#include <iterator>
#include <ranges>

namespace lunanx::ui
{
namespace
{
std::vector<luna::Game> TopGames(const std::vector<luna::Shelf>& shelves)
{
    auto ranking = std::ranges::find(shelves, "RANKINGS_ROW", &luna::Shelf::row_type);
    if (ranking == shelves.end())
        ranking = std::ranges::find_if(shelves, [](const auto& shelf) {
            return luna::NormalizedGameTitle(shelf.title).find("top") != std::string::npos;
        });
    std::vector<luna::Game> top;
    std::set<std::string> seen;
    if (ranking != shelves.end())
        for (const auto& game : ranking->games)
        {
            if (top.size() == 10)
                break;
            if (!game.title.empty() && luna::InsertGameKeys(seen, game))
                top.push_back(game);
        }
    return top;
}
}

LibraryGamePresentation PresentLibraryGame(const luna::Game& game, luna::AccountPlan plan)
{
    LibraryGamePresentation presentation;
    presentation.cover = game.image_portrait.empty() ? game.image_landscape : game.image_portrait;
    presentation.backdrop = game.image_landscape.empty() ? game.image_portrait : game.image_landscape;
    const auto append = [&](const std::string& value) {
        if (value.empty()) return;
        if (!presentation.metadata.empty()) presentation.metadata += "  ·  ";
        presentation.metadata += value;
    };
    append(game.publisher);
    for (const auto& genre : game.genres | std::views::take(2)) append(genre);
    append(game.age_rating);
    presentation.locked = !luna::IsIncludedInPlan(game, plan);
    presentation.included = !game.premium_only && plan != luna::AccountPlan::Unknown;
    if (game.premium_only)
        presentation.offer = Tr(presentation.locked ? "Nécessite Luna Premium" : "Inclus avec Luna Premium");
    else if (presentation.included)
        presentation.offer = Tr("Inclus dans ton abonnement");
    else
        presentation.offer = game.offer_tag;
    return presentation;
}

std::optional<luna::Game> FeaturedGame(const std::vector<luna::Shelf>& shelves)
{
    for (const auto& shelf : shelves)
        if (shelf.row_type.find("FEATURED") != std::string::npos)
            for (const auto& game : shelf.games)
                if (!game.title.empty() && !game.image_landscape.empty())
                    return game;
    for (const auto& game : TopGames(shelves))
        if (!game.image_landscape.empty())
            return game;
    return std::nullopt;
}

std::vector<luna::Game> LibraryGames(LibrarySection section, const std::vector<luna::Shelf>& shelves,
                                   const std::vector<luna::Game>& catalog, luna::AccountPlan plan)
{
    if (section == LibrarySection::Top)
        return TopGames(shelves);
    if (section == LibrarySection::Recent)
        return luna::RecentGamesForShelves(shelves, luna::LoadRecentGames(), catalog);
    if (section == LibrarySection::Premium)
    {
        std::vector<luna::Game> premium;
        std::ranges::copy_if(catalog, std::back_inserter(premium), &luna::Game::premium_only);
        return premium;
    }
    // Every game of the catalog: those the plan includes first, then the
    // others, shown locked, so that the count matches Luna's.
    auto ordered = catalog;
    std::ranges::stable_partition(ordered, [plan](const auto& game) { return luna::IsIncludedInPlan(game, plan); });
    if (const auto featured = FeaturedGame(shelves))
    {
        auto found = std::ranges::find_if(ordered, [&](const auto& game) {
            return (!featured->asin.empty() && game.asin == featured->asin) ||
                   luna::NormalizedGameTitle(game.title) == luna::NormalizedGameTitle(featured->title);
        });
        if (found != ordered.end())
            std::rotate(ordered.begin(), found, found + 1);
    }
    return ordered;
}
}
