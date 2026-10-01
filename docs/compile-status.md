# Compile status

**30** files with errors, **3664** errors total.

| File | Errors |
|---|---|
| core | 689 |
| fw | 238 |
| mac80211 | 181 |
| phy | 171 |
| mac | 133 |
| ps | 127 |
| chan | 125 |
| regd | 114 |
| cam | 113 |
| acpi | 107 |
| coex | 107 |
| pci | 107 |
| mac_be | 105 |
| sar | 103 |
| ser | 99 |
| pci_be | 98 |
| efuse | 96 |
| efuse_be | 96 |
| phy_be | 96 |
| rtw8852b | 96 |
| rtw8852b_common | 96 |
| rtw8852b_rfk | 96 |
| rtw8852b_table | 96 |
| rtw8852be | 92 |
| compat89_rtw89_debug_shim | 91 |
| rtw8852b_rfk_table | 91 |
| util | 91 |
| compat_rtw88_compat | 4 |
| compat89_rtw89_compat | 3 |
| compat_rtw88_thread_call | 3 |

## missing header (1)

| What | Errors | First seen | Files |
|---|---|---|---|
| `main.h` | 1 | rtw88_compat.c:1046 | rtw88_compat.c |

## undeclared function (47)

| What | Errors | First seen | Files |
|---|---|---|---|
| `ieee80211_vif_is_mld` | 18 | cam.c:366 | cam.c, chan.c, core.c, fw.c … |
| `ieee80211_tu_to_usec` | 12 | chan.c:732 | chan.c, core.c, fw.c |
| `ieee80211_hdrlen` | 4 | core.c:815 | core.c, fw.c |
| `DIV_ROUND_DOWN_ULL` | 4 | core.c:5186 | core.c, phy.c, sar.c |
| `cfg80211_channel_is_psc` | 4 | fw.c:9857 | fw.c |
| `ieee80211_return_txq` | 3 | core.c:4843 | core.c |
| `cfg80211_chandef_create` | 2 | core.c:398 | core.c |
| `for_each_station` | 2 | core.c:3929 | core.c, phy.c |
| `ieee80211_get_tid` | 2 | core.c:3973 | core.c |
| `ieee80211_stop_tx_ba_session` | 2 | core.c:4701 | core.c |
| `ieee80211_vif_usable_links` | 2 | core.c:5396 | core.c |
| `get_unaligned_le32` | 2 | fw.c:334 | fw.c, phy.c |
| `pskb_expand_head` | 2 | fw.c:4734 | fw.c |
| `__skb_queue_head_init` | 2 | fw.c:9132 | fw.c |
| `skb_queue_splice_init` | 2 | fw.c:9135 | fw.c |
| `dev_info_once` | 2 | mac.c:6250 | mac.c, phy.c |
| `put_unaligned_le32` | 1 | coex.c:3358 | coex.c |
| `ieee80211_is_any_nullfunc` | 1 | core.c:630 | core.c |
| `ieee80211_has_a4` | 1 | core.c:1041 | core.c |
| `cfg80211_find_ie` | 1 | core.c:2585 | core.c |
| `ieee80211_is_trigger` | 1 | core.c:3102 | core.c |
| `ieee80211_get_sn` | 1 | core.c:3984 | core.c |
| `ieee80211_sn_less` | 1 | core.c:3992 | core.c |
| `napi_is_scheduled` | 1 | core.c:4018 | core.c |
| `ieee80211_is_pspoll` | 1 | core.c:4516 | core.c |
| `ieee80211_has_pm` | 1 | core.c:4518 | core.c |
| `ieee80211_is_qos_nullfunc` | 1 | core.c:4520 | core.c |
| `alloc_netdev_dummy` | 1 | core.c:4583 | core.c |
| `ieee80211_next_txq` | 1 | core.c:4838 | core.c |
| `ieee80211_ready_on_channel` | 1 | core.c:5079 | core.c |
| `ieee80211_remain_on_channel_expired` | 1 | core.c:5095 | core.c |
| `_ieee80211_set_sband_iftype_data` | 1 | core.c:6385 | core.c |
| `ieee80211_beacon_cntdwn_is_complete` | 1 | core.c:6513 | core.c |
| `ieee80211_csa_finish` | 1 | core.c:6519 | core.c |
| `ieee80211_set_active_links` | 1 | core.c:7244 | core.c |
| `wiphy_rfkill_start_polling` | 1 | core.c:7348 | core.c |
| `wiphy_rfkill_stop_polling` | 1 | core.c:7356 | core.c |
| `wiphy_rfkill_set_hw_state` | 1 | core.c:7380 | core.c |
| `get_unaligned_le16` | 1 | fw.c:4005 | fw.c |
| `skb_queue_splice` | 1 | fw.c:9174 | fw.c |
| `list_for_each_safe` | 1 | fw.c:10290 | fw.c |
| `cfg80211_find_elem` | 1 | mac.c:5168 | mac.c |
| `cfg80211_bss_iter` | 1 | mac.c:5204 | mac.c |
| `ieee80211_tx_status_ni` | 1 | pci.c:498 | pci.c |
| `pci_find_ext_capability` | 1 | pci.c:4374 | pci.c |
| `pci_clear_and_set_config_dword` | 1 | pci_be.c:102 | pci_be.c |
| `ieee80211_sta_recalc_aggregates` | 1 | phy.c:3368 | phy.c |

## undeclared identifier (262)

| What | Errors | First seen | Files |
|---|---|---|---|
| `IEEE80211_MLD_MAX_NUM_LINKS` | 183 | core.h:7701 | cam.c, chan.c, coex.c, core.c … |
| `NL80211_BAND_6GHZ` | 149 | core.h:8478 | core.c, core.h, fw.c, mac.c … |
| `IEEE80211_P2P_OPPPS_CTWINDOW_MASK` | 42 | fw.h:3317 | fw.h |
| `UINT_MAX` | 34 | core.h:5935 | core.c, core.h, ps.c |
| `IEEE80211_HE_PHY_CAP3_SU_BEAMFORMER` | 29 | core.h:9069 | core.h, mac.c, mac_be.c |
| `IEEE80211_HE_PHY_CAP4_MU_BEAMFORMER` | 29 | core.h:9071 | core.h, mac.c, mac_be.c |
| `NL80211_RATE_INFO_HE_RU_ALLOC_106` | 28 | core.h:8527 | core.c, core.h |
| `IEEE80211_HE_PHY_CAP7_HE_SU_MU_PPDU_4XLTF_AND_08_US_GI` | 28 | core.h:9080 | core.c, core.h |
| `IEEE80211_HE_PHY_CAP8_HE_ER_SU_PPDU_4XLTF_AND_08_US_GI` | 28 | core.h:9090 | core.c, core.h |
| `ESRCH` | 27 | core.h:6340 | core.h |
| `NL80211_RATE_INFO_HE_RU_ALLOC_26` | 27 | core.h:8523 | core.h |
| `NL80211_RATE_INFO_HE_RU_ALLOC_52` | 27 | core.h:8525 | core.h |
| `NL80211_RATE_INFO_HE_RU_ALLOC_242` | 27 | core.h:8529 | core.h |
| `NL80211_RATE_INFO_HE_RU_ALLOC_484` | 27 | core.h:8531 | core.h |
| `NL80211_RATE_INFO_HE_RU_ALLOC_996` | 27 | core.h:8533 | core.h |
| `NL80211_RATE_INFO_HE_RU_ALLOC_2x996` | 27 | core.h:8535 | core.h |
| `IEEE80211_P2P_OPPPS_ENABLE_BIT` | 21 | fw.h:3315 | fw.h |
| `ENOLINK` | 11 | cam.c:373 | cam.c, core.c, mac80211.c |
| `S16_MIN` | 8 | acpi.c:709 | acpi.c |
| `S16_MAX` | 6 | acpi.c:541 | acpi.c |
| `IEEE80211_HT_CTL_LEN` | 5 | core.c:1016 | core.c |
| `IEEE80211_HE_PHY_CAP1_LDPC_CODING_IN_PAYLOAD` | 4 | core.c:6195 | core.c, mac.c, mac_be.c, phy.c |
| `IEEE80211_HE_PHY_CAP2_STBC_RX_UNDER_80MHZ` | 4 | core.c:6199 | core.c, mac.c, mac_be.c, phy.c |
| `IEEE80211_PPE_THRES_NSS_MASK` | 4 | fw.c:3876 | fw.c |
| `NL80211_RATE_INFO_HE_GI_3_2` | 3 | core.c:2388 | core.c, phy.c |
| `NL80211_RATE_INFO_EHT_GI_3_2` | 3 | core.c:2410 | core.c, phy.c |
| `IEEE80211_RADIOTAP_HE_DATA1_BSS_COLOR_KNOWN` | 3 | core.c:3315 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA1_SPTL_REUSE_KNOWN` | 3 | core.c:3316 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA2_TXOP_KNOWN` | 3 | core.c:3321 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA3_BSS_COLOR` | 3 | core.c:3336 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA6_TXOP` | 3 | core.c:3342 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA1_FORMAT_MU` | 3 | core.c:3658 | core.c |
| `NL80211_TID_CONFIG_ATTR_AMPDU_CTRL` | 3 | core.c:6030 | core.c |
| `NL80211_TID_CONFIG_ATTR_AMSDU_CTRL` | 3 | core.c:6043 | core.c |
| `IEEE80211_HE_PHY_CAP9_NOMINAL_PKT_PADDING_MASK` | 3 | core.c:6232 | core.c, fw.c |
| `rfc1042_header` | 3 | fw.c:32 | fw.c |
| `IEEE80211_PPE_THRES_INFO_PPET_SIZE` | 3 | fw.c:3863 | fw.c |
| `IEEE80211_EHT_PPE_THRES_INFO_PPET_SIZE` | 3 | fw.c:4010 | fw.c |
| `IEEE80211_HE_PHY_CAP5_BEAMFORMEE_NUM_SND_DIM_UNDER_80MHZ_MASK` | 3 | mac.c:6800 | mac.c, mac_be.c |
| `IEEE80211_STA_NONE` | 3 | mac80211.c:948 | mac80211.c |
| `IEEE80211_STA_AUTH` | 3 | mac80211.c:951 | mac80211.c |
| `WLAN_OUI_WFA` | 3 | ps.c:411 | ps.c |
| `NUM_NL80211_IFTYPES` | 2 | chan.c:3088 | chan.c, core.c |
| `IEEE80211_QOS_CTL_TAG1D_MASK` | 2 | core.c:662 | core.c |
| `NL80211_RATE_INFO_HE_GI_0_8` | 2 | core.c:2383 | core.c, phy.c |
| `NL80211_RATE_INFO_HE_GI_1_6` | 2 | core.c:2386 | core.c, phy.c |
| `NL80211_RATE_INFO_EHT_GI_0_8` | 2 | core.c:2405 | core.c, phy.c |
| `NL80211_RATE_INFO_EHT_GI_1_6` | 2 | core.c:2408 | core.c, phy.c |
| `RX_ENC_FLAG_LDPC` | 2 | core.c:3235 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA1_UL_DL_KNOWN` | 2 | core.c:3314 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA1_LDPC_XSYMSEG_KNOWN` | 2 | core.c:3317 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA1_DOPPLER_KNOWN` | 2 | core.c:3318 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA2_PRE_FEC_PAD_KNOWN` | 2 | core.c:3323 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA2_PE_DISAMBIG_KNOWN` | 2 | core.c:3324 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA3_UL_DL` | 2 | core.c:3331 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA4_SU_MU_SPTL_REUSE` | 2 | core.c:3339 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA3_LDPC_XSYMSEG` | 2 | core.c:3345 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA5_PRE_FEC_PAD` | 2 | core.c:3351 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA5_PE_DISAMBIG` | 2 | core.c:3354 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA6_DOPPLER` | 2 | core.c:3357 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA1_FORMAT_TRIG` | 2 | core.c:3662 | core.c |
| `RX_ENC_EHT` | 2 | core.c:3812 | core.c |
| `RX_ENC_FLAG_SHORT_GI` | 2 | core.c:4420 | core.c |
| `NL80211_TID_CONFIG_ENABLE` | 2 | core.c:6031 | core.c |
| `IEEE80211_VHT_CAP_SUPP_CHAN_WIDTH_160MHZ` | 2 | core.c:6133 | core.c, mac_be.c |
| `IEEE80211_HW_SUPPORTS_VHT_EXT_NSS_BW` | 2 | core.c:6140 | core.c |
| `IEEE80211_HE_PHY_CAP3_DCM_MAX_CONST_RX_16_QAM` | 2 | core.c:6201 | core.c, phy.c |
| `IEEE80211_HE_PHY_CAP6_PARTIAL_BW_EXT_RANGE` | 2 | core.c:6217 | core.c, phy.c |
| `IEEE80211_EHT_PHY_CAP5_COMMON_NOMINAL_PKT_PAD_MASK` | 2 | core.c:6325 | core.c, fw.c |
| `IEEE80211_EHT_MCS_NSS_RX` | 2 | core.c:6331 | core.c, phy.c |
| `IEEE80211_HE_PHY_CAP6_PPE_THRESHOLD_PRESENT` | 2 | fw.c:3847 | fw.c |
| `IEEE80211_PPE_THRES_RU_INDEX_BITMASK_MASK` | 2 | fw.c:3861 | fw.c |
| `IEEE80211_STA_NOTEXIST` | 2 | mac80211.c:947 | mac80211.c |
| `IEEE80211_STA_ASSOC` | 2 | mac80211.c:952 | mac80211.c |
| `SURVEY_INFO_NOISE_DBM` | 2 | mac80211.c:1975 | mac80211.c |
| `kzalloc_flex` | 1 | acpi.c:118 | acpi.c |
| `IEEE80211_CHANCTX_CHANGE_WIDTH` | 1 | chan.c:3382 | chan.c |
| `IEEE80211_CHANCTX_CHANGE_PUNCTURING` | 1 | chan.c:3387 | chan.c |
| `WLAN_EXT_CAPA1_EXT_CHANNEL_SWITCHING` | 1 | core.c:209 | core.c |
| `WLAN_EXT_CAPA3_MULTI_BSSID_SUPPORT` | 1 | core.c:210 | core.c |
| `WLAN_EXT_CAPA8_OPMODE_NOTIF` | 1 | core.c:211 | core.c |
| `IEEE80211_TX_CTL_NO_CCK_RATE` | 1 | core.c:761 | core.c |
| `IEEE80211_QOS_CTL_LEN` | 1 | core.c:1052 | core.c |
| `IEEE80211_QOS_CTL_EOSP` | 1 | core.c:1053 | core.c |
| `IEEE80211_TX_CTRL_PORT_CTRL_PROTO` | 1 | core.c:1165 | core.c |
| `IEEE80211_TRIGGER_TYPE_MASK` | 1 | core.c:2497 | core.c |
| `IEEE80211_TRIGGER_TYPE_BASIC` | 1 | core.c:2498 | core.c |
| `IEEE80211_TRIGGER_TYPE_MU_BAR` | 1 | core.c:2498 | core.c |
| `IEEE80211_TRIGGER_ULBW_MASK` | 1 | core.c:2507 | core.c |
| `IEEE80211_TRIGGER_ULBW_160_80P80MHZ` | 1 | core.c:2532 | core.c |
| `RX_ENC_FLAG_BF` | 1 | core.c:3233 | core.c |
| `RX_ENC_FLAG_STBC_MASK` | 1 | core.c:3237 | core.c |
| `RX_FLAG_RADIOTAP_VHT` | 1 | core.c:3262 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA1_BEAM_CHANGE_KNOWN` | 1 | core.c:3313 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA2_TXBF_KNOWN` | 1 | core.c:3322 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA3_BEAM_CHANGE` | 1 | core.c:3328 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA5_TXBF` | 1 | core.c:3348 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA1_SPTL_REUSE2_KNOWN` | 1 | core.c:3387 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA1_SPTL_REUSE3_KNOWN` | 1 | core.c:3388 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA1_SPTL_REUSE4_KNOWN` | 1 | core.c:3389 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA4_TB_SPTL_REUSE1` | 1 | core.c:3397 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA4_TB_SPTL_REUSE2` | 1 | core.c:3399 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA4_TB_SPTL_REUSE3` | 1 | core.c:3401 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA4_TB_SPTL_REUSE4` | 1 | core.c:3403 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA2_MIDAMBLE_KNOWN` | 1 | core.c:3544 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA6_MIDAMBLE_PDCTY` | 1 | core.c:3577 | core.c |
| `IEEE80211_RADIOTAP_HE_MU_FLAGS1_SIG_B_MCS_KNOWN` | 1 | core.c:3589 | core.c |
| `IEEE80211_RADIOTAP_HE_MU_FLAGS1_SIG_B_DCM_KNOWN` | 1 | core.c:3590 | core.c |
| `IEEE80211_RADIOTAP_HE_MU_FLAGS1_SIG_B_SYMS_USERS_KNOWN` | 1 | core.c:3591 | core.c |
| `IEEE80211_RADIOTAP_HE_MU_FLAGS1_SIG_B_COMP_KNOWN` | 1 | core.c:3592 | core.c |
| `IEEE80211_RADIOTAP_HE_MU_FLAGS2_BW_FROM_SIG_A_BW_KNOWN` | 1 | core.c:3593 | core.c |
| `IEEE80211_RADIOTAP_HE_MU_FLAGS1_SIG_B_MCS` | 1 | core.c:3596 | core.c |
| `IEEE80211_RADIOTAP_HE_MU_FLAGS1_SIG_B_DCM` | 1 | core.c:3599 | core.c |
| `IEEE80211_RADIOTAP_HE_MU_FLAGS2_SIG_B_SYMS_USERS` | 1 | core.c:3602 | core.c |
| `IEEE80211_RADIOTAP_HE_MU_FLAGS2_SIG_B_COMP` | 1 | core.c:3605 | core.c |
| `IEEE80211_RADIOTAP_HE_MU_FLAGS2_BW_FROM_SIG_A_BW` | 1 | core.c:3608 | core.c |
| `IEEE80211_RADIOTAP_HE_MU_FLAGS2_PUNC_FROM_SIG_A_BW_KNOWN` | 1 | core.c:3614 | core.c |
| `IEEE80211_RADIOTAP_HE_MU_FLAGS2_PUNC_FROM_SIG_A_BW` | 1 | core.c:3616 | core.c |
| `IEEE80211_RADIOTAP_HE_MU_FLAGS1_CH1_RU_KNOWN` | 1 | core.c:3630 | core.c |
| `IEEE80211_RADIOTAP_HE_MU_FLAGS1_CH2_RU_KNOWN` | 1 | core.c:3631 | core.c |
| `IEEE80211_RADIOTAP_HE_MU_FLAGS1_CH1_CTR_26T_RU_KNOWN` | 1 | core.c:3632 | core.c |
| `IEEE80211_RADIOTAP_HE_MU_FLAGS1_CH2_CTR_26T_RU_KNOWN` | 1 | core.c:3633 | core.c |
| `IEEE80211_RADIOTAP_HE_MU_FLAGS1_CH1_CTR_26T_RU` | 1 | core.c:3646 | core.c |
| `IEEE80211_RADIOTAP_HE_MU_FLAGS2_CH2_CTR_26T_RU` | 1 | core.c:3650 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA1_FORMAT_EXT_SU` | 1 | core.c:3660 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA1_FORMAT_SU` | 1 | core.c:3664 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA1_DATA_MCS_KNOWN` | 1 | core.c:3675 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA1_CODING_KNOWN` | 1 | core.c:3676 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA1_STBC_KNOWN` | 1 | core.c:3677 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA1_BW_RU_ALLOC_KNOWN` | 1 | core.c:3678 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA2_GI_KNOWN` | 1 | core.c:3679 | core.c |
| `IEEE80211_RADIOTAP_HE_DATA1_FORMAT_MASK` | 1 | core.c:3700 | core.c |
| `IEEE80211_RADIOTAP_EHT_USIG_COMMON_BW_20MHZ` | 1 | core.c:3723 | core.c |
| `RATE_INFO_BW_5` | 1 | core.c:3724 | core.c |
| `RX_FLAG_RADIOTAP_TLV_AT_END` | 1 | core.c:3749 | core.c |
| `IEEE80211_RADIOTAP_EHT` | 1 | core.c:3755 | core.c |
| `IEEE80211_RADIOTAP_EHT_KNOWN_GI` | 1 | core.c:3759 | core.c |
| `IEEE80211_RADIOTAP_EHT_DATA0_GI` | 1 | core.c:3761 | core.c |
| `IEEE80211_RADIOTAP_EHT_USER_INFO_MCS_KNOWN` | 1 | core.c:3764 | core.c |
| `IEEE80211_RADIOTAP_EHT_USER_INFO_NSS_KNOWN_O` | 1 | core.c:3765 | core.c |
| `IEEE80211_RADIOTAP_EHT_USER_INFO_CODING_KNOWN` | 1 | core.c:3766 | core.c |
| `IEEE80211_RADIOTAP_EHT_USER_INFO_MCS` | 1 | core.c:3768 | core.c |
| `IEEE80211_RADIOTAP_EHT_USER_INFO_NSS_O` | 1 | core.c:3769 | core.c |
| `IEEE80211_RADIOTAP_EHT_USER_INFO_CODING` | 1 | core.c:3772 | core.c |
| `IEEE80211_RADIOTAP_EHT_USIG` | 1 | core.c:3776 | core.c |
| `IEEE80211_RADIOTAP_EHT_USIG_COMMON_BW_KNOWN` | 1 | core.c:3788 | core.c |
| `IEEE80211_RADIOTAP_EHT_USIG_COMMON_BW` | 1 | core.c:3789 | core.c |
| `NET_SKB_PAD` | 1 | core.c:3805 | core.c |
| `IEEE80211_SN_MASK` | 1 | core.c:3903 | core.c |
| `IEEE80211_HE_OPERATION_ER_SU_DISABLE` | 1 | core.c:5867 | core.c |
| `IEEE80211_VHT_EXT_NSS_BW_CAPABLE` | 1 | core.c:6142 | core.c |
| `IEEE80211_HE_MCS_SUPPORT_0_11` | 1 | core.c:6163 | core.c |
| `IEEE80211_HE_MCS_NOT_SUPPORTED` | 1 | core.c:6165 | core.c |
| `IEEE80211_HE_MAC_CAP0_HTC_HE` | 1 | core.c:6173 | core.c |
| `IEEE80211_HE_MAC_CAP1_TF_MAC_PAD_DUR_16US` | 1 | core.c:6175 | core.c |
| `IEEE80211_HE_MAC_CAP2_ALL_ACK` | 1 | core.c:6176 | core.c |
| `IEEE80211_HE_MAC_CAP2_BSR` | 1 | core.c:6177 | core.c |
| `IEEE80211_HE_MAC_CAP3_MAX_AMPDU_LEN_EXP_EXT_2` | 1 | core.c:6178 | core.c |
| `IEEE80211_HE_MAC_CAP3_OMI_CONTROL` | 1 | core.c:6180 | core.c |
| `IEEE80211_HE_MAC_CAP4_OPS` | 1 | core.c:6181 | core.c |
| `IEEE80211_HE_MAC_CAP4_AMSDU_IN_AMPDU` | 1 | core.c:6182 | core.c |
| `IEEE80211_HE_MAC_CAP5_HT_VHT_TRIG_FRAME_RX` | 1 | core.c:6184 | core.c |
| `IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_40MHZ_IN_2G` | 1 | core.c:6187 | core.c |
| `IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_40MHZ_80MHZ_IN_5G` | 1 | core.c:6190 | core.c |
| `IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_160MHZ_IN_5G` | 1 | core.c:6192 | core.c |
| `IEEE80211_HE_PHY_CAP1_DEVICE_CLASS_A` | 1 | core.c:6194 | core.c |
| `IEEE80211_HE_PHY_CAP1_HE_LTF_AND_GI_FOR_HE_PPDUS_0_8US` | 1 | core.c:6196 | core.c |
| `IEEE80211_HE_PHY_CAP2_NDP_4x_LTF_AND_3_2US` | 1 | core.c:6197 | core.c |
| `IEEE80211_HE_PHY_CAP2_STBC_TX_UNDER_80MHZ` | 1 | core.c:6198 | core.c |
| `IEEE80211_HE_PHY_CAP2_DOPPLER_TX` | 1 | core.c:6200 | core.c |
| `IEEE80211_HE_PHY_CAP3_DCM_MAX_CONST_TX_16_QAM` | 1 | core.c:6203 | core.c |
| `IEEE80211_HE_PHY_CAP3_DCM_MAX_TX_NSS_2` | 1 | core.c:6204 | core.c |
| `IEEE80211_HE_PHY_CAP3_RX_PARTIAL_BW_SU_IN_20MHZ_MU` | 1 | core.c:6206 | core.c |
| `IEEE80211_HE_PHY_CAP4_SU_BEAMFORMEE` | 1 | core.c:6207 | core.c |
| `IEEE80211_HE_PHY_CAP4_BEAMFORMEE_MAX_STS_UNDER_80MHZ_4` | 1 | core.c:6208 | core.c |
| `IEEE80211_HE_PHY_CAP4_BEAMFORMEE_MAX_STS_ABOVE_80MHZ_4` | 1 | core.c:6210 | core.c |
| `IEEE80211_HE_PHY_CAP5_NG16_SU_FEEDBACK` | 1 | core.c:6212 | core.c |
| `IEEE80211_HE_PHY_CAP5_NG16_MU_FEEDBACK` | 1 | core.c:6213 | core.c |
| `IEEE80211_HE_PHY_CAP6_CODEBOOK_SIZE_42_SU` | 1 | core.c:6214 | core.c |
| `IEEE80211_HE_PHY_CAP6_CODEBOOK_SIZE_75_MU` | 1 | core.c:6215 | core.c |
| `IEEE80211_HE_PHY_CAP6_TRIG_SU_BEAMFORMING_FB` | 1 | core.c:6216 | core.c |
| `IEEE80211_HE_PHY_CAP7_POWER_BOOST_FACTOR_SUPP` | 1 | core.c:6218 | core.c |
| `IEEE80211_HE_PHY_CAP7_MAX_NC_1` | 1 | core.c:6220 | core.c |
| `IEEE80211_HE_PHY_CAP8_HE_ER_SU_1XLTF_AND_08_US_GI` | 1 | core.c:6222 | core.c |
| `IEEE80211_HE_PHY_CAP8_DCM_MAX_RU_996` | 1 | core.c:6223 | core.c |
| `IEEE80211_HE_PHY_CAP8_20MHZ_IN_160MHZ_HE_PPDU` | 1 | core.c:6225 | core.c |
| `IEEE80211_HE_PHY_CAP8_80MHZ_IN_160MHZ_HE_PPDU` | 1 | core.c:6226 | core.c |
| `IEEE80211_HE_PHY_CAP9_LONGER_THAN_16_SIGB_OFDM_SYM` | 1 | core.c:6227 | core.c |
| `IEEE80211_HE_PHY_CAP9_RX_1024_QAM_LESS_THAN_242_TONE_RU` | 1 | core.c:6228 | core.c |
| `IEEE80211_HE_PHY_CAP9_RX_FULL_BW_SU_USING_MU_WITH_COMP_SIGB` | 1 | core.c:6229 | core.c |
| `IEEE80211_HE_PHY_CAP9_RX_FULL_BW_SU_USING_MU_WITH_NON_COMP_SIGB` | 1 | core.c:6230 | core.c |
| `IEEE80211_HE_PHY_CAP9_NOMINAL_PKT_PADDING_16US` | 1 | core.c:6231 | core.c |
| `IEEE80211_HE_PHY_CAP9_TX_1024_QAM_LESS_THAN_242_TONE_RU` | 1 | core.c:6234 | core.c |
| `IEEE80211_HE_6GHZ_CAP_MIN_MPDU_START` | 1 | core.c:6246 | core.c |
| `IEEE80211_HE_6GHZ_CAP_MAX_AMPDU_LEN_EXP` | 1 | core.c:6248 | core.c |
| `IEEE80211_HE_6GHZ_CAP_MAX_MPDU_LEN` | 1 | core.c:6250 | core.c |
| `NL80211_CHAN_WIDTH_320` | 1 | core.c:6277 | core.c |
| `IEEE80211_EHT_MAC_CAP0_MAX_MPDU_LEN_MASK` | 1 | core.c:6288 | core.c |
| `IEEE80211_EHT_PHY_CAP0_NDP_4_EHT_LFT_32_GI` | 1 | core.c:6292 | core.c |
| `IEEE80211_EHT_PHY_CAP0_SU_BEAMFORMEE` | 1 | core.c:6293 | core.c |
| `IEEE80211_EHT_PHY_CAP0_320MHZ_IN_6GHZ` | 1 | core.c:6296 | core.c |
| `IEEE80211_EHT_PHY_CAP0_BEAMFORMEE_SS_80MHZ_MASK` | 1 | core.c:6300 | core.c |
| `IEEE80211_EHT_PHY_CAP1_BEAMFORMEE_SS_80MHZ_MASK` | 1 | core.c:6303 | core.c |
| `IEEE80211_EHT_PHY_CAP1_BEAMFORMEE_SS_160MHZ_MASK` | 1 | core.c:6305 | core.c |
| `IEEE80211_EHT_PHY_CAP1_BEAMFORMEE_SS_320MHZ_MASK` | 1 | core.c:6309 | core.c |
| `IEEE80211_EHT_PHY_CAP3_CODEBOOK_4_2_SU_FDBK` | 1 | core.c:6314 | core.c |
| `IEEE80211_EHT_PHY_CAP3_CODEBOOK_7_5_MU_FDBK` | 1 | core.c:6315 | core.c |
| `IEEE80211_EHT_PHY_CAP3_TRIG_SU_BF_FDBK` | 1 | core.c:6316 | core.c |
| `IEEE80211_EHT_PHY_CAP3_TRIG_MU_BF_PART_BW_FDBK` | 1 | core.c:6317 | core.c |
| `IEEE80211_EHT_PHY_CAP4_POWER_BOOST_FACT_SUPP` | 1 | core.c:6320 | core.c |
| `IEEE80211_EHT_PHY_CAP4_MAX_NC_MASK` | 1 | core.c:6321 | core.c |
| `IEEE80211_EHT_PHY_CAP5_COMMON_NOMINAL_PKT_PAD_20US` | 1 | core.c:6324 | core.c |
| `IEEE80211_EHT_MCS_NSS_TX` | 1 | core.c:6332 | core.c |
| `IEEE80211_WMM_IE_STA_QOSINFO_SP_ALL` | 1 | core.c:7485 | core.c |
| `IEEE80211_RADIOTAP_MCS_HAVE_FEC` | 1 | core.c:7487 | core.c |
| `IEEE80211_RADIOTAP_MCS_HAVE_STBC` | 1 | core.c:7488 | core.c |
| `IEEE80211_RADIOTAP_VHT_KNOWN_STBC` | 1 | core.c:7489 | core.c |
| `IEEE80211_RADIOTAP_VHT_KNOWN_BEAMFORMED` | 1 | core.c:7490 | core.c |
| `IEEE80211_HW_SUPPORTS_MULTI_BSSID` | 1 | core.c:7504 | core.c |
| `IEEE80211_HW_CHANCTX_STA_CSA` | 1 | core.c:7506 | core.c |
| `IEEE80211_HW_CONNECTION_MONITOR` | 1 | core.c:7512 | core.c |
| `IEEE80211_HW_AP_LINK_PS` | 1 | core.c:7515 | core.c |
| `WIPHY_FLAG_AP_UAPSD` | 1 | core.c:7532 | core.c |
| `WIPHY_FLAG_HAS_CHANNEL_SWITCH` | 1 | core.c:7533 | core.c |
| `WIPHY_FLAG_SUPPORTS_EXT_KEK_KCK` | 1 | core.c:7534 | core.c |
| `WIPHY_FLAG_SPLIT_SCAN_6GHZ` | 1 | core.c:7537 | core.c |
| `WIPHY_FLAG_SUPPORTS_MLO` | 1 | core.c:7540 | core.c |
| `kzalloc_objs` | 1 | fw.c:1147 | fw.c |
| `WLAN_CATEGORY_SA_QUERY` | 1 | fw.c:2951 | fw.c |
| `WLAN_ACTION_SA_QUERY_RESPONSE` | 1 | fw.c:2952 | fw.c |
| `IEEE80211_STYPE_DATA` | 1 | fw.c:2991 | fw.c |
| `IEEE80211_EHT_PHY_CAP5_PPE_THRESHOLD_PRESENT` | 1 | fw.c:3992 | fw.c |
| `IEEE80211_EHT_PPE_THRES_RU_INDEX_BITMASK_MASK` | 1 | fw.c:4007 | fw.c |
| `IEEE80211_EHT_PPE_THRES_INFO_HEADER_SIZE` | 1 | fw.c:4009 | fw.c |
| `IEEE80211_EML_CAP_EML_PADDING_DELAY_256US` | 1 | fw.c:5117 | fw.c |
| `IEEE80211_EML_CAP_EMLSR_TRANSITION_DELAY_256US` | 1 | fw.c:5119 | fw.c |
| `NL80211_SCAN_FLAG_COLOCATED_6GHZ` | 1 | fw.c:10117 | fw.c |
| `WLAN_EXT_CAPA10_OBSS_NARROW_BW_RU_TOLERANCE_SUPPORT` | 1 | mac.c:5172 | mac.c |
| `IEEE80211_TX_CTL_TX_OFFCHAN` | 1 | mac80211.c:31 | mac80211.c |
| `BSS_CHANGED_MLD_VALID_LINKS` | 1 | mac80211.c:730 | mac80211.c |
| `BSS_CHANGED_HE_BSS_COLOR` | 1 | mac80211.c:784 | mac80211.c |
| `BSS_CHANGED_TPE` | 1 | mac80211.c:796 | mac80211.c |
| `IEEE80211_ROC_TYPE_MGMT_TX` | 1 | mac80211.c:1528 | mac80211.c |
| `NAPI_POLL_WEIGHT` | 1 | pci.c:942 | pci.c |
| `PCI_EXP_DEVCTL2_LTR_EN` | 1 | pci.c:3050 | pci.c |
| `PCI_VENDOR_ID_ASMEDIA` | 1 | pci.c:3325 | pci.c |
| `PCI_VENDOR_ID_SPACEMIT` | 1 | pci.c:3329 | pci.c |
| `PCI_DEVICE_ID_SPACEMIT_K3` | 1 | pci.c:3330 | pci.c |
| `PCI_EXT_CAP_ID_L1SS` | 1 | pci.c:4374 | pci.c |
| `PCI_L1SS_CTL1` | 1 | pci.c:4378 | pci.c |
| `PCI_L1SS_CTL1_L1SS_MASK` | 1 | pci.c:4380 | pci.c |
| `IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_80PLUS80_MHZ_IN_5G` | 1 | phy.c:87 | phy.c |
| `IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_MASK_ALL` | 1 | phy.c:142 | phy.c |
| `IEEE80211_RC_NSS_CHANGED` | 1 | phy.c:524 | phy.c |
| `RATE_INFO_FLAGS_EHT_MCS` | 1 | phy.c:3319 | phy.c |
| `WLAN_OUI_TYPE_WFA_P2P` | 1 | ps.c:414 | ps.c |
| `IEEE80211_P2P_ATTR_ABSENCE_NOTICE` | 1 | ps.c:416 | ps.c |
| `IEEE80211_TPE_CAT_6GHZ_DEFAULT` | 1 | regd.c:993 | regd.c |
| `IEEE80211_REG_VLP_AP` | 1 | regd.c:1153 | regd.c |
| `IEEE80211_REG_LPI_AP` | 1 | regd.c:1156 | regd.c |
| `IEEE80211_REG_SP_AP` | 1 | regd.c:1159 | regd.c |
| `ENODATA` | 1 | sar.c:84 | sar.c |

## missing struct member (59)

| What | Errors | First seen | Files |
|---|---|---|---|
| `ieee80211_link_sta.he_cap` | 137 | core.h:9068 | coex.c, core.c, core.h, fw.c … |
| `rtw89_dev.assoc_link_on_macid` | 81 | core.h:7852 | core.h |
| `atomic_t.refs` | 60 | rtw89_compat.h:264 | rtw89_compat.h |
| `rtw89_tx_skb_data.wait` | 55 | core.h:9138 | core.c, core.h |
| `ieee80211_tx_info.driver_data` | 31 | core.h:8050 | core.h, pci.h |
| `rtw89_wait_info.resp` | 30 | core.h:5934 | core.c, core.h |
| `rtw89_vif.snap_link_confs` | 29 | core.h:8394 | core.h, mac80211.c |
| `ieee80211_vif.link_conf` | 28 | core.h:8400 | core.c, core.h |
| `ieee80211_sta.link` | 27 | core.h:8432 | core.h |
| `ieee80211_bss_conf.link_id` | 14 | mac.c:6924 | mac.c, mac80211.c |
| `ieee80211_link_sta.eht_cap` | 12 | core.c:5949 | core.c, fw.c, phy.c |
| `ieee80211_bss_conf.he_support` | 7 | core.c:5718 | core.c, fw.c, mac.c, phy.c |
| `ieee80211_bss_conf.eht_support` | 6 | chan.c:170 | chan.c, core.c, fw.c, mac.c |
| `ieee80211_rx_status.enc_flags` | 6 | core.c:3233 | core.c |
| `ieee80211_link_sta.rx_nss` | 6 | fw.c:3840 | fw.c, phy.c |
| `ieee80211_vif.active_links` | 5 | coex.c:9311 | coex.c, core.c, mac80211.c |
| `ieee80211_bss_conf.addr` | 5 | core.c:3141 | core.c, fw.c, mac80211.c |
| `ieee80211_chanctx_conf.def` | 4 | chan.c:3360 | chan.c |
| `ieee80211_bss_conf.chanreq` | 4 | core.c:5432 | core.c, fw.c, mac.c |
| `wiphy.tid_config_support` | 4 | core.c:7555 | core.c |
| `ieee80211_bss_conf.he_bss_color` | 3 | cam.c:777 | cam.c, mac.c, phy.c |
| `ieee80211_bss_conf.nontransmitted` | 3 | cam.c:779 | cam.c, core.c, mac.c |
| `ieee80211_sta.mlo` | 3 | cam.c:1021 | cam.c, core.c |
| `ieee80211_rx_status.eht` | 3 | core.c:2432 | core.c |
| `rate_info.eht_gi` | 3 | phy.c:3323 | phy.c |
| `ieee80211_link_sta.agg` | 3 | phy.c:3343 | phy.c |
| `ieee80211_link_sta.addr` | 2 | cam.c:838 | cam.c, core.c |
| `ieee80211_rx_status.he_gi` | 2 | core.c:2434 | core.c |
| `ieee80211_sta.max_amsdu_subframes` | 2 | core.c:6045 | core.c |
| `wiphy.fw_version` | 2 | fw.c:761 | fw.c |
| `cfg80211_scan_request.n_6ghz_params` | 2 | fw.c:9521 | fw.c |
| `ieee80211_key_conf.link_id` | 1 | cam.c:366 | cam.c |
| `cfg80211_chan_def.punctured` | 1 | chan.c:177 | chan.c |
| `ieee80211_key_conf.tx_pn` | 1 | core.c:738 | core.c |
| `ieee80211_bss_conf.transmitter_bssid` | 1 | core.c:3109 | core.c |
| `ieee80211_rx_status.link_valid` | 1 | core.c:3114 | core.c |
| `ieee80211_rx_status.link_id` | 1 | core.c:3115 | core.c |
| `ieee80211_rx_status.he_dcm` | 1 | core.c:3333 | core.c |
| `ieee80211_rx_status.boottime_ns` | 1 | core.c:4382 | core.c |
| `ieee80211_bss_conf.he_oper` | 1 | core.c:5867 | core.c |
| `ieee80211_bss_conf.csa_active` | 1 | core.c:6504 | core.c |
| `ieee80211_hw.chanctx_data_size` | 1 | core.c:7477 | core.c |
| `ieee80211_hw.max_rx_aggregation_subframes` | 1 | core.c:7483 | core.c |
| `ieee80211_hw.max_tx_aggregation_subframes` | 1 | core.c:7484 | core.c |
| `ieee80211_hw.uapsd_max_sp_len` | 1 | core.c:7485 | core.c |
| `ieee80211_hw.radiotap_mcs_details` | 1 | core.c:7487 | core.c |
| `ieee80211_hw.radiotap_vht_details` | 1 | core.c:7489 | core.c |
| `wiphy.iftype_ext_capab` | 1 | core.c:7541 | core.c |
| `wiphy.num_iftype_ext_capab` | 1 | core.c:7542 | core.c |
| `wiphy.max_remain_on_channel_duration` | 1 | core.c:7559 | core.c |
| `cfg80211_scan_request.scan_6ghz_params` | 1 | fw.c:9525 | fw.c |
| `ieee80211_bss_conf.bssid_index` | 1 | mac.c:4966 | mac.c |
| `ieee80211_tx_queue_params.mu_edca` | 1 | mac80211.c:454 | mac80211.c |
| `ieee80211_tx_queue_params.mu_edca_param_rec` | 1 | mac80211.c:457 | mac80211.c |
| `ieee80211_link_sta.link_id` | 1 | mac80211.c:1357 | mac80211.c |
| `ieee80211_vif_chanctx_switch.link_conf` | 1 | mac80211.c:1465 | mac80211.c |
| `ieee80211_supported_band.iftype_data` | 1 | regd.c:645 | regd.c |
| `ieee80211_bss_conf.tpe` | 1 | regd.c:1061 | regd.c |
| `ieee80211_bss_conf.power_type` | 1 | regd.c:1152 | regd.c |

## incomplete type (26)

| What | Errors | First seen | Files |
|---|---|---|---|
| `ieee80211_radiotap_he` | 253 | core.h:9124 | core.c, core.h |
| `ieee80211_radiotap_he_mu` | 236 | core.h:9124 | core.c, core.h |
| `ieee80211_radiotap_tlv` | 229 | core.h:9124 | core.c, core.h |
| `ieee80211_radiotap_vht` | 227 | core.h:9124 | core.c, core.h |
| `ieee80211_radiotap_eht` | 227 | core.h:9124 | core.c, core.h |
| `ieee80211_radiotap_eht_usig` | 112 | core.h:9124 | core.c, core.h |
| `rcu_head` | 54 | core.h:4692 | core.h |
| `led_classdev_mc` | 27 | core.h:5707 | core.h |
| `cfg80211_sched_scan_request` | 18 | fw.c:7732 | fw.c |
| `ieee80211_p2p_noa_desc` | 14 | chan.c:934 | chan.c, coex.c, fw.c, ps.c |
| `ieee80211_eht_cap_elem_fixed` | 14 | core.c:6286 | core.c |
| `ieee80211_eht_mcs_nss_supp` | 9 | core.c:6335 | core.c |
| `ieee80211_sta_he_cap` | 8 | core.c:6169 | core.c, phy.c |
| `ieee80211_sband_iftype_data` | 7 | core.c:6168 | core.c |
| `ieee80211_sta_eht_cap` | 7 | core.c:6281 | core.c, phy.c |
| `survey_info` | 6 | mac80211.c:1974 | mac80211.c |
| `cfg80211_scan_6ghz_params` | 5 | fw.c:9527 | fw.c |
| `ieee80211_trigger` | 4 | core.c:2492 | core.c |
| `cfg80211_tid_cfg` | 4 | core.c:6015 | core.c |
| `ieee80211_he_mu_edca_param_ac_rec` | 3 | mac80211.c:458 | mac80211.c |
| `ieee80211_eht_mcs_nss_supp_bw` | 3 | phy.c:135 | phy.c |
| `cfg80211_tid_config` | 2 | core.c:6058 | core.c |
| `ieee80211_p2p_noa_attr` | 1 | coex.c:9297 | coex.c |
| `cfg80211_bss` | 1 | mac.c:5167 | mac.c |
| `ieee80211_conf` | 1 | mac80211.c:1974 | mac80211.c |
| `ieee80211_eht_mcs_nss_supp_20mhz_only` | 1 | phy.c:145 | phy.c |

## conflicting types (1)

| What | Errors | First seen | Files |
|---|---|---|---|
| `rtw89_core_set_tid_config` | 1 | core.c:6052 | core.c |

## pointer type mismatch (7)

| What | Errors | First seen | Files |
|---|---|---|---|
| `incompatible pointer types returning 'struct (unnamed struct at ./src/compat/net/mac80211.h:537:5) *' from a function with result type 'struct ieee80211_link_sta *' [-Werror,-Wincompatible-pointer-types]` | 27 | core.h:8435 | core.h |
| `incompatible pointer types passing 'struct ieee80211_radiotap_he *' to parameter of type 'struct ieee80211_radiotap_he *' [-Werror,-Wincompatible-pointer-types]` | 3 | core.c:3710 | core.c |
| `incompatible pointer types passing 'struct cfg80211_tid_config *' to parameter of type 'struct cfg80211_tid_config *' [-Werror,-Wincompatible-pointer-types]` | 2 | mac80211.c:1566 | mac80211.c |
| `incompatible pointer types assigning to 'struct ieee80211_p2p_noa_attr *' from 'bool *' [-Werror,-Wincompatible-pointer-types]` | 1 | coex.c:9294 | coex.c |
| `incompatible pointer types passing 'struct ieee80211_radiotap_he_mu *' to parameter of type 'struct ieee80211_radiotap_he_mu *' [-Werror,-Wincompatible-pointer-types]` | 1 | core.c:3710 | core.c |
| `incompatible pointer types assigning to 'struct ieee80211_radiotap_tlv *' from 'u8 *' (aka 'unsigned char *') [-Werror,-Wincompatible-pointer-types]` | 1 | core.c:3753 | core.c |
| `incompatible pointer types initializing 'struct ieee80211_conf *' with an expression of type 'struct (unnamed struct at ./src/compat/net/mac80211.h:361:5) *' [-Werror,-Wincompatible-pointer-types]` | 1 | mac80211.c:1969 | mac80211.c |

## int/pointer conversion (4)

| What | Errors | First seen | Files |
|---|---|---|---|
| `incompatible pointer to integer conversion passing 'u16 *' (aka 'unsigned short *') to parameter of type 'u32' (aka 'unsigned int') [-Wint-conversion]` | 2 | fw.c:2625 | fw.c |
| `incompatible integer to pointer conversion assigning to 'const u8 *' (aka 'const unsigned char *') from 'int' [-Wint-conversion]` | 1 | core.c:2585 | core.c |
| `incompatible integer to pointer conversion assigning to 'struct net_device *' from 'int' [-Wint-conversion]` | 1 | core.c:4583 | core.c |
| `incompatible integer to pointer conversion assigning to 'struct ieee80211_txq *' from 'int' [-Wint-conversion]` | 1 | core.c:4838 | core.c |

## other (18)

| What | Errors | First seen | Files |
|---|---|---|---|
| `expected '…' at end of declaration list` | 108 | core.h:4700 | core.h |
| `array has incomplete element type '…'` | 55 | core.h:4744 | core.c, core.h |
| `typedef redefinition with different types ('…' vs '…')` | 30 | rtw89_compat.h:261 | rtw89_compat.h |
| `field has incomplete type '…'` | 30 | core.h:4719 | core.h, wow.h |
| `incomplete result type '…' in function definition` | 27 | core.h:8516 | core.h |
| `incomplete definition of type '…'` | 23 | mac.c:5168 | mac.c, ps.c, regd.c |
| `no member named '…' in '…'` | 20 | cam.c:1105 | cam.c, core.c, mac80211.c, phy.c |
| `assigning to '…' from incompatible type '…'` | 10 | chan.c:376 | chan.c, core.c, fw.c, mac80211.c |
| `variable has incomplete type '…'` | 8 | core.c:1231 | core.c, mac80211.c |
| `member reference base type '…' is not a structure or union` | 7 | chan.c:933 | chan.c, ps.c |
| `static assertion expression is not an integral constant expression` | 4 | sar.c:389 | sar.c |
| `invalid application of '…' to an incomplete type '…'` | 3 | core.c:318 | core.c, ps.c |
| `expected '…' after expression` | 3 | core.c:3929 | core.c, fw.c, phy.c |
| `too few arguments provided to function-like macro invocation` | 2 | acpi.c:118 | acpi.c, fw.c |
| `invalid application of '…' to an incomplete type '…' (aka '…')` | 2 | core.c:219 | core.c |
| `initializer element is not a compile-time constant` | 2 | mac.c:7457 | mac.c, mac_be.c |
| `invalid operands to binary expression ('…' and '…')` | 1 | fw.c:5102 | fw.c |
| `initializing '…' with an expression of incompatible type '…'` | 1 | mac80211.c:731 | mac80211.c |
