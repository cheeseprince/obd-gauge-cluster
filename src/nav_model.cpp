#include "nav_model.h"
#include "readouts.h"

namespace nav {

// Reading-order cursor step (defined below). Both the encoder cursor AND the Focus-view
// swipe use it, so nav works for ANY layout / enum order — no contiguity assumption.
// You can freely reorder, add, or deactivate stats in layout.h.
static void cursorStep(NavState& s, int dir);

// Return the StatId at quad page / cell by looking up the readout table.
// Falls back to StatId::Trans on a bad index (should never happen for valid inputs).
StatId statForCell(int page, int cell) {
  int idx = readoutAt(page, cell);
  return idx < 0 ? StatId::Trans : (StatId)idx;
}

// Return which quad page holds the given stat (0 if hidden/not found).
int quadPageForStat(StatId s) { return readoutPageOf((int)s); }

// Number of quad display pages, derived from the readout table.
static int pageCount() { return readoutPageCount(); }

void swipeLeft(NavState& s) {
  if (s.view == View::Quad) s.quadPage = (s.quadPage + 1) % pageCount();
  else cursorStep(s, +1);
}
void swipeRight(NavState& s) {
  if (s.view == View::Quad) s.quadPage = (s.quadPage + pageCount() - 1) % pageCount();
  else cursorStep(s, -1);
}

// Tap a quad cell: enter Focus showing that cell's stat.
void tapCell(NavState& s, int cell) {
  if (s.view != View::Quad) return;
  s.focus = statForCell(s.quadPage, cell);
  s.view  = View::Focus;
}

// Tap while in Focus: return to the quad page that holds the focused stat.
void tapBack(NavState& s) {
  s.view = View::Quad;
  s.quadPage = quadPageForStat(s.focus);
}

// Build the displayed readouts in (page, cell) reading order (page-major,
// cell-minor; hidden/empty cells skipped). Returns the count and fills order[].
// The encoder cursor walks THIS order, not StatId order, so the highlight moves
// the way tiles are laid out on screen — pages advance monotonically instead of
// bouncing when StatId order and visual order disagree (e.g. the Boost/Rpm swap).
static int readingOrder(int* order) {
  int n = 0;
  int pages = readoutPageCount();
  for (int p = 0; p < pages; p++)
    for (int c = 0; c < 4; c++) {
      int idx = readoutAt(p, c);
      if (idx >= 0 && n < STAT_COUNT) order[n++] = idx;   // bound vs order[STAT_COUNT] (guards a dup in PAGES)
    }
  return n;
}

// Step the cursor across displayed stats in reading order (dir = +1 next, -1 prev,
// wrapping). Keeps quadPage on the page holding the new focus.
static void cursorStep(NavState& s, int dir) {
  int order[STAT_COUNT];
  int n = readingOrder(order);
  if (n == 0) return;
  int pos = 0;
  for (int i = 0; i < n; i++) if (order[i] == (int)s.focus) { pos = i; break; }
  pos = (pos + dir + n) % n;
  s.focus = (StatId)order[pos];
  s.quadPage = quadPageForStat(s.focus);
}

// Encoder cursor: move the highlighted stat across all displayed stats in reading
// order (0..15 in screen layout, wrapping, never Baro), auto-paging as it goes.
void cursorNext(NavState& s) { cursorStep(s, +1); }
void cursorPrev(NavState& s) { cursorStep(s, -1); }

// Encoder short-press: in Quad, enter Focus on the highlighted stat; in Focus,
// return to the quad page holding it.
void press(NavState& s) {
  if (s.view == View::Quad) {
    s.view = View::Focus;
  } else {
    s.view = View::Quad;
    s.quadPage = quadPageForStat(s.focus);
  }
}

}  // namespace nav
