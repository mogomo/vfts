# Many searches at once: batches (vfts 0.4.1)

A report, a review of a list of search terms, a nightly run of saved searches: all the questions
are known up front, and what counts is the time until every result is stored, not the time of
one search. vfts takes many queries in one statement (the parameter `queries`, or query rows next
to the view's rows) and its results can go straight into a table (`INSERT ... SELECT`, then
`COMMIT`); Elasticsearch and OpenSearch take many searches in one request (`_msearch`).

**Sets.**

- `msmarco_top10`: the 6,980 MS MARCO dev questions, top 10 each (plain text; the engines'
  `match` query).
- `enron_top10`, `enron_count`: 500 search terms on the Enron emails
  (`bench/queries/enron_terms.tsv`, made by `bench/queries/make_terms.py` from the emails
  themselves, the same on every run): 300 words from frequent to rare, 100 prefixes (`fraud*`),
  50 two-word phrases, 50 pairs within five words (`a w/5 b`). Top 10 each, or the number of
  matching emails of each.
- `msmarco_count`: the number of matching passages of each of the 6,980 questions (a question
  matches 6 million of the 8.8 million passages in the median).

**Method** (`bench/plan_c/bulk.sh`, `bulk.py`, `bulk_shared.py`, `bulk_report.py`).

- Time: the client's clock, from the start until every result is stored (vfts: written by vsql to
  a file, or inserted into a table and committed; the engines: written to a file). Connections are
  open before the clock starts, as in an application: every vsql session logs in, then waits at a
  barrier that opens when all are ready; the engines' HTTP connections are made first.
- Each configuration: one warm-up run, then 3 measured runs, the best kept (all runs are in the
  tables). Each side's best configuration is its result: vfts with 1, 2, 4 or 8 statements at
  once (each with a share of the queries), into a file or a table; the engines with batches of
  100, 500 (Enron), 1,000 or all queries per request, 1 to 22 requests at once, and with
  `max_concurrent_searches` 22.
- Counts run with the engines' caches off (`request_cache: false` in every search, and the node
  query cache off on the index), as a report counts each term once. vfts has no cache of answers
  in SQL. Its bit sets of frequent words (docs/design.md, "Counting an OR") are made in the first
  run: see the cold runs.
- Same machine and versions as the rest of the benchmark (Vertica 26.3 on one node, 22 logical
  cores; Elasticsearch 9.5.6, OpenSearch 3.10.0); one engine runs at a time; CPU governor
  `performance`.
- b2: the 500 Enron terms and the 6,980 MS MARCO questions dealt to 10 users (made for the test).
  All results go into one table with a row access policy `requester = CURRENT_USER()`. Either
  every user runs their own share as themselves (one statement each, all at once), or one service
  user (no superuser: SELECT on the view, INSERT on the table) runs all of them and writes each
  result with the user it belongs to. The second way is only for an index whose table has no row
  access policy (every user sees the same rows, so top k, counts and scores are the same for all).
- b3: the Eon test cluster (Vertica 26.3, 5 nodes of 2 cores and 15 GB each: 3 in the primary
  subcluster, 2 in a secondary one; vfts fenced). Every node keeps a full copy of the index, and a
  statement runs on the node it connects to: a batch spreads over the cluster as one statement
  per node, each with its share of the queries. The 500 Enron terms 10 or 100 times; the clients
  run on node 1. The runs vary a lot on these small virtual machines.

## Best of each (warm: the best of 3 runs after a warm-up; time until every result is stored)

| Set | vfts | Elasticsearch | OpenSearch | vfts against the faster engine |
|---|---:|---:|---:|---:|
| 6,980 MS MARCO questions, top 10 each | 2.051 s (4 statements) | 2.401 s (batches of 100, 8 at once) | 2.280 s (batches of 100, 8 at once) | 1.11x faster |
| 500 Enron search terms, top 10 each | 0.026 s (8 statements) | 0.037 s (batches of 500, 1 at once, max_concurrent_searches 22) | 0.032 s (batches of 100, 22 at once) | 1.23x faster |
| 500 Enron search terms, matches of each | 0.020 s (4 statements) | 0.015 s (batches of 100, 8 at once) | 0.019 s (batches of 100, 22 at once) | 1.33x slower |
| 6,980 MS MARCO questions, matches of each | 0.530 s (8 statements) | 2.509 s (batches of 100, 22 at once) | 2.372 s (batches of 100, 22 at once) | 4.48x faster |

## Cold: the first run after a restart of the database or the engine (no warm-up)

| Set | vfts (4 statements) | Elasticsearch | OpenSearch |
|---|---:|---:|---:|
| 6,980 MS MARCO questions, top 10 each | 2.052 s | 3.835 s | 3.401 s |
| 500 Enron search terms, top 10 each | 0.031 s | 0.771 s | 0.319 s |
| 500 Enron search terms, matches of each | 0.024 s | 0.277 s | 0.278 s |
| 6,980 MS MARCO questions, matches of each | 0.624 s | 3.251 s | 3.317 s |

## Every configuration (best of 3 runs)

| Set | Engine | Configuration | Best | Runs |
|---|---|---|---:|---|
| 6,980 MS MARCO questions, top 10 each | vfts | 4 statements | 2.051 s | 2.051, 2.054, 2.076 |
| 6,980 MS MARCO questions, top 10 each | vfts | 2 statements | 2.107 s | 2.107, 2.118, 2.144 |
| 6,980 MS MARCO questions, top 10 each | vfts | 8 statements | 2.198 s | 2.198, 2.209, 2.202 |
| 6,980 MS MARCO questions, top 10 each | vfts | 4 statements, into a table | 2.216 s | 2.216, 2.234, 2.224 |
| 6,980 MS MARCO questions, top 10 each | vfts | 1 statement | 2.622 s | 2.64, 2.622, 2.637 |
| 6,980 MS MARCO questions, top 10 each | vfts | 1 statement, into a table | 2.730 s | 2.73, 2.758, 2.777 |
| 6,980 MS MARCO questions, top 10 each | Elasticsearch | batches of 100, 8 at once | 2.401 s | 2.401, 2.494, 2.679 |
| 6,980 MS MARCO questions, top 10 each | Elasticsearch | batches of 100, 22 at once | 2.614 s | 2.626, 2.617, 2.614 |
| 6,980 MS MARCO questions, top 10 each | Elasticsearch | batches of 1000, 4 at once | 2.643 s | 2.69, 2.667, 2.643 |
| 6,980 MS MARCO questions, top 10 each | Elasticsearch | batches of 6980, 1 at once, max_concurrent_searches 22 | 2.742 s | 2.798, 2.759, 2.742 |
| 6,980 MS MARCO questions, top 10 each | Elasticsearch | batches of 6980, 1 at once | 4.024 s | 4.024, 4.032, 4.026 |
| 6,980 MS MARCO questions, top 10 each | Elasticsearch | batches of 100, 1 at once | 4.698 s | 4.775, 4.705, 4.698 |
| 6,980 MS MARCO questions, top 10 each | OpenSearch | batches of 100, 8 at once | 2.280 s | 2.28, 2.372, 2.44 |
| 6,980 MS MARCO questions, top 10 each | OpenSearch | batches of 100, 22 at once | 2.417 s | 2.464, 2.417, 2.464 |
| 6,980 MS MARCO questions, top 10 each | OpenSearch | batches of 1000, 4 at once | 2.476 s | 2.477, 2.476, 2.519 |
| 6,980 MS MARCO questions, top 10 each | OpenSearch | batches of 6980, 1 at once, max_concurrent_searches 22 | 2.576 s | 2.607, 2.576, 2.632 |
| 6,980 MS MARCO questions, top 10 each | OpenSearch | batches of 6980, 1 at once | 3.761 s | 3.761, 3.806, 3.842 |
| 6,980 MS MARCO questions, top 10 each | OpenSearch | batches of 100, 1 at once | 4.473 s | 4.661, 4.566, 4.473 |
| 500 Enron search terms, top 10 each | vfts | 8 statements | 0.026 s | 0.03, 0.026, 0.029 |
| 500 Enron search terms, top 10 each | vfts | 2 statements | 0.027 s | 0.028, 0.027, 0.028 |
| 500 Enron search terms, top 10 each | vfts | 4 statements, into a table | 0.027 s | 0.03, 0.031, 0.027 |
| 500 Enron search terms, top 10 each | vfts | 4 statements | 0.028 s | 0.03, 0.028, 0.029 |
| 500 Enron search terms, top 10 each | vfts | 1 statement | 0.032 s | 0.036, 0.034, 0.032 |
| 500 Enron search terms, top 10 each | vfts | 1 statement, into a table | 0.033 s | 0.038, 0.038, 0.033 |
| 500 Enron search terms, top 10 each | Elasticsearch | batches of 500, 1 at once, max_concurrent_searches 22 | 0.037 s | 0.04, 0.037, 0.037 |
| 500 Enron search terms, top 10 each | Elasticsearch | batches of 100, 8 at once | 0.038 s | 0.04, 0.044, 0.038 |
| 500 Enron search terms, top 10 each | Elasticsearch | batches of 100, 22 at once | 0.040 s | 0.041, 0.042, 0.04 |
| 500 Enron search terms, top 10 each | Elasticsearch | batches of 500, 1 at once | 0.048 s | 0.05, 0.052, 0.048 |
| 500 Enron search terms, top 10 each | Elasticsearch | batches of 1000, 4 at once | 0.049 s | 0.056, 0.05, 0.049 |
| 500 Enron search terms, top 10 each | Elasticsearch | batches of 100, 1 at once | 0.093 s | 0.12, 0.093, 0.094 |
| 500 Enron search terms, top 10 each | OpenSearch | batches of 100, 22 at once | 0.032 s | 0.032, 0.036, 0.034 |
| 500 Enron search terms, top 10 each | OpenSearch | batches of 500, 1 at once, max_concurrent_searches 22 | 0.035 s | 0.039, 0.035, 0.035 |
| 500 Enron search terms, top 10 each | OpenSearch | batches of 100, 8 at once | 0.036 s | 0.039, 0.074, 0.036 |
| 500 Enron search terms, top 10 each | OpenSearch | batches of 1000, 4 at once | 0.046 s | 0.046, 0.05, 0.047 |
| 500 Enron search terms, top 10 each | OpenSearch | batches of 500, 1 at once | 0.046 s | 0.046, 0.049, 0.048 |
| 500 Enron search terms, top 10 each | OpenSearch | batches of 100, 1 at once | 0.079 s | 0.114, 0.082, 0.079 |
| 500 Enron search terms, matches of each | vfts | 4 statements | 0.020 s | 0.021, 0.021, 0.02 |
| 500 Enron search terms, matches of each | vfts | 2 statements | 0.022 s | 0.023, 0.025, 0.022 |
| 500 Enron search terms, matches of each | vfts | 4 statements, into a table | 0.023 s | 0.025, 0.023, 0.024 |
| 500 Enron search terms, matches of each | vfts | 1 statement | 0.024 s | 0.024, 0.025, 0.024 |
| 500 Enron search terms, matches of each | vfts | 8 statements | 0.024 s | 0.024, 0.027, 0.026 |
| 500 Enron search terms, matches of each | vfts | 1 statement, into a table | 0.028 s | 0.028, 0.032, 0.033 |
| 500 Enron search terms, matches of each | Elasticsearch | batches of 100, 8 at once | 0.015 s | 0.017, 0.015, 0.019 |
| 500 Enron search terms, matches of each | Elasticsearch | batches of 100, 22 at once | 0.016 s | 0.016, 0.016, 0.018 |
| 500 Enron search terms, matches of each | Elasticsearch | batches of 500, 1 at once, max_concurrent_searches 22 | 0.017 s | 0.017, 0.021, 0.022 |
| 500 Enron search terms, matches of each | Elasticsearch | batches of 1000, 4 at once | 0.020 s | 0.025, 0.021, 0.02 |
| 500 Enron search terms, matches of each | Elasticsearch | batches of 500, 1 at once | 0.020 s | 0.02, 0.023, 0.023 |
| 500 Enron search terms, matches of each | Elasticsearch | batches of 100, 1 at once | 0.030 s | 0.034, 0.03, 0.031 |
| 500 Enron search terms, matches of each | OpenSearch | batches of 100, 22 at once | 0.019 s | 0.022, 0.019, 0.02 |
| 500 Enron search terms, matches of each | OpenSearch | batches of 1000, 4 at once | 0.019 s | 0.019, 0.021, 0.021 |
| 500 Enron search terms, matches of each | OpenSearch | batches of 500, 1 at once | 0.019 s | 0.019, 0.021, 0.023 |
| 500 Enron search terms, matches of each | OpenSearch | batches of 100, 8 at once | 0.020 s | 0.026, 0.022, 0.02 |
| 500 Enron search terms, matches of each | OpenSearch | batches of 500, 1 at once, max_concurrent_searches 22 | 0.023 s | 0.024, 0.023, 0.026 |
| 500 Enron search terms, matches of each | OpenSearch | batches of 100, 1 at once | 0.025 s | 0.052, 0.029, 0.025 |
| 6,980 MS MARCO questions, matches of each | vfts | 8 statements | 0.530 s | 0.541, 0.547, 0.53 |
| 6,980 MS MARCO questions, matches of each | vfts | 4 statements | 0.541 s | 0.545, 0.541, 0.545 |
| 6,980 MS MARCO questions, matches of each | vfts | 4 statements, into a table | 0.545 s | 0.547, 0.545, 0.545 |
| 6,980 MS MARCO questions, matches of each | vfts | 2 statements | 0.562 s | 0.562, 0.568, 0.566 |
| 6,980 MS MARCO questions, matches of each | vfts | 1 statement | 0.638 s | 0.649, 0.638, 0.64 |
| 6,980 MS MARCO questions, matches of each | vfts | 1 statement, into a table | 0.641 s | 0.642, 0.641, 0.644 |
| 6,980 MS MARCO questions, matches of each | Elasticsearch | batches of 100, 22 at once | 2.509 s | 2.586, 2.513, 2.509 |
| 6,980 MS MARCO questions, matches of each | Elasticsearch | batches of 100, 8 at once | 2.510 s | 2.516, 2.513, 2.51 |
| 6,980 MS MARCO questions, matches of each | Elasticsearch | batches of 1000, 4 at once | 2.550 s | 2.55, 2.559, 2.556 |
| 6,980 MS MARCO questions, matches of each | Elasticsearch | batches of 6980, 1 at once, max_concurrent_searches 22 | 2.610 s | 2.625, 2.621, 2.61 |
| 6,980 MS MARCO questions, matches of each | Elasticsearch | batches of 6980, 1 at once | 3.604 s | 3.677, 3.604, 3.668 |
| 6,980 MS MARCO questions, matches of each | Elasticsearch | batches of 100, 1 at once | 4.009 s | 4.182, 4.021, 4.009 |
| 6,980 MS MARCO questions, matches of each | OpenSearch | batches of 100, 22 at once | 2.372 s | 2.372, 2.381, 2.389 |
| 6,980 MS MARCO questions, matches of each | OpenSearch | batches of 100, 8 at once | 2.393 s | 2.435, 2.393, 2.396 |
| 6,980 MS MARCO questions, matches of each | OpenSearch | batches of 1000, 4 at once | 2.421 s | 2.435, 2.472, 2.421 |
| 6,980 MS MARCO questions, matches of each | OpenSearch | batches of 6980, 1 at once, max_concurrent_searches 22 | 2.472 s | 2.485, 2.501, 2.472 |
| 6,980 MS MARCO questions, matches of each | OpenSearch | batches of 6980, 1 at once | 3.431 s | 3.431, 3.437, 3.465 |
| 6,980 MS MARCO questions, matches of each | OpenSearch | batches of 100, 1 at once | 3.772 s | 3.772, 3.783, 3.821 |

## Many users' batches into one table (b2): each user as themselves, or one service user

| Set | Users | Each user's own statement | One service user | Same rows | Each user sees only their rows | Service user reads results |
|---|---:|---:|---:|---|---|---|
| 500 Enron search terms, top 10 each | 10 | 0.040 s (10 statements) | 0.035 s (4 statements) | yes | 10 of 10 | no |
| 6,980 MS MARCO questions, top 10 each | 10 | 2.011 s (10 statements) | 2.077 s (4 statements) | yes | 10 of 10 | no |

## A cluster (b3): the same batch on more nodes, one statement per node

| Batch | Results | Nodes | Statements | Best | Runs |
|---|---|---:|---:|---:|---|
| 5,000 Enron terms (10 x the 500), top 10 each | the client (node 1) | 1 | 1 | 1.349 s | 1.537, 1.409, 1.349 |
| 5,000 Enron terms (10 x the 500), top 10 each | the client (node 1) | 2 | 2 | 1.282 s | 1.282, 1.434, 1.584 |
| 5,000 Enron terms (10 x the 500), top 10 each | the client (node 1) | 3 | 3 | 1.171 s | 1.182, 1.171, 1.247 |
| 5,000 Enron terms (10 x the 500), top 10 each | the client (node 1) | 5 | 5 | 0.877 s | 1.001, 0.893, 0.877 |
| 5,000 Enron terms (10 x the 500), top 10 each | the client (node 1) | 1 (control) | 5 | 2.262 s | 2.654, 2.262, 3.334 |
| 5,000 Enron terms (10 x the 500), matches of each | the client (node 1) | 1 | 1 | 2.726 s | 3.315, 3.131, 2.726 |
| 5,000 Enron terms (10 x the 500), matches of each | the client (node 1) | 2 | 2 | 1.492 s | 1.533, 1.492, 1.684 |
| 5,000 Enron terms (10 x the 500), matches of each | the client (node 1) | 3 | 3 | 0.957 s | 1.049, 0.957, 0.997 |
| 5,000 Enron terms (10 x the 500), matches of each | the client (node 1) | 5 | 5 | 0.684 s | 0.76, 0.684, 0.814 |
| 5,000 Enron terms (10 x the 500), matches of each | the client (node 1) | 1 (control) | 5 | 2.636 s | 2.636, 2.849, 3.239 |
| 50,000 Enron terms (100 x the 500), top 10 each | a table of the cluster | 1 | 1 | 25.8 s | 29.362, 28.223, 25.835 |
| 50,000 Enron terms (100 x the 500), top 10 each | a table of the cluster | 2 | 2 | 10.2 s | 14.053, 10.212, 12.694 |
| 50,000 Enron terms (100 x the 500), top 10 each | a table of the cluster | 3 | 3 | 6.113 s | 6.113, 8.485, 10.706 |
| 50,000 Enron terms (100 x the 500), top 10 each | a table of the cluster | 5 | 5 | 7.919 s | 7.919, 9.428, 8.817 |
| 50,000 Enron terms (100 x the 500), top 10 each | a table of the cluster | 1 (control) | 5 | 25.6 s | 27.163, 25.569, 26.973 |
| 50,000 Enron terms (100 x the 500), matches of each | a table of the cluster | 1 | 1 | 27.3 s | 27.697, 28.862, 27.33 |
| 50,000 Enron terms (100 x the 500), matches of each | a table of the cluster | 2 | 2 | 9.421 s | 13.333, 12.692, 9.421 |
| 50,000 Enron terms (100 x the 500), matches of each | a table of the cluster | 3 | 3 | 5.158 s | 8.369, 5.808, 5.158 |
| 50,000 Enron terms (100 x the 500), matches of each | a table of the cluster | 5 | 5 | 3.558 s | 6.753, 6.316, 3.558 |
| 50,000 Enron terms (100 x the 500), matches of each | a table of the cluster | 1 (control) | 5 | 19.9 s | 21.264, 20.932, 19.894 |

## The same answers

| Set | Against | Result |
|---|---|---|
| 6,980 MS MARCO questions, top 10 each | Elasticsearch | same scores at every rank for 6,786 of 6,980; same documents above the 10th score for 6,783 |
| 6,980 MS MARCO questions, top 10 each | OpenSearch | same scores at every rank for 6,786 of 6,980; same documents above the 10th score for 6,783 |
| 500 Enron search terms, top 10 each | Elasticsearch | same scores at every rank for 442 of 443; same documents above the 10th score for 442 |
| 500 Enron search terms, top 10 each | OpenSearch | same scores at every rank for 442 of 443; same documents above the 10th score for 442 |
| 500 Enron search terms, matches of each | Elasticsearch | counts equal for 500 of 500; within 0.1% for 500 |
| 500 Enron search terms, matches of each | OpenSearch | counts equal for 500 of 500; within 0.1% for 500 |
| 6,980 MS MARCO questions, matches of each | Elasticsearch | counts equal for 4,909 of 6,980; within 0.1% for 6,934 |
| 6,980 MS MARCO questions, matches of each | OpenSearch | counts equal for 4,909 of 6,980; within 0.1% for 6,934 |

The MS MARCO counts that differ by more than 0.1% (46 of 6,980) differ in words the two English
stemmers reduce differently: Lucene ships an older Snowball English than vfts (libstemmer 3.2.0),
and stems "organization", "organisms" and "organic" all to "organ", "university" and "universe"
both to "univers"; vfts keeps them apart ("organiz", "organism", "organic", "universiti",
"univers"). Top 10: documents tied at the 10th score are ordered by doc_id in vfts and by
internal order in the engines; prefixes score 1 for every match (constant score) on all three.

**What made vfts's counts fast** (0.4.1; docs/design.md): a word's documents are set in the
window's bit set 128 at a time; a word in at least 1 of 32 documents is turned into a bit set of
the segment the first time a count uses it; a batch of counts is cut into slices of each query's
segments, all shared by the cores, the costliest queries first. The 6,980 MS MARCO counts took
41 s in one statement with vfts 0.4.0, and 0.53 s with 0.4.1, with the same counts.
