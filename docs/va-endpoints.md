# Vision Australia portal: write endpoints (findings from BookBook)

Extends Bookworm's `docs/va-endpoints.md`. Everything here comes from reading the portal's own JavaScript
(`/modules/custom/library_features_dodp/js/*.js`, fetched while signed in at `/library/my-library`) and
from live tests against a real account in September 2026. Unofficial and may change without notice.

## Verified live

| Action | Request | Notes |
|---|---|---|
| Add to bookshelf | `GET /library/my-bookshelf/add/{bookshareId}/{formatId}?type=book` | Works. Confirmed by re-reading the shelf. |
| Remove a book from the bookshelf | `POST /library/my-bookshelf/remove/all`, form-encoded `book_active_title_ids={activeTitleId}` | Works. Send one id per request, exactly as the page's "Remove Selected" button does. Sent with `X-Requested-With: XMLHttpRequest`. |

## Correction to Bookworm's notes

`GET /library/my-bookshelf/remove/{type}/{activeTitleId}` (used by Bookworm's `RemoveFromBookshelfAsync`)
is **not** how the site removes books. In `my-bookshelf-tab.js` it is used only by the single-item
"Remove item from Bookshelf" link, which the renderer emits for **periodical issues**. Books are removed with
the row's checkbox (value = `activeTitleId`) and the **Remove Selected** button, which POSTs one request per
ticked book to `/library/my-bookshelf/remove/all`. Despite the name, `remove/all` removes the ids listed in
`book_active_title_ids`: the "clear everything" dialog uses the same endpoint but sends every id on the shelf.
Bookworm's book removal should be switched to the POST form.

Response shape (from the page's success handler): JSON with `message: "OK"`. The board does not rely on it:
it re-reads the shelf and checks the title is gone.

## Traced from the JavaScript, not yet exercised

| Action | Request |
|---|---|
| Remove from request list | `/library/request-list/remove/{activeTitleId}` (method not confirmed) |
| Remove a subscription | `/library/my-library/subscription/remove/{activeTitleId}` (method not confirmed) |
| Remove periodical issues in bulk | `POST /library/my-periodical/remove/all` (field name not confirmed) |
| Download a bookshelf item | `/library/download` (called from `button.item-download`; not traced further) |

## Other notes

- `/library/my-library` is the HTML page; `/library/my-library/my-bookshelf?...` returns the JSON the page uses.
- Search (`POST /library/quick-search`) is slow on the server side: 4 to 14 seconds per call was measured.
- The portal treats an expired session by redirecting to the HTML login page; any non-JSON answer from a JSON
  endpoint should trigger one re-login and retry.
