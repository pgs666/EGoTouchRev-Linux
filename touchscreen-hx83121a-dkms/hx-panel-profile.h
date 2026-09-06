/* SPDX-License-Identifier: GPL-2.0 */
#ifndef HX_PANEL_PROFILE_H
#define HX_PANEL_PROFILE_H

#include "hx-algo.h"

enum hx_panel_profile_kind {
	HX_PANEL_PROFILE_GENERIC,
	HX_PANEL_PROFILE_BOE_OLD,
	HX_PANEL_PROFILE_BOE_NEW,
	HX_PANEL_PROFILE_CSOT_OLD,
	HX_PANEL_PROFILE_CSOT_NEW,
};

struct hx_panel_profile {
	enum hx_panel_profile_kind kind;
	const char *project_id;
	const char *revision;
	u8 tx;
	u8 rx;
	u8 physical_width;
	u8 physical_height;
	u16 finger_threshold;
	u8 sig_mult_normal;
	u8 sig_mult_sd;
	u16 acceleration_threshold;
};

const struct hx_panel_profile *hx_panel_profile_for_id(const char *project_id);

#endif /* HX_PANEL_PROFILE_H */
