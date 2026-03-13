#!/usr/bin/env python3
"""
Scrape Facebook news feed using Playwright browser automation.
Outputs JSONL to ~/.human/feeds/ingest/facebook_YYYYMMDD.jsonl.

Facebook has no personal feed API since 2018 (post-Cambridge Analytica).
Browser automation is the only option — fragile, may break with UI changes.

Usage:
    pip install playwright
    playwright install chromium
    python scripts/scrape_facebook_feed.py [--max-posts 30]
"""

import argparse
import json
import os
import sys
from datetime import datetime
from pathlib import Path

try:
    from playwright.sync_api import sync_playwright
except ImportError:
    print("Error: playwright not installed. Run: pip install playwright && playwright install chromium")
    sys.exit(1)


INGEST_DIR = Path.home() / ".human" / "feeds" / "ingest"
FACEBOOK_URL = "https://www.facebook.com/"


def scrape_facebook(max_posts: int = 30) -> list[dict]:
    posts = []

    with sync_playwright() as p:
        browser = p.chromium.launch(headless=False)
        state_path = Path.home() / ".human" / "browser_state" / "facebook.json"
        context = browser.new_context(
            storage_state=str(state_path) if state_path.exists() else None
        )
        page = context.new_page()
        page.goto(FACEBOOK_URL, wait_until="networkidle", timeout=30000)
        page.wait_for_timeout(5000)

        seen_texts = set()
        scroll_attempts = 0
        max_scrolls = max_posts // 3 + 15

        while len(posts) < max_posts and scroll_attempts < max_scrolls:
            # Facebook post content selectors (may need updates as FB changes UI)
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

        state_dir = Path.home() / ".human" / "browser_state"
        state_dir.mkdir(parents=True, exist_ok=True)
        context.storage_state(path=str(state_dir / "facebook.json"))

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

    INGEST_DIR.mkdir(parents=True, exist_ok=True)
    outfile = INGEST_DIR / f"facebook_{datetime.now().strftime('%Y%m%d')}.jsonl"
    with open(outfile, "w") as f:
        for p_item in posts:
            f.write(json.dumps(p_item) + "\n")

    print(f"Wrote {len(posts)} items to {outfile}")


if __name__ == "__main__":
    main()
