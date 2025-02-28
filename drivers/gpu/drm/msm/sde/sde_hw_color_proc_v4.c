/* Copyright (c) 2017, The Linux Foundation. All rights reserved.
 * Copyright (c) 2018, Pal Zoltan Illes (tbalden) - kcal rgb
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <drm/msm_drm_pp.h>
#include "sde_hw_color_proc_common_v4.h"
#include "sde_hw_color_proc_v4.h"

#if 1
#include <linux/uci/uci.h>
#endif

static int sde_write_3d_gamut(struct sde_hw_blk_reg_map *hw,
		struct drm_msm_3d_gamut *payload, u32 base,
		u32 *opcode)
{
	u32 reg, tbl_len, tbl_off, scale_off, i, j;
	u32 scale_tbl_len, scale_tbl_off;
	u32 *scale_data;

	if (!payload || !opcode || !hw) {
		DRM_ERROR("invalid payload %pK opcode %pK hw %pK\n",
			payload, opcode, hw);
		return -EINVAL;
	}

	switch (payload->mode) {
	case GAMUT_3D_MODE_17:
		tbl_len = GAMUT_3D_MODE17_TBL_SZ;
		tbl_off = 0;
		scale_off = GAMUT_SCALEA_OFFSET_OFF;
		*opcode = gamut_mode_17 << 2;
		break;
	case GAMUT_3D_MODE_13:
		*opcode = (*opcode & (BIT(4) - 1)) >> 2;
		if (*opcode == gamut_mode_13a)
			*opcode = gamut_mode_13b;
		else
			*opcode = gamut_mode_13a;
		tbl_len = GAMUT_3D_MODE13_TBL_SZ;
		tbl_off = (*opcode == gamut_mode_13a) ? 0 :
			GAMUT_MODE_13B_OFF;
		scale_off = (*opcode == gamut_mode_13a) ?
			GAMUT_SCALEA_OFFSET_OFF : GAMUT_SCALEB_OFFSET_OFF;
		*opcode <<= 2;
		break;
	case GAMUT_3D_MODE_5:
		*opcode = gamut_mode_5 << 2;
		tbl_len = GAMUT_3D_MODE5_TBL_SZ;
		tbl_off = GAMUT_MODE_5_OFF;
		scale_off = GAMUT_SCALEB_OFFSET_OFF;
		break;
	default:
		DRM_ERROR("invalid mode %d\n", payload->mode);
		return -EINVAL;
	}

	if (payload->flags & GAMUT_3D_MAP_EN)
		*opcode |= GAMUT_MAP_EN;
	*opcode |= GAMUT_EN;

	for (i = 0; i < GAMUT_3D_TBL_NUM; i++) {
		reg = GAMUT_TABLE0_SEL << i;
		reg |= ((tbl_off) & (BIT(11) - 1));
		SDE_REG_WRITE(hw, base + GAMUT_TABLE_SEL_OFF, reg);
		for (j = 0; j < tbl_len; j++) {
			SDE_REG_WRITE(hw, base + GAMUT_LOWER_COLOR_OFF,
					payload->col[i][j].c2_c1);
			SDE_REG_WRITE(hw, base + GAMUT_UPPER_COLOR_OFF,
					payload->col[i][j].c0);
		}
	}

	if ((*opcode & GAMUT_MAP_EN)) {
		if (scale_off == GAMUT_SCALEA_OFFSET_OFF)
			scale_tbl_len = GAMUT_3D_SCALE_OFF_SZ;
		else
			scale_tbl_len = GAMUT_3D_SCALEB_OFF_SZ;
		for (i = 0; i < GAMUT_3D_SCALE_OFF_TBL_NUM; i++) {
			scale_tbl_off = base + scale_off +
					i * scale_tbl_len * sizeof(u32);
			scale_data = &payload->scale_off[i][0];
			for (j = 0; j < scale_tbl_len; j++)
				SDE_REG_WRITE(hw,
					scale_tbl_off + (j * sizeof(u32)),
					scale_data[j]);
		}
	}
	SDE_REG_WRITE(hw, base, *opcode);
	return 0;
}

void sde_setup_dspp_3d_gamutv4(struct sde_hw_dspp *ctx, void *cfg)
{
	struct drm_msm_3d_gamut *payload;
	struct sde_hw_cp_cfg *hw_cfg = cfg;
	u32 op_mode;

	if (!ctx || !cfg) {
		DRM_ERROR("invalid param ctx %pK cfg %pK\n", ctx, cfg);
		return;
	}

	op_mode = SDE_REG_READ(&ctx->hw, ctx->cap->sblk->gamut.base);
	if (!hw_cfg->payload) {
		DRM_DEBUG_DRIVER("disable gamut feature\n");
		SDE_REG_WRITE(&ctx->hw, ctx->cap->sblk->gamut.base, 0);
		return;
	}

	payload = hw_cfg->payload;
	sde_write_3d_gamut(&ctx->hw, payload, ctx->cap->sblk->gamut.base,
		&op_mode);

}

void sde_setup_dspp_igcv3(struct sde_hw_dspp *ctx, void *cfg)
{
	struct drm_msm_igc_lut *lut_cfg;
	struct sde_hw_cp_cfg *hw_cfg = cfg;
	int i = 0, j = 0;
	u32 *addr = NULL;
	u32 offset = 0;

	if (!ctx || !cfg) {
		DRM_ERROR("invalid param ctx %pK cfg %pK\n", ctx, cfg);
		return;
	}

	if (!hw_cfg->payload) {
		DRM_DEBUG_DRIVER("disable igc feature\n");
		SDE_REG_WRITE(&ctx->hw, IGC_OPMODE_OFF, 0);
		return;
	}

	if (hw_cfg->len != sizeof(struct drm_msm_igc_lut)) {
		DRM_ERROR("invalid size of payload len %d exp %zd\n",
				hw_cfg->len, sizeof(struct drm_msm_igc_lut));
		return;
	}

	lut_cfg = hw_cfg->payload;

	for (i = 0; i < IGC_TBL_NUM; i++) {
		addr = lut_cfg->c0 + (i * ARRAY_SIZE(lut_cfg->c0));
		offset = IGC_C0_OFF + (i * sizeof(u32));

		for (j = 0; j < IGC_TBL_LEN; j++) {
			addr[j] &= IGC_DATA_MASK;
			addr[j] |= IGC_DSPP_SEL_MASK(ctx->idx - 1);
			if (j == 0)
				addr[j] |= IGC_INDEX_UPDATE;
			/* IGC lut registers are part of DSPP Top HW block */
			SDE_REG_WRITE(&ctx->hw_top, offset, addr[j]);
		}
	}

	if (lut_cfg->flags & IGC_DITHER_ENABLE) {
		SDE_REG_WRITE(&ctx->hw, IGC_DITHER_OFF,
			lut_cfg->strength & IGC_DITHER_DATA_MASK);
	}

	SDE_REG_WRITE(&ctx->hw, IGC_OPMODE_OFF, IGC_EN);
}

void sde_setup_dspp_pccv4(struct sde_hw_dspp *ctx, void *cfg)
{
	struct sde_hw_cp_cfg *hw_cfg = cfg;
	struct drm_msm_pcc *pcc_cfg;
	struct drm_msm_pcc_coeff *coeffs = NULL;
	int i = 0;
	u32 base = 0;
#if 1
	int enable = 0, r=255,g=255,b=255, min = 20;
	int sat=255, hue=0, cont=255, val = 255;
	u32 opcode = 0, local_opcode = 0;
#endif

	if (!ctx || !cfg) {
		DRM_ERROR("invalid param ctx %pK cfg %pK\n", ctx, cfg);
		return;
	}
#if 1
	pr_info("%s [CLEANSLATE] kcal setup... \n",__func__);
        enable = uci_get_user_property_int_mm("kcal_enable", enable, 0, 1);
        r = uci_get_user_property_int_mm("kcal_red", r, 0, 256);
        g = uci_get_user_property_int_mm("kcal_green", g, 0, 256);
        b = uci_get_user_property_int_mm("kcal_blue", b, 0, 256);
        min = uci_get_user_property_int_mm("kcal_min", min, 0, 256);
	if (r<min) r= min;
	if (g<min) g= min;
	if (b<min) b= min;
        sat = uci_get_user_property_int_mm("kcal_sat", sat, 128, 383);
	// don't add HUE, not much useful
        //hue = uci_get_user_property_int_mm("kcal_hue", hue, 0, 255);
        cont = uci_get_user_property_int_mm("kcal_cont", cont,128, 383);
        val = uci_get_user_property_int_mm("kcal_val", val, 128, 383);
	if (!enable) {
		// disabled, return to defaults
		sat = 255;
		cont = 255;
		val = 255;
		hue = 0;
	}
#endif
	if (!hw_cfg->payload) {
		DRM_DEBUG_DRIVER("disable pcc feature\n");
		SDE_REG_WRITE(&ctx->hw, ctx->cap->sblk->pcc.base, 0);
		return;
	}

	if (hw_cfg->len != sizeof(struct drm_msm_pcc)) {
		DRM_ERROR("invalid size of payload len %d exp %zd\n",
				hw_cfg->len, sizeof(struct drm_msm_pcc));
		return;
	}

	pcc_cfg = hw_cfg->payload;

	for (i = 0; i < PCC_NUM_PLANES; i++) {
		base = ctx->cap->sblk->pcc.base + (i * sizeof(u32));
		switch (i) {
		case 0:
			coeffs = &pcc_cfg->r;
			SDE_REG_WRITE(&ctx->hw,
				base + PCC_RR_OFF, pcc_cfg->r_rr);
			SDE_REG_WRITE(&ctx->hw,
				base + PCC_GG_OFF, pcc_cfg->r_gg);
			SDE_REG_WRITE(&ctx->hw,
				base + PCC_BB_OFF, pcc_cfg->r_bb);
			break;
		case 1:
			coeffs = &pcc_cfg->g;
			SDE_REG_WRITE(&ctx->hw,
				base + PCC_RR_OFF, pcc_cfg->g_rr);
			SDE_REG_WRITE(&ctx->hw,
				base + PCC_GG_OFF, pcc_cfg->g_gg);
			SDE_REG_WRITE(&ctx->hw,
				base + PCC_BB_OFF, pcc_cfg->g_bb);
			break;
		case 2:
			coeffs = &pcc_cfg->b;
			SDE_REG_WRITE(&ctx->hw,
				base + PCC_RR_OFF, pcc_cfg->b_rr);
			SDE_REG_WRITE(&ctx->hw,
				base + PCC_GG_OFF, pcc_cfg->b_gg);
			SDE_REG_WRITE(&ctx->hw,
				base + PCC_BB_OFF, pcc_cfg->b_bb);
			break;
		default:
			DRM_ERROR("invalid pcc plane: %d\n", i);
			return;
		}

		SDE_REG_WRITE(&ctx->hw, base + PCC_C_OFF, coeffs->c);
// ====
// RED
#if 1
		if (enable && i==0) {
			SDE_REG_WRITE(&ctx->hw, base + PCC_R_OFF, (coeffs->r * r)/256);
			pr_info("%s [CLEANSLATE] kcal r = %d\n",__func__,(coeffs->r * r)/256);
		} else
#endif
		SDE_REG_WRITE(&ctx->hw, base + PCC_R_OFF, coeffs->r);
// GREEN
#if 1
		if (enable && i==1) {
			SDE_REG_WRITE(&ctx->hw, base + PCC_G_OFF, (coeffs->g * g)/256);
			pr_info("%s [CLEANSLATE] kcal g = %d\n",__func__,(coeffs->g * g)/256);
		} else
#endif
		SDE_REG_WRITE(&ctx->hw, base + PCC_G_OFF, coeffs->g);
// BLUE
#if 1
		if (enable && i==2) {
			SDE_REG_WRITE(&ctx->hw, base + PCC_B_OFF, (coeffs->b * b)/256);
			pr_info("%s [CLEANSLATE] kcal b = %d\n",__func__,(coeffs->b * b)/256);
		} else
#endif
		SDE_REG_WRITE(&ctx->hw, base + PCC_B_OFF, coeffs->b);
// =====
		SDE_REG_WRITE(&ctx->hw, base + PCC_RG_OFF, coeffs->rg);
		SDE_REG_WRITE(&ctx->hw, base + PCC_RB_OFF, coeffs->rb);
		SDE_REG_WRITE(&ctx->hw, base + PCC_GB_OFF, coeffs->gb);
		SDE_REG_WRITE(&ctx->hw, base + PCC_RGB_OFF, coeffs->rgb);
#if 0
		pr_info("%s [CLEANSLATE] kcal setup... drm_msm_pcc i %d r %d (rg %d) r_rr %d r_gg %d r_bb %d  \n",__func__, i, coeffs->r, coeffs->rg, pcc_cfg->r_rr, pcc_cfg->r_gg, pcc_cfg->r_bb);
		pr_info("%s [CLEANSLATE] kcal setup... drm_msm_pcc i %d g %d (rb %d) g_rr %d g_gg %d g_bb %d  \n",__func__, i, coeffs->g, coeffs->rb, pcc_cfg->g_rr, pcc_cfg->g_gg, pcc_cfg->g_bb);
		pr_info("%s [CLEANSLATE] kcal setup... drm_msm_pcc i %d b %d (gb %d rgb %d) b_rr %d b_gg %d b_bb %d  \n",__func__, i, coeffs->b, coeffs->gb, coeffs->rgb, pcc_cfg->b_rr, pcc_cfg->b_gg, pcc_cfg->b_bb);
#endif
	}

#if 1

	opcode = SDE_REG_READ(&ctx->hw, ctx->cap->sblk->hsic.base);

	// HUE
	SDE_REG_WRITE(&ctx->hw, ctx->cap->sblk->hsic.base + PA_HUE_OFF,
		hue & PA_HUE_MASK);
	local_opcode |= PA_HUE_EN;

	// SATURATION
	SDE_REG_WRITE(&ctx->hw, ctx->cap->sblk->hsic.base + PA_SAT_OFF,
		sat & PA_SAT_MASK);
	local_opcode |= PA_SAT_EN;

	// VALUE
	SDE_REG_WRITE(&ctx->hw, ctx->cap->sblk->hsic.base + PA_VAL_OFF,
		val & PA_VAL_MASK);
	local_opcode |= PA_VAL_EN;

	// CONTRAST
	SDE_REG_WRITE(&ctx->hw, ctx->cap->sblk->hsic.base + PA_CONT_OFF,
		cont & PA_CONT_MASK);
	local_opcode |= PA_CONT_EN;

	opcode |= (local_opcode | PA_EN);
	SDE_REG_WRITE(&ctx->hw, ctx->cap->sblk->hsic.base, opcode);
#endif

	SDE_REG_WRITE(&ctx->hw, ctx->cap->sblk->pcc.base, PCC_EN);
}
