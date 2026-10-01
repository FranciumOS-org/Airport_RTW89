// SPDX-License-Identifier: GPL-2.0
/*
 * core.c with rtw89_chip_info_setup() replaced, for the second smoke-test
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
#include "core.c"
#undef rtw89_chip_info_setup

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
