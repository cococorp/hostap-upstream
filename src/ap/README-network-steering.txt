License
-------
hostapd / network steering system
Copyright (c) 2016, CoCo Communications Corp

This software may be distributed under the terms of the BSD license.
See README for more details.

Introduction
------------
The network steering system steers client STAs to the infrastructure AP with the best RSSI. APs collaborate and can use several methods to direct client STAs to transition to the best AP.

Design
------
The network steering system executes a protocol state machine on each participating AP. The APs exchange protocol messages, using the DS, to determine if a client STA should transition to a different AP. APs collect client STA RSSI information from probe requests. When a client STA associates to a participating AP, the network steering system begins tracking the client STA. The associated AP begins distributing its client STA RSSI information to other participating APs. As this RSSI information is received, the receiving AP compares it to the latest RSSI value it received from probe requests. If its local RSSI value is better, it contacts the AP that sent the client STA RSSI and requests the client be transitioned to itself.

State Machine
-------------
The table below specifies the state machine that is executed on each AP for each client. An AP creates a state machine when a client associates with it, when it receives a score for the client from another AP, or it receives a probe request from the client. The top row contains the events defined in the system. The left column contains the current state. The cells contain the new state for a given event. Cells are left blank when there is no state change.
+--------------------------------------------------------------------------------------------------------------------------+
| Current state/Event | Associated | Disassociated | PeerIsWorse | PeerNotWorse | CloseClient | ClosedClient | Timeout     |
|--------------------------------------------------------------------------------------------------------------------------|
| Idle                | Associated |               | Confirming  | Rejected     | Rejected    |              |             |
|--------------------------------------------------------------------------------------------------------------------------|
| Confirming          | Associated |               | Confirming  | Rejected     |             | Associating  | Idle        |
|--------------------------------------------------------------------------------------------------------------------------|
| Associating         | Associated | Idle          | Confirming  |              | Rejected    |              |             |
|--------------------------------------------------------------------------------------------------------------------------|
| Associated          |            | Idle          | Associated  |              | Rejecting   |              | Associated  |
|--------------------------------------------------------------------------------------------------------------------------|
| Rejecting           |            | Rejected      | Confirming  |              | Rejecting   |              | Associating |
|--------------------------------------------------------------------------------------------------------------------------|
| Rejected            |            |               | Confirming  |              | Rejected    |              | Associating |
+--------------------------------------------------------------------------------------------------------------------------+

States
------
Idle - AP will allow the client to associate with it.
Confirming - AP has told another AP to blacklist the client and is waiting for it to tell us that it has blacklisted the client.
Associating - A remote AP has confirmed that it has blacklisted the client; AP is now waiting on an associate.
Associated - The client is associated to this AP.
Rejecting - The AP has blacklisted the client is waiting on a disassociate and will then send out a closed packet to remotes.
Rejected - The client is blacklisted and disassociated.
Events
Associated - A client STA has associated with the AP.
Disassociated -The client has either gone away or associated with a different AP.
TimeOut - Used to limit how long an AP waits on an event (e.g. Re).
CloseClient - The AP has been told to blacklist the client.
ClosedClient - A remote AP has confirmed that it has blacklisted the client.
PeerIsWorse - A remote AP sent a client score packet with a score worse than our local score.
PeerNotWorse - A remote AP sent a client score packet with a score the same as (or better) than our local score.

Packet Descriptions
-------------------
All packets share the same magic number and packet version. Packets received that have a later packet version number or do not match the magic number for the packet shall be ignored. All multi-byte data shall be in network byte order (big-endian byte order). Some values for packets are known and are specified, other values will need to be filled as required. Those values are shown as blanks in this design document.

HEADER
The packet header will be at the head of each set of transmitted TLVs.
+-------------------------------------------------------------------------------+
|Size (in bytes) | Description                                          | Value |
|-------------------------------------------------------------------------------|
|     1          | PACKET_MAGIC                                         |  48   |
|-------------------------------------------------------------------------------|
|     1          | PACKET_VERSION                                       |   1   |
|-------------------------------------------------------------------------------|
|                | Size of entire packet which includes one or more     |       |
|     2          | TLVS not including the magic number or the version   |       |
|                | (e.g. entire packet size - 2 bytes)                  |       |
|-------------------------------------------------------------------------------|
|     2          | Packet serial number which is a monotonically        |       |
|                | increasing number with each packet that is sent.     |       |
+-------------------------------------------------------------------------------+

SCORE_TYPE
This TLV is used to flood the scores of associated clients to all the other APs in the DS. The type of this TLV is 0.
+-------------------------------------------------------------------------------+
|Size (in bytes) | Description                                          | Value |
|-------------------------------------------------------------------------------|
|     1          | Type                                                 |  0    |
|-------------------------------------------------------------------------------|
|     6          | MAC address of client this score is being            |       |
|                | announced for                                        |       |
|-------------------------------------------------------------------------------|
|     6          | MAC address of BSSID that this score is being        |       |
|                | announced for (this is necessary for multiple        |       |
|                | BSSID APs).                                          |       |
|-------------------------------------------------------------------------------|
|     2          | Score of the client                                  |       |
|-------------------------------------------------------------------------------|
|     4          | msecs since association to BSSID                     |       |
+-------------------------------------------------------------------------------+

CLOSE_CLIENT_TYPE
This TLV is used to cause a remote AP to close out a client per the protocol state machine. The type of this TLV is 1.
+-------------------------------------------------------------------------------+
|Size (in bytes) | Description                                         | Value  |
|-------------------------------------------------------------------------------|
|     1          | Type                                                 |  1    |
|-------------------------------------------------------------------------------|
|     6          | MAC address of client to close on destination AP     |       |
|-------------------------------------------------------------------------------|
|     6          | MAC address of BSSID that is sending this TLV        |       |
|-------------------------------------------------------------------------------|
|     6          | MAC address of BSSID that is being sent this TLV     |       |
|-------------------------------------------------------------------------------|
|     1          | The channel used by the sending BSSID                |       |
+-------------------------------------------------------------------------------+

CLOSED_CLIENT_TYPE This TLV is used to notify the close-requesting AP that this AP has successfully closed the client per the protocol state machine. The type of this TLV is 2.
+-------------------------------------------------------------------------------+
|Size (in bytes) | Description                                          | Value |
|-------------------------------------------------------------------------------|
|     1          | Type                                                  |  2   |
|-------------------------------------------------------------------------------|
|     6          | MAC address of the closed client                      |      |
|-------------------------------------------------------------------------------|
|     6          | MAC address of BSSID that sent the close              |      |
+-------------------------------------------------------------------------------+

Implementation
--------------
The network steering system is implemented as a separate module in order to allow simple build-time removal of the code and avoid disruption to the existing codebase. The module includes several public APIs that are used by hostap. The network steering system includes a state machine implemented using the hostap state machine implementation. The hostap state machine macros have been complemented with additional macros to improve code readability. Each configured AP sends unicast messages using L2 frames. The steering system uses blacklist entries on the AP to deny client STA association when the system determines an AP to be a poor choice for the client STA. The system will use BSS Transition request messages, when supported by the client STA, to direct the client STA to the best AP. The protocol shares a mutual boundary with the DS and with the FT candidates.

Integration
-----------
The network steering module has several public APIs that hostap uses to communicate with the network steering system.
int net_steering_init(struct hostapd_data *hapd);
void net_steering_deinit(struct hostapd_data *hapd);
void net_steering_association(struct hostapd_data *hapd, struct sta_info *sta, int rssi);
void net_steering_disassociation(struct hostapd_data *hapd, struct sta_info *sta);

Configuration
-------------
The network steering system relies on two configuration variables to function. First, the net_steering_mode variable is used to control the behavior of the steering system. The default is “off”. Other values for net_steering_mode are: “suggest” and “force”. In “suggest” mode, the system will never blacklist a client, and will only use BSS Transition Request messages to steer client STAs. In “force” mode, the system will use both blacklist and BSS Transition Request messages to steer client STAs. Second, the list of peer APs that participate in the steering protocol are determined by the (802.11-2012 Chapter 12) mobility domain members specified in the r0kh list. This protocol provides a means for the network to trigger FT so it does make some sense to share configuration wth the mobility domain. The domain of participants has a natural overlap and shared responsibility.

Test Results
------------
Test results show that the network steering system can maintain client STA throughput when the RF environment for the associated AP degrades. The network steering system tolerates client-STA directed transitions (roaming).
