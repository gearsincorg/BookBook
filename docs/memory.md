# Memory: the authors list and the books list

BookBook shares one memory file with Bookworm (`memory.json` in Azure Blob storage, Bookworm's
`BookwormMemory` format; see `main/memory.h`). Two lists work the same way: **a list of things the member knows,
some of which are favourites.**

## Authors list (`PreferredAuthors`)

| | |
|---|---|
| Gets on the list | the member asks (`add_preferred_author`), **or** a book by that author is added to the bookshelf (automatic, not a favourite, never asked about) |
| Favourite | only when the member says so (`add_preferred_author` with `isFavorite` true; also updates an author already on the list) |
| Removal | `remove_preferred_author`, **verified first**: the assistant says which author and waits for a clear yes |
| Duplicates | none: names are compared in natural order, so `Silva, Daniel, 1960-` and `Daniel Silva` are the same author |

## Books list (`ReadingHistory`)

| | |
|---|---|
| Gets on the list | the member asks (`add_book`, works for a book never on the bookshelf, so it has no add/remove dates), **or** the book is added to the bookshelf (automatic) |
| Favourite | a rating of 4 or 5 (Bookworm's format has no separate flag; an extra field would be dropped the next time Bookworm saves). `add_book` with `isFavorite` true sets 5 stars; false clears a 4 or 5 but keeps the book on the list |
| Removal | `remove_book` deletes the entry, **verified first**. Taking a book only off the favourites needs no check |
| Duplicates | none: matched by catalogue id, else exact title. Adding to the bookshelf a book that is already listed (for example a favourite added earlier, or one borrowed before) reuses its entry with a fresh add date, no remove date, and its rating kept |
| Dates | set when the book goes on / comes off the bookshelf; a favourite that was never borrowed has neither |

## Standby list (`standby.json`)

BookBook's own **save for later** list: an alternative to putting a found book or series on the bookshelf. It is
separate from the library's request list, which really queues the title with the library and moves it to the
bookshelf by itself when a slot frees up; the standby list does nothing on the library's side.

| | |
|---|---|
| Gets on the list | the member asks (`add_to_standby`): a single book, or a series under its series name with a note. When the member has found something but not said what to do with it, the assistant offers the choice: bookshelf now, or standby. When the bookshelf is full (20 of 20), standby is the first thing it suggests, then the request list |
| Reading it | `get_standby_list` ("what's on my standby list"); the titles are also in the assistant's system prompt |
| Moving to the bookshelf | `add_to_bookshelf`, which **removes the standby entry by itself** (matched by catalogue id, else exact title; `standbyEntry` names the entry when the title differs, for example a series; `keepOnStandby` true keeps it) |
| Removal | `remove_from_standby`, **no verification**: it is the member's own scratch list |
| Duplicates | none (catalogue id, else exact title) |
| Storage | its **own blob** in the same container. Bookworm rewrites `memory.json` from its own model and would silently drop a field it does not know, so the list is kept out of that file. Once Bookworm is retired or updated, it could be folded into `memory.json` |

## Other memory

Free-text preferences (`remember_preference`), preferred genres (`add_preferred_genre`) and star ratings
(`rate_book`) can be added but not yet removed. Genres are added only after the member agrees.

## What the assistant sees

Everything except the reading history goes into the system prompt on every request, plus the favourite books
(rated 4 or 5, at most 30) and the history's size. The full history is searched with `search_reading_history`.

The "verify first" step is enforced by the assistant's instructions (as for removing a book from the
bookshelf), not by a lock in the code.
