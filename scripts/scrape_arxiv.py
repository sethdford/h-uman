#!/usr/bin/env python3
"""Scrape arXiv API for recent AI/ML papers with full abstracts.

Uses the arXiv API (export.arxiv.org/api/query) — free, no key needed.
Searches cs.AI, cs.CL, cs.LG, cs.MA categories for recent papers matching
AI agent, LLM, and systems-level keywords relevant to h-uman.
"""
import json, os, sys, time, datetime, urllib.request, urllib.parse, urllib.error
import xml.etree.ElementTree as ET

OUTPUT_DIR = os.path.expanduser("~/.human/feeds/ingest")
OUTPUT_FILE = os.path.join(OUTPUT_DIR, "arxiv.jsonl")

ARXIV_API = "https://export.arxiv.org/api/query"

# arXiv API etiquette (https://info.arxiv.org/help/api/tou.html): no more than
# one request every 3 seconds, single connection at a time. The previous
# version fired all queries back-to-back with no delay and got HTTP 429-
# throttled within ~2 requests, ingesting zero papers. REQUEST_DELAY_S is the
# proactive politeness gap; MAX_RETRIES + Retry-After honoring is the reactive
# backoff for when we get throttled anyway.
REQUEST_DELAY_S = 3.0
MAX_RETRIES = 4
DEFAULT_BACKOFF_S = 5.0

QUERIES = [
    "all:autonomous AI agent",
    "all:large language model tool use",
    "all:retrieval augmented generation",
    "all:multi-agent system LLM",
    "all:AI code generation",
    "all:model context protocol",
    "all:LLM inference optimization",
    "all:AI safety alignment",
    "all:embedding vector search",
    "all:edge AI embedded inference",
]

CATEGORIES = "cat:cs.AI OR cat:cs.CL OR cat:cs.LG OR cat:cs.MA"
NS = {"atom": "http://www.w3.org/2005/Atom", "arxiv": "http://arxiv.org/schemas/atom"}


def _retry_after_seconds(err, attempt):
    """Decide how long to wait before retrying a throttled request.

    Honors the server's Retry-After header when present (it tells us exactly
    how long to back off); otherwise falls back to exponential backoff. This
    is the SOTA behavior: trust the server's own guidance first.
    """
    if isinstance(err, urllib.error.HTTPError):
        hdr = err.headers.get("Retry-After") if err.headers else None
        if hdr:
            try:
                return max(float(hdr), DEFAULT_BACKOFF_S)
            except ValueError:
                pass  # Retry-After can be an HTTP-date; fall through to backoff
    return DEFAULT_BACKOFF_S * (2 ** attempt)  # 5, 10, 20, 40s


def fetch_arxiv(query, max_results=10):
    params = urllib.parse.urlencode({
        "search_query": f"({query}) AND ({CATEGORIES})",
        "start": 0,
        "max_results": max_results,
        "sortBy": "submittedDate",
        "sortOrder": "descending",
    })
    url = f"{ARXIV_API}?{params}"
    req = urllib.request.Request(url, headers={"User-Agent": "h-uman-feed/1.0"})
    for attempt in range(MAX_RETRIES):
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                return ET.fromstring(r.read())
        except urllib.error.HTTPError as e:
            # 429 (Too Many Requests) and 5xx are transient — back off and retry.
            # 4xx other than 429 are our fault (bad query) — don't retry.
            transient = e.code == 429 or 500 <= e.code < 600
            if not transient or attempt == MAX_RETRIES - 1:
                print(f"[arxiv] Error fetching '{query}': HTTP {e.code}", file=sys.stderr)
                return None
            wait = _retry_after_seconds(e, attempt)
            print(f"[arxiv] HTTP {e.code} on '{query}', retry {attempt + 1}/"
                  f"{MAX_RETRIES - 1} in {wait:.0f}s", file=sys.stderr)
            time.sleep(wait)
        except Exception as e:  # URLError, timeout, parse errors
            if attempt == MAX_RETRIES - 1:
                print(f"[arxiv] Error fetching '{query}': {e}", file=sys.stderr)
                return None
            wait = _retry_after_seconds(e, attempt)
            print(f"[arxiv] {type(e).__name__} on '{query}', retry "
                  f"{attempt + 1}/{MAX_RETRIES - 1} in {wait:.0f}s", file=sys.stderr)
            time.sleep(wait)
    return None


def parse_entries(root):
    entries = []
    for entry in root.findall("atom:entry", NS):
        paper_id = entry.findtext("atom:id", "", NS)
        title = entry.findtext("atom:title", "", NS).strip().replace("\n", " ")
        abstract = entry.findtext("atom:summary", "", NS).strip().replace("\n", " ")
        published = entry.findtext("atom:published", "", NS)

        authors = []
        for author in entry.findall("atom:author", NS):
            name = author.findtext("atom:name", "", NS)
            if name:
                authors.append(name)

        categories = []
        for cat in entry.findall("atom:category", NS):
            term = cat.get("term", "")
            if term:
                categories.append(term)

        pdf_url = ""
        for link in entry.findall("atom:link", NS):
            if link.get("title") == "pdf":
                pdf_url = link.get("href", "")
                break

        if title and abstract:
            entries.append({
                "id": paper_id,
                "title": title,
                "abstract": abstract[:1500],
                "authors": authors[:5],
                "categories": categories,
                "published": published,
                "pdf_url": pdf_url,
                "url": paper_id,
            })
    return entries


def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    scrape_ts = datetime.datetime.now(datetime.timezone.utc).isoformat()
    all_items = []
    seen_ids = set()

    for i, query in enumerate(QUERIES):
        if i > 0:
            time.sleep(REQUEST_DELAY_S)  # politeness gap between requests
        root = fetch_arxiv(query, max_results=5)
        if root is None:
            continue
        entries = parse_entries(root)
        for e in entries:
            if e["id"] in seen_ids:
                continue
            seen_ids.add(e["id"])
            author_str = ", ".join(e["authors"][:3])
            if len(e["authors"]) > 3:
                author_str += f" +{len(e['authors']) - 3} more"
            content = f"{e['title']}\n\nAuthors: {author_str}\nCategories: {', '.join(e['categories'])}\n\n{e['abstract']}"
            all_items.append({
                "source": "arxiv",
                "content_type": "paper",
                "content": content[:2000],
                "url": e["url"],
                "author": author_str,
                "title": e["title"],
                "published": e["published"],
                "categories": e["categories"],
                "scraped_at": scrape_ts,
            })

    with open(OUTPUT_FILE, "w") as f:
        for item in all_items:
            f.write(json.dumps(item) + "\n")

    print(f"[arxiv] {len(all_items)} papers from {len(QUERIES)} queries -> {OUTPUT_FILE}")
    if not all_items:
        sys.exit(1)


if __name__ == "__main__":
    main()
