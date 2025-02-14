#include <linux/module.h>

#include <linux/skbuff.h>
#include <net/cfg80211.h>

#include <linux/mutex.h>
#include <linux/workqueue.h>

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("virtual cfg80211 driver");

#define WIPHY_NAME "owl" /* Our WireLess */
#define NDEV_NAME WIPHY_NAME "%d"

#define SSID_DUMMY "MyHomeWiFi"
#define SSID_DUMMY_SIZE (sizeof(SSID_DUMMY) - 1)

struct owl_context {
    struct wiphy *wiphy;
    struct net_device *ndev;

    struct mutex lock;
    struct work_struct ws_connect, ws_disconnect;
    char connecting_ssid[sizeof(SSID_DUMMY)];
    u16 disconnect_reason_code;
    struct work_struct ws_scan;
    struct cfg80211_scan_request *scan_request;
};

struct owl_wiphy_priv_context {
    struct owl_context *owl;
};

struct owl_ndev_priv_context {
    struct owl_context *owl;
    struct wireless_dev wdev;
};

/* helper function to retrieve main context from "priv" data of the wiphy */
static inline struct owl_wiphy_priv_context *wiphy_get_owl_context(
    struct wiphy *wiphy)
{
    return (struct owl_wiphy_priv_context *) wiphy_priv(wiphy);
}

/* helper function to retrieve main context from "priv" data of network dev */
static inline struct owl_ndev_priv_context *ndev_get_owl_context(
    struct net_device *ndev)
{
    return (struct owl_ndev_priv_context *) netdev_priv(ndev);
}

/* Helper function that will prepare structure with "dummy" BSS information and
 * "inform" the kernel about "new" BSS
 */
static void inform_dummy_bss(struct owl_context *owl)
{
    struct cfg80211_bss *bss = NULL;
    struct cfg80211_inform_bss data = {
        /* the only channel */
        .chan = &owl->wiphy->bands[NL80211_BAND_2GHZ]->channels[0],

        /* signal "type" not specified in this module, so it is unused.
         * It can be some kind of percentage from 0 to 100 or mBm value.
         * signal "type" may be specified before wiphy registration by setting
         * wiphy->signal_type
         */
        .scan_width = NL80211_BSS_CHAN_WIDTH_20,
        .signal = 1337,
    };
    char bssid[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};

    /* array of tags that retrieved from beacon frame or probe responce */
    char ie[SSID_DUMMY_SIZE + 2] = {WLAN_EID_SSID, SSID_DUMMY_SIZE};
    memcpy(ie + 2, SSID_DUMMY, SSID_DUMMY_SIZE);

    /* It is posible to use cfg80211_inform_bss() instead. */
    bss = cfg80211_inform_bss_data(
        owl->wiphy, &data, CFG80211_BSS_FTYPE_UNKNOWN, bssid, 0,
        WLAN_CAPABILITY_ESS, 100, ie, sizeof(ie), GFP_KERNEL);

    /* cfg80211_inform_bss_data() returns cfg80211_bss structure referefence
     * counter of which should be decremented if it is unused.
     */
    cfg80211_put_bss(owl->wiphy, bss);
}

/* "Scan" routine. It informs the kernel about "dummy" BSS and "finish" scan.
 * When scan is done, it should call cfg80211_scan_done() to inform the kernel
 * that scan is finished. This routine called through workqueue, when the
 * kernel asks to scan through cfg80211_ops.
 */
static void owl_scan_routine(struct work_struct *w)
{
    struct owl_context *owl = container_of(w, struct owl_context, ws_scan);
    struct cfg80211_scan_info info = {
        /* if scan was aborted by user (calling cfg80211_ops->abort_scan) or by
         * any driver/hardware issue - field should be set to "true"
         */
        .aborted = false,
    };

    /* Pretend to do something.
     * FIXME: for unknown reason, it can not call cfg80211_scan_done right
     * away after cfg80211_ops->scan(), otherwise netlink client would not
     * get message with "scan done". Is it because "scan_routine" and
     * cfg80211_ops->scan() may run in concurrent and cfg80211_scan_done()
     * called before cfg80211_ops->scan() returns?
     */
    msleep(100);

    /* inform with dummy BSS */
    inform_dummy_bss(owl);

    if (mutex_lock_interruptible(&owl->lock))
        return;

    /* finish scan */
    cfg80211_scan_done(owl->scan_request, &info);

    owl->scan_request = NULL;

    mutex_unlock(&owl->lock);
}

/* It checks SSID of the ESS to connect and informs the kernel that connection
 * is finished. It should call cfg80211_connect_bss() when connect is finished
 * or cfg80211_connect_timeout() when connect is failed. This module can connect
 * only to ESS with SSID equal to SSID_DUMMY value.
 * This routine is called through workqueue, when the kernel asks to connect
 * through cfg80211_ops.
 */
static void owl_connect_routine(struct work_struct *w)
{
    struct owl_context *owl = container_of(w, struct owl_context, ws_connect);

    if (mutex_lock_interruptible(&owl->lock))
        return;

    if (memcmp(owl->connecting_ssid, SSID_DUMMY, sizeof(SSID_DUMMY)) != 0) {
        cfg80211_connect_timeout(owl->ndev, NULL, NULL, 0, GFP_KERNEL,
                                 NL80211_TIMEOUT_SCAN);
    } else {
        /* We can connect to known ESS. If else, technically kernel will only
         * warn. So, let's send dummy bss to the kernel before complete.
         */
        inform_dummy_bss(owl);

        /* It is possible to use cfg80211_connect_result() or
         * cfg80211_connect_done()
         */
        cfg80211_connect_bss(owl->ndev, NULL, NULL, NULL, 0, NULL, 0,
                             WLAN_STATUS_SUCCESS, GFP_KERNEL,
                             NL80211_TIMEOUT_UNSPECIFIED);
    }
    owl->connecting_ssid[0] = 0;

    mutex_unlock(&owl->lock);
}

/* Invoke cfg80211_disconnected() that informs the kernel that disconnect is
 * complete. Overall disconnect may call cfg80211_connect_timeout() if
 * disconnect interrupting connection routine, but for this module let's keep
 * it simple as possible. This routine is called through workqueue, when the
 * kernel asks to disconnect through cfg80211_ops.
 */
static void owl_disconnect_routine(struct work_struct *w)
{
    struct owl_context *owl =
        container_of(w, struct owl_context, ws_disconnect);

    if (mutex_lock_interruptible(&owl->lock))
        return;

    cfg80211_disconnected(owl->ndev, owl->disconnect_reason_code, NULL, 0, true,
                          GFP_KERNEL);

    owl->disconnect_reason_code = 0;

    mutex_unlock(&owl->lock);
}

/* callback called by the kernel when user decided to scan.
 * This callback should initiate scan routine(through work_struct) and exit with
 * 0 if everything is ok.
 * Scan routine should be finished with cfg80211_scan_done() call.
 */
static int owl_scan(struct wiphy *wiphy, struct cfg80211_scan_request *request)
{
    struct owl_context *owl = wiphy_get_owl_context(wiphy)->owl;

    if (mutex_lock_interruptible(&owl->lock))
        return -ERESTARTSYS;

    if (owl->scan_request) {
        mutex_unlock(&owl->lock);
        return -EBUSY;
    }
    owl->scan_request = request;

    mutex_unlock(&owl->lock);

    if (!schedule_work(&owl->ws_scan))
        return -EBUSY;
    return 0;
}

/* callback called by the kernel when there is need to "connect" to some
 * network. It initializes connection routine through work_struct and exits
 * with 0 if everything is ok. connect routine should be finished with
 * cfg80211_connect_bss()/cfg80211_connect_result()/cfg80211_connect_done() or
 * cfg80211_connect_timeout().
 */
static int owl_connect(struct wiphy *wiphy,
                       struct net_device *dev,
                       struct cfg80211_connect_params *sme)
{
    struct owl_context *owl = wiphy_get_owl_context(wiphy)->owl;
    size_t ssid_len = sme->ssid_len > 15 ? 15 : sme->ssid_len;

    if (mutex_lock_interruptible(&owl->lock))
        return -ERESTARTSYS;

    memcpy(owl->connecting_ssid, sme->ssid, ssid_len);
    owl->connecting_ssid[ssid_len] = 0;

    mutex_unlock(&owl->lock);

    if (!schedule_work(&owl->ws_connect))
        return -EBUSY;
    return 0;
}

/* callback called by the kernel when there is need to "diconnect" from
 * currently connected network. It initializes disconnect routine through
 * work_struct and exits with 0 if everything ok. disconnect routine should
 * call cfg80211_disconnected() to inform the kernel that disconnection is
 * complete.
 */
static int owl_disconnect(struct wiphy *wiphy,
                          struct net_device *dev,
                          u16 reason_code)
{
    struct owl_context *owl = wiphy_get_owl_context(wiphy)->owl;

    if (mutex_lock_interruptible(&owl->lock))
        return -ERESTARTSYS;

    owl->disconnect_reason_code = reason_code;

    mutex_unlock(&owl->lock);

    if (!schedule_work(&owl->ws_disconnect))
        return -EBUSY;

    return 0;
}

/* Structure of functions for FullMAC 80211 drivers.
 * Functions implemented along with fields/flags in wiphy structure would
 * represent drivers features. This module can only perform "scan" and
 * "connect". Some functions cant be implemented alone, for example: with
 * "connect" there is should be function "disconnect".
 */
static struct cfg80211_ops owl_cfg_ops = {
    .scan = owl_scan,
    .connect = owl_connect,
    .disconnect = owl_disconnect,
};

/* Network packet transmit.
 * Callback called by the kernel when packet of data should be sent.
 * In this example it does nothing.
 */
static netdev_tx_t owl_ndo_start_xmit(struct sk_buff *skb,
                                      struct net_device *dev)
{
    /* Don't forget to cleanup skb, as its ownership moved to xmit callback. */
    kfree_skb(skb);
    return NETDEV_TX_OK;
}

/* Structure of functions for network devices.
 * It should have at least ndo_start_xmit functions called for packet to be
 * sent.
 */
static struct net_device_ops owl_ndev_ops = {
    .ndo_start_xmit = owl_ndo_start_xmit,
};

/* Array of "supported" channels in 2GHz band. It is required for wiphy.
 * For demo - the only channel 6.
 */
static struct ieee80211_channel owl_supported_channels_2ghz[] = {
    {
        .band = NL80211_BAND_2GHZ,
        .hw_value = 6,
        .center_freq = 2437,
    },
};

/* Array of supported rates, required to support at least those next rates
 * for 2GHz band.
 */
static struct ieee80211_rate owl_supported_rates_2ghz[] = {
    {
        .bitrate = 10,
        .hw_value = 0x1,
    },
    {
        .bitrate = 20,
        .hw_value = 0x2,
    },
    {
        .bitrate = 55,
        .hw_value = 0x4,
    },
    {
        .bitrate = 110,
        .hw_value = 0x8,
    },
};

/* Describes supported band of 2GHz. */
static struct ieee80211_supported_band nf_band_2ghz = {
    /* FIXME: add other band capabilities if nedded, such as 40 width */
    .ht_cap.cap = IEEE80211_HT_CAP_SGI_20,
    .ht_cap.ht_supported = false,

    .channels = owl_supported_channels_2ghz,
    .n_channels = ARRAY_SIZE(owl_supported_channels_2ghz),

    .bitrates = owl_supported_rates_2ghz,
    .n_bitrates = ARRAY_SIZE(owl_supported_rates_2ghz),
};

/* Creates wiphy context and net_device with wireless_dev.
 * wiphy/net_device/wireless_dev is basic interfaces for the kernel to interact
 * with driver as wireless one. It returns driver's main "owl" context.
 */
static struct owl_context *owl_create_context(void)
{
    struct owl_context *ret = NULL;
    struct owl_wiphy_priv_context *wiphy_data = NULL;
    struct owl_ndev_priv_context *ndev_data = NULL;

    /* allocate for owl context */
    ret = kmalloc(sizeof(*ret), GFP_KERNEL);
    if (!ret)
        goto l_error;

    /* allocate wiphy context. It is possible just to use wiphy_new().
     * wiphy should represent physical FullMAC wireless device. One wiphy can
     * have serveral network interfaces - for that, we need to implement
     * add_virtual_intf() from cfg80211_ops.
     */
    ret->wiphy = wiphy_new_nm(
        &owl_cfg_ops, sizeof(struct owl_wiphy_priv_context), WIPHY_NAME);
    if (!ret->wiphy)
        goto l_error_wiphy;

    /* save owl context in wiphy private data. */
    wiphy_data = wiphy_get_owl_context(ret->wiphy);
    wiphy_data->owl = ret;

    /* FIXME: set device object as wiphy "parent" */
    /* set_wiphy_dev(ret->wiphy, dev); */

    /* wiphy should determinate its type.
     * add other required types like  "BIT(NL80211_IFTYPE_STATION) |
     * BIT(NL80211_IFTYPE_AP)" etc.
     */
    ret->wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);

    /* wiphy should have at least 1 band.
     * Also fill NL80211_BAND_5GHZ if required. In this module, only 1 band
     * with 1 "channel"
     */
    ret->wiphy->bands[NL80211_BAND_2GHZ] = &nf_band_2ghz;

    /* scan - if the device supports "scan", we need to define max_scan_ssids
     * at least.
     */
    ret->wiphy->max_scan_ssids = 69;

    /* register wiphy, if everything ok - there should be another wireless
     * device in system. use command: $ iw list
     * Wiphy owl
     */
    if (wiphy_register(ret->wiphy) < 0)
        goto l_error_wiphy_register;

    /* allocate network device context. */
    ret->ndev =
        alloc_netdev(sizeof(*ndev_data), NDEV_NAME, NET_NAME_ENUM, ether_setup);
    if (!ret->ndev)
        goto l_error_alloc_ndev;

    /* fill private data of network context. */
    ndev_data = ndev_get_owl_context(ret->ndev);
    ndev_data->owl = ret;

    /* fill wireless_dev context.
     * wireless_dev with net_device can be represented as inherited class of
     * single net_device.
     */
    ndev_data->wdev.wiphy = ret->wiphy;
    ndev_data->wdev.netdev = ret->ndev;
    ndev_data->wdev.iftype = NL80211_IFTYPE_STATION;
    ret->ndev->ieee80211_ptr = &ndev_data->wdev;

    /* FIMXE: set device object for net_device */
    /* SET_NETDEV_DEV(ret->ndev, wiphy_dev(ret->wiphy)); */

    /* set network device hooks. should implement ndo_start_xmit() at least */
    ret->ndev->netdev_ops = &owl_ndev_ops;

    /* Add here proper net_device initialization. */

    /* register network device. If everything is ok, there should be new network
     * device: $ ip a
     * owl0: <BROADCAST,MULTICAST> mtu 1500 qdisc noop state DOWN group default
     * link/ether 00:00:00:00:00:00 brd ff:ff:ff:ff:ff:ff
     */
    if (register_netdev(ret->ndev))
        goto l_error_ndev_register;

    return ret;

l_error_ndev_register:
    free_netdev(ret->ndev);
l_error_alloc_ndev:
    wiphy_unregister(ret->wiphy);
l_error_wiphy_register:
    wiphy_free(ret->wiphy);
l_error_wiphy:
    kfree(ret);
l_error:
    return NULL;
}

static void owl_free(struct owl_context *ctx)
{
    if (!ctx)
        return;

    unregister_netdev(ctx->ndev);
    free_netdev(ctx->ndev);
    wiphy_unregister(ctx->wiphy);
    wiphy_free(ctx->wiphy);
    kfree(ctx);
}

static struct owl_context *g_ctx = NULL;

static int __init vwifi_init(void)
{
    g_ctx = owl_create_context();
    if (!g_ctx)
        return 1;

    mutex_init(&g_ctx->lock);

    INIT_WORK(&g_ctx->ws_connect, owl_connect_routine);
    g_ctx->connecting_ssid[0] = 0;
    INIT_WORK(&g_ctx->ws_disconnect, owl_disconnect_routine);
    g_ctx->disconnect_reason_code = 0;
    INIT_WORK(&g_ctx->ws_scan, owl_scan_routine);
    g_ctx->scan_request = NULL;

    return 0;
}

static void __exit vwifi_exit(void)
{
    /* make sure that no work is queued */
    cancel_work_sync(&g_ctx->ws_connect);
    cancel_work_sync(&g_ctx->ws_disconnect);
    cancel_work_sync(&g_ctx->ws_scan);

    owl_free(g_ctx);
}

module_init(vwifi_init);
module_exit(vwifi_exit);
