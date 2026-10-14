# DoIP conformance review — `doipcommands.cpp` (DoipManager)

Scope: the DoIP **client/tester** in `DoipManager` against **ISO 13400-2:2012**. Reference:
<https://nvdungx.github.io/Diagnostic-over-IP-DoIP>. The generic header, payload types,
routing-activation and diagnostic-message framing are correct; the findings below are gaps against
the standard, ranked by impact. Each is fixed in this library with a loopback-mock test.

## What is already correct
- Generic header: version `0x02` / inverse `0xFD`, 2-byte payload type, 4-byte big-endian length (send
  `sendMessage`, receive `onReadyRead` with proper TCP stream reassembly).
- Payload types: `0x0001` VIR, `0x0004` vehicle announcement, `0x0005`/`0x0006` routing activation,
  `0x8001` diagnostic, `0x8002`/`0x8003` diagnostic ack.
- Routing activation request: source(2)+type(1)+reserved(4) = 7 bytes; response code read at offset 4.
- Diagnostic message: source(2)+target(2)+UDS; tester source default `0x0E00` (valid test-equipment range).

## Findings (ranked)

| # | Severity | Clause (13400-2) | Finding | Code |
|---|---|---|---|---|
| 1 | **High** | Alive check (`0x0007`/`0x0008`) | Inbound `0x0007` is never answered with `0x0008`; a gateway may close the TCP socket. | `onReadyRead` |
| 2 | High | Diagnostic neg. ack (`0x8003`) | The negative ack is received but its code is ignored; the client then waits out a generic timeout instead of reporting the reason. | `sendUdsRequest` |
| 3 | Medium | Generic header NACK (`0x0000`) | Not handled; a gateway NACK (bad length, out of memory) is silently buffered and the request times out. | `onReadyRead`/`sendUdsRequest` |
| 4 | Medium | Generic header | The received protocol-version byte is never validated; malformed/foreign frames are parsed regardless. | `onReadyRead` |
| 5 | Low | Vehicle announcement (`0x0004`) | The "further action required" byte is parsed over but dropped, so discovery can't tell the user extra activation/central-security is needed. | `discoverVehicles` |
| 6 | Low | Routing activation response | Code `0x11` (success, confirmation required) is treated identically to `0x10`. | `open` |
| 7 | Low | Vehicle discovery | IPv4 broadcast only; no IPv6/multicast. | `discoverVehicles` |
| 8 | Low | Generic header | Protocol version is hard-coded `0x02`, not configurable for `0x03` peers. | `sendMessage`/`open` |
| 9 | Low (missing service) | Entity status (`0x4001`/`0x4002`) | Not implemented: no way to query the entity's node type / socket counts. Added as `doip_entity_status`. | new |
| 10 | Low (missing service) | Power mode (`0x4003`/`0x4004`) | Not implemented: no way to query diagnostic power-mode readiness. Added as `doip_power_mode`. | new |

## Fixes (this library)
Each finding is addressed with a loopback DoIP mock in the parent repo's `tests/tst_doip.cpp` driving the
real `DoipManager`. Baseline is 2012: we keep sending `0x02` and accept `0x02`/`0x03` on receive.
Validation here is against the mock only — a real-gateway smoke test is still recommended before relying
on it in the field.
