// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace check of the vendored hostap SAE code (third_party/hostap) on
 * the kext's adaptation of it (src/compat_rtw89/rtw89_hostap.c, Mbed TLS):
 * a station and an access point run the whole exchange, commit then confirm,
 * with the same password (both must end with the same PMK and PMKID, and
 * accept each other's confirm) and with different ones (the confirm must be
 * turned down). Built and run by `make hosttest`.
 */
#include "includes.h"
#include "common.h"
#include "common/defs.h"
#include "utils/wpabuf.h"
#include "ieee802_11_defs.h"
#include "sae.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("== FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static const u8 sta_addr[ETH_ALEN] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
static const u8 ap_addr[ETH_ALEN] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x02 };

/* One side's commit, as it would go into an Authentication frame body after
 * the algorithm, sequence and status fields. */
static struct wpabuf *commit(struct sae_data *sae, const u8 *own, const u8 *peer,
                             const char *password)
{
    struct wpabuf *buf = wpabuf_alloc(512);

    CHECK(sae_set_group(sae, 19) == 0);
    sae->akmp = WPA_KEY_MGMT_SAE;
    CHECK(sae_prepare_commit(own, peer, (const u8 *)password, strlen(password), sae) == 0);
    CHECK(buf && sae_write_commit(sae, buf, NULL, NULL, 0) == 0);
    /* group 19, a 32-byte scalar and a 64-byte element */
    CHECK(wpabuf_len(buf) == 2 + 32 + 64);
    sae->state = SAE_COMMITTED;
    return buf;
}

static int receive_commit(struct sae_data *sae, const struct wpabuf *buf)
{
    static int groups[] = { 19, 0 };
    const u8 *token = NULL;
    size_t token_len = 0;
    u16 status;

    status = sae_parse_commit(sae, wpabuf_head(buf), wpabuf_len(buf), &token, &token_len,
                              groups, 0, NULL);
    if (status != WLAN_STATUS_SUCCESS)
        return -1;
    return sae_process_commit(sae);
}

static int run(const char *sta_pw, const char *ap_pw)
{
    struct sae_data sta, ap;
    struct wpabuf *sta_commit, *ap_commit, *sta_confirm, *ap_confirm;
    int ok;

    memset(&sta, 0, sizeof(sta));
    memset(&ap, 0, sizeof(ap));
    sta_commit = commit(&sta, sta_addr, ap_addr, sta_pw);
    ap_commit = commit(&ap, ap_addr, sta_addr, ap_pw);

    CHECK(receive_commit(&ap, sta_commit) == 0);
    CHECK(receive_commit(&sta, ap_commit) == 0);

    sta_confirm = wpabuf_alloc(64);
    ap_confirm = wpabuf_alloc(64);
    sta.send_confirm = 1;
    ap.send_confirm = 1;
    CHECK(sae_write_confirm(&sta, sta_confirm) == 0);
    CHECK(sae_write_confirm(&ap, ap_confirm) == 0);
    /* send-confirm and the 32-byte confirm */
    CHECK(wpabuf_len(sta_confirm) == 2 + 32);

    ok = sae_check_confirm(&ap, wpabuf_head(sta_confirm), wpabuf_len(sta_confirm), NULL) == 0;
    CHECK(ok == (sae_check_confirm(&sta, wpabuf_head(ap_confirm), wpabuf_len(ap_confirm),
                                   NULL) == 0));
    if (ok) {
        CHECK(sta.pmk_len == 32 && ap.pmk_len == 32);
        CHECK(memcmp(sta.pmk, ap.pmk, 32) == 0);
        CHECK(memcmp(sta.pmkid, ap.pmkid, SAE_PMKID_LEN) == 0);
    }

    wpabuf_free(sta_commit);
    wpabuf_free(ap_commit);
    wpabuf_free(sta_confirm);
    wpabuf_free(ap_confirm);
    sae_clear_data(&sta);
    sae_clear_data(&ap);
    return ok;
}

int main(void)
{
    int i;

    /* several runs: each draws new secrets and a new password element loop */
    for (i = 0; i < 5; i++)
        CHECK(run("correct horse battery", "correct horse battery"));
    CHECK(!run("correct horse battery", "wrong horse battery"));
    printf("== sae test: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
