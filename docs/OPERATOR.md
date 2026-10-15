
## Hard rules (learned the expensive way)

1. **Never open a node's data dir with a second writer while the node
   runs.** `campaign start` (and any CLI command that writes) appends to
   the same log file from a second process — the two writers interleave
   batches and **fork the hash chain**. After that, every other machine
   quarantines the batches (`verify 2` = BadPrev) and a joiner can never
   catch up. The correct sequence is: stop the node → CLI write → start
   the node (it replays at startup), or drive the running node through
   the dashboard API only.
2. **Force a checkpoint before admitting a joiner.** Checkpoints are
   written every 6 hours or 100,000 applied records; a joiner clones the
   newest checkpoint, so a stale one hands it a chain snapshot that no
   longer matches the live log. After heavy activity (or after any log
   repair) run:

   ```sh
   curl -X POST http://<host>:8080/api/checkpoint \
     -H "Origin: http://127.0.0.1:<port>" -H "X-Onion-Token: <api-token>" \
     -H "Content-Type: application/json" -d '{"id":"x"}'
   ```

   (The API token is `keys/api-token` in the data dir.)
