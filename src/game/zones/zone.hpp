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
#include "game/movement/position.hpp"
#include "items/item.hpp"
#include "creatures/creature.hpp"

class Tile;
class Creature;
class Monster;
class Player;
class Npc;
class Item;
class Thing;

struct Area {
	constexpr Area() = default;
	constexpr Area(Position from, Position to) :
		from(from), to(to) { }

	static bool intersects(Area a, Area b) {
		return a.from.x <= b.to.x && a.to.x >= b.from.x && a.from.y <= b.to.y && a.to.y >= b.from.y && a.from.z <= b.to.z && a.to.z >= b.from.z;
	}

	bool intersects(Area other) const {
		return intersects(*this, other);
	}

	bool contains(Position position) const {
		return position.x >= from.x && position.x <= to.x && position.y >= from.y && position.y <= to.y && position.z >= from.z && position.z <= to.z;
	}

	Position from;
	Position to;

	std::string toString() const {
		return fmt::format("Area(from: {}, to: {})", from.toString(), to.toString());
	}

	class PositionIterator {
	public:
		PositionIterator(Position startPosition, const Area &refArea) :
			currentPosition(startPosition), area(refArea) { }

		const Position &operator*() const {
			return currentPosition;
		}
		PositionIterator &operator++() {
			currentPosition.x++;
			if (currentPosition.x > area.to.x) {
				currentPosition.x = area.from.x;
				currentPosition.y++;
				if (currentPosition.y > area.to.y) {
					currentPosition.y = area.from.y;
					currentPosition.z++;
				}
			}
			return *this;
		}
		bool operator!=(const PositionIterator &other) const {
			return !(currentPosition == other.currentPosition);
		}

	private:
		Position currentPosition;
		const Area &area;
	};

	PositionIterator begin() const {
		return PositionIterator(from, *this);
	}

	PositionIterator end() const {
		Position endPosition(from.x, from.y, to.z + 1); // z is incremented so it's past the last valid position.
		return PositionIterator(endPosition, *this);
	}
};

namespace weak {
	template <typename T>
	struct ThingHasher {
		std::size_t operator()(std::weak_ptr<T> thing) const {
			const auto locked = thing.lock();
			if (!locked) {
				return 0;
			}
			return std::hash<void*> {}(locked.get());
		}
	};

	template <typename T>
	struct ThingComparator {
		bool operator()(const std::weak_ptr<T> &lhs, const std::weak_ptr<T> &rhs) const {
			return lhs.lock() == rhs.lock();
		}
	};

	template <>
	struct ThingHasher<Creature> {
		std::size_t operator()(const std::weak_ptr<Creature> &weakCreature) const {
			auto locked = weakCreature.lock();
			if (!locked) {
				return 0;
			}
			return std::hash<uint32_t> {}(locked->getID());
		}
	};

	template <>
	struct ThingComparator<Creature> {
		bool operator()(const std::weak_ptr<Creature> &lhs, const std::weak_ptr<Creature> &rhs) const {
			// Lock once; calling expired() then lock() is a TOCTOU race: the
			// object can be destroyed between the two calls, making lock()
			// return null and the subsequent ->getID() dereference null.
			const auto lockedLhs = lhs.lock();
			const auto lockedRhs = rhs.lock();
			if (!lockedLhs || !lockedRhs) {
				return false;
			}
			return lockedLhs->getID() == lockedRhs->getID();
		}
	};

	template <typename T>
	using set = std::unordered_set<std::weak_ptr<T>, ThingHasher<T>, ThingComparator<T>>;

	template <typename T>
	std::vector<std::shared_ptr<T>> lock(set<T> &weakSet) {
		std::vector<std::shared_ptr<T>> result;
		for (auto it = weakSet.begin(); it != weakSet.end();) {
			if (it->expired()) {
				it = weakSet.erase(it);
			} else {
				result.push_back(it->lock());
				++it;
			}
		}
		return result;
	}

	// Pointer-keyed cache. Hashing/equality use the raw pointer key and never
	// touch the weak_ptr control block, so insert/erase/rehash are safe even if
	// the pointee is being destroyed on another thread. lock() (which reads the
	// control block) happens only here, after the map operation, on a private
	// copy. This mirrors Zone::itemsCache, which uses the same pattern and does
	// not suffer the weak_ptr-in-hash use-after-free crash.
	template <typename T>
	using map = std::unordered_map<const T*, std::weak_ptr<T>>;

	template <typename T>
	std::vector<std::shared_ptr<T>> lockMap(map<T> &weakMap) {
		std::vector<std::shared_ptr<T>> result;
		result.reserve(weakMap.size());
		for (auto it = weakMap.begin(); it != weakMap.end();) {
			if (auto locked = it->second.lock()) {
				result.push_back(std::move(locked));
				++it;
			} else {
				it = weakMap.erase(it);
			}
		}
		return result;
	}
}

class Zone {
public:
	explicit Zone(std::string name, uint32_t id = 0) :
		name(std::move(name)), id(id) { }
	explicit Zone(uint32_t id) :
		id(id) { }

	// Deleted copy constructor and assignment operator.
	Zone(const Zone &) = delete;
	Zone &operator=(const Zone &) = delete;

	const std::string &getName() const {
		return name;
	}
	void addArea(Area area);
	void subtractArea(Area area);
	void addPosition(const Position &position) {
		positions.emplace(position);
	}
	void removePosition(const Position &position) {
		positions.erase(position);
	}
	Position getRemoveDestination(const std::shared_ptr<Creature> &creature = nullptr) const;
	void setRemoveDestination(const Position &position) {
		removeDestination = position;
	}

	std::vector<Position> getPositions() const;
	std::vector<std::shared_ptr<Creature>> getCreatures();
	std::vector<std::shared_ptr<Player>> getPlayers();
	std::vector<std::shared_ptr<Monster>> getMonsters();
	std::vector<std::shared_ptr<Npc>> getNpcs();
	std::vector<std::shared_ptr<Item>> getItems();

	void creatureAdded(const std::shared_ptr<Creature> &creature);
	void creatureRemoved(const std::shared_ptr<Creature> &creature);
	void thingAdded(const std::shared_ptr<Thing> &thing);
	void itemAdded(const std::shared_ptr<Item> &item);
	void itemRemoved(const std::shared_ptr<Item> &item);

	void removePlayers();
	void removeMonsters();
	void removeNpcs();

	void refresh();

	void setMonsterVariant(const std::string &variant);
	const std::string &getMonsterVariant() const {
		return monsterVariant;
	}

	bool isStatic() const {
		return id != 0;
	}

	static std::shared_ptr<Zone> addZone(const std::string &name, uint32_t id = 0);
	static std::shared_ptr<Zone> getZone(const std::string &name);
	static std::shared_ptr<Zone> getZone(uint32_t id);
	static std::vector<std::shared_ptr<Zone>> getZones(Position position);
	static std::vector<std::shared_ptr<Zone>> getZones();
	static void refreshAll() {
		for (const auto &[_, zone] : zones) {
			zone->refresh();
		}
	}
	static void clearZones();

	static bool loadFromXML(const std::string &fileName, uint16_t shiftID = 0);

protected:
	bool contains(const Position &position) const;

	Position removeDestination = Position();
	std::string name;
	std::string monsterVariant;
	std::unordered_set<Position> positions;
	uint32_t id = 0; // ID 0 is used in zones created dynamically from lua. The map editor uses IDs starting from 1 (automatically generated).

	mutable std::mutex cacheMutex;
	std::unordered_map<const Item*, std::weak_ptr<Item>> itemsCache;
	weak::map<Creature> creaturesCache;
	weak::map<Monster> monstersCache;
	weak::map<Npc> npcsCache;
	weak::map<Player> playersCache;

	static phmap::parallel_flat_hash_map<std::string, std::shared_ptr<Zone>> zones;
	static phmap::parallel_flat_hash_map<uint32_t, std::shared_ptr<Zone>> zonesByID;
};
