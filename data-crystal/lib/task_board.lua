-- Winter Update 2025 — Hunting Task Shop offers (consumed by IOWeeklyTasks::initializeShopOffers)
-- Adjust or replace with your datapack; structure must match C++ reader in src/io/ioweeklytasks.cpp
HUNT_TASK_SHOP_OFFERS = {
	[1] = {
		offerType = 0, -- HUNTING_SHOP_OFFER_ITEM
		name = "Crystal Coin",
		description = "Placeholder shop offer — replace in task_board.lua",
		looktypeOrItemId = 2160,
		price = 50,
	},
}

-- Items eligible for weekly delivery tasks (itemId + count per stack definition)
WEEKLY_DELIVERY_ITEMS = {
	[1] = { itemId = 2160, count = 100 },
	[2] = { itemId = 2148, count = 100 },
	[3] = { itemId = 2120, count = 20 },
}
