// The hero's slide to unlock, after SpringBoard's lock screen
// (servers/springboard): the knob follows the finger along the track, the
// label fades out over the first half of its travel, and letting go short of
// the end slides it back. Distances are in device pixels (240 across).
(function () {
  "use strict";

  var TRAVEL = 156;             // KNOB_TRAVEL in sb_internal.h
  var screen = document.getElementById("screen");
  var knob = document.getElementById("knob");
  var label = document.getElementById("label");
  var power = document.getElementById("power");
  if (!screen || !knob || !label || !power) return;

  var reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)");
  var travel = 0, startX = 0, startTravel = 0, dragging = false, moved = false;

  function setTravel(t) {
    travel = Math.max(0, Math.min(TRAVEL, t));
    knob.style.setProperty("--travel", travel);
    label.style.opacity = Math.max(0, 1 - travel * 2 / TRAVEL);
  }

  function devicePixels(cssPx) {
    return cssPx * 240 / screen.getBoundingClientRect().width;
  }

  function unlock() {
    setTravel(TRAVEL);
    screen.classList.add("unlocked");
    power.hidden = false;
    power.focus({ preventScroll: true });
  }

  function lock() {
    screen.classList.remove("unlocked");
    power.hidden = true;
    knob.classList.remove("settling");
    setTravel(0);
    knob.focus({ preventScroll: true });
  }

  // Animate the knob to `target`, then call `done`.
  function settle(target, done) {
    if (reduceMotion.matches) {
      setTravel(target);
      if (done) done();
      return;
    }
    knob.classList.add("settling");
    setTravel(target);
    knob.addEventListener("transitionend", function end() {
      knob.removeEventListener("transitionend", end);
      knob.classList.remove("settling");
      if (done) done();
    });
  }

  knob.addEventListener("pointerdown", function (e) {
    if (e.button !== 0) return;
    dragging = true;
    moved = false;
    startX = e.clientX;
    startTravel = travel;
    knob.classList.remove("settling");
    screen.classList.add("dragging");
    knob.setPointerCapture(e.pointerId);
  });

  knob.addEventListener("pointermove", function (e) {
    if (!dragging) return;
    var dx = devicePixels(e.clientX - startX);
    if (Math.abs(dx) > 2) moved = true;
    setTravel(startTravel + dx);
  });

  function release() {
    if (!dragging) return;
    dragging = false;
    screen.classList.remove("dragging");
    if (!moved) return;             // a tap; the click handler unlocks
    if (travel >= TRAVEL) unlock();
    else settle(0);
  }
  knob.addEventListener("pointerup", release);
  knob.addEventListener("pointercancel", release);

  // A tap, Enter or Space slides the knob across for you.
  knob.addEventListener("click", function () {
    if (moved) {
      moved = false;
      return;
    }
    settle(TRAVEL, unlock);
  });

  power.addEventListener("click", lock);
})();
