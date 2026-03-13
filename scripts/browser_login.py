#!/usr/bin/env python3
"""
Open a Playwright browser for interactive login to a platform.
Saves browser state to ~/.human/browser_state/<platform>.json for reuse.

Usage:
    python scripts/browser_login.py twitter
    python scripts/browser_login.py facebook
    python scripts/browser_login.py tiktok
    python scripts/browser_login.py all
"""

import sys
from pathlib import Path

try:
    from playwright.sync_api import sync_playwright
except ImportError:
    print("Error: playwright not installed. Run:")
    print("  ~/.human/scraper-venv/bin/pip install playwright")
    print("  ~/.human/scraper-venv/bin/python -m playwright install chromium")
    sys.exit(1)


PLATFORMS = {
    "twitter":  "https://x.com/home",
    "facebook": "https://www.facebook.com/",
    "tiktok":   "https://www.tiktok.com/foryou",
}

STATE_DIR = Path.home() / ".human" / "browser_state"


def login_platform(name: str, url: str) -> None:
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    state_file = STATE_DIR / f"{name}.json"

    print(f"\nOpening {name} at {url}")
    print("Log in if needed, browse around to establish session, then close the browser window.")

    with sync_playwright() as p:
        browser = p.chromium.launch(headless=False)
        context = browser.new_context(
            storage_state=str(state_file) if state_file.exists() else None,
            viewport={"width": 1280, "height": 900},
        )
        page = context.new_page()
        page.goto(url, wait_until="networkidle", timeout=60000)

        input(f"\n  Press Enter here after you've logged into {name}...")

        context.storage_state(path=str(state_file))
        browser.close()

    print(f"  Session saved to {state_file}")


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in list(PLATFORMS.keys()) + ["all"]:
        print(f"Usage: {sys.argv[0]} <{'|'.join(list(PLATFORMS.keys()) + ['all'])}>")
        sys.exit(1)

    target = sys.argv[1]
    platforms = PLATFORMS if target == "all" else {target: PLATFORMS[target]}

    for name, url in platforms.items():
        login_platform(name, url)

    print("\nDone! Browser state saved for automated scraping.")


if __name__ == "__main__":
    main()
