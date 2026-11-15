#pragma once

/**
 * @file PuzzleFrontend.h
 * @brief E-paper front end for the vendored puzzle collection (third_party/puzzles).
 *
 * Owns one puzzle's midend and draws it into the renderer's framebuffer, inside a board area
 * the screen chooses. The collection draws in colour; here every colour becomes paper, ink or a
 * checkerboard grey by its brightness, and text tells colours apart by style instead: black
 * text (Sudoku's givens) is bold, other dark colours (your digits, pencil marks) are regular, and
 * red (errors) is white on a black box. Animations are cut short: a timer the puzzle starts is
 * finished on the next service(), so a move costs one refresh.
 *
 * The midend only redraws what changed. After any call that can draw, takeDirty() says whether
 * the framebuffer changed and where, so the screen pushes one fast refresh. Main loop only.
 */

#if INX_HAS_PUZZLES

#include <GfxRenderer.h>

#include <string>

struct game;
struct midend;

class PuzzleFrontend {
 public:
  PuzzleFrontend(GfxRenderer& renderer, const game& puzzle);
  ~PuzzleFrontend();
  PuzzleFrontend(const PuzzleFrontend&) = delete;
  PuzzleFrontend& operator=(const PuzzleFrontend&) = delete;

  /** Fits the board into this screen area (centred). Call before drawing, and again after a new game. */
  void layout(int x, int y, int width, int height);
  /** The board as drawn by the last layout(). */
  GfxRenderer::DisplayRegion board() const { return {boardX, boardY, boardW, boardH}; }

  /** Parameters as the puzzle's own string (Sudoku: "3x3di"); applies to the next newGame(). */
  bool setParams(const std::string& encoded);
  /** The current game's parameters, in full (Sudoku: size, symmetry and difficulty letters). */
  std::string params() const;

  /** Generates a new puzzle with the current parameters (can take a second or two on the S3). */
  void newGame();
  /** Clears the board back to the puzzle's givens (an undoable move). */
  void restart();
  bool load(const char* path);
  bool save(const char* path) const;

  /** Draws the whole board into the framebuffer (after the screen around it was redrawn). */
  void drawAll();

  /** A tap at screen coordinates: `alternate` is the puzzle's right click (Sudoku: pencil mode). */
  bool tap(int screenX, int screenY, bool alternate);
  /** A key: a character, CURSOR_*, UI_UNDO or UI_REDO. True if the puzzle used it. */
  bool key(int button);
  bool canUndo() const;
  bool canRedo() const;
  /** +1 solved, -1 lost, 0 playing. */
  int status() const;

  /** Finishes an animation the last move started (one refresh, not many). */
  void service();
  /** True once if the framebuffer changed since the last call, with the changed area. */
  bool takeDirty(GfxRenderer::DisplayRegion& changed);

  // The collection's drawing callbacks (PuzzleFrontend.cpp); public only for its C glue.
  void drawRect(int x, int y, int w, int h, int colour);
  void drawLine(int x1, int y1, int x2, int y2, int colour);
  void drawPolygon(const int* coords, int points, int fill, int outline);
  void drawCircle(int cx, int cy, int radius, int fill, int outline);
  void drawText(int x, int y, int fontSize, int align, int colour, const char* text);
  void setClip(int x, int y, int w, int h);
  void clearClip();
  void markUpdate(int x, int y, int w, int h);
  void timerOn() { timerActive = true; }
  void timerOff() { timerActive = false; }

 private:
  enum class Tone : uint8_t { Paper, Gray, Ink };
  enum class TextStyle : uint8_t { Bold, Regular, Error };

  GfxRenderer& renderer;
  const game& puzzle;
  midend* me = nullptr;
  float* palette = nullptr;
  int colours = 0;
  int boardX = 0, boardY = 0, boardW = 0, boardH = 0;
  int clipX0 = 0, clipY0 = 0, clipX1 = 0, clipY1 = 0;
  bool clipping = false;
  bool timerActive = false;
  bool dirty = false;
  int dirtyX0 = 0, dirtyY0 = 0, dirtyX1 = 0, dirtyY1 = 0;

  void loadPalette();
  Tone tone(int colour) const;
  TextStyle textStyle(int colour) const;
  void plot(int x, int y, Tone t);
  void fill(int x, int y, int w, int h, Tone t);
  void line(int x1, int y1, int x2, int y2, Tone t);
};

#endif  // INX_HAS_PUZZLES
