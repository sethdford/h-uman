#!/usr/bin/env python3
"""
Extract Facebook page/profile data for LoRA fine-tuning.

Uses the Facebook Graph API to pull:
1. Posts (Seth's writing voice — status updates, shares with commentary)
2. Comment threads where Seth replied (natural conversation pairs)

Outputs training_pairs.jsonl in the same format as extract_imessage_pairs.py.

Usage:
    export FB_ACCESS_TOKEN="your-token-here"
    python scripts/extract_facebook_data.py [--page-id PAGE_ID] [--limit 500]

To get an access token:
    1. Go to https://developers.facebook.com/tools/explorer/
    2. Select your app (or create one)
    3. Add permissions: pages_read_engagement, pages_read_user_content
    4. Generate token
    5. For long-lived token: exchange via OAuth endpoint
"""

import json
import os
import random
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(SCRIPT_DIR, "..", "data", "facebook")

GRAPH_API_BASE = "https://graph.facebook.com/v19.0"
RATE_LIMIT_PAUSE = 2  # seconds between paginated requests


def get_token():
    token = os.environ.get("FB_ACCESS_TOKEN", "")
    if not token:
        token_file = os.path.expanduser("~/.human/fb_token")
        if os.path.exists(token_file):
            with open(token_file) as f:
                token = f.read().strip()
    if not token:
        print("ERROR: No Facebook access token found.")
        print("Set FB_ACCESS_TOKEN env var or put token in ~/.human/fb_token")
        print("\nTo get a token:")
        print("  1. Go to https://developers.facebook.com/tools/explorer/")
        print("  2. Add permissions: pages_read_engagement, pages_read_user_content")
        print("  3. Generate User Access Token")
        sys.exit(1)
    return token


def graph_get(endpoint, token, params=None):
    if params is None:
        params = {}
    params["access_token"] = token

    if endpoint.startswith("http"):
        url = endpoint
    else:
        url = f"{GRAPH_API_BASE}/{endpoint}"

    if params and "access_token" in params:
        sep = "&" if "?" in url else "?"
        url = url + sep + urllib.parse.urlencode(params)

    for attempt in range(3):
        try:
            req = urllib.request.Request(url)
            with urllib.request.urlopen(req, timeout=30) as resp:
                return json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            body = e.read().decode("utf-8", errors="replace") if e.fp else ""
            if e.code == 429 or "rate limit" in body.lower():
                wait = 60 * (attempt + 1)
                print(f"  Rate limited, waiting {wait}s...")
                time.sleep(wait)
                continue
            print(f"  API error {e.code}: {body[:200]}")
            return None
        except Exception as e:
            print(f"  Request failed: {e}")
            if attempt < 2:
                time.sleep(5)
            else:
                return None
    return None


def fetch_all_pages(endpoint, token, params=None, max_pages=50):
    """Paginate through a Graph API endpoint, collecting all items."""
    all_items = []
    data = graph_get(endpoint, token, params)
    page = 0

    while data and page < max_pages:
        items = data.get("data", [])
        if not items:
            break
        all_items.extend(items)
        page += 1

        paging = data.get("paging", {})
        next_url = paging.get("next")
        if not next_url:
            break

        print(f"    Page {page}: {len(all_items)} items so far...")
        time.sleep(RATE_LIMIT_PAUSE)
        data = graph_get(next_url, token)

    return all_items


def get_user_info(token):
    """Get the authenticated user's name and ID."""
    data = graph_get("me", token, {"fields": "id,name"})
    if data:
        return data.get("id", ""), data.get("name", "")
    return "", ""


def fetch_posts(page_id, token, limit=500):
    """Fetch posts from a page or profile."""
    print(f"Fetching posts from {page_id}...")
    params = {
        "fields": "id,message,created_time,from,type,story",
        "limit": "100",
    }
    posts = fetch_all_pages(f"{page_id}/posts", token, params, max_pages=limit // 100 + 1)
    print(f"  Fetched {len(posts)} posts")
    return posts


def fetch_comments(post_id, token):
    """Fetch comments on a post, including replies."""
    params = {
        "fields": "id,message,created_time,from,comments{id,message,created_time,from}",
        "limit": "100",
    }
    comments = fetch_all_pages(f"{post_id}/comments", token, params, max_pages=5)
    return comments


def build_training_pairs(posts, comments_by_post, user_id, user_name):
    """
    Build training pairs from Facebook data.

    Types of pairs:
    1. Post text (Seth's voice) paired with generic prompts
    2. Comment -> Seth's reply (conversation pairs)
    3. Multi-turn comment threads where Seth participates
    """
    pairs = []
    post_only = []

    for post in posts:
        post_msg = post.get("message", "").strip()
        post_id = post.get("id", "")
        post_from = post.get("from", {})
        post_author_id = post_from.get("id", "")
        post_time = post.get("created_time", "")

        is_seth_post = post_author_id == user_id

        if not post_msg or len(post_msg) < 3:
            continue

        comments = comments_by_post.get(post_id, [])

        if is_seth_post and not comments:
            post_only.append({
                "text": post_msg,
                "timestamp": post_time,
                "source": "facebook_post",
            })
            continue

        for comment in comments:
            comment_msg = comment.get("message", "").strip()
            comment_from = comment.get("from", {})
            comment_author_id = comment_from.get("id", "")
            comment_time = comment.get("created_time", "")

            if not comment_msg or len(comment_msg) < 2:
                continue

            replies = comment.get("comments", {}).get("data", [])

            seth_replied = False
            for reply in replies:
                reply_msg = reply.get("message", "").strip()
                reply_from = reply.get("from", {})
                reply_author_id = reply_from.get("id", "")

                if reply_author_id == user_id and reply_msg:
                    seth_replied = True
                    context_msgs = []

                    if is_seth_post and post_msg:
                        context_msgs.append({
                            "role": "assistant",
                            "content": post_msg,
                        })

                    context_msgs.append({
                        "role": "user",
                        "content": comment_msg,
                    })

                    pairs.append({
                        "messages": context_msgs + [{"role": "assistant", "content": reply_msg}],
                        "metadata": {
                            "source": "facebook_comment_reply",
                            "post_id": post_id,
                            "timestamp": comment_time,
                            "reply_length": len(reply_msg),
                        }
                    })

    for p in post_only:
        text = p["text"]
        if len(text) > 200:
            continue

        prompt_templates = [
            "what's on your mind?",
            "how's it going?",
            "what's new?",
            "tell me about your day",
            "what are you thinking about?",
        ]
        prompt = random.choice(prompt_templates)

        pairs.append({
            "messages": [
                {"role": "user", "content": prompt},
                {"role": "assistant", "content": text},
            ],
            "metadata": {
                "source": "facebook_post_voice",
                "timestamp": p["timestamp"],
                "reply_length": len(text),
            }
        })

    return pairs, post_only


def main():
    import argparse
    parser = argparse.ArgumentParser(description="Extract Facebook data for LoRA fine-tuning")
    parser.add_argument("--page-id", default="me", help="Facebook Page ID or 'me' for your profile")
    parser.add_argument("--limit", type=int, default=500, help="Max posts to fetch")
    parser.add_argument("--skip-comments", action="store_true", help="Skip fetching comments (faster)")
    args = parser.parse_args()

    token = get_token()

    user_id, user_name = get_user_info(token)
    if not user_id:
        print("ERROR: Could not get user info. Token may be invalid.")
        sys.exit(1)
    print(f"Authenticated as: {user_name} (ID: {user_id})")

    posts = fetch_posts(args.page_id, token, args.limit)
    if not posts:
        print("No posts found. Check your token permissions.")
        sys.exit(1)

    text_posts = [p for p in posts if p.get("message", "").strip()]
    print(f"\n{len(text_posts)} posts have text content")

    comments_by_post = {}
    if not args.skip_comments:
        print("\nFetching comments on posts...")
        for i, post in enumerate(text_posts):
            post_id = post["id"]
            if i % 10 == 0:
                print(f"  Processing post {i+1}/{len(text_posts)}...")
            comments = fetch_comments(post_id, token)
            if comments:
                comments_by_post[post_id] = comments
            time.sleep(RATE_LIMIT_PAUSE)

        total_comments = sum(len(c) for c in comments_by_post.values())
        print(f"  Fetched {total_comments} comments across {len(comments_by_post)} posts")

    pairs, standalone_posts = build_training_pairs(
        posts, comments_by_post, user_id, user_name
    )

    os.makedirs(OUT_DIR, exist_ok=True)

    pairs_path = os.path.join(OUT_DIR, "training_pairs.jsonl")
    with open(pairs_path, "w") as f:
        for pair in pairs:
            f.write(json.dumps(pair) + "\n")
    print(f"\nWrote {len(pairs)} training pairs -> {pairs_path}")

    posts_path = os.path.join(OUT_DIR, "standalone_posts.jsonl")
    with open(posts_path, "w") as f:
        for p in standalone_posts:
            f.write(json.dumps(p) + "\n")
    print(f"Wrote {len(standalone_posts)} standalone posts -> {posts_path}")

    print(f"\n--- Summary ---")
    conv_pairs = sum(1 for p in pairs if p["metadata"]["source"] == "facebook_comment_reply")
    voice_pairs = sum(1 for p in pairs if p["metadata"]["source"] == "facebook_post_voice")
    print(f"  Conversation pairs (comment -> reply): {conv_pairs}")
    print(f"  Voice samples (post text):             {voice_pairs}")
    print(f"  Standalone posts (no comments):        {len(standalone_posts)}")
    print(f"  Total training pairs:                  {len(pairs)}")

    if pairs:
        lengths = [p["metadata"]["reply_length"] for p in pairs]
        avg_len = sum(lengths) / len(lengths)
        print(f"  Avg reply length:                      {avg_len:.0f} chars")


if __name__ == "__main__":
    main()
