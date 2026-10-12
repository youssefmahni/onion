# 0013 — SWIM wire defaults and the LAN beacon group

## Context

M7 adds the SWIM membership protocol (DESIGN › Membership) over UDP 7946 and the signed LAN multicast beacon every 5 s. The DESIGN leaves the multicast group address open ("group address in a decision record"), and the wire format needs one canonical shape so every OS implementation interoperates.

## Decision (Proposed)

- **Beacon group.** The LAN beacon is sent to the IPv4 group `239.255.77.31:7946` (administratively-scoped site-local, taken from the 239.255.0.0/16 block so it does not conflict with link-local mDNS-style groups). IPv6 is out of scope for M7; a device listens on its link-local address for SWIM, and on the group address when it can join it.
- **Beacon payload.** The beacon is a SWIM gossip datagram (`{"t":"beacon", ...}`) carrying the sender's own membership update, signed like every other SWIM message: the Ed25519 signature is over the canonical JSON of the `updates` payload plus the sender device id and a timestamp (the sync-signing scheme's `method ‖ path ‖ body-hash ‖ timestamp` pattern is reused, with `method = "beacon"` and `path = ""`).
- **Wire format.** All SWIM datagrams are canonical JSON with these keys: `t` (message type: `ping`, `ack`, `indirect_ping`, `indirect_ack`, `gossip`, `beacon`), `from` (device id), `seq` (per-sender counter), `target` (for indirect messages), `ok` (boolean, for indirect acks), `updates` (an array of member records: `device_id`, `address`, `state`, `incarnation`, `vector`, `capabilities`). Timestamps are never trusted; ordering comes from incarnation numbers, matching the log's HLC discipline.
- **Ports.** UDP 7946 for SWIM gossip and the beacon; TCP 7947 stays the signed-sync HTTP port. Both bind on the home-LAN interface only.
- **Unsigned messages.** A gossip datagram whose signature does not verify is dropped, except that pings and acks are unsigned (they carry only counters); membership *state changes* travel only in signed updates, so an attacker who can inject ping/ack noise cannot change what anyone believes.

## Consequences

- `src/swim`'s transport is injectable; the platform layer provides the UDP 7946 socket and joins the beacon group on the active interface.
- The SWIM engine signs the piggybacked updates itself (the signer is injected through `set_updates_signer`, built on the existing `log::Signer`), then hands the signed payload to the transport layer via its `beacon_hook`; the platform layer only performs the UDP multicast send.
- Any device that cannot join the group (router isolation) still discovers peers through invitation seed addresses, per DESIGN › Discovery.
