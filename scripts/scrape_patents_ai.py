#!/usr/bin/env python3
"""Monitor USPTO for AI-related patent filings via the Open Data Portal API.

SOURCE MIGRATION (2026-07-26) — every previous source is gone. Verified:

  * Google Patents RSS (patents.google.com/rss/search)  -> HTTP 404
  * Google Patents HTML search page                      -> HTTP 200 but a
    JavaScript-only shell: 0 `<article>` tags, 0 `/patent/` hrefs, so the old
    regex fallback could never match anything
  * api.patentsview.org                                  -> HTTP 301 to the
    USPTO PatentsView transition guide (legacy API retired)
  * developer.uspto.gov                                  -> HTTP 301 to
    data.uspto.gov (consolidated into the Open Data Portal)
  * worldwide.espacenet.com                              -> HTTP 403

The surviving source is USPTO's Open Data Portal API, which requires a key:

    https://data.uspto.gov/apis          (request a key, free)
    https://data.uspto.gov/apis/getting-started

Then export it:

    export USPTO_API_KEY=...

Without a key this script prints one message naming the env var and exits 0 —
a missing key is a configuration gap, not a scraper failure, and a daily hard
failure in launchd is just noise.

NOTE ON VERIFICATION: the request contract below is confirmed empirically from
the API gateway's own 401 response (valid path, `X-Api-Key` auth header, GET
method). The RESPONSE parsing could NOT be verified without a key, so it is
written defensively: it accepts several plausible envelope shapes and, if it
recognizes none, logs the actual top-level keys instead of silently reporting
zero results.
"""
import json, os, sys, datetime, urllib.request, urllib.error, urllib.parse, time

OUTPUT_DIR = os.path.expanduser("~/.human/feeds/ingest")
OUTPUT_FILE = os.path.join(OUTPUT_DIR, "patents_ai.jsonl")

# Contract confirmed from the gateway 401: path exists, GET, X-Api-Key header.
ODP_SEARCH = "https://api.uspto.gov/api/v1/patent/applications/search"

PATENT_QUERIES = [
    "large language model",
    "autonomous AI agent",
    "neural network inference optimization",
    "transformer architecture",
    "retrieval augmented generation",
    "AI function calling tool use",
    "edge AI embedded inference",
    "multi-agent system",
]

PER_QUERY_LIMIT = 5
PACE_SECONDS = 1.0

# Candidate field names, most specific first. USPTO ODP nests much of the
# useful metadata under applicationMetaData; plain keys are the fallback.
TITLE_KEYS = ("inventionTitle", "patent_title", "patentTitle", "title")
DATE_KEYS = ("filingDate", "patent_date", "grantDate", "publicationDate", "date")
ID_KEYS = ("applicationNumberText", "patent_id", "patent_number",
           "patentNumber", "publicationNumber", "id")


def api_key():
    key = os.environ.get("USPTO_API_KEY", "").strip()
    return key or None


def fetch_odp(query, key):
    """GET the ODP search endpoint. Returns parsed JSON or None."""
    params = urllib.parse.urlencode({"q": query, "limit": PER_QUERY_LIMIT})
    url = f"{ODP_SEARCH}?{params}"
    req = urllib.request.Request(url, headers={
        "User-Agent": "h-uman-feed/1.0",
        "X-Api-Key": key,
        "Accept": "application/json",
    })
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as e:
        detail = ""
        try:
            detail = e.read().decode("utf-8", errors="replace")[:200]
        except Exception:
            pass
        print(f"[patents] HTTP {e.code} for '{query}' {detail}", file=sys.stderr)
        return None
    except Exception as e:
        print(f"[patents] Error fetching '{query}': {e}", file=sys.stderr)
        return None


def find_records(data):
    """Locate the result list in an unverified envelope.

    Tries the documented *DataBag convention, then any top-level list of
    dicts. Returns (records, envelope_recognized).
    """
    if not isinstance(data, dict):
        return [], False

    for k, v in data.items():
        if k.endswith("DataBag") and isinstance(v, list):
            return v, True

    for k in ("results", "patents", "data", "items", "docs"):
        v = data.get(k)
        if isinstance(v, list):
            return v, True

    for v in data.values():
        if isinstance(v, list) and v and isinstance(v[0], dict):
            return v, True

    return [], False


def pick(record, keys):
    """First non-empty value for `keys`, searching one level of nesting."""
    for k in keys:
        v = record.get(k)
        if isinstance(v, str) and v.strip():
            return v.strip()
    for v in record.values():
        if isinstance(v, dict):
            got = pick(v, keys)
            if got:
                return got
    return ""


def record_to_item(record, query, scrape_ts):
    title = pick(record, TITLE_KEYS)
    if not title:
        return None
    pid = pick(record, ID_KEYS)
    return {
        "source": "patents",
        "content_type": "patent",
        "content": title[:2000],
        "url": f"https://patents.google.com/patent/{pid}" if pid else "",
        "author": "",
        "query": query,
        "pub_date": pick(record, DATE_KEYS),
        "scraped_at": scrape_ts,
    }


def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    key = api_key()

    if not key:
        print("[patents] no USPTO_API_KEY configured — skipping. Every previous "
              "free source is retired (Google Patents RSS 404, PatentsView 301, "
              "Espacenet 403); the Open Data Portal API needs a key. Request one at "
              "https://data.uspto.gov/apis then export USPTO_API_KEY to enable "
              "this source.", file=sys.stderr)
        print(f"[patents] 0 patents (source disabled); leaving {OUTPUT_FILE} untouched")
        return 0

    scrape_ts = datetime.datetime.now(datetime.timezone.utc).isoformat()
    all_items = []
    seen = set()
    unrecognized = 0

    for i, query in enumerate(PATENT_QUERIES):
        if i > 0:
            time.sleep(PACE_SECONDS)

        data = fetch_odp(query, key)
        if data is None:
            continue

        records, recognized = find_records(data)
        if not recognized:
            unrecognized += 1
            # Surface the real shape rather than reporting a silent zero.
            print(f"[patents] unrecognized response envelope for '{query}'; "
                  f"top-level keys: {sorted(data.keys())[:12]}", file=sys.stderr)
            continue

        for record in records:
            if not isinstance(record, dict):
                continue
            item = record_to_item(record, query, scrape_ts)
            if not item:
                continue
            dedupe_on = item["url"] or item["content"]
            if dedupe_on in seen:
                continue
            seen.add(dedupe_on)
            all_items.append(item)

    if not all_items:
        # Do NOT truncate OUTPUT_FILE — a failed run must not destroy a prior harvest.
        print(f"[patents] 0 patents this run; leaving {OUTPUT_FILE} untouched")
        if unrecognized:
            print(f"[patents] {unrecognized} response(s) had an unrecognized shape — "
                  "the ODP envelope likely differs from the assumed form; see keys logged above.",
                  file=sys.stderr)
        return 1

    with open(OUTPUT_FILE, "w") as f:
        for item in all_items:
            f.write(json.dumps(item) + "\n")

    print(f"[patents] {len(all_items)} AI patents from {len(PATENT_QUERIES)} queries -> {OUTPUT_FILE}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
