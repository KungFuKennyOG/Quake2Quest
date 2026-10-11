/*
 * sofinv.cpp - asks the game's own inventory object what the player is carrying (for the
 * VR weapon wheel). Compiled against the SDK's g_local.h with the game's defines, in a
 * file of its own so the SDK's declarations stay out of the adapter.
 */
#include "g_local.h"

extern "C" int sofinv_hasWeapon(void *client, int type)
{
	gclient_t *c = (gclient_t *)client;
	if (!c || !c->inv) return 0;
	return c->inv->hasWeaponType(type);
}

extern "C" int sofinv_clipMax(void *client, int type)
{
	gclient_t *c = (gclient_t *)client;
	if (!c || !c->inv) return 0;
	return c->inv->getClipMaxByType(type);
}

extern "C" void sofinv_takeOutWeapon(void *client, int type)
{
	gclient_t *c = (gclient_t *)client;
	if (c && c->inv) c->inv->takeOutWeapon(type);
}
