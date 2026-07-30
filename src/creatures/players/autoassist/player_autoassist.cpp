////////////////////////////////////////////////////////////////////////
// Crystal Server - an MMORPG server application
////////////////////////////////////////////////////////////////////////

#include "creatures/players/autoassist/player_autoassist.hpp"

#include "creatures/players/player.hpp"
#include "creatures/players/vocations/vocation.hpp"
#include "creatures/combat/spells.hpp"
#include "lua/creature/actions.hpp"
#include "game/game.hpp"
#include "items/item.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace {

	// ---- Config storages -------------------------------------------------------
	// MUST match data/scripts/talkactions/player/autopotion.lua & autohealing.lua.
	constexpr int32_t STORAGE_POTION_ENABLED = 66000;
	constexpr int32_t STORAGE_POTION_THRESHOLD = 66001;
	constexpr int32_t STORAGE_HEAL_ENABLED = 66003;
	constexpr int32_t STORAGE_HEAL_THRESHOLD = 66004;

	constexpr int32_t DEFAULT_HEAL_THRESHOLD = 40; // percent of max HP
	constexpr int32_t DEFAULT_POTION_THRESHOLD = 30; // percent of max HP / mana

	// ---- Potion gate -----------------------------------------------------------
	// Base vocation ids come from data/XML/vocations.xml (baseid attribute):
	// 1=Sorcerer 2=Druid 3=Paladin 4=Knight 5=Monk. Same gate potions.lua applies.
	enum PotionVocMask : uint8_t {
		VOC_SORCERER = 1 << 0,
		VOC_DRUID = 1 << 1,
		VOC_PALADIN = 1 << 2,
		VOC_KNIGHT = 1 << 3,
		VOC_MONK = 1 << 4,
	};

	struct PotionEntry {
		uint16_t id;
		bool restoresHealth;
		bool restoresMana;
		uint32_t minLevel;
		uint8_t vocMask; // 0 = any vocation
		uint8_t tier; // higher = stronger; used to pick the best the player owns
	};

	// GATE ONLY (id / level / vocation / relative tier). Amounts and formulas live in
	// data/scripts/actions/items/potions.lua and must NOT be duplicated here. Keep this
	// table aligned with that script when a client bump introduces new potion tiers.
	constexpr std::array<PotionEntry, 12> POTIONS { {
		//  id     hp     mp    lvl  vocMask                                                tier
		{ 7876, true, false, 0, 0, 1 }, // small health potion
		{ 266, true, false, 0, 0, 2 }, // health potion
		{ 268, false, true, 0, 0, 2 }, // mana potion
		{ 237, false, true, 50, 0, 3 }, // strong mana potion
		{ 236, true, false, 50, VOC_PALADIN | VOC_KNIGHT | VOC_MONK, 4 }, // strong health potion
		{ 238, false, true, 80, VOC_SORCERER | VOC_DRUID | VOC_PALADIN | VOC_MONK, 5 }, // great mana potion
		{ 239, true, false, 80, VOC_KNIGHT, 5 }, // great health potion
		{ 7642, true, true, 80, VOC_PALADIN | VOC_MONK, 5 }, // great spirit potion
		{ 23373, false, true, 130, VOC_SORCERER | VOC_DRUID, 7 }, // ultimate mana potion
		{ 7643, true, false, 130, VOC_KNIGHT, 7 }, // ultimate health potion
		{ 23374, true, true, 130, VOC_PALADIN | VOC_MONK, 7 }, // ultimate spirit potion
		{ 23375, true, false, 200, VOC_KNIGHT, 9 }, // supreme health potion
	} };

	uint8_t baseVocToMask(uint8_t baseId) {
		switch (baseId) {
			case 1:
				return VOC_SORCERER;
			case 2:
				return VOC_DRUID;
			case 3:
				return VOC_PALADIN;
			case 4:
				return VOC_KNIGHT;
			case 5:
				return VOC_MONK;
			default:
				return 0;
		}
	}

	int32_t readThreshold(const std::shared_ptr<Player> &player, int32_t key, int32_t def) {
		const int32_t v = player->getStorageValue(key);
		return (v >= 1 && v <= 99) ? v : def;
	}

	// ---- Heal candidate list ---------------------------------------------------
	// All HP-restoring, self-target healing instants known to the engine, strongest
	// first. Built once: spells are loaded before any player thinks and never change
	// at runtime, and onThink runs on the single game thread (no locking needed).
	// We keep only the "exura" line (the cures share group=healing + selfTarget but
	// restore no HP; their "exana" prefix filters them out cleanly).
	const std::vector<std::shared_ptr<InstantSpell>> &getSelfHealSpells() {
		static const std::vector<std::shared_ptr<InstantSpell>> cache = [] {
			std::vector<std::shared_ptr<InstantSpell>> list;
			for (const auto &[words, spell] : g_spells().getInstantSpells()) {
				if (spell && spell->getGroup() == SPELLGROUP_HEALING && spell->getSelfTarget()
				    && spell->getWords().rfind("exura", 0) == 0) {
					list.emplace_back(spell);
				}
			}
			std::sort(list.begin(), list.end(), [](const auto &a, const auto &b) {
				if (a->getLevel() != b->getLevel()) {
					return a->getLevel() > b->getLevel(); // higher level req ~ bigger heal
				}
				return a->getWords() > b->getWords(); // stable, deterministic tie-break
			});
			return list;
		}();
		return cache;
	}

	// Silent eligibility check that mirrors Spell::playerSpellCheck WITHOUT emitting the
	// cancel messages ("You are exhausted." etc.) that would otherwise spam every tick.
	bool canSilentlyCast(const std::shared_ptr<Player> &player, const std::shared_ptr<InstantSpell> &spell) {
		if (!spell->canCast(player)) { // vocation / learned
			return false;
		}
		if (player->getLevel() < spell->getLevel()) {
			return false;
		}
		if (spell->isPremium() && !player->isPremium()) {
			return false;
		}
		if (player->getMana() < spell->getManaCost(player)) {
			return false;
		}
		const auto group = spell->getGroup();
		const auto secondary = spell->getSecondaryGroup();
		if (player->hasCondition(CONDITION_SPELLGROUPCOOLDOWN, group)) {
			return false;
		}
		if (player->hasCondition(CONDITION_SPELLCOOLDOWN, spell->getSpellId())) {
			return false;
		}
		if (secondary != SPELLGROUP_NONE && player->hasCondition(CONDITION_SPELLGROUPCOOLDOWN, secondary)) {
			return false;
		}
		return true;
	}

	bool tryHeal(const std::shared_ptr<Player> &player) {
		const int32_t maxHp = player->getMaxHealth();
		if (maxHp <= 0) {
			return false;
		}
		const int32_t threshold = readThreshold(player, STORAGE_HEAL_THRESHOLD, DEFAULT_HEAL_THRESHOLD);
		if (static_cast<int64_t>(player->getHealth()) * 100 > static_cast<int64_t>(maxHp) * threshold) {
			return false; // above threshold, nothing to do
		}

		for (const auto &spell : getSelfHealSpells()) {
			if (!canSilentlyCast(player, spell)) {
				continue;
			}
			// Real cast: live formula, real mana, real cooldown, real effect.
			std::string param;
			if (spell->playerCastInstant(player, param, Position())) {
				// Say through the normal path so each observer's emote / client
				// "show spells" setting decides how the words appear to them.
				player->saySpell(TALKTYPE_SAY, spell->getWords(), false);
				return true;
			}
		}
		return false;
	}

	bool tryPotion(const std::shared_ptr<Player> &player) {
		// Silent cooldown gate (matches the engine's own pre-check for potion use).
		if (!player->canDoPotionAction() || player->walkExhausted()) {
			return false;
		}

		const int32_t maxHp = player->getMaxHealth();
		const int32_t maxMana = player->getMaxMana();
		const int32_t threshold = readThreshold(player, STORAGE_POTION_THRESHOLD, DEFAULT_POTION_THRESHOLD);

		const bool needHealth = maxHp > 0 && static_cast<int64_t>(player->getHealth()) * 100 < static_cast<int64_t>(maxHp) * threshold;
		const bool needMana = maxMana > 0 && static_cast<int64_t>(player->getMana()) * 100 < static_cast<int64_t>(maxMana) * threshold;
		if (!needHealth && !needMana) {
			return false;
		}

		const auto &voc = player->getVocation();
		if (!voc) {
			return false;
		}
		const uint8_t vocMask = baseVocToMask(voc->getBaseId());
		const uint32_t level = player->getLevel();

		// Highest-tier potion the player qualifies for AND actually owns, matching a predicate.
		const auto pickBest = [&](auto &&pred) -> std::shared_ptr<Item> {
			const PotionEntry* chosen = nullptr;
			for (const auto &p : POTIONS) {
				if (level < p.minLevel) {
					continue;
				}
				if (p.vocMask != 0 && (p.vocMask & vocMask) == 0) {
					continue;
				}
				if (!pred(p)) {
					continue;
				}
				if (chosen && p.tier <= chosen->tier) {
					continue;
				}
				if (g_game().findItemOfType(player, p.id, true)) {
					chosen = &p;
				}
			}
			return chosen ? g_game().findItemOfType(player, chosen->id, true) : nullptr;
		};

		std::shared_ptr<Item> item;
		if (needHealth && needMana) { // one drink can cover both -> prefer a spirit potion
			item = pickBest([](const PotionEntry &p) { return p.restoresHealth && p.restoresMana; });
		}
		if (!item && needHealth) {
			item = pickBest([](const PotionEntry &p) { return p.restoresHealth; });
		}
		if (!item && needMana) {
			item = pickBest([](const PotionEntry &p) { return p.restoresMana; });
		}
		if (!item) {
			return false;
		}

		// Drink through the real action (potions.lua) so amount, flask, exhaust, sound,
		// supply tracker and achievements all behave exactly like a manual drink on self.
		const auto &parent = player->getParent();
		uint8_t stackPos = 0;
		if (parent) {
			const int32_t idx = parent->getThingIndex(player);
			if (idx >= 0) {
				stackPos = static_cast<uint8_t>(idx);
			}
		}
		return g_actions().useItemEx(player, item->getPosition(), player->getPosition(), stackPos, item, false, player);
	}

} // namespace

void AutoAssist::check(const std::shared_ptr<Player> &player) {
	if (!player) {
		return;
	}
	// Heal (spell) first, then potion: use the free mana heal before burning supplies,
	// and never let a potion pre-empt a cheaper cast. Both may fire in the same tick,
	// exactly as a manual player can cast and drink within the same second.
	if (player->getStorageValue(STORAGE_HEAL_ENABLED) == 1) {
		tryHeal(player);
	}
	if (player->getStorageValue(STORAGE_POTION_ENABLED) == 1) {
		tryPotion(player);
	}
}
