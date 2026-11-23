#!/usr/bin/env python3
"""Make a search-term list for Enron: the kind of list an e-discovery review counts in one report.

    python3 -I bench/queries/make_terms.py $VFTS_DATA/enron/docs.dat > bench/queries/enron_terms.tsv

500 terms, the same on every run (no randomness that depends on the machine):
    300 words           spread evenly over the words found in 100 to 100,000 emails
    100 prefixes        the first five letters of other such words, then *  (fraud*)
    50 phrases          two-word phrases found in at least 200 emails       ("gas trading")
    50 proximity pairs  two of the words within five words of each other   (contract w/5 price)

Words: 4 to 12 letters a-z, lower case, not in the stopword list below. docs.dat: records
"doc_id \\x01 lang \\x01 body \\x02" (bench/data/prepare.py). Python standard library only.
"""
import random
import re
import sys
from collections import Counter

STOP = set("""
able about above after again against also although always among another anyone anything
around because been before being below between both cannot could does doing down during each
either else enough even ever every from further have having here hers herself himself however
into itself just know like made make many might more most much must myself need neither never
none only other otherwise ours ourselves over same shall should since some such than that their
theirs them themselves then there these they this those though through thus together under until
upon very want were what whatever when where whether which while whom whose will with within
without would your yours yourself yourselves please thanks thank regards sent subject original
message forwarded http https www com html mailto
""".split())

WORD = re.compile(r"[a-z]{4,12}")


def main():
    path = sys.argv[1]
    df = Counter()
    bigram = Counter()
    with open(path, "rb") as f:
        data = f.read().decode("utf-8", "replace")
    for rec in data.split("\x02"):
        parts = rec.split("\x01", 2)
        if len(parts) < 3:
            continue
        words = WORD.findall(parts[2].lower())
        df.update(set(w for w in words if w not in STOP))
        pairs = set()
        for a, b in zip(words, words[1:]):
            if a not in STOP and b not in STOP and a != b:
                pairs.add(a + " " + b)
        bigram.update(pairs)

    pool = sorted((w for w, n in df.items() if 100 <= n <= 100000), key=lambda w: (-df[w], w))
    rnd = random.Random(20261123)
    # 300 words, evenly spaced over the pool (frequent to rare)
    step = len(pool) / 300
    words = [pool[int(i * step)] for i in range(300)]
    rest = [w for w in pool if w not in set(words)]
    # 100 prefixes of other words (five letters; each prefix once)
    prefixes, seen = [], set()
    for w in rnd.sample(rest, len(rest)):
        p = w[:5]
        if len(w) >= 6 and p not in seen and p not in STOP:
            seen.add(p)
            prefixes.append(p + "*")
        if len(prefixes) == 100:
            break
    # 50 phrases of two words, found in at least 200 emails, evenly spaced from frequent to rare
    phr = sorted((p for p, n in bigram.items() if n >= 200), key=lambda p: (-bigram[p], p))
    pstep = len(phr) / 50
    phrases = ['"' + phr[int(i * pstep)] + '"' for i in range(50)]
    # 50 proximity pairs of the words above
    near = []
    for _ in range(50):
        a, b = rnd.sample(words, 2)
        near.append(f"{a} w/5 {b}")
    terms = words + prefixes + phrases + near
    for i, t in enumerate(terms, 1):
        print(f"{i}\t{t}")


if __name__ == "__main__":
    main()
