#ifndef __SPLASH_H__
#define __SPLASH_H__

#include <Library/UefiLib.h>
#include <Protocol/GraphicsOutput.h>

//
// Display the embedded animated GIF splash screen.
//
// LoopCount: number of times to loop the animation (0 = play once).
//
// Returns EFI_SUCCESS or an error code. Failure is non-fatal; boot continues
// regardless.
//
EFI_STATUS
SplashDisplay (
  IN UINT8*  RawGifData,
  IN UINTN   DataLength,
  IN UINT32  LoopCount
  );

#endif
