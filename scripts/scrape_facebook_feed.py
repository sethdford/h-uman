#!/usr/bin/env python3
"""
Scrape Facebook news feed using Playwright with Chrome cookies.
Outputs JSONL to ~/.human/feeds/ingest/facebook_YYYYMMDD.jsonl.

Cookies are extracted from the local Chrome/PWA by extract_chrome_cookies.py.

Usage:
    python scripts/scrape_facebook_feed.py [--max-posts 30]
"""

import argparse
import json
import sys
from datetime import datetime
from pathlib import Path

try:
    from playwright.sync_api import sync_playwright, TimeoutError as PwTimeout
except ImportError:
    print("Error: playwright not installed. Run: pip install playwright && playwright install chromium")
    sys.exit(1)


INGEST_DIR = Path.home() / ".human" / "feeds" / "ingest"
STATE_FILE = Path.home() / ".human" / "browser_state" / "facebook.json"
FACEBOOK_URL = "https://www.facebook.com/"


def scrape_facebook(max_posts: int = 30) -> list[dict]:
    posts = []

    with sync_playwright() as p:
        browser = p.chromium.launch(headless=True)
        context = browser.new_context(
            storage_state=str(STATE_FILE) if STATE_FILE.exists() else None,
            viewport={"width": 1280, "height": 900},
            user_agent="Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36",
        )
        page = context.new_page()

        try:
            page.goto(FACEBOOK_URL, wait_until="domcontentloaded", timeout=30000)
        except PwTimeout:
            print("Warning: page load timed out, continuing anyway...")

        page.wait_for_timeout(5000)

        seen_texts = set()
        scroll_attempts = 0
        max_scrolls = max_posts // 3 + 15

        while len(posts) < max_posts and scroll_attempts < max_scrolls:
            post_elements = page.query_selector_all(
                '[data-ad-comet-preview="message"], '
                '[data-ad-preview="message"], '
                'div[dir="auto"][style*="text-align"]'
            )

            for el in post_elements:
                text = el.inner_text().strip()
                if text and len(text) > 20 and text not in seen_texts:
                    seen_texts.add(text)
                    posts.append({
                        "source": "facebook",
                        "content_type": "post",
                        "content": text[:2000],
                        "url": "",
                    })

                if len(posts) >= max_posts:
                    break

            page.evaluate("window.scrollBy(0, window.innerHeight * 1.5)")
            page.wait_for_timeout(2000)
            scroll_attempts += 1

        STATE_FILE.parent.mkdir(parents=True, exist_ok=True)
        context.storage_state(path=str(STATE_FILE))
        browser.close()

    return posts


def main():
    parser = argparse.ArgumentParser(description="Scrape Facebook news feed")
    parser.add_argument("--max-posts", type=int, default=30,
                        help="Maximum posts to scrape (default: 30)")
    args = parser.parse_args()

    print(f"Scraping up to {args.max_posts} posts from Facebook...")
    posts = scrape_facebook(args.max_posts)
    print(f"Scraped {len(posts)} posts")

    if posts:
        INGEST_DIR.mkdir(parents=True, exist_ok=True)
        outfile = INGEST_DIR / f"facebook_{datetime.now().strftime('%Y%m%d')}.jsonl"
        with open(outfile, "w") as f:
            for p_item in posts:
                f.write(json.dumps(p_item) + "\n")
        print(f"Wrote {len(posts)} items to {outfile}")
    else:
        print("No posts scraped — check cookies or login state")


if __name__ == "__main__":
    main()
