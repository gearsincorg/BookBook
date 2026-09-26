"""Publishes build/bookbook.bin so a BookBook can install it when asked ("update yourself").

Uploads two blobs to the same storage container as memory.json, using the SAS URL in
secrets/sdkconfig.secrets (CONFIG_BOOKBOOK_MEMORY_URL, which needs create and write permission):

    bookbook.bin    the app image
    bookbook.json   {"sha256", "version", "built", "size"}: uploaded last, so a device never sees a
                    manifest whose image is not there yet

The sha256 is the ELF hash stored in the image's app descriptor; the device compares it with its own to
decide whether an update is available. The SAS token is never printed.

    python tools/publish_firmware.py             # upload
    python tools/publish_firmware.py --dry-run   # show what would be published
    python tools/publish_firmware.py --status    # show what is published now
"""
import argparse
import json
import re
import struct
import sys
import urllib.error
import urllib.request
from pathlib import Path

root = Path(__file__).resolve().parent.parent
IMAGE_BLOB = "bookbook.bin"
MANIFEST_BLOB = "bookbook.json"
API_VERSION = "2020-12-06"

# esp_app_desc_t follows the 24-byte image header and the 8-byte header of the first segment.
DESC_OFFSET = 32
DESC_MAGIC = 0xABCD5432


def memory_url():
    text = (root / "secrets" / "sdkconfig.secrets").read_text(encoding="utf-8")
    m = re.search(r'^CONFIG_BOOKBOOK_MEMORY_URL="([^"]+)"', text, re.M)
    if not m:
        sys.exit("CONFIG_BOOKBOOK_MEMORY_URL not found in secrets/sdkconfig.secrets")
    return m.group(1)


def blob_url(base, blob):
    path, _, query = base.partition("?")
    return path.rsplit("/", 1)[0] + "/" + blob + ("?" + query if query else "")


def cstr(b):
    return b.split(b"\0", 1)[0].decode("ascii", "replace")


def describe(image):
    magic, = struct.unpack_from("<I", image, DESC_OFFSET)
    if magic != DESC_MAGIC:
        sys.exit("build/bookbook.bin has no app descriptor at the expected place: is it an ESP-IDF app image?")
    d = image[DESC_OFFSET:DESC_OFFSET + 256]
    return {
        "sha256": d[144:176].hex(),
        "version": cstr(d[16:48]),
        "built": cstr(d[96:112]) + " " + cstr(d[80:96]),  # date, time
        "size": len(image),
    }


def request(url, method, data=None, content_type=None):
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("x-ms-version", API_VERSION)
    if method == "PUT":
        req.add_header("x-ms-blob-type", "BlockBlob")
        req.add_header("Content-Type", content_type)
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def status(base):
    code, body = request(blob_url(base, MANIFEST_BLOB), "GET")
    if code == 404:
        print("Nothing published yet.")
    elif code == 200:
        m = json.loads(body)
        print(f"Published: {m.get('version')} built {m.get('built')}, {m.get('size')} bytes, sha256 {m.get('sha256', '')[:12]}...")
    else:
        sys.exit(f"Could not read the published manifest: HTTP {code}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dry-run", action="store_true", help="show what would be published, upload nothing")
    ap.add_argument("--status", action="store_true", help="show what is published now, upload nothing")
    args = ap.parse_args()

    base = memory_url()
    if args.status:
        status(base)
        return

    image = (root / "build" / "bookbook.bin").read_bytes()
    manifest = describe(image)
    print(f"Image: {manifest['version']} built {manifest['built']}, {manifest['size']} bytes, sha256 {manifest['sha256'][:12]}...")
    if args.dry_run:
        print("Dry run: nothing uploaded.")
        return

    code, body = request(blob_url(base, IMAGE_BLOB), "PUT", image, "application/octet-stream")
    if code != 201:
        sys.exit(f"Uploading {IMAGE_BLOB} failed: HTTP {code} {body[:300].decode('utf-8', 'replace')}")
    print(f"Uploaded {IMAGE_BLOB}")
    code, body = request(blob_url(base, MANIFEST_BLOB), "PUT", json.dumps(manifest).encode(), "application/json")
    if code != 201:
        sys.exit(f"Uploading {MANIFEST_BLOB} failed: HTTP {code} {body[:300].decode('utf-8', 'replace')}")
    print(f"Uploaded {MANIFEST_BLOB}")
    status(base)


if __name__ == "__main__":
    main()
