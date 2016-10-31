#include "net_steering.h"
#include "utils/includes.h"
#include "utils/state_machine.h"
#include "utils/common.h"
#include "utils/wpa_debug.h"
#include "utils/wpabuf.h"
#include "utils/list.h"
#include "utils/eloop.h"
#include "utils/os.h"
#include "hostapd.h"
#include "ap_config.h"
#include "sta_blacklist.h"
#include "sta_info.h"
#include "wpa_auth.h"
#include "wnm_ap.h"
#include "ap_drv_ops.h"
#include "ctrl_iface_ap.h"
#include "common/defs.h"
#include "l2_packet/l2_packet.h"

#include <sys/ioctl.h>
#include <assert.h>


#define MAX_FRAME_SIZE 1024
#define MACSTRLEN 18 /* 6 * 2 + 5 seps + NULL */

static const u16 proto = 0x8267; /* chosen at random from unassigned */
static const u8 tlv_magic = 48;
static const u8 tlv_version = 1;
static const u16 max_score = -1;

static const u32 flood_timeout_secs = 1;
static const u32 client_timeout_secs = 10;
static const u32 probe_timeout_secs = 34;

static const char* mode_off = "off";
static const char* mode_suggest = "suggest";
static const char* mode_force = "force";

/* Can't change the values of these without bumping the tlv version */
enum {
	TLV_SCORE = 0,
	TLV_CLOSE_CLIENT = 1,
	TLV_CLOSED_CLIENT = 2,
	TLV_MAP = 3,
	TLV_CLIENT_FLAGS = 4,
};

/* Pre decls */
struct net_steering_client;
struct net_steering_bss;
static void do_flood_score(struct net_steering_client *client);
static void flood_score(void *eloop_data, void *user_ctx);
static void client_timeout(void *eloop_data, void *user_ctx);
static void probe_timeout(void *eloop_data, void *user_ctx);


/*
 * Use this so we can track additional data for stas and avoid adding more members to sta_info
 * It does mean that we need to be concerned about the lifetime of sta_info objects tracked
 * by hapd
 * Also, this struct is used to track stas we hear about from other APs via score messages
 */
struct net_steering_client
{
	struct dl_list list;
	/*
	 * This will point to a sta in the hapd list pointed to by the nsb.
	 * May be NULL if client is not associated
	 */
	struct sta_info* sta;
	struct net_steering_bss* nsb;
	u16 score;

	enum {
        /*
         * AP will allow the client to associate with it.
         */
		STEERING_IDLE,
		/*
		 * AP has told another AP to blacklist the client and is waiting for it
		 * to tell us that it has blacklisted the client.
		 */
		STEERING_CONFIRMING,
		/*
		 * A remote AP has confirmed that it has blacklisted the client; AP is
		 * now waiting on an associate.
		 */
		STEERING_ASSOCIATING,
		/*
		 * The client is using this AP to communicate with other devices.
		 */
		STEERING_ASSOCIATED,
		/*
		 * The AP has blacklisted the client is waiting on a disassociate and will
		 * then send out a closed packet to remotes.
		 */
		STEERING_REJECTING,
	    /*
	     * The client is blacklisted and disassociated.
	     */
		STEERING_REJECTED,
	} STEERING_state;

	enum {
		/*
		 * The client has started to use this AP to communicate with other devices.
		 * Note that we don't attempt to explicitly model the client so (dis)associate
		 * events just appear on APs.
		 */
		STEERING_E_ASSOCIATED,
		/*
		 * The client has either gone away or associated with a different AP.
		 */
		STEERING_E_DISASSOCIATED,
		/*
		 * A remote AP sent a client score packet with a score worse than our local score.
		 */
		STEERING_E_PEER_IS_WORSE,
		/*
		 * A remote AP sent a client score packet with a score the same as (or better) than our local score.
		 */
		STEERING_E_PEER_NOT_WORSE,
		/*
		 * A remote AP sent a client score that is the maximum possible.
		 */
		STEERING_E_PEER_LOST_CLIENT,
		/*
		 * The AP has been told to blacklist/transition the client.
		 */
		STEERING_E_CLOSE_CLIENT,
		/*
		 * A remote AP has confirmed that it has blacklisted/transitioned the client.
		 */
		STEERING_E_CLOSED_CLIENT,
		/*
		 * Used to limit how long an AP waits on an event (e.g. ClosedClientPacket).
		 */
		STEERING_E_TIMEOUT,
	} STEERING_event;

	/* state machine support */
	unsigned int changed;

	/* The mac addr of the client. This is necessary since we may not have a sta_info for this client */
	u8 addr[ETH_ALEN];

	/*
	 * tracks the sender bssid for close messages
	 */
	u8 close_bssid[ETH_ALEN];

	/*
	 * tracks the remote bssid for received scores
	 */
	u8 remote_bssid[ETH_ALEN];

	/*
	 * tracks the locally adjusted association timer for the remote ap that has the client associated
	 */
	struct os_time remote_time;

	/*
	 * tracks the time of association of the client
	 */
	struct os_time association_time;

	/*
	 * channel used for Fast BSS Transition
	 */
	u8 remote_channel;
};

/* One context per bss */
struct net_steering_bss {
	/* supports a dl_list of net_steering_bss */
	struct dl_list list;
	/* contains the list of clients */
	struct dl_list clients;
	/* bss data structure */
	struct hostapd_data *hapd;
	/* frame serial number TODO can we get rid of this, else use it and manage wraparound? */
	u16 frame_sn;
	/* the steering control channel */
	struct l2_packet_data *control;

	enum {
		MODE_OFF = 0,
		MODE_SUGGEST = 1,
		MODE_FORCE = 2,
	} mode;
};

static struct dl_list nsb_list = DL_LIST_HEAD_INIT(nsb_list);

static const char* state_to_str(int state)
{
	switch(state) {
	case STEERING_IDLE: return "IDLE";
	case STEERING_CONFIRMING: return "CONFIRMING";
	case STEERING_ASSOCIATING: return "ASSOCIATING";
	case STEERING_ASSOCIATED: return "ASSOCIATED";
	case STEERING_REJECTING: return "REJECTING";
	case STEERING_REJECTED: return "REJECTED";
	default: return "UNKNOWN";
	}
}

static const char* event_to_str(int event)
{
	switch (event) {
	case STEERING_E_ASSOCIATED:	return "E_ASSOCIATED";
	case STEERING_E_DISASSOCIATED: return "E_DISASSOCIATED";
	case STEERING_E_PEER_IS_WORSE: return "E_PEER_IS_WORSE";
	case STEERING_E_PEER_NOT_WORSE: return "E_PEER_NOT_WORSE";
	case STEERING_E_PEER_LOST_CLIENT: return "E_PEER_LOST_CLIENT";
	case STEERING_E_CLOSE_CLIENT: return "E_CLOSE_CLIENT";
	case STEERING_E_CLOSED_CLIENT: return "E_CLOSED_CLIENT";
	case STEERING_E_TIMEOUT: return "E_TIMEOUT";
	default: return "UNKNOWN";
	}
}


static void client_update_remote(struct net_steering_client* client, const u8* bssid, struct os_time* local_t)
{
	os_memcpy(client->remote_bssid, bssid, ETH_ALEN);
	client->remote_time.sec = local_t->sec;
	client->remote_time.usec = local_t->usec;
}

static const u8* client_get_remote_bssid(struct net_steering_client* client)
{
	return client->remote_bssid;
}

static void client_set_close_bssid(struct net_steering_client* client, const u8* bssid)
{
	os_memcpy(client->close_bssid, bssid, ETH_ALEN);
}

static const u8* client_get_close_bssid(struct net_steering_client* client)
{
	return client->close_bssid;
}

static void client_clear_close_bssid(struct net_steering_client* client)
{
	os_memset(client->close_bssid, 0, ETH_ALEN);
}

static const u8* client_get_local_bssid(struct net_steering_client* client)
{
	return client->nsb->hapd->conf->bssid;
}

static const u8* client_get_mac(struct net_steering_client* client)
{
	/* assumption is that this is always filled in from sta or via received tlv */
	return client->addr;
}

static struct net_steering_client* client_create(struct net_steering_bss* nsb, const u8* addr)
{
	assert(nsb->hapd->conf->bssid);
	if (!nsb->hapd || !nsb->hapd->conf || !nsb->hapd->conf->bssid) {
		hostapd_logger(nsb->hapd, NULL, HOSTAPD_MODULE_NET_STEERING,
				HOSTAPD_LEVEL_WARNING, "hapd pointers! are suspect %p\n",
				nsb->hapd);
	}

	struct net_steering_client* client = (struct net_steering_client*) os_zalloc(sizeof(*client));
	if (!client)
	{
		hostapd_logger(nsb->hapd, NULL, HOSTAPD_MODULE_NET_STEERING,
				HOSTAPD_LEVEL_WARNING, "Failed to create client "MACSTR" for bssid "MACSTR"\n",
				MAC2STR(addr), MAC2STR(nsb->hapd->conf->bssid));
		return NULL;
	}

	client->nsb = nsb;
	client->remote_time.sec = 0;
	client->remote_time.usec = 0;
	client->score = max_score;
	client->STEERING_state = STEERING_IDLE;
	os_memcpy(client->addr, addr, ETH_ALEN);

	dl_list_add(&nsb->clients, &client->list);

	return client;
}

static void start_flood_timer(struct net_steering_client *client)
{
	if (eloop_register_timeout(flood_timeout_secs, 0, flood_score, client, NULL)) {
		hostapd_logger(client->nsb->hapd, NULL, HOSTAPD_MODULE_NET_STEERING,
				HOSTAPD_LEVEL_WARNING, "client "MACSTR" failed to schedule flood\n",
				MAC2STR(client_get_mac(client)));
	}
}

static void stop_flood_timer(struct net_steering_client *client)
{
	client->score = max_score;
	// It is safe if a timer is already canceled
	eloop_cancel_timeout(flood_score, client, NULL);
}

static void client_start_timer(struct net_steering_client *client)
{
	if (eloop_register_timeout(client_timeout_secs, 0, client_timeout, client, NULL)) {
		hostapd_logger(client->nsb->hapd, client_get_local_bssid(client), HOSTAPD_MODULE_NET_STEERING,
				HOSTAPD_LEVEL_WARNING, "client "MACSTR" failed to schedule timeout\n",
				MAC2STR(client_get_mac(client)));
	}
}

static void client_stop_timer(struct net_steering_client *client)
{
	// It is safe if a timer is already canceled
	eloop_cancel_timeout(client_timeout, client, NULL);
}


static void client_start_probe_timer(struct net_steering_client *client)
{
	if (eloop_register_timeout(probe_timeout_secs, 0, probe_timeout, client, NULL)) {
		hostapd_logger(client->nsb->hapd, client_get_local_bssid(client), HOSTAPD_MODULE_NET_STEERING,
				HOSTAPD_LEVEL_WARNING, "client "MACSTR" failed to schedule probe timeout\n",
				MAC2STR(client_get_mac(client)));
	}
}

static void client_stop_probe_timer(struct net_steering_client *client)
{
	// It is safe if a timer is already canceled
	eloop_cancel_timeout(probe_timeout, client, NULL);
}

static Boolean client_supports_bss_transition(struct net_steering_client *client)
{
	return (client->sta->dot11MgmtOptionBSSTransitionActivated == 1);
}

static void client_associate(struct net_steering_client* client, struct sta_info* sta)
{
	client->sta = sta;
	os_memcpy(client->addr, client->sta->addr, ETH_ALEN);

	/* now that the client is associated, cancel probe timer */
	client_stop_probe_timer(client);
}

static Boolean client_is_associated(struct net_steering_client* client)
{
	return (client->sta && client->STEERING_state == STEERING_ASSOCIATED) ? TRUE : FALSE;
}

static void client_delete(struct net_steering_client* client)
{
	stop_flood_timer(client);
	client_stop_timer(client);
	client_stop_probe_timer(client);

	dl_list_del(&client->list);
	os_memset(client, 0, sizeof(*client));
	os_free(client);
}

static u16 compute_score(int rssi)
{
	return (u16) abs(rssi);
}

static struct net_steering_client* client_find(struct net_steering_bss* nsb, const u8* sta)
{
	struct net_steering_client *client = NULL;

	dl_list_for_each(client, &nsb->clients, struct net_steering_client, list) {
		if (os_memcmp(sta, client->addr, ETH_ALEN) == 0) {
			return client;
		}
	}
	return NULL;
}

static size_t parse_header(const u8* buf, size_t len, u8* magic, u8* version, u16* packet_len, u16* sn)
{
	static u16 header_len = sizeof(*magic) + sizeof(*version) + sizeof(*sn) + sizeof(*packet_len);

	/* TODO maybe using wpabuf would make this code simpler */
	if (len < header_len) return 0;

	const u8* tmp = buf;
	os_memcpy(magic, tmp, sizeof(*magic));
	tmp += sizeof(*magic);

	os_memcpy(version, tmp, sizeof(*version));
	tmp += sizeof(*version);

	os_memcpy(packet_len, tmp, sizeof(*packet_len));
	*packet_len = ntohs(*packet_len);
	tmp += sizeof(*packet_len);

	os_memcpy(sn, tmp, sizeof(*sn));
	*sn = ntohs(*sn);
	tmp += sizeof(*sn);

	return tmp - buf;
}

static void put_tlv_header(struct wpabuf* buf, u8 tlv_type, u8 tlv_len)
{
	wpabuf_put_u8(buf, tlv_type);
	wpabuf_put_u8(buf, tlv_len);
}

static size_t parse_tlv_header(const u8* buf, size_t len, u8* tlv_type, u8* tlv_len)
{
	static const size_t header_len = sizeof(*tlv_type) + sizeof(*tlv_len);
	const u8* tmp = buf;
	if (len < header_len) return 0;

	os_memcpy(tlv_type, tmp, sizeof(*tlv_type));
	tmp += sizeof(*tlv_type);

	os_memcpy(tlv_len, tmp, sizeof(*tlv_len));
	tmp += sizeof(*tlv_len);

	return tmp - buf;
}

static void put_score(struct wpabuf* buf, const u8* sta, const u8* bssid, u16 score, u32 association_msecs)
{
	static u8 score_len = ETH_ALEN + ETH_ALEN + sizeof(score) + sizeof(association_msecs);

	put_tlv_header(buf, TLV_SCORE, score_len);
	wpabuf_put_data(buf, sta, ETH_ALEN);
	wpabuf_put_data(buf, bssid, ETH_ALEN);
	score = htons(score);
	wpabuf_put_data(buf, &score, sizeof(score));
	association_msecs = htonl(association_msecs);
	wpabuf_put_data(buf, &association_msecs, sizeof(association_msecs));
}

static void put_close_client(struct wpabuf* buf, const u8* sta, const u8* bssid, const u8* remote_bssid, u8 channel)
{
	static u8 close_len = ETH_ALEN + ETH_ALEN + ETH_ALEN + sizeof(channel);

	put_tlv_header(buf, TLV_CLOSE_CLIENT, close_len);
	wpabuf_put_data(buf, sta, ETH_ALEN);
	wpabuf_put_data(buf, bssid, ETH_ALEN);
	wpabuf_put_data(buf, remote_bssid, ETH_ALEN);
	wpabuf_put_u8(buf, channel);
}

static void put_closed_client(struct wpabuf* buf, const u8* sta, const u8* bssid)
{
	static u8 close_len = ETH_ALEN + ETH_ALEN;

	put_tlv_header(buf, TLV_CLOSED_CLIENT, close_len);
	wpabuf_put_data(buf, sta, ETH_ALEN);
	wpabuf_put_data(buf, bssid, ETH_ALEN);
}

static size_t parse_score(const u8* buf, size_t len, u8* sta, u8* bssid, u16* score, u32* association_msecs)
{
	static u8 score_len = ETH_ALEN + ETH_ALEN + sizeof(*score) + sizeof(*association_msecs);
	const u8* tmp = buf;

	if (len < score_len) return 0;

	os_memcpy(sta, tmp, ETH_ALEN);
	tmp += ETH_ALEN;
	os_memcpy(bssid, tmp, ETH_ALEN);
	tmp += ETH_ALEN;
	os_memcpy(score, tmp, sizeof(*score));
	*score = ntohs(*score);
	tmp += sizeof(*score);
	os_memcpy(association_msecs, tmp, sizeof(*association_msecs));
	*association_msecs = ntohl(*association_msecs);
	tmp += sizeof(*association_msecs);
	return tmp - buf;
}

static size_t parse_close_client(const u8* buf, size_t len, u8* sta, u8* bssid, u8* target_bssid, u8* channel)
{
	static u8 close_len = ETH_ALEN + ETH_ALEN + ETH_ALEN + sizeof(*channel);
	const u8* tmp = buf;

	if (len < close_len) return 0;

	os_memcpy(sta, tmp, ETH_ALEN);
	tmp += ETH_ALEN;
	os_memcpy(bssid, tmp, ETH_ALEN);
	tmp += ETH_ALEN;
	os_memcpy(target_bssid, tmp, ETH_ALEN);
	tmp += ETH_ALEN;
	os_memcpy(channel, tmp, sizeof(*channel));
	tmp += sizeof(*channel);

	return tmp - buf;
}

static size_t parse_closed_client(const u8* buf, size_t len, u8* sta, u8* target_bssid)
{
	static u8 closed_len = ETH_ALEN + ETH_ALEN;
	const u8* tmp = buf;

	if (len < closed_len) return 0;

	os_memcpy(sta, tmp, ETH_ALEN);
	tmp += ETH_ALEN;
	os_memcpy(target_bssid, tmp, ETH_ALEN);
	tmp += ETH_ALEN;

	return tmp - buf;
}


int probe_req_cb(void *ctx, const u8 *sa, const u8 *da, const u8 *bssid,
		   const u8 *ie, size_t ie_len, int ssi_signal)
{
	/* unused */
	(void) da;
	(void) ie;
	(void) ie_len;

	struct net_steering_bss* nsb = ctx;
	struct net_steering_client *client = NULL;

	assert(nsb->hapd);
	/* look up the client in our list */
	client = client_find(nsb, sa);

	/* if we found the client, or this probe is directed at this bss */
	if (client || os_memcmp(nsb->hapd->conf->bssid, bssid, ETH_ALEN) == 0) {
		if (!client) client = client_create(nsb, sa);
		if (!client) {
			hostapd_logger(nsb->hapd, nsb->hapd->conf->bssid, HOSTAPD_MODULE_NET_STEERING,
				HOSTAPD_LEVEL_INFO,
				"Failed to create client for "MACSTR" on received probe\n",
				MAC2STR(sa));
			return 0;
		}
		u16 score = compute_score(ssi_signal);

		if (score != client->score) {
			hostapd_logger(nsb->hapd, nsb->hapd->conf->bssid, HOSTAPD_MODULE_NET_STEERING,
			HOSTAPD_LEVEL_DEBUG, "Probe request from "MACSTR" RSSI=%d\n",
			MAC2STR(client_get_mac(client)), ssi_signal);
			/* if client is associated, publish score changes immediately */
			client->score = score;

			/* don't flood until the score is updated */
			if (client_is_associated(client)) do_flood_score(client);
		}

		if (!client_is_associated(client)) {
			/* set our timer for the next probe */
			client_stop_probe_timer(client);
			client_start_probe_timer(client);
		}
	}
	return 0;
}

static void header_put(struct wpabuf* buf, u16 sn)
{
	u16 len = 0;
	wpabuf_put_u8(buf, tlv_magic);
	wpabuf_put_u8(buf, tlv_version);
	wpabuf_put_data(buf, &len, sizeof(len));
	sn = htons(sn);
	wpabuf_put_data(buf, &sn, sizeof(sn));
}

// write the total length into the header
static void header_finalize(struct wpabuf* buf)
{
	u16* p = (u16*)(wpabuf_mhead_u8(buf) + (sizeof(tlv_magic) + sizeof(tlv_version)));
	*p = htons(wpabuf_len(buf));
}

static void flood_message(struct net_steering_bss* nsb, const struct wpabuf* buf)
{
	struct ft_remote_r0kh *r0kh = nsb->hapd->conf->r0kh_list;
	int ret;

	assert(nsb->hapd->own_addr);
	assert(buf);

	// TODO use broadcast, but figure out how to handle multiple ess
	while (r0kh) {
		u8* dst = r0kh->addr;
		// don't send to ourself
		if (os_memcmp(dst, nsb->hapd->own_addr, ETH_ALEN) != 0) {

			ret = l2_packet_send(nsb->control, dst, proto, wpabuf_head(buf), wpabuf_len(buf));
			if (ret < 0) {
				hostapd_logger(nsb->hapd, nsb->hapd->conf->bssid, HOSTAPD_MODULE_NET_STEERING,
				HOSTAPD_LEVEL_WARNING, "Failed send to "MACSTR" : error %d\n",
				MAC2STR(dst), ret);
			}
		} else {
			/*
			hostapd_logger(nsb->hapd, nsb->hapd->own_addr, HOSTAPD_MODULE_NET_STEERING,
			HOSTAPD_LEVEL_WARNING, "Don't send to myself "MACSTR"\n",
			MAC2STR(nsb->hapd->own_addr));
			*/
		}
		r0kh = r0kh->next;
	}
}

static void flood_closed_client(struct net_steering_client *client)
{
	struct net_steering_bss* nsb = client->nsb;
	struct wpabuf* buf;

	buf = wpabuf_alloc(MAX_FRAME_SIZE);
	header_put(buf, nsb->frame_sn++);
	put_closed_client(buf, client_get_mac(client), client_get_local_bssid(client));
	header_finalize(buf);

	hostapd_logger(nsb->hapd, client_get_local_bssid(client), HOSTAPD_MODULE_NET_STEERING,
			HOSTAPD_LEVEL_DEBUG, "sending closed client "MACSTR" to "MACSTR"\n",
			MAC2STR(client_get_mac(client)), MAC2STR(client_get_close_bssid(client)));

	flood_message(nsb, buf);
	wpabuf_free(buf);

	client_clear_close_bssid(client);
}

static void flood_close_client(struct net_steering_client *client)
{
	struct net_steering_bss* nsb = client->nsb;
	struct wpabuf* buf;

	buf = wpabuf_alloc(MAX_FRAME_SIZE);
	header_put(buf, nsb->frame_sn++);
	put_close_client(buf, client_get_mac(client), client_get_local_bssid(client),
			client_get_remote_bssid(client), client->nsb->hapd->iconf->channel);
	header_finalize(buf);

	hostapd_logger(nsb->hapd, client_get_local_bssid(client), HOSTAPD_MODULE_NET_STEERING,
			HOSTAPD_LEVEL_DEBUG, "sending close client "MACSTR" for "MACSTR"\n",
			MAC2STR(client_get_mac(client)),
			MAC2STR(client_get_remote_bssid(client)));

	flood_message(nsb, buf);
	wpabuf_free(buf);
}

static void do_flood_score(struct net_steering_client *client)
{
	struct net_steering_bss* nsb = client->nsb;
	struct wpabuf* buf;

	if (client->score == max_score) {
		hostapd_logger(nsb->hapd, client_get_local_bssid(client), HOSTAPD_MODULE_NET_STEERING,
			HOSTAPD_LEVEL_DEBUG, "skip flooding "MACSTR" max score %d\n",
			MAC2STR(client_get_mac(client)), client->score);
	} else {
		struct os_time now_t;
		struct os_time delta_t;
		os_memset(&delta_t, 0, sizeof(delta_t));

		os_get_time(&now_t);
		os_time_sub(&now_t, &client->association_time, &delta_t);
		// TODO need to deal with wraparound, currently about 49 days
		u32 associated_msecs = (delta_t.sec * 1000) + (delta_t.usec / 1000);

		hostapd_logger(nsb->hapd, client_get_local_bssid(client), HOSTAPD_MODULE_NET_STEERING,
			HOSTAPD_LEVEL_DEBUG, "sending "MACSTR" score %d associated %lu\n",
			MAC2STR(client_get_mac(client)), (int)client->score, (unsigned long)associated_msecs);

		buf = wpabuf_alloc(MAX_FRAME_SIZE);
		header_put(buf, nsb->frame_sn++);
		put_score(buf, client_get_mac(client), client_get_local_bssid(client), client->score, associated_msecs);
		header_finalize(buf);

		flood_message(nsb, buf);
		wpabuf_free(buf);
	}
}

static void flood_score(void *eloop_data, void *user_ctx)
{
	(void) user_ctx;
	struct net_steering_client* client = (struct net_steering_client*) eloop_data;
	do_flood_score(client);
	start_flood_timer(client);
}

static void do_client_disassociate(struct net_steering_client* client)
{
	static const int transition_timeout = 0;

	char mac[MACSTRLEN];
	if (!snprintf(mac, MACSTRLEN, MACSTR, MAC2STR(client_get_mac(client)))) return;

	if (client_is_associated(client))
	{
		if (client->nsb->mode == MODE_SUGGEST || client_supports_bss_transition(client)) {
			hostapd_logger(client->nsb->hapd, client_get_local_bssid(client), HOSTAPD_MODULE_NET_STEERING,
				HOSTAPD_LEVEL_INFO, "Fast BSS transition for "MACSTR" to "MACSTR" on channel %d\n",
				MAC2STR(client_get_mac(client)), MAC2STR(client_get_close_bssid(client)), client->remote_channel);

			wnm_send_bss_tm_req2(client->nsb->hapd, client->sta, transition_timeout,
					client_get_close_bssid(client), client->remote_channel);
		} else {
			hostapd_logger(client->nsb->hapd, client_get_local_bssid(client), HOSTAPD_MODULE_NET_STEERING,
				HOSTAPD_LEVEL_INFO, "Disassociate "MACSTR"\n", MAC2STR(client_get_mac(client)));

			if (hostapd_ctrl_iface_disassociate(client->nsb->hapd, mac))
			{
				hostapd_logger(client->nsb->hapd, client_get_local_bssid(client), HOSTAPD_MODULE_NET_STEERING,
					HOSTAPD_LEVEL_WARNING, "Failed to disassociate %s\n", mac);
			}
		}
	} else {
		hostapd_logger(client->nsb->hapd, client_get_local_bssid(client), HOSTAPD_MODULE_NET_STEERING,
			HOSTAPD_LEVEL_WARNING, "Cannot disassociate "MACSTR", not associated\n",
			MAC2STR(client_get_mac(client)));
	}
}

static void do_client_blacklist_add(struct net_steering_client* client)
{
	if (client->nsb->mode == MODE_FORCE) {
		char mac[MACSTRLEN];
		if (!snprintf(mac, MACSTRLEN, MACSTR, MAC2STR(client_get_mac(client)))) return;

		hostapd_logger(client->nsb->hapd, client_get_local_bssid(client), HOSTAPD_MODULE_NET_STEERING,
			HOSTAPD_LEVEL_WARNING, "Blacklist add "MACSTR"\n",
			MAC2STR(client_get_mac(client)));

		if (hostapd_ctrl_iface_blacklist_add(client->nsb->hapd, mac)) {
			hostapd_logger(client->nsb->hapd, client_get_local_bssid(client), HOSTAPD_MODULE_NET_STEERING,
				HOSTAPD_LEVEL_WARNING, "Failed to blacklist %s\n", mac);
		}
	}
}

static void do_client_blacklist_rm(struct net_steering_client* client)
{
	if (client->nsb->mode == MODE_FORCE) {
		char mac[MACSTRLEN];
		if (!snprintf(mac, MACSTRLEN, MACSTR, MAC2STR(client_get_mac(client)))) return;

		hostapd_logger(client->nsb->hapd, client_get_local_bssid(client), HOSTAPD_MODULE_NET_STEERING,
			HOSTAPD_LEVEL_WARNING, "Blacklist remove "MACSTR"\n", MAC2STR(client_get_mac(client)));

		if (hostapd_ctrl_iface_blacklist_rm(client->nsb->hapd, mac))
		{
			hostapd_logger(client->nsb->hapd, client_get_local_bssid(client), HOSTAPD_MODULE_NET_STEERING,
				HOSTAPD_LEVEL_WARNING, "Failed to remove %s from blacklist\n", mac);
		}
	}
}

#define STATE_MACHINE_DATA struct net_steering_client
#define STATE_MACHINE_DEBUG_PREFIX "STEERING"
#define STATE_MACHINE_ADDR sm->addr

#define SM_EVENT(machine, fromstate, e_event, tostate) \
static void sm_ ## machine ## _ ## fromstate ## _on_ ## e_event ## _ ## tostate(STATE_MACHINE_DATA *sm, \
			int global)

/* TODO maybe use a switch, but that would take a more complex set of macros */
#define SM_TRANSITION(machine, fromstate, e_event, tostate) \
	if (sm->machine ## _state == machine ## _ ## fromstate && event == machine ## _ ## e_event) { \
		hostapd_logger(sm->nsb->hapd, sm->nsb->hapd->conf->bssid, HOSTAPD_MODULE_NET_STEERING, \
		HOSTAPD_LEVEL_DEBUG, "%s => %s "MACSTR" %s\n",\
		state_to_str(sm->machine ## _state), state_to_str(machine ## _ ## tostate), \
		MAC2STR(client_get_mac(sm)), event_to_str(machine ## _ ## e_event)); \
		sm_ ## machine ## _ ## fromstate ## _on ## _ ## e_event ## _ ## tostate(sm, 0); \
		SM_ENTER(STEERING, tostate); \
		return; \
	}

#define SM_TRANS_NOOP(machine, fromstate, e_event, tostate) \
	if (sm->machine ## _state == machine ## _ ## fromstate && event == machine ## _ ## e_event) { \
		hostapd_logger(sm->nsb->hapd, sm->nsb->hapd->conf->bssid, HOSTAPD_MODULE_NET_STEERING, \
		HOSTAPD_LEVEL_DEBUG, "%s => %s "MACSTR" %s noop\n",\
		state_to_str(sm->machine ## _state), state_to_str(machine ## _ ## tostate), \
		MAC2STR(client_get_mac(sm)), event_to_str(machine ## _ ## e_event)); \
		SM_ENTER(STEERING, tostate); \
		return; \
	}

#define SM_STEP_EVENT(machine) \
static void sm_ ## machine ## _do_Event(STATE_MACHINE_DATA *sm, int event)

#define SM_STEP_EVENT_RUN(machine, event, sm) \
	sm_ ## machine ## _do_Event(sm, machine ## _ ## event);

SM_STATE(STEERING, IDLE) { SM_ENTRY(STEERING, IDLE); }
SM_STATE(STEERING, CONFIRMING) { SM_ENTRY(STEERING, CONFIRMING); }
SM_STATE(STEERING, ASSOCIATING) { SM_ENTRY(STEERING, ASSOCIATING); }
SM_STATE(STEERING, ASSOCIATED) { SM_ENTRY(STEERING, ASSOCIATED); }
SM_STATE(STEERING, REJECTING) { SM_ENTRY(STEERING, REJECTING); }
SM_STATE(STEERING, REJECTED) { SM_ENTRY(STEERING, REJECTED); }

SM_EVENT(STEERING, IDLE, E_ASSOCIATED, ASSOCIATED)
{
	start_flood_timer(sm);
}

SM_EVENT(STEERING, IDLE, E_PEER_IS_WORSE, CONFIRMING)
{
	flood_close_client(sm);
}

SM_EVENT(STEERING, IDLE, E_PEER_NOT_WORSE, REJECTED)
{
	do_client_blacklist_add(sm);
	client_start_timer(sm);
}

SM_EVENT(STEERING, IDLE, E_CLOSE_CLIENT, REJECTED)
{
	flood_close_client(sm);
	do_client_blacklist_add(sm);
	client_start_timer(sm);
}

SM_EVENT(STEERING, CONFIRMING, E_PEER_IS_WORSE, CONFIRMING)
{
	flood_close_client(sm);
}

SM_EVENT(STEERING, CONFIRMING, E_ASSOCIATED, ASSOCIATED)
{
	start_flood_timer(sm);
}

SM_EVENT(STEERING, ASSOCIATING, E_ASSOCIATED, ASSOCIATED)
{
	start_flood_timer(sm);
}

SM_EVENT(STEERING, ASSOCIATING, E_PEER_IS_WORSE, ASSOCIATING)
{
	flood_close_client(sm);
}

SM_EVENT(STEERING, ASSOCIATING, E_CLOSE_CLIENT, REJECTED)
{
	flood_closed_client(sm);
	do_client_blacklist_add(sm);
	client_start_timer(sm);
}


SM_EVENT(STEERING, ASSOCIATED, E_CLOSE_CLIENT, REJECTING)
{
	do_client_blacklist_add(sm);
	do_client_disassociate(sm);
	client_start_timer(sm);
	stop_flood_timer(sm);
}

SM_EVENT(STEERING, ASSOCIATED, E_DISASSOCIATED, IDLE)
{
	stop_flood_timer(sm);
}

SM_EVENT(STEERING, ASSOCIATED, E_PEER_IS_WORSE, ASSOCIATED)
{
	flood_close_client(sm);
}

SM_EVENT(STEERING, REJECTING, E_DISASSOCIATED, REJECTED)
{
	flood_closed_client(sm);
	client_stop_timer(sm); /* exiting REJECTING */
	client_start_timer(sm); /* enter REJECTED */
}

SM_EVENT(STEERING, REJECTING, E_PEER_IS_WORSE, CONFIRMING)
{
	do_client_blacklist_rm(sm);
	flood_close_client(sm);
	client_stop_timer(sm);
}

SM_EVENT(STEERING, REJECTING, E_PEER_LOST_CLIENT, CONFIRMING)
{
	do_client_blacklist_rm(sm);
	client_stop_timer(sm);
}

SM_EVENT(STEERING, REJECTING, E_TIMEOUT, ASSOCIATING)
{
	do_client_blacklist_rm(sm);
	client_stop_timer(sm);
}

SM_EVENT(STEERING, REJECTED, E_PEER_IS_WORSE, CONFIRMING)
{
	do_client_blacklist_rm(sm);
	flood_close_client(sm);
	client_stop_timer(sm);
}

SM_EVENT(STEERING, REJECTED, E_PEER_LOST_CLIENT, CONFIRMING)
{
	do_client_blacklist_rm(sm);
	flood_close_client(sm);
	client_stop_timer(sm);
}

SM_EVENT(STEERING, REJECTED, E_CLOSE_CLIENT, REJECTED)
{
	flood_close_client(sm);
}

SM_EVENT(STEERING, REJECTED, E_TIMEOUT, ASSOCIATING)
{
	do_client_blacklist_rm(sm);
	client_stop_timer(sm);
}


/* 

From original Alloy specification
Old State	 Event			 New State
--------------------------------------------------
Idle,		 Associated,	 Associated
Idle,		 PeerIsWorse,	 Confirming
Idle,		 PeerNotWorse,	 Rejected
Idle,		 PeerLostClient, Associating
Idle,		 CloseClient,	 Rejected

Confirming,	 ClosedClient,	 Associating
Confirming,	 Associated,	 Associated
Confirming,	 TimeOut,	     Idle
Confirming,	 PeerIsWorse,	 Confirming
Confirming,	 PeerNotWorse,	 Rejected

Associating, Associated,	 Associated
Associating, Disassociated,	 Idle
Associating, PeerIsWorse,	 Associating
Associating, CloseClient,	 Rejected

Associated,	 CloseClient,	 Rejecting
Associated,	 Disassociated,	 Idle
Associated,	 PeerIsWorse,	 Associated
Associated,	 Timer,          Associated

Rejecting,	 CloseClient,	 Rejecting
Rejecting,	 Disassociated,	 Rejected
Rejecting,	 PeerIsWorse,	 Confirming
Rejecting,	 PeerLostClient, Confirming
Rejecting,	 TimeOut,        Associating

Rejected,	 PeerIsWorse,    Confirming
Rejected,	 PeerLostClient, Confirming
Rejected,	 CloseClient,    Rejected
Rejected,	 TimeOut,        Associating

	-- NOTEs:
	-- 1) The client is only blacklisted in Rejecting and Rejected.
	-- 2) The Associated timer should fire immediately after transitioning to associated and then on an interval.
	-- 3) The TimeOutEvent in Rejecting+Rejected fires if we haven't gotten a score recently.
	-- 4) Events that don't match the above should be no-ops. Note that this does happen, for example if a MAP is in
	-- Confirming and gets PeerIsWorsePacket it will send out CloseClientPacket which means it will get two 
	-- ClosedClientPacket, one of which should be ignored.
	-- 5) The fsm should pop into existence when a MAP links up with the client and go away after being inactive.
*/

/*
Idle
{
	associated			Associated 		{}
	peer_is_worse		Confirming 		{unicast_close_client();}
	peer_not_worse		Rejected 		{blacklist();}
	peer_lost_client	Associating 	{}
	close_client		Rejected 		{unicast_closed_client(); blacklist();}
	Default				Idle			{}
}

Confirming
{
	closed_client		Associating 	{}
	associated			Associated 		{}
	time_out			Idle 			{}
	peer_is_worse		Confirming 		{unicast_close_client();}
	peer_not_worse		Rejected 		{blacklist();}
	Default				Confirming		{}
}

Associating
{
	associated			Associated 		{}
	disassociated		Idle 			{}
	peer_is_worse		Associating 	{unicast_close_client();}
	close_client		Rejected 		{unicast_closed_client(); blacklist();}
	Default				Associating		{}
}

Associated
Entry {fast_flooding(); flood_score();}
Exit  {slow_flooding();}
{
	close_client		Rejecting 		{blacklist(); disassociate(); clear_remotes();}
	disassociated		Idle 			{flood_peer_lost_client();}
	peer_is_worse		Associated 		{unicast_close_client();}
	Default				Associated		{}
}

Rejecting
Entry {start_timeout();}
Exit  {stop_timeout();}
{
	close_client		Rejecting 		{}
	disassociated		Rejected 		{unicast_closed_client();}
	peer_is_worse		Confirming 		{unicast_close_client(); unblacklist();}
	peer_lost_client	Confirming 		{unblacklist();}
	time_out			Associating 	{unblacklist();}
	Default				Rejecting		{}
}

Rejected
Entry {start_timeout();}
Exit  {stop_timeout();}
{
	peer_is_worse		Confirming 		{unicast_close_client(); unblacklist();}
	peer_lost_client	Confirming 		{unblacklist();}
	close_client		Rejected 		{unicast_closed_client();}
	time_out			Associating 	{unblacklist();}
	Default				Rejected		{}
}
*/

SM_STEP_EVENT(STEERING)
{
	/* Define transitions for every event that has an action to perform */
	/* Use no-ops for cases where only a state change is required */
	/* Beware of states that have entry/exit actions defined */
	SM_TRANSITION(STEERING, IDLE, E_ASSOCIATED, ASSOCIATED);
	SM_TRANSITION(STEERING, IDLE, E_PEER_IS_WORSE, CONFIRMING);
	SM_TRANSITION(STEERING, IDLE, E_PEER_NOT_WORSE, REJECTED);
	SM_TRANS_NOOP(STEERING, IDLE, E_PEER_LOST_CLIENT, ASSOCIATING);
	SM_TRANSITION(STEERING, IDLE, E_CLOSE_CLIENT, REJECTED);

	SM_TRANS_NOOP(STEERING, CONFIRMING, E_CLOSED_CLIENT, ASSOCIATING);
	SM_TRANSITION(STEERING, CONFIRMING, E_ASSOCIATED, ASSOCIATED);
	SM_TRANS_NOOP(STEERING, CONFIRMING, E_TIMEOUT, IDLE);
	SM_TRANSITION(STEERING, CONFIRMING, E_PEER_IS_WORSE, CONFIRMING);
	// This transition is invalid because if we are confirming, we have closed the client
	// and therefore we don't want to blacklist via rejected, so ignore this event
	//SM_TRANSITION(STEERING, CONFIRMING, E_PEER_NOT_WORSE, REJECTED);

	SM_TRANSITION(STEERING, ASSOCIATING, E_ASSOCIATED, ASSOCIATED);
	SM_TRANS_NOOP(STEERING, ASSOCIATING, E_DISASSOCIATED, IDLE);
	SM_TRANSITION(STEERING, ASSOCIATING, E_PEER_IS_WORSE, ASSOCIATING);
	SM_TRANSITION(STEERING, ASSOCIATING, E_CLOSE_CLIENT, REJECTED);

	SM_TRANSITION(STEERING, ASSOCIATED, E_CLOSE_CLIENT, REJECTING);
	SM_TRANSITION(STEERING, ASSOCIATED, E_DISASSOCIATED, IDLE);
	SM_TRANSITION(STEERING, ASSOCIATED, E_PEER_IS_WORSE, ASSOCIATED);

	SM_TRANSITION(STEERING, REJECTING, E_DISASSOCIATED, REJECTED);
	SM_TRANSITION(STEERING, REJECTING, E_PEER_IS_WORSE, CONFIRMING);
	SM_TRANSITION(STEERING, REJECTING, E_PEER_LOST_CLIENT, CONFIRMING);
	SM_TRANSITION(STEERING, REJECTING, E_TIMEOUT, ASSOCIATING);

	SM_TRANSITION(STEERING, REJECTED, E_PEER_IS_WORSE, CONFIRMING);
	SM_TRANSITION(STEERING, REJECTED, E_PEER_LOST_CLIENT, CONFIRMING);
	SM_TRANSITION(STEERING, REJECTED, E_CLOSE_CLIENT, REJECTED);
	SM_TRANSITION(STEERING, REJECTED, E_TIMEOUT, ASSOCIATING);

	/* By design, the default response is no state change */
	hostapd_logger(sm->nsb->hapd, sm->nsb->hapd->conf->bssid, HOSTAPD_MODULE_NET_STEERING,
		HOSTAPD_LEVEL_DEBUG_VERBOSE,
		"Client "MACSTR" default handler for %s - %s\n",
		MAC2STR(sm->addr), state_to_str(sm->STEERING_state), event_to_str(event));
}

static void client_timeout(void *eloop_data, void *user_ctx)
{
	(void) user_ctx;
	struct net_steering_client* client = (struct net_steering_client*) eloop_data;

	SM_STEP_EVENT_RUN(STEERING, E_TIMEOUT, client);
}

static void probe_timeout(void *eloop_data, void *user_ctx)
{
	(void) user_ctx;
	struct net_steering_client* client = (struct net_steering_client*) eloop_data;

	hostapd_logger(client->nsb->hapd, NULL, HOSTAPD_MODULE_NET_STEERING,
				HOSTAPD_LEVEL_INFO,
				"Probe timeout for client "MACSTR" score=%d\n",
				MAC2STR(client_get_mac(client)), client->score);

	client->score = max_score;
}
static void compare_scores(struct net_steering_client *client, u16 score)
{
	/* now consider the score if it is from the true owner of the client */
	if (client->score < score) {
		SM_STEP_EVENT_RUN(STEERING, E_PEER_IS_WORSE, client);
	} else {
		SM_STEP_EVENT_RUN(STEERING, E_PEER_NOT_WORSE, client);
	}
}

static void client_disassociate(struct net_steering_client* client)
{
	/* Process event first, then cleanup data structures */
	SM_STEP_EVENT_RUN(STEERING, E_DISASSOCIATED, client);

	client->sta = NULL;
	os_memset(client->remote_bssid, 0, ETH_ALEN);
	client->remote_time.sec = 0;
	client->remote_time.usec = 0;
	client->association_time.sec = 0;
	client->association_time.usec = 0;

	/* now that the client is disassociated, set probe timer */
	client_start_probe_timer(client);
}

static void receive_score(struct net_steering_bss* nsb, const u8* sta, const u8* bssid,
		u16 score, u32 association_msecs)
{
	struct net_steering_client *client = NULL;

	client = client_find(nsb, sta);
	if (!client) {
		client = client_create(nsb, sta);
		if (!client) {
			hostapd_logger(nsb->hapd, NULL, HOSTAPD_MODULE_NET_STEERING,
				HOSTAPD_LEVEL_INFO,
				"Failed to create client for "MACSTR"\n",
				MAC2STR(sta));
			return;
		}
	}

	hostapd_logger(nsb->hapd, nsb->hapd->conf->bssid, HOSTAPD_MODULE_NET_STEERING,
			HOSTAPD_LEVEL_DEBUG, MACSTR" sent score for "MACSTR" %d %lu local %d\n",
			MAC2STR(bssid), MAC2STR(client_get_mac(client)), score, (unsigned long) association_msecs, client->score);

	/* if we receive a score from a new AP for this client */
	/* Establish if the AP has newer information than the current one */
	if (os_memcmp(bssid, client_get_remote_bssid(client), ETH_ALEN) != 0) {
		struct os_time now_t;
		struct os_time association_t;
		struct os_time local_t;

		os_memset(&now_t, 0, sizeof(now_t));
		os_memset(&association_t, 0, sizeof(association_t));
		os_memset(&local_t, 0, sizeof(local_t));

		os_get_time(&now_t);

		association_t.sec = association_msecs / 1000;
		association_t.usec = (association_msecs % 1000) * 1000;

		/*
		 * Compute a local time that is corrected with associated time from remote.
		 * This allows us to determine the remote AP with the most recent information
		 * regarding the client, and consequently which AP's scores should be evaluated.
		 */
		os_time_sub(&now_t, &association_t, &local_t);

		hostapd_logger(nsb->hapd, nsb->hapd->conf->bssid, HOSTAPD_MODULE_NET_STEERING,
				HOSTAPD_LEVEL_DEBUG, MACSTR" current %ld %ld received %ld %ld\n",
				MAC2STR(bssid), client->remote_time.sec, client->remote_time.usec, local_t.sec, local_t.usec);

		/* should we switch which AP is believed to be associated with the client? */
		/* only if last remote time is before local time (giving us newer info) */
		if (os_time_before(&client->remote_time, &local_t)) {

			hostapd_logger(nsb->hapd, nsb->hapd->conf->bssid, HOSTAPD_MODULE_NET_STEERING,
					HOSTAPD_LEVEL_INFO, MACSTR" is associated with client "MACSTR"\n",
					MAC2STR(bssid), MAC2STR(client_get_mac(client)));

			/* if we are in associated state, client roamed and we need to change to disassociated */
			if (client_is_associated(client)) {
				client_disassociate(client);
				client_update_remote(client, bssid, &local_t);
			} else {

				/* client has moved to a new AP */
				client_update_remote(client, bssid, &local_t);
				compare_scores(client, score);
			}
		}
	} else {
		/* if this score is from the same AP, then check it */
		compare_scores(client, score);
	}
}

static void receive_close_client(struct net_steering_bss* nsb, const u8* sta,
		const u8* bssid, const u8* target_bssid, u8 ap_channel)
{
	struct net_steering_client *client = NULL;

	if (os_memcmp(nsb->hapd->conf->bssid, target_bssid, ETH_ALEN) == 0) {

		client = client_find(nsb, sta);
		if (!client) {
			hostapd_logger(nsb->hapd, nsb->hapd->conf->bssid, HOSTAPD_MODULE_NET_STEERING,
							HOSTAPD_LEVEL_DEBUG,
							"Close client can't find client "MACSTR"\n",
							MAC2STR(sta));
			return;
		}

		client->remote_channel = ap_channel;
		client_set_close_bssid(client, bssid);

		SM_STEP_EVENT_RUN(STEERING, E_CLOSE_CLIENT, client);
	}
}

static void receive_closed_client(struct net_steering_bss* nsb, const u8* sta, const u8* target_bssid)
{
	struct net_steering_client *client = NULL;

 	if (os_memcmp(nsb->hapd->conf->bssid, target_bssid, ETH_ALEN) == 0) {

		client = client_find(nsb, sta);
		if (!client) {
			return;
		}

		SM_STEP_EVENT_RUN(STEERING, E_CLOSED_CLIENT, client);
	}
}

static void receive(void *ctx, const u8 *src_addr, const u8 *buf, size_t len)
{
	struct net_steering_bss* nsb = ctx;
	u16 sn = 0;
	u8 magic, version, type_tlv, tlv_len = 0;
	u16 packet_len = 0;
	u16 score = 0;
	u32 association_msecs = 0;
	u8 sta[ETH_ALEN];
	u8 bssid[ETH_ALEN];
	u8 target_bssid[ETH_ALEN];
	u8 ap_channel = 0;
	size_t num_read = 0;
	const u8* buf_pos = buf;

	num_read = parse_header(buf_pos, len, &magic, &version, &packet_len, &sn);
	if (!num_read) {
		hostapd_logger(nsb->hapd, NULL, HOSTAPD_MODULE_NET_STEERING,
				HOSTAPD_LEVEL_DEBUG,
				"Dropping short message from "MACSTR": %d bytes\n",
				MAC2STR(src_addr), len);
		return;
	}

	if (len < packet_len) {
		hostapd_logger(nsb->hapd, NULL, HOSTAPD_MODULE_NET_STEERING,
				HOSTAPD_LEVEL_DEBUG,
				"Dropping short message from "MACSTR": recv %d bytes, expected %d\n",
				MAC2STR(src_addr), len, packet_len);
		return;
	}

	if (tlv_version != version || tlv_magic != magic) {
		hostapd_logger(nsb->hapd, NULL, HOSTAPD_MODULE_NET_STEERING,
				HOSTAPD_LEVEL_DEBUG,
				"Dropping invalid message from "MACSTR": magic %d version %d\n",
				MAC2STR(src_addr), magic, version);
		return;
	}
	buf_pos += num_read;

	while (buf_pos < buf + packet_len) {
		os_memset(sta, 0, ETH_ALEN);
		os_memset(bssid, 0, ETH_ALEN);
		os_memset(target_bssid, 0, ETH_ALEN);

		num_read = parse_tlv_header(buf_pos, packet_len-(buf_pos-buf), &type_tlv, &tlv_len);
		if (!num_read) {
			hostapd_logger(nsb->hapd, NULL, HOSTAPD_MODULE_NET_STEERING,
					HOSTAPD_LEVEL_DEBUG, "Could not parse tlv header from "MACSTR"\n",
					MAC2STR(src_addr));
			return;
		}
		buf_pos += num_read;

		switch (type_tlv)
		{
		case TLV_SCORE:
			num_read = parse_score(buf_pos, tlv_len, sta, bssid, &score, &association_msecs);
			if (!num_read) {
				hostapd_logger(nsb->hapd, NULL, HOSTAPD_MODULE_NET_STEERING,
						HOSTAPD_LEVEL_DEBUG, "Could not parse score from "MACSTR"\n",
						MAC2STR(src_addr));
				return;
			}
			buf_pos += num_read;

			receive_score(nsb, sta, bssid, score, association_msecs);
			break;
		case TLV_CLOSE_CLIENT:
			num_read = parse_close_client(buf_pos, tlv_len, sta, bssid, target_bssid, &ap_channel);
			if (!num_read) {
				hostapd_logger(nsb->hapd, NULL, HOSTAPD_MODULE_NET_STEERING,
						HOSTAPD_LEVEL_DEBUG, "Could not parse close client from "MACSTR"\n",
						MAC2STR(src_addr));
				return;
			}
			buf_pos += num_read;

			hostapd_logger(nsb->hapd, nsb->hapd->conf->bssid, HOSTAPD_MODULE_NET_STEERING,
					HOSTAPD_LEVEL_DEBUG, MACSTR" says "MACSTR" should close client "MACSTR"\n",
					MAC2STR(bssid), MAC2STR(target_bssid), MAC2STR(sta));

			receive_close_client(nsb, sta, bssid, target_bssid, ap_channel);
			break;
		case TLV_CLOSED_CLIENT:
			num_read = parse_closed_client(buf_pos, tlv_len, sta, target_bssid);
			if (!num_read) {
				hostapd_logger(nsb->hapd, NULL, HOSTAPD_MODULE_NET_STEERING,
						HOSTAPD_LEVEL_DEBUG, "Could not parse closed client from "MACSTR"\n",
						MAC2STR(src_addr));
				return;
			}
			buf_pos += num_read;

			hostapd_logger(nsb->hapd, nsb->hapd->conf->bssid, HOSTAPD_MODULE_NET_STEERING,
					HOSTAPD_LEVEL_DEBUG, MACSTR" closed client "MACSTR"\n",
					MAC2STR(target_bssid), MAC2STR(sta));

			receive_closed_client(nsb, sta, target_bssid);
			break;
		default:
			// skip unknown tlvs
			buf_pos += tlv_len;
			hostapd_logger(nsb->hapd, NULL, HOSTAPD_MODULE_NET_STEERING,
					HOSTAPD_LEVEL_WARNING, "Dropping unknown tlv type %d len %d from "MACSTR" : %d\n",
					type_tlv, tlv_len, MAC2STR(src_addr), ntohs(sn));
			break;
		}
	}

	//hostapd_logger(nsb->hapd, NULL, HOSTAPD_MODULE_NET_STEERING,
	//		HOSTAPD_LEVEL_DEBUG, "Received %d bytes from "MACSTR" : %d\n",
	//		len, MAC2STR(src_addr), ntohs(sn));
}


void net_steering_disassociation(struct hostapd_data *hapd, struct sta_info *sta)
{
	struct net_steering_bss* nsb = NULL;
	struct net_steering_client *client, *ctmp;

	if (dl_list_empty(&nsb_list)) return;

	// find the context
	dl_list_for_each(nsb, &nsb_list, struct net_steering_bss, list) {
		if (nsb->hapd == hapd) break;
	}

	if (!nsb) {
		hostapd_logger(hapd, hapd->conf->bssid, HOSTAPD_MODULE_NET_STEERING,
			HOSTAPD_LEVEL_WARNING, "Association to unknown bss "MACSTR"\n",
			MAC2STR(hapd->conf->bssid));
		return;
	}

	// find the client and clean it up
	dl_list_for_each_safe(client, ctmp, &nsb->clients, struct net_steering_client, list) {
		if (os_memcmp(client_get_mac(client), sta->addr, ETH_ALEN) == 0) {

			hostapd_logger(nsb->hapd, nsb->hapd->conf->bssid, HOSTAPD_MODULE_NET_STEERING,
						HOSTAPD_LEVEL_INFO, MACSTR" disassociated from "MACSTR" remote is "MACSTR"\n",
						MAC2STR(sta->addr), MAC2STR(nsb->hapd->conf->bssid),
						MAC2STR(client_get_close_bssid(client)));

			client_disassociate(client);
			break;
		}
	}
}

void net_steering_association(struct hostapd_data *hapd, struct sta_info *sta, int rssi)
{
	struct net_steering_bss* nsb = NULL;
	struct net_steering_client* client = NULL;

	if (dl_list_empty(&nsb_list)) return;

	dl_list_for_each(nsb, &nsb_list, struct net_steering_bss, list) {
		if (nsb->hapd == hapd) break;
	}

	if (!nsb) {
		hostapd_logger(hapd, hapd->conf->bssid, HOSTAPD_MODULE_NET_STEERING,
			HOSTAPD_LEVEL_WARNING, "Association to unknown bss "MACSTR"\n",
			MAC2STR(hapd->conf->bssid));
		return;
	}

	hostapd_logger(nsb->hapd, nsb->hapd->conf->bssid, HOSTAPD_MODULE_NET_STEERING,
				HOSTAPD_LEVEL_INFO, MACSTR" associated to "MACSTR" signal=%d\n",
				MAC2STR(sta->addr), MAC2STR(nsb->hapd->conf->bssid), rssi);

	if (sta->dot11MgmtOptionBSSTransitionActivated) {
		hostapd_logger(nsb->hapd, nsb->hapd->conf->bssid, HOSTAPD_MODULE_NET_STEERING,
			HOSTAPD_LEVEL_DEBUG, "Client "MACSTR" supports Fast BSS transition\n",
			MAC2STR(sta->addr));
	}

	client = client_find(nsb, sta->addr);
	if (!client)
	{
		client = client_create(nsb, sta->addr);
		if (!client) {
			hostapd_logger(nsb->hapd, nsb->hapd->conf->bssid, HOSTAPD_MODULE_NET_STEERING,
				HOSTAPD_LEVEL_WARNING, "Failed to create client "MACSTR" on bssid "MACSTR"\n",
				MAC2STR(sta->addr), MAC2STR(hapd->conf->bssid));

			return;
		}
	}

	os_memset(&client->remote_time, 0, sizeof(client->remote_time));
	os_memset(&client->remote_bssid, 0, ETH_ALEN);

	os_get_time(&client->association_time);
	client->score = compute_score(rssi);
	client_associate(client, sta);
	do_flood_score(client);
	SM_STEP_EVENT_RUN(STEERING, E_ASSOCIATED, client);
}

void net_steering_deinit(struct hostapd_data *hapd)
{
	struct net_steering_bss *nsb, *tmp;

	if (dl_list_empty(&nsb_list)) return;

	dl_list_for_each_safe(nsb, tmp, &nsb_list, struct net_steering_bss, list) {
		if (nsb->hapd == hapd) {
			struct net_steering_client *client, *ctmp;
			if (nsb->control != NULL) {
				l2_packet_deinit(nsb->control);
				wpa_printf(MSG_DEBUG, "net_steering_deinit - l2_packet_deinit");
			}

			// free all clients
			dl_list_for_each_safe(client, ctmp, &nsb->clients, struct net_steering_client, list) {
				client_delete(client);
			}

			dl_list_del(&nsb->list);
			os_memset(nsb, 0, sizeof(*nsb));
			os_free(nsb);
			break;
		}
	}
}

int net_steering_init(struct hostapd_data *hapd)
{
	struct net_steering_bss* nsb = NULL;

	/* see if there is any configuration */
	if (!hapd->conf->net_steeering_mode) {
		hostapd_logger(hapd, hapd->conf->bssid, HOSTAPD_MODULE_NET_STEERING,
				HOSTAPD_LEVEL_WARNING, "no configuration, steering disabled.\n");
		return 0;
	}
	/* if configuration is off, do nothing */
	if (os_strcmp(hapd->conf->net_steeering_mode, mode_off) == 0) {
		hostapd_logger(hapd, hapd->conf->bssid, HOSTAPD_MODULE_NET_STEERING,
				HOSTAPD_LEVEL_WARNING, "configured off, steering disabled.\n");
		return 0;
	}

#ifdef CONFIG_IEEE80211R
	// We piggy-back on fast transition configuration, and use that config to identify our peer APs
	if (!hapd->conf->r0kh_list) {
		hostapd_logger(hapd, hapd->conf->bssid, HOSTAPD_MODULE_NET_STEERING,
				HOSTAPD_LEVEL_WARNING, "No FT key holders configured, steering disabled.\n");
		return 0;
	}
#else
	hostapd_logger(hapd, hapd->conf->bssid, HOSTAPD_MODULE_NET_STEERING,
			HOSTAPD_LEVEL_WARNING, "FT feature not included in this build, steering disabled.\n");
	/* Signaling an error since user enabled steering, but R is not included */
	return -1;
#endif

	nsb = (struct net_steering_bss*) os_zalloc(sizeof(*nsb));

	if (!nsb) return -1;

	// TODO: what if there is no bridge in use? use iface?
	nsb->control = l2_packet_init(hapd->conf->bridge, NULL, proto, receive, nsb, 0);
	if (nsb->control == NULL) {
		hostapd_logger(nsb->hapd, nsb->hapd->conf->bssid, HOSTAPD_MODULE_NET_STEERING,
				HOSTAPD_LEVEL_WARNING, "net_steering_init - l2_packet_init failed for %s\n",
				hapd->conf->bridge);

		os_memset(nsb, 0, sizeof(*nsb));
		os_free(nsb);
		return -1;
	}

	nsb->hapd = hapd;
	dl_list_init(&nsb->clients);

	// add the context to the end of the list
	dl_list_add(&nsb_list, &nsb->list);
	hostapd_register_probereq_cb(hapd, probe_req_cb, nsb);

	hostapd_logger(nsb->hapd, nsb->hapd->conf->bssid, HOSTAPD_MODULE_NET_STEERING,
			HOSTAPD_LEVEL_INFO, "ready on %s, own addr "MACSTR": mode: %s\n",
			hapd->conf->bridge, MAC2STR(nsb->hapd->own_addr),
			hapd->conf->net_steeering_mode);

	if (os_strcmp(hapd->conf->net_steeering_mode, mode_suggest) == 0) nsb->mode = MODE_SUGGEST;
	else if (os_strcmp(hapd->conf->net_steeering_mode, mode_force) == 0) nsb->mode = MODE_FORCE;
	else nsb->mode = MODE_FORCE;

	return 0;
}

