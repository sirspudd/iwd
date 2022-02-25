/*
 *
 *  Wireless daemon for Linux
 *
 *  Copyright (C) 2015-2019  Intel Corporation. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <limits.h>
#include <linux/if.h>
#include <linux/if_ether.h>
#include <ell/ell.h>

#include "ell/useful.h"
#include "linux/nl80211.h"
#include "src/iwd.h"
#include "src/module.h"
#include "src/wiphy.h"
#include "src/ie.h"
#include "src/common.h"
#include "src/network.h"
#include "src/knownnetworks.h"
#include "src/nl80211cmd.h"
#include "src/nl80211util.h"
#include "src/util.h"
#include "src/p2putil.h"
#include "src/mpdu.h"
#include "src/band.h"
#include "src/scan.h"

/* User configurable options */
static double RANK_5G_FACTOR;
static uint32_t SCAN_MAX_INTERVAL;
static uint32_t SCAN_INIT_INTERVAL;

static struct l_queue *scan_contexts;

static struct l_genl_family *nl80211;

struct scan_context;

struct scan_periodic {
	struct l_timeout *timeout;
	uint16_t interval;
	scan_trigger_func_t trigger;
	scan_notify_func_t callback;
	void *userdata;
	uint32_t id;
	bool needs_active_scan:1;
};

struct scan_request {
	struct scan_context *sc;
	scan_trigger_func_t trigger;
	scan_notify_func_t callback;
	void *userdata;
	scan_destroy_func_t destroy;
	bool canceled : 1; /* Is scan_cancel being called on this request? */
	bool passive:1; /* Active or Passive scan? */
	bool started : 1; /* Has TRIGGER_SCAN succeeded at least once? */
	bool periodic : 1; /* Started as a periodic scan? */
	/*
	 * Set to true if the TRIGGER_SCAN command at the head of the 'cmds'
	 * queue was acked by the kernel indicating that the scan request was
	 * successful.  May be set and cleared multiple times during a
	 * the scan_request lifetime (as each command in the 'cmds' queue is
	 * issued to the kernel).  Will be false if the current request
	 * was not started due to an -EBUSY error from the kernel.  Also will
	 * be false when the scan is complete and GET_SCAN is pending.
	 */
	bool triggered : 1;
	bool in_callback : 1; /* Scan request complete, re-entrancy guard */
	struct l_queue *cmds;
	/* The time the current scan was started. Reported in TRIGGER_SCAN */
	uint64_t start_time_tsf;
	struct wiphy_radio_work_item work;
};

struct scan_context {
	uint64_t wdev_id;
	/*
	 * Tells us whether a scan, our own or external, is running.
	 * Set when scan gets triggered, cleared when scan done and
	 * before actual results are queried.
	 */
	enum scan_state state;
	struct scan_periodic sp;
	struct l_queue *requests;
	/* Non-zero if SCAN_TRIGGER is still running */
	unsigned int start_cmd_id;
	/* Non-zero if GET_SCAN is still running */
	unsigned int get_scan_cmd_id;
	/*
	 * Special request used for getting scan results after the firmware
	 * roamed automatically.
	 */
	unsigned int get_fw_scan_cmd_id;
	struct wiphy *wiphy;
};

struct scan_results {
	struct scan_context *sc;
	struct l_queue *bss_list;
	struct scan_freq_set *freqs;
	uint64_t time_stamp;
	struct scan_request *sr;
};

static bool start_next_scan_request(struct wiphy_radio_work_item *item);
static void scan_periodic_rearm(struct scan_context *sc);

static bool scan_context_match(const void *a, const void *b)
{
	const struct scan_context *sc = a;
	const uint64_t *wdev_id = b;

	return sc->wdev_id == *wdev_id;
}

static bool scan_request_match(const void *a, const void *b)
{
	const struct scan_request *sr = a;
	uint32_t id = L_PTR_TO_UINT(b);

	return sr->work.id == id;
}

static void scan_request_free(struct wiphy_radio_work_item *item)
{
	struct scan_request *sr = l_container_of(item, struct scan_request,
							work);

	if (sr->destroy)
		sr->destroy(sr->userdata);

	l_queue_destroy(sr->cmds, (l_queue_destroy_func_t) l_genl_msg_unref);

	l_free(sr);
}

static void scan_request_failed(struct scan_context *sc,
				struct scan_request *sr, int err)
{
	sr->in_callback = true;

	if (sr->trigger)
		sr->trigger(err, sr->userdata);
	else if (sr->callback)
		sr->callback(err, NULL, NULL, sr->userdata);

	sr->in_callback = false;
	l_queue_remove(sc->requests, sr);
	wiphy_radio_work_done(sc->wiphy, sr->work.id);
}

static struct scan_context *scan_context_new(uint64_t wdev_id)
{
	struct wiphy *wiphy = wiphy_find_by_wdev(wdev_id);
	struct scan_context *sc;

	if (!wiphy)
		return NULL;

	sc = l_new(struct scan_context, 1);

	sc->wdev_id = wdev_id;
	sc->wiphy = wiphy;
	sc->state = SCAN_STATE_NOT_RUNNING;
	sc->requests = l_queue_new();

	return sc;
}

static void scan_request_cancel(void *data)
{
	struct scan_request *sr = data;

	wiphy_radio_work_done(sr->sc->wiphy, sr->work.id);
}

static void scan_context_free(struct scan_context *sc)
{
	l_debug("sc: %p", sc);

	l_queue_destroy(sc->requests, scan_request_cancel);

	if (sc->sp.timeout)
		l_timeout_remove(sc->sp.timeout);

	if (sc->start_cmd_id && nl80211)
		l_genl_family_cancel(nl80211, sc->start_cmd_id);

	if (sc->get_scan_cmd_id && nl80211)
		l_genl_family_cancel(nl80211, sc->get_scan_cmd_id);

	if (sc->get_fw_scan_cmd_id && nl80211)
		l_genl_family_cancel(nl80211, sc->get_fw_scan_cmd_id);

	l_free(sc);
}

static void scan_request_triggered(struct l_genl_msg *msg, void *userdata)
{
	struct scan_context *sc = userdata;
	struct scan_request *sr = l_queue_peek_head(sc->requests);
	int err;

	sc->start_cmd_id = 0;

	err = l_genl_msg_get_error(msg);
	if (err < 0) {
		/* Scan in progress, assume another scan is running */
		if (err == -EBUSY) {
			sc->state = SCAN_STATE_PASSIVE;
			return;
		}

		scan_request_failed(sc, sr, err);

		l_error("Received error during CMD_TRIGGER_SCAN: %s (%d)",
			strerror(-err), -err);

		return;
	}

	sc->state = sr->passive ? SCAN_STATE_PASSIVE : SCAN_STATE_ACTIVE;
	l_debug("%s scan triggered for wdev %" PRIx64,
		sr->passive ? "Passive" : "Active", sc->wdev_id);

	sr->triggered = true;
	sr->started = true;
	l_genl_msg_unref(l_queue_pop_head(sr->cmds));

	if (sr->trigger) {
		sr->trigger(0, sr->userdata);

		/*
		 * Reset callback for the consequent scan triggerings of the
		 * multi-segmented scans.
		 */
		sr->trigger = NULL;
	}
}

struct scan_freq_append_data {
	struct l_genl_msg *msg;
	int count;
};

static void scan_freq_append(uint32_t freq, void *user_data)
{
	struct scan_freq_append_data *data = user_data;

	l_genl_msg_append_attr(data->msg, data->count++, 4, &freq);
}

static void scan_build_attr_scan_frequencies(struct l_genl_msg *msg,
						struct scan_freq_set *freqs)
{
	struct scan_freq_append_data append_data = { msg, 0 };

	l_genl_msg_enter_nested(msg, NL80211_ATTR_SCAN_FREQUENCIES);

	scan_freq_set_foreach(freqs, scan_freq_append, &append_data);

	l_genl_msg_leave_nested(msg);
}

static void scan_build_attr_ie(struct l_genl_msg *msg,
					struct scan_context *sc,
					const struct scan_parameters *params)
{
	struct iovec iov[3];
	unsigned int iov_elems = 0;
	const uint8_t *ext_capa;
	uint8_t interworking[3];

	ext_capa = wiphy_get_extended_capabilities(sc->wiphy,
							NL80211_IFTYPE_STATION);
	/*
	 * If adding IE's here ensure that ordering is not broken for
	 * probe requests (IEEE Std 802.11-2016 Table 9-33).
	 */
	/* Order 9 - Extended Capabilities */
	iov[iov_elems].iov_base = (void *) ext_capa;
	iov[iov_elems].iov_len = ext_capa[1] + 2;
	iov_elems++;

	if (test_bit(&ext_capa[2 + 3], 7)) {
		/* Order 12 - Interworking */
		interworking[0] = IE_TYPE_INTERWORKING;
		interworking[1] = 1;
		/* Private network, INet=0,ASRA=0,ESR=0,UESA=0 */
		interworking[2] = 0;

		iov[iov_elems].iov_base = interworking;
		iov[iov_elems].iov_len = 3;
		iov_elems++;
	}

	/* Order Last (assuming WSC vendor specific) */
	if (params->extra_ie && params->extra_ie_size) {
		iov[iov_elems].iov_base = (void *) params->extra_ie;
		iov[iov_elems].iov_len = params->extra_ie_size;
		iov_elems++;
	}

	l_genl_msg_append_attrv(msg, NL80211_ATTR_IE, iov, iov_elems);
}

static bool scan_mac_address_randomization_is_disabled(void)
{
	const struct l_settings *config = iwd_get_config();
	bool disabled;

	if (!l_settings_get_bool(config, "Scan",
					"DisableMacAddressRandomization",
					&disabled))
		return false;

	return disabled;
}

static struct l_genl_msg *scan_build_cmd(struct scan_context *sc,
					bool ignore_flush_flag, bool is_passive,
					const struct scan_parameters *params)
{
	struct l_genl_msg *msg;
	uint32_t flags = 0;

	msg = l_genl_msg_new(NL80211_CMD_TRIGGER_SCAN);

	l_genl_msg_append_attr(msg, NL80211_ATTR_WDEV, 8, &sc->wdev_id);

	if (wiphy_get_max_scan_ie_len(sc->wiphy))
		scan_build_attr_ie(msg, sc, params);

	if (params->freqs)
		scan_build_attr_scan_frequencies(msg, params->freqs);

	if (params->flush && !ignore_flush_flag)
		flags |= NL80211_SCAN_FLAG_FLUSH;

	if (!is_passive && params->randomize_mac_addr_hint &&
			wiphy_can_randomize_mac_addr(sc->wiphy) &&
				!scan_mac_address_randomization_is_disabled())
		/*
		 * Randomizing 46 bits (locally administered 1 and multicast 0
		 * is assumed).
		 */
		flags |= NL80211_SCAN_FLAG_RANDOM_ADDR;

	if (!is_passive && params->source_mac &&
			wiphy_can_randomize_mac_addr(sc->wiphy)) {
		static const uint8_t mask[6] =	/* No random bits */
			{ 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

		flags |= NL80211_SCAN_FLAG_RANDOM_ADDR;
		l_genl_msg_append_attr(msg, NL80211_ATTR_MAC, 6,
					params->source_mac);
		l_genl_msg_append_attr(msg, NL80211_ATTR_MAC_MASK, 6,
					mask);
	}

	if (!is_passive && wiphy_has_ext_feature(sc->wiphy,
					NL80211_EXT_FEATURE_SCAN_RANDOM_SN))
		flags |= NL80211_SCAN_FLAG_RANDOM_SN;

	if (flags)
		l_genl_msg_append_attr(msg, NL80211_ATTR_SCAN_FLAGS, 4, &flags);

	if (params->no_cck_rates) {
		static const uint8_t b_rates[] = { 2, 4, 11, 22 };
		uint8_t *scan_rates;
		const uint8_t *supported;
		unsigned int num_supported;
		unsigned int count;
		unsigned int i;

		l_genl_msg_append_attr(msg, NL80211_ATTR_TX_NO_CCK_RATE, 0,
					NULL);

		/*
		 * Assume if we're sending the probe requests at OFDM bit
		 * rates we don't want to advertise support for 802.11b rates.
		 */
		if (L_WARN_ON(!(supported = wiphy_get_supported_rates(sc->wiphy,
							NL80211_BAND_2GHZ,
							&num_supported))))
			goto done;

		scan_rates = l_malloc(num_supported);

		for (count = 0, i = 0; i < num_supported; i++)
			if (!memchr(b_rates, supported[i],
						L_ARRAY_SIZE(b_rates)))
				scan_rates[count++] = supported[i];

		if (L_WARN_ON(!count)) {
			l_free(scan_rates);
			goto done;
		}

		l_genl_msg_enter_nested(msg, NL80211_ATTR_SCAN_SUPP_RATES);
		l_genl_msg_append_attr(msg, NL80211_BAND_2GHZ,
							count, scan_rates);
		l_genl_msg_leave_nested(msg);
		l_free(scan_rates);
	}

	if (wiphy_has_ext_feature(sc->wiphy,
					NL80211_EXT_FEATURE_SET_SCAN_DWELL)) {
		if (params->duration)
			l_genl_msg_append_attr(msg,
					NL80211_ATTR_MEASUREMENT_DURATION,
					2, &params->duration);

		if (params->duration_mandatory)
			l_genl_msg_append_attr(msg,
				NL80211_ATTR_MEASUREMENT_DURATION_MANDATORY,
				0, NULL);
	}

done:
	return msg;
}

struct l_genl_msg *scan_build_trigger_scan_bss(uint32_t ifindex,
						struct wiphy *wiphy,
						uint32_t frequency,
						const uint8_t *ssid,
						uint32_t ssid_len)
{
	struct l_genl_msg *msg = l_genl_msg_new(NL80211_CMD_TRIGGER_SCAN);
	uint32_t flags = 0;

	l_genl_msg_append_attr(msg, NL80211_ATTR_IFINDEX, 4, &ifindex);

	l_genl_msg_enter_nested(msg, NL80211_ATTR_SCAN_FREQUENCIES);
	l_genl_msg_append_attr(msg, 0, 4, &frequency);
	l_genl_msg_leave_nested(msg);

	if (wiphy_has_ext_feature(wiphy, NL80211_EXT_FEATURE_SCAN_RANDOM_SN))
		flags |= NL80211_SCAN_FLAG_RANDOM_SN;

	if (flags)
		l_genl_msg_append_attr(msg, NL80211_ATTR_SCAN_FLAGS, 4, &flags);

	/* direct probe request scan */
	l_genl_msg_enter_nested(msg, NL80211_ATTR_SCAN_SSIDS);
	l_genl_msg_append_attr(msg, 0, ssid_len, ssid);
	l_genl_msg_leave_nested(msg);

	return msg;
}

struct scan_cmds_add_data {
	struct scan_context *sc;
	const struct scan_parameters *params;
	struct l_queue *cmds;
	struct l_genl_msg **cmd;
	uint8_t max_ssids_per_scan;
	uint8_t num_ssids_can_append;
};

static bool scan_cmds_add_hidden(const struct network_info *network,
					void *user_data)
{
	struct scan_cmds_add_data *data = user_data;

	if (!network->config.is_hidden)
		return true;

	l_genl_msg_append_attr(*data->cmd, NL80211_ATTR_SSID,
				strlen(network->ssid), network->ssid);
	data->num_ssids_can_append--;

	if (!data->num_ssids_can_append) {
		l_genl_msg_leave_nested(*data->cmd);
		l_queue_push_tail(data->cmds, *data->cmd);

		data->num_ssids_can_append = data->max_ssids_per_scan;

		/*
		 * Create a consecutive scan trigger in the batch of scans.
		 * The 'flush' flag is ignored, this allows to get the results
		 * of all scans in the batch after the last scan is finished.
		 */
		*data->cmd = scan_build_cmd(data->sc, true, false,
								data->params);
		l_genl_msg_enter_nested(*data->cmd, NL80211_ATTR_SCAN_SSIDS);
	}

	return true;
}

static void scan_cmds_add(struct l_queue *cmds, struct scan_context *sc,
				bool passive,
				const struct scan_parameters *params)
{
	struct l_genl_msg *cmd;
	struct scan_cmds_add_data data = {
		sc,
		params,
		cmds,
		&cmd,
		wiphy_get_max_num_ssids_per_scan(sc->wiphy),
	};

	cmd = scan_build_cmd(sc, false, passive, params);

	if (passive) {
		/* passive scan */
		l_queue_push_tail(cmds, cmd);
		return;
	}

	l_genl_msg_enter_nested(cmd, NL80211_ATTR_SCAN_SSIDS);

	if (params->ssid) {
		/* direct probe request scan */
		l_genl_msg_append_attr(cmd, NL80211_ATTR_SSID,
					params->ssid_len, params->ssid);
		l_genl_msg_leave_nested(cmd);

		l_queue_push_tail(cmds, cmd);
		return;
	}

	data.num_ssids_can_append = data.max_ssids_per_scan;
	known_networks_foreach(scan_cmds_add_hidden, &data);

	l_genl_msg_append_attr(cmd, NL80211_ATTR_SSID, 0, NULL);
	l_genl_msg_leave_nested(cmd);
	l_queue_push_tail(cmds, cmd);
}

static int scan_request_send_trigger(struct scan_context *sc,
					struct scan_request *sr)
{
	struct l_genl_msg *cmd = l_queue_peek_head(sr->cmds);

	if (!cmd)
		return -ENOMSG;

	sc->start_cmd_id = l_genl_family_send(nl80211, cmd,
						scan_request_triggered, sc,
									NULL);
	if (sc->start_cmd_id) {
		l_genl_msg_ref(cmd);

		return 0;
	}

	l_error("Scan request: failed to trigger scan.");

	return -EIO;
}

static const struct wiphy_radio_work_item_ops work_ops = {
	.do_work = start_next_scan_request,
	.destroy = scan_request_free,
};

static struct scan_request *scan_request_new(struct scan_context *sc,
						bool passive,
						scan_trigger_func_t trigger,
						scan_notify_func_t notify,
						void *userdata,
						scan_destroy_func_t destroy)
{
	struct scan_request *sr;

	sr = l_new(struct scan_request, 1);
	sr->sc = sc;
	sr->trigger = trigger;
	sr->callback = notify;
	sr->userdata = userdata;
	sr->destroy = destroy;
	sr->passive = passive;
	sr->cmds = l_queue_new();

	return sr;
}

static uint32_t scan_common(uint64_t wdev_id, bool passive,
				const struct scan_parameters *params,
				int priority,
				scan_trigger_func_t trigger,
				scan_notify_func_t notify, void *userdata,
				scan_destroy_func_t destroy)
{
	struct scan_context *sc;
	struct scan_request *sr;

	sc = l_queue_find(scan_contexts, scan_context_match, &wdev_id);

	if (!sc)
		return 0;

	sr = scan_request_new(sc, passive, trigger, notify, userdata, destroy);

	scan_cmds_add(sr->cmds, sc, passive, params);

	l_queue_push_tail(sc->requests, sr);

	return wiphy_radio_work_insert(sc->wiphy, &sr->work,
					priority, &work_ops);
}

uint32_t scan_passive(uint64_t wdev_id, struct scan_freq_set *freqs,
			scan_trigger_func_t trigger, scan_notify_func_t notify,
			void *userdata, scan_destroy_func_t destroy)
{
	struct scan_parameters params = { .freqs = freqs };

	return scan_common(wdev_id, true, &params, WIPHY_WORK_PRIORITY_SCAN,
				trigger, notify, userdata, destroy);
}

uint32_t scan_passive_full(uint64_t wdev_id,
			const struct scan_parameters *params,
			scan_trigger_func_t trigger,
			scan_notify_func_t notify, void *userdata,
			scan_destroy_func_t destroy)
{
	return scan_common(wdev_id, true, params, WIPHY_WORK_PRIORITY_SCAN,
				trigger, notify, userdata, destroy);
}

uint32_t scan_active(uint64_t wdev_id, uint8_t *extra_ie, size_t extra_ie_size,
			scan_trigger_func_t trigger,
			scan_notify_func_t notify, void *userdata,
			scan_destroy_func_t destroy)
{
	struct scan_parameters params = {};

	params.extra_ie = extra_ie;
	params.extra_ie_size = extra_ie_size;

	return scan_common(wdev_id, false, &params, WIPHY_WORK_PRIORITY_SCAN,
					trigger, notify, userdata, destroy);
}

uint32_t scan_active_full(uint64_t wdev_id,
			const struct scan_parameters *params,
			scan_trigger_func_t trigger, scan_notify_func_t notify,
			void *userdata, scan_destroy_func_t destroy)
{
	return scan_common(wdev_id, false, params, WIPHY_WORK_PRIORITY_SCAN,
					trigger, notify, userdata, destroy);
}

static void scan_add_owe_freq(struct scan_freq_set *freqs,
				const struct scan_bss *bss)
{
	int freq;

	if (bss->owe_trans->oper_class)
		freq = oci_to_frequency(bss->owe_trans->oper_class,
					bss->owe_trans->channel);
	else
		freq = bss->frequency;

	L_WARN_ON(freq < 0);

	scan_freq_set_add(freqs, freq);
}

static void add_owe_scan_cmd(struct scan_context *sc, struct scan_request *sr,
				bool ignore_flush,
				struct scan_freq_set *freqs,
				const struct scan_bss *bss)
{
	struct l_genl_msg *cmd;
	struct scan_parameters params = {};
	struct scan_freq_set *tmp;

	if (!freqs) {
		tmp = scan_freq_set_new();

		scan_add_owe_freq(tmp, bss);

		params.freqs = tmp;
	} else
		params.freqs = freqs;

	params.ssid = bss->owe_trans->ssid;
	params.ssid_len = bss->owe_trans->ssid_len;
	params.flush = true;

	cmd = scan_build_cmd(sc, ignore_flush, false, &params);

	l_genl_msg_enter_nested(cmd, NL80211_ATTR_SCAN_SSIDS);
	l_genl_msg_append_attr(cmd, 0, params.ssid_len, params.ssid);
	l_genl_msg_leave_nested(cmd);

	l_queue_push_tail(sr->cmds, cmd);

	if (!freqs)
		scan_freq_set_free(tmp);
}

uint32_t scan_owe_hidden(uint64_t wdev_id, struct l_queue *list,
			scan_trigger_func_t trigger, scan_notify_func_t notify,
			void *userdata, scan_destroy_func_t destroy)
{
	struct scan_context *sc;
	struct scan_request *sr;
	struct scan_freq_set *freqs;
	const struct l_queue_entry *entry;
	const uint8_t *ssid = NULL;
	size_t ssid_len;
	bool same_ssid = true;
	struct scan_bss *bss;
	bool ignore_flush = false;

	sc = l_queue_find(scan_contexts, scan_context_match, &wdev_id);

	if (!sc)
		return 0;

	sr = scan_request_new(sc, false, trigger, notify, userdata, destroy);

	freqs = scan_freq_set_new();

	/*
	 * Start building up a frequency list if all SSIDs are the same. This
	 * is hopefully the common case and will allow a single scan command.
	 */
	for (entry = l_queue_get_entries(list); entry; entry = entry->next) {
		bss = entry->data;

		scan_add_owe_freq(freqs, bss);

		/* First */
		if (!ssid) {
			ssid = bss->owe_trans->ssid;
			ssid_len = bss->owe_trans->ssid_len;
			continue;
		}

		if (ssid_len == bss->owe_trans->ssid_len &&
				!memcmp(ssid, bss->owe_trans->ssid,
				bss->owe_trans->ssid_len))
			continue;

		same_ssid = false;
		break;
	}

	if (same_ssid) {
		bss = l_queue_peek_head(list);

		add_owe_scan_cmd(sc, sr, ignore_flush, freqs, bss);

		scan_freq_set_free(freqs);

		goto done;
	}

	scan_freq_set_free(freqs);

	/* SSIDs differed, use separate scan commands. */
	for (entry = l_queue_get_entries(list); entry; entry = entry->next) {
		bss = entry->data;

		add_owe_scan_cmd(sc, sr, ignore_flush, NULL, bss);

		/* Ignore flush on all subsequent commands */
		if (!ignore_flush)
			ignore_flush = true;
	}

done:
	l_queue_push_tail(sc->requests, sr);

	return wiphy_radio_work_insert(sc->wiphy, &sr->work,
					WIPHY_WORK_PRIORITY_SCAN, &work_ops);
}

bool scan_active_is_enabled(void)
{
	const struct l_settings *config = iwd_get_config();
	bool enabled;

	if (!l_settings_get_bool(config, "Scan", "EnableActiveScanning",
								&enabled))
		return false;

	return enabled;
}

bool scan_cancel(uint64_t wdev_id, uint32_t id)
{
	struct scan_context *sc;
	struct scan_request *sr;

	l_debug("Trying to cancel scan id %u for wdev %" PRIx64, id, wdev_id);

	sc = l_queue_find(scan_contexts, scan_context_match, &wdev_id);
	if (!sc)
		return false;

	sr = l_queue_find(sc->requests, scan_request_match, L_UINT_TO_PTR(id));
	if (!sr)
		return false;

	/* We're in the callback and about to be removed, invoke destroy now */
	if (sr->in_callback)
		goto call_destroy;

	/* If already triggered, just zero out the callback */
	if (sr->triggered) {
		l_debug("Scan has been triggered, wait for it to complete");

		sr->callback = NULL;
		goto call_destroy;
	}

	/*
	 * Takes care of the following cases:
	 * 1. If TRIGGER_SCAN is in flight
	 * 2. TRIGGER_SCAN sent but bounced with -EBUSY
	 * 3. Scan request is done but GET_SCAN is still pending
	 *
	 * For case 3, we can easily cancel the command and proceed with the
	 * other pending requests.  For case 1 & 2, the subsequent pending
	 * request might bounce off with an -EBUSY.
	 */
	if (wiphy_radio_work_is_running(sc->wiphy, sr->work.id)) {
		l_debug("Scan is already started");

		/* l_genl_family_cancel will trigger destroy callbacks */
		sr->canceled = true;

		if (sc->start_cmd_id)
			l_genl_family_cancel(nl80211, sc->start_cmd_id);

		if (sc->get_scan_cmd_id)
			l_genl_family_cancel(nl80211, sc->get_scan_cmd_id);

		sc->start_cmd_id = 0;
		sc->get_scan_cmd_id = 0;
	}

	l_queue_remove(sc->requests, sr);
	wiphy_radio_work_done(sc->wiphy, sr->work.id);

	return true;

call_destroy:
	if (sr->destroy) {
		sr->destroy(sr->userdata);
		sr->destroy = NULL;
	}

	return true;
}

static void scan_periodic_triggered(int err, void *user_data)
{
	struct scan_context *sc = user_data;

	if (err) {
		scan_periodic_rearm(sc);
		return;
	}

	l_debug("Periodic scan triggered for wdev %" PRIx64, sc->wdev_id);

	if (sc->sp.trigger)
		sc->sp.trigger(0, sc->sp.userdata);
}

static bool scan_periodic_notify(int err, struct l_queue *bss_list,
					const struct scan_freq_set *freqs,
					void *user_data)
{
	struct scan_context *sc = user_data;

	scan_periodic_rearm(sc);

	if (sc->sp.callback)
		return sc->sp.callback(err, bss_list, freqs, sc->sp.userdata);

	return false;
}

static void scan_periodic_destroy(void *user_data)
{
	struct scan_context *sc = user_data;

	sc->sp.id = 0;
}

static bool scan_periodic_queue(struct scan_context *sc)
{
	struct scan_parameters params = {};
	struct scan_request *sr;

	if (scan_active_is_enabled() || (sc->sp.needs_active_scan && known_networks_has_hidden())) {
		params.randomize_mac_addr_hint = true;

		sc->sp.needs_active_scan = false;
		sc->sp.id = scan_common(sc->wdev_id, false, &params,
					WIPHY_WORK_PRIORITY_PERIODIC_SCAN,
					scan_periodic_triggered,
					scan_periodic_notify, sc,
					scan_periodic_destroy);
	} else
		sc->sp.id = scan_common(sc->wdev_id, true, &params,
					WIPHY_WORK_PRIORITY_PERIODIC_SCAN,
					scan_periodic_triggered,
					scan_periodic_notify, sc,
					scan_periodic_destroy);

	if (!sc->sp.id)
		return false;

	sr = l_queue_peek_tail(sc->requests);
	sr->periodic = true;

	return true;
}

static bool scan_periodic_is_disabled(void)
{
	const struct l_settings *config = iwd_get_config();
	bool disabled;

	if (!l_settings_get_bool(config, "Scan", "DisablePeriodicScan",
								&disabled))
		return false;

	return disabled;
}

void scan_periodic_start(uint64_t wdev_id, scan_trigger_func_t trigger,
				scan_notify_func_t func, void *userdata)
{
	struct scan_context *sc;

	if (scan_periodic_is_disabled())
		return;

	sc = l_queue_find(scan_contexts, scan_context_match, &wdev_id);

	if (!sc) {
		l_error("%s called without scan_wdev_add", __func__);
		return;
	}

	if (sc->sp.interval)
		return;

	l_debug("Starting periodic scan for wdev %" PRIx64, wdev_id);

	sc->sp.interval = SCAN_INIT_INTERVAL;
	sc->sp.trigger = trigger;
	sc->sp.callback = func;
	sc->sp.userdata = userdata;

	/* If nothing queued, start the first periodic scan */
	scan_periodic_queue(sc);
}

bool scan_periodic_stop(uint64_t wdev_id)
{
	struct scan_context *sc;

	sc = l_queue_find(scan_contexts, scan_context_match, &wdev_id);

	if (!sc)
		return false;

	if (!sc->sp.interval)
		return false;

	l_debug("Stopping periodic scan for wdev %" PRIx64, wdev_id);

	if (sc->sp.timeout)
		l_timeout_remove(sc->sp.timeout);

	if (sc->sp.id) {
		scan_cancel(wdev_id, sc->sp.id);
		sc->sp.id = 0;
	}

	sc->sp.interval = 0;
	sc->sp.trigger = NULL;
	sc->sp.callback = NULL;
	sc->sp.userdata = NULL;
	sc->sp.needs_active_scan = false;

	return true;
}

uint64_t scan_get_triggered_time(uint64_t wdev_id, uint32_t id)
{
	struct scan_context *sc;
	struct scan_request *sr;

	sc = l_queue_find(scan_contexts, scan_context_match, &wdev_id);
	if (!sc)
		return 0;

	sr = l_queue_find(sc->requests, scan_request_match, L_UINT_TO_PTR(id));
	if (!sr || !sr->triggered)
		return 0;

	return sr->start_time_tsf;
}

static void scan_periodic_timeout(struct l_timeout *timeout, void *user_data)
{
	struct scan_context *sc = user_data;

	l_debug("%" PRIx64, sc->wdev_id);

	/*
	 * Timeout triggered before periodic scan could even start, just rearm
	 * with the same interval.
	 */
	if (sc->sp.id) {
		l_debug("Periodic scan timer called before scan could start!");
		scan_periodic_rearm(sc);
		return;
	}

	sc->sp.interval *= 2;
	if (sc->sp.interval > SCAN_MAX_INTERVAL)
		sc->sp.interval = SCAN_MAX_INTERVAL;

	scan_periodic_queue(sc);
}

static void scan_periodic_timeout_destroy(void *user_data)
{
	struct scan_context *sc = user_data;

	sc->sp.timeout = NULL;
}

static void scan_periodic_rearm(struct scan_context *sc)
{
	l_debug("Arming periodic scan timer: %u", sc->sp.interval);

	if (sc->sp.timeout)
		l_timeout_modify(sc->sp.timeout, sc->sp.interval);
	else
		sc->sp.timeout = l_timeout_create(sc->sp.interval,
						scan_periodic_timeout, sc,
						scan_periodic_timeout_destroy);
}

static bool start_next_scan_request(struct wiphy_radio_work_item *item)
{
	struct scan_request *sr = l_container_of(item,
						struct scan_request, work);
	struct scan_context *sc = sr->sc;

	if (sc->state != SCAN_STATE_NOT_RUNNING)
		return false;

	if (!scan_request_send_trigger(sc, sr))
		return false;

	scan_request_failed(sc, sr, -EIO);

	return true;
}

static void scan_parse_vendor_specific(struct scan_bss *bss, const void *data,
					uint16_t len)
{
	uint16_t cost_level;
	uint16_t cost_flags;
	bool dgaf_disable;

	if (!bss->wpa && is_ie_wpa_ie(data, len)) {
		bss->wpa = l_memdup(data - 2, len + 2);
		return;
	}

	if (!bss->osen && is_ie_wfa_ie(data, len, IE_WFA_OI_OSEN)) {
		bss->osen = l_memdup(data - 2, len + 2);
		return;
	}

	if (is_ie_wfa_ie(data, len, IE_WFA_OI_HS20_INDICATION)) {
		if (ie_parse_hs20_indication_from_data(data - 2, len + 2,
					&bss->hs20_version, NULL, NULL,
					&dgaf_disable) < 0)
			return;

		bss->hs20_dgaf_disable = dgaf_disable;
		bss->hs20_capable = true;
		return;
	}

	if (is_ie_wfa_ie(data, len, IE_WFA_OI_OWE_TRANSITION)) {
		_auto_(l_free) struct ie_owe_transition_info *owe_trans =
				l_new(struct ie_owe_transition_info, 1);

		if (ie_parse_owe_transition(data - 2, len + 2, owe_trans) < 0)
			return;

		if (owe_trans->oper_class &&
				oci_to_frequency(owe_trans->oper_class,
						owe_trans->channel) < 0)
			return;

		bss->owe_trans = l_steal_ptr(owe_trans);
		return;
	}

	if (is_ie_wfa_ie(data, len, IE_WFA_OI_CONFIGURATOR_CONNECTIVITY))
		bss->dpp_configurator = true;

	if (!ie_parse_network_cost(data, len, &cost_level, &cost_flags)) {
		bss->cost_level = cost_level;
		bss->cost_flags = cost_flags;
		return;
	}

	if (is_ie_default_sae_group_oui(data, len))
		bss->force_default_sae_group = true;
}

/*
 * Fully parses the Advertisement Protocol Element. The only thing being looked
 * for is the ANQP protocol ID, but this could be buried behind several other
 * advertisement tuples so the entire IE may need to be parsed.
 */
static bool scan_parse_advertisement_protocol(struct scan_bss *bss,
						const void *data, uint16_t len)
{
	const uint8_t *ptr = data;

	l_debug("");

	while (len) {
		/*
		 * TODO: Store query info for GAS response length verification
		 */
		uint8_t id = ptr[1];

		switch (id) {
		/*
		 * IEEE 802.11-2016 Section 11.25.3.3.1
		 *
		 * "A non-AP STA shall not transmit an ANQP request to
		 * an AP for any ANQP-element unless the ANQP
		 * Advertisement Protocol ID is included..."
		 */
		case IE_ADVERTISEMENT_ANQP:
			bss->anqp_capable = true;
			return true;
		case IE_ADVERTISEMENT_MIH_SERVICE:
		case IE_ADVERTISEMENT_MIH_DISCOVERY:
		case IE_ADVERTISEMENT_EAS:
		case IE_ADVERTISEMENT_RLQP:
			len -= 2;
			ptr += 2;
			break;
		case IE_ADVERTISEMENT_VENDOR_SPECIFIC:
			/* IEEE 802.11-2016 Section 9.4.2.26 */
			len -= ptr[3];
			ptr += ptr[3];
			break;
		default:
			return false;
		}
	}

	return true;
}

static bool scan_parse_bss_information_elements(struct scan_bss *bss,
					const void *data, uint16_t len)
{
	struct ie_tlv_iter iter;
	bool have_ssid = false;

	ie_tlv_iter_init(&iter, data, len);

	while (ie_tlv_iter_next(&iter)) {
		uint8_t tag = ie_tlv_iter_get_tag(&iter);

		switch (tag) {
		case IE_TYPE_SSID:
			if (iter.len > 32)
				return false;

			memcpy(bss->ssid, iter.data, iter.len);
			bss->ssid_len = iter.len;
			have_ssid = true;
			break;
		case IE_TYPE_RSN:
			if (!bss->rsne)
				bss->rsne = l_memdup(iter.data - 2,
								iter.len + 2);
			break;
		case IE_TYPE_RSNX:
			if (!bss->rsnxe)
				bss->rsnxe = l_memdup(iter.data - 2,
								iter.len + 2);
			break;
		case IE_TYPE_BSS_LOAD:
			if (ie_parse_bss_load(&iter, NULL, &bss->utilization,
						NULL) < 0)
				l_warn("Unable to parse BSS Load IE for "
					MAC, MAC_STR(bss->addr));
			else
				l_debug("Load: %u/255", bss->utilization);

			break;
		case IE_TYPE_VENDOR_SPECIFIC:
			scan_parse_vendor_specific(bss, iter.data, iter.len);
			break;
		case IE_TYPE_MOBILITY_DOMAIN:
			if (!bss->mde_present && iter.len == 3) {
				memcpy(bss->mde, iter.data, iter.len);
				bss->mde_present = true;
			}

			break;
		case IE_TYPE_RM_ENABLED_CAPABILITIES:
			if (iter.len != 5)
				break;

			/* Only interested in Neighbor Reports */

			bss->cap_rm_neighbor_report =
				(iter.data[0] & IE_RM_CAP_NEIGHBOR_REPORT) > 0;
			break;
		case IE_TYPE_COUNTRY:
			if (bss->cc_present || iter.len < 6)
				break;

			bss->cc[0] = iter.data[0];
			bss->cc[1] = iter.data[1];
			bss->cc[2] = iter.data[2];
			bss->cc_present = true;

			break;
		case IE_TYPE_HT_CAPABILITIES:
			bss->ht_capable = true;
			break;
		case IE_TYPE_VHT_CAPABILITIES:
			bss->vht_capable = true;
			break;
		case IE_TYPE_ADVERTISEMENT_PROTOCOL:
			if (iter.len < 2)
				return false;

			scan_parse_advertisement_protocol(bss, iter.data,
								iter.len);
			break;
		case IE_TYPE_INTERWORKING:
			/*
			 * No bits indicate if venue/HESSID is included, so the
			 * length is the only way to know.
			 * (IEEE 802.11-2016 - Figure 9-439)
			 */
			if (iter.len == 9)
				memcpy(bss->hessid, iter.data + 3, 6);
			else if (iter.len == 7)
				memcpy(bss->hessid, iter.data + 1, 6);
			break;
		case IE_TYPE_ROAMING_CONSORTIUM:
			if (iter.len < 2)
				return false;

			bss->rc_ie = l_memdup(iter.data - 2, iter.len + 2);

			break;

		case IE_TYPE_EXTENDED_CAPABILITIES:
			/* 802.11-2020 9.4.2.26
			 *
			 * "The length of the Extended Capabilities field is
			 * variable. If fewer bits are received in an Extended
			 * Capabilities field than shown in Table 9-153, the
			 * rest of the Extended Capabilities field bits are
			 * assumed to be zero"
			 *
			 * Currently only Proxy ARP bit (12) is checked, and if
			 * not found, this is not a fatal error.
			 */
			if (iter.len < 2)
				break;

			bss->proxy_arp = test_bit(iter.data, 12);
		}
	}

	bss->wsc = ie_tlv_extract_wsc_payload(data, len, &bss->wsc_size);

	switch (bss->source_frame) {
	case SCAN_BSS_PROBE_RESP:
		bss->p2p_probe_resp_info = l_new(struct p2p_probe_resp, 1);

		if (p2p_parse_probe_resp(data, len, bss->p2p_probe_resp_info) ==
				0)
			break;

		l_free(bss->p2p_probe_resp_info);
		bss->p2p_probe_resp_info = NULL;
		break;
	case SCAN_BSS_PROBE_REQ:
		bss->p2p_probe_req_info = l_new(struct p2p_probe_req, 1);

		if (p2p_parse_probe_req(data, len, bss->p2p_probe_req_info) ==
				0)
			break;

		l_free(bss->p2p_probe_req_info);
		bss->p2p_probe_req_info = NULL;
		break;
	case SCAN_BSS_BEACON:
	{
		/*
		 * Beacon and Probe Response P2P IE subelement formats are
		 * mutually incompatible and can help us distinguish one frame
		 * subtype from the other if the driver is not exposing enough
		 * information.  As a result of trusting the frame contents on
		 * this, no critical code should depend on the
		 * bss->source_frame information being right.
		 */
		struct p2p_beacon info;
		int r;

		r = p2p_parse_beacon(data, len, &info);
		if (r == 0) {
			bss->p2p_beacon_info = l_memdup(&info, sizeof(info));
			break;
		}

		if (r == -ENOENT)
			break;

		bss->p2p_probe_resp_info = l_new(struct p2p_probe_resp, 1);

		if (p2p_parse_probe_resp(data, len, bss->p2p_probe_resp_info) ==
				0) {
			bss->source_frame = SCAN_BSS_PROBE_RESP;
			break;
		}

		l_free(bss->p2p_probe_resp_info);
		bss->p2p_probe_resp_info = NULL;
		break;
	}
	}

	bss->wfd = ie_tlv_extract_wfd_payload(data, len, &bss->wfd_size);

	return have_ssid;
}

/*
 * Maps 0..100 values to -10000..0
 *
 * This isn't really mapping to mBm since the input is unit-less and we have no
 * idea what the driver itself does to come up with this 'strength' value but
 * this is really the best that can be done for these drivers (its only 4 in
 * tree drivers after all).
 */
static int32_t signal_unspec_to_mbm(uint8_t strength)
{
	if (L_WARN_ON(strength > 100))
		return 0;

	return ((int32_t)strength * 100) - 10000;
}

static struct scan_bss *scan_parse_attr_bss(struct l_genl_attr *attr,
						struct wiphy *wiphy,
						uint32_t *out_seen_ms_ago)
{
	uint16_t type, len;
	const void *data;
	struct scan_bss *bss;
	const uint8_t *ies = NULL;
	size_t ies_len;
	const uint8_t *beacon_ies = NULL;
	size_t beacon_ies_len;

	bss = l_new(struct scan_bss, 1);
	bss->utilization = 127;
	bss->source_frame = SCAN_BSS_BEACON;

	while (l_genl_attr_next(attr, &type, &len, &data)) {
		switch (type) {
		case NL80211_BSS_BSSID:
			if (len != sizeof(bss->addr))
				goto fail;

			memcpy(bss->addr, data, len);
			break;
		case NL80211_BSS_CAPABILITY:
			if (len != sizeof(uint16_t))
				goto fail;

			bss->capability = *((uint16_t *) data);
			break;
		case NL80211_BSS_FREQUENCY:
			if (len != sizeof(uint32_t))
				goto fail;

			bss->frequency = *((uint32_t *) data);
			break;
		case NL80211_BSS_SIGNAL_MBM:
			if (len != sizeof(int32_t))
				goto fail;

			bss->signal_strength = *((int32_t *) data);
			break;
		case NL80211_BSS_SIGNAL_UNSPEC:
			if (len != 1)
				goto fail;

			bss->signal_strength =
					signal_unspec_to_mbm(l_get_u8(data));
			break;
		case NL80211_BSS_INFORMATION_ELEMENTS:
			ies = data;
			ies_len = len;
			break;
		case NL80211_BSS_PARENT_TSF:
			if (len != sizeof(uint64_t))
				goto fail;

			bss->parent_tsf = l_get_u64(data);
			break;
		case NL80211_BSS_PRESP_DATA:
			bss->source_frame = SCAN_BSS_PROBE_RESP;
			break;
		case NL80211_BSS_BEACON_IES:
			beacon_ies = data;
			beacon_ies_len = len;
			break;
		case NL80211_BSS_SEEN_MS_AGO:
			if (L_WARN_ON(len != sizeof(uint32_t)))
				break;

			*out_seen_ms_ago = l_get_u32(data);
			break;
		case NL80211_BSS_LAST_SEEN_BOOTTIME:
			if (L_WARN_ON(len != sizeof(uint64_t)))
				break;

			bss->time_stamp = l_get_u64(data) / L_NSEC_PER_USEC;
			break;
		}
	}

	/*
	 * Try our best at deciding whether the IEs come from a Probe
	 * Response based on the hints explained in nl80211.h
	 * (enum nl80211_bss).
	 */
	if (bss->source_frame == SCAN_BSS_BEACON && ies && (
				!beacon_ies ||
				ies_len != beacon_ies_len ||
				memcmp(ies, beacon_ies, ies_len)))
		bss->source_frame = SCAN_BSS_PROBE_RESP;

	/* Set data rate to something low, just in case estimation fails */
	bss->data_rate = 2000000;

	if (ies) {
		int ret;

		if (!scan_parse_bss_information_elements(bss, ies, ies_len))
			goto fail;

		ret = wiphy_estimate_data_rate(wiphy, ies, ies_len, bss,
						&bss->data_rate);
		if (ret < 0 && ret != -ENETUNREACH)
			l_warn("wiphy_estimate_data_rate() failed");
	}

	return bss;

fail:
	scan_bss_free(bss);
	return NULL;
}

static struct scan_freq_set *scan_parse_attr_scan_frequencies(
						struct l_genl_attr *attr)
{
	uint16_t type, len;
	const void *data;
	struct scan_freq_set *set;

	set = scan_freq_set_new();

	while (l_genl_attr_next(attr, &type, &len, &data)) {
		uint32_t freq;

		if (len != sizeof(uint32_t))
			continue;

		freq = *((uint32_t *) data);
		scan_freq_set_add(set, freq);
	}

	return set;
}

static struct scan_bss *scan_parse_result(struct l_genl_msg *msg,
						struct wiphy *wiphy,
						uint32_t *out_seen_ms_ago)
{
	struct l_genl_attr attr, nested;
	uint16_t type;
	struct scan_bss *bss = NULL;

	if (!l_genl_attr_init(&attr, msg))
		return NULL;

	while (l_genl_attr_next(&attr, &type, NULL, NULL)) {
		switch (type) {
		case NL80211_ATTR_BSS:
			if (!l_genl_attr_recurse(&attr, &nested))
				return NULL;

			bss = scan_parse_attr_bss(&nested, wiphy,
							out_seen_ms_ago);
			break;
		}
	}

	return bss;
}

static void scan_bss_compute_rank(struct scan_bss *bss)
{
	static const double RANK_HIGH_UTILIZATION_FACTOR = 0.8;
	static const double RANK_LOW_UTILIZATION_FACTOR = 1.2;
	double rank;
	uint32_t irank;
	/*
	 * Maximum rate is 2340Mbps (VHT)
	 */
	double max_rate = 2340000000;

	rank = (double)bss->data_rate / max_rate * USHRT_MAX;

	/* Prefer 5G networks over 2.4G */
	if (bss->frequency > 4000)
		rank *= RANK_5G_FACTOR;

	/* Rank loaded APs lower and lighly loaded APs higher */
	if (bss->utilization >= 192)
		rank *= RANK_HIGH_UTILIZATION_FACTOR;
	else if (bss->utilization <= 63)
		rank *= RANK_LOW_UTILIZATION_FACTOR;

	irank = rank;

	if (irank > USHRT_MAX)
		bss->rank = USHRT_MAX;
	else
		bss->rank = irank;
}

struct scan_bss *scan_bss_new_from_probe_req(const struct mmpdu_header *mpdu,
						const uint8_t *body,
						size_t body_len,
						uint32_t frequency, int rssi)

{
	struct scan_bss *bss;

	bss = l_new(struct scan_bss, 1);
	memcpy(bss->addr, mpdu->address_2, 6);
	bss->utilization = 127;
	bss->source_frame = SCAN_BSS_PROBE_REQ;
	bss->frequency = frequency;
	bss->signal_strength = rssi;

	if (!scan_parse_bss_information_elements(bss, body, body_len))
		goto fail;

	return bss;

fail:
	scan_bss_free(bss);
	return NULL;
}

void scan_bss_free(struct scan_bss *bss)
{
	l_free(bss->rsne);
	l_free(bss->rsnxe);
	l_free(bss->wpa);
	l_free(bss->wsc);
	l_free(bss->osen);
	l_free(bss->rc_ie);
	l_free(bss->wfd);
	l_free(bss->owe_trans);

	switch (bss->source_frame) {
	case SCAN_BSS_PROBE_RESP:
		if (!bss->p2p_probe_resp_info)
			break;

		p2p_clear_probe_resp(bss->p2p_probe_resp_info);
		l_free(bss->p2p_probe_resp_info);
		break;
	case SCAN_BSS_PROBE_REQ:
		if (!bss->p2p_probe_req_info)
			break;

		p2p_clear_probe_req(bss->p2p_probe_req_info);
		l_free(bss->p2p_probe_req_info);
		break;
	case SCAN_BSS_BEACON:
		if (!bss->p2p_beacon_info)
			break;

		p2p_clear_beacon(bss->p2p_beacon_info);
		l_free(bss->p2p_beacon_info);
		break;
	}

	l_free(bss);
}

int scan_bss_get_rsn_info(const struct scan_bss *bss, struct ie_rsn_info *info)
{
	/*
	 * If both an RSN and a WPA elements are present currently
	 * RSN takes priority and the WPA IE is ignored.
	 */
	if (bss->rsne) {
		int res = ie_parse_rsne_from_data(bss->rsne, bss->rsne[1] + 2,
							info);
		if (res < 0) {
			l_debug("Cannot parse RSN field (%d, %s)",
					res, strerror(-res));
			return res;
		}
	} else if (bss->wpa) {
		int res = ie_parse_wpa_from_data(bss->wpa, bss->wpa[1] + 2,
							info);
		if (res < 0) {
			l_debug("Cannot parse WPA IE (%d, %s)",
					res, strerror(-res));
			return res;
		}
	} else if (bss->osen) {
		int res = ie_parse_osen_from_data(bss->osen, bss->osen[1] + 2,
							info);
		if (res < 0) {
			l_debug("Cannot parse OSEN IE (%d, %s)",
					res, strerror(-res));
			return res;
		}
	} else
		return -ENOENT;

	return 0;
}

int scan_bss_rank_compare(const void *a, const void *b, void *user_data)
{
	const struct scan_bss *new_bss = a, *bss = b;

	if (bss->rank == new_bss->rank)
		return (bss->signal_strength >
					new_bss->signal_strength) ? 1 : -1;

	return (bss->rank > new_bss->rank) ? 1 : -1;
}

static void get_scan_callback(struct l_genl_msg *msg, void *user_data)
{
	struct scan_results *results = user_data;
	struct scan_context *sc = results->sc;
	struct scan_bss *bss;
	uint64_t wdev_id;
	uint32_t seen_ms_ago = 0;

	l_debug("get_scan_callback");

	if (nl80211_parse_attrs(msg, NL80211_ATTR_WDEV, &wdev_id,
					NL80211_ATTR_UNSPEC) < 0)
		return;

	if (wdev_id != sc->wdev_id) {
		l_warn("wdev mismatch in get_scan_callback");
		return;
	}

	bss = scan_parse_result(msg, sc->wiphy, &seen_ms_ago);
	if (!bss)
		return;

	if (!bss->time_stamp)
		bss->time_stamp = results->time_stamp -
					seen_ms_ago * L_USEC_PER_MSEC;

	scan_bss_compute_rank(bss);
	l_queue_insert(results->bss_list, bss, scan_bss_rank_compare, NULL);
}

static void discover_hidden_network_bsses(struct scan_context *sc,
						struct l_queue *bss_list)
{
	const struct l_queue_entry *bss_entry;

	for (bss_entry = l_queue_get_entries(bss_list); bss_entry;
						bss_entry = bss_entry->next) {
		struct scan_bss *bss = bss_entry->data;

		if (!util_ssid_is_hidden(bss->ssid_len, bss->ssid))
			continue;

		sc->sp.needs_active_scan = true;
	}
}

static void scan_finished(struct scan_context *sc,
				int err, struct l_queue *bss_list,
				const struct scan_freq_set *freqs,
				struct scan_request *sr)
{
	bool new_owner = false;
	scan_notify_func_t callback = sr ? sr->callback : sc->sp.callback;
	void *userdata = sr ? sr->userdata : sc->sp.userdata;

	if (bss_list)
		discover_hidden_network_bsses(sc, bss_list);

	if (sr)
		sr->in_callback = true;

	if (callback)
		new_owner = callback(err, bss_list, freqs, userdata);

	if (bss_list && !new_owner)
		l_queue_destroy(bss_list,
				(l_queue_destroy_func_t) scan_bss_free);

	if (!sr)
		return;

	/*
	 * Can start a new scan now that we've removed this one from the
	 * queue.  If this were an external scan request (sr NULL) then the
	 * SCAN_FINISHED or SCAN_ABORTED handler would have taken care of
	 * sending the next command for a new or ongoing scan.
	 */
	sr->in_callback = false;
	l_queue_remove(sc->requests, sr);
	wiphy_radio_work_done(sc->wiphy, sr->work.id);
}

static void get_scan_done(void *user)
{
	struct scan_results *results = user;
	struct scan_context *sc = results->sc;

	l_debug("get_scan_done");

	sc->get_scan_cmd_id = 0;

	if (!results->sr || !results->sr->canceled)
		scan_finished(sc, 0, results->bss_list,
						results->freqs, results->sr);
	else
		l_queue_destroy(results->bss_list,
				(l_queue_destroy_func_t) scan_bss_free);

	if (results->freqs)
		scan_freq_set_free(results->freqs);

	l_free(results);
}

static bool scan_parse_flush_flag_from_msg(struct l_genl_msg *msg)
{
	struct l_genl_attr attr;
	uint16_t type, len;
	const void *data;

	if (!l_genl_attr_init(&attr, msg))
		return false;

	while (l_genl_attr_next(&attr, &type, &len, &data))
		if (type == NL80211_SCAN_FLAG_FLUSH)
			return true;

	return false;
}

static void scan_parse_new_scan_results(struct l_genl_msg *msg,
					struct scan_results *results)
{
	struct l_genl_attr attr, nested;
	uint16_t type, len;
	const void *data;

	if (!l_genl_attr_init(&attr, msg))
		return;

	while (l_genl_attr_next(&attr, &type, &len, &data)) {
		switch (type) {
		case NL80211_ATTR_SCAN_FREQUENCIES:
			if (!l_genl_attr_recurse(&attr, &nested)) {
				l_warn("Failed to parse ATTR_SCAN_FREQUENCIES");
				break;
			}

			results->freqs =
				scan_parse_attr_scan_frequencies(&nested);
			break;
		}
	}
}

static void scan_notify(struct l_genl_msg *msg, void *user_data)
{
	struct l_genl_attr attr;
	uint16_t type, len;
	const void *data;
	uint8_t cmd;
	uint64_t wdev_id;
	uint32_t wiphy_id;
	struct scan_context *sc;
	bool active_scan = false;
	uint64_t start_time_tsf = 0;
	struct scan_request *sr;

	cmd = l_genl_msg_get_command(msg);

	if (nl80211_parse_attrs(msg, NL80211_ATTR_WDEV, &wdev_id,
					NL80211_ATTR_WIPHY, &wiphy_id,
					NL80211_ATTR_UNSPEC) < 0)
		return;

	sc = l_queue_find(scan_contexts, scan_context_match, &wdev_id);
	if (!sc)
		return;

	l_debug("Scan notification %s(%u)", nl80211cmd_to_string(cmd), cmd);

	if (!l_genl_attr_init(&attr, msg))
		return;

	while (l_genl_attr_next(&attr, &type, &len, &data)) {
		switch (type) {
		case NL80211_ATTR_SCAN_SSIDS:
			active_scan = true;
			break;
		case NL80211_ATTR_SCAN_START_TIME_TSF:
			if (len != sizeof(uint64_t))
				return;

			start_time_tsf = l_get_u64(data);
			break;
		}
	}

	sr = l_queue_peek_head(sc->requests);

	switch (cmd) {
	case NL80211_CMD_NEW_SCAN_RESULTS:
	{
		struct l_genl_msg *scan_msg;
		struct scan_results *results;
		bool send_next = false;
		bool retry = false;
		bool get_results = false;

		sc->state = SCAN_STATE_NOT_RUNNING;

		/* Was this our own scan or an external scan */
		if (sr && sr->triggered) {
			sr->triggered = false;

			if (!sr->callback) {
				scan_finished(sc, -ECANCELED, NULL, NULL, sr);
				break;
			}

			/*
			 * If this was the last command for the current request
			 * avoid starting the next request until the GET_SCAN
			 * dump callback so that any current request is always
			 * at the top of the queue and handling is simpler.
			 */
			if (l_queue_isempty(sr->cmds))
				get_results = true;
			else
				send_next = true;
		} else {
			if (sc->get_scan_cmd_id)
				break;

			if (sc->sp.callback)
				get_results = true;

			/*
			 * Drop the ongoing scan if an external scan flushed
			 * our results.  Otherwise, try to retry the trigger
			 * request if it failed with an -EBUSY.
			 */
			if (sr && sr->started &&
					scan_parse_flush_flag_from_msg(msg))
				scan_finished(sc, -EAGAIN, NULL, NULL, sr);
			else
				retry = true;

			sr = NULL;
		}

		/*
		 * Send the next command of an ongoing request, or continue
		 * with a previously busy scan attempt due to an external
		 * scan.
		 */
		if (send_next || retry) {
			struct scan_request *next = l_queue_peek_head(
								sc->requests);

			if (next && wiphy_radio_work_is_running(sc->wiphy,
								next->work.id))
				start_next_scan_request(&next->work);
		}

		if (!get_results)
			break;

		results = l_new(struct scan_results, 1);
		results->sc = sc;
		results->time_stamp = l_time_now();
		results->sr = sr;
		results->bss_list = l_queue_new();

		scan_parse_new_scan_results(msg, results);

		scan_msg = l_genl_msg_new_sized(NL80211_CMD_GET_SCAN, 8);
		l_genl_msg_append_attr(scan_msg, NL80211_ATTR_WDEV, 8,
					&sc->wdev_id);
		sc->get_scan_cmd_id = l_genl_family_dump(nl80211, scan_msg,
							get_scan_callback,
							results, get_scan_done);

		break;
	}

	case NL80211_CMD_TRIGGER_SCAN:
		if (active_scan)
			sc->state = SCAN_STATE_ACTIVE;
		else
			sc->state = SCAN_STATE_PASSIVE;

		if (sr)
			sr->start_time_tsf = start_time_tsf;

		break;

	case NL80211_CMD_SCAN_ABORTED:
		sc->state = SCAN_STATE_NOT_RUNNING;

		/*
		 * If there's nothing pending, then most likely an external
		 * scan got aborted.  We don't care, ignore.
		 */
		if (!sr)
			break;

		if (sr->triggered) {
			sr->triggered = false;

			/* If periodic scan, don't report the abort */
			if (sr->periodic) {
				l_queue_remove(sc->requests, sr);
				wiphy_radio_work_done(sc->wiphy, sr->work.id);
			} else
				scan_finished(sc, -ECANCELED, NULL, NULL, sr);
		} else if (wiphy_radio_work_is_running(sc->wiphy,
							sr->work.id)) {
			/*
			 * If this was an external scan that got aborted
			 * we may be able to now queue our own scan although
			 * the abort could also have been triggered by the
			 * hardware or the driver because of another activity
			 * starting in which case we should just get an EBUSY.
			 */
			start_next_scan_request(&sr->work);
		}

		break;
	}
}

static void get_fw_scan_done(void *userdata)
{
	struct scan_results *results = userdata;
	struct scan_request *sr = results->sr;
	struct scan_context *sc = results->sc;
	int err = l_queue_length(results->bss_list) == 0 ? -ENOENT : 0;
	bool new_owner = false;

	sc->get_fw_scan_cmd_id = 0;

	if (sr->callback)
		new_owner = sr->callback(err, results->bss_list, NULL,
						sr->userdata);

	if (!new_owner)
		l_queue_destroy(results->bss_list,
				(l_queue_destroy_func_t) scan_bss_free);

	if (sr->destroy)
		sr->destroy(sr->userdata);

	l_free(sr);
	l_free(results);
}

bool scan_get_firmware_scan(uint64_t wdev_id, scan_notify_func_t notify,
				void *userdata, scan_destroy_func_t destroy)
{
	struct l_genl_msg *scan_msg;
	struct scan_results *results;
	struct scan_request *sr;
	struct scan_context *sc = l_queue_find(scan_contexts,
						scan_context_match, &wdev_id);

	if (!sc)
		return false;

	sr = l_new(struct scan_request, 1);
	sr->callback = notify;
	sr->destroy = destroy;
	sr->userdata = userdata;

	results = l_new(struct scan_results, 1);
	results->sc = sc;
	results->time_stamp = l_time_now();
	results->bss_list = l_queue_new();
	results->sr = sr;

	scan_msg = l_genl_msg_new_sized(NL80211_CMD_GET_SCAN, 8);
	l_genl_msg_append_attr(scan_msg, NL80211_ATTR_WDEV, 8, &sc->wdev_id);

	sc->get_fw_scan_cmd_id = l_genl_family_dump(nl80211, scan_msg,
							get_scan_callback,
							results,
							get_fw_scan_done);
	if (!sc->get_fw_scan_cmd_id) {
		l_queue_destroy(results->bss_list,
				(l_queue_destroy_func_t) scan_bss_free);
		l_free(results);
		l_free(sr);
		return false;
	}

	return true;
}

bool scan_wdev_add(uint64_t wdev_id)
{
	struct scan_context *sc;

	if (l_queue_find(scan_contexts, scan_context_match, &wdev_id))
		return false;

	sc = scan_context_new(wdev_id);
	if (!sc)
		return false;

	l_queue_push_head(scan_contexts, sc);

	if (l_queue_length(scan_contexts) > 1)
		goto done;

	nl80211 = l_genl_family_new(iwd_get_genl(), NL80211_GENL_NAME);
	l_genl_family_register(nl80211, "scan", scan_notify, NULL, NULL);

done:
	return true;
}

bool scan_wdev_remove(uint64_t wdev_id)
{
	struct scan_context *sc;

	sc = l_queue_remove_if(scan_contexts, scan_context_match, &wdev_id);

	if (!sc)
		return false;

	l_info("Removing scan context for wdev %" PRIx64, wdev_id);
	scan_context_free(sc);

	if (l_queue_isempty(scan_contexts)) {
		l_genl_family_free(nl80211);
		nl80211 = NULL;
	}

	return true;
}

static int scan_init(void)
{
	const struct l_settings *config = iwd_get_config();

	scan_contexts = l_queue_new();

	if (!l_settings_get_double(config, "Rank", "BandModifier5Ghz",
					&RANK_5G_FACTOR))
		RANK_5G_FACTOR = 1.0;

	if (!l_settings_get_uint(config, "Scan", "InitialPeriodicScanInterval",
					&SCAN_INIT_INTERVAL))
		SCAN_INIT_INTERVAL = 10;

	if (SCAN_INIT_INTERVAL > UINT16_MAX)
		SCAN_INIT_INTERVAL = UINT16_MAX;

	if (!l_settings_get_uint(config, "Scan", "MaximumPeriodicScanInterval",
					&SCAN_MAX_INTERVAL))
		SCAN_MAX_INTERVAL = 300;

	if (SCAN_MAX_INTERVAL > UINT16_MAX)
		SCAN_MAX_INTERVAL = UINT16_MAX;

	return 0;
}

static void scan_exit(void)
{
	l_queue_destroy(scan_contexts,
				(l_queue_destroy_func_t) scan_context_free);
	scan_contexts = NULL;
	l_genl_family_free(nl80211);
	nl80211 = NULL;
}

IWD_MODULE(scan, scan_init, scan_exit)
