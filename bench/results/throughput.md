| Engine | enron | pubmed | msmarco | wiki_he | wiki_ar |
|---|---:|---:|---:|---:|---:|
| vtext, tserve (one statement per session) | 6,832 (6,810-7,225) | 11,740 (11,560-11,905) | 3,218 (3,210-3,219) | 20,138 (20,062-20,271) | 17,559 (17,330-17,810) |
| vtext, batches (8 sessions, idle-else-queue) | 2,366 (2,352-2,394) | 3,029 (3,016-3,039) | 1,801 (1,799-1,804) | 3,554 (3,547-3,563) | 3,413 (3,406-3,425) |
| vtext, one statement per query (32 sessions) | 1,246 (1,229-1,248) | 1,269 (1,268-1,277) | 928 (923-929) | 1,297 (1,289-1,302) | 1,313 (1,311-1,316) |
| Elasticsearch | 3,902 (3,684-4,044) | 7,964 (7,813-8,339) | 2,729 (2,581-2,798) | 10,100 (10,057-10,181) | 10,050 (10,013-10,134) |
| OpenSearch | 3,881 (3,782-3,918) | 8,122 (8,075-8,290) | 2,907 (2,732-2,991) | 10,166 (10,041-10,305) | 10,469 (10,455-10,528) |

One client: mean ms per query (best of 3 runs per query), median over rounds; p99 in brackets

| Engine | enron | pubmed | msmarco | wiki_he | wiki_ar |
|---|---:|---:|---:|---:|---:|
| vtext, tserve (one statement per session) | 1.77 (18.2) | 1.01 (4.9) | 2.95 (13.8) | 0.58 (3.2) | 0.66 (5.2) |
| vtext, one statement per query (32 sessions) | 3.88 (14.1) | 3.36 (5.6) | 3.93 (6.0) | 3.17 (5.2) | 3.22 (6.7) |
| Elasticsearch | 2.75 (31.4) | 1.30 (5.3) | 3.95 (16.2) | 1.09 (4.5) | 1.07 (4.2) |
| OpenSearch | 2.40 (28.7) | 1.07 (2.8) | 3.60 (16.0) | 1.06 (4.1) | 1.09 (4.3) |

vtext serve / best of Elasticsearch and OpenSearch (median q/s):
- enron: 1.75x
- pubmed: 1.45x
- msmarco: 1.11x
- wiki_he: 1.98x
- wiki_ar: 1.68x
