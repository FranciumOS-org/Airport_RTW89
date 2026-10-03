// SPDX-License-Identifier: GPL-2.0
/*
 * Self-test for the compat code written for rtw89 (rtw89_compat.h helpers,
 * rtw89_cfg80211.c, rtw89_mac80211.c). Compiled like a driver file, linked
 * only into the userspace smoke test, never into the kext.
 */
#include "rtw89_net80211.h"
#include "rtw89_crypto.h"

static int failures;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            IOLog("== SELFTEST FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                   \
    } while (0)

/* ---- helpers in rtw89_compat.h ---- */

static int guard_early_return(spinlock_t *lock, int *inside)
{
    scoped_guard(spinlock_irqsave, lock) {
        *inside = 1;
        return 7;
    }
    return 0;
}

struct flex {
    u32 n;
    u16 items[];
};

static void test_helpers(void)
{
    struct sk_buff_head a, b;
    struct sk_buff *skb;
    struct flex *f;
    spinlock_t lock;
    int i, inside = 0;
    u8 *before;

    CHECK(FIELD_PREP_CONST(0x00f0, 0x5) == 0x50);
    CHECK(FIELD_GET_SIGNED(0x0f00, 0x0f00) == -1);
    CHECK(FIELD_GET_SIGNED(0x0f00, 0x0700) == 7);
    CHECK(u32_replace_bits(0xffffffff, 0x2, 0x0000000c) == 0xfffffffb);
    CHECK(sign_extend32(0x80, 7) == -128);
    CHECK(hweight16(0xf0f0) == 8);
    CHECK(BITS_TO_LONGS(65) == 2);
    CHECK(DIV_ROUND_UP_ULL(10, 4) == 3);
    CHECK(IS_ENABLED(CONFIG_CFG80211) == 1);
    CHECK(IS_ENABLED(CONFIG_MAC80211_LEDS) == 0);

    /* min()/max() evaluate each argument once */
    i = 0;
    CHECK(min(i++, 5) == 0);
    CHECK(i == 1);
    CHECK(max_t(u8, 300, 7) == 44);     /* (u8)300 == 44 */
    CHECK(clamp(15, 0, 10) == 10);

    /* scoped_guard releases on early return: a second take must not block
     * forever (the lock is recursive, so check through the return value). */
    spin_lock_init(&lock);
    CHECK(guard_early_return(&lock, &inside) == 7);
    CHECK(inside == 1);
    {
        guard(rcu)();
    }
    spin_lock_destroy(&lock);

    f = kzalloc_flex(*f, items, 10);
    CHECK(f != NULL);
    if (f) {
        f->items[9] = 0xbeef;
        kfree(f);
    }
    f = (struct flex *)kzalloc_objs(u32, 4);
    CHECK(f != NULL);
    kfree(f);

    /* lockless skb queue splice keeps order and counts */
    __skb_queue_head_init(&a);
    __skb_queue_head_init(&b);
    for (i = 0; i < 3; i++) {
        skb = dev_alloc_skb(32);
        skb->priority = i;
        __skb_queue_tail(&a, skb);
    }
    skb = dev_alloc_skb(32);
    skb->priority = 9;
    __skb_queue_tail(&b, skb);
    skb_queue_splice_init(&a, &b);      /* a's frames go in front of b's */
    CHECK(skb_queue_len(&a) == 0 && skb_queue_len(&b) == 4);
    for (i = 0; i < 4; i++) {
        skb = skb_peek(&b);
        CHECK(skb && skb->priority == (i < 3 ? (u32)i : 9));
        __skb_unlink(skb, &b);
        kfree_skb(skb);
    }

    /* dev_alloc_skb reserves NET_SKB_PAD; skb_copy keeps head- and tailroom */
    skb = dev_alloc_skb(100);
    CHECK(skb_headroom(skb) == NET_SKB_PAD && skb_tailroom(skb) >= 100);
    skb_reserve(skb, 10);
    memcpy(skb_put(skb, 5), "hello", 5);
    skb->priority = 6;
    {
        struct sk_buff *copy = skb_copy(skb, GFP_KERNEL);

        CHECK(copy && copy->len == 5 && !memcmp(copy->data, "hello", 5));
        CHECK(copy && skb_headroom(copy) == skb_headroom(skb));
        CHECK(copy && skb_tailroom(copy) >= skb_tailroom(skb));
        CHECK(copy && copy->priority == 6);
        kfree_skb(copy);
    }
    kfree_skb(skb);

    /* pskb_expand_head keeps the payload and adds headroom */
    skb = dev_alloc_skb(64);
    skb_reserve(skb, 8);
    memcpy(skb_put(skb, 4), "abcd", 4);
    before = skb->head;
    CHECK(pskb_expand_head(skb, 32, 0, GFP_KERNEL) == 0);
    CHECK(skb->head != before);
    CHECK(skb_headroom(skb) == NET_SKB_PAD + 40 && skb->len == 4 && !memcmp(skb->data, "abcd", 4));
    kfree_skb(skb);
}

/* ---- WPA2 cryptography against published test vectors ---- */

static bool hex_is(const u8 *got, const char *hex, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        char c[3] = { hex[2 * i], hex[2 * i + 1], 0 };
        u8 v = 0;
        int j;

        for (j = 0; j < 2; j++)
            v = (u8)(v << 4 | (c[j] <= '9' ? c[j] - '0' : c[j] - 'a' + 10));
        if (got[i] != v)
            return false;
    }
    return true;
}

static void test_crypto(void)
{
    static const u8 aa[6] = { 0xa0, 0xa1, 0xa1, 0xa3, 0xa4, 0xa5 };
    static const u8 spa[6] = { 0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5 };
    u8 out[64], key[20], in[16], wrapped[24], data[76];
    int i;

    rtw89_sha1((const u8 *)"abc", 3, out);                      /* FIPS 180 */
    CHECK(hex_is(out, "a9993e364706816aba3e25717850c26c9cd0d89d", 20));

    rtw89_hmac_sha1((const u8 *)"Jefe", 4,                      /* RFC 2202 */
                    (const u8 *)"what do ya want for nothing?", 28, out);
    CHECK(hex_is(out, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79", 20));

    /* IEEE 802.11 J.4.2: passphrase to PSK */
    rtw89_pbkdf2_sha1((const u8 *)"password", 8, (const u8 *)"IEEE", 4, 4096, out, 32);
    CHECK(hex_is(out, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e", 32));

    /* IEEE 802.11 J.3.2: PRF */
    memset(key, 0x0b, sizeof(key));
    rtw89_sha1_prf(key, 20, "prefix", (const u8 *)"Hi There", 8, out, 24);
    CHECK(hex_is(out, "bcd4c650b30b9684951829e0d75f9d54b862175ed9f00606", 24));

    /* IEEE 802.11 J.7.1: pairwise key derivation (KCK, KEK, TK) */
    rtw89_pbkdf2_sha1((const u8 *)"ThisIsAPassword", 15, (const u8 *)"ThisIsASSID", 11,
                      4096, out, 32);
    CHECK(hex_is(out, "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af", 32));
    memcpy(data, aa, 6);
    memcpy(data + 6, spa, 6);
    for (i = 0; i < 32; i++) {
        /* SNonce c0..e5 sorts before ANonce e0..05, both in the vector's odd numbering */
        static const u8 snonce[32] = {
            0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xd0, 0xd1, 0xd2,
            0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf,
            0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5,
        };
        static const u8 anonce[32] = {
            0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xf0, 0xf1, 0xf2,
            0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
        };

        data[12 + i] = snonce[i];
        data[44 + i] = anonce[i];
    }
    {
        u8 pmk[32];

        memcpy(pmk, out, 32);
        rtw89_sha1_prf(pmk, 32, "Pairwise key expansion", data, sizeof(data), out, 48);
    }
    CHECK(hex_is(out, "379f9852d0199236b94e407ce4c00ec8"
                      "47c9edc01c2c6e5b4910caddfb3e51a7"
                      "b2360c79e9710fdd58bea93deaf06599", 48));

    /* FIPS 197 appendix C.1, and RFC 3394 4.1 */
    for (i = 0; i < 16; i++) {
        key[i] = (u8)i;
        in[i] = (u8)(i * 0x11);
    }
    rtw89_aes128_encrypt(key, in, out);
    CHECK(hex_is(out, "69c4e0d86a7b0430d8cdb78070b4c55a", 16));
    rtw89_aes128_decrypt(key, out, out + 16);
    CHECK(!memcmp(out + 16, in, 16));
    rtw89_aes_wrap(key, in, 16, wrapped);
    CHECK(hex_is(wrapped, "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5", 24));
    CHECK(rtw89_aes_unwrap(key, wrapped, 24, out) && !memcmp(out, in, 16));
    wrapped[5] ^= 0x01;
    CHECK(!rtw89_aes_unwrap(key, wrapped, 24, out));

    /* SHA-256: FIPS 180-4 examples, and a message over several blocks */
    rtw89_sha256((const u8 *)"abc", 3, out);
    CHECK(hex_is(out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", 32));
    rtw89_sha256((const u8 *)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, out);
    CHECK(hex_is(out, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", 32));
    {
        static u8 a1000[1000];

        memset(a1000, 'a', sizeof(a1000));
        rtw89_sha256(a1000, sizeof(a1000), out);
        CHECK(hex_is(out, "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3", 32));
    }

    /* HMAC-SHA256: RFC 4231 test cases 1 and 2 */
    memset(key, 0x0b, 20);
    rtw89_hmac_sha256(key, 20, (const u8 *)"Hi There", 8, out);
    CHECK(hex_is(out, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", 32));
    rtw89_hmac_sha256((const u8 *)"Jefe", 4, (const u8 *)"what do ya want for nothing?", 28, out);
    CHECK(hex_is(out, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", 32));

    /* KDF-SHA-256 (IEEE 802.11 12.7.1.6.2), against Python's hmac/hashlib */
    {
        u8 k32[32], ctx[32];

        for (i = 0; i < 32; i++) {
            k32[i] = (u8)i;
            ctx[i] = (u8)(32 + i);
        }
        rtw89_kdf_sha256(k32, 32, "SAE KCK and PMK", ctx, 32, out, 64);
        CHECK(hex_is(out, "ced4f66be71c033c714c7ee12190fb6ad0af16af84cfd5295459a1a92e629f39"
                          "bbd19d36640a76f845075c52bac62ac9559a9b3a03e0377b223aced14d17593e", 64));
        rtw89_kdf_sha256((const u8 *)"key", 3, "Pairwise key expansion",
                         (const u8 *)"context-bytes", 13, out, 48);
        CHECK(hex_is(out, "abbc62d4db671fd70ed3820b247151aa8a14b4827eca916e"
                          "95e3e582e7ded9548990aa8b2bddc76609817ff72b2549fd", 48));
    }

    /* AES-CMAC: RFC 4493 examples 1-4 */
    {
        static const u8 k[16] = { 0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
                                  0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c };
        static const u8 m[64] = {
            0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
            0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c, 0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51,
            0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4, 0x11, 0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef,
            0xf6, 0x9f, 0x24, 0x45, 0xdf, 0x4f, 0x9b, 0x17, 0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10,
        };

        rtw89_aes_cmac(k, m, 0, out);
        CHECK(hex_is(out, "bb1d6929e95937287fa37d129b756746", 16));
        rtw89_aes_cmac(k, m, 16, out);
        CHECK(hex_is(out, "070a16b46b4d4144f79bdd9dd04a287c", 16));
        rtw89_aes_cmac(k, m, 40, out);
        CHECK(hex_is(out, "dfa66747de9ae63030ca32611497c827", 16));
        rtw89_aes_cmac(k, m, 64, out);
        CHECK(hex_is(out, "51f0bebf7e3b9d92fc49741779363cfe", 16));
        {
            /* the same message in uneven pieces */
            const u8 *pieces[3] = { m, m + 7, m + 33 };
            size_t lens[3] = { 7, 26, 31 };

            rtw89_aes_cmac_vector(k, 3, pieces, lens, out);
            CHECK(hex_is(out, "51f0bebf7e3b9d92fc49741779363cfe", 16));
        }
    }
}

/* ---- cfg80211 helpers ---- */

static void test_cfg80211_helpers(void)
{
    static const u8 ies[] = {
        WLAN_EID_SSID, 3, 'a', 'b', 'c',
        WLAN_EID_DS_PARAMS, 1, 11,
        WLAN_EID_VENDOR_SPECIFIC, 4, 0x50, 0x6f, 0x9a, 0x09,
    };
    static const u8 wfa_p2p[] = { 0x50, 0x6f, 0x9a, 0x09 };
    const struct element *elem;
    struct rate_info ri;

    CHECK(ieee80211_hdrlen(cpu_to_le16(IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_BEACON)) == 24);
    CHECK(ieee80211_hdrlen(cpu_to_le16(IEEE80211_FTYPE_DATA | IEEE80211_STYPE_QOS_DATA)) == 26);
    CHECK(ieee80211_hdrlen(cpu_to_le16(IEEE80211_FTYPE_DATA | IEEE80211_FCTL_TODS |
                                       IEEE80211_FCTL_FROMDS)) == 30);
    CHECK(ieee80211_hdrlen(cpu_to_le16(IEEE80211_FTYPE_CTL | IEEE80211_STYPE_ACK)) == 10);
    CHECK(ieee80211_hdrlen(cpu_to_le16(IEEE80211_FTYPE_CTL | IEEE80211_STYPE_PSPOLL)) == 16);

    CHECK(ieee80211_channel_to_freq_khz(1, NL80211_BAND_2GHZ) == 2412000);
    CHECK(ieee80211_channel_to_freq_khz(14, NL80211_BAND_2GHZ) == 2484000);
    CHECK(ieee80211_channel_to_freq_khz(36, NL80211_BAND_5GHZ) == 5180000);
    CHECK(ieee80211_channel_to_freq_khz(1, NL80211_BAND_6GHZ) == 5955000);
    CHECK(ieee80211_freq_khz_to_channel(2437000) == 6);
    CHECK(ieee80211_freq_khz_to_channel(5745000) == 149);
    CHECK(ieee80211_freq_khz_to_channel(5955000) == 1);

    CHECK(cfg80211_get_ies_channel_number(ies, sizeof(ies), NL80211_BAND_2GHZ) == 11);
    elem = cfg80211_find_elem_match(WLAN_EID_VENDOR_SPECIFIC, ies, sizeof(ies),
                                    wfa_p2p, sizeof(wfa_p2p), 0);
    CHECK(elem && elem->datalen == 4);
    CHECK(!cfg80211_find_elem_match(WLAN_EID_VENDOR_SPECIFIC, ies, sizeof(ies),
                                    wfa_p2p, sizeof(wfa_p2p), 1));

    /* Units are 100 kbit/s. */
    memset(&ri, 0, sizeof(ri));
    ri.legacy = 540;
    CHECK(cfg80211_calculate_bitrate(&ri) == 540);

    memset(&ri, 0, sizeof(ri));
    ri.flags = RATE_INFO_FLAGS_MCS;
    ri.mcs = 7;
    ri.bw = RATE_INFO_BW_20;
    CHECK(cfg80211_calculate_bitrate(&ri) == 650);          /* HT 65 Mbit/s */

    memset(&ri, 0, sizeof(ri));
    ri.flags = RATE_INFO_FLAGS_VHT_MCS | RATE_INFO_FLAGS_SHORT_GI;
    ri.mcs = 9;
    ri.nss = 2;
    ri.bw = RATE_INFO_BW_80;
    CHECK(cfg80211_calculate_bitrate(&ri) == 8667);         /* VHT 866.7 Mbit/s */

    memset(&ri, 0, sizeof(ri));
    ri.flags = RATE_INFO_FLAGS_HE_MCS;
    ri.mcs = 11;
    ri.nss = 2;
    ri.bw = RATE_INFO_BW_80;
    ri.he_gi = NL80211_RATE_INFO_HE_GI_0_8;
    CHECK(cfg80211_calculate_bitrate(&ri) / 10 == 1200);    /* HE 1201 Mbit/s */
}

/* ---- wiphy work, hw, TXQs ---- */

static struct {
    int wake_tx_queue;
    int config;
    int order[4];
    int n_order;
    bool mutex_held_in_work;
    char alpha2[3];
    int reg_calls;
    int stas_seen;
    int vifs_seen;
} st;

static void st_tx(struct ieee80211_hw *hw, struct ieee80211_tx_control *control,
                  struct sk_buff *skb)
{
    ieee80211_free_txskb(hw, skb);
}
static int st_start(struct ieee80211_hw *hw) { return 0; }
static void st_stop(struct ieee80211_hw *hw, bool suspend) { }
static int st_add_interface(struct ieee80211_hw *hw, struct ieee80211_vif *vif) { return 0; }
static void st_remove_interface(struct ieee80211_hw *hw, struct ieee80211_vif *vif) { }
static int st_config(struct ieee80211_hw *hw, int radio_idx, u32 changed)
{
    st.config++;
    return 0;
}
static void st_configure_filter(struct ieee80211_hw *hw, unsigned int changed_flags,
                                unsigned int *total_flags, u64 multicast) { }
static void st_wake_tx_queue(struct ieee80211_hw *hw, struct ieee80211_txq *txq)
{
    st.wake_tx_queue++;
    ieee80211_schedule_txq(hw, txq);
}

static const struct ieee80211_ops st_ops = {
    .tx = st_tx,
    .start = st_start,
    .stop = st_stop,
    .add_interface = st_add_interface,
    .remove_interface = st_remove_interface,
    .config = st_config,
    .configure_filter = st_configure_filter,
    .wake_tx_queue = st_wake_tx_queue,
};

struct st_work {
    struct wiphy_work work;
    int id;
};

static void st_work_fn(struct wiphy *wiphy, struct wiphy_work *work)
{
    struct st_work *w = container_of(work, struct st_work, work);

    /* The runner must hold the wiphy mutex: a trylock from here fails. */
    if (!mutex_trylock(&wiphy->mtx))
        st.mutex_held_in_work = true;
    else
        mutex_unlock(&wiphy->mtx);
    if (st.n_order < ARRAY_SIZE(st.order))
        st.order[st.n_order++] = w->id;
}

static void st_reg_notifier(struct wiphy *wiphy, struct regulatory_request *request)
{
    memcpy(st.alpha2, request->alpha2, sizeof(st.alpha2));
    st.reg_calls++;
}

static void st_sta_iter(void *data, struct ieee80211_sta *sta) { st.stas_seen++; }
static void st_vif_iter(void *data, u8 *mac, struct ieee80211_vif *vif) { st.vifs_seen++; }

static void wait_for(int *counter, int value)
{
    int i;

    for (i = 0; i < 200 && *counter < value; i++)
        msleep(5);
}

static struct ieee80211_channel st_channels[] = {
    { .band = NL80211_BAND_2GHZ, .center_freq = 2412, .hw_value = 1 },
    { .band = NL80211_BAND_2GHZ, .center_freq = 2437, .hw_value = 6 },
};
static struct ieee80211_supported_band st_band = {
    .band = NL80211_BAND_2GHZ,
    .channels = st_channels,
    .n_channels = ARRAY_SIZE(st_channels),
};

static void test_stack(void)
{
    static const u8 own[ETH_ALEN] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
    static const u8 ap[ETH_ALEN]  = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
    struct wiphy_delayed_work dwork;
    struct ieee80211_chanctx_conf ctx;
    struct ieee80211_hdr_3addr *hdr;
    struct ieee80211_pspoll *pspoll;
    struct st_work w[3], dw_target;
    struct ieee80211_txq *txq, *t;
    struct ieee80211_sta *sta;
    struct ieee80211_vif *vif;
    struct ieee80211_hw *hw;
    unsigned long frames, bytes;
    struct sk_buff *skb, *out;
    struct wiphy *wiphy;
    int i;

    memset(&st, 0, sizeof(st));

    hw = ieee80211_alloc_hw(128, &st_ops);
    CHECK(hw != NULL);
    if (!hw)
        return;
    wiphy = hw->wiphy;
    CHECK(wiphy_to_ieee80211_hw(wiphy) == hw);
    /* cfg80211's defaults: thresholds off, standard retry limits */
    CHECK(wiphy->rts_threshold == (u32)-1 && wiphy->frag_threshold == (u32)-1);
    CHECK(wiphy->retry_short == 7 && wiphy->retry_long == 4);
    CHECK(((uintptr_t)hw->priv & (NETDEV_ALIGN - 1)) == 0);
    memset(hw->priv, 0xa5, 128);        /* the driver area is really ours */

    hw->extra_tx_headroom = 16;
    hw->txq_data_size = 24;
    hw->vif_data_size = 40;
    hw->sta_data_size = 56;
    wiphy->bands[NL80211_BAND_2GHZ] = &st_band;
    wiphy->reg_notifier = st_reg_notifier;
    CHECK(ieee80211_register_hw(hw) == 0);
    CHECK(hw->conf.chandef.chan == &st_channels[0]);
    /* registration reports the world domain before it returns */
    CHECK(st.reg_calls == 1 && !strcmp(st.alpha2, "00"));

    /* wiphy works run in FIFO order with the mutex held; cancel removes one */
    for (i = 0; i < 3; i++) {
        wiphy_work_init(&w[i].work, st_work_fn);
        w[i].id = i + 1;
    }
    wiphy_lock(wiphy);                  /* hold the runner back while queueing */
    wiphy_work_queue(wiphy, &w[0].work);
    wiphy_work_queue(wiphy, &w[1].work);
    wiphy_work_queue(wiphy, &w[2].work);
    wiphy_work_queue(wiphy, &w[0].work);    /* already queued: no duplicate */
    wiphy_work_cancel(wiphy, &w[1].work);
    wiphy_unlock(wiphy);
    wait_for(&st.n_order, 2);
    msleep(20);
    CHECK(st.n_order == 2 && st.order[0] == 1 && st.order[1] == 3);
    CHECK(st.mutex_held_in_work);

    /* flush runs a pending work inline, in the caller's context */
    st.n_order = 0;
    wiphy_lock(wiphy);
    wiphy_work_queue(wiphy, &w[1].work);
    wiphy_work_flush(wiphy, &w[1].work);
    CHECK(st.n_order == 1 && st.order[0] == 2);
    wiphy_unlock(wiphy);

    /* delayed work fires once after its delay; cancel stops a pending one */
    st.n_order = 0;
    dw_target.id = 42;
    wiphy_delayed_work_init(&dwork, st_work_fn);
    wiphy_delayed_work_queue(wiphy, &dwork, msecs_to_jiffies(30));
    CHECK(wiphy_delayed_work_pending(wiphy, &dwork));
    CHECK(st.n_order == 0);
    wait_for(&st.n_order, 1);
    CHECK(st.n_order == 1);
    wiphy_delayed_work_queue(wiphy, &dwork, msecs_to_jiffies(50));
    wiphy_lock(wiphy);
    wiphy_delayed_work_cancel(wiphy, &dwork);
    wiphy_unlock(wiphy);
    msleep(80);
    CHECK(st.n_order == 1);
    (void)dw_target;

    /* a driver regulatory hint comes back through reg_notifier */
    CHECK(regulatory_hint(wiphy, "DE") == 0);
    wait_for(&st.reg_calls, 2);
    CHECK(st.reg_calls == 2 && !strcmp(st.alpha2, "DE"));

    /* interfaces and stations */
    vif = rtw89_m80211_vif_alloc(hw, NL80211_IFTYPE_STATION, own);
    CHECK(vif && vif->txq && vif->link_conf[0] == &vif->bss_conf);
    CHECK(vif && vif->bss_conf.bssid && is_zero_ether_addr(vif->bss_conf.bssid));
    CHECK(vif && ether_addr_equal(vif->bss_conf.addr, own));
    if (!vif)
        goto out_hw;
    CHECK(!ieee80211_vif_is_mld(vif));
    memset(vif->drv_priv, 0x5a, hw->vif_data_size);
    ieee80211_iterate_active_interfaces_atomic(hw, 0, st_vif_iter, NULL);
    CHECK(st.vifs_seen == 0);           /* not in the driver yet */
    rtw89_m80211_vif_set_in_driver(vif, true);
    ieee80211_iterate_active_interfaces_atomic(hw, 0, st_vif_iter, NULL);
    CHECK(st.vifs_seen == 1);

    memcpy(vif->cfg.ap_addr, ap, ETH_ALEN);
    vif->cfg.aid = 5;
    sta = rtw89_m80211_sta_alloc(vif, ap);
    CHECK(sta && sta->txq[0] && sta->txq[7] && !sta->txq[IEEE80211_NUM_TIDS]);
    if (!sta)
        goto out_vif;
    CHECK(sta->txq[0]->ac == IEEE80211_AC_BE && sta->txq[1]->ac == IEEE80211_AC_BK);
    CHECK(sta->txq[5]->ac == IEEE80211_AC_VI && sta->txq[6]->ac == IEEE80211_AC_VO);
    memset(sta->drv_priv, 0x5a, hw->sta_data_size);
    memset(sta->txq[0]->drv_priv, 0x5a, hw->txq_data_size);
    CHECK(!ieee80211_find_sta(vif, ap));        /* not uploaded yet */
    rtw89_m80211_sta_set_uploaded(sta, true);
    CHECK(ieee80211_find_sta(vif, ap) == sta);
    CHECK(!ieee80211_find_sta(vif, own));
    ieee80211_iterate_stations_atomic(hw, st_sta_iter, NULL);
    CHECK(st.stas_seen == 1);
    CHECK(__ieee80211_iterate_stations(hw, NULL) == sta);
    CHECK(__ieee80211_iterate_stations(hw, sta) == NULL);

    /* TXQ: enqueue wakes the driver, one TXQ per scheduling round */
    txq = sta->txq[0];
    skb = dev_alloc_skb(200);
    skb_put_zero(skb, 100);
    rtw89_m80211_tx(hw, txq, skb);
    CHECK(st.wake_tx_queue == 1);
    ieee80211_txq_get_depth(txq, &frames, &bytes);
    CHECK(frames == 1 && bytes == 100);

    ieee80211_txq_schedule_start(hw, txq->ac);
    t = ieee80211_next_txq(hw, txq->ac);
    CHECK(t == txq);
    out = t ? ieee80211_tx_dequeue(hw, t) : NULL;
    CHECK(out == skb);
    CHECK(ieee80211_tx_dequeue(hw, txq) == NULL);
    ieee80211_return_txq(hw, txq, true);        /* forced back on the list... */
    CHECK(ieee80211_next_txq(hw, txq->ac) == NULL);     /* ...but not this round */
    ieee80211_txq_schedule_end(hw, txq->ac);
    ieee80211_txq_schedule_start(hw, txq->ac);
    CHECK(ieee80211_next_txq(hw, txq->ac) == txq);
    ieee80211_txq_schedule_end(hw, txq->ac);
    ieee80211_txq_get_depth(txq, &frames, &bytes);
    CHECK(frames == 0 && bytes == 0);
    if (out)
        ieee80211_free_txskb(hw, out);

    /* stopped queues hold frames back; waking hands them to the driver */
    ieee80211_stop_queues(hw);
    skb = dev_alloc_skb(200);
    skb_put_zero(skb, 60);
    rtw89_m80211_tx(hw, txq, skb);
    CHECK(st.wake_tx_queue == 1);
    CHECK(ieee80211_tx_dequeue(hw, txq) == NULL);
    ieee80211_wake_queues(hw);
    wait_for(&st.wake_tx_queue, 2);
    CHECK(st.wake_tx_queue == 2);
    /* that frame stays queued: freeing the station must release it */

    /* frame templates */
    skb = ieee80211_probereq_get(hw, own, (const u8 *)"net", 3, 20);
    CHECK(skb && skb->len == 24 + 5 && skb_headroom(skb) >= 16 && skb_tailroom(skb) >= 20);
    if (skb) {
        /* what rtw89_append_probe_req_ie() does: append IEs to a copy */
        struct sk_buff *copy = skb_copy(skb, GFP_KERNEL);

        CHECK(copy && skb_tailroom(copy) >= 20);
        if (copy) {
            skb_put_zero(copy, 20);
            kfree_skb(copy);
        }
    }
    if (skb) {
        hdr = (struct ieee80211_hdr_3addr *)skb->data;
        CHECK(ieee80211_is_probe_req(hdr->frame_control));
        CHECK(is_broadcast_ether_addr(hdr->addr1) && ether_addr_equal(hdr->addr2, own));
        CHECK(skb->data[24] == WLAN_EID_SSID && skb->data[25] == 3 && skb->data[26] == 'n');
        kfree_skb(skb);
    }
    skb = ieee80211_nullfunc_get(hw, vif, -1, false);
    CHECK(skb && skb->len == 24);
    if (skb) {
        hdr = (struct ieee80211_hdr_3addr *)skb->data;
        CHECK(ieee80211_is_nullfunc(hdr->frame_control) && ieee80211_has_tods(hdr->frame_control));
        CHECK(ether_addr_equal(hdr->addr1, ap) && ether_addr_equal(hdr->addr2, own));
        kfree_skb(skb);
    }
    sta->wme = true;
    skb = ieee80211_nullfunc_get(hw, vif, -1, true);
    CHECK(skb && skb->len == 26);
    if (skb) {
        hdr = (struct ieee80211_hdr_3addr *)skb->data;
        CHECK(ieee80211_is_qos_nullfunc(hdr->frame_control));
        kfree_skb(skb);
    }
    skb = ieee80211_pspoll_get(hw, vif);
    CHECK(skb && skb->len == 16);
    if (skb) {
        pspoll = (struct ieee80211_pspoll *)skb->data;
        CHECK(ieee80211_is_pspoll(pspoll->frame_control));
        CHECK(le16_to_cpu(pspoll->aid) == (0xc000 | 5));
        CHECK(ether_addr_equal(pspoll->bssid, ap) && ether_addr_equal(pspoll->ta, own));
        kfree_skb(skb);
    }

    /* emulated chanctx: a new channel reaches ->config() once */
    memset(&ctx, 0, sizeof(ctx));
    cfg80211_chandef_create(&ctx.def, &st_channels[1], NL80211_CHAN_HT20);
    CHECK(ctx.def.width == NL80211_CHAN_WIDTH_20 && ctx.def.center_freq1 == 2437);
    CHECK(ieee80211_emulate_add_chanctx(hw, &ctx) == 0);
    CHECK(st.config == 1 && hw->conf.chandef.chan == &st_channels[1]);
    ieee80211_emulate_change_chanctx(hw, &ctx, 0);
    CHECK(st.config == 1);
    ieee80211_emulate_remove_chanctx(hw, &ctx);
    CHECK(st.config == 2 && hw->conf.chandef.chan == &st_channels[0]);

    rtw89_m80211_sta_free(sta);
out_vif:
    rtw89_m80211_vif_free(vif);
out_hw:
    ieee80211_unregister_hw(hw);
    ieee80211_free_hw(hw);
}

int rtw89_selftest(void);
int rtw89_selftest(void)
{
    int ret;

    failures = 0;
    ret = rtw88_compat_init();
    if (ret)
        return ret;

    test_helpers();
    test_crypto();
    test_cfg80211_helpers();
    test_stack();

    rtw88_compat_exit();
    return failures;
}
