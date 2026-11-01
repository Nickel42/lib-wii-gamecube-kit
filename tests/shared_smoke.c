/* SPDX-License-Identifier: GPL-2.0-or-later */

/* Plain C, linked against the shared library: the header works from C and the
 * DLL/.so/.dylib exports what it declares. */

#include <stdio.h>
#include <string.h>

#include "lib-wii-gamecube-kit.h"

/* The C# binding (bindings/csharp/LibWiiGameCubeKit/Native.cs) hard-codes these. */
#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFu
_Static_assert(sizeof(wii_gamecube_kit_disc_info) == 376, "wii_gamecube_kit_disc_info layout changed: update Native.cs");
_Static_assert(sizeof(wii_gamecube_kit_convert_options) == 48, "wii_gamecube_kit_convert_options layout changed: update Native.cs");
_Static_assert(sizeof(wii_gamecube_kit_hash_result) == 112, "wii_gamecube_kit_hash_result layout changed: update Native.cs");
#endif

int main(int argc, char **argv)
{
	wii_gamecube_kit_context *ctx = NULL;
	wii_gamecube_kit_disc_info info;
	wii_gamecube_kit_hash_result hash;

	if (argc < 2 || wii_gamecube_kit_abi_version() != WII_GAMECUBE_KIT_ABI_VERSION)
		return 1;
	if (wii_gamecube_kit_context_create(&ctx) != WII_GAMECUBE_KIT_OK)
		return 1;
	wii_gamecube_kit_disc_info_init(&info);
	if (wii_gamecube_kit_probe(ctx, argv[1], &info) != WII_GAMECUBE_KIT_OK || info.platform != WII_GAMECUBE_KIT_PLATFORM_GAMECUBE) {
		fprintf(stderr, "probe failed: %s\n", wii_gamecube_kit_last_error(ctx));
		return 1;
	}
	wii_gamecube_kit_hash_result_init(&hash);
	if (wii_gamecube_kit_hash(ctx, argv[1], &hash) != WII_GAMECUBE_KIT_OK || strlen(hash.sha1) != 40) {
		fprintf(stderr, "hash failed: %s\n", wii_gamecube_kit_last_error(ctx));
		return 1;
	}
	printf("%s\n%s %s\n", wii_gamecube_kit_version(), info.game_id, hash.sha1);
	wii_gamecube_kit_context_destroy(ctx);
	return 0;
}
