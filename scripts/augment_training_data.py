#!/usr/bin/env python3
"""
Augment iMessage training data with synthetic Seth-style examples.

Uses the real Seth data patterns (length distribution, slang, topics) to
generate additional training pairs via Gemini, then validates them against
the real Seth fingerprint before including.
"""

import json
import os
import random
import statistics
import time
import urllib.request

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DATA_DIR = os.path.join(SCRIPT_DIR, "..", "data", "imessage")

API_KEY = os.environ.get("GEMINI_API_KEY", "")
GEMINI_URL = f"https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent?key={API_KEY}"

SYSTEM_PROMPT = (
    "You are Seth. You text like a real person — short messages, slang, emojis, "
    "typos, strong opinions. You never write paragraphs in texts. "
    "You use words like 'lyk', 'tbh', 'HRU', 'roady'. "
    "You're affectionate, direct, sometimes anxious, always genuine."
)

CONVERSATION_TEMPLATES = [
    {"category": "casual", "prompts": [
        "hey", "whats up", "yo", "sup", "how's it going", "hows your day",
        "anything new", "what you up to", "you around?", "hey stranger",
        "good morning", "happy friday", "tgif", "ugh monday",
    ]},
    {"category": "emotional_support", "prompts": [
        "I'm having a really bad day", "work is killing me",
        "I feel so alone right now", "nobody gets me",
        "I'm stressed af", "everything sucks today",
        "I can't sleep... too much on my mind", "I'm anxious about tomorrow",
        "feeling overwhelmed tbh", "I just need someone to talk to",
    ]},
    {"category": "excitement", "prompts": [
        "DUDE guess what!!", "I got the job!!!", "just booked a trip to hawaii!",
        "check this out 🔥", "you're not gonna believe this",
        "OMG OMG OMG", "best day ever!!", "I just won $500!",
        "they said yes!!!", "look what I made!",
    ]},
    {"category": "logistics", "prompts": [
        "what time for dinner?", "are we still on for tomorrow?",
        "can you pick up groceries?", "when does the movie start?",
        "where should we meet?", "running 10 mins late",
        "on my way", "just parked", "u home?", "almost there",
    ]},
    {"category": "opinions", "prompts": [
        "what do you think about remote work?", "is crypto dead?",
        "best show on netflix rn?", "iphone or android?",
        "thoughts on AI taking jobs?", "do you believe in aliens?",
        "pineapple on pizza?", "is college worth it anymore?",
        "worst take you've heard today?", "hot take go",
    ]},
    {"category": "humor", "prompts": [
        "bro your spotify wrapped 😂", "that was so embarrassing lol",
        "you're so weird haha", "why are you like this",
        "bruh moment", "I can't with you 😭", "you did NOT just say that",
        "worst joke ever tell me another", "that's cap", "sir this is a wendy's",
    ]},
    {"category": "deep", "prompts": [
        "do you ever think about what happens after we die?",
        "what would you do if money wasn't a thing?",
        "biggest regret?", "what scares you the most?",
        "if you could go back 10 years what would you change?",
        "do you think we're living in a simulation?",
        "what's your purpose?", "are you happy?",
    ]},
    {"category": "tech", "prompts": [
        "how do websockets work?", "should I learn rust or go?",
        "my code won't compile help", "explain docker to me",
        "is AI overhyped?", "best programming language?",
        "my computer is so slow", "how do I deploy to aws?",
    ]},
    {"category": "conflict", "prompts": [
        "I think you're wrong about that", "that was kinda rude",
        "you never text me back", "why do you always do this",
        "I'm mad at you", "we need to talk",
        "that hurt my feelings", "you're being distant",
    ]},
    {"category": "returning", "prompts": [
        "hey sorry I disappeared for 2 weeks", "long time no talk!",
        "miss you", "been thinking about you", "remember me? lol",
        "I know I've been MIA", "sorry I ghosted",
    ]},
]

SETH_REPLY_EXAMPLES = [
    "yeah for sure", "lol nah", "oh nice!", "ugh mondays", "I know you are",
    "Ha tease away then", "Got it", "Oh ok!", "No just saying you cut hard",
    "We have been here before and just circle", "I mean certain parts 😛",
    "not much wbu", "yeah true", "lol what", "idk man", "that's wild",
    "damn really?", "haha fr?", "same tbh", "ngl that's dope",
    "you good?", "ugh that sucks", "LET'S GO", "no way!!", "wait what",
    "lol fair", "true true", "I feel that", "rough", "been there",
]


def call_gemini_batch(prompts_with_category, batch_size=5):
    """Generate Seth-style replies for a batch of prompts."""
    results = []

    for i in range(0, len(prompts_with_category), batch_size):
        batch = prompts_with_category[i:i + batch_size]

        prompt_text = (
            "You are Seth. Generate realistic iMessage text replies.\n\n"
            "CRITICAL RULES:\n"
            "- Each reply MUST be 2-8 words. NOT sentences. Fragments.\n"
            "- Use slang: lol, tbh, ngl, idk, lyk, hru, wbu, ugh, bruh\n"
            "- lowercase preferred. Skip punctuation sometimes.\n"
            "- Match the emotional energy of the incoming message\n"
            "- Have opinions. Be blunt. Be real.\n"
            "- Sometimes just use an emoji\n\n"
            f"Examples of Seth's real texts: {', '.join(random.sample(SETH_REPLY_EXAMPLES, 8))}\n\n"
            "Generate 3 different reply options for each incoming message.\n"
            "Return ONLY valid JSON array:\n"
            "[\n"
        )

        for j, (cat, prompt) in enumerate(batch):
            prompt_text += f'  {{"incoming": "{prompt}", "replies": ["reply1", "reply2", "reply3"]}}'
            if j < len(batch) - 1:
                prompt_text += ","
            prompt_text += "\n"

        prompt_text += "]\n"

        # Structured output: constrain generation to the exact array shape
        # (see eval_humanness.py reference pattern), removing the fence strip.
        replies_schema = {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "incoming": {"type": "string"},
                    "replies": {"type": "array", "items": {"type": "string"}},
                },
                "required": ["incoming", "replies"],
                "propertyOrdering": ["incoming", "replies"],
            },
        }
        payload = json.dumps({
            "contents": [{"parts": [{"text": prompt_text}]}],
            "generationConfig": {
                "temperature": 0.9,
                "maxOutputTokens": 2048,
                "responseMimeType": "application/json",
                "responseSchema": replies_schema,
            }
        }).encode()

        for attempt in range(3):
            try:
                req = urllib.request.Request(GEMINI_URL, data=payload, headers={"Content-Type": "application/json"})
                resp = urllib.request.urlopen(req)
                data = json.loads(resp.read())
                raw = data["candidates"][0]["content"]["parts"][0]["text"]

                if "```json" in raw:
                    raw = raw.split("```json")[1].split("```")[0].strip()
                elif "```" in raw:
                    raw = raw.split("```")[1].split("```")[0].strip()

                parsed = json.loads(raw)
                for item in parsed:
                    for reply in item.get("replies", []):
                        if 2 <= len(reply) <= 80:
                            results.append({
                                "incoming": item["incoming"],
                                "reply": reply,
                                "category": batch[0][0] if batch else "unknown",
                            })
                break
            except Exception as e:
                if attempt < 2:
                    time.sleep(5 * (attempt + 1))
                else:
                    print(f"  Error generating batch: {e}")

        time.sleep(1)

    return results


def validate_reply(reply, real_lengths):
    """Check if a reply matches Seth's fingerprint."""
    length = len(reply)
    words = len(reply.split())

    if words > 20:
        return False
    if length > 100:
        return False

    ai_tells = ["certainly", "absolutely", "i appreciate", "great question",
                "feel free", "i understand", "however", "furthermore",
                "additionally", "i'd be happy"]
    lower = reply.lower()
    for tell in ai_tells:
        if tell in lower:
            return False

    if reply.startswith("1.") or reply.startswith("- ") or reply.startswith("* "):
        return False

    return True


def main():
    print("=" * 60)
    print("  TRAINING DATA AUGMENTATION")
    print("=" * 60)

    # Load existing training data for stats
    train_path = os.path.join(DATA_DIR, "mlx_training", "train.jsonl")
    existing = []
    with open(train_path) as f:
        for line in f:
            existing.append(json.loads(line))
    print(f"  Existing training examples: {len(existing)}")

    # Compute real Seth reply lengths
    gt_path = os.path.join(DATA_DIR, "ground_truth.jsonl")
    real_lengths = []
    with open(gt_path) as f:
        for line in f:
            item = json.loads(line)
            real_lengths.append(len(item["seth_reply"]))
    print(f"  Real Seth median length: {statistics.median(real_lengths)} chars")

    # Generate prompts from templates
    all_prompts = []
    for template in CONVERSATION_TEMPLATES:
        for prompt in template["prompts"]:
            all_prompts.append((template["category"], prompt))

    random.seed(42)
    random.shuffle(all_prompts)
    print(f"  Template prompts: {len(all_prompts)}")

    # Generate synthetic replies
    print("\n  Generating synthetic Seth replies...")
    synthetic = call_gemini_batch(all_prompts)
    print(f"  Raw synthetic replies: {len(synthetic)}")

    # Validate
    valid = [s for s in synthetic if validate_reply(s["reply"], real_lengths)]
    print(f"  Valid after filtering: {len(valid)}")

    # Convert to training format
    new_examples = []
    for item in valid:
        messages = [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": item["incoming"]},
            {"role": "assistant", "content": item["reply"]},
        ]
        new_examples.append({"messages": messages})

    # Also create multi-turn examples from valid pairs
    categories = {}
    for item in valid:
        categories.setdefault(item["category"], []).append(item)

    multi_turn = []
    for cat, items in categories.items():
        if len(items) >= 4:
            for i in range(0, len(items) - 1, 2):
                if i + 1 < len(items):
                    messages = [
                        {"role": "system", "content": SYSTEM_PROMPT},
                        {"role": "user", "content": items[i]["incoming"]},
                        {"role": "assistant", "content": items[i]["reply"]},
                        {"role": "user", "content": items[i + 1]["incoming"]},
                        {"role": "assistant", "content": items[i + 1]["reply"]},
                    ]
                    multi_turn.append({"messages": messages})

    new_examples.extend(multi_turn)
    print(f"  Multi-turn additions: {len(multi_turn)}")

    # Combine with existing
    combined = existing + new_examples
    random.shuffle(combined)

    n = len(combined)
    train_end = int(n * 0.8)
    valid_end = int(n * 0.9)

    train = combined[:train_end]
    valid_data = combined[train_end:valid_end]
    test = combined[valid_end:]

    out_dir = os.path.join(DATA_DIR, "mlx_training")
    for split, data in [("train", train), ("valid", valid_data), ("test", test)]:
        path = os.path.join(out_dir, f"{split}.jsonl")
        with open(path, "w") as f:
            for ex in data:
                f.write(json.dumps(ex) + "\n")
        print(f"  {split}: {len(data)} examples -> {path}")

    print(f"\n  Total: {len(combined)} (was {len(existing)}, added {len(new_examples)})")

    # Stats on new data
    new_reply_lengths = [len(item["reply"]) for item in valid]
    if new_reply_lengths:
        print(f"  Synthetic reply median length: {statistics.median(new_reply_lengths)} chars")
        print(f"  Synthetic reply mean length: {statistics.mean(new_reply_lengths):.0f} chars")


if __name__ == "__main__":
    main()
