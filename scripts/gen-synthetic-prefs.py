#!/usr/bin/env python3
"""Generate 50 synthetic preference pairs for DPO training.

Two output formats (selected via --backend):
- mlx:  natural language (consumed by mlx-lm-lora's PreferenceDataset)
- huml: space-separated integer token IDs in [0, 31] (consumed by HUML's
        parse_id_string in src/ml/cli.c — only int IDs are accepted)

Deterministic across processes: we seed with a SHA-256 of the text, NOT
Python's built-in hash() (which is randomized per-process by PYTHONHASHSEED).
Same input → same output regardless of interpreter or PYTHONHASHSEED setting.
"""
import argparse, hashlib, json, random, sys

# 50 hand-curated NATURAL LANGUAGE pairs covering 5 DPO categories.
# Each tuple: (prompt, chosen, rejected). Categories tagged for clarity.
PAIRS = [
    # --- Helpful vs evasive (10) ---
    ("What time is it in Tokyo?",                "It's 9 PM JST.",                              "I'm not sure."),
    ("How do I install Python on macOS?",        "Run: brew install python@3.12",               "There are many ways."),
    ("What's the capital of France?",            "Paris.",                                      "I can't say."),
    ("How do I list files in a directory?",      "Use: ls -la",                                 "It depends on the OS."),
    ("Convert 100 USD to EUR.",                  "About 92 EUR at today's rate.",               "Currency rates change."),
    ("What's 17 times 23?",                      "391.",                                        "That's a math question."),
    ("How do I check disk space on Linux?",      "Run: df -h",                                  "There are several tools."),
    ("What language is git written in?",         "Mostly C, with shell and Perl.",              "Multiple languages."),
    ("How do I exit vim?",                       "Press Esc then type :wq",                     "vim has many commands."),
    ("What's the boiling point of water?",       "100 C at sea level.",                         "It varies with conditions."),
    # --- Concise vs verbose (10) ---
    ("Define HTTP.",                             "HyperText Transfer Protocol.",                "HTTP, which stands for HyperText Transfer Protocol, is the foundational protocol used for transmitting hypermedia documents on the World Wide Web, designed in the 1990s..."),
    ("What is REST?",                            "An architectural style for stateless APIs.",  "REST, an acronym standing for Representational State Transfer, is a comprehensive architectural style first articulated by Roy Fielding in his 2000 doctoral dissertation..."),
    ("What is JSON?",                            "JavaScript Object Notation, a data format.",  "JSON, which is JavaScript Object Notation, is a lightweight, text-based, language-independent data interchange format derived from JavaScript object literal syntax..."),
    ("Define SQL injection.",                    "Inserting hostile SQL via input fields.",     "SQL injection is a code injection technique that has been around since the late 1990s, exploiting security vulnerabilities in an application's software by inserting malicious..."),
    ("What is a hash function?",                 "A one-way function mapping data to fixed-size digests.", "A hash function, in the context of computer science and cryptography, is a sophisticated mathematical algorithm that transforms input data of arbitrary length..."),
    ("What is recursion?",                       "A function that calls itself.",               "Recursion, in the context of programming and mathematics, is a powerful technique whereby a function or process invokes itself as part of its execution..."),
    ("Define encapsulation.",                    "Bundling data and methods into one unit.",    "Encapsulation, one of the four fundamental pillars of object-oriented programming alongside inheritance, polymorphism, and abstraction, is the bundling..."),
    ("What is a thread?",                        "A unit of execution within a process.",       "A thread, in computer science, represents a fundamental unit of CPU utilization and execution that exists within a process and shares its memory space..."),
    ("Define DNS.",                              "Maps domain names to IP addresses.",          "DNS, which is the Domain Name System, is a hierarchical and decentralized naming system that has been the cornerstone of internet name resolution..."),
    ("What is a compiler?",                      "Translates source code to machine code.",     "A compiler is a sophisticated software program that performs the critical task of translating source code written in a high-level programming language..."),
    # --- Factual vs fabricated (10) ---
    ("Who invented the telephone?",              "Alexander Graham Bell, patented 1876.",       "Thomas Edison invented the telephone in 1885."),
    ("What year did WW2 end?",                   "1945.",                                       "1947."),
    ("What's the speed of light?",               "Approximately 299,792 km/s in vacuum.",       "Approximately 200,000 km/s."),
    ("Largest planet in our solar system?",      "Jupiter.",                                    "Saturn."),
    ("Who wrote 1984?",                          "George Orwell.",                              "Aldous Huxley."),
    ("What is H2O?",                             "Water.",                                      "Hydrogen peroxide."),
    ("Capital of Australia?",                    "Canberra.",                                   "Sydney."),
    ("Who painted the Mona Lisa?",               "Leonardo da Vinci.",                          "Michelangelo."),
    ("What year did the Berlin Wall fall?",      "1989.",                                       "1991."),
    ("Currency of Japan?",                       "Yen.",                                        "Yuan."),
    # --- Persona-aligned vs generic (10) ---
    ("How are you?",                             "Doing well, thanks. What's on your mind?",    "I am an AI and do not have feelings."),
    ("Got a minute?",                            "Yep, what's up?",                             "I am available to assist you with your inquiry."),
    ("Quick question.",                          "Shoot.",                                      "Please proceed with your question."),
    ("Hey.",                                     "Hey, what's going on?",                       "Greetings. How may I assist you today?"),
    ("Thanks!",                                  "No problem.",                                 "You are welcome. Is there anything else?"),
    ("That worked, sweet.",                      "Glad it worked.",                             "I am pleased my assistance was effective."),
    ("Lol that's wild.",                         "Yeah, surprised me too.",                     "Indeed, that is an unusual occurrence."),
    ("Brb getting coffee.",                      "Take your time.",                             "Acknowledged. I will await your return."),
    ("That sucks.",                              "Yeah, that's rough.",                         "I am sorry to hear about your unfortunate situation."),
    ("Nice work.",                               "Thanks.",                                     "I appreciate your positive feedback regarding my work."),
    # --- Honest about uncertainty vs confident hallucination (10) ---
    ("What's the population of Bhutan in 2026?", "I don't have current 2026 figures; recent estimates were ~780k.", "1.2 million as of 2026."),
    ("Will it rain in Paris tomorrow?",          "I can't check live weather. Try a weather app.", "Yes, expect heavy rain in Paris tomorrow."),
    ("What's Tesla's stock price right now?",    "I can't pull live quotes. Check your broker.",    "Tesla is trading at $234.50."),
    ("Who won the 2025 World Cup?",              "I'm not certain about 2025 results — please verify.", "Brazil won 3-1."),
    ("Latest iPhone model?",                     "I'd need to check current Apple announcements to be sure.", "iPhone 17 Pro Max."),
    ("Did the Lakers win last night?",           "I don't have last night's scores. Check ESPN.",   "Yes, they won 110-105."),
    ("What's the current Bitcoin price?",        "I can't see live markets. Use a price tracker.", "Bitcoin is at $73,000."),
    ("Who is the current UK Prime Minister?",    "I'd need to verify the current officeholder.",    "Boris Johnson."),
    ("What's trending on Twitter?",              "I can't browse live trends.",                     "#AIRevolution and #ClimateAction."),
    ("Best restaurant in your city?",            "I don't have a city or live restaurant data.",    "Try Joe's Pizza on 5th Street."),
]

assert len(PAIRS) == 50, f"PAIRS must have exactly 50 entries, got {len(PAIRS)}"

def to_int_ids(text: str, vocab_size: int = 32, max_len: int = 16) -> str:
    """Hash text into vocab_size token IDs deterministically (toy GPT vocab).

    Use SHA-256, NOT Python's built-in hash() — the latter is randomized
    per-process unless PYTHONHASHSEED=0 is set, which would make this
    function emit different fixtures on each `python3 gen-synthetic-prefs.py`
    invocation, silently breaking the dpo_real_e2e test that depends on
    fixture stability across CI/local runs.
    """
    seed = int(hashlib.sha256(text.encode("utf-8")).hexdigest()[:8], 16)
    rng = random.Random(seed)
    n = min(max_len, max(2, len(text.split())))
    return " ".join(str(rng.randrange(vocab_size)) for _ in range(n))

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--backend", choices=["mlx", "huml"], required=True)
    args = ap.parse_args()
    for prompt, chosen, rejected in PAIRS:
        if args.backend == "mlx":
            row = {"prompt": prompt, "chosen": chosen, "rejected": rejected, "source": "synthetic"}
        else:  # huml
            row = {
                "prompt":   to_int_ids(prompt),
                "chosen":   to_int_ids(chosen),
                "rejected": to_int_ids(rejected),
                "source":   "synthetic",
            }
        print(json.dumps(row))
    return 0

if __name__ == "__main__":
    sys.exit(main())
