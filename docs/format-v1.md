# Payload format v1

The final 80 bytes of a generated application are the footer. Decoders must reject unknown versions, inconsistent ranges, integer overflow, manifests larger than 1 MiB, and any bytes between the end of the manifest and footer.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 8 | ASCII magic `LWWEB001` |
| 8 | 4 | format version, little endian |
| 12 | 4 | flags, little endian |
| 16 | 8 | payload offset |
| 24 | 8 | payload size |
| 32 | 8 | manifest offset |
| 40 | 8 | manifest size |
| 48 | 32 | SHA-256 of the exact payload byte range |

Flags:

- `0x00000001`: payload is a ZIP archive.
- `0x00000002`: manifest describes an online URL.

The JSON manifest has format identifier `lw-web-app` and version `1`. Unknown JSON members are ignored so compatible fields can be added later.

