#!/usr/bin/env python3
"""Scrape Semantic Scholar API for trending AI papers.

Uses the Semantic Scholar Academic Graph API for recent highly-cited AI papers
with abstracts, citation counts, and influence scores.

Rate limits (measured 2026-07-26): the UNAUTHENTICATED pool is heavily
throttled but INTERMITTENTLY available — most requests return HTTP 429, yet
some slip through (an observed run harvested 5 papers while other queries in
the same run were refused). So retries and per-query persistence buy real
yield here; do not give up after the first 429.

A key raises the limit substantially and is strongly recommended. Request one
(free) at:

    https://www.semanticscholar.org/product/api#api-key-form

then export it so this script can authenticate:

    export S2_API_KEY=...        # or SEMANTIC_SCHOLAR_API_KEY

Every query is attempted with exponential backoff + jitter (honoring
Retry-After). If ALL queries end up throttled and no key is configured, the
script prints a message naming the env var and exits 0 — a missing key is a
configuration gap, not a scraper failure. Genuine errors still exit 1.
"""
import json, os, sys, datetime, urllib.request, urllib.error, urllib.parse, time, random

OUTPUT_DIR = os.path.expanduser("~/.human/feeds/ingest")
OUTPUT_FILE = os.path.join(OUTPUT_DIR, "semantic_scholar.jsonl")

S2_API = "https://api.semanticscholar.org/graph/v1/paper/search"
S2_FIELDS = "title,abstract,authors,year,citationCount,influentialCitationCount,url,publicationDate,externalIds"

QUERIES = [
    "autonomous AI agent tool use",
    "large language model inference optimization",
    "retrieval augmented generation",
    "AI code generation assistant",
]

# Authenticated S2 allows ~1 req/sec; unauthenticated shares a saturated pool.
PACE_SECONDS = 1.5
MAX_ATTEMPTS = 4
BACKOFF_BASE = 4.0
BACKOFF_CAP = 30.0


class Throttled(Exception):
    """HTTP 429 — the caller decides whether to keep going."""


def api_key():
    for var in ("S2_API_KEY", "SEMANTIC_SCHOLAR_API_KEY"):
        key = os.environ.get(var, "").strip()
        if key:
            return key
    return None


def retry_delay(attempt, retry_after):
    """Honor Retry-After when the server sends it, else exponential + jitter."""
    if retry_after:
        try:
            return min(float(retry_after), BACKOFF_CAP)
        except (TypeError, ValueError):
            pass
    return min(BACKOFF_BASE * (2 ** attempt), BACKOFF_CAP) + random.uniform(0, 1.5)


def fetch_s2(query, key, limit=10):
    """Return parsed JSON, or raise Throttled if 429 survives all attempts."""
    params = urllib.parse.urlencode({
        "query": query,
        "limit": limit,
        "fields": S2_FIELDS,
        "sort": "citationCount:desc",
        "year": "2024-2026",
    })
    url = f"{S2_API}?{params}"
    headers = {"User-Agent": "h-uman-feed/1.0"}
    if key:
        headers["x-api-key"] = key

    for attempt in range(MAX_ATTEMPTS):
        try:
            req = urllib.request.Request(url, headers=headers)
            with urllib.request.urlopen(req, timeout=30) as r:
                return json.loads(r.read())
        except urllib.error.HTTPError as e:
            # 429 and 5xx are transient; anything else is a real error.
            if e.code != 429 and e.code < 500:
                print(f"[semantic_scholar] Error fetching '{query}': HTTP {e.code}", file=sys.stderr)
                return None
            if attempt == MAX_ATTEMPTS - 1:
                if e.code == 429:
                    raise Throttled(query)
                print(f"[semantic_scholar] Error fetching '{query}': HTTP {e.code} (gave up)",
                      file=sys.stderr)
                return None
            delay = retry_delay(attempt, e.headers.get("Retry-After") if e.headers else None)
            print(f"[semantic_scholar] HTTP {e.code} on '{query}', "
                  f"retry {attempt + 1}/{MAX_ATTEMPTS - 1} in {delay:.1f}s", file=sys.stderr)
            time.sleep(delay)
        except Exception as e:
            print(f"[semantic_scholar] Error fetching '{query}': {e}", file=sys.stderr)
            return None
    return None


def paper_to_item(paper, scrape_ts):
    title = paper.get("title", "")
    if not title:
        return None

    authors = [a.get("name", "") for a in (paper.get("authors") or [])[:5]]
    authors = [a for a in authors if a]
    author_str = ", ".join(authors[:3])
    if len(authors) > 3:
        author_str += f" +{len(authors) - 3} more"

    abstract = paper.get("abstract", "") or ""
    citations = paper.get("citationCount", 0) or 0
    influential = paper.get("influentialCitationCount", 0) or 0
    pub_date = paper.get("publicationDate", "") or ""
    year = paper.get("year", "") or ""
    pid = paper.get("paperId", "")
    url = paper.get("url", "") or f"https://www.semanticscholar.org/paper/{pid}"

    ext_ids = paper.get("externalIds") or {}

    content = f"{title}\n\nAuthors: {author_str}"
    if citations:
        content += f"\nCitations: {citations}"
    if influential:
        content += f" ({influential} influential)"
    if abstract:
        content += f"\n\n{abstract}"

    return {
        "source": "semantic_scholar",
        "content_type": "paper",
        "content": content[:2000],
        "url": url,
        "author": author_str,
        "title": title,
        "citations": citations,
        "influential_citations": influential,
        "published": pub_date or str(year),
        "arxiv_id": ext_ids.get("ArXiv", ""),
        "doi": ext_ids.get("DOI", ""),
        "scraped_at": scrape_ts,
    }


def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    scrape_ts = datetime.datetime.now(datetime.timezone.utc).isoformat()
    key = api_key()
    all_items = []
    seen_ids = set()
    throttled = 0

    for i, query in enumerate(QUERIES):
        if i > 0:
            time.sleep(PACE_SECONDS)

        try:
            data = fetch_s2(query, key, limit=5)
        except Throttled:
            # The unauthenticated pool refuses most requests but lets some
            # through, so keep going — a later query may well succeed.
            throttled += 1
            print(f"[semantic_scholar] throttled on '{query}' — continuing", file=sys.stderr)
            continue

        if not data or "data" not in data:
            continue

        for paper in data["data"]:
            pid = paper.get("paperId", "")
            if not pid or pid in seen_ids:
                continue
            seen_ids.add(pid)
            item = paper_to_item(paper, scrape_ts)
            if item:
                all_items.append(item)

    if not all_items:
        # Do NOT truncate OUTPUT_FILE — a throttled run must not destroy the
        # previous harvest by overwriting it with an empty file.
        print(f"[semantic_scholar] 0 papers this run; leaving {OUTPUT_FILE} untouched")
        if throttled and not key:
            # Missing configuration, not a scraper fault: exit clean so launchd
            # does not report a spurious hard failure every 6 hours.
            print(f"[semantic_scholar] all {throttled} queries throttled and no API key "
                  "configured — set S2_API_KEY to enable this source: "
                  "https://www.semanticscholar.org/product/api#api-key-form", file=sys.stderr)
            return 0
        return 1

    all_items.sort(key=lambda x: x.get("citations", 0), reverse=True)
    with open(OUTPUT_FILE, "w") as f:
        for item in all_items:
            f.write(json.dumps(item) + "\n")

    print(f"[semantic_scholar] {len(all_items)} papers from {len(QUERIES)} queries -> {OUTPUT_FILE}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
