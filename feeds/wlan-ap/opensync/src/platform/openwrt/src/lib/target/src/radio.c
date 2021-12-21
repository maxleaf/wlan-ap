/* SPDX-License-Identifier: BSD-3-Clause */

#include <stdio.h>
#include <stdbool.h>
#include <time.h>

#include <uci.h>
#include <uci_blob.h>

#include <target.h>

#include "ovsdb.h"
#include "ovsdb_update.h"
#include "ovsdb_sync.h"
#include "ovsdb_table.h"
#include "ovsdb_cache.h"

#include "nl80211.h"
#include "radio.h"
#include "vif.h"
#include "phy.h"
#include "log.h"
#include "evsched.h"
#include "uci.h"
#include "utils.h"
#include "captive.h"
#include "rrm_config.h"
#include "vlan.h"
#include "radius_proxy.h"
#include "timer.h"
#include "fixup.h"

ovsdb_table_t table_Hotspot20_Config;
ovsdb_table_t table_Hotspot20_OSU_Providers;
ovsdb_table_t table_Hotspot20_Icon_Config;
ovsdb_table_t table_Radius_Proxy_Config;

ovsdb_table_t table_APC_Config;
ovsdb_table_t table_APC_State;
ovsdb_table_t table_Wifi_VIF_Config;
ovsdb_table_t table_Wifi_Inet_Config;
ovsdb_table_t table_Node_Config;

static bool full_config = false;
unsigned int radproxy_apc = 0;

static struct uci_package *wireless;
struct uci_context *uci;
struct blob_buf b = { };
struct blob_buf del = { };
static struct timespec startup_time;

enum {
	WDEV_ATTR_PATH,
	WDEV_ATTR_DISABLED,
	WDEV_ATTR_CHANNEL,
	WDEV_ATTR_TXPOWER,
	WDEV_ATTR_BEACON_INT,
	WDEV_ATTR_HTMODE,
	WDEV_ATTR_HWMODE,
	WDEV_ATTR_COUNTRY,
	WDEV_ATTR_CHANBW,
	WDEV_ATTR_TX_ANTENNA,
	WDEV_ATTR_RX_ANTENNA,
	WDEV_ATTR_FREQ_BAND,
	WDEV_ATTR_CHANNELS,
	WDEV_ATTR_DISABLE_B_RATES,
	WDEV_ATTR_MAXASSOC_CLIENTS,
	WDEV_ATTR_LOCAL_PWR_CONSTRAINT,
	__WDEV_ATTR_MAX,
};

static const struct blobmsg_policy wifi_device_policy[__WDEV_ATTR_MAX] = {
	[WDEV_ATTR_PATH] = { .name = "path", .type = BLOBMSG_TYPE_STRING },
	[WDEV_ATTR_DISABLED] = { .name = "disabled", .type = BLOBMSG_TYPE_BOOL },
	[WDEV_ATTR_CHANNEL] = { .name = "channel", .type = BLOBMSG_TYPE_INT32 },
	[WDEV_ATTR_TXPOWER] = { .name = "txpower", .type = BLOBMSG_TYPE_INT32 },
	[WDEV_ATTR_BEACON_INT] = { .name = "beacon_int", .type = BLOBMSG_TYPE_INT32 },
	[WDEV_ATTR_HTMODE] = { .name = "htmode", .type = BLOBMSG_TYPE_STRING },
	[WDEV_ATTR_HWMODE] = { .name = "hwmode", .type = BLOBMSG_TYPE_STRING },
	[WDEV_ATTR_COUNTRY] = { .name = "country", .type = BLOBMSG_TYPE_STRING },
	[WDEV_ATTR_CHANBW] = { .name = "chanbw", .type = BLOBMSG_TYPE_INT32 },
	[WDEV_ATTR_TX_ANTENNA] = { .name = "txantenna", .type = BLOBMSG_TYPE_INT32 },
	[WDEV_ATTR_RX_ANTENNA] = { .name = "rxantenna", .type = BLOBMSG_TYPE_INT32 },
	[WDEV_ATTR_FREQ_BAND] = { .name = "freq_band", .type = BLOBMSG_TYPE_STRING },
	[WDEV_ATTR_CHANNELS] = {.name = "channels", .type = BLOBMSG_TYPE_ARRAY},
	[WDEV_ATTR_DISABLE_B_RATES] = { .name = "legacy_rates", .type = BLOBMSG_TYPE_BOOL },
	[WDEV_ATTR_MAXASSOC_CLIENTS] = { .name = "maxassoc", .type = BLOBMSG_TYPE_INT32 },
	[WDEV_ATTR_LOCAL_PWR_CONSTRAINT] = { .name = "local_pwr_constraint", .type = BLOBMSG_TYPE_INT32 },
};

#define SCHEMA_CUSTOM_OPT_SZ            24
#define SCHEMA_CUSTOM_OPTS_MAX          3

static const char custom_options_table[SCHEMA_CUSTOM_OPTS_MAX][SCHEMA_CUSTOM_OPT_SZ] =
{
	SCHEMA_CONSTS_DISABLE_B_RATES,
	SCHEMA_CONSTS_MAX_CLIENTS,
	SCHEMA_CONSTS_LOCAL_PWR_CONSTRAINT,
};

static void radio_config_custom_opt_set(struct blob_buf *b, struct blob_buf *del,
                                      const struct schema_Wifi_Radio_Config *rconf)
{
	int i;
	char value[20];
	const char *opt;
	const char *val;

	for (i = 0; i < SCHEMA_CUSTOM_OPTS_MAX; i++) {
		opt = custom_options_table[i];
		val = SCHEMA_KEY_VAL(rconf->custom_options, opt);

		if (!val)
			strncpy(value, "0", 20);
		else
			strncpy(value, val, 20);

		if (strcmp(opt, SCHEMA_CONSTS_DISABLE_B_RATES ) == 0) {
			if (strcmp(value, "1") == 0)
				blobmsg_add_bool(b, "legacy_rates", 0);
			else 
				blobmsg_add_bool(del, "legacy_rates", 0);
		} else if (strcmp(opt, SCHEMA_CONSTS_MAX_CLIENTS) == 0) {
			int maxassoc;
			maxassoc = strtol(value, NULL, 10);
			if (maxassoc <= 0) {
				blobmsg_add_u32(del, "maxassoc", maxassoc);
			} else {
				if (maxassoc > 100)
					maxassoc = 100;
				blobmsg_add_u32(b, "maxassoc", maxassoc);
			}
		} else if (strcmp(opt, SCHEMA_CONSTS_LOCAL_PWR_CONSTRAINT) == 0) {
			int pwr = atoi(value);
			if (!strcmp(rconf->freq_band, "2.4G")) {
				blobmsg_add_u32(del, "local_pwr_constraint", pwr);
			} else if (pwr >= 0 && pwr <=255) {
				blobmsg_add_u32(b, "local_pwr_constraint", pwr);
			} else {
				blobmsg_add_u32(del, "local_pwr_constraint", pwr);
			}
		}
	}
}

static void set_custom_option_state(struct schema_Wifi_Radio_State *rstate,
                                    int *index, const char *key,
                                    const char *value)
{
	STRSCPY(rstate->custom_options_keys[*index], key);
	STRSCPY(rstate->custom_options[*index], value);
	*index += 1;
	rstate->custom_options_len = *index;
}

static void radio_state_custom_options_get(struct schema_Wifi_Radio_State *rstate,
                                         struct blob_attr **tb)
{
	int i;
	int index = 0;
	const char *opt;
	char buf[5];

	for (i = 0; i < SCHEMA_CUSTOM_OPTS_MAX; i++) {
		opt = custom_options_table[i];

		if ((strcmp(opt, SCHEMA_CONSTS_DISABLE_B_RATES) == 0) && (tb[WDEV_ATTR_DISABLE_B_RATES])) {
			if (blobmsg_get_bool(tb[WDEV_ATTR_DISABLE_B_RATES])) {
				set_custom_option_state(rstate, &index, opt, "0");
			} else {
				set_custom_option_state(rstate, &index, opt, "1");
			}
		} else if (strcmp(opt, SCHEMA_CONSTS_MAX_CLIENTS) == 0) {
			if (tb[WDEV_ATTR_MAXASSOC_CLIENTS]) {
				snprintf(buf, sizeof(buf), "%d", blobmsg_get_u32(tb[WDEV_ATTR_MAXASSOC_CLIENTS]));
				set_custom_option_state(rstate, &index, opt, buf);
			} else {
                                set_custom_option_state(rstate, &index, opt, "0");
			}
		} else if (strcmp(opt, SCHEMA_CONSTS_LOCAL_PWR_CONSTRAINT) == 0) {
			if (tb[WDEV_ATTR_LOCAL_PWR_CONSTRAINT]) {
				snprintf(buf, sizeof(buf), "%d", blobmsg_get_u32(tb[WDEV_ATTR_LOCAL_PWR_CONSTRAINT]));
				set_custom_option_state(rstate, &index, opt, buf);
			}
		}
	}
}

static void set_channel_max_power(struct schema_Wifi_Radio_State *rstate,
                                    int *index, int channel,
                                    int max_power)
{
	rstate->channel_max_power_keys[*index] = channel;
	rstate->channel_max_power[*index] = max_power;
	*index += 1;
	rstate->channel_max_power_len = *index;
}

// Update the entire channel_max_power map in the radio state
static void update_channel_max_power(char* phy, struct schema_Wifi_Radio_State *rstate) {
	int channels[64];
	int channel_count = phy_get_channels(phy, channels);
	channel_count += phy_get_dfs_channels(phy, channels + channel_count);

	// Clear the data previously stored in channel_max_power
	memset(rstate->channel_max_power_keys, 0, sizeof(rstate->channel_max_power_keys));
	memset(rstate->channel_max_power, 0, sizeof(rstate->channel_max_power));
	rstate->channel_max_power_len = 0;

	// Set the new values for channel_max_power
	int i, index = 0;
	for (i = 0; i < channel_count && i < 64; i++) {
		int channel = channels[i];
		int max_tx_power = phy_get_max_tx_power(phy, channel);
		set_channel_max_power(rstate, &index, channel, max_tx_power);
	}
}

static int get_channel_tx_power_debugfs(char *stats_path, int *tx_power)
{
	bool found = false;
	char str[512];
	FILE * fp = NULL;
	int txpower = -1;
	int ret = -1;

	fp = fopen(stats_path, "r");
	if (!fp) {
		LOGI("%s: ERR Failed to get tx power, stats file does not exist", __func__);
		return -1;
	}

	const char *substr = "Channel TX power";

	while(fgets(str, 512, fp)) {
		if((strstr(str, substr)) != NULL) {
			char * plast = NULL, * pcur = str;
			while ((pcur = strtok( pcur, " "))) {
				plast = pcur;
				pcur = NULL;
			}
			txpower = atoi(plast);
			found = true;
			break;
		}
	}
	if (!found) {
		LOGE("%s: Unable to find Tx Power in stats", __func__);
	} else {
		/* txpower is set as 2 units per dBm in FW */
		*tx_power = txpower/2;
		ret = 0;
	}
	if (fp)
		fclose(fp);
	return ret;
}

static int get_channel_tx_power(char* phy, int *tx_power)
{
	char ath_driver[ATH_DRIVER_NAME_LEN] = {'\0'};
	char stats_path[128];

	if (phy_get_ath_driver_name(phy, ath_driver)) {
		LOGE("%s: Failed to get ATH driver name", __func__);
		return -1;
	}
	if (!strncmp(ath_driver, "ath10k", ATH_DRIVER_NAME_LEN)) {
		snprintf(stats_path, sizeof(stats_path),
				"/sys/kernel/debug/ieee80211/%s/%s/fw_stats", phy,
				ath_driver);
	} else if (!strncmp(ath_driver, "ath11k", ATH_DRIVER_NAME_LEN)) {
		snprintf(stats_path, sizeof(stats_path),
				"/sys/kernel/debug/ieee80211/%s/%s/fw_stats/pdev_stats", phy,
				ath_driver);
	} else {
		LOGE("%s: Unknown ATH driver", __func__);
		return -1;
	}

	if (get_channel_tx_power_debugfs(stats_path, tx_power)) {
		LOGI("%s: ERR Failed to get Channel Tx Power", __func__);
		return -1;
	}

	return 0;
}

int is_cloud_channel_change(const struct schema_Wifi_Radio_Config *rconf)
{
	int channel = 0;

	channel = radio_fixup_get_primary_chan(rconf->if_name);
	if (channel == rconf->channel)
		return -1;

	return 0;
}

const struct uci_blob_param_list wifi_device_param = {
	.n_params = __WDEV_ATTR_MAX,
	.params = wifi_device_policy,
};

static bool radio_state_update(struct uci_section *s, struct schema_Wifi_Radio_Config *rconf, bool force)
{
	struct blob_attr *tb[__WDEV_ATTR_MAX] = { };
	struct schema_Wifi_Radio_State  rstate;
	char phy[6];
	int antenna;
	int tx_power = -1;
	uint32_t chan = 0;

	LOGT("%s: get state", s->e.name);

	memset(&rstate, 0, sizeof(rstate));
	schema_Wifi_Radio_State_mark_all_present(&rstate);
	rstate._partial_update = true;
	rstate.channel_sync_present = false;
	rstate.channel_mode_present = false;
	rstate.radio_config_present = false;
	rstate.vif_states_present = false;

	blob_buf_init(&b, 0);
	uci_to_blob(&b, s, &wifi_device_param);
	blobmsg_parse(wifi_device_policy, __WDEV_ATTR_MAX, tb, blob_data(b.head), blob_len(b.head));

	SCHEMA_SET_STR(rstate.if_name, s->e.name);

	if (!tb[WDEV_ATTR_PATH] ||
	    phy_from_path(blobmsg_get_string(tb[WDEV_ATTR_PATH]), phy)) {
		LOGE("%s has no phy", rstate.if_name);
		return false;
	}

	update_channel_max_power(phy, &rstate);

	if (tb[WDEV_ATTR_CHANNEL]) {
		nl80211_channel_get(phy, &chan);
		if(chan)
			SCHEMA_SET_INT(rstate.channel, chan);
		else
			SCHEMA_SET_INT(rstate.channel, blobmsg_get_u32(tb[WDEV_ATTR_CHANNEL]));

		radio_fixup_set_primary_chan(rstate.if_name,
			     blobmsg_get_u32(tb[WDEV_ATTR_CHANNEL]));
	}

	SCHEMA_SET_INT(rstate.enabled, 1);
	if (!force && tb[WDEV_ATTR_DISABLED] && blobmsg_get_bool(tb[WDEV_ATTR_DISABLED]))
		SCHEMA_SET_INT(rstate.enabled, 0);
	else
		SCHEMA_SET_INT(rstate.enabled, 1);

	if (!get_channel_tx_power(phy, &tx_power)) {
		SCHEMA_SET_INT(rstate.tx_power, tx_power);
	} else {
		LOGI("%s:ERR: Failed to update Channel Tx Power", __func__);
		/* Set Tx Power to max OVSDB value */
		SCHEMA_SET_INT(rstate.tx_power, 32);
	}


	if (tb[WDEV_ATTR_BEACON_INT])
		SCHEMA_SET_INT(rstate.bcn_int, blobmsg_get_u32(tb[WDEV_ATTR_BEACON_INT]));
	else
		SCHEMA_SET_INT(rstate.bcn_int, 100);

	if (tb[WDEV_ATTR_HTMODE])
		SCHEMA_SET_STR(rstate.ht_mode, blobmsg_get_string(tb[WDEV_ATTR_HTMODE]));

	if (tb[WDEV_ATTR_HWMODE])
		SCHEMA_SET_STR(rstate.hw_mode, blobmsg_get_string(tb[WDEV_ATTR_HWMODE]));

	if (tb[WDEV_ATTR_TX_ANTENNA]) {
		antenna = blobmsg_get_u32(tb[WDEV_ATTR_TX_ANTENNA]);
		if (!antenna)
			antenna = phy_get_tx_available_antenna(phy);
		if ((antenna & 0xf0) && !(antenna & 0x0f))
			antenna= antenna >> 4;
		SCHEMA_SET_INT(rstate.tx_chainmask, antenna);
	}
	else
		SCHEMA_SET_INT(rstate.tx_chainmask, phy_get_tx_chainmask(phy));

	if (tb[WDEV_ATTR_RX_ANTENNA]) {

		antenna = blobmsg_get_u32(tb[WDEV_ATTR_RX_ANTENNA]);
		if (!antenna)
			antenna = phy_get_rx_available_antenna(phy);
		if ((antenna & 0xf0) && !(antenna & 0x0f))
			antenna= antenna >> 4;
		SCHEMA_SET_INT(rstate.rx_chainmask, antenna);
	}
	else
		SCHEMA_SET_INT(rstate.rx_chainmask, phy_get_rx_chainmask(phy));

	if (rstate.hw_mode_exists && rstate.ht_mode_exists) {
		struct mode_map *m = mode_map_get_cloud(rstate.ht_mode, rstate.hw_mode);

		if (m) {
			SCHEMA_SET_STR(rstate.hw_mode, m->hwmode);
			radio_fixup_set_hw_mode(rstate.if_name, m->hwmode);

			if (m->htmode)
				SCHEMA_SET_STR(rstate.ht_mode, m->htmode);
			else
				rstate.ht_mode_exists = false;
		} else {
			LOGE("%s: failed to decode ht/hwmode", rstate.if_name);
			rstate.hw_mode_exists = false;
			rstate.ht_mode_exists = false;
		}
	}

	if (tb[WDEV_ATTR_COUNTRY])
		SCHEMA_SET_STR(rstate.country, blobmsg_get_string(tb[WDEV_ATTR_COUNTRY]));

	nl80211_allowed_channels_get(phy);

	rstate.allowed_channels_len = phy_get_channels(phy, rstate.allowed_channels);
	rstate.allowed_channels_present = true;

	if (tb[WDEV_ATTR_FREQ_BAND])
		SCHEMA_SET_STR(rstate.freq_band, blobmsg_get_string(tb[WDEV_ATTR_FREQ_BAND]));
	else if (!phy_get_band(phy, rstate.freq_band))
		rstate.freq_band_exists = true;

	rstate.channels_len = phy_get_channels_state(phy, &rstate);

	if (strcmp(rstate.freq_band, "2.4G"))	{
                STRSCPY(rstate.hw_config_keys[0], "dfs_enable");
                snprintf(rstate.hw_config[0], sizeof(rstate.hw_config[0]), "1");
                STRSCPY(rstate.hw_config_keys[1], "dfs_ignorecac");
                snprintf(rstate.hw_config[1], sizeof(rstate.hw_config[0]), "0");
                STRSCPY(rstate.hw_config_keys[2], "dfs_usenol");
                snprintf(rstate.hw_config[2], sizeof(rstate.hw_config[0]), "1");
                rstate.hw_config_len = 3;
	}

	if (!phy_get_mac(phy, rstate.mac))
		rstate.mac_exists = true;

	radio_state_custom_options_get(&rstate, tb);

	if (rconf) {
		LOGN("%s: updating radio config", rstate.if_name);
		radio_state_to_conf(&rstate, rconf);
		SCHEMA_SET_STR(rconf->hw_type, "ath10k");
		radio_ops->op_rconf(rconf);
	}
	LOGT("%s: updating radio state", rstate.if_name);
	radio_ops->op_rstate(&rstate);

	return true;
}

bool target_radio_config_set2(const struct schema_Wifi_Radio_Config *rconf,
			      const struct schema_Wifi_Radio_Config_flags *changed)
{
	blob_buf_init(&b, 0);
	blob_buf_init(&del, 0);
	char phy[6];
	char ifname[8];
	struct mode_map *m;

	strncpy(ifname, rconf->if_name, sizeof(ifname));
	strncpy(phy, target_map_ifname(ifname), sizeof(phy));

	if (full_config && !changed->channel)
		full_config = false;

	if (changed->channel && rconf->channel) {
		/* If radio channel config has changed then rebalance channel,
		 * if not, that means the channel state changed due to a 
		 * radar detection and shall remain in the backup channel */
		if (is_cloud_channel_change(rconf) == 0) {
			/* In case of full config push, do not send CSA as hostapd
			 * is anyway going to reload with reload_config script detecting
			 * the UCI change. This would help avoid back to back channel switch.
			 */
			if (full_config) {
				LOGD("channel change due to full config push. New channel is %d", rconf->channel);
				blobmsg_add_u32(&b, "channel", rconf->channel);
				radio_fixup_set_primary_chan(rconf->if_name, rconf->channel);
				full_config = false;
			} else if (rrm_radio_rebalance_channel(rconf) == 0) {
				LOGD("channel change due to RRM rebalancing. New channel is %d", rconf->channel);
				blobmsg_add_u32(&b, "channel", rconf->channel);
				radio_fixup_set_primary_chan(rconf->if_name, rconf->channel);
			}
		}
	}

	if (changed->enabled)
		blobmsg_add_u8(&b, "disabled", rconf->enabled ? 0 : 1);

	if (changed->tx_power) {
		int max_tx_power;
		max_tx_power=phy_get_max_tx_power(phy,rconf->channel);
		if (rconf->tx_power<=max_tx_power) {

			rrm_config_txpower(rconf->if_name, rconf->tx_power);
			blobmsg_add_u32(&b, "txpower", rconf->tx_power);
		}
		else {
			rrm_config_txpower(rconf->if_name, max_tx_power);
			blobmsg_add_u32(&b, "txpower", max_tx_power);
		}
	}

	if (changed->tx_chainmask) {
		int tx_ant_avail,txchainmask;
		tx_ant_avail=phy_get_tx_available_antenna(phy);

		if ((tx_ant_avail & 0xf0) && !(tx_ant_avail & 0x0f)) {
			txchainmask = rconf->tx_chainmask << 4;
		} else {
			txchainmask = rconf->tx_chainmask;
		}
		if (rconf->tx_chainmask == 0) {
			blobmsg_add_u32(&b, "txantenna", tx_ant_avail);
		} else if ((txchainmask & tx_ant_avail) != txchainmask) {
			blobmsg_add_u32(&b, "txantenna", tx_ant_avail);
		} else {
			blobmsg_add_u32(&b, "txantenna", txchainmask);
		}
	}

	if (changed->rx_chainmask) {
		int rx_ant_avail, rxchainmask;
		rx_ant_avail=phy_get_rx_available_antenna(phy);

		if ((rx_ant_avail & 0xf0) && !(rx_ant_avail & 0x0f)) {
			rxchainmask = rconf->rx_chainmask << 4;
		} else {
			rxchainmask = rconf->rx_chainmask;
		}
		if (rconf->rx_chainmask == 0) {
			blobmsg_add_u32(&b, "rxantenna", rx_ant_avail);
		} else if ((rxchainmask & rx_ant_avail) != rxchainmask) {
			blobmsg_add_u32(&b, "rxantenna", rx_ant_avail);
		} else {
			blobmsg_add_u32(&b, "rxantenna", rxchainmask);
		}
	}

	if (changed->country)
		blobmsg_add_string(&b, "country", rconf->country);

	if (changed->bcn_int) {
		int beacon_int = rconf->bcn_int;

		if ((rconf->bcn_int < 50) || (rconf->bcn_int > 400))
			beacon_int = 100;
		blobmsg_add_u32(&b, "beacon_int", beacon_int);
	}

	if ((changed->channel || changed->ht_mode) || (changed->hw_mode) || (changed->freq_band)) {
		int channel_freq;
		char buffer[8];
		FILE *confFile_p;
		const char* hw_mode = rconf->hw_mode;

		channel_freq = ieee80211_channel_to_frequency(rconf->channel);
		if (!strcmp(rconf->hw_mode, "auto")) {
			char command[] = "auto-conf ";
			strcat(command, phy);
			confFile_p = popen(command, "r");
			
			if (confFile_p)
			{
				fgets(buffer, sizeof(buffer), confFile_p);
				pclose(confFile_p);
				buffer[strlen(buffer) - 1] = '\0'; // Remove extra \n that got added from 'echo' in script
				hw_mode = buffer;
			}
		}

		m = mode_map_get_uci(rconf->freq_band, get_max_channel_bw_channel(channel_freq, rconf->ht_mode), hw_mode);
		if (m) {
			blobmsg_add_string(&b, "htmode", m->ucihtmode);
			blobmsg_add_string(&b, "hwmode", m->ucihwmode);
			blobmsg_add_u32(&b, "chanbw", 20);
			radio_fixup_set_hw_mode(rconf->if_name, m->hwmode);
		} else
			 LOGE("%s: failed to set ht/hwmode", rconf->if_name);
	}

	struct blob_attr *n;
	int backup_channel = 0;
	backup_channel = rrm_get_backup_channel(rconf->freq_band);
	if(backup_channel) {
		n = blobmsg_open_array(&b, "channels");
		blobmsg_add_u32(&b, NULL, backup_channel);
		blobmsg_close_array(&b, n);
	} else {
		n = blobmsg_open_array(&del, "channels");
		blobmsg_add_u32(&del, NULL, backup_channel);
		blobmsg_close_array(&del, n);
	}	

	if (changed->custom_options)
		radio_config_custom_opt_set(&b, &del, rconf);

	blob_to_uci_section(uci, "wireless", rconf->if_name, "wifi-device",
			    b.head, &wifi_device_param, del.head);

	uci_commit_all(uci);
	return true;
}

static void periodic_task(void *arg)
{
	int ret = 0;
	struct uci_element *e = NULL, *tmp = NULL;
	if (startup_time.tv_sec) {
		static struct timespec current_time;

		clock_gettime(CLOCK_MONOTONIC, &current_time);
		current_time.tv_sec -= startup_time.tv_sec;
		if (current_time.tv_sec > 60 * 10) {
			startup_time.tv_sec = 0;
			radio_maverick(NULL);
		}
	}
	LOGD("periodic: start state update ");
	ret = uci_load(uci, "wireless", &wireless);
	if (ret) {
		LOGE("%s: uci_load() failed with rc %d", __func__, ret);
		goto out;
	}
	uci_foreach_element_safe(&wireless->sections, tmp, e) {
		struct uci_section *s = uci_to_section(e);

		if (!strcmp(s->type, "wifi-device"))
			radio_state_update(s, NULL, false);
	}

	uci_foreach_element_safe(&wireless->sections, tmp, e) {
		struct uci_section *s = uci_to_section(e);

		if (!strcmp(s->type, "wifi-iface"))
			vif_state_update(s, NULL);
	}
	uci_unload(uci, wireless);
	LOGD("periodic: stop state update ");

out:
	evsched_task_reschedule_ms(EVSCHED_SEC(15));
}

bool target_radio_config_init2(void)
{
	struct schema_Wifi_Radio_Config rconf;
	struct schema_Wifi_VIF_Config vconf;
	struct uci_element *e = NULL, *tmp = NULL;
	int invalidVifFound = 0;

	uci_load(uci, "wireless", &wireless);
	uci_foreach_element(&wireless->sections, e) {
		struct uci_section *s = uci_to_section(e);

		if (!strcmp(s->type, "wifi-device"))
			radio_state_update(s, &rconf, false);
	}

	uci_foreach_element_safe(&wireless->sections, tmp, e) {
		struct uci_section *s = uci_to_section(e);
		int radio_idx;

		if (!strcmp(s->type, "wifi-iface")) {
			if (sscanf((char*)s->e.name, "wlan%d", &radio_idx)) {
				vif_state_update(s, &vconf);
			} else {
				uci_section_del(uci, "vif", "wireless", (char *)s->e.name, "wifi-iface");
				invalidVifFound = 1;
			}
		}
	}
	if (invalidVifFound) {
		uci_commit(uci, &wireless, false);
	}
	uci_unload(uci, wireless);

	return true;
}

enum {
	SYS_ATTR_DEPLOYED,
	__SYS_ATTR_MAX,
};

static const struct blobmsg_policy system_policy[__SYS_ATTR_MAX] = {
	[SYS_ATTR_DEPLOYED] = { .name = "deployed", .type = BLOBMSG_TYPE_INT32 },
};

static const struct uci_blob_param_list system_param = {
	.n_params = __SYS_ATTR_MAX,
	.params = system_policy,
};

void radio_maverick(void *arg)
{
	struct blob_attr *tb[__SYS_ATTR_MAX] = { };
	struct schema_Wifi_Radio_Config rconf;
	struct uci_element *e = NULL;

	blob_buf_init(&b, 0);
	if (uci_section_to_blob(uci, "system", "tip", &b, &system_param))
		return;

	blobmsg_parse(system_policy, __SYS_ATTR_MAX, tb, blob_data(b.head), blob_len(b.head));
	if (tb[SYS_ATTR_DEPLOYED] && blobmsg_get_u32(tb[SYS_ATTR_DEPLOYED]))
		return;

	uci_load(uci, "wireless", &wireless);
	uci_foreach_element(&wireless->sections, e) {
		struct uci_section *s = uci_to_section(e);
		struct schema_Wifi_VIF_Config conf;
		char band[8];
		char ifname[] = "wlan0";

		if (strcmp(s->type, "wifi-device"))
			continue;
		radio_state_update(s, &rconf, true);

		if (strncmp(rconf.if_name, "radio", 5))
			continue;

		ifname[4] = rconf.if_name[5];
		memset(&conf, 0, sizeof(conf));
		schema_Wifi_VIF_Config_mark_all_present(&conf);
		conf._partial_update = true;
		conf._partial_update = true;

	        SCHEMA_SET_INT(conf.rrm, 1);
	        SCHEMA_SET_INT(conf.ft_psk, 0);
	        SCHEMA_SET_INT(conf.group_rekey, 0);
		strscpy(conf.mac_list_type, "none", sizeof(conf.mac_list_type));
		conf.mac_list_len = 0;
		SCHEMA_SET_STR(conf.if_name, ifname);
		SCHEMA_SET_STR(conf.ssid_broadcast, "enabled");
		SCHEMA_SET_STR(conf.mode, "ap");
		SCHEMA_SET_INT(conf.enabled, 1);
		SCHEMA_SET_INT(conf.btm, 1);
		SCHEMA_SET_INT(conf.uapsd_enable, true);
		SCHEMA_SET_STR(conf.bridge, "lan");
		SCHEMA_SET_INT(conf.vlan_id, 1);
		SCHEMA_SET_STR(conf.ssid, "Maverick");
		STRSCPY(conf.security_keys[0], "encryption");
	        STRSCPY(conf.security[0], "OPEN");
	        conf.security_len = 1;
		phy_get_band(target_map_ifname(rconf.if_name), band);
		if (strstr(band, "5"))
			SCHEMA_SET_STR(conf.min_hw_mode, "11ac");
		else
			SCHEMA_SET_STR(conf.min_hw_mode, "11n");

		radio_ops->op_vconf(&conf, rconf.if_name);
	}
	uci_unload(uci, wireless);
	uci_commit_all(uci);
	sync();
	LOGI("====Calling reload_config for Maverick ssid====");
	system("ubus call uci reload_config");
}


static void callback_Hotspot20_Config(ovsdb_update_monitor_t *mon,
				 struct schema_Hotspot20_Config *old,
				 struct schema_Hotspot20_Config *conf)
{

	switch (mon->mon_type)
	{
	case OVSDB_UPDATE_NEW:
	case OVSDB_UPDATE_MODIFY:
		(void) 	vif_hs20_update(conf);
		break;

	case OVSDB_UPDATE_DEL:
		break;

	default:
		LOG(ERR, "Hotspot20_Config: unexpected mon_type %d %s", mon->mon_type, mon->mon_uuid);
		break;
	}
	set_config_apply_timeout(mon);
	return;
}

static void callback_Hotspot20_OSU_Providers(ovsdb_update_monitor_t *mon,
				 struct schema_Hotspot20_OSU_Providers *old,
				 struct schema_Hotspot20_OSU_Providers *conf)
{
	switch (mon->mon_type)
	{
	case OVSDB_UPDATE_NEW:
	case OVSDB_UPDATE_MODIFY:
		(void) 	vif_hs20_osu_update(conf);
		break;

	case OVSDB_UPDATE_DEL:
		(void) vif_section_del("osu-provider");
		break;

	default:
		LOG(ERR, "Hotspot20_OSU_Providers: unexpected mon_type %d %s",
				mon->mon_type, mon->mon_uuid);
		break;
	}
	set_config_apply_timeout(mon);
	return;
}

static void callback_Hotspot20_Icon_Config(ovsdb_update_monitor_t *mon,
				 struct schema_Hotspot20_Icon_Config *old,
				 struct schema_Hotspot20_Icon_Config *conf)
{

	switch (mon->mon_type)
	{
	case OVSDB_UPDATE_NEW:
	case OVSDB_UPDATE_MODIFY:
		(void) 	vif_hs20_icon_update(conf);
		break;

	case OVSDB_UPDATE_DEL:
		(void) vif_section_del("hs20-icon");
		break;

	default:
		LOG(ERR, "Hotspot20_Icon_Config: unexpected mon_type %d %s",
				mon->mon_type, mon->mon_uuid);
		break;
	}
	set_config_apply_timeout(mon);
	return;

}

enum {
	WIF_APC_ENABLE,
	__WIF_APC_MAX,
};

static const struct blobmsg_policy apc_enpolicy[__WIF_APC_MAX] = {
		[WIF_APC_ENABLE] = { .name = "enabled", BLOBMSG_TYPE_BOOL },
};

const struct uci_blob_param_list apc_param = {
	.n_params = __WIF_APC_MAX,
	.params = apc_enpolicy,
};

void APC_config_update(struct schema_APC_Config *conf)
{
	struct blob_buf apcb = { };
	struct uci_context *apc_uci;

	apc_uci = uci_alloc_context();

	blob_buf_init(&apcb, 0);
	if (conf && conf->enabled == true) {
		blobmsg_add_bool(&apcb, "enabled", 1);
		system("/etc/init.d/apc start");
	} else {
		blobmsg_add_bool(&apcb, "enabled", 0);
		system("/etc/init.d/apc stop");
	}

	blob_to_uci_section(apc_uci, "apc", "apc", "apc",
			apcb.head, &apc_param, NULL);

	uci_commit_all(apc_uci);
	uci_free_context(apc_uci);
}

static void callback_APC_Config(ovsdb_update_monitor_t *mon,
                                struct schema_APC_Config *old,
                                struct schema_APC_Config *conf)
{
	if (mon->mon_type == OVSDB_UPDATE_DEL)
		APC_config_update(NULL);
	else
		APC_config_update(conf);

}

static void callback_APC_State(ovsdb_update_monitor_t *mon,
                                struct schema_APC_State *old,
                                struct schema_APC_State *conf)
{
	LOGN("APC_state: enabled:%s dr_addr:%s bdr_addr:%s mode:%s",
	     (conf->enabled_changed)? "changed":"unchanged", 
	     (conf->dr_addr_changed)? "changed":"unchanged",
	     (conf->bdr_addr_changed)? "changed":"unchanged",
	     (conf->mode_changed)? "changed":"unchanged");

	/* APC changed: start / stop radius proxy service if needed */
	vif_check_radius_proxy();
}

struct schema_APC_State apc_state;
enum {
	APC_ATTR_MODE,
	APC_ATTR_DR_ADDR,
	APC_ATTR_BDR_ADDR,
	APC_ATTR_ENABLED,
	__APC_ATTR_MAX,
};

static const struct blobmsg_policy apc_policy[__APC_ATTR_MAX] = {
	[APC_ATTR_MODE] = { .name = "mode", .type = BLOBMSG_TYPE_STRING },
	[APC_ATTR_DR_ADDR] = { .name = "dr_addr", .type = BLOBMSG_TYPE_STRING },
	[APC_ATTR_BDR_ADDR] = { .name = "bdr_addr", .type = BLOBMSG_TYPE_STRING },
	[APC_ATTR_ENABLED] = { .name = "enabled", .type = BLOBMSG_TYPE_BOOL },
};

struct schema_APC_Config apc_conf;


bool apc_read_conf(struct schema_APC_Config *apcconf)
{
	json_t *jrows;
	int cnt = 0;
	int i = 0;
	pjs_errmsg_t perr;

	jrows = ovsdb_sync_select_where(SCHEMA_TABLE(APC_Config), NULL);
	if(!jrows)
	{
		return false;
	}
	cnt = json_array_size(jrows);
	if(!cnt)
	{
		json_decref(jrows);
		return false;
	}

	for (i = 0; i < cnt; i++)
	{
		if(!schema_APC_Config_from_json(apcconf, json_array_get(jrows, i),
						false, perr))
		{
			LOGE("Unable to parse APC Config column: %s", perr);
			json_decref(jrows);
			return false;
		}
	}
	json_decref(jrows);

	return true;
}

bool apc_read_state(struct schema_APC_State *apcst)
{
	json_t *jrows;
	int cnt = 0;
	int i = 0;
	pjs_errmsg_t perr;

	jrows = ovsdb_sync_select_where(SCHEMA_TABLE(APC_State), NULL);
	if(!jrows)
	{
		return false;
	}
	cnt = json_array_size(jrows);
	if(!cnt)
	{
		json_decref(jrows);
		return false;
	}

	for (i = 0; i < cnt; i++)
	{
		if(!schema_APC_State_from_json(apcst, json_array_get(jrows, i),
						false, perr))
		{
			LOGE("Unable to parse APC State column: %s", perr);
			json_decref(jrows);
			return false;
		}
	}
	json_decref(jrows);

	return true;
}

/* Check if apc conf is disabled, if disabled the update state
 * with NC mode and return, this is to avoid the apc ubus
 * notifications which come after the APC is disabled */
bool apc_conf_en()
{
	struct schema_APC_Config apcconf;
	struct schema_APC_State apc_state;

	if(apc_read_conf(&apcconf) == false)
		return true;

	if (apcconf.enabled == false) {
		SCHEMA_SET_STR(apc_state.mode, "NC");
		SCHEMA_SET_STR(apc_state.dr_addr, "0.0.0.0");
		SCHEMA_SET_STR(apc_state.bdr_addr, "0.0.0.0");
		SCHEMA_SET_INT(apc_state.enabled, false);
		if (!ovsdb_table_update(&table_APC_State, &apc_state))
			LOG(ERR, "APC_state: failed to update");
		return false;
	}
	return true;
}

void apc_state_set(struct blob_attr *msg)
{
	struct blob_attr *tb[__APC_ATTR_MAX] = { };
	struct schema_APC_State apc_state;

	if(apc_conf_en() == false)
		return;

	blobmsg_parse(apc_policy, __APC_ATTR_MAX, tb,
		      blob_data(msg), blob_len(msg));

	if (tb[APC_ATTR_MODE]) {
		LOGD("APC mode: %s", blobmsg_get_string(tb[APC_ATTR_MODE]));
		SCHEMA_SET_STR(apc_state.mode,
			       blobmsg_get_string(tb[APC_ATTR_MODE]));
	}
	if (tb[APC_ATTR_DR_ADDR]) {
		LOGD("APC dr-addr: %s", blobmsg_get_string(tb[APC_ATTR_DR_ADDR]));
		SCHEMA_SET_STR(apc_state.dr_addr,
			       blobmsg_get_string(tb[APC_ATTR_DR_ADDR]));
	}
	if (tb[APC_ATTR_BDR_ADDR]) {
		LOGD("APC bdr-addr: %s", blobmsg_get_string(tb[APC_ATTR_BDR_ADDR]));
		SCHEMA_SET_STR(apc_state.bdr_addr,
			       blobmsg_get_string(tb[APC_ATTR_BDR_ADDR]));
	}
	if (tb[APC_ATTR_ENABLED]) {
		LOGD("APC enabled: %d", blobmsg_get_bool(tb[APC_ATTR_ENABLED]));
		if (blobmsg_get_bool(tb[APC_ATTR_ENABLED])) {
			SCHEMA_SET_INT(apc_state.enabled, true);
		}
		else {
			SCHEMA_SET_INT(apc_state.enabled, false);
		}
	}

	LOGI("APC_state Updating: mode: %s, dr-addr: %s bdr-addr: %s", 
	     apc_state.mode, apc_state.dr_addr, apc_state.bdr_addr);

	if (!ovsdb_table_update(&table_APC_State, &apc_state))
		LOG(ERR, "APC_state: failed to update");
	if (radproxy_apc) {
		radproxy_apc = 0;
		system("ubus call service event '{\"type\": \"config.change\", \"data\": { \"package\": \"wireless\" }}'");
	}
}

static ovsdb_table_t table_Manager;
static int conn_since = 0;
#define APC_CLOUD_MON_PERIOD 60

static void apc_enable(bool flag) {

	struct schema_APC_State apc_state;
	
	LOGI("APC %s: %s APC", __func__, flag?"enable":"disable");
	if (flag == false) {
		if(apc_read_state(&apc_state) == false) {
			LOG(ERR, "%s: APC_State read failed", __func__);
			apc_state.enabled = true;
		}

		if (apc_state.enabled == true) {
			SCHEMA_SET_INT(apc_conf.enabled, flag);
			if (!ovsdb_table_update(&table_APC_Config, &apc_conf)) {
				LOG(ERR, "%s:APC_Config: failed to update", __func__);
				return;
			}
			SCHEMA_SET_STR(apc_state.mode, "NC");
			SCHEMA_SET_STR(apc_state.dr_addr, "0.0.0.0");
			SCHEMA_SET_STR(apc_state.bdr_addr, "0.0.0.0");
			SCHEMA_SET_INT(apc_state.enabled, false);
			if (!ovsdb_table_update(&table_APC_State, &apc_state))
				LOG(ERR, "APC_state: failed to update");
		}
	} else {
		SCHEMA_SET_INT(apc_conf.enabled, flag);
		if (!ovsdb_table_update(&table_APC_Config, &apc_conf)) {
			LOG(ERR, "%s:APC_Config: failed to update", __func__);
			return;
		}
	}
}

static void
apc_cld_mon_cb(struct schema_Manager *mgr)
{
	int i = 0;
	conn_since = 0;
	struct schema_APC_State apc_state;
	int ret = 0;
	int link = 1;

	if(apc_read_state(&apc_state) == false) {
		LOG(ERR, "%s: APC_State read failed", __func__);
		return;
	}

	/*Checks if wan ethernet port is down and disables apc*/
	ret = system("/bin/check_wan_link.sh");
	if (WIFEXITED(ret)) {
		link = WEXITSTATUS(ret);
		if (link == 0) {
			LOGD("APC link down");
			apc_enable(false);
			return;
		}
	}

	/*if cloud conn is false then disable apc*/
	if (mgr->is_connected == false) {
			apc_enable(false);
	}
	else {
		for(i=0; i < mgr->status_len; i++) {
			if(!strncmp(mgr->status_keys[i] , "sec_since_connect",
					       strlen("sec_since_connect"))) {
				conn_since = atoi(mgr->status[i]);
				break;
			}
		}

		/*if the APC was stopped earlier, start it if connection good
		 * for atleast 60 secs*/
		if (!apc_state.enabled && conn_since > APC_CLOUD_MON_PERIOD) {
			apc_enable(true);
		}
	}
}

/*Monitor the cloud connection*/
static void callback_Manager(ovsdb_update_monitor_t *mon,
			     struct schema_Manager *old,
			     struct schema_Manager *conf)
{
	switch (mon->mon_type)
	{
	case OVSDB_UPDATE_NEW:
	case OVSDB_UPDATE_MODIFY:
		apc_cld_mon_cb(conf);
		break;

	case OVSDB_UPDATE_DEL:
		apc_enable(false);
		break;

	default:
		break;
	}
	return;
}

void cloud_disconn_mon(void)
{
	OVSDB_TABLE_INIT_NO_KEY(Manager);
	OVSDB_TABLE_MONITOR(Manager, false);
}

void apc_init()
{
	/* APC Config */
	OVSDB_TABLE_INIT_NO_KEY(APC_Config);
	OVSDB_TABLE_MONITOR(APC_Config, false);
	/* Disable APC by default, enable when cloud connected*/
	SCHEMA_SET_INT(apc_conf.enabled, false);
	LOGI("APC state/config Initialize");
	if (!ovsdb_table_insert(&table_APC_Config, &apc_conf)) {
		LOG(ERR, "APC_Config: failed to initialize");
		return;
	}

	/* APC State */
	OVSDB_TABLE_INIT_NO_KEY(APC_State);
	OVSDB_TABLE_MONITOR(APC_State, false);
	SCHEMA_SET_STR(apc_state.mode, "NC");
	SCHEMA_SET_STR(apc_state.dr_addr, "0.0.0.0");
	SCHEMA_SET_STR(apc_state.bdr_addr, "0.0.0.0");
	SCHEMA_SET_INT(apc_state.enabled, false);
	if (!ovsdb_table_insert(&table_APC_State, &apc_state)) {
		LOG(ERR, "APC_state: failed to initialize");
		return;
	}

	/* Cloud connection monitor - if cloud unreachable
	 * for certain time, disable APC and enable after the
	 * cloud connection becomes stable. */
	cloud_disconn_mon();

}
static void apply_config_handler(struct timeout *timeout)
{
	radius_proxy_fixup();
	uci_commit_all(uci);
	sync();
	LOGI("====Calling reload_config====");
	system("/sbin/reload_config");
}

static struct timeout config_timer = {
		.cb = apply_config_handler
};

static void config_timer_task(void *arg)
{
	timer_expiry_check(&config_timer);
	evsched_task_reschedule_ms(EVSCHED_SEC(1));
}

void set_config_apply_timeout(ovsdb_update_monitor_t *mon)
{
	static bool firstconfig = true;
	LOGI("=====Received config update - table:%s uuid:%s Action:%d======", mon->mon_table, mon->mon_uuid, mon->mon_type);
	if(firstconfig) {
		firstconfig = false;
		timeout_set(&config_timer, CONFIG_APPLY_TIMEOUT * 1000);
		evsched_task(&config_timer_task, NULL, EVSCHED_SEC(1));
	} else {
		timeout_set(&config_timer, CONFIG_APPLY_TIMEOUT * 1000);
	}
}

static void callback_Wifi_Inet_Config(ovsdb_update_monitor_t *mon,
		struct schema_Wifi_Inet_Config *old_rec,
		struct schema_Wifi_Inet_Config *iconf)
{
	full_config = true;
	set_config_apply_timeout(mon);
}

static void callback_Node_Config(ovsdb_update_monitor_t *mon,
		struct schema_Node_Config *old,
		struct schema_Node_Config *conf)
{
	set_config_apply_timeout(mon);
}

bool target_radio_init(const struct target_radio_ops *ops)
{
	uci = uci_alloc_context();

	captive_portal_init();
	target_map_init();

	target_map_insert("radio0", "phy0");
	target_map_insert("radio1", "phy1");
	target_map_insert("radio2", "phy2");

	radio_ops = ops;

	OVSDB_TABLE_INIT(Hotspot20_Config, _uuid);
	OVSDB_TABLE_MONITOR(Hotspot20_Config, false);

	OVSDB_TABLE_INIT(Hotspot20_OSU_Providers, _uuid);
	OVSDB_TABLE_MONITOR(Hotspot20_OSU_Providers, false);

	OVSDB_TABLE_INIT(Hotspot20_Icon_Config, _uuid);
	OVSDB_TABLE_MONITOR(Hotspot20_Icon_Config, false);

	OVSDB_TABLE_INIT(Wifi_RRM_Config, _uuid);
	OVSDB_TABLE_MONITOR(Wifi_RRM_Config, false);

	OVSDB_TABLE_INIT(Radius_Proxy_Config, _uuid);
	OVSDB_TABLE_MONITOR(Radius_Proxy_Config, false);

	OVSDB_TABLE_INIT(Wifi_Inet_Config, _uuid);
	OVSDB_TABLE_MONITOR(Wifi_Inet_Config, false);

	OVSDB_TABLE_INIT(Node_Config, _uuid);
	OVSDB_TABLE_MONITOR(Node_Config, false);

	evsched_task(&periodic_task, NULL, EVSCHED_SEC(5));

	radio_nl80211_init();
	radio_ubus_init();
	apc_init();

	clock_gettime(CLOCK_MONOTONIC, &startup_time);

	return true;
}

bool target_radio_config_need_reset(void)
{
	return true;
}

