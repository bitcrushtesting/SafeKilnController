/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A self-contained canvas chart -- FR-WEB-06, FR-WEB-07, FR-WEB-08, FR-WEB-10.
 *
 * Hand-written rather than a library, which resolves OQ-04: the requirement is
 * two axes and a handful of series, and a dependency would cost more than the
 * entire asset budget of architecture 12.4 and bring a licence to audit with it.
 *
 * It draws the min/max band for the measured trace rather than a single line,
 * because that is the whole point of FR-LOG-11's extrema-preserving decimation:
 * at 800 points for a 24 h firing, a brief overshoot exists in the data and has
 * to be visible in the picture.
 */

const PAD = { l: 48, r: 44, t: 12, b: 24 };

function css(el, name, fallback) {
  const v = getComputedStyle(el).getPropertyValue(name).trim();
  return v || fallback;
}

function niceStep(span, target) {
  const raw = span / Math.max(1, target);
  const mag = Math.pow(10, Math.floor(Math.log10(raw)));
  for (const m of [1, 2, 2.5, 5, 10]) {
    if (raw <= m * mag) return m * mag;
  }
  return 10 * mag;
}

function fmtTime(s) {
  s = Math.max(0, Math.round(s));
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60);
  if (h > 0) return `${h}:${String(m).padStart(2, '0')}`;
  return `${m}m`;
}

export class Chart {
  constructor(canvas, opts = {}) {
    this.c = canvas;
    this.ctx = canvas.getContext('2d');
    this.interactive = opts.interactive !== false;

    this.cols = null;      // column-name -> index, from the API response
    this.rows = [];
    this.plan = [];        // FR-WEB-08: the intended remainder
    this.vis = { sp: true, plan: true, duty: false, cur: false, case: false };

    this.view = null;      // {t0,t1} when zoomed, else null for "everything"
    this.cursor = null;

    if (this.interactive) this._bindInput();
    this._ro = new ResizeObserver(() => this.draw());
    this._ro.observe(canvas);
  }

  /* The /api/log response, verbatim.  Indexing by the response's own `columns`
   * means the client is not hard-coding an array order it cannot verify. */
  setData(resp) {
    this.cols = {};
    (resp.columns || []).forEach((name, i) => { this.cols[name] = i; });
    this.rows = resp.series || [];
    this.draw();
  }

  /* Vertices from /api/programs/{id}/preview, offset to where the run is now. */
  setPlan(curve, offsetMs = 0) {
    this.plan = (curve || []).map(p => ({ t: p.t_s * 1000 + offsetMs, c: p.c }));
    this.draw();
  }

  setVisible(v) { Object.assign(this.vis, v); this.draw(); }
  resetZoom() { this.view = null; this.draw(); }

  _extent() {
    let t0 = Infinity, t1 = -Infinity;
    const ti = this.cols ? this.cols.t_rel_ms : 0;
    for (const r of this.rows) {
      if (r[ti] < t0) t0 = r[ti];
      if (r[ti] > t1) t1 = r[ti];
    }
    if (this.vis.plan) {
      for (const p of this.plan) {
        if (p.t < t0) t0 = p.t;
        if (p.t > t1) t1 = p.t;
      }
    }
    if (!isFinite(t0)) { t0 = 0; t1 = 1000; }
    if (t1 <= t0) t1 = t0 + 1000;
    return { t0, t1 };
  }

  _bindInput() {
    const c = this.c;
    let drag = null, pinch = null;

    const at = ev => {
      const r = c.getBoundingClientRect();
      return ev.clientX - r.left;
    };

    c.addEventListener('pointerdown', ev => {
      c.setPointerCapture(ev.pointerId);
      const v = this.view || this._extent();
      drag = { x: at(ev), t0: v.t0, t1: v.t1 };
    });
    c.addEventListener('pointermove', ev => {
      const x = at(ev);
      if (drag) {
        const w = c.clientWidth - PAD.l - PAD.r;
        const span = drag.t1 - drag.t0;
        const dt = -(x - drag.x) / Math.max(1, w) * span;
        this.view = { t0: drag.t0 + dt, t1: drag.t1 + dt };
        this.draw();
      } else {
        this.cursor = x;
        this.draw();
      }
    });
    const end = ev => {
      if (drag) { c.releasePointerCapture(ev.pointerId); drag = null; }
    };
    c.addEventListener('pointerup', end);
    c.addEventListener('pointercancel', end);
    c.addEventListener('pointerleave', () => { this.cursor = null; this.draw(); });

    c.addEventListener('wheel', ev => {
      ev.preventDefault();
      const v = this.view || this._extent();
      const w = c.clientWidth - PAD.l - PAD.r;
      const frac = Math.min(1, Math.max(0, (at(ev) - PAD.l) / Math.max(1, w)));
      const focus = v.t0 + (v.t1 - v.t0) * frac;
      const k = ev.deltaY > 0 ? 1.25 : 0.8;
      /* Zoom about the cursor, so the thing being looked at stays put. */
      this.view = { t0: focus - (focus - v.t0) * k, t1: focus + (v.t1 - focus) * k };
      this.draw();
    }, { passive: false });

    c.addEventListener('touchstart', ev => {
      if (ev.touches.length === 2) {
        const v = this.view || this._extent();
        pinch = { d: Math.abs(ev.touches[0].clientX - ev.touches[1].clientX), t0: v.t0, t1: v.t1 };
      }
    }, { passive: true });
    c.addEventListener('touchmove', ev => {
      if (pinch && ev.touches.length === 2) {
        const d = Math.abs(ev.touches[0].clientX - ev.touches[1].clientX);
        const k = Math.max(0.1, pinch.d / Math.max(1, d));
        const mid = (pinch.t0 + pinch.t1) / 2, half = (pinch.t1 - pinch.t0) / 2 * k;
        this.view = { t0: mid - half, t1: mid + half };
        this.draw();
      }
    }, { passive: true });
    c.addEventListener('touchend', () => { pinch = null; }, { passive: true });
  }

  draw() {
    const c = this.c, ctx = this.ctx;
    const dpr = window.devicePixelRatio || 1;
    const w = c.clientWidth || 600;
    const h = parseInt(c.getAttribute('height'), 10) || 300;

    if (c.width !== Math.round(w * dpr) || c.height !== Math.round(h * dpr)) {
      c.width = Math.round(w * dpr);
      c.height = Math.round(h * dpr);
    }
    c.style.height = h + 'px';
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);

    const col = {
      text: css(c, '--text', '#eee'),
      dim:  css(c, '--dim', '#999'),
      line: css(c, '--line', '#333'),
      acc:  css(c, '--accent', '#ff5f1f'),
      info: css(c, '--info', '#60a5fa'),
      ok:   css(c, '--ok', '#34d399'),
      warn: css(c, '--warn', '#fbbf24'),
      faint: css(c, '--faint', '#666'),
    };

    const px = { l: PAD.l, r: w - PAD.r, t: PAD.t, b: h - PAD.b };
    const plotW = px.r - px.l, plotH = px.b - px.t;

    if (!this.rows.length && !this.plan.length) {
      ctx.fillStyle = col.dim;
      ctx.font = '12px system-ui, sans-serif';
      ctx.textAlign = 'center';
      ctx.fillText('no logged data for this run', w / 2, h / 2);
      return;
    }

    const { t0, t1 } = this.view || this._extent();
    const ci = this.cols || {};
    const idx = n => (n in ci ? ci[n] : -1);

    /* Temperature axis from whatever is actually on screen, so zooming into a
     * dwell does not leave the trace as a flat line at the top. */
    let lo = Infinity, hi = -Infinity;
    const visible = [];
    for (const r of this.rows) {
      const t = r[idx('t_rel_ms')];
      if (t < t0 || t > t1) continue;
      visible.push(r);
      lo = Math.min(lo, r[idx('kiln_min_c')]);
      hi = Math.max(hi, r[idx('kiln_max_c')]);
      if (this.vis.sp) {
        lo = Math.min(lo, r[idx('sp_min_c')]);
        hi = Math.max(hi, r[idx('sp_max_c')]);
      }
      if (this.vis.case) {
        lo = Math.min(lo, r[idx('case_min_c')]);
        hi = Math.max(hi, r[idx('case_max_c')]);
      }
    }
    if (this.vis.plan) {
      for (const p of this.plan) {
        if (p.t < t0 || p.t > t1) continue;
        lo = Math.min(lo, p.c); hi = Math.max(hi, p.c);
      }
    }
    if (!isFinite(lo)) { lo = 0; hi = 100; }
    if (hi - lo < 10) { const m = (lo + hi) / 2; lo = m - 5; hi = m + 5; }
    const padY = (hi - lo) * 0.08;
    lo -= padY; hi += padY;

    const X = t => px.l + (t - t0) / (t1 - t0) * plotW;
    const Y = v => px.b - (v - lo) / (hi - lo) * plotH;
    /* The secondary axis, for the series whose units are not degrees. */
    const Y2 = (v, max) => px.b - Math.min(1, v / max) * plotH;

    ctx.font = '10px system-ui, sans-serif';
    ctx.lineWidth = 1;

    /* grid + left axis */
    const stepY = niceStep(hi - lo, 5);
    ctx.strokeStyle = col.line;
    ctx.fillStyle = col.dim;
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    for (let v = Math.ceil(lo / stepY) * stepY; v <= hi; v += stepY) {
      const y = Y(v);
      ctx.beginPath(); ctx.moveTo(px.l, y); ctx.lineTo(px.r, y); ctx.stroke();
      ctx.fillText(Math.round(v) + '°', px.l - 6, y);
    }

    /* time axis */
    const stepT = niceStep((t1 - t0) / 1000, 6) * 1000;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'top';
    for (let t = Math.ceil(t0 / stepT) * stepT; t <= t1; t += stepT) {
      const x = X(t);
      ctx.strokeStyle = col.line;
      ctx.beginPath(); ctx.moveTo(x, px.t); ctx.lineTo(x, px.b); ctx.stroke();
      ctx.fillStyle = col.dim;
      ctx.fillText(fmtTime(t / 1000), x, px.b + 4);
    }

    /* right axis label, when a secondary series is shown */
    if (this.vis.duty || this.vis.cur) {
      ctx.textAlign = 'left'; ctx.textBaseline = 'middle';
      ctx.fillStyle = col.faint;
      ctx.fillText(this.vis.duty ? '100%' : 'max', px.r + 6, Y2(1, 1));
      ctx.fillText('0', px.r + 6, px.b);
    }

    const band = (minKey, maxKey, fill) => {
      if (!visible.length) return;
      ctx.beginPath();
      visible.forEach((r, i) => {
        const x = X(r[idx('t_rel_ms')]), y = Y(r[idx(maxKey)]);
        i ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
      });
      for (let i = visible.length - 1; i >= 0; i--) {
        const r = visible[i];
        ctx.lineTo(X(r[idx('t_rel_ms')]), Y(r[idx(minKey)]));
      }
      ctx.closePath();
      ctx.fillStyle = fill;
      ctx.fill();
    };

    const line = (key, colour, width, dash, scale) => {
      if (!visible.length) return;
      ctx.beginPath();
      visible.forEach((r, i) => {
        const x = X(r[idx('t_rel_ms')]);
        const y = scale ? Y2(r[idx(key)], scale) : Y(r[idx(key)]);
        i ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
      });
      ctx.strokeStyle = colour;
      ctx.lineWidth = width;
      ctx.setLineDash(dash || []);
      ctx.stroke();
      ctx.setLineDash([]);
    };

    /* FR-WEB-08: the planned remainder, dashed, behind the actual trace. */
    if (this.vis.plan && this.plan.length > 1) {
      ctx.beginPath();
      this.plan.forEach((p, i) => {
        const x = X(p.t), y = Y(p.c);
        i ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
      });
      ctx.strokeStyle = col.faint;
      ctx.lineWidth = 1.5;
      ctx.setLineDash([4, 4]);
      ctx.stroke();
      ctx.setLineDash([]);
    }

    if (this.vis.case) line('case_max_c', col.ok, 1.2, [2, 3]);
    if (this.vis.duty) line('duty_max', col.warn, 1.2, [], 1000);
    if (this.vis.cur)  line('cur_max_a', col.info, 1.2, [], 60);
    if (this.vis.sp)   line('sp_max_c', col.info, 1.4, [6, 3]);

    /* The measured trace last, as a band plus its upper edge: the band is the
     * decimated min/max, which is where a brief excursion lives. */
    band('kiln_min_c', 'kiln_max_c', col.acc + '44');
    line('kiln_max_c', col.acc, 1.8);

    /* FR-WEB-10: a readout at the cursor. */
    if (this.cursor !== null && visible.length) {
      const t = t0 + (this.cursor - px.l) / plotW * (t1 - t0);
      let best = null, bd = Infinity;
      for (const r of visible) {
        const d = Math.abs(r[idx('t_rel_ms')] - t);
        if (d < bd) { bd = d; best = r; }
      }
      if (best) {
        const x = X(best[idx('t_rel_ms')]);
        ctx.strokeStyle = col.faint;
        ctx.setLineDash([2, 2]);
        ctx.beginPath(); ctx.moveTo(x, px.t); ctx.lineTo(x, px.b); ctx.stroke();
        ctx.setLineDash([]);

        const parts = [
          fmtTime(best[idx('t_rel_ms')] / 1000),
          `${best[idx('kiln_max_c')].toFixed(1)}°`,
        ];
        if (this.vis.sp)   parts.push(`sp ${best[idx('sp_max_c')].toFixed(1)}°`);
        if (this.vis.duty) parts.push(`${(best[idx('duty_max')] / 10).toFixed(0)}%`);
        if (this.vis.cur)  parts.push(`${best[idx('cur_max_a')].toFixed(1)}A`);
        if (this.vis.case) parts.push(`case ${best[idx('case_max_c')].toFixed(1)}°`);
        const label = parts.join('   ');

        ctx.font = '11px system-ui, sans-serif';
        const tw = ctx.measureText(label).width + 10;
        const bx = Math.min(px.r - tw, Math.max(px.l, x + 6));
        ctx.fillStyle = css(c, '--surface', '#111');
        ctx.fillRect(bx, px.t + 2, tw, 18);
        ctx.strokeStyle = col.line;
        ctx.strokeRect(bx, px.t + 2, tw, 18);
        ctx.fillStyle = col.text;
        ctx.textAlign = 'left'; ctx.textBaseline = 'middle';
        ctx.fillText(label, bx + 5, px.t + 11);
      }
    }
  }
}
