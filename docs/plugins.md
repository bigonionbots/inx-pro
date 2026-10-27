# Plugins

Plugins are small Lua 5.4 packages that extend fixed places in the firmware. They cannot draw, run in the
background, or reach the network: the firmware calls a plugin function when one of its hooks is used, and
draws whatever the function returns.

## Package

A plugin is a ZIP (at most 512 KB, 256 KB extracted) containing a flat set of files:

- `manifest.json`: the plugin's `id` (letters, digits, `-`, `_`), `name`, and the hooks it provides.
- `main.lua`: run once when the plugin is first used; it defines the global functions the hooks name.

Installed plugins live in `/.system/plugins/<id>/`. The Lua state is kept alive between calls, so globals
persist until the plugin is reinstalled or removed. When several plugins declare the same hook, the first one
found on the SD card is used.

## Hooks

| Manifest key | Fields | Function argument | Function result |
|---|---|---|---|
| `reader_action` | `label`, `function` | `selected_text`, `book_title`, `chapter_title`, `page_number`, `spine_index`, `timestamp` | `true` on success |
| `reader_suggestion` | `function`, `label`, `group_field` | `path`, `title`, `author` | JSON text: `{path, title, <group_field>}` |
| `library_menu` | `label`, `function`, `group_field`, `order_field` | none | JSON text: a list of `{path, <group_field>, <order_field>}` |
| `home_widget` | `function`, `label`, `refresh_minutes` | `year`, `month`, `day`, `hour`, `minute`, `weekday` (1 = Monday), `width`, `height` | table: `before`, `highlight`, `after`, `footer` |
| `web` | `page`, `script`, `label`, `path`, `icon`, `order` | via `/api/plugin/<id>/<function>`: the request's JSON body or query string | text sent as the response |

### Home widget

A theme can place the plugin widget in any home screen slot (Settings → Theme). The firmware calls the function
when the widget is first drawn and then every `refresh_minutes` (default 1; `0` means only when the home screen
is drawn again). The date and time fields are missing when the device clock has not been set.

The result is laid out by the firmware: `before`, `highlight` (bold), and `after` flow together as one
paragraph in the largest font that fits the slot, and `footer` is drawn as a small italic line at the bottom
right. Text placed directly against the highlight (for example a closing full stop) stays attached to it. All
fields are optional strings. The widget is not offered for sleep themes, which are drawn once and would show
stale text.

```json
{
  "id": "clock-demo",
  "name": "Clock Demo",
  "home_widget": { "label": "Words Clock", "function": "render_widget", "refresh_minutes": 1 }
}
```

```lua
function render_widget(args)
  if not args.hour then return {before = "The clock has not been set."} end
  return {
    before = "It was ",
    highlight = string.format("%d:%02d", args.hour, args.minute),
    after = " when she looked up.",
    footer = "Clock Demo",
  }
end
```

## Lua environment

Available libraries: `base` (without `dofile` and `loadfile`), `table`, `string`, `math`, `utf8`, and
`coroutine`. There is no `io`, `os`, `require`, or `debug`.

The firmware adds an `inx` table:

| Function | Purpose |
|---|---|
| `inx.storage.append_json(file, table)` | Append a table as one JSON line |
| `inx.storage.append_text(file, text)` | Append text |
| `inx.storage.write_text(file, text)` | Replace a file's contents |
| `inx.storage.read_all(file)` | Read a file (up to 256 KB); returns `""` when it is missing |
| `inx.log(...)` | Print to the serial log with a `[LUA]` prefix |

Storage file names are flat names inside the plugin's own folder, so a plugin can also read files shipped in
its package. Plugin functions run on the calling task without a time limit, so keep them short.
