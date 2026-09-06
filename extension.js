import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import Gio from 'gi://Gio';
import Soup from 'gi://Soup';
import St from 'gi://St';
import Clutter from 'gi://Clutter';
import Cairo from 'cairo';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PanelMenu from 'resource:///org/gnome/shell/ui/panelMenu.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const CURSOR_API_URL = 'https://cursor.com/api/usage-summary';
const CURSOR_PAGE_URL = 'https://cursor.com/dashboard/usage';
const CLAUDE_API_URL = 'https://api.anthropic.com/api/oauth/usage';
const CLAUDE_PAGE_URL = 'https://claude.ai/settings/usage';
const CLAUDE_TOKEN_URL = 'https://console.anthropic.com/v1/oauth/token';
// Public client id the Claude Code CLI uses for its own OAuth flow.
const CLAUDE_CLIENT_ID = '9d1c250a-e61b-44d9-88ed-5944d1962f5e';
// Renew slightly early so a refresh mid-request cannot 401 us.
const CLAUDE_EXPIRY_SKEW_MS = 120 * 1000;
const CODEX_API_URL = 'https://chatgpt.com/backend-api/wham/usage';
const CODEX_PAGE_URL = 'https://chatgpt.com/codex';
const TRACK_WIDTH = 296;
const RING_SIZE = 16;
const RING_WIDTH = 2.5;

// Everything the menu needs to draw a provider lives in one place, so a block,
// its icon, its accent and its links can never drift apart.
const PROVIDERS = {
    cursor: {
        label: 'Cursor',
        icon: 'cursor-symbolic.svg',
        accent: '#7e8aff',
        page: CURSOR_PAGE_URL,
        meters: ['Auto', 'API'],
    },
    claude: {
        label: 'Claude',
        icon: 'claude-symbolic.svg',
        accent: '#d97757',
        page: CLAUDE_PAGE_URL,
        meters: ['5h', '7d'],
    },
    codex: {
        label: 'Codex',
        icon: 'codex-symbolic.svg',
        accent: '#57bf9e',
        page: CODEX_PAGE_URL,
        meters: ['Primary', 'Weekly'],
    },
};
const PROVIDER_IDS = ['cursor', 'claude', 'codex'];

// History backing the sparkline charts.
const HISTORY_MAX_POINTS = 720;
const HISTORY_MAX_AGE = 7 * 24 * 3600;
// Two refreshes landing seconds apart are one observation, not two.
const HISTORY_MERGE_WINDOW = 45;
const HISTORY_WRITE_INTERVAL = 60;
const GRAPH_MIN_SPAN = 15 * 60;

function severity(util) {
    if (util >= 90)
        return 'usage-critical';
    if (util >= 75)
        return 'usage-high';
    return 'usage-low';
}

function severityRgb(util) {
    if (util >= 90)
        return [0.88, 0.11, 0.14];
    if (util >= 75)
        return [1.0, 0.47, 0.0];
    return [0.2, 0.82, 0.48];
}

function colorRgb(c) {
    const scale = Math.max(c.red, c.green, c.blue) > 1 ? 255 : 1;
    return [c.red / scale, c.green / scale, c.blue / scale];
}

function hexRgb(hex) {
    const n = parseInt(hex.replace('#', ''), 16);
    return [((n >> 16) & 0xff) / 255, ((n >> 8) & 0xff) / 255, (n & 0xff) / 255];
}

function humanDuration(seconds) {
    const s = Math.max(0, Math.floor(seconds));
    if (s < 60)
        return `${s}s`;
    const mins = Math.round(s / 60);
    if (mins < 60)
        return `${mins}m`;
    const hrs = Math.floor(mins / 60);
    if (hrs < 24)
        return `${hrs}h ${mins % 60}m`;
    const days = Math.floor(hrs / 24);
    return `${days}d ${hrs % 24}h`;
}

function relativeReset(isoOrUnix) {
    let target;
    if (typeof isoOrUnix === 'number' && Number.isFinite(isoOrUnix))
        target = isoOrUnix * 1000;
    else
        target = Date.parse(isoOrUnix ?? '');
    if (Number.isNaN(target))
        return '';
    const diff = target - Date.now();
    if (diff <= 0)
        return 'resetting';
    return `resets in ${humanDuration(diff / 1000)}`;
}

function statusLabel(status) {
    if (status === 401 || status === 403)
        return 'Session expired';
    if (status === 404)
        return 'Not available';
    if (status === 429)
        return 'Rate limited';
    if (status >= 500)
        return 'Service down';
    return `HTTP ${status}`;
}

function planLabel(value, fallback = 'AI') {
    const raw = `${value ?? ''}`.trim();
    if (!raw)
        return fallback;
    const n = raw.toLowerCase().replace(/[\s_-]/g, '');
    const known = [
        ['ultra', 'ULTRA'], ['proplus', 'PRO+'], ['business', 'BUSINESS'],
        ['enterprise', 'ENT'], ['pro', 'PRO'], ['max', 'MAX'], ['team', 'TEAM'],
        ['student', 'STUDENT'], ['free', 'FREE'], ['go', 'GO'], ['plus', 'PLUS'],
    ];
    for (const [key, label] of known) {
        if (n.includes(key))
            return label;
    }
    return raw.toUpperCase();
}

function decodeJwtPayload(token) {
    try {
        const parts = `${token}`.split('.');
        if (parts.length < 2)
            return null;
        let payload = parts[1].replace(/-/g, '+').replace(/_/g, '/');
        while (payload.length % 4)
            payload += '=';
        return JSON.parse(new TextDecoder('utf-8').decode(GLib.base64_decode(payload)));
    } catch (_e) {
        return null;
    }
}

function sessionCookieFromToken(token) {
    const payload = decodeJwtPayload(token);
    if (!payload?.sub)
        return null;
    let sub = `${payload.sub}`;
    if (sub.includes('|'))
        sub = sub.split('|').pop();
    return `${sub}%3A%3A${token}`;
}

function centsLabel(cents) {
    if (typeof cents !== 'number' || !Number.isFinite(cents))
        return '-';
    return `$${(cents / 100).toFixed(2)}`;
}

function pct(value) {
    const n = typeof value === 'number' && Number.isFinite(value) ? value : 0;
    return Math.min(100, Math.max(0, n));
}

function pickPool(a, b, mode) {
    const known = p => (p && !p.missing ? p : null);
    const ka = known(a);
    const kb = known(b);
    if (mode === 'auto')
        return ka ?? kb;
    if (mode === 'api')
        return kb ?? ka;
    if (!ka)
        return kb;
    if (!kb)
        return ka;
    return (ka.utilization ?? 0) >= (kb.utilization ?? 0) ? ka : kb;
}

// Rolling record of every utilisation sample, so the menu can draw a real
// trend instead of a single instantaneous number. Kept in the cache dir: it is
// nice to have across restarts, and worthless enough to lose.
class History {
    constructor() {
        this._dir = GLib.build_filenamev([GLib.get_user_cache_dir(), 'ai-usage']);
        this._path = GLib.build_filenamev([this._dir, 'history.json']);
        this._data = {};
        for (const id of PROVIDER_IDS)
            this._data[id] = [];
        this._dirty = false;
        this._lastWrite = 0;
        this._load();
    }

    _load() {
        try {
            const [ok, contents] = GLib.file_get_contents(this._path);
            if (!ok)
                return;
            const parsed = JSON.parse(new TextDecoder('utf-8').decode(contents));
            for (const id of PROVIDER_IDS) {
                const list = Array.isArray(parsed?.[id]) ? parsed[id] : [];
                this._data[id] = list
                    .filter(p => p && Number.isFinite(p.t))
                    .map(p => ({
                        t: p.t,
                        a: Number.isFinite(p.a) ? p.a : null,
                        b: Number.isFinite(p.b) ? p.b : null,
                    }));
                this._prune(id);
            }
        } catch (_e) {
            // A missing or corrupt file just means we start collecting now.
        }
    }

    _prune(id) {
        const cutoff = Date.now() / 1000 - HISTORY_MAX_AGE;
        let list = this._data[id].filter(p => p.t >= cutoff);
        if (list.length > HISTORY_MAX_POINTS)
            list = list.slice(list.length - HISTORY_MAX_POINTS);
        this._data[id] = list;
    }

    push(id, a, b) {
        if (!this._data[id])
            return;
        const clean = v => (typeof v === 'number' && Number.isFinite(v)
            ? Math.max(0, Math.min(100, v))
            : null);
        const point = {t: Math.round(Date.now() / 1000), a: clean(a), b: clean(b)};
        if (point.a === null && point.b === null)
            return;
        const list = this._data[id];
        const last = list[list.length - 1];
        if (last && point.t - last.t < HISTORY_MERGE_WINDOW)
            list[list.length - 1] = point;
        else
            list.push(point);
        this._prune(id);
        this._dirty = true;
    }

    // Samples inside the requested window, plus the one just before it so the
    // line enters from the left edge instead of starting mid-chart.
    series(id, maxAgeSeconds) {
        const list = this._data[id] ?? [];
        const cutoff = Date.now() / 1000 - maxAgeSeconds;
        const from = list.findIndex(p => p.t >= cutoff);
        if (from < 0)
            return [];
        return list.slice(Math.max(0, from - 1));
    }

    flush(force = false) {
        const now = Date.now() / 1000;
        if (!this._dirty || (!force && now - this._lastWrite < HISTORY_WRITE_INTERVAL))
            return;
        try {
            GLib.mkdir_with_parents(this._dir, 0o700);
            const bytes = new TextEncoder().encode(JSON.stringify(this._data));
            Gio.File.new_for_path(this._path).replace_contents(
                bytes, null, false, Gio.FileCreateFlags.REPLACE_DESTINATION, null);
            this._dirty = false;
            this._lastWrite = now;
        } catch (_e) {
            // Losing history is not worth surfacing to the user.
        }
    }
}

class Meter {
    constructor(name) {
        this.root = new St.BoxLayout({vertical: true, style_class: 'cu-meter'});
        const row = new St.BoxLayout({style_class: 'cu-meter-row'});
        this._name = new St.Label({text: name, style_class: 'cu-meter-name'});
        this._caption = new St.Label({
            text: '',
            style_class: 'cu-caption',
            x_expand: true,
            y_align: Clutter.ActorAlign.CENTER,
        });
        this._pct = new St.Label({text: '…', style_class: 'cu-meter-pct'});
        row.add_child(this._name);
        row.add_child(this._caption);
        row.add_child(this._pct);
        this._track = new St.BoxLayout({style_class: 'cu-track', x_expand: true});
        this._fill = new St.Widget({style_class: 'cu-fill usage-low'});
        this._track.add_child(this._fill);
        this._ratio = 0;
        // The fill used to be sized from a hardcoded 260px constant, so it
        // drifted out of the track under any text-scaling factor.
        this._trackNotifyId = this._track.connect('notify::width', () => this._applyFill());
        this.root.add_child(row);
        this.root.add_child(this._track);
    }

    _applyFill() {
        const width = this._track.get_width() || TRACK_WIDTH;
        const filled = Math.round(this._ratio * width);
        // A sliver of colour still has to read as a rounded pill, so anything
        // above zero gets at least the width of the track's corner radius.
        this._fill.set_width(this._ratio > 0 ? Math.max(8, filled) : 0);
    }

    setValue(util, caption, displayValue = util, suffix = 'used') {
        const clamped = Math.max(0, Math.min(100, util));
        this._pct.text = `${displayValue.toFixed(0)}% ${suffix}`;
        this._pct.style_class = `cu-meter-pct ${severity(util)}`;
        this._ratio = clamped / 100;
        this._applyFill();
        this._fill.style_class = `cu-fill ${severity(util)}`;
        this._caption.text = caption ?? '';
        this._track.visible = true;
    }

    setMuted(detail = '-') {
        this._pct.text = detail;
        this._pct.style_class = 'cu-meter-pct';
        this._ratio = 0;
        this._applyFill();
        this._fill.style_class = 'cu-fill';
        this._caption.text = '';
        this._track.visible = false;
    }

    setVisible(v) {
        this.root.visible = v;
    }

    destroy() {
        if (this._trackNotifyId) {
            this._track.disconnect(this._trackNotifyId);
            this._trackNotifyId = null;
        }
        this.root?.destroy();
        this.root = null;
    }
}

// Time-series chart of the two pools of one provider: dashed gridlines, a
// filled primary series and a dashed secondary one, drawn straight in Cairo so
// it follows the shell theme's foreground colour.
const UsageGraph = GObject.registerClass(
class UsageGraph extends St.DrawingArea {
    _init(accent) {
        super._init({style_class: 'cu-graph', x_expand: true});
        this._accent = accent;
        this._points = [];
        this._invert = false;
    }

    setSeries(points, invert) {
        this._points = points ?? [];
        this._invert = !!invert;
        this.queue_repaint();
    }

    // Contiguous runs of known values: a pool that briefly reported nothing
    // leaves a gap in the line rather than a fabricated straight segment.
    _segments(key) {
        const segments = [];
        let run = [];
        for (const p of this._points) {
            const v = p[key];
            if (typeof v !== 'number' || !Number.isFinite(v)) {
                if (run.length)
                    segments.push(run);
                run = [];
                continue;
            }
            run.push({t: p.t, v: this._invert ? 100 - v : v});
        }
        if (run.length)
            segments.push(run);
        return segments;
    }

    vfunc_repaint() {
        const cr = this.get_context();
        try {
            this._draw(cr);
        } catch (_e) {
            // Never let a paint failure take the whole menu down.
        } finally {
            cr.$dispose();
        }
    }

    _draw(cr) {
        const [w, h] = this.get_surface_size();
        const [fr, fg, fb] = colorRgb(this.get_theme_node().get_foreground_color());
        const padTop = 5;
        const padBottom = 3;
        const plot = Math.max(1, h - padTop - padBottom);
        const yFor = v => padTop + (1 - Math.max(0, Math.min(100, v)) / 100) * plot;

        cr.setLineWidth(1);
        cr.setDash([3, 3], 0);
        cr.setSourceRGBA(fr, fg, fb, 0.11);
        for (const level of [25, 50, 75]) {
            const y = Math.floor(yFor(level)) + 0.5;
            cr.moveTo(0, y);
            cr.lineTo(w, y);
        }
        cr.stroke();
        cr.setDash([], 0);

        const baseline = Math.floor(yFor(0)) + 0.5;
        cr.setSourceRGBA(fr, fg, fb, 0.2);
        cr.moveTo(0, baseline);
        cr.lineTo(w, baseline);
        cr.stroke();

        if (this._points.length < 2)
            return;

        const first = this._points[0].t;
        const span = Math.max(Date.now() / 1000 - first, GRAPH_MIN_SPAN);
        // Keep the "now" marker inside the surface: a dot centred on the last
        // pixel would be sliced in half by the actor's edge.
        const right = Math.max(1, w - 5);
        const xFor = t => Math.max(0, Math.min(right, ((t - first) / span) * right));
        const [ar, ag, ab] = this._accent;

        // Secondary pool sits behind, dashed and dimmed.
        cr.setLineWidth(1.2);
        cr.setLineJoin(Cairo.LineJoin.ROUND);
        cr.setLineCap(Cairo.LineCap.ROUND);
        cr.setDash([2.5, 2.5], 0);
        cr.setSourceRGBA(ar, ag, ab, 0.55);
        for (const run of this._segments('b')) {
            if (run.length < 2)
                continue;
            run.forEach((p, i) => {
                const x = xFor(p.t);
                const y = yFor(p.v);
                if (i === 0)
                    cr.moveTo(x, y);
                else
                    cr.lineTo(x, y);
            });
            cr.stroke();
        }
        cr.setDash([], 0);

        const runs = this._segments('a');
        const gradient = new Cairo.LinearGradient(0, padTop, 0, h);
        gradient.addColorStopRGBA(0, ar, ag, ab, 0.30);
        gradient.addColorStopRGBA(1, ar, ag, ab, 0.02);
        for (const run of runs) {
            if (run.length < 2)
                continue;
            cr.moveTo(xFor(run[0].t), baseline);
            for (const p of run)
                cr.lineTo(xFor(p.t), yFor(p.v));
            cr.lineTo(xFor(run[run.length - 1].t), baseline);
            cr.closePath();
            cr.setSource(gradient);
            cr.fill();
        }

        cr.setLineWidth(1.8);
        cr.setSourceRGBA(ar, ag, ab, 1);
        for (const run of runs) {
            if (run.length < 2)
                continue;
            run.forEach((p, i) => {
                const x = xFor(p.t);
                const y = yFor(p.v);
                if (i === 0)
                    cr.moveTo(x, y);
                else
                    cr.lineTo(x, y);
            });
            cr.stroke();
        }

        // Marker on the newest reading so "where am I now" is unmistakable.
        const lastRun = runs[runs.length - 1];
        if (lastRun?.length) {
            const last = lastRun[lastRun.length - 1];
            const x = xFor(last.t);
            const y = yFor(last.v);
            cr.setSourceRGBA(ar, ag, ab, 0.25);
            cr.arc(x, y, 4.5, 0, 2 * Math.PI);
            cr.fill();
            cr.setSourceRGBA(ar, ag, ab, 1);
            cr.arc(x, y, 2.2, 0, 2 * Math.PI);
            cr.fill();
        }
    }
});

class ProviderBlock {
    constructor(id, extensionPath) {
        const info = PROVIDERS[id];
        this.root = new St.BoxLayout({vertical: true, style_class: 'cu-card'});

        const header = new St.BoxLayout({style_class: 'cu-header'});
        this._icon = new St.Icon({
            gicon: Gio.icon_new_for_string(
                GLib.build_filenamev([extensionPath, 'icons', info.icon])),
            style_class: 'cu-brand',
            icon_size: 16,
            y_align: Clutter.ActorAlign.CENTER,
            style: `color: ${info.accent};`,
        });
        this._title = new St.Label({
            text: info.label,
            style_class: 'cu-title',
            y_align: Clutter.ActorAlign.CENTER,
        });
        this._tier = new St.Label({
            text: '',
            style_class: 'cu-tier',
            y_align: Clutter.ActorAlign.CENTER,
        });
        this._headline = new St.Label({
            text: '…',
            style_class: 'cu-headline',
            x_expand: true,
            x_align: Clutter.ActorAlign.END,
            y_align: Clutter.ActorAlign.CENTER,
        });
        header.add_child(this._icon);
        header.add_child(this._title);
        header.add_child(this._tier);
        header.add_child(this._headline);
        this.root.add_child(header);

        this.meterA = new Meter(info.meters[0]);
        this.meterB = new Meter(info.meters[1]);
        this.root.add_child(this.meterA.root);
        this.root.add_child(this.meterB.root);

        this._graphBox = new St.BoxLayout({vertical: true, style_class: 'cu-graph-box'});
        this._graph = new UsageGraph(hexRgb(info.accent));
        this._graphEmpty = new St.Label({
            text: 'Collecting history…',
            style_class: 'cu-graph-empty',
            x_align: Clutter.ActorAlign.CENTER,
        });
        const legend = new St.BoxLayout({style_class: 'cu-legend'});
        const swatch = (cls, opacity) => {
            const w = new St.Widget({
                style_class: `cu-swatch ${cls}`,
                style: `background-color: ${info.accent};`,
                y_align: Clutter.ActorAlign.CENTER,
            });
            w.opacity = opacity;
            return w;
        };
        legend.add_child(swatch('cu-swatch-a', 255));
        legend.add_child(new St.Label({
            text: info.meters[0],
            style_class: 'cu-legend-label',
        }));
        legend.add_child(swatch('cu-swatch-b', 140));
        legend.add_child(new St.Label({
            text: info.meters[1],
            style_class: 'cu-legend-label',
        }));
        this._span = new St.Label({
            text: '',
            style_class: 'cu-legend-span',
            x_expand: true,
            x_align: Clutter.ActorAlign.END,
        });
        legend.add_child(this._span);
        this._legend = legend;
        this._graphBox.add_child(this._graph);
        this._graphBox.add_child(this._graphEmpty);
        this._graphBox.add_child(legend);
        this.root.add_child(this._graphBox);

        this._billing = new St.BoxLayout({style_class: 'cu-billing'});
        this._billingIcon = new St.Icon({
            icon_name: 'dialog-information-symbolic',
            style_class: 'cu-billing-icon',
            icon_size: 12,
            y_align: Clutter.ActorAlign.CENTER,
        });
        this._billingLabel = new St.Label({text: '', style_class: 'cu-billing-label'});
        this._billing.add_child(this._billingIcon);
        this._billing.add_child(this._billingLabel);
        this._billing.visible = false;
        this.root.add_child(this._billing);

        this._status = new St.BoxLayout({style_class: 'cu-status'});
        this._statusIcon = new St.Icon({
            icon_name: 'dialog-warning-symbolic',
            style_class: 'cu-status-icon',
            icon_size: 14,
            y_align: Clutter.ActorAlign.CENTER,
        });
        this._statusLabel = new St.Label({
            text: '',
            style_class: 'cu-status-label',
            y_align: Clutter.ActorAlign.CENTER,
        });
        this._status.add_child(this._statusIcon);
        this._status.add_child(this._statusLabel);
        this._status.visible = false;
        this.root.add_child(this._status);
    }

    setTier(text, visible) {
        this._tier.text = text ?? '';
        this._tier.visible = !!text && visible;
    }

    setHeadline(text, cls = '') {
        this._headline.text = text;
        this._headline.style_class = `cu-headline ${cls}`.trim();
    }

    // One clear line replaces the meters when a provider cannot be read, so a
    // login prompt never masquerades as 0% usage.
    setStatus(text) {
        this._statusLabel.text = text ?? '';
        this._status.visible = !!text;
        this.meterA.setVisible(!text);
        this.meterB.setVisible(!text);
        if (text) {
            this._graphBox.visible = false;
            this._billing.visible = false;
        }
    }

    setBilling(text) {
        this._billingLabel.text = text ?? '';
        this._billing.visible = !!text;
    }

    setGraph(points, invert, spanText, visible) {
        this._graphBox.visible = visible;
        if (!visible)
            return;
        const enough = points.length >= 2;
        this._graph.visible = enough;
        this._graphEmpty.visible = !enough;
        this._legend.visible = enough;
        this._span.text = spanText ?? '';
        if (enough)
            this._graph.setSeries(points, invert);
    }

    setVisible(v) {
        this.root.visible = v;
    }

    destroy() {
        this.meterA?.destroy();
        this.meterB?.destroy();
        this.root?.destroy();
        this.root = null;
    }
}

const Ring = GObject.registerClass(
class Ring extends St.DrawingArea {
    _init() {
        super._init({
            style_class: 'cu-ring',
            width: RING_SIZE,
            height: RING_SIZE,
            y_align: Clutter.ActorAlign.CENTER,
        });
        this._util = null;
        this._color = null;
    }

    setValue(util) {
        this._util = Math.max(0, Math.min(100, util));
        this._color = severityRgb(util);
        this.queue_repaint();
    }

    setUnknown() {
        this._util = null;
        this._color = null;
        this.queue_repaint();
    }

    vfunc_repaint() {
        const cr = this.get_context();
        try {
            const [w, h] = this.get_surface_size();
            const cx = w / 2;
            const cy = h / 2;
            const radius = Math.min(w, h) / 2 - RING_WIDTH / 2;
            cr.setLineWidth(RING_WIDTH);
            cr.setLineCap(Cairo.LineCap.ROUND);
            const [fr, fg, fb] = colorRgb(this.get_theme_node().get_foreground_color());
            if (this._util === null) {
                // Nothing read yet: a dashed ring says "no value" instead of
                // showing a convincing empty gauge.
                cr.setDash([1.6, 2.4], 0);
                cr.setSourceRGBA(fr, fg, fb, 0.35);
                cr.arc(cx, cy, radius, 0, 2 * Math.PI);
                cr.stroke();
                cr.setDash([], 0);
                return;
            }
            cr.setSourceRGBA(fr, fg, fb, 0.22);
            cr.arc(cx, cy, radius, 0, 2 * Math.PI);
            cr.stroke();
            if (this._util > 0) {
                const [r, g, b] = this._color ?? severityRgb(this._util);
                cr.setSourceRGBA(r, g, b, 1);
                cr.arc(cx, cy, radius, -Math.PI / 2,
                    -Math.PI / 2 + (this._util / 100) * 2 * Math.PI);
                cr.stroke();
            }
        } catch (_e) {
            // A failed paint must not take the panel down.
        } finally {
            cr.$dispose();
        }
    }
});

const CursorUsageIndicator = GObject.registerClass(
class CursorUsageIndicator extends PanelMenu.Button {
    _init(extensionPath, settings, openPreferences) {
        super._init(0.0, 'AI Usage');
        this._extensionPath = extensionPath;
        this._settings = settings;
        this._openPreferences = openPreferences;
        this._session = this._createSession();
        this._history = new History();
        this._last = {cursor: null, claude: null, codex: null};
        this._countdownTimer = null;
        this._timerId = null;
        this._settingsChangedId = null;
        this._cancellable = null;
        this._tokenProc = null;
        this._pending = 0;
        this._claudeRenewing = false;

        const box = new St.BoxLayout({style_class: 'cu-panel'});
        // A symbolic SVG recolours with the panel theme; the old PNG stayed a
        // fixed colour and looked foreign next to the other status icons.
        this._icon = new St.Icon({
            gicon: Gio.icon_new_for_string(
                GLib.build_filenamev([this._extensionPath, 'icons', 'ai-usage-symbolic.svg'])),
            style_class: 'cu-panel-icon system-status-icon',
            icon_size: 14,
            y_align: Clutter.ActorAlign.CENTER,
        });
        this._ring = new Ring();
        this._label = new St.Label({
            text: '...',
            y_align: Clutter.ActorAlign.CENTER,
            style_class: 'cu-panel-pct',
        });
        this._panelTier = new St.Label({
            text: '',
            y_align: Clutter.ActorAlign.CENTER,
            style_class: 'cu-panel-tier',
        });
        box.add_child(this._icon);
        box.add_child(this._ring);
        box.add_child(this._label);
        box.add_child(this._panelTier);
        this.add_child(box);

        this._createMenu();
        this._applyPanelChrome();

        this._settingsChangedId = this._settings.connect('changed', (_s, key) => {
            if (key === 'refresh-interval')
                this._restartTimer();
            else if (key === 'proxy-url')
                this._recreateSession();
            else if (key === 'show-icon' || key === 'show-tier' || key === 'display-mode') {
                this._applyPanelChrome();
                // The tier badge lives in both the panel and every card.
                if (key === 'show-tier')
                    this._renderAll();
            } else if (
                key === 'usage-display' || key === 'panel-window' || key === 'panel-provider' ||
                key === 'show-billing' || key === 'show-graph' || key === 'graph-hours' ||
                key === 'show-cursor' || key === 'show-claude' || key === 'show-codex'
            ) {
                this._applyProviderVisibility();
                this._renderAll();
                if (key.startsWith('show-'))
                    this._refreshUsage();
            }
        });

        this.menu.connectObject('open-state-changed', (_menu, open) => {
            if (open)
                this._refreshUsage();
        }, this);

        this._refreshUsage();
        this._startTimer();
    }

    _cancelPending() {
        if (this._cancellable && !this._cancellable.is_cancelled())
            this._cancellable.cancel();
        this._cancellable = null;
        if (this._tokenProc) {
            try {
                this._tokenProc.force_exit();
            } catch (_e) {
                // ignore
            }
            this._tokenProc = null;
        }
        this._claudeRenewing = false;
    }

    _newCancellable() {
        this._cancelPending();
        this._cancellable = new Gio.Cancellable();
        return this._cancellable;
    }

    _applyPanelChrome() {
        const mode = this._settings.get_string('display-mode');
        this._icon.visible = this._settings.get_boolean('show-icon');
        this._ring.visible = mode === 'ring';
        this._panelTier.visible = this._settings.get_boolean('show-tier');
        this._renderPanel();
    }

    _applyProviderVisibility() {
        for (const id of PROVIDER_IDS)
            this._blocks[id].setVisible(this._settings.get_boolean(`show-${id}`));
    }

    _createSession() {
        const session = new Soup.Session();
        const proxyUrl = this._settings.get_string('proxy-url').trim();
        if (proxyUrl !== '')
            session.set_proxy_resolver(Gio.SimpleProxyResolver.new(proxyUrl, null));
        return session;
    }

    _recreateSession() {
        this._cancelPending();
        this._session?.abort();
        this._session = this._createSession();
        this._refreshUsage();
    }

    _createMenu() {
        const item = new PopupMenu.PopupBaseMenuItem({reactive: false, can_focus: false});
        const root = new St.BoxLayout({vertical: true, style_class: 'cu-popup'});
        item.add_child(root);
        this.menu.addMenuItem(item);

        this._blocks = {};
        for (const id of PROVIDER_IDS) {
            const block = new ProviderBlock(id, this._extensionPath);
            this._blocks[id] = block;
            root.add_child(block.root);
        }
        this._applyProviderVisibility();

        const footer = new St.BoxLayout({style_class: 'cu-footer'});
        this._updatedIcon = new St.Icon({
            icon_name: 'view-refresh-symbolic',
            style_class: 'cu-updated-icon',
            icon_size: 12,
            y_align: Clutter.ActorAlign.CENTER,
        });
        this._updated = new St.Label({
            text: '',
            style_class: 'cu-updated',
            x_expand: true,
            y_align: Clutter.ActorAlign.CENTER,
        });
        footer.add_child(this._updatedIcon);
        footer.add_child(this._updated);

        // Icon + label buttons: the same targets as before, but they now read
        // as buttons rather than stray words under the meters.
        const mkLink = (label, iconName, fn) => {
            const box = new St.BoxLayout({style_class: 'cu-link-box'});
            box.add_child(new St.Icon({
                icon_name: iconName,
                icon_size: 12,
                style_class: 'cu-link-icon',
                y_align: Clutter.ActorAlign.CENTER,
            }));
            box.add_child(new St.Label({
                text: label,
                y_align: Clutter.ActorAlign.CENTER,
            }));
            const btn = new St.Button({
                child: box,
                style_class: 'cu-link',
                can_focus: true,
                reactive: true,
                track_hover: true,
                accessible_name: label,
            });
            btn.connect('clicked', fn);
            footer.add_child(btn);
            return btn;
        };
        mkLink('Refresh', 'view-refresh-symbolic', () => this._refreshUsage());
        mkLink('Open', 'web-browser-symbolic', () => {
            this.menu.close();
            const p = this._settings.get_string('panel-provider');
            Gio.AppInfo.launch_default_for_uri(
                PROVIDERS[p]?.page ?? CURSOR_PAGE_URL, null);
        });
        mkLink('Prefs', 'preferences-system-symbolic', () => {
            this.menu.close();
            this._openPreferences();
        });
        root.add_child(footer);
    }

    _startTimer() {
        this._timerId = GLib.timeout_add_seconds(
            GLib.PRIORITY_DEFAULT,
            this._settings.get_int('refresh-interval'),
            () => {
                this._refreshUsage();
                return GLib.SOURCE_CONTINUE;
            }
        );
    }

    _stopTimer() {
        if (this._timerId) {
            GLib.source_remove(this._timerId);
            this._timerId = null;
        }
    }

    _restartTimer() {
        this._stopTimer();
        this._startTimer();
    }

    _refreshUsage() {
        const cancellable = this._newCancellable();
        this._pending = 0;
        this._markRefreshing();
        if (this._settings.get_boolean('show-cursor')) {
            this._pending++;
            this._refreshCursor(cancellable);
        }
        if (this._settings.get_boolean('show-claude')) {
            this._pending++;
            this._refreshClaude(cancellable);
        }
        if (this._settings.get_boolean('show-codex')) {
            this._pending++;
            this._refreshCodex(cancellable);
        }
        if (this._pending === 0) {
            this._label.text = '-';
            this._ring.setUnknown();
            this._stamp(false);
        }
    }

    _doneOne() {
        this._pending = Math.max(0, this._pending - 1);
        if (this._pending === 0)
            this._stamp(true);
    }

    // --- Cursor ---

    _cliAuthPath() {
        return GLib.build_filenamev([GLib.get_home_dir(), '.config', 'cursor', 'auth.json']);
    }

    _desktopDbPath() {
        return GLib.build_filenamev([
            GLib.get_home_dir(), '.config', 'Cursor', 'User', 'globalStorage', 'state.vscdb',
        ]);
    }

    _refreshCursor(cancellable) {
        const envToken = (GLib.getenv('CURSOR_SESSION_TOKEN') ?? '').trim();
        if (envToken) {
            const token = envToken.includes('::') || envToken.includes('%3A%3A')
                ? envToken.split(/::|%3A%3A/).pop()
                : envToken;
            this._fetchCursor(token, cancellable);
            return;
        }
        const authFile = Gio.File.new_for_path(this._cliAuthPath());
        if (authFile.query_exists(null)) {
            authFile.load_contents_async(cancellable, (file, result) => {
                try {
                    const [, contents] = file.load_contents_finish(result);
                    const auth = JSON.parse(new TextDecoder('utf-8').decode(contents));
                    const token = auth.accessToken ?? auth.access_token ?? null;
                    if (!token) {
                        this._tryDesktopToken(cancellable);
                        return;
                    }
                    this._fetchCursor(token, cancellable);
                } catch (_e) {
                    if (cancellable.is_cancelled())
                        return;
                    this._tryDesktopToken(cancellable);
                }
            });
            return;
        }
        this._tryDesktopToken(cancellable);
    }

    _tryDesktopToken(cancellable) {
        const dbPath = this._desktopDbPath();
        if (!Gio.File.new_for_path(dbPath).query_exists(null)) {
            this._setProviderUnavailable('cursor', 'Login required');
            this._doneOne();
            return;
        }
        try {
            const proc = Gio.Subprocess.new(
                [
                    'sqlite3', dbPath,
                    "SELECT value FROM ItemTable WHERE key='cursorAuth/accessToken' LIMIT 1;",
                ],
                Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_PIPE
            );
            this._tokenProc = proc;
            proc.communicate_utf8_async(null, cancellable, (_proc, result) => {
                this._tokenProc = null;
                try {
                    const [, stdout] = proc.communicate_utf8_finish(result);
                    const token = (stdout ?? '').trim();
                    if (!token) {
                        this._setProviderUnavailable('cursor', 'No auth');
                        this._doneOne();
                        return;
                    }
                    this._fetchCursor(token, cancellable);
                } catch (_e) {
                    if (cancellable.is_cancelled())
                        return;
                    this._setProviderUnavailable('cursor', 'No auth');
                    this._doneOne();
                }
            });
        } catch (_e) {
            this._setProviderUnavailable('cursor', 'No auth');
            this._doneOne();
        }
    }

    _fetchCursor(accessToken, cancellable) {
        const cookie = sessionCookieFromToken(accessToken);
        if (!cookie) {
            this._setProviderUnavailable('cursor', 'Bad token');
            this._doneOne();
            return;
        }
        this._httpGet(CURSOR_API_URL, {
            Cookie: `WorkosCursorSessionToken=${cookie}`,
            'User-Agent': 'cursor-usage-extension',
        }, cancellable, (ok, data, err) => {
            if (!ok) {
                this._setProviderUnavailable('cursor', err);
                this._doneOne();
                return;
            }
            this._last.cursor = this._normalizeCursor(data);
            this._recordHistory('cursor');
            this._renderProvider('cursor');
            this._renderPanel();
            this._scheduleCountdown();
            this._doneOne();
        });
    }

    _normalizeCursor(data) {
        const plan = data.individualUsage?.plan ?? {};
        const onDemand = data.individualUsage?.onDemand ?? null;
        const end = data.billingCycleEnd ?? null;
        const fromMsg = msg => {
            const m = `${msg ?? ''}`.match(/([\d.]+)\s*%/);
            return m ? Number(m[1]) : null;
        };
        const auto = pct(plan.autoPercentUsed ?? fromMsg(data.autoModelSelectedDisplayMessage));
        const api = pct(plan.apiPercentUsed ?? fromMsg(data.namedModelSelectedDisplayMessage));
        const total = pct(plan.totalPercentUsed ?? Math.max(auto, api));
        return {
            id: 'cursor',
            tier: planLabel(data.membershipType, 'CURSOR'),
            isUnlimited: !!data.isUnlimited,
            a: {name: 'Auto', utilization: auto, resets_at: end},
            b: {name: 'API', utilization: api, resets_at: end},
            total: {utilization: total, resets_at: end},
            billing: (() => {
                if (!this._settings.get_boolean('show-billing'))
                    return '';
                const parts = [];
                if (plan.enabled && plan.breakdown?.included != null) {
                    let t = centsLabel(plan.breakdown.included);
                    if (plan.breakdown.bonus)
                        t += ` + ${centsLabel(plan.breakdown.bonus)} bonus`;
                    parts.push(t);
                }
                if (onDemand?.enabled) {
                    const lim = onDemand.limit == null ? '∞' : centsLabel(onDemand.limit);
                    parts.push(`on-demand ${centsLabel(onDemand.used ?? 0)} / ${lim}`);
                }
                return parts.join(' · ');
            })(),
        };
    }

    // --- Claude ---

    _claudeCredPath() {
        return GLib.build_filenamev([GLib.get_home_dir(), '.claude', '.credentials.json']);
    }

    _refreshClaude(cancellable) {
        const env = (GLib.getenv('CLAUDE_CODE_OAUTH_TOKEN') ?? '').trim();
        if (env) {
            this._fetchClaude(env, planLabel(null, 'CLAUDE'), cancellable);
            return;
        }
        const file = Gio.File.new_for_path(this._claudeCredPath());
        if (!file.query_exists(null)) {
            this._setProviderUnavailable('claude', 'Run claude to sign in');
            this._doneOne();
            return;
        }
        file.load_contents_async(cancellable, (_f, result) => {
            try {
                const [, contents] = file.load_contents_finish(result);
                const auth = JSON.parse(new TextDecoder('utf-8').decode(contents));
                const oauth = auth.claudeAiOauth ?? auth ?? null;
                const token = oauth?.accessToken ?? null;
                if (!token) {
                    this._setProviderUnavailable('claude', 'No OAuth');
                    this._doneOne();
                    return;
                }
                const tier = planLabel(oauth.subscriptionType, 'CLAUDE');
                const expired = typeof oauth.expiresAt === 'number' &&
                    oauth.expiresAt > 0 &&
                    oauth.expiresAt - CLAUDE_EXPIRY_SKEW_MS <= Date.now();
                // A stored token that has aged out used to leave the menu stuck
                // on "HTTP 401" until the user next ran the CLI by hand.
                if (expired && oauth.refreshToken) {
                    this._renewClaudeToken(auth, cancellable, fresh => {
                        if (cancellable.is_cancelled())
                            return;
                        // If the renewal could not happen -- clock skew, or the
                        // token endpoint briefly unreachable -- the stored token
                        // may well still be accepted, so try it rather than
                        // declaring the session dead.
                        // No retry flag: a renewal has just been attempted, so
                        // a rejection here is a genuinely dead session.
                        this._fetchClaude(fresh ?? token, tier, cancellable, false);
                    });
                    return;
                }
                this._fetchClaude(token, tier, cancellable, !expired);
            } catch (_e) {
                if (cancellable.is_cancelled())
                    return;
                this._setProviderUnavailable('claude', 'No auth');
                this._doneOne();
            }
        });
    }

    // Exchanges the stored refresh token for a new access token and saves the
    // rotated pair back. The server invalidates the old refresh token as soon
    // as it answers, so if the write fails we deliberately discard the new
    // token rather than silently log the CLI out on its next start.
    _renewClaudeToken(auth, cancellable, cb) {
        if (this._claudeRenewing) {
            cb(null, 'Refreshing');
            return;
        }
        const oauth = auth.claudeAiOauth ?? auth;
        this._claudeRenewing = true;
        const done = (token, err) => {
            this._claudeRenewing = false;
            cb(token, err);
        };

        const message = Soup.Message.new('POST', CLAUDE_TOKEN_URL);
        const payload = JSON.stringify({
            grant_type: 'refresh_token',
            refresh_token: oauth.refreshToken,
            client_id: CLAUDE_CLIENT_ID,
        });
        message.set_request_body_from_bytes(
            'application/json',
            new GLib.Bytes(new TextEncoder().encode(payload))
        );
        message.request_headers.append('Accept', 'application/json');
        message.request_headers.append('User-Agent', 'claude-code/2.1.72');

        this._session.send_and_read_async(
            message,
            GLib.PRIORITY_DEFAULT,
            cancellable,
            (session, result) => {
                try {
                    const bytes = session.send_and_read_finish(result);
                    if (message.status_code !== 200) {
                        done(null, message.status_code === 400 || message.status_code === 401
                            ? 'Session expired'
                            : `HTTP ${message.status_code}`);
                        return;
                    }
                    const data = JSON.parse(
                        new TextDecoder('utf-8').decode(bytes.get_data()));
                    if (!data.access_token) {
                        done(null, 'Refresh rejected');
                        return;
                    }
                    const next = {...oauth, accessToken: data.access_token};
                    if (data.refresh_token)
                        next.refreshToken = data.refresh_token;
                    next.expiresAt = Date.now() +
                        (Number(data.expires_in) || 8 * 3600) * 1000;

                    const updated = auth.claudeAiOauth
                        ? {...auth, claudeAiOauth: next}
                        : next;
                    if (!this._writeClaudeCreds(updated)) {
                        done(null, 'Cannot save token');
                        return;
                    }
                    done(data.access_token, null);
                } catch (_e) {
                    if (cancellable.is_cancelled())
                        return;
                    done(null, 'Refresh failed');
                }
            }
        );
    }

    _writeClaudeCreds(auth) {
        try {
            const path = this._claudeCredPath();
            const file = Gio.File.new_for_path(path);
            const bytes = new TextEncoder().encode(`${JSON.stringify(auth, null, 2)}\n`);
            const [ok] = file.replace_contents(
                bytes, null, false, Gio.FileCreateFlags.PRIVATE, null);
            if (!ok)
                return false;
            // Keep the file owner-only, the way the CLI writes it.
            file.set_attribute_uint32(
                'unix::mode', 0o600, Gio.FileQueryInfoFlags.NONE, null);
            return true;
        } catch (_e) {
            return false;
        }
    }

    _fetchClaude(token, tier, cancellable, mayRetry = false) {
        this._httpGet(CLAUDE_API_URL, {
            Authorization: `Bearer ${token}`,
            'anthropic-beta': 'oauth-2025-04-20',
            'User-Agent': 'claude-code/2.1.72',
            'Content-Type': 'application/json',
        }, cancellable, (ok, data, err, status) => {
            if (!ok) {
                // The token can be refused before its recorded expiry when it
                // has been revoked or the clock is skewed; one forced renewal
                // turns that into a working request.
                if (mayRetry && (status === 401 || status === 403)) {
                    this._retryClaudeAfterRefresh(tier, cancellable, err);
                    return;
                }
                this._setProviderUnavailable('claude', err);
                this._doneOne();
                return;
            }
            this._last.claude = this._normalizeClaude(data, tier);
            this._recordHistory('claude');
            this._renderProvider('claude');
            this._renderPanel();
            this._scheduleCountdown();
            this._doneOne();
        });
    }

    _retryClaudeAfterRefresh(tier, cancellable, previousError) {
        let auth = null;
        try {
            const [ok, contents] = GLib.file_get_contents(this._claudeCredPath());
            if (ok)
                auth = JSON.parse(new TextDecoder('utf-8').decode(contents));
        } catch (_e) {
            auth = null;
        }
        const oauth = auth?.claudeAiOauth ?? auth;
        if (!oauth?.refreshToken) {
            this._setProviderUnavailable('claude', previousError);
            this._doneOne();
            return;
        }
        this._renewClaudeToken(auth, cancellable, (fresh, err) => {
            if (fresh) {
                this._fetchClaude(fresh, tier, cancellable);
                return;
            }
            if (cancellable.is_cancelled())
                return;
            this._setProviderUnavailable('claude', err ?? previousError);
            this._doneOne();
        });
    }

    _normalizeClaude(data, tier) {
        const five = data.five_hour ?? null;
        const seven = data.seven_day ?? null;
        let billing = '';
        if (this._settings.get_boolean('show-billing') && data.extra_usage?.is_enabled) {
            const u = data.extra_usage;
            billing = `extra ${pct(u.utilization).toFixed(0)}%`;
            if (u.used_credits != null && u.monthly_limit != null)
                billing = `extra ${u.used_credits} / ${u.monthly_limit}`;
        }
        return {
            id: 'claude',
            tier,
            isUnlimited: false,
            a: {
                name: '5h',
                utilization: pct(five?.utilization),
                resets_at: five?.resets_at ?? null,
                missing: five?.utilization == null,
            },
            b: {
                name: '7d',
                utilization: pct(seven?.utilization),
                resets_at: seven?.resets_at ?? null,
                missing: seven?.utilization == null,
            },
            total: {
                utilization: Math.max(pct(five?.utilization), pct(seven?.utilization)),
                resets_at: seven?.resets_at ?? five?.resets_at ?? null,
                missing: five?.utilization == null && seven?.utilization == null,
            },
            billing,
        };
    }

    // --- Codex ---

    _codexAuthPath() {
        return GLib.build_filenamev([GLib.get_home_dir(), '.codex', 'auth.json']);
    }

    _refreshCodex(cancellable) {
        const file = Gio.File.new_for_path(this._codexAuthPath());
        if (!file.query_exists(null)) {
            this._setProviderUnavailable('codex', 'Login required');
            this._doneOne();
            return;
        }
        file.load_contents_async(cancellable, (_f, result) => {
            try {
                const [, contents] = file.load_contents_finish(result);
                const auth = JSON.parse(new TextDecoder('utf-8').decode(contents));
                const token = auth.tokens?.access_token ?? auth.access_token ?? null;
                const accountId = auth.tokens?.account_id ?? auth.account_id ?? null;
                if (!token) {
                    this._setProviderUnavailable('codex', 'No auth');
                    this._doneOne();
                    return;
                }
                this._fetchCodex(token, accountId, cancellable);
            } catch (_e) {
                if (cancellable.is_cancelled())
                    return;
                this._setProviderUnavailable('codex', 'No auth');
                this._doneOne();
            }
        });
    }

    _fetchCodex(token, accountId, cancellable) {
        const headers = {
            Authorization: `Bearer ${token}`,
            Accept: 'application/json',
            'User-Agent': 'cursor-usage-extension',
        };
        if (accountId)
            headers['ChatGPT-Account-Id'] = accountId;
        this._httpGet(CODEX_API_URL, headers, cancellable, (ok, data, err) => {
            if (!ok) {
                this._setProviderUnavailable('codex', err);
                this._doneOne();
                return;
            }
            this._last.codex = this._normalizeCodex(data);
            this._recordHistory('codex');
            this._renderProvider('codex');
            this._renderPanel();
            this._scheduleCountdown();
            this._doneOne();
        });
    }

    _normalizeCodex(data) {
        const primary = data.rate_limit?.primary_window ?? null;
        const secondary = data.rate_limit?.secondary_window ?? null;
        const toPool = (win, name) => {
            if (!win)
                return null;
            return {
                name,
                utilization: pct(win.used_percent),
                resets_at: typeof win.reset_at === 'number' ? win.reset_at : null,
            };
        };
        const a = toPool(primary, 'Primary') ??
            {name: 'Primary', utilization: 0, resets_at: null, missing: true};
        const b = toPool(secondary, 'Weekly');
        let billing = '';
        if (this._settings.get_boolean('show-billing') && data.credits?.has_credits) {
            if (data.credits.unlimited)
                billing = 'credits unlimited';
            else if (data.credits.balance != null)
                billing = `credits $${Number(data.credits.balance).toFixed(2)}`;
        }
        return {
            id: 'codex',
            tier: planLabel(data.plan_type, 'CODEX'),
            isUnlimited: !!data.credits?.unlimited,
            a,
            b: b ?? {name: 'Weekly', utilization: 0, resets_at: null, missing: true},
            total: {
                utilization: Math.max(a.utilization, b?.utilization ?? 0),
                resets_at: b?.resets_at ?? a.resets_at,
                missing: !!a.missing && (b?.missing ?? true),
            },
            billing,
        };
    }

    // --- HTTP ---

    _httpGet(url, headers, cancellable, cb) {
        const message = Soup.Message.new('GET', url);
        for (const [k, v] of Object.entries(headers))
            message.request_headers.append(k, v);
        this._session.send_and_read_async(
            message,
            GLib.PRIORITY_DEFAULT,
            cancellable,
            (session, result) => {
                try {
                    const bytes = session.send_and_read_finish(result);
                    if (message.status_code !== 200) {
                        cb(false, null, statusLabel(message.status_code),
                            message.status_code);
                        return;
                    }
                    const data = JSON.parse(new TextDecoder('utf-8').decode(bytes.get_data()));
                    cb(true, data, null, 200);
                } catch (_e) {
                    if (cancellable.is_cancelled())
                        return;
                    cb(false, null, 'API failed', 0);
                }
            }
        );
    }

    // --- Render ---

    _blockFor(id) {
        return this._blocks[id];
    }

    _setProviderUnavailable(id, detail) {
        this._last[id] = null;
        const block = this._blockFor(id);
        block.setTier('', false);
        block.setHeadline('—');
        block.setBilling('');
        block.setStatus(detail || 'Unavailable');
        this._renderPanel();
    }

    _displayValue(util) {
        return this._settings.get_string('usage-display') === 'remaining'
            ? 100 - util
            : util;
    }

    _applyMeter(meter, win) {
        if (!win || win.missing) {
            meter.setVisible(false);
            return;
        }
        meter.setVisible(true);
        const util = pct(win.utilization);
        const suffix = this._settings.get_string('usage-display') === 'remaining'
            ? 'left'
            : 'used';
        meter.setValue(util, relativeReset(win.resets_at), this._displayValue(util), suffix);
    }

    _renderProvider(id) {
        const data = this._last[id];
        const block = this._blockFor(id);
        if (!data) {
            this._setProviderUnavailable(id, '—');
            return;
        }
        block.setStatus('');
        block.setTier(data.tier ?? '', this._settings.get_boolean('show-tier'));
        if (data.isUnlimited) {
            block.setHeadline('∞', 'usage-low');
            block.meterA.setVisible(true);
            block.meterB.setVisible(true);
            block.meterA.setMuted('unlimited');
            block.meterB.setMuted('unlimited');
        } else {
            const headline = data.total && !data.total.missing
                ? pct(data.total.utilization)
                : Math.max(pct(data.a?.utilization), pct(data.b?.utilization));
            block.setHeadline(
                `${Math.round(this._displayValue(headline))}%`, severity(headline));
            this._applyMeter(block.meterA, data.a);
            this._applyMeter(block.meterB, data.b);
            if (!block.meterA.root.visible && !block.meterB.root.visible) {
                block.meterA.setVisible(true);
                block.meterA.setMuted('no data');
                block.setHeadline('—');
            }
        }
        block.setBilling(
            this._settings.get_boolean('show-billing') ? data.billing ?? '' : '');
        this._renderGraph(id);
    }

    _recordHistory(id) {
        const data = this._last[id];
        if (!data || data.isUnlimited)
            return;
        const value = win => (win && !win.missing ? pct(win.utilization) : null);
        this._history.push(id, value(data.a), value(data.b));
        this._history.flush();
    }

    _renderGraph(id) {
        const block = this._blockFor(id);
        const show = this._settings.get_boolean('show-graph') && !!this._last[id] &&
            !this._last[id].isUnlimited;
        if (!show) {
            block.setGraph([], false, '', false);
            return;
        }
        const hours = Math.max(1, this._settings.get_int('graph-hours'));
        const points = this._history.series(id, hours * 3600);
        const span = points.length >= 2
            ? `last ${humanDuration(Date.now() / 1000 - points[0].t)}`
            : '';
        block.setGraph(
            points,
            this._settings.get_string('usage-display') === 'remaining',
            span,
            true);
    }

    _renderAll() {
        for (const id of PROVIDER_IDS) {
            if (this._last[id])
                this._renderProvider(id);
        }
        this._renderPanel();
    }

    _selectedSnapshot() {
        const mode = this._settings.get_string('panel-provider');
        const enabled = [];
        if (this._settings.get_boolean('show-cursor') && this._last.cursor)
            enabled.push(this._last.cursor);
        if (this._settings.get_boolean('show-claude') && this._last.claude)
            enabled.push(this._last.claude);
        if (this._settings.get_boolean('show-codex') && this._last.codex)
            enabled.push(this._last.codex);
        if (!enabled.length)
            return null;
        // Only honour the chosen provider while it is both enabled and up;
        // otherwise fall through to the most-used of whatever is left.
        if (mode !== 'max' && enabled.includes(this._last[mode]))
            return this._last[mode];
        // max across providers, and the fallback when the chosen one is down
        let best = null;
        let bestUtil = -1;
        for (const snap of enabled) {
            const win = this._poolFromSnap(snap);
            const u = win?.utilization ?? 0;
            if (u > bestUtil) {
                bestUtil = u;
                best = snap;
            }
        }
        return best;
    }

    _poolFromSnap(snap) {
        if (!snap)
            return null;
        const mode = this._settings.get_string('panel-window');
        if (snap.isUnlimited)
            return {utilization: 0, unlimited: true};
        if (mode === 'total')
            return snap.total && !snap.total.missing
                ? snap.total
                : pickPool(snap.a, snap.b, 'max');
        return pickPool(snap.a, snap.b, mode);
    }

    _renderPanel() {
        const snap = this._selectedSnapshot();
        if (!snap) {
            this._label.text = '-';
            this._label.style_class = 'cu-panel-pct';
            this._ring.setUnknown();
            this._panelTier.text = '';
            return;
        }
        this._panelTier.text = this._settings.get_boolean('show-tier') ? snap.tier : '';
        if (snap.isUnlimited) {
            this._label.text = '∞';
            this._label.style_class = 'cu-panel-pct usage-low';
            this._ring.setValue(0);
            return;
        }
        const win = this._poolFromSnap(snap);
        if (!win) {
            this._label.text = '-';
            this._label.style_class = 'cu-panel-pct';
            this._ring.setUnknown();
            return;
        }
        const util = pct(win.utilization);
        const display = this._settings.get_string('usage-display') === 'remaining'
            ? 100 - util
            : util;
        this._label.text = `${Math.round(display)}%`;
        this._label.style_class = `cu-panel-pct ${severity(util)}`;
        this._ring.setValue(util);
    }

    _scheduleCountdown() {
        if (this._countdownTimer) {
            GLib.source_remove(this._countdownTimer);
            this._countdownTimer = null;
        }
        const snap = this._selectedSnapshot();
        const end = this._poolFromSnap(snap)?.resets_at ??
            snap?.a?.resets_at ?? snap?.b?.resets_at;
        if (end == null)
            return;
        const target = typeof end === 'number' ? end * 1000 : Date.parse(end);
        if (Number.isNaN(target))
            return;
        const seconds = (target - Date.now()) / 1000;
        if (!(seconds > 0))
            return;
        this._countdownTimer = GLib.timeout_add_seconds(
            GLib.PRIORITY_DEFAULT,
            seconds < 90 ? 1 : 30,
            () => {
                this._countdownTimer = null;
                this._renderAll();
                this._scheduleCountdown();
                return GLib.SOURCE_REMOVE;
            }
        );
    }

    _stamp(ok) {
        const now = GLib.DateTime.new_now_local();
        this._updated.text = `${ok ? 'Updated' : 'Checked'} ${now.format('%H:%M')}`;
    }

    _markRefreshing() {
        this._updated.text = 'Refreshing…';
    }

    destroy() {
        this._stopTimer();
        if (this._countdownTimer) {
            GLib.source_remove(this._countdownTimer);
            this._countdownTimer = null;
        }
        this._cancelPending();
        this.menu.disconnectObject(this);
        this._session?.abort();
        this._session = null;
        if (this._settingsChangedId) {
            this._settings.disconnect(this._settingsChangedId);
            this._settingsChangedId = null;
        }
        this._settings = null;
        this._openPreferences = null;
        this._last = {cursor: null, claude: null, codex: null};
        this._history?.flush(true);
        this._history = null;
        for (const id of PROVIDER_IDS)
            this._blocks?.[id]?.destroy();
        this._blocks = null;
        super.destroy();
    }
});

export default class CursorUsageExtension extends Extension {
    enable() {
        this._settings = this.getSettings();
        this._indicator = new CursorUsageIndicator(
            this.path,
            this._settings,
            () => this.openPreferences()
        );
        Main.panel.addToStatusArea(this.uuid, this._indicator);
    }

    disable() {
        this._indicator?.destroy();
        this._indicator = null;
        this._settings = null;
    }
}
