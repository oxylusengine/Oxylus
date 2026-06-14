#pragma once

#include "Core/Types.hpp"

namespace ox {
// 1 to 1 from SDL enum
enum class KeyCode : u32 {
  None = 0x00000000u, // "Unknown" in SDL3

  Return = 0x0000000du,
  Escape = 0x0000001bu,
  Backspace = 0x00000008u,
  Tab = 0x00000009u,
  Space = 0x00000020u,
  Exclaim = 0x00000021u,          // '!'
  DoubleApostrophe = 0x00000022u, // '"'
  Hash = 0x00000023u,             // '#'
  Dollar = 0x00000024u,           // '$'
  Percent = 0x00000025u,          // '%'
  Ampersand = 0x00000026u,        // '&'
  Apostrophe = 0x00000027u,       // '''
  LeftParentheses = 0x00000028u,  // '('
  RightParentheses = 0x00000029u, // ')'
  Asterisk = 0x0000002au,         // '*'
  Plus = 0x0000002bu,             // '+'
  Comma = 0x0000002cu,            // ','
  Minus = 0x0000002du,            // '-'
  Period = 0x0000002eu,           // '.'
  Slash = 0x0000002fu,            // '/'
  Zero = 0x00000030u,             // '0'
  One = 0x00000031u,              // '1'
  Two = 0x00000032u,              // '2'
  Three = 0x00000033u,            // '3'
  Four = 0x00000034u,             // '4'
  Five = 0x00000035u,             // '5'
  Six = 0x00000036u,              // '6'
  Seven = 0x00000037u,            // '7'
  Eight = 0x00000038u,            // '8'
  Nine = 0x00000039u,             // '9'
  Colon = 0x0000003au,            // ':'
  Semicolon = 0x0000003bu,        // ';'
  Less = 0x0000003cu,             // '<'
  Equals = 0x0000003du,           // '='
  Greater = 0x0000003eu,          // '>'
  Question = 0x0000003fu,         // '?'
  At = 0x00000040u,
  LeftBracket = 0x0000005bu,
  Backslash = 0x0000005cu,
  RightBracket = 0x0000005du,
  Caret = 0x0000005eu,
  Underscore = 0x0000005fu,
  Grave = 0x00000060u,

  // --- Alphabet ---
  A = 0x00000061u,
  B = 0x00000062u,
  C = 0x00000063u,
  D = 0x00000064u,
  E = 0x00000065u,
  F = 0x00000066u,
  G = 0x00000067u,
  H = 0x00000068u,
  I = 0x00000069u,
  J = 0x0000006au,
  K = 0x0000006bu,
  L = 0x0000006cu,
  M = 0x0000006du,
  N = 0x0000006eu,
  O = 0x0000006fu,
  P = 0x00000070u,
  Q = 0x00000071u,
  R = 0x00000072u,
  S = 0x00000073u,
  T = 0x00000074u,
  U = 0x00000075u,
  V = 0x00000076u,
  W = 0x00000077u,
  X = 0x00000078u,
  Y = 0x00000079u,
  Z = 0x0000007au,

  // --- Braces & Misc ---
  LeftBrace = 0x0000007bu,
  Pipe = 0x0000007cu,
  RightBrace = 0x0000007du,
  Tilde = 0x0000007eu,
  Delete = 0x0000007fu,
  PlusMinus = 0x000000b1u,

  // --- Function Keys ---
  CapsLock = 0x40000039u,
  F1 = 0x4000003au,
  F2 = 0x4000003bu,
  F3 = 0x4000003cu,
  F4 = 0x4000003du,
  F5 = 0x4000003eu,
  F6 = 0x4000003fu,
  F7 = 0x40000040u,
  F8 = 0x40000041u,
  F9 = 0x40000042u,
  F10 = 0x40000043u,
  F11 = 0x40000044u,
  F12 = 0x40000045u,
  F13 = 0x40000068u,
  F14 = 0x40000069u,
  F15 = 0x4000006au,
  F16 = 0x4000006bu,
  F17 = 0x4000006cu,
  F18 = 0x4000006du,
  F19 = 0x4000006eu,
  F20 = 0x4000006fu,
  F21 = 0x40000070u,
  F22 = 0x40000071u,
  F23 = 0x40000072u,
  F24 = 0x40000073u,

  // --- Navigation & Editing ---
  PrintScreen = 0x40000046u,
  ScrollLock = 0x40000047u,
  Pause = 0x40000048u,
  Insert = 0x40000049u,
  Home = 0x4000004au,
  PageUp = 0x4000004bu,
  End = 0x4000004du,
  PageDown = 0x4000004eu,
  Right = 0x4000004fu,
  Left = 0x40000050u,
  Down = 0x40000051u,
  Up = 0x40000052u,

  // --- Keypad ---
  NumLock = 0x40000053u,
  KeypadDivide = 0x40000054u,
  KeypadMultiply = 0x40000055u,
  KeypadMinus = 0x40000056u,
  KeypadPlus = 0x40000057u,
  KeypadEnter = 0x40000058u,
  Keypad1 = 0x40000059u,
  Keypad2 = 0x4000005au,
  Keypad3 = 0x4000005bu,
  Keypad4 = 0x4000005cu,
  Keypad5 = 0x4000005du,
  Keypad6 = 0x4000005eu,
  Keypad7 = 0x4000005fu,
  Keypad8 = 0x40000060u,
  Keypad9 = 0x40000061u,
  Keypad0 = 0x40000062u,
  KeypadPeriod = 0x40000063u,
  KeypadEquals = 0x40000067u,
  KeypadComma = 0x40000085u,
  KeypadEqualsAS400 = 0x40000086u,
  Keypad00 = 0x400000b0u,
  Keypad000 = 0x400000b1u,
  KeypadLeftParen = 0x400000b6u,
  KeypadRightParen = 0x400000b7u,
  KeypadLeftBrace = 0x400000b8u,
  KeypadRightBrace = 0x400000b9u,
  KeypadTab = 0x400000bau,
  KeypadBackspace = 0x400000bbu,
  KeypadA = 0x400000bcu,
  KeypadB = 0x400000bdu,
  KeypadC = 0x400000beu,
  KeypadD = 0x400000bfu,
  KeypadE = 0x400000c0u,
  KeypadF = 0x400000c1u,
  KeypadXor = 0x400000c2u,
  KeypadPower = 0x400000c3u,
  KeypadPercent = 0x400000c4u,
  KeypadLess = 0x400000c5u,
  KeypadGreater = 0x400000c6u,
  KeypadAmpersand = 0x400000c7u,
  KeypadDoubleAmpersand = 0x400000c8u,
  KeypadVerticalBar = 0x400000c9u,
  KeypadDoubleVerticalBar = 0x400000cau,
  KeypadColon = 0x400000cbu,
  KeypadHash = 0x400000ccu,
  KeypadSpace = 0x400000cdu,
  KeypadAt = 0x400000ceu,
  KeypadExclaim = 0x400000cfu,
  KeypadMemStore = 0x400000d0u,
  KeypadMemRecall = 0x400000d1u,
  KeypadMemClear = 0x400000d2u,
  KeypadMemAdd = 0x400000d3u,
  KeypadMemSubtract = 0x400000d4u,
  KeypadMemMultiply = 0x400000d5u,
  KeypadMemDivide = 0x400000d6u,
  KeypadPlusMinus = 0x400000d7u,
  KeypadClear = 0x400000d8u,
  KeypadClearEntry = 0x400000d9u,
  KeypadBinary = 0x400000dau,
  KeypadOctal = 0x400000dbu,
  KeypadDecimal = 0x400000dcu,
  KeypadHexadecimal = 0x400000ddu,

  // --- Modifiers ---
  LeftControl = 0x400000e0u,
  LeftShift = 0x400000e1u,
  LeftAlt = 0x400000e2u,
  LeftSuper = 0x400000e3u,
  RightControl = 0x400000e4u,
  RightShift = 0x400000e5u,
  RightAlt = 0x400000e6u,
  RightSuper = 0x400000e7u,

  // --- Misc / Multimedia / Application ---
  Application = 0x40000065u,
  Power = 0x40000066u,
  Execute = 0x40000074u,
  Help = 0x40000075u,
  Menu = 0x40000076u,
  Select = 0x40000077u,
  Stop = 0x40000078u,
  Again = 0x40000079u,
  Undo = 0x4000007au,
  Cut = 0x4000007bu,
  Copy = 0x4000007cu,
  Paste = 0x4000007du,
  Find = 0x4000007eu,
  Mute = 0x4000007fu,
  VolumeUp = 0x40000080u,
  VolumeDown = 0x40000081u,
  AltErase = 0x40000099u,
  SysReq = 0x4000009au,
  Cancel = 0x4000009bu,
  Clear = 0x4000009cu,
  Prior = 0x4000009du,
  Return2 = 0x4000009eu,
  Separator = 0x4000009fu,
  Out = 0x400000a0u,
  Oper = 0x400000a1u,
  ClearAgain = 0x400000a2u,
  CrSel = 0x400000a3u,
  ExSel = 0x400000a4u,
  ThousandsSeparator = 0x400000b2u,
  DecimalSeparator = 0x400000b3u,
  CurrencyUnit = 0x400000b4u,
  CurrencySubunit = 0x400000b5u,
  Mode = 0x40000101u,
  Sleep = 0x40000102u,
  Wake = 0x40000103u,
  ChannelIncrement = 0x40000104u,
  ChannelDecrement = 0x40000105u,
  MediaPlay = 0x40000106u,
  MediaPause = 0x40000107u,
  MediaRecord = 0x40000108u,
  MediaFastForward = 0x40000109u,
  MediaRewind = 0x4000010au,
  MediaNextTrack = 0x4000010bu,
  MediaPreviousTrack = 0x4000010cu,
  MediaStop = 0x4000010du,
  MediaEject = 0x4000010eu,
  MediaPlayPause = 0x4000010fu,
  MediaSelect = 0x40000110u,

  // --- AC (Application Control) Keys ---
  AcNew = 0x40000111u,
  AcOpen = 0x40000112u,
  AcClose = 0x40000113u,
  AcExit = 0x40000114u,
  AcSave = 0x40000115u,
  AcPrint = 0x40000116u,
  AcProperties = 0x40000117u,
  AcSearch = 0x40000118u,
  AcHome = 0x40000119u,
  AcBack = 0x4000011au,
  AcForward = 0x4000011bu,
  AcStop = 0x4000011cu,
  AcRefresh = 0x4000011du,
  AcBookmarks = 0x4000011eu,

  // --- Mobile / Soft Keys ---
  SoftLeft = 0x4000011fu,
  SoftRight = 0x40000120u,
  Call = 0x40000121u,
  EndCall = 0x40000122u,

  // --- Extended ---
  LeftTab = 0x20000001u,
  Level5Shift = 0x20000002u,
  MultiKeyCompose = 0x20000003u,
  LeftMeta = 0x20000004u,
  RightMeta = 0x20000005u,
  LeftHyper = 0x20000006u,
  RightHyper = 0x20000007u
};
consteval void enable_bitmask(KeyCode);

// 1 to 1 from SDL enum
enum class ScanCode : u32 {
  Unknown = 0,

  A = 4,
  B = 5,
  C = 6,
  D = 7,
  E = 8,
  F = 9,
  G = 10,
  H = 11,
  I = 12,
  J = 13,
  K = 14,
  L = 15,
  M = 16,
  N = 17,
  O = 18,
  P = 19,
  Q = 20,
  R = 21,
  S = 22,
  T = 23,
  U = 24,
  V = 25,
  W = 26,
  X = 27,
  Y = 28,
  Z = 29,

  One = 30,
  Two = 31,
  Three = 32,
  Four = 33,
  Five = 34,
  Six = 35,
  Seven = 36,
  Eight = 37,
  Nine = 38,
  Zero = 39,

  Return = 40,
  Escape = 41,
  Backspace = 42,
  Tab = 43,
  Space = 44,
  Minus = 45,
  Equals = 46,
  LeftBracket = 47,
  RightBracket = 48,
  Backslash = 49,
  Nonushash = 50,
  Semicolon = 51,
  Apostrophe = 52,
  Grave = 53,
  Comma = 54,
  Period = 55,
  Slash = 56,
  CapsLock = 57,

  F1 = 58,
  F2 = 59,
  F3 = 60,
  F4 = 61,
  F5 = 62,
  F6 = 63,
  F7 = 64,
  F8 = 65,
  F9 = 66,
  F10 = 67,
  F11 = 68,
  F12 = 69,
  PrintScreen = 70,
  ScrollLock = 71,
  Pause = 72,
  Insert = 73,
  Home = 74,
  PageUp = 75,
  Delete = 76,
  End = 77,
  PageDown = 78,
  Right = 79,
  Left = 80,
  Down = 81,
  Up = 82,
  NumlockClear = 83,
  KeypadDivide = 84,
  KeypadMultiply = 85,
  KeypadMinus = 86,
  KeypadPlus = 87,
  KeypadEnter = 88,
  Keypad3 = 91,
  Keypad1 = 89,
  Keypad2 = 90,
  Keypad4 = 92,
  Keypad5 = 93,
  Keypad6 = 94,
  Keypad7 = 95,
  Keypad8 = 96,
  Keypad9 = 97,
  Keypad0 = 98,
  KeypadPeriod = 99,
  Nonusbackslash = 100,
  Application = 101,
  Power = 102,
  KeypadEquals = 103,
  F13 = 104,
  F14 = 105,
  F15 = 106,
  F16 = 107,
  F17 = 108,
  F18 = 109,
  F19 = 110,
  F20 = 111,
  F21 = 112,
  F22 = 113,
  F23 = 114,
  F24 = 115,
  Execute = 116,
  Help = 117,
  Menu = 118,
  Select = 119,
  Stop = 120,
  Again = 121,
  Undo = 122,
  Cut = 123,
  Copy = 124,
  Paste = 125,
  Find = 126,
  Mute = 127,
  VolumeUp = 128,
  VolumeDown = 129,
  KeypadComma = 133,
  KeypadEqualsas400 = 134,
  International1 = 135,
  International2 = 136,
  International3 = 137,
  International4 = 138,
  International5 = 139,
  International6 = 140,
  International7 = 141,
  International8 = 142,
  International9 = 143,
  Lang1 = 144,
  Lang2 = 145,
  Lang3 = 146,
  Lang4 = 147,
  Lang5 = 148,
  Lang6 = 149,
  Lang7 = 150,
  Lang8 = 151,
  Lang9 = 152,
  Alterase = 153,
  Sysreq = 154,
  Cancel = 155,
  Clear = 156,
  Prior = 157,
  Return2 = 158,
  Separator = 159,
  Out = 160,
  Oper = 161,
  ClearAgain = 162,
  Crsel = 163,
  Exsel = 164,
  Kp00 = 176,
  Kp000 = 177,
  ThousandsSeparator = 178,
  DecimalSeparator = 179,
  CurrencyUnit = 180,
  CurrencysubUnit = 181,
  KeyPadLeftParen = 182,
  KeyPadRightparen = 183,
  KeyPadLeftbrace = 184,
  KeyPadRightbrace = 185,
  KeyPadTab = 186,
  KeyPadBackspace = 187,
  KeyPadA = 188,
  KeyPadB = 189,
  KeyPadC = 190,
  KeyPadD = 191,
  KeyPadE = 192,
  KeyPadF = 193,
  KeyPadXor = 194,
  KeyPadPower = 195,
  KeyPadPercent = 196,
  KeyPadLess = 197,
  KeyPadGreater = 198,
  KeyPadAmpersand = 199,
  KeyPadDblampersand = 200,
  KeyPadVerticalbar = 201,
  KeyPadDblverticalbar = 202,
  KeyPadColon = 203,
  KeyPadHash = 204,
  KeyPadSpace = 205,
  KeyPadAt = 206,
  KeyPadExclam = 207,
  KeyPadMemstore = 208,
  KeyPadMemrecall = 209,
  KeyPadMemclear = 210,
  KeyPadMemadd = 211,
  KeyPadMemsubtract = 212,
  KeyPadMemmultiply = 213,
  KeyPadMemdivide = 214,
  KeyPadPlusminus = 215,
  KeyPadClear = 216,
  KeyPadClearentry = 217,
  KeyPadBinary = 218,
  KeyPadOctal = 219,
  KeyPadDecimal = 220,
  KeyPadHexadecimal = 221,
  LeftControl = 224,
  LeftShift = 225,
  LeftAlt = 226,
  LeftGui = 227,
  RightControl = 228,
  RightShift = 229,
  RightAlt = 230,
  RightGui = 231,
  Mode = 257,
  Sleep = 258,              /**<Sleep*/
  Wake = 259,               /**<Wake*/
  ChannelIncrement = 260,   /**<ChannelIncrement*/
  ChannelDecrement = 261,   /**<ChannelDecrement*/
  MediaPlay = 262,          /**<Play*/
  MediaPause = 263,         /**<Pause*/
  MediaRecord = 264,        /**<Record*/
  MediaFastForward = 265,   /**<FastForward*/
  MediaRewind = 266,        /**<Rewind*/
  MediaNextTrack = 267,     /**<NextTrack*/
  MediaPreviousTrack = 268, /**<PreviousTrack*/
  MediaStop = 269,          /**<Stop*/
  MediaEject = 270,         /**<Eject*/
  MediaPlayPause = 271,     /**<Play/Pause*/
  MediaSelect = 272,        /*MediaSelect*/
  AcNew = 273,              /**<AcNew*/
  AcOpen = 274,             /**<AcOpen*/
  AcClose = 275,            /**<AcClose*/
  AcExit = 276,             /**<AcExit*/
  AcSave = 277,             /**<AcSave*/
  AcPrint = 278,            /**<AcPrint*/
  AcProperties = 279,       /**<AcProperties*/
  AcSearch = 280,           /**<AcSearch*/
  AcHome = 281,             /**<AcHome*/
  AcBack = 282,             /**<AcBack*/
  AcForward = 283,          /**<AcForward*/
  AcStop = 284,             /**<AcStop*/
  AcRefresh = 285,          /**<AcRefresh*/
  AcBookmarks = 286,        /**<AcBookmarks*/
  Softleft = 287,
  Softright = 288,
  Call = 289,               /**<UsedForAcceptingPhoneCalls.*/
  Endcall = 290,            /**<UsedForRejectingPhoneCalls.*/
  Reserved = 400,           /**<400500ReservedForDynamicKeycodes*/
  Count = 512               /**<NotAKey,JustMarksTheNumberOfScancodesForArrayBounds*/
};
consteval void enable_bitmask(ScanCode);

enum class ModCode : u16 {
  None = 0x0000u,
  LeftShift = 0x0001u,
  RightShift = 0x0002u,
  Level5 = 0x0004u,
  LeftControl = 0x0040u,
  RightControl = 0x0080u,
  LeftAlt = 0x0100u,
  RightAlt = 0x0200u,
  LeftSuper = 0x0400u,
  RightSuper = 0x0800u,
  NumLock = 0x1000u,
  CapsLock = 0x2000u,
  AltGr = 0x4000u,
  ScrollLock = 0x8000u,
  AnyControl = LeftControl | RightControl,
  AnyShift = LeftShift | RightShift,
  AnyAlt = LeftAlt | RightAlt,
  AnySuper = LeftSuper | RightSuper,
};
consteval void enable_bitmask(ModCode);

// Checks if given mods are matching while ignoring 'Locks' and compares at Any level.
[[nodiscard]]
constexpr bool mod_matches(ModCode current, ModCode required) noexcept {
  constexpr u16 lock_mask = static_cast<u16>(ModCode::NumLock) | static_cast<u16>(ModCode::CapsLock) |
                            static_cast<u16>(ModCode::ScrollLock);

  u16 current_bits = static_cast<u16>(current) & ~lock_mask;
  u16 required_bits = static_cast<u16>(required);

  u16 normalized_current = current_bits;

  if (current_bits & static_cast<u16>(ModCode::AnyControl)) {
    normalized_current |= static_cast<u16>(ModCode::AnyControl);
  }
  if (current_bits & static_cast<u16>(ModCode::AnyShift)) {
    normalized_current |= static_cast<u16>(ModCode::AnyShift);
  }
  if (current_bits & static_cast<u16>(ModCode::AnyAlt)) {
    normalized_current |= static_cast<u16>(ModCode::AnyAlt);
  }
  if (current_bits & static_cast<u16>(ModCode::AnySuper)) {
    normalized_current |= static_cast<u16>(ModCode::AnySuper);
  }

  constexpr u16 mod_mask = static_cast<u16>(ModCode::AnyControl) | static_cast<u16>(ModCode::AnyShift) |
                           static_cast<u16>(ModCode::AnyAlt) | static_cast<u16>(ModCode::AnySuper);

  return (normalized_current & mod_mask) == (required_bits & mod_mask);
}

enum class MouseCode : u16 {
  Left = 1,
  Middle = 2,
  Right = 3,
  Forward = 4,
  Backward = 5,
};

enum class MouseAxisCode : i16 { None = -1, AxisX, AxisY, AxisXY };

enum class GamepadButtonCode : i16 {
  None = -1,
  South, /* Bottom face button (e.g. Xbox A button) */
  East,  /* East face button (e.g. Xbox B button) */
  West,  /* West face button (e.g. Xbox X button) */
  North, /* North face button (e.g. Xbox Y button) */
  Back,
  Guide,
  Start,
  LeftStick,
  RightStick,
  LeftShoulder,
  RightShoulder,
  DPadUp,
  DPadDown,
  DPadLeft,
  DPadRight,
  Misc1,        /* Additional button (e.g. Xbox Series X share button, PS5 microphone button,
                   Nintendo Switch Pro capture button, Amazon Luna microphone button, Google Stadia capture button) */
  RightPaddle1, /* Upper or primary paddle, under your right hand (e.g. Xbox Elite paddle P1) */
  LeftPaddle1,  /* Upper or primary paddle, under your left hand (e.g. Xbox Elite paddle P3) */
  RightPaddle2, /* Lower or secondary paddle, under your right hand (e.g. Xbox Elite paddle P2) */
  LeftPaddle2,  /* Lower or secondary paddle, under your left hand (e.g. Xbox Elite paddle P4) */
  Touchpad,     /**< PS4/PS5 touchpad button */
  Misc2,        /**< Additional button */
  Misc3,        /**< Additional button */
  Misc4,        /**< Additional button */
  Misc5,        /**< Additional button */
  Misc6,        /**< Additional button */
};

enum class GamepadAxisCode : i16 {
  None = -1,
  AxisLeftX,
  AxisLeftY,
  AxisRightX,
  AxisRightY,
  AxisLeftTrigger,
  AxisRightTrigger,
};
} // namespace ox
