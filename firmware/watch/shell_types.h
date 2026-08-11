#pragma once
/*
 * shell_types.h — types that appear in function signatures.
 *
 * THIS FILE EXISTS FOR ONE REASON. The Arduino .ino preprocessor generates
 * function prototypes and hoists them to the top of the sketch, above anything
 * declared in the .ino itself. Any function whose signature mentions a type
 * declared later in the .ino therefore fails to compile with the memorably
 * unhelpful "'Gesture' does not name a type".
 *
 * Prototypes are inserted after the last #include, so a type that arrives via a
 * header is visible to them. That is the whole trick. translator-p01.ino works
 * around the same hazard by restricting itself to primitives in signatures; this
 * is the other way out, and it keeps the type safety.
 *
 * If you are tempted to move these back into the .ino: that is the bug.
 */
#include <stdint.h>

/* Derived from coordinate deltas, not read from the touch controller — the
 * CST820's gesture register reports 0x00 for everything when polled. */
enum Gesture : uint8_t {
  G_NONE,
  G_TAP,
  G_SWIPE_L,
  G_SWIPE_R,
  G_SWIPE_U,
  G_SWIPE_D,
};

/*
 * One app. Function pointers rather than virtuals: no vtables, no allocation,
 * and the whole registry stays readable as a single array literal.
 *
 * `id` matches a feature id in server/utils/features.ts, so the console's build
 * matrix reports which apps a given build ships — the fleet view keeps doubling
 * as a progress read, which is the property that made features.ts worth having.
 *
 * The shell owns the button, the capture and the token stream. An app is handed
 * a finished capture and asked to draw; it never touches the codec. That is what
 * makes push-to-talk-into-an-LLM reusable across every app instead of
 * reimplemented per app.
 */
struct App {
  const char *id;
  const char *title;
  const char *hint;
  void (*onEnter)();                  // full repaint of the body region
  void (*onTick)(uint32_t now);       // ~10 Hz; partial repaints only
  void (*onGesture)(Gesture g);       // taps and vertical swipes; L/R is the shell's
  void (*onCapture)(float secs);      // a push-to-talk capture just completed

  /* Full-bleed: the shell draws no status strip, title or page dots. True for the
   * watch face, because chrome across a photo looks like a screenshot rather than
   * a watch. Apps want the chrome; the face is not an app you use. */
  bool fullscreen;
};
