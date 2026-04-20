# splash

GIF playback on boot for EDK2.

Drop `splash.c` / `splash.h` into your bootloader source, call `SplashDisplay(loops)` somewhere early in your entry point, and you'll get the GIF rendered centered on screen via the Graphics Output Protocol.

## what you need to wire up yourself

- `splash_data.h`: should expose `splash_gif_data[]` (the raw GIF bytes) and `SPLASH_GIF_SIZE`. Easiest way is `xxd -i your.gif > splash_data.h` and rename the symbols to match.
- `memory_manager`: the code calls `mm_allocate_pool`, `mm_free_pool`, `mm_fill_memory`, `mm_copy_memory`. Either provide your own with those names, or swap them for `gBS->AllocatePool` / `FreePool` / `SetMem` / `CopyMem`.
- Add `splash.c` to your `.inf` sources and make sure `UefiBootServicesTableLib` and `gEfiGraphicsOutputProtocolGuid` are in scope.

## usage

```c
SplashDisplay(2); // play the gif 3 times then return
```

Returns `EFI_UNSUPPORTED` if there's no GOP, `EFI_COMPROMISED_DATA` if the GIF is malformed, otherwise `EFI_SUCCESS`. Failure is non-fatal, just don't show anything and keep booting.

## what it actually does

Parses GIF87a/89a, decodes LZW frames into an offscreen RGB buffer, blits to the framebuffer, honors frame delays (sped up 2x because the default GIF timing is sluggish on a boot screen), handles transparency and disposal methods 2 and 3, supports interlaced frames. No external decoder, no allocations beyond two canvas buffers.

Tested on real hardware and OVMF. YMMV on weird GIFs, if a frame fails to decode it's skipped and the animation keeps going.
