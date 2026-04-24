#include "splash.h"
#include "../memory_manager/memory_manager.h"
#include <Library/UefiBootServicesTableLib.h>

#define GIF_MAX_CODES   4096
#define GIF_STACK_SIZE  4096

STATIC
UINT8
GifReadU8 (
  CONST UINT8  *Data,
  UINT64       *Pos
  )
{
  return Data[(*Pos)++];
}

STATIC
UINT16
GifReadU16 (
  CONST UINT8  *Data,
  UINT64       *Pos
  )
{
  UINT16  Value = Data[*Pos] | ((UINT16)Data[*Pos + 1] << 8);
  *Pos += 2;
  return Value;
}

typedef struct {
  CONST UINT8  *Data;
  UINT64       Pos;
  UINT32       BitBuf;
  UINT8        BitsAvail;
  UINT8        BlockRem;
} LZW_READER;

STATIC
VOID
LzwReaderInit (
  LZW_READER   *Reader,
  CONST UINT8  *Data,
  UINT64       Pos
  )
{
  Reader->Data      = Data;
  Reader->Pos       = Pos;
  Reader->BitBuf    = 0;
  Reader->BitsAvail = 0;
  Reader->BlockRem  = 0;
}

STATIC
UINT8
LzwReadByte (
  LZW_READER  *Reader
  )
{
  if (Reader->BlockRem == 0) {
    Reader->BlockRem = Reader->Data[Reader->Pos++];
  }

  Reader->BlockRem--;
  return Reader->Data[Reader->Pos++];
}

STATIC
UINT16
LzwReadCode (
  LZW_READER  *Reader,
  UINT8       CodeSize
  )
{
  while (Reader->BitsAvail < CodeSize) {
    Reader->BitBuf    |= (UINT32)LzwReadByte (Reader) << Reader->BitsAvail;
    Reader->BitsAvail += 8;
  }

  UINT16  Code = (UINT16)(Reader->BitBuf & ((1u << CodeSize) - 1));
  Reader->BitBuf  >>= CodeSize;
  Reader->BitsAvail -= CodeSize;
  return Code;
}

STATIC
VOID
LzwSkipBlocks (
  LZW_READER  *Reader
  )
{
  Reader->Pos       += Reader->BlockRem;
  Reader->BlockRem  = 0;
  Reader->BitsAvail = 0;
  Reader->BitBuf    = 0;

  for (;;) {
    UINT8  Size = Reader->Data[Reader->Pos++];
    if (Size == 0) {
      break;
    }
    Reader->Pos += Size;
  }
}

typedef struct {
  UINT16  Prefix;
  UINT8   Suffix;
} LZW_ENTRY;

typedef struct {
  UINT16  Delay;
  UINT8   Disposal;
  UINT8   TransparentFlag;
  UINT8   TransparentIdx;
} GIF_GCE;

typedef struct {
  CONST UINT8  *Data;
  UINT64       Size;
  UINT64       Pos;

  UINT16       Width;
  UINT16       Height;
  UINT8        BgIndex;

  UINT32       GlobalPalette[256];
  UINT16       GlobalPaletteSize;

  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Canvas;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *PrevCanvas;

  LZW_ENTRY    Table[GIF_MAX_CODES];
  UINT8        Stack[GIF_STACK_SIZE];

  GIF_GCE      Gce;

  // Position after header + global palette, used to rewind for looping.
  UINT64       DataStartPos;
} GIF_DECODER;

STATIC
VOID
GifParsePalette (
  CONST UINT8  *Data,
  UINT64       *Pos,
  UINT32       *Palette,
  UINT16       Count
  )
{
  for (UINT16 Index = 0; Index < Count; Index++) {
    UINT8  R = Data[(*Pos)++];
    UINT8  G = Data[(*Pos)++];
    UINT8  B = Data[(*Pos)++];
    Palette[Index] = ((UINT32)B) | ((UINT32)G << 8) | ((UINT32)R << 16) | (0xFFu << 24);
  }
}

STATIC
EFI_STATUS
GifDecodeFrame (
  GIF_DECODER  *Gif,
  UINT16       Fx,
  UINT16       Fy,
  UINT16       Fw,
  UINT16       Fh,
  UINT32       *Palette,
  UINT8        Interlaced
  )
{
  UINT8  MinCodeSize = GifReadU8 (Gif->Data, &Gif->Pos);

  if ((MinCodeSize < 2) || (MinCodeSize > 11)) {
    return EFI_COMPROMISED_DATA;
  }

  UINT16  ClearCode = (UINT16)(1u << MinCodeSize);
  UINT16  EoiCode   = ClearCode + 1;

  LZW_READER  Reader;
  LzwReaderInit (&Reader, Gif->Data, Gif->Pos);

  UINT16  TableSize = EoiCode + 1;
  UINT8   CodeSize  = MinCodeSize + 1;

  for (UINT16 Index = 0; Index < ClearCode; Index++) {
    Gif->Table[Index].Prefix = 0xFFFF;
    Gif->Table[Index].Suffix = (UINT8)Index;
  }

  UINT16  PrevCode    = 0xFFFF;
  UINT32  PixelIdx    = 0;
  UINT32  TotalPixels = (UINT32)Fw * (UINT32)Fh;

  STATIC CONST UINT8  IlStart[] = { 0, 4, 2, 1 };
  STATIC CONST UINT8  IlStep[]  = { 8, 8, 4, 2 };

  for (;;) {
    UINT16  Code = LzwReadCode (&Reader, CodeSize);

    if (Code == EoiCode) {
      break;
    }

    if (Code == ClearCode) {
      TableSize = EoiCode + 1;
      CodeSize  = MinCodeSize + 1;
      PrevCode  = 0xFFFF;
      continue;
    }

    UINT16  Sp = 0;
    UINT16  C  = Code;

    if (C >= TableSize) {
      if (PrevCode == 0xFFFF) {
        return EFI_COMPROMISED_DATA;
      }

      UINT16  Tmp = PrevCode;
      while (Gif->Table[Tmp].Prefix != 0xFFFF) {
        Tmp = Gif->Table[Tmp].Prefix;
      }

      Gif->Stack[Sp++] = Gif->Table[Tmp].Suffix;
      C = PrevCode;
    }

    while ((C != 0xFFFF) && (Sp < GIF_STACK_SIZE)) {
      Gif->Stack[Sp++] = Gif->Table[C].Suffix;
      C = Gif->Table[C].Prefix;
    }

    for (INT32 Index = (INT32)Sp - 1; Index >= 0; Index--) {
      if (PixelIdx >= TotalPixels) {
        break;
      }

      UINT8  ColorIdx = Gif->Stack[Index];

      if (Gif->Gce.TransparentFlag && (ColorIdx == Gif->Gce.TransparentIdx)) {
        PixelIdx++;
        continue;
      }

      UINT32  Packed = Palette[ColorIdx];
      EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Pixel;
      Pixel.Blue     = (UINT8)(Packed);
      Pixel.Green    = (UINT8)(Packed >> 8);
      Pixel.Red      = (UINT8)(Packed >> 16);
      Pixel.Reserved = 0;

      UINT16  Lx = (UINT16)(PixelIdx % Fw);
      UINT16  Ly;

      if (Interlaced) {
        UINT32  Row   = PixelIdx / Fw;
        UINT32  Accum = 0;
        Ly = 0;
        for (UINT8 Pass = 0; Pass < 4; Pass++) {
          UINT16  RowsInPass = 0;
          for (UINT16 Rr = IlStart[Pass]; Rr < Fh; Rr += IlStep[Pass]) {
            RowsInPass++;
          }

          if (Row < Accum + RowsInPass) {
            Ly = IlStart[Pass] + (UINT16)(Row - Accum) * IlStep[Pass];
            break;
          }
          Accum += RowsInPass;
        }
      } else {
        Ly = (UINT16)(PixelIdx / Fw);
      }

      UINT32  Cx = Fx + Lx;
      UINT32  Cy = Fy + Ly;

      if ((Cx < Gif->Width) && (Cy < Gif->Height)) {
        Gif->Canvas[Cy * Gif->Width + Cx] = Pixel;
      }

      PixelIdx++;
    }

    if ((PrevCode != 0xFFFF) && (TableSize < GIF_MAX_CODES)) {
      UINT16  Tmp = Code;
      if (Code >= TableSize) {
        Tmp = PrevCode;
      }
      while (Gif->Table[Tmp].Prefix != 0xFFFF) {
        Tmp = Gif->Table[Tmp].Prefix;
      }

      Gif->Table[TableSize].Prefix = PrevCode;
      Gif->Table[TableSize].Suffix = Gif->Table[Tmp].Suffix;
      TableSize++;

      if ((TableSize >= (1u << CodeSize)) && (CodeSize < 12)) {
        CodeSize++;
      }
    }

    PrevCode = Code;
  }

  LzwSkipBlocks (&Reader);
  Gif->Pos = Reader.Pos;

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
GifOpen (
  GIF_DECODER  *Gif,
  CONST UINT8  *Data,
  UINT64       Size
  )
{
  Gif->Data = Data;
  Gif->Size = Size;
  Gif->Pos  = 0;

  if ((Size < 13) || (Data[0] != 'G') || (Data[1] != 'I') || (Data[2] != 'F')) {
    return EFI_COMPROMISED_DATA;
  }

  Gif->Pos = 6;

  Gif->Width   = GifReadU16 (Data, &Gif->Pos);
  Gif->Height  = GifReadU16 (Data, &Gif->Pos);

  UINT8  Packed = GifReadU8 (Data, &Gif->Pos);
  Gif->BgIndex  = GifReadU8 (Data, &Gif->Pos);
  GifReadU8 (Data, &Gif->Pos); // aspect ratio

  UINT8  HasGct    = (Packed >> 7) & 1;
  UINT8  GctSizeP  = Packed & 7;

  Gif->GlobalPaletteSize = 0;

  if (HasGct) {
    Gif->GlobalPaletteSize = (UINT16)(1u << (GctSizeP + 1));
    GifParsePalette (Data, &Gif->Pos, Gif->GlobalPalette, Gif->GlobalPaletteSize);
  }

  Gif->DataStartPos = Gif->Pos;

  UINT64  CanvasBytes = (UINT64)Gif->Width * Gif->Height * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);

  EFI_STATUS  Status = mm_allocate_pool ((VOID **)&Gif->Canvas, CanvasBytes, EfiBootServicesData);
  if (Status != EFI_SUCCESS) {
    return Status;
  }

  Status = mm_allocate_pool ((VOID **)&Gif->PrevCanvas, CanvasBytes, EfiBootServicesData);
  if (Status != EFI_SUCCESS) {
    mm_free_pool (Gif->Canvas);
    return Status;
  }

  mm_fill_memory ((UINT8 *)Gif->Canvas, CanvasBytes, 0);
  mm_fill_memory ((UINT8 *)Gif->PrevCanvas, CanvasBytes, 0);

  Gif->Gce.Delay           = 10;
  Gif->Gce.Disposal        = 0;
  Gif->Gce.TransparentFlag = 0;
  Gif->Gce.TransparentIdx  = 0;

  return EFI_SUCCESS;
}

STATIC
VOID
GifClose (
  GIF_DECODER  *Gif
  )
{
  if (Gif->Canvas != NULL) {
    mm_free_pool (Gif->Canvas);
  }
  if (Gif->PrevCanvas != NULL) {
    mm_free_pool (Gif->PrevCanvas);
  }
  Gif->Canvas     = NULL;
  Gif->PrevCanvas = NULL;
}

STATIC
VOID
GifReset (
  GIF_DECODER  *Gif
  )
{
  Gif->Pos = Gif->DataStartPos;
  UINT64  CanvasBytes = (UINT64)Gif->Width * Gif->Height * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
  mm_fill_memory ((UINT8 *)Gif->Canvas, CanvasBytes, 0);

  Gif->Gce.Delay           = 10;
  Gif->Gce.Disposal        = 0;
  Gif->Gce.TransparentFlag = 0;
  Gif->Gce.TransparentIdx  = 0;
}

STATIC
UINT64
GifNextFrame (
  GIF_DECODER  *Gif,
  EFI_STATUS   *StatusOut
  )
{
  *StatusOut = EFI_SUCCESS;

  for (;;) {
    if (Gif->Pos >= Gif->Size) {
      return 0;
    }

    UINT8  BlockType = GifReadU8 (Gif->Data, &Gif->Pos);

    if (BlockType == 0x3B) { // trailer
      return 0;
    }

    if (BlockType == 0x21) { // extension
      UINT8  ExtLabel = GifReadU8 (Gif->Data, &Gif->Pos);

      if (ExtLabel == 0xF9) { // graphics control
        GifReadU8 (Gif->Data, &Gif->Pos); // block size (always 4)

        UINT8  GcePacked = GifReadU8 (Gif->Data, &Gif->Pos);
        Gif->Gce.Disposal        = (GcePacked >> 2) & 7;
        Gif->Gce.TransparentFlag = GcePacked & 1;
        Gif->Gce.Delay           = GifReadU16 (Gif->Data, &Gif->Pos);
        Gif->Gce.TransparentIdx  = GifReadU8 (Gif->Data, &Gif->Pos);

        GifReadU8 (Gif->Data, &Gif->Pos); // terminator
      } else {
        for (;;) {
          UINT8  Size = GifReadU8 (Gif->Data, &Gif->Pos);
          if (Size == 0) {
            break;
          }
          Gif->Pos += Size;
        }
      }
      continue;
    }

    if (BlockType == 0x2C) { // image descriptor
      UINT16  Fx = GifReadU16 (Gif->Data, &Gif->Pos);
      UINT16  Fy = GifReadU16 (Gif->Data, &Gif->Pos);
      UINT16  Fw = GifReadU16 (Gif->Data, &Gif->Pos);
      UINT16  Fh = GifReadU16 (Gif->Data, &Gif->Pos);

      UINT8  ImgPacked  = GifReadU8 (Gif->Data, &Gif->Pos);
      UINT8  HasLocal   = (ImgPacked >> 7) & 1;
      UINT8  Interlaced = (ImgPacked >> 6) & 1;
      UINT8  LctSizeP   = ImgPacked & 7;

      UINT32  LocalPalette[256];
      UINT32  *Palette = Gif->GlobalPalette;

      if (HasLocal) {
        UINT16  LctCount = (UINT16)(1u << (LctSizeP + 1));
        GifParsePalette (Gif->Data, &Gif->Pos, LocalPalette, LctCount);
        Palette = LocalPalette;
      }

      // Save canvas for disposal method 3 (restore-to-previous).
      UINT64  CanvasBytes = (UINT64)Gif->Width * Gif->Height * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
      mm_copy_memory ((UINT8 *)Gif->PrevCanvas, (CONST UINT8 *)Gif->Canvas, CanvasBytes);

      *StatusOut = GifDecodeFrame (Gif, Fx, Fy, Fw, Fh, Palette, Interlaced);

      UINT64  DelayUs = (UINT64)Gif->Gce.Delay * 5000; // 2x speed
      if (DelayUs == 0) {
        DelayUs = 50000;
      }

      return DelayUs;
    }

    // Unknown block type, byte was already consumed; loop and try again.
  }
}

STATIC
VOID
GifApplyDisposal (
  GIF_DECODER  *Gif
  )
{
  UINT64  CanvasBytes = (UINT64)Gif->Width * Gif->Height * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);

  if (Gif->Gce.Disposal == 2) {
    mm_fill_memory ((UINT8 *)Gif->Canvas, CanvasBytes, 0);
  } else if (Gif->Gce.Disposal == 3) {
    mm_copy_memory ((UINT8 *)Gif->Canvas, (CONST UINT8 *)Gif->PrevCanvas, CanvasBytes);
  }
}

STATIC
EFI_GRAPHICS_OUTPUT_PROTOCOL *
SplashGetGop (
  VOID
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop = NULL;
  EFI_GUID  GopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

  EFI_STATUS  Status = gBS->LocateProtocol (&GopGuid, NULL, (VOID **)&Gop);
  return (Status == EFI_SUCCESS) ? Gop : NULL;
}

STATIC
VOID
SplashClearScreen (
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Black = { 0, 0, 0, 0 };

  Gop->Blt (
         Gop,
         &Black,
         EfiBltVideoFill,
         0, 0, 0, 0,
         Gop->Mode->Info->HorizontalResolution,
         Gop->Mode->Info->VerticalResolution,
         0
         );
}

STATIC
VOID
SplashBltCanvas (
  EFI_GRAPHICS_OUTPUT_PROTOCOL   *Gop,
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Canvas,
  UINT16                         GifW,
  UINT16                         GifH
  )
{
  UINT32  ScreenW = Gop->Mode->Info->HorizontalResolution;
  UINT32  ScreenH = Gop->Mode->Info->VerticalResolution;

  UINT32  DestX = (ScreenW > GifW) ? (ScreenW - GifW) / 2 : 0;
  UINT32  DestY = (ScreenH > GifH) ? (ScreenH - GifH) / 2 : 0;

  UINT32  BltW = (GifW < ScreenW) ? GifW : ScreenW;
  UINT32  BltH = (GifH < ScreenH) ? GifH : ScreenH;

  Gop->Blt (
         Gop,
         Canvas,
         EfiBltBufferToVideo,
         0, 0,
         DestX, DestY,
         BltW, BltH,
         GifW * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)
         );
}

EFI_STATUS
SplashDisplay (
  IN UINT8*  RawGifData,
  IN UINT32  LoopCount
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop = SplashGetGop ();
  if (Gop == NULL) {
    return EFI_UNSUPPORTED;
  }

  GIF_DECODER  Gif;
  mm_fill_memory ((UINT8 *)&Gif, sizeof (Gif), 0);

  EFI_STATUS  Status = GifOpen (&Gif, splash_gif_data, SPLASH_GIF_SIZE);
  if (Status != EFI_SUCCESS) {
    return Status;
  }

  SplashClearScreen (Gop);

  for (UINT32 Loop = 0; Loop <= LoopCount; Loop++) {
    if (Loop > 0) {
      GifReset (&Gif);
    }

    for (;;) {
      EFI_STATUS  FrameStatus;
      UINT64  DelayUs = GifNextFrame (&Gif, &FrameStatus);

      if (DelayUs == 0) {
        break;
      }

      // Skip bad frames but keep the animation going.
      if (FrameStatus != EFI_SUCCESS) {
        continue;
      }

      SplashBltCanvas (Gop, Gif.Canvas, Gif.Width, Gif.Height);
      gBS->Stall (DelayUs);
      GifApplyDisposal (&Gif);
    }
  }

  // Hold the last frame briefly before boot continues.
  gBS->Stall (1000000);

  GifClose (&Gif);

  return EFI_SUCCESS;
}
