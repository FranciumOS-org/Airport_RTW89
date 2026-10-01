// SPDX-License-Identifier: GPL-2.0
/*
 * cfg80211_calculate_bitrate() and its per-PHY helpers, copied unmodified from
 * Linux net/wireless/util.c at the pinned commit (third_party/rtw89/UPSTREAM.md):
 *
 * Copyright 2007-2009	Johannes Berg <johannes@sipsolutions.net>
 * Copyright 2013-2014  Intel Mobile Communications GmbH
 * Copyright 2017	Intel Deutschland GmbH
 * Copyright (C) 2018-2023, 2025-2026 Intel Corporation
 *
 * Regenerate with tools/extract_bitrate.sh after an upstream bump; do not edit
 * the code below by hand.
 */
#include "rtw89_net80211.h"

static u32 cfg80211_calculate_bitrate_ht(struct rate_info *rate)
{
	int modulation, streams, bitrate;

	/* the formula below does only work for MCS values smaller than 32 */
	if (WARN_ON_ONCE(rate->mcs >= 32))
		return 0;

	modulation = rate->mcs & 7;
	streams = (rate->mcs >> 3) + 1;

	bitrate = (rate->bw == RATE_INFO_BW_40) ? 13500000 : 6500000;

	if (modulation < 4)
		bitrate *= (modulation + 1);
	else if (modulation == 4)
		bitrate *= (modulation + 2);
	else
		bitrate *= (modulation + 3);

	bitrate *= streams;

	if (rate->flags & RATE_INFO_FLAGS_SHORT_GI)
		bitrate = (bitrate / 9) * 10;

	/* do NOT round down here */
	return (bitrate + 50000) / 100000;
}

static u32 cfg80211_calculate_bitrate_dmg(struct rate_info *rate)
{
	static const u32 __mcs2bitrate[] = {
		/* control PHY */
		[0] =   275,
		/* SC PHY */
		[1] =  3850,
		[2] =  7700,
		[3] =  9625,
		[4] = 11550,
		[5] = 12512, /* 1251.25 mbps */
		[6] = 15400,
		[7] = 19250,
		[8] = 23100,
		[9] = 25025,
		[10] = 30800,
		[11] = 38500,
		[12] = 46200,
		/* OFDM PHY */
		[13] =  6930,
		[14] =  8662, /* 866.25 mbps */
		[15] = 13860,
		[16] = 17325,
		[17] = 20790,
		[18] = 27720,
		[19] = 34650,
		[20] = 41580,
		[21] = 45045,
		[22] = 51975,
		[23] = 62370,
		[24] = 67568, /* 6756.75 mbps */
		/* LP-SC PHY */
		[25] =  6260,
		[26] =  8340,
		[27] = 11120,
		[28] = 12510,
		[29] = 16680,
		[30] = 22240,
		[31] = 25030,
	};

	if (WARN_ON_ONCE(rate->mcs >= ARRAY_SIZE(__mcs2bitrate)))
		return 0;

	return __mcs2bitrate[rate->mcs];
}

static u32 cfg80211_calculate_bitrate_extended_sc_dmg(struct rate_info *rate)
{
	static const u32 __mcs2bitrate[] = {
		[6 - 6] = 26950, /* MCS 9.1 : 2695.0 mbps */
		[7 - 6] = 50050, /* MCS 12.1 */
		[8 - 6] = 53900,
		[9 - 6] = 57750,
		[10 - 6] = 63900,
		[11 - 6] = 75075,
		[12 - 6] = 80850,
	};

	/* Extended SC MCS not defined for base MCS below 6 or above 12 */
	if (WARN_ON_ONCE(rate->mcs < 6 || rate->mcs > 12))
		return 0;

	return __mcs2bitrate[rate->mcs - 6];
}

static u32 cfg80211_calculate_bitrate_edmg(struct rate_info *rate)
{
	static const u32 __mcs2bitrate[] = {
		/* control PHY */
		[0] =   275,
		/* SC PHY */
		[1] =  3850,
		[2] =  7700,
		[3] =  9625,
		[4] = 11550,
		[5] = 12512, /* 1251.25 mbps */
		[6] = 13475,
		[7] = 15400,
		[8] = 19250,
		[9] = 23100,
		[10] = 25025,
		[11] = 26950,
		[12] = 30800,
		[13] = 38500,
		[14] = 46200,
		[15] = 50050,
		[16] = 53900,
		[17] = 57750,
		[18] = 69300,
		[19] = 75075,
		[20] = 80850,
	};

	if (WARN_ON_ONCE(rate->mcs >= ARRAY_SIZE(__mcs2bitrate)))
		return 0;

	return __mcs2bitrate[rate->mcs] * rate->n_bonded_ch;
}

static u32 cfg80211_calculate_bitrate_vht(struct rate_info *rate)
{
	static const u32 base[4][12] = {
		{   6500000,
		   13000000,
		   19500000,
		   26000000,
		   39000000,
		   52000000,
		   58500000,
		   65000000,
		   78000000,
		/* not in the spec, but some devices use this: */
		   86700000,
		   97500000,
		  108300000,
		},
		{  13500000,
		   27000000,
		   40500000,
		   54000000,
		   81000000,
		  108000000,
		  121500000,
		  135000000,
		  162000000,
		  180000000,
		  202500000,
		  225000000,
		},
		{  29300000,
		   58500000,
		   87800000,
		  117000000,
		  175500000,
		  234000000,
		  263300000,
		  292500000,
		  351000000,
		  390000000,
		  438800000,
		  487500000,
		},
		{  58500000,
		  117000000,
		  175500000,
		  234000000,
		  351000000,
		  468000000,
		  526500000,
		  585000000,
		  702000000,
		  780000000,
		  877500000,
		  975000000,
		},
	};
	u32 bitrate;
	int idx;

	if (rate->mcs > 11)
		goto warn;

	switch (rate->bw) {
	case RATE_INFO_BW_160:
		idx = 3;
		break;
	case RATE_INFO_BW_80:
		idx = 2;
		break;
	case RATE_INFO_BW_40:
		idx = 1;
		break;
	case RATE_INFO_BW_5:
	case RATE_INFO_BW_10:
	default:
		goto warn;
	case RATE_INFO_BW_20:
		idx = 0;
	}

	bitrate = base[idx][rate->mcs];
	bitrate *= rate->nss;

	if (rate->flags & RATE_INFO_FLAGS_SHORT_GI)
		bitrate = (bitrate / 9) * 10;

	/* do NOT round down here */
	return (bitrate + 50000) / 100000;
 warn:
	WARN_ONCE(1, "invalid rate bw=%d, mcs=%d, nss=%d\n",
		  rate->bw, rate->mcs, rate->nss);
	return 0;
}

static u32 cfg80211_calculate_bitrate_he(struct rate_info *rate)
{
#define SCALE 6144
	u32 mcs_divisors[14] = {
		102399, /* 16.666666... */
		 51201, /*  8.333333... */
		 34134, /*  5.555555... */
		 25599, /*  4.166666... */
		 17067, /*  2.777777... */
		 12801, /*  2.083333... */
		 11377, /*  1.851725... */
		 10239, /*  1.666666... */
		  8532, /*  1.388888... */
		  7680, /*  1.250000... */
		  6828, /*  1.111111... */
		  6144, /*  1.000000... */
		  5690, /*  0.926106... */
		  5120, /*  0.833333... */
	};
	u32 rates_160M[3] = { 960777777, 907400000, 816666666 };
	u32 rates_996[3] =  { 480388888, 453700000, 408333333 };
	u32 rates_484[3] =  { 229411111, 216666666, 195000000 };
	u32 rates_242[3] =  { 114711111, 108333333,  97500000 };
	u32 rates_106[3] =  {  40000000,  37777777,  34000000 };
	u32 rates_52[3]  =  {  18820000,  17777777,  16000000 };
	u32 rates_26[3]  =  {   9411111,   8888888,   8000000 };
	u64 tmp;
	u32 result;

	if (WARN_ON_ONCE(rate->mcs > 13))
		return 0;

	if (WARN_ON_ONCE(rate->he_gi > NL80211_RATE_INFO_HE_GI_3_2))
		return 0;
	if (WARN_ON_ONCE(rate->he_ru_alloc >
			 NL80211_RATE_INFO_HE_RU_ALLOC_2x996))
		return 0;
	if (WARN_ON_ONCE(rate->nss < 1 || rate->nss > 8))
		return 0;

	if (rate->bw == RATE_INFO_BW_160 ||
	    (rate->bw == RATE_INFO_BW_HE_RU &&
	     rate->he_ru_alloc == NL80211_RATE_INFO_HE_RU_ALLOC_2x996))
		result = rates_160M[rate->he_gi];
	else if (rate->bw == RATE_INFO_BW_80 ||
		 (rate->bw == RATE_INFO_BW_HE_RU &&
		  rate->he_ru_alloc == NL80211_RATE_INFO_HE_RU_ALLOC_996))
		result = rates_996[rate->he_gi];
	else if (rate->bw == RATE_INFO_BW_40 ||
		 (rate->bw == RATE_INFO_BW_HE_RU &&
		  rate->he_ru_alloc == NL80211_RATE_INFO_HE_RU_ALLOC_484))
		result = rates_484[rate->he_gi];
	else if (rate->bw == RATE_INFO_BW_20 ||
		 (rate->bw == RATE_INFO_BW_HE_RU &&
		  rate->he_ru_alloc == NL80211_RATE_INFO_HE_RU_ALLOC_242))
		result = rates_242[rate->he_gi];
	else if (rate->bw == RATE_INFO_BW_HE_RU &&
		 rate->he_ru_alloc == NL80211_RATE_INFO_HE_RU_ALLOC_106)
		result = rates_106[rate->he_gi];
	else if (rate->bw == RATE_INFO_BW_HE_RU &&
		 rate->he_ru_alloc == NL80211_RATE_INFO_HE_RU_ALLOC_52)
		result = rates_52[rate->he_gi];
	else if (rate->bw == RATE_INFO_BW_HE_RU &&
		 rate->he_ru_alloc == NL80211_RATE_INFO_HE_RU_ALLOC_26)
		result = rates_26[rate->he_gi];
	else {
		WARN(1, "invalid HE MCS: bw:%d, ru:%d\n",
		     rate->bw, rate->he_ru_alloc);
		return 0;
	}

	/* now scale to the appropriate MCS */
	tmp = result;
	tmp *= SCALE;
	do_div(tmp, mcs_divisors[rate->mcs]);

	/* and take NSS, DCM into account */
	tmp *= rate->nss;
	do_div(tmp, 8);
	if (rate->he_dcm)
		do_div(tmp, 2);

	result = tmp;

	return result / 10000;
}

static u32 _cfg80211_calculate_bitrate_eht_uhr(struct rate_info *rate)
{
#define SCALE 6144
	static const u32 mcs_divisors[] = {
		[ 0] = 102399, /* 16.666666... */
		[ 1] =  51201, /*  8.333333... */
		[ 2] =  34134, /*  5.555555... */
		[ 3] =  25599, /*  4.166666... */
		[ 4] =  17067, /*  2.777777... */
		[ 5] =  12801, /*  2.083333... */
		[ 6] =  11377, /*  1.851725... */
		[ 7] =  10239, /*  1.666666... */
		[ 8] =   8532, /*  1.388888... */
		[ 9] =   7680, /*  1.250000... */
		[10] =   6828, /*  1.111111... */
		[11] =   6144, /*  1.000000... */
		[12] =   5690, /*  0.926106... */
		[13] =   5120, /*  0.833333... */
		[14] = 409600, /* 66.666666... */
		[15] = 204800, /* 33.333333... */
		[17] =  38400, /*  6.250180... */
		[19] =  19200, /*  3.125090... */
		[20] =  15360, /*  2.500000... */
		[23] =   9600, /*  1.562545... */
	};
	static const u32 rates_996[3] =  { 480388888, 453700000, 408333333 };
	static const u32 rates_484[3] =  { 229411111, 216666666, 195000000 };
	static const u32 rates_242[3] =  { 114711111, 108333333,  97500000 };
	static const u32 rates_106[3] =  {  40000000,  37777777,  34000000 };
	static const u32 rates_52[3]  =  {  18820000,  17777777,  16000000 };
	static const u32 rates_26[3]  =  {   9411111,   8888888,   8000000 };
	u64 tmp;
	u32 result;

	if (WARN_ON_ONCE(rate->eht_gi > NL80211_RATE_INFO_EHT_GI_3_2))
		return 0;
	if (WARN_ON_ONCE(rate->eht_ru_alloc >
			 NL80211_RATE_INFO_EHT_RU_ALLOC_4x996))
		return 0;
	if (WARN_ON_ONCE(rate->nss < 1 || rate->nss > 8))
		return 0;

	/* Bandwidth checks for MCS 14 */
	if (rate->mcs == 14) {
		if ((rate->bw != RATE_INFO_BW_EHT_RU &&
		     rate->bw != RATE_INFO_BW_80 &&
		     rate->bw != RATE_INFO_BW_160 &&
		     rate->bw != RATE_INFO_BW_320) ||
		    (rate->bw == RATE_INFO_BW_EHT_RU &&
		     rate->eht_ru_alloc != NL80211_RATE_INFO_EHT_RU_ALLOC_996 &&
		     rate->eht_ru_alloc != NL80211_RATE_INFO_EHT_RU_ALLOC_2x996 &&
		     rate->eht_ru_alloc != NL80211_RATE_INFO_EHT_RU_ALLOC_4x996)) {
			WARN(1, "invalid EHT BW for MCS 14: bw:%d, ru:%d\n",
			     rate->bw, rate->eht_ru_alloc);
			return 0;
		}
	}

	if (rate->bw == RATE_INFO_BW_320 ||
	    (rate->bw == RATE_INFO_BW_EHT_RU &&
	     rate->eht_ru_alloc == NL80211_RATE_INFO_EHT_RU_ALLOC_4x996))
		result = 4 * rates_996[rate->eht_gi];
	else if (rate->bw == RATE_INFO_BW_EHT_RU &&
		 rate->eht_ru_alloc == NL80211_RATE_INFO_EHT_RU_ALLOC_3x996P484)
		result = 3 * rates_996[rate->eht_gi] + rates_484[rate->eht_gi];
	else if (rate->bw == RATE_INFO_BW_EHT_RU &&
		 rate->eht_ru_alloc == NL80211_RATE_INFO_EHT_RU_ALLOC_3x996)
		result = 3 * rates_996[rate->eht_gi];
	else if (rate->bw == RATE_INFO_BW_EHT_RU &&
		 rate->eht_ru_alloc == NL80211_RATE_INFO_EHT_RU_ALLOC_2x996P484)
		result = 2 * rates_996[rate->eht_gi] + rates_484[rate->eht_gi];
	else if (rate->bw == RATE_INFO_BW_160 ||
		 (rate->bw == RATE_INFO_BW_EHT_RU &&
		  rate->eht_ru_alloc == NL80211_RATE_INFO_EHT_RU_ALLOC_2x996))
		result = 2 * rates_996[rate->eht_gi];
	else if (rate->bw == RATE_INFO_BW_EHT_RU &&
		 rate->eht_ru_alloc ==
		 NL80211_RATE_INFO_EHT_RU_ALLOC_996P484P242)
		result = rates_996[rate->eht_gi] + rates_484[rate->eht_gi]
			 + rates_242[rate->eht_gi];
	else if (rate->bw == RATE_INFO_BW_EHT_RU &&
		 rate->eht_ru_alloc == NL80211_RATE_INFO_EHT_RU_ALLOC_996P484)
		result = rates_996[rate->eht_gi] + rates_484[rate->eht_gi];
	else if (rate->bw == RATE_INFO_BW_80 ||
		 (rate->bw == RATE_INFO_BW_EHT_RU &&
		  rate->eht_ru_alloc == NL80211_RATE_INFO_EHT_RU_ALLOC_996))
		result = rates_996[rate->eht_gi];
	else if (rate->bw == RATE_INFO_BW_EHT_RU &&
		 rate->eht_ru_alloc == NL80211_RATE_INFO_EHT_RU_ALLOC_484P242)
		result = rates_484[rate->eht_gi] + rates_242[rate->eht_gi];
	else if (rate->bw == RATE_INFO_BW_40 ||
		 (rate->bw == RATE_INFO_BW_EHT_RU &&
		  rate->eht_ru_alloc == NL80211_RATE_INFO_EHT_RU_ALLOC_484))
		result = rates_484[rate->eht_gi];
	else if (rate->bw == RATE_INFO_BW_20 ||
		 (rate->bw == RATE_INFO_BW_EHT_RU &&
		  rate->eht_ru_alloc == NL80211_RATE_INFO_EHT_RU_ALLOC_242))
		result = rates_242[rate->eht_gi];
	else if (rate->bw == RATE_INFO_BW_EHT_RU &&
		 rate->eht_ru_alloc == NL80211_RATE_INFO_EHT_RU_ALLOC_106P26)
		result = rates_106[rate->eht_gi] + rates_26[rate->eht_gi];
	else if (rate->bw == RATE_INFO_BW_EHT_RU &&
		 rate->eht_ru_alloc == NL80211_RATE_INFO_EHT_RU_ALLOC_106)
		result = rates_106[rate->eht_gi];
	else if (rate->bw == RATE_INFO_BW_EHT_RU &&
		 rate->eht_ru_alloc == NL80211_RATE_INFO_EHT_RU_ALLOC_52P26)
		result = rates_52[rate->eht_gi] + rates_26[rate->eht_gi];
	else if (rate->bw == RATE_INFO_BW_EHT_RU &&
		 rate->eht_ru_alloc == NL80211_RATE_INFO_EHT_RU_ALLOC_52)
		result = rates_52[rate->eht_gi];
	else if (rate->bw == RATE_INFO_BW_EHT_RU &&
		 rate->eht_ru_alloc == NL80211_RATE_INFO_EHT_RU_ALLOC_26)
		result = rates_26[rate->eht_gi];
	else {
		WARN(1, "invalid EHT or UHR MCS: bw:%d, ru:%d\n",
		     rate->bw, rate->eht_ru_alloc);
		return 0;
	}

	/* now scale to the appropriate MCS */
	tmp = result;
	tmp *= SCALE;
	do_div(tmp, mcs_divisors[rate->mcs]);

	/* and take NSS */
	tmp *= rate->nss;
	do_div(tmp, 8);

	/* and handle interference mitigation - 0.9x */
	if (rate->flags & RATE_INFO_FLAGS_UHR_IM) {
		if (WARN(rate->nss != 1 || rate->mcs == 15,
			 "invalid NSS or MCS for UHR IM\n"))
			return 0;
		tmp *= 9000;
		do_div(tmp, 10000);
	}

	result = tmp;

	return result / 10000;
}

static u32 cfg80211_calculate_bitrate_eht(struct rate_info *rate)
{
	if (WARN_ONCE(rate->mcs > 15, "bad EHT MCS %d\n", rate->mcs))
		return 0;

	if (WARN_ONCE(rate->flags & (RATE_INFO_FLAGS_UHR_ELR_MCS |
				     RATE_INFO_FLAGS_UHR_IM),
		      "bad EHT MCS flags 0x%x\n", rate->flags))
		return 0;

	return _cfg80211_calculate_bitrate_eht_uhr(rate);
}

static u32 cfg80211_calculate_bitrate_uhr(struct rate_info *rate)
{
	if (rate->flags & RATE_INFO_FLAGS_UHR_ELR_MCS) {
		WARN_ONCE(rate->eht_gi != NL80211_RATE_INFO_EHT_GI_1_6,
			  "bad UHR ELR guard interval %d\n",
			  rate->eht_gi);
		WARN_ONCE(rate->mcs > 1, "bad UHR ELR MCS %d\n", rate->mcs);
		WARN_ONCE(rate->nss != 1, "bad UHR ELR NSS %d\n", rate->nss);
		WARN_ONCE(rate->bw != RATE_INFO_BW_20,
			  "bad UHR ELR bandwidth %d\n",
			  rate->bw);
		WARN_ONCE(rate->flags & RATE_INFO_FLAGS_UHR_IM,
			  "bad UHR MCS flags 0x%x\n", rate->flags);
		if (rate->mcs == 0)
			return 17;
		return 33;
	}

	switch (rate->mcs) {
	case 0 ... 15:
	case 17:
	case 19:
	case 20:
	case 23:
		return _cfg80211_calculate_bitrate_eht_uhr(rate);
	}

	WARN_ONCE(1, "bad UHR MCS %d\n", rate->mcs);
	return 0;
}

static u32 cfg80211_calculate_bitrate_s1g(struct rate_info *rate)
{
	/* For 1, 2, 4, 8 and 16 MHz channels */
	static const u32 base[5][11] = {
		{  300000,
		   600000,
		   900000,
		  1200000,
		  1800000,
		  2400000,
		  2700000,
		  3000000,
		  3600000,
		  4000000,
		  /* MCS 10 supported in 1 MHz only */
		  150000,
		},
		{  650000,
		  1300000,
		  1950000,
		  2600000,
		  3900000,
		  5200000,
		  5850000,
		  6500000,
		  7800000,
		  /* MCS 9 not valid */
		},
		{  1350000,
		   2700000,
		   4050000,
		   5400000,
		   8100000,
		  10800000,
		  12150000,
		  13500000,
		  16200000,
		  18000000,
		},
		{  2925000,
		   5850000,
		   8775000,
		  11700000,
		  17550000,
		  23400000,
		  26325000,
		  29250000,
		  35100000,
		  39000000,
		},
		{  8580000,
		  11700000,
		  17550000,
		  23400000,
		  35100000,
		  46800000,
		  52650000,
		  58500000,
		  70200000,
		  78000000,
		},
	};
	u32 bitrate;
	/* default is 1 MHz index */
	int idx = 0;

	if (rate->mcs >= 11)
		goto warn;

	switch (rate->bw) {
	case RATE_INFO_BW_16:
		idx = 4;
		break;
	case RATE_INFO_BW_8:
		idx = 3;
		break;
	case RATE_INFO_BW_4:
		idx = 2;
		break;
	case RATE_INFO_BW_2:
		idx = 1;
		break;
	case RATE_INFO_BW_1:
		idx = 0;
		break;
	case RATE_INFO_BW_5:
	case RATE_INFO_BW_10:
	case RATE_INFO_BW_20:
	case RATE_INFO_BW_40:
	case RATE_INFO_BW_80:
	case RATE_INFO_BW_160:
	default:
		goto warn;
	}

	bitrate = base[idx][rate->mcs];
	bitrate *= rate->nss;

	if (rate->flags & RATE_INFO_FLAGS_SHORT_GI)
		bitrate = (bitrate / 9) * 10;
	/* do NOT round down here */
	return (bitrate + 50000) / 100000;
warn:
	WARN_ONCE(1, "invalid rate bw=%d, mcs=%d, nss=%d\n",
		  rate->bw, rate->mcs, rate->nss);
	return 0;
}

u32 cfg80211_calculate_bitrate(struct rate_info *rate)
{
	if (rate->flags & RATE_INFO_FLAGS_MCS)
		return cfg80211_calculate_bitrate_ht(rate);
	if (rate->flags & RATE_INFO_FLAGS_DMG)
		return cfg80211_calculate_bitrate_dmg(rate);
	if (rate->flags & RATE_INFO_FLAGS_EXTENDED_SC_DMG)
		return cfg80211_calculate_bitrate_extended_sc_dmg(rate);
	if (rate->flags & RATE_INFO_FLAGS_EDMG)
		return cfg80211_calculate_bitrate_edmg(rate);
	if (rate->flags & RATE_INFO_FLAGS_VHT_MCS)
		return cfg80211_calculate_bitrate_vht(rate);
	if (rate->flags & RATE_INFO_FLAGS_HE_MCS)
		return cfg80211_calculate_bitrate_he(rate);
	if (rate->flags & RATE_INFO_FLAGS_EHT_MCS)
		return cfg80211_calculate_bitrate_eht(rate);
	if (rate->flags & RATE_INFO_FLAGS_UHR_MCS)
		return cfg80211_calculate_bitrate_uhr(rate);
	if (rate->flags & RATE_INFO_FLAGS_S1G_MCS)
		return cfg80211_calculate_bitrate_s1g(rate);

	return rate->legacy;
}
