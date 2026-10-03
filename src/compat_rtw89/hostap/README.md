# Stand-in headers for the vendored hostap SAE code

`third_party/hostap/` holds only `sae.c`, `dragonfly.c`, `wpabuf.c` and the
headers they share. These files stand in for the hostap headers that were not
fetched, declaring just what that code uses; the functions behind them are in
`../rtw89_hostap.c` (crypto on Mbed TLS and on `rtw89_crypto.c`, memory,
randomness, logging). This directory comes first on the include path.
