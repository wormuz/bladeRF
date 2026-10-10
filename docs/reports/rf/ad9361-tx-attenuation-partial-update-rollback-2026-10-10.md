# AD9361 TX attenuation partial-update rollback

## Finding

`ad9361_set_tx_atten()` writes the selected TX attenuation registers in sequence. A dual-channel update could program TX1 and then fail while writing TX2, leaving the RF outputs at different requested levels. The SPI layer may report failure after a byte has reached the device, so the failed register itself can also have changed. In the immediate-update path, the update control bit still needs restoration on every exit.

## Change

The setter now reads the prior attenuation for every selected channel before writing. On a programming error it replays each selected channel's old two-byte attenuation while immediate updates remain disabled, then restores the immediate-update control bit. Rollback failures are logged separately and the original SPI error remains the function result. Fault-injection source now models an ambiguous failure on the first TX2 attenuation byte after TX1 has changed and checks both channels and the update bit return to their prior values.

## RF relevance and limits

This closes a concrete software path that could leave a requested dual-channel TX mute/attenuation operation partially applied. It does not establish that a particular reported spur was caused by this path. If rollback writes also fail, software cannot guarantee restored RF output state; callers still receive the original error.

## Disposition

Source-only change. `git diff --check` only; tests/build/simulation/synthesis and hardware operations remain deferred until the complete RX+TX source audit closes.
