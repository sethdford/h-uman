#!/usr/bin/env python3
"""
Scrape TikTok For You feed using Playwright browser automation.
Extracts video captions/descriptions. Video content itself requires multimodal.
Outputs JSONL to ~/.human/feeds/ingest/tiktok_YYYYMMDD.jsonl.

TikTok has no personal feed reading API — browser automation only.

Usage:
    pip install playwright
    playwright install chromium
    python scripts/scrape_tiktok_feed.py [--max-videos 30]
"""

import argparse
import json
import sys
from datetime import datetime
from pathlib import Path

try:
    from playwright.sync_api import sync_playwright
except ImportError:
    print("Error: playwright not installed. Run: pip install playwright && playwright install chromium")
    sys.exit(1)


INGEST_DIR = Path.home() / ".human" / "feeds" / "ingest"
TIKTOK_URL = "https://www.tiktok.com/foryou"


def scrape_tiktok(max_videos: int = 30) -> list[dict]:
    videos = []

    with sync_playwright() as p:
        browser = p.chromium.launch(headless=False)
        state_path = Path.home() / ".human" / "browser_state" / "tiktok.json"
        context = browser.new_context(
            storage_state=str(state_path) if state_path.exists() else None
        )
        page = context.new_page()
        page.goto(TIKTOK_URL, wait_until="networkidle", timeout=30000)
        page.wait_for_timeout(5000)

        seen_texts = set()
        scroll_attempts = 0
        max_scrolls = max_videos + 10

        while len(videos) < max_videos and scroll_attempts < max_scrolls:
            # TikTok video description selectors
            desc_elements = page.query_selector_all(
                '[data-e2e="browse-video-desc"], '
                '[data-e2e="video-desc"], '
                'span.tiktok-j2a19r-SpanText'
            )

            for el in desc_elements:
                text = el.inner_text().strip()
                if text and len(text) > 10 and text not in seen_texts:
                    seen_texts.add(text)
                    # Try to get the video URL
                    link = el.evaluate(
                        "el => el.closest('[data-e2e=\"recommend-list-item-container\"]')?.querySelector('a')?.href || ''"
                    )
                    videos.append({
                        "source": "tiktok",
                        "content_type": "video_caption",
                        "content": text[:2000],
                        "url": link or "",
                    })

                if len(videos) >= max_videos:
                    break

            # TikTok uses vertical scroll for video feed
            page.evaluate("window.scrollBy(0, window.innerHeight)")
            page.wait_for_timeout(2000)
            scroll_attempts += 1

        state_dir = Path.home() / ".human" / "browser_state"
        state_dir.mkdir(parents=True, exist_ok=True)
        context.storage_state(path=str(state_dir / "tiktok.json"))

        browser.close()

    return videos


def main():
    parser = argparse.ArgumentParser(description="Scrape TikTok For You feed")
    parser.add_argument("--max-videos", type=int, default=30,
                        help="Maximum videos to scrape (default: 30)")
    args = parser.parse_args()

    print(f"Scraping up to {args.max_videos} videos from TikTok...")
    videos = scrape_tiktok(args.max_videos)
    print(f"Scraped {len(videos)} video descriptions")

    INGEST_DIR.mkdir(parents=True, exist_ok=True)
    outfile = INGEST_DIR / f"tiktok_{datetime.now().strftime('%Y%m%d')}.jsonl"
    with open(outfile, "w") as f:
        for v in videos:
            f.write(json.dumps(v) + "\n")

    print(f"Wrote {len(videos)} items to {outfile}")


if __name__ == "__main__":
    main()
