#!/usr/bin/env python3
"""
Scrape Twitter/X home timeline using Playwright with Chrome cookies.
Outputs JSONL to ~/.human/feeds/ingest/twitter_YYYYMMDD.jsonl.

Cookies are extracted from the local Chrome/PWA by extract_chrome_cookies.py.

Usage:
    python scripts/scrape_twitter_feed.py [--max-tweets 50]
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
STATE_FILE = Path.home() / ".human" / "browser_state" / "twitter.json"
TWITTER_URL = "https://x.com/home"


def scrape_twitter(max_tweets: int = 50) -> list[dict]:
    tweets = []

    with sync_playwright() as p:
        browser = p.chromium.launch(headless=True)
        context = browser.new_context(
            storage_state=str(STATE_FILE) if STATE_FILE.exists() else None,
            viewport={"width": 1280, "height": 900},
            user_agent="Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36",
        )
        page = context.new_page()

        try:
            page.goto(TWITTER_URL, wait_until="domcontentloaded", timeout=30000)
        except PwTimeout:
            print("Warning: page load timed out, continuing anyway...")

        # Wait for tweet content to appear (up to 15s)
        try:
            page.wait_for_selector('[data-testid="tweetText"]', timeout=15000)
        except PwTimeout:
            print("Warning: no tweets found — may need to re-extract cookies")
            browser.close()
            return tweets

        page.wait_for_timeout(2000)

        seen_texts = set()
        scroll_attempts = 0
        max_scrolls = max_tweets // 5 + 10

        while len(tweets) < max_tweets and scroll_attempts < max_scrolls:
            tweet_elements = page.query_selector_all('[data-testid="tweetText"]')

            for el in tweet_elements:
                text = el.inner_text().strip()
                if text and text not in seen_texts:
                    seen_texts.add(text)
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

        # Update saved state with any new cookies from this session
        STATE_FILE.parent.mkdir(parents=True, exist_ok=True)
        context.storage_state(path=str(STATE_FILE))
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

    if tweets:
        INGEST_DIR.mkdir(parents=True, exist_ok=True)
        outfile = INGEST_DIR / f"twitter_{datetime.now().strftime('%Y%m%d')}.jsonl"
        with open(outfile, "w") as f:
            for t in tweets:
                f.write(json.dumps(t) + "\n")
        print(f"Wrote {len(tweets)} items to {outfile}")
    else:
        print("No tweets scraped — check cookies or login state")


if __name__ == "__main__":
    main()
