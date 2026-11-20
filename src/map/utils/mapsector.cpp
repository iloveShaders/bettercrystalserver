////////////////////////////////////////////////////////////////////////
// Crystal Server - an opensource roleplaying game
////////////////////////////////////////////////////////////////////////
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
////////////////////////////////////////////////////////////////////////

#include "map/utils/mapsector.hpp"

#include "creatures/creature.hpp"

bool MapSector::newSector = false;

void MapSector::addCreature(const std::shared_ptr<Creature> &c) {
	creature_list.emplace_back(c);
	if (c->getPlayer()) {
		player_list.emplace_back(c);
	} else if (c->getMonster()) {
		monster_list.emplace_back(c);
	} else if (c->getNpc()) {
		npc_list.emplace_back(c);
	}
}

namespace {
	// Swap-and-pop removal. Returns false when the creature was not in the list.
	bool eraseCreatureFromList(std::vector<std::shared_ptr<Creature>> &list, const std::shared_ptr<Creature> &c) {
		auto iter = std::ranges::find(list, c);
		if (iter == list.end()) {
			return false;
		}

		*iter = list.back();
		list.pop_back();
		return true;
	}
} // namespace

void MapSector::removeCreature(const std::shared_ptr<Creature> &c) {
	if (!c) {
		return;
	}

	const bool inCreatureList = eraseCreatureFromList(creature_list, c);

	// Never short-circuit on a miss. All four lists hold shared_ptr, and Spectators (a
	// friend of this class) reads player_list / monster_list / npc_list directly, so
	// returning early after a failed creature_list lookup used to leave a phantom entry
	// pinned in the typed list for the rest of uptime: never freed, and still handed out
	// by spectator lookups. Each list is now cleaned independently of the others.
	bool inTypedList = true;
	const char* typedListName = "creature_list";
	if (c->getPlayer()) {
		typedListName = "player_list";
		inTypedList = eraseCreatureFromList(player_list, c);
	} else if (c->getMonster()) {
		typedListName = "monster_list";
		inTypedList = eraseCreatureFromList(monster_list, c);
	} else if (c->getNpc()) {
		typedListName = "npc_list";
		inTypedList = eraseCreatureFromList(npc_list, c);
	}

	if (!inCreatureList || !inTypedList) {
		// Either this creature is registered in a different sector than the one its
		// current tile belongs to, or it is being de-registered twice (Game::removeCreature
		// only sets isRemoved() at the very end, so Lua running in onRemoveCreature or in a
		// STEP_OUT move event can re-enter the whole removal). The old message named
		// neither the creature nor the place, which made it untrackable.
		g_logger().error(
			"[{}]: '{}' not found in this sector (creature_list: {}, {}: {}) at position '{}'",
			__FUNCTION__, c->getName(), inCreatureList, typedListName, inTypedList, c->getPosition().toString()
		);
	}
}
