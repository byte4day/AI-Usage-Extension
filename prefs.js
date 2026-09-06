import Adw from 'gi://Adw';
import Gtk from 'gi://Gtk';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import {ExtensionPreferences} from 'resource:///org/gnome/Shell/Extensions/js/extensions/prefs.js';

const GRAPH_HOURS = [3, 12, 24, 72];

export default class CursorUsagePreferences extends ExtensionPreferences {
    // Combo rows all follow the same shape: a fixed list of values, mapped to
    // and from the string stored in GSettings.
    _comboRow(settings, key, title, subtitle, labels, values) {
        const row = new Adw.ComboRow({title, subtitle});
        const model = new Gtk.StringList();
        for (const label of labels)
            model.append(label);
        row.set_model(model);
        const current = values.indexOf(settings.get_string(key));
        row.set_selected(current < 0 ? 0 : current);
        row.connect('notify::selected', () => {
            const next = values[row.get_selected()];
            if (next !== undefined && next !== settings.get_string(key))
                settings.set_string(key, next);
        });
        return row;
    }

    _brandIcon(name) {
        const path = GLib.build_filenamev([this.path, 'icons', `${name}-color.svg`]);
        const image = Gtk.Image.new_from_gicon(
            Gio.FileIcon.new(Gio.File.new_for_path(path)));
        image.set_pixel_size(22);
        return image;
    }

    fillPreferencesWindow(window) {
        const settings = this.getSettings();
        window.set_default_size(620, 720);

        // --- Usage ---------------------------------------------------------
        const usagePage = new Adw.PreferencesPage({
            title: 'Usage',
            icon_name: 'utilities-system-monitor-symbolic',
        });
        window.add(usagePage);

        const providers = new Adw.PreferencesGroup({
            title: 'Providers',
            description: 'Each provider is read from the session the matching CLI already stored — no separate sign-in.',
        });
        usagePage.add(providers);

        for (const [key, icon, title, subtitle] of [
            ['show-cursor', 'cursor', 'Cursor', 'Auto and API meters from cursor.com'],
            ['show-claude', 'claude', 'Claude', '5-hour and 7-day OAuth usage'],
            ['show-codex', 'codex', 'Codex', 'Primary and weekly rate limits'],
        ]) {
            const row = new Adw.SwitchRow({title, subtitle});
            row.add_prefix(this._brandIcon(icon));
            settings.bind(key, row, 'active', Gio.SettingsBindFlags.DEFAULT);
            providers.add(row);
        }

        const polling = new Adw.PreferencesGroup({title: 'Refresh'});
        usagePage.add(polling);

        const refresh = new Adw.SpinRow({
            title: 'Refresh interval',
            subtitle: 'Seconds between updates (keep ≥120 for Claude)',
            adjustment: new Gtk.Adjustment({
                lower: 10,
                upper: 600,
                step_increment: 10,
                page_increment: 60,
                value: settings.get_int('refresh-interval'),
            }),
        });
        settings.bind('refresh-interval', refresh, 'value', Gio.SettingsBindFlags.DEFAULT);
        polling.add(refresh);

        const network = new Adw.PreferencesGroup({title: 'Network'});
        usagePage.add(network);

        const proxy = new Adw.EntryRow({
            title: 'Proxy URL',
            show_apply_button: true,
        });
        proxy.set_text(settings.get_string('proxy-url'));
        proxy.connect('apply', () => settings.set_string('proxy-url', proxy.get_text()));
        network.add(proxy);

        // --- Appearance ----------------------------------------------------
        const lookPage = new Adw.PreferencesPage({
            title: 'Appearance',
            icon_name: 'applications-graphics-symbolic',
        });
        window.add(lookPage);

        const panel = new Adw.PreferencesGroup({
            title: 'Top panel',
            description: 'What the indicator next to the clock shows.',
        });
        lookPage.add(panel);

        panel.add(this._comboRow(
            settings, 'display-mode', 'Display', 'Ring gauge or percentage only',
            ['Ring + percentage', 'Percentage only'], ['ring', 'text']));
        panel.add(this._comboRow(
            settings, 'panel-provider', 'Provider',
            'Which service the panel percentage follows',
            ['Most used', 'Cursor', 'Claude', 'Codex'],
            ['max', 'cursor', 'claude', 'codex']));
        panel.add(this._comboRow(
            settings, 'panel-window', 'Pool',
            'Cursor Auto/API, Claude 5h/7d, Codex primary/weekly',
            ['Most used', 'Primary / Auto / 5h', 'Secondary / API / 7d', 'Total'],
            ['max', 'auto', 'api', 'total']));
        panel.add(this._comboRow(
            settings, 'usage-display', 'Values', 'Count up from zero, or down to the limit',
            ['Used', 'Remaining'], ['used', 'remaining']));

        const showIcon = new Adw.SwitchRow({title: 'Show icon'});
        settings.bind('show-icon', showIcon, 'active', Gio.SettingsBindFlags.DEFAULT);
        panel.add(showIcon);

        const showTier = new Adw.SwitchRow({
            title: 'Show plan tier',
            subtitle: 'PRO, MAX, ULTRA … in the panel and on each card',
        });
        settings.bind('show-tier', showTier, 'active', Gio.SettingsBindFlags.DEFAULT);
        panel.add(showTier);

        const menu = new Adw.PreferencesGroup({
            title: 'Menu',
            description: 'The card for each provider in the drop-down.',
        });
        lookPage.add(menu);

        const graph = new Adw.SwitchRow({
            title: 'Usage trend chart',
            subtitle: 'Plot both pools over time under the meters',
        });
        settings.bind('show-graph', graph, 'active', Gio.SettingsBindFlags.DEFAULT);
        menu.add(graph);

        const hoursRow = new Adw.ComboRow({
            title: 'Trend window',
            subtitle: 'How far back the chart reaches',
        });
        const hoursModel = new Gtk.StringList();
        for (const h of GRAPH_HOURS)
            hoursModel.append(h < 24 ? `${h} hours` : `${h / 24} days`);
        hoursRow.set_model(hoursModel);
        const storedHours = settings.get_int('graph-hours');
        const closest = GRAPH_HOURS.reduce(
            (best, h, i) => (Math.abs(h - storedHours) < Math.abs(GRAPH_HOURS[best] - storedHours)
                ? i
                : best),
            0);
        hoursRow.set_selected(closest);
        hoursRow.connect('notify::selected', () => {
            settings.set_int('graph-hours', GRAPH_HOURS[hoursRow.get_selected()]);
        });
        settings.bind('show-graph', hoursRow, 'sensitive', Gio.SettingsBindFlags.GET);
        menu.add(hoursRow);

        const billing = new Adw.SwitchRow({
            title: 'Billing and credits',
            subtitle: 'Cursor spend, Claude extra usage, Codex credits',
        });
        settings.bind('show-billing', billing, 'active', Gio.SettingsBindFlags.DEFAULT);
        menu.add(billing);

        const about = new Adw.PreferencesGroup();
        lookPage.add(about);
        const project = new Adw.ActionRow({
            title: 'AI Usage',
            subtitle: 'github.com/byte4day/AI-Usage-Extension',
            activatable: true,
        });
        project.add_prefix(this._brandIcon('ai-usage'));
        project.add_suffix(new Gtk.Image({icon_name: 'adw-external-link-symbolic'}));
        project.connect('activated', () => {
            Gtk.show_uri(window, 'https://github.com/byte4day/AI-Usage-Extension', 0);
        });
        about.add(project);
    }
}
