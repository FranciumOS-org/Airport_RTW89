// SPDX-License-Identifier: GPL-2.0
/*
 * core.c (through its wrapper) with rtw89_chip_info_setup() replaced, for the second smoke-test
 * binary (hosttest_fakechip).
 *
 * The real function powers the chip on, downloads firmware and reads the
 * efuse, which nothing in userspace can answer. This version keeps every step
 * that is pure software (waiting for and parsing the real firmware image, chip
 * data and RFE parameter setup) and fills in by hand what the hardware would
 * have reported. Probe then runs on through IRQ setup and rtw89_core_register()
 * and the test can remove a fully probed device, which the dead-device run
 * never reaches.
 */
#define rtw89_chip_info_setup rtw89_real_chip_info_setup
#define rtw89_core_start rtw89_real_core_start
#define rtw89_core_stop rtw89_real_core_stop
#include "rtw89_core_wrap.c"
#undef rtw89_chip_info_setup
#undef rtw89_core_start
#undef rtw89_core_stop

/*
 * Starting the radio is hardware from top to bottom (MAC/BB/RF init, firmware
 * download, calibration). Pretend it worked, so the test can go on to add an
 * interface, build a scan request and take it all down again; the commands
 * the driver then sends go into a firmware-command ring nobody drains.
 */
int rtw89_core_start(struct rtw89_dev *rtwdev)
{
    /* The driver checks these enable bits before touching the MAC. */
    rtw89_write32_set(rtwdev, R_AX_DMAC_FUNC_EN, B_AX_MAC_FUNC_EN | B_AX_DMAC_FUNC_EN);
    rtw89_write32_set(rtwdev, R_AX_CMAC_FUNC_EN, B_AX_CMAC_EN);

    set_bit(RTW89_FLAG_POWERON, rtwdev->flags);

    /* As the real start does: the periodic tracking works run while up. */
    wiphy_delayed_work_queue(rtwdev->hw->wiphy, &rtwdev->track_work,
                             RTW89_TRACK_WORK_PERIOD);
    wiphy_delayed_work_queue(rtwdev->hw->wiphy, &rtwdev->track_ps_work,
                             RTW89_TRACK_PS_WORK_PERIOD);

    set_bit(RTW89_FLAG_RUNNING, rtwdev->flags);
    return 0;
}

void rtw89_core_stop(struct rtw89_dev *rtwdev)
{
    struct wiphy *wiphy = rtwdev->hw->wiphy;

    if (!test_bit(RTW89_FLAG_RUNNING, rtwdev->flags))
        return;

    clear_bit(RTW89_FLAG_RUNNING, rtwdev->flags);
    wiphy_delayed_work_cancel(wiphy, &rtwdev->track_work);
    wiphy_delayed_work_cancel(wiphy, &rtwdev->track_ps_work);
    cancel_delayed_work_sync(&rtwdev->txq_reinvoke_work);
    /* As the real stop does: frames the chip never reported as sent are
     * released and the rings start from zero. The pretend chip reports none,
     * so without this a second start (Wi-Fi off and on, sleep and wake)
     * finds no transmit room left. */
    rtw89_hci_reset(rtwdev);
    clear_bit(RTW89_FLAG_POWERON, rtwdev->flags);
}

int rtw89_chip_info_setup(struct rtw89_dev *rtwdev)
{
    static const u8 mac[ETH_ALEN] = { 0x00, 0xe0, 0x4c, 0x88, 0x52, 0xbe };
    const struct rtw89_chip_info *chip = rtwdev->chip;
    struct rtw89_efuse *efuse = &rtwdev->efuse;
    struct rtw89_hal *hal = &rtwdev->hal;
    int ret;

    hal->cv = CHIP_CBV;                 /* rtw89_read_chip_ver() */

    ret = rtw89_wait_firmware_completion(rtwdev);
    if (ret)
        return ret;

    ret = rtw89_fw_recognize(rtwdev);
    if (ret)
        return ret;

    /* rtw89_chip_efuse_info_setup(): efuse and PHY capability */
    ether_addr_copy(efuse->addr, mac);
    efuse->rfe_type = 1;
    hal->tx_nss = chip->tx_nss;
    hal->rx_nss = chip->rx_nss;
    hal->antenna_tx = RF_AB;
    hal->antenna_rx = RF_AB;
    rtw89_core_setup_phycap(rtwdev);

    ret = rtw89_fw_recognize_elements(rtwdev);
    if (ret)
        return ret;

    ret = rtw89_chip_board_info_setup(rtwdev);
    if (ret)
        return ret;

    ret = rtw89_chip_data_setup(rtwdev);
    if (ret)
        return ret;

    rtw89_core_setup_rfe_parms(rtwdev);
    rtwdev->ps_mode = rtw89_update_ps_mode(rtwdev);

    return 0;
}
