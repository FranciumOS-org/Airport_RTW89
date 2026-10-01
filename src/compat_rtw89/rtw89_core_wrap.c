// SPDX-License-Identifier: GPL-2.0
/*
 * The driver's core.c, unmodified, plus one function that needs access to its
 * static helpers. This file is compiled in place of third_party/rtw89/core.c.
 *
 * Why: rtw89_core_rx() parks every received data frame on ppdu_sts.rx_queue
 * until the chip's status report for the same transmission (PPDU) arrives, so
 * that the signal measurements can be attached to it; if no report comes, the
 * frame is passed on when the next transmission is received. On the RTL8852BE
 * in this machine the report often did not come for larger frames, and the
 * next thing received was the access point's beacon, up to 102 ms later.
 * rtw89_glue.c therefore calls rtw89_compat_flush_ppdu_rx() a couple of
 * milliseconds after a poll that left frames parked.
 */
#include "core.c"

/* Pass on whatever is still waiting for a PPDU status report. Must run on the
 * thread that calls rtw89_core_rx(). Returns the number of frames. */
unsigned int rtw89_compat_flush_ppdu_rx(struct rtw89_dev *rtwdev);
unsigned int rtw89_compat_flush_ppdu_rx(struct rtw89_dev *rtwdev)
{
    /* what the driver's own flush is given, minus a frame to take it from:
     * band 0, and nothing known about the transmission that ends the wait */
    struct rtw89_rx_desc_info desc_info = {};
    unsigned int n = skb_queue_len(&rtwdev->ppdu_sts.rx_queue[RTW89_PHY_0]);

    if (n)
        rtw89_core_flush_ppdu_rx_queue(rtwdev, &desc_info);
    return n;
}
