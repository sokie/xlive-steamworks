# What happens to the SPA

Every GFWL title embeds an SPA file (an XDBF container, resource `SPAFILE` of type `RT_RCDATA`
in the exe). GFWL used it as the title's service definition. On Steam it splits in two: the parts
that describe how the title *looks* stay in the exe and keep being read by this wrapper. The
parts that describe what the *service* stores have to exist on Steamworks.

## What the SPA holds

| Section | Content | On Steam |
| --- | --- | --- |
| `XTHD` | title id | read by the wrapper (`title.title_id` overrides) |
| `XSTC` | default language | read by the wrapper |
| `XSTR` (namespace 3, one per language) | all localized strings | read by the wrapper for achievement names and descriptions, presence formats, view and column names |
| `XACH` | achievements: id, label/description/unachieved string ids, image id, gamerscore, type flags | **ids, text and gamerscore stay here**, the *unlock state* moves to Steam achievements |
| namespace 2 | PNG images: achievement art, title icon | read by the wrapper for `XUserReadAchievementPicture`, Steam wants the same art uploaded once |
| `XVC2` | stats views: view id, column property ids and types | read by the wrapper to lay out leaderboard details, each view the title writes needs a Steam leaderboard |
| `XSRC` | the original XLAST project (gzip XML): friendly names, presence modes, context value strings, matchmaking queries, constants | read by the wrapper for presence text and search filters |
| `XCXT`, `XPRP`, `XMAT`, `XGAA` | contexts, properties, matchmaking schema, avatar awards | contexts and properties are used through XLAST, avatar awards have no Steam counterpart |

## What stays as it is

- The SPA resource in the exe. Do not strip it. Without it the wrapper still runs, achievement
  text then comes from Steam's `name`/`desc` display attributes and ids follow Steam's order,
  which is only right if the title's achievement ids happen to be 1..N in that order.
- The title's calls. `XUserWriteAchievements(id)` keeps its ids, the wrapper turns each id into a
  Steam API name (`ACH_<id>` by default, or the `achievements.map` in the config, or
  `XlsSetAchievementName` at run time).
- Presence: `XUserSetContext(X_CONTEXT_PRESENCE, n)` picks the SPA format string, `{cN}` and
  `{pN}` placeholders are filled from the contexts and properties the title set, and the line is
  published as Steam rich presence under the `status` key.

## What must be created on Steamworks

| SPA item | Steamworks item | Naming |
| --- | --- | --- |
| each `XACH` achievement | Stats & Achievements > achievement | API name `ACH_<id>` or the config map, display name and description from the SPA strings (all languages), icons from the SPA PNGs |
| each `XVC2` view the title writes | Leaderboard | `LB_<viewId>` or the config map, sort/display type per view. With `leaderboards.create_if_missing` the first write creates it |
| optional stat mirrors | Stats | any name, `stats` in the config maps `<viewId>:<columnId>` to it |
| presence | Rich Presence localization | token `#status` = `%status%` (`presence.steam_display`) |
| content packages | DLC apps | app id per package, `content.dlc` mapping |
| title managed storage, profile | Steam Cloud | enable Cloud with a byte quota and a file count that covers `xlive/profile.bin` plus the title's files |

`tools/spa2steam.py` reads the SPA out of the exe and writes:

- `achievements.csv`: id, API name, gamerscore, hidden flag, name/description/unachieved text per
  language. This is the checklist for the achievements page.
- `icons/<api name>.png`: the achievement art, ready to upload.
- `leaderboards.csv`: view id, suggested name, columns with their types and which one is the
  rating column.
- `presence.txt`: the presence modes and their format strings with the context values, to build
  the rich presence localization if the studio wants tokens beyond `#status`.
- `xlive_steamworks.json`: a starting config with the explicit achievement and leaderboard maps.

## Leaderboard columns

A GFWL view had several columns, a Steam leaderboard has one score and up to 64 `int32` details.
The wrapper keeps the whole view:

- The rating column (config `rating_column`, else the first column of the view) is the Steam
  score.
- Every other column is packed into the details array in SPA column order: `INT32`/`CONTEXT`
  take one slot, `INT64`/`DOUBLE`/`DATETIME` two, `FLOAT` one (bit pattern). `UNICODE` and
  `BINARY` columns are skipped.
- Reads unpack the details with the same layout, so `XUserReadStats` returns the columns the
  title asked for.

Keep the column set of a view stable once entries exist, changing it changes the details layout.

## TrueSkill

`X_STATS_VIEW_SKILL` writes are dropped and `XSessionCalculateSkill` returns a plain aggregate.
Steam has no rating service, a title that ranks by skill needs its own.
