/* SPDX-License-Identifier: GPL-2.0-or-later */

/* Plain C, linked against the shared library: the header works from C and the
 * DLL/.so/.dylib exports what it declares. */

#include <stdio.h>
#include <string.h>

#include "dolphinrvz.h"

int main(int argc, char **argv)
{
	drvz_context *ctx = NULL;
	drvz_disc_info info;
	drvz_hash_result hash;

	if (argc < 2 || drvz_abi_version() != DRVZ_ABI_VERSION)
		return 1;
	if (drvz_context_create(&ctx) != DRVZ_OK)
		return 1;
	drvz_disc_info_init(&info);
	if (drvz_probe(ctx, argv[1], &info) != DRVZ_OK || info.platform != DRVZ_PLATFORM_GAMECUBE) {
		fprintf(stderr, "probe failed: %s\n", drvz_last_error(ctx));
		return 1;
	}
	drvz_hash_result_init(&hash);
	if (drvz_hash(ctx, argv[1], &hash) != DRVZ_OK || strlen(hash.sha1) != 40) {
		fprintf(stderr, "hash failed: %s\n", drvz_last_error(ctx));
		return 1;
	}
	printf("%s\n%s %s\n", drvz_version(), info.game_id, hash.sha1);
	drvz_context_destroy(ctx);
	return 0;
}
