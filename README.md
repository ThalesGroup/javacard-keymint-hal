# Thales Android Javacard KeyMint HAL

## Transport selection

The SE transport is chosen at **compile time** via the `-DOMAPI_TRANSPORT` flag in [keymint/5.0/hal/Android.bp](keymint/5.0/hal/Android.bp).

| Flag present | Transport used |
|---|---|
| Yes (default) | `OmapiTransport` — via Android SE OMAPI service |
| No | `SeTransport` — direct SE communication |

> On emulator builds, `SocketTransport` is always used regardless of this flag.

To switch to SE direct transport, comment out `-DOMAPI_TRANSPORT` in **all three** `cflags` blocks of `Android.bp` and rebuild.

## OMAPI configuration

When using OMAPI transport, push the mapping config to the device:

```bash
adb push keymint/5.0/hal/etc/hal_uuid_map_config.xml /vendor/etc/
```
