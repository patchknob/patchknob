#!/usr/bin/env python3
"""Mirror the Canonical Csound Reference Manual into vendor/csound-manual.

The Csound editor's help drawer (sdlui/views/csound_editor) browses this
mirror off local disk with the vendored litehtml renderer.  The mirror is NOT
tracked in our history -- it is ~33 MB of upstream documentation, fetched on
demand, the same treatment vendor/rtmidi and vendor/rtaudio get.  If it is
absent the drawer falls back to a small baked-in quick reference, so the build
never depends on running this.

The manual is published by the Csound project at https://csound.com/docs/manual/
under the GNU Free Documentation License; this only makes a local copy for
offline use and redistributes nothing.

Usage:
    python3 tools/fetch_csound_manual.py [--out DIR] [--jobs N]

Only pages under /docs/manual/ are followed, so the crawl cannot wander into
the rest of csound.com.  Transient connection failures are retried in a second
pass -- the site rate-limits a wide crawl and drops a sizable fraction of the
first attempt.
"""

import argparse
import os
import sys
import threading
import time
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from html.parser import HTMLParser

BASE = "https://csound.com/docs/manual/"
PATH_PREFIX = "/docs/manual/"
USER_AGENT = "patchknob-manual-mirror/1.0"

# Belt and braces against a redirect loop or an unexpectedly linked archive.
MAX_PAGES = 6000
MAX_BYTES = 350 * 1024 * 1024

LINK_ATTRS = {("a", "href"), ("link", "href"), ("img", "src")}


class LinkExtractor(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.links = []

    def handle_starttag(self, tag, attrs):
        d = dict(attrs)
        for t, a in LINK_ATTRS:
            if tag == t and d.get(a):
                self.links.append(d[a])


def url_to_relpath(url):
    """Map a manual URL onto its path inside the output dir, or None."""
    path = urllib.parse.unquote(urllib.parse.urlsplit(url).path)
    if not path.startswith(PATH_PREFIX):
        return None
    rel = path[len(PATH_PREFIX):]
    if rel == "" or rel.endswith("/"):
        rel += "index.html"
    # Characters that are legal on POSIX but not on Windows: the tree has to
    # check out on both.
    for bad in ':*?"<>|':
        rel = rel.replace(bad, "_")
    return rel


def fetch(url, attempts=1):
    err = None
    for i in range(attempts):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            with urllib.request.urlopen(req, timeout=20) as resp:
                return resp.read(), resp.headers.get("Content-Type", ""), None
        except Exception as exc:  # noqa: BLE001 - report whatever the network gives us
            err = str(exc)
            if i + 1 < attempts:
                time.sleep(0.5 * (i + 1))
    return None, None, err


def write_out(out_dir, url, data):
    rel = url_to_relpath(url)
    if rel is None:
        return False
    dest = os.path.join(out_dir, rel)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with open(dest, "wb") as fh:
        fh.write(data)
    return True


def run(out_dir, jobs):
    os.makedirs(out_dir, exist_ok=True)
    lock = threading.Lock()
    visited = set()
    queued = {BASE}
    failures = {}
    pages = 0
    total = 0

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        while queued:
            with lock:
                batch = list(queued)[: jobs * 4]
                for url in batch:
                    queued.discard(url)
                    visited.add(url)
            if not batch:
                break
            futures = {pool.submit(fetch, u): u for u in batch}
            for future in as_completed(futures):
                url = futures[future]
                data, ctype, err = future.result()
                if err:
                    failures[url] = err
                    continue
                failures.pop(url, None)
                if not write_out(out_dir, url, data):
                    continue
                pages += 1
                total += len(data)
                if pages % 100 == 0:
                    print(f"  {pages} files, {total / 1e6:.1f} MB", file=sys.stderr)
                if pages > MAX_PAGES or total > MAX_BYTES:
                    print("quota reached, stopping crawl", file=sys.stderr)
                    queued.clear()
                    break
                if "text/html" not in (ctype or ""):
                    continue
                parser = LinkExtractor()
                try:
                    parser.feed(data.decode("utf-8", errors="replace"))
                except Exception:  # noqa: BLE001 - a malformed page must not stop the crawl
                    continue
                for link in parser.links:
                    if link.startswith(("#", "mailto:", "javascript:")):
                        continue
                    target = urllib.parse.urljoin(url, link).split("#")[0]
                    if not target.startswith(BASE):
                        continue
                    with lock:
                        if target not in visited and target not in queued:
                            queued.add(target)

    # Second pass: the site drops a lot of connections under a wide crawl, and
    # those URLs are otherwise simply missing from the mirror.
    for attempt in range(3):
        if not failures:
            break
        pending = sorted(failures)
        print(f"retry pass {attempt + 1}: {len(pending)} URLs", file=sys.stderr)
        failures = {}
        with ThreadPoolExecutor(max_workers=max(2, jobs // 4)) as pool:
            futures = {pool.submit(fetch, u, 4): u for u in pending}
            for future in as_completed(futures):
                url = futures[future]
                data, _ctype, err = future.result()
                if err:
                    failures[url] = err
                elif write_out(out_dir, url, data):
                    pages += 1
                    total += len(data)

    print(f"done: {pages} files, {total / 1e6:.1f} MB -> {out_dir}", file=sys.stderr)
    if failures:
        print(f"{len(failures)} URLs could not be fetched:", file=sys.stderr)
        for url, err in sorted(failures.items())[:20]:
            print(f"  {url}: {err}", file=sys.stderr)
    index = os.path.join(out_dir, "index.html")
    if not os.path.exists(index):
        print(f"ERROR: {index} missing -- the drawer will fall back to the "
              f"built-in quick reference", file=sys.stderr)
        return 1
    return 0


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(here, "vendor", "csound-manual"),
                    help="output directory (default: vendor/csound-manual)")
    ap.add_argument("--jobs", type=int, default=16, help="concurrent requests (default: 16)")
    args = ap.parse_args()
    return run(args.out, max(1, args.jobs))


if __name__ == "__main__":
    sys.exit(main())
