#!/usr/bin/env python3
"""
Scrape Twitter/X home timeline from the installed PWA using Playwright.
Outputs JSONL to ~/.human/feeds/ingest/twitter_YYYYMMDD.jsonl.

Fallback for when Twitter API Basic tier ($100/mo) is not available.
Fragile by nature — UI changes will break selectors.

Usage:
    pip install playwright
    playwright install chromium
    python scripts/scrape_twitter_feed.py [--max-tweets 50]
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
TWITTER_URL = "https://x.com/home"


def scrape_twitter(max_tweets: int = 50) -> list[dict]:
    tweets = []

    with sync_playwright() as p:
        browser = p.chromium.launch(headless=False)
        context = browser.new_context(
            storage_state=str(Path.home() / ".human" / "browser_state" / "twitter.json")
            if (Path.home() / ".human" / "browser_state" / "twitter.json").exists()
            else None
        )
        page = context.new_page()
        page.goto(TWITTER_URL, wait_until="networkidle", timeout=30000)
        page.wait_for_timeout(3000)

        seen_texts = set()
        scroll_attempts = 0
        max_scrolls = max_tweets // 5 + 10

        while len(tweets) < max_tweets and scroll_attempts < max_scrolls:
            tweet_elements = page.query_selector_all('[data-testid="tweetText"]')

            for el in tweet_elements:
                text = el.inner_text().strip()
                if text and text not in seen_texts:
                    seen_texts.add(text)
                    # Try to get the tweet URL from parent article
                    article = el.evaluate(
                        "el => el.closest('article')?.querySelector('a[href*=\"/status/\"]')?.href"
                    )
                    tweets.append({
                        "source": "twitter",
                        "content_type": "tweet",
                        "content": text[:2000],
                        "url": article or "",
                    })

                if len(tweets) >= max_tweets:
                    break

            page.evaluate("window.scrollBy(0, window.innerHeight)")
            page.wait_for_timeout(1500)
            scroll_attempts += 1

        # Save browser state for next run
        state_dir = Path.home() / ".human" / "browser_state"
        state_dir.mkdir(parents=True, exist_ok=True)
        context.storage_state(path=str(state_dir / "twitter.json"))

        browser.close()

    return tweets


def main():
    parser = argparse.ArgumentParser(description="Scrape Twitter/X home timeline")
    parser.add_argument("--max-tweets", type=int, default=50,
                        help="Maximum tweets to scrape (default: 50)")
    args = parser.parse_args()

    print(f"Scraping up to {args.max_tweets} tweets from Twitter/X...")
    tweets = scrape_twitter(args.max_tweets)
    print(f"Scraped {len(tweets)} tweets")

    INGEST_DIR.mkdir(parents=True, exist_ok=True)
    outfile = INGEST_DIR / f"twitter_{datetime.now().strftime('%Y%m%d')}.jsonl"
    with open(outfile, "w") as f:
        for t in tweets:
            f.write(json.dumps(t) + "\n")

    print(f"Wrote {len(tweets)} items to {outfile}")


if __name__ == "__main__":
    main()
