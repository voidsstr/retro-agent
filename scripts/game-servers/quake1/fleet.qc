/*
	fleet.qc - NSC retro fleet: keep the NetQuake server backfilled with
	FrikBot X bots. Compiled into the server's progs.dat by build-frikbot.sh.

	The fleet rule is "3 bots, Hard, backfilling": an empty server shows
	FLEET_BOTS bots, and each human who joins replaces one (a bot is kicked
	when humans + bots would exceed FLEET_BOTS, and added back when a human
	leaves). FrikBot's skill scale is Quake's own 0..3 - 0 Easy, 1 Normal,
	2 Hard, 3 Nightmare - so 2 is "Hard" by name.

	Runs from StartFrame, at most one add or kick every 3 seconds, and stays
	out of the way while FrikBot is still re-seating the bots it carried over
	from the previous map (saved_bots).
*/

float FLEET_BOTS  = 3;
float FLEET_SKILL = 2;
float fleet_next;

void() FleetBotFrame =
{
	local float humans, num;
	local entity e;

	if (time < fleet_next)
		return;
	fleet_next = time + 3;
	if (!deathmatch || saved_bots || max_clients < 2)
		return;

	humans = 0;
	num = 0;
	e = nextent(world);
	while (num < max_clients)
	{
		if ((active_clients & ClientBitFlag(num)) && e.ishuman == TRUE)
			humans = humans + 1;
		e = nextent(e);
		num = num + 1;
	}

	if (humans + bot_count < FLEET_BOTS)
		BotConnect(0, 0, FLEET_SKILL);
	else if (bot_count > 0 && humans + bot_count > FLEET_BOTS)
		KickABot();
};
