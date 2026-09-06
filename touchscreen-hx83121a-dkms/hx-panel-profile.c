// SPDX-License-Identifier: GPL-2.0
/* Versioned metadata extracted from the Gaokun TSAPrmt project records. */
#ifdef HX_ALGO_HOST_TEST
#include "../tests/host-compat.h"
#else
#include <linux/kernel.h>
#include <linux/string.h>
#endif

#include "hx-panel-profile.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

static const struct hx_panel_profile hx_generic_profile = {
	.kind = HX_PANEL_PROFILE_GENERIC,
	.project_id = "DFLT000000",
	.revision = "generic",
	.tx = HX_COLS,
	.rx = HX_ROWS,
};

static const struct hx_panel_profile hx_gaokun_profiles[] = {
	{
		.kind = HX_PANEL_PROFILE_BOE_OLD,
		.project_id = "W273AS1300",
		.revision = "BOE-old-mapping",
		.tx = 60, .rx = 40, .physical_width = 0x48,
		.physical_height = 0x42, .finger_threshold = 0x0258,
		.sig_mult_normal = 0x40, .sig_mult_sd = 0x40,
		.acceleration_threshold = 0x0010,
	},
	{
		.kind = HX_PANEL_PROFILE_BOE_NEW,
		.project_id = "W273AS1310",
		.revision = "BOE-new-mapping",
		.tx = 60, .rx = 40, .physical_width = 0x48,
		.physical_height = 0x42, .finger_threshold = 0x0258,
		.sig_mult_normal = 0x40, .sig_mult_sd = 0x40,
		.acceleration_threshold = 0x0010,
	},
	{
		.kind = HX_PANEL_PROFILE_CSOT_OLD,
		.project_id = "W273AS2700",
		.revision = "CSOT-old-mapping",
		.tx = 60, .rx = 40, .physical_width = 0x45,
		.physical_height = 0x42, .finger_threshold = 0x0258,
		.sig_mult_normal = 0x40, .sig_mult_sd = 0x40,
		.acceleration_threshold = 0x0010,
	},
	{
		.kind = HX_PANEL_PROFILE_CSOT_NEW,
		.project_id = "W273AS2710",
		.revision = "CSOT-new-mapping",
		.tx = 60, .rx = 40, .physical_width = 0x45,
		.physical_height = 0x42, .finger_threshold = 0x0258,
		.sig_mult_normal = 0x40, .sig_mult_sd = 0x40,
		.acceleration_threshold = 0x0010,
	},
};

const struct hx_panel_profile *hx_panel_profile_for_id(const char *project_id)
{
	size_t i;

	if (project_id)
		for (i = 0; i < ARRAY_SIZE(hx_gaokun_profiles); i++)
			if (!strcmp(project_id, hx_gaokun_profiles[i].project_id))
				return &hx_gaokun_profiles[i];
	return &hx_generic_profile;
}
