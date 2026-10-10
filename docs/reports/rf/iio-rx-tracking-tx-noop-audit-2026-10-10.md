# RX-only tracking setters accepted TX attributes as no-ops

The AD9361 IIO getters for RF DC, baseband DC, and quadrature tracking return
`-ENOENT` for output/TX channels. Their setters parsed a value, skipped the RX
operation when `channel->ch_out` was true, then returned the input length as
success. A caller could therefore believe a TX tracking setting had been
applied even though no such TX control exists.

All three setters now reject TX/output channels with `-ENOENT`, matching their
getters, and propagate RX setter failures unchanged. Source-only verification:
`git diff --check` in the no-OS and bladeRF worktrees. No build or test was run
under the active full RX+TX source-audit gate. Release validation should
exercise all three TX attribute stores for `-ENOENT` and confirm RX stores
continue to propagate AD9361 SPI failures.
