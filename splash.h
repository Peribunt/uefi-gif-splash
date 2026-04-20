#pragma once
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
  UINT32  LoopCount
  );
