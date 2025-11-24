# OTA Status JSON Reference

> Legacy: kept for the previous BLE chunked OTA flow. The current firmware
> uses the ElegantOTA web page described in `README.md`/`BLE_PROTOCOL.md`.

This reference enumerates the JSON documents that the OTA status characteristic
(`d19d3c86-9ba9-4a52-9244-99118bd88d08`) emits so that Android/iOS clients know
what to expect while coordinating updates.

## Common Fields
- `state`: String identifier of the status stage.
- `total`: Total firmware size in bytes (present during receiving and chunk
  acknowledgements).
- `received`: Number of bytes accepted so far (only on `receiving` updates).
- `next`: Offset that the device expects next (only on `chunk_ack` updates).
- `message`: Additional human-readable status such as `rebooting` or an error
  code.
- `offset`: When an error applies to a specific chunk, the byte offset that
  triggered the failure.

## Typical Flow
1. **Idle** – `{"state":"idle"}` broadcast after boot and after clearing a
   session.
2. **Session start** – `{"state":"receiving","received":0,"total":262144}`
   emitted after a valid `CMD=START`.
3. **Chunk acknowledgement** – for every accepted data packet the device
   responds with `{"state":"chunk_ack","next":960,"total":262144}` where `next`
   equals the subsequent byte offset. The mobile client must wait for this JSON
   notification before sending the next packet.
4. **Periodic progress** – as the transfer advances the firmware repeats the
   receiving update with the `received` counter, e.g.
   `{"state":"receiving","received":65536,"total":262144}`.
5. **Validation** – once all bytes arrive the device re-reads the image,
   emitting `{"state":"validating"}`. Clients should keep the link alive until
   the final state arrives.
6. **Success** – a valid image leads to
   `{"state":"ready","message":"rebooting"}` and a delayed restart.
7. **Error** – checksum, offset, or link problems surface as
   `{"state":"error","message":"crc_mismatch","offset":960}` (error codes include
   `crc_mismatch`, `sha_mismatch`, `disconnect`, `size_mismatch`, etc.). After
   an error the service returns to the idle state and is ready for a fresh
   `CMD=START`.

Mobile clients should log every JSON notification to provide actionable details
when diagnosing OTA failures.
