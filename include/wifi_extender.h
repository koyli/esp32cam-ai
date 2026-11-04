#pragma once

// Optional Wi-Fi range extender: runs a soft AP alongside the station link
// and NATs AP clients onto the upstream network. Enabled by defining
// EXTENDER_SSID in config.h; otherwise every function here is a no-op.

bool extenderEnabled();

// (Re)starts the soft AP on the station's current channel, points DHCP
// clients at a real DNS server and turns on NAT. Call after every station
// (re)connect attempt, successful or not — a station reconnect restarts the
// radio, and the AP must follow the upstream channel.
void extenderBegin();

// Number of stations currently associated with the soft AP.
int extenderClients();

// Call periodically: turns NAT back on if the AP has lost it.
void extenderPoll();
