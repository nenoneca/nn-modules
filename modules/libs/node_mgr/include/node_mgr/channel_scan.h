/*
 * channel_scan -- answer the hub's CHANNEL_SCAN_REQ (0x0046) with an
 * energy scan of channels 11..26 from this device's own radio
 * (CHANNEL_SCAN_REPLY 0x0047, same tid).  The hub combines the scans of
 * gateways and sensors into its channel vote: energy at the sensors is
 * what a gateway elsewhere cannot see.
 */
#ifndef NODE_MGR_CHANNEL_SCAN_H_
#define NODE_MGR_CHANNEL_SCAN_H_

int channel_scan_start(void);

#endif
