# FEAR2 Inspector

A single-page, offline, read-only browser UI for the `fear2vr` DLL's live
diagnostic HTTP API. Plain HTML + vanilla JS + CSS: no build step, no npm,
no frameworks, no CDN. It only ever performs `GET` requests.

**This tool is READ-ONLY. It never calls any game/engine function and never
writes game memory.** Every value it shows comes from the DLL's own
diagnostics endpoints, which themselves only read memory through the SDK's
guarded readers (see `AGENTS.md` / `TESTING.MD` in the repo root). The one
pre-existing endpoint that mutates DLL state, `/unload`, is intentionally
**not** exposed anywhere in this UI (not even on the Raw tab) so a stray
click can't unload the mod out from under you.

## Starting it

1. Start F.E.A.R. 2: Project Origin.
2. Inject `fear2vr.dll` with the project's injector
   (`build/bin/injector.exe --inject`), and give it a few seconds to come up.
3. From this directory, run:

   ```
   python serve.py
   ```

   This starts a dependency-free static file server (Python standard library
   only) on `http://127.0.0.1:8080/` and prints the URL. It serves
   `index.html`, `app.js`, and `style.css` — nothing else. It does **not**
   proxy the game's API.
4. Open `http://127.0.0.1:8080/` in a browser.
5. The **Base URL** field in the top bar defaults to
   `http://127.0.0.1:8798` (the DLL's own HTTP server) and is saved to
   `localStorage`, so it only needs to be changed if the DLL's IPC port was
   reconfigured. The browser talks to that URL **directly** — this works
   cross-origin because the DLL sends `Access-Control-Allow-Origin: *` on
   every response.

If the DLL is not injected (or the game isn't running), every tab shows a
red **"API unreachable — is the DLL injected?"** banner and keeps
displaying whatever data it last successfully fetched, rather than going
blank.

## Tabs

- **Live State** — polls `/api/state` on an interval you choose (Off /
  250 ms / 1 s / 5 s, default 1 s, saved to `localStorage`). Shows player
  vitals (health/armor as bars against their maxima, air as a percentage,
  health lost, alive/consistent flags), movement (crouch/move flags, speed,
  velocity), and a Camera panel: the active clamp record, state, the
  predicted clamp (highlighted in the six-clamp table of degree/radian
  ranges), pitch-clamp before/after values, and the recovery timer. The
  pitch numbers are explicitly labelled "last violation, not current frame";
  when nothing has ever triggered the clamp this session (both values zero
  and the timer inactive) the panel says so instead of showing a misleading
  `0.000`.
- **Subsystems** — the player's subsystem-slot table from
  `/api/subsystems`: offset, name (or a muted `‹unnamed›` placeholder),
  object/vtable/ctor addresses, size lower bound, and delegate node count.
  Rows where `is_class_instance` is false or `owner_is_player` is false are
  highlighted and labelled — those are the two known anomalies in that data.
- **Database** — a three-pane browser: categories (filter + paged list,
  with a note when a category's names aren't unique), records within the
  selected category (filter + paged), and the selected record's attributes.
  Attributes show their name (or the raw hash in muted monospace when
  unnamed), type, value count, and rendered value. Record-link attributes
  (types 9/10) render `link_target` as a clickable link that navigates to
  that record — it is looked up in the current category first, then in
  `_Structures`, the shared cross-category record pool.
- **Attribute Search** — searches every record for a given attribute name
  via `/api/db/find` and lists every match's category/record/type/value;
  clicking a row opens that record in the Database tab.
- **Console** — the registered console command table from `/api/console`,
  filterable by name. Commands whose handler is a no-op stub are flagged
  "NO-OP (does nothing)"; the registrar (and its call-site offset) is shown
  when known.
- **Vars** — the cached console-variable table from `/api/vars`, filterable
  by name. Rows whose current value differs from the engine's registered
  default are flagged "CHANGED FROM DEFAULT".
- **Raw** — pick any endpoint (including the pre-existing `/health`,
  `/sdk/targets`, `/sdk/database`, `/sdk/objects`, `/sdk/models`,
  `/sdk/interfaces`, `/sdk/shader-params`, plus every `/api/*` endpoint
  above) from the dropdown, edit the path/query if needed, fetch it, and
  get the pretty-printed JSON with a one-click copy button. `/unload` and
  `/engine-hook` are deliberately excluded (destructive / parameter-only).

## Notes

- All paging in the Database, Console (implicit), and Vars views is capped
  at the API's documented maximum of 500 items per request.
- Every fetch is defensive: a network failure or malformed response raises
  the unreachable banner without discarding the last good render; a missing
  field renders as an em dash (`—`) rather than crashing or inventing a
  zero.
- Nothing here talks to any port other than the configured base URL and the
  page's own origin (`127.0.0.1:8080`, from `serve.py`).
