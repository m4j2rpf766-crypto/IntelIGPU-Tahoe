# HEVC worker/transport round-trip regression

This offline check reads compressed HEVC samples from an MP4 or MOV. It calls the production worker's `pack_sample`, then the production transport's `make_sample_buffer`, and compares each reconstructed sample with the original. It needs x86_64 macOS, Xcode command-line tools, and a readable HEVC video track.

```sh
python3 roundtrip/run_roundtrip.py --video /path/to/sample-hevc.mp4
```

The script compiles both clients in a temporary directory and removes them afterward. Its worker links the explicit unavailable-backend adapter so it can call the serialization routine without probing or submitting to a GPU.

The comparison requires at least one sample, matching PTS and duration, key-frame flags, coded bytes, parameter sets, dimensions and color attachments. It reads through the complete input and rejects extra records. It does not require a fixed frame count, resolution, key-frame interval or GOP.

The regression caught a missing copy of `WEXEncodedInfo` into the payload returned by `pack_sample`. The transport reads that header before reconstructing the sample. The 48-byte header is now copied after its fields are filled and zero-initialized.
