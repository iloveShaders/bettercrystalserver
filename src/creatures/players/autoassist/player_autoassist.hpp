////////////////////////////////////////////////////////////////////////
// Crystal Server - an MMORPG server application
////////////////////////////////////////////////////////////////////////

#pragma once

#include <memory>

class Player;

// Bot-like auto-heal / auto-potion, driven from Player::onThink (1x/sec per player).
//
// Design: we never reimplement spells or potions. When a configured threshold is
// crossed we invoke the *real* self-heal cast (InstantSpell::playerCastInstant +
// Player::saySpell) and the *real* potion action (Actions::useItemEx -> potions.lua).
// That means the live formula, mana cost, spell/group cooldown, magic effect, flask
// return, potion exhaust, drink sound, supply tracker, achievements AND per-observer
// spell-emote / client "show spells" handling are all inherited for free and can
// never drift out of sync.
//
// Player-facing config lives in storages set by the !autohealing / !autopotion
// talkactions (see data/scripts/talkactions/player/). Nothing is registered as a
// creature event; this runs entirely from the think tick.
namespace AutoAssist {
	// Evaluate and, if needed, cast a heal and/or drink a potion for this player.
	void check(const std::shared_ptr<Player> &player);
}
