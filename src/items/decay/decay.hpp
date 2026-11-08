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

#pragma once

#include <mutex>

class Item;

class Decay {
public:
	Decay() = default;

	Decay(const Decay &) = delete;
	void operator=(const Decay &) = delete;

	static Decay &getInstance();

	void startDecay(const std::shared_ptr<Item> &item, int32_t duration);
	void stopDecay(const std::shared_ptr<Item> &item, int64_t timestamp);

private:
	void checkDecay();
	static void internalDecayItem(const std::shared_ptr<Item> &item);

	// The bodies of startDecay/stopDecay minus their argument checks. The caller
	// must ALREADY hold decayMutex. They exist so startDecay can reach stopDecay's
	// logic without releasing and retaking the lock in between.
	void startDecayLocked(const std::shared_ptr<Item> &item, int32_t duration);
	void stopDecayLocked(const std::shared_ptr<Item> &item, int64_t timestamp);

	// decayMap is global mutable state reached from the PARALLEL dispatcher
	// groups, not only from the dispatcher thread: Map::getTile() ->
	// MapCache::getOrCreateTileFromCache() calls Item::startDecaying() on every
	// item of a tile it materialises, and that runs inside
	// Dispatcher::executeParallelEvents (async pathfinding, sight lines, creature
	// placement) on the thread pool. Without this lock two threads push_back into
	// the same std::vector and one reallocates the buffer the other is still
	// moving out of -- an access violation inside std::_Uninitialized_move --
	// while concurrent operator[] can corrupt the std::map's tree as well.
	//
	// checkDecay() must NOT hold this across internalDecayItem():
	// transformItem/internalRemoveItem re-enter startDecay and would self-deadlock
	// on a non-recursive mutex.
	std::mutex decayMutex;

	uint32_t eventId { 0 };
	// order is important, so we use an std::map
	std::map<int64_t, std::vector<std::shared_ptr<Item>>> decayMap;
};

constexpr auto g_decay = Decay::getInstance;
