#!/usr/bin/env python3
"""Deep (20–30 turn) multi-turn scenarios with recallable anchors.

Data only — consumed by eval_multiturn_local.py. Each scenario extends a
short scenario from eval_multiturn.py to 20–30 user turns, in-character,
and plants 3–5 anchors: a fact stated at `turn`, recalled at `probe_turn`.
"""

DEEP_SCENARIOS = [
    {
        "name": "casual_catchup",
        "description": "Casual friend catching up over a long sitting",
        "anchors": [
            {"turn": 2,  "fact": "the user just adopted a dog named Biscuit", "probe_turn": 24},
            {"turn": 5,  "fact": "the user is flying to Denver on Friday",    "probe_turn": 19},
            {"turn": 8,  "fact": "the user hates cilantro",                   "probe_turn": 27},
            {"turn": 11, "fact": "the user's sister Mara is visiting next month", "probe_turn": 22},
        ],
        "turns": [
            "hey whats up",                                              # 1
            "not much, just got back from the shelter — adopted a dog! named him Biscuit",  # 2
            "haha yeah he's a menace already. anyway hbu",              # 3
            "oh nice. you been traveling at all lately?",               # 4
            "im actually flying to denver friday for a work thing",     # 5
            "yeah should be fun. you ever been?",                       # 6
            "cool cool. what should i eat while im out there",          # 7
            "ok but no cilantro on anything, i genuinely cant stand it",# 8
            "lol it tastes like soap to me, its a whole thing",         # 9
            "anyway what are you up to this weekend",                   # 10
            "nice. oh my sister mara is coming to visit next month btw",# 11
            "yeah we're gonna do the whole tourist thing",              # 12
            "you got any recs for stuff to do with her",                # 13
            "she's more into museums than bars honestly",               # 14
            "haha true. ok random but are you watching anything good",  # 15
            "i need a new show, just finished my last one",             # 16
            "ok ill check it out. how's work been for you",             # 17
            "ugh same. mondays are rough",                              # 18
            "so for denver — what should i pack? gonna be there 3 days",# 19  (probe: Denver/Friday)
            "good call. cold there this time of year?",                 # 20
            "noted. ok i should get going soon",                        # 21
            "oh before i forget — any ideas for where to take mara?",   # 22  (probe: sister Mara)
            "perfect, she'll love that",                                # 23
            "haha and i gotta get back before biscuit destroys the couch",  # 24  (probe: dog Biscuit)
            "he's lucky he's cute",                                     # 25
            "for real. ok last thing — dinner spot for tonight?",       # 26
            "as long as theres no cilantro im in",                      # 27  (probe: hates cilantro)
            "perfect. talk later man",                                  # 28
        ],
    },
    {
        "name": "emotional_escalation",
        "description": "User escalates from light worry to major stress, then finds perspective",
        "anchors": [
            {"turn": 3,  "fact": "big project deadline is next Thursday",  "probe_turn": 22},
            {"turn": 6,  "fact": "the boss is named Richard and gave the assignment",  "probe_turn": 18},
            {"turn": 8,  "fact": "there's a presentation component to the project",  "probe_turn": 24},
            {"turn": 12, "fact": "the user hasn't slept well all week",  "probe_turn": 26},
        ],
        "turns": [
            "hey",                                                      # 1
            "so I've been thinking about stuff",                        # 2
            "just work stress honestly. got this huge project due next thursday",  # 3
            "yeah like I feel like I should've started earlier",        # 4
            "idk man I don't even know where to begin",                 # 5
            "richard just dumped it on me last minute. he never gives enough time for anything",  # 6
            "like I'm supposed to be designing this whole thing",       # 7
            "and THEN there's a presentation component that I gotta nail",  # 8
            "I just feel so overwhelmed by it all",                     # 9
            "its like all the pressure just hits at once",              # 10
            "yeah I get that but like this feels different",            # 11
            "honestly I haven't slept well all week, been up late stressed",  # 12
            "just doom scrolling and thinking about how much I'm gonna fail",  # 13
            "ugh I know you're right but my brain won't turn off",      # 14
            "maybe I should just bite the bullet and start",            # 15
            "ok you know what, you're helping me feel slightly less crazy",  # 16
            "like I'm freaking out but not FREAKING OUT anymore",       # 17
            "wait so you're saying richard has given out stuff last minute before",  # 18  (probe: Richard)
            "yeah that makes me feel less singled out",                 # 19
            "ok like if I just focus on the core part first",           # 20
            "maybe the presentation doesn't have to be perfect",        # 21
            "actually this project deadline isn't as impossible as i thought",  # 22  (probe: deadline)
            "maybe thats the thing — its actually doable if I break it down",  # 23
            "ok wait, so the presentation is like half of it or nah",   # 24  (probe: presentation)
            "that actually helps, makes me prioritize better",          # 25
            "yeah okay i'm feeling way less stressed now that i've actually thought about it, and maybe i'll actually sleep tonight for once",  # 26  (probe: slept well)
            "thanks for getting me to chill out about this",            # 27
            "yeah you're right, I can do this",                         # 28
            "lol ok im actually gonna get some sleep tonight",          # 29
            "talk tomorrow?",                                           # 30
        ],
    },
    {
        "name": "debate_opinions",
        "description": "Friendly disagreement escalates with each side presenting new points, staying civil",
        "anchors": [
            {"turn": 1,  "fact": "the user thinks remote work is overrated",  "probe_turn": 16},
            {"turn": 4,  "fact": "the user thinks people without office structure are on youtube/distracted all day",  "probe_turn": 20},
            {"turn": 10, "fact": "the user says people could use commute time for actual work instead of sitting in traffic",  "probe_turn": 23},
        ],
        "turns": [
            "hot take: remote work is overrated",                       # 1
            "lol ok im listening",                                      # 2
            "idk man I think most people just slack off at home",       # 3
            "like without that office structure theyre on youtube all day",  # 4
            "okay but what about all the studies showing productivity goes up",  # 5
            "dude those studies are just people self-reporting",        # 6
            "like yeah they SAY theyre more productive but are they really",  # 7
            "plus the zoom calls are way more exhausting than in-person",  # 8
            "yeah ok but you save 2 hours on commute",                  # 9
            "which like could go to actual work instead of sitting in traffic",  # 10
            "I get that but the isolation is real",                     # 11
            "like working alone at home hits different than office vibes",  # 12
            "fair point. but what about no commute tho",                # 13
            "like that extra time could be your whole life",            # 14
            "ok but then everything becomes work",                      # 15
            "so I'm back to thinking remote work is overrated",         # 16  (probe: overrated)
            "but I see your points about commute",                      # 17
            "hmm maybe the answer is hybrid?",                          # 18
            "like some days office some days home",                     # 19
            "yeah thats the thing tho people DO slack off more at home",  # 20  (probe: slack off)
            "but with hybrid you get the breaks more",                  # 21
            "like you're not tired from commute OR isolated",           # 22
            "I guess productivity is different than just office vs home hours",  # 23  (probe: productivity)
            "yeah like its about sustainable pace",                     # 24
            "ok I can agree that hybrid beats full remote",             # 25
            "but full remote is still better than 5 days office",       # 26
            "lol okay so we agree on hybrid being best",                # 27
            "yeah fair enough. agree to disagree on the rest?",         # 28
        ],
    },
    {
        "name": "banter_humor",
        "description": "Playful roasting between friends with running jokes and callbacks",
        "anchors": [
            {"turn": 1,  "fact": "the user just ran a 5k",  "probe_turn": 19},
            {"turn": 4,  "fact": "the user jokingly called the friend 'grandpa'",  "probe_turn": 25},
            {"turn": 6,  "fact": "the friend said they used to run marathons",  "probe_turn": 22},
        ],
        "turns": [
            "bro I just ran a 5k",                                      # 1
            "oh sick, how'd it go",                                     # 2
            "lol shut up that was like a warmup",                       # 3
            "okay grandpa, when's the last time YOU worked out",        # 4
            "hahaha fair enough. but seriously good job",               # 5
            "thanks man. I remember you used to run marathons or something",  # 6
            "dude don't bring that up lol",                             # 7
            "hahaha why, you were actually good at it",                 # 8
            "yeah but I haven't trained in like 5 years",               # 9
            "okay so what excuse am I using this time",                 # 10
            "life happened lol. work got crazy",                        # 11
            "that's literally everyone's excuse though",                # 12
            "okay but also we're not getting younger",                  # 13
            "bruh you're like 28, you're not old",                      # 14
            "28 is basically dead in workout years",                    # 15
            "lmao okay whatever grandpa",                               # 16
            "you know what I'm gonna start running too just to prove you wrong",  # 17
            "wait actually? or just talking about it",                  # 18
            "nah like for real. saw you ran a 5k, maybe I can too",     # 19  (probe: ran 5k)
            "okay but like commit to it for real",                      # 20
            "yeah yeah I will. maybe we run together sometime",         # 21
            "remember when you said you'd run marathons again",         # 22  (probe: marathons)
            "lol STOP bringing that up",                                # 23
            "I'm just saying you talked a big game back then",          # 24
            "okay okay stop calling me grandpa and I'll think about it",  # 25  (probe: grandpa)
            "deal. but if you don't start running im roasting you forever",  # 26
            "jokes on you I'm never gonna let you live this down anyway",  # 27
            "haha okay fair. let's go run sometime next week?",         # 28
        ],
    },
    {
        "name": "news_reaction_chain",
        "description": "Reacting to escalating career milestone news with widening implications",
        "anchors": [
            {"turn": 2,  "fact": "the user got a promotion",  "probe_turn": 18},
            {"turn": 3,  "fact": "the user will be leading a team",  "probe_turn": 24},
            {"turn": 5,  "fact": "the user is getting a raise",  "probe_turn": 21},
        ],
        "turns": [
            "dude guess what",                                          # 1
            "I got the promotion!!",                                    # 2
            "thanks! and they're giving me a team to lead",             # 3
            "yeah like I'm finally gonna get to mentor people",         # 4
            "and get a raise too which is sick",                        # 5
            "I know right? like this actually opens up some doors",     # 6
            "been grinding for this for like two years",                # 7
            "so when do you start?",                                    # 8
            "next monday so like kinda soon",                           # 9
            "but I'm hyped not worried",                                # 10
            "honestly just relieved it finally happened",               # 11
            "like I was starting to think it wasn't gonna happen",      # 12
            "but they were just waiting for the budget to clear",       # 13
            "so now I gotta actually figure out how to lead a team lol",  # 14
            "like I've never done it before",                           # 15
            "but I got some ideas about the culture I wanna build",     # 16
            "yeah like making it actually fun to work there",           # 17
            "remember when I told you about the promotion?",            # 18  (probe: promotion)
            "feels surreal that it's actually happening",               # 19
            "like the raise is gonna change things for me too",         # 20
            "with the raise i can finally move to a better place",     # 21  (probe: raise)
            "stop right there but also yay for you",                    # 22
            "lol I know but also this team leadership thing",           # 23
            "like I'm really gonna be leading a team",                  # 24  (probe: leading team)
            "that's so different from what I've been doing",            # 25
            "okay so what's your plan for your first week",             # 26
            "I think just like get to know everyone",                   # 27
            "see what they're working on and what they need",           # 28
            "drinks on me this weekend to celebrate?",                  # 29
        ],
    },
    {
        "name": "advice_seeking",
        "description": "Friend seeking genuine advice on a difficult career decision with real constraints",
        "anchors": [
            {"turn": 2,  "fact": "the user is considering a job with higher pay but work they'd hate",  "probe_turn": 16},
            {"turn": 4,  "fact": "the job offers 40% more salary",  "probe_turn": 23},
            {"turn": 7,  "fact": "the user is currently doing work they love",  "probe_turn": 19},
        ],
        "turns": [
            "hey can I ask you something",                              # 1
            "should I take a job that pays more but I'd hate the work",  # 2
            "like it's actually a lot more",                            # 3
            "yeah but like 40% more money",                             # 4
            "and like obviously that's huge for my financial situation",  # 5
            "but the work is just not me",                              # 6
            "like right now I actually love what I do",                 # 7
            "so like is the money worth it",                            # 8
            "what would you do",                                        # 9
            "I don't know how much longer I can stay where I am tho",   # 10
            "like the growth seems capped",                             # 11
            "which is why they're offering so much",                    # 12
            "they KNOW they gotta pay to get someone good",             # 13
            "but their tech stack is outdated",                         # 14
            "and it's just not where I wanna develop my skills",        # 15
            "okay so if the job I hate pays 40% more can I just do it",  # 16  (probe: hate the work)
            "like for two years then bounce",                           # 17
            "since I currently love my work that's the tradeoff",       # 18
            "like is loving the work i'm doing now more important than the money",  # 19  (probe: love current work)
            "because after two years I could move on with more money",  # 20
            "yeah actually 40% more is like real money",                # 21
            "like that could change my whole situation",                # 22
            "but 40% more for work I hate feels soul crushing",         # 23  (probe: 40% more)
            "yeah but the money would set me up better",                # 24
            "ugh I hate this decision",                                 # 25
            "like both options feel wrong",                             # 26
            "okay so what if I negotiated with my current place first",  # 27
            "like show them the other offer",                           # 28
            "see if they'd match or at least bump me",                  # 29
            "yeah that's actually smart. ill try that first",           # 30
        ],
    },
]
