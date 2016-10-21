/*
 * hostapd / network steering system
 * Copyright (c) 2016, CoCo Communications Corp
 * 
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#ifndef NETSTEERING_H
#define NETSTEERING_H

struct sta_info;
struct hostapd_data;

int net_steering_init(struct hostapd_data *hapd);
void net_steering_deinit(struct hostapd_data *hapd);
void net_steering_association(struct hostapd_data *hapd, struct sta_info *sta,
			      int rssi);
void net_steering_disassociation(struct hostapd_data *hapd,
				 struct sta_info *sta);

#endif /* NETSTEERING_H */
